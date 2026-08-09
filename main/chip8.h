#ifndef CHIP8_H
#define CHIP8_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CHIP8_MEM_SIZE   4096
#define CHIP8_DISPLAY_W  64
#define CHIP8_DISPLAY_H  32
#define CHIP8_ROM_START  0x200
#define CHIP8_FONT_START 0x50

#define CHIP8_ROM_MAX    (CHIP8_MEM_SIZE - CHIP8_ROM_START)

typedef struct {
    uint8_t  mem[CHIP8_MEM_SIZE];
    uint8_t  V[16];
    uint16_t I;
    uint16_t pc;

    uint16_t stack[16];
    uint8_t  sp;

    uint8_t  delay_timer;
    uint8_t  sound_timer;

    uint8_t  display[CHIP8_DISPLAY_W * CHIP8_DISPLAY_H];
    bool     keys[16];

    /* Fault reporting. The core never prints — printing costs ~82 us per
       call over UART on the ESP32 target, and an unimplemented opcode
       inside a loop would drag the frame clock. The frontend polls these,
       reports once, and clears bad_op. */
    bool     bad_op;
    uint16_t bad_opcode;
    uint16_t bad_pc;
} Chip8;

void chip8_init(Chip8 *c);

/* Copy a ROM image already in memory. Rejects len > CHIP8_ROM_MAX rather
   than truncating. The only load path the core knows about. */
bool chip8_load(Chip8 *c, const uint8_t *data, size_t len);

void chip8_cycle(Chip8 *c);

/* Decrement delay and sound timers toward zero. Call at 60 Hz. */
void chip8_tick_timers(Chip8 *c);

#endif