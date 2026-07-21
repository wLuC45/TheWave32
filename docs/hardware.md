# Referência de hardware — placa ESP32-S3 da v0rtex

Este documento é um perfil detalhado, extraído majoritariamente por máquina, da
placa de desenvolvimento ESP32-S3 específica que alimenta os módulos TheWave32 inclusos.
Cada valor aqui foi obtido diretamente do chip em
2026-05-06 (logs de boot `esp_rom_printf` ao vivo, saída de chip-id do esptool,
prints de `app_init` / `heap_init` / `octal_psram` / `wifi` / `phy_init`
do ESP-IDF e enumeração `pyserial.tools.list_ports`) ou lido do
próprio efuse do chip.

Se você receber uma placa S3 "diferente", espere que os fatos *internos do SoC*
(mapa de memória, árvore de clock, conjunto de periféricos, matriz de GPIO, pinos de strapping)
sejam iguais. Espere que os fatos *de nível de placa* (qual ponte USB-serial,
quais pinos são expostos, se o USB nativo está em um conector)
sejam diferentes.

Tudo que possui carimbo de data/hora ou versão reflete o estado em
2026-05-06 com o firmware `wifi-probe-logger v0.2.0` em execução.

---

## 0. Resumo

| Característica              | Valor                                                         |
| --------------------------- | ------------------------------------------------------------- |
| SoC                         | ESP32-S3 (QFN56) revisão **v0.2**                            |
| CPU                         | Dual Xtensa LX7, **240 MHz**                                  |
| Coprocessador               | LP Core (RISC-V, ULP)                                         |
| Cristal                     | **40 MHz**                                                    |
| SRAM interna (heap)         | ~338 KiB utilizável em 4 regiões (veja §3)                    |
| PSRAM                       | **8 MB** Octal, AP @ 3 V, geração 3, 80 MHz                   |
| Flash                       | **16 MB**, MXIC, modo DIO, frequência 80 MHz                  |
| Wi-Fi                       | 802.11 b/g/n + **LR** da Espressif, ROM `e7ae62f`, FW `79fa3f41ba`, cert `v7.0` |
| Bluetooth                   | BT 5 (**somente LE**, sem Classic)                            |
| 802.15.4 (Thread/Zigbee)    | Presente no SoC, **não usado por nenhum módulo v1**           |
| MAC Wi-Fi STA               | `dc:b4:d9:13:aa:34`                                           |
| Endereço público BLE        | `dc:b4:d9:13:aa:36` (= MAC STA + 2)                           |
| Bootloader ROM              | `esp32s3-20210327`, compilado em 27 Mar 2021                  |
| Alvo ESP-IDF                | v5.4.1                                                        |
| Ponte USB-serial            | WCH **CH343** serial única (UART0)                            |
| USB ID (CH343)              | VID `0x1A86` PID `0x55D3`                                     |
| Dispositivo Linux           | **`/dev/ttyACM0`** (CDC ACM, 115200 8N1)                      |
| USB nativo S3 (GPIO19/20)   | **NÃO exposto externamente** nesta placa                      |
| Grupo de usuário necessário | `uucp` (Arch) / `dialout` (Debian/Ubuntu)                     |
| esptool                     | Compatível com v5.x (usa subcomandos com hífen)               |
| Último motivo de reset visto | `0x1 (POWERON)`, também vistos `0x8 (SPI_FAST_FLASH_BOOT)`, `0xc (RTC_SW_CPU_RST)` |

---

## 1. Identificando esta placa

### 1.1 Verificações programáticas

```bash
# 1) O Linux enxerga o chip USB-serial
ls /dev/ttyACM*                      # → /dev/ttyACM0
.venv/bin/python -c "
from serial.tools import list_ports
for p in list_ports.comports():
    if p.vid: print(p.device, hex(p.vid), hex(p.pid), p.product)
"
# → /dev/ttyACM0 0x1a86 0x55d3 'USB Single Serial'

# 2) esptool consegue se comunicar com ele
source ~/ESP32S3/export.sh
python -m esptool --port /dev/ttyACM0 chip-id
# → Chip type: ESP32-S3 (QFN56) (revision v0.2)
# → Features:  Wi-Fi, BT 5 (LE), Dual Core + LP Core, 240MHz,
# →            Embedded PSRAM 8MB (AP_3v3)
# → Crystal frequency: 40MHz
# → MAC: dc:b4:d9:13:aa:34
```

### 1.2 Assinatura do bootloader ROM

