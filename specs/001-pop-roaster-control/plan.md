# Implementation Plan: Pop Roaster Control Platform

**Branch**: `[001-pop-roaster-control]` | **Date**: 2026-07-06 | **Spec**: `specs/001-pop-roaster-control/spec.md`

**Input**: Feature specification from `specs/001-pop-roaster-control/spec.md`

## Summary

Construir uma plataforma embarcada para controle seguro de torra em pipoqueira elétrica (1200W), com interface local em display ESP32 (inicialmente JC4827W543), web UI em tempo real e compatibilidade bidirecional com Artisan (gated por modo). A abordagem técnica separa domínio de torra/safety das camadas de UI e integração para viabilizar expansão futura de displays, idiomas e modos de operação sem reescrever regras críticas. Após 12 sessões de clarificação (41 requisitos funcionais), o escopo agora inclui: dois modos de torra (Perfil e Manual/Artisan, com transição unidirecional), múltiplas camadas de segurança (piso fixo de ventilação, corte absoluto de temperatura, watchdog de duração, detecção indireta de falha de ventilação via RoR, parada de emergência dedicada, reconhecimento manual de alarmes), calibração de sensor, OTA com esquema A/B seguro, exportação/importação de dados (sessões e perfis), curva de RoR e DTR% em tempo real, marcos de torra completos (charge → dry end → first/second crack → drop → cooling) e i18n (EN/PT/ES, EN padrão).

## Technical Context

**Language/Version**: C/C++ (ESP-IDF estável compatível com ESP32-S3)

**Primary Dependencies**:
- ESP-IDF (FreeRTOS, NVS, OTA com esquema de partição A/B)
- `board_config` (componente próprio, `firmware/components/board_config/`) — abstração de hardware por board com Kconfig para pinos de periféricos externos
- `esp_lcd_nv3041` (componente do IDF Component Registry, `eric-c-e/esp_lcd_nv3041`) para o driver do painel NV3041A via `esp_lcd_panel_io_spi`
- `esp_lvgl_port` (integração oficial Espressif entre `esp_lcd` e LVGL) + LVGL (UI local, gráfico de curva BT+RoR, i18n de strings)
- `esp_lcd_touch_gt911` (família `esp_lcd_touch`) para o touch capacitivo GT911 confirmado via I2C
- Driver MAX6675 (aquisição de temperatura, com offset de calibração aplicado em software)
- Stack HTTP/WebSocket do ESP-IDF (web UI, telemetria em tempo real, upload de firmware/perfis)
- Adapter de protocolo Artisan (serial USB e/ou bridge TCP local, bidirecional apenas em Modo Manual/Artisan)

**Storage**:
- NVS para perfis, calibração, configuração de rede e idioma
- File system embarcado (LittleFS/SPIFFS) para histórico de sessões (retenção circular, padrão 30 sessões) e snapshots de perfil por sessão

**Testing**:
- Unit tests para regras de domínio (state machine de fases/modos, safety manager, cálculo de RoR/DTR)
- Integration/HIL para periféricos (sensor, SSR, PWM, detecção indireta de falha de ventilação)
- Contract tests para API web, exportação/importação (CSV/.alog/JSON) e adapter de compatibilidade Artisan (leitura e escrita)

**Target Platform**: JC4827W543 (Guition) — ESP32-S3-WROOM-1-N4R8 (8MB PSRAM, 4MB Flash), display 480x272 com driver NV3041A via QSPI, touch capacitivo GT911 via I2C (SCL:4, SDA:8, RST:38, INT:3)

**Project Type**: Sistema embarcado com UI local + web app embarcada + adapter de integração externa bidirecional

**Performance Goals**:
- Atualização de telemetria e gráfico (BT + RoR) em 2-5 Hz sem travamentos perceptíveis
- Aplicação de comandos críticos (incluindo Parada de Emergência) com latência local percebida abaixo de 200 ms

**Constraints**:
- Aquecimento nunca pode operar com ventilação abaixo do piso fixo de 30%
- Falha de leitura do sensor ou detecção indireta de falha de ventilação (via RoR) deve forçar aquecimento OFF
- Corte absoluto de segurança a 260°C (aviso prévio a 240°C), independente do perfil ou modo ativo
- Watchdog de duração máxima de torra (padrão 25 min) força resfriamento automático
- Controle local deve continuar funcional mesmo sem rede/web
- Artisan só pode enviar comandos de controle quando a sessão estiver em Modo Manual/Artisan; em Modo Perfil é somente leitura
- Troca de Modo Perfil → Manual/Artisan é permitida mid-roast, mas irreversível na mesma sessão
- Alarmes críticos exigem reconhecimento manual do operador (sem buzzer; sinalização exclusivamente visual)
- Memória embarcada limitada, exigindo retenção rotativa de histórico e snapshot de perfil por sessão

**Scale/Scope**:
- 1 equipamento por instância
- 1 operador por sessão de torra
- Múltiplos perfis (com export/import JSON) e histórico local com retenção configurável (padrão 30 sessões)
- Interface em 3 idiomas (EN padrão, PT, ES)

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

