/**
 * @file profile_curve_follower.c
 * @brief See header.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_timer.h"

#include "roast_core/session_state_machine.h"
#include "roast_core/command_dispatcher.h"
#include "roast_core/roast_telemetry_service.h"
#include "roast_core/roast_profile.h"
#include "roast_core/heater_pid.h"
#include "storage/profile_store.h"
#include "roast_core/profile_curve_follower.h"

static const char *TAG = "profile_curve_follower";
#define FOLLOWER_PERIOD_US (1000 * 1000) /* 1s - fine enough granularity for a piecewise-linear curve. */

/* Cooling fan speed used whenever there's no profile-defined Cooling
 * segment to follow yet (no profile selected, Manual/Artisan mode, or an
 * early Cancel/Emergency Stop before the roast reached the profile's own
 * Cooling segment) - full blast for fastest, safest cool-down. */
#define COOLING_FALLBACK_FAN_PCT 100

/* Cooling is considered "done" (safe to finalize the session) once BT drops
 * below this, regardless of what triggered cooling - used as the
 * auto-complete condition for sessions with no profile-defined Cooling
 * duration (Manual/Artisan mode, or a Cancel/Emergency Stop). */
#define COOLING_AUTO_COMPLETE_TEMP_C 50.0f

/* Failsafe: force-finalize after this long in COOLING regardless of
 * temperature, in case the sensor is invalid/stuck - never leave a session
 * stranded in COOLING forever. */
#define COOLING_FAILSAFE_MS (15 * 60 * 1000)

static esp_timer_handle_t s_timer;

static roast_profile_t s_profile;
static bool s_profile_loaded = false;
static roast_phase_t s_last_phase = ROAST_PHASE_IDLE;
static int64_t s_cooling_entered_at_ms = 0;
static bool s_auto_finished = false;

static bool s_override_active = false;
static uint8_t s_override_segment_idx = 0;
static int s_last_written_fan = -1;
static int s_last_written_heater = -1;
static bool s_fallback_fan_written = false;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool phase_is_session_active(roast_phase_t phase)
{
    return phase == ROAST_PHASE_PREHEAT || phase == ROAST_PHASE_ROASTING || phase == ROAST_PHASE_DEVELOPMENT ||
           phase == ROAST_PHASE_COOLING;
}

static void reset_override_tracking(void)
{
    s_override_active = false;
    s_override_segment_idx = 0;
    s_last_written_fan = -1;
    s_last_written_heater = -1;
    s_fallback_fan_written = false;
}

/** Drives fan+heater from the profile's curve during a normal (non-cooling) heating segment, honoring a manual override (T035) until the next segment boundary. Heater duty comes from the closed-loop PID (heater_pid.h) tracking target_temp_c against the live BT sensor - there is no open-loop heater setpoint anymore. */
static void drive_heating_segment(uint32_t elapsed_s, uint8_t segment_idx)
{
    roast_telemetry_snapshot_t snap;
    roast_telemetry_service_get_snapshot(&snap);

    float target_temp = roast_profile_get_target_temp_c(&s_profile, elapsed_s);
    uint8_t target_fan = roast_profile_get_target_fan_pct(&s_profile, elapsed_s);
    /* Keep the PID's internal state (integral/derivative) ticking every
     * follower period regardless of override, so it doesn't need to "catch
     * up" with a derivative kick once an override expires; only whether we
     * ACT on its output is gated below. */
    uint8_t target_heater = heater_pid_update(target_temp, snap.sensor_valid ? snap.bean_temp_c : target_temp,
                                               FOLLOWER_PERIOD_US / 1000000.0f);

    if (s_override_active) {
        if (segment_idx != s_override_segment_idx) {
            ESP_LOGI(TAG, "Manual override expired at the next curve segment - resuming automatic profile control");
            s_override_active = false;
        } else {
            return; /* Operator's override still stands for the rest of this segment (T035). */
        }
    }

    if (s_last_written_fan >= 0 && (snap.fan_pct != s_last_written_fan || snap.heater_pct != s_last_written_heater)) {
        ESP_LOGI(TAG, "Manual override detected (fan %d->%d, heater %d->%d) - pausing curve follower until next segment",
                 s_last_written_fan, snap.fan_pct, s_last_written_heater, snap.heater_pct);
        s_override_active = true;
        s_override_segment_idx = segment_idx;
        return;
    }

    /* Fan first, then heater - Safety Manager rejects heater > 0 unless fan
     * is already at/above the 30% floor. */
    command_dispatcher_set_fan_pct(target_fan, SAFETY_CMD_SOURCE_PROFILE_CURVE);
    command_dispatcher_set_heater_pct(target_heater, SAFETY_CMD_SOURCE_PROFILE_CURVE);
    s_last_written_fan = target_fan;
    s_last_written_heater = target_heater;
}