Cada reset imprime (da ROM, antes de qualquer coisa que você tenha gravado ser executada):

```
ESP-ROM:esp32s3-20210327
Build:Mar 27 2021
```

Esta revisão de ROM específica é a padrão para chips ESP32-S3
fabricados a partir do final de 2021. A data é a data de compilação da ROM, não uma
data de fabricação.

### 1.3 Causa do reset

A próxima linha da ROM decodifica o último reset:

| Padrão                                         | Significado                                       |
| ---------------------------------------------- | ------------------------------------------------- |
| `rst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)` | Alimentação ligada, boot normal por flash         |
| `rst:0x3 (DEEPSLEEP),boot:...`                   | Despertar do deep sleep                           |
| `rst:0xc (RTC_SW_CPU_RST),boot:...`               | `esp_restart()`, `abort()` ou task watchdog disparado |
| `rst:0xf (RTCWDT_RTC_RESET)`                      | Reset do watchdog RTC                             |
| `rst:0x10 (RTCWDT_TASK_RESET)`                    | Task watchdog                                     |
| `rst:0x16 (RTC_SLOW_CLK_FAULT)`                   | Falha do clock lento RTC                          |

`boot:0x8 (SPI_FAST_FLASH_BOOT)` é o único estado que devemos ver
normalmente — boot da flash SPI interna com sondagem em modo rápido.

---

## 2. Capacidades do SoC (Espressif ESP32-S3)

Coisas que o silício suporta. Apenas um subconjunto está conectado em nossos
módulos de firmware v1 — mas toda característica abaixo está disponível se você quiser adicionar um
módulo que a utilize.

### 2.1 Computação

- 2× **Xtensa LX7** cores, 240 MHz máx cada.
- **LP core** (coprocessador de ultrabaixo consumo) — RISC-V no S3, executa a partir da
  RAM RTC, pode rodar enquanto a CPU principal está em deep sleep. **Não usado** por
  nenhum módulo v1.
- **AES-128/192/256** por hardware (usado implicitamente por `esp-now` /
  `mbedtls`).
- **SHA-1 / SHA-2 (224/256/384/512)** por hardware.
- **HMAC** e **periférico de Assinatura Digital** por hardware.
- **RSA** por hardware (até 4096 bits).
- **RNG** por hardware (verdadeiramente aleatório quando Wi-Fi/BT ativo).

### 2.2 Memória

| Região            | Tamanho   | Faixa (típica)                  |
| ----------------- | --------- | ------------------------------- |
| SRAM Interna      | 512 KiB   | `0x3FC8_8000` – `0x3FCF_FFFF`   |
| RAM RTC (Lenta)   | 8 KiB     | `0x600F_E000` – `0x600F_FFFF`   |
| RAM RTC (Rápida)  | 8 KiB     | `0x600F_E000`+ (visão do LP-core) |
| PSRAM Externa     | até 8 MB  | via SPI Octal em pinos dedicados |
| Flash Externa     | até 32 MB | via SPI Octal/Quad              |

O layout real do heap relatado **nesta** placa na inicialização está no §3
abaixo.

### 2.3 Conectividade

- **Wi-Fi 4** (802.11 b/g/n) em 2,4 GHz, com o modo de longo alcance
  proprietário da Espressif (`WIFI_PROTOCOL_LR`).
- **Bluetooth 5 LE** — Coded PHY, 2 Mbps PHY, extensões de advertising,
  suporte a longo alcance. **Sem BR/EDR (Classic)** no S3.
- **ESP-NOW** — camada de enlace 2,4 GHz sem conexão da Espressif.
- **IEEE 802.15.4** — presente no chip, exposto via componente
  `ieee802154` do IDF (Thread, Zigbee). Não usado por nenhum módulo v1.
- **Sem Wi-Fi 5/6**, **sem UWB**, **sem celular**, **sem sub-GHz**.

### 2.4 USB

- **USB 1.1 OTG** (full-speed, 12 Mbit/s). Nativo, nos **GPIO19 (D-)**
  e **GPIO20 (D+)**. **Não exposto externamente** nesta placa, veja §6.
- **Controlador USB Serial / JTAG** — mesmos pinos, função alternativa.
  Quando o tinyusb está habilitado, o chip se enumera como `idVendor 0x303A,
  idProduct 0x4001` (vendor Espressif + USB JTAG/CDC).

### 2.5 Periféricos (quantidades)

