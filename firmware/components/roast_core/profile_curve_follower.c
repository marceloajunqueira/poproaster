/**
 * @file profile_curve_follower.c
 * @brief See header.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

#include "roast_core/session_state_machine.h"
#include "roast_core/command_dispatcher.h"
#include "roast_core/roast_telemetry_service.h"
#include "roast_core/roast_profile.h"
#include "roast_core/roast_events.h"
#include "roast_core/heater_pid.h"
#include "roast_core/pid_autotune.h"
#include "roast_core/pid_debug_log.h"
#include "storage/profile_store.h"
#include "hal/fan_pwm.h"
#include "hal/ssr_heater.h"
#include "safety/safety_manager.h"
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
static float s_manual_target_temp_c = 0.0f;

static bool s_override_active = false;
static uint8_t s_override_segment_idx = 0;
static int s_last_written_fan = -1;
static int s_last_written_heater = -1;
static bool s_fallback_fan_written = false;

/* BUG FIX (operator-reported): the fan used to stay on FOREVER once a
 * Profile-mode roast auto-completed after Cooling - nothing ever commanded
 * it back to 0, since session_sm_complete() itself doesn't touch actuators
 * and the "no active session" branch below only drives the Manual-mode
 * heater PID, never the fan. `s_fan_off_pending` tracks a best-effort,
 * retried-until-safe fan shutoff: set the moment the session completes,
 * retried every tick afterward (the very first attempt commonly succeeds
 * immediately, since COOLING_AUTO_COMPLETE_TEMP_C=50C is already well under
 * the fan's own SAFETY_FAN_STOP_MIN_TEMP_C=100C anti-scorch floor - but a
 * completion triggered by the profile's own cooling DURATION elapsing, or
 * the cooling failsafe, could still have BT >=100C, so the very first
 * attempt can legitimately be rejected and needs a retry once it's safe). 
 * Bounded by FAN_OFF_RETRY_WINDOW_MS and abandoned early if the operator
 * manually changes the fan themselves (checked against s_last_written_fan,
 * same override-detection idea already used elsewhere in this file) - this
 * must never fight an operator's own explicit fan command. */
#define FAN_OFF_RETRY_WINDOW_MS (5 * 60 * 1000)
static bool s_fan_off_pending = false;
static int64_t s_fan_off_pending_since_ms = 0;

/* PID tuning: open-loop step-response test state (see
 * profile_curve_follower_set_step_test_heater_pct() in the header). -1
 * means inactive. */
