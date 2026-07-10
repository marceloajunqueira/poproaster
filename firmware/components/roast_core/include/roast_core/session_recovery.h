/**
 * @file session_recovery.h
 * @brief FR-022: power-loss auto-resume. Attempts to resume a roast session
 *        that was still active when the device lost power, re-validating
 *        fan-floor/sensor conditions before allowing heat again.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Call once at boot, after session_state_machine_init(), safety_manager_init()
 * and max6675_init()/fan_pwm_init(). If a session was active when power was
 * lost (per session_sm_has_pending_recovery()), restores it
 * (session_sm_resume_from_recovery()) and re-validates the current sensor
 * reading: if invalid, forces the safe state (heater OFF - already the boot
 * default - fan forced to the safety floor) and raises a critical alarm
 * requiring manual acknowledgment (FR-029) before the roast may continue;
 * otherwise resumes ventilation at the safety floor and lets the operator
 * carry on normally. Heating itself is never auto-resumed - it always
 * requires an explicit new command from the operator/profile, validated as
 * usual by safety_manager.h.
 */
esp_err_t session_recovery_try_resume(void);

#ifdef __cplusplus
}
#endif
