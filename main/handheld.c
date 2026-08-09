#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "chip8.h"
#include "rom_pong.h"

#define PIN_SCK   12
#define PIN_MOSI  11
#define PIN_MISO  13
#define PIN_CS    10
#define PIN_DC    14
#define PIN_RST   15
#define PIN_BL    16

// LANDSCAPE. Panel is natively 240x320 portrait; swap_xy + mirror rotate it,
// so the framebuffer is addressed 320 wide x 240 tall. Same 153600 bytes.
#define LCD_W 320
#define LCD_H 240

#define LCD_MIRROR_X  false
#define LCD_MIRROR_Y  false

// Fixed frame clock. 50 ms = 20 fps -- Step 4's measured worst case.
#define FRAME_US  50000

// ---------------------------------------------------------------- chip-8

// Emulated rates, stated as RATES rather than per-frame counts so they stay
// correct if FRAME_US changes (double buffering will change it). The laptop
// frontend uses these same two constants against a 60 fps frame period and
// derives different counts from them -- that's the point of the form.
//
//   CHIP8_HZ 600 matches the laptop build (10 cycles @ 60 fps). It is not a
//   spec value -- CHIP-8 has no defined clock -- but it is the speed Pong and
//   Tetris were played at during Phase 0, so it's the known-good reference.
#define CHIP8_HZ  600
#define TIMER_HZ  60

#define CYCLES_PER_FRAME       ((CHIP8_HZ * FRAME_US) / 1000000)  // 30
#define TIMER_TICKS_PER_FRAME  ((TIMER_HZ * FRAME_US) / 1000000)  // 3

// 5x integer scale: 64*5 = 320 (full panel width), 32*5 = 160.
// 240 - 160 = 80 rows of letterbox, 40 top and 40 bottom.
#define SCALE     5
#define IMG_W     (CHIP8_DISPLAY_W * SCALE)   // 320
#define IMG_H     (CHIP8_DISPLAY_H * SCALE)   // 160
#define IMG_X0    ((LCD_W - IMG_W) / 2)       // 0
#define IMG_Y0    ((LCD_H - IMG_H) / 2)       // 40

// CHIP-8 is monochrome. Both of these are byte-swap invariant, so the
// pre-swapped framebuffer convention costs nothing here.
#define COLOR_FG      0xFFFF   // white
#define COLOR_BG      0x0000   // black
#define COLOR_LETTER  0x0000   // letterbox bars

// ~6.2 KB. Static, so it lands in internal RAM (.bss) rather than the stack
// or PSRAM -- mem[] is touched twice per cycle and PSRAM cache misses would
// show up directly in the per-frame cost.
static Chip8 chip;

// Which CHIP-8 hex key each physical button presses.
//
// Classic PONG.ch8 uses 1/4 for the left paddle and C/D for the right. This
// mapping is a convention of the ROM, not of CHIP-8 -- every ROM picks its
// own keys. If a button does nothing, this table is the only thing to edit.
static const uint8_t btn_to_key[] = {
    0xC,   // BTN_LEFT  -> right paddle up
    0xD,   // BTN_RIGHT -> right paddle down
    0x1,   // BTN_UP    -> left paddle up
    0x4,   // BTN_DOWN  -> left paddle down
};

// ---------------------------------------------------------------- audio

#define PIN_I2S_BCLK  GPIO_NUM_42
#define PIN_I2S_WS    GPIO_NUM_41
#define PIN_I2S_DOUT  GPIO_NUM_40

#define SAMPLE_RATE   22050
#define BEEP_HZ       880
#define BEEP_AMP      8000
#define BEEP_MS       80


#define AUDIO_CHUNK   1536
#define BEEP_HALF     (SAMPLE_RATE / (BEEP_HZ * 2))

static i2s_chan_handle_t audio_tx = NULL;

static int      beep_remaining = 0;   // samples of tone left to play; 0 = silent
static uint32_t beep_phase     = 0;   // persists across frames -- no seam click
static int16_t  audio_buf[AUDIO_CHUNK];

static void audio_init(void)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    // Without this an underrun replays the stale DMA buffer -- a loud
    // repeating fragment instead of silence. With it, "don't write" ==
    // silence, so the channel can stay enabled for the whole program.
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &audio_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCLK,
            .ws   = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { false, false, false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(audio_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(audio_tx));
}


