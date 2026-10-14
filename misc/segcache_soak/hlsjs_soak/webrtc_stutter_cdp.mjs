#!/usr/bin/env node
/**
 * Drive an already-running Chrome (CDP) for multi-rendition stutter checks.
 * Always uses exactly the requested number of tabs (default 3): closes prior
 * probe tabs first, opens fresh ones, closes them on exit unless --keep-tabs.
 *
 *   CDP_URL=http://127.0.0.1:9222 \
 *   OME_SOAK_HOST=127.0.0.1 OME_SOAK_PORT=3333 \
 *   node webrtc_stutter_cdp.mjs --duration-s 40
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
function hasFlag(name) {
  return process.argv.includes(name);
}
function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

const cdpUrl = env('CDP_URL', 'http://127.0.0.1:9222');
const host = env('OME_SOAK_HOST', '127.0.0.1');
const port = env('OME_SOAK_PORT', '3333');
const app = env('OME_SOAK_APP', 'app');
const stream = env('OME_SOAK_STREAM', 'stream');
const playlist = env('OME_SOAK_PLAYLIST', 'auto');
const durationS = Number(arg('--duration-s', '40'));
const warmupS = Number(arg('--warmup-s', '3'));
const maxFreezeEvents = Number(arg('--max-freeze-events', '3'));
const maxStallMs = Number(arg('--max-stall-ms', '2000'));
const maxFreezeCount = Number(arg('--max-freeze-count', '3'));
const scoredDurationS = Math.max(1, durationS - warmupS);
const minDecoded = Number(arg('--min-decoded', String(Math.max(60, Math.floor(scoredDurationS * 20)))));
const relay = ['1', 'true', 'yes'].includes(env('OME_SOAK_ICE_RELAY', '0'));
const bindHost = env('STATIC_BIND', '0.0.0.0');
const publicHost = env('STATIC_HOST', host);
const keepTabs = hasFlag('--keep-tabs');

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
      res.writeHead(200, {
        'Content-Type': MIME[path.extname(filePath)] || 'application/octet-stream',
        'Access-Control-Allow-Origin': '*',
      });
      res.end(data);
    } catch {
      res.writeHead(404);
      res.end('not found');
    }
  });
  await new Promise((resolve) => server.listen(0, bindHost, resolve));
  return { server, port: server.address().port };
}

function isProbeUrl(url) {
  return typeof url === 'string' && url.includes('webrtc_stutter.html');
}

async function closeProbePages(context) {
  const pages = context.pages();
  let closed = 0;
  for (const page of pages) {
    const url = page.url();
    if (isProbeUrl(url)) {
      await page.close().catch(() => {});
      closed++;
    }
  }
  return closed;
}

async function ensureBlankPage(context) {
  const pages = context.pages();
  if (pages.length === 0) {
    await context.newPage();
  }
}

async function main() {
  const { server, port: staticPort } = await startStaticServer(__dirname);
  let wsBase = `ws://${host}:${port}/${app}/${stream}/${playlist}`;
  if (relay) wsBase += (wsBase.includes('?') ? '&' : '?') + 'transport=relay';

  console.log(`cdp=${cdpUrl}`);
  console.log(`static=http://${publicHost}:${staticPort}/webrtc_stutter.html`);
  console.log(
    `ws=${wsBase} duration=${durationS}s warmup=${warmupS}s exactly ${clients.length} tabs: ${clients
      .map((c) => c.label + '>' + c.rendition)
      .join(',')}`,
  );

  const browser = await chromium.connectOverCDP(cdpUrl);
  const context = browser.contexts()[0] ?? (await browser.newContext());

  const closed = await closeProbePages(context);
  if (closed) console.log(`closed ${closed} prior probe tab(s)`);
  await ensureBlankPage(context);

  const pages = [];
  for (const c of clients) {
    const page = await context.newPage();
    const pageUrl =
      `http://${publicHost}:${staticPort}/webrtc_stutter.html` +
      `?ws=${encodeURIComponent(wsBase)}` +
      `&rendition=${encodeURIComponent(c.rendition)}` +
      `&label=${encodeURIComponent(c.label)}` +
      `&warmup=${encodeURIComponent(String(warmupS))}` +
      (relay ? '&relay=1' : '');
    console.log(`open [${c.label}] ${c.rendition}`);
    await page.goto(pageUrl, { waitUntil: 'domcontentloaded', timeout: 60000 });
    pages.push({ ...c, page });
  }

  const probeCount = context.pages().filter((p) => isProbeUrl(p.url())).length;
  if (probeCount !== clients.length) {
    console.log(`WARN probe tab count=${probeCount} expected=${clients.length}`);
  } else {
    console.log(`tabs ok: ${probeCount} probe tab(s)`);
  }

  const deadline = Date.now() + durationS * 1000;
  let lastReport = 0;
  while (Date.now() < deadline) {
    await sleep(1000);
    if (Date.now() - lastReport < 5000) continue;
    lastReport = Date.now();
    for (const c of pages) {
      const s = await c.page.evaluate(() => window.__STUTTER__?.summary ?? null).catch(() => null);
      if (!s) continue;
      console.log(
        `progress [${s.label}] ${s.frameWidth}x${s.frameHeight} recv=${s.framesReceived ?? '-'} ` +
          `dec=${s.framesDecoded} ratio=${(s.decodeRatio ?? 0).toFixed(2)} mbps=${(s.bitrateMbps ?? 0).toFixed(1)} ` +
          `freezeEvents=${s.freezeEvents} stallMs=${s.stallMs} freezeCount=${s.freezeCount} lost=${s.packetsLost} ` +
          `ice=${s.localCandidateType}/${s.remoteCandidateType}/${s.localProtocol} ` +
          `rtt=${s.currentRoundTripTime ?? '-'} net=${s.networkType ?? '-'}`,
      );
    }
  }

  let exitCode = 0;
  for (const c of pages) {
    const state = await c.page
      .evaluate(() => JSON.parse(JSON.stringify(window.__STUTTER__ ?? null)))
      .catch(() => null);
    const s = state?.summary;
    const err = state?.error;
    if (err) {
      console.log(`FAIL [${c.label}] error=${err}`);
      exitCode = 1;
      continue;
    }
    if (!s || s.state !== 'playing') {
      console.log(`FAIL [${c.label}] not playing state=${s?.state} ice=${s?.ice}`);
      exitCode = 1;
      continue;
    }
    const reasons = [];
    if (s.framesDecoded < minDecoded) reasons.push(`decoded=${s.framesDecoded}<${minDecoded}`);
    if (s.freezeEvents > maxFreezeEvents) reasons.push(`freezeEvents=${s.freezeEvents}>${maxFreezeEvents}`);
    if (s.stallMs > maxStallMs) reasons.push(`stallMs=${s.stallMs}>${maxStallMs}`);
    if ((s.freezeCount ?? 0) > maxFreezeCount) reasons.push(`freezeCount=${s.freezeCount}>${maxFreezeCount}`);
    if (reasons.length) {
      console.log(`FAIL [${c.label}] ${reasons.join('; ')} :: ${JSON.stringify(s)}`);
      exitCode = 1;
    } else {
      console.log(
        `OK [${c.label}] ${s.frameWidth}x${s.frameHeight} decoded=${s.framesDecoded} ` +
          `freezeEvents=${s.freezeEvents} stallMs=${s.stallMs} freezeCount=${s.freezeCount} lost=${s.packetsLost}`,
      );
    }
  }

  if (keepTabs) {
    console.log('keeping 3 probe tabs open (--keep-tabs)');
  } else {
    for (const c of pages) {
      await c.page.close().catch(() => {});
    }
    console.log('closed probe tabs');
  }

  await browser.close();
  server.close();
  if (exitCode === 0) console.log('PASS all clients');
  process.exit(exitCode);
}

main().catch((e) => {
  console.error(e);
  process.exit(2);
});
