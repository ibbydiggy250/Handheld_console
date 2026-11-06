# Engineering Notes — Handheld Console (Phase 1)

Measurements, constraints, and the decisions they forced.

---

## Platform

| | |
|---|---|
| MCU | ESP32-S3-DevKitC-1 **v1.1**, N8R8 |
| Flash / PSRAM | 8 MB / 8 MB octal (quad is the IDF default and silently fails on N8R8) |
| CPU | 160 MHz, dual core |
| Display | HiLetgo ILI9341, 240×320, SPI, landscape 320×240 |
| Audio | MAX98357A I2S → 28 mm 8 Ω speaker |
| Framework | ESP-IDF v6.0.2, `esp_lcd` + `espressif/esp_lcd_ili9341` v2.0.2 |

**Pin map**

| Function | Pins |
|---|---|
| Display (SPI2) | SCK=12, MOSI=11, MISO=13, CS=10, DC=14, RST=8, BL=9 |
| Buttons | 4, 5, 6, 7 — internal pull-up, switch to ground, active low |
| Audio | LRC=40, BCLK=41, DIN=42, Vin=3V3, GAIN/SD floating |
| Onboard WS2812 | **GPIO38**, not 48 (v1.1 board rev) |
| Unusable | 26–37 (octal PSRAM), 0/3/45/46 (strapping), 19/20 (USB) |

11/12/13 are SPI2's IO_MUX defaults — bypasses the GPIO matrix.
SD card (Step 6) joins the same SPI bus; only its CS is new.

---

## Measurements

### Display / DMA

| | Line-by-line @10 MHz | Full-frame @10 MHz | Full-frame @40 MHz |
|---|---|---|---|
| Wall clock | 187.9 ms | 130.7 ms | 40.7 ms |
| Bus floor | 122.9 ms | 122.9 ms | 30.7 ms |
| CPU busy | ~188 ms (pinned) | 5.86 ms | 5.83 ms |
| CPU free | ~0% | 95.5% | 85.7% |

Frame = 240×320×2 = **153,600 B**, whole payload every frame (ILI9341 has no addressable memory).

- One transaction instead of 320 → **1.44×**. Per-row `draw_bitmap` is ~7 SPI transactions each (~203 µs/line).
- 10 → 40 MHz → **3.21×**. Combined **4.62×**.
- Changed one variable at a time so each speedup is attributable.

**The ~5.8 ms fixed CPU cost is cache-writeback bound, not clock bound.** Predicted flat across clocks, confirmed: 5859 → 5832 µs while transfer got 3.2× faster. Measured a third time at 5828 µs (Step 11d) and a fourth at 5818 µs (Step 4). At 80 MHz this puts CPU-free at ~71% — the double-buffering argument as a number.

### Frame decomposition (Step 4)

```
input 2 us | render 8216 us | cpu 14034 us | frame 49999 us
```

Render is **serialized**, not overlapped — single buffer means frame N+1 can't start until DMA finishes reading N. Double buffering predicted: 50.0 → ~41.8 ms, ~1.20×, cost 153,600 B of 8 MB.

Frame timing is deterministic to the microsecond (~0.02% jitter). A/B deltas of a few percent are trustworthy without averaging.

### Buttons

- Bounce: ~165 µs max, **release edge only**, 3 of 11 presses. Resolution floor ~82 µs (`ESP_LOGI` blocking, not the switch — intervals were 81/81/85/84 µs, too regular to be mechanical).
- Honest claim: burst under ~250 µs on release; fine structure unresolved.
- Poll cost: **2 µs** for two buttons including debounce.
- 165 µs against a 50 ms frame is ~300× margin.

### Audio

- **`sinf` = ~18 µs/call** (~2900 cycles). 405 ms to generate 22050 samples. Fatal per-sample; fine for an init-time table.
- Square wave generation: **94 µs/beep-frame**, ~1 µs idle.
- `i2s_channel_write` returned 944 ms on a 1 s tone — it blocks on DMA handoff, not playback. Looks exactly like a sample-rate bug.

### CHIP-8 (Step 11d/e)

