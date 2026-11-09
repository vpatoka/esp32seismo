/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Vlad Patoka (VE3VPT) */
/**
 * @file vseismo.c
 * @brief Two-channel geophone digitizer on ESP32-C5: boot, WiFi, LED, button, OTA self-test.
 *
 * Chain: L-15B geophones -> Webtronics EQAMP -> divider -> ADC 15 Click
 * (ADS131M02, simultaneous sampling) -> ESP32-C5 -> web tuning page now,
 * WinSDR over the DevKit's UART port later.
 *
 * WiFi policy (webcontrol / esp32nixie style):
 *   - No credentials: open provisioning AP, captive portal, nothing else.
 *   - Credentials: station mode, 2.4 + 5 GHz, retry forever with backoff.
 *     Never falls back to AP on its own. The BOOT button re-provisions:
 *       hold 3 s  reboot
 *       hold 6 s  forget WiFi, reboot into the portal
 *       hold 10 s factory reset (all NVS)
 *   - Modem power save is off. Power-save wakeups follow the AP beacon
 *     (102.4 ms, a 9.77 Hz line right in the seismic band) and modulate the
 *     supply; a constant RF load is easier to filter than a periodic one.
 *
 * Status LED (on-board RGB, GPIO27), highest priority first:
 *   button held      white countdown blink, then solid on the chosen tier
 *   OTA writing      magenta fast blink
 *   Class-A fault    solid red
 *   provisioning     blue 5 Hz
 *   ADC missing      red double blip
 *   WiFi down        amber 2.5 Hz
 *   healthy, NTP     green double blip at 1 Hz
 *   healthy, no NTP  cyan double blip
 *
 * OTA self-test: a fresh image boots in PENDING_VERIFY and is marked valid
 * once the station has an IP, the web server is up and no Class-A fault is
 * latched. Miss the deadline, or crash before it, and the bootloader goes
 * back to the previous slot.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/rmt_tx.h"
#include "sdkconfig.h"

#include "acq.h"
#include "captive_dns.h"
#include "console.h"
#include "event_log.h"
#include "fault.h"
#include "io_map.h"
#include "timebase.h"
#include "web_server.h"

static const char *TAG = "SEISMO";

volatile seismo_wifi_mode_t g_wifi_mode = SEISMO_WIFI_NONE;
volatile bool g_wifi_connected = false;
char g_ip_address[16] = "0.0.0.0";

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_dhcp_timer;
static uint32_t s_retry_ms = WIFI_RETRY_INITIAL_MS;

static TaskHandle_t s_button_task;
static volatile bool s_led_override;

static bool s_ota_pending;
static esp_timer_handle_t s_ota_deadline;

/* ═════════════════════════════════════════════════════════════════════════
 * Status LED: one addressable RGB pixel through RMT
 * ═════════════════════════════════════════════════════════════════════════ */

static rmt_channel_handle_t s_led_chan;
static rmt_encoder_handle_t s_led_enc;
static uint32_t s_led_rgb = 0xFFFFFFFF;

#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define LB STATUS_LED_BRIGHTNESS
#define C_OFF     RGB(0, 0, 0)
#define C_RED     RGB(LB, 0, 0)
#define C_GREEN   RGB(0, LB, 0)
#define C_BLUE    RGB(0, 0, LB)
#define C_AMBER   RGB(LB, LB / 3, 0)
#define C_CYAN    RGB(0, LB / 2, LB / 2)
#define C_MAGENTA RGB(LB, 0, LB)
#define C_WHITE   RGB(LB / 2, LB / 2, LB / 2)

static esp_err_t led_init(void)
{
    const rmt_tx_channel_config_t ch = {
        .gpio_num = STATUS_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 48,
        .trans_queue_depth = 2,
    };
    esp_err_t err = rmt_new_tx_channel(&ch, &s_led_chan);
    if (err != ESP_OK) return err;
    const rmt_bytes_encoder_config_t enc = {
        .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },
        .bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },
        .flags.msb_first = 1,
    };
    err = rmt_new_bytes_encoder(&enc, &s_led_enc);
    if (err != ESP_OK) return err;
    return rmt_enable(s_led_chan);
}

static void led_set(uint32_t rgb)
{
    if (!s_led_chan || rgb == s_led_rgb) return;
    s_led_rgb = rgb;
    static uint8_t grb[3];
    grb[0] = (uint8_t)(rgb >> 8);
    grb[1] = (uint8_t)(rgb >> 16);
    grb[2] = (uint8_t)rgb;
    const rmt_transmit_config_t tx = { .loop_count = 0 };
    rmt_transmit(s_led_chan, s_led_enc, grb, sizeof(grb), &tx);
}

