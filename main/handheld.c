#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include <assert.h>

#define PIN_SCK   12
#define PIN_MOSI  11
#define PIN_MISO  13
#define PIN_CS    10
#define PIN_DC    14
#define PIN_RST   15
#define PIN_BL    16
#define BTN_A_GPIO 4

#define LCD_W 240
#define LCD_H 320

static SemaphoreHandle_t trans_done_sem;

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx)
{
    BaseType_t hp_task_awoken = pdFALSE;
    xSemaphoreGiveFromISR(trans_done_sem, &hp_task_awoken);
    return hp_task_awoken == pdTRUE;
}
static void buttons_init(void)
{
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BTN_A_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn_cfg));
}
void app_main(void)
{
    // Backlight on
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_cfg));
    ESP_ERROR_CHECK(gpio_set_level(PIN_BL, 1));

    // 1. SPI bus
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = PIN_SCK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * LCD_H * sizeof(uint16_t),   // 153600
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    // 2. Panel IO
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_DC,
        .cs_gpio_num = PIN_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &io));
    trans_done_sem = xSemaphoreCreateBinary();
    assert(trans_done_sem != NULL);

    esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = on_color_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io, &cbs, NULL));

    // 3. Panel
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &panel_cfg, &panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    // Framebuffer in PSRAM
    size_t fb_size = LCD_W * LCD_H * sizeof(uint16_t);   // 153600
    uint16_t *fb = heap_caps_aligned_alloc(64, fb_size, MALLOC_CAP_SPIRAM);
    assert(fb != NULL);
    ESP_LOGI("fb", "framebuffer at %p, %u bytes", fb, (unsigned)fb_size);

    buttons_init();
    while (1) {
        ESP_LOGI("btn", "level=%d", gpio_get_level(BTN_A_GPIO));
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Fill screen, one 240px line at a time
    uint16_t colors[] = { 0x00F8, 0xE007, 0x1F00 };  // red, green, blue

    while (1) {
        for (int c = 0; c < 3; c++) {
            for (int i = 0; i < LCD_W * LCD_H; i++) fb[i] = colors[c];

            int64_t t0 = esp_timer_get_time();
            esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, fb);
            int64_t t_cpu = esp_timer_get_time();

            xSemaphoreTake(trans_done_sem, portMAX_DELAY);
            int64_t t_done = esp_timer_get_time();

            ESP_LOGI("perf", "cpu busy: %lld us | transfer done: %lld us",
                     t_cpu - t0, t_done - t0);

            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}