static int s_step_test_heater_pct = -1;
static int64_t s_step_test_start_ms = 0;

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
    /* Tell the thermal-protector observer what was ACTUALLY driving the
     * element over the interval that just elapsed, before asking the PID to
     * evaluate it - a temperature collapse only implicates the protector if
     * real power was being delivered. */
    heater_pid_note_applied_pct(ssr_heater_get_duty_pct());
    /* Keep the PID's internal state (integral/derivative) ticking every
     * follower period regardless of override, so it doesn't need to "catch
     * up" with a derivative kick once an override expires; only whether we
     * ACT on its output is gated below. */
    uint8_t target_heater = heater_pid_update(target_temp, snap.sensor_valid ? snap.bean_temp_c : target_temp,
                                               FOLLOWER_PERIOD_US / 1000000.0f);

    /* Operator-requested PID tuning log (roast_core/pid_debug_log.h) - one
     * row per follower tick, regardless of override state, so the whole
     * picture (including gaps where an override was in effect) is
     * visible. applied_heater_pct/real_fan_pct read back whatever is
     * CURRENTLY actually running (from before this tick's own commands
     * take effect) - a harmless one-tick lag for tuning purposes. */
    pid_debug_log_record("PROFILE", session_sm_get_state()->phase, elapsed_s, target_temp,
                          snap.sensor_valid ? snap.bean_temp_c : target_temp, snap.sensor_valid, target_heater,
                          ssr_heater_get_duty_pct(), target_fan, fan_pwm_get_pct());

    if (s_override_active) {
        if (segment_idx != s_override_segment_idx) {
            ESP_LOGI(TAG, "Manual override expired at the next curve segment - resuming automatic profile control");
            s_override_active = false;
        } else {
            return; /* Operator's override still stands for the rest of this segment (T035). */
        }
    }

    /* BUG FIX: each of fan/heater must only be override-checked once THIS
     * follower has actually gotten a successful write in for it - checking
     * both under a single "s_last_written_fan >= 0" guard (as before)
     * silently assumed both always become valid together, which broke once
     * heater writes started being independently success-gated (see below):
     * during the fan's soft-start ramp, the fan write succeeds immediately
     * while the heater write is CORRECTLY rejected (fan not physically at
     * the floor yet) for a few ticks - leaving s_last_written_heater at its
     * -1 sentinel while s_last_written_fan already holds a real value.
     * Casting that -1 sentinel to uint8_t (255) against a real 0-100
     * request always "mismatched", permanently freezing the heater via a
     * false override the moment the fan's first write succeeded -
     * operator-reported ("fan subiu aos poucos, mas o Heater nunca foi
     * acionado"). Fix: gate each field's mismatch check independently on
     * ITS OWN sentinel. */
    bool fan_override = (s_last_written_fan >= 0) && (fan_pwm_get_target_pct() != (uint8_t)s_last_written_fan);
    bool heater_override = (s_last_written_heater >= 0) &&
                            (safety_manager_get_last_requested_heater_pct() != (uint8_t)s_last_written_heater);
    if (fan_override || heater_override) {
        ESP_LOGI(TAG, "Manual override detected (fan %d->%d, heater %d->%d) - pausing curve follower until next segment",
                 s_last_written_fan, fan_pwm_get_target_pct(), s_last_written_heater,
                 safety_manager_get_last_requested_heater_pct());
        s_override_active = true;
        s_override_segment_idx = segment_idx;
        return;
    }

    /* Fan first, then heater - Safety Manager rejects heater > 0 unless fan
     * is already at/above the 60% (Level 1) floor. Only remember what was
     * successfully applied - not merely attempted - so a temporary/
     * expected rejection (e.g. the fan still soft-start ramping toward
     * this exact target) doesn't get misread as an external override on
     * the very next tick. */
    esp_err_t fan_err = command_dispatcher_set_fan_pct(target_fan, SAFETY_CMD_SOURCE_PROFILE_CURVE);
    esp_err_t heater_err = command_dispatcher_set_heater_pct(target_heater, SAFETY_CMD_SOURCE_PROFILE_CURVE);
    if (fan_err == ESP_OK) {
        s_last_written_fan = target_fan;
    }
    if (heater_err == ESP_OK) {
        s_last_written_heater = target_heater;
    }
}

/** Drives ONLY the fan during COOLING (heater is never touched here - it's
 * already forced off by the phase transition itself), either following the
 * profile's own Cooling segment curve (if elapsed time genuinely falls
 * within one) or a fixed fallback speed otherwise (T035 override still
 * honored in the profile-driven case). */
static void drive_cooling(uint32_t elapsed_s, bool within_profile_cooling_segment, uint8_t segment_idx)
{
    if (within_profile_cooling_segment) {
        if (s_override_active) {
            if (segment_idx != s_override_segment_idx) {
                ESP_LOGI(TAG, "Manual override expired at the next Cooling segment - resuming automatic profile control");
                s_override_active = false;
            } else {
                return;
            }
        }

        uint8_t target_fan = roast_profile_get_target_fan_pct(&s_profile, elapsed_s);
        if (s_last_written_fan >= 0 && fan_pwm_get_target_pct() != (uint8_t)s_last_written_fan) {
            ESP_LOGI(TAG, "Manual fan override detected during Cooling (%d->%d) - pausing until next segment",
                     s_last_written_fan, fan_pwm_get_target_pct());
            s_override_active = true;
            s_override_segment_idx = segment_idx;
            return;
        }
        esp_err_t fan_err = command_dispatcher_set_fan_pct(target_fan, SAFETY_CMD_SOURCE_PROFILE_CURVE);
        if (fan_err == ESP_OK) {
            s_last_written_fan = target_fan;
        }
        return;
    }

    /* Reached only if COOLING was somehow entered outside the profile's own
     * Cooling segment - not expected in the current design (operator
     * Cancel/Emergency Stop now abort immediately via session_sm_abort()
     * instead of routing through COOLING at all), but kept as a defensive
     * fallback: kick the fan to a fixed safe speed ONCE and then leave it
     * to the operator (Manual tab) from then on, bounded only by the
     * SAFETY_FAN_STOP_MIN_TEMP_C hard floor in safety_manager.h. */
    if (!s_fallback_fan_written) {
        command_dispatcher_set_fan_pct(COOLING_FALLBACK_FAN_PCT, SAFETY_CMD_SOURCE_PROFILE_CURVE);
        s_fallback_fan_written = true;
    }
}