static void audio_beep_ms(int ms)
{
    beep_remaining = SAMPLE_RATE * ms / 1000;
}

// Once per frame, non-blocking. Returns bytes the DMA buffer accepted --
// the underrun gauge.
static size_t audio_feed(void)
{
    if (beep_remaining <= 0) {
        return 0;
    }

    int n = beep_remaining < AUDIO_CHUNK ? beep_remaining : AUDIO_CHUNK;

    uint32_t ph = beep_phase;
    for (int i = 0; i < n; i++) {
        audio_buf[i] = (ph < BEEP_HALF) ? BEEP_AMP : -BEEP_AMP;
        if (++ph >= BEEP_HALF * 2) ph = 0;
    }

    size_t written = 0;
    i2s_channel_write(audio_tx, audio_buf, n * sizeof(int16_t), &written, 0);

    int samples_written = written / sizeof(int16_t);

    // Advance the persistent phase by ONLY what was accepted, so unsent
    // samples regenerate identically next frame -- continuous waveform.
    beep_phase = (beep_phase + samples_written) % (BEEP_HALF * 2);
    beep_remaining -= samples_written;

    return written;
}

// ---------------------------------------------------------------- buttons

#define BTN_LEFT   0
#define BTN_RIGHT  1
#define BTN_UP     2
#define BTN_DOWN   3
#define BTN_COUNT  4

static const int btn_gpios[BTN_COUNT] = { 4, 5, 6, 7 };

#define DEBOUNCE_SAMPLES 2

typedef struct {
    bool stable;
    bool candidate;
    int  count;
    bool went_down;
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
        btns[i].went_down = false;
    }
}

// Active low: pull-up holds high, switch pulls to GND.
static void buttons_poll(void)
{
    for (int i = 0; i < BTN_COUNT; i++) {
        btns[i].went_down = false;
        bool raw = (gpio_get_level(btn_gpios[i]) == 0);

        if (raw != btns[i].candidate) {
            btns[i].candidate = raw;
            btns[i].count = 1;
        } else if (raw != btns[i].stable) {
            if (++btns[i].count >= DEBOUNCE_SAMPLES) {
                bool was = btns[i].stable;
                btns[i].stable = raw;
                if (!was && raw) btns[i].went_down = true;
            }
        }
    }
}

static inline bool btn_down(int b)      { return btns[b].stable; }
static inline bool btn_went_down(int b) { return btns[b].went_down; }

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

// ---------------------------------------------------------------- glue
//
// This is the whole port. update() and render() keep the exact signatures
// Step 8 designed them with; the CHIP-8 core dropped into the socket and
// nothing around it moved.

// Frontend -> core: physical buttons become hex keypad state.
static void input_to_keys(void)
{
    for (int i = 0; i < 16; i++)
        chip.keys[i] = false;

    for (int b = 0; b < BTN_COUNT; b++)
        if (btn_down(b))
            chip.keys[btn_to_key[b]] = true;
}

static void update(void)
{
    input_to_keys();

    for (int i = 0; i < CYCLES_PER_FRAME; i++)
        chip8_cycle(&chip);

    for (int i = 0; i < TIMER_TICKS_PER_FRAME; i++)
        chip8_tick_timers(&chip);

    // Core -> frontend: sound timer nonzero means "make noise". Re-armed
    // every frame from the CURRENT timer value, so the beep always ends
    // exactly when the timer does. Re-arming is safe because beep_phase
    // persists -- the waveform doesn't restart, so there's no click.
    if (chip.sound_timer > 0)
        audio_beep_ms(chip.sound_timer * 1000 / TIMER_HZ);
}

