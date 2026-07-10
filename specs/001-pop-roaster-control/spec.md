# Feature Specification: Pop Roaster Control Platform

**Feature Branch**: `[001-pop-roaster-control]`

**Created**: 2026-07-06

**Status**: Draft

**Input**: User description: "Converter uma pipoqueira elétrica de 1200W em torrador de café com controle por display ESP32, compatibilidade com Artisan, interface web e recursos iniciais de perfis, teste de periféricos, ajustes em tempo real e update de firmware."

## Clarifications

### Session 2026-07-06

- Q: Qual o modelo de segurança de acesso para controle web e upload de firmware na v1? → A: Sem autenticação nenhuma (rede local é considerada confiável)
- Q: O que o sistema deve fazer ao retomar energia após uma queda durante torra ativa? → A: Tentar retomar a torra automaticamente de onde parou, revalidando as condições de segurança
- Q: Como o dispositivo deve ser configurado na rede Wi-Fi local? → A: Modo Access Point próprio na primeira configuração com portal cativo, depois conecta como cliente (STA)
- Q: O que fazer quando o histórico de sessões de torra atingir o limite de armazenamento? → A: Descartar automaticamente a sessão mais antiga para abrir espaço (rotação circular)
- Q: Como tratar múltiplos clientes web enviando comandos de controle simultaneamente? → A: Qualquer cliente conectado pode enviar comandos livremente, sem restrição (último comando vence)

### Session 2026-07-06 (2)

- Q: Deve existir um limite absoluto de temperatura de segurança, independente do perfil ativo? → A: Sim, limite absoluto de 260°C (hard cutoff) com aviso prévio em 240°C
- Q: Deve haver um controle dedicado de Parada de Emergência, separado do fluxo normal de finalizar torra? → A: Sim, comando de Parada de Emergência sempre acessível no display e na web, distinto de finalizar torra
- Q: A transição para resfriamento deve ser manual, automática pelo perfil, ou ambas? → A: Ambas — manual a qualquer momento, ou automática ao atingir o ponto de drop do perfil
- Q: Qual o valor padrão do limite de retenção de sessões de torra no histórico? → A: 30 sessões
- Q: Como deve funcionar a edição interativa da curva de perfil de torra? → A: Combinação — gráfico visual somente leitura + edição via tabela numérica ao lado

### Session 2026-07-06 (3)

- Q: Quando o operador faz ajuste manual durante torra guiada por perfil, esse ajuste deve persistir por quanto tempo? → A: Até o próximo ponto programado da curva, depois o perfil retoma o controle automaticamente
- Q: Como deve funcionar a recuperação em caso de falha na atualização de firmware? → A: Esquema de 2 partições (A/B); só chaveia para a nova partição se a atualização for concluída e validada com segurança, senão permanece na partição atual sem risco
- Q: Alarmes críticos devem limpar automaticamente ou exigir reconhecimento manual do operador? → A: Alarmes críticos exigem reconhecimento manual, mesmo que a condição já tenha passado
- Q: Como o único sensor MAX6675 deve ser tratado para fins de curva de perfil e compatibilidade com Artisan (BT ou ET)? → A: Como BT (Bean Temperature) real, pois o sensor fica dentro da câmara em contato direto com os grãos

### Session 2026-07-06 (4)

- Q: Como verificar se a ventoinha está realmente girando durante o aquecimento, já que não há sensor dedicado de RPM? → A: Verificação indireta usando a curva de temperatura como proxy (subida anômala indica possível falha de ventilação)
- Q: Deve haver proteção física independente (termostato/fusível térmico) contra falha do SSR travado ligado, além do corte por software? → A: Não, confiar apenas no corte por software (FR-026 a 260°C)
- Q: O sistema deve permitir exportar sessões de torra concluídas para análise posterior, além do streaming ao vivo para o Artisan? → A: Sim, exportar em CSV e/ou formato nativo do Artisan (.alog) pela interface web
- Q: O sistema precisa de relógio real (RTC/NTP) para registrar horário absoluto das torras? → A: Não é necessário; usar apenas timestamps relativos ao início da sessão (tempo decorrido)

### Session 2026-07-06 (5)

