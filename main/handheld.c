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

#define PIN_SCK   12
#define PIN_MOSI  11
#define PIN_MISO  13
#define PIN_CS    10
#define PIN_DC    14
#define PIN_RST   15
#define PIN_BL    16

// LANDSCAPE. The panel is natively 240x320 portrait; swap_xy + mirror below
// rotate it 90 degrees, so the framebuffer is addressed 320 wide x 240 tall.
// Byte count is identical (153600) -- this is a reinterpretation of the same
// buffer, not a reallocation.
//
// The reason for landscape: CHIP-8 is a 64x32 (2:1) frame. Portrait allows
// only 3x integer scale (192x96, 24% of the panel used). Landscape allows 5x
// (320x160, full width, bars top and bottom only).
#define LCD_W 320
#define LCD_H 240

// Which mirror pairs with swap_xy to give the correct rotation depends on the
// panel's native scan direction and is not derivable -- it's a two-option A/B.
// If the image comes out rotated 180 degrees (snake travels away from the
// RIGHT button), flip these two.
#define LCD_MIRROR_X  true
#define LCD_MIRROR_Y  false

// Fixed frame clock. 50 ms = 20 fps -- known to fit from Step 4's measured
// worst case (~50.0 ms wall). The deadline is absolute, so the frame rate is
// now a decision, not whatever DMA completion happens to dictate.
#define FRAME_US  50000

// ---------------------------------------------------------------- audio

#define PIN_I2S_BCLK  GPIO_NUM_42
#define PIN_I2S_WS    GPIO_NUM_41
#define PIN_I2S_DOUT  GPIO_NUM_40

#define SAMPLE_RATE   22050
#define BEEP_HZ       880
#define BEEP_AMP      8000        // ~24% FS; louder than 5a's 3200, still headroom
#define BEEP_MS       80          // how long one press sounds

// A chunk sized a bit over one 50 ms frame's worth of samples (~1103). The
// non-blocking write self-limits to the DMA buffer's free space, so offering
// slightly more than a frame's drain keeps the buffer topped up; the surplus
// we generate but can't hand over gets regenerated next frame from the same
// phase, so nothing is skipped and there's no click at the seam.
#define AUDIO_CHUNK   1536

// Half a square-wave period, in samples. At 22050 Hz / 880 Hz this is 12,
// so the emitted tone is 22050/(12*2) = ~919 Hz -- integer rounding shifts
// the pitch slightly, which is inaudible for a beep.
#define BEEP_HALF     (SAMPLE_RATE / (BEEP_HZ * 2))

static i2s_chan_handle_t audio_tx = NULL;

static int      beep_remaining = 0;   // samples of tone left to play; 0 = silent
static uint32_t beep_phase     = 0;   // where in the cycle we are; PERSISTS across
                                      // frames so the waveform never discontinuities
static int16_t  audio_buf[AUDIO_CHUNK];

static void audio_init(void)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    // Underrun replays the stale DMA buffer otherwise -- a loud repeating
    // fragment instead of silence. With this, "don't write" == silence, which
    // is why the channel can stay enabled permanently and we just skip the
    // write when nothing is playing.
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
    // Left enabled for the whole program -- see auto_clear note above.
}

// Arm a beep. Just sets the sample countdown; the loop does the actual work.
static void audio_beep(void)
{
    beep_remaining = SAMPLE_RATE * BEEP_MS / 1000;
}
static void audio_beep_ms(int ms)
{
    beep_remaining = SAMPLE_RATE * ms / 1000;
}

