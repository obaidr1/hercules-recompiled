/* Widescreen for Hercules (SCES-00893).
 *
 * PSXRecomp keeps PSX titles at 4:3 until a trusted plugin opts in. With the
 * native-wide renderer the 4:3 picture stays exact and the extra width is
 * drawn on a separate host surface ([widescreen] gte_game_mode in game.toml
 * marks level frames as gameplay). The game's own culls still assume 320
 * pixels (psxrecomp/docs/WIDESCREEN.md).
 *
 * HUD: every HUD piece is one SPRT built by a small helper (ENGINE1
 * func_80049F9C, ENGINE3 func_80046D50: GetClut(0x160, 0x20 + n), x in $s5,
 * caller's $ra at sp+0x2C). Packets come from the shared, double-buffered
 * primitive pool, so their addresses change every frame; the helper's
 * closing `sw $a0, 0($s0)` is hooked and each packet built for the HUD
 * routine (ENGINE1 func_80049128 / func_80049D64, ENGINE3 func_800465B0) is
 * anchored to the left or right screen edge by the call that drew it: the
 * right group is the few calls with a fixed x of 236 or more (the right
 * emblem and its wing, and func_80049D64's strip at the bottom), everything
 * else belongs to the health bar on the left. Deciding by x instead split the
 * health bar once it grows (max health D_80035068 sets its length, up to 255
 * px): its 64-px pieces at x 220/284 and its end cap at length + 65 went to
 * the right edge (issue #5). Other callers (menus) stay centred.
 *
 * Backdrop: the far scenery (sky, distant forest, meadow) is a separate 2D
 * map of 16x16 cells (ENGINE1 func_8003D348, ENGINE3 func_8003C214; arg a0 =
 * {scrollX, scrollY}). Each row draws 21 columns from screen x = -(scrollX&15),
 * merging equal neighbours into wider SPRTs (POLY_FT4 for flipped cells), so
 * it ends 9 px past the 4:3 edge. Instruction hooks widen that loop while the
 * native-wide margin is open: start column k earlier (mod map width), fine
 * scroll +16k (so x starts 16k further left) and both `t5 < 21` bounds become
 * `t5 < 21 + 2k`. (Calling the builder repeatedly does not work: its exit
 * splices OT buckets 0x88/0x8C into 0x70/0x7C, which loops on a second call.)
 */
#include <stdlib.h>
#include <string.h>
#include "mod_plugins.h"
#include "cpu_state.h"
#include "hercules_addrs.h" /* per PAL version, hercules/tools/port_version.py */

#define REG_S0 16
#define REG_SP 29
#define REG_A0 4
#define REG_GP 28
#define REG_RA 31
#define SW_A0_0_S0 0xAE040000u   /* sw $a0, 0x0($s0) */
/* Return addresses (minus the HUD routine's start) of the calls that draw the
 * right group, read off the call sites (x set by `ori $a0, $zero, x`): x 278 /
 * 279, 236 and 259, plus ENGINE1 func_80049D64 (x 188..271, y 224..230). The
 * code is the same in every PAL release, only moved, so the offsets hold for
 * all five. */
#define MAX_RIGHT 4
typedef struct {
    const char* name;
    uint32_t site;               /* the packet's closing tag store */
    uint32_t check_addr, check_word; /* GetClut call inside the helper */
    uint32_t hud_lo, hud_hi;     /* callers that draw the in-game HUD */
    uint16_t right[MAX_RIGHT];   /* right-group call offsets, 0 = unused */
} HudHelper;

static const HudHelper k_helpers[] = {
    {"ENGINE1", HERC_E1_HUD_SITE, HERC_E1_HUD_CHECK_SITE, HERC_E1_HUD_CHECK_WORD,
     HERC_E1_HUD_LO, HERC_E1_HUD_HI, {0x042C, 0x04A0, 0x055C, 0x0CD8}},
    {"ENGINE3", HERC_E3_HUD_SITE, HERC_E3_HUD_CHECK_SITE, HERC_E3_HUD_CHECK_WORD,
     HERC_E3_HUD_LO, HERC_E3_HUD_HI, {0x03A8, 0x03D0, 0x048C, 0}},
};
#define HELPER_COUNT (sizeof(k_helpers) / sizeof(k_helpers[0]))

static void on_hud_sprite(struct CPUState* cpu, uint32_t address) {
    const HudHelper* h = NULL;
    for (uint32_t i = 0; i < HELPER_COUNT; i++)
        if ((k_helpers[i].site & 0x1FFFFFFFu) == (address & 0x1FFFFFFFu)) h = &k_helpers[i];
    /* Both engines load at the same address: confirm the helper is resident. */
    if (!h || psx_mod_read_word(h->check_addr) != h->check_word) return;
    const uint32_t ra = psx_mod_read_word(cpu->gpr[REG_SP] + 0x2Cu);
    if (ra < h->hud_lo || ra >= h->hud_hi) return;
    int side = -1;
    for (int i = 0; i < MAX_RIGHT; i++)
        if (h->right[i] && ra - h->hud_lo == h->right[i]) side = +1;
    psx_mod_anchor_hud_primitive(cpu->gpr[REG_S0], side);
    psx_mod_counter_add("hercules.widescreen.hud_anchor", 1);
}

