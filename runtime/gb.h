// The Game Boy and Game Boy Color under recompiled code: the machine, the SM83's
// state, and the helpers the generated C++ is written in.
//
// Time is counted in ticks of 8.388608 MHz: a dot (the LCD's clock, 4.194304 MHz)
// is 2 ticks, a CPU cycle 2 ticks at normal speed and 1 in the CGB's double
// speed.  A line is 456 dots, a frame 154 lines (59.73 Hz).
//
// The CPU is MAME's lr35902 core as its source has it: every memory access
// costs 4 CPU cycles, charged after the access, the internal delays where MAME
// puts them; each charge is also handed to the timer (DIV and TIMA count CPU
// cycles, so they run twice as fast in double speed).  The PPU, the OAM DMA,
// the HDMA and the sound are brought up to the present when a register of
// theirs is touched, and at every instruction boundary past their next event.
//
// The rule that holds the recompiled code and the interpreter together: both
// make the same accesses in the same order with the same charges, and both
// look at the machine's events and the interrupts at every instruction
// boundary at or past c.next.  So the machine at every frame is the same
// whichever way the code ran: the lockstep self-test (gb --lockstep) compares it.
#pragma once
#include <cstdint>
#include <cstring>

struct GB;
typedef void (*GBFn)(GB&, uint16_t);   // a region: entered at an address of its window

enum {
    VIS_W = 160, VIS_H = 144,
    DOT_T = 2,                         // ticks per dot
    LINE_DOTS = 456,
    LINE_T = LINE_DOTS * DOT_T,
    FRAME_T = 154 * LINE_T,
    TICK_HZ = 8388608,
};

// the mappers the runtime knows (cart.py's slots)
enum { MBC_ROM, MBC_1, MBC_2, MBC_3, MBC_5, MBC_HUC1, MBC_HUC3, MBC_7, MBC_NONE };
// interrupt sources (IF / IE bits); vectors $40 + 8n
enum { I_VBL = 1, I_STAT = 2, I_TIMER = 4, I_SERIAL = 8, I_JOYPAD = 16 };
// the F register
enum { FZ = 0x80, FN = 0x40, FH = 0x20, FC = 0x10 };

struct GBTrace;   // what the tracer measures (gb_core.cpp)
struct GBLock;    // what Pocketshift's lockstep replays (gb_lock.cpp)

struct PPU {
    uint8_t lcdc, stat, scy, scx, ly, lyc, bgp, obp0, obp1, wy, wx;
    uint8_t mode;             // what STAT shows (0-3)
    uint8_t st;               // the state machine's state
    uint64_t at;              // the tick of its next step
    uint8_t line;             // the line being run (LY, but counting on with the LCD off)
    uint8_t stat_line;        // the STAT interrupt line: an interrupt on its rise
    uint8_t vram_lock, oam_lock, pal_lock;
    uint8_t win_y_hit;        // WY matched a line this frame
    uint8_t win_line;         // the window's own line counter
    uint16_t m3_extra;        // the line's mode-3 penalty (fine SCX, sprites, window)
    uint8_t blank_frame;      // the first frame after the LCD is switched on is not shown
    // the OAM DMA (MAME's: the copy 8 dots after the write, then OAM locked for 640)
    uint8_t dma_reg; uint64_t dma_copy_at, dma_end_at;
    // the CGB
    uint8_t vbk, bcps, ocps;
    uint16_t bpal[32], opal[32];
    uint8_t hdma1, hdma2, hdma3, hdma4, hdma5, hdma_on, hdma_possible;
    uint8_t opri;
    uint8_t regs[0x40];       // $FF40-$FF7F as read back where nothing else answers
    uint8_t src_m2, src_lyc;  // STAT's mode-2 and LY=LYC sources (set and dropped at MAME's points)
    uint8_t frame_due, in_sync;
    uint8_t nspr;             // the line's sprites, selected in mode 2: y x tile attr index
    uint8_t spr[10][5];
};