- Q: O sistema deve permitir calibração do sensor de temperatura pelo operador? → A: Sim, offset de calibração configurável, validado com referência conhecida, acessível na tela de testes
- Q: Deve haver um limite máximo de duração de torra como rede de segurança contra torra esquecida/travada? → A: Sim, limite configurável que força resfriamento automático e alarme crítico se atingido

### Session 2026-07-06 (6)

- Q: Qual o valor padrão do limite mínimo de ventilação durante aquecimento? → A: 30%, fixo em firmware (não configurável pelo operador); perfis podem reduzir ventilação ao longo da torra à medida que os grãos perdem massa/umidade, nunca abaixo desse piso
- Q: O alarme de falha indireta de ventilação (FR-030) também deve exigir reconhecimento manual, como os demais alarmes críticos? → A: Sim, segue a mesma regra dos outros alarmes críticos (FR-029)
- Q: O que acontece com sessões históricas quando o perfil usado é excluído posteriormente? → A: A sessão guarda uma cópia (snapshot) da curva/parâmetros do perfil no momento da torra; excluir o perfil não afeta o histórico
- Q: O sistema deve sinalizar quando o pré-aquecimento atingir a temperatura alvo, aguardando confirmação antes de carregar os grãos? → A: Sim, sinalização apenas visual na tela (sistema não possui buzzer/sinal sonoro)

### Session 2026-07-06 (7)

- Q: O sistema deve exibir também a curva de RoR (taxa de variação de temperatura) junto à curva de temperatura? → A: Sim, exibir curva de RoR junto com a curva de temperatura, no display e na web
- Q: Devem existir marcos adicionais de Dry End e Second Crack, além de charge/turning point/first crack/drop? → A: Sim, adicionar Dry End e Second Crack (início/fim) como marcos disponíveis
- Q: O sistema deve calcular e exibir o DTR% (Development Time Ratio) em tempo real durante a torra? → A: Sim, calcular e exibir DTR% em tempo real assim que first crack for marcado

### Session 2026-07-06 (8)

- Requisito adicionado a pedido do usuário: suporte a múltiplos idiomas na interface (inglês, português e espanhol), com inglês como idioma padrão.

### Session 2026-07-06 (9)

- Q: O Artisan deve poder apenas ler dados da torra, ou também enviar comandos de controle ao torrador? → A: Bidirecional — Artisan também pode enviar comandos, sempre validados pelas mesmas regras de segurança
- Q: Como conciliar controle do Artisan com o modo de operação do torrador? → A: Unificar em um "Modo Manual/Artisan" (sem curva de perfil própria, aceita comandos do operador e do Artisan); no "Modo Perfil" o Artisan fica somente leitura

### Session 2026-07-06 (11)

- Q: Pode trocar de Modo Perfil para Modo Manual/Artisan durante a torra ativa? E o inverso? → A: Só numa direção (Perfil → Manual/Artisan), com confirmação explicando que é irreversível para a sessão; não é possível voltar ao Modo Perfil na mesma sessão
- Q: Deve ser possível exportar/importar perfis de torra como backup, além da exportação de sessões já prevista? → A: Sim, exportar e importar perfis (JSON) pela interface web

### Session 2026-07-06 (12)

- Q: O dashboard de torra ao vivo deve exibir um gráfico de timeline (BT+RoR) sendo desenhado em tempo real, além dos indicadores numéricos já existentes? → A: Sim, ambos simultaneamente — indicadores numéricos na parte superior da tela, gráfico compacto ao vivo na parte inferior (referência visual: ROEST Coffee, Gaggiuino, Kaleido)
- Q: No Modo Perfil, o gráfico ao vivo deve mostrar a curva-alvo do perfil sobreposta à curva real? → A: Sim, sobrepor a curva-alvo do perfil ativo à curva real sempre que a sessão estiver em Modo Perfil
- Q: Os eventos marcados (turning point, dry end, first/second crack, drop) devem aparecer como linhas verticais no gráfico? → A: Sim, tanto no gráfico ao vivo quanto na revisão de sessões concluídas
- Q: Essa experiência de gráfico (curva ao vivo + curva-alvo + marcadores) vale só para o display local ou também para a web? → A: Vale para ambos, com paridade funcional entre display e interface web
- Q: Como deve ser a navegação entre as telas do display (Torra, Perfis, Histórico, Configurações)? → A: Menu lateral fixo à esquerda, estilo Material Design, sempre visível, com item destacado indicando a tela ativa

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Torrar com segurança pelo display (Priority: P1)

