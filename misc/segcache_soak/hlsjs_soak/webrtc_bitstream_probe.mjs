#!/usr/bin/env node
/**
 * WebRTC bitstream probe: records exactly the H.264 that a Chrome receiver
 * assembles from OME's RTP (post-depacketizer, pre-decoder) so it can be
 * validated offline with a strict decoder. The page never decodes, so the
 * probe adds one viewer's send load but almost no local CPU.
 *
 * Usage:
 *   node webrtc_bitstream_probe.mjs --duration-s 600 --out /path/to/dir [--rendition Bypass]
 *
 * Output: <out>/probe.h264 (Annex B), <out>/frames.jsonl (one line per frame:
 *   t, rtp, type, ssrc, pt, w, h, size, offset), <out>/stats.jsonl
 *
 * Env: OME_SOAK_HOST OME_SOAK_PORT OME_SOAK_APP OME_SOAK_STREAM OME_SOAK_PLAYLIST
 */

import { chromium } from 'playwright';
import { createServer } from 'node:http';
import { readFile, mkdir } from 'node:fs/promises';
import { createWriteStream } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const env = (name, fallback) => process.env[name] ?? fallback;
function arg(name, fallback) {
  const i = process.argv.indexOf(name);
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : fallback;
}

const host = env('OME_SOAK_HOST', '127.0.0.1');
const port = env('OME_SOAK_PORT', '13333');
const app = env('OME_SOAK_APP', '_filler');
const stream = env('OME_SOAK_STREAM', 'stream');
const playlist = env('OME_SOAK_PLAYLIST', 'hls');
const durationS = Number(arg('--duration-s', '60'));
const outDir = arg('--out', path.join(__dirname, 'probe_out'));
const rendition = arg('--rendition', '');

async function main() {
  await mkdir(outDir, { recursive: true });
  const h264 = createWriteStream(path.join(outDir, 'probe.h264'));
  const framesLog = createWriteStream(path.join(outDir, 'frames.jsonl'));
  const statsLog = createWriteStream(path.join(outDir, 'stats.jsonl'));
  let offset = 0;
  let frames = 0;
  let lastRtp = null;

  const server = createServer(async (req, res) => {
    try {
      const data = await readFile(path.join(__dirname, 'webrtc_bitstream_probe.html'));
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
      res.end(data);
    } catch {
      res.writeHead(404);
      res.end();
    }
  });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));

  const wsUrl = `ws://${host}:${port}/${app}/${stream}/${playlist}`;
  const browser = await chromium.launch({
    headless: true,
    args: ['--autoplay-policy=no-user-gesture-required', ...(process.env.OME_SOAK_CHROME_ARGS ?? '').split(' ').filter(Boolean)],
  });
  console.log(`probe start ws=${wsUrl} rendition=${rendition || '(default)'} duration=${durationS}s out=${outDir} browser=${browser.version()}`);

  const page = await browser.newPage();
  page.on('pageerror', (e) => console.log(`pageerror: ${e.message}`));
  await page.exposeFunction('__probeFrames', (json) => {
    for (const f of JSON.parse(json)) {
      const buf = Buffer.from(f.b64, 'base64');
      h264.write(buf);
      // Flag RTP timestamp regressions/jumps right away (90kHz; 60fps = 1500).
      const jump = lastRtp == null ? 0 : ((f.rtp - lastRtp) >>> 0);
      framesLog.write(JSON.stringify({ t: f.t, rtp: f.rtp, d_rtp: jump, type: f.type, ssrc: f.ssrc, pt: f.pt, w: f.w, h: f.h, size: buf.length, offset }) + '\n');
      lastRtp = f.rtp;
      offset += buf.length;
      frames++;
    }
  });
  const url = `http://127.0.0.1:${server.address().port}/?ws=${encodeURIComponent(wsUrl)}&rendition=${encodeURIComponent(rendition)}`;
  await page.goto(url, { waitUntil: 'domcontentloaded' });

  const deadline = Date.now() + durationS * 1000;
  while (Date.now() < deadline) {
    await new Promise((r) => setTimeout(r, 5000));
    const s = await page.evaluate(() => window.__PROBE__);
    statsLog.write(JSON.stringify(s) + '\n');
    console.log(`progress state=${s.state} ice=${s.ice} api=${s.api} red=${s.red} frames=${frames} bytes=${offset} ` +
                `lost=${s.stats?.packetsLost} nack=${s.stats?.nackCount} rtx=${s.stats?.retransmittedPacketsReceived} fec=${s.stats?.fecPacketsReceived} err=${s.error ?? '-'}`);
    if (s.error) break;
  }

  await browser.close();
  server.close();
  await Promise.all([h264, framesLog, statsLog].map((w) => new Promise((r) => w.end(r))));
  console.log(`probe done frames=${frames} bytes=${offset}`);
}

main().catch((e) => {
  console.error(e);
  process.exit(2);
});
