# Sourced by tools/pc/*.sh. Paths and the PC (x86 Linux) run environment.
#   ROOT   repo root;   BUILD  build/ (binary in build/linux, data in build/game)
#   PC_ENV extra env for the game on this host: under VMware, llvmpipe and the
#          spin throttles (docs/06). PC_GPU=1 keeps the host GL driver.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$ROOT/build"
BIN="$BUILD/linux/nfsu2_recomp"
GAME_ENV=(NFSU2_GAME_DIR="$BUILD/game" NFSU2_SAVE_DIR="$BUILD/save-linux" RECOMP_FPS_LOG=1)
PC_ENV=()
if [ "${PC_GPU:-0}" != 1 ] && [ "$(systemd-detect-virt 2>/dev/null)" = vmware ]; then
    PC_ENV=(LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe LP_NUM_THREADS="${LP_NUM_THREADS:-8}"
            RECOMP_KNOB_SPIN_WAKE_EVERY=64 RECOMP_KNOB_SPIN_YIELD_IDLE=1)
fi
# Start -> menus -> a race with the stock save (times from the first pad read).
# Skip the skippable boot screens (PSA, IntroFMV, Splash) with early A presses,
# then drive the menus. With RECOMP_SKIP_INTRO=1 the EA logo + THX are already
# gone (docs/06): the boot flow is LangSelect -> PSA -> IntroFMV -> Splash ->
# MC screens -> UI_Main. A skips the movies; Start confirms the menu.
MENU_PAD="3000:a:200,5000:a:200,7000:a:200,9000:a:200,11000:a:200,13000:a:200,15000:start:300,25000:start:300,35000:start:300,45000:a:200,55000:a:200,65000:start:300,75000:a:200,85000:a:200,95000:a:200"
game_pid() { pgrep -f "linux/nfsu2_recomp$" | head -1; }
