/**
 * @file heater_pid.c
 * @brief See header.
 */
#include <stdbool.h>

#include "roast_core/heater_pid.h"

/* Coffee roast drums/BT probes have a large thermal lag, so this loop leans
 * on the integral term to close the steady-state gap rather than reacting
 * aggressively on proportional error alone (which would overshoot/oscillate
 * against an SSR's on/off duty-cycling). Derivative acts on the MEASURED
 * temperature (not on the error) to avoid a derivative "kick" every time
 * the profile curve's target jumps at a segment boundary. These gains are
 * conservative defaults - retune here only, nothing else depends on them. */
#define PID_KP 4.0f
#define PID_KI 0.05f
#define PID_KD 8.0f
#define PID_OUTPUT_MIN 0.0f
#define PID_OUTPUT_MAX 100.0f

/* SAFETY BACKSTOP: this drum's BT sensor sits in the circulating air (not
 * on the element itself), so there's a real, significant thermal lag
 * between the heater actually running and the sensor showing it -
 * operator-reported. During that lag, error stays strongly positive for a
 * while, which can wind the integral term up far beyond what's needed.
 * Independent of the anti-windup fix below, if the measured temperature is
 * already this many degrees ABOVE target, force the heater fully off
 * immediately, no PID math involved - a hard, unconditional ceiling.
 * Directly addresses an operator-reported burn risk: "coloquei 100 graus,
 * mesmo passando a temperatura o heater continuou no maximo". */
#define PID_HARD_OVERSHOOT_MARGIN_C 3.0f

static float s_integral;
static float s_prev_measured_c;
static bool s_has_prev;

void heater_pid_reset(void)
{
    s_integral = 0.0f;
    s_prev_measured_c = 0.0f;
    s_has_prev = false;
}

uint8_t heater_pid_update(float target_temp_c, float measured_temp_c, float dt_s)
{
    if (dt_s <= 0.0f) {
        dt_s = 1.0f;
    }

    float error = target_temp_c - measured_temp_c;

    if (error <= -PID_HARD_OVERSHOOT_MARGIN_C) {
        /* Already meaningfully over target - cut immediately and reset the
         * integral so there's no leftover windup once temperature comes
         * back down toward target. */
        s_integral = 0.0f;
        s_prev_measured_c = measured_temp_c;
        s_has_prev = true;
        return 0;
    }

    float d_measured = s_has_prev ? (measured_temp_c - s_prev_measured_c) / dt_s : 0.0f;
    s_prev_measured_c = measured_temp_c;
    s_has_prev = true;

    float p_term = PID_KP * error;
    float d_term = -PID_KD * d_measured;

    /* Back-calculation anti-windup: rather than merely FREEZING the
     * integral while saturated (the previous approach), continuously
     * resynchronize it to whatever value would be EXACTLY consistent with
     * the actually-applied (clamped) output. This means the integral can
     * never silently balloon far beyond what's needed to just barely
     * saturate - critical given this system's thermal lag can otherwise
     * let error stay positive for a long time, winding the integral up so
     * much that once the temperature finally catches up/overshoots, the
     * old freeze-only approach could take many minutes of negative error
     * to unwind it - during which the heater stayed pinned near 100% well
     * past the target (the exact burn-risk behavior reported). With this
     * fix, the moment the raw (unclamped) computation would no longer
     * saturate, the integral already reflects reality with no artificial
     * backlog, so output starts dropping immediately. */
    float integral_candidate = s_integral + error * dt_s;
    float unclamped_output = p_term + PID_KI * integral_candidate + d_term;

    float output = unclamped_output;
    if (output > PID_OUTPUT_MAX) {
        output = PID_OUTPUT_MAX;
    }
    if (output < PID_OUTPUT_MIN) {
        output = PID_OUTPUT_MIN;
    }

    s_integral = (PID_KI > 0.0f) ? ((output - p_term - d_term) / PID_KI) : integral_candidate;

    return (uint8_t)(output + 0.5f);
}
