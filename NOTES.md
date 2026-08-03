# Engineering Notes — Handheld Console (Phase 1)

Running log of measurements, constraints, and the decisions they forced.
Written as I go so the README doesn't have to be reconstructed from memory later.

---

## Platform

| | |
|---|---|
| MCU | ESP32-S3-DevKitC-1 **v1.1**, N8R8 |
| Flash / PSRAM | 8 MB / 8 MB octal |
| CPU clock | 160 MHz, dual core |
| PSRAM clock | 40 MHz, **octal mode** (quad is the IDF default and silently fails on N8R8) |
| Display | HiLetgo ILI9341, 240×320, SPI |
| Framework | ESP-IDF v6.0.2 |
| Display driver | `esp_lcd` + `espressif/esp_lcd_ili9341` v2.0.2 |

**Board rev gotcha:** DevKitC-1 v1.1 puts the onboard WS2812 on **GPIO38, not GPIO48**.
v1.1 moved it because GPIO47/48 run off the 1.8 V SPI supply shared with the octal PSRAM.

**Display wiring** (SPI2_HOST): SCK=12, MOSI=11, MISO=13, CS=10, DC=14, RST=15, BL=16.
All on the DevKitC-1 left edge; 10–14 are five consecutive pins.
The five `T_*` pins (XPT2046 touch controller) are intentionally unwired — touch is out of scope.
SD slot shares SCK/MOSI/MISO and will need only its own CS in Step 6.

**Pin selection constraints:** GPIO 26–37 are unusable — the octal PSRAM owns them.
Strapping pins (0, 3, 45, 46) and native USB (19, 20) also excluded. That leaves
4–18 and 38–42. Chose 11/12/13 for MOSI/SCK/MISO because they're SPI2's IO_MUX
defaults — a direct path that bypasses the GPIO matrix, and the configuration every
reference example uses.

### Display transport stack

Three layers, configured separately:

1. **SPI bus** (`spi_bus_initialize`) — the physical peripheral. Pins, DMA channel,
   `max_transfer_sz`. Shared: the SD slot joins *this same bus* in Step 6 rather than
   initializing its own. Only its CS is new.
2. **Panel IO** (`esp_lcd_new_panel_io_spi`) — turns SPI bytes into display protocol.
   Owns DC and CS, so commands go out with DC low and params with DC high automatically.
   Also owns `pclk_hz` — the clock lives here, not on the bus.
3. **Panel** (`esp_lcd_new_panel_ili9341`) — the chip itself. `esp_lcd_panel_init()` runs
   the datasheet startup sequence: power control, gamma tables, memory access mode,
   pixel format.

`draw_bitmap(panel, x0, y0, x1, y1, buf)` expands to CASET → RASET → RAMWR + payload.
The panel keeps an internal cursor and auto-increments through the window.

---

## Step 3 — PSRAM framebuffer pushed over DMA

**Goal:** stop the CPU from babysitting the display, so Phase 2 has CPU time to emulate with.

### The physics floor

A full frame is 240 × 320 × 2 bytes = **153,600 bytes = 1,228,800 bits**.
The ILI9341 has no addressable memory — every frame is the whole payload over the wire.

| SPI clock | Minimum possible frame time |
|---|---|
| 10 MHz | 122.9 ms |
| 40 MHz | 30.7 ms |

No code change moves these numbers. Everything measured above them is overhead.

### Measurements

| | Line-by-line @ 10 MHz | Full-frame @ 10 MHz | Full-frame @ 40 MHz |
|---|---|---|---|
| Wall clock | 187.9 ms | 130.7 ms | 40.7 ms |
| Frame rate | 5.3 fps | 7.7 fps | 24.6 fps |
| Bus payload (floor) | 122.9 ms | 122.9 ms | 30.7 ms |
| Overhead | 65.0 ms (34.6%) | 7.8 ms (6.0%) | 10.0 ms (24.6%) |
| CPU busy | ~188 ms (pinned) | 5.86 ms | 5.83 ms |
| CPU free | ~0% | 95.5% | 85.7% |

Jitter across 20+ consecutive frames was ~0.02% (187867 µs repeated to the microsecond).
Deltas of even a few percent are trustworthy on this platform.

### Two independent optimizations, measured separately

