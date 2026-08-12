# Pop Roaster

Um torrador de café caseiro feito em cima de uma pipoqueira elétrica de ar
quente, controlado por um ESP32-S3. A ideia é pegar um equipamento barato e
transformar ele num torrador de verdade, com controle de temperatura,
perfis de torra e acompanhamento em tempo real.

## BOM (lista de componentes)

| Item | Part Number | Link | Comentário | Preço |
|---|---|---|---|---|
| Pipoqueira | | [link](https://shopee.com.br/Pipoqueira-El%C3%A9trica-Super-Premium-1200W-Sem-%C3%93leo-Ar-Quente-i.389800143.22498754445) | | R$ 120,00 |
| Display | JC4827W543 | [link](https://pt.aliexpress.com/item/1005006729377800.html) | Selecionar Capacitive Touch | R$ 115,00 |
| SSR | SSR-40DA | [link](https://pt.aliexpress.com/item/1005005943909513.html) | | R$ 9,50 |
| PWM | XY-MOS | [link](https://pt.aliexpress.com/item/1005012364216704.html) | | R$ 3,36 |
| Termopar | MAX6675 | [link](https://pt.aliexpress.com/item/1005003841451196.html) | Selecionar Kit with probe | R$ 10,79 |
| Fonte | 24V 2A | [link](https://br.shp.ee/zNRZV278) | | R$ 19,90 |

## O que ele faz

- Tela touch no próprio equipamento com dashboard da torra ao vivo (BT, RoR, tempo).
- Dashboard também pelo navegador (Wi-Fi), com os mesmos controles.
- Perfis de torra configuráveis por segmento (tempo + temperatura + fan), com
  rampa de temperatura entre os segmentos (evita ficar "parado" num degrau).
- Controle de temperatura em malha fechada (PID) - você escolhe a
  temperatura, o firmware calcula a potência do aquecedor.
- Histórico das torras salvo no próprio equipamento, com gráfico de BT x tempo.
- Integração com o Artisan via Modbus TCP.
- Atualização de firmware por OTA (sem precisar abrir o equipamento).
- Camadas de segurança: corte automático por temperatura, watchdog de
  duração da torra, detecção de falha de sensor/ventoinha, etc.

Mais detalhes técnicos (build, pinagem, etc.) estão em
[`firmware/README.md`](firmware/README.md).
