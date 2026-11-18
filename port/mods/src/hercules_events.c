/* Event log for Hercules (all five PAL releases): the game state that
 * achievements are made of, written as one JSON line per change to
 * hercules_events.jsonl in the game's folder (docs: hercules/docs/
 * achievements_proposal.md). Nothing leaves the machine here; the launcher
 * may send the file later, only if the player agreed.
 *
 * Polled once per VBlank: every variable below is the game's own (main EXE
 * data, the same address in every release) and the pickup handler (ENGINE1
 * func_800611EC, jtbl_80071EA0) changes exactly one of them per pickup type,
 * so a change names its cause:
 *   letters D_80035064 |= 1 << index (0x200B)   password pieces D_80035059 bits (0x201A-D)
 *   coins D_8003505C (0x2019) / D_80035060 (0x2018), level totals at +2
 *   max energy D_80035066 += 0x20 (0x2002)       energy D_8003506C (+0x20 pickup, drops = damage)
 *   power charges D_8003506E/70/72/74 += 0x80 (0x2015/16/17/14)   lives D_80035054 (0x2012)
 * plus the level end / game over code D_8003504B, deaths in the level
 * D_80035055, difficulty, continues, cheat bits, demo flags, the password
 * flag in TITLE and TITLE task 0's step (rating 0xD2, ending 0xBC/0xBE/0xCD,
 * game over 0x8C-0x96 / 0xC8 / 0xC9, menu 0x28).
 *
 * A save state or a rewind changes the same variables in one step; such a
 * jump is logged like any other change (the analysis sees it as one).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "mod_plugins.h"
#include "hercules_addrs.h" /* per PAL version, hercules/tools/port_version.py */

#define LOG_FILE  "hercules_events.jsonl"
#define LOG_LIMIT (4u << 20)     /* stop at 4 MB until the file is sent or removed */
#define EVENTS_VERSION 1

typedef struct {
    int title, engine, step;
    int level, exit_code, lives, deaths, difficulty, continues, pw, cheats_known;
    int coins_a, coins_a_total, coins_b, coins_b_total, letters, max_energy, energy;
    int power[4];
    uint32_t cheats, password, demo;
} State;

static int s_on;
static int s_have;
static State s_prev;
static uint32_t s_vblank;
static FILE* s_log;
static long s_size;

static uint32_t rd_u8(uint32_t a) { return (psx_mod_read_word(a & ~3u) >> (8u * (a & 3u))) & 0xFFu; }
static uint32_t rd_u16(uint32_t a) { return (psx_mod_read_word(a & ~3u) >> (8u * (a & 2u))) & 0xFFFFu; }
static int rd_s16(uint32_t a) { const uint32_t v = rd_u16(a); return v & 0x8000u ? (int)v - 0x10000 : (int)v; }

static void read_state(State* s) {
    memset(s, 0, sizeof *s);
    s->title = psx_mod_read_word(HERC_TI_DECODE_SITE) == HERC_TI_DECODE_WORD;
    s->engine = psx_mod_read_word(HERC_E1_DECODE_SITE) == HERC_E1_DECODE_WORD ? 1
              : psx_mod_read_word(HERC_E3_DECODE_SITE) == HERC_E3_DECODE_WORD ? 3 : 0;
    s->step = s->title && psx_mod_read_word(HERC_TI_TASK0) == HERC_TI_INTRO_HANDLER
                  ? (int)psx_mod_read_word(HERC_TI_TASK0 + 4u) : -1;
    s->level = (int)rd_u8(HERC_LEVEL);
    s->exit_code = (int)rd_u8(HERC_EXIT_CODE);
    s->lives = (int)rd_u8(HERC_LIVES);
    s->deaths = (int)rd_u8(HERC_DEATHS);
    s->difficulty = (int)rd_u8(HERC_DIFFICULTY);
    s->continues = (int)rd_u8(HERC_CONTINUES);
    s->pw = (int)rd_u8(HERC_PW_BITS);
    s->coins_a = (int)rd_u16(HERC_COINS_A);
    s->coins_a_total = (int)rd_u16(HERC_COINS_A + 2u);
    s->coins_b = (int)rd_u16(HERC_COINS_B);
    s->coins_b_total = (int)rd_u16(HERC_COINS_B + 2u);
    s->letters = (int)rd_u16(HERC_LETTERS);
    s->max_energy = rd_s16(HERC_MAX_ENERGY);
    s->energy = rd_s16(HERC_ENERGY);
    for (int i = 0; i < 4; ++i) s->power[i] = rd_s16(HERC_POWER + 2u * (uint32_t)i);
    s->cheats = psx_mod_read_word(HERC_CHEATS);
    s->password = s->title ? psx_mod_read_word(HERC_TI_PASSWORD) : 0;
    s->demo = rd_u8(HERC_DEMO_FLAG_A) | rd_u8(HERC_DEMO_FLAG_B) << 8;
}