static void led_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    uint32_t tick = 0;                         /* 50 ms ticks */
    for (;;) {
        if (!s_led_override) {
            uint32_t p20 = tick % 20;
            bool blip = (p20 == 0 || p20 == 1 || p20 == 4 || p20 == 5);
            tb_status_t tb;
            timebase_get_status(&tb);
            uint32_t c;
            if (webserver_ota_active())                          c = (tick & 1) ? C_MAGENTA : C_OFF;
            else if (fault_active())                             c = C_RED;
            else if (g_wifi_mode == SEISMO_WIFI_AP_PROVISIONING) c = ((tick / 2) & 1) ? C_BLUE : C_OFF;
            else if (fault_get_mask() & FAULT_ADC_MISSING)       c = blip ? C_RED : C_OFF;
            else if (!g_wifi_connected)                          c = ((tick / 4) & 1) ? C_AMBER : C_OFF;
            else if (tb.ntp_syncs > 0)                           c = blip ? C_GREEN : C_OFF;
            else                                                 c = blip ? C_CYAN : C_OFF;
            led_set(c);
        }
        tick++;
        xTaskDelayUntil(&last, pdMS_TO_TICKS(50));
    }
}

/* ═════════════════════════════════════════════════════════════════════════
 * BOOT button
 * ═════════════════════════════════════════════════════════════════════════ */

static void IRAM_ATTR button_isr(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    if (s_button_task) vTaskNotifyGiveFromISR(s_button_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static void button_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (gpio_get_level(BUTTON_GPIO) != 0) continue;

        ESP_LOGI(TAG, "Button held: 3 s reboot, 6 s WiFi reset, 10 s factory reset");
        s_led_override = true;
        int64_t start = esp_timer_get_time();
        TickType_t last = xTaskGetTickCount();
        uint32_t held = 0;
        int tier = 0;
        while (gpio_get_level(BUTTON_GPIO) == 0) {
            held = (uint32_t)((esp_timer_get_time() - start) / 1000);
            int t = held >= BUTTON_HOLD_FACTORY_MS ? 3 : held >= BUTTON_HOLD_WIFI_MS ? 2 :
                    held >= BUTTON_HOLD_REBOOT_MS ? 1 : 0;
            if (t != tier) {
                tier = t;
                static const char *const names[] = { "", "REBOOT", "WiFi RESET", "FACTORY RESET" };
                ESP_LOGW(TAG, ">>> %s on release <<<", names[tier]);
            }
            static const uint32_t col[] = { C_WHITE, C_GREEN, C_AMBER, C_RED };
            uint32_t period = tier == 3 ? 0 : (tier ? 100 : 200);
            led_set(period == 0 || ((held / period) & 1) ? col[tier] : C_OFF);
            xTaskDelayUntil(&last, pdMS_TO_TICKS(BUTTON_SAMPLE_MS));
        }
        s_led_override = false;
        switch (tier) {
            case 3: config_factory_reset(); break;
            case 2: config_wifi_reset();    break;
            case 1:
                ESP_LOGW(TAG, "Reboot from button");
                seismo_schedule_restart(100);
                break;
            default:
                ESP_LOGI(TAG, "Button released after %lu ms: no action", (unsigned long)held);
                break;
        }
        ulTaskNotifyTake(pdTRUE, 0);
    }
}

static void button_init(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);
    if (xTaskCreate(button_task, "button", 3072, NULL, PRIO_BUTTON, &s_button_task) != pdPASS) {
        fault_raise(FAULT_INIT_TASK);
        return;
    }
    gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL);
}

/* ═════════════════════════════════════════════════════════════════════════
 * OTA self-test
 * ═════════════════════════════════════════════════════════════════════════ */

static void ota_deadline_cb(void *arg)
{
    (void)arg;
    ESP_LOGE(TAG, "OTA self-test not passed within %d s: rolling back", CONFIG_SEISMO_OTA_VERIFY_S);
    fault_safe_outputs();
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

static void ota_selftest_begin(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) return;
    s_ota_pending = true;
    const esp_timer_create_args_t a = { .callback = ota_deadline_cb, .name = "ota_verify" };
    if (esp_timer_create(&a, &s_ota_deadline) == ESP_OK) {
        esp_timer_start_once(s_ota_deadline, (uint64_t)CONFIG_SEISMO_OTA_VERIFY_S * 1000000);
    }
    ESP_LOGW(TAG, "New firmware on probation: self-test deadline %d s", CONFIG_SEISMO_OTA_VERIFY_S);
}