/** Manual/Artisan mode: drives ONLY the heater, automatically, via the same
 * closed-loop PID Profile mode uses, tracking whatever target bean
 * temperature the operator set via profile_curve_follower_set_manual_target_temp_c()
 * (Manual screen's "Target Temp" slider) - fan is left entirely to the
 * operator's own Fan slider/command. Operator-reported bug: requesting
 * heat while the fan was left off did nothing (Safety Manager's 60% floor
 * silently rejected it) - auto-raises the fan to that floor here instead,
 * whenever the PID actually wants to apply heat. */
static void drive_manual_heater(void)
{
    roast_telemetry_snapshot_t snap;
    roast_telemetry_service_get_snapshot(&snap);

    /* See drive_heating_segment(): the observer needs the duty that was
     * really applied over the interval just elapsed. */
    heater_pid_note_applied_pct(ssr_heater_get_duty_pct());

    uint8_t heater_target = heater_pid_update(s_manual_target_temp_c,
                                               snap.sensor_valid ? snap.bean_temp_c : s_manual_target_temp_c,
                                               FOLLOWER_PERIOD_US / 1000000.0f);

    if (heater_target > 0 && snap.fan_pct < SAFETY_FAN_MIN_PCT_DURING_HEAT) {
        command_dispatcher_set_fan_pct(SAFETY_FAN_MIN_PCT_DURING_HEAT, SAFETY_CMD_SOURCE_DISPLAY);
    }
    command_dispatcher_set_heater_pct(heater_target, SAFETY_CMD_SOURCE_DISPLAY);

    /* Operator-requested PID tuning log - only while a real target is set
     * (this function runs on EVERY follower tick regardless of session
     * state, including IDLE, so gating on a nonzero target avoids logging
     * 24/7 while the roaster just sits idle). */
    if (s_manual_target_temp_c > 0.0f) {
        pid_debug_log_record("MANUAL", session_sm_get_state()->phase,
                              (uint32_t)(session_sm_get_state()->elapsed_ms / 1000), s_manual_target_temp_c,
                              snap.sensor_valid ? snap.bean_temp_c : s_manual_target_temp_c, snap.sensor_valid,
                              heater_target, ssr_heater_get_duty_pct(), fan_pwm_get_target_pct(), fan_pwm_get_pct());
    }
}

/** PID tuning: drives the heater at a FIXED, PID-bypassed duty for as long
 * as a step-response test is active (see header), logging every tick to
 * the same PID debug log under mode="STEPTEST" so the resulting bean-temp
 * rise/fall curve can be analyzed offline - the fan is left entirely to
 * the operator's own control since this hardware heats via forced air
 * through the coil (fan can never be off during a real heat test). */
static void drive_step_test(void)
{
    roast_telemetry_snapshot_t snap;
    roast_telemetry_service_get_snapshot(&snap);

    command_dispatcher_set_heater_pct((uint8_t)s_step_test_heater_pct, SAFETY_CMD_SOURCE_DISPLAY);

    uint32_t elapsed_s = (uint32_t)((now_ms() - s_step_test_start_ms) / 1000);
    pid_debug_log_record("STEPTEST", ROAST_PHASE_IDLE, elapsed_s, 0.0f,
                          snap.sensor_valid ? snap.bean_temp_c : 0.0f, snap.sensor_valid,
                          (uint8_t)s_step_test_heater_pct, ssr_heater_get_duty_pct(), fan_pwm_get_target_pct(),
                          fan_pwm_get_pct());
}

/** Relay-feedback autotune: bypasses the PID and lets pid_autotune.c drive
 * the duty directly, logged under mode="AUTOTUNE". */
static bool s_autotune_result_applied;
static bool s_autotune_fan_release_attempted;

