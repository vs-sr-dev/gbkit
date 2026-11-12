// The APU: two pulse channels (sweep on the first), the wave channel, noise, the
// frame sequencer at 512 Hz (lengths, sweep, envelopes), and the mixer.  It is run
// on demand, to the tick, when a sound register is touched and at every frame, so
// what a game reads back (NR52's channel bits, the wave RAM) is the same whichever
// way the code ran.  Read masks as MAME's.  The output is not part of the state compared.
#include "gb.h"
#include <cstdio>

static const uint8_t READ_MASK[0x30] = {
    0x80,0x3f,0x00,0xff,0xbf,0xff,0x3f,0x00,0xff,0xbf,0x7f,0xff,0x9f,0xff,0xbf,0xff,
    0xff,0x00,0x00,0xbf,0x00,0x00,0x70,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};
static const uint8_t DUTY[4] = { 0x01, 0x81, 0x87, 0x7E };
static const uint8_t NOISE_DIV[8] = { 8, 16, 32, 48, 64, 80, 96, 112 };
enum { FS_T = TICK_HZ / 512, SAMPLE_T = TICK_HZ / SND_RATE };

static inline uint16_t ch_freq(const SND& s, int n) {
    int b = 0x10 + n * 5;   // NRx3 at b + 3 - 0x10 ...
    (void)b;
    const uint8_t* r = s.regs;
    int lo = n == 0 ? 0x03 : n == 1 ? 0x08 : 0x0D;
    return (uint16_t)(r[lo] | (r[lo + 1] & 7) << 8);
}

static int32_t period(const SND& s, int n) {
    const SND::Ch& ch = s.ch[n];
    if (n < 2) return (2048 - ch.freq) * 8;                    // a duty step: 4 dots
    if (n == 2) return (2048 - ch.freq) * 4;                   // a sample: 2 dots
    int d = NOISE_DIV[s.regs[0x12] & 7] << (s.regs[0x12] >> 4);
    return d * 2;
}

static uint16_t sweep_calc(SND& s) {
    SND::Ch& ch = s.ch[0];
    uint16_t d = (uint16_t)(ch.sweep_freq >> ch.sweep_shift);
    uint16_t f = ch.sweep_neg ? (uint16_t)(ch.sweep_freq - d) : (uint16_t)(ch.sweep_freq + d);
    if (f > 2047) ch.on = 0;
    return f;
}

static void frame_step(SND& s) {
    int st = s.fs_step;
    if (!(st & 1)) {                                           // lengths
        for (int n = 0; n < 4; n++) {
            SND::Ch& ch = s.ch[n];
            if (ch.len_en && ch.len) { if (--ch.len == 0) ch.on = 0; }
        }
    }
    if (st == 2 || st == 6) {                                  // the sweep
        SND::Ch& ch = s.ch[0];
        if (ch.sweep_count && --ch.sweep_count == 0) {
            ch.sweep_count = ch.sweep_period ? ch.sweep_period : 8;
            if (ch.sweep_on && ch.sweep_period) {
                uint16_t f = sweep_calc(s);
                if (f <= 2047 && ch.sweep_shift) {
                    ch.sweep_freq = f; ch.freq = f;
                    s.regs[0x03] = (uint8_t)f; s.regs[0x04] = (uint8_t)((s.regs[0x04] & 0xF8) | (f >> 8));
                    sweep_calc(s);
                }
            }
        }
    }
    if (st == 7) {                                             // envelopes
        for (int n = 0; n < 4; n++) {
            if (n == 2) continue;
            SND::Ch& ch = s.ch[n];
            if (!ch.env_period) continue;
            if (ch.env_count && --ch.env_count == 0) {
                ch.env_count = ch.env_period;
                if (ch.env_dir && ch.vol < 15) ch.vol++;
                else if (!ch.env_dir && ch.vol > 0) ch.vol--;
            }
        }
    }
    s.fs_step = (uint8_t)((st + 1) & 7);
}

static void run_channels(SND& s, int32_t dt) {
    for (int n = 0; n < 4; n++) {
        SND::Ch& ch = s.ch[n];
        if (!ch.on) continue;
        int32_t p = period(s, n);
        if (p <= 0) p = 1;
        ch.timer -= dt;
        while (ch.timer <= 0) {
            ch.timer += p;
            if (n < 2) ch.duty_pos = (uint8_t)((ch.duty_pos + 1) & 7);
            else if (n == 2) { ch.wave_pos = (uint8_t)((ch.wave_pos + 1) & 31); ch.wave_buf = s.wave[ch.wave_pos >> 1]; }
            else {
                uint16_t x = (ch.lfsr ^ (ch.lfsr >> 1)) & 1;
                ch.lfsr = (uint16_t)((ch.lfsr >> 1) | x << 14);
                if (s.regs[0x12] & 8) ch.lfsr = (uint16_t)((ch.lfsr & ~0x40) | x << 6);
            }
        }
    }
}