/* Option "aspect" ("N:M", e.g. 16:9, or 20:9 to fill a phone): the start
 * value; the in-game menu switches live. At 4:3 the hooks below do nothing
 * (no side margins). */
static void on_activate(void) {
    char aspect[8] = "";
    unsigned num = 16, den = 9;
    if (psx_mod_current_option_value("aspect", aspect, sizeof(aspect))) {
        const char* colon = strchr(aspect, ':');
        const int n = atoi(aspect), d = colon ? atoi(colon + 1) : 0;
        if (n > 0 && d > 0) { num = (unsigned)n; den = (unsigned)d; }
    }
    if (!psx_mod_set_fixed_display_aspect(num, den))
        psx_mod_counter_add("hercules.widescreen.aspect_refused", 1);
}

/* Backdrop builder sites (same layout in both engines, +0x1134 apart). */
#define REG_V0 2
#define REG_T5 13
#define REG_S3 19
#define REG_S7 23
#define BACKDROP_COLS 21
#define SW_S7_FINE   0xAFB70020u   /* sw s7,0x20(sp): fine scroll x (scrollX & 15) */
#define SW_S7_COL    0xAFB70008u   /* sw s7,0x8(sp): start column (scrollX/16 mod width) */
#define BEQZ_RUN     0x10400007u   /* beqz v0 after the run-scan `slti v0,t5,21` */
#define BNEZ_LOOP    0x1440FF9Cu   /* bnez v0 after the column-loop `slti v0,t5,21` */

typedef struct { uint32_t fine, col, run, loop; } Backdrop;
static const Backdrop k_backdrops[] = {
    {HERC_E1_BD_FINE_SITE, HERC_E1_BD_COL_SITE, HERC_E1_BD_RUN_SITE, HERC_E1_BD_LOOP_SITE},
    /* ENGINE1 func_8003D348 */
    {HERC_E3_BD_FINE_SITE, HERC_E3_BD_COL_SITE, HERC_E3_BD_RUN_SITE, HERC_E3_BD_LOOP_SITE},
    /* ENGINE3 func_8003C214 */
};
#define BACKDROP_COUNT (sizeof(k_backdrops) / sizeof(k_backdrops[0]))

/* Extra 16-px columns per side; 0 at 4:3, so every hook is then a no-op. */
static int backdrop_extra(void) {
    const int margin = psx_mod_widescreen_view_x_margin();
    return margin > 0 ? (margin + 15) / 16 + 1 : 0;
}

static void on_backdrop(struct CPUState* cpu, uint32_t address) {
    const int k = backdrop_extra();
    if (k == 0) return;
    const uint32_t a = address & 0x1FFFFFFFu;
    for (uint32_t i = 0; i < BACKDROP_COUNT; i++) {
        const Backdrop* b = &k_backdrops[i];
        if (a == (b->fine & 0x1FFFFFFFu)) {
            cpu->gpr[REG_S7] += 16u * (uint32_t)k;          /* start k columns further left */
        } else if (a == (b->col & 0x1FFFFFFFu)) {
            const int32_t width = (int32_t)cpu->gpr[REG_S3];
            if (width > 0) {
                int32_t col = ((int32_t)cpu->gpr[REG_S7] - k) % width;
                cpu->gpr[REG_S7] = (uint32_t)(col < 0 ? col + width : col);
            }
        } else if (a == (b->run & 0x1FFFFFFFu) || a == (b->loop & 0x1FFFFFFFu)) {
            cpu->gpr[REG_V0] = (int32_t)cpu->gpr[REG_T5] < BACKDROP_COLS + 2 * k;
            if (a == (b->loop & 0x1FFFFFFFu)) psx_mod_counter_add("hercules.widescreen.backdrop_cols", 1);
        }
    }
}

/* Panorama skies (level overlays): a 256-px wide image repeating horizontally,
 * drawn as three SPRTs per layer by builder(x, y, w, h, [sp+0x10] u, [sp+0x14]
 * v, [sp+0x18] depth) -- the partial left piece, one full 256-px repeat and
 * the partial right piece. While the margin is open, every full repeat also
 * gets the neighbouring repeats at x-256 and x+256 (same stack arguments), so
 * the sky continues into the margins. Each call just adds one SPRT to the OT. */
#define JAL_SETSPRT 0x0C00600Eu
#define ORI_V0_64   0x34020064u
typedef struct { uint32_t func, jal_addr, ori_addr; } Panorama;
static const Panorama k_panoramas[] = {
    {HERC_OV2_SKY, HERC_OV2_SKY_JAL_SITE, HERC_OV2_SKY_ORI_SITE},   /* OV2 (level 2) func_80068910 */
    {HERC_OV8_SKY, HERC_OV8_SKY_JAL_SITE, HERC_OV8_SKY_ORI_SITE},   /* OV8 (level 8) func_800687E8 */
    {HERC_OV9_SKY, HERC_OV9_SKY_JAL_SITE, HERC_OV9_SKY_ORI_SITE},   /* OV9 (level 9) func_800753E0 */
};
#define PANORAMA_COUNT (sizeof(k_panoramas) / sizeof(k_panoramas[0]))
static int s_in_panorama;

