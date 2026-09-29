# NFSU2 (Xbox) static recompilation — notes for Claude

Xbox NTSC-U Need for Speed: Underground 2, lifted to C with xboxrecomp and
built for Linux and Nintendo Switch (libnx NRO). This repo is
https://github.com/antoxa2584x/nfsu2-sw (`main`, commits as
`Anton Artemov <antoxa2584@gmail.com>`). On 2026-09-29 it replaced the old PS2
port there (history backed up in `/root/nfsu2x/nfsu2-sw-ps2-backup.bundle`).
The toolkit is vendored in `xboxrecomp/`; the default for `XBOXRECOMP_DIR`.

## Rules

- Never commit game data (disc, `default.xbe`, `switch_sd/`) or generated C
  (`gen/`). Ask before committing or pushing anything.
- The Switch build reads the **unpacked** disc at `sdmc:/switch/nfsu2x/game/`,
  never the ISO.
- Don't launch or kill Eden unless the user asked for an Eden check; check
  `tasklist.exe | grep -i eden` first. Eden's `sdmc\switch` is a junction to
  `nfsu2-xbox\switch_sd\switch`, so an Eden run overwrites the log
  there — back up a hardware log before running Eden. The user drops real
  console logs in `switch_sd/switch/logs/`.
- One test at a time on Linux (runs share `fb/`, `gfb/`).
- Never `pkill -f` a pattern that also matches your own command line (it
  kills the shell, and `run_eden.sh` then force-closes Eden).
- Clean up every Linux test run. `pkill -x nfsu2_recomp` does NOT match (the
  process renames itself), and `timeout`/`xvfb-run` leave orphans that keep
  burning CPU; kill by PID:
  `ps -eo pid,args | awk '$2 ~ /nfsu2_recomp$/ || $2 ~ /^Xvfb$/ {print $1}' | xargs -r kill`

## Layout and builds

| Where | What |
|---|---|
| `/root/nfsu2x/` (WSL) | `game/` extracted disc, `gen/` lifted C, `build-pr128/` Linux, `build-switch/` Switch, `dis.py ADDR [+N]`, `snap.sh SECS ENV=..` (gdb stacks, `BIN=`), `run_eden.sh SECS` |
| `xboxrecomp/` | vendored toolkit = xboxrecomp main + PR #128 + all port changes (copied from the `/root/nfsu2x/xboxrecomp-pr128` worktree; keep the two in sync) |
| `src/main.c` | boot; defaults RECOMP_VBLANK=1, RECOMP_AC97_READY=plain, RECOMP_USB=1, RECOMP_PB_EXEC=1; APU at 0xFE800000 via `xbox_MmioRegister`; Switch runs the game on a 16 MB pthread |
| `src/recomp_manual.c` | memmove ×2 (0x2A7EE0, 0x2A9450), AC97 reset 0x33518D, DSP ack wrapper 0x32EB65, D3D fence wrapper 0x2E8F20 |
| `src/switch_nx.c` | log, `nfsu2x_env.txt`, exception handler, t= stamps |

- Regenerate C: `tools/regen.sh` (`LIFT_ONLY=1` after manual-override edits;
  full run after seed changes). Passes `--mmio-sections DSOUND,XPP`.
- Switch: `XBOXRECOMP_DIR=/root/nfsu2x/xboxrecomp-pr128 bash switch/build.sh`
  (without `XBOXRECOMP_DIR` it picks the main checkout and fails on OpenSSL).
  The copy step fails with "Permission denied" while Eden has the NRO open.
- Linux: `cmake --build /root/nfsu2x/build-pr128 -j8`; run with
  `NFSU2_GAME_DIR=/root/nfsu2x/game xvfb-run -a …/nfsu2_recomp`.
- Menu pad script (Linux): `RECOMP_PAD_SCRIPT="10000:start:300,…,70000:start:300,80000:a:200,90000:a:200"`
  (times from the first pad read). `RECOMP_GL_DUMP=<prefix>,N` dumps frames.
