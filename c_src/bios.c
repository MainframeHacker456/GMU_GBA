#ifdef WASM_STANDALONE
#include "wasm_compat.h"
#else
#include <math.h>
#include <string.h>
#endif
#include "bios.h"
#include "arm.h"
#include "arm_mem.h"
#include "io.h"

bool bios_hle_enabled = true;

void bios_init() {
    bios_hle_enabled = true;
}

void bios_direct_boot() {
    // Standard GBA post-boot registers
    arm_r.r[0] = 0x00000000;
    arm_r.r[1] = 0x000000ea;
    arm_r.r[2] = 0x00000000;
    arm_r.r[3] = 0x00000000;
    arm_r.r[4] = 0x00000000;
    arm_r.r[5] = 0x00000000;
    arm_r.r[6] = 0x00000000;
    arm_r.r[7] = 0x00000000;
    arm_r.r[8] = 0x00000000;
    arm_r.r[9] = 0x00000000;
    arm_r.r[10] = 0x00000000;
    arm_r.r[11] = 0x00000000;
    arm_r.r[12] = 0x00000000;

    arm_r.r8_usr = 0;
    arm_r.r9_usr = 0;
    arm_r.r10_usr = 0;
    arm_r.r11_usr = 0;
    arm_r.r12_usr = 0;

    arm_r.r13_usr = 0x03007f00;
    arm_r.r14_usr = 0x00000000;
    arm_r.r13_irq = 0x03007fa0;
    arm_r.r14_irq = 0x00000000;
    arm_r.r13_svc = 0x03007fe0;
    arm_r.r14_svc = 0x00000000;

    arm_r.r[13] = arm_r.r13_usr;
    arm_r.r[14] = arm_r.r14_usr;
    arm_r.r[15] = 0x08000000; // Cartridge entry point

    arm_r.cpsr = ARM_SYS; // System mode (0x1f), Thumb off, IRQs unmasked

    wait_cnt.w = 0x4317;
    update_ws();

    // Populate BIOS interrupt vectors
    *(uint32_t *)(bios + 0x00) = 0xea1ffffe; // b 0x08000000 (Reset vector)
    *(uint32_t *)(bios + 0x08) = 0xe25ef004; // subs pc, lr, #4 (SWI return fallback)

    // Official GBA BIOS IRQ dispatcher at 0x18
    static const uint32_t irq_stub[] = {
        0xe92d500f, // stmfd sp!, {r0-r3, r12, lr}
        0xe3a00204, // mov r0, #0x04000000 (IO base)
        0xe2800c02, // add r0, r0, #0x200 (r0 = 0x04000200 = IE)
        0xe5901000, // ldr r1, [r0] (r1 = IE | (IF << 16))
        0xe0012821, // and r2, r1, r1, lsr #16 (r2 = IE & IF = triggered)
        0xe3a03403, // mov r3, #0x03000000
        0xe2833c7f, // add r3, r3, #0x7f00
        0xe28330f8, // add r3, r3, #0xf8 (r3 = 0x03007FF8)
        0xe1d340b0, // ldrh r4, [r3]
        0xe1844002, // orr r4, r4, r2
        0xe1c340b0, // strh r4, [r3] (BIOS IntrCheck: 0x03007FF8 |= triggered)
        0xe1c020b2, // strh r2, [r0, #2] (IF = triggered acknowledge)
        0xe5933004, // ldr r3, [r3, #4] (User handler from 0x03007FFC)
        0xe3530000, // cmp r3, #0
        0x11a0e00f, // movne lr, pc
        0x112fff13, // bxne r3
        0xe8bd500f, // ldmfd sp!, {r0-r3, r12, lr}
        0xe25ef004  // subs pc, lr, #4
    };
    for (int i = 0; i < (int)(sizeof(irq_stub) / sizeof(uint32_t)); i++) {
        *(uint32_t *)(bios + 0x18 + i * 4) = irq_stub[i];
    }

    // Reload pipeline from 0x08000000
    pipe_reload = true;
    arm_pipe[0] = arm_read_n(0x08000000);
    arm_pipe[1] = arm_read_s(0x08000004);
    arm_r.r[15] = 0x08000008;
}

