// The PPU: the LCD's modes and their timing (after MAME's gb_lcd, its first engine),
// STAT and its interrupt, LY and LYC, the window, ten sprites a line, the OAM DMA,
// and the CGB's VRAM bank, palettes and HDMA.  A line is drawn whole as mode 3
// begins, with the registers as they are then.
//
// A visible line (dots): LY changes; 4 later mode 2 (80); mode 3 (169 + the line's
// penalty: SCX's fine scroll, the sprites as MAME counts them, the window); mode 0
// for the rest of the 456.  Line 144 raises VBlank 4 dots after LY changes; LY
// reads 0 from 4 dots into line 153 on; line 0 begins in mode 0 for 4 dots.
#include "gb.h"
#include "gb_trace.h"
#include <cstdio>

enum { P_M2, P_M3, P_M0, P_INC, P_VBL, P_VBL_INC, P_L153, P_L153_1, P_L153_2, P_L0_M0, P_ON, P_OFF };

static const uint16_t DMG_SHADE[4] = { 0x7FFF, 0x5294, 0x294A, 0x0000 };


static inline void at_after(GB& c, int dots) { c.v.at += (uint64_t)dots * DOT_T; }

// ---- the STAT interrupt: a line, the OR of its sources; an interrupt as it rises ----

static void stat_update(GB& c, bool force = false) {
    PPU& v = c.v;
    uint8_t line = 0;
    if (v.lcdc & 0x80) {
        if (v.mode == 0 && (v.stat & 0x08) && v.st != P_L0_M0 && v.st != P_ON) line = 1;
        if (v.mode == 1 && (v.stat & 0x10)) line = 1;
        if (c.v.src_m2 && (v.stat & 0x20)) line = 1;
        if (c.v.src_lyc && (v.stat & 0x40)) line = 1;
    }
    if (line && (!v.stat_line || force)) gb_irq(c, I_STAT);
    v.stat_line = line;
}

static void lyc_check(GB& c) {
    PPU& v = c.v;
    if (v.ly == v.lyc) { v.stat |= 4; c.v.src_lyc = 1; } else { v.stat &= ~4; c.v.src_lyc = 0; }
}

// ---- sprites: the first ten on the line in OAM order, and MAME's count of their cycles ----
static void select_sprites(GB& c) {
    PPU& v = c.v;
    c.v.nspr = 0;
    v.m3_extra = v.scx & 7;
    if (!(v.lcdc & 2)) return;
    int h = (v.lcdc & 4) ? 16 : 8, line = v.ly + 16, total = 0, cyc = 0;
    uint8_t occurs[32] = { 0 };
    bool dma = c.v.dma_end_at && c.t < c.v.dma_end_at;
    for (int i = 0; i < 40; i++) {
        const uint8_t* o = c.oam + i * 4;
        if (dma) break;                                  // OAM is the DMA's
        if (line >= o[0] && line < o[0] + h) {
            total++;
            if (c.v.nspr < 10) {
                { uint8_t* q = c.v.spr[c.v.nspr++]; q[0] = o[0]; q[1] = o[1]; q[2] = o[2]; q[3] = o[3]; q[4] = (uint8_t)i; }
                if (o[1] < 168) {
                    int x = o[1] ? o[1] + (v.scx & 7) : 0;
                    if (occurs[x >> 3]) cyc += 3;
                    cyc += 3;
                    occurs[x >> 3] |= (uint8_t)(1 << (x & 7));
                }
            }
        }
    }
    static const uint8_t extra[32] = { 3, 8, 7, 8, 6, 8, 7, 8, 5, 8, 7, 8, 6, 8, 7, 8, 4, 8, 7, 8, 6, 8, 7, 8, 5, 8, 7, 8, 6, 8, 7, 8 };
    if (c.v.nspr) for (int i = 0; i < 22; i++) if (occurs[i]) cyc += extra[occurs[i] & 0x1F];
    v.m3_extra = (uint16_t)(v.m3_extra + cyc);
    if (c.tr) {
        if ((uint32_t)total > c.tr->max_sprites_line) c.tr->max_sprites_line = (uint32_t)total;
        if (total > 8) c.tr->lines_over8++;
        if (total > 10) c.tr->lines_over10++;
        if (total && h == 16) c.tr->tall_f = 1;
    }
}

