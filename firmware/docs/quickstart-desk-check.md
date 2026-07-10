# Quickstart Validation — Desk Check (T060)

**Status: static code-path cross-reference only — NOT executed on real
hardware.** This document maps each scenario in
[`specs/001-pop-roaster-control/quickstart.md`](../../specs/001-pop-roaster-control/quickstart.md)
to the firmware code paths that are expected to satisfy it, as a "desk
check" performed by reading the implementation. It does **not** substitute
for actually running the 8 scenarios on a powered roaster with a real
MAX6675/SSR/fan/display, which the operator still needs to do. Where a gap
or an honest caveat exists, it's called out explicitly rather than glossed
over.

## Cenário 1: Segurança básica de aquecimento

| Step | Expected | Code path |
|------|----------|-----------|
| 3 (fan < 30%, heater blocked) | Blocked + alarm | `safety_manager_request_heater_pct()` in [`components/safety/safety_manager.c`](../components/safety/safety_manager.c) rejects heater>0 while `s_last_fan_pct < SAFETY_MIN_FAN_PCT_FOR_HEATER` (30) |
| 4 (fan ≥ 30%, heater accepted) | Accepted | Same function — passes once fan ≥ 30 |
| 5 (sensor failure) | Auto heater-off + critical alarm | `safety_manager_on_temperature_sample()` detects invalid reads, calls `ssr_heater_force_off()` + raises `SAFETY_ALARM_SENSOR_FAILURE` |
| 6 (manual ack required) | New heater command blocked until ack | `s_alarm_needs_ack` checked unconditionally in `safety_manager_request_heater_pct()`; cleared only via `command_dispatcher_acknowledge_alarm()` |
| 7 (E-Stop) | Immediate heater cut + session cancel | `command_dispatcher_emergency_stop()` calls `safety_manager_emergency_stop()` + `session_sm_cancel()` (forces COOLING immediately, distinct from a normal Cooling-segment completion) |

**Desk-check verdict:** all 5 expectations have a clear, direct code path.
No gaps identified by inspection.

## Cenário 2: Fluxo operacional completo

| Step | Expected | Code path |
|------|----------|-----------|
| 2 (CHARGE confirmation) | CHARGE only after explicit operator action | `session_sm_confirm_charge()` (PREHEAT→ROASTING), triggered only by the dashboard's "Charge" button / `command_dispatcher_confirm_charge()` — never automatic |
| 3 (live BT/RoR curve) | Real-time updates | `roast_telemetry_service` samples every 500ms; `roast_dashboard.c`'s `refresh_timer_cb` (also 500ms) repaints chart+labels from the cached snapshot |
| 4 (FC mark → DTR%) | DTR appears once FC marked | `roast_events_mark(ROAST_EVENT_TYPE_FC_START)` calls `roast_telemetry_service_mark_first_crack()`; `dtr_calculator.h` computes DTR% only once that timestamp is set |
| 5 (manual override holds until next curve point) | Override then curve resumes | `profile_curve_follower.c`: compares actual vs. last-written fan/heater each tick; on mismatch sets `s_override_active` until `roast_profile_get_segment_index()` changes (next segment boundary) |
| 6 (Cooling — heater off, fan on) | Correct | `profile_curve_follower.c`'s COOLING branch never writes heater; fan follows either the profile's own cooling segment or a fallback 100% |
| 7 (duration watchdog, 25 min default) | Auto-cooling + critical alarm | `safety/duration_watchdog.c`, 1s timer, compares `session_sm_get_state()->elapsed_ms` against `SAFETY_DEFAULT_MAX_DURATION_MS`; on exceed calls `safety_manager_report_duration_exceeded()` + `session_sm_start_cooling()` |

**Desk-check verdict:** covered end-to-end. One nuance worth flagging to
the operator: DTR%/RoR math have not been validated against a real roast
curve for numerical correctness (arithmetic was unit-reasoned, not measured
against known-good values from an actual bean mass/heater combination) —
worth a sanity comparison against Artisan's own DTR/RoR numbers during the
first real test roast.

## Cenário 3: Perfis, persistência em NVS e backup

| Step | Expected | Code path |
|------|----------|-----------|
| 1-2 (create/save) | Persisted | `profile_editor.c` + `profile_store_create()`/`update()` (NVS blob) |
| 3 (export JSON) | Correct JSON | `GET /api/presets/export?id=N` in [`presets_routes.c`](../components/web_api/presets_routes.c) |
| 4-5 (reboot, reload, start session) | Profile survives reboot | NVS persistence is inherently reboot-safe; `profile_store_init()` reads the same index/blob keys on every boot |
| 6 (delete original, history keeps its own snapshot) | History unaffected by deletion | `session_meta_t.profile` is a **full struct copy**, captured at roast start (`roast_telemetry_service_on_roast_started()`), independent of the live `profile_store` entry |
| 7 (re-import JSON) | Profile recreated | `POST /api/presets/import` in `presets_routes.c`, calls `profile_store_create()` |

**Desk-check verdict:** covered. Import/export round-trip (export then
re-import the exact same file) has not been executed end-to-end on device —
only each direction was build-verified independently.

## Cenário 4: Web UI paridade mínima

| Step | Expected | Code path |
|------|----------|-----------|
| 1-2 (open web UI, view chart/state) | Consistent with display | `dashboard_routes.c`'s `/ws/telemetry` WebSocket broadcasts the same `roast_telemetry_service` snapshot the display reads, now at 500ms (2Hz, bumped from 1Hz this session for T059) |
| 3 (fan adjust via web) | Goes through safety validation | `POST /api/control` → `command_dispatcher_set_fan_pct(..., SAFETY_CMD_SOURCE_WEB)` — same `safety_manager` gate as any other source |
| (implicit) web disconnect doesn't affect local control | True by construction | Web clients only ever read the snapshot / dispatch through `command_dispatcher`; nothing in the control loop (`profile_curve_follower`, safety timers) depends on a live WS connection |

