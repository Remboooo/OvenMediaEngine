#!/usr/bin/env node
/**
 * Headless Chromium WebRTC decode-order check.
 *
 * Plays a WebRTC stream and inspects every received H.264 frame before it is
 * decoded (Encoded Transform). Fails if any frame breaks H.264 decode order
 * (frame_num sequence), e.g. B-frames sent before their reference P-frame —
 * which Chrome silently conceals as blocky artifacts.
 *
 * Fails on:
 *   - any decode-order violation
 *   - fewer than --min-frames frames checked (vacuous run)
 *   - Chrome decoding less than --min-decode-ratio of received frames
 *     (e.g. keyframes-only playback: libwebrtc's software H.264 decoder
 *     rejects B-frame streams sent in correct decode order)
 *   - signalling / peer connection error
 *
 * Env (SOAK_DOGFOOD=1 sets host/port defaults):
 *   OME_SOAK_HOST OME_SOAK_PORT OME_SOAK_APP OME_SOAK_STREAM
 *   OME_SOAK_CHROME_ARGS  extra Chromium flags (space separated)
 *   OME_SOAK_BROWSER      chromium (default) or firefox
 *   OME_SOAK_ICE_RELAY    1 = connect only via OME's TcpRelay (TURN/TCP)
 *   OME_SOAK_FIREFOX_ICE_INTERFACE  Firefox only: ICE interface (default lo)
 *   OME_SOAK_OPENH264_DIR Firefox only: a gmp-gmpopenh264/<version> directory
 *                         (e.g. from a normal Firefox profile). Playwright's
 *                         Firefox ships without H.264; this copies the plugin
 *                         into a throwaway profile.
 *
 * Usage:
 *   SOAK_DOGFOOD=1 node webrtc_order.mjs --duration-s 30
 */

import { chromium, firefox } from 'playwright';
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { cp, mkdtemp, rm } from 'node:fs/promises';
import os from 'node:os';

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

const host = env('OME_SOAK_HOST', '127.0.0.1');
const port = env('OME_SOAK_PORT', '3333');
const app = env('OME_SOAK_APP', '_filler');
const stream = env('OME_SOAK_STREAM', '_filler');
const wsUrl = arg('--ws', `ws://${host}:${port}/${app}/${stream}`);
const durationS = Number(arg('--duration-s', '30'));
const minFrames = Number(arg('--min-frames', '300'));
const minDecodeRatio = Number(arg('--min-decode-ratio', '0.9'));

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
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

function fmtFrame(f) {
  const exp = f.expected !== undefined ? ` expected=${f.expected}` : '';
  return `#${f.n} ${f.type}${f.ref ? '(ref)' : ''} frame_num=${f.frameNum}${exp} ts=${f.ts}`;
}