- Header changes in `templates/runtime/recomp_types.h` must be copied to
  `gen/recomp_types.h` (regen does it) and rebuild all generated code.

## Switch (Horizon) findings

- **Memory:** `svcCreateSharedMemory` → 0x4201 in Eden and may kill the
  process on hardware; `svcMapPhysicalMemory` → 0xFA01. Guest RAM uses code
  memory (`svcCreateCodeMemory` + `svcControlCodeMemory(MapOwner)`) — the
  default; `RECOMP_NX_SHM=1` opts into shared memory. Code memory cannot
  alias, and the title needs aliasing: it reaches RAM at 0x0 and at the
  0x80000000 contiguous window, and on hardware read a stale 0xAAAAAAAA
  pointer through 0x80xxxxxx after Start (crash, exception 257). Second
  views of a mapping now try `svcMapProcessMemory` on our own process
  (log: `[NX] aliasing views with svcMapProcessMemory` or `... refused
  (0xRC)`). If that is refused, the fallback is folding 0x80000000–0x83FFFFFF
  onto low RAM in `XBOX_PTR` *and* the runtime's translations.
- **Logging / boot time:** the SD log was the boot bottleneck — the runtime
  `fflush(stderr)`s after many lines and each flush was an SD write (console:
  ~33 s to display mode; Linux 0.3 s; Eden 5 s). Now stdout+stderr go to a
  `log:` devoptab (switch_nx.c) that appends to RAM with a `[  t.ttt]`
  timestamp per line; a thread writes it to the card every 0.5 s (crash
  handler drains it). Eden boot to display mode dropped 5 s → 2 s.
- **Loading screen:** NFSU2 logo (`assets/nfsu2_logo.png`, SteamGridDB →
  `tools/make_logo.py` → `src/nfsu2_logo.h`, RLE) + a moving bar, drawn on the
  SDL window/GL context that the renderer then adopts
  (`nv2a_gl_adopt_window`), and kept on presents until the title's first real
  draw (`nv2a_gl_draw_placeholder`, `s_drew_any`). A libnx framebuffer can't be
  used: `SDL_CreateWindow` hangs forever after one was open. `NFSU2_LOADER=0`
  disables it.
- **Unaligned atomics:** x86 `lock cmpxchg`/`xadd` on unaligned addresses are
  legal; AArch64 atomics fault (exception 259 = 0x103 unaligned data). NFSU2's
  XNet `sub_0030A496` ORs a field at …306 after Start. `RECOMP_ATOMIC_*` on
  aarch64 fall back to a spinlock for misaligned addresses. Eden does not model
  the fault — only hardware shows it.
- **Threads/cores:** libnx creates every pthread at priority 59 (the only
  priority Horizon time-slices on cores 0-2) with the process core mask, but
  preferred core = default core 0. `xbox_nx_spread_thread()` deals preferred
  cores 0-2 round-robin (log: `[NX] threads spread over core mask 0x7`).
  Never put threads on core 3: Eden reports mask 0xF, and core 3 does not
  time-slice 59 — busy threads there starved each other (log stopped at 40 s).
- **Hardware boot hang after GPU trap 8** (`NOP(1825046561)`, ~33 s): main
  guest thread (stack 0x00F7Fxxx) stops making kernel calls, GPU idle, never
  reaches D3D's `in al,dx` / `SetDisplayMode`; one run in two. Cause not yet
  found. `nfsu2x_env.txt` with `RECOMP_WATCHDOG_SECS=50` +
  `RECOMP_WATCHDOG_KEEP=1` snapshots the main thread without exiting.