**1. One transaction instead of 320 → 1.44×**
The line-by-line version called `esp_lcd_panel_draw_bitmap` once per row. Each call is
~7 SPI transactions, not one: CASET (cmd + 4 params), RASET (cmd + 4 params), RAMWR,
then the 480-byte payload — each with its own setup, DC toggle, and completion interrupt.
Cost: **~203 µs per line**, 65 ms per frame, 34.6% of wall time spent on overhead
for data that could move in a single transfer.

**2. 10 MHz → 40 MHz → 3.21×**
Pure bus time, scales linearly as expected.

**Combined: 187.9 ms → 40.7 ms, 4.62×.**

Deliberately changed one variable at a time so each speedup is attributable.
Doing both at once would have produced one big number with no explanation.

### The actual claim: CPU time recovered

`esp_lcd_panel_draw_bitmap` queues the transfer and returns — it does not block.
The DMA engine moves the bytes; the CPU walks away. Measured with a
`on_color_trans_done` ISR callback giving a binary semaphore, so the loop can
timestamp both "CPU is free" and "pixels have landed":

```
cpu busy: 5859 us | transfer done: 129122 us     @ 10 MHz
```

**5.9 ms occupied out of a 130.7 ms frame — 95.5% of the frame returned to the CPU.**

That gap is Phase 2's budget. A DMG core needs ~4M emulated cycles/sec; without
DMA there is no CPU left to run it and the project stops at "a screen that works."

The 5.9 ms is *not* SPI time — it's dominated by the cache writeback over the
150 KB PSRAM buffer (DMA must see current data, not stale cache lines), plus the
CASET/RASET/RAMWR command transactions, which are synchronous.

**Predicted before measuring:** this cost is memory-bandwidth bound, not clock
bound, so it should stay ~5.9 ms at 40 MHz even though the transfer gets 3.2×
shorter — meaning the CPU-free fraction should *fall*.

**Confirmed:**
cpu busy: 5859 us | transfer done: 129122 us @ 10 MHz → 95.5% free
cpu busy: 5832 us | transfer done: 40711 us @ 40 MHz → 85.7% free

The transfer got 3.2× faster; the CPU cost moved 0.5%, which is noise. Two
independent bottlenecks. **The fixed cost is what eventually bites, not the wire** —
extrapolating to 80 MHz (~20 ms transfer) puts CPU-free at ~71%. That's the
argument for double buffering, and now it's a number rather than a hunch.

---

## Decisions

**Framebuffer stored pre-byte-swapped.**
RGB565 must be byte-swapped: the ILI9341 reads big-endian, the ESP32 stores
little-endian, so red is `0x00F8`, not `0xF800`.

Chose to store the framebuffer already swapped rather than swapping during the DMA push.
Reason: the Phase 2 pixel path is palette-index-based (CHIP-8 = 2 colors, DMG = 4 shades),
so every pixel comes from a lookup table. Swap once when building the palette and the
per-pixel cost is zero forever. Swapping at push time would mean a 76,800-word CPU pass
over the buffer every frame — at exactly the moment DMA is supposed to have freed the CPU —
and would need a second buffer anyway, since you can't rewrite a buffer DMA is reading.

**`esp_lcd` over LovyanGFX.**
Step 3's DMA framebuffer is the portfolio claim. LovyanGFX's sprite system would own
that buffer instead of me. LovyanGFX stays available later for fonts — it rides the
same transport.

**Backlight on GPIO16, not tied to 3V3.**
Costs a pin and a `gpio_set_level()` at init. Buys PWM brightness control and the
ability to kill it in sleep — the backlight is the largest single power draw in the
build (~60–100 mA), so Step 10 needs it switchable.

Side effect worth knowing: the panel is dark on power-up until firmware drives the pin.
A dark screen at boot is *expected*, not a fault.

**40 MHz SPI, 2.7× over datasheet.**
ILI9341 specs a 66 ns minimum write cycle (~15 MHz). Running at 40 MHz on breadboard
jumpers with no artifacts — solid fills, correct colors, no speckling. Community
consensus is that this part overclocks reliably. Revisit if signal integrity degrades;
"breadboard parasitics capped SPI at X MHz" would itself be a finding, and an argument
for the Stage B PCB.

---

## Gotchas (learned the hard way)

