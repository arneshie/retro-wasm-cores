# retro-cores — libretro cores as WebAssembly, behind one tiny JS API

Builds open-source emulator cores (NES: **FCEUmm**, SNES: **Snes9x**) to WebAssembly with Emscripten and links each
with one shared C "frontend" (`host/libretro_host.c`), so every core exposes the same flat API to JavaScript.
`js/retro-core.js` wraps that API (video as RGBA, audio as int16 stereo, input mask, save states, battery RAM).

```
host/libretro_host.c   libretro frontend -> exported host_* functions      (MIT)
js/retro-core.js       RetroCore: loadGame / runFrame / frame / takeAudio / setInput / saveState ...  (MIT)
build.sh               clones nothing; builds src/<core> then links dist/<core>.js + .wasm
test/run-core.mjs      headless Node smoke test (boots a ROM, checks picture/audio/state, writes PNGs)
```

## Build
```
git clone --depth 1 https://github.com/libretro/libretro-fceumm src/libretro-fceumm   # ac0d00a (2026-10-11)
git clone --depth 1 https://github.com/libretro/snes9x          src/snes9x            # fae2fea (2026-09-19)
./build.sh all          # needs emsdk (EMSDK_ENV=/path/to/emsdk_env.sh), tested with Emscripten 4.0.8
node test/run-core.mjs "$PWD/dist/fceumm.js" createFceummCore path/to/game.nes build/nes 240
```
Adding another libretro core (Genesis Plus GX, PicoDrive, ...) is one more `build_<core>()` in `build.sh`.

## Licences — read before you redistribute the .wasm files
- This repo's own code (host, wrapper, scripts): MIT.
- **FCEUmm: GPL-2.0.** A built `fceumm.wasm` is a combined work under the GPL, so anyone you give it to (including website
  visitors) is entitled to the corresponding source: this repo + the FCEUmm commit above.
- **Snes9x: its own licence — free for non-commercial use only, commercial use needs the authors' permission.** Fine for a
  personal portfolio; not OSI "open source". Swap in bsnes (GPL-3.0) if you need a fully open licence (much heavier in WASM).
- No ROMs are included or distributed. `testroms/` is for local testing only (nestest by kevtris; jsmolka/gba-tests is MIT).
