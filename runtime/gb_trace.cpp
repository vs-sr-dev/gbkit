// The tracer: every instruction the interpreter runs, every access it makes
// (the tracer maps no page directly), and what the PPU draws.  report.txt is
// read by recomp/discover.py and tracer/survey.py; cov.bin is a flag per ROM byte.
#include "gb_trace.h"
#include <algorithm>
#include <cstdio>

static const uint8_t LEN[256] = {
    1,3,1,1,1,1,2,1,3,1,1,1,1,1,2,1, 2,3,1,1,1,1,2,1,2,1,1,1,1,1,2,1,
    2,3,1,1,1,1,2,1,2,1,1,1,1,1,2,1, 2,3,1,1,1,1,2,1,2,1,1,1,1,1,2,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,3,3,3,1,2,1,1,1,3,2,3,3,2,1, 1,1,3,1,3,1,2,1,1,1,3,1,3,1,2,1,
    2,1,1,1,1,1,2,1,2,1,3,1,1,1,2,1, 2,1,1,1,1,1,2,1,2,1,3,1,1,1,2,1,
};

static inline int32_t rom_at(const GB& c, uint16_t a) {
    if (a >= 0x8000) return -1;
    uint32_t b = a < 0x4000 ? c.bank0 : c.bankx;
    uint32_t o = (b << 14) + (a & 0x3FFF);
    return o < c.romsize ? (int32_t)o : -1;
}

static std::string loc(const GB& c, uint16_t a) {
    char b[24];
    if (a < 0x4000) std::snprintf(b, sizeof b, "%03X:%04X", c.bank0, a);
    else if (a < 0x8000) std::snprintf(b, sizeof b, "%03X:%04X", c.bankx, a);
    else std::snprintf(b, sizeof b, "RAM:%04X", a);
    return b;
}

static inline void mark(GB& c, uint16_t a, uint8_t f) {
    int32_t o = rom_at(c, a);
    if (o >= 0) c.tr->cov[o] |= f;
}

// the byte at a without side effects (for the tracer's own looking)
static uint8_t raw(const GB& c, uint16_t a) {
    if (const uint8_t* p = c.rd_pg[a >> 12]) return p[a & 0xFFF];
    if (a >= 0xFF80 && a < 0xFFFF) return c.hram[a - 0xFF80];
    if (a >= 0x8000 && a < 0xA000) return c.vram[(c.cgb ? c.v.vbk * 0x2000 : 0) + (a - 0x8000)];
    if (a >= 0xF000 && a < 0xFE00) return c.rd_pg[13][a & 0xFFF];
    return 0;
}

// a RAM instruction's version (len << 24 | its bytes) -> its opcode alone (the CB's second byte with it)
static inline uint32_t ramop(uint32_t k) {
    return (k & 0xFF000000u) | ((k & 0xFF) == 0xCB ? (k & 0xFFFF) : (k & 0xFF));
}

void gb_trace_insn(GB& c, uint16_t at) {
    GBTrace* t = c.tr;
    uint8_t op = raw(c, at);
    int len = LEN[op];
    t->cur_at = at; t->cur_len = (uint8_t)len; t->cur_op = op;
    t->insn_n++;
    t->ops[op == 0xCB ? 256 + raw(c, (uint16_t)(at + 1)) : op]++;
    if (at < 0x8000) {
        int w = at >> 14;
        t->where[(uint64_t)w << 32 | (w ? c.bankx : c.bank0)]++;
        mark(c, at, R_OP);
        for (int i = 1; i < len; i++) mark(c, (uint16_t)(at + i), R_ARG);
    } else {
        t->ramexec_n++;
        if (at >= 0xFF80) t->ramexec_hram++; else if (at >= 0xC000) t->ramexec_wram++; else t->ramexec_other++;
        for (int i = 0; i < len; i++) if ((uint16_t)(at + i) >= 0x8000) t->ramexec[(uint16_t)(at + i) - 0x8000] = 1;
        uint32_t k = (uint32_t)len << 24;
        for (int i = 0; i < len; i++) k |= (uint32_t)raw(c, (uint16_t)(at + i)) << (8 * i);
        const uint32_t MANY = GBTrace::RAMVER_MANY, ok = ramop(k);
        std::vector<uint32_t>& vs = t->ramver[at];
        if (!vs.empty() && vs[0] == MANY) { if (vs[1] != ok) vs[1] = MANY; return; }
        if (std::find(vs.begin(), vs.end(), k) == vs.end()) {
            if (vs.size() >= GBTrace::RAMVER_MAX) {       // too many: whether the opcode stays the same
                uint32_t o = ok;
                for (uint32_t x : vs) if (ramop(x) != ok) o = MANY;
                vs.assign({ MANY, o });
                return;
            }
            vs.push_back(k);
        }
    }
}

