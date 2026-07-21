# Criando um Módulo para o TheWave32

Este documento é a referência completa para adicionar um módulo ao TheWave32.
Ao final dele, você saberá como:

- **Adicionar um firmware pré-compilado** (você tem `bootloader.bin`,
  `partition-table.bin` e um `firmware.bin` de alguma fonte) e
  fazer com que o framework o reconheça, **ou**
- **Escrever um módulo do zero** em C/C++ sobre o ESP-IDF v5.x usando
  a biblioteca compartilhada `tw32_common`, compilá-lo, copiar os artefatos
  para o lugar certo e fazê-lo aparecer na TUI / CLI junto com os
  módulos inclusos.

Ele também cataloga todas as armadilhas concretas que já afetaram os
módulos inclusos durante o desenvolvimento, para que você não as repita.

> Convenções usadas abaixo
> - `<slug>` — o nome do seu módulo em kebab-case, ex.: `wifi-scanner`.
>   Este é o nome do diretório e o valor de `[module].name` no
>   manifesto.
> - `<REPO_ROOT>` é a raiz do repositório. Ajuste conforme seu
>   ambiente.
> - Todos os trechos de shell assumem bash + um ESP-IDF sourceado (`source
>   $IDF_PATH/export.sh` uma vez por sessão de shell).

---

## 1. O que é um "módulo"?

Um módulo é uma imagem única de firmware ESP-IDF mais um pequeno manifesto TOML
que diz ao gravador Python (`thewave32`) **onde** colocar cada binário
na flash, **quais entradas** o firmware espera (credenciais Wi-Fi, payloads,
chaves, etc.) e **como** identificar o módulo na interface.

A parte do Python nunca compila seu firmware. Ela apenas:

1. Lê `modules/<slug>/module.toml`,
2. Opcionalmente constrói uma imagem de partição NVS / SPIFFS a partir dos
   valores `--input` do usuário,
3. Chama `esptool` para gravar `bootloader.bin`, `partition-table.bin`,
   `firmware.bin` e qualquer imagem de partição gerada nos offsets que o
   manifesto declarar.

Essa separação significa que **um distribuidor de módulo pode enviar apenas os quatro
arquivos** (`module.toml` + três `.bin`) e um usuário sem ESP-IDF
instalado pode gravá-los. A árvore de origem `firmware/` existe apenas para
quem deseja *compilar* o firmware.

---

## 2. Estrutura do repositório

```
TheWave32/
├── pyproject.toml                # Gravador Python (você não mexe nisto)
├── src/thewave32/                # CLI / TUI / gravação / construtor NVS
├── tests/                        # Suíte pytest para o gravador
├── firmware/                     # Árvores de fonte ESP-IDF, uma por módulo
│   ├── tw32_common/              # Biblioteca C compartilhada (use-a; não fork)
│   ├── wifi-scanner/             # Módulo de referência — use como modelo
│   ├── wifi-sniffer/
│   ├── ble-scanner/
│   ├── ble-hid-keyboard/
│   ├── espnow-bridge/
│   ├── spectrum-analyzer/
│   ├── wifi-probe-logger/
│   └── wifi-eap-watcher/
├── modules/                      # Artefatos compilados que o gravador consome
│   └── <slug>/
│       ├── module.toml
│       ├── bootloader.bin
│       ├── partition-table.bin
│       └── firmware.bin
├── scripts/
│   └── build.sh <slug>           # idf.py build → copia para modules/<slug>/
└── docs/
    └── new_module.md             # este arquivo
```

Os dois diretórios que importam para autores de módulos:

- `firmware/<slug>/` — seu código-fonte.
- `modules/<slug>/` — seu manifesto e os artefatos compilados.

Os diretórios `firmware/<slug>/build/` e `firmware/<slug>/managed_components/`
são artefatos gerados e ignorados pelo git.

---

## 3. O caminho rápido: adicionando um firmware pré-compilado

Use este caminho quando:

- Você compilou o firmware com `idf.py` em outro lugar,
- Você tem um `.bin` de terceiros (ex.: uma release comunitária do Marauder) que
  deseja que o framework grave,
- Ou você cross-compilou em um host diferente e quer apenas publicar a
  saída.

### 3.1 Arquivos necessários

Crie `modules/<slug>/` com **pelo menos** estes quatro arquivos:

```
modules/<slug>/
├── module.toml
├── bootloader.bin
├── partition-table.bin
└── firmware.bin
```

Isso é suficiente para que `thewave32 list`, `thewave32 info <slug>` e
`thewave32 flash <slug>` funcionem, e para que a TUI exiba o módulo.

### 3.2 O manifesto mínimo

```toml
[module]
name        = "my-cool-module"
version     = "0.1.0"
description = "Resumo de uma linha; mostrado na lista da TUI"
target      = "esp32s3"
author      = "your-handle"
source_url  = "https://example.com/repo"  # opcional

[flash]
artifacts = [
  { path = "bootloader.bin",      offset = "0x0000"  },
  { path = "partition-table.bin", offset = "0x8000"  },
  { path = "firmware.bin",        offset = "0x10000" },
]
```

### 3.3 Offsets padrão do ESP32-S3 (use estes a menos que saiba o que está fazendo)

| Artefato             | Offset   | Notas                                        |
| -------------------- | -------- | -------------------------------------------- |
| `bootloader.bin`     | `0x0000` | ESP32-S3 inicializa aqui                     |
| `partition-table.bin`| `0x8000` | `CONFIG_PARTITION_TABLE_OFFSET` padrão       |
| App `firmware.bin`   | `0x10000`| Offset padrão do `factory` app               |