// ---- drawing a line ----
static void draw_line(GB& c) {
    PPU& v = c.v;
    int y = v.ly;
    if (y >= VIS_H) return;
    uint16_t* out = c.fb[y];
    uint8_t bgc[VIS_W];       // the background's colour index (0-3) per pixel
    uint8_t bgp[VIS_W];       // the CGB: the attribute's priority bit
    const uint8_t* vr = c.vram;
    bool cgb = c.cgb;
    bool bg_on = cgb || (v.lcdc & 1);
    // the window: WY matched on some line of this frame, WX in range
    bool win = (v.lcdc & 0x20) && v.win_y_hit && v.wx < 167;
    int wx0 = v.wx - 7;
    for (int x = 0; x < VIS_W; x++) {
        bool inwin = win && x >= wx0;
        int px, py, map;
        if (inwin) { px = x - wx0; py = v.win_line; map = (v.lcdc & 0x40) ? 0x1C00 : 0x1800; }
        else { px = (x + v.scx) & 255; py = (y + v.scy) & 255; map = (v.lcdc & 8) ? 0x1C00 : 0x1800; }
        if (!bg_on) { bgc[x] = 0; bgp[x] = 0; out[x] = DMG_SHADE[0]; continue; }
        int mo = map + (py >> 3) * 32 + (px >> 3);
        uint8_t tile = vr[mo];
        uint8_t attr = cgb ? vr[0x2000 + mo] : 0;
        int row = py & 7, col = px & 7;
        if (attr & 0x40) row = 7 - row;
        if (attr & 0x20) col = 7 - col;
        int to = (v.lcdc & 0x10) ? tile * 16 : 0x1000 + (int8_t)tile * 16;
        if (attr & 0x08) to += 0x2000;
        uint8_t lo = vr[to + row * 2], hi = vr[to + row * 2 + 1];
        int ci = ((lo >> (7 - col)) & 1) | ((hi >> (7 - col)) & 1) << 1;
        bgc[x] = (uint8_t)ci; bgp[x] = attr & 0x80;
        if (cgb) {
            out[x] = v.bpal[(attr & 7) * 4 + ci];
            if (c.tr) c.tr->cgb_bpal_used |= 1u << (attr & 7);
        } else out[x] = DMG_SHADE[(v.bgp >> (ci * 2)) & 3];
    }
    if (win && wx0 < VIS_W) { v.win_line++; if (c.tr) c.tr->win_f = 1; }
    // sprites: the DMG's priority is the lower X (then OAM order), the CGB's OAM order;
    // drawn from the lowest priority up
    if (!(v.lcdc & 2) || !c.v.nspr) return;
    int order[10], n = c.v.nspr;
    for (int i = 0; i < n; i++) order[i] = i;
    if (!cgb)
        for (int i = 1; i < n; i++) {           // a stable sort by X
            int k = order[i], j = i - 1;
            while (j >= 0 && v.spr[order[j]][1] > v.spr[k][1]) { order[j + 1] = order[j]; j--; }
            order[j + 1] = k;
        }
    int h = (v.lcdc & 4) ? 16 : 8;
    for (int oi = n - 1; oi >= 0; oi--) {
        const uint8_t* sp = v.spr[order[oi]];
        uint8_t attr = sp[3];
        int sx = sp[1] - 8, row = y + 16 - sp[0];
        if (attr & 0x40) row = h - 1 - row;
        uint8_t tile = h == 16 ? (sp[2] & 0xFE) : sp[2];
        int to = tile * 16 + row * 2 + ((cgb && (attr & 8)) ? 0x2000 : 0);
        uint8_t lo = vr[to], hi = vr[to + 1];
        for (int k = 0; k < 8; k++) {
            int x = sx + k;
            if (x < 0 || x >= VIS_W) continue;
            int b = (attr & 0x20) ? k : 7 - k;
            int ci = ((lo >> b) & 1) | ((hi >> b) & 1) << 1;
            if (!ci) continue;
            if (cgb) {
                if ((v.lcdc & 1) && bgc[x] && ((attr & 0x80) || bgp[x])) continue;
                out[x] = v.opal[(attr & 7) * 4 + ci];
                if (c.tr) c.tr->cgb_opal_used |= 1u << (attr & 7);
            } else {
                if ((attr & 0x80) && bgc[x]) continue;
                uint8_t pal = (attr & 0x10) ? v.obp1 : v.obp0;
                out[x] = DMG_SHADE[(pal >> (ci * 2)) & 3];
            }
        }
    }
}

