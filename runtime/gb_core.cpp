// The machine: the bus and the cartridge's mappers, the I/O ports, the timer and
// the serial port (MAME's), interrupts, HALT and STOP, the SM83 interpreter in its
// two forms (flat: the reference; nested: the fallback under native code), the
// table of native entries, and the state compared by the lockstep.
#include "gb.h"
#include "gb_trace.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

// ======================================================================
// the timer and the serial port, as MAME's gb.cpp runs them: called with
// every CPU charge (cycles_passed), in CPU cycles
// ======================================================================
static void sio_tick(GB& c) {
    if (!(c.sc & 0x80)) return;
    if (c.sio_count & 1) c.sb = (uint8_t)(c.sb << 1 | 1);   // a bit in (nothing plugged in: 1s)
    c.sio_count--;
    if (c.sio_count == 0) { c.sc &= 0x7F; gb_irq(c, I_SERIAL); }
}

static void timer_check_irq(GB& c) {
    c.reloading = 0;
    if (c.triggering) {
        c.triggering = 0;
        if (c.tima == 0) { c.tima = c.tma; gb_irq(c, I_TIMER); c.reloading = 1; }
    }
}

static void timer_increment(GB& c) {
    timer_check_irq(c);
    c.tima++;
    if (c.tima == 0) c.triggering = 1;
}

void gb_timer(GB& c, uint32_t n) {
    uint16_t old = c.divcount, olds = c.sio_clock;
    c.divcount = (uint16_t)(c.divcount + n);
    c.sio_clock = (uint16_t)(c.sio_clock + n);
    timer_check_irq(c);
    if (c.tac & 4) {
        uint16_t oc = (uint16_t)(old >> c.shift), nc = (uint16_t)(c.divcount >> c.shift);
        if (n > c.shift_cycles) { timer_increment(c); oc++; }
        if (nc != oc) {
            timer_increment(c);
            if ((uint32_t)(nc << c.shift) < c.divcount) timer_check_irq(c);
        }
    }
    if (((c.sio_clock ^ olds) & c.sio_freq) && (c.sc & 1)) sio_tick(c);
}

void gb_irq(GB& c, uint8_t bit) {
    c.IF |= bit;
    c.next = 0;          // looked at the next boundary
}

// ======================================================================
// the cartridge
// ======================================================================
static inline uint32_t rom_off(const GB& c, uint32_t bank) { return (bank % c.rom_banks) << 14; }

static void rtc_second(GB& c) {
    if (c.rtc[4] & 0x40) return;                     // halted
    if (++c.rtc[0] != 60) return; c.rtc[0] = 0;
    if (++c.rtc[1] != 60) return; c.rtc[1] = 0;
    if (++c.rtc[2] != 24) return; c.rtc[2] = 0;
    uint16_t d = (uint16_t)(c.rtc[3] | (c.rtc[4] & 1) << 8);
    d++;
    if (d == 512) { d = 0; c.rtc[4] |= 0x80; }       // the day counter's carry
    c.rtc[3] = (uint8_t)d; c.rtc[4] = (uint8_t)((c.rtc[4] & 0xFE) | (d >> 8));
}

// the clock runs on the machine's time, so the two machines of a lockstep see the same
static void rtc_sync(GB& c) {
    if (!c.has_rtc) return;
    while (c.t >= c.rtc_at) { rtc_second(c); c.rtc_at += TICK_HZ; }
}

void gb_map(GB& c) {
    // ROM
    switch (c.mbc) {
    case MBC_1: {
        uint32_t lo = c.mreg[1] & 0x1F; if (!lo) lo = 1;
        uint32_t hi = c.mreg[2] & 3;
        c.bankx = hi << 5 | lo;
        c.bank0 = (c.mreg[3] & 1) ? hi << 5 : 0;
        c.ram_bank = (c.mreg[3] & 1) ? hi : 0;
        break;
    }
    case MBC_2: c.bankx = (c.mreg[1] & 0x0F) ? (c.mreg[1] & 0x0F) : 1; c.bank0 = 0; break;
    case MBC_3: c.bankx = (c.mreg[1] & 0x7F) ? (c.mreg[1] & 0x7F) : 1; c.bank0 = 0;
                c.ram_bank = c.mreg[2] & 3; c.rtc_sel = (c.mreg[2] >= 8 && c.mreg[2] <= 0x0C && c.has_rtc) ? c.mreg[2] : 0; break;
    case MBC_5: case MBC_7: c.bankx = (uint32_t)c.mreg[1] | (uint32_t)(c.mreg[3] & 1) << 8; c.bank0 = 0;
                c.ram_bank = c.mreg[2] & 0x0F; break;
    case MBC_HUC1: c.bankx = (c.mreg[1] & 0x3F) ? (c.mreg[1] & 0x3F) : 1; c.bank0 = 0; c.ram_bank = c.mreg[2] & 3; break;
    case MBC_HUC3: c.bankx = (c.mreg[1] & 0x7F) ? (c.mreg[1] & 0x7F) : 1; c.bank0 = 0; c.ram_bank = c.mreg[2] & 3; break;
    default: c.bankx = 1; c.bank0 = 0; c.ram_bank = 0; break;
    }
    c.bank0 %= c.rom_banks; c.bankx %= c.rom_banks;
    const uint8_t** rd = c.rd_pg; uint8_t** wr = c.wr_pg;
    for (int p = 0; p < 4; p++) { rd[p] = c.rom + rom_off(c, c.bank0) + p * 0x1000; wr[p] = nullptr; }
    for (int p = 0; p < 4; p++) { rd[4 + p] = c.rom + rom_off(c, c.bankx) + p * 0x1000; wr[4 + p] = nullptr; }
    // VRAM: a handler (locked in mode 3)
    rd[8] = rd[9] = nullptr; wr[8] = wr[9] = nullptr;
    // the cartridge's RAM: direct when it is plain RAM, enabled, and there
    rd[10] = rd[11] = nullptr; wr[10] = wr[11] = nullptr;
    bool plain = c.mbc == MBC_ROM ? c.sram_size >= 0x2000 :
                 (c.mbc == MBC_1 || c.mbc == MBC_3 || c.mbc == MBC_5 || c.mbc == MBC_HUC1) && c.ram_on && c.sram_size >= 0x2000
                 && !c.rtc_sel;
    if (plain) {
        uint32_t base = (c.ram_bank * 0x2000) % c.sram_size;
        for (int p = 0; p < 2; p++) { rd[10 + p] = c.sram + base + p * 0x1000; wr[10 + p] = c.sram + base + p * 0x1000; }
    }
    // WRAM (the CGB's bank at $D000), and $E000-$EFFF of its echo
    uint32_t wb = c.cgb ? ((c.svbk & 7) ? (c.svbk & 7) : 1) : 1;
    rd[12] = wr[12] = c.wram;
    rd[13] = wr[13] = c.wram + wb * 0x1000;
    rd[14] = wr[14] = c.wram;
    rd[15] = nullptr; wr[15] = nullptr;
    for (int p = 0; p < 16; p++) { c.rdp[p] = c.tr ? nullptr : rd[p]; c.wrp[p] = c.tr ? nullptr : wr[p]; }
}

