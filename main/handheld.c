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
#include "rom_tetris.h"

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

// 41 ms. Measured floor at 40 MHz SPI: work lands at ~39990 us, of which
// 30720 us is the irreducible payload time for 153600 bytes. Going lower
// means overrunning every frame. Raising the rate from 20 to 24.4 fps is
// what actually fixed the CHIP-8 XOR flicker -- narrowing the sampling gap
// beat masking it with a longer persistence decay.
#define FRAME_US  41000

// ---------------------------------------------------------------- chip-8

// Emulated rates, stated as RATES so they stay correct when FRAME_US moves.
// This is the form that made the 50000 -> 41000 change free.
//
// NOTE: at 41000, TIMER_TICKS_PER_FRAME truncates to 2, so the delay/sound
// timers run at 48 Hz rather than the spec's 60 Hz -- every timed effect is
// ~20% slow. Invisible in Pong. Fix if it ever matters is an accumulator
// rather than a fixed count.
#define CHIP8_HZ  600
#define TIMER_HZ  60

#define CYCLES_PER_FRAME       ((CHIP8_HZ * FRAME_US) / 1000000)  // 24
#define TIMER_TICKS_PER_FRAME  ((TIMER_HZ * FRAME_US) / 1000000)  // 2

// 5x integer scale: 64*5 = 320 (full panel width), 32*5 = 160.
#define SCALE     5
#define IMG_W     (CHIP8_DISPLAY_W * SCALE)   // 320
#define IMG_H     (CHIP8_DISPLAY_H * SCALE)   // 160
#define IMG_X0    ((LCD_W - IMG_W) / 2)       // 0
#define IMG_Y0    ((LCD_H - IMG_H) / 2)       // 40

#define COLOR_FG      0xFFFF   // white
#define COLOR_BG      0x0000   // black
#define COLOR_LETTER  0x0000   // letterbox bars
#define COLOR_SEL     0x1F00   // blue, pre-byte-swapped -- menu highlight
#define COLOR_DIM     0x1084   // grey, pre-byte-swapped -- unselected text

// ~6.2 KB. Static so it lands in internal RAM (.bss) rather than PSRAM --
// mem[] is touched twice per cycle and cache misses would show up per-frame.
static Chip8 chip;

// ---------------------------------------------------------------- rom table

// Each ROM carries its own keypad mapping, because CHIP-8 defines no
// convention -- every ROM picks whichever hex keys it likes. Pong uses 1/4
// and C/D; Tetris uses a different set entirely. Bundling the mapping with
// the ROM is what makes adding a third ROM a one-line change.
typedef struct {
    const char          *name;
    const unsigned char *data;
    unsigned             len;
    uint8_t              keys[4];   // indexed by button index 0..3
} rom_t;

static const rom_t roms[] = {
    // PONG: 1/4 drive the left paddle, C/D the right.
    { "PONG",   pong_ch8,   pong_ch8_len,   { 0xC, 0xD, 0x1, 0x4 } },

    // TETRIS: commonly documented as 4=rotate, 5=left, 6=right, 7=drop.
    // UNVERIFIED on this specific ROM -- if a button does nothing, this row
    // is the only thing to edit.
    { "TETRIS", tetris_ch8, tetris_ch8_len, { 0x5, 0x6, 0x4, 0x7 } },
};

#define ROM_COUNT (int)(sizeof(roms) / sizeof(roms[0]))

static const rom_t *active_rom = NULL;

// ---------------------------------------------------------------- font
//
// 5x7 uppercase bitmap font. Each glyph is 7 bytes, one per row, low 5 bits
// used, bit 4 = leftmost pixel. Hand-built because pulling in a font library
// for 40 glyphs would cost more than it saves -- and this is the same
// "everything per-pixel is a table" rule the byte-swap and palette paths hit.

#define GLYPH_W 5
#define GLYPH_H 7

static const uint8_t font5x7[][GLYPH_H] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, //  0 space
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, //  1 -
    {0x00,0x00,0x00,0x00,0x00,0x00,0x04}, //  2 .
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, //  3 > (cursor)
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, //  4 0
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, //  5 1
    {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}, //  6 2
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, //  7 3
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, //  8 4
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, //  9 5
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 10 6
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 11 7
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 12 8
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 13 9
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, // 14 A
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, // 15 B
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, // 16 C
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, // 17 D
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, // 18 E
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, // 19 F
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, // 20 G
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, // 21 H
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}, // 22 I
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, // 23 J
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, // 24 K
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, // 25 L
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, // 26 M
    {0x11,0x11,0x19,0x15,0x13,0x11,0x11}, // 27 N
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, // 28 O
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, // 29 P
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, // 30 Q
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, // 31 R
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, // 32 S
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, // 33 T
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, // 34 U
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, // 35 V
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, // 36 W
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, // 37 X
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, // 38 Y
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, // 39 Z
};

static int glyph_index(char ch)
{
    if (ch >= 'a' && ch <= 'z') ch -= 32;          // fold to uppercase
    if (ch >= '0' && ch <= '9') return  4 + (ch - '0');
    if (ch >= 'A' && ch <= 'Z') return 14 + (ch - 'A');
    switch (ch) {
        case '-': return 1;
        case '.': return 2;
        case '>': return 3;
        default:  return 0;                        // space / unknown
    }
}