static void blank_line(GB& c, int y) {
    if (y < 0 || y >= VIS_H) return;
    for (int x = 0; x < VIS_W; x++) c.fb[y][x] = 0x7FFF;
}

// ---- the DMAs ----
static uint8_t dma_src(GB& c, uint16_t a) {
    if (a < 0x8000) return c.rd_pg[a >> 12][a & 0xFFF];
    if (a < 0xA000) return c.vram[(c.cgb ? c.v.vbk * 0x2000 : 0) + (a - 0x8000)];
    if (a < 0xC000) return c.rd_pg[a >> 12] ? c.rd_pg[a >> 12][a & 0xFFF] : 0xFF;
    if (a < 0xE000) return c.rd_pg[a >> 12][a & 0xFFF];
    return c.rd_pg[(a - 0x2000) >> 12] ? c.rd_pg[(a - 0x2000) >> 12][a & 0xFFF] : 0xFF;
}

static void hdma_block(GB& c, int len) {
    PPU& v = c.v;
    uint16_t src = (uint16_t)(v.hdma1 << 8 | (v.hdma2 & 0xF0));
    uint16_t dst = (uint16_t)(0x8000 | (v.hdma3 & 0x1F) << 8 | (v.hdma4 & 0xF0));
    for (int i = 0; i < len; i++) {
        uint16_t sh = src & 0xF000;
        uint8_t b = (sh < 0x8000 || (sh >= 0xA000 && sh < 0xE000)) ? dma_src(c, src) : 0xFF;
        if (dst & 0x8000) c.vram[v.vbk * 0x2000 + (dst & 0x1FFF)] = b;
        src++; dst++;
    }
    v.hdma1 = (uint8_t)(src >> 8); v.hdma2 = (uint8_t)src;
    v.hdma3 = (uint8_t)(dst >> 8); v.hdma4 = (uint8_t)dst;
    c.burn += 4 + len * 2;
    c.next = 0;
}

// ---- the state machine ----
static void ppu_step(GB& c) {
    PPU& v = c.v;
    switch (v.st) {
    case P_M2:
        v.mode = 2; v.oam_lock = 1;
        if (v.ly == v.lyc) v.stat |= 4;
        c.v.src_lyc = v.ly == v.lyc;
        stat_update(c);
        select_sprites(c);
        if ((v.lcdc & 0x20) && v.ly == v.wy) v.win_y_hit = 1;
        v.st = P_M3; at_after(c, 80);
        break;
    case P_ON:                                    // the first line after the LCD is switched on
    case P_M3:
        if (v.st == P_ON) { c.v.nspr = 0; v.m3_extra = v.scx & 7; }
        c.v.src_m2 = 0;
        v.mode = 3; v.oam_lock = v.vram_lock = 1; if (c.cgb) v.pal_lock = 1;
        stat_update(c);
        if ((v.lcdc & 0x20) && v.wx < 167 && v.wy < 144) v.m3_extra = (uint16_t)(v.m3_extra + (v.wx == 0x0F ? 12 : 4));
        draw_line(c);
        v.st = P_M0; at_after(c, 169 + v.m3_extra);
        break;
    case P_M0:
        v.mode = 0; v.oam_lock = v.vram_lock = v.pal_lock = 0;
        stat_update(c);
        if (c.cgb && v.ly < 144) {
            if (v.hdma_on) {
                hdma_block(c, 16);
                if (c.tr) c.tr->hdma_hblank++;
                v.hdma5--;
                if ((v.hdma5 & 0x7F) == 0x7F) { v.hdma5 = 0xFF; v.hdma_on = 0; }
            } else v.hdma_possible = 1;
        }
        v.st = P_INC; at_after(c, 203 - v.m3_extra);
        break;
    case P_INC:
        v.hdma_possible = 0;
        v.ly++; v.line = v.ly;
        c.v.src_lyc = 0; v.stat &= ~4;
        c.v.src_m2 = 1;                             // MAME: the mode-2 source rises here (line 144 too)
        stat_update(c);
        if (v.ly == 144) { v.st = P_VBL; at_after(c, 4); }
        else { v.st = P_M2; at_after(c, 4); }
        break;
    case P_VBL:
        c.v.src_m2 = 0;
        if (v.ly == 144) {
            gb_irq(c, I_VBL);
            v.mode = 1;
            c.v.frame_due = true;
            c.lcd_shown = !v.blank_frame;
            if (v.blank_frame) { for (int y = 0; y < VIS_H; y++) blank_line(c, y); v.blank_frame = 0; }
        }
        lyc_check(c);
        stat_update(c);
        v.st = P_VBL_INC; at_after(c, 452);
        break;
    case P_VBL_INC:
        v.ly++; v.line = v.ly; v.stat &= ~4; c.v.src_lyc = 0;
        stat_update(c);
        v.st = v.ly == 153 ? P_L153 : P_VBL; at_after(c, 4);
        break;
    case P_L153:
        lyc_check(c); stat_update(c);
        v.ly = 0;                                 // LY reads 0 from here on
        v.st = P_L153_1; at_after(c, 4);
        break;
    case P_L153_1:
        v.stat &= ~4;
        v.st = P_L153_2; at_after(c, 4);
        break;
    case P_L153_2:
        lyc_check(c); stat_update(c);
        v.st = P_L0_M0; at_after(c, 444);
        break;
    case P_L0_M0:
        v.mode = 0; v.line = 0; v.win_y_hit = 0; v.win_line = 0;
        stat_update(c);
        v.st = P_M2; at_after(c, 4);
        break;
    case P_OFF:                                   // the LCD off: lines go on (blank), and frames
        v.line = (uint8_t)((v.line + 1) % 154);
        if (v.line < 144) blank_line(c, v.line);
        if (v.line == 144) { c.v.frame_due = true; c.lcd_shown = false; }
        at_after(c, LINE_DOTS);
        break;
    }
}

