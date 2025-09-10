/* Console example esp32_nat_router.c
   Build for T-SIM7070G

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "esp_system.h"
#include "esp_log.h"
#include "esp_console.h"
#include "esp_vfs_dev.h"
#include "driver/uart.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "esp_vfs_usb_serial_jtag.h"
#include "driver/usb_serial_jtag.h"
#endif
#include "linenoise/linenoise.h"
#include "argtable3/argtable3.h"
#include "esp_vfs_fat.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_eap_client.h"

#include "lwip/opt.h"
#include "lwip/err.h"
#include "lwip/sys.h"

#include "dhcpserver/dhcpserver.h"
#include "dhcpserver/dhcpserver_options.h"

#include "cmd_decl.h"
#include <esp_http_server.h>

#if !IP_NAPT
#error "IP_NAPT must be defined"
#endif
#include "lwip/lwip_napt.h"

#include "router_globals.h"

// On board LED
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define BLINK_GPIO 44
#else
#define BLINK_GPIO 2
#endif


#ifndef DEFAULT_AP_SSID
#define DEFAULT_AP_SSID     "NozzleBOX"
#endif
#ifndef DEFAULT_AP_PASS
#define DEFAULT_AP_PASS     ""   // 8–63 chars
#endif
#ifndef DEFAULT_STA_SSID
#define DEFAULT_STA_SSID    ""   // optional
#endif
#ifndef DEFAULT_STA_PASS
#define DEFAULT_STA_PASS    "NozzleCAM"   // optional
#endif
#ifndef DEFAULT_ENT_USER
#define DEFAULT_ENT_USER    ""   // for WPA2-Enterprise (optional)
#endif
#ifndef DEFAULT_ENT_IDENT
#define DEFAULT_ENT_IDENT   ""   // for WPA2-Enterprise (optional)
#endif

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about one event
 * - are we connected to the AP with an IP? */
const int WIFI_CONNECTED_BIT = BIT0;

#define DEFAULT_AP_IP "192.168.5.1"
#define DEFAULT_DNS "8.8.8.8"

/* Global vars */
uint16_t connect_count = 0;
bool ap_connect = false;
bool has_static_ip = false;

uint32_t my_ip;
uint32_t my_ap_ip;

struct portmap_table_entry {
  u32_t daddr;
  u16_t mport;
  u16_t dport;
  u8_t proto;
  u8_t valid;
};
struct portmap_table_entry portmap_tab[IP_PORTMAP_MAX];

esp_netif_t* wifiAP;
esp_netif_t* wifiSTA;

httpd_handle_t start_webserver(void);

static const char *TAG = "ESP32 NAT router";

// helper at top of file (or static in wifi_init)
static const char* auth_to_str(wifi_auth_mode_t a) {
    switch (a) {
        case WIFI_AUTH_OPEN:            return "OPEN";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA-PSK";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2-PSK";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
#ifdef WIFI_AUTH_WPA2_WPA3_PSK
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
#endif
#ifdef WIFI_AUTH_WPA3_PSK
        case WIFI_AUTH_WPA3_PSK:        return "WPA3-PSK";
#endif
        default:                        return "UNKNOWN";
    }
}

/* Console command history can be stored to and loaded from a file.
 * The easiest way to do this is to use FATFS filesystem on top of
 * wear_levelling library.
 */
#if CONFIG_STORE_HISTORY

#define MOUNT_PATH "/data"
#define HISTORY_PATH MOUNT_PATH "/history.txt"

#include "nvs.h"
#include "nvs_flash.h"

static void seed_config_from_defaults(void) {
    nvs_handle_t nvh;
    if (nvs_open("esp32_nat", NVS_READWRITE, &nvh) != ESP_OK) return;

    char buf[65]; size_t len;

    // AP SSID
    len = sizeof(buf); 
    if (nvs_get_str(nvh, "ap_ssid", buf, &len) != ESP_OK || buf[0] == '\0') {
        nvs_set_str(nvh, "ap_ssid", DEFAULT_AP_SSID);
    }
    // AP PASS
    len = sizeof(buf);
    if (nvs_get_str(nvh, "ap_passwd", buf, &len) != ESP_OK) {
        nvs_set_str(nvh, "ap_passwd", DEFAULT_AP_PASS);
    }
    // AP IP
    len = sizeof(buf);
    if (nvs_get_str(nvh, "ap_ip", buf, &len) != ESP_OK || buf[0] == '\0') {
        nvs_set_str(nvh, "ap_ip", DEFAULT_AP_IP);
    }
    // STA SSID
    len = sizeof(buf);
    if (nvs_get_str(nvh, "ssid", buf, &len) != ESP_OK) {
        nvs_set_str(nvh, "ssid", DEFAULT_STA_SSID);
    }
    // STA PASS
    len = sizeof(buf);
    if (nvs_get_str(nvh, "passwd", buf, &len) != ESP_OK) {
        nvs_set_str(nvh, "passwd", DEFAULT_STA_PASS);
    }

    nvs_commit(nvh);
    nvs_close(nvh);
}


