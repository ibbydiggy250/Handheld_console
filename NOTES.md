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
| Audio | MAX98357A I2S class-D amp → 28 mm 8 Ω 2 W speaker |
| Framework | ESP-IDF v6.0.2 |
| Display driver | `esp_lcd` + `espressif/esp_lcd_ili9341` v2.0.2 |

**Board rev gotcha:** DevKitC-1 v1.1 puts the onboard WS2812 on **GPIO38, not GPIO48**.
v1.1 moved it because GPIO47/48 run off the 1.8 V SPI supply shared with the octal PSRAM.

**Display wiring** (SPI2_HOST): SCK=12, MOSI=11, MISO=13, CS=10, DC=14, RST=15, BL=16.
All on the DevKitC-1 left edge; 10–14 are five consecutive pins.
The five `T_*` pins (XPT2046 touch controller) are intentionally unwired — touch is out of scope.
SD slot shares SCK/MOSI/MISO and will need only its own CS in Step 6.

**Button wiring:** GPIO 4 (left), GPIO 5 (right). Internal pull-up, switch to ground.

**Audio wiring:** BCLK=42, LRC/WS=41, DIN=40, Vin=3V3, GND=G.
`GAIN` and `SD` deliberately left floating — see Step 5.

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

**Re-confirmed in Step 4** on a different workload: with a render pass added, the
fixed cost measured 14034 − 8216 = **5818 µs**, against 5832 µs in Step 3. Within
noise, and now corroborated across two builds doing different amounts of work.

---

## Step 4 — Input

**Goal:** physical buttons on GPIO, polled in the game loop, debounced in software.
Two buttons to prove the pattern, expandable to six.

### Pin selection

Buttons go on **GPIO 4–9**. Free after the PSRAM reserve (26–37), the strapping pins
(0, 3, 45, 46), native USB (19, 20), and the display's 10–16.

Chose the 4–9 block over 38–42 for a reason beyond availability: on the S3, only
**GPIO 0–21 are RTC GPIOs**, and `esp_sleep_enable_ext1_wakeup()` requires an RTC pin.
Step 10 will want "press a button to wake from deep sleep." Putting buttons on 38–42
would silently foreclose that. The whole 4–9 block is RTC-capable, so the six-button
expansion stays wake-capable.

If phantom presses ever appear, GPIO 1 and 2 on the opposite header are the escape
hatch — also free, also RTC-capable, and physically away from the 40 MHz SPI bundle.
A 45 kΩ internal pull-up is a high-impedance node; don't run button jumpers parallel
to SCK/MOSI if it can be avoided.

### Electrical design

**Internal pull-up, switch to ground. Active low: released = 1, pressed = 0.**

No external resistors, no capacitors. Two wires per button.

Chose pull-up over pull-down for two reasons. The ground rail is already distributed
from the display wiring, so ground is the cheap net to reach. And the failure mode is
better: if a jumper falls out the pin floats high to "released" rather than latching a
phantom press.

Active-low is kept all the way through to the input layer rather than normalized at the
driver, because **the DMG joypad register (`P1`/`JOYP`) also reads 0 for pressed.** In
Phase 2 the raw pin state maps onto the emulated register with no inversion.

### The expansion seam

Buttons are addressed by **index, not by pin**, with the count derived from the array:

```c
#define BTN_LEFT   0
#define BTN_RIGHT  1
#define BTN_COUNT  2

static const int btn_gpios[BTN_COUNT] = { 4, 5 };
```

Every loop is `for (i = 0; i < BTN_COUNT; i++)`. Nothing above the GPIO layer knows a
pin number or a button count. Adding the remaining four buttons is: bump `BTN_COUNT`,
add the index defines, add the pins to `btn_gpios`. No other edit.

`btn_down()` takes the index and is the single place the active-low convention is
stated:

```c
static inline bool btn_down(int b) { return btns[b].stable; }
```

Only two buttons are wired now, and that is the correct scope — the Step 9 game is
Pong or Snake, and the CHIP-8 port (Step 11) needs a handful of keys at most. Pong is
two per player; Tetris is left/rotate/right/drop. The full 16-key CHIP-8 keypad is a
spec detail no ROM in hand actually exercises.

### Tactile switch pinout — the rule that's actually orientation-independent

A 6×6 mm tactile switch has **4 legs = 2 terminals of 2 legs each.** The two legs of a
terminal are internally connected; pressing bridges terminal to terminal.

