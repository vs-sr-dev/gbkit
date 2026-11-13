// --lockrec FILE: what Pocketshift (the GB-to-Game-Gear translator) needs to run a translation
// in lockstep with this interpreter.  The two machines' clocks cannot agree, so the common
// measure is the count of instructions run from the ROM (the translation has one label per
// such instruction; code in RAM -- the OAM DMA routine -- is the translator's own business).
// Recorded, in order:
//   'R' reg value count          a read of a register whose value is the hardware's moment
//                                (P1, DIV, IF, NR52, STAT, LY): replayed on the other side
//   'I' n halted SP top frame count WRAM[8192] HRAM[127]
//                                an interrupt taken after `count` instructions (from HALT or
//                                not), with the work RAM as it was: compared on the other side
//   'H' frame count              HALT left with IME clear (or not entered: an interrupt
//                                already pending): the other side's HALT must end there too
// Little endian, after the header "PSLK" and a version byte (1).
#include "gb.h"
#include <cstdio>
#include <cstring>

struct GBLock {
    FILE* f = nullptr;
    uint64_t count = 0;        // instructions run from the ROM
    uint16_t top = 0xFFFE;     // where the stack starts: the last LD SP
    bool woke = false;         // HALT just left with IME set: the interrupt comes from it
};

static void put(FILE* f, const void* p, size_t n) { std::fwrite(p, 1, n, f); }

bool gb_lock_open(GB& c, const char* path) {
    GBLock* k = new GBLock;
    k->f = std::fopen(path, "wb");
    if (!k->f) { delete k; return false; }
    put(k->f, "PSLK\x01", 5);
    c.lk = k;
    return true;
}

void gb_lock_close(GB& c) {
    if (!c.lk) return;
    std::fclose(c.lk->f);
    delete c.lk;
    c.lk = nullptr;
}

void gb_lock_insn(GB& c, uint16_t at) {
    if (at < 0x8000) c.lk->count++;
}

bool gb_lock_volatile(uint8_t reg) {
    return reg == 0x00 || reg == 0x04 || reg == 0x0F || reg == 0x26 || reg == 0x41 || reg == 0x44;
}

void gb_lock_read(GB& c, uint8_t reg, uint8_t v) {
    GBLock* k = c.lk;
    std::fputc('R', k->f);
    std::fputc(reg, k->f);
    std::fputc(v, k->f);
    put(k->f, &k->count, 8);
}

void gb_lock_irq(GB& c, int n) {
    GBLock* k = c.lk;
    std::fputc('I', k->f);
    std::fputc(n, k->f);
    std::fputc(k->woke ? 1 : 0, k->f);
    put(k->f, &c.SP, 2);
    put(k->f, &k->top, 2);
    uint32_t fr = (uint32_t)c.frame;
    put(k->f, &fr, 4);
    put(k->f, &k->count, 8);
    put(k->f, c.wram, 0x2000);
    put(k->f, c.hram, 0x7F);
    k->woke = false;
}

void gb_lock_wake(GB& c) {
    GBLock* k = c.lk;
    if (c.ime) { k->woke = true; return; }
    std::fputc('H', k->f);
    uint32_t fr = (uint32_t)c.frame;
    put(k->f, &fr, 4);
    put(k->f, &k->count, 8);
}

void gb_lock_sp(GB& c) {
    c.lk->top = c.SP;
}
