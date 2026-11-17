/* Skip the boot sequence of Hercules (SCES-00893).
 *
 * TITLE runs its screens as tasks in a 16-slot table at D_80065CB8 (0x18
 * bytes each: +0 handler, +4 step, +8 frames until the next call; dispatcher
 * func_80041CF0, spawn func_80041A18). On a cold boot slot 0 runs the intro
 * handler func_8003EF94 through these steps (traced with the debug server):
 *
 *   0x33-0x35  piracy notice          0x36, 0x3D-0x40  "SCEE presents"
 *   0x26/0x27  Disney Interactive FMV 0x25 Eurocom FMV  0x24 Hercules logo FMV
 *   0xE7       spawns the memory card check (func_8004EAA4), waits for it
 *   0xE6       "LADEN" screen, loads the title menu (func_80042E88)
 *
 * The skipped steps only draw pictures and play movies. Feature skip-intro
 * moves the first notice step (0x33, only reached on a cold boot) on to 0xE7;
 * feature skip-memcard moves 0xE7 on to 0xE6, so the card check task is never
 * spawned (the title menu reads the card itself when a game is loaded).
 *
 * Feature direct-start replaces the title menu (the launcher is the menu):
 * With skip-intro off the movies play first and the jump happens at 0xE7.
 * "Spiel starten" ends in step 0x29, which increments the level D_80035048
 * and picks the level's story movie and intro screen from jtbl_80079CB0
 * (level 1 -> step 0x64, 2 -> 0x65, 3 -> 0x66, 4 and 5 -> 0x67 with the level
 * set to 5, 6 -> 0x69 ... 11 -> 0x6E). It is the same path the game takes
 * between levels. On a cold boot the mod sets the level to target - 1 and the
 * first step to 0x29. Level 0 is not in that table (new level 0 goes back to
 * the title); the launcher starts it as level 1 with hercules.level-select.
 *
 * Feature launcher-return leaves for the launcher when the game reaches its
 * title menu (step 0x28) after a level has run: after a game over (traced:
 * 0x8E game over movie, 0xC9 game over screen, 0xE6 "LADEN", 0x28 menu) or
 * the ending. Continuing after a game over goes back to the level, not the
 * menu. A cold boot without direct-start reaches the menu before any level
 * and stays there.
 *
 * Feature skip-fmv switches on the runtime's automatic FMV skip (MDEC decode
 * with an XA stream running = a movie; it holds START, which ends Hercules'
 * movie player). The runtime leaves that setting to a title mod.
 */
#include "mod_plugins.h"
#include "hercules_addrs.h" /* per PAL version, hercules/tools/port_version.py */

#define TITLE_SITE    HERC_TI_DECODE_SITE /* lw $a0, staging($gp) in TITLE */
#define TITLE_WORD    HERC_TI_DECODE_WORD
#define TASK0_HANDLER HERC_TI_TASK0
#define TASK0_STEP    (HERC_TI_TASK0 + 4u)
#define TASK0_WAIT    (HERC_TI_TASK0 + 8u)
#define INTRO_HANDLER HERC_TI_INTRO_HANDLER
#define STEP_NOTICE   0x33u
#define STEP_MEMCARD  0xE7u
#define STEP_LOADING  0xE6u
#define STEP_NEW_GAME 0x29u
#define STEP_MENU     0x28u
#define LEVEL         HERC_LEVEL

#include <stdlib.h>

static int s_skip_intro;
static int s_skip_memcard;
static int s_direct_level; /* 0 = off, else the level a new game starts in */
static int s_launcher_return;
static int s_level_ran;    /* TITLE was not resident at some VBlank: a level ran */

static void on_vblank(void) {
    if (psx_mod_read_word(TITLE_SITE) != TITLE_WORD) { s_level_ran = 1; return; }
    if (psx_mod_read_word(TASK0_HANDLER) != INTRO_HANDLER) return;
    const uint32_t step = psx_mod_read_word(TASK0_STEP);
    if (s_launcher_return && s_level_ran && step == STEP_MENU) {
        s_level_ran = 0;
        if (psx_mod_request_return_to_launcher())
            psx_mod_counter_add("hercules.launcher-return", 1);
        return;
    }
    uint32_t next = step;
    /* direct start: at the first notice, or after the intro movies when they
     * are wanted (skip-intro off) -- step 0xE7 comes right after them */
    if (s_direct_level && step == (s_skip_intro ? STEP_NOTICE : STEP_MEMCARD)) {
        psx_mod_write_byte(LEVEL, (uint8_t)(s_direct_level - 1));
        next = STEP_NEW_GAME;
    } else if (s_skip_intro && step == STEP_NOTICE)
        next = s_skip_memcard ? STEP_LOADING : STEP_MEMCARD;
    else if (s_skip_memcard && step == STEP_MEMCARD)
        next = STEP_LOADING;
    if (next == step) return;
    psx_mod_write_word(TASK0_STEP, next);
    psx_mod_write_word(TASK0_WAIT, 1);
    psx_mod_counter_add(next == STEP_NEW_GAME ? "hercules.skip-intro.direct_start"
                        : next == STEP_LOADING ? "hercules.skip-intro.to_loading"
                                               : "hercules.skip-intro.to_memcard", 1);
}

static void on_activate_intro(void) { s_skip_intro = 1; }
static void on_activate_memcard(void) { s_skip_memcard = 1; }
static void on_activate_return(void) { s_launcher_return = 1; }
static void on_activate_fmv(void) {
    if (!psx_mod_set_auto_skip_fmv(1)) psx_mod_counter_add("hercules.skip-fmv.refused", 1);
}
static void on_activate_direct(void) {
    char value[16] = "";
    s_direct_level = 1;
    if (psx_mod_current_option_value("level", value, sizeof(value))) {
        const int level = atoi(value);
        if (level >= 1 && level <= 11) s_direct_level = level;
    }
}

PSX_MOD_CONSTRUCTOR(hercules_register_skip_intro) {
    (void)psx_mod_register_activation_plugin("hercules.skip-intro", on_activate_intro);
    (void)psx_mod_register_vblank_plugin("hercules.skip-intro", on_vblank);
    (void)psx_mod_register_activation_plugin("hercules.skip-memcard", on_activate_memcard);
    (void)psx_mod_register_vblank_plugin("hercules.skip-memcard", on_vblank);
    (void)psx_mod_register_activation_plugin("hercules.skip-fmv", on_activate_fmv);
    (void)psx_mod_register_activation_plugin("hercules.direct-start", on_activate_direct);
    (void)psx_mod_register_vblank_plugin("hercules.direct-start", on_vblank);
    (void)psx_mod_register_activation_plugin("hercules.launcher-return", on_activate_return);
    (void)psx_mod_register_vblank_plugin("hercules.launcher-return", on_vblank);
}