static void bios_soft_reset() {
    bios_direct_boot();
}

static void bios_reg_ram_reset(uint32_t flags) {
    if (flags & 0x01) { // 256K WRAM
        memset(wram, 0, 0x40000);
    }
    if (flags & 0x02) { // 32K IWRAM (excluding top 0x200 bytes)
        memset(iwram, 0, 0x7e00);
    }
    if (flags & 0x04) { // Palette
        memset(pram, 0, 0x400);
        for (int i = 0; i < 0x200; i++) palette[i] = 0;
    }
    if (flags & 0x08) { // VRAM
        memset(vram, 0, 0x18000);
    }
    if (flags & 0x10) { // OAM
        memset(oam, 0, 0x400);
    }
    if (flags & 0x80) { // Reset other I/O registers
        disp_cnt.w = 0x0080;
    }
}

static void bios_div(bool arm_order) {
    int32_t num = arm_order ? (int32_t)arm_r.r[1] : (int32_t)arm_r.r[0];
    int32_t den = arm_order ? (int32_t)arm_r.r[0] : (int32_t)arm_r.r[1];

    if (den != 0) {
        int32_t quot = num / den;
        int32_t rem  = num % den;
        arm_r.r[0] = (uint32_t)quot;
        arm_r.r[1] = (uint32_t)rem;
        arm_r.r[3] = (uint32_t)(quot < 0 ? -quot : quot);
    }
}

static void bios_sqrt() {
    uint32_t val = arm_r.r[0];
    arm_r.r[0] = (uint32_t)sqrt((double)val);
}

static void bios_arctan() {
    int16_t x = (int16_t)(arm_r.r[0] & 0xffff);
    double rad = atan((double)x / 16384.0);
    arm_r.r[0] = (uint32_t)(rad * (16384.0 / (M_PI * 0.5)));
}

static void bios_arctan2() {
    int16_t x = (int16_t)(arm_r.r[0] & 0xffff);
    int16_t y = (int16_t)(arm_r.r[1] & 0xffff);
    double rad = atan2((double)y, (double)x);
    if (rad < 0) rad += 2.0 * M_PI;
    arm_r.r[0] = (uint32_t)(rad * (65536.0 / (2.0 * M_PI)));
}

static void bios_cpu_set() {
    uint32_t src  = arm_r.r[0];
    uint32_t dst  = arm_r.r[1];
    uint32_t mode = arm_r.r[2];
    uint32_t count = mode & 0x1fffff;
    bool fill = (mode & (1 << 24)) != 0;
    bool is_32 = (mode & (1 << 26)) != 0;

    if (is_32) {
        uint32_t data = fill ? arm_read(src) : 0;
        for (uint32_t i = 0; i < count; i++) {
            if (!fill) data = arm_read(src + i * 4);
            arm_write(dst + i * 4, data);
        }
    } else {
        uint16_t data = fill ? arm_readh(src) : 0;
        for (uint32_t i = 0; i < count; i++) {
            if (!fill) data = arm_readh(src + i * 2);
            arm_writeh(dst + i * 2, data);
        }
    }
}

static void bios_cpu_fast_set() {
    uint32_t src  = arm_r.r[0];
    uint32_t dst  = arm_r.r[1];
    uint32_t mode = arm_r.r[2];
    uint32_t count = (mode & 0x1fffff);
    count = (count + 7) & ~7; // Rounded up to multiple of 8
    bool fill = (mode & (1 << 24)) != 0;

    uint32_t data = fill ? arm_read(src) : 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!fill) data = arm_read(src + i * 4);
        arm_write(dst + i * 4, data);
    }
}

