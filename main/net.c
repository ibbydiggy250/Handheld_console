#include "net.h"
#include "wifi.h"
#include "supabase_secret.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "net";

/* 204 No Content: an empty body, so a logged 204 can only mean DNS, TCP,
   the TLS handshake, and the HTTP exchange all worked. Nothing to parse,
   nothing to buffer -- the narrowest possible test of the transport. */
#define TEST_URL  "https://httpbin.org/status/204"

#define NET_NEVER      -1
#define NET_IN_FLIGHT  -2
#define NET_ERROR      -3

/* mbedTLS wants real stack for a handshake. Undersizing this panics with a
   stack overflow that reads like a TLS bug. The high-water mark is logged
   after every Supabase request -- that number decides whether 8 KB holds. */
#define NET_TASK_STACK  8192
#define NET_TASK_PRIO   4

static TaskHandle_t   net_task_handle = NULL;
static volatile int   last_status  = NET_NEVER;
static volatile bool  in_flight    = false;

/* Job flags, set by the game loop, consumed by the net task. A plain task
   notification wakes the task; the flags say why. */
static volatile bool  test_pending  = false;
static volatile bool  pair_pending  = false;
static volatile bool  pair_cancel   = false;
static volatile bool  pairing       = false;

// ---------------------------------------------------------------- shared view

static portMUX_TYPE view_lock = portMUX_INITIALIZER_UNLOCKED;
static pair_view_t  view = { .state = PAIR_IDLE };

static void set_view(pair_state_t st, const char *code, int64_t expires_us)
{
    taskENTER_CRITICAL(&view_lock);
    view.state = st;
    if (code) {
        strncpy(view.code, code, sizeof(view.code) - 1);
        view.code[sizeof(view.code) - 1] = '\0';
    } else {
        view.code[0] = '\0';
    }
    view.expires_us = expires_us;
    taskEXIT_CRITICAL(&view_lock);
}

void net_pair_view(pair_view_t *out)
{
    taskENTER_CRITICAL(&view_lock);
    *out = view;
    taskEXIT_CRITICAL(&view_lock);
}

// ---------------------------------------------------------------- session (RAM only until 3.4d)

/* A Supabase access token is a JWT, measured at ~795 chars in 3.3. Refresh
   tokens are short (~12). Sized with headroom; an oversize token is rejected
   rather than truncated. */
static char access_token[1536];
static char refresh_token[128];
static char user_id[40];

// ---------------------------------------------------------------- HTTP helper

/* Response bodies: the largest is device-poll's approved reply (~1 KB). */
static char resp_buf[3072];
static char auth_hdr[1600];

/* One Supabase request. POST if body != NULL, else GET. bearer may be NULL.
   Fills resp_buf (NUL-terminated). Returns the HTTP status, or NET_ERROR if
   there was no HTTP response (DNS / TCP / TLS) or the body didn't fit. */
static int sb_call(const char *url, const char *body, const char *bearer)
{
    esp_http_client_config_t cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 10000,
        .method            = body ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        /* Defaults are 512. The request header carries an ~800-char bearer
           JWT, which overflows a 512 B tx buffer outright. */
        .buffer_size       = 2048,
        .buffer_size_tx    = 2048,
    };

    resp_buf[0] = '\0';
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NET_ERROR;

    esp_http_client_set_header(c, "apikey", SB_KEY);
    if (body)
        esp_http_client_set_header(c, "Content-Type", "application/json");
    if (bearer) {
        snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", bearer);
        esp_http_client_set_header(c, "Authorization", auth_hdr);
    }

    int status = NET_ERROR;
    int len    = body ? (int)strlen(body) : 0;
    int64_t t0 = esp_timer_get_time();

    if (esp_http_client_open(c, len) != ESP_OK) goto done;
    if (len && esp_http_client_write(c, body, len) != len) goto done;
    if (esp_http_client_fetch_headers(c) < 0) goto done;

    int total = 0;
    while (total < (int)sizeof(resp_buf) - 1) {
        int n = esp_http_client_read(c, resp_buf + total, sizeof(resp_buf) - 1 - total);
        if (n < 0) goto done;
        if (n == 0) break;
        total += n;
    }
    resp_buf[total] = '\0';

    if (total == (int)sizeof(resp_buf) - 1 && !esp_http_client_is_complete_data_received(c)) {
        ESP_LOGE(TAG, "response larger than %u B", (unsigned)sizeof(resp_buf));
        goto done;
    }

    status = esp_http_client_get_status_code(c);

done:
    /* Path only (strip the host) -- and never log bodies: they carry tokens. */
    ESP_LOGI(TAG, "%s %s -> %d in %lld us, stack free %u B",
             body ? "POST" : "GET", url + strlen(SB_URL), status,
             esp_timer_get_time() - t0,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return status;
}

/* Copy a string field out of a parsed object. False if missing, not a
   string, or too long for dst. */
static bool json_str(const cJSON *obj, const char *key, char *dst, size_t cap)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(it) || !it->valuestring) return false;
    size_t n = strlen(it->valuestring);
    if (n >= cap) return false;
    memcpy(dst, it->valuestring, n + 1);
    return true;
}

static int json_int(const cJSON *obj, const char *key, int fallback)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(it) ? it->valueint : fallback;
}

// ---------------------------------------------------------------- pairing