static void mix(SND& s) {
    int l = 0, r = 0;
    uint8_t nr51 = s.regs[0x15], nr50 = s.regs[0x14];
    for (int n = 0; n < 4; n++) {
        SND::Ch& ch = s.ch[n];
        int o = 0;
        if (ch.on && ch.dac) {
            if (n < 2) o = ((DUTY[ch.duty] >> (7 - ch.duty_pos)) & 1) ? ch.vol : 0;
            else if (n == 2) {
                int smp = (ch.wave_pos & 1) ? (ch.wave_buf & 0x0F) : (ch.wave_buf >> 4);
                int sh = (s.regs[0x0C] >> 5) & 3;
                o = sh ? smp >> (sh - 1) : 0;
            } else o = (~ch.lfsr & 1) ? ch.vol : 0;
        }
        if (nr51 & (0x10 << n)) l += o;
        if (nr51 & (0x01 << n)) r += o;
    }
    l *= ((nr50 >> 4) & 7) + 1; r *= (nr50 & 7) + 1;
    if (s.out_n + 2 > s.out_cap) {
        uint32_t cap = s.out_cap ? s.out_cap * 2 : 65536;
        int16_t* p = new int16_t[cap];
        if (s.out) { std::memcpy(p, s.out, s.out_n * sizeof(int16_t)); delete[] s.out; }
        s.out = p; s.out_cap = cap;
    }
    s.out[s.out_n++] = (int16_t)(l * 64); s.out[s.out_n++] = (int16_t)(r * 64);
}

void snd_sync(GB& c) {
    SND& s = c.s;
    while (s.done < c.t) {
        uint64_t nxt = s.fs_at;
        uint64_t smp = (s.done / SAMPLE_T + 1) * SAMPLE_T;
        if (smp < nxt) nxt = smp;
        if (c.t < nxt) nxt = c.t;
        if (s.on) run_channels(s, (int32_t)(nxt - s.done));
        s.done = nxt;
        if (s.done == s.fs_at) { if (s.on) frame_step(s); s.fs_at += FS_T; }
        if (s.done == smp && c.snd_out) mix(s);
    }
}

static void trigger(SND& s, int n) {
    SND::Ch& ch = s.ch[n];
    const uint8_t* r = s.regs;
    ch.on = ch.dac;
    if (ch.len == 0) ch.len = n == 2 ? 256 : 64;
    ch.timer = period(s, n);
    if (n == 2) { ch.wave_pos = 0; ch.wave_buf = s.wave[0]; return; }
    int nrx2 = n == 0 ? 0x02 : n == 1 ? 0x07 : 0x11;
    ch.vol = r[nrx2] >> 4; ch.env_dir = (r[nrx2] >> 3) & 1; ch.env_period = r[nrx2] & 7; ch.env_count = ch.env_period;
    if (n == 3) ch.lfsr = 0x7FFF;
    if (n == 0) {
        ch.sweep_freq = ch.freq;
        ch.sweep_period = (r[0x00] >> 4) & 7; ch.sweep_neg = (r[0x00] >> 3) & 1; ch.sweep_shift = r[0x00] & 7;
        ch.sweep_count = ch.sweep_period ? ch.sweep_period : 8;
        ch.sweep_on = ch.sweep_period || ch.sweep_shift;
        if (ch.sweep_shift) sweep_calc(s);
    }
}

uint8_t snd_read(GB& c, uint16_t a) {
    snd_sync(c);
    SND& s = c.s;
    int o = a - 0xFF10;
    if (o >= 0x20) {
        if (s.ch[2].on) return c.cgb ? s.wave[s.ch[2].wave_pos >> 1] : 0xFF;
        return s.wave[o - 0x20];
    }
    if (!s.on) return READ_MASK[o];
    if (o == 0x16)
        return (uint8_t)((s.regs[0x16] & 0xF0) | (s.ch[0].on ? 1 : 0) | (s.ch[1].on ? 2 : 0) | (s.ch[2].on ? 4 : 0) | (s.ch[3].on ? 8 : 0) | 0x70);
    return (uint8_t)(s.regs[o] | READ_MASK[o]);
}

