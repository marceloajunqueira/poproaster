# Quickstart: Pop Roaster Control Platform Validation

Este guia valida o comportamento ponta a ponta do recurso, sem entrar em detalhes de implementação interna.

## Pré-requisitos

- Protótipo com ESP32 + display JC4827W543.
- MAX6675 conectado e com leitura válida.
- Controle da resistência via SSR 40A.
- Controle da ventoinha via PWM 15A.
- Rede local para acesso web.
- Cliente Artisan instalado (opcional para cenários de compatibilidade).

## Cenário 1: Segurança básica de aquecimento (obrigatório)

Objetivo: garantir que aquecimento nunca opere com ventilação inválida e que a Parada de Emergência sempre funcione.

Passos:
1. Iniciar sistema e entrar no Modo Manual/Artisan.
2. Ajustar ventoinha para abaixo do piso fixo de 30%.
3. Tentar ativar aquecimento.
4. Ajustar ventoinha para 30% ou mais e ativar aquecimento.
5. Simular falha de sensor de temperatura.
6. Reconhecer manualmente o alarme crítico gerado.
7. Reiniciar aquecimento e acionar a Parada de Emergência.

Resultado esperado:
- Passo 3: comando de aquecimento bloqueado e alarme exibido.
- Passo 4: aquecimento aceito somente com ventilação válida (>= 30%).
- Passo 5: aquecimento desligado automaticamente e alarme crítico sinalizado.
- Passo 6: torra só pode prosseguir após reconhecimento manual do alarme.
- Passo 7: Parada de Emergência corta aquecimento imediatamente, distinta do fluxo normal de finalizar torra.

## Cenário 2: Fluxo operacional completo (pré-aquecimento -> torra -> resfriamento)

Objetivo: validar o fluxo mínimo de operação para usuário iniciante, incluindo confirmação de carga, RoR/DTR e watchdog.

Passos:
1. Iniciar sessão de torra em Modo Perfil.
2. Aguardar sinalização visual de pré-aquecimento pronto e confirmar carga dos grãos (evento CHARGE).
3. Acompanhar curva de BT e RoR no display durante a torra.
4. Marcar evento de first crack e observar o DTR% calculado em tempo real.
5. Fazer ajuste manual de ventilação/aquecimento e verificar que o perfil retoma controle no próximo ponto da curva.
6. Entrar em resfriamento (manual ou automático via ponto de drop do perfil).
7. (Opcional) Deixar a torra ativa além do limite de duração configurado (padrão 25 min) sem intervenção.

Resultado esperado:
- Curva de BT e RoR atualizam em tempo real.
- CHARGE só é registrado após confirmação do operador.
- DTR% aparece assim que first crack é marcado.
- Ajuste manual vale até o próximo ponto da curva, depois o perfil retoma.
- Em resfriamento, aquecimento permanece desligado e ventilação ativa.
- Passo 7: ao atingir o limite de duração, o sistema força resfriamento automático e alarme crítico.

## Cenário 3: Perfis, persistência em NVS e backup

Objetivo: validar criação, reuso e backup de perfis de torra.

Passos:
1. Criar perfil com curva interativa (gráfico + tabela numérica).
2. Salvar perfil.
3. Exportar o perfil em JSON pela interface web.
4. Reiniciar o equipamento.
5. Carregar perfil salvo e iniciar nova sessão em Modo Perfil.
6. Excluir o perfil original e verificar sessões históricas anteriores.
7. Importar o arquivo JSON exportado no passo 3.

Resultado esperado:
- Perfil permanece íntegro após reinicialização.
- Sessão usa parâmetros e curva do perfil selecionado.
- Sessões históricas mantêm seu snapshot próprio da curva mesmo após o perfil original ser excluído.
- Importação do JSON recria o perfil corretamente.

## Cenário 4: Web UI em paridade funcional mínima

Objetivo: validar acompanhamento e comando remoto em rede local.

Passos:
1. Abrir interface web no celular/computador.
2. Acompanhar gráfico e estado da torra.
3. Aplicar ajuste de ventilação.

Resultado esperado:
- Dados exibidos em tempo real de forma consistente com display.
- Comandos aplicados via web passam por validação de safety.
- Perda de conexão web não afeta controle local.

## Cenário 5: Compatibilidade com Artisan (bidirecional gated por modo)

Objetivo: validar contrato de telemetria externa e controle condicionado ao modo.

Passos:
1. Conectar dispositivo ao Artisan (serial USB ou bridge TCP local).
2. Iniciar torra em Modo Perfil e enviar um comando de controle a partir do Artisan.
3. Trocar para Modo Manual/Artisan (confirmando a mensagem de irreversibilidade).
4. Enviar novamente um comando de controle a partir do Artisan.
5. Marcar evento manual e observar stream de dados (BT, RoR) no software.

Resultado esperado:
- Passo 2: comando do Artisan é ignorado (Modo Perfil = somente leitura).
- Passo 3: sistema exibe aviso de que a troca é irreversível para a sessão.
- Passo 4: comando do Artisan é aceito e validado pelas regras de segurança.
- Passo 5: evento manual aparece na linha do tempo do cliente externo; desconexão/reconexão do cliente não interrompe torra local.

## Cenário 5b: Exportação de sessões concluídas

Objetivo: validar exportação de dados históricos.

Passos:
1. Concluir uma sessão de torra.
2. Exportar a sessão em formato CSV pela interface web.
3. Exportar a mesma sessão em formato `.alog` do Artisan.

Resultado esperado:
- Ambos os arquivos são gerados corretamente, independente de conexão ao vivo com o Artisan.

## Cenário 6: Tela de testes de periféricos e calibração de sensor

Objetivo: validar manutenção, diagnóstico básico e calibração.

Passos:
1. Abrir tela de testes.
2. Executar teste do sensor.
3. Executar teste da ventoinha.
4. Executar teste da resistência com confirmação explícita.
5. Aplicar offset de calibração no sensor usando referência conhecida (ex.: água fervendo a 100°C).

Resultado esperado:
- Cada periférico retorna status independente.
- Teste de resistência só executa após confirmação do operador.
- Offset de calibração é aplicado a todas as leituras subsequentes.

## Cenário 7: Atualização de firmware via upload (esquema A/B)

Objetivo: validar manutenção remota segura sem risco de brick.

Passos:
1. Abrir tela web de atualização.
2. Enviar arquivo de firmware inválido.
3. Enviar arquivo de firmware válido.
4. Aguardar validação/instalação na partição secundária.
5. Confirmar versão após reinício.
6. Simular falha durante a instalação (ex.: corte de energia).

Resultado esperado:
- Passo 2: upload inválido é rejeitado com mensagem clara.
- Passo 3-5: upload válido instala com sucesso na partição secundária, chaveia o boot e confirma nova versão.
- Passo 6: sistema permanece na partição atual válida, sem interrupção do funcionamento.

## Cenário 8: Troca de idioma (i18n)

Objetivo: validar suporte a múltiplos idiomas.

Passos:
1. Verificar que o idioma padrão ao primeiro uso é inglês (EN).
2. Trocar o idioma para português (PT) pelas configurações.
3. Trocar o idioma para espanhol (ES).

Resultado esperado:
- Interface local (display) e web refletem o idioma selecionado imediatamente.
- Nenhuma funcionalidade é perdida na troca de idioma.

## Referências

- Contrato web: contracts/web-api.yaml
- Contrato Artisan: contracts/artisan-compatibility.md
- Modelo de dados: data-model.md
