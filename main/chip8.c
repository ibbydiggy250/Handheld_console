#include "chip8.h"
#include <stdlib.h>
#include <string.h>

static const uint8_t fontset[80] = {
    0xF0, 0x90, 0x90, 0x90, 0xF0, // 0
    0x20, 0x60, 0x20, 0x20, 0x70, // 1
    0xF0, 0x10, 0xF0, 0x80, 0xF0, // 2
    0xF0, 0x10, 0xF0, 0x10, 0xF0, // 3
    0x90, 0x90, 0xF0, 0x10, 0x10, // 4
    0xF0, 0x80, 0xF0, 0x10, 0xF0, // 5
    0xF0, 0x80, 0xF0, 0x90, 0xF0, // 6
    0xF0, 0x10, 0x20, 0x40, 0x40, // 7
    0xF0, 0x90, 0xF0, 0x90, 0xF0, // 8
    0xF0, 0x90, 0xF0, 0x10, 0xF0, // 9
    0xF0, 0x90, 0xF0, 0x90, 0x90, // A
    0xE0, 0x90, 0xE0, 0x90, 0xE0, // B
    0xF0, 0x80, 0x80, 0x80, 0xF0, // C
    0xE0, 0x90, 0x90, 0x90, 0xE0, // D
    0xF0, 0x80, 0xF0, 0x80, 0xF0, // E
    0xF0, 0x80, 0xF0, 0x80, 0x80, // F
};

void chip8_init(Chip8 *c)
{
    memset(c, 0, sizeof(*c));
    c->pc = CHIP8_ROM_START;
    memcpy(c->mem + CHIP8_FONT_START, fontset, sizeof(fontset));
}

bool chip8_load(Chip8 *c, const uint8_t *data, size_t len)
{
    if (!data || len == 0 || len > CHIP8_ROM_MAX)
        return false;

    memcpy(c->mem + CHIP8_ROM_START, data, len);
    return true;
}

void chip8_tick_timers(Chip8 *c)
{
    if (c->delay_timer > 0)
        c->delay_timer--;
    if (c->sound_timer > 0)
        c->sound_timer--;
}