Como operador iniciante, quero iniciar, acompanhar e finalizar uma torra pelo display com proteções automáticas, para evitar danos no equipamento e conduzir o processo com segurança.

**Why this priority**: Segurança operacional e proteção do equipamento são requisitos críticos para qualquer torra.

**Independent Test**: Pode ser testada de ponta a ponta iniciando uma torra no display, aplicando ajustes manuais e concluindo em resfriamento, com validação das travas de segurança durante todo o ciclo.

**Acceptance Scenarios**:

1. **Given** que a torra está em andamento, **When** o operador solicita aquecimento, **Then** o sistema só permite aquecimento com ventilação ativa e leitura válida de temperatura.
2. **Given** que a torra está em andamento com aquecimento ativo, **When** o operador reduz a ventilação, **Then** o sistema bloqueia valores abaixo do mínimo fixo de 30% para aquecimento.
3. **Given** que o operador entra na etapa final de resfriamento, **When** ele ativa resfriamento, **Then** o sistema mantém ventilação ativa e garante aquecimento desligado.

---

### User Story 2 - Criar e usar perfis de torra (Priority: P1)

Como operador, quero criar perfis de torra com curvas interativas e reutilizá-los em novos lotes, para repetir resultados e reduzir variação entre torras.

**Why this priority**: Repetibilidade de perfil é o principal valor funcional para evoluir de controle manual para processo consistente.

**Independent Test**: Pode ser testada criando um perfil no display, salvando, carregando em nova sessão e verificando execução guiada por curva.

**Acceptance Scenarios**:

1. **Given** que o operador está na tela de perfis, **When** ele cria e salva um perfil com curva de torra, **Then** o perfil fica disponível para seleção futura.
2. **Given** que existem perfis salvos, **When** o operador seleciona um perfil antes de iniciar a torra, **Then** os parâmetros e curva desse perfil são aplicados à nova sessão.

---

### User Story 3 - Monitorar por web e integrar com Artisan (Priority: P2)

Como operador, quero acompanhar a torra por navegador e usar também software de torra no computador, para ter flexibilidade de monitoramento e análise.

**Why this priority**: Aumenta usabilidade em bancada e habilita fluxo de trabalho com ferramentas já usadas na comunidade de torra.

**Independent Test**: Pode ser testada iniciando uma torra e validando que os mesmos dados principais e comandos estão disponíveis no display e na interface web, e que os dados necessários chegam ao software externo compatível.

**Acceptance Scenarios**:

1. **Given** que a torra está ativa, **When** o operador abre a interface web, **Then** ele visualiza em tempo real a curva e os estados da torra.
2. **Given** que o sistema está conectado ao software de torra compatível, **When** a torra evolui, **Then** as leituras e eventos da torra são publicados no formato esperado pelo software.
3. **Given** que o sistema está no Modo Manual/Artisan e conectado ao Artisan, **When** o Artisan envia um comando de controle (ex.: ajuste de ventilação), **Then** o sistema aplica as mesmas validações de segurança usadas para comandos locais antes de executar o comando.
4. **Given** que o sistema está no Modo Perfil e conectado ao Artisan, **When** o Artisan envia um comando de controle, **Then** o sistema ignora o comando e mantém apenas o envio de telemetria (conexão somente leitura).

---

### User Story 4 - Validar hardware e manutenção (Priority: P3)

Como operador, quero uma tela de teste de periféricos e atualização de firmware por upload web, para diagnosticar falhas rapidamente e manter o sistema atualizado.

**Why this priority**: Reduz tempo de manutenção e facilita evolução do produto sem depender de ferramentas externas complexas.

**Independent Test**: Pode ser testada acionando testes individuais de sensor, ventilação e aquecimento, e realizando atualização por arquivo com confirmação de sucesso.

**Acceptance Scenarios**:

1. **Given** que o operador está na tela de testes, **When** ele executa teste de periféricos, **Then** o sistema informa status individual de sensor, ventilação e aquecimento.
2. **Given** que há um arquivo de firmware válido, **When** o operador realiza upload pela interface web, **Then** o sistema valida, instala e confirma versão atualizada.