// Called once per frame, non-blocking. Generates up to a chunk of square wave
// and hands it to the DMA buffer. Returns bytes the buffer actually accepted --
// the underrun gauge: if it swallows everything every frame, the buffer was
// empty when we arrived and we're on the edge of a click.
static size_t audio_feed(void)
{
    if (beep_remaining <= 0) {
        return 0;   // nothing playing; auto_clear makes the buffer go silent
    }

    int n = beep_remaining < AUDIO_CHUNK ? beep_remaining : AUDIO_CHUNK;

    // Generate from a LOCAL copy of the phase so we can advance the real phase
    // by only the samples that actually get written. Square wave: high for the
    // first half of each period, low for the second.
    uint32_t ph = beep_phase;
    for (int i = 0; i < n; i++) {
        audio_buf[i] = (ph < BEEP_HALF) ? BEEP_AMP : -BEEP_AMP;
        if (++ph >= BEEP_HALF * 2) ph = 0;
    }

    size_t written = 0;
    // timeout 0: take what fits right now, don't wait for the buffer to drain.
    i2s_channel_write(audio_tx, audio_buf, n * sizeof(int16_t), &written, 0);

    int samples_written = written / sizeof(int16_t);

    // Advance the persistent phase by ONLY what was accepted, so the samples we
    // generated but the buffer didn't take are regenerated identically next
    // frame -- continuous waveform, no seam click.
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

// Measured bounce was ~165 us worst case (release edge). At the ~40 ms frame
// clock a single confirming sample is already generous; 2 is cheap insurance
// against a glitch landing exactly on a sample boundary.
#define DEBOUNCE_SAMPLES 2

typedef struct {
    bool stable;     // debounced state, true = pressed
    bool candidate;  // most recent raw reading
    int  count;      // consecutive samples agreeing with candidate
    bool went_down;  // true for exactly one poll when stable goes false->true
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

// Active low: internal pull-up holds the line high, the switch pulls it to GND.
// Kept active-low all the way through because the DMG joypad register (P1/JOYP)
// also reads 0 for pressed -- Phase 2 maps raw pin state on with no inversion.
static void buttons_poll(void)
{
    for (int i = 0; i < BTN_COUNT; i++) {
        btns[i].went_down = false;   // clear the one-shot edge each poll
        bool raw = (gpio_get_level(btn_gpios[i]) == 0);

        if (raw != btns[i].candidate) {
            btns[i].candidate = raw;
            btns[i].count = 1;
        } else if (raw != btns[i].stable) {
            if (++btns[i].count >= DEBOUNCE_SAMPLES) {
                bool was = btns[i].stable;
                btns[i].stable = raw;
                if (!was && raw) btns[i].went_down = true;  // false->true edge
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

// Colors are stored pre-byte-swapped: the ILI9341 reads big-endian, the S3
// stores little-endian. See NOTES.md -- the swap folds into the Phase 2 palette.
#define COLOR_BG  0x1F00   // blue
#define COLOR_SQ  0xFFFF   // white (swap-invariant)

// ---------------------------------------------------------------- snake game
//
// TEMPORARY. Snake is retained for exactly one flash, as the orientation
// reference: it is a display path already known good, so if the screen looks
// wrong here the fault is the rotation and nothing else. Step 11d deletes this
// whole section -- the CHIP-8 core replaces update()/render().

#define COLOR_SNAKE  0xE007   // green, pre-swapped
#define COLOR_FOOD   0x00F8   // red, pre-swapped

// 16 px cells. In landscape: 320/16 = 20 wide, 240/16 = 15 tall. Both still
// clean divisors, and the cell COUNT is unchanged at 300 -- the grid is the
// same 300 cells rotated, so nothing about the game logic or the circular
// buffer sizing moves. Derived from LCD_W/LCD_H, which is why this was free.
#define CELL       16
#define GRID_W     (LCD_W / CELL)    // 20
#define GRID_H     (LCD_H / CELL)    // 15
#define GRID_CELLS (GRID_W * GRID_H) // 300 = max snake length

#define MOVE_EVERY 4                 // move-tick every 4 frames = 5 moves/sec
#define START_LEN  3

typedef enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT } dir_t;

// Body is a circular buffer of cell coords. head_i is the newest segment;
// tail is (head_i - len + 1) mod GRID_CELLS. Advancing = write new head,
// grow = don't retire the tail that tick. No shifting, O(1) per move.
typedef struct {
    uint8_t x[GRID_CELLS], y[GRID_CELLS];
    int     head_i;
    int     len;
    dir_t   dir;        // direction consumed at the move-tick
    dir_t   next_dir;   // latched from input every frame
    int     food_x, food_y;
    int     tick;       // frames since last move
    bool    dead;
} game_state_t;

static bool snake_occupies(const game_state_t *g, int x, int y)
{
    for (int i = 0; i < g->len; i++) {
        int idx = (g->head_i - i + GRID_CELLS) % GRID_CELLS;
        if (g->x[idx] == x && g->y[idx] == y) return true;
    }
    return false;
}

static void place_food(game_state_t *g)
{
    do {
        g->food_x = rand() % GRID_W;
        g->food_y = rand() % GRID_H;
    } while (snake_occupies(g, g->food_x, g->food_y));
}

static void game_reset(game_state_t *g)
{
    g->len    = START_LEN;
    g->head_i = START_LEN - 1;
    for (int i = 0; i < START_LEN; i++) {           // horizontal, head rightmost
        g->x[i] = GRID_W / 2 - (START_LEN - 1) + i;
        g->y[i] = GRID_H / 2;
    }
    g->dir = g->next_dir = DIR_RIGHT;
    g->tick = 0;
    g->dead = false;
    place_food(g);
}

static void update(game_state_t *g)
{
    // Latch direction every frame (responsive), reject 180-degree reversal.
    if (btn_went_down(BTN_UP)    && g->dir != DIR_DOWN)  g->next_dir = DIR_UP;
    if (btn_went_down(BTN_DOWN)  && g->dir != DIR_UP)    g->next_dir = DIR_DOWN;
    if (btn_went_down(BTN_LEFT)  && g->dir != DIR_RIGHT) g->next_dir = DIR_LEFT;
    if (btn_went_down(BTN_RIGHT) && g->dir != DIR_LEFT)  g->next_dir = DIR_RIGHT;

    if (g->dead) {
        // Any press restarts.
        for (int i = 0; i < BTN_COUNT; i++)
            if (btn_went_down(i)) { game_reset(g); return; }
        return;
    }

    if (++g->tick < MOVE_EVERY) return;   // not a move-tick
    g->tick = 0;

    g->dir = g->next_dir;
    int hx = g->x[g->head_i], hy = g->y[g->head_i];
    switch (g->dir) {
        case DIR_UP:    hy--; break;
        case DIR_DOWN:  hy++; break;
        case DIR_LEFT:  hx--; break;
        case DIR_RIGHT: hx++; break;
    }

    // Wall or self = death. Self-check runs BEFORE writing the new head; tail
    // cell is still counted, which is the conservative (classic) rule.
    if (hx < 0 || hx >= GRID_W || hy < 0 || hy >= GRID_H ||
        snake_occupies(g, hx, hy)) {
        g->dead = true;
        audio_beep_ms(400);
        return;
    }

    bool ate = (hx == g->food_x && hy == g->food_y);

    g->head_i = (g->head_i + 1) % GRID_CELLS;
    g->x[g->head_i] = hx;
    g->y[g->head_i] = hy;

    if (ate) {
        g->len++;               // keep tail this tick = grow
        audio_beep();
        if (g->len < GRID_CELLS) place_food(g);
    }
    // Not ate: len unchanged, tail retires implicitly via the modular window.
}

static void fill_cell(uint16_t *fb, int cx, int cy, uint16_t color)
{
    for (int y = cy * CELL; y < (cy + 1) * CELL; y++) {
        uint16_t *row = fb + y * LCD_W;
        for (int x = cx * CELL; x < (cx + 1) * CELL; x++) row[x] = color;
    }
}

static void render(const game_state_t *g, uint16_t *fb)
{
    // Dead = red flash background so the state is unmissable.
    uint16_t bg = g->dead ? COLOR_FOOD : COLOR_BG;
    for (int i = 0; i < LCD_W * LCD_H; i++) fb[i] = bg;

    fill_cell(fb, g->food_x, g->food_y, COLOR_FOOD);
    for (int i = 0; i < g->len; i++) {
        int idx = (g->head_i - i + GRID_CELLS) % GRID_CELLS;
        fill_cell(fb, g->x[idx], g->y[idx], COLOR_SNAKE);
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

    // Rotate to landscape. MUST come AFTER esp_lcd_panel_init() -- init runs
    // the datasheet startup sequence, which includes writing memory access
    // mode (MADCTL). These two calls rewrite that same register, so calling
    // them first would just get overwritten.
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, LCD_MIRROR_X, LCD_MIRROR_Y));

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    // 4. Framebuffer in PSRAM, 64-byte aligned for clean DMA cache writeback
    size_t fb_size = LCD_W * LCD_H * sizeof(uint16_t);   // 153600, unchanged
    uint16_t *fb = heap_caps_aligned_alloc(64, fb_size, MALLOC_CAP_SPIRAM);
    assert(fb != NULL);
    ESP_LOGI("fb", "framebuffer at %p, %u bytes, %dx%d",
             fb, (unsigned)fb_size, LCD_W, LCD_H);

    // 5. Input
    buttons_init();

    // 6. Audio -- channel created once, enabled for the whole program.
    audio_init();

    srand((unsigned)esp_timer_get_time());
    game_state_t game;
    game_reset(&game);
    int frame = 0;

    // Absolute deadline for the fixed frame clock. Advancing by += FRAME_US
    // (never "now + FRAME_US") means a long frame is followed by a short one
    // and the average rate holds -- no drift accumulation.
    int64_t next_frame = esp_timer_get_time();

    // ------------------------------------------------------------ game loop
    while (1) {
        next_frame += FRAME_US;
        int64_t t0 = esp_timer_get_time();

        // --- input
        buttons_poll();
        int64_t t_input = esp_timer_get_time();

        // --- update: pure logic against the state struct
        update(&game);

        // --- audio top-up. Non-blocking; feeds the DMA buffer whatever the
        // current beep needs, then returns. audio_bytes is the underrun gauge.
        size_t audio_bytes = audio_feed();
        int64_t t_audio = esp_timer_get_time();

        // --- render state into the framebuffer
        render(&game, fb);
        int64_t t_render = esp_timer_get_time();

        // --- push. Queues the DMA transfer and returns; does not block.
        esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, fb);
        int64_t t_cpu = esp_timer_get_time();

        // Single-buffered: must not touch fb again until DMA has finished
        // reading it. This wait is also what yields the CPU to IDLE -- and it
        // is the ~36 ms window the audio DMA buffer has to play through on its
        // own, since the CPU tops up audio only once per trip around the loop.
        xSemaphoreTake(trans_done_sem, portMAX_DELAY);
        int64_t t_done = esp_timer_get_time();

        // --- frame clock: sleep off whatever budget remains
        int64_t now = esp_timer_get_time();
        if (next_frame > now) {
            // Slack exists: yield until the deadline. vTaskDelay granularity
            // is 10 ms ticks -- coarse, but fine at a 50 ms frame. Revisit if
            // the budget ever tightens.
            vTaskDelay(pdMS_TO_TICKS((next_frame - now) / 1000));
        } else {
            // Overran the budget: resync rather than bursting to catch up.
            next_frame = now;
        }

        if (++frame % 30 == 0) {
            ESP_LOGI("perf",
                     "input %lld | audio %lld (%u B) | render %lld | cpu %lld | work %lld | frame %lld us",
                     t_input  - t0,
                     t_audio  - t_input, (unsigned)audio_bytes,
                     t_render - t_audio,
                     t_cpu    - t0,
                     t_done   - t0,
                     esp_timer_get_time() - t0);
        }
    }
}