static uint8_t cart_read(GB& c, uint16_t a) {
    uint16_t o = a & 0x1FFF;
    switch (c.mbc) {
    case MBC_ROM: return c.sram_size ? c.sram[o % c.sram_size] : 0xFF;
    case MBC_2:
        if (!c.ram_on) return 0xFF;
        return (uint8_t)(0xF0 | (c.sram[o & 0x1FF] & 0x0F));
    case MBC_3:
        if (!c.ram_on) return 0xFF;
        if (c.rtc_sel) { rtc_sync(c); return c.rtc_latch[c.rtc_sel - 8]; }
        if (!c.sram_size) return 0xFF;
        return c.sram[(c.ram_bank * 0x2000 + o) % c.sram_size];
    case MBC_HUC1: case MBC_HUC3:
        if (!c.ram_on) return 0xFF;
        if (!c.sram_size) return 0xFF;
        return c.sram[(c.ram_bank * 0x2000 + o) % c.sram_size];
    case MBC_7: return 0xFF;                                           // the accelerometer: not covered
    default:
        if (!c.ram_on || !c.sram_size) return 0xFF;
        return c.sram[(c.ram_bank * 0x2000 + o) % c.sram_size];
    }
}

static void cart_write(GB& c, uint16_t a, uint8_t v) {
    uint16_t o = a & 0x1FFF;
    switch (c.mbc) {
    case MBC_ROM: if (c.sram_size) c.sram[o % c.sram_size] = v; return;
    case MBC_2: if (c.ram_on) c.sram[o & 0x1FF] = v & 0x0F; return;
    case MBC_3:
        if (!c.ram_on) return;
        if (c.rtc_sel) {
            rtc_sync(c);
            int r = c.rtc_sel - 8;
            static const uint8_t mask[5] = { 0x3F, 0x3F, 0x1F, 0xFF, 0xC1 };
            c.rtc[r] = v & mask[r]; c.rtc_latch[r] = c.rtc[r];
            if (r == 0) c.rtc_sub = 0;
            return;
        }
        if (c.sram_size) c.sram[(c.ram_bank * 0x2000 + o) % c.sram_size] = v;
        return;
    case MBC_7: return;
    default:
        if (c.ram_on && c.sram_size) c.sram[(c.ram_bank * 0x2000 + o) % c.sram_size] = v;
        return;
    }
}

// a write to $0000-$7FFF: the mapper's registers
static void mbc_write(GB& c, uint16_t a, uint8_t v) {
    int r = a >> 13;    // 0-3
    switch (c.mbc) {
    case MBC_ROM: return;
    case MBC_1:
        if (r == 0) c.ram_on = (v & 0x0F) == 0x0A; else c.mreg[r] = v;
        break;
    case MBC_2:
        if (a >= 0x4000) return;
        if (a & 0x100) c.mreg[1] = v; else c.ram_on = (v & 0x0F) == 0x0A;
        break;
    case MBC_3:
        if (r == 0) c.ram_on = (v & 0x0F) == 0x0A;
        else if (r == 3) {
            if (c.has_rtc && c.rtc_prev == 0 && v == 1) { rtc_sync(c); std::memcpy(c.rtc_latch, c.rtc, 5); }
            c.rtc_prev = v;
        } else c.mreg[r] = v;
        break;
    case MBC_5: case MBC_7:
        if (r == 0) c.ram_on = (v & 0x0F) == 0x0A;
        else if (r == 1) { if (a < 0x3000) c.mreg[1] = v; else c.mreg[3] = v; }
        else if (r == 2) c.mreg[2] = v;
        break;
    case MBC_HUC1:
        if (r == 0) c.ram_on = (v & 0x0F) == 0x0A;     // $0E: the IR port (not covered)
        else c.mreg[r] = v;
        break;
    case MBC_HUC3:
        if (r == 0) c.ram_on = (v & 0x0F) == 0x0A;
        else c.mreg[r] = v;
        break;
    default: return;
    }
    uint32_t o0 = c.bank0, ox = c.bankx;
    gb_map(c);
    if (c.tr) gb_trace_bank(c, o0, ox);
}

// ======================================================================
// the ports
// ======================================================================
static void joyp_latch(GB& c, uint8_t v) {
    // MAME: the value is made at the write, from the keys held then
    uint8_t j = (uint8_t)(0xCF | v);
    if (!(v & 0x20)) j &= (uint8_t)(~(c.keys >> 4) | 0xF0);    // buttons: A B select start
    if (!(v & 0x10)) j &= (uint8_t)(~c.keys | 0xF0);            // directions: right left up down
    c.joyp = j;
}