static void ota_selftest_try_pass(void)
{
    if (!s_ota_pending) return;
    bool net = g_wifi_connected || g_wifi_mode == SEISMO_WIFI_AP_PROVISIONING;
    if (!net || !webserver_running() || fault_active()) return;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        s_ota_pending = false;
        if (s_ota_deadline) esp_timer_stop(s_ota_deadline);
        ESP_LOGI(TAG, "OTA self-test passed: firmware marked valid");
    }
}

/* ═════════════════════════════════════════════════════════════════════════
 * WiFi
 * ═════════════════════════════════════════════════════════════════════════ */

static void print_network_banner(void)
{
    esp_netif_ip_info_t ip = { 0 };
    if (s_sta_netif) esp_netif_get_ip_info(s_sta_netif, &ip);
    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_STA, mac);

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "  NETWORK SETTINGS");
    ESP_LOGI(TAG, "--------------------------------------------------");
    wifi_ap_record_t ap;
    if (g_wifi_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        ESP_LOGI(TAG, "  WiFi      : %s (RSSI %d dBm, channel %u, %s GHz)",
                 g_config.wifi_ssid, ap.rssi, (unsigned)ap.primary, ap.primary > 14 ? "5" : "2.4");
        ESP_LOGI(TAG, "  AP BSSID  : " MACSTR, MAC2STR(ap.bssid));
    } else {
        ESP_LOGI(TAG, "  WiFi      : %s (connecting)", g_config.wifi_ssid);
    }
    ESP_LOGI(TAG, "  Radio MAC : " MACSTR, MAC2STR(mac));
    ESP_LOGI(TAG, "  Hostname  : %s", g_config.hostname);
    if (ip.ip.addr) {
        ESP_LOGI(TAG, "  IP address: " IPSTR "  gw " IPSTR, IP2STR(&ip.ip), IP2STR(&ip.gw));
        if (g_config.http_port == 80) ESP_LOGI(TAG, "  Open      : http://" IPSTR, IP2STR(&ip.ip));
        else ESP_LOGI(TAG, "  Open      : http://" IPSTR ":%u", IP2STR(&ip.ip), g_config.http_port);
    }
    ESP_LOGI(TAG, "  Web login : %s", g_config.weblogin_enabled ? "enabled" : "disabled (open)");
    ESP_LOGI(TAG, "  Heap free : %lu KB", (unsigned long)(esp_get_free_heap_size() / 1024));
    ESP_LOGI(TAG, "==================================================");
}

static void reconnect_cb(void *arg)
{
    (void)arg;
    if (g_wifi_mode == SEISMO_WIFI_STA && !g_wifi_connected) {
        ESP_LOGI(TAG, "WiFi reconnect attempt");
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "WiFi connect: %s, retry in %lu s",
                     esp_err_to_name(err), (unsigned long)(s_retry_ms / 1000));
            esp_timer_start_once(s_reconnect_timer, (uint64_t)s_retry_ms * 1000);
            s_retry_ms = s_retry_ms * 2 > WIFI_RETRY_MAX_MS ? WIFI_RETRY_MAX_MS : s_retry_ms * 2;
        }
    }
}

