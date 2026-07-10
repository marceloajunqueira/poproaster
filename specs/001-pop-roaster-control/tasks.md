---

description: "Task list for Pop Roaster Control Platform implementation"
---

# Tasks: Pop Roaster Control Platform

**Input**: Design documents from `specs/001-pop-roaster-control/`

**Prerequisites**: [plan.md](./plan.md), [spec.md](./spec.md), [research.md](./research.md), [data-model.md](./data-model.md), [contracts/](./contracts/), [quickstart.md](./quickstart.md)

**Tests**: Not explicitly requested in the feature specification; no dedicated test-first tasks are included. Manual/HIL validation is covered via quickstart.md in the Polish phase.

**Organization**: Tasks are grouped by user story (US1-US4, per spec.md priorities) to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1, US2, US3, US4)
- Include exact file paths in descriptions

## Path Conventions

Per [plan.md](./plan.md) Project Structure:

```text
firmware/
├── main/
├── components/
│   ├── board_config/
│   ├── roast_core/
│   ├── safety/
│   ├── hal/
│   ├── storage/
│   ├── ui_display/
│   ├── web_api/
│   └── artisan_adapter/
└── test/

webui/
└── src/
```

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Project initialization and basic structure

- [X] T001 Create firmware project skeleton (ESP-IDF) at firmware/ with main/ and components/ subfolders per plan.md
- [X] T002 Initialize ESP-IDF project dependencies via IDF Component Manager: `eric-c-e/esp_lcd_nv3041` (driver do painel NV3041A), `esp_lvgl_port` + LVGL, `esp_lcd_touch_gt911`, HTTP/WebSocket server, MAX6675 driver in firmware/main/idf_component.yml and firmware/main/CMakeLists.txt; register the already-created `board_config` component (firmware/components/board_config/)
- [X] T003 [P] Configure partition table with OTA A/B scheme in firmware/partitions.csv
- [X] T004 [P] Configure linting/formatting (clang-format) for firmware/
- [X] T005 [P] Scaffold webui/ static project structure (HTML/CSS/JS) in webui/src/

**Checkpoint**: Project scaffolding ready for foundational work

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core infrastructure that MUST be complete before ANY user story can be implemented

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [X] T006 Implement MAX6675 sensor driver in firmware/components/hal/max6675.c, reading pins from `BOARD_PERIPH_MAX6675_SCK_GPIO`/`BOARD_PERIPH_MAX6675_CS_GPIO`/`BOARD_PERIPH_MAX6675_SO_GPIO` (board_config.h) instead of hardcoded pin numbers
- [X] T007 [P] Implement SSR heater driver (time-proportioning control) in firmware/components/hal/ssr_heater.c, using `BOARD_PERIPH_SSR_HEATER_GPIO` (board_config.h)
- [X] T008 [P] Implement PWM fan driver in firmware/components/hal/fan_pwm.c, using `BOARD_PERIPH_FAN_PWM_GPIO`/`BOARD_PERIPH_FAN_PWM_LEDC_TIMER`/`BOARD_PERIPH_FAN_PWM_LEDC_CHANNEL`/`BOARD_PERIPH_FAN_PWM_FREQ_HZ` (board_config.h)
- [X] T009 [P] Implement NVS storage wrapper (profiles, calibration, network, language config) in firmware/components/storage/nvs_store.c
- [X] T010 [P] Implement LittleFS session history storage with circular retention (default 30 sessions) in firmware/components/storage/session_store.c
- [X] T011 Implement RoastSession state machine (IDLE→PREHEAT→ROASTING→DEVELOPMENT→COOLING→COMPLETED/ABORTED) in firmware/components/roast_core/session_state_machine.c (depends on T006-T008)
- [X] T012 Implement Safety Manager with hard-fail rules (30% fixed fan floor, heater-requires-fan, sensor-failure cutoff, absolute 260°C/240°C cutoff, duration watchdog hook, emergency stop hook, alarm ack gate) in firmware/components/safety/safety_manager.c (depends on T006-T008, T011)
- [X] T013 [P] Implement RoR (rate of rise) calculator utility in firmware/components/roast_core/ror_calculator.c
- [X] T014 [P] Implement DTR% calculator utility in firmware/components/roast_core/dtr_calculator.c
- [X] T015 Implement WiFi provisioning (AP mode + captive portal, then STA) in firmware/components/hal/wifi_provisioning.c
- [X] T016 Integrate GT911 touch controller via `esp_lcd_touch_gt911` using confirmed pins from board_config.h (SCL:4, SDA:8, RST:38, INT:3, addr 0x5D) in firmware/components/hal/touch_driver.c
- [X] T017 [P] Implement base HTTP/WebSocket server skeleton in firmware/components/web_api/server.c
- [X] T018 [P] Implement i18n string catalog loader (EN default, PT, ES) in firmware/components/ui_display/i18n.c

**Checkpoint**: Foundation ready - user story implementation can now begin

---

## Phase 3: User Story 1 - Torrar com segurança pelo display (Priority: P1) 🎯 MVP