static uint8_t io_read(GB& c, uint16_t a) {
    uint8_t o = a & 0xFF;
    if (o >= 0x10 && o < 0x40) return snd_read(c, a);
    if (o >= 0x40) {
        if (c.cgb) {
            if (o == 0x4D) return (uint8_t)(0x7E | c.ds << 7 | c.key1);
            if (o == 0x70) return (uint8_t)(c.svbk & 7);
        }
        return ppu_read(c, a);
    }
    switch (o) {
    case 0x00: return c.joyp;
    case 0x01: return c.sb;
    case 0x02: return c.sc;
    case 0x04: return (uint8_t)(c.divcount >> 8);
    case 0x05: return c.tima;
    case 0x06: return c.tma;
    case 0x07: return c.tac;
    case 0x0F: ppu_sync(c); return (uint8_t)(0xE0 | c.IF);
    default: return 0xFF;
    }
}

static void io_write(GB& c, uint16_t a, uint8_t v) {
    static const uint8_t shifts[4] = { 10, 4, 6, 8 };
    uint8_t o = a & 0xFF;
    if (o >= 0x10 && o < 0x40) { snd_write(c, a, v); return; }
    if (o >= 0x40) {
        if (c.cgb) {
            if (o == 0x4D) { c.key1 = v & 1; return; }
            if (o == 0x70) { c.svbk = v & 7; gb_map(c); }
        }
        if (o == 0x50) return;     // the boot ROM's switch: there is none
        ppu_write(c, a, v);
        return;
    }
    switch (o) {
    case 0x00: joyp_latch(c, v); return;
    case 0x01: c.sb = v; return;
    case 0x02:
        if ((v & 0x81) == 0x81 && c.sio_n < sizeof c.sio_out) c.sio_out[c.sio_n++] = c.sb;   // a byte sent
        c.sio_count = (v & 0x80) ? 16 : 0;
        if (c.cgb) c.sio_freq = (v & 2) ? 8 : 256;
        c.sc = (uint8_t)(v | (c.cgb ? 0x7C : 0x7E));
        if (c.cgb) c.sc = (uint8_t)((c.sc & ~2) | (v & 2));
        return;
    case 0x04:
        if ((c.divcount >> (c.shift - 1)) & 1) timer_increment(c);
        c.divcount = 0;
        return;
    case 0x05:
        if ((c.tac & 4) && c.tima == c.tma && (c.divcount & (c.shift_cycles - 1)) == 4) v = c.tma;
        c.tima = v; return;
    case 0x06:
        if ((c.tac & 4) && c.tima == c.tma && (c.divcount & (c.shift_cycles - 1)) == 4) c.tima = v;
        c.tma = v; return;
    case 0x07:
        v |= 0xF8;
        if ((!(v & 4) && (c.tac & 4)) || ((v & 4) && (c.tac & 4) && (v & 3) != (c.tac & 3)))
            if ((c.divcount & (c.shift_cycles - 1)) >= (c.shift_cycles >> 1)) timer_increment(c);
        c.shift = shifts[v & 3]; c.shift_cycles = (uint16_t)(1 << c.shift);
        c.tac = v; return;
    case 0x0F: ppu_sync(c); c.IF = v & 0x1F; gb_recalc(c); return;
    default: return;
    }
}

uint8_t gb_rd_slow(GB& c, uint16_t a) {
    if (c.tr) {
        gb_trace_read(c, a);
        if (const uint8_t* p = c.rd_pg[a >> 12]) return p[a & 0xFFF];
    }
    if (a < 0x8000) return c.rom[rom_off(c, a < 0x4000 ? c.bank0 : c.bankx) + (a & 0x3FFF)];
    if (a < 0xA000) return vram_read(c, a);
    if (a < 0xC000) return cart_read(c, a);
    if (a < 0xFE00) return c.rd_pg[(a - 0x2000) >> 12][a & 0xFFF];   // the echo of $C000-$DDFF
    if (a < 0xFF00) return oam_read(c, a);
    if (a < 0xFF80) return io_read(c, a);
    if (a < 0xFFFF) return c.hram[a - 0xFF80];
    return c.IE;
}

void gb_wr_slow(GB& c, uint16_t a, uint8_t v) {
    if (c.tr) {
        gb_trace_write(c, a, v);
        if (uint8_t* p = c.wr_pg[a >> 12]) { p[a & 0xFFF] = v; return; }
    }
    if (a < 0x8000) { mbc_write(c, a, v); return; }
    if (a < 0xA000) { vram_write(c, a, v); return; }
    if (a < 0xC000) { cart_write(c, a, v); return; }
    if (a < 0xFE00) { c.wr_pg[(a - 0x2000) >> 12][a & 0xFFF] = v; return; }
    if (a < 0xFF00) { oam_write(c, a, v); return; }
    if (a < 0xFF80) { io_write(c, a, v); return; }
    if (a < 0xFFFF) { c.hram[a - 0xFF80] = v; return; }
    c.IE = v; gb_recalc(c);
}

// ======================================================================
// events and interrupts
// ======================================================================
void gb_recalc(GB& c) {
    // a frame that ended in the middle of an instruction (a register access caught the PPU up)
    // is delivered at the very next boundary, whatever recomputed c.next since
    if ((c.ime && (c.IE & c.IF & 0x1F)) || c.ei || c.burn || c.v.frame_due) { c.next = 0; return; }
    uint64_t n = ppu_next(c);
    if (c.has_rtc && c.rtc_at < n) n = c.rtc_at;
    c.next = n;
}

