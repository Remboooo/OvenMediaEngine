#!/usr/bin/env node
/**
 * Oracle viewer runner: plays one WebRTC stream in headless Chromium and records what that viewer
 * experiences (webrtc_oracle.html), for misc/traffic_sim/limits.py to judge.
 *
 * Usage:
 *   node webrtc_oracle.mjs --ws ws://<lan-ip>:33333/live/s720/auto?rendition=720p --out DIR
 *                          [--duration-s 60] [--capture] [--no-decode] [--no-transform]
 *
 * Output in DIR: assembled.jsonl, presented.jsonl, stats.jsonl, lag.jsonl, meta.json,
 *   and with --capture probe.h264 (Annex B, assembly order) for h264_decode_check.sh.
 * OME_SOAK_CDP=http://host:port drives a remote Chrome (--remote-debugging-port), OME_SOAK_BIDI=ws://host:port a
 * remote Firefox (--remote-debugging-port, WebDriver BiDi) instead: either in a context of its own.
 * It runs until --duration-s or SIGTERM, whichever comes first, then flushes and exits 0.
 * Exit 3: the session failed (meta.json has the error).
 */

import { chromium } from 'playwright';
import puppeteer from 'puppeteer-core';
import { createServer } from 'node:http';
import { readFile, mkdir, writeFile } from 'node:fs/promises';
import { createWriteStream } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const arg = (name, fallback) => {
  const i = process.argv.indexOf(name);
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : fallback;
};
const flag = (name) => process.argv.includes(name);

const wsUrl = arg('--ws');
const outDir = arg('--out');
const durationS = Number(arg('--duration-s', '3600'));
const capture = flag('--capture');
const decode = !flag('--no-decode');
const transform = !flag('--no-transform');
if (!wsUrl || !outDir) {
  console.error('usage: webrtc_oracle.mjs --ws URL --out DIR [--duration-s N] [--capture] [--no-decode]');
  process.exit(2);
}