static void bios_bg_affine_set() {
    uint32_t src = arm_r.r[0];
    uint32_t dst = arm_r.r[1];
    uint32_t num = arm_r.r[2];

    for (uint32_t i = 0; i < num; i++) {
        int32_t ox = (int32_t)arm_read(src + 0);
        int32_t oy = (int32_t)arm_read(src + 4);
        int16_t cx = (int16_t)arm_readh(src + 8);
        int16_t cy = (int16_t)arm_readh(src + 10);
        int16_t sx = (int16_t)arm_readh(src + 12);
        int16_t sy = (int16_t)arm_readh(src + 14);
        uint16_t theta = arm_readh(src + 16) >> 8;
        src += 20;

        double rad = (theta * 2.0 * M_PI) / 256.0;
        int16_t sin_a = (int16_t)(sin(rad) * 256.0);
        int16_t cos_a = (int16_t)(cos(rad) * 256.0);

        int16_t pa = (int16_t)(((int32_t)cos_a * sx) >> 8);
        int16_t pb = (int16_t)(((int32_t)-sin_a * sx) >> 8);
        int16_t pc = (int16_t)(((int32_t)sin_a * sy) >> 8);
        int16_t pd = (int16_t)(((int32_t)cos_a * sy) >> 8);

        int32_t rx = ox - (pa * cx + pb * cy);
        int32_t ry = oy - (pc * cx + pd * cy);

        arm_writeh(dst + 0, (uint16_t)pa);
        arm_writeh(dst + 2, (uint16_t)pb);
        arm_writeh(dst + 4, (uint16_t)pc);
        arm_writeh(dst + 6, (uint16_t)pd);
        arm_write(dst + 8, (uint32_t)rx);
        arm_write(dst + 12, (uint32_t)ry);
        dst += 16;
    }
}

static void bios_obj_affine_set() {
    uint32_t src = arm_r.r[0];
    uint32_t dst = arm_r.r[1];
    uint32_t num = arm_r.r[2];
    uint32_t offset = arm_r.r[3] ? arm_r.r[3] : 2;

    for (uint32_t i = 0; i < num; i++) {
        int16_t sx = (int16_t)arm_readh(src + 0);
        int16_t sy = (int16_t)arm_readh(src + 2);
        uint16_t theta = arm_readh(src + 4) >> 8;
        src += 8;

        double rad = (theta * 2.0 * M_PI) / 256.0;
        int16_t sin_a = (int16_t)(sin(rad) * 256.0);
        int16_t cos_a = (int16_t)(cos(rad) * 256.0);

        int16_t pa = (int16_t)(((int32_t)cos_a * sx) >> 8);
        int16_t pb = (int16_t)(((int32_t)-sin_a * sx) >> 8);
        int16_t pc = (int16_t)(((int32_t)sin_a * sy) >> 8);
        int16_t pd = (int16_t)(((int32_t)cos_a * sy) >> 8);

        arm_writeh(dst + 0 * offset, (uint16_t)pa);
        arm_writeh(dst + 1 * offset, (uint16_t)pb);
        arm_writeh(dst + 2 * offset, (uint16_t)pc);
        arm_writeh(dst + 3 * offset, (uint16_t)pd);
        dst += 4 * offset;
    }
}

static uint8_t decomp_buf[0x40000];

static void bios_lz77_uncomp(bool vram_mode) {
    uint32_t src = arm_r.r[0];
    uint32_t dst = arm_r.r[1];
    uint32_t header = arm_read(src);
    src += 4;
    uint32_t size = header >> 8;
    if (size > sizeof(decomp_buf)) size = sizeof(decomp_buf);

    uint32_t out_idx = 0;

    while (out_idx < size) {
        uint8_t flags = arm_readb(src++);
        for (int i = 0; i < 8 && out_idx < size; i++) {
            if (flags & 0x80) {
                uint8_t b1 = arm_readb(src++);
                uint8_t b2 = arm_readb(src++);
                int length = (b1 >> 4) + 3;
                int disp = (((b1 & 0x0f) << 8) | b2) + 1;

                for (int j = 0; j < length && out_idx < size; j++) {
                    decomp_buf[out_idx] = decomp_buf[out_idx - disp];
                    out_idx++;
                }
            } else {
                decomp_buf[out_idx++] = arm_readb(src++);
            }
            flags <<= 1;
        }
    }

    if (vram_mode) {
        for (uint32_t i = 0; i < size; i += 2) {
            uint16_t h = decomp_buf[i] | ((i + 1 < size ? decomp_buf[i + 1] : 0) << 8);
            arm_writeh(dst + i, h);
        }
    } else {
        for (uint32_t i = 0; i < size; i++) {
            arm_writeb(dst + i, decomp_buf[i]);
        }
    }
}