// Returns false when a flat interpreter is to stop here (the frame event).
static bool events(GB& c) {
    while (c.burn) {                               // the HDMA's cycles: the CPU waits
        uint32_t n = c.burn < 4 ? c.burn : 4;
        c.burn -= n; gb_tick(c, n);
    }
    for (;;) {
        ppu_sync(c);
        rtc_sync(c);
        if (!ppu_frame_due(c)) break;
        c.frame++;
        if (c.tr) gb_trace_frame(c);
        snd_sync(c);                               // the same point in both machines, hooks or not
        gb_recalc(c);
        if (c.frame_hook) c.frame_hook(c);         // may throw GBStop
        if (!c.natives && c.flat_stop) { gb_recalc(c); return false; }
    }
    gb_recalc(c);
    return true;
}

// No interrupt at the boundary after EI.  A boundary may be polled twice (the nested
// interpreter, then the native code it hands over to): the answer is the same both times.
static bool ei_blocked(GB& c, uint16_t at) {
    if (!c.ei) return false;
    if (c.ei == 1) { c.ei = 2; c.ei_pc = at; c.ei_t = c.t; return true; }
    if (c.ei_pc == at && c.ei_t == c.t) return true;
    c.ei = 0;
    gb_recalc(c);
    return false;
}

void gb_ei(GB& c) { c.ime = 1; c.ei = 1; c.next = 0; }
void gb_di(GB& c) { c.ime = 0; c.ei = 0; gb_recalc(c); }

// MAME's check_interrupts with IME set: the lowest pending source; 12 cycles, then
// PC is written at SP-2 (the low byte first), and on to the vector
static void take_interrupt(GB& c, uint16_t at) {
    uint8_t irq = c.IE & c.IF & 0x1F;
    int n = 0; while (!(irq & (1 << n))) n++;
    c.ime = 0;
    c.IF &= (uint8_t)~(1 << n);
    c.n_irq++;
    gb_tick(c, 12);
    c.SP -= 2;
    gb_wr(c, c.SP, (uint8_t)at); gb_wr(c, (uint16_t)(c.SP + 1), (uint8_t)(at >> 8));
    c.PC = (uint16_t)(0x40 + n * 8);
    if (c.tr) gb_trace_irq(c, n, at);
    gb_recalc(c);
}

bool gb_poll(GB& c, uint16_t at) {
    for (;;) {
        events(c);
        if (ei_blocked(c, at)) return true;
        if (!(c.ime && (c.IE & c.IF & 0x1F))) return true;
        const uint16_t s0 = c.SP;
        take_interrupt(c, at);
        gb_call(c, c.PC);
        if (c.resume != at || c.SP != s0) { c.n_unwind++; return false; }
    }
}

bool gb_poll_at(GB& c, uint16_t at, int w, uint32_t bank) {
    if (!gb_poll(c, at)) return false;
    if ((w ? c.bankx : c.bank0) != bank) { gb_call(c, at); return false; }   // switched under the code
    return true;
}

// HALT at `at`, as MAME runs it: the next byte is prefetched (4 cycles); then 2 cycles
// (the DMG) or 4 (the CGB) at a time until an interrupt is pending, whatever IME;
// leaving costs MAME's delays, and with IME clear the prefetched byte runs next
// without a fetch -- on the DMG with PC left on it (the HALT bug: it is read twice).
// Returns false at a flat interpreter's frame stop, still halted (it goes on from there).
static bool halt_run(GB& c) {
    for (;;) {
        if (c.t >= c.next && !events(c)) return false;
        if (c.IE & c.IF & 0x1F) break;
        gb_tick(c, c.cgb ? 4 : 2);
        c.halt_entering = 0;
    }
    uint8_t irq = c.IE & c.IF & 0x1F;
    int n = 0; while (!(irq & (1 << n))) n++;
    if (!c.cgb) {
        if ((n == 1 && !(c.IF & 1)) || n == 2) { gb_tick(c, 4); if (n == 2) gb_tick(c, 2); }
    } else {
        gb_tick(c, 4);
        if (!c.ime && !c.halt_entering) gb_tick(c, 4);
    }
    c.halted = 0;
    c.PC = (uint16_t)(c.halt_at + 1);
    if (!c.ime) c.hfetch = 1;           // the byte after HALT, prefetched as it halted, runs next
    gb_recalc(c);
    return true;
}

void gb_halt(GB& c, uint16_t at) {
    c.n_halt++;
    if (c.tr) c.tr->halts++;
    if (!c.ime && (c.IE & c.IF & 0x1F)) {
        // the HALT bug (as the hardware has it, not MAME's DMG core, which does it at every exit
        // with IME clear): an interrupt already pending, HALT does not halt and the next byte is
        // read twice -- the opcode fetch does not advance PC
        c.hbug = 1;
        if (c.tr) c.tr->halt_bugs++;
        return;
    }
    gb_tick(c, 4);
    c.halted = 1; c.halt_entering = 1; c.halt_at = at;
    if (c.natives || !c.flat_stop) halt_run(c);       // else the flat interpreter's loop runs it
}

// STOP (its second byte read): the CGB's speed switch when KEY1 armed it, else nothing (MAME).
// The switch's pause: 2050 M-cycles at the old speed (Pan Docs; MAME's is ~1.4 s).
void gb_stop(GB& c) {
    if (c.cgb && c.key1) {
        for (int i = 0; i < 2050; i++) gb_tick(c, 4);
        c.ds ^= 1; c.tmul = c.ds ? 1 : 2;
        if (c.tr) c.tr->speed_switches++;
    }
    c.key1 = 0;
    if (c.tr) c.tr->stops++;
    gb_recalc(c);
}

void gb_daa(GB& c) {
    int tmp = c.A;
    if (!(c.F & FN)) {
        if ((c.F & FH) || (tmp & 0x0F) > 9) tmp += 6;
        if ((c.F & FC) || tmp > 0x9F) tmp += 0x60;
    } else {
        if (c.F & FH) { tmp -= 6; if (!(c.F & FC)) tmp &= 0xFF; }
        if (c.F & FC) tmp -= 0x60;
    }
    c.F &= (uint8_t)~(FH | FZ);
    if (tmp & 0x100) c.F |= FC;
    c.A = (uint8_t)tmp;
    if (!c.A) c.F |= FZ;
}

