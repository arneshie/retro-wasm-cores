/*
 * libretro_host.c - a tiny libretro "frontend" compiled to WebAssembly together with a core.
 *
 * Every libretro core (fceumm, snes9x, ...) is linked with this one file, so each resulting
 * .wasm exposes the SAME flat C API to JavaScript (the host_* functions below):
 *
 *   host_init()                      retro_init + callbacks
 *   host_load(path, data, size)      load a ROM that JS already wrote into the heap / MEMFS
 *   host_run()                       run exactly one frame
 *   host_video_ptr/w/h               latest frame as RGBA8888 (ready for canvas ImageData)
 *   host_audio_ptr/frames/clear      interleaved int16 stereo produced since the last clear
 *   host_set_input(port, mask)       16-bit joypad mask, bit n = RETRO_DEVICE_ID_JOYPAD_* n
 *   host_fps / host_sample_rate      timing from retro_get_system_av_info
 *   host_state_size/save/load        save states
 *   host_sram_size/ptr               battery RAM (so saves can be persisted by the page)
 *
 * License: MIT for this file. The cores it links against keep their own licences
 * (fceumm: GPL-2.0, snes9x: Snes9x non-commercial licence).
 */
#include <emscripten.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libretro.h"

#define MAX_W 640
#define MAX_H 480
#define AUDIO_CAP (48000 * 2 * 2)          /* up to ~2 s of stereo int16 */

static uint32_t video_rgba[MAX_W * MAX_H];
static unsigned video_w = 256, video_h = 224;
static enum retro_pixel_format pix_fmt = RETRO_PIXEL_FORMAT_0RGB1555;

static int16_t audio_buf[AUDIO_CAP];
static unsigned audio_frames = 0;

static uint16_t pad[2];
static double fps = 60.0, sample_rate = 44100.0;
static unsigned core_w = 256, core_h = 224;
static int loaded = 0;
static void *state_buf = NULL;
static size_t state_len = 0;

/* ---------- callbacks ---------- */
static void log_cb(enum retro_log_level level, const char *fmt, ...) {
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  if (level >= RETRO_LOG_WARN) fprintf(stderr, "[core] %s", line);
}

static bool environment_cb(unsigned cmd, void *data) {
  switch (cmd) {
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
      *(bool *)data = true;
      return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      pix_fmt = *(enum retro_pixel_format *)data;
      return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
      *(const char **)data = "/";
      return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
      ((struct retro_log_callback *)data)->log = log_cb;
      return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
      *(unsigned *)data = 0;
      return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:
      ((struct retro_variable *)data)->value = NULL;   /* use core defaults */
      return false;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
      *(bool *)data = false;
      return true;
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
      *(int *)data = 3;                                  /* audio + video on */
      return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
      *(unsigned *)data = RETRO_LANGUAGE_ENGLISH;
      return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
    case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
    case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
      return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY: {
      const struct retro_game_geometry *g = data;
      core_w = g->base_width;
      core_h = g->base_height;
      return true;
    }
    default:
      return false;
  }
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch) {
  if (!data) return;                                   /* frame dupe: keep last picture */
  if (w > MAX_W) w = MAX_W;
  if (h > MAX_H) h = MAX_H;
  video_w = w;
  video_h = h;
  for (unsigned y = 0; y < h; y++) {
    uint32_t *dst = video_rgba + y * w;
    if (pix_fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
      const uint32_t *src = (const uint32_t *)((const uint8_t *)data + y * pitch);
      for (unsigned x = 0; x < w; x++) {
        uint32_t p = src[x];                           /* 0x00RRGGBB -> bytes R,G,B,A (little endian) */
        dst[x] = 0xFF000000u | ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF);
      }
    } else if (pix_fmt == RETRO_PIXEL_FORMAT_RGB565) {
      const uint16_t *src = (const uint16_t *)((const uint8_t *)data + y * pitch);
      for (unsigned x = 0; x < w; x++) {
        uint32_t p = src[x];
        uint32_t r = ((p >> 11) & 31) * 255 / 31, g = ((p >> 5) & 63) * 255 / 63, b = (p & 31) * 255 / 31;
        dst[x] = 0xFF000000u | (b << 16) | (g << 8) | r;
      }
    } else {                                           /* 0RGB1555 */
      const uint16_t *src = (const uint16_t *)((const uint8_t *)data + y * pitch);
      for (unsigned x = 0; x < w; x++) {
        uint32_t p = src[x];
        uint32_t r = ((p >> 10) & 31) * 255 / 31, g = ((p >> 5) & 31) * 255 / 31, b = (p & 31) * 255 / 31;
        dst[x] = 0xFF000000u | (b << 16) | (g << 8) | r;
      }
    }
  }
}

static void audio_sample_cb(int16_t l, int16_t r) {
  if (audio_frames * 2 + 2 > AUDIO_CAP) return;
  audio_buf[audio_frames * 2] = l;
  audio_buf[audio_frames * 2 + 1] = r;
  audio_frames++;
}