static void drive_autotune(void)
{
    roast_telemetry_snapshot_t snap;
    roast_telemetry_service_get_snapshot(&snap);

    uint8_t duty = 0;
    if (snap.sensor_valid) {
        duty = pid_autotune_update(snap.bean_temp_c);
    } else {
        pid_autotune_abort("Sensor reading invalid");
    }

    command_dispatcher_set_heater_pct(duty, SAFETY_CMD_SOURCE_DISPLAY);

    /* BUG FIX: this MUST be fetched AFTER pid_autotune_update() (which is
     * what actually transitions RUNNING -> SUCCEEDED/FAILED internally,
     * synchronously, on whichever tick the run finishes), not before -
     * fetching it before update() meant this always saw the STALE
     * (still-RUNNING) state, so the SUCCEEDED branch below could never
     * fire: by the next tick, pid_autotune_is_active() (checked by
     * follower_timer_cb before it will call this function again) already
     * reports false, so drive_autotune() never even runs again to see the
     * new state. The result: the "auto-apply+save to NVS on success"
     * feature never actually triggered. */
    pid_autotune_status_t st;
    pid_autotune_get_status(&st);

    /* Fully automatic per operator request: apply+persist the instant the
     * run converges, regardless of whether any UI (on-device screen or the
     * web Diagnostics page) happens to be open to see it. "No overshoot" is
     * the deliberately conservative pick of the three Ziegler-Nichols rule
     * sets pid_autotune.c computes - Ziegler-Nichols classic is known to
     * leave ~25% overshoot, right where this machine's thermal protector
     * tends to trip. */
    if (st.state == PID_AUTOTUNE_SUCCEEDED && !s_autotune_result_applied) {
        esp_err_t err = pid_autotune_apply_result(&st.no_overshoot, true);
        s_autotune_result_applied = true;
        ESP_LOGI(TAG, "Autotune converged - applied+saved No-Overshoot gains (kp=%.3f ki=%.4f kd=%.2f): %s",
                 (double)st.no_overshoot.kp, (double)st.no_overshoot.ki, (double)st.no_overshoot.kd,
                 esp_err_to_name(err));
    } else if (st.state == PID_AUTOTUNE_RUNNING) {
        s_autotune_result_applied = false; /* Reset so the next run's success also gets applied. */
    }

    /* Bug report: "cancelei e o fan continuou ligado" - raising the fan to
     * full speed to run the relay test is this subsystem's own side effect
     * (pid_autotune_screen.c / diagnostics_routes.c), so the instant a run
     * STOPS for any reason (converged, self-aborted, or an operator
     * Cancel), try ONCE to hand the fan back - covers every stop path in
     * one place, not just whichever UI's own Cancel button happened to be
     * used. Rejected (fan stays on) while BT is still >= 100C, same
     * anti-scorch floor as everywhere else in the firmware - that's
     * expected, not a bug; the operator still has full manual control of
     * the fan afterward regardless. */
    if (st.state != PID_AUTOTUNE_RUNNING && st.state != PID_AUTOTUNE_IDLE && !s_autotune_fan_release_attempted) {
        s_autotune_fan_release_attempted = true;
        esp_err_t fan_err = command_dispatcher_set_fan_pct(0, SAFETY_CMD_SOURCE_DISPLAY);
        ESP_LOGI(TAG, "Autotune stopped - fan release attempt: %s", esp_err_to_name(fan_err));
    } else if (st.state == PID_AUTOTUNE_RUNNING) {
        s_autotune_fan_release_attempted = false; /* Reset so the next run's stop also tries this. */
    }

    pid_debug_log_record("AUTOTUNE", ROAST_PHASE_IDLE, st.elapsed_s, st.setpoint_c,
                          snap.sensor_valid ? snap.bean_temp_c : 0.0f, snap.sensor_valid, duty,
                          ssr_heater_get_duty_pct(), fan_pwm_get_target_pct(), fan_pwm_get_pct());
}