[[noreturn]] void gb_jam(GB& c, const char* why) {
    c.jammed = 1;
    std::fprintf(stderr, "gb: %s at %04X (bank %u, frame %llu)\n", why, c.PC, c.bankx, (unsigned long long)c.frame);
    throw GBStop();
}

// ======================================================================
// native functions, by (window, ROM bank, address)
// ======================================================================
static std::unordered_map<uint32_t, GBFn*> g_nat;    // key: window << 16 | bank -> 16 KB of entries
static inline uint32_t nkey(int w, uint32_t bank) { return (uint32_t)w << 16 | bank; }

void gb_register(int w, uint32_t bank, uint16_t pc, GBFn fn) {
    GBFn*& t = g_nat[nkey(w, bank)];
    if (!t) { t = new GBFn[0x4000]; for (int i = 0; i < 0x4000; i++) t[i] = nullptr; }
    t[pc & 0x3FFF] = fn;
}

GBFn gb_lookup(GB& c, uint16_t pc) {
    if (!c.natives || pc >= 0x8000 || g_nat.empty()) return nullptr;
    int w = pc >> 14;
    auto it = g_nat.find(nkey(w, w ? c.bankx : c.bank0));
    return it == g_nat.end() ? nullptr : it->second[pc & 0x3FFF];
}

// ======================================================================
// the interpreter
// ======================================================================
enum Step { ST_NEXT, ST_RET };

static inline uint8_t r8(GB& c, int i) {
    switch (i) {
    case 0: return c.B; case 1: return c.C; case 2: return c.D; case 3: return c.E;
    case 4: return c.H; case 5: return c.L; case 6: return gb_rd(c, GB_HL(c)); default: return c.A;
    }
}
static inline void w8(GB& c, int i, uint8_t v) {
    switch (i) {
    case 0: c.B = v; break; case 1: c.C = v; break; case 2: c.D = v; break; case 3: c.E = v; break;
    case 4: c.H = v; break; case 5: c.L = v; break; case 6: gb_wr(c, GB_HL(c), v); break; default: c.A = v; break;
    }
}
static inline uint16_t rr(GB& c, int i) {
    switch (i) { case 0: return GB_BC(c); case 1: return GB_DE(c); case 2: return GB_HL(c); default: return c.SP; }
}
static inline void wrr(GB& c, int i, uint16_t v) {
    switch (i) { case 0: gb_setbc(c, v); break; case 1: gb_setde(c, v); break; case 2: gb_sethl(c, v); break; default: c.SP = v; break; }
}