async function main() {
  await mkdir(outDir, { recursive: true });
  const logs = Object.fromEntries(['assembled', 'presented', 'stats', 'lag'].map((k) => [k, createWriteStream(path.join(outDir, `${k}.jsonl`))]));
  const h264 = capture ? createWriteStream(path.join(outDir, 'probe.h264')) : null;
  let offset = 0;

  const server = createServer(async (req, res) => {
    try {
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
      res.end(await readFile(path.join(__dirname, 'webrtc_oracle.html')));
    } catch {
      res.writeHead(404);
      res.end();
    }
  });
  // A remote browser loads the page from this machine's LAN address (the signalling host): Chrome's Local
  // Network Access blocks a page from any other origin from reaching a private address
  const bidi = process.env.OME_SOAK_BIDI;
  const cdp = process.env.OME_SOAK_CDP ?? bidi;
  const pageHost = cdp ? new URL(wsUrl).hostname : '127.0.0.1';
  await new Promise((r) => server.listen(0, pageHost, r));

  // OME_SOAK_CDP=http://host:port drives a browser on another machine (Chrome with --remote-debugging-port)
  // instead of a local headless Chromium: real GPU decoding, a real display. Its clock is mapped onto ours.
  // Firefox's remote agent accepts only its own address as Host (DNS-rebinding guard), and the port reaches
  // it through a proxy on that machine
  const browser = bidi ? await puppeteer.connect({ browserWSEndpoint: `${bidi}/session`, protocol: 'webDriverBiDi',
                                                    headers: { Host: `127.0.0.1:${process.env.OME_SOAK_BIDI_PORT ?? '9224'}` } })
    : cdp ? await chromium.connectOverCDP(cdp) : await chromium.launch({
    headless: true,
    handleSIGTERM: false,  // a stop signal ends the recording below, which then closes the browser
    handleSIGINT: false,
    args: ['--autoplay-policy=no-user-gesture-required', '--disable-renderer-backgrounding',
           '--disable-background-timer-throttling', '--disable-backgrounding-occluded-windows',
           ...(process.env.OME_SOAK_CHROME_ARGS ?? '').split(' ').filter(Boolean)],
  });
  // viewport: null: the real window, not Playwright's emulated 1280x720 viewport and screen (a device-metrics
  // override the compositor scales for)
  // OME_SOAK_VIEWPORT=emulate: Playwright's default instead, to compare
  const context = bidi ? await browser.createBrowserContext()
    : cdp ? await browser.newContext(process.env.OME_SOAK_VIEWPORT === 'emulate' ? {} : { viewport: null }) : null;
  const page = await (context ?? browser).newPage();
  if (cdp && !bidi && process.env.OME_SOAK_RAISE === '1') {
    // Our own window to the top: a desktop window covered by another (Firefox's, after a Firefox run)
    // may lose frames at the compositor, whatever the backgrounding flags. Restoring a minimized window
    // raises it.
    const session = await context.newCDPSession(page);
    const { windowId } = await session.send('Browser.getWindowForTarget');
    await session.send('Browser.setWindowBounds', { windowId, bounds: { windowState: 'minimized' } });
    await session.send('Browser.setWindowBounds', { windowId, bounds: { windowState: 'normal' } });
    await session.detach();
  }
  let clockOffset = 0;  // remote epoch ms minus local epoch ms
  const EPOCH_FIELDS = ['t', 'when', 'edt', 'recv'];
  page.on('pageerror', (e) => console.log(`pageerror: ${e.message}`));
  await page.exposeFunction('__oracleRecords', (kind, json) => {
    for (const r of JSON.parse(json)) {
      if (kind === 'assembled' && r.b64 !== undefined) {
        const buf = Buffer.from(r.b64, 'base64');
        delete r.b64;
        h264.write(buf);
        r.offset = offset;
        offset += buf.length;
      }
      if (clockOffset) {
        for (const f of EPOCH_FIELDS) if (typeof r[f] === 'number') r[f] -= clockOffset;
      }
      logs[kind].write(JSON.stringify(r) + '\n');
    }
  });

  let stopping = false;
  const stop = () => {
    stopping = true;
    // Stuck before the recording loop (a page that never loads): still close our own context, so a remote
    // browser keeps no stray window and Firefox frees its one remote session
    setTimeout(async () => {
      console.log('stop: recording loop did not end in 15 s; closing our context');
      try {
        if (cdp) await context.close();
        if (bidi) await browser.disconnect();
        if (!cdp) await browser.close();
      } catch {}
      process.exit(4);
    }, 15000).unref();
  };
  process.on('SIGTERM', stop);
  process.on('SIGINT', stop);

  // http: OME's signalling is ws://, which an https page may not open (mixed content)
  const base = `http://${pageHost}:${server.address().port}`;
  if (cdp) {
    // Best of several round trips: the remote clock at the midpoint
    await page.goto('about:blank');
    let best = Infinity;
    for (let i = 0; i < 9; i++) {
      const a = Date.now();
      const remote = await page.evaluate(() => performance.timeOrigin + performance.now());
      const b = Date.now();
      if (b - a < best) {
        best = b - a;
        clockOffset = remote - (a + b) / 2;
      }
    }
    console.log(`remote browser ${cdp}: clock offset ${clockOffset.toFixed(1)} ms (rtt ${best} ms)`);
  }
  const url = `${base}/?ws=${encodeURIComponent(wsUrl)}&capture=${capture ? 1 : 0}&decode=${decode ? 1 : 0}&transform=${transform ? 1 : 0}`;
  const started = Date.now();
  await page.goto(url, { waitUntil: 'domcontentloaded' });
  if (bidi) {
    // Firefox blocks audible autoplay without a user gesture: a click through BiDi is one
    await page.mouse.click(5, 5);
    // Not awaited in the page: play() on an element without a source yet (offline stream) never settles
    await page.evaluate(() => { document.getElementById('a').play().catch(() => {}); });
  }
  console.log(`oracle start ws=${wsUrl} decode=${decode} capture=${capture} browser=${await browser.version()}`);

  let state = {};
  const deadline = started + durationS * 1000;
  while (!stopping && Date.now() < deadline) {
    await new Promise((r) => setTimeout(r, 500));
    state = await page.evaluate(() => { const o = { ...window.__ORACLE__ }; delete o.offer; return o; });
    if (state.error) break;
  }
  const offer = await page.evaluate(() => window.__ORACLE__.offer).catch(() => null);
  await new Promise((r) => setTimeout(r, 400));  // last batch
  if (cdp) {
    // Only our own context (its window and pages): the remote browser, and the tabs its user keeps open,
    // stay up. The connection is dropped when this process exits.
    await context.close();
    if (bidi) await browser.disconnect();
  } else {
    await browser.close();
  }
  server.close();
  await Promise.all([...Object.values(logs), ...(h264 ? [h264] : [])].map((w) => new Promise((r) => w.end(r))));
  await writeFile(path.join(outDir, 'meta.json'), JSON.stringify({ ws: wsUrl, decode, capture, started, ended: Date.now(), ...state,
    ...(cdp ? { remote: cdp, clock_offset_ms: clockOffset,
                connected_at: state.connected_at != null ? state.connected_at - clockOffset : state.connected_at } : {}),
    offer }, null, 1));
  console.log(`oracle done assembled=${state.assembled} presented=${state.presented} error=${state.error ?? '-'}`);
  process.exit(state.error && !state.connected_at ? 3 : 0);
}

main().catch((e) => {
  console.error(e);
  process.exit(2);
});