- **Guest threads race (the crash/freeze after Start):** NFSU2's stream
  system hands a freshly allocated block (allocator fill 0xAA) to its worker
  before filling it in; parallel guest threads read 0xAAAAAAAA as a pointer
  (`sub_00072F90`, `sub_0025337B`). Saves live in `game/UDATA`, not `save/`
  (`partition1\UDATA` → game dir). Fix: **the guest lock** (kernel_bridge.c,
  `RECOMP_GIL=0` disables) — one thread runs lifted code at a time; released
  in every kernel call, every 64 poll-loop turns (`recomp_spin_yield`), and at
  lifted function entry when someone waited >1 ms (`RECOMP_PREEMPT`, emitted by
  the translator, skipped in ISR/DPC and at IRQL ≥ DISPATCH). Pinning guest
  threads to one core (`RECOMP_GUEST_ONE_CORE=1`) also fixes it but Horizon's
  10 ms slices cost ~90% speed. Console profile for tests: copied over MTP
  (PowerShell Shell.Application, "Цей ПК\Nintendo Switch\microSD card") into
  `/root/nfsu2x/game/UDATA/4541005a/005413381036`.
- **Atmosphère fatal `std::abort()` in program 010041544D530000 (ams.mitm)**
  and whole-console freezes after save checks: leaked directory handles.
  `NtQueryDirectoryFile` kept a `DIR*` per handle and only closed it when a
  scan reached the end; NFSU2 stops early and closes the handle. Each leak is
  an SD session in ams.mitm, which aborts after enough. Fixed:
  `xbox_dir_forget()` on NtClose (kernel_file.c / bridge_NtClose). Check on
  Linux: `ls -l /proc/<pid>/fd | grep UDATA` stays empty.
- **Hang starting Quick Race / Career** (after the transmission pick or the
  career intro): `PersistDisplay` (0x2EA9C0) waits for D3D's pending flips,
  and `BlockOnTime` (0x2E8F20) waits for a `NOP(5)` trap event. Fixed chain:
  DPC queue locked + `KDPC.Inserted` (ISRs queue from several threads);
  vblank bits held until D3D's DPC reads them; the PGRAPH (0x2F22F0) and
  vblank (0x2F1D80) handlers wrapped in recomp_manual.c at DISPATCH, taking
  traps through a lock handshake with the executor and reporting retired
  flips; `FLIP_STALL` waits for a retire (read != write); `DMA_GET` is
  published as the executor walks. Frame rate is now vblank-locked.
- **Red races / black loading screen** (renderer, nv2a_gl): the race colour
  grade samples two LUTs with DEPENDENT_AR / DEPENDENT_GB texture modes
  (implemented in gl_psh.c, source stage from SHADER_OTHER_STAGE_INPUT), and
  the final combiner's C0/C1 are SPECULAR_FOG_FACTOR0/1, not stage 0's. The
  loading screen's quads sit at z = 1.0 and GL clipped them: GL_DEPTH_CLAMP.
  Headless Linux tests: `SDL_VIDEODRIVER=offscreen` (no Xvfb) works.
- **Files:** no `open()` on directories (`XBOX_DIR_FD` sentinel); FAT can't
  hold sparse files (partition images created empty, size reported).
- **Save load/create froze the whole console** (log stops right after
  `SaveMeta.xbx` opens; Eden/Linux fine): read()/write() handed guest RAM
  (code memory) to the fs service over IPC; the save code's small unaligned
  reads (2, 280 bytes) jammed it. Fixed (confirmed on hardware 2026-09-28):
  on `__SWITCH__` NtReadFile/NtWriteFile go through a .bss bounce buffer
  (`host_read`/`host_write`, kernel_file.c). Never pass guest memory to a
  Horizon IPC buffer.
- **USB:** the title starts its USB driver late on hardware (25–130 s); the
  OHCI thread must never give up waiting (it used to stop at 30 s → no pad).
  `OHCI_TICK_MS` is 4 ms.
- **Present:** Switch Mesa's scaled, flipped `glBlitFramebuffer` writes outside
  the destination rect (edge columns smeared into the pillarbox bars) → blit
  is scissored to the picture rect. Clear on presents with no surface too.