- **SPI to the display is write-only — there is no back-channel.** A wrong DC pin,
  swapped MOSI/SCK, wrong SPI host, or a bad init sequence all produce the identical
  symptom: blank screen, zero error output. The chip cannot tell the panel didn't
  hear it. **Check wiring continuity before flashing**, or a wiring fault and a
  firmware fault are indistinguishable.
  Cheap partial test with no meter: move the display's LED wire to 3V3. If the
  backlight lights, VCC/GND/panel are confirmed alive — 3 of 9 wires validated,
  and the fault is isolated to the remaining 6.
- **`rgb_ele_order`** wrong = image renders with red and blue swapped. Distinctive and
  self-diagnosing, unlike most display faults. ILI9341 wants `BGR`.
  `spi_mode` must be 0; wrong mode garbles every byte.
- **`ESP_ERROR_CHECK` on every init call.** Any failure panics with file, line, and
  error code. Without it a failed init just returns and you debug a blank screen with
  no information.
- **`idf.py add-dependency` only writes `main/idf_component.yml`.** The component manager
  doesn't fetch until CMake reconfigures, so on an already-built project you get
  "No such file or directory" on the new header. **Always `idf.py fullclean` after
  adding a dependency.** Cost me time twice.
- **Charge-only USB cable.** Board powers up with a solid red LED and never enumerates
  in Device Manager — looks like a driver problem, isn't. Known-good data cable is now
  labeled. Also: connect the board directly to the laptop, not through the USB hub.
- **`max_transfer_sz` sizes the DMA descriptor chain at `spi_bus_initialize()` time.**
  Too small and it fails at *init*, not at runtime. If init succeeded, the size took.
  Cost is small: ~38 descriptors × 12 bytes for a full frame.
- **IDF reserves 32 KB of internal RAM for DMA.** A 150 KB framebuffer will not come
  from there. Must be `MALLOC_CAP_SPIRAM` explicitly — plain `malloc` may hand back
  internal RAM and succeed until it doesn't. Log the pointer: PSRAM is `0x3C…`,
  internal is `0x3FC…`.
- **64-byte alignment on the framebuffer.** DMA reads PSRAM through the cache;
  misalignment means the driver can't cleanly write back the lines it needs.
  153,600 divides evenly by 64, so both ends land clean.
- **`assert()` compiles to nothing under `NDEBUG`**, which IDF defines in release builds.
  Fine for a pointer check that's followed by a log, not fine for anything load-bearing.
- **`xSemaphoreTake(..., portMAX_DELAY)` waits forever.** If the callback never fires,
  the task hangs silently and the log just stops — reads like a crash, isn't one.
- **VS Code red squiggles on ESP-IDF includes are cosmetic.** IntelliSense doesn't know
  the header paths; the terminal build is the source of truth. Fix with
  `"C_Cpp.default.compileCommands": "${workspaceFolder}/build/compile_commands.json"`.

---

## Observed, filed for later

**Tearing.** At 130 ms/frame the red→green transition visibly *slid* down the screen —
the panel painting top-to-bottom slow enough for the eye to follow. At 40 ms it reads as
instant. Nothing changed about how it paints, only the speed. Harmless with solid-color
frames; will be a real artifact in Phase 2 when the top half shows frame N and the bottom
half frame N+1 during a scroll. Likely answer is double buffering — needs 300 KB of the
8 MB PSRAM, which is affordable.

---

## Open questions

- [ ] Try 80 MHz. Bus floor would be 15.4 ms (~65 fps), but that's well past what
      breadboard wiring is likely to hold. Worth finding the ceiling and writing it down.
- [ ] Double buffering: measure tearing on a real moving image before deciding.
- [ ] Does partial-region update beat full-frame for mostly-static screens?
      DMG is 160×144 — scaled into 240×320 that's a lot of unchanged border.

---

## Step log

- **Step 1 — Toolchain + blink.** ✅ Aug 1 2026. ESP-IDF v6.0.2 build → flash → monitor
  proven end-to-end. RGB LED on GPIO38 via `led_strip` over the RMT peripheral.
- **Step 2 — Display bring-up.** ✅ Aug 1 2026. ILI9341 cycling full-screen R/G/B over SPI.
- **Step 3 — PSRAM framebuffer + DMA.** ✅ Aug 2 2026. 4.62× faster, 95% of CPU recovered.
- **Step 4 — Input.** Next: draw a square, move it with buttons, software debounce.