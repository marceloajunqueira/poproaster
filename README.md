![Pop Roaster](images/poproaster.jpg)

# Pop Roaster

Um torrador de café caseiro feito em cima de uma pipoqueira elétrica de ar
quente, controlado por um ESP32-S3. A ideia é pegar um equipamento barato e
transformar ele num torrador de verdade, com controle de temperatura,
perfis de torra e acompanhamento em tempo real.

A ideia é uma pipoqueira barata, *overengineered* de propósito, para torrar
café de forma barata. Ele consegue torrar até **80 gramas** de café por vez.

![Pop Roaster em funcionamento](images/pop.gif)

## BOM (lista de componentes)

![BOM](images/bom.jpg)

| Item | Part Number | Link | Comentário |
|---|---|---|---|
| Pipoqueira | | [link](https://shopee.com.br/Pipoqueira-El%C3%A9trica-Super-Premium-1200W-Sem-%C3%93leo-Ar-Quente-i.389800143.22498754445) | |
| Display | JC4827W543 | [link](https://pt.aliexpress.com/item/1005006729377800.html) | Selecionar Capacitive Touch |
| SSR | SSR-40DA | [link](https://pt.aliexpress.com/item/1005005943909513.html) | Aquecedor |
| Módulo MOSFET (PWM do fan) | XY-MOS | [link](https://pt.aliexpress.com/item/1005012364216704.html) | |
| Termopar | MAX6675 | [link](https://pt.aliexpress.com/item/1005003841451196.html) | Selecionar Kit with probe |
| Fonte | 24V 2A | [link](https://br.shp.ee/zNRZV278) | Alimenta o Mini560 e o XY-MOS |
| Step-down | Mini560 (saída 5V) | [link](https://pt.aliexpress.com/item/1005008280160694.html) | Alimenta o display (conector P1) a partir dos 24V |
| Capacitor cerâmico | 100nF | | Soldado nos pads do termopar do MAX6675, para estabilizar a leitura |
| Manta térmica de escapamento de moto (fibra de vidro) | | | Isola o aquecedor da carcaça de metal; o kit já vem com as braçadeiras (usei 2) |
| Fita de alumínio fina | | | Cobre a manta, para bloquear a radiação IR |
| Conectores/cabos JST1.25 de 4 pinos | | [link](https://pt.aliexpress.com/item/1005007277110532.html) | Selecionar o de 4 pinos. 3 unidades (P1, P2 e P4) |

## Montagem

A pinagem, o diagrama de ligações e as observações da montagem estão em
[`firmware/README.md`](firmware/README.md#hardware-wiring) (diagrama em
[`firmware/docs/wiring-diagram.svg`](firmware/docs/wiring-diagram.svg)). Os
arquivos 3D (suportes, caixa do display e STEP dos módulos) estão na pasta
[`3D/my-b022`](3D/my-b022).

> **Atenção - termostato original:** o termostato que vem na pipoqueira desarma
> em 180 °C e precisa ser **removido por completo**, senão ele corta o
> aquecedor no meio da torra e o processo falha. Sem ele, a proteção contra
> superaquecimento passa a ser só a do firmware (corte em 240 °C).
>
> **Atenção - rede elétrica:** o SSR e a fonte trabalham com tensão da rede.
> Use fiação e conectores adequados à corrente do aquecedor, isole tudo e
> aterre a carcaça. As proteções do firmware **não** substituem uma instalação
> elétrica correta.

## O que ele faz

- Tela touch no próprio equipamento com dashboard da torra ao vivo (BT -
  temperatura do grão, RoR - taxa de subida, tempo).
- Dashboard também pelo navegador (Wi-Fi), com os mesmos controles, além das
  páginas de presets, histórico, diagnóstico (ajuste do PID e autotune) e
  atualização de firmware.
- Modo Profile (perfil de torra) e modo Manual (você define a temperatura
  alvo e o nível do fan).
- Perfis de torra configuráveis por segmento (tempo + temperatura + fan), com
  rampa de temperatura entre os segmentos (evita ficar "parado" num degrau).
  Podem ser criados/editados na tela ou pelo navegador, e importados/exportados.
- Controle de temperatura em malha fechada (PID) - você escolhe a
  temperatura, o firmware calcula a potência do aquecedor. O ganho do PID pode
  ser ajustado (ou calculado por autotune) pela página de diagnóstico.
- Fan com 3 níveis fixos (90 / 95 / 100%).
- Histórico das torras salvo no próprio equipamento, com gráfico de BT x tempo
  e exportação em CSV.
- Integração com o Artisan via Modbus TCP
  ([passo a passo](firmware/docs/artisan-integration.md)).
- Atualização de firmware por OTA (sem precisar abrir o equipamento).
- Interface em inglês, português e espanhol (na tela, em Config > Language).
- Camadas de segurança: aviso em 230 °C e corte automático por temperatura em
  240 °C, watchdog de duração da torra, detecção de falha de sensor/ventoinha,
  botão de Emergency Stop sempre visível, etc.

## Gravar o firmware

Os binários ficam em [Releases](https://github.com/marceloajunqueira/poproaster/releases)
(criadas ao publicar uma tag `vX.Y.Z`, ex.: `git tag v0.1.0 && git push origin v0.1.0`):

- `pop_roaster-full.bin`: imagem completa, para gravar no offset `0x0` (primeira instalação).
- `pop_roaster-ota.bin`: só o app, para atualização por OTA.

Os binários de cada build também ficam como artifact na execução do workflow.

Sem instalar nada: abra a página do flasher em
<https://marceloajunqueira.github.io/poproaster/> (Chrome ou Edge), conecte o
display por USB e clique em **Instalar firmware**. A página é publicada
automaticamente pelo workflow [`firmware.yml`](.github/workflows/firmware.yml)
a cada alteração em `firmware/` ou `flasher/` na branch `main`.

### Atualização por OTA

Com o equipamento na sua rede Wi-Fi, abra `http://poproaster.local/ota` (ou o
IP do equipamento) e envie o arquivo `pop_roaster-ota.bin`.

## Primeiro uso (Wi-Fi)

Sem rede configurada, o equipamento cria a rede aberta **PopRoaster-Setup**.
Conecte nela e informe o SSID e a senha da sua rede. Depois disso, o dashboard
fica em `http://poproaster.local/` (ou no IP atribuído pelo roteador). A rede
também pode ser trocada depois pela página `/wifi`.

O dashboard, o Modbus TCP do Artisan e a atualização por OTA **não têm
autenticação**: use apenas em rede local confiável e não exponha o
equipamento à internet.

## Compilar o firmware

Requer ESP-IDF v6.0 e o target `esp32s3`:

```powershell
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p <PORTA> flash monitor
```

Mais detalhes técnicos (build, pinagem, segurança, etc.) estão em
[`firmware/README.md`](firmware/README.md).