// Draw one glyph at (x,y), each source pixel expanded to sc x sc.
// Clipped against the framebuffer so a long string can't scribble off-screen.
static void draw_glyph(uint16_t *fb, int x, int y, char ch, int sc, uint16_t col)
{
    const uint8_t *g = font5x7[glyph_index(ch)];

    for (int gy = 0; gy < GLYPH_H; gy++) {
        for (int gx = 0; gx < GLYPH_W; gx++) {
            if (!(g[gy] & (0x10 >> gx))) continue;

            for (int py = 0; py < sc; py++) {
                int fy = y + gy * sc + py;
                if (fy < 0 || fy >= LCD_H) continue;
                uint16_t *row = fb + fy * LCD_W;

                for (int px = 0; px < sc; px++) {
                    int fx = x + gx * sc + px;
                    if (fx >= 0 && fx < LCD_W) row[fx] = col;
                }
            }
        }
    }
}

#define TEXT_ADVANCE(sc) ((GLYPH_W + 1) * (sc))    // one blank column between

static void draw_text(uint16_t *fb, int x, int y, const char *s, int sc, uint16_t col)
{
    for (int i = 0; s[i]; i++)
        draw_glyph(fb, x + i * TEXT_ADVANCE(sc), y, s[i], sc, col);
}

static int text_width(const char *s, int sc)
{
    int n = 0;
    while (s[n]) n++;
    return n * TEXT_ADVANCE(sc) - sc;               // trailing gap doesn't count
}

static void draw_text_centered(uint16_t *fb, int y, const char *s, int sc, uint16_t col)
{
    draw_text(fb, (LCD_W - text_width(s, sc)) / 2, y, s, sc, col);
}

// ---------------------------------------------------------------- audio

#define PIN_I2S_BCLK  GPIO_NUM_42
#define PIN_I2S_WS    GPIO_NUM_41
#define PIN_I2S_DOUT  GPIO_NUM_40

#define SAMPLE_RATE   22050
#define BEEP_HZ       880
#define BEEP_AMP      8000

#define AUDIO_CHUNK   1536
#define BEEP_HALF     (SAMPLE_RATE / (BEEP_HZ * 2))

static i2s_chan_handle_t audio_tx = NULL;

static int      beep_remaining = 0;   // samples of tone left; 0 = silent
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
    if (beep_remaining <= 0) return 0;

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
    for (int i = 0; i < BTN_COUNT; i++)
        mask |= (1ULL << btn_gpios[i]);   // 1ULL: pin_bit_mask is uint64_t

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

// ---------------------------------------------------------------- menu

// Locked selection: once a ROM is chosen the menu is gone until reset. That
// keeps all four buttons available to the emulated keypad, which is the
// binding constraint -- there is no spare button for a "back to menu" combo
// that some ROM won't also want.
typedef enum { MODE_MENU, MODE_RUN } mode_t;

static mode_t mode      = MODE_MENU;
static int    menu_sel  = 0;
static bool   menu_dirty = true;   // only redraw when something changed

// Returns true when a ROM was picked.
static bool menu_update(void)
{
    if (btn_went_down(BTN_UP)) {
        menu_sel = (menu_sel - 1 + ROM_COUNT) % ROM_COUNT;
        menu_dirty = true;
        audio_beep_ms(30);
    }
    if (btn_went_down(BTN_DOWN)) {
        menu_sel = (menu_sel + 1) % ROM_COUNT;
        menu_dirty = true;
        audio_beep_ms(30);
    }
    // Either of the remaining two selects, so you don't have to remember
    // which physical switch is which.
    if (btn_went_down(BTN_LEFT) || btn_went_down(BTN_RIGHT)) {
        audio_beep_ms(120);
        return true;
    }
    return false;
}

static void menu_render(uint16_t *fb)
{
    if (!menu_dirty) return;      // static screen; no reason to repaint
    menu_dirty = false;

    for (int i = 0; i < LCD_W * LCD_H; i++) fb[i] = COLOR_BG;

    draw_text_centered(fb, 24, "CHIP-8", 5, COLOR_FG);
    draw_text_centered(fb, 70, "SELECT A GAME", 2, COLOR_DIM);

    for (int i = 0; i < ROM_COUNT; i++) {
        int y   = 110 + i * 44;
        bool on = (i == menu_sel);

        if (on) draw_glyph(fb, 40, y, '>', 4, COLOR_SEL);
        draw_text(fb, 80, y, roms[i].name, 4, on ? COLOR_FG : COLOR_DIM);
    }
}

// ---------------------------------------------------------------- glue

// One-frame phosphor decay. CHIP-8 has no sprite erase -- moving anything
// means XOR it off then XOR it on at the new spot, two DXYN instructions
// apart, and between them the sprite genuinely isn't in display[]. Sampling
// that at frame rate catches the gap sometimes, which is the flicker.
static uint8_t persist[CHIP8_DISPLAY_W * CHIP8_DISPLAY_H];