Se seu partitions.csv tiver um app `factory` em um offset não padrão
(ex.: `ble-hid-keyboard` usa `0x20000` porque seu NVS é maior), defina
o offset de `firmware.bin` de acordo. O manifesto deve concordar com os
offsets embutidos no partition-table.bin que você está distribuindo; o
gravador **não** analisa a tabela de partições para você.

### 3.4 Verifique se foi carregado

```bash
cd <REPO_ROOT>
source .venv/bin/activate
thewave32 list                   # seu slug deve aparecer
thewave32 info my-cool-module    # exibe o manifesto interpretado
thewave32 flash my-cool-module   # grava os .bin em /dev/ttyACM0
```

Isso é tudo para o caminho de adição rápida. Avance para §10 (Validação) e §11
(Checklist final).

---

## 4. O caminho completo: compilando um módulo a partir do código-fonte

Use este caminho quando quiser escrever o firmware você mesmo ou modificar um
existente.

### 4.1 Pré-requisitos (uma vez)

```bash
# Instale o ESP-IDF v5.x (versão 5.4 é a que os módulos acompanham).
source $IDF_PATH/export.sh

# Ative o venv Python que tem as ferramentas do gravador.
cd <REPO_ROOT>
source .venv/bin/activate
```

Ambos são necessários **no mesmo shell** para que `./scripts/build.sh` funcione.

### 4.2 Esqueleto — copie da referência mais simples

A referência mais simples é `wifi-scanner`. Cinco arquivos:

```
firmware/<slug>/
├── CMakeLists.txt          # nível do projeto
├── sdkconfig.defaults      # informa ao idf.py quais valores Kconfig usar
├── partitions.csv          # layout de partição personalizado (opcional, mas comum)
└── main/
    ├── CMakeLists.txt      # nível do componente
    └── <slug>.c            # o código propriamente dito; app_main() vive aqui
```

### 4.3 `CMakeLists.txt` (nível do projeto)

```cmake
cmake_minimum_required(VERSION 3.16)

# Inclui o componente compartilhado firmware/tw32_common/. O caminho é
# relativo a este CMakeLists.txt (então irmãos de firmware/ também funcionam).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../tw32_common")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(<slug_with_underscores>)         # ex.: wifi_scanner, NÃO wifi-scanner
```

Duas coisas para saber:

- Projetos ESP-IDF usam **underscores** em `project()` porque esse nome
  vira o nome do arquivo .elf/.bin, e CMake não gosta de hífens. Use
  underscores. O `<slug>` em si (manifesto, diretório, nome do módulo) é
  hifenizado — é uma escolha de interface.
- `EXTRA_COMPONENT_DIRS` deve apontar para `firmware/tw32_common/` (um
  diretório acima + irmão). A compilação falhará com "Failed to resolve
  component 'tw32_common'" se você esquecer isto.

### 4.4 `main/CMakeLists.txt`

```cmake
idf_component_register(
  SRCS "<slug_with_underscores>.c"
  INCLUDE_DIRS "."
  REQUIRES tw32_common esp_wifi nvs_flash esp_event freertos log
  # Adições comuns, dependendo do que você faz:
  #   esp_timer        — necessário se você chamar esp_timer_get_time()
  #   bt               — para qualquer uso de NimBLE / Bluedroid
  #   esp_driver_uart  — apenas se você assumir o controle da UART; tw32_io
  #                      já puxa isto via tw32_common
)
```

Checklist de armadilhas para `REQUIRES`:

- **`esp_now` não existe** como componente separado no IDF v5.x. Ele
  faz parte de `esp_wifi`. Listá-lo dispara um erro de configuração
  "could not be found".
- **`esp_tinyusb` foi movido para fora da árvore do IDF** para o Gerenciador
  de Componentes do IDF (`espressif/esp_tinyusb`). Se você realmente precisar,
  adicione um `idf_component.yml` ao lado de seu `main/CMakeLists.txt`. Mas neste
  hardware o USB nativo não é exposto externamente — veja §8 — então você
  quase certamente não vai querer usá-lo.
- **`bt`** é o nome correto para o pacote do controlador Bluetooth + host NimBLE.
  Não `nimble`, não `esp_nimble`. Apenas `bt`.

### 4.5 `partitions.csv` (tabela de partições personalizada)

A maioria dos módulos usa este layout:

```
# Name,   Type, SubType, Offset,   Size
nvs,      data, nvs,     0x9000,   0x6000
phy_init, data, phy,     0xf000,   0x1000
factory,  app,  factory, 0x10000,  0x1F0000
```

Se você precisar de um NVS maior (ex.: para payloads DuckyScript), aumente o
tamanho de `nvs` e empurre o offset `factory` para frente:

```
nvs,      data, nvs,     0x9000,   0xC000      # 48 KB em vez de 24 KB
phy_init, data, phy,     0x15000,  0x1000
factory,  app,  factory, 0x20000,  0x1E0000
```

**Sempre que você alterar o offset factory aqui, também altere o
offset de `firmware.bin` em `module.toml`** para coincidir. O gravador confia
no manifesto, não no CSV.

Se você precisar de uma partição SPIFFS para arquivos (ex.: arquivos de payload em vez de
strings NVS):

```
storage,  data, spiffs,  0x110000, 0xE0000     # ajuste conforme necessário
```

Em seguida, declare-a em `[partitions]` no manifesto e adicione `[[inputs]]`
com `target = "spiffs"` (veja §5).

### 4.6 `sdkconfig.defaults`

Este é seu Kconfig padrão — valores que você deseja em tempo de compilação, sem necessidade
de GUI. O modelo incluso é:

