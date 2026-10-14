#!/usr/bin/env node
/**
 * Concurrent multi-rendition WebRTC stutter probe.
 *
 * Opens N Chromium pages against the same OME playlist, optionally switches
 * each to a named rendition, and fails if decoded-frame gaps / freezeCount
 * exceed thresholds while all clients run together.
 *
 * Usage:
 *   node webrtc_stutter.mjs --duration-s 45 \
 *     --client 'bypass:Bypass+opus' --client '1080:1080p' --client '720:720p'
 *
 * Env:
 *   OME_SOAK_HOST OME_SOAK_PORT OME_SOAK_APP OME_SOAK_STREAM OME_SOAK_PLAYLIST
 *   OME_SOAK_ICE_RELAY=1  force TURN/TCP
 *   OME_SOAK_CHROME_ARGS  extra Chromium flags
 */

import { chromium } from 'playwright';
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

function env(name, fallback) {
  return process.env[name] ?? fallback;
}

function arg(name, fallback) {
  const i = process.argv.indexOf(name);
  if (i >= 0 && process.argv[i + 1]) return process.argv[i + 1];
  return fallback;
}

function argsAll(name) {
  const out = [];
  for (let i = 0; i < process.argv.length; i++) {
    if (process.argv[i] === name && process.argv[i + 1]) out.push(process.argv[++i]);
  }
  return out;
}

const host = env('OME_SOAK_HOST', '127.0.0.1');
const port = env('OME_SOAK_PORT', '3333');
const app = env('OME_SOAK_APP', 'app');
const stream = env('OME_SOAK_STREAM', 'stream');
const playlist = env('OME_SOAK_PLAYLIST', 'auto');
const durationS = Number(arg('--duration-s', '45'));
const warmupS = Number(arg('--warmup-s', '8'));
const maxFreezeEvents = Number(arg('--max-freeze-events', '2'));
const maxStallMs = Number(arg('--max-stall-ms', '1500'));
const maxFreezeCount = Number(arg('--max-freeze-count', '3'));
const minDecoded = Number(arg('--min-decoded', String(Math.max(30, (durationS - warmupS) * 15))));
const relay = ['1', 'true', 'yes'].includes(env('OME_SOAK_ICE_RELAY', '0'));

const clientSpecs = argsAll('--client');
if (clientSpecs.length === 0) {
  clientSpecs.push('bypass:Bypass+opus', '1080:1080p', '720:720p');
}

const clients = clientSpecs.map((spec) => {
  const colon = spec.indexOf(':');
  if (colon < 0) return { label: spec, rendition: spec };
  return { label: spec.slice(0, colon), rendition: spec.slice(colon + 1) };
});

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
};