/* One line: common fields, then the event's own ("k":v pairs, already JSON). */
static void emit(const State* s, const char* ev, const char* fields) {
    if (s_size >= (long)LOG_LIMIT) return;
    if (!s_log) {
        s_log = fopen(LOG_FILE, "a");
        if (!s_log) { psx_mod_counter_add("hercules.events.open_failed", 1); s_on = 0; return; }
        fseek(s_log, 0, SEEK_END);
        s_size = ftell(s_log);
    }
    const int n = fprintf(s_log, "{\"v\":%d,\"g\":\"%s\",\"t\":%lld,\"vb\":%u,\"ev\":\"%s\",\"lvl\":%d,\"diff\":%d%s%s}\n",
                          EVENTS_VERSION, HERC_GAME_ID, (long long)time(NULL), s_vblank, ev, s->level,
                          s->difficulty, fields && *fields ? "," : "", fields ? fields : "");
    if (n > 0) s_size += n;
    fflush(s_log);
    psx_mod_counter_add("hercules.events.lines", 1);
}

static void emitf(const State* s, const char* ev, const char* fmt, int a, int b, int c) {
    char f[160];
    snprintf(f, sizeof f, fmt, a, b, c);
    emit(s, ev, f);
}

static void state_fields(const State* s, char* f, size_t cap) {
    snprintf(f, cap, "\"lives\":%d,\"cont\":%d,\"deaths\":%d,\"letters\":%d,\"pw\":%d,\"coins\":[%d,%d,%d,%d],"
             "\"maxe\":%d,\"e\":%d,\"cheats\":%u,\"demo\":%u,\"engine\":%d,\"step\":%d",
             s->lives, s->continues, s->deaths, s->letters, s->pw, s->coins_a, s->coins_a_total,
             s->coins_b, s->coins_b_total, s->max_energy, s->energy, (unsigned)s->cheats,
             (unsigned)s->demo, s->engine, s->step);
}

static void input_trace(void);
static void on_vblank(void) {
    input_trace();
    if (!s_on) return;
    ++s_vblank;
    State n;
    read_state(&n);
    char f[400];
    if (!s_have) {
        s_have = 1;
        state_fields(&n, f, sizeof f);
        emit(&n, "session", f);
        s_prev = n;
        return;
    }
    const State* p = &s_prev;
    if (n.engine != p->engine || n.title != p->title) {
        state_fields(&n, f, sizeof f);
        emit(&n, n.engine ? "level_running" : n.title ? "title" : "loading", f);
    }
    if (n.step != p->step && n.step >= 0)
        emitf(&n, "title_step", "\"step\":%d,\"from\":%d", n.step, p->step, 0);
    if (n.level != p->level) emitf(&n, "level", "\"from\":%d", p->level, 0, 0);
    if (n.exit_code != p->exit_code) emitf(&n, "exit_code", "\"code\":%d,\"from\":%d", n.exit_code, p->exit_code, 0);
    if (n.difficulty != p->difficulty) emitf(&n, "difficulty", "\"from\":%d", p->difficulty, 0, 0);
    if (n.cheats != p->cheats) emitf(&n, "cheats", "\"bits\":%d,\"from\":%d", (int)n.cheats, (int)p->cheats, 0);
    if (n.demo != p->demo) emitf(&n, "demo", "\"flags\":%d,\"from\":%d", (int)n.demo, (int)p->demo, 0);
    if (n.password != p->password) emitf(&n, "password", "\"value\":%d,\"from\":%d", (int)n.password, (int)p->password, 0);
    for (int b = 0; b < 8; ++b)
        if ((n.letters & ~p->letters) >> b & 1) emitf(&n, "letter", "\"index\":%d,\"bits\":%d", b, n.letters, 0);
    if (n.letters != p->letters && (n.letters & p->letters) != p->letters)
        emitf(&n, "letters_reset", "\"bits\":%d,\"from\":%d", n.letters, p->letters, 0);
    for (int b = 0; b < 4; ++b)
        if ((n.pw & ~p->pw) >> b & 1) emitf(&n, "password_piece", "\"index\":%d,\"bits\":%d", b, n.pw, 0);
    if (n.pw != p->pw && (n.pw & p->pw) != p->pw)
        emitf(&n, "password_reset", "\"bits\":%d,\"from\":%d", n.pw, p->pw, 0);
    if (n.coins_a != p->coins_a || n.coins_a_total != p->coins_a_total)
        emitf(&n, "coins_a", "\"n\":%d,\"from\":%d,\"total\":%d", n.coins_a, p->coins_a, n.coins_a_total);
    if (n.coins_b != p->coins_b || n.coins_b_total != p->coins_b_total)
        emitf(&n, "coins_b", "\"n\":%d,\"from\":%d,\"total\":%d", n.coins_b, p->coins_b, n.coins_b_total);
    if (n.max_energy != p->max_energy) emitf(&n, "max_energy", "\"n\":%d,\"from\":%d", n.max_energy, p->max_energy, 0);
    if (n.energy != p->energy)
        emitf(&n, n.energy < p->energy ? "damage" : "energy_up", "\"n\":%d,\"from\":%d", n.energy, p->energy, 0);
    for (int i = 0; i < 4; ++i)   /* charges: only gains (one of them drains over time) */
        if (n.power[i] > p->power[i]) emitf(&n, "power", "\"slot\":%d,\"n\":%d,\"from\":%d", i, n.power[i], p->power[i]);
    if (n.lives != p->lives) emitf(&n, "lives", "\"n\":%d,\"from\":%d", n.lives, p->lives, 0);
    if (n.deaths != p->deaths) emitf(&n, "deaths", "\"n\":%d,\"from\":%d", n.deaths, p->deaths, 0);
    if (n.continues != p->continues) emitf(&n, "continues", "\"n\":%d,\"from\":%d", n.continues, p->continues, 0);
    s_prev = n;
}

/* HERC_INPUT_TRACE=1 (tests only): when the game's own pad words change
 * (ENGINE1 func_80038B58 fills D_800350A4..D_800350AE from the pad) and when
 * the hero (object at *D_80074A00, x +2, y +6) first moves after that, one
 * stdout line each with CLOCK_MONOTONIC -- the clock of the runtime's
 * PSX_INPUT_TRACE lines and of SurfaceFlinger's frame times. */
static int s_it = -1;
static uint32_t s_it_pad[3], s_it_vb, s_it_press_vb;
static int s_it_hx, s_it_hy, s_it_wait;

static void input_trace(void) {
    if (s_it < 0) { const char* e = getenv("HERC_INPUT_TRACE"); s_it = e && *e == '1'; }
    if (!s_it) return;
    ++s_it_vb;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    const unsigned long long ns = (unsigned long long)t.tv_sec * 1000000000ull + (unsigned long long)t.tv_nsec;
    const uint32_t pad[3] = {psx_mod_read_word(0x800350A4u), psx_mod_read_word(0x800350A8u), psx_mod_read_word(0x800350ACu)};
    const uint32_t hero = psx_mod_read_word(0x80074A00u);
    int hx = 0, hy = 0;
    if ((hero & 0xFF000000u) == 0x80000000u) {
        hx = rd_s16(hero + 2u);
        hy = rd_s16(hero + 6u);
    }
    if (pad[0] != s_it_pad[0] || pad[1] != s_it_pad[1] || pad[2] != s_it_pad[2]) {
        const int newly = (pad[0] & 0xFFFFu) && !(s_it_pad[0] & 0xFFFFu);
        fprintf(stdout, "herc-trace pad %08x %08x %08x vb=%u t=%llu\n", pad[0], pad[1], pad[2], s_it_vb, ns);
        if (newly) { s_it_press_vb = s_it_vb; s_it_wait = 1; s_it_hx = hx; s_it_hy = hy; }
        memcpy(s_it_pad, pad, sizeof pad);
    }
    if (s_it_wait && (hx != s_it_hx || hy != s_it_hy)) {
        fprintf(stdout, "herc-trace hero moves %d,%d -> %d,%d after %u vb t=%llu\n", s_it_hx, s_it_hy, hx, hy,
                s_it_vb - s_it_press_vb, ns);
        s_it_wait = 0;
    } else if (s_it_wait && s_it_vb - s_it_press_vb > 60) {
        s_it_wait = 0;
    }
    fflush(stdout);
}

static void on_activate(void) { s_on = 1; }

PSX_MOD_CONSTRUCTOR(hercules_register_events) {
    (void)psx_mod_register_activation_plugin("hercules.events", on_activate);
    (void)psx_mod_register_vblank_plugin("hercules.events", on_vblank);
}