/* Sleep up to ms, waking early on cancel. Returns true if cancelled. */
static bool wait_or_cancel(uint32_t ms)
{
    int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
    while (!pair_cancel) {
        int64_t left_us = until - esp_timer_get_time();
        if (left_us <= 0) return false;
        TickType_t t = pdMS_TO_TICKS(left_us / 1000);
        ulTaskNotifyTake(pdTRUE, t ? t : 1);   // pdMS_TO_TICKS truncates <10 ms to 0
    }
    return true;
}

/* GET /auth/v1/user with the new token: proof the session is real, not
   just a well-formed response. */
static bool verify_session(void)
{
    int st = sb_call(SB_URL "/auth/v1/user", NULL, access_token);
    if (st != 200) return false;

    cJSON *root = cJSON_Parse(resp_buf);
    char id[40] = "", email[96] = "";
    bool ok = root && json_str(root, "id", id, sizeof(id));
    if (root) json_str(root, "email", email, sizeof(email));
    cJSON_Delete(root);

    if (!ok || strcmp(id, user_id) != 0) return false;
    ESP_LOGI(TAG, "paired as %s (%s)", email, id);
    return true;
}

static void run_pairing(void)
{
    static char device_code[96];   // base64url of 32 bytes = 43 chars
    char user_code[8];
    char body[160];

    while (!pair_cancel) {
        if (!wifi_is_connected()) {
            set_view(PAIR_NO_WIFI, NULL, 0);
            if (wait_or_cancel(1000)) break;
            continue;
        }

        // ---- issue
        set_view(PAIR_REQUESTING, NULL, 0);
        int st = sb_call(SB_URL "/functions/v1/device-issue", "{}", NULL);

        cJSON *root = (st == 200) ? cJSON_Parse(resp_buf) : NULL;
        bool ok = root
               && json_str(root, "device_code", device_code, sizeof(device_code))
               && json_str(root, "user_code",   user_code,   sizeof(user_code))
               && strlen(user_code) == 6;
        int expires_in = root ? json_int(root, "expires_in", 600) : 600;
        int interval   = root ? json_int(root, "interval", 5)     : 5;
        cJSON_Delete(root);

        if (!ok) {
            ESP_LOGW(TAG, "device-issue failed (%d)", st);
            set_view(PAIR_ERROR, NULL, 0);
            if (wait_or_cancel(5000)) break;
            continue;
        }

        if (interval < 2) interval = 2;      // never hammer the server
        int64_t expires_us = esp_timer_get_time() + (int64_t)expires_in * 1000000;
        set_view(PAIR_SHOW_CODE, user_code, expires_us);
        ESP_LOGI(TAG, "code %s, expires in %d s, polling every %d s",
                 user_code, expires_in, interval);

        // ---- poll until approved, expired, or cancelled
        snprintf(body, sizeof(body), "{\"device_code\":\"%s\"}", device_code);

        while (!pair_cancel) {
            if (wait_or_cancel((uint32_t)interval * 1000)) break;

            st = sb_call(SB_URL "/functions/v1/device-poll", body, NULL);
            if (st == NET_ERROR) continue;   // network blip: the code is still good

            root = cJSON_Parse(resp_buf);
            char status[16] = "";
            if (root) json_str(root, "status", status, sizeof(status));

            if (strcmp(status, "pending") == 0) {
                cJSON_Delete(root);
                continue;
            }

            if (strcmp(status, "approved") == 0) {
                bool got = json_str(root, "access_token",  access_token,  sizeof(access_token))
                        && json_str(root, "refresh_token", refresh_token, sizeof(refresh_token))
                        && json_str(root, "user_id",       user_id,       sizeof(user_id));
                cJSON_Delete(root);

                if (got && verify_session()) {
                    ESP_LOGI(TAG, "session: access %u chars, refresh %u chars",
                             (unsigned)strlen(access_token), (unsigned)strlen(refresh_token));
                    set_view(PAIR_LINKED, NULL, 0);
                    return;
                }
                ESP_LOGE(TAG, "approved but session invalid");
                access_token[0] = refresh_token[0] = user_id[0] = '\0';
                break;   // the code is spent; get a new one
            }

            /* expired / invalid / mint_failed: this code is dead. Get a fresh
               one -- the web app tells the user the screen will change. */
            ESP_LOGW(TAG, "poll -> %d %s, re-issuing", st, status[0] ? status : "?");
            cJSON_Delete(root);
            break;
        }
    }

    set_view(PAIR_IDLE, NULL, 0);
}

// ---------------------------------------------------------------- test GET (3.2)

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

// ---------------------------------------------------------------- task

static void net_task(void *arg)
{
    while (1) {
        /* Blocks with zero CPU cost until something notifies. */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (pair_pending) {
            pair_pending = false;
            pair_cancel  = false;
            run_pairing();
            pairing = false;
        }

        if (test_pending) {
            test_pending = false;
            last_status = NET_IN_FLIGHT;
            do_request();
            in_flight = false;
        }
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
    if (in_flight || pairing || !net_task_handle)
        return false;

    in_flight = true;
    test_pending = true;
    xTaskNotifyGive(net_task_handle);
    return true;
}

int net_last_status(void)
{
    return last_status;
}

void net_pair_start(void)
{
    if (pairing || !net_task_handle) return;
    pairing = true;
    pair_pending = true;
    xTaskNotifyGive(net_task_handle);
}

void net_pair_cancel(void)
{
    if (!pairing || !net_task_handle) return;
    pair_cancel = true;
    xTaskNotifyGive(net_task_handle);   // wake it out of a poll wait
}