### Edge Cases

- Sensor de temperatura indisponível, travado ou com leitura fora de faixa durante torra ativa.
- Queda e retorno de energia no meio da torra: o sistema tenta retomar automaticamente a sessão do último estado conhecido, mas só reativa aquecimento se ventilação e sensor estiverem novamente válidos; caso contrário, permanece em estado seguro aguardando o operador.
- Solicitação de aquecimento com ventilação abaixo do mínimo de segurança.
- Troca entre modos manual e perfil durante curva ativa.
- Memória de perfis cheia ou com dados corrompidos.
- Histórico de sessões de torra atingindo o limite de armazenamento configurado.
- Perda de conectividade web sem interromper controle local no display.
- Falha ao conectar à rede Wi-Fi configurada, exigindo reentrada em modo Access Point para nova configuração.
- Falha durante upload ou instalação de firmware (arquivo corrompido, queda de energia na gravação), mantendo o sistema na partição atual válida sem interrupção do funcionamento.
- Comandos conflitantes vindos simultaneamente de display, web e Artisan durante o Modo Manual/Artisan (último comando válido vence, sempre sujeito às validações de segurança).
- Comando de controle recebido do Artisan enquanto a sessão está no Modo Perfil (o sistema ignora o comando e mantém a conexão apenas como somente leitura).
- Tentativa de retornar ao Modo Perfil após já ter trocado para o Modo Manual/Artisan na mesma sessão (não permitido; a troca é irreversível até o fim da sessão).
- Tentativa de iniciar resfriamento com aquecimento ainda ativo.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: O sistema MUST permitir iniciar, pausar, retomar e finalizar uma torra pelo display local.
- **FR-002**: O sistema MUST exibir em tempo real temperatura do grão (BT), ventilação, potência de aquecimento, tempo de torra e fase atual.
- **FR-003**: O sistema MUST impedir ativação de aquecimento quando a ventilação não estiver ativa.
- **FR-004**: O sistema MUST aplicar um limite mínimo fixo de 30% de ventilação durante aquecimento (valor fixo em firmware, não configurável pelo operador) e MUST bloquear qualquer comando de ventilação abaixo desse piso enquanto o aquecimento estiver ativo; perfis de torra podem reduzir gradualmente a ventilação ao longo da curva, para compensar a perda de massa/umidade dos grãos, sem nunca ultrapassar esse piso mínimo.
- **FR-005**: O sistema MUST permitir ventilação ativa com aquecimento desligado na etapa de resfriamento.
- **FR-006**: O sistema MUST desligar aquecimento automaticamente e sinalizar alarme quando houver falha de leitura de temperatura durante aquecimento.
- **FR-007**: O sistema MUST permitir ajuste manual em tempo real de ventilação e aquecimento durante torra ativa dentro de limites seguros; durante execução guiada por perfil, o ajuste manual MUST persistir até o próximo ponto programado da curva, quando o perfil MUST retomar o controle automaticamente.
- **FR-008**: O sistema MUST permitir criação, edição, duplicação, exclusão e seleção de perfis de torra por interface gráfica interativa, combinando um gráfico visual da curva (somente leitura) com uma tabela numérica editável de pontos de tempo/temperatura ao lado.
- **FR-009**: O sistema MUST persistir perfis de torra em armazenamento não volátil e recuperá-los após reinicialização.
- **FR-010**: O sistema MUST registrar histórico de torra com curva temporal de temperatura e ajustes aplicados.
- **FR-011**: O sistema MUST apresentar gráfico da curva de torra no display durante execução e revisão de lote concluído, incluindo tanto a curva de temperatura (BT) quanto a curva de RoR (taxa de variação de temperatura por minuto); durante a torra ativa, a tela principal (dashboard) MUST exibir simultaneamente os indicadores numéricos (BT, RoR, DTR%, ventilação, aquecimento, fase, tempo) na parte superior da tela e um gráfico compacto com a curva sendo desenhada em tempo real na parte inferior.
- **FR-012**: O sistema MUST disponibilizar interface web responsiva para monitorar e controlar a torra com paridade funcional mínima com a interface local.
- **FR-013**: O sistema MUST disponibilizar dados de torra e eventos em formato compatível com o software Artisan em tempo real. Quando a sessão estiver no Modo Manual/Artisan, o sistema MUST também aceitar comandos de controle enviados pelo Artisan (ex.: ajuste de ventilação/aquecimento), aplicando sempre as mesmas validações de segurança usadas para comandos locais e da interface web; quando a sessão estiver no Modo Perfil, o sistema MUST tratar a conexão com o Artisan como somente leitura, sem executar comandos de controle recebidos.
- **FR-014**: O sistema MUST oferecer tela de teste de periféricos com testes individuais para sensor, ventilação e aquecimento.
- **FR-015**: O sistema MUST oferecer atualização de firmware via interface web com upload de arquivo, validando o pacote e instalando-o em uma partição OTA secundária (esquema A/B); o sistema MUST chavear o boot para a nova partição somente após a instalação ser concluída e validada com sucesso, e MUST permanecer na partição atual sem qualquer risco operacional caso a atualização falhe.
- **FR-016**: O sistema MUST manter controle local no display mesmo quando a interface web estiver indisponível.
- **FR-017**: O sistema MUST suportar inicialmente o display JC4827W543 e MUST permitir expansão futura para outros tamanhos de tela sem redefinir regras de negócio de torra.
- **FR-018**: O sistema MUST permitir marcar eventos de torra (charge, turning point, dry end, first crack start/end, second crack start/end, drop, início do resfriamento, entre outros) para análise posterior do lote.
- **FR-019**: O sistema MUST permitir cadastro de metadados de lote (café, origem, peso de carga, observações) vinculados ao histórico de torra.
- **FR-020**: O sistema MUST exigir confirmação explícita do operador antes de qualquer teste que acione aquecimento.
- **FR-021**: O sistema NÃO MUST exigir autenticação para acesso à interface web, envio de comandos de controle ou upload de firmware na v1, assumindo a rede local como confiável.
- **FR-022**: Após queda e retorno de energia durante torra ativa, o sistema MUST tentar retomar automaticamente a sessão a partir do último estado conhecido, reaplicando as validações de ventilação mínima e leitura válida de sensor antes de reativar aquecimento; se essas condições não forem atendidas, o sistema MUST entrar em estado seguro (aquecimento OFF, ventilação ligada) até decisão do operador.
- **FR-023**: O sistema MUST oferecer modo de configuração de rede via Access Point próprio com portal cativo na primeira utilização ou quando não houver credenciais válidas, permitindo ao operador inserir dados da rede Wi-Fi local sem ferramentas externas; após configurado, o sistema MUST conectar-se como cliente (STA) à rede local.
- **FR-024**: O sistema MUST manter um número fixo configurável de sessões de torra mais recentes (padrão de 30 sessões) e, ao atingir o limite de armazenamento, MUST descartar automaticamente a sessão mais antiga para abrir espaço para a nova, sem exigir intervenção manual do operador.
- **FR-025**: O sistema MUST aceitar comandos de controle de qualquer cliente web conectado à rede local ou do display sem mecanismo de bloqueio exclusivo entre essas fontes (last-write-wins), aplicando sempre as validações de segurança antes de executar qualquer comando recebido; quando em Modo Manual/Artisan, comandos do Artisan também participam desse mesmo esquema last-write-wins.
- **FR-026**: O sistema MUST aplicar um limite absoluto de temperatura de segurança de 260°C (leitura do sensor), independente do perfil ativo, desligando o aquecimento imediatamente e sinalizando alarme crítico caso esse limite seja atingido ou ultrapassado; o sistema MUST emitir um aviso prévio ao atingir 240°C para permitir ação manual do operador antes do corte automático.
- **FR-027**: O sistema MUST oferecer um comando dedicado de Parada de Emergência, sempre visível e acessível no display e na interface web, que corta o aquecimento imediatamente e coloca o sistema em estado seguro (ventilação ligada), distinto do fluxo normal de finalizar torra.
- **FR-028**: O sistema MUST permitir que a transição para a etapa de resfriamento seja acionada manualmente pelo operador a qualquer momento, e MUST também permitir que perfis de torra definam um ponto de drop que dispare essa transição automaticamente quando atingido.
- **FR-029**: O sistema MUST exigir reconhecimento manual do operador para limpar alarmes críticos (corte de segurança por temperatura, falha de sensor, falha indireta de ventilação detectada via FR-030, parada de emergência) antes que a torra possa continuar, mesmo que a condição que originou o alarme já tenha sido resolvida.
- **FR-030**: O sistema MUST monitorar a taxa de variação da temperatura (RoR) durante aquecimento como indicador indireto de falha de ventilação (ex.: ventoinha travada ou desconectada mecanicamente); ao detectar padrão anormal de subida de temperatura incompatível com o comando de ventilação ativo, o sistema MUST desligar o aquecimento automaticamente e sinalizar alarme crítico.
- **FR-031**: O sistema MUST permitir exportar sessões de torra concluídas pela interface web em formato CSV e/ou no formato nativo do Artisan (.alog), para análise ou compartilhamento posterior independentemente de conexão ao vivo com o Artisan.
- **FR-032**: O sistema MUST permitir ao operador configurar um offset de calibração para o sensor de temperatura, acessível pela tela de teste de periféricos, validado contra uma referência conhecida (ex.: água fervendo); esse offset MUST ser aplicado a todas as leituras subsequentes até ser reconfigurado.
- **FR-033**: O sistema MUST aplicar um limite máximo configurável de duração de torra (padrão de 25 minutos) como rede de segurança; ao atingir esse limite, o sistema MUST sinalizar alarme crítico e forçar transição automática para resfriamento, mesmo sem ação do operador.
- **FR-034**: O sistema MUST armazenar em cada sessão de torra uma cópia (snapshot) da curva e dos parâmetros do perfil utilizado no momento da execução, de forma que a exclusão posterior do perfil original NÃO afete a visualização ou integridade do histórico de torras já realizadas.
- **FR-035**: O sistema MUST sinalizar visualmente no display e na interface web quando a temperatura de pré-aquecimento alvo do perfil for atingida, indicando que está pronto para carregar os grãos, e MUST aguardar confirmação do operador antes de iniciar a contagem de tempo de torra e marcar o evento de carga (CHARGE).
- **FR-036**: O sistema MUST exibir a curva de RoR (taxa de variação de temperatura por minuto) junto com a curva de temperatura (BT) tanto no display quanto na interface web, tanto durante a torra ativa quanto na revisão de sessões concluídas.
- **FR-037**: O sistema MUST calcular e exibir em tempo real o DTR% (Development Time Ratio) a partir do momento em que o evento de first crack for marcado, comparando o tempo decorrido desde o first crack contra o tempo total de torra até aquele instante.
- **FR-038**: O sistema MUST oferecer suporte a múltiplos idiomas na interface local (display) e na interface web, incluindo inglês, português e espanhol, com inglês como idioma padrão; o operador MUST poder trocar o idioma da interface a qualquer momento pelas configurações do sistema.
- **FR-039**: O sistema MUST permitir ao operador selecionar, antes de iniciar a torra, entre o Modo Perfil (execução guiada por curva de perfil salvo, com Artisan em somente leitura) e o Modo Manual/Artisan (sem curva de perfil própria, com ajuste direto de ventilação e aquecimento pelo operador via display/web e/ou por comandos recebidos do Artisan).
- **FR-040**: O sistema MUST permitir que o operador troque do Modo Perfil para o Modo Manual/Artisan durante uma torra ativa, exigindo confirmação explícita que informe claramente que essa troca é irreversível para a sessão atual; o sistema NÃO MUST permitir retornar ao Modo Perfil na mesma sessão após essa troca.
- **FR-041**: O sistema MUST permitir exportar e importar perfis de torra em formato JSON pela interface web, para backup e compartilhamento independente do armazenamento interno em NVS.
- **FR-042**: Quando a sessão de torra ativa estiver no Modo Perfil, o sistema MUST sobrepor a curva-alvo do perfil selecionado à curva real (BT) no gráfico ao vivo, tanto no display quanto na interface web, permitindo comparação visual entre o planejado e o executado.
- **FR-043**: O sistema MUST desenhar cada evento de torra marcado (turning point, dry end, first crack início/fim, second crack início/fim, drop, início do resfriamento, entre outros de FR-018) como uma linha vertical de referência no gráfico da curva, tanto no gráfico ao vivo durante a torra quanto na revisão de sessões concluídas, no display e na interface web.
- **FR-044**: O sistema MUST manter paridade funcional entre display e interface web para o gráfico ao vivo, incluindo a curva real, a sobreposição da curva-alvo do perfil (quando aplicável) e as linhas verticais de eventos marcados.
- **FR-045**: O sistema MUST oferecer, no display local, um menu de navegação lateral fixo (estilo Material Design) sempre visível, permitindo alternar entre as telas de Torra (dashboard ao vivo), Perfis, Histórico e Configurações, com indicação visual clara da tela ativa.
- **FR-046**: O sistema MUST consolidar em uma tela de Configurações (acessível pelo menu lateral) o acesso a troca de idioma (FR-038), calibração de sensor (FR-032), teste de periféricos (FR-014) e configuração de rede Wi-Fi (FR-023).