The standard advice is "straddle the center trench and wire the two sides." **That
advice is wrong, or at least not reliable.** Which physical axis the terminals separate
along depends on how the part happens to sit in the board, and the switch will happily
bridge the trench — the trench separates nothing.

Wiring both jumpers to the same terminal produces a pin that reads **0 constantly** and
does not respond to presses. Removing either jumper sends it back to 1. That's the
signature.

**The only orientation-independent rule: wire two DIAGONALLY OPPOSITE legs.**
Diagonal legs are never the same terminal, whichever way the part is turned. Same-side
and same-row pairs may or may not be, depending on orientation.

Second consequence, easy to miss: **if a terminal spans the trench, that entire row is
live across both halves of the board.** Rows normally treated as two independent nodes
become one. Don't park anything else in a row a button occupies, and give the next
button rows well clear of it.

### Determining the pairing: use the MCU, not the meter

Don't try to establish the pinout with a multimeter. Wire it diagonally, flash a
polling loop, and read the log:

```c
gpio_config_t btn_cfg = {
    .pin_bit_mask = mask,            // OR of (1ULL << gpio) for each button
    .mode         = GPIO_MODE_INPUT,
    .pull_up_en   = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_DISABLE,
};
ESP_ERROR_CHECK(gpio_config(&btn_cfg));
```

`1ULL`, not `1` — `pin_bit_mask` is `uint64_t`, and `1 << 38` on a 32-bit int is
undefined behaviour that compiles clean. Harmless at GPIO 4, bites on a high pin.

Log the raw level before writing any game logic. Three states, all unambiguous:

| Log | Meaning |
|---|---|
| Steady 1, 0 while held | Correct. Whole chain proven. |
| Steady 1, no response | Circuit never closes — legs not in clips, or a jumper on the wrong pin |
| Steady 0, no response | Both jumpers on the same terminal |

The pull-up makes this self-diagnosing in a way a continuity test isn't.

### Contact bounce — measured

**A game-loop press counter cannot measure bounce.** At a 50 ms frame clock the loop
samples 20×/sec; any bounce burst finishes entirely between two samples. A counter
driven off frame-rate polling reads exactly N for N presses whether or not a single
line of debounce logic exists. That is a green test carrying zero information.

Measured instead with a dedicated tight loop, run once before the game loop, timestamping
every level change on GPIO 4:

```c
int64_t t_start = esp_timer_get_time();
int last = gpio_get_level(btn_gpios[0]);
while (esp_timer_get_time() - t_start < 15000000) {
    int now = gpio_get_level(btn_gpios[0]);
    if (now != last) {
        ESP_LOGI("bounce", "%d at %lld", now, esp_timer_get_time());
        last = now;
    }
}
```

**Results — 11 presses:**

| | |
|---|---|
| Presses showing bounce | 3 of 11 (~27%) |
| Edge affected | **Release only.** No press edge bounced once. |
| Burst duration | ~165 µs max |
| Transitions per burst | 2 extra |
| Resolution floor | **~82 µs** (see below) |

Sample burst:

```
8153929 (0)  press
8318222 (1)  release
8318303 (0)  ← +81 us
8318384 (1)  ← +81 us
```

**The asymmetry is the real finding.** Releases bounce, presses don't. Mechanically
consistent: on press the dome is driven closed under finger force; on release it
separates under spring force alone with no damping.

**The 82 µs figure is the instrument, not the switch.** Intervals across three
independent bursts were 81, 81, 85, 84, 85, 84 µs — far too regular for mechanical
bounce. `ESP_LOGI` blocks while pushing bytes to UART, so during a burst the effective
sample period is the log-print time, not the loop period. **Bounce finer than ~82 µs is
invisible to this method.** Honest claim: *burst under ~250 µs on release; fine
structure unresolved.*

### Debounce implementation

**N consecutive agreeing samples**, not a millisecond lockout:

```c
#define DEBOUNCE_SAMPLES 2
```

A state only changes after it reads the same way N polls in a row. One counter per
button, no timestamps, and it degrades gracefully if loop rate drifts. A fixed-time
lockout would need retuning every time frame timing changes — and frame timing *will*
change in Phase 2.