struct SND {
    uint8_t regs[0x30];       // $FF10-$FF3F as written
    uint8_t wave[16];
    uint8_t on;               // NR52 bit 7
    struct Ch {
        uint8_t on, dac, duty, duty_pos, vol, env_dir, env_period, env_count, len_en;
        uint16_t len, freq; int32_t timer;
        uint8_t sweep_on, sweep_period, sweep_count, sweep_neg, sweep_shift; uint16_t sweep_freq;
        uint8_t wave_pos, wave_buf;
        uint16_t lfsr; uint8_t lfsr7, div, shift;
    } ch[4];
    uint8_t fs_step;          // the frame sequencer's step (512 Hz)
    uint64_t done;            // ticks run so far
    uint64_t fs_at;           // the next frame sequencer step
    uint32_t frac;            // the output's sample clock
    int16_t* out; uint32_t out_n, out_cap;
};

struct GB {
    // ---- the SM83 ----
    uint8_t A = 0, F = 0, B = 0, C = 0, D = 0, E = 0, H = 0, L = 0;
    uint16_t SP = 0, PC = 0;
    uint8_t ime = 0;
    uint8_t ei = 0;           // EI ran: no interrupt at the next boundary (2: blocked at ei_pc, ei_t)
    uint16_t ei_pc = 0; uint64_t ei_t = 0;
    uint8_t hfetch = 0;       // after HALT: the next opcode is already fetched (no cycles);
    uint8_t hbug = 0;         // ... and on the DMG with IME clear, PC is not advanced past it
    uint8_t halted = 0, halt_entering = 0; uint16_t halt_at = 0;   // in HALT (at halt_at)
    uint16_t resume = 0;      // where the last RET / RETI went
    uint8_t jammed = 0;
    uint8_t ds = 0;           // double speed (the CGB)
    uint8_t key1 = 0;         // a speed switch armed
    uint8_t tmul = 2;         // ticks per CPU cycle

    // ---- time ----
    uint64_t t = 0;           // ticks
    uint64_t next = 0;        // the next tick anything happens (0: an interrupt is to be looked at)
    uint64_t frame = 0;
    uint32_t burn = 0;        // DMA cycles the CPU still owes (MAME's dma_cycles_to_burn)

    // ---- the timer and the serial port (MAME's) ----
    uint16_t divcount = 0; uint8_t tima = 0, tma = 0, tac = 0xF8, shift = 10;
    uint16_t shift_cycles = 1024; uint8_t triggering = 0, reloading = 0;
    uint8_t sb = 0, sc = 0x7E, sio_count = 0; uint16_t sio_clock = 0, sio_freq = 256;
    uint8_t sio_out[4096]; uint32_t sio_n = 0;   // the bytes sent (the test ROMs' output)

    // ---- interrupts ----
    uint8_t IE = 0, IF = 0;

    // ---- the pad: bits 0-7 right left up down A B select start (1 = held) ----
    uint8_t keys = 0, joyp = 0xCF;

    // ---- memory: 4 KB pages; null = a handler (all null under the tracer, which sees every access) ----
    const uint8_t* rdp[16];
    uint8_t* wrp[16];
    const uint8_t* rd_pg[16];     // what is mapped (rdp is this, but under the tracer)
    uint8_t* wr_pg[16];
    uint8_t wram[0x8000];
    uint8_t vram[0x4000];
    uint8_t oam[0xA0];
    uint8_t hram[0x80];
    uint8_t svbk = 0;
    bool cgb = false;         // the machine: a CGB (in colour mode), else a DMG

    // ---- the cartridge ----
    const uint8_t* rom = nullptr;
    uint32_t romsize = 0, rom_banks = 2;
    int mbc = MBC_ROM;
    bool battery = false, has_rtc = false;
    uint8_t sram[0x20000];
    uint32_t sram_size = 0;
    uint8_t mreg[4] = { 0, 1, 0, 0 };   // the mapper's registers as written
    uint8_t ram_on = 0;
    uint32_t bank0 = 0, bankx = 1;       // the ROM banks at $0000 and $4000
    uint32_t ram_bank = 0;
    uint8_t rtc_sel = 0;                 // MBC3: $08-$0C = an RTC register at $A000
    uint8_t rtc[5] = { 0 }, rtc_latch[5] = { 0 }, rtc_prev = 0xFF;
    uint64_t rtc_at = 0;                 // the RTC's next second (emulated time: deterministic)
    uint32_t rtc_sub = 0;