static Step step(GB& c) {
    const uint16_t at = c.PC;
    if (c.tr) gb_trace_insn(c, at);
    uint8_t op;
    if (c.hfetch) { op = gb_peek(c, at); c.hfetch = 0; c.PC++; }       // prefetched by HALT: no cycles
    else if (c.hbug) { op = gb_rd(c, at); c.hbug = 0; }               // the HALT bug: PC stays
    else op = gb_rd(c, c.PC++);
    c.n_interp++;
    GB_HIST(c, at);
    const int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    if (x == 1) {
        if (op == 0x76) { gb_halt(c, at); return ST_NEXT; }
        w8(c, y, r8(c, z)); return ST_NEXT;
    }
    if (x == 2) { gb_alu(c, y, r8(c, z)); return ST_NEXT; }
    switch (op) {
    case 0x00: return ST_NEXT;
    case 0x01: case 0x11: case 0x21: { uint8_t lo = gb_rd(c, c.PC++); uint8_t hi = gb_rd(c, c.PC++); wrr(c, op >> 4, (uint16_t)(hi << 8 | lo)); return ST_NEXT; }
    case 0x31: c.SP = gb_rd16(c, c.PC); c.PC += 2; return ST_NEXT;
    case 0x02: gb_wr(c, GB_BC(c), c.A); return ST_NEXT;
    case 0x12: gb_wr(c, GB_DE(c), c.A); return ST_NEXT;
    case 0x22: gb_wr(c, GB_HL(c), c.A); gb_sethl(c, (uint16_t)(GB_HL(c) + 1)); return ST_NEXT;
    case 0x32: gb_wr(c, GB_HL(c), c.A); gb_sethl(c, (uint16_t)(GB_HL(c) - 1)); return ST_NEXT;
    case 0x0A: c.A = gb_rd(c, GB_BC(c)); return ST_NEXT;
    case 0x1A: c.A = gb_rd(c, GB_DE(c)); return ST_NEXT;
    case 0x2A: c.A = gb_rd(c, GB_HL(c)); gb_sethl(c, (uint16_t)(GB_HL(c) + 1)); return ST_NEXT;
    case 0x3A: c.A = gb_rd(c, GB_HL(c)); gb_sethl(c, (uint16_t)(GB_HL(c) - 1)); return ST_NEXT;
    case 0x03: case 0x13: case 0x23: case 0x33: wrr(c, op >> 4, (uint16_t)(rr(c, op >> 4) + 1)); gb_tick(c, 4); return ST_NEXT;
    case 0x0B: case 0x1B: case 0x2B: case 0x3B: wrr(c, op >> 4, (uint16_t)(rr(c, op >> 4) - 1)); gb_tick(c, 4); return ST_NEXT;
    case 0x09: case 0x19: case 0x29: case 0x39: gb_addhl(c, rr(c, op >> 4)); gb_tick(c, 4); return ST_NEXT;
    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x3C: w8(c, y, gb_inc8(c, r8(c, y))); return ST_NEXT;
    case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x3D: w8(c, y, gb_dec8(c, r8(c, y))); return ST_NEXT;
    case 0x34: { uint16_t a = GB_HL(c); uint8_t v = gb_rd(c, a); uint8_t f = c.F & FC; v++; gb_wr(c, a, v);
                 c.F = (uint8_t)(f | (v ? 0 : FZ) | ((v & 0xF) ? 0 : FH)); return ST_NEXT; }
    case 0x35: { uint16_t a = GB_HL(c); uint8_t v = gb_rd(c, a); uint8_t f = (c.F & FC) | FN; v--; gb_wr(c, a, v);
                 c.F = (uint8_t)(f | (v ? 0 : FZ) | ((v & 0xF) == 0xF ? FH : 0)); return ST_NEXT; }
    case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x3E: { uint8_t v = gb_rd(c, c.PC++); w8(c, y, v); return ST_NEXT; }
    case 0x36: { uint8_t v = gb_rd(c, c.PC++); gb_wr(c, GB_HL(c), v); return ST_NEXT; }
    case 0x07: c.A = (uint8_t)(c.A << 1 | c.A >> 7); c.F = (c.A & 1) ? FC : 0; return ST_NEXT;
    case 0x0F: c.A = (uint8_t)(c.A >> 1 | c.A << 7); c.F = (c.A & 0x80) ? FC : 0; return ST_NEXT;
    case 0x17: { uint8_t f = (c.A & 0x80) ? FC : 0; c.A = (uint8_t)(c.A << 1 | ((c.F & FC) ? 1 : 0)); c.F = f; return ST_NEXT; }
    case 0x1F: { uint8_t f = (c.A & 1) ? FC : 0; c.A = (uint8_t)(c.A >> 1 | ((c.F & FC) ? 0x80 : 0)); c.F = f; return ST_NEXT; }
    case 0x27: gb_daa(c); return ST_NEXT;
    case 0x2F: c.A = (uint8_t)~c.A; c.F |= FN | FH; return ST_NEXT;
    case 0x37: c.F = (uint8_t)((c.F & FZ) | FC); return ST_NEXT;
    case 0x3F: c.F = (uint8_t)((c.F & FZ) | ((c.F & FC) ? 0 : FC)); return ST_NEXT;
    case 0x08: { uint16_t a = gb_rd16(c, c.PC); gb_wr(c, a, (uint8_t)c.SP); gb_wr(c, (uint16_t)(a + 1), (uint8_t)(c.SP >> 8)); c.PC += 2; return ST_NEXT; }
    case 0x10: gb_rd(c, c.PC++); gb_stop(c); return ST_NEXT;
    case 0x18: { int8_t e = (int8_t)gb_rd(c, c.PC++); c.PC = (uint16_t)(c.PC + e); gb_tick(c, 4);
                 if (c.tr) gb_trace_jump(c, 'j', at, c.PC); return ST_NEXT; }
    case 0x20: case 0x28: case 0x30: case 0x38: {
        int8_t e = (int8_t)gb_rd(c, c.PC++);
        if (gb_cond(c, (op >> 3) & 3)) { c.PC = (uint16_t)(c.PC + e); gb_tick(c, 4); if (c.tr) gb_trace_jump(c, 'j', at, c.PC); }
        return ST_NEXT;
    }
    case 0xC3: { c.PC = gb_rd16(c, c.PC); gb_tick(c, 4); if (c.tr) gb_trace_jump(c, 'j', at, c.PC); return ST_NEXT; }
    case 0xC2: case 0xCA: case 0xD2: case 0xDA: {
        uint16_t a = gb_rd16(c, c.PC); c.PC += 2;
        if (gb_cond(c, (op >> 3) & 3)) { c.PC = a; gb_tick(c, 4); if (c.tr) gb_trace_jump(c, 'j', at, a); }
        return ST_NEXT;
    }
    case 0xE9: c.PC = GB_HL(c); if (c.tr) gb_trace_jump(c, 'i', at, c.PC); return ST_NEXT;
    case 0xCD: { uint16_t a = gb_rd16(c, c.PC); c.PC += 2; gb_tick(c, 4); gb_push(c, c.PC);
                 if (c.tr) gb_trace_call(c, at, a, c.PC); c.PC = a; return ST_NEXT; }
    case 0xC4: case 0xCC: case 0xD4: case 0xDC: {
        uint16_t a = gb_rd16(c, c.PC); c.PC += 2;
        if (gb_cond(c, (op >> 3) & 3)) { gb_tick(c, 4); gb_push(c, c.PC); if (c.tr) gb_trace_call(c, at, a, c.PC); c.PC = a; }
        return ST_NEXT;
    }
    case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF:
        gb_tick(c, 4); gb_push(c, c.PC); if (c.tr) gb_trace_call(c, at, (uint16_t)(op & 0x38), c.PC);
        c.PC = op & 0x38; return ST_NEXT;
    case 0xC9: { uint16_t s = c.SP; c.PC = gb_rd16(c, c.SP); c.SP += 2; gb_tick(c, 4); if (c.tr) gb_trace_ret(c, at, s, c.PC); return ST_RET; }
    case 0xD9: { uint16_t s = c.SP; c.PC = gb_rd16(c, c.SP); c.SP += 2; c.ime = 1; gb_tick(c, 4); gb_recalc(c);
                 if (c.tr) gb_trace_ret(c, at, s, c.PC); return ST_RET; }
    case 0xC0: case 0xC8: case 0xD0: case 0xD8:
        gb_tick(c, 4);
        if (gb_cond(c, (op >> 3) & 3)) { uint16_t s = c.SP; c.PC = gb_rd16(c, c.SP); c.SP += 2; gb_tick(c, 4);
                                         if (c.tr) gb_trace_ret(c, at, s, c.PC); return ST_RET; }
        return ST_NEXT;
    case 0xC1: case 0xD1: case 0xE1: { uint8_t lo = gb_rd(c, c.SP++); uint8_t hi = gb_rd(c, c.SP++); wrr(c, (op >> 4) & 3, (uint16_t)(hi << 8 | lo)); return ST_NEXT; }
    case 0xF1: c.F = gb_rd(c, c.SP++); c.A = gb_rd(c, c.SP++); c.F &= 0xF0; return ST_NEXT;
    case 0xC5: case 0xD5: case 0xE5: gb_tick(c, 4); gb_push(c, rr(c, (op >> 4) & 3)); return ST_NEXT;
    case 0xF5: gb_tick(c, 4); c.F &= 0xF0; gb_push(c, (uint16_t)(c.A << 8 | c.F)); return ST_NEXT;
    case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE: { uint8_t v = gb_rd(c, c.PC++); gb_alu(c, y, v); return ST_NEXT; }
    case 0xE0: { uint8_t n = gb_rd(c, c.PC++); gb_wr(c, (uint16_t)(0xFF00 | n), c.A); return ST_NEXT; }
    case 0xF0: { uint8_t n = gb_rd(c, c.PC++); c.A = gb_rd(c, (uint16_t)(0xFF00 | n)); return ST_NEXT; }
    case 0xE2: gb_wr(c, (uint16_t)(0xFF00 | c.C), c.A); return ST_NEXT;
    case 0xF2: c.A = gb_rd(c, (uint16_t)(0xFF00 | c.C)); return ST_NEXT;
    case 0xEA: { uint16_t a = gb_rd16(c, c.PC); gb_wr(c, a, c.A); c.PC += 2; return ST_NEXT; }
    case 0xFA: { uint16_t a = gb_rd16(c, c.PC); c.A = gb_rd(c, a); c.PC += 2; return ST_NEXT; }
    case 0xE8: { uint8_t e = gb_rd(c, c.PC++); c.SP = gb_spe(c, e); gb_tick(c, 8); return ST_NEXT; }
    case 0xF8: { uint8_t e = gb_rd(c, c.PC++); gb_sethl(c, gb_spe(c, e)); gb_tick(c, 4); return ST_NEXT; }
    case 0xF9: c.SP = GB_HL(c); gb_tick(c, 4); return ST_NEXT;
    case 0xF3: gb_di(c); return ST_NEXT;
    case 0xFB: gb_ei(c); return ST_NEXT;
    case 0xCB: {
        uint8_t cb = gb_rd(c, c.PC++);
        int cx = cb >> 6, cy = (cb >> 3) & 7, cz = cb & 7;
        if (cz == 6) {
            uint16_t a = GB_HL(c);
            uint8_t v = gb_rd(c, a);
            if (cx == 1) { gb_bit(c, cy, v); return ST_NEXT; }
            v = cx == 0 ? gb_cbrot(c, cy, v) : cx == 2 ? (uint8_t)(v & ~(1 << cy)) : (uint8_t)(v | 1 << cy);
            gb_wr(c, a, v);
            return ST_NEXT;
        }
        uint8_t v = r8(c, cz);
        if (cx == 1) { gb_bit(c, cy, v); return ST_NEXT; }
        v = cx == 0 ? gb_cbrot(c, cy, v) : cx == 2 ? (uint8_t)(v & ~(1 << cy)) : (uint8_t)(v | 1 << cy);
        w8(c, cz, v);
        return ST_NEXT;
    }
    default:
        // undefined: MAME logs it and goes on
        if (c.tr) c.tr->undefined[op]++;
        return ST_NEXT;
    }
}