**Margin:** 165 µs bounce against a 50,000 µs frame clock is ~300×. Even at a 16 ms
Phase 2 frame time it's ~100×. `DEBOUNCE_SAMPLES 2` is insurance against switches not
yet measured — the six-button expansion may include parts that behave worse — not
against anything observed on these.

### Interrupts rejected

Poll in the game loop. GPIO interrupts interact badly with contact bounce — a single
press fires a burst of edges, and the debounce has to live in an ISR or a deferred
task either way. Polling at frame rate is simpler, and the game loop already runs on a
frame clock.

**Measured poll cost: 2 µs** for two buttons, including debounce state machine.
Rounding error against a 50 ms frame. Six buttons will not change that.

### The render cost — double buffering, argued a second way

Adding a render pass moved the frame from **40.7 ms → 50.0 ms**:

```
input 2 us | render 8216 us | cpu 14034 us | frame 49999 us
```

Frame time decomposes cleanly:

| Stage | Time | Notes |
|---|---|---|
| Input poll | 2 µs | negligible |
| Render (clear + square) | 8216 µs | 153,600 B written to PSRAM |
| DMA setup / cache writeback | 5818 µs | matches Step 3's 5832 µs |
| DMA transfer | ~35,965 µs | CPU idle, blocked on semaphore |
| **Total** | **49,999 µs** | 20.0 fps |

The 8.2 ms render is **serialized, not overlapped**. Single-buffered means frame N+1
cannot start rendering until DMA finishes reading frame N, so render time adds directly
to the frame instead of hiding underneath the transfer.

**This is the same conclusion as Step 3's extrapolation, reached independently.** Step 3
argued double buffering from a fixed cost that would grow as a fraction when the bus got
faster. Step 4 argues it from a render cost that is pure serialization overhead today.
With two buffers the 8.2 ms disappears under the 36 ms transfer: **50.0 ms → ~41.8 ms,
20.0 → 23.9 fps, ~1.20×** — before touching the SPI clock.

Cost is 153,600 more bytes of the 8 MB PSRAM. Measured twice, from two directions, is a
stronger case than either alone.

Jitter note: `frame 49999 us` repeated to the microsecond across every sample, matching
the ~0.02% determinism seen in Step 3. This platform's timing is trustworthy enough that
small deltas mean something.

---

## Step 5a — Audio bring-up (I2S transport)

**Goal:** prove the whole audio chain — I2S peripheral → three wires → MAX98357A →
speaker — before any game logic can be a suspect.

Split into 5a (fixed tone above the game loop, blocking write, no buttons) and 5b
(non-blocking, button-triggered, cost measured in the frame budget) for the same
reason Step 4 logged the raw GPIO level before writing any game logic:
**I2S is write-only, exactly like SPI.** A wrong pin, wrong slot mode, or bad clock
config all produce the identical symptom — silence, zero error output. Prove the
transport first and 5b's bugs can only be in 5b.

### The passive buzzer was skipped, deliberately

The plan listed the starter-kit buzzer as a cheaper sanity check. Cut it. A passive
buzzer exercises **LEDC PWM** — different peripheral, different clock tree, different
data model (duty cycle vs. sample stream). Nothing it proves carries into I2S and
nothing it fails would say anything about the amp path. It's a detour producing code
that gets thrown away.

### Pin selection — the mirror image of Step 4's argument

**BCLK = GPIO42, LRC/WS = GPIO41, DIN = GPIO40.**

Step 4 spent the RTC-capable 4–9 block on buttons specifically to preserve
`esp_sleep_enable_ext1_wakeup()` for Step 10. I2S has **no wake requirement**, so
audio should spend the *non-RTC* pins. 39–42 are four consecutive pins on the right
header; 38 is the onboard WS2812, so take 40/41/42 and leave 39 spare.

Secondary benefit: physically opposite the 40 MHz SPI bundle at the bottom-left. That
matters more here than it did for buttons — these are real clock lines, not a
high-impedance pull-up node.

**BCLK runs at 22050 × 16 × 2 = 705.6 kHz.** Two orders of magnitude under the SPI
clock, so breadboard parasitics are a non-issue for audio. The 80 MHz SPI experiment
in Open Questions is unaffected by anything on this bus.

### Wiring

| MAX98357A | ESP32-S3 | Note |
|---|---|---|
| Vin | **3V3** | not 5V — see below |
| GND | G | |
| DIN | 40 | |
| BCLK | 42 | |
| LRC | 41 | |
| GAIN | *floating* | default 9 dB |
| SD | *floating* | default (L+R)/2 mono |
| +, − | speaker | bridge-tied load |