async function startStaticServer(root) {
  const server = createServer(async (req, res) => {
    try {
      const u = new URL(req.url || '/', 'http://127.0.0.1');
      const filePath = path.join(root, decodeURIComponent(u.pathname));
      if (!filePath.startsWith(root)) {
        res.writeHead(403);
        res.end('forbidden');
        return;
      }
      const data = await readFile(filePath);
      res.writeHead(200, { 'Content-Type': MIME[path.extname(filePath)] || 'application/octet-stream' });
      res.end(data);
    } catch {
      res.writeHead(404);
      res.end('not found');
    }
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  return { server, port: server.address().port };
}

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

async function main() {
  const { server, port: staticPort } = await startStaticServer(__dirname);
  let wsBase = `ws://${host}:${port}/${app}/${stream}/${playlist}`;
  if (relay) wsBase += (wsBase.includes('?') ? '&' : '?') + 'transport=relay';

  console.log(
    `webrtc_stutter start clients=${clients.map((c) => c.label + '>' + c.rendition).join(',')} ` +
      `ws=${wsBase} duration=${durationS}s warmup=${warmupS}s relay=${relay}`,
  );

  const browser = await chromium.launch({
    headless: true,
    channel: process.env.OME_SOAK_CHROME_CHANNEL || undefined,
    args: [
      '--autoplay-policy=no-user-gesture-required',
      '--use-fake-ui-for-media-stream',
      ...(process.env.OME_SOAK_CHROME_ARGS ?? '').split(' ').filter(Boolean),
    ],
  });
  console.log(`browser=${browser.version()}`);

  const pages = [];
  for (const c of clients) {
    const page = await browser.newPage();
    page.on('pageerror', (e) => console.log(`[${c.label}] pageerror: ${e.message}`));
    const pageUrl =
      `http://127.0.0.1:${staticPort}/webrtc_stutter.html` +
      `?ws=${encodeURIComponent(wsBase)}` +
      `&rendition=${encodeURIComponent(c.rendition)}` +
      `&label=${encodeURIComponent(c.label)}` +
      (relay ? '&relay=1' : '');
    await page.goto(pageUrl, { waitUntil: 'domcontentloaded', timeout: 30000 });
    pages.push({ ...c, page });
  }

  const deadline = Date.now() + durationS * 1000;
  let lastReport = 0;
  while (Date.now() < deadline) {
    await sleep(1000);
    if (Date.now() - lastReport < 5000) continue;
    lastReport = Date.now();
    for (const c of pages) {
      const s = await c.page.evaluate(() => window.__STUTTER__?.summary ?? null);
      if (!s) continue;
      console.log(
        `progress [${s.label}] state=${s.state} ice=${s.ice} ${s.frameWidth}x${s.frameHeight} ` +
          `recv=${s.framesReceived ?? '-'} dec=${s.framesDecoded} ratio=${(s.decodeRatio ?? 0).toFixed(2)} ` +
          `mbps=${(s.bitrateMbps ?? 0).toFixed(1)} freezeEvents=${s.freezeEvents} stallMs=${s.stallMs} ` +
          `maxGap=${s.maxGapMs} freezeCount=${s.freezeCount} lost=${s.packetsLost}`,
      );
    }
  }

  const results = [];
  for (const c of pages) {
    const state = await c.page.evaluate(() => JSON.parse(JSON.stringify(window.__STUTTER__ ?? null)));
    results.push({ label: c.label, rendition: c.rendition, state });
  }

  await browser.close();
  server.close();

  let exitCode = 0;
  for (const r of results) {
    const s = r.state?.summary;
    const err = r.state?.error;
    if (err) {
      console.log(`FAIL [${r.label}] error=${err}`);
      exitCode = 1;
      continue;
    }
    if (!s || s.state !== 'playing') {
      console.log(`FAIL [${r.label}] not playing state=${s?.state} ice=${s?.ice}`);
      exitCode = 1;
      continue;
    }
    // Ignore warmup window by requiring enough decoded frames and checking end metrics.
    const postWarmDecoded = s.framesDecoded;
    const failReasons = [];
    if (postWarmDecoded < minDecoded) {
      failReasons.push(`decoded=${postWarmDecoded} < ${minDecoded}`);
    }
    if (s.freezeEvents > maxFreezeEvents) {
      failReasons.push(`freezeEvents=${s.freezeEvents} > ${maxFreezeEvents}`);
    }
    if (s.stallMs > maxStallMs) {
      failReasons.push(`stallMs=${s.stallMs} > ${maxStallMs}`);
    }
    if ((s.freezeCount ?? 0) > maxFreezeCount) {
      failReasons.push(`freezeCount=${s.freezeCount} > ${maxFreezeCount}`);
    }
    if (failReasons.length) {
      console.log(`FAIL [${r.label}] ${failReasons.join('; ')} :: ${JSON.stringify(s)}`);
      exitCode = 1;
    } else {
      console.log(
        `OK [${r.label}] ${s.frameWidth}x${s.frameHeight} decoded=${s.framesDecoded} ` +
          `freezeEvents=${s.freezeEvents} stallMs=${s.stallMs} freezeCount=${s.freezeCount}`,
      );
    }
  }

  if (exitCode === 0) console.log('PASS all clients');
  process.exit(exitCode);
}

main().catch((e) => {
  console.error(e);
  process.exit(2);
});
