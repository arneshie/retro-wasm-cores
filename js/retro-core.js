/**
 * RetroCore - thin JS wrapper around a libretro core compiled to WebAssembly with host/libretro_host.c.
 * Works in browsers and Node. One wrapper, any core (fceumm, snes9x, ...).
 *
 *   const core = await RetroCore.create(createFceummCore);   // factory from fceumm.js
 *   core.loadGame(romBytes, 'game.nes');
 *   core.setInput(0, RetroCore.BUTTON.A | RetroCore.BUTTON.START);
 *   core.runFrame();
 *   core.frame  -> { width, height, data: Uint8ClampedArray(RGBA) }
 *   core.takeAudio() -> Int16Array (interleaved stereo at core.sampleRate)
 */

// RETRO_DEVICE_ID_JOYPAD_* bit positions
const ID = { B: 0, Y: 1, SELECT: 2, START: 3, UP: 4, DOWN: 5, LEFT: 6, RIGHT: 7, A: 8, X: 9, L: 10, R: 11 };
const BUTTON = Object.fromEntries(Object.entries(ID).map(([k, v]) => [k, 1 << v]));

export class RetroCore {
  static ID = ID;
  static BUTTON = BUTTON;

  /** @param {(opts?: object) => Promise<any>} factory  the MODULARIZE factory exported by <core>.js */
  static async create(factory, moduleOpts = {}) {
    const mod = await factory(moduleOpts);
    const core = new RetroCore(mod);
    core.m._host_init();
    return core;
  }

  constructor(mod) {
    this.m = mod;
    this.loaded = false;
    this._romPtr = 0;
    this.fps = 60;
    this.sampleRate = 44100;
  }

  get name() { return this.m.UTF8ToString(this.m._host_core_name()); }
  get version() { return this.m.UTF8ToString(this.m._host_core_version()); }

  /** @param {Uint8Array} bytes @param {string} filename (extension matters to some cores) */
  loadGame(bytes, filename = 'game.bin') {
    const m = this.m;
    this.unload();
    const path = '/' + filename.replace(/[^\w.-]/g, '_');
    try { m.FS.writeFile(path, bytes); } catch { /* cores that take data don't need the file */ }
    this._romPtr = m._malloc(bytes.length);
    m.HEAPU8.set(bytes, this._romPtr);
    const pathLen = m.lengthBytesUTF8(path) + 1;
    const pathPtr = m._malloc(pathLen);
    m.stringToUTF8(path, pathPtr, pathLen);
    const ok = m._host_load(pathPtr, this._romPtr, bytes.length);
    // the core keeps its own copy of the path/data for the lifetime of the game on most cores; keep the ROM
    // buffer alive until unload() and leak the tiny path string rather than risk a dangling pointer.
    if (!ok) { this.unload(); throw new Error(`${this.name} could not load ${filename}`); }
    this.loaded = true;
    this.fps = m._host_fps();
    this.sampleRate = m._host_sample_rate();
    return true;
  }

  runFrame() { if (this.loaded) this.m._host_run(); }
  reset() { if (this.loaded) this.m._host_reset(); }

  unload() {
    if (this.loaded) this.m._host_unload();
    if (this._romPtr) { this.m._free(this._romPtr); this._romPtr = 0; }
    this.loaded = false;
  }

  /** mask: OR of RetroCore.BUTTON.* ; port 0 or 1 */
  setInput(port, mask) { this.m._host_set_input(port, mask); }

  /** Latest picture as RGBA. `data` is a view into WASM memory: copy it (or draw it) before the next runFrame(). */
  get frame() {
    const m = this.m, w = m._host_video_w(), h = m._host_video_h(), ptr = m._host_video_ptr();
    return { width: w, height: h, data: new Uint8ClampedArray(m.HEAPU8.buffer, ptr, w * h * 4) };
  }

  /** Interleaved stereo int16 generated since the previous call (copied), then clears the core's buffer. */
  takeAudio() {
    const m = this.m, n = m._host_audio_frames();
    if (!n) return new Int16Array(0);
    const out = new Int16Array(n * 2);
    out.set(new Int16Array(m.HEAPU8.buffer, m._host_audio_ptr(), n * 2));
    m._host_audio_clear();
    return out;
  }

  saveState() {
    const m = this.m, size = m._host_state_size();
    if (!size) return null;
    const ptr = m._host_state_save();
    return ptr ? new Uint8Array(m.HEAPU8.buffer, ptr, size).slice() : null;
  }

  loadState(bytes) {
    const m = this.m, ptr = m._malloc(bytes.length);
    m.HEAPU8.set(bytes, ptr);
    const ok = m._host_state_load(ptr, bytes.length);
    m._free(ptr);
    return !!ok;
  }

  /** Battery-backed save RAM, or null if the game has none. */
  getSram() {
    const m = this.m, size = m._host_sram_size(), ptr = m._host_sram_ptr();
    return size && ptr ? new Uint8Array(m.HEAPU8.buffer, ptr, size).slice() : null;
  }

  /** Call right after loadGame() to restore a previous save. */
  setSram(bytes) {
    const m = this.m, size = m._host_sram_size(), ptr = m._host_sram_ptr();
    if (!size || !ptr) return false;
    m.HEAPU8.set(bytes.subarray(0, size), ptr);
    return true;
  }
}