**3V3 over 5V.** The DevKitC's 5V pin is raw USB VBUS, shared with the regulator
feeding the MCU. A 2 W amp's current transients on that rail is a brownout that would
present as a mysterious reset. At 3.3 V into 8 Ω the output is still ~0.4 W, which is
loud on a desk. Revisit at Step 10 when the LiPo (3.7 V nominal) makes this a real
decision.

**`SD` must stay floating, not tied to 3V3.** The board's onboard divider sets
(L+R)/2 mono by default. Pulling SD above 1.4 V selects **left channel only** — and if
the data lands in the right slot the result is silence from a correctly wired board.
Don't spend a pin here.

**Speaker polarity does not matter.** The MAX98357A output is bridge-tied: the amp
drives both terminals in antiphase and *neither* is ground. Swapping the leads inverts
the waveform, which is inaudible. Polarity only matters for stereo pairs.

### IDF v6 API

`driver/i2s.h` was **removed** in IDF v6. Any tutorial calling `i2s_driver_install()`
predates v5 and will not compile. The current path is:

```
i2s_new_channel()  →  i2s_channel_init_std_mode()  →  i2s_channel_enable()
                   →  i2s_channel_write()
```

Header is `driver/i2s_std.h`. `main/CMakeLists.txt` may need `esp_driver_i2s` in
`REQUIRES`.

**`chan_cfg.auto_clear = true` is not optional.** The default is false, which means an
underrun replays whatever was last in the DMA buffer — a loud repeating fragment
rather than silence. With `auto_clear`, an underrun is a gap. It also gives idle
silence for free: leave the channel enabled permanently and simply don't write when
nothing is playing, which avoids the enable/disable click entirely.

### Sine, not square, for the first tone

A square wave already sounds buzzy, so it cannot distinguish "correct" from "clipping"
or "phase discontinuity." A sine's failure modes are each audible and distinct:
warble → clock config, crackle → underrun or loose speaker lead, harshness → clipping.

5b switches to square, which is what both the CHIP-8 sound timer and the DMG square
channels actually want.

Amplitude 3200 out of 32767 (~10%) — deliberately low to leave clipping headroom while
the signal path was unproven.

### Results

```
I (1588) audio: generated 22050 samples in 404963 us
I (2538) audio: wrote 44100 bytes in 944114 us (expect ~1000000)
```

Audible tone, one second, correct duration.
### Finding 1: `sinf` costs ~18 µs per sample

**404,963 µs to generate 22,050 samples.** On a 160 MHz core with a single-precision
FPU that is enormous — roughly 2,900 CPU cycles per call. Likely a flash cache miss
per call, or a fallback to a software double-precision path.

**Consequence: transcendental generation cannot live in a real-time audio loop.**
Filling even a 512-sample DMA block would cost ~9.2 ms, against an 11.6 ms playback
window for that block at 22050 Hz — a 79% CPU load to produce a single sine wave.

This is why 5b uses either a precomputed wavetable or integer square waves. It is also
a preview of the Phase 2 constraint: the DMG APU has four channels, and none of them
can afford a float per sample.

Worth noting the shape of this: it's the same lesson as Step 3's byte-swap decision.
Anything per-sample or per-pixel must be a table lookup, not a computation.

### Finding 2: `i2s_channel_write` returns before the audio finishes

944,114 µs against an expected 1,000,000 µs looks like a 5.6% sample-rate error. It
isn't. **The call blocks until the last sample is handed to the DMA buffers, not until
it reaches the speaker.** The ~56 ms shortfall *is* the DMA buffer depth still
draining.

Same distinction as Step 3's `esp_lcd_panel_draw_bitmap`: there are two meaningful
timestamps, "CPU is free" and "the peripheral is done," and they are not the same
number. Reading the return as "playback complete" would have led to a phantom
sample-rate bug.

The genuine sample-rate check is the 440/880 pitch A/B, not this timing.

### Diagnostic split, for later

The `wrote ... us` figure is the fault-isolation instrument for this subsystem: if it
reports ~1 s, the peripheral and clock ran correctly and any silence is downstream
(wires, terminal, speaker). If it returns far too fast, the fault is in the config.
One log line splits the candidate faults into two halves.

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