```
# Chip alvo
CONFIG_IDF_TARGET="esp32s3"

# CPU + RTOS
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_FREERTOS_HZ=1000

# Console do IDF desabilitado — tw32_io é dona da UART0 para o protocolo de linhas JSON.
# O bootloader ainda imprime mensagens iniciais na UART0 diretamente via ROM.
CONFIG_ESP_CONSOLE_NONE=y

# ESP_LOG mantido em ERROR para que panics ainda imprimam, mas INFO não polua
# o fluxo JSON. Se quiser depurar temporariamente, defina
# CONFIG_LOG_DEFAULT_LEVEL_INFO e aceite linhas misturadas.
CONFIG_LOG_DEFAULT_LEVEL_ERROR=y

# PSRAM (8 MB na placa v0rtex; seguro deixar ativado mesmo se seu hardware não tiver)
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_USE_MALLOC=y

# Bootloader mais rápido.
CONFIG_BOOTLOADER_LOG_LEVEL_WARN=y

# Diz ao IDF para usar nosso partitions.csv personalizado.
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"

# Compilador.
CONFIG_COMPILER_OPTIMIZATION_PERF=y
```

Variações por categoria de módulo:

- **Módulos Wi-Fi** que operam em modo promíscuo: aperte as contagens de buffer lwIP
  (ex.: `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=4`). Veja `wifi-sniffer`.
- **Módulos BLE**: ative o controlador + host NimBLE:
  ```
  CONFIG_BT_ENABLED=y
  CONFIG_BT_NIMBLE_ENABLED=y
  CONFIG_BT_BLUEDROID_ENABLED=n
  CONFIG_BT_CONTROLLER_ENABLED=y
  CONFIG_BT_NIMBLE_NVS_PERSIST=n          # `=y` se você quiser pareamento
  CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=4096
  ```
  **Não desabilite papéis individuais do NimBLE**
  (`CONFIG_BT_NIMBLE_ROLE_*=n`) — a árvore de fontes do NimBLE no IDF v5.4.x tem
  referências de símbolos entre papéis que não são protegidas por `#ifdef`, então a
  compilação quebra. Deixe os padrões ativados.
- **Teclado HID**: pareamento + bonding:
  ```
  CONFIG_BT_NIMBLE_NVS_PERSIST=y
  CONFIG_BT_NIMBLE_SM_LEGACY=y
  CONFIG_BT_NIMBLE_SM_SC=y
  ```

`sdkconfig` (sem `.defaults`) é **gerado** e ignorado pelo git. Edite
`sdkconfig.defaults`, nunca `sdkconfig`.

### 4.7 O esqueleto da aplicação (`main/<slug>.c`)

Cada módulo incluso segue a mesma estrutura. Preencha as partes marcadas
com `/* … */`:

```c
/*
 * TheWave32 / <slug>
 *
 * <um parágrafo descrevendo o que ele faz>
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tw32_cdc_cli.h"      // Dispatcher CLI + helpers de ack
#include "tw32_io.h"           // Inicialização UART0 + leitura/escrita de linhas
#include "tw32_json_out.h"     // Emissor de linhas JSON em streaming
#include "tw32_nvs_kv.h"       // Getters NVS tipados

#define MODULE_NAME    "<slug>"
#define MODULE_VERSION "0.1.0"

/* --- estado em execução ------------------------------------------------- */

typedef struct {
    volatile bool running;
    /* … seus campos … */
} state_t;
static state_t s_state = { .running = false };

/* --- tarefas worker ----------------------------------------------------- */

static void worker_task(void *arg)
{
    (void)arg;
    while (true) {
        if (!s_state.running) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        /* … uma iteração do seu trabalho … */
        /* Emite um evento JSON quando algo interessante acontece: */
        tw32_json_begin();
        tw32_json_kv_str("event", "tick");
        tw32_json_kv_uint("ts", (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
        tw32_json_end();
    }
}

/* --- Handlers CLI ------------------------------------------------------- */

static int cmd_start(tw32_cli_ctx_t *c, int a, char **v)
{ (void)c;(void)a;(void)v; s_state.running = true;  tw32_cli_ack_ok("start"); return 0; }

static int cmd_stop(tw32_cli_ctx_t *c, int a, char **v)
{ (void)c;(void)a;(void)v; s_state.running = false; tw32_cli_ack_ok("stop");  return 0; }

static int cmd_stats(tw32_cli_ctx_t *c, int a, char **v)
{
    (void)c; (void)a; (void)v;
    tw32_json_begin();
    tw32_json_kv_str ("cmd", "stats");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_bool("running", s_state.running);
    tw32_json_end();
    return 0;
}

static const tw32_cli_cmd_t cli_table[] = {
    { "start", "iniciar trabalho", cmd_start },
    { "stop",  "parar trabalho",   cmd_stop  },
    { "stats", "exibir estado",   cmd_stats },
};

/* --- ponto de entrada --------------------------------------------------- */

void app_main(void)
{
    tw32_nvs_init();
    tw32_cdc_init();

    /* … outra inicialização: esp_wifi, NimBLE, ESP-NOW, etc. … */

    xTaskCreatePinnedToCore(worker_task, "tw32-worker",
                            4096, NULL, 4, NULL, /* core */ 1);

    tw32_cli_run(MODULE_NAME, MODULE_VERSION,
                 cli_table, sizeof(cli_table)/sizeof(cli_table[0]));
}
```

Isso compila, linka, imprime um banner `{"event":"ready",…}` na UART0,
responde a `version` / `help` / `start` / `stop` / `stats`. Daqui
você adiciona seu loop de trabalho, seu parser IE, seu serviço NimBLE, sua
tabela de pares ESP-NOW, etc.

---

## 5. O manifesto `module.toml` em detalhes

O schema completo (validado por `src/thewave32/manifest.py`):