    PPU v;
    SND s;
    uint16_t fb[VIS_H][VIS_W];           // the picture: 15-bit BGR (the CGB's)

    // ---- how the code runs ----
    bool natives = true;      // false: the interpreter alone (the reference)
    bool flat_stop = false;   // the flat interpreter stops at the frame event
    bool snd_out = false;     // keep the sound's samples
    void (*frame_hook)(GB&) = nullptr;
    bool lcd_shown = false;   // the frame just ended was drawn (not blank, not off)

    // the last instructions run, native and interpreted (with GB_HISTORY)
    uint32_t hist[65536]; uint64_t htick[65536]; uint32_t hn = 0;
    GBTrace* tr = nullptr;
    GBLock* lk = nullptr;     // --lockrec: what Pocketshift's lockstep replays (gb_lock.cpp)
    uint32_t* prof = nullptr; // --profile: interpreted instructions by (window, bank, address)

    // ---- what ran ----
    uint64_t n_interp = 0, n_native_calls = 0, n_fallback = 0, n_unwind = 0, n_irq = 0, n_halt = 0;
    int depth = 0, max_depth = 0;

    GB() {
        std::memset(wram, 0, sizeof wram); std::memset(vram, 0, sizeof vram); std::memset(oam, 0, sizeof oam);
        std::memset(hram, 0, sizeof hram); std::memset(sram, 0, sizeof sram); std::memset(fb, 0, sizeof fb);
        std::memset(&v, 0, sizeof v); std::memset(&s, 0, sizeof s);
        for (int i = 0; i < 16; i++) { rdp[i] = rd_pg[i] = nullptr; wrp[i] = wr_pg[i] = nullptr; }
    }
};

// ---- time ----
void gb_timer(GB& c, uint32_t n);                 // MAME's timer callback, out of line
// MAME's cycles_passed(n): n CPU cycles
static inline void gb_tick(GB& c, uint32_t n) {
    c.t += n * c.tmul;
    if (!(c.tac & 4) && !c.triggering && !(c.sc & 0x81 & (c.sc << 7))) { c.divcount += n; c.sio_clock += n; return; }
    gb_timer(c, n);
}

// ---- the bus ----
uint8_t gb_rd_slow(GB& c, uint16_t a);
void gb_wr_slow(GB& c, uint16_t a, uint8_t v);
static inline uint8_t gb_peek(GB& c, uint16_t a) {     // a read with its side effects but no time
    const uint8_t* p = c.rdp[a >> 12];
    return p ? p[a & 0xFFF] : gb_rd_slow(c, a);
}
static inline uint8_t gb_rd(GB& c, uint16_t a) {
    const uint8_t* p = c.rdp[a >> 12];
    uint8_t v = p ? p[a & 0xFFF] : gb_rd_slow(c, a);
    gb_tick(c, 4);
    return v;
}
static inline void gb_wr(GB& c, uint16_t a, uint8_t v) {
    uint8_t* p = c.wrp[a >> 12];
    if (p) p[a & 0xFFF] = v; else gb_wr_slow(c, a, v);
    gb_tick(c, 4);
}
static inline uint16_t gb_rd16(GB& c, uint16_t a) {
    uint16_t lo = gb_rd(c, a); return (uint16_t)(lo | gb_rd(c, (uint16_t)(a + 1)) << 8);
}
void gb_map(GB& c);                    // after a mapper register, SVBK or VBK changed

// ---- registers ----
#define GB_BC(c) ((uint16_t)((c).B << 8 | (c).C))
#define GB_DE(c) ((uint16_t)((c).D << 8 | (c).E))
#define GB_HL(c) ((uint16_t)((c).H << 8 | (c).L))
static inline void gb_setbc(GB& c, uint16_t v) { c.B = (uint8_t)(v >> 8); c.C = (uint8_t)v; }
static inline void gb_setde(GB& c, uint16_t v) { c.D = (uint8_t)(v >> 8); c.E = (uint8_t)v; }
static inline void gb_sethl(GB& c, uint16_t v) { c.H = (uint8_t)(v >> 8); c.L = (uint8_t)v; }