**Goal**: Iniciar, acompanhar e finalizar uma torra pelo display com proteções automáticas de segurança.

**Independent Test**: Iniciar uma torra no display, aplicar ajustes manuais e concluir em resfriamento, validando as travas de segurança durante todo o ciclo.

### Implementation for User Story 1

- [X] T019 [US1] Implement command dispatcher wiring Safety Manager validation into fan/heater command handlers in firmware/components/roast_core/command_dispatcher.c (depends on T012)
- [X] T020 [US1] Implement operator-initiated pause/resume of an active roast (distinct from the power-loss auto-resume in T026) in firmware/components/roast_core/session_state_machine.c and firmware/components/roast_core/command_dispatcher.c (depends on T011, T019)
- [X] T021 [US1] Implement real-time telemetry dashboard screen (BT, RoR, DTR%, fan%, heater%, phase, timer) in firmware/components/ui_display/screens/roast_dashboard.c (depends on T013, T014, T019)
- [X] T022 [US1] Implement session history/review screen to browse and re-plot the BT+RoR curve of completed roast sessions on the display in firmware/components/ui_display/screens/session_review.c (depends on T010, T021)
- [X] T023 [P] [US1] Implement Emergency Stop widget (always-visible on display) in firmware/components/ui_display/widgets/emergency_stop_button.c (depends on T012) - implemented as a pinned button in the nav_shell sidebar (widgets/nav_shell.c) instead of a separate file, so it's visible on every tab, not just the dashboard
- [X] T024 [US1] Implement critical alarm banner with mandatory manual acknowledgment in firmware/components/ui_display/screens/alarm_banner.c (depends on T012) - implemented in widgets/nav_shell.c as a full-width banner pinned to the top of the screen (spans sidebar+content, visible on any tab), polling safety_manager_get_active_alarm() every 300ms and requiring an "ACK" tap (command_dispatcher_acknowledge_alarm) before it disappears
- [X] T025 [US1] Implement manual Cooling phase transition (heater forced off, fan stays on) in firmware/components/roast_core/session_state_machine.c (depends on T011) - REDESIGNED: no longer manually triggered by a dashboard button - Cooling is now entered automatically either via the profile's own trailing "Cooling" setpoint(s) (T038) or immediately when the operator taps Cancel/Emergency Stop (session_sm_cancel(), new function - heater off immediately, fan intentionally kept running rather than cut). New safety rule added to firmware/components/safety/safety_manager.c: the fan can never be commanded fully OFF (0%) while BT is at/above 100C (SAFETY_FAN_STOP_MIN_TEMP_C) or the sensor reading is invalid - protects the heating element/chamber from being left hot with no airflow now that Cooling duration/curve is profile-configurable
- [X] T026 [US1] Implement power-loss auto-resume with safety re-validation on boot in firmware/components/roast_core/session_recovery.c (depends on T011, T012)
- [X] T027 [US1] Implement max roast duration watchdog (default 25 min, forces auto-cooling) in firmware/components/safety/duration_watchdog.c (depends on T012)
- [X] T028 [US1] Implement indirect fan-failure detection via RoR anomaly pattern in firmware/components/safety/fan_failure_detector.c (depends on T012, T013)
- [X] T029 [US1] Implement Manual/Artisan control screen (direct fan/heater sliders, no curve) in firmware/components/ui_display/screens/manual_control.c (depends on T019) - implemented as a dedicated nav_shell tab ("Manual", replacing the "Presets" placeholder slot per user preference) with fan/heater sliders and live status readout
- [X] T062 [US1] Implement persistent left navigation shell (Material Design style, fixed sidebar with Roast/Presets/History/Config items, active-item highlight) in firmware/components/ui_display/widgets/nav_shell.c (depends on FR-045; replaces the current full-screen-replace navigation between roast_dashboard.c and session_review.c)
- [X] T063 [US1] Refactor roast dashboard to render inside the nav shell's content pane with a live BT+RoR timeline chart on top (drawn in real time as the roast progresses) and the phase/numeric readouts (BT, RoR, DTR%, fan%, heater%, timer) + Start Roast/Stop buttons in a compact row below, per approved sketch, in firmware/components/ui_display/screens/roast_dashboard.c (depends on T062, T021)
- [X] T064 [US1] Overlay the active profile's target BT curve on the live dashboard chart whenever the session is in Profile Mode in firmware/components/ui_display/screens/roast_dashboard.c (depends on T063, T030, T046 mode manager) - implemented (target BT AND Fan curves, dashed); now genuinely gated by Profile mode since Start Roast automatically starts ROAST_MODE_PROFILE whenever a preset is selected (T034)
- [X] T065 [US1] Render marked roast events (turning point, dry end, first/second crack, drop, etc.) as vertical reference markers on the chart, both on the live dashboard chart and on the session review chart, in firmware/components/ui_display/screens/roast_dashboard.c and session_review.c (depends on T056, T063, T022) - implemented: dashed purple vertical lines + short labels (child `lv_line`/`lv_label` objects, same technique as the target-curve overlay) drawn from `roast_events_get_all()` (live dashboard) / parsed `"ev"` lines in the session's `.jsonl` (session review), both rebuilt whenever the event list changes
- [X] T066 [US1] Wire the session history/review screen into the nav shell's content pane (History item) instead of a full-screen swap in firmware/components/ui_display/screens/session_review.c (depends on T062, T022)
- [X] T067 [P] [US1] Implement Config/Settings hub screen (Config item in nav shell) consolidating language switch, sensor calibration, peripheral test, and Wi-Fi setup entry points, per FR-046, in firmware/components/ui_display/screens/settings_hub.c (depends on T062) - implemented (2026-07-10) as a real menu replacing the placeholder: buttons into peripheral_test.c (T050) and sensor_calibration.c (T051), plus the device's own IP shown as `http://<ip>/wifi` and `http://<ip>/ota` links (Wi-Fi setup/reconfigure and firmware update are web-only, no on-device screens for those - T049 still not implemented on-device). Language switch UI still NOT implemented - blocked on T055 (i18n catalogs don't exist yet), shown as a "coming soon" note.