```toml
# OBRIGATÓRIO. Identifica o módulo.
[module]
name        = "wifi-eap-watcher"            # slug em kebab-case; coincide com o nome do dir
version     = "0.1.0"                       # formato livre, estilo semver
description = "Observador passivo 802.1X EAP-Identity"
target      = "esp32s3"                     # obrigatório para o gravador validar
author      = "v0rtex"                      # opcional
source_url  = "https://github.com/.../tree/main/firmware/wifi-eap-watcher"  # opcional

# OBRIGATÓRIO. Os arquivos .bin a serem gravados.
# `path` é relativo a modules/<slug>/. `offset` aceita strings hex ou ints.
[flash]
artifacts = [
  { path = "bootloader.bin",      offset = "0x0000"  },
  { path = "partition-table.bin", offset = "0x8000"  },
  { path = "firmware.bin",        offset = "0x10000" },
]

# OPCIONAL. Declara partições que o gravador deve preencher a partir de valores --input.
# Apenas partições NVS e SPIFFS precisam ser declaradas aqui; as partições de
# app e bootloader são inferidas de [flash].
[partitions]
nvs    = { offset = "0x9000",   size = "0x6000"   }
storage= { offset = "0x110000", size = "0xE0000"  }   # SPIFFS, label "storage"

# OPCIONAL. Um bloco [[inputs]] por dado que o usuário pode fornecer.
[[inputs]]
key       = "ssid"                          # Chave NVS OU nome base de arquivo SPIFFS
prompt    = "Wi-Fi SSID"                    # Mostrado na caixa de diálogo de entrada da TUI
type      = "string"                        # string | int | bool | choice | file
target    = "nvs"                           # nvs | spiffs
namespace = "wifi"                          # obrigatório quando target=nvs
required  = true                            # padrão: true
default   = "TheWave32"                     # usado se o usuário pular e required=false

[[inputs]]
key      = "tx_power_dbm"
prompt   = "Potência TX (dBm)"
type     = "int"
target   = "nvs"
namespace= "wifi"
default  = 11
required = false

[[inputs]]
key      = "mode"
prompt   = "Modo"
type     = "choice"
target   = "nvs"
namespace= "wifi"
options  = ["sta", "ap", "apsta"]
default  = "sta"
required = false

[[inputs]]
key      = "payload"
prompt   = "Payload DuckyScript"
type     = "string"                         # veja §5.2 — string vs blob
target   = "nvs"
namespace= "ducky"
required = false

[[inputs]]
key      = "ducky_script"                   # Exemplo SPIFFS
prompt   = "Caminho para arquivo DuckyScript (.txt)"
type     = "file"                           # o usuário fornece um caminho no host;
                                            #   o conteúdo vai para o SPIFFS
target   = "spiffs"
dest     = "/payload.txt"                   # caminho absoluto dentro do SPIFFS
required = false
```

### 5.1 Regras de validação do manifesto

Estas são aplicadas pelo pydantic; erros fazem o carregamento falhar com uma mensagem clara.

- Todos os `[flash].artefatos[].offset` e `[partitions].*.offset` devem ser
  **únicos**. O validador do manifesto rejeita sobreposições.
- Cada `[[inputs]].target` deve referenciar uma chave em `[partitions]`
  (então `target="nvs"` exige `[partitions].nvs`, etc.).
- `target="nvs"` exige `namespace`.
- `target="spiffs"` exige `dest`, que deve começar com `/` e não
  conter `..`.
- `type="choice"` exige `options` (lista não vazia).
- Chaves TOML desconhecidas são rejeitadas (model_config = forbid). Ortografia
  é importante.

### 5.2 Codificação NVS — corresponda leituras do firmware aos tipos do manifesto

Este detalhe já nos pegou uma vez e vale uma subseção inteira. O construtor
Python (`src/thewave32/builder.py`) mapeia os tipos de entrada do manifesto para as
codificações NVS usadas por `nvs_partition_gen`:

| `type` no manifesto | Codificação NVS | Ler com                                 |
| ------------------- | --------------- | --------------------------------------- |
| `string`            | `string`        | `tw32_nvs_get_str` / `nvs_get_str`      |
| `int`               | `i32`           | `tw32_nvs_get_i32` / `nvs_get_i32`      |
| `bool`              | `u8`            | `tw32_nvs_get_bool` / `nvs_get_u8`      |
| `choice`            | `string`        | `tw32_nvs_get_str`                      |

Se seu firmware ler um valor com a **API errada** (ex.:
`nvs_get_blob` contra uma entrada gravada como `string`), a leitura falha
e você recebe um valor padrão — silenciosamente. Sintoma: "Eu forneci `--input foo=bar`
mas meu firmware age como se não houvesse nada no NVS." Correção: use tipos correspondentes.

`type="file"` se aplica apenas a SPIFFS; não passa pelo NVS.

### 5.3 Imagens de partição SPIFFS

Se você declarar uma partição SPIFFS e alguma entrada com `target="spiffs"`, o
gravador gera uma imagem SPIFFS com o arquivo de entrada no caminho `dest`
declarado e a grava no offset da partição antes de gravar o
app. A imagem é construída em um diretório temporário; o tamanho da partição em
`[partitions]` é o tamanho da imagem.

Para ler o arquivo do firmware, monte a partição com
`esp_vfs_spiffs_register` (label = a chave que você usou em
`[partitions]`, ex.: `"storage"`), então `fopen("/spiffs/payload.txt",
"r")`.

---

## 6. A API da biblioteca `tw32_common`

A biblioteca compartilhada em `firmware/tw32_common/` fornece quatro cabeçalhos.
Eles são deliberadamente pequenos. Leia os arquivos .c em `tw32_common/src/`
em caso de dúvida.

### 6.1 `tw32_io.h` — E/S de linha UART0

```c
void   tw32_cdc_init(void);
size_t tw32_cdc_write(const void *buf, size_t len);
size_t tw32_cdc_write_str(const char *s);
int    tw32_cdc_readline(char *out, size_t cap, uint32_t timeout_ms);
```