void ppu_sync(GB& c) {
    if (c.v.in_sync) return;
    c.v.in_sync = true;
    PPU& v = c.v;
    for (;;) {
        uint64_t t = v.at; int k = 0;
        if (v.dma_copy_at && v.dma_copy_at <= t) { t = v.dma_copy_at; k = 1; }
        if (v.dma_end_at && v.dma_end_at < t) { t = v.dma_end_at; k = 2; }
        if (c.t < t) break;
        if (k == 1) {
            uint16_t s = (uint16_t)(v.dma_reg << 8);
            for (int i = 0; i < 0xA0; i++) c.oam[i] = dma_src(c, (uint16_t)(s + i));
            v.dma_copy_at = 0;
            v.dma_end_at = t + 640 * DOT_T;
            continue;
        }
        if (k == 2) { v.dma_end_at = 0; continue; }
        ppu_step(c);
    }
    c.v.in_sync = false;
}

uint64_t ppu_next(const GB& c) {
    uint64_t t = c.v.at;
    if (c.v.dma_copy_at && c.v.dma_copy_at < t) t = c.v.dma_copy_at;
    if (c.v.dma_end_at && c.v.dma_end_at < t) t = c.v.dma_end_at;
    return t;
}

bool ppu_frame_due(GB& c) {
        bool f = c.v.frame_due; c.v.frame_due = 0; return f;
}

// ---- the CPU's side ----
uint8_t vram_read(GB& c, uint16_t a) {
    ppu_sync(c);
    if (c.v.vram_lock) return 0xFF;
    return c.vram[(c.cgb ? c.v.vbk * 0x2000 : 0) + (a & 0x1FFF)];
}

void vram_write(GB& c, uint16_t a, uint8_t x) {
    ppu_sync(c);
    if (c.v.vram_lock) return;
    c.vram[(c.cgb ? c.v.vbk * 0x2000 : 0) + (a & 0x1FFF)] = x;
}

static inline bool oam_dma_busy(const GB& c) { return c.v.dma_end_at && c.t < c.v.dma_end_at; }

uint8_t oam_read(GB& c, uint16_t a) {
    ppu_sync(c);
    if (c.v.oam_lock || oam_dma_busy(c)) return 0xFF;
    uint8_t o = a & 0xFF;
    return o < 0xA0 ? c.oam[o] : 0x00;
}

