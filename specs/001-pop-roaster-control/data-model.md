# Data Model: Pop Roaster Control Platform

## 1) RoastProfile

Descrição: Perfil reutilizável de torra com curva alvo e limites operacionais.

Campos:
- id (string, UUID)
- name (string, 1..64, obrigatório, único)
- description (string, 0..256)
- roastCurve (array de CurvePoint, obrigatório)
- maxHeaterPct (int, 0..100, obrigatório)
- preheatTargetC (float, opcional)
- developmentTargetPct (float, 0..100, opcional)
- dropCurvePointRef (referência opcional a um CurvePoint que dispara transição automática para resfriamento)
- createdAt (datetime)
- updatedAt (datetime)
- revision (int, >=1)

Regras de validação:
- roastCurve deve ter pelo menos 2 pontos ordenados por tempo.
- Não pode existir ponto com tempo negativo.
- targetFanPct de qualquer ponto da curva NÃO pode ser menor que o piso global fixo de 30% (constante de firmware, não pertence ao perfil).

Nota: `minFanPctDuringHeat` deixou de ser campo do perfil; o piso de ventilação durante aquecimento é uma constante global de firmware (30%), não configurável por perfil ou operador.

## 2) CurvePoint

Descrição: Ponto de referência de curva para controle guiado.

Campos:
- tSec (int, >=0)
- targetTempC (float, 20..350)
- targetFanPct (int, 0..100, opcional)
- targetHeaterPct (int, 0..100, opcional)

Regras de validação:
- tSec estritamente crescente dentro de roastCurve.
- targetTempC deve respeitar envelope de segurança do equipamento.

## 3) RoastSession

Descrição: Execução de torra de um lote, no Modo Perfil (vinculada a um perfil, com snapshot próprio) ou no Modo Manual/Artisan (sem perfil).

Campos:
- id (string, UUID)
- controlMode (enum: PROFILE, MANUAL_ARTISAN)
- profileId (string, nullable — referência ao perfil original, apenas informativa)
- profileSnapshot (object, nullable — cópia completa da curva/parâmetros do perfil no momento da execução; obrigatório quando controlMode = PROFILE)
- modeSwitchedAt (int64, nullable — tsMs em que a sessão trocou de PROFILE para MANUAL_ARTISAN, se aplicável)
- status (enum: IDLE, PREHEAT, ROASTING, DEVELOPMENT, COOLING, COMPLETED, ABORTED)
- startedAt (datetime)
- endedAt (datetime, nullable)
- operatorNote (string, 0..1024)
- currentSetpoints (object)
- safetyState (enum: OK, WARNING, TRIPPED)
- activeAlarmIds (array de string — alarmes críticos pendentes de reconhecimento manual)
- batchRecordId (string, nullable)

Regras de validação:
- endedAt só pode existir após startedAt.
- status COMPLETED/ABORTED exige endedAt preenchido.
- controlMode só pode transicionar PROFILE -> MANUAL_ARTISAN uma vez por sessão (irreversível); nunca MANUAL_ARTISAN -> PROFILE.
- Enquanto activeAlarmIds não estiver vazio, a torra permanece bloqueada aguardando reconhecimento manual (FR-029).

Transições de estado (fase):
- IDLE -> PREHEAT
- PREHEAT -> ROASTING (após confirmação de carga dos grãos, FR-035)
- ROASTING -> DEVELOPMENT
- ROASTING -> COOLING (manual ou automático via dropCurvePointRef/watchdog)
- DEVELOPMENT -> COOLING
- COOLING -> COMPLETED
- Qualquer estado ativo -> ABORTED (por emergência/falha)

Transições de controlMode:
- PROFILE -> MANUAL_ARTISAN (com confirmação explícita de irreversibilidade, FR-040)

## 4) TelemetrySample

Descrição: Amostra temporal de processo para gráfico e análise.

Campos:
- sessionId (string, obrigatório)
- tsMs (int64, obrigatório)
- beanTempC (float, nullable se falha sensor — já com offset de calibração aplicado)
- fanPct (int, 0..100)
- heaterPct (int, 0..100)
- phase (enum igual status operacional)
- controlMode (enum: PROFILE, MANUAL_ARTISAN)
- commandSource (enum: DISPLAY, WEB, ARTISAN, PROFILE_CURVE — origem do último comando aplicado)
- sensorQuality (enum: VALID, STALE, OUT_OF_RANGE, DISCONNECTED)
- rorCPerMin (float, opcional — taxa de variação de temperatura por minuto)
- dtrPct (float, opcional — Development Time Ratio, calculado a partir do FIRST_CRACK_START)

