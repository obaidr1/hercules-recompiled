# Hercules game target, shared by every PAL version's CMakeLists.txt.

# --- Opt-in product surfaces (OFF by default in the framework) -------------
# Scaffold prompts / flags fill these. Without recomp-ui, PSX_RECOMP_UI is OFF.
# The wizard (WIZARD_CMAKE_BLOCK / ENABLE_SETUP_WIZARD) is the first-run disc
# picker; releases are compiled from the committed generated/ C, so it never
# needs to generate or rebuild anything on a player's machine.
set(PSX_RECOMP_UI OFF CACHE BOOL
    "No recomp-ui submodule in this scaffold" FORCE)
# Full netplay UI — enable after adding recomp-ui + testing:
# if(EXISTS "${PSXRECOMP_ROOT}/lib/recomp-net/CMakeLists.txt")
#     set(PSX_NETPLAY ON CACHE BOOL "…" FORCE)
# endif()
# First-run setup wizard — enable after testing:
# set(PSX_SETUP_WIZARD ON CACHE BOOL "…" FORCE)
include("${PSXRECOMP_ROOT}/runtime/runtime.cmake")

# --- PAL version --------------------------------------------------------
# The PAL discs are the same game built per language (hercules
# docs/pal_versions.md). One build = one version.
# HERCULES_VERSION and HERC_ROOT come from the including CMakeLists.txt: the
# project root's (German) or versions/<ID>/CMakeLists.txt. The game directory
# (game.toml, aot/, generated/) is always that CMakeLists' own directory --
# the framework reads <source dir>/aot/overlays.json.
set(HERC_VERSION_DIR "${HERC_ROOT}/versions/${HERCULES_VERSION}")
set(HERC_GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if(NOT EXISTS "${HERC_VERSION_DIR}/hercules_addrs.h")
    message(FATAL_ERROR "No ${HERC_VERSION_DIR}/hercules_addrs.h: run hercules/tools/port_version.py")
endif()
string(REGEX REPLACE "^SCES-(...)(..)$" "SCES_\\1.\\2" HERC_BOOT_EXE "${HERCULES_VERSION}")
message(STATUS "Hercules: building ${HERCULES_VERSION} (${HERC_BOOT_EXE}) from ${HERC_GAME_DIR}")

# This project's own mod catalog. The FRAMEWORK stages it -- together with its
# builtin psx.* packages -- into <exe-dir>/mods/bundled, and refuses to
# configure if a catalog exists here but is not declared. Never copy it by
# hand: five titles did, and framework 4cc04be3's rename of the staged
# directory broke all five silently (bead beads-eio.3.101).
set(PSX_PRELOADED_MODS "${HERC_ROOT}/mods/preloaded")
if(NOT IS_DIRECTORY "${PSX_PRELOADED_MODS}")
    set(PSX_PRELOADED_MODS "NONE")
endif()

psxrecomp_add_game_runtime(psx-runtime
    PRELOADED_MODS_DIR "${PSX_PRELOADED_MODS}"
    # ENGINE1/ENGINE3/TITLE and level overlays, compiled from the original disc
    # (aot/overlays.json, written by hercules/tools/aot_profile.py).
    GAME_OVERLAY_STATIC_C "${HERC_GAME_DIR}/generated/overlays_static.c"
    # ENABLE_NETPLAY_IF_PRESENT
    # NETPLAY_LOBBY_URL "ws://netplay.retcomm.net:8765"
    # ENABLE_SETUP_WIZARD
    WINDOW_TITLE "Hercules Recompiled"
    DEFAULT_GAME_CONFIG_PATH "game.toml"
    MAX_PLAYERS 1
    # LAUNCHER_BOXART "${HERC_ROOT}/launcher_assets/img/boxart.tga"
    APP_ICON "${HERC_ROOT}/assets/psxrecomp.ico"
    # Committed game C (psxrecomp_cli.py generate; git add generated). Release
    # CI configures with -DPSXRECOMP_REQUIRE_GAME_C=ON so a checkout without
    # it fails instead of building a setup host.
    GEN_MARKER "${HERC_GAME_DIR}/generated/${HERC_BOOT_EXE}_dispatch.c"
    GEN_FULL_GLOB "${HERC_GAME_DIR}/generated/${HERC_BOOT_EXE}_full_*.c"
    # CODEGEN_SETUP_SOURCES — add with recomp-ui / wizard later
)

# Stage runtime configs next to the exe for direct build/ launches. Only copy
# files that exist — never fail POST_BUILD when game_options.toml is absent.
foreach(_PSX_RUNTIME_CONFIG game.toml game_options.toml)
    set(_herc_cfg "${HERC_GAME_DIR}/${_PSX_RUNTIME_CONFIG}")
    if(NOT EXISTS "${_herc_cfg}")
        set(_herc_cfg "${HERC_ROOT}/${_PSX_RUNTIME_CONFIG}")
    endif()
    if(EXISTS "${_herc_cfg}")
        add_custom_command(TARGET psx-runtime POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_herc_cfg}"
                "$<TARGET_FILE_DIR:psx-runtime>/${_PSX_RUNTIME_CONFIG}"
            COMMENT "Staging ${_PSX_RUNTIME_CONFIG}"
            VERBATIM)
    endif()
endforeach()

# Game-owned trusted plugins (activated by mods/preloaded manifests).
target_sources(psx-runtime PRIVATE
    "${HERC_ROOT}/mods/src/hercules_hd_sprites.c"
    "${HERC_ROOT}/mods/src/hercules_level_select.c"
    "${HERC_ROOT}/mods/src/hercules_skip_intro.c"
    "${HERC_ROOT}/mods/src/hercules_widescreen.c"
    "${HERC_ROOT}/mods/src/hercules_frame_blend.c"
    "${HERC_ROOT}/mods/src/hercules_ntsc_speed.c"
    "${HERC_ROOT}/mods/src/hercules_events.c")
target_include_directories(psx-runtime PRIVATE "${HERC_VERSION_DIR}")

# The launcher, compiled into the game executable (psx_game_launcher.h): a
# menu in the style of the 1997 title screens, its art decoded from the
# player's disc at start-up. --no-launcher (and the launcher's own relaunch)
# skips it.
target_sources(psx-runtime PRIVATE
    "${HERC_ROOT}/launcher_native/hl_data.cpp"
    "${HERC_ROOT}/launcher_native/hl_launcher.cpp")
target_include_directories(psx-runtime PRIVATE "${HERC_ROOT}/launcher_native")
# The host's toasts ("Scanlines on", fast forward ...), which the framework
# only shows with recomp-ui. The in-game menu itself is in hl_launcher.cpp.
target_compile_definitions(psx-runtime PRIVATE PSX_HOST_OSD_VISUAL=1)
# the launcher picks the build for the player's disc release (launcher_native/hl_launcher.cpp)
target_compile_definitions(psx-runtime PRIVATE HERCULES_GAME_ID="${HERCULES_VERSION}")