static void dhcp_timeout_cb(void *arg)
{
    (void)arg;
    if (g_wifi_mode == SEISMO_WIFI_STA && !g_wifi_connected) {
        ESP_LOGW(TAG, "No DHCP lease in %d s, dropping association", WIFI_DHCP_TIMEOUT_MS / 1000);
        esp_wifi_disconnect();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                if (g_wifi_mode == SEISMO_WIFI_STA) {
                    esp_err_t err = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
                    if (err != ESP_OK) ESP_LOGW(TAG, "Band mode auto: %s", esp_err_to_name(err));
                    esp_wifi_connect();
                }
                break;
            case WIFI_EVENT_STA_CONNECTED:
                if (g_wifi_mode != SEISMO_WIFI_STA) break;
                esp_timer_stop(s_dhcp_timer);
                esp_timer_start_once(s_dhcp_timer, (uint64_t)WIFI_DHCP_TIMEOUT_MS * 1000);
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                if (g_wifi_mode != SEISMO_WIFI_STA) break;
                const wifi_event_sta_disconnected_t *e = data;
                g_wifi_connected = false;
                esp_timer_stop(s_dhcp_timer);
                ESP_LOGW(TAG, "WiFi disconnected (reason %d), retry in %lu s",
                         e->reason, (unsigned long)(s_retry_ms / 1000));
                esp_timer_stop(s_reconnect_timer);
                esp_timer_start_once(s_reconnect_timer, (uint64_t)s_retry_ms * 1000);
                s_retry_ms = s_retry_ms * 2 > WIFI_RETRY_MAX_MS ? WIFI_RETRY_MAX_MS : s_retry_ms * 2;
                break;
            }
            case WIFI_EVENT_AP_STACONNECTED: {
                const wifi_event_ap_staconnected_t *e = data;
                ESP_LOGI(TAG, "Client " MACSTR " joined the setup AP", MAC2STR(e->mac));
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED: {
                const wifi_event_ap_stadisconnected_t *e = data;
                ESP_LOGI(TAG, "Client " MACSTR " left the setup AP", MAC2STR(e->mac));
                break;
            }
            default:
                break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        snprintf(g_ip_address, sizeof(g_ip_address), IPSTR, IP2STR(&e->ip_info.ip));
        g_wifi_connected = true;
        if (s_dhcp_timer) esp_timer_stop(s_dhcp_timer);
        s_retry_ms = WIFI_RETRY_INITIAL_MS;
        print_network_banner();
        timebase_start_sntp();
        ota_selftest_try_pass();
    } else if (base == IP_EVENT && id == IP_EVENT_GOT_IP6) {
        const ip_event_got_ip6_t *e = data;
        if (e->esp_netif != s_sta_netif) return;
        esp_ip6_addr_type_t type = esp_netif_ip6_get_addr_type((esp_ip6_addr_t *)&e->ip6_info.ip);
        const char *kind = type == ESP_IP6_ADDR_IS_GLOBAL       ? "global"
                         : type == ESP_IP6_ADDR_IS_LINK_LOCAL   ? "link-local"
                         : type == ESP_IP6_ADDR_IS_UNIQUE_LOCAL ? "unique-local" : "other";
        ESP_LOGI(TAG, "IPv6 %s address: " IPV6STR, kind, IPV62STR(e->ip6_info.ip));
        if (type != ESP_IP6_ADDR_IS_GLOBAL || g_wifi_connected) return;
        g_wifi_connected = true;
        if (s_dhcp_timer) esp_timer_stop(s_dhcp_timer);
        s_retry_ms = WIFI_RETRY_INITIAL_MS;
        timebase_start_sntp();
        ota_selftest_try_pass();
    }
}

static void sta_linklocal_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    if (g_wifi_mode != SEISMO_WIFI_STA) return;
    if (esp_netif_create_ip6_linklocal(s_sta_netif) != ESP_OK) ESP_LOGW(TAG, "IPv6 link-local setup failed");
}

static void wifi_start_sta(void)
{
    g_wifi_mode = SEISMO_WIFI_STA;
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta_netif, g_config.hostname);
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED,
                                                        sta_linklocal_handler, NULL, NULL));
    const esp_timer_create_args_t a = { .callback = reconnect_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&a, &s_reconnect_timer));
    const esp_timer_create_args_t d = { .callback = dhcp_timeout_cb, .name = "wifi_dhcp" };
    ESP_ERROR_CHECK(esp_timer_create(&d, &s_dhcp_timer));

    /* Full scan, strongest AP for the SSID: on a dual-band AP under one name
     * the 5 GHz radio usually wins, and 5 GHz is the quieter band for a
     * station sitting next to high-gain analog electronics. */
    wifi_config_t wc = {
        .sta = {
            .threshold.authmode = g_config.wifi_password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
        },
    };
    memcpy(wc.sta.ssid, g_config.wifi_ssid, strnlen(g_config.wifi_ssid, sizeof(wc.sta.ssid)));
    memcpy(wc.sta.password, g_config.wifi_password, strnlen(g_config.wifi_password, sizeof(wc.sta.password)));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "WiFi station: SSID \"%s\", 2.4 + 5 GHz, retrying until connected", g_config.wifi_ssid);
}