/** Drives ONLY the fan during COOLING (heater is never touched here - it's
 * already forced off by the phase transition itself), either following the
 * profile's own Cooling segment curve (if elapsed time genuinely falls
 * within one) or a fixed fallback speed otherwise (T035 override still
 * honored in the profile-driven case). */
static void drive_cooling(uint32_t elapsed_s, bool within_profile_cooling_segment, uint8_t segment_idx)
{
    if (within_profile_cooling_segment) {
        roast_telemetry_snapshot_t snap;
        roast_telemetry_service_get_snapshot(&snap);

        if (s_override_active) {
            if (segment_idx != s_override_segment_idx) {
                ESP_LOGI(TAG, "Manual override expired at the next Cooling segment - resuming automatic profile control");
                s_override_active = false;
            } else {
                return;
            }
        }

        uint8_t target_fan = roast_profile_get_target_fan_pct(&s_profile, elapsed_s);
        if (s_last_written_fan >= 0 && snap.fan_pct != s_last_written_fan) {
            ESP_LOGI(TAG, "Manual fan override detected during Cooling (%d->%d) - pausing until next segment",
                     s_last_written_fan, snap.fan_pct);
            s_override_active = true;
            s_override_segment_idx = segment_idx;
            return;
        }
        command_dispatcher_set_fan_pct(target_fan, SAFETY_CMD_SOURCE_PROFILE_CURVE);
        s_last_written_fan = target_fan;
        return;
    }

    /* No profile-defined Cooling curve to follow (Manual/Artisan mode, no
     * profile selected, or cancelled/emergency-stopped before reaching the
     * profile's own Cooling segment) - just kick the fan to a fixed safe
     * speed ONCE and then leave it to the operator (Manual tab) from then
     * on, bounded only by the SAFETY_FAN_STOP_MIN_TEMP_C hard floor in
     * safety_manager.h. */
    if (!s_fallback_fan_written) {
        command_dispatcher_set_fan_pct(COOLING_FALLBACK_FAN_PCT, SAFETY_CMD_SOURCE_PROFILE_CURVE);
        s_fallback_fan_written = true;
    }
}

