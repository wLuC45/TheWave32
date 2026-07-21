/*
 * TheWave32 / net-port-scanner
 *
 * The first associated-mode module: connects to a Wi-Fi network whose
 * credentials are configured via NVS, gets a DHCP lease, and probes
 * given TCP ports across an IP range.
 *
 * NVS inputs (namespace "netps"):
 *   ssid     (string, required)
 *   password (string, required)
 *   ports    (string, default "22,80,443,8080"), comma-separated
 *
 * CLI:
 *   connect             associate, wait for IP, emit `event:"connected"`
 *   disconnect          drop association
 *   ports <csv>         change port list at runtime
 *   scan auto           scan own /24 subnet (skip own IP)
 *   scan <a> [<b>]      scan single IP a, or range a..b inclusive
 *   stats               state + counters
 *
 * For each open port found: `{"event":"open","ip":"...","port":N}`. After
 * a scan completes: `{"cmd":"scan","ok":true,"hosts_seen":N,
 * "ports_open":N}`.
 */

#include <ctype.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <fcntl.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "nvs_flash.h"

#include "tw32_cdc_cli.h"
#include "tw32_io.h"
#include "tw32_json_out.h"
#include "tw32_nvs_kv.h"

#define MODULE_NAME    "net-port-scanner"
#define MODULE_VERSION "0.1.0"

#define MAX_PORTS    32
#define PORT_LIST_DEFAULT "22,80,443,8080"
#define CONNECT_TIMEOUT_MS 15000
#define PROBE_TIMEOUT_MS 200

#define BIT_GOT_IP   (1 << 0)
#define BIT_FAILED   (1 << 1)

typedef struct {
    volatile bool          connected;
    esp_netif_t           *netif;
    EventGroupHandle_t     evt;
    uint16_t               ports[MAX_PORTS];
    int                    port_count;
    /* Last-known IP info from DHCP. Written from the WiFi/IP event
     * task, read from the CLI task; volatile keeps the compiler from
     * caching them across the event-group wait that publishes them. */
    volatile uint32_t      ip;        /* network byte order */
    volatile uint32_t      netmask;
    volatile uint32_t      gw;
    /* Counters. */
    uint32_t               total_scans;
    uint32_t               hosts_seen;
    uint32_t               ports_open;
} state_t;

static state_t s_state;

static int parse_ports_csv(const char *csv, uint16_t *out, int max)
{
    int n = 0;
    const char *p = csv;
    while (*p && n < max) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p) break;
        if (v > 0 && v < 65536) out[n++] = (uint16_t)v;
        p = end;
    }
    return n;
}