| Periférico                          | Quantidade |
| ----------------------------------- | ---------- |
| UART                                | 3          |
| SPI (uso geral)                     | 4 (SPI2, SPI3 usuário; SPI0/1 são MSPI para flash/PSRAM) |
| I2C                                 | 2          |
| I2S                                 | 2          |
| LEDC (PWM)                          | 8 canais, 4 timers |
| MCPWM                               | 2 módulos  |
| Timer Group                         | 2 (4 timers gerais + 2 watchdogs) |
| RMT (onda de borda rica)            | 8 canais (4 TX + 4 RX) |
| Pulse Counter                       | 4 unidades / 8 canais |
| ADC                                 | 2 SAR ADCs, 12-bit, 20 canais no total |
| DAC                                 | **nenhum no S3** (diferente do ESP32 clássico) |
| Touch Sensor                        | 14 canais (capacitivos) |
| TWAI (barramento CAN)               | 1          |
| SDIO host                           | 1          |
| Interface paralela LCD/Camera       | 1 (modos DVP/I80/RGB) |
| Canais GDMA                         | 5 RX / 5 TX, assimétricos |

A ausência de DAC é a surpresa mais comum para quem migra do ESP32 clássico.
A reprodução de áudio tipicamente usa I2S ou LEDC PWM como alternativa.

### 2.6 Recursos de energia

- Modos Light sleep + Deep sleep.
- Fontes de wake do deep sleep: timer, GPIO, touch, ULP.
- Dois domínios de energia (LP / HP).

---

## 3. Mapa de memória nesta placa (boot ao vivo)

`heap_init` relata quatro regiões de heap imediatamente antes de `app_main`
executar, em um boot limpo:

| Índice | Endereço base | Comprimento   | Bucket  | Notas                                    |
| ------ | ------------- | ------------- | ------- | ---------------------------------------- |
| 0      | `0x3FCA_3F00` | `0x0004_5810` (278 KiB) | RAM Interna | Maior, pool padrão do `malloc`           |
| 1      | `0x3FCE_9710` | `0x0000_5724` (21 KiB)  | RAM Interna | Fragmento menor após .bss                |
| 2      | `0x3FCF_0000` | `0x0000_8000` (32 KiB)  | DRAM    | Pool DRAM (capaz de DMA)                 |
| 3      | `0x600F_E11C` | `0x0000_1ECC` (7 KiB)   | RTCRAM  | Sobrevive à maioria dos resets, pequeno  |
|        |               | **+ 8192 KiB**          | PSRAM   | Adicionado por `esp_psram` após init da psram |
|        |               | **− 32 KiB**            | (reserva DMA) | Reservado da RAM interna para DMA        |

Portanto, em estado estável, o alocador de heap pode disponibilizar:
- ~338 KiB de RAM interna (capaz de DMA + não-DMA), e
- 8 MB de PSRAM (mais lenta, sem DMA da CPU diretamente sem IRAM_ATTR).

Implicações práticas:

- Um `wifi_ap_record_t records[32]` (≈ 2,5 KiB) na **stack** de uma
  task de 4 KiB está no limite do overflow. Mantenha arrays maiores que 1 KiB fora
  das stacks de task.
- PSRAM é adequada para ringbufs grandes (sniffer / probe-logger); a
  configuração `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` mantém buffers
  de Wi-Fi/lwIP na PSRAM quando possível, liberando RAM interna.
- A reserva de 32 KiB exclusiva para DMA deve ser suficiente para rajadas
  de SPI / I2S.

### 3.1 Detalhes da PSRAM (do driver `octal_psram`)

```
vendor id    : 0x0d (AP)             — AP Memory
dev id       : 0x02 (generation 3)
density      : 0x03 (64 Mbit chip — but 8 MB total via die-stack)
good-die     : 0x01 (Pass)
Latency      : 0x01 (Fixed)
VCC          : 0x01 (3 V)
SRF          : 0x01 (Fast Refresh)
BurstType    : 0x01 (Hybrid Wrap)
BurstLen     : 0x01 (32 Byte)
Readlatency  : 0x02 (10 cycles @ Fixed)
DriveStrength: 0x00 (1/1)
MSPI Timing  : tuning index 6
Found 8MB PSRAM device, Speed: 80MHz
```

A placa é a variante OPI (octal) — barramento de dados de 8 bits + strobe DQS,
80 MHz. Configure com:

```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
```

A pinagem para PSRAM OPI é fixa e compartilhada com a flash OPI.
**Não** tente redirecionar esses pinos. O S3 possui
GPIO33–GPIO37 dedicados para flash SPI, GPIO26–GPIO32 para suporte a
flash octal/PSRAM. Na placa da v0rtex, estes estão ligados ao die
PSRAM no-chip.