static void wifi_start_provisioning(void)
{
    g_wifi_mode = SEISMO_WIFI_AP_PROVISIONING;
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[33];
    snprintf(ssid, sizeof(ssid), "%s%02X%02X", CONFIG_SEISMO_AP_SSID_PREFIX, mac[4], mac[5]);

    wifi_config_t wc = {
        .ap = {
            .channel = WIFI_AP_CHANNEL,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    size_t sl = strlen(ssid);
    memcpy(wc.ap.ssid, ssid, sl);
    wc.ap.ssid_len = (uint8_t)sl;

    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_ip_info_t ip = { 0 };
    esp_netif_str_to_ip4(WIFI_AP_IP, &ip.ip);
    esp_netif_str_to_ip4(WIFI_AP_GATEWAY, &ip.gw);
    esp_netif_str_to_ip4(WIFI_AP_NETMASK, &ip.netmask);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ip));

    esp_netif_dns_info_t dns = { 0 };
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4 = ip.ip;
    esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    uint8_t offer_dns = 1;
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                           &offer_dns, sizeof(offer_dns));
    static const char portal_uri[] = "http://" WIFI_AP_IP "/portal";
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                           (void *)portal_uri, strlen(portal_uri));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap_netif));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* The portal's scan sees 5 GHz networks too; the AP itself stays on
     * 2.4 GHz channel 1 where every client can find it. */
    esp_err_t err = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    if (err != ESP_OK) ESP_LOGW(TAG, "Band mode auto: %s", esp_err_to_name(err));

    snprintf(g_ip_address, sizeof(g_ip_address), "%s", WIFI_AP_IP);
    if (captive_dns_start(WIFI_AP_IP) != ESP_OK) ESP_LOGE(TAG, "Captive DNS failed to start");

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "  WiFi setup: join the open network \"%s\"", ssid);
    ESP_LOGI(TAG, "  The setup page should open by itself. If not:");
    ESP_LOGI(TAG, "  http://%s/portal", WIFI_AP_IP);
    ESP_LOGI(TAG, "==================================================");
}

/* ═════════════════════════════════════════════════════════════════════════
 * Boot
 * ═════════════════════════════════════════════════════════════════════════ */

static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:  return "power-on";
        case ESP_RST_EXT:      return "external pin";
        case ESP_RST_SW:       return "software";
        case ESP_RST_PANIC:    return "panic";
        case ESP_RST_INT_WDT:  return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT:      return "other watchdog";
        case ESP_RST_DEEPSLEEP:return "deep sleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_USB:      return "USB";
        case ESP_RST_JTAG:     return "JTAG";
        default:               return "unknown";
    }
}

static i2c_master_bus_handle_t i2c_bus_init(void)
{
    const i2c_master_bus_config_t cfg = {
        .i2c_port = -1,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init: %s", esp_err_to_name(err));
        fault_raise(FAULT_INIT_I2C);
        return NULL;
    }
    return bus;
}

void app_main(void)
{
    fault_init();
    event_log_init();

    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    esp_reset_reason_t rr = esp_reset_reason();

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "  VSEISMO  vseismo v%s  (%s %s)", app->version, app->date, app->time);
    ESP_LOGI(TAG, "  ESP32-C5 rev v%d.%d, %d core, IDF %s",
             chip.revision / 100, chip.revision % 100, chip.cores, app->idf_ver);
    ESP_LOGI(TAG, "  Reset reason: %s", reset_reason_name(rr));
    ESP_LOGI(TAG, "  VE3VPT");
    ESP_LOGI(TAG, "==================================================");

    if (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT) {
        fault_raise(FAULT_ABNORMAL_RESET);
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    config_load();

    /* One GPIO ISR service for the whole app (DRDY, 1 Hz ref, BOOT button). */
    err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK) ESP_LOGE(TAG, "GPIO ISR service: %s", esp_err_to_name(err));

    /* Acquisition first: it does not need the network, and the ring starts
     * filling while WiFi comes up. */
    acq_settings_t as;
    config_to_acq(&g_config, &as);
    if (acq_start(&as) != ESP_OK) fault_raise(FAULT_INIT_TASK);

    i2c_master_bus_handle_t bus = g_config.rtc ? i2c_bus_init() : NULL;
    if (timebase_init((tb_ref_t)g_config.ref, g_config.ref_rising, g_config.rtc, bus) != ESP_OK) {
        fault_raise(FAULT_INIT_TASK);
    }

    ota_selftest_begin();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_GOT_IP6, wifi_event_handler, NULL, NULL));
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    if (led_init() != ESP_OK) ESP_LOGW(TAG, "Status LED unavailable");
    if (xTaskCreate(led_task, "status_led", 2560, NULL, PRIO_STATUS_LED, NULL) != pdPASS) {
        fault_raise(FAULT_INIT_TASK);
    }
    button_init();

    if (console_start() != ESP_OK) ESP_LOGW(TAG, "USB console unavailable (acquisition keeps running)");

    if (g_config.wifi_configured && g_config.wifi_ssid[0]) {
        wifi_start_sta();
        if (webserver_start() != ESP_OK) ESP_LOGE(TAG, "Web server failed (acquisition keeps running)");
    } else {
        ESP_LOGI(TAG, "No WiFi credentials: starting setup AP");
        wifi_start_provisioning();
        if (webserver_start_portal_mode() != ESP_OK) ESP_LOGE(TAG, "Portal web server failed");
        ota_selftest_try_pass();
    }
}