**Desk-check verdict:** covered.

## Cenário 5: Compatibilidade com Artisan

| Step | Expected | Code path |
|------|----------|-----------|
| 2 (Artisan command ignored in Profile mode) | Read-only gate | `command_dispatcher.c` checks `session_sm_get_state()->control_mode == ROAST_MODE_PROFILE` and rejects `SAFETY_CMD_SOURCE_ARTISAN` writes |
| 3 (irreversible mode-switch warning) | Warning shown | `session_sm_switch_to_manual_artisan()` gate + confirmation UI — **only reachable from the web dashboard's "Switch to Manual" button today; no on-device (display) trigger exists yet** (documented gap, carried over from an earlier session, not newly introduced) |
| 4 (Artisan command accepted post-switch) | Accepted + safety-validated | Same `command_dispatcher` path, now allowed once `control_mode == ROAST_MODE_MANUAL_ARTISAN` |
| 5 (manual event mark visible externally) | Visible over Modbus | `roast_events_mark()` persists via `roast_telemetry_service_record_event()`; `artisan_bridge.c` (esp-modbus v2.1 TCP slave) exposes live telemetry registers Artisan polls — event-specific register exposure was not re-verified this session (Artisan's own timeline reconstruction relies on register polling cadence, not a push notification) |

**Desk-check verdict:** mostly covered; the two caveats above (no on-device
Manual-mode-switch trigger, and event-register exposure not freshly
re-verified) are genuine gaps/unknowns, not resolved by this desk check.

## Cenário 5b: Exportação de sessões concluídas

| Step | Expected | Code path |
|------|----------|-----------|
| 2 (CSV export) | Works | `GET /api/sessions/export?...&format=csv` in `history_routes.c` |
| 3 (.alog export) | Works | **Not implemented** — `history_routes.c`'s comment explicitly defers `.alog` export; only CSV exists |

**Desk-check verdict:** partial gap, already known/documented in
`tasks.md` — `.alog` (Artisan native log format) export was never built.

## Cenário 6: Testes de periféricos e calibração

| Step | Expected | Code path |
|------|----------|-----------|
| 1-4 (peripheral test screen, sensor/fan/heater tests) | Independent status per peripheral, heater test gated by confirmation | `ui_display/screens/peripheral_test.c` |
| 5 (calibration offset) | Applied to all subsequent reads | `ui_display/screens/sensor_calibration.c` + `max6675.c` applying stored NVS offset |

**Desk-check verdict:** covered (implementation reviewed to exist and wire
correctly; exact UX/confirmation wording not re-verified this session).

## Cenário 7: Atualização de firmware (OTA)

| Step | Expected | Code path |
|------|----------|-----------|
| 2 (invalid upload rejected) | Clear rejection | `ota_manager_end_and_activate()`'s `esp_ota_end()` call validates magic/header/checksum before switching boot pointer |
| 3-5 (valid upload installs, switches boot, confirms version) | Standard ESP-IDF OTA A/B flow | `ota_manager.c` + `partitions.csv`'s `ota_0`/`ota_1` scheme |
| 6 (power loss mid-install) | Falls back to current valid partition | Inherent ESP-IDF OTA A/B guarantee — bootloader only switches to the new slot after a full, validated write; an interrupted write never gets marked bootable |

**Desk-check verdict:** covered by ESP-IDF's own well-tested OTA
mechanism; this project doesn't override any of that safety behavior. See
[`README.md`](../README.md#security-considerations) for the (deliberate)
lack of image signature verification — this is a real limitation, not
resolved by this task.

## Cenário 8: Troca de idioma (i18n)

| Step | Expected | Code path |
|------|----------|-----------|
| 1 (EN default) | Default language | `i18n.c`'s default `s_current_lang = I18N_LANG_EN` |
| 2-3 (switch PT/ES) | Immediate UI update, no functionality lost | `settings_hub.c`'s Language button cycles via `i18n_set_language()` + `nav_shell_refresh_labels()` + full rebuild of the current screen |

**Desk-check verdict — HONEST SCOPE CAVEAT (carried over from T055's own
notes):** only the navigation sidebar and the Config screen's own text are
currently i18n-driven end-to-end. Screens like the roast dashboard, manual
control, profile editor, and session history still use hardcoded English
strings. Scenario 8 as literally written ("Nenhuma funcionalidade é
perdida na troca de idioma" / interface reflects the language everywhere)
is **only partially satisfied** — no functionality is lost (nothing
breaks), but most screen text does not yet translate. This should be
called out to the user explicitly, not silently marked as fully passing.

## Overall summary

Of 8 scenarios (+5b), the large majority of individual expected behaviors
have a clear, directly-traceable code path verified by static reading of
the source. Known, honestly-documented gaps found during this desk check:

1. No on-device (display) trigger for the Manual/Artisan mode switch —
   web-dashboard-only today (Cenário 5).
2. `.alog` (Artisan native format) session export was never implemented —
   CSV only (Cenário 5b).
3. i18n coverage is partial — only nav sidebar + Config screen text,
   not a full per-screen retrofit (Cenário 8).
4. DTR%/RoR numerical correctness has not been cross-checked against a
   real roast or Artisan's own computed values (Cenário 2).
5. Artisan Modbus register exposure for manually-marked events was not
   freshly re-verified this session (Cenário 5, step 5).

None of these are safety-critical (Cenário 1, the mandatory scenario, has
no identified gaps). All require real hardware / a live Artisan session to
close out — they cannot be resolved through further static code review
alone.