- `update` **19–35 µs** — 0.05% of frame. 19/35 alternation is `DXYN` frames vs. not, not noise.
- `render` **8216 → 4938 µs** with the 5× scaler, because the 80 letterbox rows are never touched.
- `work` pins at ~39,988 µs regardless of frame period; 30,720 of it is bus payload. `FRAME_US` 41000 is the floor.

---

## Decisions

- **Framebuffer stored pre-byte-swapped.** Pixel path is palette-index based, so the swap folds into the palette LUT at zero per-pixel cost. Swapping at push time = a 76,800-word CPU pass exactly when DMA is supposed to have freed the CPU.
- **`esp_lcd` over LovyanGFX.** The DMA framebuffer is the portfolio claim; LovyanGFX's sprite system would own that buffer. Still available later for fonts.
- **Backlight on GPIO9, not 3V3.** Largest single power draw (~60–100 mA); Step 10 needs it switchable. Side effect: dark panel at boot is expected.
- **40 MHz SPI**, 2.7× over the datasheet's ~15 MHz. No artifacts on breadboard jumpers.
- **Buttons on RTC-capable GPIOs (4–9).** `esp_sleep_enable_ext1_wakeup()` requires RTC pins; only 0–21 qualify. Preserves deep-sleep wake for Step 10.
- **Audio on non-RTC pins (40–42).** The complement — I2S never needs to wake the chip, so it spends the pins that can't.
- **Amp on 3V3, not 5V.** 5V is raw VBUS shared with the MCU regulator; class-D transients there are a brownout. Costs ~0.4 W vs ~1 W. Reopen at Step 10.
- **Debounce by sample count (2), not elapsed time.** Survives frame-rate changes without retuning — and Phase 2 will change frame rate.
- **Active-low carried through to the input layer.** The DMG joypad register also reads 0 for pressed; normalizing here would mean inverting back in Phase 2.
- **Interrupts rejected for buttons.** Bounce fires an edge burst, debounce has to live somewhere either way, and the game loop already runs on a frame clock.
- **Per-ROM keypad mapping in `rom_t`.** CHIP-8 defines no key convention, so the mapping belongs to the ROM. Adding a game is one line plus a header.
- **Step 7 (dual-core) closed twice on measurement.** Audio at 94 µs, `update` at 19–35 µs. Nothing justifies it.

---

## Gotchas

### Instruments

- **A multimeter probe cannot reach a breadboard clip.** ~2 mm cone wedges in the funnel; bare holes always read open. Probe a seated jumper's exposed end instead. Cost hours and a false diagnosis.
- **Flash before you meter.** A configured GPIO with a known pull-up is an unambiguous binary. Reach for the meter only for nodes the MCU can't see (speaker coil, Vin/GND short).
- **Calibrate first.** Probes together = lead resistance floor (004 here). ANENG DM850 shows a bare `1` for open, not `OL`.
- **Both probes on the same wire measures the wire, not the circuit.** Loose banana plugs read as a dead short from anything to anything.
- **`ESP_LOGI` sets the resolution floor (~82 µs here).** Suspiciously regular intervals in timing data are the tell. Buffer timestamps in RAM, print after the window.
- **A test that cannot fail is not a test.** Break it deliberately and confirm the result changes.
- **Isolate by bypass.** Short the GPIO to ground to test pin + pull-up + jumper with the switch out. Move the display's LED wire to 3V3 to validate 3 of 9 wires.

### Wiring

- **Read the silkscreen, don't count header positions.** Left edge is 3V3, 3V3, RST, 4, 5, 6… Counting to the fourth pin lands several GPIOs off.
- **SPI and I2S are both write-only — no back-channel.** Wrong pin, wrong mode, bad init all give the identical symptom (blank screen / silence, zero error output). This is why every subsystem was proven transport-first.
- **Tactile switch: wire two DIAGONALLY OPPOSITE legs.** Only orientation-independent rule. Both jumpers on one terminal reads a constant 0. A terminal spanning the trench makes that whole row live on both halves.
- **Charge-only USB cable** = solid red LED, never enumerates. Looks like a driver problem. Also: plug direct, not through a hub, and use the **UART** connector, not native USB.
- **Display whiteouts were mechanical**, not firmware — the module cantilevers off the breadboard edge with all 14 connections on one short edge.