static void nvs_get_or_set_default(const char *key, char *dst, size_t dstlen, const char *def_val, bool force_set)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    nvs_handle_t nvh;
    if (nvs_open("esp32_nat", NVS_READWRITE, &nvh) != ESP_OK) {
        strlcpy(dst, def_val, dstlen);
        return;
    }

    size_t len = dstlen;
    err = nvs_get_str(nvh, key, dst, &len);

    bool missing = (err != ESP_OK) || (len == 0) || (dst[0] == '\0');
    if (missing || force_set) {
        strlcpy(dst, def_val, dstlen);
        nvs_set_str(nvh, key, dst);
        nvs_commit(nvh);
    }
    nvs_close(nvh);
}


static void initialize_filesystem(void)
{
    static wl_handle_t wl_handle;
    const esp_vfs_fat_mount_config_t mount_config = {
            .max_files = 4,
            .format_if_mount_failed = true
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(MOUNT_PATH, "storage", &mount_config, &wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FATFS (%s)", esp_err_to_name(err));
        return;
    }
}
#endif // CONFIG_STORE_HISTORY

static void initialize_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK( nvs_flash_erase() );
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

esp_err_t apply_portmap_tab() {
    for (int i = 0; i<IP_PORTMAP_MAX; i++) {
        if (portmap_tab[i].valid) {
            ip_portmap_add(portmap_tab[i].proto, my_ip, portmap_tab[i].mport, portmap_tab[i].daddr, portmap_tab[i].dport);
        }
    }
    return ESP_OK;
}

esp_err_t delete_portmap_tab() {
    for (int i = 0; i<IP_PORTMAP_MAX; i++) {
        if (portmap_tab[i].valid) {
            ip_portmap_remove(portmap_tab[i].proto, portmap_tab[i].mport);
        }
    }
    return ESP_OK;
}

void print_portmap_tab() {
    for (int i = 0; i<IP_PORTMAP_MAX; i++) {
        if (portmap_tab[i].valid) {
            printf ("%s", portmap_tab[i].proto == PROTO_TCP?"TCP ":"UDP ");
            ip4_addr_t addr;
            addr.addr = my_ip;
            printf (IPSTR":%d -> ", IP2STR(&addr), portmap_tab[i].mport);
            addr.addr = portmap_tab[i].daddr;
            printf (IPSTR":%d\n", IP2STR(&addr), portmap_tab[i].dport);
        }
    }
}

esp_err_t get_portmap_tab() {
    esp_err_t err;
    nvs_handle_t nvs;
    size_t len;

    err = nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_blob(nvs, "portmap_tab", NULL, &len);
    if (err == ESP_OK) {
        if (len != sizeof(portmap_tab)) {
            err = ESP_ERR_NVS_INVALID_LENGTH;
        } else {
            err = nvs_get_blob(nvs, "portmap_tab", portmap_tab, &len);
            if (err != ESP_OK) {
                memset(portmap_tab, 0, sizeof(portmap_tab));
            }
        }
    }
    nvs_close(nvs);

    return err;
}

esp_err_t add_portmap(u8_t proto, u16_t mport, u32_t daddr, u16_t dport) {
    esp_err_t err;
    nvs_handle_t nvs;

    for (int i = 0; i<IP_PORTMAP_MAX; i++) {
        if (!portmap_tab[i].valid) {
            portmap_tab[i].proto = proto;
            portmap_tab[i].mport = mport;
            portmap_tab[i].daddr = daddr;
            portmap_tab[i].dport = dport;
            portmap_tab[i].valid = 1;

            err = nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs);
            if (err != ESP_OK) {
                return err;
            }
            err = nvs_set_blob(nvs, "portmap_tab", portmap_tab, sizeof(portmap_tab));
            if (err == ESP_OK) {
                err = nvs_commit(nvs);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "New portmap table stored.");
                }
            }
            nvs_close(nvs);

            ip_portmap_add(proto, my_ip, mport, daddr, dport);

            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t del_portmap(u8_t proto, u16_t mport) {
    esp_err_t err;
    nvs_handle_t nvs;

    for (int i = 0; i<IP_PORTMAP_MAX; i++) {
        if (portmap_tab[i].valid && portmap_tab[i].mport == mport && portmap_tab[i].proto == proto) {
            portmap_tab[i].valid = 0;

            err = nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs);
            if (err != ESP_OK) {
                return err;
            }
            err = nvs_set_blob(nvs, "portmap_tab", portmap_tab, sizeof(portmap_tab));
            if (err == ESP_OK) {
                err = nvs_commit(nvs);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "New portmap table stored.");
                }
            }
            nvs_close(nvs);

            ip_portmap_remove(proto, mport);
            return ESP_OK;
        }
    }
    return ESP_OK;
}