- Init assume o controle da UART0 com um FIFO RX de 1 KB + FIFO TX de 4 KB. Idempotente.
- `write*` é protegido por mutex, seguro de qualquer tarefa.
- `readline` remove CR ou LF como terminador de linha (qualquer um de CR / LF /
  CRLF funciona), termina com NUL, retorna bytes escritos ou 0 em
  vazio/timeout, -1 em estouro. Consumidor único.

O prefixo "cdc" é histórico — o contrato é "fluxo exclusivo baseado em linha
para o host", que é o que um canal USB-CDC seria. Nesta
placa, o transporte é UART0 roteada através de uma ponte USB-UART CH343 (veja
§8). Se uma placa diferente tiver USB-CDC nativo exposto, você pode trocar a
implementação; a API permanece.

### 6.2 `tw32_json_out.h` — Emissor de linhas JSON em streaming

```c
void tw32_json_begin(void);                                   // trava o mutex
void tw32_json_kv_str  (const char *key, const char *val);    // string é escapada
void tw32_json_kv_int  (const char *key, int64_t val);
void tw32_json_kv_uint (const char *key, uint64_t val);
void tw32_json_kv_bool (const char *key, bool val);
void tw32_json_kv_mac  (const char *key, const uint8_t mac[6]);
void tw32_json_array_begin(const char *key);
void tw32_json_array_str  (const char *val);
void tw32_json_array_int  (int64_t val);
void tw32_json_array_end  (void);
void tw32_json_end(void);                                     // adiciona "}\n", libera o mutex
```

Restrições:

- Sempre emparelhe `begin` / `end`. `end` é o que libera o mutex interno
  e envia a linha.
- Um buffer estático de 1024 bytes; registros muito longos são truncados.
- Chaves são escritas literalmente — use ASCII alfanumérico/underscore.
- Strings (valores e itens de array) são escapadas para `"`, `\` e caracteres
  de controle.
- Sem suporte a objetos aninhados. Se você precisar de um objeto aninhado, formate manualmente o
  JSON interno em uma string e emita-o via `tw32_json_kv_str` (o
  `spectrum-analyzer` faz isso para registros por canal).

### 6.3 `tw32_cdc_cli.h` — Dispatcher CLI

```c
typedef struct tw32_cli_ctx tw32_cli_ctx_t;
typedef int (*tw32_cli_fn_t)(tw32_cli_ctx_t *ctx, int argc, char **argv);
typedef struct {
    const char *name;
    const char *help;
    tw32_cli_fn_t fn;
} tw32_cli_cmd_t;

void tw32_cli_ack_ok (const char *cmd);
void tw32_cli_ack_err(const char *cmd, const char *err);

void tw32_cli_run(const char *module_name,
                  const char *version,
                  const tw32_cli_cmd_t *table,
                  size_t table_len);