static void bios_rl_uncomp(bool vram_mode) {
    uint32_t src = arm_r.r[0];
    uint32_t dst = arm_r.r[1];
    uint32_t header = arm_read(src);
    src += 4;
    uint32_t size = header >> 8;
    if (size > sizeof(decomp_buf)) size = sizeof(decomp_buf);

    uint32_t out_idx = 0;

    while (out_idx < size) {
        uint8_t flag = arm_readb(src++);
        int len = (flag & 0x7f);

        if (flag & 0x80) { // Compressed run
            len += 3;
            uint8_t val = arm_readb(src++);
            for (int i = 0; i < len && out_idx < size; i++) {
                decomp_buf[out_idx++] = val;
            }
        } else { // Uncompressed data
            len += 1;
            for (int i = 0; i < len && out_idx < size; i++) {
                decomp_buf[out_idx++] = arm_readb(src++);
            }
        }
    }

    if (vram_mode) {
        for (uint32_t i = 0; i < size; i += 2) {
            uint16_t h = decomp_buf[i] | ((i + 1 < size ? decomp_buf[i + 1] : 0) << 8);
            arm_writeh(dst + i, h);
        }
    } else {
        for (uint32_t i = 0; i < size; i++) {
            arm_writeb(dst + i, decomp_buf[i]);
        }
    }
}

bool bios_swi(uint8_t comment) {
    switch (comment) {
        case 0x00: // SoftReset
            bios_soft_reset();
            return true;

        case 0x01: // RegisterRamReset
            bios_reg_ram_reset(arm_r.r[0]);
            return true;

        case 0x02: // Halt
            int_halt = true;
            return true;

        case 0x03: // Stop
            int_halt = true;
            return true;

        case 0x04: // IntrWait
            if (arm_r.r[0]) {
                int_ack.w &= ~arm_r.r[1];
            }
            int_enb.w |= arm_r.r[1];
            int_enb_m.w |= 1;
            int_halt = true;
            return true;

        case 0x05: // VBlankIntrWait
            disp_stat.w |= VBLK_IRQ;
            int_enb.w |= VBLK_FLAG;
            int_enb_m.w |= 1;
            int_ack.w &= ~VBLK_FLAG;
            int_halt = true;
            return true;

        case 0x06: // Div
            bios_div(false);
            return true;

        case 0x07: // DivArm
            bios_div(true);
            return true;

        case 0x08: // Sqrt
            bios_sqrt();
            return true;

        case 0x09: // ArcTan
            bios_arctan();
            return true;

        case 0x0a: // ArcTan2
            bios_arctan2();
            return true;

        case 0x0b: // CpuSet
            bios_cpu_set();
            return true;

        case 0x0c: // CpuFastSet
            bios_cpu_fast_set();
            return true;

        case 0x0d: // GetBiosChecksum
            arm_r.r[0] = 0xBAAE187F;
            return true;

        case 0x0e: // BgAffineSet
            bios_bg_affine_set();
            return true;

        case 0x0f: // ObjAffineSet
            bios_obj_affine_set();
            return true;

        case 0x11: // LZ77UnCompWram
            bios_lz77_uncomp(false);
            return true;

        case 0x12: // LZ77UnCompVram
            bios_lz77_uncomp(true);
            return true;

        case 0x14: // RLUnCompWram
            bios_rl_uncomp(false);
            return true;

        case 0x15: // RLUnCompVram
            bios_rl_uncomp(true);
            return true;

        default:
            return false;
    }
}