### Firmware

- **`driver/i2s.h` is gone in IDF v6.** Use `driver/i2s_std.h` + the channel API. `i2s_driver_install()` is pre-v5.
- **`auto_clear` defaults to false, and false is wrong.** Underrun replays the stale DMA buffer — a loud repeating fragment instead of silence.
- **`idf.py fullclean` after every `add-dependency`.** The component manager doesn't fetch until CMake reconfigures. Cost time twice.
- **`pin_bit_mask` is `uint64_t` — shift `1ULL`.** `1 << 38` is UB that compiles clean.
- **Framebuffer must be `MALLOC_CAP_SPIRAM` explicitly.** IDF reserves only 32 KB internal for DMA. Log the pointer: PSRAM is `0x3C…`, internal `0x3FC…`. Keep 64-byte alignment.
- **`vTaskDelay(1)` is 10 ms** at the default tick rate.
- **Code after the game loop's `while (1)` is unreachable, no warning.** Instrumentation goes above the loop.
- **A tight polling loop starves IDLE0 and trips the watchdog at exactly 5 s**, free-running — it doesn't correlate with whatever the code is doing. Log noise only; panic is off by default.
- **`xSemaphoreTake(..., portMAX_DELAY)` waits forever.** A callback that never fires reads like a crash.
- **`assert()` compiles to nothing under `NDEBUG`**, which IDF defines in release.
- **`ESP_ERROR_CHECK` on every init call** — otherwise a failed init returns silently.
- **`rgb_ele_order` wrong = red/blue swapped** (ILI9341 wants BGR). `spi_mode` must be 0.
- **`swap_xy` alone is a transpose, not a rotation** — needs a mirror on one axis. Both write MADCTL, so they must come *after* `panel_init()`. A mirrored state survives symmetric content unnoticed; validate orientation against asymmetric content only.
- **VS Code squiggles on IDF headers are cosmetic.** Point `C_Cpp.default.compileCommands` at `build/compile_commands.json`.

### General

**Everything per-unit must be a lookup.** Three subsystems hit this from different directions: RGB565 byte swap (per pixel), palette (per pixel), `sinf` (per sample).

---

## Open questions

- [ ] 80 MHz SPI. Bus floor 15.4 ms (~65 fps); likely past what breadboard wiring holds. The ceiling is itself a finding and a PCB argument.
- [ ] Double buffering. Predicted ~1.20× at 40 MHz from two independent measurements. Implement and check against the prediction.
- [ ] Partial-region update for mostly-static screens — DMG is 160×144 in 320×240, a lot of unchanged border.
- [ ] `TIMER_TICKS_PER_FRAME` truncates 2.46 → 2, so CHIP-8 timers run 48 Hz not 60. Known deviation, unfixed.
- [ ] Why is `sinf` 18 µs — flash cache miss or software double fallback? Generalizes to every float in the Phase 2 hot path.
- [ ] Re-measure bounce on the remaining switches; `DEBOUNCE_SAMPLES 2` is sized for unmeasured parts.
- [ ] Tearing will be a real artifact in Phase 2 during scrolls. Double buffering is the likely answer.

---

## Step log