// 64x32 -> 320x160, nearest-neighbour at 5x. Only the image band is written;
// the letterbox bars are painted once before the loop and never change.
//
// Each source row expands to 5 identical destination rows, so build one and
// memcpy it 4 times rather than running the inner loop 5 times. 51200 pixel
// writes plus 4 row copies, against the 76800 a full-frame clear would cost.
static void render(uint16_t *fb)
{
    for (int cy = 0; cy < CHIP8_DISPLAY_H; cy++) {
        uint16_t      *row0 = fb + (IMG_Y0 + cy * SCALE) * LCD_W + IMG_X0;
        const uint8_t *src  = chip.display + cy * CHIP8_DISPLAY_W;

        for (int cx = 0; cx < CHIP8_DISPLAY_W; cx++) {
            uint16_t  col = src[cx] ? COLOR_FG : COLOR_BG;
            uint16_t *p   = row0 + cx * SCALE;
            for (int k = 0; k < SCALE; k++)
                p[k] = col;
        }

        for (int r = 1; r < SCALE; r++)
            memcpy(row0 + r * LCD_W, row0, IMG_W * sizeof(uint16_t));
    }
}

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

    // Rotate to landscape. MUST come after panel_init() -- init writes MADCTL
    // as part of the datasheet startup sequence and would overwrite these.
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, LCD_MIRROR_X, LCD_MIRROR_Y));

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    // 4. Framebuffer in PSRAM, 64-byte aligned for clean DMA cache writeback
    size_t fb_size = LCD_W * LCD_H * sizeof(uint16_t);
    uint16_t *fb = heap_caps_aligned_alloc(64, fb_size, MALLOC_CAP_SPIRAM);
    assert(fb != NULL);
    ESP_LOGI("fb", "framebuffer at %p, %u bytes, %dx%d",
             fb, (unsigned)fb_size, LCD_W, LCD_H);

    // Letterbox bars: painted once. render() never touches these rows again,
    // which is why the per-frame render cost is below Snake's despite the
    // scaler doing more work per pixel.
    for (int i = 0; i < LCD_W * LCD_H; i++)
        fb[i] = COLOR_LETTER;

    // 5. Input
    buttons_init();

    // 6. Audio
    audio_init();

    // 7. Emulator
    srand((unsigned)esp_timer_get_time());
    chip8_init(&chip);
    if (!chip8_load(&chip, pong_ch8, pong_ch8_len)) {
        ESP_LOGE("chip8", "ROM load failed (%u bytes)", pong_ch8_len);
        abort();
    }
    ESP_LOGI("chip8", "loaded %u bytes, pc=0x%03X, %d cycles/frame, %d timer ticks/frame",
             pong_ch8_len, chip.pc, CYCLES_PER_FRAME, TIMER_TICKS_PER_FRAME);

    int frame = 0;

    // Absolute deadline: += FRAME_US, never "now + FRAME_US", so a long frame
    // is followed by a short one and the average rate holds with no drift.
    int64_t next_frame = esp_timer_get_time();

    // ------------------------------------------------------------ game loop
    while (1) {
        next_frame += FRAME_US;
        int64_t t0 = esp_timer_get_time();

        // --- input
        buttons_poll();
        int64_t t_input = esp_timer_get_time();

        // --- update: CHIP8_HZ worth of emulation, then the 60 Hz timers
        update();
        int64_t t_update = esp_timer_get_time();

        // --- audio top-up, non-blocking
        size_t audio_bytes = audio_feed();
        int64_t t_audio = esp_timer_get_time();

        // --- render the 64x32 buffer into the framebuffer at 5x
        render(fb);
        int64_t t_render = esp_timer_get_time();

        // --- push. Queues the DMA transfer and returns; does not block.
        esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, fb);
        int64_t t_cpu = esp_timer_get_time();

        // Single-buffered: must not touch fb until DMA is done reading it.
        xSemaphoreTake(trans_done_sem, portMAX_DELAY);
        int64_t t_done = esp_timer_get_time();

        // --- frame clock: sleep off whatever budget remains
        int64_t now = esp_timer_get_time();
        if (next_frame > now) {
            vTaskDelay(pdMS_TO_TICKS((next_frame - now) / 1000));
        } else {
            next_frame = now;   // overran: resync, don't burst to catch up
        }

        // The core sets a flag rather than printing -- ESP_LOGI blocks on
        // UART for ~82 us, and an unimplemented opcode inside a loop would
        // otherwise drag the frame clock. Report once, then clear.
        if (chip.bad_op) {
            ESP_LOGW("chip8", "unimplemented opcode 0x%04X at 0x%03X",
                     chip.bad_opcode, chip.bad_pc);
            chip.bad_op = false;
        }

        if (++frame % 30 == 0) {
            ESP_LOGI("perf",
                     "input %lld | update %lld | audio %lld (%u B) | render %lld | cpu %lld | work %lld | frame %lld us",
                     t_input  - t0,
                     t_update - t_input,
                     t_audio  - t_update, (unsigned)audio_bytes,
                     t_render - t_audio,
                     t_cpu    - t0,
                     t_done   - t0,
                     esp_timer_get_time() - t0);
        }
    }
}