// ---- the stack (MAME's PUSH: the high byte first) ----
static inline void gb_push(GB& c, uint16_t v) {
    c.SP--; gb_wr(c, c.SP, (uint8_t)(v >> 8)); c.SP--; gb_wr(c, c.SP, (uint8_t)v);
}
static inline uint16_t gb_pop(GB& c) {
    uint16_t lo = gb_rd(c, c.SP++); uint16_t hi = gb_rd(c, c.SP++); return (uint16_t)(lo | hi << 8);
}

// ---- the ALU, flags as MAME's macros set them ----
static inline uint8_t gb_inc8(GB& c, uint8_t x) {
    uint8_t r = (uint8_t)(x + 1); uint8_t f = c.F & FC;
    if (!r) f |= FZ; if (!(r & 0xF)) f |= FH; c.F = f; return r;
}
static inline uint8_t gb_dec8(GB& c, uint8_t x) {
    uint8_t r = (uint8_t)(x - 1); uint8_t f = (c.F & FC) | FN;
    if (!r) f |= FZ; if ((r & 0xF) == 0xF) f |= FH; c.F = f; return r;
}
static inline void gb_add(GB& c, uint8_t x) {
    unsigned r1 = (c.A & 0xF) + (x & 0xF), r2 = c.A + x;
    c.A = (uint8_t)r2; c.F = (uint8_t)((c.A ? 0 : FZ) | (r2 > 0xFF ? FC : 0) | (r1 > 0xF ? FH : 0));
}
static inline void gb_adc(GB& c, uint8_t x) {
    unsigned ci = (c.F & FC) ? 1 : 0;
    unsigned r1 = (c.A & 0xF) + (x & 0xF) + ci, r2 = c.A + x + ci;
    c.A = (uint8_t)r2; c.F = (uint8_t)((c.A ? 0 : FZ) | (r2 > 0xFF ? FC : 0) | (r1 > 0xF ? FH : 0));
}
static inline void gb_sub(GB& c, uint8_t x) {
    uint16_t r1 = (uint16_t)((c.A & 0xF) - (x & 0xF)), r2 = (uint16_t)(c.A - x);
    c.A = (uint8_t)r2; c.F = (uint8_t)(FN | (c.A ? 0 : FZ) | (r2 > 0xFF ? FC : 0) | (r1 > 0xF ? FH : 0));
}
static inline void gb_sbc(GB& c, uint8_t x) {
    unsigned ci = (c.F & FC) ? 1 : 0;
    uint16_t r1 = (uint16_t)((c.A & 0xF) - (x & 0xF) - ci), r2 = (uint16_t)(c.A - x - ci);
    c.A = (uint8_t)r2; c.F = (uint8_t)(FN | (c.A ? 0 : FZ) | (r2 > 0xFF ? FC : 0) | (r1 > 0xF ? FH : 0));
}
static inline void gb_cp(GB& c, uint8_t x) {
    uint16_t r1 = (uint16_t)((c.A & 0xF) - (x & 0xF)), r2 = (uint16_t)(c.A - x);
    c.F = (uint8_t)(FN | ((uint8_t)r2 ? 0 : FZ) | (r2 > 0xFF ? FC : 0) | (r1 > 0xF ? FH : 0));
}
static inline void gb_and(GB& c, uint8_t x) { c.A &= x; c.F = c.A ? FH : (FH | FZ); }
static inline void gb_xor(GB& c, uint8_t x) { c.A ^= x; c.F = c.A ? 0 : FZ; }
static inline void gb_or(GB& c, uint8_t x) { c.A |= x; c.F = c.A ? 0 : FZ; }
static inline void gb_alu(GB& c, int op, uint8_t x) {
    switch (op) {
    case 0: gb_add(c, x); break; case 1: gb_adc(c, x); break; case 2: gb_sub(c, x); break; case 3: gb_sbc(c, x); break;
    case 4: gb_and(c, x); break; case 5: gb_xor(c, x); break; case 6: gb_or(c, x); break; default: gb_cp(c, x); break;
    }
}
static inline void gb_addhl(GB& c, uint16_t x) {
    uint16_t hl = GB_HL(c);
    uint32_t r1 = (uint32_t)hl + x, r2 = (uint32_t)(hl & 0xFFF) + (x & 0xFFF);
    c.F = (uint8_t)((c.F & FZ) | (r1 > 0xFFFF ? FC : 0) | (r2 > 0xFFF ? FH : 0));
    gb_sethl(c, (uint16_t)r1);
}
static inline uint16_t gb_spe(GB& c, uint8_t e) {        // ADD SP,e and LD HL,SP+e: the sum, F set
    int n = (int8_t)e;
    c.F = (uint8_t)(((c.SP & 0xFF) + (uint8_t)n > 0xFF ? FC : 0) | ((c.SP & 0xF) + (n & 0xF) > 0xF ? FH : 0));
    return (uint16_t)(c.SP + n);
}
void gb_daa(GB& c);
// CB rotates and shifts: op 0-7 = rlc rrc rl rr sla sra swap srl
static inline uint8_t gb_cbrot(GB& c, int op, uint8_t x) {
    uint8_t f = 0, r;
    switch (op) {
    case 0: r = (uint8_t)(x << 1 | x >> 7); f = (r & 1) ? FC : 0; break;
    case 1: r = (uint8_t)(x >> 1 | x << 7); f = (r & 0x80) ? FC : 0; break;
    case 2: r = (uint8_t)(x << 1 | ((c.F & FC) ? 1 : 0)); f = (x & 0x80) ? FC : 0; break;
    case 3: r = (uint8_t)(x >> 1 | ((c.F & FC) ? 0x80 : 0)); f = (x & 1) ? FC : 0; break;
    case 4: r = (uint8_t)(x << 1); f = (x & 0x80) ? FC : 0; break;
    case 5: r = (uint8_t)((int8_t)x >> 1); f = (x & 1) ? FC : 0; break;
    case 6: r = (uint8_t)(x >> 4 | x << 4); break;
    default: r = (uint8_t)(x >> 1); f = (x & 1) ? FC : 0; break;
    }
    c.F = (uint8_t)(f | (r ? 0 : FZ)); return r;
}
static inline void gb_bit(GB& c, int n, uint8_t x) {
    c.F = (uint8_t)(FH | (c.F & FC) | ((x >> n) & 1 ? 0 : FZ));
}
static inline bool gb_cond(const GB& c, int cc) {   // nz z nc c
    switch (cc) { case 0: return !(c.F & FZ); case 1: return (c.F & FZ) != 0; case 2: return !(c.F & FC); default: return (c.F & FC) != 0; }
}