static void load_ports(void)
{
    char buf[128];
    bool ok = tw32_nvs_get_str("netps", "ports", buf, sizeof(buf));
    s_state.port_count = parse_ports_csv(ok ? buf : PORT_LIST_DEFAULT,
                                         s_state.ports, MAX_PORTS);
    if (s_state.port_count == 0) {
        s_state.port_count = parse_ports_csv(PORT_LIST_DEFAULT,
                                             s_state.ports, MAX_PORTS);
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_state.connected = false;
        xEventGroupSetBits(s_state.evt, BIT_FAILED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        s_state.ip      = e->ip_info.ip.addr;
        s_state.netmask = e->ip_info.netmask.addr;
        s_state.gw      = e->ip_info.gw.addr;
        s_state.connected = true;
        xEventGroupSetBits(s_state.evt, BIT_GOT_IP);
    }
}

static void wifi_init_sta(void)
{
    { esp_err_t _e = esp_event_loop_create_default();
      if (_e != ESP_OK && _e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(_e); }
    ESP_ERROR_CHECK(esp_netif_init());
    s_state.netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* --- TCP probing --------------------------------------------------- */

/*
 * Probe a single TCP port. Returns true iff the remote responded with a
 * SYN-ACK that completed the 3-way handshake (RFC 9293 sec 3.10.7,
 * Nmap "open" state).
 *
 * Hardened against the cases the original code skipped:
 *   - socket() may succeed but fcntl(F_GETFL) may fail; without the
 *     non-blocking flag the connect() then blocks for the full lwIP
 *     TCP retransmission window (seconds), stalling the scan.
 *   - select() may return error / be interrupted (EINTR) and must not
 *     be treated as "open".
 *   - select with only the write-set misses peers that complete the
 *     handshake with an immediate RST; including the exception set
 *     wakes us up on that case so we still call SO_ERROR and report
 *     the correct closed/refused state.
 *
 * Closed (RST -> ECONNREFUSED), filtered (timeout) and unreachable
 * (ENETUNREACH / EHOSTUNREACH) all map to "not open" in this module
 * by design; only fully established connections are reported.
 */
static bool probe_port(uint32_t ip_be, uint16_t port, int timeout_ms)
{
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return false;
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(s);
        return false;
    }

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(port);
    dst.sin_addr.s_addr = ip_be;

    int rc = connect(s, (struct sockaddr *)&dst, sizeof(dst));
    if (rc == 0) { close(s); return true; }
    if (errno != EINPROGRESS) { close(s); return false; }

    fd_set wset, eset;
    FD_ZERO(&wset);
    FD_ZERO(&eset);
    FD_SET(s, &wset);
    FD_SET(s, &eset);
    struct timeval tv = {
        .tv_sec  = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    rc = select(s + 1, NULL, &wset, &eset, &tv);
    bool open = false;
    if (rc > 0 && !FD_ISSET(s, &eset)) {
        int err = 0;
        socklen_t err_len = sizeof(err);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &err_len) == 0 && err == 0) {
            open = true;
        }
    }
    close(s);
    return open;
}

static void emit_open(uint32_t ip_be, uint16_t port)
{
    char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u",
             (unsigned)(ip_be & 0xFF),
             (unsigned)((ip_be >> 8)  & 0xFF),
             (unsigned)((ip_be >> 16) & 0xFF),
             (unsigned)((ip_be >> 24) & 0xFF));
    tw32_json_begin();
    tw32_json_kv_str("event", "open");
    tw32_json_kv_str("ip",    ip_str);
    tw32_json_kv_int("port",  port);
    tw32_json_end();
    s_state.ports_open++;
}

static void scan_ip(uint32_t ip_be, bool *any_open)
{
    bool found = false;
    for (int i = 0; i < s_state.port_count; i++) {
        if (probe_port(ip_be, s_state.ports[i], PROBE_TIMEOUT_MS)) {
            emit_open(ip_be, s_state.ports[i]);
            found = true;
        }
    }
    if (found) {
        s_state.hosts_seen++;
        *any_open = true;
    }
}

static int parse_ipv4(const char *s, uint32_t *out)
{
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return -1;
    if (a > 255 || b > 255 || c > 255 || d > 255) return -1;
    *out = (uint32_t)(a | (b << 8) | (c << 16) | (d << 24));
    return 0;
}

/* Convert between network-byte-order uint32 (as stored in lwIP / sockets)
 * and a host-order uint32 where the first dotted octet sits in the high
 * byte. Doing arithmetic in host order avoids the BE-uint32 trap that
 * made the original auto-scan implicitly /24-only. */
static inline uint32_t be_to_host(uint32_t ip_be)
{
    return ((ip_be & 0xFFu)       << 24)
         | ((ip_be & 0xFF00u)     <<  8)
         | ((ip_be & 0xFF0000u)   >>  8)
         | ((ip_be & 0xFF000000u) >> 24);
}

static inline uint32_t host_to_be(uint32_t ip_host)
{
    return be_to_host(ip_host); /* swap is self-inverse */
}

/* Cap auto-scan to keep CLI responsive: a /20 is 4094 hosts which at
 * 200ms per port over 4 ports is already > 50 minutes. Refuse anything
 * wider than that and ask the user to give an explicit range. */
#define AUTO_SCAN_MAX_HOSTS 4096u

/* --- CLI ----------------------------------------------------------- */