static void edge(GB& c, char k, uint16_t from, uint16_t to) {
    c.tr->edges[std::string(1, k) + " " + loc(c, from) + " -> " + loc(c, to)]++;
}

// a transfer is recorded as an edge when the target's bank is not the source's to know:
// across the windows ($0000 / $4000 / RAM), or computed
void gb_trace_jump(GB& c, char k, uint16_t from, uint16_t to) {
    mark(c, to, R_JTGT);
    int wf = from < 0x4000 ? 0 : from < 0x8000 ? 1 : 2, wt = to < 0x4000 ? 0 : to < 0x8000 ? 1 : 2;
    if (k == 'i' || wf != wt || wt == 2) edge(c, k, from, to);
}

void gb_trace_call(GB& c, uint16_t from, uint16_t to, uint16_t ret) {
    GBTrace* t = c.tr;
    mark(c, to, R_JTGT);
    mark(c, ret, R_RET);
    uint8_t kind = (to & ~0x38) == 0 && c.tr->cur_op >= 0xC7 && (c.tr->cur_op & 7) == 7 ? 1 : 0;
    if (t->slots.count(c.SP)) t->dropped++;
    t->slots[c.SP] = { ret, kind, ret < 0x4000 ? c.bank0 : c.bankx };
    int wf = from < 0x4000 ? 0 : from < 0x8000 ? 1 : 2, wt = to < 0x4000 ? 0 : to < 0x8000 ? 1 : 2;
    if (wf != wt || wt == 2) edge(c, kind ? 's' : 'c', from, to);
}

void gb_trace_ret(GB& c, uint16_t at, uint16_t sp, uint16_t to) {
    GBTrace* t = c.tr;
    mark(c, to, R_JTGT);
    // p: a return nobody's call pushed (push + ret); r: a call's, elsewhere; a: an RST's, elsewhere
    // (its arguments inline after it)
    // (r and a are recorded from the address the call pushed: where its caller goes on instead)
    auto it = t->slots.find(sp);
    if (it == t->slots.end()) { edge(c, 'p', at, to); t->comp_ret++; }
    else {
        if (it->second.ret != to) {
            char b[24];
            uint16_t r = it->second.ret;
            if (r < 0x8000) std::snprintf(b, sizeof b, "%03X:%04X", it->second.bank, r);
            else std::snprintf(b, sizeof b, "RAM:%04X", r);
            c.tr->edges[std::string(1, it->second.kind == 1 ? 'a' : 'r') + " " + b + " -> " + loc(c, to)]++;
            t->comp_ret++;
        }
        if (it->second.kind == 2 && t->irq_depth > 0) t->irq_depth--;
        t->slots.erase(it);
    }
}

void gb_trace_irq(GB& c, int n, uint16_t at) {
    GBTrace* t = c.tr;
    t->irqs[n]++;
    if (n == 1) t->stat_f++;
    if (t->irq_depth == 0) t->irq_at[(uint32_t)(at < 0x4000 ? c.bank0 : at < 0x8000 ? c.bankx : 0xFFF) << 16 | at]++;
    if (++t->irq_depth > t->irq_depth_max) t->irq_depth_max = t->irq_depth;
    if (t->slots.count(c.SP)) t->dropped++;
    t->slots[c.SP] = { at, 2, at < 0x4000 ? c.bank0 : c.bankx };
    mark(c, (uint16_t)(0x40 + 8 * n), R_JTGT);
}

void gb_trace_bank(GB& c, uint32_t old0, uint32_t oldx) {
    GBTrace* t = c.tr;
    t->bank_w++;
    if (c.bankx != oldx || c.bank0 != old0) {
        t->bank_change++;
        if ((t->cur_at >= 0x4000 && t->cur_at < 0x8000 && c.bankx != oldx) || (t->cur_at < 0x4000 && c.bank0 != old0))
            t->bank_own++;
    }
}

static inline bool fixed_addr(uint8_t op) { return op == 0xE0 || op == 0xEA || op == 0xF0 || op == 0xFA || op == 0x08; }