**Buttons on RTC-capable GPIOs (4–9).**
Costs nothing now, preserves deep-sleep wake for Step 10. See Step 4 above.

**Audio on non-RTC GPIOs (40–42).**
The complement of the button decision. I2S never needs to wake the chip, so it spends
the pins that can't. Keeps the whole RTC-capable 4–9 block free for the six-button
expansion. See Step 5a.

**Amp powered from 3V3, not the 5V rail.**
Avoids putting class-D current transients on the raw VBUS rail shared with the MCU
regulator. Costs output power (~0.4 W instead of ~1 W) and buys a brownout that never
happens. Reopen at Step 10 with the LiPo.

**Debounce by sample count, not by elapsed time.**
Survives frame-rate changes without retuning. Phase 2 will change frame rate.

**Active-low carried through to the input layer, not normalized.**
The DMG joypad register reads 0 for pressed. Normalizing here would mean inverting
back in Phase 2.

---

## Gotchas (learned the hard way)

### Instruments

- **A multimeter probe cannot reach a breadboard clip.** The spring contact sits several
  mm below the surface and the hole is sized for a ~0.6 mm pin; a ~2 mm probe cone wedges
  in the plastic funnel and stops short. **Bare holes always read open, regardless of
  what's plugged in.** Seat a jumper and probe its exposed end instead. This produced a
  false "the legs aren't seated" diagnosis that survived hours of debugging.
- **Calibrate the meter before trusting a number.** Touch the probe tips together and
  read the value — that's lead resistance and the floor. Measured **004** here, so
  anything reading below 004 is noise, not a better connection. On this meter (ANENG
  DM850) an open circuit displays a bare **`1`** on the left of the screen, not `OL`.
- **Both probes on the same wire measures the wire, not the circuit.** Beeping proves
  the jumper is continuous end to end. To test whether two nodes are connected, each
  probe must be on a *different* conductor with the component between them.
- **Banana plugs work loose.** A marginal lead reads as a dead short from anything to
  anything, and it comes and goes as the leads move. Push both plugs fully home, then
  re-baseline: probes apart must be silent, probes together must beep. If it beeps in
  open air, stop — nothing downstream means anything.
- **Probe geometry matters.** Holding both probes in one hand with the tips converging
  lets them touch each other. One probe per hand, approaching from opposite directions,
  tips well apart.
- **Flash before you meter.** The MCU settled in thirty seconds what the meter couldn't
  in three hours. A configured GPIO with a known pull-up gives an unambiguous binary
  with no probe geometry, no lead seating, and no dial position to get wrong. For
  anything on a GPIO, the microcontroller is the better instrument; reach for the meter
  only for nodes the MCU can't see — the speaker coil and a Vin/GND short being the two
  real examples so far.
- **`ESP_LOGI` is a blocking instrument and it sets the resolution floor.** Printing
  inside a sampling loop means the sample period is the UART write time, not the loop
  time. Measured ~82 µs on this build. Suspiciously regular intervals in timing data
  are the tell — mechanical events don't repeat to the microsecond. To resolve finer,
  buffer timestamps in RAM and print after the window closes.
- **A test that cannot fail is not a test.** Before trusting a passing measurement,
  break it deliberately — set the debounce window to zero, sample slower than the
  event, or change the tone frequency and check the pitch moves — and confirm the
  result changes. If it doesn't, the test isn't measuring what it claims to.

### Wiring

- **Read the silkscreen label, don't count header positions.** The DevKitC-1's left edge
  runs 3V3, 3V3, RST, 4, 5, 6… so counting to the fourth pin lands several GPIOs off
  from the pin labelled `4`. A ground jumper that was one position off the `G` pin
  caused a "shorting to ground does nothing" result that looked like a dead pin.
- **SPI to the display is write-only — there is no back-channel.** A wrong DC pin,
  swapped MOSI/SCK, wrong SPI host, or a bad init sequence all produce the identical
  symptom: blank screen, zero error output. The chip cannot tell the panel didn't
  hear it.
  Cheap partial test with no meter: move the display's LED wire to 3V3. If the
  backlight lights, VCC/GND/panel are confirmed alive — 3 of 9 wires validated,
  and the fault is isolated to the remaining 6.
