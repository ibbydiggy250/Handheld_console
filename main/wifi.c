#include "wifi.h"
#include "wifi_secret.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "wifi";

/* Written from the event loop task, read from the game loop task. Single
   machine word, single writer -- volatile is sufficient, no lock needed. */
static volatile bool connected = false;

static int retry_count = 0;

static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        connected = false;

        /* The reason code is the whole diagnostic here. 201 = AP not found
           (SSID typo, or the 2.4 GHz band isn't reachable). 15 = 4-way
           handshake timeout, which is almost always a wrong password.
           Anything else and the band/credentials are fine. */
        ESP_LOGW(TAG, "disconnected, reason %d, retry %d", d->reason, ++retry_count);
        esp_wifi_connect();
        return;
    }
}

static void on_got_ip(void *arg, esp_event_base_t base,
                      int32_t id, void *data)
{
    ip_event_got_ip_t *e = data;
    retry_count = 0;
    connected = true;
    ESP_LOGI(TAG, "connected, ip " IPSTR, IP2STR(&e->ip_info.ip));
}

void wifi_start(void)
{
    /* WiFi calibration data lives in NVS whether or not we store anything
       there ourselves, so this init is not optional. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip, NULL, NULL));

    wifi_config_t sta_cfg = {
        .sta = {
            /* WPA2 minimum. Google Wifi runs WPA2/WPA3 mixed; leaving the
               IDF default (OPEN) would let the driver associate to a rogue
               open AP with the same SSID. */
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)sta_cfg.sta.ssid,     WIFI_SSID, sizeof(sta_cfg.sta.ssid));
    strncpy((char *)sta_cfg.sta.password, WIFI_PASS, sizeof(sta_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Returns here with the radio scanning. STA_START fires on the event
       task and calls esp_wifi_connect(); the game loop starts immediately. */
    ESP_LOGI(TAG, "started, connecting to %s", WIFI_SSID);
}

bool wifi_is_connected(void)
{
    return connected;
}