static void initialize_console(void)
{
    /* Disable buffering on stdin */
    setvbuf(stdin, NULL, _IONBF, 0);

#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    /* Drain stdout before reconfiguring it */
    fflush(stdout);
    fsync(fileno(stdout));
    
    /* Minicom, screen, idf_monitor send CR when ENTER key is pressed */
    esp_vfs_dev_uart_port_set_rx_line_endings(0, ESP_LINE_ENDINGS_CR);
    /* Move the caret to the beginning of the next line on '\n' */
    esp_vfs_dev_uart_port_set_tx_line_endings(0, ESP_LINE_ENDINGS_CRLF);

    /* Configure UART. Note that REF_TICK is used so that the baud rate remains
     * correct while APB frequency is changing in light sleep mode.
     */
    const uart_config_t uart_config = {
            .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            #if defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S2)
                .source_clk = UART_SCLK_REF_TICK,
            #else
                .source_clk = UART_SCLK_XTAL,
            #endif
    };
    /* Install UART driver for interrupt-driven reads and writes */
    ESP_ERROR_CHECK( uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM,
            256, 0, 0, NULL, 0) );
    ESP_ERROR_CHECK( uart_param_config(CONFIG_ESP_CONSOLE_UART_NUM, &uart_config) );

    /* Tell VFS to use UART driver */
    esp_vfs_dev_uart_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
#endif

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    /* Enable non-blocking mode on stdin and stdout */
    fcntl(fileno(stdout), F_SETFL, O_NONBLOCK);
    fcntl(fileno(stdin), F_SETFL, O_NONBLOCK);

    /* Minicom, screen, idf_monitor send CR when ENTER key is pressed */
    esp_vfs_dev_usb_serial_jtag_set_rx_line_endings(ESP_LINE_ENDINGS_CR);

    /* Move the caret to the beginning of the next line on '\n' */
    esp_vfs_dev_usb_serial_jtag_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
    usb_serial_jtag_driver_config_t usb_serial_jtag_config = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 256,
    };

    /* Install USB-SERIAL-JTAG driver for interrupt-driven reads and writes */
    usb_serial_jtag_driver_install(&usb_serial_jtag_config);

    /* Tell vfs to use usb-serial-jtag driver */
    esp_vfs_usb_serial_jtag_use_driver();
#endif

    /* Initialize the console */
    esp_console_config_t console_config = {
            .max_cmdline_args = 8,
            .max_cmdline_length = 256,
#if CONFIG_LOG_COLORS
            .hint_color = atoi(LOG_COLOR_CYAN)
#endif
    };
    ESP_ERROR_CHECK( esp_console_init(&console_config) );

    /* Configure linenoise line completion library */
    /* Enable multiline editing. If not set, long commands will scroll within
     * single line.
     */
    linenoiseSetMultiLine(1);

    /* Tell linenoise where to get command completions and hints */
    linenoiseSetCompletionCallback(&esp_console_get_completion);
    linenoiseSetHintsCallback((linenoiseHintsCallback*) &esp_console_get_hint);

    /* Set command history size */
    linenoiseHistorySetMaxLen(100);

#if CONFIG_STORE_HISTORY
    /* Load command history from filesystem */
    linenoiseHistoryLoad(HISTORY_PATH);
#endif
}

