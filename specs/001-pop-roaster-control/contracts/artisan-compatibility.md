# Contract: Artisan Compatibility (v1)

## Objetivo

Definir o contrato mínimo para interoperar com o Artisan durante monitoramento de torra em tempo real, incluindo controle bidirecional gated por modo de operação.

## Escopo v1

- Publicar temperatura do processo (BT), RoR e timestamp em stream contínuo.
- Publicar eventos relevantes de torra (charge, turning point, dry end, first/second crack, drop, alarmes).
- Aceitar comandos de controle do Artisan **somente quando a sessão estiver em Modo Manual/Artisan**.
- Em Modo Perfil, a conexão com o Artisan é estritamente somente leitura (comandos recebidos são ignorados).
- Manter operação local independente do cliente Artisan.

## Modelo de controle por modo

| Modo da sessão | Artisan pode enviar comandos? | Comportamento |
|---|---|---|
| Modo Perfil | Não | Somente leitura: telemetria e eventos publicados; qualquer comando recebido é descartado. |
| Modo Manual/Artisan | Sim | Comandos aceitos e validados pelo mesmo Safety Manager que valida comandos do display/web (last-write-wins entre display, web e Artisan). |

A transição de Modo Perfil para Modo Manual/Artisan é feita pelo operador (irreversível na sessão); o Artisan não pode iniciar essa transição remotamente.

## Transportes

- Primário: Serial USB virtual (recomendado para setup inicial).
- Opcional: TCP bridge local para integração em rede.

## Frequência e latência alvo

- Frequência de atualização: 2 a 5 Hz para monitoramento estável.
- Latência ponta a ponta: preferencialmente abaixo de 500 ms em rede local.

## Campos mínimos publicados

- timestamp_ms
- bean_temp_c (BT)
- ror_c_per_min
- fan_pct
- heater_pct
- phase
- control_mode (PROFILE | MANUAL_ARTISAN)
- event (quando aplicável)

## Comandos aceitos (somente em Modo Manual/Artisan)

- set_fan_pct
- set_heater_pct
- start_cooling
- mark_event

## Regras de robustez

- Se sensor estiver inválido, publicar estado de qualidade (stale/disconnected) em vez de valor inventado.
- Queda de conexão com Artisan não pode interromper torra local.
- Reconnect do cliente deve retomar stream sem reiniciar sessão.
- Todo comando recebido do Artisan passa pelo Safety Manager antes de qualquer execução, independentemente do modo.

## Critérios de conformidade

- Dados recebidos no Artisan representam a curva atual (BT + RoR) sem gaps perceptíveis para o operador.
- Eventos manuais e marcos de torra aparecem no timeline externo.
- Em Modo Manual/Artisan, comandos enviados pelo Artisan são aplicados com a mesma validação de segurança que comandos locais.
- Em Modo Perfil, comandos enviados pelo Artisan são ignorados e não afetam a execução da curva.
- Em desconexão, controle local continua íntegro e sem travamento da UI.

## Itens fora do escopo v1

- Múltiplos clientes Artisan concorrentes com arbitragem complexa.
- Controle remoto do Artisan em Modo Perfil (bloqueado por design).
- PID fechado do lado do Artisan controlando diretamente a curva onboard.