static void follower_timer_cb(void *arg)
{
    (void)arg;
    const roast_session_t *session = session_sm_get_state();
    roast_phase_t phase = session->phase;

    if (!phase_is_session_active(phase)) {
        s_last_phase = phase;
        return;
    }

    /* Just started an active roast (came from PREHEAT via CHARGE) - load
     * whichever profile was selected at session start, once. */
    if (!phase_is_session_active(s_last_phase)) {
        s_profile_loaded = (session->control_mode == ROAST_MODE_PROFILE) &&
                            (profile_store_get_selected(&s_profile) == ESP_OK);
        reset_override_tracking();
        heater_pid_reset();
        s_auto_finished = false;
        if (session->control_mode == ROAST_MODE_PROFILE && !s_profile_loaded) {
            ESP_LOGW(TAG, "Session is in Profile mode but no profile could be loaded - curve follower idle");
        }
    }

    bool just_entered_cooling = (phase == ROAST_PHASE_COOLING && s_last_phase != ROAST_PHASE_COOLING);
    if (just_entered_cooling) {
        s_cooling_entered_at_ms = now_ms();
        reset_override_tracking();
    }

    s_last_phase = phase;

    if (session->paused) {
        return;
    }

    uint32_t elapsed_s = (uint32_t)(session->elapsed_ms / 1000);

    if (phase == ROAST_PHASE_PREHEAT) {
        if (!s_profile_loaded) {
            return; /* Manual/Artisan preheat - operator drives fan/heater manually via the Manual tab. */
        }
        /* Operator request: preheat should actually heat toward the first
         * setpoint's target bean temperature (not just idle with the
         * heater off) - there's no "start of roast" elapsed-time reference
         * yet (that begins at CHARGE, session_sm_confirm_charge()), so
         * hold segment 0's target flat via elapsed_s=0 instead of
         * evolving through the curve. */
        drive_heating_segment(0, 0);
        return;
    }

    if (phase == ROAST_PHASE_ROASTING || phase == ROAST_PHASE_DEVELOPMENT) {
        if (!s_profile_loaded) {
            return; /* Manual/Artisan mode - operator drives fan/heater directly, nothing to do here. */
        }
        uint8_t segment_idx = roast_profile_get_segment_index(&s_profile, elapsed_s);
        if (s_profile.points[segment_idx].is_cooling) {
            /* T038: the profile's own trailing Cooling segment has been
             * reached naturally - transition the session phase; the actual
             * cooling fan control happens on the NEXT tick (phase will read
             * back as COOLING then). */
            ESP_LOGI(TAG, "Profile curve reached its Cooling segment - auto-starting Cooling");
            session_sm_start_cooling();
            return;
        }
        drive_heating_segment(elapsed_s, segment_idx);
        return;
    }

    /* phase == ROAST_PHASE_COOLING (reached naturally above, or forced
     * immediately by session_sm_cancel() - Cancel/Emergency Stop). */
    uint32_t total_s = s_profile_loaded ? roast_profile_total_duration_s(&s_profile) : 0;
    uint8_t segment_idx = (s_profile_loaded && total_s > 0) ? roast_profile_get_segment_index(&s_profile, elapsed_s) : 0;
    bool within_profile_cooling_segment =
        s_profile_loaded && total_s > 0 && elapsed_s < total_s && s_profile.points[segment_idx].is_cooling;

    drive_cooling(elapsed_s, within_profile_cooling_segment, segment_idx);

    if (s_auto_finished) {
        return;
    }

    roast_telemetry_snapshot_t snap;
    roast_telemetry_service_get_snapshot(&snap);

    bool profile_cooling_done = s_profile_loaded && total_s > 0 && elapsed_s >= total_s;
    bool temp_safe = snap.sensor_valid && snap.bean_temp_c < COOLING_AUTO_COMPLETE_TEMP_C;
    bool failsafe_elapsed = (now_ms() - s_cooling_entered_at_ms) >= COOLING_FAILSAFE_MS;

    /* When we're actually following the profile's own Cooling segment, its
     * configured duration is the authority for how long to cool - don't
     * let temp_safe short-circuit it, or a profile tested/roasted at a low
     * bean temperature (BT already under the "safe" threshold the moment
     * Cooling starts) would "finish" after a single 1s tick instead of
     * running the full Cooling time the operator configured. temp_safe
     * still applies whenever there's no profile Cooling curve to follow
     * (Manual/Artisan mode, no profile selected, or a Cancel/E-Stop before
     * the curve ever reached the profile's own Cooling segment) - and the
     * hard failsafe always applies regardless, as a last-resort safety
     * net. */
    bool should_finish = failsafe_elapsed ||
                          (within_profile_cooling_segment ? profile_cooling_done : (profile_cooling_done || temp_safe));

    if (should_finish) {
        s_auto_finished = true;
        if (failsafe_elapsed && !profile_cooling_done && !temp_safe) {
            ESP_LOGW(TAG, "Cooling failsafe duration reached (%d min) - finalizing session regardless of temperature",
                     (int)(COOLING_FAILSAFE_MS / 60000));
        } else {
            ESP_LOGI(TAG, "Cooling finished (%s) - finalizing session",
                     profile_cooling_done ? "profile timeline elapsed" : "BT below safe threshold");
        }
        session_sm_complete();
    }
}

esp_err_t profile_curve_follower_init(void)
{
    s_profile_loaded = false;
    s_last_phase = ROAST_PHASE_IDLE;
    s_cooling_entered_at_ms = 0;
    s_auto_finished = false;
    reset_override_tracking();
    heater_pid_reset();

    const esp_timer_create_args_t args = {
        .callback = follower_timer_cb,
        .name = "profile_curve_follower",
    };
    esp_err_t err = esp_timer_create(&args, &s_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_timer_start_periodic(s_timer, FOLLOWER_PERIOD_US);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_start_periodic failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Profile curve follower init OK");
    return ESP_OK;
}