void snd_write(GB& c, uint16_t a, uint8_t v) {
    snd_sync(c);
    SND& s = c.s;
    int o = a - 0xFF10;
    if (o >= 0x20) { s.wave[o - 0x20] = v; return; }
    if (o == 0x16) {
        bool was = s.on;
        s.on = (v & 0x80) != 0;
        if (was && !s.on) {
            std::memset(s.regs, 0, 0x16);
            for (int n = 0; n < 4; n++) { uint16_t len = s.ch[n].len; std::memset(&s.ch[n], 0, sizeof s.ch[n]); if (!c.cgb) s.ch[n].len = len; }
        }
        if (!was && s.on) { s.fs_step = 0; }
        s.regs[0x16] = (uint8_t)(v & 0x80);
        return;
    }
    if (!s.on) {
        // powered off: only the lengths can be written (the DMG)
        if (!c.cgb && (o == 0x01 || o == 0x06 || o == 0x0B || o == 0x10)) {
            int n = o == 0x01 ? 0 : o == 0x06 ? 1 : o == 0x0B ? 2 : 3;
            s.ch[n].len = (uint16_t)(n == 2 ? 256 - v : 64 - (v & 63));
        }
        return;
    }
    s.regs[o] = v;
    switch (o) {
    case 0x00: break;
    case 0x01: case 0x06: { int n = o == 1 ? 0 : 1; s.ch[n].duty = v >> 6; s.ch[n].len = (uint16_t)(64 - (v & 63)); break; }
    case 0x02: case 0x07: case 0x11: {
        int n = o == 0x02 ? 0 : o == 0x07 ? 1 : 3;
        s.ch[n].dac = (v & 0xF8) != 0; if (!s.ch[n].dac) s.ch[n].on = 0;
        break;
    }
    case 0x03: case 0x08: case 0x0D: { int n = o == 0x03 ? 0 : o == 0x08 ? 1 : 2; s.ch[n].freq = ch_freq(s, n); break; }
    case 0x04: case 0x09: case 0x0E: {
        int n = o == 0x04 ? 0 : o == 0x09 ? 1 : 2;
        s.ch[n].freq = ch_freq(s, n);
        s.ch[n].len_en = (v >> 6) & 1;
        if (v & 0x80) trigger(s, n);
        break;
    }
    case 0x0A: s.ch[2].dac = (v & 0x80) != 0; if (!s.ch[2].dac) s.ch[2].on = 0; break;
    case 0x0B: s.ch[2].len = (uint16_t)(256 - v); break;
    case 0x0C: break;
    case 0x10: s.ch[3].len = (uint16_t)(64 - (v & 63)); break;
    case 0x12: break;
    case 0x13: s.ch[3].len_en = (v >> 6) & 1; if (v & 0x80) trigger(s, 3); break;
    default: break;
    }
}

void snd_reset(GB& c) {
    SND& s = c.s;
    int16_t* out = s.out; uint32_t cap = s.out_cap;
    std::memset(&s, 0, sizeof s);
    s.out = out; s.out_cap = cap; s.out_n = 0;
    s.fs_at = FS_T;
    // as the boot ROM leaves it
    snd_write(c, 0xFF26, 0x80);
    snd_write(c, 0xFF24, 0x77);
    snd_write(c, 0xFF25, 0xF3);
    snd_write(c, 0xFF11, 0x80);
    snd_write(c, 0xFF12, 0xF3);
}

void gb_write_wav(const char* path, const int16_t* smp, size_t n, int rate) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    uint32_t bytes = (uint32_t)(n * 2), x;
    std::fwrite("RIFF", 1, 4, f); x = 36 + bytes; std::fwrite(&x, 4, 1, f);
    std::fwrite("WAVEfmt ", 1, 8, f); x = 16; std::fwrite(&x, 4, 1, f);
    uint16_t fmt[2] = { 1, 2 }; std::fwrite(fmt, 2, 2, f);
    x = (uint32_t)rate; std::fwrite(&x, 4, 1, f); x = (uint32_t)rate * 4; std::fwrite(&x, 4, 1, f);
    uint16_t al[2] = { 4, 16 }; std::fwrite(al, 2, 2, f);
    std::fwrite("data", 1, 4, f); std::fwrite(&bytes, 4, 1, f);
    std::fwrite(smp, 2, n, f);
    std::fclose(f);
}