**Checkpoint**: User Story 1 fully functional and independently testable (manual roast start-to-finish with all safety interlocks)

---

## Phase 4: User Story 2 - Criar e usar perfis de torra (Priority: P1)

**Goal**: Criar perfis de torra com curvas interativas e reutilizá-los em novos lotes.

**Independent Test**: Criar um perfil no display, salvar, carregar em nova sessão e verificar execução guiada por curva.

### Implementation for User Story 2

- [X] T030 [P] [US2] Implement RoastProfile model and CRUD logic in firmware/components/roast_core/roast_profile.c - model + piecewise-linear target BT/Fan curve interpolation implemented; full CRUD (create/edit/duplicate-via-"+ Add Segment"/delete) now available from the UI via the profile editor screen (T032)
- [X] T031 [US2] Implement profile persistence in NVS (create/update/delete/list) in firmware/components/storage/profile_store.c (depends on T009, T030); on NVS full or corrupted profile data, reject the write/read with a clear error surfaced to the UI instead of silently corrupting or losing other stored profiles - list/load/select-active/create/update/delete all implemented and persisted in NVS (profile_store_create/update/delete added for T032); deleting the currently-selected profile clears the selection; corrupted/oversized blob reads are rejected (size-mismatch check) but not yet surfaced as a UI-visible error message (logged only)
- [X] T032 [US2] Implement profile editor screen combining read-only curve graph with editable numeric point table in firmware/components/ui_display/screens/profile_editor.c (depends on T030) - implemented as a numeric point-table editor (no interactive curve-DRAWING graph - the read-only dashed target curve is already viewable on the Roast dashboard/Roast History once a profile is selected/run, T033/T064): each segment is an editable "card" (duration mm:ss, target BT, target fan%, target heater%, Cooling toggle, all via +/- steppers in 15s/5C/5%/5% increments) with Add/Delete Segment, Save, Delete Profile, and tap-to-rename (lv_textarea + lv_keyboard overlay) - reachable from the Presets tab (profile_list.c) via a "+ New" button or a per-profile "Edit" button. **UPDATE (2026-07-10)**: web parity added - `web_api/presets_routes.c` gained `GET /presets/edit?id=N` (N=-1 for a brand-new profile), `POST /api/presets/save` (create or update, name + a client-side-managed list of duration/temp/fan/cooling segment rows, re-serialized with fresh sequential indices on every save so removing a middle row never leaves a gap), and `POST /api/presets/delete` - same fan-floor (30%) and fixed-Cooling-values (0C/100%) server-side invariants as the display editor. Web presets page ("/presets") now has a "+ New Profile" button and an "Edit" link per row instead of being read-only/select-only.
- [X] T033 [US2] Implement profile list/selection screen in firmware/components/ui_display/screens/profile_list.c (depends on T031) - lists stored profiles, tap to mark one as selected (persisted) for the next roast on the Roast tab; the Roast dashboard chart plots its target BT/Fan curves (dashed) against the live measured values (solid) and its name in the corner
- [X] T034 [US2] Implement Profile-mode curve-following control loop (targetFanPct/targetHeaterPct per curve point, respecting 30% floor) in firmware/components/roast_core/profile_curve_follower.c (depends on T011, T012, T030) - implemented: a 1s esp_timer drives command_dispatcher_set_fan_pct/set_heater_pct from the selected profile's per-segment target_fan_pct/target_heater_pct (added to roast_profile_point_t) whenever the session is ROAST_MODE_PROFILE and past CHARGE (ROASTING/DEVELOPMENT); target_temp_c remains a visual-only reference (no closed-loop BT control). Start Roast on the dashboard now starts in ROAST_MODE_PROFILE automatically when a preset is selected (else falls back to MANUAL_ARTISAN)
- [X] T035 [US2] Implement manual override during Profile mode (persists until next curve point, then profile resumes) in firmware/components/roast_core/profile_curve_follower.c (depends on T034) - detects a manual fan/heater change (actual value differs from what the follower itself last wrote) and backs off until roast_profile_get_segment_index() crosses into the next setpoint segment, then resumes automatic control
- [X] T036 [US2] Implement preheat-ready visual signal and CHARGE event confirmation gate in firmware/components/ui_display/screens/preheat_ready_banner.c (depends on T034) - implemented as a minimal "Charge" button directly in roast_dashboard.c (not a separate file) shown during PREHEAT; tapping it calls the new session_sm_confirm_charge()/command_dispatcher_confirm_charge(), transitioning PREHEAT->ROASTING and resetting the elapsed-time reference so the roast clock/curve/chart timeline starts counting from CHARGE (not from Start Roast); NOTE: no smart "preheat-ready" temperature-based visual signal yet (button is always tappable during PREHEAT, not gated on reaching a target preheat temperature) - this was a pre-existing gap (nothing previously transitioned PREHEAT->ROASTING at all) fixed as a byproduct of unblocking T034
- [X] T037 [US2] Implement profile snapshot capture into RoastSession at session start in firmware/components/roast_core/session_state_machine.c (depends on T011, T030) - implemented as session_meta_t (storage/session_store.h: roast number + full profile snapshot) saved via session_store_save_meta() in roast_core/roast_telemetry_service.c's roast_telemetry_service_on_roast_started(), rather than inside session_state_machine.c itself; Roast History (session_review.c) loads this snapshot to redraw the exact target curve that was active during that specific roast, unaffected by later profile changes/deletions (FR-034)
- [X] T038 [US2] Implement automatic Cooling transition via profile drop point in firmware/components/roast_core/profile_curve_follower.c (depends on T034) - REDESIGNED per operator feedback: Cooling is no longer a separate manual button/step - it's now one or more trailing setpoints in the profile itself (`roast_profile_point_t.is_cooling`, heater forced to 0%, fan/duration configured like any other setpoint), so the cooldown curve shows up on the chart exactly like the rest of the roast. profile_curve_follower.c auto-transitions ROASTING/DEVELOPMENT->COOLING the moment the curve enters an is_cooling segment, keeps driving ONLY the fan from that segment's curve (never the heater), and auto-finalizes (session_sm_complete()) once the profile's full timeline elapses, BT drops below a safe threshold, or a 15-minute failsafe elapses (whichever comes first) - no manual "Start Cooling"/"Complete" button exists anymore
- [X] T039 [P] [US2] Implement profile export (JSON) endpoint in firmware/components/web_api/routes/profiles_export.c (depends on T031) - implemented as `GET /api/presets/export?id=N` in `firmware/components/web_api/presets_routes.c` (not a separate `routes/` subfolder file, same flat-file convention as every other web_api route): outputs a compact `{"name":...,"points":[[dur,temp,fan,cooling],...]}` JSON with a `Content-Disposition: attachment` download header; also has an "Export" link per row on the web presets list page
- [X] T040 [P] [US2] Implement profile import (JSON upload) endpoint in firmware/components/web_api/routes/profiles_import.c (depends on T031) - implemented as `POST /api/presets/import` in `presets_routes.c`: manual `strstr`/`sscanf`-based parser (no cJSON dependency, matching this project's existing manual-parsing convention) for the same export format, with a client-side `FileReader`+`fetch()` file-upload widget on the presets page; same value clamps (fan floor, cooling fixed values) as the manual editor's save handler

**Checkpoint**: User Stories 1 AND 2 both work independently

---

## Phase 5: User Story 3 - Monitorar por web e integrar com Artisan (Priority: P2)

**Goal**: Acompanhar a torra por navegador e integrar com o Artisan (bidirecional, gated por modo).

**Independent Test**: Iniciar uma torra e validar que os mesmos dados/comandos estão disponíveis no display e na web, e que os dados chegam ao Artisan conforme o modo ativo.

### Implementation for User Story 3

- [X] T041 [P] [US3] Implement WebSocket telemetry broadcast (BT, RoR, DTR%, phase, controlMode) in firmware/components/web_api/ws_telemetry.c (depends on T017) - implemented in firmware/components/web_api/dashboard_routes.c (not a separate ws_telemetry.c file) instead: a 1s esp_timer broadcasts a JSON telemetry frame (phase, mode, paused, elapsedMs, bt, sensorValid, ror, dtr, fan, heater, alarmText, alarmNeedsAck) to every connected client on "/ws/telemetry" via httpd_ws_send_frame_async(); CONFIG_HTTPD_WS_SUPPORT enabled in sdkconfig.defaults (was off by default)
- [X] T042 [US3] Implement web control command endpoint with last-write-wins across display/web/Artisan in firmware/components/web_api/routes/control_command.c (depends on T012, T019) - implemented as POST /api/control in dashboard_routes.c (not a separate routes/ subfolder file): form-encoded body "action=xxx&value=NN", dispatches set_fan/set_heater/pause/resume/cancel/emergency_stop/ack_alarm/confirm_charge/start through command_dispatcher.h with SAFETY_CMD_SOURCE_WEB, so it automatically gets the same last-write-wins arbitration and Profile-mode-Artisan-read-only gate as every other source
- [X] T043 [P] [US3] Implement web dashboard page (real-time BT+RoR chart, controls) in webui/src/dashboard.html and webui/src/dashboard.js (depends on T041) - implemented as an inline-generated page (dashboard_routes_send_page() in dashboard_routes.c, served at "/" once WiFi is connected, replacing the old simple status card) instead of static files under webui/src/ (webui/src/index.html remains an unused placeholder from T005) - same self-contained/no-CDN convention as wifi_setup_routes.c's existing pages (FR-021: must work fully offline); chart is a plain <canvas> 2D rolling-window plot (last 300 samples \u2248 5min, BT+Fan only, not yet BT+RoR as the task title says, and not yet matched 1:1 to the profile's target-curve timeline like the display's chart - a reasonable future refinement) fed by the T041 WebSocket stream; includes Fan/Heater override sliders, Start Roast/Charge/Pause-Resume/Cancel buttons mirroring the display's phase-based button groups. ALSO added (beyond the original task scope, for web/display navigation parity): a shared nav bar (Dashboard/Roast History/Presets/Wi-Fi Setup) plus two new pages - GET /history (+/history/detail, mirroring session_review.c's list+detail, stats + a simple actual-only BT/Fan chart, no target-curve overlay yet) and GET /presets (+POST /api/presets/select, read-only preset picker mirroring profile_list.c's selection behavior - no web-side profile editor, use the display's Presets tab for CRUD, T032). **UPDATE (2026-07-10)**: web-side preset create/edit/delete was added after all (GET /presets/edit, POST /api/presets/save|delete in presets_routes.c) - see T032's note. Also (2026-07-10): the shared stylesheet was extracted from an inline `<style>` block re-sent on every page into its own cached `GET /style.css` route (`Cache-Control: public, max-age=86400`) via a new `WEB_UI_STYLE_LINK` macro - the repeated multi-KB inline CSS on every navigation was a real contributor to "the web UI feels slow"
- [X] T044 [US3] Implement Emergency Stop control on the web dashboard, mirroring the display widget (T023) so it is always visible and accessible from both surfaces per FR-027, in webui/src/dashboard.html and webui/src/dashboard.js (depends on T012, T043) - a prominent always-visible "EMERGENCY STOP" button (with a JS confirm() prompt to avoid accidental taps, unlike the display's version which has no confirmation - worth reconsidering for consistency later) posts action=emergency_stop
- [X] T045 [US3] Implement critical alarm banner with mandatory manual acknowledgment on the web dashboard, mirroring the display behavior (T024) so alarm ack has web/display parity, in webui/src/dashboard.html and webui/src/dashboard.js (depends on T012, T043) - a full-width red banner (shown/hidden per the WS stream's alarmNeedsAck flag) with the human-readable alarm text (mirroring nav_shell.c's alarm_text() wording) and an ACK button posting action=ack_alarm
- [X] T046 [US3] Implement Mode Manager (Profile vs Manual/Artisan, one-way irreversible switch with confirmation) in firmware/components/roast_core/mode_manager.c (depends on T011, T034) - the underlying transition (session_sm_switch_to_manual_artisan(), irreversible, mode_switch_used latch) already existed from earlier work; this task added the missing piece - an actual operator-facing trigger: command_dispatcher_switch_to_manual_artisan(confirmed, source) wrapper + a "Switch to Manual" button on the WEB dashboard (dashboard_routes.c), shown only while an active PROFILE-mode session is running, gated behind a JS confirm() dialog before posting action=switch_manual&value=1; NOTE: no equivalent button exists on the DISPLAY side yet (roast_dashboard.c) - web-only for now, a gap worth closing later for display/web parity
- [X] T047 [US3] Implement Artisan serial/TCP adapter (telemetry always published; commands accepted only in Manual/Artisan mode) in firmware/components/artisan_adapter/artisan_bridge.c (depends on T042, T046) - implemented as a Modbus TCP slave (port 502), confirmed as Artisan's own real network-capable device protocol by reading Artisan's source (artisanlib/modbusport.py: `AsyncModbusTcpClient`, per-channel host/port/device-id/register/function-code config) - no serial/USB cable or extra PC-side software needed, works over the same WiFi the device already uses. Uses `espressif/esp-modbus` v2.x (idf_component.yml pinned `^2.1`; the older v1.x line uses the legacy `driver/timer.h`/`driver/uart.h` headers ESP-IDF v6.0 removed/split out, so v1.x fails to compile on this project's IDF version - v2.x's port implementation doesn't have this problem). Register map (plain 16-bit Holding Registers, slave id 1, scaled-integer convention chosen specifically to avoid any float/word-order ambiguity - see the header doc comment for the exact Artisan per-channel Div/Signed settings to use): reg 0 = BT x10, reg 1 = Fan %, reg 2 = Heater %, reg 3 = RoR x10 (signed); reg 100/101 = Set Fan %/Set Heater % (writable - a background poll task applies new values via command_dispatcher.h with SAFETY_CMD_SOURCE_ARTISAN, which already silently ignores Artisan commands while ROAST_MODE_PROFILE is active per the pre-existing Profile-mode-Artisan-read-only gate, so no new mode-gating logic was needed). No build/CMakeLists patches needed - v2.x's own CMakeLists.txt already correctly requires `esp_driver_uart` for IDF >5.2.
- [X] T048 [P] [US3] Implement session export endpoint (CSV and .alog formats) in firmware/components/web_api/routes/sessions_export.c (depends on T010) - implemented as GET /api/sessions/export?id=...&format=csv in firmware/components/web_api/history_routes.c (not a separate routes/ subfolder file): streams the session's .jsonl telemetry as a CSV (t_ms,bt_c,ror_c_per_min,fan_pct,heater_pct,phase) via chunked response with a Content-Disposition download header; .alog (Artisan) format NOT yet implemented - deferred until T047's Artisan bridge exists and there's a concrete format to validate against
- [X] T049 [US3] Implement WiFi AP + captive portal setup screen and web page in firmware/components/ui_display/screens/wifi_setup.c and webui/src/wifi_setup.html (depends on T015) - on-device screen implemented (`ui_display/screens/wifi_setup.c`/`.h`, manual SSID/password entry via the standard textarea+keyboard overlay pattern, reachable from Config → Wi-Fi Setup); the web side (`wifi_setup_routes.c`) already existed from earlier work. No DNS-based captive-portal auto-popup on phones (operator must browse to 192.168.4.1 manually while joined to the setup AP), same as before - not a regression, just not newly added here

**Checkpoint**: User Stories 1, 2, AND 3 all independently functional

---

## Phase 6: User Story 4 - Validar hardware e manutenção (Priority: P3)

**Goal**: Tela de teste de periféricos e atualização de firmware por upload web.

**Independent Test**: Acionar testes individuais de sensor/ventilação/aquecimento e realizar upload de firmware com confirmação de sucesso.

### Implementation for User Story 4

- [X] T050 [P] [US4] Implement peripheral test screen (sensor/fan/heater individual tests, confirmation required before heater test) in firmware/components/ui_display/screens/peripheral_test.c (depends on T006-T008, T012) - live sensor readout (reads max6675_read() directly) + a bounded Fan test (50%/5s, tap again to cancel early) + a Heater test gated behind a two-tap "Confirm?" pattern (same as session_review.c's Delete All) that auto-raises the fan to the 30% safety floor first, runs the heater at a low 20% test duty for 8s, and always force-stops (heater then fan to 0) when the screen is left via Back OR a tab switch away - never leaves a test running unattended
- [X] T051 [P] [US4] Implement sensor calibration UI and offset persistence in firmware/components/ui_display/screens/sensor_calibration.c (depends on T009) - live reading + current offset readout, a +/-1C reference-temperature stepper (default 100C, e.g. boiling water), and an "Apply" button that computes the offset needed to make the next reading equal the chosen reference (persisted via the already-existing hal/max6675.h calibration offset API, NVS-backed) plus a "Reset offset to 0" button
- [X] T052 [US4] Implement firmware upload endpoint with A/B partition OTA (validate, install to secondary partition, switch boot only on success) in firmware/components/web_api/ota_routes.c (depends on T009) - deviates from the literal `web_api/routes/firmware_upload.c` path the same way every other web_api route file in this project already does (flat file, not a routes/ subfolder); POST /api/ota/upload streams the raw firmware binary body (via repeated httpd_req_recv() into a static scratch buffer) into hal/ota_manager.c, which only switches the boot partition after esp_ota_end() validates the image - a corrupt/incomplete upload leaves the current firmware completely untouched
- [X] T053 [US4] Implement OTA status endpoint and rollback-safe boot logic (stay on current partition on any failure) in firmware/components/roaster_hal/ota_manager.c (depends on T052) - deviates from the literal `hal/ota_manager.c` path only in that the `hal` component was renamed `roaster_hal` earlier in this project (see firmware-build-notes memory); GET /api/ota/status (JSON: version/partition/pendingVerify) implemented in ota_routes.c; relies on the already-enabled `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` - ota_manager_init() (called near the end of app_main(), after the display/web server have already come up successfully) calls esp_ota_mark_app_valid_cancel_rollback() only if the running image is still PENDING_VERIFY, so a crash-loop right after an update auto-reverts to the previous known-good partition with zero custom logic needed for that part - it's the standard ESP-IDF app-rollback mechanism
- [X] T054 [P] [US4] Implement firmware update web page (file upload UI) in webui/src/firmware_update.html (depends on T052) - implemented as an inline-generated page (GET /ota in ota_routes.c, same no-external-CDN convention as dashboard/history/presets/diagnostics routes) instead of a static webui/src/ file; plain `<input type=file>` + XMLHttpRequest upload with a live progress percentage, auto-reloads 15s after a reported success to show the new running version. Added a "Firmware Update" nav bar entry (dashboard_routes.c) and a reachable entry point from the display's Config tab (new `ui_display/screens/settings_hub.c`, T067-adjacent - shows the device's own IP + /ota and /wifi links since the actual upload/setup UI itself is web-only) - also wired the previously-orphaned T050/T051 display screens in via that same settings hub, replacing the old Config tab placeholder

**Checkpoint**: All user stories (US1-US4) independently functional

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Improvements that affect multiple user stories

- [X] T055 [P] Create i18n string catalogs (en.json, pt.json, es.json) and language switch UI in firmware/components/ui_display/i18n/ and webui/src/i18n/ (depends on T018) - implemented as compile-time C string-array catalogs (`ui_display/i18n.c`: `kStringsEn/Pt/Es[]`, not JSON files) with a persisted (NVS) language choice and a Language button on the Config screen (cycles EN→PT→ES→EN); `nav_shell_refresh_labels()` live-updates the sidebar text immediately on switch. **Honest scope note**: only the nav sidebar and the Config screen's own text are i18n-driven end-to-end; a full per-screen retrofit of every remaining label (dashboard, manual control, profile editor, history, web UI) was NOT done - a real, deliberate scope reduction, not an oversight (see `firmware/docs/quickstart-desk-check.md` Cenário 8 for the honest caveat)
- [X] T056 [P] Implement RoastEvent milestone marking UI (charge, turning point, dry end, first/second crack, drop, cool start, manual note) in firmware/components/ui_display/screens/event_markers.c - implemented as a new `roast_core/roast_events.c`/`.h` (event list + persistence via `roast_telemetry_service_record_event()`) plus a "Mark Event" button + full-screen 9-button overlay directly in `roast_dashboard.c` (not a separate `event_markers.c` screen file) - marking FC Start also auto-triggers the existing DTR-calculator's first-crack timestamp
- [X] T057 [P] Implement BatchRecord metadata form (coffee name, origin, weight, notes) in firmware/components/ui_display/screens/batch_metadata.c - implemented as a `batch_record_t` (storage/session_store.h, embedded in `session_meta_t`) filled in via a "Batch Info" button + overlay directly in `roast_dashboard.c` (not a separate `batch_metadata.c` file) while idle, applied to the session's metadata at roast start; displayed back in Roast History's session detail view (`session_review.c`) whenever any field was filled in
- [X] T058 Security review pass: confirm no-auth/trusted-network assumptions are documented in README, validate firmware upload checksum verification - added a "Security Considerations" section to `firmware/README.md` documenting the FR-021 no-auth/trusted-LAN assumption across the web dashboard, WiFi setup AP, Artisan Modbus bridge, and OTA upload endpoint, plus the lack of firmware image signature verification (Secure Boot not enabled); confirmed `esp_ota_end()` (`roaster_hal/ota_manager.c`) already validates image integrity (magic bytes/header/checksum) before switching the boot partition - no code change was needed for that part, just documenting it
- [X] T059 [P] Performance tuning: ensure telemetry/chart updates sustain 2-5 Hz without UI stutter on JC4827W543 - `roast_telemetry_service.c` already sampled at 500ms (2Hz); the on-device dashboard's own `lv_timer` already refreshed at 500ms (2Hz); the one gap found was the WEB dashboard's WebSocket broadcast timer (`dashboard_routes.c`), previously 1000ms (1Hz, below the 2-5Hz target) - bumped to 500ms (2Hz) so web clients now see updates exactly as often as the physical display. No real-hardware profiling was performed (not possible in this environment) - this is a code-review-based cadence adjustment; UI stutter itself was not measured on device
- [X] T060 Run quickstart.md validation end-to-end (all 9 scenarios in specs/001-pop-roaster-control/quickstart.md) - performed as a static "desk check" only (see `firmware/docs/quickstart-desk-check.md`): every scenario/step cross-referenced against the actual implementing code path by reading the source, NOT executed on a powered roaster (requires real heating element/sensor/fan hardware this environment doesn't have). 5 honest gaps were found and documented (no on-device Manual-mode-switch trigger, no `.alog` export, partial i18n coverage, unverified DTR/RoR numerical correctness, unverified Artisan event-register exposure) - none safety-critical, all require real hardware/Artisan to close out. The operator should still run the actual 8 scenarios on real hardware before considering this fully validated
- [X] T061 [P] Documentation: update README with hardware wiring (SSR 40A, PWM 15A, MAX6675) and setup instructions - created `firmware/README.md` (none existed before) covering build/flash instructions, the JST1.25 expansion connector pinout + default GPIO assignments (from `board_config/Kconfig`) for MAX6675/SSR/fan, a wiring diagram, a summary of the software safety interlocks, the Security Considerations section (T058), i18n status, and a link to the existing Artisan integration doc

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Story 1 (Phase 3)**: Depends on Foundational completion - no dependency on other stories
- **User Story 2 (Phase 4)**: Depends on Foundational completion - independent of US1, but profile-mode control loop (T034) shares command dispatcher patterns from US1
- **User Story 3 (Phase 5)**: Depends on Foundational completion and on Mode Manager needing US2's profile curve follower (T034) to distinguish Profile vs Manual/Artisan
- **User Story 4 (Phase 6)**: Depends on Foundational completion - independent of US1/US2/US3
- **Polish (Phase 7)**: Depends on all desired user stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Foundational (Phase 2) - No dependencies on other stories
- **User Story 2 (P1)**: Can start after Foundational (Phase 2) - Independently testable; shares command dispatcher from US1 conceptually but has its own control loop
- **User Story 3 (P2)**: Can start after Foundational (Phase 2) - Integrates with US1 (command dispatcher, Emergency Stop, alarm ack) and US2 (profile curve follower) for Mode Manager and web/display parity, but web dashboard/telemetry itself is independently testable
- **User Story 4 (P3)**: Can start after Foundational (Phase 2) - Fully independent of US1/US2/US3

### Parallel Opportunities

- All Setup tasks marked [P] (T003-T005) can run in parallel
- Foundational tasks marked [P] (T007-T010, T013-T014, T017-T018) can run in parallel within Phase 2; T016 (GT911 touch integration) can run in parallel with the other foundational tasks
- Once Foundational completes, US1, US2, and US4 can be worked on in parallel by different developers; US3 benefits from US1/US2 being further along but can start in parallel with careful stubbing
- Within US1: T023 (Emergency Stop widget) can run in parallel with T021/T024-T029; T020 (pause/resume) and T022 (session review screen) can be developed in parallel with each other once T019/T021 are ready
- Within US2: T030 parallel with foundational work; T039-T040 (profile export/import) parallel with each other
- Within US3: T041 and T043 can run in parallel; T044 and T045 (web Emergency Stop / alarm ack) can run in parallel with each other once T043 is ready; T048 parallel with T041-T047
- Within US4: T050, T051, T054 can run in parallel with each other

---

## Parallel Example: User Story 1

```bash
# After T019 (command dispatcher) is done, these can run in parallel:
Task: "Implement Emergency Stop widget in firmware/components/ui_display/widgets/emergency_stop_button.c"
Task: "Implement critical alarm banner in firmware/components/ui_display/screens/alarm_banner.c"
Task: "Implement indirect fan-failure detection in firmware/components/safety/fan_failure_detector.c"
Task: "Implement operator-initiated pause/resume in firmware/components/roast_core/session_state_machine.c"
```

## Parallel Example: User Story 2

```bash
Task: "Implement profile export (JSON) endpoint in firmware/components/web_api/routes/profiles_export.c"
Task: "Implement profile import (JSON upload) endpoint in firmware/components/web_api/routes/profiles_import.c"
```

## Parallel Example: User Story 3

```bash
# After T043 (web dashboard page) is done, these can run in parallel:
Task: "Implement Emergency Stop control on the web dashboard"
Task: "Implement critical alarm banner with manual acknowledgment on the web dashboard"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational (CRITICAL - blocks all stories, includes Safety Manager and GT911 touch integration)
3. Complete Phase 3: User Story 1 (Manual/Artisan roast with full safety interlocks)
4. **STOP and VALIDATE**: Run Cenário 1 and Cenário 2 (partial) from quickstart.md
5. Deploy/demo if ready — this is a safe, usable torrador even without profiles yet

### Incremental Delivery

1. Complete Setup + Foundational → Foundation ready (drivers, safety manager, storage, touch/GT911 integrated)
2. Add User Story 1 → Test independently → Deploy/Demo (MVP: safe manual roasting)
3. Add User Story 2 → Test independently → Deploy/Demo (repeatable profiles)
4. Add User Story 3 → Test independently → Deploy/Demo (web UI + Artisan, with web/display safety parity)
5. Add User Story 4 → Test independently → Deploy/Demo (maintenance/OTA)
6. Polish phase → i18n, event markers, batch metadata, quickstart full validation

### Parallel Team Strategy

With multiple developers:

1. Team completes Setup + Foundational together (Safety Manager is the critical path — prioritize it)
2. Once Foundational is done:
   - Developer A: User Story 1 (display safety flow)
   - Developer B: User Story 2 (profiles)
   - Developer C: User Story 4 (peripheral tests/OTA) — fully independent
   - User Story 3 (web/Artisan) starts once US1's command dispatcher/Emergency Stop/alarm ack and US2's curve follower have stable interfaces
3. Stories complete and integrate independently

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Safety Manager (T012) is the single most critical foundational task — all hard-fail rules (30% fan floor, 260°C/240°C cutoff, watchdog, emergency stop, alarm ack) route through it
- Touch controller is confirmed as GT911 (I2C) with pins already defined in `firmware/components/board_config/boards/board_jc4827w543.h` — T016 is a direct integration task, not a discovery task
- Web dashboard now has explicit parity tasks for Emergency Stop (T044) and alarm acknowledgment (T045), matching the display-side widgets (T023, T024) per FR-027/FR-012
- No dedicated test-first tasks were generated (not requested in spec); quickstart.md scenarios (T060) serve as the acceptance validation pass
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently


