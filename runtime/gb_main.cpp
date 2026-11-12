// The Game Boy executable: the recompiled game, or (built with no native code)
// the interpreter and the tracer.
//
//   gb ROM.gb [--frames N] [--snap F1,F2,..] [--out DIR]   headless, the monkey pad
//   gb ROM.gb --interp ...          the interpreter alone (the reference)
//   gb ROM.gb --lockstep ...        native and interpreter side by side: the
//                                   machine state compared at every frame
//   gb ROM.gb --trace DIR ...       the interpreter, measuring for the recompiler
//   gb ROM.gb --window [--scale 3]  in a window, with the keyboard and sound (SDL2)
//   gb ROM.gb --wav FILE ...        headless, the sound written to a WAV
//   gb ROM.gb --window --record PAD ... and the pad written to PAD, a byte a frame
//   gb ROM.gb --replay PAD ...      headless, the pad from PAD instead of the monkey
//   --dmg / --cgb                   the machine (by default: a CGB for a game with bit 7 of $0143)
//   --mbc NAME --ram BYTES --battery --rtc   what gbkit/cart.py knows (MAME's lists)
//   --serial                        print what the game sent on the serial port (the test ROMs)
//   --regs                          the CPU's registers at the end (mooneye's verdict)
//   --peek ADDR:N                   N bytes from ADDR at the end, as the CPU reads them (blargg's $A000; repeatable)
//   --state                         the PPU's and the ports' state at the end
//   --profile                       where the interpreter ran under native code
//
// Keys: arrows, X = A, Z = B, Enter = Start, Backspace = Select, F12 a screenshot, Esc quits.
#include "gb.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#ifdef GB_SDL
#include <SDL2/SDL.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
// the ROM's path as Windows has it (UTF-16): the set's file names hold characters no code page does
static FILE* open_arg(int index, const char* narrow) {
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    FILE* f = nullptr;
    if (w && index < n) f = _wfopen(w[index], L"rb");
    if (w) LocalFree(w);
    return f ? f : std::fopen(narrow, "rb");
}
#else
static FILE* open_arg(int, const char* narrow) { return std::fopen(narrow, "rb"); }
#endif

// ---- the monkey: a deterministic pad ----
static uint32_t rng = 12345;
static uint32_t rnd() { rng = rng * 1103515245u + 12345u; return rng >> 16; }
enum { K_RIGHT = 1, K_LEFT = 2, K_UP = 4, K_DOWN = 8, K_A = 16, K_B = 32, K_SELECT = 64, K_START = 128 };
static uint8_t monkey(long f) {
    static uint8_t held = 0, fire = 0;
    if (f < 120) return 0;
    // Start to get past the titles and menus: early on, then now and then twice
    // (in a game, a pause and its end)
    if ((f >= 120 && f < 130) || (f >= 240 && f < 250) || (f >= 400 && f < 410) || (f >= 600 && f < 610) ||
        (f % 1800 >= 900 && f % 1800 < 910) || (f % 1800 >= 930 && f % 1800 < 940)) return K_START;
    int ph = (int)((f - 120) % 300);
    if ((ph >= 30 && ph < 38) || (ph >= 90 && ph < 98)) return K_A;
    if (ph % 16 == 0) {
        static const uint8_t dirs[] = { 0, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_UP | K_RIGHT, K_UP | K_LEFT,
                                        K_DOWN | K_RIGHT, K_DOWN | K_LEFT, K_RIGHT, K_LEFT };
        held = dirs[rnd() % 11];
        fire = 0;
        if (rnd() % 100 < 40) fire |= K_A;
        if (rnd() % 100 < 25) fire |= K_B;
    }
    return (uint8_t)(held | fire);
}

struct Opts {
    std::string rom, out = ".", wav, trace, record, replay, mbc;
    std::vector<std::string> peek;
    long frames = 6000;
    bool interp = false, lockstep = false, window = false, quiet = false, rtc = false, battery = false;
    bool serial = false, regs = false, state = false, hist = false;
    int model = -1;           // -1: as the header says; 0 DMG, 1 CGB
    long ram = -1;
    int scale = 3;
    std::vector<long> snaps;
};
static Opts O;
static GB* g_ref = nullptr;     // the interpreter, in lockstep
static std::vector<int16_t> g_samples;
static long g_mismatch = -1;
static std::vector<uint8_t> g_pad;     // --record / --replay: the pad, a byte a frame
static uint32_t* g_prof = nullptr;

static void write_ppm(const GB& c, const std::string& path) {
    static uint32_t px[VIS_W * VIS_H];
    gb_rgb(c, px);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", VIS_W, VIS_H);
    uint8_t row[VIS_W * 3];
    for (int y = 0; y < VIS_H; y++) {
        for (int x = 0; x < VIS_W; x++) {
            uint32_t v = px[y * VIS_W + x];
            row[3 * x] = (uint8_t)(v >> 16); row[3 * x + 1] = (uint8_t)(v >> 8); row[3 * x + 2] = (uint8_t)v;
        }
        std::fwrite(row, 1, sizeof row, f);
    }
    std::fclose(f);
}

static void diff(const GB& a, const GB& b) {
    auto cpu = [](const char* who, const GB& c) {
        std::fprintf(stderr, "  %s: A=%02X F=%02X B=%02X C=%02X D=%02X E=%02X H=%02X L=%02X SP=%04X PC=%04X ime=%d ei=%d "
                     "halt=%d t=%llu LY=%d mode=%d bank=%u\n", who, c.A, c.F, c.B, c.C, c.D, c.E, c.H, c.L, c.SP, c.PC,
                     c.ime, c.ei, c.halted, (unsigned long long)c.t, c.v.ly, c.v.mode, c.bankx);
    };
    cpu("interp", a);
    cpu("native", b);
    int n = 0;
    for (int i = 0; i < 0x8000 && n < 12; i++)
        if (a.wram[i] != b.wram[i]) { std::fprintf(stderr, "  wram[%04X] %02X / %02X\n", i, a.wram[i], b.wram[i]); n++; }
    for (int i = 0; i < 0x80 && n < 16; i++)
        if (a.hram[i] != b.hram[i]) { std::fprintf(stderr, "  hram[%04X] %02X / %02X\n", 0xFF80 + i, a.hram[i], b.hram[i]); n++; }
    for (int i = 0; i < 0x4000 && n < 20; i++)
        if (a.vram[i] != b.vram[i]) { std::fprintf(stderr, "  vram[%04X] %02X / %02X\n", i, a.vram[i], b.vram[i]); n++; }
    for (uint32_t i = 0; i < a.sram_size && n < 24; i++)
        if (a.sram[i] != b.sram[i]) { std::fprintf(stderr, "  sram[%05X] %02X / %02X\n", i, a.sram[i], b.sram[i]); n++; }
    if (a.IE != b.IE || a.IF != b.IF) std::fprintf(stderr, "  IE %02X/%02X IF %02X/%02X\n", a.IE, b.IE, a.IF, b.IF);
    if (a.divcount != b.divcount || a.tima != b.tima) std::fprintf(stderr, "  div %04X/%04X tima %02X/%02X\n", a.divcount, b.divcount, a.tima, b.tima);
    if (std::memcmp(&a.v, &b.v, sizeof a.v)) std::fprintf(stderr, "  the video state differs\n");
    if (std::memcmp(a.fb, b.fb, sizeof a.fb)) std::fprintf(stderr, "  the picture differs\n");
    if (std::memcmp(a.oam, b.oam, sizeof a.oam)) std::fprintf(stderr, "  OAM differs\n");
}

static void history(const char* who, const GB& c) {
    std::string path = O.out + "/hist_" + who + ".txt";
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    uint32_t n = c.hn < 65536 ? c.hn : 65536;
    for (uint32_t i = c.hn - n; i != c.hn; i++)
        std::fprintf(f, "%03X:%04X %llu\n", c.hist[i & 65535] >> 16, c.hist[i & 65535] & 0xFFFF, (unsigned long long)c.htick[i & 65535]);
    std::fclose(f);
    std::fprintf(stderr, "  %s: the last %u instructions in %s\n", who, n, path.c_str());
}

static void mismatch(GB& c, const char* where) {
    std::fprintf(stderr, "lockstep: the machines differ at frame %llu%s\n", (unsigned long long)c.frame, where);
    diff(*g_ref, c);
    history("interp", *g_ref);
    history("native", c);
    g_mismatch = (long)c.frame;
    throw GBStop();
}

static void frame_common(GB& c) {
    long f = (long)c.frame;
    for (long s : O.snaps)
        if (s == f) { char n[32]; std::snprintf(n, sizeof n, "/f%05ld.ppm", f); write_ppm(c, O.out + n); }
    if (f >= O.frames) throw GBStop();
}

static void take_sound(GB& c) {
    if (!c.snd_out) return;
    g_samples.insert(g_samples.end(), c.s.out, c.s.out + c.s.out_n);
    c.s.out_n = 0;
}

static void hook_headless(GB& c) {
    long f = (long)c.frame;
    if (g_ref) {
        // the reference runs to the same frame event, then the two are compared
        while ((long)g_ref->frame < f)
            if (!gb_flat_frame(*g_ref)) break;
        if (gb_hash(*g_ref) != gb_hash(c)) mismatch(c, "");
    }
    take_sound(c);
    uint8_t p = !O.replay.empty() ? (f < (long)g_pad.size() ? g_pad[f] : 0) : monkey(f);
    c.keys = p;
    if (g_ref) g_ref->keys = p;
    frame_common(c);
}

#ifdef GB_SDL
static SDL_Window* win; static SDL_Renderer* ren; static SDL_Texture* tex;
static SDL_AudioDeviceID adev;
static std::chrono::steady_clock::time_point t_next;
static void hook_window(GB& c) {
    static uint32_t px[VIS_W * VIS_H];
    gb_rgb(c, px);
    SDL_UpdateTexture(tex, nullptr, px, VIS_W * 4);
    SDL_RenderClear(ren); SDL_RenderCopy(ren, tex, nullptr, nullptr); SDL_RenderPresent(ren);
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) throw GBStop();
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F12) {      // a screenshot, named by its frame
            char n[48]; std::snprintf(n, sizeof n, "/shot_%05llu.ppm", (unsigned long long)c.frame);
            write_ppm(c, O.out + n);
            std::printf("screenshot: frame %llu -> %s%s\n", (unsigned long long)c.frame, O.out.c_str(), n);
            std::fflush(stdout);
        }
    }
    const uint8_t* k = SDL_GetKeyboardState(nullptr);
    uint8_t j = 0;
    if (k[SDL_SCANCODE_RIGHT]) j |= K_RIGHT;
    if (k[SDL_SCANCODE_LEFT]) j |= K_LEFT;
    if (k[SDL_SCANCODE_UP]) j |= K_UP;
    if (k[SDL_SCANCODE_DOWN]) j |= K_DOWN;
    if (k[SDL_SCANCODE_X]) j |= K_A;
    if (k[SDL_SCANCODE_Z]) j |= K_B;
    if (k[SDL_SCANCODE_BACKSPACE]) j |= K_SELECT;
    if (k[SDL_SCANCODE_RETURN]) j |= K_START;
    c.keys = j;
    if (!O.record.empty()) {
        if (g_pad.size() <= c.frame) g_pad.resize(c.frame + 1, 0);
        g_pad[c.frame] = c.keys;
    }
    g_samples.clear();
    take_sound(c);
    if (adev) {
        uint32_t queued = SDL_GetQueuedAudioSize(adev), fr = (uint32_t)g_samples.size() * 2;
        if (queued < fr * 6) SDL_QueueAudio(adev, g_samples.data(), fr);
    }
    t_next += std::chrono::microseconds(16742);   // 70224 dots at 4.194304 MHz
    std::this_thread::sleep_until(t_next);
}
#endif

// the header's type byte -> the mapper, battery, RTC
static void from_header(const std::vector<uint8_t>& rom, int& mbc, bool& batt, bool& rtc, uint32_t& ram) {
    uint8_t t = rom[0x147];
    mbc = MBC_ROM; batt = false; rtc = false;
    switch (t) {
    case 0x00: case 0x08: break; case 0x09: batt = true; break;
    case 0x01: case 0x02: mbc = MBC_1; break; case 0x03: mbc = MBC_1; batt = true; break;
    case 0x05: mbc = MBC_2; break; case 0x06: mbc = MBC_2; batt = true; break;
    case 0x0F: mbc = MBC_3; batt = rtc = true; break; case 0x10: mbc = MBC_3; batt = rtc = true; break;
    case 0x11: case 0x12: mbc = MBC_3; break; case 0x13: mbc = MBC_3; batt = true; break;
    case 0x19: case 0x1A: case 0x1C: case 0x1D: mbc = MBC_5; break;
    case 0x1B: case 0x1E: mbc = MBC_5; batt = true; break;
    case 0x22: mbc = MBC_7; batt = true; break;
    case 0xFE: mbc = MBC_HUC3; batt = true; break;
    case 0xFF: mbc = MBC_HUC1; batt = true; break;
    default: mbc = rom.size() > 0x8000 ? MBC_1 : MBC_ROM; break;
    }
    static const uint32_t sizes[6] = { 0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000 };
    uint8_t r = rom[0x149];
    ram = r < 6 ? sizes[r] : 0;
    if (t == 0x00 || t == 0x01 || t == 0x05 || t == 0x06 || t == 0x0F || t == 0x11 || t == 0x19 || t == 0x1C) ram = 0;
}

static int mbc_by_name(const std::string& s) {
    if (s == "rom") return MBC_ROM; if (s == "rom_mbc1") return MBC_1; if (s == "rom_mbc2") return MBC_2;
    if (s == "rom_mbc3" || s == "rom_mbc30") return MBC_3; if (s == "rom_mbc5") return MBC_5;
    if (s == "rom_huc1") return MBC_HUC1; if (s == "rom_huc3") return MBC_HUC3;
    if (s == "rom_mbc7_2k" || s == "rom_mbc7_4k") return MBC_7;
    return MBC_NONE;
}