// The nested interpreter: run from PC until a return takes SP above `base` (the stack as the
// caller left it, its return address pushed).  Native code takes over at every native entry.
static inline bool sp_above(const GB& c, int base) { return (int)c.SP > base; }

void gb_exec(GB& c, int base) {
    c.n_fallback++;
    for (;;) {
        if (c.t >= c.next) {
            const uint16_t at = c.PC;                  // a handler may run the interpreter too
            if (!gb_poll(c, at)) {
                if (sp_above(c, base)) return;
                c.PC = c.resume;
                continue;
            }
            c.PC = at;
        }
        if (GBFn fn = (c.hfetch | c.hbug) ? nullptr : gb_lookup(c, c.PC)) {
            c.n_native_calls++;
            fn(c, c.PC);
            if (sp_above(c, base)) return;
            c.PC = c.resume;
            continue;
        }
        if (c.prof) c.prof[(c.PC < 0x4000 ? c.bank0 : c.PC < 0x8000 ? c.bankx : 0) * 0x10000u + c.PC]++;
        Step st = step(c);
        if (st == ST_RET) {
            c.resume = c.PC;
            if (sp_above(c, base)) return;
        }
    }
}

// Deeper than this, a call does not nest: it leaves its target in c.resume and returns, and its
// callers unwind to the dispatcher at the top, which goes on there -- the CPU's stack, which holds
// every return address, is untouched.
enum { CALL_DEPTH = 256 };

void gb_call(GB& c, uint16_t pc) {
    if (c.depth >= CALL_DEPTH) { c.resume = pc; c.n_unwind++; return; }
    if (++c.depth > c.max_depth) c.max_depth = c.depth;
    if (GBFn fn = (c.hfetch | c.hbug) ? nullptr : gb_lookup(c, pc)) { c.n_native_calls++; fn(c, pc); }
    else { c.PC = pc; gb_exec(c, c.SP); }
    c.depth--;
}

// ---- the flat interpreter: the reference, no native code, no nesting ----
bool gb_flat_frame(GB& c) {
    for (;;) {
        if (c.jammed) return false;
        if (c.halted) { if (!halt_run(c)) return true; continue; }
        if (c.t >= c.next) {
            if (!events(c)) return true;            // the frame: stop before delivering
            if (!ei_blocked(c, c.PC) && c.ime && (c.IE & c.IF & 0x1F)) {
                take_interrupt(c, c.PC);
                continue;                           // the handler's first boundary polls too
            }
        }
        if (c.prof) c.prof[(c.PC < 0x4000 ? c.bank0 : c.PC < 0x8000 ? c.bankx : 0) * 0x10000u + c.PC]++;
        step(c);
    }
}

