#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <assert.h>
#include <stdbool.h>

#define PIN_SCK   12
#define PIN_MOSI  11
#define PIN_MISO  13
#define PIN_CS    10
#define PIN_DC    14
#define PIN_RST   15
#define PIN_BL    16

#define LCD_W 240
#define LCD_H 320

// ---------------------------------------------------------------- buttons

#define BTN_LEFT   0
#define BTN_RIGHT  1
#define BTN_COUNT  2

// GPIO 4-9 are free and RTC-capable (needed for ext1 deep-sleep wake in Step 10).
// GPIO 5 is not physically wired yet -- the pull-up makes it read "released".
static const int btn_gpios[BTN_COUNT] = { 4, 5 };

// Measured bounce was ~487 us worst case (release edge). At the ~40 ms frame
// clock a single confirming sample is already generous; 2 is cheap insurance
// against a glitch landing exactly on a sample boundary.
#define DEBOUNCE_SAMPLES 2

typedef struct {
    bool stable;     // debounced state, true = pressed
    bool candidate;  // most recent raw reading
    int  count;      // consecutive samples agreeing with candidate
} btn_state_t;

static btn_state_t btns[BTN_COUNT];

static void buttons_init(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < BTN_COUNT; i++) {
        mask |= (1ULL << btn_gpios[i]);   // 1ULL: pin_bit_mask is uint64_t
    }

    gpio_config_t btn_cfg = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn_cfg));

    for (int i = 0; i < BTN_COUNT; i++) {
        btns[i].stable    = false;
        btns[i].candidate = false;
        btns[i].count     = 0;
    }
}

// Active low: internal pull-up holds the line high, the switch pulls it to GND.
// Kept active-low all the way through because the DMG joypad register (P1/JOYP)
// also reads 0 for pressed -- Phase 2 maps raw pin state on with no inversion.
static void buttons_poll(void)
{
    for (int i = 0; i < BTN_COUNT; i++) {
        bool raw = (gpio_get_level(btn_gpios[i]) == 0);

        if (raw != btns[i].candidate) {
            btns[i].candidate = raw;
            btns[i].count = 1;
        } else if (raw != btns[i].stable) {
            if (++btns[i].count >= DEBOUNCE_SAMPLES) {
                btns[i].stable = raw;
            }
        }
    }
}

static inline bool btn_down(int b) { return btns[b].stable; }

// ---------------------------------------------------------------- display

static SemaphoreHandle_t trans_done_sem;

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx)
{
    BaseType_t hp_task_awoken = pdFALSE;
    xSemaphoreGiveFromISR(trans_done_sem, &hp_task_awoken);
    return hp_task_awoken == pdTRUE;
}

// Colors are stored pre-byte-swapped: the ILI9341 reads big-endian, the S3
// stores little-endian. See NOTES.md -- the swap folds into the Phase 2 palette.
#define COLOR_BG  0x1F00   // blue
#define COLOR_SQ  0xFFFF   // white (swap-invariant)

#define SQ_SIZE   32
#define SQ_SPEED  4        // pixels per frame while held

// ---------------------------------------------------------------- main

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

    // 4. Framebuffer in PSRAM, 64-byte aligned for clean DMA cache writeback
    size_t fb_size = LCD_W * LCD_H * sizeof(uint16_t);   // 153600
    uint16_t *fb = heap_caps_aligned_alloc(64, fb_size, MALLOC_CAP_SPIRAM);
    assert(fb != NULL);
    ESP_LOGI("fb", "framebuffer at %p, %u bytes", fb, (unsigned)fb_size);

    // 5. Input
    buttons_init();

    int sq_x = (LCD_W - SQ_SIZE) / 2;
    int sq_y = (LCD_H - SQ_SIZE) / 2;
    int frame = 0;

    // ------------------------------------------------------------ game loop
    while (1) {
        int64_t t0 = esp_timer_get_time();

        // --- input
        buttons_poll();
        if (btn_down(BTN_LEFT))  sq_x -= SQ_SPEED;
        if (btn_down(BTN_RIGHT)) sq_x += SQ_SPEED;

        if (sq_x < 0)                 sq_x = 0;
        if (sq_x > LCD_W - SQ_SIZE)   sq_x = LCD_W - SQ_SIZE;

        int64_t t_input = esp_timer_get_time();

        // --- render into the framebuffer
        for (int i = 0; i < LCD_W * LCD_H; i++) {
            fb[i] = COLOR_BG;
        }
        for (int y = sq_y; y < sq_y + SQ_SIZE; y++) {
            uint16_t *row = fb + y * LCD_W;
            for (int x = sq_x; x < sq_x + SQ_SIZE; x++) {
                row[x] = COLOR_SQ;
            }
        }

        int64_t t_render = esp_timer_get_time();

        // --- push. Queues the DMA transfer and returns; does not block.
        esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, fb);
        int64_t t_cpu = esp_timer_get_time();

        // Single-buffered: must not touch fb again until DMA has finished
        // reading it. This wait is also what yields the CPU to IDLE.
        xSemaphoreTake(trans_done_sem, portMAX_DELAY);
        int64_t t_done = esp_timer_get_time();

        if (++frame % 30 == 0) {
            ESP_LOGI("perf",
                     "input %lld us | render %lld us | cpu %lld us | frame %lld us",
                     t_input  - t0,
                     t_render - t_input,
                     t_cpu    - t0,
                     t_done   - t0);
        }
    }
}