### Key Entities *(include if feature involves data)*

- **Roast Profile**: Define uma estratégia de torra reutilizável com curva alvo, limites operacionais, fases e parâmetros padrão de ventilação/aquecimento.
- **Roast Session**: Representa uma execução real de torra com timeline de leituras, comandos aplicados, eventos marcados e resultado final.
- **Hardware Channel**: Representa cada atuador/sensor controlado ou monitorado (aquecimento, ventilação, temperatura) com estado, limites e diagnóstico.
- **Safety Rule**: Representa regras de intertravamento e proteção que validam comandos antes da aplicação.
- **Batch Record**: Registro de lote contendo metadados do café, referência ao perfil usado, sessão executada e notas pós-torra.
- **Firmware Package**: Arquivo de atualização submetido via web, com metadados de versão e status de validação/instalação.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 100% das tentativas de aquecimento com ventilação abaixo do mínimo fixo de 30% são bloqueadas automaticamente.
- **SC-002**: Pelo menos 95% dos operadores conseguem iniciar uma torra e chegar ao resfriamento sem assistência em até 5 minutos de uso da interface.
- **SC-003**: Em sessões de teste contínuo de 30 minutos, a atualização visual da curva de torra ocorre sem lacunas perceptíveis para o operador.
- **SC-004**: Pelo menos 90% dos perfis salvos permanecem utilizáveis sem retrabalho após ciclo de desligar e ligar o equipamento.
- **SC-005**: Em pelo menos 95% das sessões monitoradas por web, o operador consegue acompanhar e ajustar parâmetros em tempo real sem perda de controle local.
- **SC-006**: A execução de testes de periféricos permite identificar corretamente o periférico com falha em pelo menos 95% dos cenários simulados.