static int cmd_connect(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    char ssid[33] = {0}, pass[65] = {0};
    if (!tw32_nvs_get_str("netps", "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        tw32_cli_ack_err("connect", "no_ssid"); return -1;
    }
    tw32_nvs_get_str("netps", "password", pass, sizeof(pass));
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) {
        tw32_cli_ack_err("connect", "set_config_failed"); return -1;
    }
    xEventGroupClearBits(s_state.evt, BIT_GOT_IP | BIT_FAILED);
    if (esp_wifi_connect() != ESP_OK) {
        tw32_cli_ack_err("connect", "connect_failed"); return -1;
    }
    EventBits_t bits = xEventGroupWaitBits(s_state.evt, BIT_GOT_IP | BIT_FAILED,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    if (!(bits & BIT_GOT_IP)) {
        esp_wifi_disconnect();
        tw32_cli_ack_err("connect", "timeout_or_failed"); return -1;
    }
    char ip_s[16], gw_s[16], mask_s[16];
    snprintf(ip_s, sizeof(ip_s), "%u.%u.%u.%u",
             (unsigned)(s_state.ip & 0xFF), (unsigned)((s_state.ip >> 8) & 0xFF),
             (unsigned)((s_state.ip >> 16) & 0xFF), (unsigned)((s_state.ip >> 24) & 0xFF));
    snprintf(gw_s, sizeof(gw_s), "%u.%u.%u.%u",
             (unsigned)(s_state.gw & 0xFF), (unsigned)((s_state.gw >> 8) & 0xFF),
             (unsigned)((s_state.gw >> 16) & 0xFF), (unsigned)((s_state.gw >> 24) & 0xFF));
    snprintf(mask_s, sizeof(mask_s), "%u.%u.%u.%u",
             (unsigned)(s_state.netmask & 0xFF), (unsigned)((s_state.netmask >> 8) & 0xFF),
             (unsigned)((s_state.netmask >> 16) & 0xFF), (unsigned)((s_state.netmask >> 24) & 0xFF));
    tw32_json_begin();
    tw32_json_kv_str ("cmd", "connect");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_str ("ssid", ssid);
    tw32_json_kv_str ("ip",   ip_s);
    tw32_json_kv_str ("gw",   gw_s);
    tw32_json_kv_str ("mask", mask_s);
    tw32_json_end();
    return 0;
}

static int cmd_disconnect(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    /* Mark down BEFORE asking the supplicant to leave so that any
     * scan racing against this CLI call (future async use) sees the
     * disconnected state. esp_wifi_disconnect() returns synchronously
     * but the DISCONNECTED event is delivered from the WiFi task and
     * will redundantly set connected=false plus BIT_FAILED. Clearing
     * the event bits here gives the next cmd_connect a clean slate. */
    s_state.connected = false;
    esp_wifi_disconnect();
    xEventGroupClearBits(s_state.evt, BIT_GOT_IP | BIT_FAILED);
    tw32_cli_ack_ok("disconnect");
    return 0;
}

static int cmd_ports(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) {
        /* Read-only emit */
        tw32_json_begin();
        tw32_json_kv_str ("cmd", "ports");
        tw32_json_kv_bool("ok", true);
        tw32_json_array_begin("ports");
        for (int i = 0; i < s_state.port_count; i++) tw32_json_array_int(s_state.ports[i]);
        tw32_json_array_end();
        tw32_json_end();
        return 0;
    }
    /* Set */
    uint16_t tmp[MAX_PORTS];
    int n = parse_ports_csv(argv[1], tmp, MAX_PORTS);
    if (n == 0) { tw32_cli_ack_err("ports", "bad_list"); return -1; }
    memcpy(s_state.ports, tmp, sizeof(uint16_t) * n);
    s_state.port_count = n;
    tw32_cli_ack_ok("ports");
    return 0;
}