void oam_write(GB& c, uint16_t a, uint8_t x) {
    ppu_sync(c);
    uint8_t o = a & 0xFF;
    if (c.v.oam_lock || o >= 0xA0 || oam_dma_busy(c)) return;
    c.oam[o] = x;
}

static void lcd_on(GB& c) {
    PPU& v = c.v;
    v.line = 0; v.ly = 0; v.win_line = 0; v.win_y_hit = 0;
    v.oam_lock = v.vram_lock = v.pal_lock = 0;
    c.v.src_m2 = 0; c.v.src_lyc = 0; v.stat_line = 0;
    v.hdma_possible = 0;
    if (v.ly == v.lyc) v.stat |= 4; else v.stat &= ~4;
    if ((v.lcdc & 0x20) && v.wy == 0) v.win_y_hit = 1;
    v.mode = 0;
    v.st = P_ON; v.at = c.t + 80 * DOT_T;
    v.blank_frame = 1;
}

static void lcd_off(GB& c) {
    PPU& v = c.v;
    v.stat &= ~3; v.mode = 0; v.ly = 0;
    v.oam_lock = v.vram_lock = v.pal_lock = 0;
    c.v.src_m2 = 0; c.v.src_lyc = 0; v.stat_line = 0;
    if (v.line < 144) blank_line(c, v.line);
    v.st = P_OFF; v.at = c.t + LINE_DOTS * DOT_T;
}

uint8_t ppu_read(GB& c, uint16_t a) {
    ppu_sync(c);
    PPU& v = c.v;
    uint8_t o = a & 0xFF;
    switch (o) {
    case 0x40: return v.lcdc;
    case 0x41: return (uint8_t)(0x80 | (v.stat & 0x7C) | ((v.lcdc & 0x80) ? v.mode : 0));
    case 0x42: return v.scy;
    case 0x43: return v.scx;
    case 0x44: return v.ly;
    case 0x45: return v.lyc;
    case 0x46: return v.dma_reg;
    case 0x47: return v.bgp;
    case 0x48: return v.obp0;
    case 0x49: return v.obp1;
    case 0x4A: return v.wy;
    case 0x4B: return v.wx;
    }
    if (!c.cgb) return 0xFF;
    switch (o) {
    case 0x4F: return (uint8_t)(0xFE | v.vbk);
    case 0x55: return v.hdma5;
    case 0x68: return v.bcps;
    case 0x69: return v.pal_lock ? 0xFF : v.regs[0x29];
    case 0x6A: return v.ocps;
    case 0x6B: return v.pal_lock ? 0xFF : v.regs[0x2B];
    case 0x6C: return (uint8_t)(0xFE | v.opri);
    case 0x72: case 0x73: case 0x74: case 0x75: return v.regs[o - 0x40];
    default: return 0xFF;
    }
}