// ---- time and interrupts ----
void gb_recalc(GB& c);                     // c.next from the events and the interrupt line
bool gb_poll(GB& c, uint16_t at);          // the boundary before `at`: events, an interrupt (false: unwinding)
void gb_halt(GB& c, uint16_t at);          // HALT at `at` (PC is at+1): until an interrupt is pending
void gb_stop(GB& c);                       // STOP's second byte read: the CGB's speed switch
void gb_ei(GB& c);                         // EI
void gb_di(GB& c);                         // DI
#ifdef GB_HISTORY
#define GB_HIST(c, pc) ((c).hist[(c).hn & 65535] = (uint32_t)(c).bankx << 16 | (pc), (c).htick[(c).hn++ & 65535] = (c).t)
#else
#define GB_HIST(c, pc) ((void)0)
#endif
// in a function compiled for window w holding ROM bank `bank`: leave if the bank changed under it
bool gb_poll_at(GB& c, uint16_t at, int w, uint32_t bank);    // false: the caller returns
#define GB_POLL(c, at, w, bank) do { if ((c).t >= (c).next && !gb_poll_at((c), (at), (w), (bank))) return; \
    GB_HIST(c, at); } while (0)
// code compiled from RAM (WRAM, HRAM: the OAM DMA routine) holds only while RAM holds the bytes it was
// compiled from: `ok` says so.  The poll leaves to the interpreter when they changed under it.
typedef bool (*GBGuard)(const GB&);
bool gb_poll_ram(GB& c, uint16_t at, GBGuard ok);             // false: the caller returns
#define GB_POLLR(c, at, ok) do { if ((c).t >= (c).next && !gb_poll_ram((c), (at), (ok))) return; \
    GB_HIST(c, at); } while (0)