Regras de validação:
- tsMs monotônico dentro da sessão.
- heaterPct > 0 requer fanPct >= 30 (piso fixo global).
- dtrPct só é preenchido após o evento FIRST_CRACK_START existir na sessão.

## 5) RoastEvent

Descrição: Evento marcado automaticamente ou pelo operador (timeline).

Campos:
- id (string, UUID)
- sessionId (string)
- tsMs (int64)
- type (enum: CHARGE, TURNING_POINT, DRY_END, FIRST_CRACK_START, FIRST_CRACK_END, SECOND_CRACK_START, SECOND_CRACK_END, DROP, COOL_START, MANUAL_NOTE, ALARM)
- payload (object livre e validado por tipo)

Regras de validação:
- type ALARM exige payload.code, payload.severity e payload.requiresAck (bool).
- type CHARGE só é criado após confirmação do operador na sinalização de pré-aquecimento pronto (FR-035).

## 6) BatchRecord

Descrição: Metadados do lote torrado para rastreabilidade e aprendizado.

Campos:
- id (string, UUID)
- sessionId (string)
- coffeeName (string, 1..80)
- origin (string, 0..80)
- process (string, 0..40)
- cropYear (int, opcional)
- chargeWeightG (float, >0)
- dropWeightG (float, >=0, opcional)
- finalColorNote (string, 0..120)
- cuppingNotes (string, 0..1024)
- createdAt (datetime)

## 7) HardwareChannel

Descrição: Canal lógico de I/O para controle e diagnóstico.

Campos:
- id (enum: HEATER, FAN, THERMOCOUPLE)
- enabled (bool)
- commandValue (float/int dependendo do canal)
- feedbackValue (float/int)
- health (enum: OK, DEGRADED, FAILED)
- lastUpdateMs (int64)

## 8) SafetyRule

Descrição: Regra declarativa de intertravamento e proteção.

Campos:
- id (string)
- description (string)
- conditionExpr (string)
- action (enum: BLOCK_COMMAND, FORCE_HEATER_OFF, FORCE_COOLING, RAISE_ALARM)
- severity (enum: INFO, WARN, CRITICAL)
- enabled (bool)

## 9) FirmwarePackage

Descrição: Pacote de firmware para atualização OTA via upload web (esquema de partição A/B).

Campos:
- filename (string)
- version (string semver-like)
- checksum (string)
- sizeBytes (int)
- targetPartition (enum: OTA_0, OTA_1 — partição secundária de destino)
- uploadedAt (datetime)
- validationStatus (enum: PENDING, VALID, INVALID)
- installStatus (enum: NOT_STARTED, IN_PROGRESS, SUCCESS, KEPT_CURRENT_PARTITION, FAILED)

Regra de negócio: o boot só chaveia para `targetPartition` quando installStatus = SUCCESS; qualquer outro resultado mantém o boot na partição atual (installStatus = KEPT_CURRENT_PARTITION), sem risco operacional.

## 10) SensorCalibration

Descrição: Offset de calibração do sensor MAX6675, configurável pelo operador na tela de testes.

Campos:
- offsetC (float, aplicado a todas as leituras subsequentes)
- referenceUsed (string, ex.: "boiling_water_100c")
- calibratedAt (datetime)

## 11) LanguageSetting

Descrição: Preferência de idioma da interface (display e web).

Campos:
- language (enum: EN, PT, ES; padrão: EN)

## Relacionamentos

- RoastProfile 1:N RoastSession (apenas quando controlMode = PROFILE)
- RoastSession 1:N TelemetrySample
- RoastSession 1:N RoastEvent
- RoastSession 1:1 BatchRecord
- RoastSession 1:N HardwareChannel snapshots (lógicos)
- RoastSession N:N SafetyRule (regras ativas por configuração de runtime; aplicadas independentemente de controlMode)
- RoastSession 0:1 profileSnapshot (cópia embutida, sem dependência do RoastProfile original)

## Política de Retenção Inicial

- Perfis: retenção permanente (até limite configurável)
- Sessões + telemetria: retenção circular por espaço disponível
- Eventos e metadados de lote: retenção priorizada em relação a telemetria bruta