void ppu_write(GB& c, uint16_t a, uint8_t x) {
    ppu_sync(c);
    PPU& v = c.v;
    uint8_t o = a & 0xFF;
    switch (o) {
    case 0x40: {
        uint8_t was = v.lcdc;
        v.lcdc = x;
        if (!(x & 0x80) && (was & 0x80)) lcd_off(c);
        else if ((x & 0x80) && !(was & 0x80)) lcd_on(c);
        break;
    }
    case 0x41: {
        uint8_t was_line = v.stat_line;
        v.stat = (uint8_t)((x & 0x78) | (v.stat & 0x07));
        if (v.lcdc & 0x80) {
            // the DMG's STAT write bug: writing STAT in mode 0 or 1 (or with LY = LYC) raises the interrupt
            bool bug = !c.cgb && !was_line && (v.mode == 0 || v.mode == 1 || (v.stat & 4));
            stat_update(c, false);
            if (bug && !(c.IF & I_STAT)) gb_irq(c, I_STAT);
        }
        break;
    }
    case 0x42: v.scy = x; break;
    case 0x43: v.scx = x; break;
    case 0x44: break;
    case 0x45:
        if (v.lyc != x) {
            v.lyc = x;
            if (v.lcdc & 0x80) {
                bool line153 = v.st == P_L153_1 || v.st == P_L153_2 ? false : false;
                (void)line153;
                if (v.ly == x) { v.stat |= 4; c.v.src_lyc = 1; } else { v.stat &= ~4; c.v.src_lyc = 0; }
                stat_update(c);
            }
        }
        break;
    case 0x46:
        v.dma_reg = x;
        v.dma_copy_at = c.t + 8 * DOT_T;
        if (c.tr) c.tr->oam_dma++;
        break;
    case 0x47: v.bgp = x; break;
    case 0x48: v.obp0 = x; break;
    case 0x49: v.obp1 = x; break;
    case 0x4A: v.wy = x; break;
    case 0x4B: v.wx = x; break;
    default:
        if (!c.cgb) break;
        switch (o) {
        case 0x4F: v.vbk = x & 1; break;
        case 0x51: v.hdma1 = x; break;
        case 0x52: v.hdma2 = x; break;
        case 0x53: v.hdma3 = x; break;
        case 0x54: v.hdma4 = x; break;
        case 0x55:
            if (!(x & 0x80)) {
                if (v.hdma_on) { v.hdma_on = 0; v.hdma5 = (uint8_t)(v.hdma5 | 0x80); }
                else {
                    hdma_block(c, ((x & 0x7F) + 1) * 16);       // general: all at once, the CPU waits
                    if (c.tr) c.tr->hdma_general++;
                    v.hdma5 = 0xFF;
                }
            } else {
                v.hdma_on = 1; v.hdma5 = x & 0x7F;
                if (v.hdma_possible) {
                    hdma_block(c, 16);
                    if (c.tr) c.tr->hdma_hblank++;
                    v.hdma5--;
                    if ((v.hdma5 & 0x7F) == 0x7F) { v.hdma5 = 0xFF; v.hdma_on = 0; }
                    v.hdma_possible = 0;
                }
            }
            break;
        case 0x68: v.bcps = x; v.regs[0x29] = (x & 1) ? (uint8_t)(v.bpal[(x >> 1) & 0x1F] >> 8) : (uint8_t)v.bpal[(x >> 1) & 0x1F]; break;
        case 0x69: {
            if (v.pal_lock) break;                   // MAME: dropped, and the index stays
            v.regs[0x29] = x;
            uint16_t& p = v.bpal[(v.bcps >> 1) & 0x1F];
            p = (uint16_t)(((v.bcps & 1) ? (x << 8 | (p & 0xFF)) : ((p & 0xFF00) | x)) & 0x7FFF);
            if (v.bcps & 0x80) v.bcps = (uint8_t)((v.bcps + 1) & 0xBF);
            break;
        }
        case 0x6A: v.ocps = x; v.regs[0x2B] = (x & 1) ? (uint8_t)(v.opal[(x >> 1) & 0x1F] >> 8) : (uint8_t)v.opal[(x >> 1) & 0x1F]; break;
        case 0x6B: {
            if (v.pal_lock) break;
            v.regs[0x2B] = x;
            uint16_t& p = v.opal[(v.ocps >> 1) & 0x1F];
            p = (uint16_t)(((v.ocps & 1) ? (x << 8 | (p & 0xFF)) : ((p & 0xFF00) | x)) & 0x7FFF);
            if (v.ocps & 0x80) v.ocps = (uint8_t)((v.ocps + 1) & 0xBF);
            break;
        }
        case 0x6C: v.opri = x & 1; break;
        case 0x72: case 0x73: case 0x74: v.regs[o - 0x40] = x; break;
        case 0x75: v.regs[0x35] = (uint8_t)(0x8F | (x & 0x70)); break;
        }
        break;
    }
}

void ppu_reset(GB& c) {
    PPU& v = c.v;
    std::memset(&v, 0, sizeof v);
    c.v.frame_due = false; c.v.src_m2 = 0; c.v.src_lyc = 0;
    v.lcdc = 0x91; v.stat = 0x00; v.bgp = 0xFC; v.obp0 = 0xFF; v.obp1 = 0xFF;
    v.hdma5 = 0xFF;
    v.regs[0x35] = 0x8F;
    for (int i = 0; i < 32; i++) { v.bpal[i] = 0x7FFF; v.opal[i] = 0x0000; }
    for (int y = 0; y < VIS_H; y++) blank_line(c, y);
    lcd_on(c);
}