void chip8_cycle(Chip8 *c)
{
    uint16_t opcode = (c->mem[c->pc] << 8) | c->mem[c->pc + 1];
    c->pc += 2;

    uint8_t  x   = (opcode >> 8) & 0xF;
    uint8_t  y   = (opcode >> 4) & 0xF;
    uint8_t  n   = opcode & 0xF;
    uint8_t  nn  = opcode & 0xFF;
    uint16_t nnn = opcode & 0xFFF;

    switch (opcode & 0xF000) {

    case 0x0000: {
        switch (nn) {
        case 0xE0: { /* 00E0 - CLS */
            memset(c->display, 0, sizeof(c->display));
            break;
        }

        case 0xEE: { /* 00EE - RET */
            c->sp--;
            c->pc = c->stack[c->sp];
            break;
        }

        default:
            goto unimplemented;
        }
        break;
    }

    case 0x1000: { /* 1NNN - JP nnn */
        c->pc = nnn;
        break;
    }

    case 0x2000: { /* 2NNN - CALL nnn */
        c->stack[c->sp] = c->pc;
        c->sp++;
        c->pc = nnn;
        break;
    }

    case 0x3000: { /* 3XNN - SE Vx, nn */
        if (c->V[x] == nn)
            c->pc += 2;
        break;
    }

    case 0x4000: { /* 4XNN - SNE Vx, nn */
        if (c->V[x] != nn)
            c->pc += 2;
        break;
    }

    case 0x5000: {
        switch (n) {
        case 0x0: { /* 5XY0 - SE Vx, Vy */
            if (c->V[x] == c->V[y])
                c->pc += 2;
            break;
        }

        default:
            goto unimplemented;
        }
        break;
    }

    case 0x6000: { /* 6XNN - LD Vx, nn */
        c->V[x] = nn;
        break;
    }

    case 0x7000: { /* 7XNN - ADD Vx, nn */
        c->V[x] += nn;
        break;
    }

    case 0x8000: {
        switch (n) {
        case 0x0: { /* 8XY0 - LD Vx, Vy */
            c->V[x] = c->V[y];
            break;
        }

        case 0x1: { /* 8XY1 - OR Vx, Vy */
            c->V[x] = c->V[x] | c->V[y];
            break;
        }

        case 0x2: { /* 8XY2 - AND Vx, Vy */
            c->V[x] = c->V[x] & c->V[y];
            break;
        }

        case 0x3: { /* 8XY3 - XOR Vx, Vy */
            c->V[x] = c->V[x] ^ c->V[y];
            break;
        }

        case 0x4: { /* 8XY4 - ADD Vx, Vy */
            uint8_t carry = (c->V[x] + c->V[y]) > 0xFF;
            c->V[x] = c->V[x] + c->V[y];
            c->V[0xF] = carry;
            break;
        }

        case 0x5: { /* 8XY5 - SUB Vx, Vy */
            uint8_t nb = (c->V[x] >= c->V[y]);
            c->V[x] = c->V[x] - c->V[y];
            c->V[0xF] = nb;
            break;
        }

        case 0x6: { /* 8XY6 - SHR Vx */
            uint8_t lsb = (c->V[x] & 0x01);
            c->V[x] >>= 1;
            c->V[0xF] = lsb;
            break;
        }

        case 0x7: { /* 8XY7 - SUBN Vx, Vy */
            uint8_t nb = (c->V[y] >= c->V[x]);
            c->V[x] = c->V[y] - c->V[x];
            c->V[0xF] = nb;
            break;
        }

        case 0xE: { /* 8XYE - SHL Vx */
            uint8_t msb = (c->V[x] & 0x80) >> 7;
            c->V[x] <<= 1;
            c->V[0xF] = msb;
            break;
        }

        default:
            goto unimplemented;
        }
        break;
    }

    case 0x9000: {
        switch (n) {
        case 0x0: { /* 9XY0 - SNE Vx, Vy */
            if (c->V[x] != c->V[y])
                c->pc += 2;
            break;
        }

        default:
            goto unimplemented;
        }
        break;
    }

    case 0xA000: { /* ANNN - LD I, nnn */
        c->I = nnn;
        break;
    }

    case 0xB000: { /* BNNN - JP V0, nnn */
        c->pc = nnn + c->V[0];
        break;
    }

    case 0xC000: { /* CXNN - RND Vx, nn */
        c->V[x] = (uint8_t)(rand() & nn);
        break;
    }

    case 0xD000: { /* DXYN - DRW Vx, Vy, n */
        uint8_t sx = c->V[x] % CHIP8_DISPLAY_W;
        uint8_t sy = c->V[y] % CHIP8_DISPLAY_H;
        c->V[0xF] = 0;

        for (int row = 0; row < n; row++) {
            if (sy + row >= CHIP8_DISPLAY_H)
                break;
            uint8_t sprite_byte = c->mem[c->I + row];

            for (int col = 0; col < 8; col++) {
                if (sx + col >= CHIP8_DISPLAY_W)
                    break;
                if (!(sprite_byte & (0x80 >> col)))
                    continue;

                int idx = (sy + row) * CHIP8_DISPLAY_W + (sx + col);
                if (c->display[idx])
                    c->V[0xF] = 1;
                c->display[idx] ^= 1;
            }
        }
        break;
    }

    case 0xE000: {
        switch (nn) {
        case 0x9E: { /* EX9E - SKP Vx */
            if (c->keys[c->V[x] & 0xF])
                c->pc += 2;
            break;
        }

        case 0xA1: { /* EXA1 - SKNP Vx */
            if (!c->keys[c->V[x] & 0xF])
                c->pc += 2;
            break;
        }

        default:
            goto unimplemented;
        }
        break;
    }

    case 0xF000: {
        switch (nn) {
        case 0x07: { /* FX07 - LD Vx, DT */
            c->V[x] = c->delay_timer;
            break;
        }

        case 0x0A: { /* FX0A - LD Vx, K */
            int z = 0;
            for (int i = 0; i < 16; i++) {
                if ((c->keys[i])) {
                    c->V[x] = i;
                    z = 1;
                    break;
                }
            }
            if (z == 0) {
                c->pc -= 2;
            }
            break;
        }

        case 0x15: { /* FX15 - LD DT, Vx */
            c->delay_timer = c->V[x];
            break;
        }

        case 0x18: { /* FX18 - LD ST, Vx */
            c->sound_timer = c->V[x];
            break;
        }

        case 0x1E: { /* FX1E - ADD I, Vx */
            c->I += c->V[x];
            break;
        }

        case 0x29: { /* FX29 - LD F, Vx */
            c->I = CHIP8_FONT_START + (c->V[x] & 0xF) * 5;
            break;
        }

        case 0x33: { /* FX33 - LD B, Vx */
            uint8_t dec = c->V[x];
            c->mem[c->I + 2] = dec % 10;
            dec /= 10;
            c->mem[c->I + 1] = (dec % 10);
            dec /= 10;
            c->mem[c->I] = dec % 10;
            break;
        }

        case 0x55: { /* FX55 - LD [I], Vx */
            for (int i = 0; i <= x; i++) {
                c->mem[c->I + i] = c->V[i];
            }
            break;
        }

        case 0x65: { /* FX65 - LD Vx, [I] */
            for (int i = 0; i <= x; i++) {
                c->V[i] = c->mem[c->I + i];
            }
            break;
        }

        default:
            goto unimplemented;
        }
        break;
    }

    default:
    unimplemented:
        c->bad_op     = true;
        c->bad_opcode = opcode;
        c->bad_pc     = c->pc - 2;
        break;
    }
}