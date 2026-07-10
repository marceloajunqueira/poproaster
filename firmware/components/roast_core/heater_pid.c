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

    float d_measured = s_has_prev ? (measured_temp_c - s_prev_measured_c) / dt_s : 0.0f;
    s_prev_measured_c = measured_temp_c;
    s_has_prev = true;

    float p_term = PID_KP * error;
    float d_term = -PID_KD * d_measured;

    /* Anti-windup (clamp-and-freeze): only accumulate the integral term
     * while the output isn't already saturated in a direction that would
     * make the saturation worse. */
    float unclamped_output = p_term + PID_KI * s_integral + d_term;
    bool saturated_high = unclamped_output > PID_OUTPUT_MAX;
    bool saturated_low = unclamped_output < PID_OUTPUT_MIN;
    if (!((saturated_high && error > 0.0f) || (saturated_low && error < 0.0f))) {
        s_integral += error * dt_s;
    }

    float output = p_term + PID_KI * s_integral + d_term;
    if (output > PID_OUTPUT_MAX) {
        output = PID_OUTPUT_MAX;
    }
    if (output < PID_OUTPUT_MIN) {
        output = PID_OUTPUT_MIN;
    }

    return (uint8_t)(output + 0.5f);
}