- **Widescreen (default on, `RECOMP_WIDESCREEN=0` for 4:3):** NFSU2 has a
  real 16:9 mode (anamorphic 640x480, wider FOV, HUD in the 4:3 safe area,
  movies pillarboxed by the game). It needs `XC_VIDEO` in EEPROM layout
  (`XGetVideoFlags` 0x21AEA8 returns `(v >> 16) & 0x5F`, widescreen =
  0x00010000) *and* the same bit in the `AvSendTVEncoderOption(6)` word —
  D3D's mode search (0x2F1B06) fails CreateDevice on a widescreen present
  without it (boot stalls, no window). The presenter then stretches to 16:9.
- **Launch:** title override (hold R on a game) for full memory; applet mode
  has ~400 MB.
- Buttons map by label (Switch A = Xbox A); `RECOMP_PAD_LAYOUT=position`
  swaps to Xbox positions. Y opens the in-game Help box, closed with B.

## Audio (host output)

- Linux/Switch output is SDL2 (`SDL_QueueAudio`) behind the `xa2_*` API
  (POSIX half of apu_xaudio2.c); log `[AUDIO] SDL <driver> output`. The APU
  still paces by wall clock and only feeds the device (waiting on the queue
  crawled under WSLg PulseAudio and slowed boot). `RECOMP_AUDIO=0` off,
  `RECOMP_AUDIO_BLOCKS` queue depth (default 8, Switch 12), `RECOMP_AUDIO_VOLUME`
  0..100. Capture on Linux: `SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE=out.raw`
  (48 kHz s16 stereo, real-time paced); `dummy` for tests without sound.
- APU IRQ 5 was raised on Windows only (`#if _WIN32` in apu_core.c) -> no
  DirectSound voice ever started elsewhere.
- NFSU2 mixes in software (EA engine, thread `sub_00274CA0`) into three 50 ms
  5.1 ring voices (v0F4-F6). Its scheduler sleeps `deadline - KeTickCount`
  (+10 ms a turn). KeTickCount was only written by the NV2A ack thread, which
  blocks in the pushbuffer executor (traps, FLIP_STALL) -> clock froze ~5 s
  after boot, scheduler ran at 2 Hz. Now `tick_count_thread` (1 ms).