// the class of an address, for the sites (0: plain RAM, not recorded)
static inline uint8_t aclass(uint16_t a) {
    if (a < 0x8000) return A_MBC;
    if (a < 0xA000) return A_VRAM;
    if (a < 0xC000) return A_SRAM;
    if (a < 0xE000) return 0;
    if (a < 0xFE00) return A_ECHO;
    if (a < 0xFEA0) return A_OAM;
    if (a < 0xFF00) return 0;
    if (a < 0xFF80) return A_IO;
    return a == 0xFFFF ? A_IE : 0;
}

static void site(GB& c, uint16_t a, bool w) {
    uint8_t k = aclass(a);
    if (!k) return;
    GBTrace* t = c.tr;
    uint16_t at = t->cur_at;
    uint32_t b = at < 0x4000 ? c.bank0 : at < 0x8000 ? c.bankx : 0xFFF;
    GBSite& s = t->sites[b << 16 | at];
    s.n++;
    if (w) { s.w |= k; if (k == A_IO) s.io_w[(a >> 6) & 1] |= 1ull << (a & 63); }
    else { s.r |= k; if (k == A_IO) s.io_r[(a >> 6) & 1] |= 1ull << (a & 63); }
}

void gb_trace_read(GB& c, uint16_t a) {
    GBTrace* t = c.tr;
    if ((uint16_t)(a - t->cur_at) < t->cur_len) return;      // the instruction's own bytes
    if (a >= 0x8000) site(c, a, false);
    if (a < 0x8000) { mark(c, a, R_READ); return; }
    if (a >= 0xE000 && a < 0xFE00) { t->echo_n++; return; }
    if (a >= 0xFF00) { t->rd_n[a & 0xFF]++; if (!fixed_addr(t->cur_op)) t->io_r_ptr++; }
}

void gb_trace_write(GB& c, uint16_t a, uint8_t v) {
    GBTrace* t = c.tr;
    (void)v;
    site(c, a, true);
    if (a < 0x8000) { if (a < 0x2000) t->ram_enable++; return; }
    if (a >= 0x8000 && t->ramexec[a - 0x8000]) t->smc_n++;
    if (a < 0xA000) {
        if (fixed_addr(t->cur_op)) t->vram_w_fixed++; else t->vram_w_ptr++;
        if (a < 0x9800) { int tile = (a - 0x8000) >> 4, b = c.cgb ? (c.v.vbk & 1) : 0; t->tiles[b][tile >> 3] |= (uint8_t)(1 << (tile & 7)); }
        return;
    }
    if (a >= 0xE000 && a < 0xFE00) { t->echo_n++; return; }
    if (a >= 0xFE00 && a < 0xFEA0) { t->oam_w_cpu++; return; }
    if (a >= 0xFF00) {
        uint8_t r = a & 0xFF;
        t->wr_n[r]++;
        if (a < 0xFF80) { if (fixed_addr(t->cur_op)) t->io_w_fixed++; else t->io_w_ptr++; }
        static const char* names[0x30] = { "LCDC", 0, "SCY", "SCX", 0, 0, 0, "BGP", "OBP0", "OBP1", "WY", "WX" };
        const char* nm = r >= 0x40 && r < 0x4C ? names[r - 0x40] : r >= 0x68 && r <= 0x6B ? "CGBPAL" : nullptr;
        if (nm) {
            ppu_sync(c);
            if ((c.v.lcdc & 0x80) && c.v.ly < 144 && c.v.mode != 1) { t->raster_w++; t->raster_regs[nm]++; }
        }
        if (r == 0x46) t->oam_dma++;
    }
}

void gb_trace_frame(GB& c) {
    GBTrace* t = c.tr;
    if (c.lcd_shown) t->frames_drawn++;
    t->frames_window += t->win_f; t->frames_tall_sprites += t->tall_f;
    t->stat_irq_frame += t->stat_f; t->stat_irq_max = std::max<uint64_t>(t->stat_irq_max, t->stat_f);
    t->win_f = t->tall_f = 0; t->stat_f = 0;
}

void gb_trace_begin(GB& c) {
    c.tr = new GBTrace();
    c.tr->cov.assign(c.romsize, 0);
    c.natives = false;
    gb_map(c);
}