static void follower_timer_cb(void *arg)
{
    (void)arg;

    /* Every esp_timer callback in the whole firmware - this one, the SSR
     * PWM window, and the safety manager's own 240C absolute-cutoff check -
     * runs on the SAME single shared esp_timer task. A hang anywhere in
     * THIS control loop would silently freeze the SSR GPIO at its last
     * state AND stop the safety cutoff from ever being checked again, with
     * nothing left to catch it once the physical thermal protector is
     * removed (2026-07-31). Subscribing this task to the Task Watchdog and
     * resetting it every tick means a genuine hang reboots the board
     * (CONFIG_ESP_TASK_WDT_PANIC, sdkconfig.defaults) instead of hanging
     * forever - safe, since GPIOs default OFF at boot. */
    static bool s_wdt_subscribed;
    if (!s_wdt_subscribed) {
        s_wdt_subscribed = (esp_task_wdt_add(NULL) == ESP_OK);
    }
    if (s_wdt_subscribed) {
        esp_task_wdt_reset();
    }

    if (pid_autotune_is_active()) {
        drive_autotune();
        return; /* Supersedes Manual/Profile PID control entirely while active. */
    }

    if (s_step_test_heater_pct >= 0) {
        drive_step_test();
        return; /* Supersedes Manual/Profile PID control entirely while active. */
    }

    const roast_session_t *session = session_sm_get_state();
    roast_phase_t phase = session->phase;

    if (!phase_is_session_active(phase)) {
        /* No active roast session (IDLE/COMPLETED/ABORTED) - Manual control
         * (Fan level buttons + Target Temp "Aplicar" on the Manual tab)
         * must work freely regardless of session state, per operator
         * requirement: entering Manual mode should never require "Start
         * Roast" first - the operator may just want direct control, or to
         * hand off to Artisan, with no formal roast session at all. Drive
         * the same closed-loop PID toward whatever Target Temp was last
         * applied (defaults to 0.0f / heater off at boot until the
         * operator explicitly sets one via the Manual screen). Never treat
         * this as Profile mode - a stale s_profile_loaded from a previous
         * session must not linger here. */
        if (phase_is_session_active(s_last_phase)) {
            /* BUG FIX: a session just ended THIS tick (Cancel/Abort or
             * Emergency Stop - both go straight to ABORTED per the current
             * design, see drive_cooling()'s comment above). Manual mode's
             * Target Temp is intentionally session-independent (the fix
             * above this one), but that means it was NEVER being cleared
             * here - so a Manual/Artisan session cancelled while a nonzero
             * Target Temp was still applied would have this exact branch
             * call drive_manual_heater() again on THIS SAME tick with the
             * stale target still in place, and since session_sm_abort()
             * only force-offs the heater ONCE (not an alarm - no ack
             * needed, unlike Emergency Stop), the PID would happily
             * re-command the heater back on toward that stale target
             * within ~1s of pressing Cancel - operator-facing symptom:
             * "Cancel doesn't actually stop the heat". Clear it so
             * ending a session always requires a fresh explicit Target
             * Temp from the operator afterward, same guarantee already
             * given when a NEW session starts below. */
            ESP_LOGI(TAG, "Session ended - clearing Manual Target Temp so heating can't silently resume");
            s_manual_target_temp_c = 0.0f;
        }
        if (s_fan_off_pending) {
            /* Keep retrying the deferred post-cooling fan shutoff every
             * tick while inactive, until it succeeds, the operator takes
             * the fan back over themselves, or the retry window expires. */
            bool operator_took_over = (s_last_written_fan >= 0) && (fan_pwm_get_target_pct() != (uint8_t)s_last_written_fan);
            bool window_expired = (now_ms() - s_fan_off_pending_since_ms) >= FAN_OFF_RETRY_WINDOW_MS;
            if (operator_took_over || window_expired) {
                s_fan_off_pending = false;
            } else if (command_dispatcher_set_fan_pct(0, SAFETY_CMD_SOURCE_PROFILE_CURVE) == ESP_OK) {
                ESP_LOGI(TAG, "Fan turned off automatically now that BT is below the safe-stop threshold");
                s_fan_off_pending = false;
            }
        }
        s_profile_loaded = false;
        s_last_phase = phase;
        drive_manual_heater();
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
        s_fan_off_pending = false;
        /* Never silently inherit a stale target from a previous Manual
         * session - the operator must set a fresh one via the Target Temp
         * slider each time. */
        s_manual_target_temp_c = 0.0f;
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
            if (session->control_mode == ROAST_MODE_MANUAL_ARTISAN) {
                drive_manual_heater();
            }
            return; /* Manual/Artisan preheat with no target set yet, or no profile could be loaded - nothing to do. */
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
            if (session->control_mode == ROAST_MODE_MANUAL_ARTISAN) {
                drive_manual_heater();
            }
            return; /* Manual/Artisan mode - fan is fully operator-controlled, heater is the PID above. */
        }
        uint8_t segment_idx = roast_profile_get_segment_index(&s_profile, elapsed_s);
        if (s_profile.points[segment_idx].is_cooling) {
            /* T038: the profile's own trailing Cooling segment has been
             * reached naturally - transition the session phase; the actual
             * cooling fan control happens on the NEXT tick (phase will read
             * back as COOLING then). Also auto-marks Cool Start - on this
             * popcorn-popper hardware there's no separate unload/drop step
             * (cooling starts immediately via the fan), so this single
             * automatic marker represents "Drop" too; no manual button
             * needed for either. */
            ESP_LOGI(TAG, "Profile curve reached its Cooling segment - auto-starting Cooling");
            session_sm_start_cooling();
            roast_events_mark(ROAST_EVENT_COOL_START);
            return;
        }
        drive_heating_segment(elapsed_s, segment_idx);
        return;
    }

    /* phase == ROAST_PHASE_COOLING - only ever reached via the profile's own
     * trailing Cooling segment now (T038 branch above); operator
     * Cancel/Emergency Stop abort immediately via session_sm_abort()
     * instead of routing through COOLING. */
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

        /* BUG FIX (operator-reported): the fan used to keep running
         * forever after a Profile roast finished Cooling - nothing ever
         * commanded it back off. Attempt it right away (this almost always
         * succeeds immediately, since COOLING_AUTO_COMPLETE_TEMP_C=50C is
         * already well under the fan's SAFETY_FAN_STOP_MIN_TEMP_C=100C
         * anti-scorch floor); if BT still isn't safe yet (e.g. this
         * completion was triggered by the profile's own cooling duration
         * elapsing, or the failsafe, rather than by temperature), defer
         * and keep retrying every tick from the "no active session" branch
         * above until it's safe. */
        s_fan_off_pending_since_ms = now_ms();
        if (command_dispatcher_set_fan_pct(0, SAFETY_CMD_SOURCE_PROFILE_CURVE) == ESP_OK) {
            s_fan_off_pending = false;
        } else {
            s_fan_off_pending = true;
            ESP_LOGI(TAG, "Fan-off deferred (BT still above the safe-stop threshold) - will keep retrying");
        }
    }
}

