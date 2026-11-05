#include "net.h"
#include "wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "net";

/* 204 No Content: an empty body, so a logged 204 can only mean DNS, TCP,
   the TLS handshake, and the HTTP exchange all worked. Nothing to parse,
   nothing to buffer -- the narrowest possible test of the transport. */
#define TEST_URL  "https://httpbin.org/status/204"

#define NET_NEVER      -1
#define NET_IN_FLIGHT  -2
#define NET_ERROR      -3

/* mbedTLS wants real stack for a handshake. Undersizing this panics with a
   stack overflow that reads like a TLS bug. */
#define NET_TASK_STACK  8192
#define NET_TASK_PRIO   4

static TaskHandle_t   net_task_handle = NULL;
static volatile int   last_status  = NET_NEVER;
static volatile bool  in_flight    = false;

static void do_request(void)
{
    if (!wifi_is_connected()) {
        ESP_LOGW(TAG, "no ip yet, skipping request");
        last_status = NET_ERROR;
        return;
    }

    esp_http_client_config_t cfg = {
        .url               = TEST_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,   // IDF's root CA set
        .timeout_ms        = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "client init failed");
        last_status = NET_ERROR;
        return;
    }

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(client);
    int64_t elapsed = esp_timer_get_time() - t0;

    if (err == ESP_OK) {
        last_status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "status %d in %lld us", last_status, elapsed);
    } else {
        /* Transport-level failure: DNS, TCP, or TLS. Distinct from an HTTP
           error status, which means the whole chain worked and the server
           simply said no. */
        last_status = NET_ERROR;
        ESP_LOGE(TAG, "perform failed: %s (%lld us)", esp_err_to_name(err), elapsed);
    }

    esp_http_client_cleanup(client);
}

static void net_task(void *arg)
{
    while (1) {
        /* Blocks with zero CPU cost until net_request() notifies. */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        last_status = NET_IN_FLIGHT;
        do_request();
        in_flight = false;
    }
}

void net_start(void)
{
    xTaskCreate(net_task, "net", NET_TASK_STACK, NULL,
                NET_TASK_PRIO, &net_task_handle);
    ESP_LOGI(TAG, "task started");
}

bool net_request(void)
{
    if (in_flight || !net_task_handle)
        return false;

    in_flight = true;
    xTaskNotifyGive(net_task_handle);
    return true;
}

int net_last_status(void)
{
    return last_status;
}