- Translator: `lahf` was a comment (fixed: AH from the flags' owner) and a jcc
  after two comiss/ucomiss predecessors fell back to `if (_flags)` (fixed in
  `_merge_flag_states`); tests `tools/recomp/test_flag_sse_compare.py`. EA's
  mixer muted every channel through these. ~1660 other `_flags` fallbacks
  remain in gen/ (je/jo/js...), not audited.
- Debug: `RECOMP_APU_TRACE=1` (FE methods, per-second voice summary). gdb
  hardware watchpoints report only value *changes* -- stamp a nonzero value
  first when the writer stores zeros.

## Performance findings (Switch focus)

- Profile on Linux: `/usr/lib/linux-tools-6.8.0-142/perf record -e cpu-clock -F 499 -p $(pgrep -n -x nfsu2_recomp)`
  (the `/usr/bin/perf` wrapper doesn't work on this WSL kernel; `-e cpu-clock` is required).
- `nv2a_flag_thread` / `nv2a_ack_thread` looped on `Sleep(0)` and burned a core
  each. Now they sleep (≤1 ms) when the pushbuffer is idle and are woken by
  `recomp_spin_wake()`, which `RECOMP_SPIN_HINT` calls. A fixed sleep instead
  of the wake slowed the game (every kickoff waited) — don't go back to that.
- `tools/recomp/spin_hint.py` marks the title's poll loops (45 in NFSU2: D3D
  fence 0x2E9057, PGRAPH polls, DirectSound's APU-clock polls on 0xFE820010)
  with `RECOMP_SPIN_HINT()`: pause, wake hardware threads every 16, yield
  every 64. Poll = single block, back-edge to itself, no stores/calls, fixed
  addresses, no loop-carried registers. Tests: `tools/recomp/test_spin_hint.py`.
- Movie frames are 640x480 linear A8R8G8B8 (fmt 0x12) textures replaced every
  frame; they were decoded per texel through `sample_texture`. The GL
  renderer now uploads 0x12 straight from guest memory (`glTexSubImage2D`,
  `GL_UNPACK_ROW_LENGTH`). `RECOMP_TEX_STATS=1` lists decoded formats/sizes.
- Movies are paced by DirectSound's play cursor = the APU clock. With no
  host audio (Switch, Linux) `throttle()` in apu_core.c paces by wall clock;
  it used to reset after any block >5.3 ms late, and Horizon's 10 ms slices
  made the clock lose time. Now late blocks are caught up (`EP_CATCHUP_US`,
  100 ms). Hardware `[perf] APU n frames/s` reads 1500 = real time.
- **Clock overflow:** `qemu_clock_get_us/ns` (apu_shim.h, nv2a/qemu_shim.h)
  did `count * 1e6 / freq`; QPC on POSIX/Switch is ns since *host boot*, so
  it wrapped after 2.6 h of uptime (1e9 variant: 9 s) and APU pacing +
  XGSCNT went to garbage (`APU 0 frames/s`). Now `qemu_qpc_scale()`. Results
  that depend on audio pacing from a long-running console/WSL before this
  fix are suspect.
- Movie decoding runs on the game thread: MMX IDCT `sub_0026EB34`, MC
  `sub_0025ECB4`, `sub_0026FBB1` (Linux perf of the movies). The translator
  keeps registers of MMX *leaf* functions in shadowing C locals
  (`_localize_leaf_registers`, `recomp_leaf_ld_*`/`st_*`; 21 functions,
  `RECOMP_LEAF_LOCALS=0` at regen disables): IDCT ~7.8x, MC ~2.5x on Linux,
  output unchanged (frame dumps).
- Switch log: floats printed from inside the log device's `%f` timestamp
  shared newlib's dtoa buffer, so every `[perf]` fps figure was a copy of its
  timestamp's digits. The timestamp is integer-formatted now.
- Switch defaults: `RECOMP_QUIET=1` (kernel summaries, [READ], DMA_PUT, GPU
  stats each flushed stderr = an SD write); lifted code built `-O2`
  (`NFSU2_GEN_OPT`, others `-O1`); build with `JOBS=6` so -O2 fits in RAM.
- **Races (2026-09-29, Linux):** NFSU2 renders races at 30 fps (every other
  vblank) and issues ~1300-1600 draws a frame (~45k/s): world ~460, two
  reflection passes (320x240, 4x 128x128), post-processing, HUD ~190. The
  executor thread is the likely Switch limit (~10 us per draw on x86). Done:
  only present attributes uploaded (was 256 B/vertex), vertex/index data
  streamed into two orphaned ring buffers (glMapBufferRange unsynchronized),
  render state / program / uniforms / 192 VS constants / sampler state set only
  on change (`state_dirty()` after clears, presents, new surfaces), D3D's
  vblank handler no longer spins for the ack thread (`xbox_Nv2aVblankTaken`),
  executor waits for traps and flip retires on an event, not Sleep(0).
  Next candidates: merge consecutive draws with identical state, cheaper
  vertex fetch in the executor. Measure fps from memory: D3D flips at
  device block (+0x2F7798 -> +0x1C28) +0x1CC; read /proc/<pid>/mem unbuffered.
- Regen from the worktree needs `tools/{disasm,func_id,abi_analysis}/output`
  — symlinked from the main checkout (don't commit the links).

## Status (2026-09-28)

- Linux: boot → movies → title → profile → Main Menu → Quick Race race and
  Career explore mode, loading screens and race colours correct (2026-09-29).
- Eden: reaches Main Menu.
- Hardware: boot → movies → profile load/create → Main Menu. Movies were
  slow (APU clock losing time + decoder cost); APU clock fixed and confirmed
  at 1500 frames/s, decoder speed-up awaiting a hardware test.
- Audio plays on Linux (2026-09-29); Switch audio awaiting a test.
- Open: cube maps, bump/dot-product texture modes (dependent AR/GB done),
  fixed-function lighting, APU performance on Switch.