static void on_panorama(struct CPUState* cpu, uint32_t address) {
    if (s_in_panorama || psx_mod_widescreen_view_x_margin() <= 0) return;
    const Panorama* pn = NULL;
    for (uint32_t i = 0; i < PANORAMA_COUNT; i++)
        if ((k_panoramas[i].func & 0x1FFFFFFFu) == (address & 0x1FFFFFFFu) &&
            psx_mod_read_word(k_panoramas[i].jal_addr) == JAL_SETSPRT &&
            psx_mod_read_word(k_panoramas[i].ori_addr) == ORI_V0_64)
            pn = &k_panoramas[i];
    if (!pn || cpu->gpr[6] != 0x100u) return;     /* only the full repeat */
    const uint32_t ra = cpu->gpr[REG_RA];
    const uint32_t x = cpu->gpr[4], y = cpu->gpr[5], w = cpu->gpr[6], h = cpu->gpr[7];
    s_in_panorama = 1;
    (void)psx_mod_call_guest(cpu, pn->func, ra, x - 0x100u, y, w, h);
    (void)psx_mod_call_guest(cpu, pn->func, ra, x + 0x100u, y, w, h);
    s_in_panorama = 0;
    /* restore the original arguments; the original body then draws its own repeat */
    cpu->gpr[4] = x; cpu->gpr[5] = y; cpu->gpr[6] = w; cpu->gpr[7] = h;
    psx_mod_counter_add("hercules.widescreen.panorama", 1);
}

/* The Hydra arena (level 6) is drawn about 1.7 times as wide as high: its
 * rock wall ends there and the margins of a wider picture show the plain
 * backdrop. While that level runs, a wider aspect is narrowed to 16:10 (the
 * window keeps its size, the sides stay black) and given back afterwards --
 * also the one the player picks in the in-game menu meanwhile. */
#define HYDRA_LEVEL 6
static int s_hydra_narrowed;
static uint32_t s_player_num, s_player_den;

static void on_vblank_hydra(void) {
    const int in_level = psx_mod_read_word(HERC_E1_DECODE_SITE) == HERC_E1_DECODE_WORD &&
                         (psx_mod_read_word(HERC_LEVEL & ~3u) >> (8u * (HERC_LEVEL & 3u)) & 0xFFu) == HYDRA_LEVEL;
    uint32_t n = 0, d = 0;
    psx_mod_get_display_aspect(&n, &d);
    if (in_level) {
        if (s_hydra_narrowed && !(n == 16 && d == 10)) {    /* changed in the menu meanwhile */
            s_player_num = n; s_player_den = d;
            s_hydra_narrowed = 0;
        }
        if (!s_hydra_narrowed && d && n * 10u > d * 16u) {
            s_player_num = n; s_player_den = d;
            if (psx_mod_set_display_aspect_live(16, 10)) {
                s_hydra_narrowed = 1;
                psx_mod_counter_add("hercules.widescreen.hydra_narrowed", 1);
            }
        }
    } else if (s_hydra_narrowed) {
        if (n == 16 && d == 10) (void)psx_mod_set_display_aspect_live(s_player_num, s_player_den);
        s_hydra_narrowed = 0;
    }
}

PSX_MOD_CONSTRUCTOR(hercules_register_widescreen) {
    (void)psx_mod_register_vblank_plugin("hercules.widescreen", on_vblank_hydra);
    (void)psx_mod_register_activation_plugin("hercules.widescreen", on_activate);
    for (uint32_t i = 0; i < HELPER_COUNT; i++)
        (void)psx_mod_register_instruction_plugin("hercules.widescreen", k_helpers[i].site,
                                                  SW_A0_0_S0, on_hud_sprite);
    for (uint32_t i = 0; i < BACKDROP_COUNT; i++) {
        const Backdrop* b = &k_backdrops[i];
        (void)psx_mod_register_instruction_plugin("hercules.widescreen", b->fine, SW_S7_FINE, on_backdrop);
        (void)psx_mod_register_instruction_plugin("hercules.widescreen", b->col, SW_S7_COL, on_backdrop);
        (void)psx_mod_register_instruction_plugin("hercules.widescreen", b->run, BEQZ_RUN, on_backdrop);
        (void)psx_mod_register_instruction_plugin("hercules.widescreen", b->loop, BNEZ_LOOP, on_backdrop);
    }
    for (uint32_t i = 0; i < PANORAMA_COUNT; i++)
        (void)psx_mod_register_function_entry_plugin("hercules.widescreen", k_panoramas[i].func,
                                                     on_panorama);
}
