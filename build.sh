#!/usr/bin/env bash
# Build libretro cores to WebAssembly. Usage: ./build.sh [fceumm|snes9x|all]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
if ! command -v emcc >/dev/null 2>&1; then
  : "${EMSDK_ENV:?emcc not on PATH - set EMSDK_ENV=/path/to/emsdk/emsdk_env.sh}"
  source "$EMSDK_ENV" >/dev/null 2>&1
fi
OUT="$HERE/dist"; mkdir -p "$OUT"
JOBS="$(nproc)"

EXPORTS='_malloc,_free,_host_init,_host_need_fullpath,_host_core_name,_host_core_version,_host_load,_host_run,_host_reset,_host_unload,_host_video_ptr,_host_video_w,_host_video_h,_host_core_w,_host_core_h,_host_fps,_host_sample_rate,_host_audio_ptr,_host_audio_frames,_host_audio_clear,_host_set_input,_host_state_size,_host_state_save,_host_state_load,_host_sram_size,_host_sram_ptr'
RUNTIME='HEAPU8,FS,UTF8ToString,stringToUTF8,lengthBytesUTF8'

link() {   # link <name> <export-name> <core.bc> <include-dir> <compiler>
  local name=$1 export=$2 core=$3 inc=$4 cc=$5
  echo ">> linking $name"
  cp "$core" "$HERE/build/$name.a"; core="$HERE/build/$name.a"   # libretro names its ar archive *.bc
  $cc -O3 -I"$inc" -c "$HERE/host/libretro_host.c" -o "$HERE/build/host_$name.o"
  # libretro-common helpers that cores expect the *frontend* to provide when statically linked
  local lc="$inc/.."; [ -d "$lc/compat" ] || lc="$inc/libretro-common"
  local objs=()
  for f in compat/compat_strl.c file/file_path.c file/file_path_io.c streams/file_stream.c streams/file_stream_transforms.c vfs/vfs_implementation.c string/stdstring.c encodings/encoding_utf.c encodings/encoding_deflate.c streams/trans_stream.c streams/trans_stream_pipe.c streams/trans_stream_deflate.c time/rtime.c compat/fopen_utf8.c; do
    [ -f "$lc/$f" ] || continue
    o="$HERE/build/lc_${name}_$(echo "$f" | tr '/' '_').o"
    $cc -O2 -I"$inc" -I"$lc/include" -DHAVE_FILE_STREAM_STDIO -c "$lc/$f" -o "$o" && objs+=("$o")
  done
  em++ -O3 "$HERE/build/host_$name.o" "${objs[@]}" "$core" -o "$OUT/$name.js" \
    -sMODULARIZE=1 -sEXPORT_NAME="$export" -sENVIRONMENT=web,worker,node \
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=67108864 -sSTACK_SIZE=1048576 \
    -sEXPORTED_FUNCTIONS="$EXPORTS" -sEXPORTED_RUNTIME_METHODS="$RUNTIME" \
    -sFILESYSTEM=1 -sEXIT_RUNTIME=0 -sASSERTIONS=0
  ls -la "$OUT/$name".{js,wasm}
}

mkdir -p "$HERE/build"
build_fceumm() {
  cd "$HERE/src/libretro-fceumm"
  emmake make -f Makefile.libretro platform=emscripten -j"$JOBS" >"$HERE/build/fceumm.log" 2>&1
  link fceumm createFceummCore "$PWD/fceumm_libretro_emscripten.bc" "$PWD/src/drivers/libretro/libretro-common/include" emcc
}
build_snes9x() {
  cd "$HERE/src/snes9x/libretro"
  emmake make platform=emscripten -j"$JOBS" >"$HERE/build/snes9x.log" 2>&1
  link snes9x createSnes9xCore "$PWD/snes9x_libretro_emscripten.bc" "$PWD" emcc
}
case "${1:-all}" in
  fceumm) build_fceumm ;;
  snes9x) build_snes9x ;;
  all) build_fceumm; build_snes9x ;;
esac