- **I2S is write-only too, and fails the same silent way.** A swapped BCLK/LRC, a wrong
  slot mode, or a floating DIN all give silence with no error. This is why Step 5 was
  split into transport-first and integration-second.
- **Charge-only USB cable.** Board powers up with a solid red LED and never enumerates
  in Device Manager — looks like a driver problem, isn't. Known-good data cable is now
  labeled. Also: connect the board directly to the laptop, not through the USB hub.
  If `idf.py flash` reports "the system cannot find the file specified" on COM3, the
  port doesn't exist — the board isn't enumerating. Check the **UART** USB connector,
  not the native USB one.


### Firmware

- **`rgb_ele_order`** wrong = image renders with red and blue swapped. Distinctive and
  self-diagnosing, unlike most display faults. ILI9341 wants `BGR`.
  `spi_mode` must be 0; wrong mode garbles every byte.
- **`ESP_ERROR_CHECK` on every init call.** Any failure panics with file, line, and
  error code. Without it a failed init just returns and you debug a blank screen with
  no information.
- **`driver/i2s.h` no longer exists in IDF v6.** Use `driver/i2s_std.h` and the
  channel API. Anything calling `i2s_driver_install()` is pre-v5 and won't compile.
- **`auto_clear` defaults to false, and false is the wrong answer.** An underrun
  replays the stale DMA buffer — a loud repeating fragment instead of silence.
- **`i2s_channel_write` returns when the DMA buffers are fed, not when the sound
  stops.** Measured a ~56 ms gap on a 1 s tone. Don't read the return as playback
  completion; it looks exactly like a sample-rate bug.
- **`sinf` is ~18 µs per call here.** Fine for a one-off table build at init, fatal in
  a per-sample loop. Precompute.
- **`idf.py add-dependency` only writes `main/idf_component.yml`.** The component manager
  doesn't fetch until CMake reconfigures, so on an already-built project you get
  "No such file or directory" on the new header. **Always `idf.py fullclean` after
  adding a dependency.** Cost me time twice.
- **`idf.py` needs the project root.** It looks for `CMakeLists.txt` in the current
  directory; running it anywhere else fails with "CMakeLists.txt not found."
- **Code placed after the game loop's `while (1)` is unreachable and produces no
  warning.** A diagnostic block appended to the bottom of `app_main` silently never
  runs. The tell is in the log timestamps: if the first `perf` line lands ~1.5 s after
  boot, nothing before the loop took any time. Temporary instrumentation goes *above*
  the loop.
- **A tight polling loop starves IDLE0 and trips the task watchdog at exactly 5 s.** It
  fires on a free-running timeout, repeating every 5 s, and has nothing to do with what
  the code is doing at that moment — chasing it as if it correlates with an input event
  wastes time. Panic is off by default, so it's log noise and no reboot. Cannot be fed
  from the offending loop: it's the *idle* task being starved, not this one. Fix by
  keeping the window under ~4 s, or accept the noise.
- **`vTaskDelay(1)` is 10 ms at the default tick rate.** Far too coarse to sample sub-
  millisecond events. Any loop that needs to observe bounce cannot yield.
- **`pin_bit_mask` is `uint64_t` — always shift `1ULL`.** `1 << 38` on a 32-bit int is
  undefined behaviour that compiles without warning.
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
8 MB PSRAM, which is affordable. Step 4's render-serialization measurement is the second
independent reason to do it.

**Isolate by bypass.** The move that resolved both the display and the input bring-up:
when a chain of components produces one ambiguous symptom, remove the suspect component
and test the path around it. Shorting a GPIO straight to ground tests pin + pull-up +
jumper with the switch out of the circuit entirely — one variable, and it splits the
candidate faults cleanly. Same shape as moving the display's LED wire to 3V3 to validate
3 of 9 wires, and as reading the `wrote ... us` figure to split I2S faults into
peripheral-side vs. speaker-side.

**Frame timing is deterministic to the microsecond.** `49999 us` repeated across every
logged frame, matching the ~0.02% jitter seen in Step 3. Useful property: A/B changes of
even a few percent are measurable here without averaging.

**Everything per-unit must be a lookup.** Three subsystems have now hit the same wall
from different directions: the RGB565 byte swap (per pixel), the palette path (per
pixel), and `sinf` (per sample). The general rule for this platform is that anything
executing once per pixel or once per sample has to be a table, not a computation.

---

## Open questions

