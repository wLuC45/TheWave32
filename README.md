<p align="center">
  <img src="assets/TheWave32.svg" alt="TheWave32" width="380">
</p>

<h1 align="center">TheWave32</h1>

<p align="center">
  Pesquisa de Wi-Fi, Bluetooth e rádio 2.4 GHz na ESP32-S3.
  <br>
  Escolha um módulo de firmware, grave e opere pela linha de comando ou pela interface gráfica.
</p>

---

## Sobre

O TheWave32 tem duas partes no mesmo repositório.

A primeira é um gravador escrito em Python, disponível como linha de comando
(`thewave32`) e como aplicativo gráfico (`thewave32-gui`). Ele lê uma pasta de
módulos de firmware já compilados, grava o módulo escolhido na ESP32-S3 pela
USB e, na mesma passada, monta as imagens NVS e SPIFFS com os dados que você
fornece (credenciais de Wi-Fi, payloads DuckyScript e afins). Depois da
gravação, a interface conduz o firmware: envia comandos, interpreta o protocolo
de linhas JSON sobre USB-CDC, separa os fluxos binários e mostra tudo num
visualizador próprio de cada módulo.

A segunda é a biblioteca de firmware em `firmware/`. São módulos ESP-IDF de
propósito único cobrindo Wi-Fi, BLE, USB-HID e rádio 2.4 GHz. Cada um é
compilado para os artefatos em `modules/<nome>/` que o gravador consome.

> Uso autorizado: o TheWave32 é destinado a pesquisa de segurança, ensino e
> testes defensivos. Use apenas em redes e dispositivos que você possui ou tem
> permissão por escrito para avaliar.

## Requisitos

- Python 3.11 ou superior
- ESP-IDF 5.x, necessário apenas para compilar firmware a partir do código
  (caminho padrão `/home/v0rtex/ESP32S3`, sobrescrito por `$IDF_PATH`)
- Uma placa ESP32-S3 com USB nativo
- No Linux, usuário no grupo `dialout` (Debian, Ubuntu) ou `uucp` (Arch)

## Instalação

```bash
pipx install -e .
# ou
uv tool install .
```

A instalação registra os dois comandos: `thewave32` (linha de comando) e
`thewave32-gui` (interface gráfica). As dependências de execução, incluindo a
camada gráfica, são resolvidas automaticamente.

## Uso

### Interface gráfica

```bash
thewave32-gui
```

Janela única: a barra lateral lista os módulos, a gravação é feita com um
clique e o firmware em execução é operado por um visualizador ao vivo
específico do módulo.

### Linha de comando

```bash
thewave32                         # interface de texto (padrão)
thewave32 list                    # lista os módulos disponíveis
thewave32 info wifi-scanner       # mostra o manifesto de um módulo
thewave32 flash wifi-scanner
thewave32 flash ble-hid-keyboard --input payload=/caminho/do/script.txt
```

### Privacidade

Por padrão os eventos do dispositivo são registrados como vêm. Para
pseudonimizar MACs e redigir material sensível (PMKID, handshake, chaves)
antes de gravar ou exibir, defina `THEWAVE32_PRIVACY`:

```bash
THEWAVE32_PRIVACY=macs      # MACs viram pseudônimos estáveis por sessão
THEWAVE32_PRIVACY=secrets   # campos sensíveis viram "[redacted]"
THEWAVE32_PRIVACY=all       # os dois
```

## Módulos

São 24 módulos de propósito único. Todos são compilados para
`modules/<nome>/` e falam o mesmo protocolo de linhas JSON (ou um fluxo
binário PCAP/CSI) sobre USB-CDC.

### Wi-Fi, ofensivo

| Módulo | Descrição |
| --- | --- |
| `wifi-scanner` | Scanner passivo de APs com salto de canal |
| `wifi-sniffer` | Captura promíscua 802.11, fluxo PCAP |
| `wifi-deauth` | Deautenticação multi-alvo (até 16 BSSIDs) |
| `wifi-beacon-spam` | Inundação de beacons com lista de SSIDs arbitrária |
| `wifi-bssid-clone` | Clona SSID e BSSID do alvo no mesmo canal |
| `wifi-evil-twin` | AP rogue aberto clonando um SSID alvo |
| `wifi-ghost` | Estilo KARMA: aprende SSIDs de probe requests e os anuncia |
| `wifi-handshake-capture` | Detecta o handshake WPA2 de 4 vias |
| `wifi-pmkid-capture` | Extrai o PMKID do EAPOL-Key M1, sem cliente associado |

### Wi-Fi, passivo e defensivo

| Módulo | Descrição |
| --- | --- |
| `wifi-probe-logger` | Registra probe requests: MAC de origem, SSID e RSSI |
| `wifi-eap-watcher` | Observa EAP-Identity 802.1X (auditoria de exposição de IMSI) |
| `wifi-mac-tracker` | Rastreia a presença de MACs de uma watchlist |
| `wifi-csi-collector` | Transmite Channel State Information em registros binários |
| `wifi-deauth-detector` | Alerta sobre floods de deauth e disassoc |
| `wifi-evil-twin-detector` | Whitelist de (SSID, BSSID); alerta sobre clones rogue |
| `wifi-clock-skew` | Fingerprint de clock por AP via TSF dos beacons; sinaliza um BSSID acionado por dois transmissores ([doc](docs/wifi-clock-skew.md)) |

### Bluetooth LE

| Módulo | Descrição |
| --- | --- |
| `ble-scanner` | Scanner passivo de advertising BLE |
| `ble-spam` | Spam de advertising BLE (popups de proximidade Apple, Samsung, Google) |
| `ble-airtag-finder` | Observador de beacons Apple Find My |
| `ble-hid-keyboard` | Teclado HID por BLE com interpretador de DuckyScript |

### USB, rádio e rede

| Módulo | Descrição |
| --- | --- |
| `usb-hid-keyboard` | Teclado HID pela USB-OTG nativa da S3 |
| `espnow-bridge` | Ponte ESP-NOW para UART, com AES-CCM opcional |
| `spectrum-analyzer` | Heatmap de RSSI em 2.4 GHz, canais 1 a 13 |
| `net-port-scanner` | Varredura de portas TCP numa rede Wi-Fi à qual a placa se associa |

## Estrutura do repositório

```
TheWave32/
├── assets/              # logo e mídia estática
├── src/thewave32/       # gravador em Python (CLI, TUI e GUI)
├── tests/               # suíte pytest (testes de hardware são opcionais)
├── firmware/            # código-fonte ESP-IDF de cada módulo
├── modules/             # artefatos compilados que o gravador consome
├── scripts/             # scripts auxiliares de build
└── docs/                # perfil de hardware e guia de autoria de módulos
```

## Desenvolvimento

```bash
pip install -e .[dev]
pytest                    # os testes de hardware ficam de fora por padrão
```

Os testes marcados como `hardware` exigem uma ESP32-S3 conectada e só rodam com
`pytest --hardware`.

## Compilar firmware

A interface gráfica recompila os módulos com código-fonte mais novo que o
artefato ao iniciar. Para compilar um módulo manualmente:

```bash
source /home/v0rtex/ESP32S3/export.sh
cd firmware/wifi-scanner
idf.py build
```

## Documentação

- [`docs/new_module.md`](docs/new_module.md): guia para escrever um módulo novo
- [`docs/hardware.md`](docs/hardware.md): perfil da placa de desenvolvimento