int main(int argc, char** argv) {
    int rom_arg = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--frames") O.frames = std::atol(val().c_str());
        else if (a == "--interp") O.interp = true;
        else if (a == "--lockstep") O.lockstep = true;
        else if (a == "--hist") O.hist = true;
        else if (a == "--state") O.state = true;
        else if (a == "--regs") O.regs = true;
        else if (a == "--serial") O.serial = true;
        else if (a == "--peek") O.peek.push_back(val());
        else if (a == "--window") O.window = true;
        else if (a == "--quiet") O.quiet = true;
        else if (a == "--profile") { static uint32_t prof[512 * 0x10000]; g_prof = prof; }
        else if (a == "--scale") O.scale = std::atoi(val().c_str());
        else if (a == "--out") O.out = val();
        else if (a == "--wav") O.wav = val();
        else if (a == "--record") O.record = val();
        else if (a == "--replay") O.replay = val();
        else if (a == "--trace") O.trace = val();
        else if (a == "--dmg") O.model = 0;
        else if (a == "--cgb") O.model = 1;
        else if (a == "--mbc") O.mbc = val();
        else if (a == "--ram") O.ram = std::atol(val().c_str());
        else if (a == "--battery") O.battery = true;
        else if (a == "--rtc") O.rtc = true;
        else if (a == "--snap") {
            std::string s = val(); size_t p = 0;
            while (p < s.size()) { O.snaps.push_back(std::atol(s.c_str() + p));
                                   p = s.find(',', p); if (p == std::string::npos) break; p++; }
        } else { O.rom = a; rom_arg = i; }
    }
    if (O.rom.empty()) { std::fprintf(stderr, "gb ROM.gb [--frames N] [--interp|--lockstep|--trace DIR|--window]\n"); return 1; }
    FILE* f = open_arg(rom_arg, O.rom.c_str());
    if (!f) { std::perror(O.rom.c_str()); return 1; }
    static std::vector<uint8_t> rom;
    { uint8_t buf[65536]; size_t n; while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) rom.insert(rom.end(), buf, buf + n); }
    std::fclose(f);
    if (rom.size() < 0x150) { std::fprintf(stderr, "%s: too small\n", O.rom.c_str()); return 1; }
    if (rom.size() < 0x8000) rom.resize(0x8000, 0xFF);
    bool cgb = O.model >= 0 ? O.model == 1 : (rom[0x143] & 0x80) != 0;
    int mbc; bool batt, rtc; uint32_t ram;
    from_header(rom, mbc, batt, rtc, ram);
    if (!O.mbc.empty()) {
        mbc = mbc_by_name(O.mbc);
        if (mbc == MBC_NONE) { std::fprintf(stderr, "%s: the mapper %s is not covered\n", O.rom.c_str(), O.mbc.c_str()); return 3; }
        batt = O.battery; rtc = O.rtc;
        if (O.ram >= 0) ram = (uint32_t)O.ram;
    }

    if (!O.replay.empty()) {
        FILE* pf = std::fopen(O.replay.c_str(), "rb");
        if (!pf) { std::perror(O.replay.c_str()); return 1; }
        int b; while ((b = std::fgetc(pf)) != EOF) g_pad.push_back((uint8_t)b);
        std::fclose(pf);
        if (O.frames == 6000) O.frames = (long)g_pad.size();
    }
    gb_register_all();
    static GB c, ref;
    gb_load(c, rom.data(), (uint32_t)rom.size(), cgb, mbc, ram, batt, rtc);
    gb_reset(c);
    c.keys = monkey(0);
    c.natives = !O.interp && O.trace.empty();
    c.prof = g_prof;
    c.frame_hook = hook_headless;
    c.snd_out = !O.wav.empty();
    if (!O.trace.empty()) gb_trace_begin(c);
    if (O.lockstep) {
        gb_load(ref, rom.data(), (uint32_t)rom.size(), cgb, mbc, ram, batt, rtc);
        gb_reset(ref);
        ref.keys = monkey(0);
        ref.natives = false;
        ref.flat_stop = true;
        g_ref = &ref;
    }
#ifdef GB_SDL
    if (O.window) {
        SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
        SDL_AudioSpec want{}, have{};
        want.freq = SND_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
        adev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (adev) SDL_PauseAudioDevice(adev, 0);
        c.snd_out = true;
        win = SDL_CreateWindow("gbkit", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               VIS_W * O.scale, VIS_H * O.scale, SDL_WINDOW_RESIZABLE);
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, VIS_W, VIS_H);
        c.frame_hook = hook_window;
        c.keys = 0;
        t_next = std::chrono::steady_clock::now();
    }
