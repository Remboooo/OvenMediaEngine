#!/usr/bin/env node
/**
 * Headless Chromium + hls.js long-play soak for SegmentCache HLS.
 *
 * Fails on:
 *   - hls.js fatal that does not recover
 *   - media stall (currentTime not advancing) beyond --stall-s
 *   - DISC-SEQ vanish / decrease observed in page invariants
 *
 * Env (SOAK_DOGFOOD=1 sets host/port defaults):
 *   OME_SOAK_HOST OME_SOAK_PORT OME_SOAK_APP OME_SOAK_STREAM
 *
 * Usage:
 *   SOAK_DOGFOOD=1 node play.mjs --duration-s 400
 */

import { chromium } from 'playwright';
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

function env(name, fallback) {
  return process.env[name] ?? fallback;
}

if (['1', 'true', 'yes'].includes(env('SOAK_DOGFOOD', '0'))) {
  process.env.OME_SOAK_HOST ??= '127.0.0.1';
  process.env.OME_SOAK_PORT ??= '13333';
}

function arg(name, fallback) {
  const i = process.argv.indexOf(name);
  if (i >= 0 && process.argv[i + 1]) return process.argv[i + 1];
  return fallback;
}

const durationS = Number(arg('--duration-s', '400'));
const stallS = Number(arg('--stall-s', '20'));
const host = env('OME_SOAK_HOST', '127.0.0.1');
const port = env('OME_SOAK_PORT', '3333');
const app = env('OME_SOAK_APP', '_filler');
const stream = env('OME_SOAK_STREAM', '_filler');
const src =
  arg('--src', '') ||
  `http://${host}:${port}/${app}/${stream}/ts:playlist.m3u8`;

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.map': 'application/json',
  '.css': 'text/css',
};

async function startStaticServer(root) {
  const server = createServer(async (req, res) => {
    try {
      const u = new URL(req.url || '/', 'http://127.0.0.1');
      let rel = decodeURIComponent(u.pathname);
      if (rel === '/') rel = '/index.html';
      const filePath = path.join(root, rel);
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
  const addr = server.address();
  return { server, port: addr.port };
}

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

async function main() {
  const { server, port: staticPort } = await startStaticServer(__dirname);
  const pageUrl = `http://127.0.0.1:${staticPort}/index.html?src=${encodeURIComponent(src)}`;

  console.log(`hlsjs_soak start src=${src}`);
  console.log(`player page ${pageUrl}`);
  console.log(`duration=${durationS}s stall_limit=${stallS}s`);

  const browser = await chromium.launch({
    headless: true,
    channel: process.env.OME_SOAK_CHROME_CHANNEL || undefined,
    args: ['--autoplay-policy=no-user-gesture-required'],
  });

  const page = await browser.newPage();
  page.on('console', (msg) => {
    const t = msg.type();
    if (t === 'error' || t === 'warning' || t === 'log') {
      console.log(`browser:${t}: ${msg.text()}`);
    }
  });

  await page.goto(pageUrl, { waitUntil: 'domcontentloaded', timeout: 30000 });

  const deadline = Date.now() + durationS * 1000;
  let ready = false;
  let exitCode = 0;
  let lastReport = 0;

  while (Date.now() < deadline) {
    const state = await page.evaluate(() => window.__OME_SOAK__ || null);
    if (!state) {
      console.log('waiting for page state…');
      await sleep(1000);
      continue;
    }

    if (state.ready) ready = true;

    if (state.fatal) {
      console.log(`FAIL fatal=${state.fatal}`);
      exitCode = 1;
      break;
    }

    if (ready && state.stallMs > stallS * 1000) {
      console.log(
        `FAIL stall: currentTime not advancing for ${(state.stallMs / 1000).toFixed(1)}s ` +
          `(ct=${state.currentTime.toFixed(2)} bufEnd=${state.bufferedEnd.toFixed(2)} ` +
          `msn=${state.lastMediaSeq} discSeq=${state.lastDiscSeq})`,
      );
      exitCode = 1;
      break;
    }

    const now = Date.now();
    if (now - lastReport > 5000) {
      lastReport = now;
      console.log(
        `ok ct=${(state.currentTime || 0).toFixed(1)}s ` +
          `stall=${(state.stallMs / 1000).toFixed(1)}s ` +
          `msn=${state.lastMediaSeq} discSeq=${state.lastDiscSeq ?? '-'} ` +
          `errors=${(state.errors || []).length}`,
      );
    }

    await sleep(1000);
  }

  if (exitCode === 0 && !ready) {
    console.log('FAIL never became ready');
    exitCode = 1;
  }

  if (exitCode === 0) {
    const state = await page.evaluate(() => window.__OME_SOAK__);
    console.log(
      `PASS duration=${durationS}s ct=${(state?.currentTime || 0).toFixed(1)} ` +
        `msn=${state?.lastMediaSeq} discSeq=${state?.lastDiscSeq ?? '-'} ` +
        `errors=${(state?.errors || []).length}`,
    );
  }

  await browser.close();
  server.close();
  process.exit(exitCode);
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