void * led_status_thread(void * p)
{
    gpio_reset_pin(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);

    while (true)
    {
        gpio_set_level(BLINK_GPIO, ap_connect);

        for (int i = 0; i < connect_count; i++)
        {
            gpio_set_level(BLINK_GPIO, 1 - ap_connect);
            vTaskDelay(50 / portTICK_PERIOD_MS);
            gpio_set_level(BLINK_GPIO, ap_connect);
            vTaskDelay(50 / portTICK_PERIOD_MS);
        }

        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    esp_netif_dns_info_t dns;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGI(TAG,"disconnected - retry to connect to the AP");
        ap_connect = false;
        esp_wifi_connect();
        ESP_LOGI(TAG, "retry to connect to the AP");
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        ap_connect = true;
        my_ip = event->ip_info.ip.addr;
        delete_portmap_tab();
        apply_portmap_tab();
        if (esp_netif_get_dns_info(wifiSTA, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK)
        {
            esp_netif_set_dns_info(wifiAP, ESP_NETIF_DNS_MAIN, &dns);
            ESP_LOGI(TAG, "set dns to:" IPSTR, IP2STR(&(dns.ip.u_addr.ip4)));
        }
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        connect_count++;
        ESP_LOGI(TAG,"%d. station connected", connect_count);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        connect_count--;
        ESP_LOGI(TAG,"station disconnected - %d remain", connect_count);
    }
}

const int CONNECTED_BIT = BIT0;
#define JOIN_TIMEOUT_MS (2000)


void wifi_init(const uint8_t* mac,
               const char* ssid,
               const char* ent_username,
               const char* ent_identity,
               const char* passwd,
               const char* static_ip,
               const char* subnet_mask,
               const char* gateway_addr,
               const uint8_t* ap_mac,
               const char* ap_ssid,
               const char* ap_passwd,
               const char* ap_ip)
{
    // ---------- Resolve inputs with compile-time defaults ----------
    char sta_ssid[33], sta_pass[65], ent_user[64], ent_ident[64];
    char ap_ssid_buf[33], ap_pass_buf[65], ap_ip_buf[16];

    strlcpy(sta_ssid,   (ssid         && ssid[0])         ? ssid         : DEFAULT_STA_SSID,  sizeof(sta_ssid));
    strlcpy(sta_pass,   (passwd       && passwd[0])       ? passwd       : DEFAULT_STA_PASS,  sizeof(sta_pass));
    strlcpy(ent_user,   (ent_username && ent_username[0]) ? ent_username : DEFAULT_ENT_USER,  sizeof(ent_user));
    strlcpy(ent_ident,  (ent_identity && ent_identity[0]) ? ent_identity : DEFAULT_ENT_IDENT, sizeof(ent_ident));
    strlcpy(ap_ssid_buf,(ap_ssid      && ap_ssid[0])      ? ap_ssid      : DEFAULT_AP_SSID,   sizeof(ap_ssid_buf));
    strlcpy(ap_pass_buf,(ap_passwd    && ap_passwd[0])    ? ap_passwd    : DEFAULT_AP_PASS,   sizeof(ap_pass_buf));
    strlcpy(ap_ip_buf,  (ap_ip        && ap_ip[0])        ? ap_ip        : DEFAULT_AP_IP,     sizeof(ap_ip_buf));

    // ---------- Netifs ----------
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifiAP  = esp_netif_create_default_wifi_ap();
    wifiSTA = esp_netif_create_default_wifi_sta();

    // ---------- Optional static IP on STA ----------
    if (sta_ssid[0] && static_ip && static_ip[0] && subnet_mask && subnet_mask[0] && gateway_addr && gateway_addr[0]) {
        has_static_ip = true;
        esp_netif_ip_info_t ipInfo_sta = {0};
        ipInfo_sta.ip.addr      = esp_ip4addr_aton(static_ip);
        ipInfo_sta.gw.addr      = esp_ip4addr_aton(gateway_addr);
        ipInfo_sta.netmask.addr = esp_ip4addr_aton(subnet_mask);
        esp_netif_dhcpc_stop(wifiSTA);
        ESP_ERROR_CHECK(esp_netif_set_ip_info(wifiSTA, &ipInfo_sta));
        apply_portmap_tab();
    }

    // ---------- Configure AP IP + DHCP ----------
    my_ap_ip = esp_ip4addr_aton(ap_ip_buf);
    esp_netif_ip_info_t ipInfo_ap = {0};
    ipInfo_ap.ip.addr = my_ap_ip;
    ipInfo_ap.gw.addr = my_ap_ip;
    esp_netif_set_ip4_addr(&ipInfo_ap.netmask, 255,255,255,0);
    esp_netif_dhcps_stop(wifiAP);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(wifiAP, &ipInfo_ap));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(wifiAP));

    // ---------- Event handlers ----------
    esp_event_handler_instance_t instance_any_id, instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    // ---------- Wi-Fi init ----------
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // ---------- AP config ----------
    wifi_config_t ap_config = {0};
    ap_config.ap.channel         = 6;      // safe default
    ap_config.ap.ssid_hidden     = 0;
    ap_config.ap.max_connection  = 8;
    ap_config.ap.beacon_interval = 100;

    strlcpy((char*)ap_config.ap.ssid, ap_ssid_buf, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen((const char*)ap_config.ap.ssid);

    if (strlen(ap_pass_buf) < 8) {
        ap_config.ap.authmode   = WIFI_AUTH_OPEN;
        ap_config.ap.password[0] = '\0';   // MUST be empty for OPEN
    } else {
    #ifdef CONFIG_ESP_WIFI_ENABLE_WPA3_SAE
        ap_config.ap.authmode   = WIFI_AUTH_WPA2_WPA3_PSK;
    #else
        ap_config.ap.authmode   = WIFI_AUTH_WPA2_PSK;
    #endif
        strlcpy((char*)ap_config.ap.password, ap_pass_buf, sizeof(ap_config.ap.password));
    }

    // ---------- STA config (optional) ----------
    bool do_sta = (sta_ssid[0] != '\0');
    wifi_config_t sta_config = {0};

    if (do_sta) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        strlcpy((char*)sta_config.sta.ssid, sta_ssid, sizeof(sta_config.sta.ssid));

        if (ent_user[0] == '\0') {
            // PSK
            ESP_LOGI(TAG, "STA regular connection");
            strlcpy((char*)sta_config.sta.password, sta_pass, sizeof(sta_config.sta.password));
        }

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

    #ifdef CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT
        if (ent_user[0] != '\0') {
            // WPA2-Enterprise (PEAP/MSCHAPv2, etc.)
            ESP_LOGI(TAG, "STA enterprise connection");
            const char* ident = (ent_ident[0] != '\0') ? ent_ident : ent_user;
            ESP_ERROR_CHECK(esp_eap_client_set_identity((const uint8_t*)ident, strlen(ident)));
            ESP_ERROR_CHECK(esp_eap_client_set_username((const uint8_t*)ent_user, strlen(ent_user)));
            ESP_ERROR_CHECK(esp_eap_client_set_password((const uint8_t*)sta_pass, strlen(sta_pass)));
            ESP_ERROR_CHECK(esp_wifi_sta_enterprise_enable());
        }
    #endif

        if (mac) {
            ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, mac));
        }
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    }

    // Apply AP config + optional AP MAC
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    if (ap_mac) {
        ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_AP, ap_mac));
    }

    // ---------- DHCP server: offer DNS ----------
    {
        dhcps_offer_t dhcps_dns_value = OFFER_DNS;
        esp_netif_dhcps_option(wifiAP, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                               &dhcps_dns_value, sizeof(dhcps_dns_value));

        esp_netif_dns_info_t dnsserver = {0};
    #ifdef DEFAULT_DNS
        dnsserver.ip.u_addr.ip4.addr = esp_ip4addr_aton(DEFAULT_DNS);
    #else
        dnsserver.ip.u_addr.ip4.addr = esp_ip4addr_aton("8.8.8.8");
    #endif
        dnsserver.ip.type = ESP_IPADDR_TYPE_V4;
        ESP_ERROR_CHECK(esp_netif_set_dns_info(wifiAP, ESP_NETIF_DNS_MAIN, &dnsserver));
    }

    // ---------- Boot banner ----------
    ESP_LOGI(TAG, "================== ESP32 NAT Boot ==================");

    // HTTP server compile-time limits (from sdkconfig)
    #ifdef CONFIG_HTTPD_MAX_REQ_HDR_LEN
        ESP_LOGI(TAG, "HTTPD: MAX_REQ_HDR_LEN=%d, MAX_URI_LEN=%d",
                CONFIG_HTTPD_MAX_REQ_HDR_LEN,
    #ifdef CONFIG_HTTPD_MAX_URI_LEN
                CONFIG_HTTPD_MAX_URI_LEN
    #else
                512
    #endif
        );
    #endif

        // NAT/LWIP features
    #if defined(CONFIG_LWIP_IP_FORWARD)
        ESP_LOGI(TAG, "LWIP: IP_FORWARD=%d  IPV4_NAPT=%d",
                (int)CONFIG_LWIP_IP_FORWARD,
    #ifdef CONFIG_LWIP_IPV4_NAPT
                (int)CONFIG_LWIP_IPV4_NAPT
    #else
                0
    #endif
        );
    #endif

    #ifdef APPLY_DEFAULTS_EVERY_BOOT
        ESP_LOGI(TAG, "Defaults applied each boot: YES");
    #else
        ESP_LOGI(TAG, "Defaults applied each boot: NO (NVS persists)");
    #endif

        // AP params
        ESP_LOGI(TAG, "AP:  SSID=\"%s\"  auth=%s  ch=%d  ip=" IPSTR "/24",
                (char*)ap_config.ap.ssid,
                auth_to_str(ap_config.ap.authmode),
                ap_config.ap.channel,
                IP2STR(&ipInfo_ap.ip));

        // STA params (mask password, show length only)
        if (do_sta) {
            ESP_LOGI(TAG, "STA: SSID=\"%s\"  pass_len=%u  static_ip=%s",
                    (char*)sta_config.sta.ssid,
                    (unsigned)strlen(sta_pass),
                    (has_static_ip ? "YES" : "NO"));

        #ifdef CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT
            if (ent_user[0]) {
                ESP_LOGI(TAG, "STA: WPA2-Enterprise  user=\"%s\"  ident=\"%s\"",
                        ent_user,
                        (ent_ident[0] ? ent_ident : ent_user));
            }
        #endif
        } else {
            ESP_LOGI(TAG, "STA: disabled (AP-only mode)");
        }

    #ifdef DEFAULT_AP_SSID
        // helpful to see what compiled-in defaults are
        ESP_LOGI(TAG, "Build defaults: AP_SSID=\"%s\"  AP_pass_len=%u  STA_SSID=\"%s\"",
                DEFAULT_AP_SSID,
    #ifdef DEFAULT_AP_PASS
                (unsigned)strlen(DEFAULT_AP_PASS),
    #else
                0u,
    #endif
    #ifdef DEFAULT_STA_SSID
                DEFAULT_STA_SSID
    #else
                ""
    #endif
        );
    #endif

    ESP_LOGI(TAG, "====================================================");

    // ---------- Start Wi-Fi ----------
    ESP_ERROR_CHECK(esp_wifi_start());
    if (do_sta) {
        ESP_ERROR_CHECK(esp_wifi_connect());
        (void)xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, pdFALSE, pdTRUE,
                                  JOIN_TIMEOUT_MS / portTICK_PERIOD_MS);
        ESP_LOGI(TAG, "wifi_init_apsta finished. connect to upstream SSID: %s", sta_ssid);
    } else {
        ESP_LOGI(TAG, "wifi_init_ap finished (AP only).");
    }
}