---

## 4. Armazenamento flash

```
spi_flash: detected chip: mxic        — Macronix
spi_flash: flash io: dio              — Modo DIO (dados de 2 bits, cmd/addr de 1 bit)
Auto-detected flash size: 16 MB
```

- **Fabricante:** Macronix (mxic)
- **Tamanho:** 16 MB
- **Modo:** DIO (Dual I/O) — poderia ser QIO/QOUT, mas o padrão do ESP-IDF
  para compatibilidade é DIO. Mudar para QIO melhoraria marginalmente a
  latência de leitura com risco de compatibilidade com algumas placas.
- **Frequência:** 80 MHz (padrão com `CONFIG_ESPTOOLPY_FLASHFREQ_80M`).

### 4.1 Layout de memória do bootloader

Quando a ROM passa o controle para seu bootloader (qualquer um que você tenha gravado em
`0x0000`), ela relata os segmentos que carregou:

```
load:0x3fce2820,len:0x1120     — .data do bootloader na HP SRAM
load:0x403c8700,len:0x4        — cabeçalho minúsculo
load:0x403c8704,len:0xa84      — .text do bootloader na IROM
load:0x403cb700,len:0x2d1c     — continuação do .text do bootloader
entry 0x403c8890                — ponto de entrada do bootloader
```

Estes endereços mudam entre versões do ESP-IDF; os valores acima são
para o bootloader do IDF v5.4.1.

### 4.2 Layout de partição (padrão para nossos módulos)

Os módulos inclusos acompanham este `partitions.csv`:

```
nvs,      data, nvs,     0x9000,   0x6000     — 24 KiB NVS
phy_init, data, phy,     0xf000,   0x1000     — 4 KiB dados de calibração PHY
factory,  app,  factory, 0x10000,  0x1F0000   — ~2 MiB slot de app
```

Três módulos divergem:
- `ble-hid-keyboard` usa um NVS de 48 KiB (`0xC000`), empurrando `factory`
  para `0x20000`.

Slot 0x10000 → 0x200000 = ~2 MB. Todos os firmwares v1 cabem confortavelmente:

| Módulo               | Tamanho (bytes) | % da partição |
| -------------------- | --------------- | -------------- |
| `wifi-scanner`       | 631.104         | 30 %           |
| `wifi-sniffer`       | 627.168         | 30 %           |
| `ble-scanner`        | 532.880         | 25 %           |
| `ble-hid-keyboard`   | 536.704         | 27 %           |
| `espnow-bridge`      | 636.848         | 30 %           |
| `spectrum-analyzer`  | 627.008         | 30 %           |
| `wifi-probe-logger`  | 628.432         | 30 %           |
| `wifi-eap-watcher`   | 629.120         | 30 %           |

Há bastante espaço livre para partições OTA, NVS maior, ou um
SPIFFS para arquivos de payload.

---

## 5. Rádio

### 5.1 Wi-Fi (do log de boot)

```
pp rom version:           e7ae62f
net80211 rom version:     e7ae62f
wifi driver task:         3fcb5cb4, prio:23, stack:6656, core=0
wifi firmware version:    79fa3f41ba
wifi certification version: v7.0
phy_version:              700,8582a7fd, Feb 10 2025, 20:13:11
```

- O driver da camada MAC Wi-Fi executa como uma task FreeRTOS fixada no
  **core 0**, prioridade 23 (muito alta), stack de 6656 bytes. É por isso
  que toda task worker do TheWave32 é fixada no **core 1** — para deixar
  o core 0 para RF.
- A calibração `phy_init` executa uma vez por boot; os dados de calibração são
  armazenados em cache na partição `phy_init` para boots subsequentes mais rápidos.

#### Modos usados pelos módulos v1

| Módulo              | Modo                                  |
| ------------------- | ------------------------------------- |
| `wifi-scanner`      | `WIFI_MODE_STA` (sem conectar, apenas scan) |
| `wifi-sniffer`      | `WIFI_MODE_NULL` + promíscuo (todos os frames) |
| `wifi-probe-logger` | `WIFI_MODE_NULL` + promíscuo (apenas mgmt) |
| `wifi-eap-watcher`  | `WIFI_MODE_NULL` + promíscuo (apenas dados) |
| `spectrum-analyzer` | `WIFI_MODE_NULL` + promíscuo (qualquer, apenas RSSI) |
| `espnow-bridge`     | `WIFI_MODE_STA` + `WIFI_PROTOCOL_LR` para alcance |

