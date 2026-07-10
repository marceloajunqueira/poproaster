# Research: Pop Roaster Control Platform

## Decision 1: Firmware stack e arquitetura de runtime

- Decision: Usar ESP-IDF (C/C++) como base de firmware com arquitetura em camadas: HAL de hardware, Core de torra (estado, modos e segurança), e adapters de interface (display, web, Artisan).
- Rationale: ESP-IDF oferece controle determinístico de tarefas, watchdog, NVS, OTA A/B e ecossistema robusto para produto embarcado. A separação por camadas desacopla regras de torra da UI e facilita suporte a múltiplos displays, idiomas e modos de operação.
- Alternatives considered:
  - Arduino framework puro: menor curva inicial, mas menos previsibilidade para concorrência e manutenção em crescimento.
  - Micropython: rápido para protótipo, porém com limitações de desempenho/integração para UI gráfica e telemetria contínua.

## Decision 2: Hardware real do JC4827W543 e stack de display/touch

- Decision: Confirmado via documentação oficial do fabricante ([github.com/lsdlsd88/JC4827W543](https://github.com/lsdlsd88/JC4827W543)) e via documentação comunitária detalhada ([github.com/profi-max/JC4827W543_4.3inch_ESP32S3_board](https://github.com/profi-max/JC4827W543_4.3inch_ESP32S3_board)) que o JC4827W543 usa MCU **ESP32-S3-WROOM-1-N4R8** (520KB RAM, 8MB PSRAM OSPI, 4MB Flash QSPI), display **480x272** RGB565 com driver de painel **NV3041A** via QSPI (CS:45, SCK:47, D0:21, D1:48, D2:40, D3:39, backlight:1), e touch capacitivo **GT911 via I2C** (SCL:4, SDA:8, RST:38, INT:3, endereço 0x5D). A placa expõe até 10 GPIOs livres via conectores JST1.25 para periféricos externos (SSR, PWM, MAX6675). Para o firmware ESP-IDF nativo, usar o componente do IDF Component Registry `eric-c-e/esp_lcd_nv3041` (via `idf.py add-dependency "eric-c-e/esp_lcd_nv3041^0.1.1"`), construído sobre `esp_lcd_panel_io_spi`, integrado ao LVGL via `esp_lvgl_port` (componente oficial Espressif); o touch GT911 é integrado via componente `esp_lcd_touch_gt911` da família `esp_lcd_touch`. Um componente `board_config` foi criado (`firmware/components/board_config/`) definindo um contrato de macros que qualquer board futuro deve implementar (display/touch fixos por header específico em `boards/`), enquanto os pinos dos periféricos externos (MAX6675, SSR, PWM da ventoinha) são configuráveis via Kconfig/`idf.py menuconfig`, independente do board selecionado.
- Rationale: Os exemplos oficiais do fabricante são baseados em Arduino_GFX + LVGL sobre Arduino core, mas existe um driver ESP-IDF nativo equivalente para o NV3041A no Component Registry, permitindo manter a arquitetura ESP-IDF pura (Decision 1) sem reescrever um driver de painel do zero. Separar pinos fixos do board (display/touch) de pinos configuráveis do usuário (periféricos via JST1.25) atende ao requisito de suportar futuros displays (FR-017) sem reescrever regras de negócio, e permite ao usuário testar diferentes GPIOs para os periféricos sem editar código.
- Pinagem física confirmada dos conectores JST1.25 (4 pinos cada, na ordem de conexão): P1 = GND, RXD, TXD, +5V (UART/energia); P2 = IO46, IO9, IO14, IO5; P3 = IO6, IO7, IO15, IO16; P4 = GND, 3.3V, IO17, IO18. IO46 é **input-only** no ESP32-S3 (não pode acionar saídas), portanto foi atribuído por padrão ao SO/MISO do MAX6675 (sinal de leitura); SCK/CS do MAX6675, SSR e PWM da ventoinha usam pinos de saída (IO5, IO6, IO7, IO15 por padrão), com IO9/IO14/IO16/IO17/IO18 disponíveis como reserva para expansão futura (ex.: feedback de RPM da ventoinha).
- Alternatives considered:
  - Usar Arduino core + Arduino_GFX + LVGL (stack oficial do fabricante): bring-up mais rápido (exemplos prontos), mas introduziria uma segunda camada de framework (Arduino sobre IDF) com overhead e menos controle fino sobre tarefas/memória — rejeitado.
  - Escrever driver NV3041A/QSPI customizado do zero em ESP-IDF puro: mais controle, porém trabalho redundante dado que já existe um componente publicado e mantido — rejeitado em favor de reaproveitar o componente `esp_lcd_nv3041`.
  - Hardcode dos pinos de periféricos externos no código-fonte: mais simples, porém exigiria recompilar a cada teste de fiação diferente — rejeitado em favor de Kconfig configurável via menuconfig.

## Decision 3: Interface gráfica local para JC4827W543, i18n e escalabilidade de telas

- Decision: Usar LVGL com camada de abstração de layout (tokens de dimensão, grid responsivo, componentes reutilizáveis) e um catálogo de strings traduzíveis (EN padrão, PT, ES) carregado pela UI, independente da resolução do display.
- Rationale: LVGL é padrão de mercado em ESP32 com boa performance; a separação de strings em catálogo permite trocar idioma em runtime (FR-038) e escalar para novos displays sem duplicar lógica de domínio.
- Alternatives considered:
  - UI proprietária fixa por resolução/idioma: entrega inicial rápida, mas alto retrabalho em novos displays e sem suporte real a múltiplos idiomas.
  - Strings hardcoded no código: simples, mas exige recompilação por idioma — descartado por não atender troca em runtime (FR-038).

## Decision 4: Modos de operação e Safety Manager

- Decision: Modelar dois modos de sessão mutuamente exclusivos: **Modo Perfil** (segue curva de `RoastProfile`, Artisan somente leitura) e **Modo Manual/Artisan** (sem curva própria; aceita comandos do operador via display/web e do Artisan, todos sob o mesmo esquema last-write-wins). Transição permitida apenas Perfil → Manual/Artisan, mediante confirmação explícita e irreversível na sessão (FR-039, FR-040). Um Safety Manager centralizado intercepta todo comando de qualquer fonte (display, web, Artisan) antes da aplicação, independente do modo.
- Rationale: Atende ao requisito de segurança do usuário (Artisan só controla quando o operador sai conscientemente do Modo Perfil) sem duplicar lógica de validação por canal de entrada.
- Alternatives considered:
  - Bidirecional sempre-ativo (sem gate de modo): mais simples, porém permite que um software externo no PC acione atuadores mesmo durante execução automática de perfil — rejeitado por risco de segurança.
  - Três modos separados (Manual, Perfil, Artisan): mais granular, mas o usuário preferiu unificar Manual e Artisan em um único modo simplificado.

## Decision 5: Regras de segurança hard-fail (Safety Rules)

- Decision: Implementar as seguintes regras não negociáveis no Safety Manager, aplicadas independentemente de modo ou fonte de comando:
  1. Aquecimento requer ventilação ≥ 30% (piso fixo em firmware, não configurável) sempre que ativo.
  2. Corte absoluto de temperatura em 260°C (aviso prévio em 240°C), independente do perfil.
  3. Falha de leitura do sensor força aquecimento OFF e alarme crítico.
  4. Detecção indireta de falha de ventilação via padrão anômalo de RoR força aquecimento OFF e alarme crítico.
  5. Watchdog de duração máxima de torra (padrão 25 min, configurável) força transição automática para resfriamento.
  6. Parada de Emergência dedicada, sempre acessível, corta aquecimento imediatamente.
  7. Alarmes críticos (itens 2-4 e 6) exigem reconhecimento manual do operador antes de a torra continuar.
- Rationale: Requisitos de segurança críticos definidos ao longo de 6+ sessões de clarificação para proteger o equipamento (resistência de pipoqueira) e compensar a ausência de sensores redundantes (sem tacômetro de ventoinha, sem proteção física independente do SSR).
- Alternatives considered:
  - Proteção física redundante (fusível térmico/termostato) contra falha do SSR: recomendada como boa prática, mas explicitamente recusada pelo usuário para a v1 (risco residual documentado no spec).
  - Feedback de RPM na ventoinha: mais robusto, mas descartado pelo usuário em favor de detecção indireta via curva de temperatura (mais simples, sem hardware adicional).

## Decision 6: Aquisição de temperatura, calibração e papel do sensor (BT)

- Decision: Leitura MAX6675 tratada como BT (Bean Temperature) real, pois o sensor fica em contato direto com os grãos. Pipeline de validação (faixa plausível, valor travado, timeout) mais um offset de calibração configurável pelo operador (validado contra referência conhecida, ex. água fervendo), aplicado a todas as leituras subsequentes.
- Rationale: Sensor único fisicamente posicionado nos grãos torna a nomenclatura BT correta e compatível com o Artisan; calibração compensa erro sistemático comum em termopares baratos, essencial para um usuário leigo confiar na curva.
- Alternatives considered:
  - Tratar como ET (Environment Temperature): descartado, pois não reflete a posição real do sensor.
  - Sem calibração (leitura bruta sempre): descartado por gerar curvas sistematicamente deslocadas sem que o operador perceba.

## Decision 7: Persistência local, snapshot de perfil e backup

- Decision: Persistir perfis, calibração, configuração de rede/idioma em NVS; histórico de sessões em LittleFS/SPIFFS com retenção circular (padrão 30 sessões, configurável). Cada sessão armazena um snapshot próprio da curva/parâmetros do perfil usado no momento da execução (para não depender do perfil original após exclusão). Perfis exportáveis/importáveis em JSON via web; sessões exportáveis em CSV e/ou `.alog` (Artisan).
- Rationale: NVS é ótimo para dados pequenos/chave-valor; snapshot por sessão garante integridade histórica mesmo com exclusão de perfis; export/import cobre backup e continuidade de dados em caso de troca/falha de dispositivo.
- Alternatives considered:
  - Sessão apenas referenciando `profileId`: mais simples, mas quebra o histórico se o perfil for excluído — rejeitado.
  - Banco externo obrigatório: aumentaria complexidade e dependência de rede — rejeitado.

## Decision 8: Interface Web unificada, tempo real e i18n

- Decision: Servir dashboard web no próprio ESP32 com WebSocket para telemetria/controle em tempo real (BT, RoR, DTR%, fase, modo) e endpoints HTTP para configuração, perfis (incl. export/import), sessões (incl. export), OTA upload e idioma. UI web compartilha o mesmo catálogo de strings i18n do display.
- Rationale: Mantém operação local sem internet, permite acesso por celular/computador na mesma rede, e evita duplicar traduções entre display e web.
- Alternatives considered:
  - Polling HTTP puro: pior latência e experiência para gráfico de curva/RoR em tempo real.
  - App mobile nativo no v1: alto custo para versão inicial.

## Decision 9: Compatibilidade com Artisan (bidirecional gated por modo)

- Decision: Adapter de protocolo compatível com o Artisan via serial USB e/ou bridge TCP local. Em **Modo Manual/Artisan**, o adapter aceita comandos de controle do Artisan (aplicados pelo mesmo Safety Manager que valida comandos locais/web). Em **Modo Perfil**, o adapter opera estritamente como somente leitura (telemetria e eventos), ignorando qualquer comando recebido.
- Rationale: Reflete o uso real do Artisan (operador humano ajustando sliders/botões no software) sem comprometer a automação de perfil; maximiza interoperabilidade com a comunidade de torra mantendo a segurança local intacta.
- Alternatives considered:
  - Somente leitura sempre: mais simples e seguro, mas não atende ao caso de uso de controle remoto que o usuário quer explicitamente.
  - Bidirecional irrestrito em qualquer modo: rejeitado por risco de conflito com a automação de perfil.

## Decision 10: Estratégia de OTA (esquema A/B seguro)

- Decision: OTA por upload web com validação de imagem, instalação em partição secundária (esquema A/B); o boot só chaveia para a nova partição após instalação completa e validada com sucesso. Falha em qualquer etapa mantém o sistema na partição atual, sem risco operacional.
- Rationale: Elimina risco de "brick" do dispositivo por atualização malsucedida — crítico para um usuário sem experiência em recuperação manual de firmware.
- Alternatives considered:
  - Atualização apenas por cabo/IDE: barreira alta para manutenção.
  - Partição única sem rollback: risco operacional elevado, rejeitado explicitamente pelo usuário.

## Decision 11: Telemetria avançada (RoR, DTR%, marcos de torra)

- Decision: Calcular e exibir curva de RoR junto à curva de BT (display e web); calcular e exibir DTR% em tempo real a partir do evento de first crack; suportar marcos completos de torra (charge, turning point, dry end, first crack start/end, second crack start/end, drop, cool start, manual note, alarm).
- Rationale: Replica as características centrais de interfaces profissionais de referência (Kaleido, Stratto Lab, Artisan) citadas pelo usuário, fornecendo dados-chave para decisão de drop mesmo para operador iniciante.
- Alternatives considered:
  - Apenas curva de BT, sem RoR/DTR: mais simples, mas descartado por reduzir o valor educativo/profissional da interface.

## Decision 12: Testabilidade

- Decision: Estratégia de testes em 3 níveis: unitários do Core de torra/safety (incluindo state machine de modos e cálculo de RoR/DTR), integração de drivers em hardware-in-loop (sensor, SSR, PWM, detecção indireta de falha), e testes de contrato para web, export/import e adapter Artisan (leitura e escrita).
- Rationale: Cobertura equilibrada para regras críticas de segurança e interfaces externas bidirecionais.
- Alternatives considered:
  - Só testes manuais: insuficiente para regressão de segurança em um sistema com múltiplos modos e fontes de comando.

## Decision 13: Itens adicionais de segurança e usabilidade para iniciantes

- Decision: Incluir desde o v1: confirmações obrigatórias (teste de aquecimento, troca de modo, upload de firmware), calibração guiada de sensor, sinalização visual de pré-aquecimento pronto (sem buzzer — hardware não possui), watchdog de duração máxima, e i18n (EN padrão) para acessibilidade a diferentes usuários.
- Rationale: Acelera aprendizado, reduz risco de erro operacional e amplia o alcance do projeto para além do público de língua portuguesa.
- Alternatives considered:
  - Deixar i18n e calibração para versões futuras: reduziria valor prático e seria uma regressão em relação às decisões já tomadas com o usuário.

