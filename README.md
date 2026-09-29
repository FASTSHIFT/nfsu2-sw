# Need for Speed: Underground 2 — Xbox static recompilation

The Xbox (NTSC-U) release of NFSU2, lifted to C with
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp) and built for Linux and
Nintendo Switch homebrew (libnx NRO).

No game data is included. You need your own copy of the disc, extracted
(`default.xbe`, `NFSUNDER/`, ...).

## Layout

| Path | What |
|---|---|
| `src/main.c` | boot and runtime defaults |
| `src/recomp_manual.c` | hand-written overrides of lifted functions |
| `src/switch_nx.c` | Switch log device, env file, exception handler, loading screen |
| `config/seed_functions.json` | entry points the static pass cannot see |
| `xboxrecomp/` | the toolkit (MIT), vendored with this port's changes: NV2A OpenGL renderer, SDL audio, Switch platform layer, translator fixes |
| `tools/regen.sh` | XBE → lifted C (`gen/`, never committed) |
| `switch/build.sh` | Switch NRO build + SD-card staging |

## Build

```sh
# 1. Lift the XBE to C (writes NFSU2_GEN_DIR, default /root/nfsu2x/gen)
NFSU2_XBE=/path/to/game/default.xbe NFSU2_GEN_DIR=/path/to/gen tools/regen.sh

# 2a. Linux (SDL2 + OpenGL)
cmake -S . -B build -G Ninja -DNFSU2_GEN_DIR=/path/to/gen
cmake --build build
NFSU2_GAME_DIR=/path/to/game build/nfsu2_recomp

# 2b. Nintendo Switch (devkitA64, switch-sdl2, switch-mesa)
NFSU2_GEN_DIR=/path/to/gen NFSU2_GAME_SRC=/path/to/game switch/build.sh
```

On the Switch the NRO reads the **extracted** disc from
`sdmc:/switch/nfsu2x/game/`. Launch it with title takeover (hold R on a game)
for full memory. Runtime switches go in `sdmc:/switch/nfsu2x/nfsu2x_env.txt`
(`KEY=VALUE` per line), and the log is written to `sdmc:/switch/nfsu2x/`.

## Status

- Linux: boot, movies, profile, Main Menu, Quick Race and Career, with audio.
- Switch hardware: boot, movies, profile load/create, Main Menu.

`CLAUDE.md` holds the detailed engineering notes.