### 5.2 Bluetooth LE

Os módulos BLE inclusos usam NimBLE (menor que Bluedroid):

```
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_BLUEDROID_ENABLED=n
CONFIG_BT_CONTROLLER_ENABLED=y
```

O host NimBLE executa em sua própria task. O pareamento em `ble-hid-keyboard` é
Just-Works (`sm_io_cap = NO_IO`) com bonding armazenado na NVS.

Capacidades verificadas ao vivo:

- Scan passivo (janela/intervalo 100 ms contínuo): capturou 85 frames
  de advertising em 8 s, 6 MACs únicos em nossa janela de teste.
- HID-over-GATT: anuncia `TW32-Keyboard` / aparência 0x03C1
  (Keyboard) no endereço BLE público `dc:b4:d9:13:aa:36`. Confirmado
  visível via `bluetoothctl scan le` a -42 a -50 dBm em distância
  de mesa.
- Extensões de advertising BLE, PHY de 2 Mbps, Coded PHY: suportados pelo hardware,
  não exercitados pelo v1.

### 5.3 ESP-NOW

- Peer de broadcast pré-registrado, peers adicionais adicionados sob demanda.
- Criptografia AES-CCM suportada via `esp_now_set_pmk()` se uma chave
  de 16 bytes for fornecida na NVS (`espnow-bridge` lê `espnow/aes_key`).
- Modo LR (`WIFI_PROTOCOL_LR`) habilitado no `espnow-bridge` para
  alcance estendido.
- Payload máximo: 250 bytes (`ESP_NOW_MAX_DATA_LEN`).
- Caminho TX validado: 6 frames de broadcast, `tx_ok=6, tx_fail=0`. Validação
  de RX requer um segundo dispositivo.

---

## 6. Topologia USB

Esta é a coisa mais mal compreendida sobre esta placa específica, então
tem sua própria seção.

### 6.1 O caminho que existe (o único que usamos)

```
PC USB-C  ──►  CH343 (WCH USB-UART)  ──► UART0 do S3 (TX=GPIO43, RX=GPIO44)
                                                    │
                                                    └── RTS/DTR → EN/IO0 para auto-reset
```

- O CH343 se enumera no host como `/dev/ttyACM0` com VID `0x1A86`,
  PID `0x55D3`, descritor "USB Single Serial".
- O auto-reset / entrada em modo de boot são conectados via DTR (→ EN) e RTS
  (→ GPIO0); o esptool os aciona automaticamente.
- Este é **o** caminho usado para: gravação, I/O de protocolo de linha para todos
  os módulos v1, captura de log.
- Velocidade: 115200 8N1 por padrão em nosso firmware. UART0 suporta velocidades
  muito maiores (esptool grava a 460800, às vezes 921600; prints do bootloader
  saem a 115200).

### 6.2 O caminho que NÃO existe nesta placa

O ESP32-S3 possui um controlador USB 1.1 OTG integrado nos **GPIO19/20**.
Na ESP32-S3-DevKitC-1 oficial existem *duas* portas USB-C
chamadas "UART" e "USB" — a segunda é o USB nativo. **Esta
placa possui apenas a porta UART.** GPIO19 e GPIO20 podem estar expostos em
pinos de header, mas não estão conectados a um conector USB.

Consequências:

- `tinyusb_driver_install` terá sucesso e o chip dirá
  `tw32-cdc: USB-CDC ready`, mas **nenhum host jamais enxerga o dispositivo** —
  as linhas de dados não estão conectadas. É exatamente esta falha que encontramos ao
  tentar usar tinyusb-CDC para o protocolo de linha; migramos
  para UART0 para tudo.
- Depuração USB JTAG através do USB nativo não é possível sem
  modificar a placa.
- Funcionalidade de host USB (armazenamento em massa, entrada HID, áudio) não é
  possível sem modificar a placa.

Se você algum dia quiser usar o USB nativo neste hardware, precisará
soldar fios aos pads GPIO19/20 + conector USB + resistores
de terminação. Não é um problema do TheWave32 v1.

### 6.3 Reset / entrada em modo de boot

O `esptool` entra em modo de download:
1. RTS baixo + DTR baixo (chip em reset, GPIO0 flutuando)
2. RTS alto (libera reset; GPIO0 ainda mantido pelo sequenciamento de DTR)
3. DTR pulsa para puxar GPIO0 baixo brevemente enquanto EN sobe
4. Resultado: chip inicia em modo de download UART

