// Headless smoke test:  node test/run-core.mjs <core.js dist file> <exportName> <rom> <outPrefix> [frames]
// Boots the core, runs frames, taps START, checks video/audio/state, and writes PNGs you can look at.
import { createRequire } from 'node:module';
import { readFileSync, writeFileSync } from 'node:fs';
import { deflateSync } from 'node:zlib';
import { RetroCore } from '../js/retro-core.js';

const [, , corePath, exportName, romPath, outPrefix = 'out', framesArg = '240'] = process.argv;
const require = createRequire(import.meta.url);
const factory = require(corePath);

function png(file, w, h, rgba) {
  const crcT = Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
  const crc = (b) => { let c = ~0; for (const x of b) c = crcT[(c ^ x) & 255] ^ (c >>> 8); return ~c >>> 0; };
  const chunk = (t, d) => { const b = Buffer.alloc(12 + d.length); b.writeUInt32BE(d.length, 0); b.write(t, 4); d.copy(b, 8); b.writeUInt32BE(crc(b.subarray(4, 8 + d.length)), 8 + d.length); return b; };
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) { raw[y * (w * 4 + 1)] = 0; Buffer.from(rgba.buffer, rgba.byteOffset + y * w * 4, w * 4).copy(raw, y * (w * 4 + 1) + 1); }
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 6;
  writeFileSync(file, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
}
const stats = (f) => { const d = f.data; const seen = new Set(); let lit = 0; for (let i = 0; i < d.length; i += 4) { seen.add((d[i] << 16) | (d[i + 1] << 8) | d[i + 2]); if (d[i] | d[i + 1] | d[i + 2]) lit++; } return { colors: seen.size, litPixels: lit }; };
const fail = (m) => { console.error('FAIL:', m); process.exit(1); };

const core = await RetroCore.create(factory);
console.log(`core: ${core.name} ${core.version}`);
const rom = new Uint8Array(readFileSync(romPath));
core.loadGame(rom, romPath.split('/').pop());
console.log(`loaded ${romPath} | fps ${core.fps.toFixed(3)} | audio ${core.sampleRate} Hz`);

const total = +framesArg;
let audioFrames = 0, shotA = null;
for (let i = 0; i < total; i++) {
  core.runFrame();
  audioFrames += core.takeAudio().length / 2;
  if (i === total - 1) { const f = core.frame; shotA = { width: f.width, height: f.height, data: f.data.slice() }; }
}
const sA = stats(shotA);
console.log(`frame ${total}: ${shotA.width}x${shotA.height}`, sA, `| audio frames ${audioFrames} (expect ~${Math.round(total / core.fps * core.sampleRate)})`);
png(`${outPrefix}-a.png`, shotA.width, shotA.height, shotA.data);

// tap START (and A) for 10 frames, then run more
core.setInput(0, RetroCore.BUTTON.START | RetroCore.BUTTON.A);
for (let i = 0; i < 10; i++) { core.runFrame(); core.takeAudio(); }
core.setInput(0, 0);
for (let i = 0; i < 90; i++) { core.runFrame(); core.takeAudio(); }
const fB = core.frame, shotB = { width: fB.width, height: fB.height, data: fB.data.slice() };
png(`${outPrefix}-b.png`, shotB.width, shotB.height, shotB.data);
const differs = shotA.data.some((v, i) => v !== shotB.data[i]);
console.log('picture changed after input:', differs, stats(shotB));

// save state round-trip
const st = core.saveState();
console.log('state bytes:', st && st.length);
core.runFrame();
if (st) console.log('loadState ok:', core.loadState(st));
core.reset(); core.runFrame();

if (sA.litPixels === 0) fail('first picture is blank');
if (audioFrames < total * core.sampleRate / core.fps * 0.5) fail('too little audio');
console.log('PASS');