void gb_run(GB& c) {
    gb_call(c, c.PC);
    for (;;) { c.PC = c.resume; gb_exec(c, 0x10000); }
}

// ======================================================================
// the machine
// ======================================================================
bool gb_load(GB& c, const uint8_t* rom, uint32_t size, bool cgb, int mbc, uint32_t ram_size, bool battery, bool rtc) {
    c.rom = rom; c.romsize = size; c.cgb = cgb;
    c.rom_banks = size >= 0x8000 ? (size + 0x3FFF) >> 14 : 2;
    c.mbc = mbc; c.battery = battery; c.has_rtc = rtc;
    if (mbc == MBC_2) ram_size = 0x200;
    c.sram_size = ram_size > sizeof c.sram ? (uint32_t)sizeof c.sram : ram_size;
    std::memset(c.sram, 0, sizeof c.sram);
    return true;
}

void gb_reset(GB& c) {
    // the CPU as the boot ROM leaves it (Pan Docs): A tells the game which machine it is on
    if (c.cgb) { c.A = 0x11; c.F = 0x80; c.B = 0x00; c.C = 0x00; c.D = 0xFF; c.E = 0x56; c.H = 0x00; c.L = 0x0D; }
    else { c.A = 0x01; c.F = 0xB0; c.B = 0x00; c.C = 0x13; c.D = 0x00; c.E = 0xD8; c.H = 0x01; c.L = 0x4D; }
    c.SP = 0xFFFE; c.PC = 0x0100;
    c.ime = 0; c.ei = 0; c.hfetch = c.hbug = 0; c.halted = c.halt_entering = 0; c.jammed = 0; c.ds = 0; c.key1 = 0; c.tmul = 2;
    c.t = 0; c.frame = 0; c.burn = 0;
    std::memset(c.wram, 0, sizeof c.wram); std::memset(c.vram, 0, sizeof c.vram);
    std::memset(c.oam, 0, sizeof c.oam); std::memset(c.hram, 0, sizeof c.hram);
    c.svbk = 1;
    // the cartridge, as its mapper starts
    c.mreg[0] = 0; c.mreg[1] = 1; c.mreg[2] = 0; c.mreg[3] = 0;
    c.ram_on = c.mbc == MBC_ROM; c.rtc_sel = 0; c.rtc_prev = 0xFF;
    std::memset(c.rtc, 0, sizeof c.rtc); std::memset(c.rtc_latch, 0, sizeof c.rtc_latch);
    c.rtc_at = TICK_HZ; c.rtc_sub = 0;
    // the timer and the ports (MAME's machine_reset, and DIV as the boot ROM leaves it)
    c.divcount = c.cgb ? 0x1EA0 : 0xABCC;
    c.tima = 0; c.tma = 0; c.tac = 0xF8; c.shift = 10; c.shift_cycles = 1024; c.triggering = 0; c.reloading = 0;
    c.sb = 0; c.sc = 0x7E; c.sio_count = 0; c.sio_clock = 0; c.sio_freq = 256; c.sio_n = 0;
    c.IE = 0; c.IF = 0x01;
    c.keys = 0; c.joyp = 0xCF;
    gb_map(c);
    ppu_reset(c);
    snd_reset(c);
    gb_recalc(c);
}

static uint64_t fnv(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

uint64_t gb_hash(const GB& c) {
    uint64_t h = 1469598103934665603ull;
    h = fnv(h, c.wram, c.cgb ? 0x8000 : 0x2000);
    h = fnv(h, c.vram, c.cgb ? 0x4000 : 0x2000);
    h = fnv(h, c.oam, sizeof c.oam);
    h = fnv(h, c.hram, sizeof c.hram);
    if (c.sram_size) h = fnv(h, c.sram, c.sram_size);
    h = fnv(h, c.fb, sizeof c.fb);
    uint8_t cpu[18] = { c.A, c.F, c.B, c.C, c.D, c.E, c.H, c.L, (uint8_t)c.SP, (uint8_t)(c.SP >> 8),
                        c.ime, c.ei, c.hfetch, c.hbug, c.ds, c.key1, c.halted, c.halt_entering };
    h = fnv(h, cpu, sizeof cpu);
    h = fnv(h, &c.t, sizeof c.t);
    uint8_t io[20] = { c.IE, c.IF, c.tima, c.tma, c.tac, c.shift, c.triggering, c.sb, c.sc, c.sio_count,
                       c.joyp, c.svbk, c.ram_on, c.mreg[0], c.mreg[1], c.mreg[2], c.mreg[3], c.rtc_sel, c.rtc_prev,
                       (uint8_t)c.divcount };
    h = fnv(h, io, sizeof io);
    uint16_t dv[2] = { c.divcount, c.sio_clock };
    h = fnv(h, dv, sizeof dv);
    h = fnv(h, c.rtc, 5); h = fnv(h, c.rtc_latch, 5);
    // the PPU, all but the pointers
    h = fnv(h, &c.v, sizeof c.v);
    // the sound's registers (what a game can read back), not its output
    h = fnv(h, c.s.regs, sizeof c.s.regs); h = fnv(h, c.s.wave, sizeof c.s.wave);
    uint8_t son[5] = { c.s.on, c.s.ch[0].on, c.s.ch[1].on, c.s.ch[2].on, c.s.ch[3].on };
    h = fnv(h, son, sizeof son);
    return h;
}

void gb_rgb(const GB& c, uint32_t* out) {
    for (int y = 0; y < VIS_H; y++)
        for (int x = 0; x < VIS_W; x++) {
            uint16_t p = c.fb[y][x];
            uint32_t r = p & 31, g = (p >> 5) & 31, b = (p >> 10) & 31;
            out[y * VIS_W + x] = (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
        }
}