async function main() {
  const { server, port: staticPort } = await startStaticServer(__dirname);
  // Relay mode: `?transport=relay` makes OME offer only its TcpRelay (via iceServers).
  const relay = ['1', 'true', 'yes'].includes(env('OME_SOAK_ICE_RELAY', '0'));
  const signallingUrl = relay ? `${wsUrl}${wsUrl.includes('?') ? '&' : '?'}transport=relay` : wsUrl;
  const pageUrl = `http://127.0.0.1:${staticPort}/webrtc_order.html?ws=${encodeURIComponent(signallingUrl)}${relay ? '&relay=1' : ''}`;

  console.log(`webrtc_order start ws=${wsUrl} duration=${durationS}s min_frames=${minFrames}`);

  const browserName = env('OME_SOAK_BROWSER', 'chromium');
  let browser;
  let page;
  let profile = null;
  if (browserName === 'firefox') {
    const prefs = {
      'media.autoplay.default': 0,
      // Allow loopback ICE and plain host candidates (OME can't resolve mDNS).
      'media.peerconnection.ice.loopback': true,
      'media.peerconnection.ice.obfuscate_host_addresses': false,
      'media.peerconnection.ice.force_interface': env('OME_SOAK_FIREFOX_ICE_INTERFACE', 'lo'),
    };
    profile = await mkdtemp(path.join(os.tmpdir(), 'webrtc-order-ff-'));
    const openh264 = process.env.OME_SOAK_OPENH264_DIR;
    if (openh264) {
      const version = path.basename(openh264);
      await cp(openh264, path.join(profile, 'gmp-gmpopenh264', version), { recursive: true });
      Object.assign(prefs, {
        'media.gmp-provider.enabled': true,
        'media.gmp-gmpopenh264.enabled': true,
        'media.gmp-gmpopenh264.visible': true,
        'media.gmp-gmpopenh264.version': version,
        'media.gmp-gmpopenh264.autoupdate': false,
        'media.gmp-manager.updateEnabled': false,
      });
    }
    browser = await firefox.launchPersistentContext(profile, { headless: true, firefoxUserPrefs: prefs });
    page = browser.pages()[0] ?? (await browser.newPage());
    console.log(`browser=firefox openh264=${openh264 ? path.basename(openh264) : 'none'}`);
  } else {
    browser = await chromium.launch({
      headless: true,
      channel: process.env.OME_SOAK_CHROME_CHANNEL || undefined,
      args: ['--autoplay-policy=no-user-gesture-required', ...(process.env.OME_SOAK_CHROME_ARGS ?? '').split(' ').filter(Boolean)],
    });
    console.log(`browser=chromium ${browser.version()}`);
    page = await browser.newPage();
  }

  page.on('console', (msg) => {
    if (msg.type() === 'error' || msg.type() === 'warning') console.log(`browser:${msg.type()}: ${msg.text()}`);
  });
  page.on('pageerror', (e) => console.log(`browser:pageerror: ${e.message}`));

  await page.goto(pageUrl, { waitUntil: 'domcontentloaded', timeout: 30000 });

  const deadline = Date.now() + durationS * 1000;
  let state = null;
  let lastReport = 0;

  while (Date.now() < deadline) {
    await sleep(1000);
    state = await page.evaluate(() => JSON.parse(JSON.stringify(window.__ORDER__ ?? null)));
    if (state?.error) break;
    if (Date.now() - lastReport >= 5000 && state?.order) {
      lastReport = Date.now();
      const o = state.order;
      console.log(
        `progress frames=${o.frames} checked=${o.checked} idr=${o.idr} violations=${o.violations} ` +
          `decoded=${state.rtp?.framesDecoded ?? '-'} lost=${state.rtp?.packetsLost ?? '-'}`,
      );
    }
  }

  await browser.close();
  server.close();
  if (profile) await rm(profile, { recursive: true, force: true });

  const o = state?.order;
  console.log(`mode=${state?.mode} state=${state?.state} ice=${state?.ice} rtp=${JSON.stringify(state?.rtp ?? null)}`);
  if (process.env.OME_SOAK_DEBUG) {
    console.log(`offer: ${JSON.stringify(state?.offerVideo)}`);
    console.log(`answer: ${JSON.stringify(state?.answerVideo)}`);
    console.log(`remote candidates: ${JSON.stringify(state?.remoteCandidates)}`);
    console.log(`local candidates: ${JSON.stringify(state?.localCandidates)}`);
    console.log(`ice servers: ${JSON.stringify(state?.iceServers)}`);
  }

  let exitCode = 0;
  if (state?.error) {
    console.log(`FAIL error=${state.error}`);
    exitCode = 1;
  } else if (!o || o.checked < minFrames) {
    console.log(`FAIL only ${o?.checked ?? 0} frames checked (< ${minFrames})`);
    exitCode = 1;
  } else if (o.violations > 0) {
    console.log(`FAIL ${o.violations}/${o.checked} frames out of decode order`);
    console.log('  last frames before first violation:');
    for (const f of o.recent) console.log(`    ${fmtFrame(f)}`);
    console.log('  first violations:');
    for (const f of o.examples) console.log(`    ${fmtFrame(f)}`);
    exitCode = 1;
  } else {
    console.log(`order OK: ${o.checked} frames checked, ${o.idr} IDR, 0 decode-order violations`);
  }

  const rtp = state?.rtp;
  // Firefox may not report framesReceived; fall back to frames seen by the checker.
  const received = rtp?.framesReceived ?? o?.frames ?? 0;
  const decodeRatio = received ? (rtp?.framesDecoded ?? 0) / received : 0;
  if (!state?.error && decodeRatio < minDecodeRatio) {
    console.log(
      `FAIL browser decoded ${rtp?.framesDecoded ?? 0}/${received} frames ` +
        `(${(decodeRatio * 100).toFixed(1)}% < ${minDecodeRatio * 100}%), pliCount=${rtp?.pliCount ?? '-'}`,
    );
    exitCode = 1;
  }
  if (exitCode === 0) console.log('PASS');
  if (o?.parseErrors) console.log(`note: ${o.parseErrors} frames could not be parsed`);
  if (o?.gapsAllowed) console.log('note: SPS allows frame_num gaps; check is not meaningful');

  process.exit(exitCode);
}

main().catch((e) => {
  console.error(e);
  process.exit(2);
});