## Assumptions

- O projeto é voltado inicialmente para operação por um único usuário por equipamento.
- O equipamento será usado em ambiente supervisionado, com operador presente durante toda a torra.
- O sistema prioriza segurança e continuidade do controle local mesmo com falhas de rede.
- Perfis e históricos serão armazenados localmente no dispositivo embarcado na primeira versão.
- A compatibilidade com Artisan cobre os dados essenciais de monitoramento e registro de torra esperados pelo usuário final.
- O escopo inicial não inclui controle automático fechado por receita com autoajuste sem intervenção humana.
- O hardware de potência e isolamento elétrico será montado conforme boas práticas elétricas aplicáveis para a carga da pipoqueira.
- A rede local (Wi-Fi doméstica/bancada) é considerada confiável na v1; não há autenticação para controle web ou upload de firmware, ficando a proteção de acesso a cargo do isolamento da rede do usuário.
- O sensor MAX6675 fica posicionado dentro da câmara em contato direto com os grãos, portanto sua leitura é tratada como BT (Bean Temperature) real em toda a especificação e na compatibilidade com o Artisan.
- A proteção contra falha mecânica do próprio módulo SSR (travamento no estado ligado) depende exclusivamente do corte por software em 260°C (FR-026); não há dispositivo de segurança físico independente definido nesta especificação. Este é um risco residual assumido conscientemente para a v1.
- O sistema não depende de relógio real (RTC/NTP); sessões e eventos são registrados com timestamps relativos ao início de cada sessão de torra.
- O hardware não possui buzzer ou sinalização sonora; todos os alarmes, avisos e sinalizações (incluindo pré-aquecimento pronto e alarmes críticos) são exclusivamente visuais, exibidos no display local e na interface web.
