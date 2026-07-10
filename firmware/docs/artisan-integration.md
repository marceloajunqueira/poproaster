# Integração com o Artisan (Modbus TCP)

Este documento explica como configurar o [Artisan](https://artisan-scope.org/) para ler a temperatura/RoR e (opcionalmente) controlar o Fan/Heater do PopRoaster pela rede, sem cabo serial e sem nenhum programa extra no PC.

O PopRoaster expõe um **servidor Modbus TCP** na porta **502** (implementado em `firmware/components/artisan_adapter/artisan_bridge.c`). O Artisan tem suporte nativo a Modbus (confirmado no código-fonte dele, `artisanlib/modbusport.py`), então basta apontar o Artisan para o IP do torrador.

> **Nota**: os passos exatos (nomes de botões/abas) foram confirmados lendo o código-fonte e os arquivos de ajuda do Artisan, mas não foram testados clicando na interface real do Artisan neste ambiente. Se algum nome de campo estiver ligeiramente diferente na sua versão, o conceito (Modbus, TCP, host/porta, registradores) é o mesmo.

## 1. Pré-requisitos

- O torrador precisa estar conectado à sua rede WiFi (não no modo "PopRoaster-Setup").
- Descubra o IP do torrador: tela **Config** no display (mostra `http://<ip>/wifi` e `http://<ip>/ota`) ou a página `/wifi` / `/diagnostics` na web.
- Anote esse IP - vamos usar como "Host" no Artisan.

## 2. Configurar o dispositivo Modbus no Artisan

1. Menu **Config → Device...** (ou **Port...**/**Comm...**, dependendo da versão - é a tela onde se escolhe o "Meter"/dispositivo de leitura de temperatura, **não** a lista de marcas/modelos de torrador ("Machine"), que não tem opção Modbus).
2. Em **Meter**, selecione **Modbus**.
3. Abra as configurações de porta (botão **Port...** ou **Comm...** dentro dessa mesma tela) e configure:
   - **Comm Type / Mode**: `TCP`
   - **Host**: o IP do torrador (ex.: `192.168.5.4`)
   - **Port**: `502`
   - **Byte/Word order**: não é usado (não usamos registradores float de 32 bits, veja a tabela abaixo) - pode deixar no padrão.

## 3. Mapear os canais (registradores)

O PopRoaster usa **registradores Holding (function code 3 para leitura, 6 para escrita)**, com valores inteiros escalados (sem float), para evitar qualquer ambiguidade de "word order". Configure os canais assim:

| Canal Artisan | Device ID (Slave) | Registrador | Function | Div | Signed |
|---|---|---|---|---|---|
| BT (Bean Temp) | 1 | 0 | 3 (Read Holding Registers) | 1/10 | não |
| Channel extra "Fan" | 1 | 1 | 3 | nenhum | não |
| Channel extra "Heater" | 1 | 2 | 3 | nenhum | não |
| Channel extra "RoR" (opcional - o Artisan já calcula RoR sozinho a partir do BT) | 1 | 3 | 3 | 1/10 | **sim** |

- **ET (Environment Temp)**: deixe o Device ID do ET como `0` (desliga esse canal) - o PopRoaster só tem um sensor (BT).
- **Div = 1/10**: o registrador guarda a temperatura multiplicada por 10 (ex.: `2350` = 235.0°C) - o Artisan divide de volta automaticamente.
- **Signed** no canal de RoR: o valor pode ficar negativo durante o resfriamento.

## 4. Controlar Fan/Heater pelo Artisan (opcional)

Isso só funciona quando o torrador está em **modo Manual/Artisan** (se estiver em modo Perfil, os comandos do Artisan são ignorados de propósito - trava de segurança do projeto).

1. Menu **Config → Events → Sliders...** (Event Custom Sliders).
2. Crie (ou reaproveite) um slider, marque para aparecer (**Event**), e configure:
   - **Action**: `Modbus Command`
   - **Command**:
     - Slider de Fan: `writeSingle(1,100,{})`
     - Slider de Heater: `writeSingle(1,101,{})`
   - **Min**: `0`, **Max**: `100`
3. Ao soltar o slider, o Artisan escreve o valor (0-100) no registrador correspondente via Modbus function 6, e o torrador aplica o comando (Fan/Heater) exatamente como se tivesse sido feito pelo display ou pela web.

Você também pode usar um **Event Button** com a mesma Action `Modbus Command` e um comando fixo, por exemplo `writeSingle(1,101,0)` para "desligar o heater".

## 5. Tabela de registradores (referência rápida)

| Registrador | Conteúdo | Leitura/Escrita | Observação |
|---|---|---|---|
| 0 | Bean Temp × 10 | Leitura | Div=1/10 no Artisan |
| 1 | Fan % (0-100) | Leitura | valor real aplicado no ventilador |
| 2 | Heater % (0-100) | Leitura | valor real aplicado na resistência |
| 3 | RoR × 10 (com sinal) | Leitura | Div=1/10, Signed |
| 100 | Set Fan % (0-100) | **Escrita** | só tem efeito em modo Manual/Artisan |
| 101 | Set Heater % (0-100) | **Escrita** | só tem efeito em modo Manual/Artisan |

## 6. Testando

1. No Artisan, clique em **ON** (ligar a leitura). O BT deve aparecer e acompanhar a leitura do display do torrador.
2. Se não conectar: confirme que o PC e o torrador estão na mesma rede, que o IP está correto, e que nenhum firewall do PC está bloqueando a porta 502 de saída.
3. Se os valores de temperatura aparecerem estranhos (ex.: 10x maior/menor): confira se o Div=1/10 foi realmente aplicado ao canal de BT.

## Referências

- Firmware: `firmware/components/artisan_adapter/artisan_bridge.h` (mapa de registradores documentado no código também).
- Protocolo confirmado em `artisanlib/modbusport.py` no repositório oficial do Artisan: <https://github.com/artisan-roaster-scope/artisan>.
- Comandos de slider/botão Modbus (`writeSingle`, `read`, etc.) documentados na ajuda embutida do Artisan (Config → Events → Sliders/Buttons → botão de ajuda).