Se seu auto-reset não estiver funcionando (esptool diz "no ESP32-S3 device
found"), verifique se:
- O CH343 é reconhecido (`/dev/ttyACM0` existe)
- Nenhum outro processo está segurando a porta (`fuser /dev/ttyACM0`)
- O RTS/DTR da placa estão conectados (alguns clones os deixam flutuando;
  você então precisa segurar BOOT e pulsar RESET manualmente)

---

## 7. Pinagem (padrão do SoC)

Esta placa é uma carrier genérica — nenhuma pinagem específica de nível de placa
está documentada como parte deste repositório, portanto a referência abaixo são as
atribuições de pino do próprio SoC. Se sua placa expõe um pino, ele quase
certamente mapeia para o pino do SoC de mesmo número.

### 7.1 Pinos de strapping (travados no reset)

| GPIO | Função no reset                     | Valor padrão | Notas |
| ---- | ----------------------------------- | ------------ | ----- |
| 0    | Modo de boot (0 = download UART, 1 = flash SPI) | pull up → SPI | Mantenha baixo no reset para modo download |
| 3    | Seleção de fonte JTAG               | weak high    | JTAG interno vs USB |
| 45   | Seleção de tensão VDD_SPI           | weak low → 3,3 V | Não altere a menos que saiba o que faz |
| 46   | Habilitação de mensagens ROM        | weak low → silencioso | Alto aqui desabilita logs da ROM |

Não acione estes pinos externamente durante o reset a menos que você queira
alterar o comportamento do boot.

### 7.2 Reservados (geralmente não utilizáveis como GPIO)

| Faixa GPIO  | Função                                          |
| ----------- | ------------------------------------------------ |
| 26..32      | Dados + clock + DQS da flash SPI Octal / PSRAM    |
| 33..37      | (Depende da variante) reservado em variantes OPI |
| 19, 20      | USB D-/D+ quando o periférico USB está ativo      |
| 43, 44      | UART0 TX/RX (conectado ao CH343 nesta placa)      |
| 5..10 (alguns) | SPI PSRAM no-pacote em certas variantes N (não S3-WROOM-1) |

O módulo S3 nesta placa parece ser S3-WROOM-1 com PSRAM
OPI embarcada, portanto GPIOs 26..37 são proibidos.

### 7.3 GPIOs geralmente disponíveis

GPIOs 1–18, 21, 38–42, 45 (com cautela), 46 (com cautela), 47, 48
são tipicamente utilizáveis com total liberdade de atribuição do IO Mux (qualquer UART /
SPI / I2C / I2S / LEDC / RMT pode ser roteado para qualquer um deles via matriz
de GPIO). O S3 removeu a restrição de somente entrada que o ESP32 clássico
tinha nos GPIO 34–39 — todo GPIO no S3 é bidirecional.

### 7.4 Mapeamento ADC (para completude)

- **ADC1:** GPIO 1..10 (10 canais)
- **ADC2:** GPIO 11..20 (10 canais) — compartilha com Wi-Fi; ler
  ADC2 enquanto o Wi-Fi estiver ativo retornará ESP_ERR_TIMEOUT.

Se você construir um módulo de sensor analógico, prefira ADC1 a menos que o Wi-Fi esteja desligado.

### 7.5 Pinos de Toque

Sensores de toque estão nos GPIO 1..14 (subconjunto dos pinos ADC1). Até 14
canais de toque.

---

## 8. Clocks

```
cpu freq:          240 000 000 Hz
crystal freq:      40 000 000 Hz
FreeRTOS tick:     1000 Hz (CONFIG_FREERTOS_HZ=1000)
```

- O cristal de 40 MHz é a frequência de entrada canônica do S3. O PLL multiplica
  para 240 MHz para a CPU e 480 MHz para alguns periféricos.
- O clock lento RTC padrão é 150 kHz RC interno. Se você precisar de
  precisão no temporizador de deep sleep, um cristal externo de 32,768 kHz pode
  ser conectado aos GPIO 0/1; esta placa não possui um.

---

## 9. Temporização de boot

Medido de POWERON até `main_task: Calling app_main()` (timestamps
em milissegundos, do log de boot):

| Estágio                                    | Tempo     | Notas                           |
| ------------------------------------------ | --------- | ------------------------------- |
| Bootloader ROM                             | 0–~30 ms  | Antes das linhas `I (...)`      |
| Init + tuning do `octal_psram`             | 163 ms    | Primeiro log IDF                 |
| `cpu_start: Multicore app`                 | 212 ms    |                                 |
| `esp_psram: SPI SRAM memory test OK`       | 622 ms    | Salto grande — memtest completo da PSRAM |
| `app_init: Project name`                   | 631 ms    |                                 |
| Regiões `heap_init`                        | 670–686 ms |                               |
| `main_task: Started on CPU0`               | 717 ms    |                                 |
| `main_task: Calling app_main()`            | 741 ms    | Código da aplicação começa       |
| Task do driver Wi-Fi pronta                | 945 ms    | Disponível para esp_wifi_*       |
| `wifi:mode : sta (...)`                    | 1078 ms   | Wi-Fi STA totalmente operacional |

Portanto, **app_main executa ~750 ms após a alimentação**, e o Wi-Fi está estável
~1,1 s após. Planeje seu evento "pronto" de acordo.

---

## 10. Matriz de compatibilidade esptool / ESP-IDF

### 10.1 esptool

Esta placa é sensível a **esptool ≥ v5**. Mudanças notáveis:

- Subcomandos são **hifenizados**: `chip-id`, `write-flash`,
  `default-reset`, `hard-reset`, `--flash-size`. As formas com underscore
  do v4 ainda funcionam, mas emitem avisos de depreciação.
- A linha `Chip is ESP32-S3 (revision X)` na saída do v4 agora é
  `Chip type: ESP32-S3 (QFN56) (revision vX.Y)` — regex atualizada em
  `src/thewave32/flasher.py:48`.

### 10.2 Versão do ESP-IDF

Temos como alvo a **v5.4.1** em `~/ESP32S3`. Testada na placa com esta versão.

Coisas que mudaram entre versões principais do IDF e que importam para
autores de módulo:

- **esp_now é parte do esp_wifi** desde v5.x (era um componente
  separado no v4). Não o liste em `REQUIRES`.
- **esp_tinyusb foi movido para o Gerenciador de Componentes do IDF** na v5.0+. Use
  `idf_component.yml` com `espressif/esp_tinyusb: ^1.4.5`.
- **O fonte do NimBLE tem referências simbólicas entre funções** que não são
  protegidas por flags de função Kconfig no v5.4.x. Não desabilite
  `BT_NIMBLE_ROLE_*` individuais para "economizar flash"; a compilação quebra. Mantenha
  os padrões ativados.

### 10.3 Flasher Python

Requer um venv Python com `pyserial`, `esptool` e o pacote
gerador de NVS do IDF:

```bash
python3 -m venv .venv
.venv/bin/pip install -e '.[dev]'
.venv/bin/pip install esp-idf-nvs-partition-gen
```

O `esp-idf-nvs-partition-gen` é necessário porque o `nvs_partition_gen.py`
do IDF v5 agora é um wrapper fino que importa o pacote do PyPI.

---

## 11. Log de boot anotado (real, 2026-05-06)

Capturado ao vivo pulsando manualmente o RTS no firmware atual
`wifi-probe-logger v0.2.0`:

```
ESP-ROM:esp32s3-20210327                        ← Identidade da ROM (data é compilação da ROM)
Build:Mar 27 2021                               ← Data de compilação da ROM
rst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT) ← causa do reset + modo de boot
SPIWP:0xee                                      ← valor do efuse, padrão normal
mode:DIO, clock div:1                           ← velocidade/modo da flash (DIO @ 80 MHz)
load:0x3fce2820,len:0x1120                      ← segmentos do bootloader
load:0x403c8700,len:0x4
load:0x403c8704,len:0xa84
load:0x403cb700,len:0x2d1c
entry 0x403c8890                                ← entrada do bootloader
{"event":"ready","module":"wifi-probe-logger","version":"0.2.0"}
                                                ← Primeira linha do nosso app:
                                                  CONFIG_ESP_CONSOLE_NONE=y suprime
                                                  logs do ESP-IDF acima disto; apenas
                                                  as escritas UART da aplicação sobrevivem.
```

Se você quiser que os logs ricos de heap_init / wifi / phy_init apareçam,
altere temporariamente `CONFIG_LOG_DEFAULT_LEVEL_INFO=y` e
`CONFIG_ESP_CONSOLE_UART_DEFAULT=y`. Esteja ciente de que seu stream JSON
se misturará com texto.

---

## 12. Números de desempenho medidos nesta unidade exata

Estes vêm das execuções de validação de hardware dos módulos v1 (veja mensagens
de commit para as capturas exatas):

| Métrica                             | Valor           | Módulo / contexto                              |
| ----------------------------------- | --------------- | ---------------------------------------------- |
| Scan ativo Wi-Fi, 13 canais         | ~1,5 s          | `wifi-scanner`, dwell min/máx 80/120 ms         |
| APs visíveis em sala urbana típica  | 5..8 por scan   | `wifi-scanner`                                 |
| Advs BLE capturados                 | ~10/s           | `ble-scanner`, scan contínuo de 100 ms          |
| Taxa de frames promíscuos (canal 6) | ~25/s           | `wifi-sniffer`, 0 perdas com fila de 64         |
| Orçamento de dwell do sweep RSSI    | 30 ms × 13 can  | `spectrum-analyzer`, ~2,7 sweeps/s             |
| Taxa de probe-request               | ~1/s por dispositivo | `wifi-probe-logger`, ambiente urbano       |
| Velocidade de gravação flash        | ~750 kbit/s     | Gravação esptool baud padrão (115200)           |
| Boot até evento JSON `ready`        | < 1 s           | Todos os módulos                                |
| Heap livre no início de app_main    | ~290 KiB int + 8 MB PSRAM | pós-init de Wi-Fi/BLE              |

Estas são observações brutas, não benchmarks — úteis como números
aproximados, não como garantias.

---

## 13. O que esta placa NÃO tem

Coisas que você pode desejar, classificadas por frequência com que precisamos delas:

- **Rádio sub-GHz** (CC1101, RFM69, etc.) → sem portão de garagem / estação
  meteorológica / controles remotos 433 MHz.
- **NRF24** (multiprotocolo 2,4 GHz) → sem Mousejack / Crazyflie / RC.
- **Leitor NFC/RFID** (PN532, RC522, RDM6300) → sem clone/emulação de tag.
- **LED IV + receptor** → sem replay de controle remoto de TV / AC.
- **Módulo GPS** → sem wardriving com localização.
- **Display** (OLED / TFT / e-ink) → apenas headless.
- **Botões** além de BOOT e EN.
- **Cristal externo de 32,768 kHz** → precisão RTC em deep sleep é de nível RC.
- **Slot MicroSD** → sem armazenamento local grande.
- **Microfone** / **alto-falante** / **codec** → sem áudio.
- **Bateria + circuito de carga** → alimentado apenas por USB.
- **Conector USB nativo** → veja §6.

Cada um destes, se adicionado, abre uma nova categoria de módulo. O conjunto
de firmware v1 assume que nenhum deles está presente.

---

## 14. Fontes

Toda afirmação neste documento é fundamentada em uma das seguintes fontes.
Nenhuma vem de um datasheet citado de memória.

| Seção | Fonte                                                           |
| ----- | --------------------------------------------------------------- |
| §0 Tabela Resumo | `python -m esptool chip-id` nesta unidade, mais logs de boot |
| §1 Identificação | `serial.tools.list_ports`, esptool `chip-id`, banner ROM    |
| §2 SoC geral     | README + Kconfig dos componentes ESP-IDF v5.4 + TRM da Espressif |
| §3 Mapa de memória | Log de boot `heap_init` nesta unidade (capturado 2026-05-05)  |
| §3.1 PSRAM       | Log de boot do driver `octal_psram` nesta unidade               |
| §4 Flash         | Log de boot `spi_flash` + `--flash-size detect` do esptool      |
| §5 Wi-Fi/BLE     | Logs de boot `pp` / `net80211` / `wifi` / `phy_init`           |
| §6 USB           | `lsusb` (onde disponível) + print `tusb_desc` do tinyusb + falha |
|                   | observada de enumeração do USB nativo; verificado pela árvore USB no host |
| §7 Pinagem       | Datasheet da série ESP32-S3 + docs da API GPIO do ESP-IDF      |
| §8 Clocks        | Logs de boot `cpu_start` + `app_init`, mais `CONFIG_FREERTOS_HZ` |
| §9 Temporização de boot | Timestamps do log de boot                              |
| §10 Ferramentas  | Experiência prática com esptool 5.2.0 + ESP-IDF v5.4.1         |
| §11 Log de boot  | Captura ao vivo 2026-05-06                                      |
| §12 Desempenho   | Execuções de validação de hardware dos módulos TheWave32        |

Se você substituir esta placa por um módulo ESP32-S3 diferente, recapture
§0, §3, §3.1, §4, §6, §11 — estas são as partes que variam por unidade
ou por variante de placa. Todo o resto é nível SoC e estável entre
variantes.