static size_t audio_batch_cb(const int16_t *data, size_t frames) {
  if ((audio_frames + frames) * 2 > AUDIO_CAP) return frames;      /* drop if the page isn't draining */
  memcpy(audio_buf + audio_frames * 2, data, frames * 2 * sizeof(int16_t));
  audio_frames += frames;
  return frames;
}

static void input_poll_cb(void) {}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
  if (port > 1 || device != RETRO_DEVICE_JOYPAD || index != 0) return 0;
  if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)pad[port];
  return (pad[port] >> id) & 1;
}

/* ---------- exported API ---------- */
EMSCRIPTEN_KEEPALIVE void host_init(void) {
  retro_set_environment(environment_cb);
  retro_init();
  retro_set_video_refresh(video_cb);
  retro_set_audio_sample(audio_sample_cb);
  retro_set_audio_sample_batch(audio_batch_cb);
  retro_set_input_poll(input_poll_cb);
  retro_set_input_state(input_state_cb);
}

EMSCRIPTEN_KEEPALIVE int host_need_fullpath(void) {
  struct retro_system_info si;
  memset(&si, 0, sizeof si);
  retro_get_system_info(&si);
  return si.need_fullpath ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE const char *host_core_name(void) {
  struct retro_system_info si;
  memset(&si, 0, sizeof si);
  retro_get_system_info(&si);
  return si.library_name;
}

EMSCRIPTEN_KEEPALIVE const char *host_core_version(void) {
  struct retro_system_info si;
  memset(&si, 0, sizeof si);
  retro_get_system_info(&si);
  return si.library_version;
}

EMSCRIPTEN_KEEPALIVE int host_load(const char *path, const void *data, size_t size) {
  struct retro_game_info gi;
  memset(&gi, 0, sizeof gi);
  gi.path = path;
  gi.data = data;
  gi.size = size;
  if (!retro_load_game(&gi)) return 0;
  struct retro_system_av_info av;
  memset(&av, 0, sizeof av);
  retro_get_system_av_info(&av);
  fps = av.timing.fps > 1 ? av.timing.fps : 60.0;
  sample_rate = av.timing.sample_rate > 1 ? av.timing.sample_rate : 44100.0;
  core_w = av.geometry.base_width;
  core_h = av.geometry.base_height;
  loaded = 1;
  audio_frames = 0;
  return 1;
}

EMSCRIPTEN_KEEPALIVE void host_run(void) {
  if (loaded) retro_run();
}

EMSCRIPTEN_KEEPALIVE void host_reset(void) {
  if (loaded) retro_reset();
}

EMSCRIPTEN_KEEPALIVE void host_unload(void) {
  if (loaded) retro_unload_game();
  loaded = 0;
  audio_frames = 0;
}

EMSCRIPTEN_KEEPALIVE const uint32_t *host_video_ptr(void) { return video_rgba; }
EMSCRIPTEN_KEEPALIVE unsigned host_video_w(void) { return video_w; }
EMSCRIPTEN_KEEPALIVE unsigned host_video_h(void) { return video_h; }
EMSCRIPTEN_KEEPALIVE unsigned host_core_w(void) { return core_w; }
EMSCRIPTEN_KEEPALIVE unsigned host_core_h(void) { return core_h; }
EMSCRIPTEN_KEEPALIVE double host_fps(void) { return fps; }
EMSCRIPTEN_KEEPALIVE double host_sample_rate(void) { return sample_rate; }

EMSCRIPTEN_KEEPALIVE const int16_t *host_audio_ptr(void) { return audio_buf; }
EMSCRIPTEN_KEEPALIVE unsigned host_audio_frames(void) { return audio_frames; }
EMSCRIPTEN_KEEPALIVE void host_audio_clear(void) { audio_frames = 0; }

EMSCRIPTEN_KEEPALIVE void host_set_input(unsigned port, unsigned mask) {
  if (port < 2) pad[port] = (uint16_t)mask;
}

EMSCRIPTEN_KEEPALIVE size_t host_state_size(void) { return loaded ? retro_serialize_size() : 0; }

EMSCRIPTEN_KEEPALIVE void *host_state_save(void) {
  size_t n = retro_serialize_size();
  if (!n) return NULL;
  free(state_buf);
  state_buf = malloc(n);
  state_len = n;
  return retro_serialize(state_buf, n) ? state_buf : NULL;
}

EMSCRIPTEN_KEEPALIVE int host_state_load(const void *data, size_t size) {
  return retro_unserialize(data, size) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE size_t host_sram_size(void) { return retro_get_memory_size(RETRO_MEMORY_SAVE_RAM); }
EMSCRIPTEN_KEEPALIVE void *host_sram_ptr(void) { return retro_get_memory_data(RETRO_MEMORY_SAVE_RAM); }