// the byte at a in WRAM ($C000-$DFFF, the CGB's bank as mapped) or HRAM, without side effects
static inline uint8_t gb_ram_byte(const GB& c, uint16_t a) {
    return a >= 0xFF80 ? c.hram[a - 0xFF80] : c.rd_pg[a >> 12][a & 0xFFF];
}

// ---- calls between native and interpreted code ----
void gb_call(GB& c, uint16_t pc);          // run code at pc as a called routine
GBFn gb_lookup(GB& c, uint16_t pc);        // the native function there (banks as now), or null
void gb_exec(GB& c, int base);             // interpret from PC until a return lifts SP above base
void gb_register(int w, uint32_t bank, uint16_t pc, GBFn fn);   // fn(c, pc) runs from pc
void gb_register_ram(uint16_t pc, GBFn fn, GBGuard ok);          // ... in RAM, while ok(c)
void gb_register_all();                    // generated: every native function
[[noreturn]] void gb_jam(GB& c, const char* why);

// ---- the machine ----
bool gb_load(GB& c, const uint8_t* rom, uint32_t size, bool cgb, int mbc, uint32_t ram_size, bool battery, bool rtc);
void gb_reset(GB& c);
void gb_run(GB& c);                // from the reset, forever (frame_hook decides)
bool gb_flat_frame(GB& c);         // the interpreter alone, to the next frame event

// ---- --lockrec (gb_lock.cpp): the interpreter's run, for Pocketshift's lockstep ----
bool gb_lock_open(GB& c, const char* path);
void gb_lock_close(GB& c);
void gb_lock_insn(GB& c, uint16_t at);           // an instruction about to run
bool gb_lock_volatile(uint8_t reg);              // a register read the hardware's moment decides
void gb_lock_read(GB& c, uint8_t reg, uint8_t v);
void gb_lock_irq(GB& c, int n);                  // an interrupt about to be taken
void gb_lock_wake(GB& c);                        // HALT left, or not entered (the HALT bug)
void gb_lock_sp(GB& c);                          // SP set by LD SP
uint64_t gb_hash(const GB& c);
void gb_rgb(const GB& c, uint32_t* out);   // the 160x144 picture, 0xRRGGBB
struct GBStop {};                  // thrown by a frame hook to end the run
void gb_irq(GB& c, uint8_t bit);   // a source raises its interrupt flag

// ---- video (gb_video.cpp) ----
void ppu_sync(GB& c);              // run the PPU up to now
uint64_t ppu_next(const GB& c);    // its next step (the OAM DMA's too)
uint8_t ppu_read(GB& c, uint16_t a);       // $FF40-$FF7F
void ppu_write(GB& c, uint16_t a, uint8_t v);
uint8_t vram_read(GB& c, uint16_t a);
void vram_write(GB& c, uint16_t a, uint8_t v);
uint8_t oam_read(GB& c, uint16_t a);
void oam_write(GB& c, uint16_t a, uint8_t v);
void ppu_reset(GB& c);
bool ppu_frame_due(GB& c);         // a frame ended at the last sync (cleared as read)

// ---- sound (gb_sound.cpp) ----
void snd_sync(GB& c);
uint8_t snd_read(GB& c, uint16_t a);       // $FF10-$FF3F
void snd_write(GB& c, uint16_t a, uint8_t v);
void snd_reset(GB& c);
void gb_write_wav(const char* path, const int16_t* s, size_t n, int rate);
enum { SND_RATE = 32768 };

// ---- the tracer ----
void gb_trace_begin(GB& c);
void gb_trace_report(GB& c, const char* dir, const char* rom_path);