```

- `tw32_cli_run` nunca retorna. Chame-a como a última coisa em `app_main`.
  Ela cria uma tarefa FreeRTOS de 4 KB fixada no **core 1**, imprime um
  banner `{"event":"ready","module":...,"version":...}`, então faz um loop
  lendo linhas.
- `help` e `version` são adicionados automaticamente — não os defina em
  sua tabela.
- Comandos desconhecidos respondem com
  `{"cmd":"<name>","ok":false,"err":"unknown_command"}`.
- Handlers são chamados a partir da tarefa CLI. Eles executam sincronamente entre
  leituras. Trabalho longo vai em sua **própria** tarefa worker; handlers CLI devem
  virar uma flag e retornar imediatamente (veja todos os módulos inclusos).

### 6.4 `tw32_nvs_kv.h` — Getters NVS tipados

```c
void     tw32_nvs_init(void);
bool     tw32_nvs_get_bool(const char *ns, const char *key, bool default_value);
uint32_t tw32_nvs_get_u32 (const char *ns, const char *key, uint32_t default_value);
int32_t  tw32_nvs_get_i32 (const char *ns, const char *key, int32_t default_value);
bool     tw32_nvs_get_str (const char *ns, const char *key, char *out, size_t cap);
bool     tw32_nvs_get_blob(const char *ns, const char *key, void *out, size_t *len);
```

`tw32_nvs_init()` chama `nvs_flash_init()` e apaga+reinicializa se
a partição foi truncada. Chame-a uma vez no topo de `app_main`,
antes de qualquer handler que leia NVS.

A variante `_str` copia no máximo `cap-1` bytes mais NUL. A variante `_blob`
atualiza `*len` para o tamanho real copiado.

---

## 7. O contrato do protocolo de linhas

Por convenção, todo módulo conversa com o host em um pequeno protocolo baseado em
linhas na UART0:

- **Entrada:** comandos ASCII simples, terminados por `\n` ou `\r`. Tokens
  separados por espaços. Até 8 tokens, 256 bytes por linha.
- **Saída:** um objeto JSON por linha, terminado por `\n`. Formatos
  do objeto:
  - Na inicialização:
    `{"event":"ready","module":"<slug>","version":"<v>"}`
  - Em um comando CLI:
    `{"cmd":"<name>","ok":true|false[,"err":"<code>"]}`
    Alguns comandos incluem chaves extras (`stats`, `peers`, etc.) antes
    de `ok`. Ou o caso de sucesso é apenas `{"cmd":..,"ok":true}` ou
    se estende com quaisquer dados que você quiser.
  - Eventos assíncronos: `{"event":"<name>", ...campos do domínio...}`.

Mantenha eventos pequenos (< 1 KB) porque o buffer JSON é limitado. Mantenha
nomes de comando curtos e em minúsculas (`start`, `stop`, `chan`, `stats`,
`hop`, `dwell`, etc.).

Para fluxos de dados binários (PCAP de `wifi-sniffer` / `wifi-probe-logger`,
quadros ESP-NOW de `espnow-bridge`), mude o canal para binário
**após** enviar uma linha JSON de ack; volte ao texto no `stop`. O host
verá uma borda limpa de texto → binário → texto na linha JSON de ack.

---

## 8. Especificações de hardware da placa de desenvolvimento v0rtex

Os módulos inclusos assumem a placa de desenvolvimento descrita em
`docs/hardware.md`:

- ESP32-S3 (QFN56) revisão v0.2, 8 MB PSRAM octal, 16 MB flash.
- Ponte USB-serial: WCH **CH343 serial única**, VID `0x1A86`, PID
  **`0x55D3`**.
- Nó de dispositivo: **`/dev/ttyACM0`** (CDC ACM via CH343), não
  `/dev/ttyUSB*`.
- Os **pinos USB nativos do S3 (GPIO19/20) NÃO são expostos externamente**
  nesta placa — apenas o caminho CH343→UART0 alcança um conector. Isto
  é por que `tw32_io` usa `driver/uart.h` e não tinyusb. Se você portar para
  uma placa onde o USB nativo ESTEJA exposto (a maioria das variantes
  ESP32-S3-DevKitC-1 tem uma porta "USB" separada), troque `tw32_io.c` por uma
  implementação `tinyusb_cdcacm`; a API do cabeçalho permanece.
- O endereço público BLE é `<MAC STA do Wi-Fi> + 2` no último byte. Para
  `dc:b4:d9:13:aa:34` (Wi-Fi STA), o BLE é `dc:b4:d9:13:aa:36`.
- O usuário deve estar no grupo `uucp` (Arch) ou `dialout` (Debian/Ubuntu) para
  acesso a `/dev/ttyACM0`.
- esptool ≥ v5: nomes de comando usam hífens (`write-flash`, `chip-id`,
  `default-reset`, `hard-reset`, `--flash-size`). O gravador incluso
  usa as formas v5.

---

## 9. Postura de desempenho e robustez

Os módulos inclusos seguem um pequeno conjunto de convenções. Não lute
contra elas; elas existem por causa de bugs reais que encontramos:

### 9.1 Fixação de core

- **Core 0**: tarefas dos drivers Wi-Fi / BLE vivem aqui por padrão do IDF.
- **Core 1**: sua tarefa worker, a tarefa CLI e qualquer tarefa de escrita UART.

`xTaskCreatePinnedToCore(fn, "name", stack, arg, prio, NULL, 1)`. Não
deixe tarefas RF compartilharem um core com tarefas de serialização verbosas.

### 9.2 Dimensionamento de pilha

- Tarefa CLI: 4 KB (definido por `tw32_cli_run`). Não adicione variáveis locais
  gigantes na pilha em handlers.
- Tarefas worker: **mínimo de 6 KB** se fizerem trabalho não trivial. Arrays de
  registros de varredura Wi-Fi sozinhos (32 × ~80 B) consomem metade de uma pilha de 4 KB.
  Mova registros grandes para armazenamento `static` em seu módulo.

> Bug real, corrigido em `wifi-scanner`: a tarefa original tinha 4 KB de pilha
> e `wifi_ap_record_t records[32]` na pilha — estourou e o chip
> reiniciou com `RTC_SW_CPU_RST`. A correção foi tornar `records[]`
> `static` e aumentar a pilha da tarefa para 6 KB.

### 9.3 Estático vs heap

- Buffers em caminho crítico em callbacks: `static`. Callbacks Wi-Fi RX e BLE GAP
  executam em agendamentos RTOS apertados; `malloc` lá é pedir por problemas.
- Registros por evento que passam por uma fila: prefira entradas de fila de tamanho
  fixo com uma struct de rascunho reutilizável estática no callback.
- Alocações únicas (ex.: parsing de um payload NVS na inicialização): heap é ok.

### 9.4 Transferência entre cores

Para dados do callback RF (core 0) para seu serializador (core 1), use uma
FreeRTOS Queue com entradas de tamanho fixo. `wifi-sniffer` e
`wifi-probe-logger` usam isto. `xQueueSend(... , 0)` a partir do callback (sem
back-pressure no caminho RF) mais um contador de descartes é o padrão
correto.

### 9.5 Atributo de caminho crítico

Callbacks Wi-Fi promíscuos devem ser marcados com `IRAM_ATTR` para que não
toquem na flash em caso de miss (o que pode travar o agendamento RF):

```c
static IRAM_ATTR void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t t)
{ /* … */ }
```

### 9.6 Disciplina do `ESP_LOG`

Com `CONFIG_LOG_DEFAULT_LEVEL_ERROR`, chamadas `ESP_LOGI` são compiladas
para fora. `ESP_LOGE` ainda executa. Não use `ESP_LOGI` em caminhos críticos de
callback nem durante o desenvolvimento — são ruídos que podem não ser limpos.

---

## 10. Compilando, gravando, validando

### 10.1 Primeira compilação

```bash
source $IDF_PATH/export.sh
cd <REPO_ROOT>/firmware/<slug>
idf.py set-target esp32s3        # cria sdkconfig a partir de seus defaults
idf.py build                     # ~30-90 s na primeira vez, ~10 s incremental
```

Após isso, gere os artefatos e copie-os para `modules/<slug>/`:

```bash
cd <REPO_ROOT>
./scripts/build.sh <slug>
# Final da saída deve listar os quatro arquivos do módulo com tamanhos.
```

`build.sh` executa `idf.py -B build build` então copia:
- `build/bootloader/bootloader.bin` → `modules/<slug>/bootloader.bin`
- `build/partition_table/partition-table.bin` → `modules/<slug>/partition-table.bin`
- O primeiro `build/*.bin` que não corresponder aos acima → `modules/<slug>/firmware.bin`

Se o nome do seu projeto em `CMakeLists.txt` por acaso começar com
`bootloader` ou `partition-table` (não deveria — use seu slug), o
padrão de descoberta quebra. Não faça isso.

### 10.2 Manifesto

Escreva manualmente `modules/<slug>/module.toml` conforme §5. O comando
`thewave32 list` do gravador exibirá erros do manifesto com o campo ofensor.

### 10.3 Gravação

```bash
source .venv/bin/activate
thewave32 flash <slug>
# ou com entradas:
thewave32 flash <slug> --input "key=value" --input "another=val"
```

Se seu manifesto declarar entradas NVS, o gravador invoca o
pacote Python `esp-idf-nvs-partition-gen` para montar a imagem NVS.
Esse pacote está no venv do projeto (`pyproject.toml` não o fixa
ainda; instale com `pip install esp-idf-nvs-partition-gen` se estiver faltando).

### 10.4 Teste de fumaça no hardware

```bash
# escolha qualquer terminal serial de sua preferência
tio /dev/ttyACM0
# ou
.venv/bin/python -m serial.tools.miniterm /dev/ttyACM0 115200
```

Você deve ver, imediatamente após a inicialização ou ao pressionar o botão
EN do dispositivo:

```
{"event":"ready","module":"<slug>","version":"0.1.0"}
```

Em seguida, digite:

```
> version
< {"cmd":"version","ok":true,"module":"<slug>","version":"0.1.0"}
> help
< {"cmd":"help","ok":true,"commands":["help","version","start","stop","stats", ...]}
> start
< {"cmd":"start","ok":true}
> stats
< {"cmd":"stats","ok":true,"running":true, ...}
> stop
< {"cmd":"stop","ok":true}
```

Se você não vir o evento `ready`, veja §13 (Armadilhas).

### 10.5 Testes do gravador Python

A suíte de testes pytest do gravador está em `tests/`. Ela não exercita seu
firmware, mas analisa seu `module.toml`. Após adicionar um módulo:

```bash
.venv/bin/pytest -q
```

Ainda deve estar 60/60 (ou qualquer que seja a contagem atual). Uma falha em
`test_registry` ou `test_manifest` geralmente significa que seu manifesto está
mal formatado.

---

## 11. A integração com TUI / CLI é automática

Você **não** edita nenhum arquivo Python para registrar um módulo. O gravador
descobre tudo sob `modules/<slug>/` em tempo de execução via
`registry.discover()`. Desde que um `modules/<slug>/module.toml` seja analisado
corretamente e os artefatos referenciados existam, o módulo aparece em:

- `thewave32 list`
- `thewave32 info <slug>`
- A `ListView` principal da TUI
- `thewave32 flash <slug>`

Se seu módulo tiver `[[inputs]]`, a TUI exibe um diálogo de configuração e
a CLI aceita `--input key=value` (repetível). Entradas obrigatórias
sem um valor via CLI abortam a gravação com um erro; entradas opcionais usam
seu `default`.

---

## 12. Distribuindo um módulo

Duas maneiras de compartilhar um módulo com outra pessoa:

### 12.1 Apenas os artefatos (pronto para uso, sem IDF no lado do consumidor)

Compacte estes quatro arquivos:

```
modules/<slug>/module.toml
modules/<slug>/bootloader.bin
modules/<slug>/partition-table.bin
modules/<slug>/firmware.bin
```

O consumidor extrai em seu diretório `modules/<slug>/` e executa
`thewave32 flash <slug>`.

### 12.2 Código-fonte + artefatos (reprodutibilidade completa)

Envie uma árvore de diretórios como os módulos inclusos:

```
firmware/<slug>/             # compilável
modules/<slug>/              # pronto para gravar
```

A convenção neste repositório é commit ambos: produtores podem compilar,
consumidores podem gravar sem ESP-IDF.

Os arquivos `modules/<slug>/firmware.bin` inclusos neste repositório têm
entre 500 KB e 700 KB — pequenos o suficiente para commit. Se sua compilação for
muito maior (território LFS), prefira anexos de release.

---

## 13. Armadilhas comuns e como reconhecê-las

Bugs concretos que já afetaram os módulos inclusos. Se seu módulo
se comportar de forma estranha, varra esta lista primeiro.

| Sintoma | Causa | Correção |
|---------|-------|----------|
| `Failed to resolve component 'esp_now'` na configuração | `esp_now` não é um componente separado no IDF v5.x | Remova de `REQUIRES`; APIs ESP-NOW vêm com `esp_wifi` |
| `Failed to resolve component 'esp_tinyusb'` | Movido para o Gerenciador de Componentes do IDF | Adicione `idf_component.yml` com `dependencies: espressif/esp_tinyusb: "^1.4.5"` — mas reconsidere; nesta placa o USB nativo não é exposto (§8) |
| `error: implicit declaration of function 'ble_gap_call_conn_event_cb'` dentro do fonte NimBLE | O fonte NimBLE tem referências de símbolo entre papéis não protegidas por Kconfig | Não desabilite `BT_NIMBLE_ROLE_*` individuais; deixe os padrões ativados |
| Erro de compilação: `unknown type name 'size_t'` | Header usa `size_t` sem `#include <stddef.h>` | Adicione o include; `<stdbool.h>` **não** puxa `stddef.h` |
| Chip reinicia com `rst:0xc (RTC_SW_CPU_RST)` após algumas iterações | Estouro de pilha na tarefa worker — geralmente registros grandes na pilha | Mova arrays grandes para armazenamento `static`; aumente a pilha da tarefa para ≥ 6 KB |
| `--input key=value` aceito pelo gravador mas o firmware não vê o valor | Discrepância entre tipo do manifesto/leitura do firmware (ex.: `type="string"` gravado como `nvs_str`, firmware lendo `nvs_blob`) | Corresponda a API de leitura ao tipo do manifesto — veja §5.2 |
| `screen /dev/ttyACM0 115200` não mostra nada quando você digita | `screen` não faz eco local via serial; firmware também não faz eco; ALÉM DISSO `screen` envia apenas CR no Enter | `tw32_io` (pós-correção) aceita CR ou LF; se você forkou a biblioteca pré-correção, digite Ctrl-J ou use `tio` |
| Gravador: `Could not open /dev/ttyACM0, the port is busy` | Um programa serial (`screen`/`tio`/`picocom`/ModemManager) detém a porta | `fuser /dev/ttyACM0` para encontrar, `fuser -k /dev/ttyACM0` para liberar; para ModemManager: `sudo systemctl disable --now ModemManager` |
| `esptool` relata `no ESP32-S3 device found on any serial port` | A lista de VID/PID do detector não inclui seu chip USB-serial | Adicione o par (VID,PID) a `KNOWN_VID_PIDS` em `src/thewave32/flasher.py` (CH343 serial única usa `0x1A86:0x55D3`) |
| Logs de boot visíveis mas nenhum `{"event":"ready"}` após | `ESP_CONSOLE_NONE` não definido, console do IDF está disputando a UART0 com seu driver UART | Defina `CONFIG_ESP_CONSOLE_NONE=y` em `sdkconfig.defaults` |
| `idf.py build` falha apenas com avisos de depreciação no final, returncode 0 | Ruído de depreciação do esptool v5, não é falha de compilação | Atualize invocações do gravador para formas v5 (já feito em `src/thewave32/flasher.py`) |
| Gravação bem-sucedida, app inicializa, mas nenhuma saída JSON nos primeiros ~1 s | Inicialização NVS ou Wi-Fi/BLE é lenta na primeira vez após uma gravação nova (calibração de primeira inicialização) | Aguarde 1-2 s; inicializações subsequentes são < 1 s |
| Módulo aparece em `thewave32 list` mas a gravação falha com `module 'X' not found` | O `[module].name` dentro de `module.toml` não corresponde ao nome do diretório | Faça-os corresponder exatamente |
| Saída do comando `peers` truncada no meio da linha | Linha JSON excedeu o buffer de 1 KB em `tw32_json_out.c` | Instantâneo primeiro, emita uma linha `{"event":"peer", ...}` **por peer**, não todos em uma |
| Tecla Enter da TUI não faz nada | `ListView` consome Enter para emitir `Selected`; um binding `enter` no nível do App nunca dispara | Use `on_list_view_selected`; garanta `_list.focus()` e `index = 0` após atualizar; envolva a ação em um worker `@work` para que `push_screen_wait` seja permitido |

---

## 14. Checklist final antes de enviar um módulo

Um checklist copiável:

- [ ] Diretório `firmware/<slug>/` existe com `CMakeLists.txt`,
      `partitions.csv`, `sdkconfig.defaults`, `main/CMakeLists.txt`,
      e `main/<slug_underscored>.c`.
- [ ] `idf.py build` a partir de `firmware/<slug>/` é bem-sucedido sem avisos
      tratados como erros.
- [ ] `./scripts/build.sh <slug>` produziu quatro arquivos em
      `modules/<slug>/`: `module.toml`, `bootloader.bin`,
      `partition-table.bin`, `firmware.bin`.
- [ ] `thewave32 list` mostra o módulo com a descrição correta.
- [ ] `thewave32 info <slug>` analisa o manifesto corretamente.
- [ ] `pytest -q` continua verde.
- [ ] Gravar em um ESP32-S3 real:
      `thewave32 flash <slug>` é bem-sucedido.
- [ ] Abrir um terminal serial (`tio /dev/ttyACM0` etc.) e confirmar:
  - [ ] `{"event":"ready", ...}` chega em ~1 s.
  - [ ] `version` retorna `{"cmd":"version","ok":true,...}`.
  - [ ] `help` lista todo comando em sua `cli_table` mais
        `help` e `version`.
  - [ ] `start` / `stop` alternam o comportamento esperado.
  - [ ] `stats` relata contadores plausíveis (hopping=true após start,
        running condiz com o estado, etc.).
- [ ] Se seu módulo tiver `[[inputs]]`:
  - [ ] `thewave32 flash <slug> --input ...` grava os valores.
  - [ ] Após reinicialização, o firmware lê os valores de volta (use um
        comando CLI `get <key>` ou verifique via `stats`).
  - [ ] O tipo de codificação NVS corresponde à API de leitura do firmware (§5.2).
- [ ] `git status` está limpo exceto por seus novos arquivos. `firmware/<slug>/`
      NÃO deve incluir `build/`, `managed_components/`,
      `dependencies.lock` ou `sdkconfig` (todos ignorados pelo git).
- [ ] Commit com uma mensagem `feat(<slug>):` ou `feat: módulo <slug>`
      que descreva o que o módulo faz e o resultado principal
      da validação em hardware.

---

## 15. Onde ler a seguir

- `firmware/wifi-scanner/main/wifi_scanner.c` — referência mais simples, ~220
  linhas incluindo comentários. Leia isto primeiro.
- `firmware/wifi-eap-watcher/main/wifi_eap_watcher.c` — módulo mais densamente
  comentado, tem exemplos práticos de parsing de quadros, correspondência
  de padrões e testes de parser no dispositivo estilo `inject_test`.
- `firmware/tw32_common/include/*.h` — superfície pública da biblioteca
  compartilhada. Curto, leia tudo.
- Documentação do ESP-IDF: <https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s3/>
- Gerenciador de Componentes: <https://components.espressif.com/>
