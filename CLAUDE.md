# CLAUDE.md — Handheld Console (ESP32-S3 + CHIP-8 → Game Boy + Cloud Saves)

Engineering log with every measurement and decision: @NOTES.md
Read the relevant section of NOTES.md before touching a subsystem.

## What this is

A handheld built from a bare ESP32-S3, running a platform-agnostic C emulator core,
syncing save states to a Supabase backend with a React frontend.

- **Phase 0** — CHIP-8 on laptop: DONE
- **Phase 1** — Hardware + firmware: Stage A DONE (display, DMA framebuffer, input, audio,
  game loop, CHIP-8 port + ROM menu). Step 10 (battery) waiting on parts. Step 6 (SD) deferred.
- **Phase 3** — Cloud saves: IN PROGRESS (pulled ahead of Phase 2; backend stores opaque blobs)
- **Phase 2** — Game Boy DMG emulator: not started. Laptop first, then port.

## Repos

- Handheld firmware: `C:\esp\projects\handheld` (ESP-IDF v6.0.2)
- CHIP-8 laptop build: `chip8\dev\` (MSYS2 mingw64 gcc, raylib, mingw32-make)
- Build/flash: `idf.py -p COM3 flash monitor` from the ESP-IDF PowerShell, run from project root
- Laptop build has `-Werror=implicit-function-declaration` — it was once silently broken for weeks

## Hard rules

1. **Core is platform-agnostic C.** `chip8.c`, `chip8_state.c/.h` contain no raylib, no ESP-IDF,
   no `printf`. `chip8_file.c` is host-only and **never** goes in the handheld build.
2. **One variable per step.** Each step closes only when validated by measurement or a test.
   Don't combine changes; don't move to the next step on an unvalidated one.
3. **Measure before claiming.** Performance claims need a number. Prefer a test that can fail.
4. **Never block the game loop on the network.** Network work lives in the `net.c` FreeRTOS task.
5. **No second copy of unvalidated code.** `chip8_state.c/.h` gets copied into `handheld/main/`
   at Phase 3 Step 3.5, not before.
6. **Per-pixel / per-sample work must be a table lookup**, not a computation (`sinf` ≈ 18 µs here).
7. **Hand-written by Ibrahim** (explain and review, don't generate unless asked): opcode flag
   semantics, MMU routing, interrupt dispatch, PPU, MBC banking, save-state serialization.
   Scaffolding and mechanical tables are fine to generate.

## Hardware (ESP32-S3-DevKitC-1 v1.1, N8R8)

- 160 MHz dual core; 8 MB flash; 8 MB **octal** PSRAM (quad silently fails)
- GPIO 26–37 owned by PSRAM. Avoid strapping pins 0/3/45/46, USB 19/20, 47/48 (1.8 V).
- Onboard RGB LED: GPIO38 (not 48 on v1.1)
- Display ILI9341, SPI2_HOST @ 40 MHz: SCK 12, MOSI 11, MISO 13, CS 10, DC 14, RST 8, BL 9.
  Landscape 320×240 (`swap_xy` + `mirror` after `panel_init`). RGB565 stored pre-byte-swapped.
- Framebuffer: `heap_caps_aligned_alloc(64, 153600, MALLOC_CAP_SPIRAM)` (PSRAM addr 0x3C…)
- Buttons: GPIO 4–7, internal pull-up, active-low (matches DMG joypad), 2-sample debounce
- Audio: MAX98357A I2S — WS/LRC 40, BCLK 41, DIN 42, 3V3 supply, GAIN/SD floating, `auto_clear = true`
- Frame clock: `FRAME_US 41000` (~24.4 fps), absolute deadline. Fixed DMA/cache cost ≈ 5.8 ms.

## Phase 3 status

Done: 3.0 serializer (4,429 B blob, magic `C8ST`, explicit little-endian) · 3.1 WiFi STA
(`wifi.c`, creds in gitignored `wifi_secret.h`) · 3.2 HTTPS GET via cert bundle (`net.c`,
triggered by 4-button chord held 12 frames) · 3.3 Supabase `save_states` + private `saves`
bucket + RLS (`backend/schema.sql`, 12/12 curl tests) and session-mint spike passed (11/11) —
Edge Function `generateLink` → `verifyOtp` works.

Supabase: URL + publishable key in gitignored `backend/sb.env` (`source` it in Git Bash).
Test users `usera@example.com` / `userb@example.com`. Edge Function URL slug ≠ display name.

Remaining:
- **3.4** Device pairing (code flow): `device_codes` table + issue/poll Edge Functions →
  pairing screen + polling in `net.c`, refresh token in NVS → React "Link device" page
- **3.5** Upload: blob first, then metadata row. Queue + retry.
- **3.6** React library view
- **3.7** Pull latest on boot → resume

Decisions:
- `game_id` = hash of ROM bytes
- Keep full save-state history (not latest-only)
- Device sign-in = pairing code flow (no password on device); BLE deferred
- Path convention: `{user_id}/{game_id}/{timestamp}.c8st`
- Requests send `apikey: <publishable key>` + `Authorization: Bearer <user JWT>`
- Refresh tokens rotate — always persist the new one to NVS after a refresh

Blockers before 3.5:
- Panel-recovery chord (LEFT+RIGHT) is a subset of the save chord (all four) — resolve
- Deserialize uses ~6 KB stack — check against the FreeRTOS task stack
- Pairing screen: code alphabet must exist in the 40-glyph menu font; avoid 0/O, 1/I

## ESP-IDF v6 gotchas

- `idf.py fullclean` after `idf.py add-dependency`
- Any explicit `REQUIRES` in `main` disables the implicit component set — list everything
- `driver/i2s.h` is gone; use `driver/i2s_std.h`
- mbedTLS cert bundle: Component config → mbedTLS → Certificates
- Net task needs ≥ 8 KB stack (TLS handshake)
- `pdMS_TO_TICKS(5)` truncates to 0 at 100 Hz tick
- Code after the game loop's `while (1)` is unreachable with no warning — instrument above it
- `pin_bit_mask` is `uint64_t` — shift `1ULL`
- `xSemaphoreTake(..., portMAX_DELAY)` hangs silently if the callback never fires

## Phase 2 gates (when it starts)

- Blargg `cpu_instrs` fully passing before any PPU work
- dmg-acid2 after the PPU; scanline renderer before any FIFO
- v1 scope: SM83, MMU, interrupts/timers, scanline PPU, MBC1 + MBC3 (RTC), joypad, battery
  saves, save states. Cut: APU, MBC5, GBC, FIFO, Mooneye, link cable.

## Working with Ibrahim

- Answer-first, concise. No long preambles.
- Clarify before committing to an answer; research instead of guessing.
- Push back where warranted, without nitpicking.
- If the conversation drifts, bring it back to the current step.
- Update NOTES.md whenever something costs an hour or a measurement is taken.