### Pre-Phase 0

- Constitution file (`.specify/memory/constitution.md`) at the time of initial planning contained only unfilled placeholders (no enforceable principles).
- Gate status at that time: **PASS PROVISÓRIO** (no formal rules to block planning).
- Mitigation applied in the plan: safety-by-design, 3-level testability, and domain/UI separation.

### Post-Phase 1 Re-check (updated after constitution was ratified)

- Constitution ratified 2026-07-06 (v1.0.0) with 5 core principles: Safety-First, Domain/Interface Separation, Configuration Over Hardcoding, Data Integrity and Continuity, Spec-Driven Development — see `.specify/memory/constitution.md`.
- Re-checked this plan and `tasks.md` against all 5 principles:
  - **I. Safety-First**: PASS — single `safety_manager.c` (T012) validates every command from display/web/Artisan; alarms require manual ack (T024/T045); Emergency Stop mirrored on display and web (T023/T044).
  - **II. Domain/Interface Separation**: PASS — `board_config` component isolates hardware; `roast_core`/`safety` never reference display/web/Artisan specifics.
  - **III. Configuration Over Hardcoding**: PASS — external peripheral GPIOs configurable via Kconfig (`board_config/Kconfig`); fixed board hardware (display/touch) defined once per board header.
  - **IV. Data Integrity and Continuity**: PASS — profile snapshot per session (T037), A/B safe OTA (T052/T053), profile/session export-import (T039/T040/T048).
  - **V. Spec-Driven Development**: PASS — spec.md went through 9 `/speckit.clarify` sessions before this plan; `/speckit.analyze` was run and its findings (C1-C7) were remediated in `tasks.md`.
- Re-check status: **PASS** (no violations against the ratified constitution).

### Constitution Amendment Trigger

- If the trust boundary changes (e.g., exposing the device beyond a local trusted network), the Security & Trust Boundaries section of the constitution requires re-running `/speckit.clarify` before implementation — this is a standing amendment trigger, not yet activated in this feature's v1 scope.

## Phase 0 Research Output

Pesquisa consolidada em `specs/001-pop-roaster-control/research.md` com decisões para:
- stack de firmware e arquitetura por camadas
- modelagem de safety/intertravamento (piso fixo de ventilação, corte absoluto de temperatura, watchdog de duração, detecção indireta de falha de ventilação via RoR, parada de emergência, reconhecimento manual de alarmes)
- compatibilidade com Artisan bidirecional gated por Modo Perfil/Manual-Artisan
- estratégia de web UI em tempo real
- persistência, retenção e snapshot de perfil por sessão
- OTA com esquema A/B e rollback seguro
- calibração de sensor, exportação/importação de dados (sessões e perfis)
- curva de RoR, DTR% e marcos de torra completos
- suporte a i18n (EN padrão, PT, ES)

## Phase 1 Design Output

- Modelo de dados: `specs/001-pop-roaster-control/data-model.md`
- Contratos:
  - `specs/001-pop-roaster-control/contracts/web-api.yaml`
  - `specs/001-pop-roaster-control/contracts/artisan-compatibility.md`
- Guia de validação: `specs/001-pop-roaster-control/quickstart.md`

### Agent Context Update

- Tentativa de execução do script de atualização: `.specify/scripts/powershell/update-agent-context.ps1`
- Resultado: **indisponivel neste template** (`MISSING:update-agent-context.ps1`)
- Impacto: nenhum bloqueio para planejamento; etapa de tasks/implement pode seguir normalmente.

## Project Structure

### Documentation (this feature)

```text
specs/001-pop-roaster-control/
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   ├── web-api.yaml
│   └── artisan-compatibility.md
└── tasks.md
```

### Source Code (repository root)

```text
firmware/
├── main/
├── components/
│   ├── board_config/     # Kconfig + headers de board (display/touch fixos, GPIOs de perifericos configuraveis)
│   ├── roast_core/       # state machine de fases, modos (Perfil/Manual-Artisan), RoR/DTR
│   ├── safety/           # safety manager: piso de ventilacao, corte absoluto, watchdog, ack de alarmes
│   ├── hal/              # drivers MAX6675 (com calibracao), SSR, PWM
│   ├── storage/          # NVS (perfis, config, idioma), LittleFS (sessoes, snapshots)
│   ├── ui_display/       # LVGL, i18n de strings, telas (perfil, manual/artisan, testes, OTA)
│   ├── web_api/          # HTTP/WebSocket, export/import (CSV/.alog/JSON), OTA upload
│   └── artisan_adapter/  # serial/TCP bridge, leitura sempre + escrita gated por modo
└── test/

webui/
└── src/

tests/
├── unit/
├── integration/
├── contract/
└── hil/
```

**Structure Decision**: Arquitetura de firmware modular com separacao entre dominio e adaptadores de interface. O mesmo core de torra/safety atende display local, web UI e integração Artisan.

## Complexity Tracking

Nenhuma excecao formal registrada nesta fase.