uint8_t* mac = NULL;
char* ssid = NULL;
char* ent_username = NULL;
char* ent_identity = NULL;
char* passwd = NULL;
char* static_ip = NULL;
char* subnet_mask = NULL;
char* gateway_addr = NULL;
uint8_t* ap_mac = NULL;
char* ap_ssid = NULL;
char* ap_passwd = NULL;
char* ap_ip = NULL;

char* param_set_default(const char* def_val) {
    char * retval = malloc(strlen(def_val)+1);
    strcpy(retval, def_val);
    return retval;
}

void app_main(void)
{
    initialize_nvs();
    seed_config_from_defaults();

#if CONFIG_STORE_HISTORY
    initialize_filesystem();
    ESP_LOGI(TAG, "Command history enabled");
#else
    ESP_LOGI(TAG, "Command history disabled");
#endif

    get_config_param_blob("mac", &mac, 6);
    get_config_param_str("ssid", &ssid);
    if (ssid == NULL) {
        ssid = param_set_default("");
    }
    get_config_param_str("ent_username", &ent_username);
    if (ent_username == NULL) {
        ent_username = param_set_default("");
    }
    get_config_param_str("ent_identity", &ent_identity);
    if (ent_identity == NULL) {
        ent_identity = param_set_default("");
    }
    get_config_param_str("passwd", &passwd);
    if (passwd == NULL) {
        passwd = param_set_default("");
    }
    get_config_param_str("static_ip", &static_ip);
    if (static_ip == NULL) {
        static_ip = param_set_default("");
    }
    get_config_param_str("subnet_mask", &subnet_mask);
    if (subnet_mask == NULL) {
        subnet_mask = param_set_default("");
    }
    get_config_param_str("gateway_addr", &gateway_addr);
    if (gateway_addr == NULL) {
        gateway_addr = param_set_default("");
    }
    get_config_param_blob("ap_mac", &ap_mac, 6);
    get_config_param_str("ap_ssid", &ap_ssid);
    if (ap_ssid == NULL) {
        ap_ssid = param_set_default("ESP32_NAT_Router");
    }   
    get_config_param_str("ap_passwd", &ap_passwd);
    if (ap_passwd == NULL) {
        ap_passwd = param_set_default("");
    }
    get_config_param_str("ap_ip", &ap_ip);
    if (ap_ip == NULL) {
        ap_ip = param_set_default(DEFAULT_AP_IP);
    }

    get_portmap_tab();

    // Setup WIFI
    wifi_init(mac, ssid, ent_username, ent_identity, passwd, static_ip, subnet_mask, gateway_addr, ap_mac, ap_ssid, ap_passwd, ap_ip);

    pthread_t t1;
    pthread_create(&t1, NULL, led_status_thread, NULL);

    ip_napt_enable(my_ap_ip, 1);
    ESP_LOGI(TAG, "NAT is enabled");

    char* lock = NULL;
    get_config_param_str("lock", &lock);
    if (lock == NULL) {
        lock = param_set_default("0");
    }
    if (strcmp(lock, "0") ==0) {
        ESP_LOGI(TAG,"Starting config web server");
        start_webserver();
    }
    free(lock);

    initialize_console();

    /* Register commands */
    esp_console_register_help_command();
    register_system();
    register_nvs();
    register_router();

    /* Prompt to be printed before each line.
     * This can be customized, made dynamic, etc.
     */
    const char* prompt = LOG_COLOR_I "esp32> " LOG_RESET_COLOR;

    printf("\n"
           "ESP32 NAT ROUTER\n"
           "Type 'help' to get the list of commands.\n"
           "Use UP/DOWN arrows to navigate through command history.\n"
           "Press TAB when typing command name to auto-complete.\n");

    if (strlen(ssid) == 0) {
         printf("\n"
               "Unconfigured WiFi\n"
               "Configure using 'set_sta' and 'set_ap' and restart.\n");       
    }

    /* Figure out if the terminal supports escape sequences */
    int probe_status = linenoiseProbe();
    if (probe_status) { /* zero indicates success */
        printf("\n"
               "Your terminal application does not support escape sequences.\n"
               "Line editing and history features are disabled.\n"
               "On Windows, try using Putty instead.\n");
        linenoiseSetDumbMode(1);
#if CONFIG_LOG_COLORS
        /* Since the terminal doesn't support escape sequences,
         * don't use color codes in the prompt.
         */
        prompt = "esp32> ";
#endif //CONFIG_LOG_COLORS
    }

    /* Main loop */
    while(true) {
        /* Get a line using linenoise.
         * The line is returned when ENTER is pressed.
         */
        char* line = linenoise(prompt);
        if (line == NULL) { /* Ignore empty lines */
            continue;
        }
        /* Add the command to the history */
        linenoiseHistoryAdd(line);
#if CONFIG_STORE_HISTORY
        /* Save command history to filesystem */
        linenoiseHistorySave(HISTORY_PATH);
#endif

        /* Try to run the command */
        int ret;
        esp_err_t err = esp_console_run(line, &ret);
        if (err == ESP_ERR_NOT_FOUND) {
            printf("Unrecognized command\n");
        } else if (err == ESP_ERR_INVALID_ARG) {
            // command was empty
        } else if (err == ESP_OK && ret != ESP_OK) {
            printf("Command returned non-zero error code: 0x%x (%s)\n", ret, esp_err_to_name(ret));
        } else if (err != ESP_OK) {
            printf("Internal error: %s\n", esp_err_to_name(err));
        }
        /* linenoise allocates line buffer on the heap, so need to free it */
        linenoiseFree(line);
    }
}