static int cmd_scan(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (!s_state.connected) {
        tw32_cli_ack_err("scan", "not_connected"); return -1;
    }
    if (argc < 2) {
        tw32_cli_ack_err("scan", "usage: scan auto | scan <ip> [<ip>]"); return -1;
    }
    uint32_t start_host, end_host;
    if (!strcmp(argv[1], "auto")) {
        /* Walk the actual masked subnet, not a hard-coded /24. The
         * network address (host bits all zero) and the directed
         * broadcast (host bits all one) are skipped per RFC 1812
         * section 5.3.5. A /32 has neither and yields a single host. */
        uint32_t ip_host   = be_to_host(s_state.ip);
        uint32_t mask_host = be_to_host(s_state.netmask);
        uint32_t host_bits = ~mask_host;
        uint32_t network   = ip_host & mask_host;
        uint32_t broadcast = network | host_bits;
        if (host_bits == 0) {                       /* /32, scan self only */
            start_host = end_host = ip_host;
        } else if (host_bits == 1) {                /* /31, both ends are hosts */
            start_host = network;
            end_host   = broadcast;
        } else {
            start_host = network + 1;
            end_host   = broadcast - 1;
        }
        uint32_t count = end_host - start_host + 1;
        if (count > AUTO_SCAN_MAX_HOSTS) {
            tw32_cli_ack_err("scan", "subnet_too_large_use_explicit_range");
            return -1;
        }
    } else if (argc == 2) {
        uint32_t one_be;
        if (parse_ipv4(argv[1], &one_be) != 0) {
            tw32_cli_ack_err("scan", "bad_ip"); return -1;
        }
        start_host = end_host = be_to_host(one_be);
    } else {
        uint32_t a_be, b_be;
        if (parse_ipv4(argv[1], &a_be) != 0 || parse_ipv4(argv[2], &b_be) != 0) {
            tw32_cli_ack_err("scan", "bad_ip"); return -1;
        }
        start_host = be_to_host(a_be);
        end_host   = be_to_host(b_be);
        if (start_host > end_host) {
            tw32_cli_ack_err("scan", "start_after_end"); return -1;
        }
    }

    s_state.total_scans++;
    uint32_t hosts_seen_before = s_state.hosts_seen;
    uint32_t ports_open_before = s_state.ports_open;
    uint32_t self_host = be_to_host(s_state.ip);
    /* Inclusive sweep; uint32 increment is safe because we stop on
     * equality with end_host before the would-overflow add. */
    for (uint32_t h = start_host; ; h++) {
        if (h != self_host) {                          /* skip self */
            bool any = false;
            scan_ip(host_to_be(h), &any);
        }
        if (h == end_host) break;
    }

    tw32_json_begin();
    tw32_json_kv_str ("cmd", "scan");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_uint("hosts_seen", s_state.hosts_seen - hosts_seen_before);
    tw32_json_kv_uint("ports_open", s_state.ports_open - ports_open_before);
    tw32_json_end();
    return 0;
}

static int cmd_stats(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    tw32_json_begin();
    tw32_json_kv_str ("cmd", "stats");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_bool("connected",  s_state.connected);
    tw32_json_kv_uint("total_scans",s_state.total_scans);
    tw32_json_kv_uint("hosts_seen", s_state.hosts_seen);
    tw32_json_kv_uint("ports_open", s_state.ports_open);
    tw32_json_kv_int ("port_count", s_state.port_count);
    tw32_json_end();
    return 0;
}

static const tw32_cli_cmd_t cli_table[] = {
    { "connect",    "associate using NVS ssid+password",     cmd_connect    },
    { "disconnect", "drop association",                       cmd_disconnect },
    { "ports",      "ports [csv] - read or set port list",    cmd_ports      },
    { "scan",       "scan auto | scan <ip> [<ip>]",           cmd_scan       },
    { "stats",      "state + counters",                       cmd_stats      },
};

void app_main(void)
{
    tw32_nvs_init();
    tw32_cdc_init();
    s_state.evt = xEventGroupCreate();
    configASSERT(s_state.evt);
    load_ports();
    wifi_init_sta();
    tw32_cli_run(MODULE_NAME, MODULE_VERSION,
                 cli_table, sizeof(cli_table) / sizeof(cli_table[0]));
}