void gb_trace_report(GB& c, const char* dir, const char* rom_path) {
    GBTrace* t = c.tr;
    std::string d = dir;
    FILE* f = std::fopen((d + "/cov.bin").c_str(), "wb");
    if (f) { std::fwrite(t->cov.data(), 1, t->cov.size(), f); std::fclose(f); }
    f = std::fopen((d + "/report.txt").c_str(), "w");
    if (!f) return;
    auto U = [](uint64_t x) { return (unsigned long long)x; };
    std::fprintf(f, "rom %s\nsize %u cgb %d mbc %d ram %u battery %d rtc %d frames %llu jammed %d\n", rom_path, c.romsize,
                 c.cgb, c.mbc, c.sram_size, c.battery, c.has_rtc, U(c.frame), c.jammed);
    uint64_t und = 0; for (int i = 0; i < 256; i++) und += t->undefined[i];
    std::fprintf(f, "insns %llu irq %llu halt %llu halt_bug %llu stop %llu speed_switch %llu undefined %llu irq_depth_max %d\n",
                 U(t->insn_n), U(c.n_irq), U(t->halts), U(t->halt_bugs), U(t->stops), U(t->speed_switches), U(und), t->irq_depth_max);
    std::fprintf(f, "irqs");
    for (int i = 0; i < 5; i++) std::fprintf(f, " %d:%llu", i, U(t->irqs[i]));
    std::fprintf(f, "\nbank_w %llu bank_change %llu bank_own %llu ram_enable %llu\n", U(t->bank_w), U(t->bank_change),
                 U(t->bank_own), U(t->ram_enable));
    for (auto& kv : t->where)
        std::fprintf(f, "where win %u bank %03X insns %llu\n", (unsigned)(kv.first >> 32), (unsigned)(kv.first & 0xFFFFFFFF), U(kv.second));
    uint32_t nb = (c.romsize + 0x3FFF) >> 14;
    for (uint32_t b = 0; b < nb; b++) {
        uint32_t op = 0, arg = 0, rdn = 0, both = 0;
        for (uint32_t i = b << 14; i < ((b + 1) << 14) && i < c.romsize; i++) {
            uint8_t x = t->cov[i];
            if (x & R_OP) op++;
            if (x & (R_OP | R_ARG)) arg++;
            if (x & R_READ) rdn++;
            if ((x & (R_OP | R_ARG)) && (x & R_READ)) both++;
        }
        if (op || rdn) std::fprintf(f, "bank %03X code_bytes %u insn_starts %u data_read %u overlap %u\n", b, arg, op, rdn, both);
    }
    int ramcode = 0, hramcode = 0;
    for (int i = 0; i < 0x8000; i++) { ramcode += t->ramexec[i]; if (i >= 0x7F80) hramcode += t->ramexec[i]; }
    std::fprintf(f, "ramexec_bytes %d hram_bytes %d ramexec_insns %llu hram_insns %llu wram_insns %llu other_insns %llu smc_writes %llu\n",
                 ramcode, hramcode, U(t->ramexec_n), U(t->ramexec_hram), U(t->ramexec_wram), U(t->ramexec_other), U(t->smc_n));
    std::fprintf(f, "ramexec");
    for (int i = 0; i < 0x8000; i++)
        if (t->ramexec[i] && (i == 0 || !t->ramexec[i - 1])) {
            int j = i; while (j < 0x8000 && t->ramexec[j]) j++;
            std::fprintf(f, " %04X-%04X", 0x8000 + i, 0x8000 + j - 1);
        }
    std::fprintf(f, "\n");
    // the bytes of each instruction run from RAM, every version seen ('*': too many; '*OP': too many, all with
    // that opcode -- only the operands rewritten)
    std::vector<uint16_t> ras;
    for (auto& kv : t->ramver) ras.push_back(kv.first);
    std::sort(ras.begin(), ras.end());
    for (uint16_t a : ras) {
        std::fprintf(f, "ramcode %04X", a);
        const std::vector<uint32_t>& vs = t->ramver[a];
        if (vs[0] == GBTrace::RAMVER_MANY) {
            std::fprintf(f, " *");
            if (vs[1] != GBTrace::RAMVER_MANY)
                for (int i = 0; i < ((vs[1] & 0xFF) == 0xCB ? 2 : 1); i++) std::fprintf(f, "%02X", (vs[1] >> (8 * i)) & 0xFF);
        } else
            for (uint32_t k : vs) {
                std::fprintf(f, " ");
                for (uint32_t i = 0; i < (k >> 24); i++) std::fprintf(f, "%02X", (k >> (8 * i)) & 0xFF);
            }
        std::fprintf(f, "\n");
    }
    // the sites: each instruction that read or wrote VRAM, OAM, I/O, the mapper, the cartridge's RAM, the echo
    // RAM or IE -- the classes (A_*) and the I/O registers ($FF00 + n) it reached
    std::vector<uint32_t> sk;
    for (auto& kv : t->sites) sk.push_back(kv.first);
    std::sort(sk.begin(), sk.end());
    auto ios = [&](const uint64_t* m) {
        bool any = false;
        for (int i = 0; i < 128; i++)
            if (m[i >> 6] >> (i & 63) & 1) { std::fprintf(f, "%s%02X", any ? "," : "", i); any = true; }
        if (!any) std::fprintf(f, "-");
    };
    for (uint32_t k : sk) {
        const GBSite& s = t->sites[k];
        if ((k >> 16) == 0xFFF) std::fprintf(f, "site RAM:%04X", k & 0xFFFF);
        else std::fprintf(f, "site %03X:%04X", k >> 16, k & 0xFFFF);
        std::fprintf(f, " w %02X r %02X io_w ", s.w, s.r);
        ios(s.io_w);
        std::fprintf(f, " io_r ");
        ios(s.io_r);
        std::fprintf(f, " n %llu\n", U(s.n));
    }
    std::fprintf(f, "computed_ret %llu dropped_returns %llu\n", U(t->comp_ret), U(t->dropped));
    std::fprintf(f, "rd");
    for (int i = 0; i < 256; i++) if (t->rd_n[i]) std::fprintf(f, " %02X:%llu", i, U(t->rd_n[i]));
    std::fprintf(f, "\nwr");
    for (int i = 0; i < 256; i++) if (t->wr_n[i]) std::fprintf(f, " %02X:%llu", i, U(t->wr_n[i]));
    int nops = 0; for (int i = 0; i < 512; i++) nops += t->ops[i] != 0;
    std::fprintf(f, "\nops_used %d\nundefined_ops", nops);
    for (int i = 0; i < 256; i++) if (t->undefined[i]) std::fprintf(f, " %02X:%llu", i, U(t->undefined[i]));
    std::fprintf(f, "\nirq_hits");
    std::vector<std::pair<uint64_t, uint32_t>> hits;
    for (auto& kv : t->irq_at) hits.push_back({ kv.second, kv.first });
    std::sort(hits.rbegin(), hits.rend());
    for (size_t i = 0; i < hits.size() && i < 8; i++)
        std::fprintf(f, " %03X:%04X:%llu", hits[i].second >> 16, hits[i].second & 0xFFFF, U(hits[i].first));
    std::fprintf(f, " (%zu places)\n", hits.size());
    // the survey's columns (and Pocketshift's)
    int tiles0 = 0, tiles1 = 0;
    for (int i = 0; i < 48; i++) for (int b = 0; b < 8; b++) { tiles0 += (t->tiles[0][i] >> b) & 1; tiles1 += (t->tiles[1][i] >> b) & 1; }
    int bp = 0, opal = 0; for (int i = 0; i < 8; i++) { bp += (t->cgb_bpal_used >> i) & 1; opal += (t->cgb_opal_used >> i) & 1; }
    std::fprintf(f, "survey frames_drawn %llu double_speed %d hdma_general %llu hdma_hblank %llu cgb_bpals %d cgb_opals %d "
                    "tiles_bank0 %d tiles_bank1 %d frames_window %llu raster_writes %llu stat_irq_per_frame %.2f stat_irq_max %llu "
                    "max_sprites_line %u lines_over8 %llu lines_over10 %llu frames_8x16 %llu oam_dma %llu oam_cpu_writes %llu "
                    "vram_w_fixed %llu vram_w_ptr %llu io_w_fixed %llu io_w_ptr %llu io_r_ptr %llu echo %llu\n",
                 U(t->frames_drawn), t->speed_switches ? 1 : 0, U(t->hdma_general), U(t->hdma_hblank), bp, opal, tiles0, tiles1,
                 U(t->frames_window), U(t->raster_w), c.frame ? (double)t->stat_irq_frame / c.frame : 0.0, U(t->stat_irq_max),
                 t->max_sprites_line, U(t->lines_over8), U(t->lines_over10), U(t->frames_tall_sprites), U(t->oam_dma),
                 U(t->oam_w_cpu), U(t->vram_w_fixed), U(t->vram_w_ptr), U(t->io_w_fixed), U(t->io_w_ptr), U(t->io_r_ptr),
                 U(t->echo_n));
    std::fprintf(f, "raster");
    for (auto& kv : t->raster_regs) std::fprintf(f, " %s:%llu", kv.first.c_str(), U(kv.second));
    std::fprintf(f, "\n");
    for (auto& kv : t->edges) std::fprintf(f, "edge %s x%llu\n", kv.first.c_str(), U(kv.second));
    std::fclose(f);
}