esp_err_t profile_curve_follower_init(void)
{
    s_profile_loaded = false;
    s_last_phase = ROAST_PHASE_IDLE;
    s_cooling_entered_at_ms = 0;
    s_auto_finished = false;
    s_manual_target_temp_c = 0.0f;
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

void profile_curve_follower_set_manual_target_temp_c(float target_c)
{
    /* SECURITY/SAFETY: this is reachable directly from the unauthenticated
     * web API (`POST /api/control action=set_target_temp&value=N`,
     * dashboard_routes.c) as a raw atoi()'d integer with NO validation at
     * that layer - unlike the on-device Manual screen, which already
     * bounds its slider to MANUAL_TARGET_TEMP_MIN_C..MAX_C. Without a
     * clamp here, a bogus/malicious request (or a client-side bug) could
     * set an absurd target (e.g. hugely negative or in the tens of
     * thousands); a very negative value is harmless (the PID's hard-
     * overshoot cutoff forces the heater off immediately), but a very
     * large positive value would have the PID legitimately try to drive
     * the heater at 100% indefinitely with nothing but the last-resort
     * 240C absolute safety cutoff (safety_manager.c) ever stopping it -
     * far more aggressive than any real profile/operator intent, and well
     * past the point operator testing showed the plastic housing melts.
     * Clamp to the same range the display's own Target Temp slider
     * enforces (0 is still allowed through as the "off" sentinel). */
    if (target_c < 0.0f) {
        target_c = 0.0f;
    } else if (target_c > MANUAL_TARGET_TEMP_MAX_C) {
        target_c = MANUAL_TARGET_TEMP_MAX_C;
    }
    s_manual_target_temp_c = target_c;
}

float profile_curve_follower_get_manual_target_temp_c(void)
{
    return s_manual_target_temp_c;
}

void profile_curve_follower_set_step_test_heater_pct(int pct)
{
    if (pct < 0) {
        s_step_test_heater_pct = -1;
        command_dispatcher_set_heater_pct(0, SAFETY_CMD_SOURCE_DISPLAY);
        return;
    }
    if (pct > 100) {
        pct = 100;
    }
    if (s_step_test_heater_pct < 0) {
        s_step_test_start_ms = now_ms(); /* Only reset the elapsed-time reference on a fresh start, not on a mid-test duty change. */
    }
    s_step_test_heater_pct = pct;
}

int profile_curve_follower_get_step_test_heater_pct(void)
{
    return s_step_test_heater_pct;
}