#endif
    auto t0 = std::chrono::steady_clock::now();
    try {
        if (!c.natives) {
            c.flat_stop = false;
            for (;;) if (!gb_flat_frame(c)) break;
        } else gb_run(c);
    } catch (GBStop&) {}
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (c.jammed && g_ref) {
        while (!g_ref->jammed && g_ref->frame < c.frame)
            try { if (!gb_flat_frame(*g_ref)) break; } catch (GBStop&) { break; }
        diff(*g_ref, c);
        history("interp", *g_ref);
        history("native", c);
    } else if (c.jammed || O.hist) history(c.natives ? "native" : "interp", c);
    if (g_prof) {                      // where the interpreter ran, under native code
        std::vector<std::pair<uint32_t, uint32_t>> v;
        for (uint32_t i = 0; i < 512u * 0x10000; i++) if (g_prof[i]) v.push_back({ g_prof[i], i });
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < v.size() && i < 16; i++) std::printf("  interp %03X:%04X x%u\n", v[i].second >> 16, v[i].second & 0xFFFF, v[i].first);
    }
    if (O.serial) {
        std::printf("serial: ");
        for (uint32_t i = 0; i < c.sio_n; i++) std::putchar(c.sio_out[i] >= 32 || c.sio_out[i] == '\n' ? c.sio_out[i] : '.');
        std::printf("\n");
    }
    if (O.regs)
        std::printf("regs A=%02X F=%02X B=%02X C=%02X D=%02X E=%02X H=%02X L=%02X SP=%04X PC=%04X\n",
                    c.A, c.F, c.B, c.C, c.D, c.E, c.H, c.L, c.SP, c.PC);
    for (const std::string& pk : O.peek) {
        unsigned long a = std::strtoul(pk.c_str(), nullptr, 16), n = 1;
        size_t k = pk.find(':'); if (k != std::string::npos) n = std::strtoul(pk.c_str() + k + 1, nullptr, 10);
        std::printf("peek %04lX:", a);
        for (unsigned long i = 0; i < n; i++) std::printf(" %02X", gb_peek(c, (uint16_t)(a + i)));
        std::printf("\n");
    }
    if (O.state) {
        const PPU& v = c.v;
        std::printf("lcdc %02X stat %02X scy %02X scx %02X ly %d lyc %d bgp %02X obp %02X %02X wy %d wx %d mode %d\n",
                    v.lcdc, v.stat, v.scy, v.scx, v.ly, v.lyc, v.bgp, v.obp0, v.obp1, v.wy, v.wx, v.mode);
        std::printf("IE %02X IF %02X ime %d PC %04X SP %04X bank %u/%u ram_on %d ram_bank %u svbk %d vbk %d ds %d\n",
                    c.IE, c.IF, c.ime, c.PC, c.SP, c.bank0, c.bankx, c.ram_on, c.ram_bank, c.svbk, v.vbk, c.ds);
    }
    write_ppm(c, O.out + "/last.ppm");
    if (!O.wav.empty()) gb_write_wav(O.wav.c_str(), g_samples.data(), g_samples.size(), SND_RATE);
    if (!O.trace.empty()) gb_trace_report(c, O.trace.c_str(), O.rom.c_str());
    if (!O.record.empty()) {
        if (FILE* pf = std::fopen(O.record.c_str(), "wb")) {
            for (uint8_t k : g_pad) std::fputc(k, pf);
            std::fclose(pf);
        }
        std::printf("recorded %zu frames of the pad to %s\n", g_pad.size(), O.record.c_str());
    }
    if (!O.quiet) {
        std::printf("frames %llu  %.2fs (%.0fx)  hash %016llx\n", (unsigned long long)c.frame, secs,
                    c.frame / 59.7275 / (secs > 0 ? secs : 1e-9), (unsigned long long)gb_hash(c));
        std::printf("interpreted insns %llu  native calls %llu  fallbacks %llu  unwinds %llu  "
                    "max depth %d  irq %llu  halt %llu  ticks %llu\n",
                    (unsigned long long)c.n_interp, (unsigned long long)c.n_native_calls,
                    (unsigned long long)c.n_fallback, (unsigned long long)c.n_unwind, c.max_depth,
                    (unsigned long long)c.n_irq, (unsigned long long)c.n_halt, (unsigned long long)c.t);
        if (O.lockstep)
            std::printf(g_mismatch < 0 ? "lockstep: identical for %llu frames\n"
                                       : "lockstep: MISMATCH at frame %llu\n",
                        (unsigned long long)(g_mismatch < 0 ? c.frame : g_mismatch));
    }
    return g_mismatch >= 0 ? 2 : (c.jammed ? 4 : 0);
}