### Phase 1
- **1 — Toolchain + blink.** ✅ Aug 1. Build → flash → monitor proven. WS2812 on GPIO38 via `led_strip`/RMT.
- **2 — Display.** ✅ Aug 1. ILI9341 cycling full-screen R/G/B over SPI.
- **3 — PSRAM framebuffer + DMA.** ✅ Aug 2. 4.62× faster, 95% of CPU recovered.
- **4 — Input.** ✅ Aug 5. Buttons on GPIO 4/5, pull-up, active low, 2-sample debounce at frame rate. Bounce characterized; render measured at 8.2 ms serialized. Most of the elapsed time went to instrument error, not circuit error.
- **5a — Audio transport.** ✅ Aug 6. MAX98357A on I2S; 1 s 440 Hz sine, 440→880 A/B validates clock, sample rate, slot mode, and wiring as a chain. First solder work on the project.
- **5b — Audio integration.** ✅ Aug 6. Square wave, press-edge triggered, non-blocking write topped up once per frame. Persistent phase accumulator — no seam click. DMA buffer never ran dry.
- **8 — Game loop.** ✅ Aug 6. Fixed timestep, `FRAME_US 50000`, **absolute** deadline (`next_frame += FRAME_US`) so a long frame is followed by a short one with no drift; overrun resyncs to `now`. `update()`/`render()` split is the Step 11 socket. Validated by A/B at 60000, since at 50000 DMA already paces the loop — a test that cannot fail. Four buttons live.
- **9 — Snake.** ✅ Aug 6. Dropped into the Step 8 seam; frame clock, input, audio, DMA untouched. New patterns: move-tick gate (`MOVE_EVERY 4`, since 20 fps movement is unplayable), direction latch consumed at the move tick with 180° rejected against the *consumed* direction, circular-buffer body (O(1) per move). Any-button restart off edge detection so the killing press can't restart.
- **11a — Core split.** ✅ Aug 8. `fopen`/`fread` moved to `chip8_file.c` behind a separate header, so embedded misuse is a link error. `fprintf` replaced with `bad_op`/`bad_opcode`/`bad_pc` fields the frontend polls (~82 µs × 24 cycles/frame would have read as "the emulator is slow"). Timers into `chip8_tick_timers()`. Per-frame counts restated as rates. Bounds check on load.
- **11b — Landscape.** ✅ Aug 8. `swap_xy(true)` + `mirror()` after `panel_init()`. 320/240 = same 153,600 B reinterpreted. Enables 5× integer scale vs 3× in portrait.
- **11c — ROM in flash.** ✅ Aug 8. `pong.ch8` (246 B) as a `static const` array via `od`/`sed` (no `xxd` in MSYS2). Verified on its own flash first: first two bytes `6A 02` decode as V10=2, Pong's first paddle coordinate — checkable, not merely plausible. Confirms Step 6 is not a CHIP-8 prerequisite.
- **11d — Core in the loop.** ✅ Aug 9. Snake deleted, core dropped into the seam, signatures unchanged. 5× nearest-neighbour scaler, letterbox painted once. `FRAME_US` 50000 → 41000 spent 10 ms of measured slack: 24.4 fps, latency 50 → 41 ms, one-line edit because the rates derive. **It also fixed the XOR flicker where a persistence buffer only masked it** — raising the rate narrows the gap being sampled; decay 3–4 traded flicker for smear.
- **11e — ROM table + menu.** ✅ Aug 9. `rom_t` + 40-glyph 5×7 hand-built font; menu repaints only on selection change. Selection locked updatuntil reset — all four buttons are consumed by the emulated keypad. Tetris (494 B) runs at `update` 15–33 µs. Its silence confirmed *correct* by instrumenting `sound_timer` rather than assuming a bug — the ROM never writes `FX18`. **Phase 1 Stage A complete.**

**Deferred:** SD — only needed for Phase 2 DMG ROMs, dual core — closed twice on measurement, power — pending LiPo parts.

### Phase 3