- [ ] Try 80 MHz. Bus floor would be 15.4 ms (~65 fps), but that's well past what
      breadboard wiring is likely to hold. Worth finding the ceiling and writing it down.
      (Audio is unaffected — BCLK is 705.6 kHz, two orders below.)
- [ ] Double buffering: now argued from two independent measurements (Step 3
      extrapolation, Step 4 render serialization). Predicted gain ~1.20× at 40 MHz.
      Implement and confirm against the prediction.
- [ ] Does partial-region update beat full-frame for mostly-static screens?
      DMG is 160×144 — scaled into 240×320 that's a lot of unchanged border.
- [ ] Resolve bounce fine structure below 82 µs: buffer timestamps to a RAM array in the
      sampling loop, print after the window closes. Would establish whether the observed
      ~165 µs burst is two transitions or many.
- [ ] Re-measure bounce on the four additional switches when the six-button set is wired.
      `DEBOUNCE_SAMPLES 2` is sized for unmeasured parts, not for the two characterized
      ones.
- [ ] Why is `sinf` ~18 µs? Flash cache miss per call, or a software double-precision
      fallback? Worth ten minutes with a table-based comparison — the answer generalizes
      to every float in the Phase 2 hot path.
- [ ] Measure the audio top-up cost per frame in 5b, and decide whether audio belongs on
      the second core. Step 4's decomposition has room, but the DMA buffer must not
      underrun while the render pass is serialized.

---

## Step log

- **Step 1 — Toolchain + blink.** ✅ Aug 1 2026. ESP-IDF v6.0.2 build → flash → monitor
  proven end-to-end. RGB LED on GPIO38 via `led_strip` over the RMT peripheral.
- **Step 2 — Display bring-up.** ✅ Aug 1 2026. ILI9341 cycling full-screen R/G/B over SPI.
- **Step 3 — PSRAM framebuffer + DMA.** ✅ Aug 2 2026. 4.62× faster, 95% of CPU recovered.
- **Step 4 — Input.** ✅ Aug 5 2026. Two buttons on GPIO 4/5, internal pull-up, active
  low, polled at frame rate with 2-sample debounce. Square moves under button control.
  Bounce characterized: ~165 µs, release edge only, ~27% of presses, resolution floor
  ~82 µs set by the logging instrument. Poll cost 2 µs. Render pass measured at 8.2 ms
  serialized — second independent argument for double buffering. Most of the elapsed
  time went to instrument error, not circuit error — see Gotchas → Instruments.
- **Step 5a — Audio transport.** ✅ Aug 6 2026. MAX98357A on I2S, BCLK=42 / WS=41 /
  DIN=40, powered from 3V3, GAIN and SD floating. Header and screw terminal
  hand-soldered (first solder work on the project). 1 s 440 Hz sine plays; 440→880 A/B
  confirms requested frequency is delivered frequency, which validates clock, sample
  rate, slot mode, and wiring as a chain. Two findings: `sinf` costs ~18 µs/sample
  (405 ms for 22050 samples) so per-sample float generation is off the table, and
  `i2s_channel_write` returns ~56 ms before the sound stops because it blocks on DMA
  buffer handoff, not on playback — which looks exactly like a sample-rate bug and
  isn't. Passive-buzzer sanity check was deliberately skipped: LEDC PWM shares nothing
  with the I2S path.
- **Step 5b — Audio integration.** ✅ Aug 6 2026. Audio moved into the game loop:
  square wave (integer compare, no `sinf`), triggered on the button *press edge* via a
  new `btn_went_down()`, non-blocking `i2s_channel_write` (timeout 0) topped up once per
  frame. Persistent phase accumulator carries the waveform across frame boundaries — no
  seam click, confirmed by ear. Generation cost 94 us/beep-frame, ~1 us idle (vs. ~10 ms
  the same samples would cost via `sinf`) — the square-wave-not-`sinf` argument now
  measured on both sides. An 80 ms beep spreads cleanly across ~2 frames; the DMA buffer
  never ran dry (accepted count held at 1128 B, below the offered chunk), so the feared
  1.3× cushion is comfortable for short beeps. Clean beeps, no click, no stutter under
  rapid presses. Second-core audio task (Step 7) left unbuilt — no measurement justifies
  it. **Phase 1 Stage A core subsystems (display, framebuffer/DMA, input, audio) are now
  all proven.**