static void rom_start(const rom_t *r, uint16_t *fb)
{
    active_rom = r;

    chip8_init(&chip);
    if (!chip8_load(&chip, r->data, r->len)) {
        ESP_LOGE("chip8", "ROM load failed: %s (%u bytes)", r->name, r->len);
        abort();
    }

    memset(persist, 0, sizeof(persist));

    // Letterbox bars: painted once here, never touched by render() again.
    for (int i = 0; i < LCD_W * LCD_H; i++) fb[i] = COLOR_LETTER;

    ESP_LOGI("chip8", "running %s: %u bytes, %d cycles/frame, %d timer ticks/frame",
             r->name, r->len, CYCLES_PER_FRAME, TIMER_TICKS_PER_FRAME);
}

// Frontend -> core: physical buttons become hex keypad state, through the
// active ROM's own mapping.
static void input_to_keys(void)
{
    for (int i = 0; i < 16; i++)
        chip.keys[i] = false;

    for (int b = 0; b < BTN_COUNT; b++)
        if (btn_down(b))
            chip.keys[active_rom->keys[b]] = true;
}

static void update(void)
{
    input_to_keys();

    for (int i = 0; i < CYCLES_PER_FRAME; i++)
        chip8_cycle(&chip);

    for (int i = 0; i < TIMER_TICKS_PER_FRAME; i++)
        chip8_tick_timers(&chip);

    // Core -> frontend: re-armed every frame from the CURRENT timer value, so
    // the beep ends exactly when the timer does. Safe because beep_phase
    // persists -- the waveform doesn't restart, so there's no click.
    if (chip.sound_timer > 0)
        audio_beep_ms(chip.sound_timer * 1000 / TIMER_HZ);
}

// 64x32 -> 320x160, nearest-neighbour at 5x. Each source row expands to 5
// identical destination rows, so build one and memcpy it 4 times.
static void render(uint16_t *fb)
{
    for (int i = 0; i < CHIP8_DISPLAY_W * CHIP8_DISPLAY_H; i++)
        persist[i] = chip.display[i] ? 2 : (persist[i] ? persist[i] - 1 : 0);

    for (int cy = 0; cy < CHIP8_DISPLAY_H; cy++) {
        uint16_t      *row0 = fb + (IMG_Y0 + cy * SCALE) * LCD_W + IMG_X0;
        const uint8_t *src  = persist + cy * CHIP8_DISPLAY_W;

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

    esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = on_color_trans_done };
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

    // 5. Input
    buttons_init();

    // 6. Audio
    audio_init();

    srand((unsigned)esp_timer_get_time());
    ESP_LOGI("menu", "%d ROMs available", ROM_COUNT);

    int frame = 0;
    int64_t next_frame = esp_timer_get_time();

    // ------------------------------------------------------------ game loop
    while (1) {
        next_frame += FRAME_US;
        int64_t t0 = esp_timer_get_time();

        buttons_poll();
        int64_t t_input = esp_timer_get_time();

        // Panel recovery. The breadboard display can lose its init under
        // mechanical disturbance -- 14 contacts on one cantilevered edge.
        // This is a mitigation, not a fix; the fix is the Stage B carrier.
        if (btn_down(BTN_LEFT) && btn_down(BTN_RIGHT)) {
            ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
            ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
            ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
            ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, LCD_MIRROR_X, LCD_MIRROR_Y));
            ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
            if (mode == MODE_RUN) {
                for (int i = 0; i < LCD_W * LCD_H; i++) fb[i] = COLOR_LETTER;
            } else {
                menu_dirty = true;
            }
            ESP_LOGW("lcd", "panel re-initialized");
        }

        if (mode == MODE_MENU) {
            if (menu_update()) {
                rom_start(&roms[menu_sel], fb);
                mode = MODE_RUN;
            }
        } else {
            update();
        }
        int64_t t_update = esp_timer_get_time();

        size_t audio_bytes = audio_feed();
        int64_t t_audio = esp_timer_get_time();

        if (mode == MODE_MENU) menu_render(fb);
        else                   render(fb);
        int64_t t_render = esp_timer_get_time();

        // Queues the DMA transfer and returns; does not block.
        esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, fb);
        int64_t t_cpu = esp_timer_get_time();

        // Single-buffered: must not touch fb until DMA is done reading it.
        xSemaphoreTake(trans_done_sem, portMAX_DELAY);
        int64_t t_done = esp_timer_get_time();

        int64_t now = esp_timer_get_time();
        if (next_frame > now) {
            vTaskDelay(pdMS_TO_TICKS((next_frame - now) / 1000));
        } else {
            next_frame = now;   // overran: resync, don't burst to catch up
        }

        // The core sets a flag rather than printing -- ESP_LOGI blocks on
        // UART for ~82 us, and a bad opcode in a loop would drag the clock.
        if (chip.bad_op) {
            ESP_LOGW("chip8", "unimplemented opcode 0x%04X at 0x%03X",
                     chip.bad_opcode, chip.bad_pc);
            chip.bad_op = false;
        }

        if (++frame % 30 == 0 && mode == MODE_RUN) {
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