- **3.0 — Save state serializer.** ✅ Aug 18. Platform-agnostic `chip8_state.c`: magic + version + 16 B ROM name + machine state, explicit LE field-by-field (never a struct `memcpy` — padding/endianness differ across ESP32 → Postgres → WASM), 4429 B with the display bitpacked 2048 → 256. Excludes `keys` (repolled), `bad_op` (transient), phosphor buffer (frontend-side). Deserialize validates and decodes into a scratch `Chip8` so a bad blob can't half-write the live one — 6 KB of stack, revisit against the ESP32 task stack at 3.5. Blob carries its own `mem`, so it restores with no ROM argument — the property the web app needs. **Found on the way: the laptop build had been broken since 11a** — `chip8_load_rom` deleted from `chip8.c`, `chip8_file.c` never written, every validation since ran on hardware. Implicit declaration is only a warning; it failed at link. Added `-Werror=implicit-function-declaration`. Desktop-proves-first only holds if the desktop build is still built.
- - **3.1 — WiFi station.** ✅ Aug 18. Non-blocking `esp_wifi` STA in `wifi.c`; `wifi_start()` returns with the radio scanning and association completes on the event task, so the game loop never waits on the radio. Credentials in a gitignored `wifi_secret.h`. `threshold.authmode = WPA2_PSK` over the IDF default (OPEN), which would let the driver associate to a rogue open AP with the same SSID. **WiFi cost is unmeasurable in the frame budget** — `work` held ~39,989 µs through scanning and association, so the core-pinning question closed before it needed answering, third time a step closed on measurement. Google Wifi's single SSID is a non-issue: band steering puts 2.4 GHz-only clients on 2.4 GHz, and the usual mesh failure is phone-app provisioning, which firmware with a hardcoded SSID skips entirely. Two gotchas: **adding any `REQUIRES` to `main` disables the implicit common-component set**, so every include must then be listed explicitly — presents as a working build failing on headers you didn't touch. And a header-only edit to `wifi_secret.h` didn't take; `idf.py fullclean` was needed. Disconnect `reason` code is the whole diagnostic: 201 = AP not found, 15 = wrong password.
- **3.3a–c — Supabase table, bucket, RLS.** ✅ Nov 5. Schema in `backend/schema.sql`: `save_states` (path `{user_id}/{game_id}/{ts}.c8st`, `game_id` = sha256 of ROM bytes, index on `(user_id, game_id, created_at desc)` for 3.7's "latest"), private `saves` bucket (1 MB cap), RLS keyed on `auth.uid()` for rows and on the first path segment for objects. Insert also requires `path` to start with the caller's uid, so a row can't point into another user's folder. **No update policy** — history is immutable by construction. Validated by curl with two users, **12/12**: the real `state.c8st` (4429 B, from the laptop F5) round-trips byte-identical (`cmp`); A sees its 1 row; B sees `[]`; B reading A's blob → `NoSuchKey`; B writing into A's folder → `AccessDenied`; A forging `user_id` or a path in B's folder → `42501`; no user JWT → `[]` / `NoSuchKey`; A's PATCH on its own row affects 0 rows. Denials were checked by error code, not just status — and B's token was confirmed live via `/auth/v1/user` first, otherwise every negative passes trivially. **Gotchas:** RLS *filters* reads (200 + `[]`), it doesn't 403 — an empty list is only evidence alongside a positive test. **Storage returns HTTP 400 for both denied and missing objects**; the real status is in the body's `statusCode` (403/404) — firmware must parse the body, not branch on the HTTP code. Windows PowerShell's `curl` is `Invoke-WebRequest`; run curl from Git Bash.
- **3.3d — Server-minted session spike.** ✅ Nov 5. **Answer: yes.** An Edge Function with the service-role key does `admin.generateLink({type:'magiclink'})` (no email sent) → `verifyOtp({token_hash})` and gets a full session for that user — the 3.4 pairing path works without the device ever holding a password. Spike source in `backend/spike/`; it mints only for the *caller* (identity from their JWT, body ignored), so it can't be used to become someone else. 11/11: minted session is new, authenticates as A, reads A's saves; B asking for A's email gets B; no JWT → 401. **Refresh rotation, measured rather than assumed:** each refresh returns a new refresh token; reusing the parent *while its child is still unused* returns the **same child** (not an error) — lost-response recovery; once the child has been used, older tokens fail `400 refresh_token_already_used`; the active token survives those rejected reuses (no family revocation observed). **Consequence for firmware:** a power cut between receiving a new token and the NVS write is recoverable — the old token still yields the same new one as long as it hasn't been used. Persist-after-every-refresh stays the rule; never refresh from two places at once. **Gotchas:** the dashboard's display name isn't the URL — the function was named `mint-session-spike` but deployed at slug `smart-processor`; copy the endpoint, don't guess it. And the first run reported 8/8 with check 6 *failing*: a `$(…)` inside the `check` label ran after `[ ]` and reset `$?` to 0 — the label read `200: None` while saying PASS. Capture `r=$?` immediately after the test.