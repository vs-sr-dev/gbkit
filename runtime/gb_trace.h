// What the tracer measures: what a recompiler faces in a game, and the columns
// of the library's survey (the ones Pocketshift, the GB-to-Game-Gear translator,
// needs too).  Only the interpreter feeds it (the tracer runs no native code).
#pragma once
#include "gb.h"
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

// a flag per ROM byte (cov.bin)
enum { R_OP = 1, R_ARG = 2, R_READ = 4, R_JTGT = 8, R_RET = 16 };

struct GBSlot { uint16_t ret; uint8_t kind; uint32_t bank; };   // a return address pushed (kind 0 call, 1 rst, 2 irq)

// what one instruction (a site: its bank and address) reads and writes beyond plain RAM -- Pocketshift's
// question: which accesses reach the hardware, and through which addresses
enum { A_VRAM = 1, A_OAM = 2, A_IO = 4, A_MBC = 8, A_SRAM = 16, A_ECHO = 32, A_IE = 64 };
struct GBSite { uint8_t w = 0, r = 0; uint64_t io_w[2] = { 0, 0 }, io_r[2] = { 0, 0 }; uint64_t n = 0; };

struct GBTrace {
    std::vector<uint8_t> cov;                  // a flag set per ROM byte
    std::map<uint64_t, uint64_t> where;        // (window, bank) -> instructions run there
    std::vector<uint8_t> ramexec;              // $8000-$FFFF: run from there
    std::vector<uint8_t> ramwritten;           // ... and written after it ran (SMC)
    uint64_t ramexec_n = 0, smc_n = 0, ramexec_hram = 0, ramexec_wram = 0, ramexec_other = 0;
    // the bytes each instruction in RAM was run with (len << 24 | bytes), up to RAMVER_MAX versions
    // an address; more: RAMVER_MANY (code rewritten all the time: left to the interpreter)
    enum { RAMVER_MAX = 4 };
    static const uint32_t RAMVER_MANY = 0xFFFFFFFFu;
    std::unordered_map<uint16_t, std::vector<uint32_t>> ramver;
    std::map<std::string, uint64_t> edges;     // "k from -> to"
    std::unordered_map<uint32_t, GBSite> sites;  // (bank << 16 | address; bank $FFF: RAM) -> its accesses
    std::unordered_map<uint16_t, GBSlot> slots;  // by SP
    uint64_t insn_n = 0, comp_ret = 0, dropped = 0, halts = 0, halt_bugs = 0, stops = 0, speed_switches = 0;
    uint64_t undefined[256] = { 0 };
    uint64_t irqs[5] = { 0 };
    uint64_t rd_n[256] = { 0 }, wr_n[256] = { 0 };      // $FF00-$FFFF
    uint64_t ops[512] = { 0 };
    uint64_t bank_w = 0, bank_change = 0, bank_own = 0, ram_enable = 0;
    uint64_t echo_n = 0;                       // accesses to $E000-$FDFF
    // VRAM and I/O written through a fixed address (ld [nn],a / ldh [n],a) or a computed one
    uint64_t vram_w_fixed = 0, vram_w_ptr = 0, io_w_fixed = 0, io_w_ptr = 0, io_r_ptr = 0;
    uint64_t oam_w_cpu = 0, oam_dma = 0, hdma_general = 0, hdma_hblank = 0;
    uint64_t raster_w = 0;                     // SCX SCY LCDC WX WY palettes written in lines 0-143 outside VBlank
    std::map<std::string, uint64_t> raster_regs;
    uint64_t frames_window = 0, frames_tall_sprites = 0, frames_drawn = 0;
    uint32_t max_sprites_line = 0;             // sprites on a line before the limit of 10
    uint64_t lines_over8 = 0, lines_over10 = 0;
    uint64_t stat_irq_frame = 0, stat_irq_max = 0, lyc_hits = 0;
    uint32_t cgb_bpal_used = 0, cgb_opal_used = 0;    // bit masks of the palettes drawn with
    uint8_t tiles[2][384 / 8] = { { 0 } };     // tiles written, per VRAM bank
    std::map<uint32_t, uint64_t> irq_at;       // where the program was at an interrupt (bank << 16 | pc)
    int irq_depth = 0, irq_depth_max = 0;
    // the current instruction (for telling an operand fetch from a data read)
    uint16_t cur_at = 0; uint8_t cur_len = 0, cur_op = 0;
    // this frame's
    uint8_t win_f = 0, tall_f = 0; uint32_t stat_f = 0;
    GBTrace() : ramexec(0x8000, 0), ramwritten(0x8000, 0) {}
};

void gb_trace_insn(GB& c, uint16_t at);          // before the instruction's fetch
void gb_trace_bank(GB& c, uint32_t old0, uint32_t oldx);   // after a mapper write
void gb_trace_jump(GB& c, char k, uint16_t from, uint16_t to);
void gb_trace_call(GB& c, uint16_t from, uint16_t to, uint16_t ret);
void gb_trace_ret(GB& c, uint16_t at, uint16_t sp, uint16_t to);
void gb_trace_irq(GB& c, int n, uint16_t at);
void gb_trace_read(GB& c, uint16_t a);
void gb_trace_write(GB& c, uint16_t a, uint8_t v);
void gb_trace_frame(GB& c);
