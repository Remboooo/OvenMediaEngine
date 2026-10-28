#!/usr/bin/env python3
"""Traffic simulator: normal traffic on an imperfect network with imperfect clients.

Runs its own OME (conf/, build/Release binary) behind sim_relay.py and, for
--duration-s, keeps it busy the way an evening of real use does:

  publishers  stream slots that go live and offline at random, stop neatly,
              crash, or lose their uplink without a word; restarts reuse the name
  viewers     arrive at random, mostly zappers and some long sessions, each with
              a protocol (WebRTC, LL-HLS), a rendition, a network class
              (classes.json), maybe a Wi-Fi outage mid-session, a way of leaving
              (polite, tab killed, network vanishes) and some patience to retry

and judges the server, not the clients' luck:

  health      same PID, no crash dump, no unexpected E/C log lines
  sessions    OME never reports more WebRTC sessions than could still exist
              (viewers alive + vanished within Timeout + slack); none at the end
  streams     a live publisher's stream exists; a stopped one disappears
  leaks       after quiescing, threads/fds/RSS are back near the warm baseline

Each check prints PASS/FAIL; exit code 0 only if all pass. Every random choice
comes from --seed, so a run's schedule is reproducible. Output: logs/sim_<ts>/.

Production is never touched. The simulator refuses to start while production
has a live stream and stops early (ABORTED) if one appears during the run.

  ./traffic_sim.py                          # 30 min, defaults
  ./traffic_sim.py --duration-s 300 --seed 7
  ./traffic_sim.py --duration-s 28800       # overnight soak
"""
import argparse
import asyncio
import base64
import collections
import json
import math
import os
import random
import re
import signal
import socket
import statistics
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parent.parent
MEDIA = Path(os.environ.get('OME_SIM_MEDIA', REPO / 'misc' / 'segcache_soak' / 'dogfood' / 'media'))  # make_sources.sh
RTC_TESTER = REPO / 'misc' / 'oven_rtc_tester' / 'OvenRtcTester'
LLHLS_CLIENT = REPO / 'misc' / 'segcache_soak' / 'llhls_client.py'



def lan_ip():
    """This machine's address on the network the desktop browsers use: OME_TEST_LAN_IP, else the source address of
    the default route (a UDP connect sends nothing)."""
    if os.environ.get('OME_TEST_LAN_IP'):
        return os.environ['OME_TEST_LAN_IP']
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect(('192.0.2.1', 9))
        return s.getsockname()[0]


LAN = lan_ip()
os.environ['OME_TEST_LAN_IP'] = LAN  # conf/Server.xml's ICE candidates (${env:OME_TEST_LAN_IP}), sim_relay.py
SIG_PORT = 33333
API = 'http://127.0.0.1:38081/v1'
TOKEN = 'traffic-sim-local'
INGEST = 'rtmp://127.0.0.1:31936'
PROD_CONF = Path('/usr/share/ovenmediaengine/conf/Server.xml')

WEBRTC_TIMEOUT_S = 30  # Publishers/WebRTC/Timeout in conf/Server.xml
SESSION_SLACK_S = 15   # extra time OME may take to notice a vanished viewer

SLOTS = [
    # GPU slot: a production-like ladder (NVENC 1080/720/480) on a 4K60 Constrained Baseline source with a 1 s GOP,
    # the way a hardware encoder or OBS sends it. Real footage (OME_SIM_4K_SRC) is best; make_sources.sh makes a
    # synthetic one. src_span_s: how far into the source a publisher may start.
    dict(id='p0', app='livegpu', stream='gpu4k', src=os.environ.get('OME_SIM_4K_SRC', 'sim_2160p60_baseline.ts'),
         src_span_s=int(os.environ.get('OME_SIM_4K_SPAN_S', '840')),
         renditions=['auto', 'Bypass+opus', '1080p', '720p', '480p'], r_weights=[5, 1, 2, 1, 1],
         hls=['auto', '1080p', '480p']),
    # Other publishers: OBS-like High profile with B-frames, and a 30 fps Main profile.
    dict(id='p1', app='live', stream='s1080', src='sim_1080p60_high.ts', src_span_s=150,
         renditions=['auto', 'Bypass+opus'], r_weights=[7, 3], hls=['auto']),
    dict(id='p2', app='live', stream='s720', src='sim_720p30_main.ts', src_span_s=150,
         renditions=['auto', 'Bypass+opus'], r_weights=[7, 3], hls=['auto']),
]
SLOT_WEIGHTS = [2, 1, 1]
VIEWER_CLASSES = ['fiber', 'residential', 'wifi', 'mobile_4g', 'terrible']
VIEWER_CLASS_WEIGHTS = [30, 35, 20, 10, 5]
UPLINK_CLASSES = ['uplink_clean', 'uplink_jitter', 'uplink_stalls']
UPLINK_WEIGHTS = [6, 3, 1]


def now_s():
    return time.monotonic()


# ===================================================================== plumbing
class Run:
    def __init__(self, args):
        self.args = args
        self.rng = random.Random(args.seed)
        self.t0 = now_s()
        self.out = Path(args.out or ROOT / 'logs' / time.strftime('sim_%Y%m%d_%H%M%S')).resolve()  # OME runs in conf/
        (self.out / 'clients').mkdir(parents=True, exist_ok=True)
        self.events = open(self.out / 'events.jsonl', 'w', buffering=1)
        self.samples = open(self.out / 'samples.jsonl', 'w', buffering=1)
        self.failures = []  # (check, detail)
        self.procs = set()
        self.ome = None
        self.relay = None
        self.ctl = None
        self.ctl_lock = asyncio.Lock()
        self.connect_lock = asyncio.Lock()  # one WebRTC connect at a time, so relay tags are unambiguous
        self.stop = asyncio.Event()         # end of traffic (duration, abort, Ctrl-C)
        self.quiet = asyncio.Event()        # quiesce: publishers and viewers leave now
        self.aborted = None
        self.pubs = {}                      # slot id -> Publisher
        self.viewers = {}                   # viewer id -> Viewer (alive)
        self.vanished = []                  # (t, app, stream, vid) WebRTC viewers whose network vanished
        self.recent_exits = []              # (t, app, stream) WebRTC viewers that left politely or were killed
        self.vid = 0
        self.qoe = collections.defaultdict(lambda: collections.Counter())
        self.lingering_ingest = {}
        self.generations = collections.Counter()
        self.desktop = open(self.out / 'desktop.jsonl', 'w', buffering=1)
        self.desktop_sessions = []

    def t(self):
        return round(now_s() - self.t0, 2)

    def event(self, _kind, **kw):
        kind = _kind
        rec = dict(t=self.t(), ev=kind, **kw)
        self.events.write(json.dumps(rec) + '\n')
        if self.args.verbose or kind.startswith('FAIL'):
            print(f"[{rec['t']:8.1f}] {kind} {json.dumps(kw)}", flush=True)

    def fail(self, check, detail, **kw):
        self.failures.append((check, detail))
        self.event('FAIL', check=check, detail=detail, **kw)

    async def spawn(self, name, *argv, log=None, nice=10, env=None, stdout_pipe=False):
        logf = open(log, 'ab') if log and not stdout_pipe else asyncio.subprocess.DEVNULL
        p = await asyncio.create_subprocess_exec(
            'nice', '-n', str(nice), *map(str, argv),
            stdin=asyncio.subprocess.DEVNULL,
            stdout=asyncio.subprocess.PIPE if stdout_pipe else logf,
            stderr=asyncio.subprocess.STDOUT if (stdout_pipe or log) else asyncio.subprocess.DEVNULL,
            env=env, start_new_session=True)
        p.sim_name = name
        self.procs.add(p)
        return p

    def signal(self, p, sig):
        if p is not None and p.returncode is None:
            try:
                os.kill(p.pid, sig)
            except ProcessLookupError:
                pass

    async def reap(self, p, timeout=10):
        if p is None:
            return None
        try:
            await asyncio.wait_for(p.wait(), timeout)
        except asyncio.TimeoutError:
            self.signal(p, signal.SIGKILL)
            await p.wait()
        self.procs.discard(p)
        return p.returncode

    async def relay_cmd(self, **req):
        async with self.ctl_lock:
            r, w = self.ctl
            w.write((json.dumps(req) + '\n').encode())
            await w.drain()
            return json.loads(await r.readline())


def http_json(url, token=TOKEN, timeout=3):
    req = urllib.request.Request(url, headers={'Authorization': 'Basic ' + base64.b64encode(token.encode()).decode()})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read())
    except urllib.error.HTTPError as e:
        return {'statusCode': e.code}
    except Exception as e:
        return {'statusCode': 0, 'error': str(e)}


async def api(path, **kw):
    return await asyncio.to_thread(http_json, API + path, **kw)


def production_live():
    """Live (non-scheduled) stream count across production's apps (None if its API is unreachable).
    The always-on Scheduled filler channel doesn't count: nobody is broadcasting."""
    try:
        conf = PROD_CONF.read_text()
        token = re.search(r'<AccessToken>([^<]+)', conf).group(1)
        port = re.search(r'<Managers>\s*<API>\s*<Port>(\d+)', conf).group(1)
    except (OSError, AttributeError):
        return None
    base = f'http://127.0.0.1:{port}/v1/vhosts/default/apps'
    apps = http_json(base, token).get('response')
    if apps is None:
        return None
    live = 0
    for a in apps:
        for s in http_json(f'{base}/{a}/streams', token).get('response') or []:
            info = http_json(f'{base}/{a}/streams/{s}', token).get('response') or {}
            live += (info.get('input') or {}).get('sourceType') != 'Scheduled'
    return live


def proc_sample(pid):
    try:
        st = Path(f'/proc/{pid}/status').read_text()
        rss = int(re.search(r'VmRSS:\s+(\d+)', st).group(1)) // 1024
        threads = int(re.search(r'Threads:\s+(\d+)', st).group(1))
        fds = len(os.listdir(f'/proc/{pid}/fd'))
        cpu = sum(int(x) for x in Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()[11:13])
        # Anonymous is the heap; Private_Clean is mapped files (CUDA/NVENC libraries paged in once)
        sm = Path(f'/proc/{pid}/smaps_rollup').read_text()
        anon = int(re.search(r'Anonymous:\s+(\d+)', sm).group(1)) // 1024
        clean = int(re.search(r'Private_Clean:\s+(\d+)', sm).group(1)) // 1024
        return dict(rss_mb=rss, anon_mb=anon, file_mb=clean, threads=threads, fds=fds, cpu_ticks=cpu)
    except (OSError, AttributeError):
        return None


# ===================================================================== publishers
class Publisher:
    def __init__(self, run, slot):
        self.run, self.slot = run, slot
        self.state = 'off'          # off | starting | live | stopping
        self.since = now_s()
        self.proc = None
        self.tag = None
        self.prev_listed = False
        self.gone_by = None         # deadline for the stream to disappear after a stop
        self.stop_style = None

    async def life(self):
        run, rng, s = self.run, self.run.rng, self.slot
        await asyncio.sleep(rng.uniform(0, 20))  # stagger first starts
        while not run.stop.is_set() and not run.quiet.is_set():
            await self.go_live(rng.choices(UPLINK_CLASSES, UPLINK_WEIGHTS)[0])
            live_for = rng.uniform(run.args.live_min_s, run.args.live_max_s)
            await wait_any(run, live_for, self.proc)
            if self.proc.returncode is not None and not run.quiet.is_set():
                await self.died()
            else:
                style = 'neat' if run.quiet.is_set() else rng.choices(['neat', 'kill', 'uplink_dead'], run.args.stop_weights)[0]
                await self.go_offline(style)
            if run.quiet.is_set():
                break
            await wait_any(run, rng.uniform(run.args.off_min_s, run.args.off_max_s))

    async def go_live(self, uplink):
        run, s = self.run, self.slot
        run.generations[s['id']] += 1  # survives quiesce: relay tags must stay unique
        tag = f"{s['id']}g{run.generations[s['id']]}"
        self.tag, self.state, self.since, self.gone_by, self.stop_style = tag, 'starting', now_s(), None, None
        # Start exactly on a keyframe. Past one, ffmpeg -ss with -c copy drops video until the next
        # keyframe (up to a GOP: 8 s on s720), and its muxer holds the audio back until video comes,
        # so nothing reaches OME and a PacketSilenceTimeoutMs reaps it. A live encoder has no such gap.
        # Rounded down to the ms: a hair before a keyframe still starts on it.
        offset = math.floor(run.rng.choice([k for k in keyframes(s['src']) if k <= s['src_span_s']]) * 1000) / 1000
        listed = await app_streams(s['app'])
        self.prev_listed = listed is not None and s['stream'] in listed
        async with run.connect_lock:
            await run.relay_cmd(cmd='expect_ingest', id=tag, cls=uplink)
            self.proc = await run.spawn(
                tag, 'ffmpeg', '-hide_banner', '-nostdin', '-loglevel', 'warning', '-re', '-stream_loop', '-1',
                '-ss', f'{offset:.3f}', '-i', MEDIA / s['src'], '-c', 'copy', '-bsf:a', 'aac_adtstoasc',
                '-f', 'flv', f"{INGEST}/{s['app']}/{s['stream']}",
                log=run.out / 'clients' / f'{tag}.log', nice=5)
            await asyncio.sleep(1.0)
        self.state, self.since = 'live', now_s()
        run.event('publish', slot=s['id'], tag=tag, uplink=uplink, offset=offset)

    async def go_offline(self, style):
        run = self.run
        self.state, self.since, self.stop_style = 'stopping', now_s(), style
        run.event('unpublish', slot=self.slot['id'], tag=self.tag, style=style)
        if style == 'neat':
            run.signal(self.proc, signal.SIGINT)
            await run.reap(self.proc, 10)
            self.gone_by = now_s() + 10
        elif style == 'kill':
            run.signal(self.proc, signal.SIGKILL)
            await run.reap(self.proc)
            self.gone_by = now_s() + 10
        else:
            # The encoder's uplink dies; the encoder then gives up and exits. OME only
            # learns from the silence. Restore the path after the limit so a stream
            # OME never noticed doesn't block this slot for the rest of the run.
            await run.relay_cmd(cmd='blackhole', id=self.tag)
            await asyncio.sleep(run.rng.uniform(2, 8))
            run.signal(self.proc, signal.SIGKILL)
            await run.reap(self.proc)
            # Judged in check_streams when the limit passes (the slot may publish again before then).
            run.lingering_ingest[self.tag] = (self.slot, now_s() + run.args.ingest_dead_limit_s)
        self.state = 'off'

    async def died(self):
        """The encoder exited on its own. Right after connecting, that means OME refused the publish."""
        run, s = self.run, self.slot
        rc, alive_s = self.proc.returncode, now_s() - self.since
        await run.reap(self.proc)
        self.state, self.since, self.gone_by = 'off', now_s(), None
        held_by = [tag for tag, (slot, _) in run.lingering_ingest.items() if slot is s] if self.prev_listed else []
        run.event('publisher_died', slot=s['id'], tag=self.tag, rc=rc, alive_s=round(alive_s, 1), held_by=held_by)
        if alive_s < 15:
            why = (f"while {', '.join(held_by)}'s dead uplink still held the stream" if held_by
                   else 'while the previous stream was still listed' if self.prev_listed
                   else 'with no previous stream listed (reaped, or refused for another reason: see the OME log)')
            run.fail('streams', f"publish {self.tag} refused {alive_s:.1f}s after connecting (ffmpeg rc={rc}) {why}")


_KEYFRAMES = {}


def keyframes(src):
    """Video keyframe offsets (s from the file's start) of a source, cached next to it."""
    if src not in _KEYFRAMES:
        path = MEDIA / src
        cache = path.with_name(path.name + '.keyframes.json')
        if cache.exists() and cache.stat().st_mtime >= path.stat().st_mtime:
            _KEYFRAMES[src] = json.loads(cache.read_text())
        else:
            import subprocess
            out = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
                                  'packet=pts_time,flags', '-of', 'csv=p=0', str(path)],
                                 capture_output=True, text=True, check=True).stdout
            pts = [float(t) for t, f in (l.split(',', 1) for l in out.splitlines() if ',' in l) if 'K' in f and t]
            start = min(pts)
            _KEYFRAMES[src] = sorted(round(t - start, 3) for t in pts)
            cache.write_text(json.dumps(_KEYFRAMES[src]))
    return _KEYFRAMES[src]


async def wait_any(run, seconds, proc=None):
    """Sleep up to `seconds`, waking early on stop/quiesce or when `proc` exits."""
    waits = [asyncio.ensure_future(run.stop.wait()), asyncio.ensure_future(run.quiet.wait())]
    if proc is not None:
        waits.append(asyncio.ensure_future(proc.wait()))
    try:
        await asyncio.wait(waits, timeout=seconds, return_when=asyncio.FIRST_COMPLETED)
    finally:
        for w in waits:
            w.cancel()


# ===================================================================== viewers
class Viewer:
    def __init__(self, run):
        run.vid += 1
        rng = run.rng
        self.run = run
        self.id = f'v{run.vid}'
        self.slot = rng.choices(SLOTS, SLOT_WEIGHTS)[0]
        self.proto = rng.choices(['webrtc', 'llhls'], [7, 3])[0]
        if self.proto == 'webrtc':
            self.rendition = rng.choices(self.slot['renditions'], self.slot['r_weights'])[0]
        else:
            self.rendition = rng.choice(self.slot['hls'])
        self.cls = rng.choices(VIEWER_CLASSES, VIEWER_CLASS_WEIGHTS)[0]
        kind = rng.choices(['zapper', 'medium', 'long'], [6, 3, 1])[0]
        self.kind = kind
        self.life = {'zapper': rng.uniform(5, 30), 'medium': rng.uniform(60, 300), 'long': rng.uniform(600, 1800)}[kind]
        exits = ['polite', 'kill', 'vanish'] if self.proto == 'webrtc' else ['polite', 'kill']
        self.exit = rng.choices(exits, [5, 3, 2][:len(exits)])[0]
        self.outage = None
        if self.proto == 'webrtc' and self.cls in ('wifi', 'mobile_4g', 'terrible') and rng.random() < 0.3:
            self.outage = (rng.uniform(0.2, 0.8) * self.life, rng.uniform(2, 8))
        self.patience = rng.choice([1, 3, 1000])  # attempts before giving up on an offline stream
        self.proc = None
        self.attempt = 0
        self.connected_once = False

    def desc(self):
        return dict(id=self.id, stream=self.slot['stream'], proto=self.proto, rendition=self.rendition,
                    cls=self.cls, kind=self.kind, life=round(self.life, 1), exit=self.exit,
                    outage=[round(x, 1) for x in self.outage] if self.outage else None)

    async def live(self):
        run = self.run
        run.event('viewer_join', **self.desc())
        run.viewers[self.id] = self
        self.t_start = now_s()
        end = self.t_start + self.life
        failures = 0
        try:
            while now_s() < end and not run.quiet.is_set() and not run.stop.is_set():
                self.attempt += 1
                ok = await (self.webrtc_attempt(end) if self.proto == 'webrtc' else self.llhls_attempt(end))
                if ok == 'left':
                    return
                failures += 0 if ok == 'played' else 1
                if failures >= self.patience:
                    run.qoe[self.cls]['gave_up'] += 1
                    run.event('viewer_gave_up', id=self.id, attempts=self.attempt)
                    return
                await wait_any(run, run.rng.uniform(2, 5))
        finally:
            run.viewers.pop(self.id, None)
            run.qoe[self.cls]['viewers'] += 1
            if self.connected_once:
                run.qoe[self.cls]['played'] += 1

    def url(self):
        s = self.slot
        if self.proto == 'webrtc':
            q = '' if self.rendition == 'auto' else f'?rendition={urllib.request.quote(self.rendition)}'
            return f"ws://{LAN}:{SIG_PORT}/{s['app']}/{s['stream']}/auto{q}"
        return f"http://127.0.0.1:{self.run.http_ports[self.cls]}/{s['app']}/{s['stream']}/{self.rendition}.m3u8"

    async def webrtc_attempt(self, end):
        """'played' / 'failed' / 'left' (the viewer's life ended while connected or trying)."""
        run, s = self.run, self.slot
        tag = f'{self.id}a{self.attempt}'
        log = run.out / 'clients' / f'{tag}.log'
        async with run.connect_lock:
            await run.relay_cmd(cmd='expect_viewer', id=tag, cls=self.cls)
            self.proc = p = await run.spawn(tag, RTC_TESTER, '-url', self.url(), '-sint', '2000',
                                            log=log, stdout_pipe=True)
            state = Watch(p)
            reader = asyncio.ensure_future(state.read(run, log))
            # Hold the lock until this client's ICE flows exist (or it failed), so the
            # next client's flows can't be tagged as this one's.
            t_conn = now_s()
            while now_s() - t_conn < 5 and state.ice is None and not state.failed and p.returncode is None:
                await asyncio.sleep(0.1)
        run.event('webrtc_connect', id=self.id, tag=tag, url=self.url(), ice=state.ice, failed=state.failed)
        outage_at = self.t_start + self.outage[0] if self.outage else None
        result = 'failed'
        try:
            while True:
                t = now_s()
                if run.quiet.is_set() or run.stop.is_set() or t >= end:
                    result = 'left'
                    await self.leave(tag, p, run.quiet.is_set() or run.stop.is_set())
                    break
                if p.returncode is not None or state.failed:
                    break
                if outage_at and t >= outage_at:
                    outage_at = None
                    run.event('viewer_outage', id=self.id, tag=tag, seconds=round(self.outage[1], 1))
                    await run.relay_cmd(cmd='blackhole', id=tag)
                    await wait_any(run, self.outage[1])
                    await run.relay_cmd(cmd='release', id=tag)
                if state.first_media is not None:
                    if not self.connected_once:
                        run.qoe[self.cls]['ttff_ms_sum'] += int(1000 * (state.first_media - t_conn))
                        run.qoe[self.cls]['ttff_n'] += 1
                    self.connected_once = True
                    result = 'played'
                    # A player whose picture froze reloads; OvenPlayer users do the same by hand.
                    if t - state.last_progress > 10:
                        pub = run.pubs.get(s['id'])
                        ended = pub is None or pub.state != 'live' or pub.since > state.last_progress
                        run.qoe[self.cls]['stream_ended' if ended else 'freeze_reload'] += 1
                        run.event('viewer_frozen', id=self.id, tag=tag, packets=state.packets, stream_ended=ended)
                        await self.leave(tag, p, True)
                        result = 'failed'
                        break
                elif t - t_conn > 15:
                    run.event('viewer_no_media', id=self.id, tag=tag)
                    await self.leave(tag, p, True)
                    break
                await asyncio.sleep(0.5)
        finally:
            await run.reap(p, 5)
            reader.cancel()
            if not run.args.keep_client_logs and result != 'failed':
                log.unlink(missing_ok=True)
            run.recent_exits.append((now_s(), s['app'], s['stream']))
            await run.relay_cmd(cmd='forget', id=tag)
        return result

    async def leave(self, tag, p, polite):
        run, s = self.run, self.slot
        style = 'polite' if polite else self.exit
        run.event('viewer_leave', id=self.id, tag=tag, style=style)
        if style == 'polite':
            run.signal(p, signal.SIGINT)
        elif style == 'kill':
            run.signal(p, signal.SIGKILL)
        else:
            await run.relay_cmd(cmd='blackhole', id=tag)
            run.vanished.append((now_s(), s['app'], s['stream'], tag))
            # The user stares at the frozen picture for a while, then closes the tab.
            await asyncio.sleep(run.rng.uniform(2, 20))
            run.signal(p, signal.SIGKILL)

    async def llhls_attempt(self, end):
        run = self.run
        tag = f'{self.id}a{self.attempt}'
        log = run.out / 'clients' / f'{tag}.log'
        dur = max(1.0, end - now_s())
        p = await run.spawn(tag, sys.executable, '-u', LLHLS_CLIENT, self.url(), '--duration-s', f'{dur:.1f}',
                            '--stall-s', '12', log=log)
        self.proc = p
        run.event('llhls_start', id=self.id, tag=tag, url=self.url())
        await wait_any(run, dur + 30, p)
        if p.returncode is None:
            run.signal(p, signal.SIGINT if self.exit == 'polite' else signal.SIGKILL)
        rc = await run.reap(p, 5)
        text = log.read_text(errors='replace') if log.exists() else ''
        played = 'OK master' in text and 'FAIL' not in text
        run.event('llhls_end', id=self.id, tag=tag, rc=rc, fail=(re.findall(r'^FAIL.*$', text, re.M) or [None])[0])
        if played or rc == 0:
            self.connected_once = True
        if 'FAIL stall' in text:
            run.qoe[self.cls]['hls_stall'] += 1
        if played and not run.args.keep_client_logs:
            log.unlink(missing_ok=True)
        if now_s() >= end - 1 or run.quiet.is_set() or run.stop.is_set():
            return 'left'
        return 'played' if played else 'failed'


ORACLE = REPO / 'misc' / 'segcache_soak' / 'hlsjs_soak' / 'webrtc_oracle.mjs'
CONTROL_PORT = 19220  # misc/remote_viewer/Setup-RemoteViewer.ps1's control service on a desktop


class DesktopViewer:
    """A real browser on a desktop (Chrome over CDP, Firefox over WebDriver BiDi) that watches back to back:
    joins a stream with a rendition and network class, maybe has an outage, leaves politely or by vanishing.
    One session at a time per browser. Each session is judged afterwards (webrtc_impair/viewer_check.py).
    The oracle is only killed as a last resort: that would leave its window open on the desktop."""

    def __init__(self, run, name, endpoint):
        self.run, self.name, self.endpoint = run, name, endpoint
        self.env = dict(os.environ, **({'OME_SOAK_BIDI': endpoint} if endpoint.startswith('ws') else {'OME_SOAK_CDP': endpoint}))
        self.n = 0
        self.fast_failures = 0

    async def life(self):
        run = self.run
        while not run.stop.is_set() and not run.quiet.is_set():
            # Joins live streams only (the cheap viewers cover offline ones)
            live = [s for s in SLOTS if (p := run.pubs.get(s['id'])) is not None and p.state == 'live']
            if not live:
                await wait_any(run, 2)
                continue
            rec = await self.session(run.rng.choices(live, [SLOT_WEIGHTS[SLOTS.index(s)] for s in live])[0])
            # Firefox allows one remote session at a time and frees it a moment after the last one ended
            failed_fast = any(e[1] == 'oracle_exit' for e in rec['events']) and rec['secs'] < 10
            self.fast_failures = self.fast_failures + 1 if failed_fast else 0
            if self.fast_failures >= 3:
                # The browser is stuck (e.g. a killed client left Firefox's one remote session taken):
                # the desktop's control service (Setup-RemoteViewer.ps1) restarts the test browser
                self.fast_failures = 0
                res = await asyncio.to_thread(self.restart_browser)
                run.event('desktop_restart', browser=self.name, result=res)
                print(f"[{run.t():8.1f}] desktop {self.name}: browser stuck, restart: {res}", flush=True)
            await wait_any(run, 15 if failed_fast else run.rng.uniform(2, 10))

    def restart_browser(self):
        u = urllib.parse.urlsplit(self.endpoint)
        which = 'firefox' if u.scheme.startswith('ws') else 'chrome'
        req = urllib.request.Request(f'http://{u.hostname}:{CONTROL_PORT}/restart/{which}', method='POST', data=b'')
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                return json.loads(resp.read()).get('answering')
        except Exception as e:
            return f'failed: {e}'

    async def session(self, slot):
        run, rng = self.run, self.run.rng
        self.n += 1
        v = Viewer(run)  # its draws: network class, outage; overridden below where a desktop differs
        v.slot = slot
        v.id = f'{self.name}_{self.n}'
        v.proto = 'webrtc'
        v.rendition = rng.choices(v.slot['renditions'], v.slot['r_weights'])[0]
        v.kind = rng.choices(['zapper', 'medium', 'long'], [2, 6, 2])[0]
        v.life = {'zapper': rng.uniform(15, 40), 'medium': rng.uniform(60, 240), 'long': rng.uniform(360, 720)}[v.kind]
        v.exit = rng.choices(['polite', 'vanish'], [7, 3])[0]
        v.outage = None
        if v.cls in ('wifi', 'mobile_4g', 'terrible') and rng.random() < 0.3:
            v.outage = (rng.uniform(0.2, 0.8) * v.life, rng.uniform(2, 8))
        d = run.out / 'desktop' / v.id
        d.mkdir(parents=True, exist_ok=True)
        rec = dict(v.desc(), browser=self.name, endpoint=self.endpoint)
        run.event('desktop_join', **rec)
        run.viewers[v.id] = v
        t0 = now_s()
        pub = run.pubs.get(v.slot['id'])
        rec['stream_live_at_join'] = pub is not None and pub.state == 'live'
        rec['events'] = []
        style = None
        try:
            async with run.connect_lock:
                await run.relay_cmd(cmd='expect_viewer', id=v.id, cls=v.cls)
                await run.relay_cmd(cmd='trace', id=v.id, path=str(d / 'trace.csv'))
                v.proc = p = await run.spawn(v.id, 'node', ORACLE, '--ws', v.url(), '--out', d, '--duration-s', v.life + 60,
                                             '--no-transform', log=d / 'oracle.log', nice=0, env=self.env)
                # Hold the lock until this browser's ICE flow exists: the relay tags new flows with the last expect
                while now_s() - t0 < 12 and p.returncode is None:
                    st = (await run.relay_cmd(cmd='stats'))['clients'].get(v.id, {})
                    if st.get('udp_flows'):
                        break
                    await asyncio.sleep(0.2)
            presented = d / 'presented.jsonl'
            end = t0 + v.life
            outage_at = t0 + v.outage[0] if v.outage else None
            last_size, last_growth, first = 0, now_s(), None
            while True:
                t = now_s()
                if p.returncode is not None:
                    rec['events'].append([round(t - t0, 1), 'oracle_exit', p.returncode])
                    break
                if run.quiet.is_set() or run.stop.is_set() or t >= end:
                    style = 'polite' if run.quiet.is_set() or run.stop.is_set() else v.exit
                    break
                if outage_at and t >= outage_at:
                    outage_at = None
                    rec['events'].append([round(t - t0, 1), 'outage', round(v.outage[1], 1)])
                    run.event('desktop_outage', id=v.id, seconds=round(v.outage[1], 1))
                    await run.relay_cmd(cmd='blackhole', id=v.id)
                    await wait_any(run, v.outage[1])
                    await run.relay_cmd(cmd='release', id=v.id)
                    last_growth = now_s()
                size = presented.stat().st_size if presented.exists() else 0
                if size > last_size:
                    last_size, last_growth = size, t
                    if first is None:
                        first = t
                        rec['join_s'] = round(t - t0, 2)
                elif first is not None and t - last_growth > 10:
                    # Frozen picture: a real viewer reloads
                    pub = run.pubs.get(v.slot['id'])
                    ended = pub is None or pub.state != 'live' or pub.since > now_s() - (t - last_growth)
                    rec['events'].append([round(t - t0, 1), 'stream_ended' if ended else 'freeze_reload'])
                    run.event('desktop_frozen', id=v.id, stream_ended=ended)
                    style = 'polite'
                    break
                elif first is None and t - t0 > 20:
                    rec['events'].append([round(t - t0, 1), 'no_picture'])
                    style = 'polite'
                    break
                await asyncio.sleep(0.5)
            rec['leave'] = style
            run.event('desktop_leave', id=v.id, style=style)
            if style == 'vanish':
                await run.relay_cmd(cmd='blackhole', id=v.id)
                run.vanished.append((now_s(), v.slot['app'], v.slot['stream'], v.id))
                await asyncio.sleep(rng.uniform(2, 20))
            run.signal(p, signal.SIGTERM)
            # The oracle closes its own context within 15 s of SIGTERM; reap's SIGKILL after 30 s is the last
            # resort, which may leave its window open on the desktop and, in Firefox, the one remote session taken
            if await run.reap(p, 30) == -signal.SIGKILL:
                rec['events'].append([round(now_s() - t0, 1), 'oracle_killed'])
        finally:
            run.viewers.pop(v.id, None)
            if style != 'vanish':
                run.recent_exits.append((now_s(), v.slot['app'], v.slot['stream']))
            await run.relay_cmd(cmd='trace_stop', id=v.id)
            await run.relay_cmd(cmd='forget', id=v.id)
        rec['secs'] = round(now_s() - t0, 1)
        try:
            rec['judged'] = await asyncio.to_thread(judge_desktop, d)
        except Exception as e:  # a judging bug must not end the soak; it shows in the report
            rec['judged'] = dict(ok=False, error=f'judge: {e!r}')
        (d / 'trace.csv').unlink(missing_ok=True)  # large; the judged numbers are kept
        run.desktop.write(json.dumps(rec) + '\n')
        run.desktop_sessions.append(rec)
        return rec


def judge_desktop(d):
    sys.path.insert(0, str(REPO / 'misc' / 'webrtc_impair'))
    import viewer_check
    trace = d / 'trace.csv'
    return viewer_check.check(d, trace if trace.exists() else None)


class Watch:
    """Follows an OvenRtcTester's output: ICE state, packets received over time."""
    PKTS = re.compile(r'Total Packets\((\d+)\)')

    def __init__(self, proc):
        self.proc = proc
        self.ice = None
        self.failed = False
        self.packets = 0
        self.first_media = None
        self.last_progress = now_s()

    async def read(self, run, log):
        with open(log, 'ab') as f:
            async for line in self.proc.stdout:
                f.write(line)
                s = line.decode(errors='replace')
                if 'connection state has changed' in s:
                    self.ice = s.rsplit(' ', 2)[-2]
                elif 'track has started' in s:
                    if self.first_media is None:  # pion fires OnTrack on a track's first RTP packet
                        self.first_media = self.last_progress = now_s()
                elif 'failed to run' in s:
                    self.failed = True
                else:
                    m = self.PKTS.search(s)
                    if m:
                        n = int(m.group(1))
                        if n > self.packets:
                            self.packets = n
                            self.last_progress = now_s()


async def viewer_arrivals(run):
    a = run.args
    rng = run.rng
    mean_life = 0.6 * 17.5 + 0.3 * 180 + 0.1 * 1200
    interval = mean_life / a.viewers
    tasks = set()
    ncpu = os.cpu_count() or 4
    while not run.stop.is_set() and not run.quiet.is_set():
        await wait_any(run, rng.expovariate(1 / interval))
        if run.stop.is_set() or run.quiet.is_set():
            break
        if len(run.viewers) >= a.max_viewers:
            continue
        if os.getloadavg()[0] > ncpu * 0.9:
            run.event('arrival_skipped_load', load=os.getloadavg()[0])
            continue
        v = Viewer(run)
        t = asyncio.ensure_future(v.live())
        tasks.add(t)
        t.add_done_callback(tasks.discard)
        t.add_done_callback(lambda t: sim_crash(run, t))
    if tasks:
        await asyncio.gather(*tasks, return_exceptions=True)


def sim_crash(run, task):
    """A simulator bug must not pass silently as fewer clients."""
    if task.cancelled() or task.exception() is None:
        return
    import traceback
    tb = ''.join(traceback.format_exception(task.exception()))
    print(tb, file=sys.stderr, flush=True)
    run.fail('simulator', tb.strip().splitlines()[-1])


# ===================================================================== checks
async def monitor(run):
    """Every 5 s: sessions vs. what can exist, streams vs. publishers, resources."""
    a = run.args
    last_prod = 0
    while not run.stop.is_set():
        await asyncio.sleep(5)
        t = now_s()
        if run.ome.returncode is not None:
            run.fail('server alive', f'OME exited rc={run.ome.returncode}')
            run.aborted = 'OME died'
            run.stop.set()
            return
        if not a.allow_production_live and t - last_prod > 30:
            last_prod = t
            live = await asyncio.to_thread(production_live)
            if live:
                run.aborted = f'production went live ({live} stream(s))'
                run.event('ABORT', reason=run.aborted)
                print(f'ABORT: {run.aborted}; ending the run', flush=True)
                run.stop.set()
                return
        await check_streams(run, t)
        await check_sessions(run, t)
        sample = proc_sample(run.ome.pid)
        relay = await run.relay_cmd(cmd='stats')
        run.samples.write(json.dumps(dict(t=run.t(), ome=sample, viewers=len(run.viewers),
                                          live=[p.slot['stream'] for p in run.pubs.values() if p.state == 'live'],
                                          relay_lag_p99_ms=relay.get('loop_lag_p99_ms'), relay_flows=relay.get('flows'),
                                          load=os.getloadavg()[0])) + '\n')
        run.relay_lag_max = max(getattr(run, 'relay_lag_max', 0), relay.get('loop_lag_p99_ms') or 0)


async def app_streams(app):
    r = await api(f'/vhosts/default/apps/{app}/streams')
    return set(r.get('response') or []) if r.get('statusCode') == 200 else None


async def check_streams(run, t):
    run.listed = {}
    for app in sorted({s['app'] for s in SLOTS}):
        present = await app_streams(app)
        if present is None:
            continue
        run.listed[app] = present
        for p in run.pubs.values():
            s = p.slot
            if s['app'] != app:
                continue
            if p.state == 'live' and t - p.since > run.args.stream_ready_s and s['stream'] not in present \
                    and p.proc is not None and p.proc.returncode is None:
                key = ('missing', p.tag)
                if key not in run.reported:
                    run.reported.add(key)
                    run.fail('streams', f"{s['stream']} not listed {t - p.since:.0f}s after {p.tag} went live")
            if p.state == 'off' and p.gone_by and s['stream'] in present and t > p.gone_by:
                key = ('linger', p.tag)
                if key not in run.reported:
                    run.reported.add(key)
                    run.fail('streams', f"{s['stream']} still listed {t - p.since:.0f}s after {p.stop_style} stop of {p.tag}")
    # Uplinks that died: release the relay's hold once the limit passed (see Publisher.go_offline).
    for tag, (slot, deadline) in list(run.lingering_ingest.items()):
        if t > deadline:
            del run.lingering_ingest[tag]
            pub = run.pubs.get(slot['id'])
            # A newer generation of the slot owns whatever is listed now; its own deadline judges it.
            if slot['stream'] in run.listed.get(slot['app'], ()) and not (pub and pub.tag != tag):
                run.fail('streams', f"{slot['stream']}: OME still holds the stream {run.args.ingest_dead_limit_s:.0f}s after "
                         f"{tag}'s uplink died silently (zombie publisher; a reconnecting encoder is refused)")
            await run.relay_cmd(cmd='release', id=tag)
            await run.relay_cmd(cmd='forget', id=tag)


async def check_sessions(run, t):
    grace = WEBRTC_TIMEOUT_S + SESSION_SLACK_S
    run.vanished = [v for v in run.vanished if t - v[0] < grace]
    run.recent_exits = [e for e in run.recent_exits if t - e[0] < 10]
    for s in SLOTS:
        if s['stream'] not in getattr(run, 'listed', {}).get(s['app'], ()):
            continue  # offline: OME logs every API 404 as an error, so don't ask
        r = await api(f"/stats/current/vhosts/default/apps/{s['app']}/streams/{s['stream']}")
        if r.get('statusCode') != 200:
            continue
        conns = (r.get('response') or {}).get('connections', {})
        reported = conns.get('webrtc', 0)
        alive = sum(1 for v in run.viewers.values()
                    if v.slot is s and v.proto == 'webrtc' and v.proc is not None and v.proc.returncode is None)
        dark = sum(1 for v in run.vanished if v[1] == s['app'] and v[2] == s['stream'])
        recent = sum(1 for e in run.recent_exits if e[1] == s['app'] and e[2] == s['stream'])
        bound = alive + dark + recent
        run.session_peak = max(getattr(run, 'session_peak', 0), reported)
        if reported > bound:
            key = ('zombie', s['stream'], reported - bound)
            if key not in run.reported:
                run.reported.add(key)
                run.fail('sessions', f"{s['stream']}: OME reports {reported} WebRTC sessions, at most {bound} can exist "
                         f"(alive {alive}, vanished <{grace}s {dark}, just left {recent})")


def scan_log(run, start_offset):
    """E/C lines in the sim OME's log since start, minus allowlist.txt patterns."""
    log = ROOT / 'logs' / 'ovenmediaengine.log'
    allow = [re.compile(l.strip()) for l in (ROOT / 'allowlist.txt').read_text().splitlines()
             if l.strip() and not l.startswith('#')]
    bad, allowed = collections.Counter(), collections.Counter()
    try:
        with open(log, 'rb') as f:
            f.seek(start_offset)
            pending = None
            for raw in f:
                line = raw.decode(errors='replace').rstrip()
                if pending is not None:
                    # http_exchange logs the error with an empty message; request/response follow.
                    if line.startswith('[Response]'):
                        line = pending + ' ' + re.sub(r'\(.*', '', line)
                        pending = None
                    elif line.startswith('[Request]'):
                        pending += ' ' + re.sub(r'\?.*|\(.*', '', line)
                        continue
                    else:
                        pending = None
                if not re.match(r'^\[[^\]]+\] [EC] ', line):
                    continue
                if line.endswith('|') and 'http_exchange' in line:
                    pending = line
                    continue
                msg = re.sub(r'^\[[^\]]+\] [EC] \[[^\]]+\] ', '', line)
                norm = re.sub(r'0x[0-9a-f]+|\d+', '#', msg)[:200]
                (allowed if any(p.search(line) for p in allow) else bad)[norm] += 1
    except OSError:
        pass
    return bad, allowed


async def quiesce(run, label):
    """Everyone leaves; wait until OME should have forgotten them all; sample."""
    run.quiet.set()
    run.event('quiesce_start', label=label)
    await asyncio.gather(*run.pub_tasks, return_exceptions=True)
    if run.arrivals:
        await asyncio.gather(run.arrivals, return_exceptions=True)
    await asyncio.gather(*run.desktop_tasks, return_exceptions=True)
    for tag in list(run.lingering_ingest):
        await run.relay_cmd(cmd='release', id=tag)
    run.lingering_ingest.clear()
    deadline = now_s() + WEBRTC_TIMEOUT_S + SESSION_SLACK_S
    while now_s() < deadline:
        await asyncio.sleep(5)
        await check_streams(run, now_s())
        await check_sessions(run, now_s())

    async def connections():
        totals = await api('/stats/current/vhosts/default')
        return {k: v for k, v in ((totals.get('response') or {}).get('connections') or {}).items() if v}

    conns = await connections()
    # Leftovers: a slow expiry drains, a lost decrement doesn't. Watch a little longer to tell which.
    waited = 0
    while conns and waited < 60:
        await asyncio.sleep(10)
        waited += 10
        conns = await connections()
    streams = {app: sorted(await app_streams(app) or []) for app in {s['app'] for s in SLOTS}}
    await asyncio.sleep(5)  # let freed pools settle before sampling
    sample = proc_sample(run.ome.pid)
    run.event('quiesce_done', label=label, connections=conns, streams=streams, sample=sample)
    if conns:
        run.fail('sessions', f'{label}: {conns} sessions still counted {WEBRTC_TIMEOUT_S + SESSION_SLACK_S + waited}s '
                 f'after the last viewer left (streams: {streams})')
    if any(streams.values()):
        run.fail('streams', f'{label}: streams left with no publishers: {streams}')
    run.quiet.clear()
    return sample


def start_traffic(run):
    run.pubs = {s['id']: Publisher(run, s) for s in SLOTS if not (s['app'] == 'livegpu' and run.args.no_gpu)}
    run.pub_tasks = [asyncio.ensure_future(p.life()) for p in run.pubs.values()]
    for t in run.pub_tasks:
        t.add_done_callback(lambda t: sim_crash(run, t))
    run.arrivals = asyncio.ensure_future(viewer_arrivals(run))
    run.desktop_tasks = []
    for spec in run.args.desktop:
        name, endpoint = spec.split('=', 1)
        t = asyncio.ensure_future(DesktopViewer(run, name, endpoint).life())
        t.add_done_callback(lambda t: sim_crash(run, t))
        run.desktop_tasks.append(t)


# ===================================================================== main
async def main_async(args):
    if not args.allow_production_live:
        live = await asyncio.to_thread(production_live)
        if live:
            print(f'production has {live} live stream(s); refusing (use --allow-production-live)', file=sys.stderr)
            return 2
    for need in [RTC_TESTER, LLHLS_CLIENT, args.bin] + [MEDIA / s['src'] for s in SLOTS]:
        if not Path(need).exists():
            print(f'missing {need}', file=sys.stderr)
            return 2
    if (await api('/vhosts')).get('statusCode') != 0:
        print('something already answers on the sim API port 38081 (another sim running?)', file=sys.stderr)
        return 2

    for s in SLOTS:
        keyframes(s['src'])
    run = Run(args)
    run.reported = set()
    print(f'traffic sim seed={args.seed} duration={args.duration_s}s viewers~{args.viewers} gpu={not args.no_gpu} out={run.out}',
          flush=True)
    (ROOT / 'logs').mkdir(exist_ok=True)
    ome_log = ROOT / 'logs' / 'ovenmediaengine.log'
    log_offset = ome_log.stat().st_size if ome_log.exists() else 0
    conf = make_conf(args)
    run.dumps_dir = conf / 'dumps'
    dumps_before = set(os.listdir(run.dumps_dir)) if run.dumps_dir.exists() else set()
    control = ROOT / 'logs' / 'relay.sock'

    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, lambda: (setattr(run, 'aborted', 'interrupted'), run.stop.set()))

    os.chdir(conf)  # Logger.xml's ../logs is relative to the working directory
    run.ome = await run.spawn('ome', args.bin, '-c', conf, log=run.out / 'ome_stdout.log', nice=args.ome_nice)
    run.relay = await run.spawn('relay', sys.executable, '-u', ROOT / 'sim_relay.py', '--classes', ROOT / 'classes.json',
                                '--control', control, '--seed', args.seed, log=run.out / 'relay.log', nice=args.ome_nice)
    try:
        for _ in range(60):
            await asyncio.sleep(0.5)
            if control.exists() and (await api('/vhosts')).get('statusCode') == 200:
                break
        else:
            print('sim OME or relay did not come up; see', run.out, file=sys.stderr)
            return 2
        run.ctl = await asyncio.open_unix_connection(str(control))
        run.http_ports = (await run.relay_cmd(cmd='ports'))['http']
        await asyncio.sleep(2)
        idle = proc_sample(run.ome.pid)
        run.event('baseline_idle', sample=idle)

        mon = asyncio.ensure_future(monitor(run))
        warm = None
        warm_until = now_s() + min(args.warmup_s, args.duration_s / 3)
        end = now_s() + args.duration_s
        start_traffic(run)
        await wait_until(run, warm_until)
        if not run.stop.is_set():
            warm = await quiesce(run, 'warm-up')
            start_traffic(run)
        await wait_until(run, end)
        final = await quiesce(run, 'final') if run.ome.returncode is None else None
        run.stop.set()
        await asyncio.gather(mon, return_exceptions=True)
        return report(run, idle, warm, final, log_offset, dumps_before)
    finally:
        for p in list(run.procs):
            run.signal(p, signal.SIGTERM if p is run.ome else signal.SIGKILL)
        for p in list(run.procs):
            await run.reap(p, 15)


def make_conf(args):
    """conf/ as is, or a copy with config overrides (conf_override/, next to conf/ so ../logs still resolves)."""
    if args.rtmp_silence_ms is None and args.rtmp_first_media_ms is None and args.playout_delay is None \
            and not args.no_switches:
        return ROOT / 'conf'
    out = ROOT / 'conf_override'
    out.mkdir(exist_ok=True)
    xml = (ROOT / 'conf' / 'Server.xml').read_text()
    opts = ''.join(f'<{k}>{v}</{k}>' for k, v in (('PacketSilenceTimeoutMs', args.rtmp_silence_ms),
                                                   ('FirstMediaWaitTimeoutMs', args.rtmp_first_media_ms)) if v is not None)
    if opts:
        xml = re.sub(r'(<Providers>\s*)<RTMP(?: />|>.*?</RTMP>)', rf'\1<RTMP>{opts}</RTMP>', xml, flags=re.S)
        print(f'conf override: RTMP {opts} in {xml.count(opts)} app(s)')
    if args.playout_delay is not None:
        lo, hi = args.playout_delay.split(':')
        pd = f'<PlayoutDelay><Min>{lo}</Min><Max>{hi}</Max>' + ('<Adaptive>true</Adaptive>' if args.adaptive_playout else '') + '</PlayoutDelay>'
        # Applications' publishers only (<Bind> has a <Publishers><WebRTC> too)
        xml = re.sub(r'<Application>.*?</Application>', lambda m: re.sub(r'(<Publishers>.*?<WebRTC>\s*)', rf'\1{pd}', m.group(0), flags=re.S),
                     xml, flags=re.S)
        print(f'conf override: WebRTC {pd} in {xml.count(pd)} app(s)')
    if args.no_switches:
        # A build without them (e.g. production's) refuses the elements
        xml, n = re.subn(r'[ \t]*<(JitterBufferDelay|SendPacing|VideoShedding)>.*?</\1>\n', '', xml, flags=re.S)
        print(f'conf override: {n} switch element(s) removed')
    (out / 'Server.xml').write_text(xml)
    (out / 'Logger.xml').write_text((ROOT / 'conf' / 'Logger.xml').read_text())
    return out


async def wait_until(run, deadline):
    while now_s() < deadline and not run.stop.is_set():
        await asyncio.sleep(min(1.0, max(0.0, deadline - now_s())))


def report(run, idle, warm, final, log_offset, dumps_before):
    a = run.args
    lines = []

    def check(name, ok, detail):
        lines.append(f"{'PASS' if ok else 'FAIL'}  {name}  {detail}")

    by_check = collections.defaultdict(list)
    for c, d in run.failures:
        by_check[c].append(d)

    alive = run.ome.returncode is None
    check('server alive', alive, f'pid {run.ome.pid}' if alive else f'exited rc={run.ome.returncode}')
    dumps_dir = run.dumps_dir
    new_dumps = (set(os.listdir(dumps_dir)) if dumps_dir.exists() else set()) - dumps_before
    check('no crash dump', not new_dumps, f'new: {sorted(new_dumps)}' if new_dumps else 'none')
    bad, allowed = scan_log(run, log_offset)
    check('server log', not bad, f'{sum(bad.values())} unexpected E/C lines' if bad else
          f'no unexpected E/C lines ({sum(allowed.values())} allow-listed)')
    for msg, n in bad.most_common(15):
        lines.append(f'        {n:6d}x {msg}')
    for name in ('sessions', 'streams', 'simulator'):
        items = by_check.get(name, [])
        check(name, not items, f'{len(items)} violation(s)' if items else 'consistent throughout')
        for d in items[:10]:
            lines.append(f'        {d}')
    if warm and final:
        d_thr, d_fd, d_rss = final['threads'] - warm['threads'], final['fds'] - warm['fds'], final['rss_mb'] - warm['rss_mb']
        rss_limit = max(a.leak_rss_mb, warm['rss_mb'] * 0.15)
        ok = d_thr <= a.leak_threads and d_fd <= a.leak_fds and d_rss <= rss_limit
        check('leaks', ok, f"threads {warm['threads']}->{final['threads']} (<=+{a.leak_threads}), "
              f"fds {warm['fds']}->{final['fds']} (<=+{a.leak_fds}), "
              f"rss {warm['rss_mb']}->{final['rss_mb']} MB (<=+{rss_limit:.0f}; heap {warm.get('anon_mb')}->{final.get('anon_mb')}, "
              f"files {warm.get('file_mb')}->{final.get('file_mb')})")
    else:
        lines.append('SKIP  leaks  run too short or aborted before the final quiesce')
    lag = getattr(run, 'relay_lag_max', 0)
    lines.append(f"{'INFO' if lag < 50 else 'WARN'}  relay  worst 60 s loop-lag p99 {lag} ms"
                 + ('' if lag < 50 else ' (relay overloaded: network classes were harsher than configured)'))
    lines.append(f"INFO  idle baseline  {idle}")
    lines.append(f"INFO  peak WebRTC sessions on one stream  {getattr(run, 'session_peak', 0)}")
    lines.append('INFO  viewers by network class (played = got media at least once):')
    for cls in VIEWER_CLASSES:
        q = run.qoe.get(cls)
        if not q:
            continue
        ttff = f"{q['ttff_ms_sum'] / q['ttff_n']:.0f} ms" if q['ttff_n'] else '-'
        lines.append(f"        {cls:12s} viewers {q['viewers']:4d}  played {q['played']:4d}  gave_up {q['gave_up']:3d}  "
                     f"freeze_reload {q['freeze_reload']:3d}  stream_ended {q['stream_ended']:3d}  hls_stall {q['hls_stall']:3d}  webrtc ttff {ttff}")
    if run.desktop_sessions:
        lines.extend(desktop_report(run))
    fails = sum(1 for l in lines if l.startswith('FAIL'))
    verdict = (f'TRAFFIC SIM ABORTED ({run.aborted})' if run.aborted and run.aborted != 'OME died' else
               'TRAFFIC SIM PASS' if fails == 0 else f'TRAFFIC SIM FAIL: {fails} check(s)')
    lines.append('')
    lines.append(f'{verdict} ({run.out})')
    text = '\n'.join(lines)
    print(text, flush=True)
    (run.out / 'summary.txt').write_text(text + '\n')
    return 0 if fails == 0 and not run.aborted else 1


def desktop_report(run):
    """Real browsers, per browser and per network class. Informational: a freeze can be the network class's
    doing (stalls, outages) or the uplink's; WARN lines are the sessions to look at."""
    out = ['INFO  desktop viewers (real browsers; freezes per minute of picture, longest, renderer drops, send-to-display p50/p95):']
    groups = collections.defaultdict(list)
    for r in run.desktop_sessions:
        groups[(r['browser'], r['cls'])].append(r)
    for (browser, cls), rs in sorted(groups.items()):
        j = [r['judged'] for r in rs if r.get('judged', {}).get('ok')]
        mins = sum(x['window_s'] for x in j) / 60
        frz = sum(x['freezes'] for x in j)
        longest = max([x['longest_freeze_s'] for x in j], default=0)
        drops = [x['dropped_ratio'] for x in j if x.get('dropped_ratio') is not None]
        e2e = sorted(x['e2e_ms']['p50'] for x in j if x.get('e2e_ms'))
        e2e95 = sorted(x['e2e_ms']['p95'] for x in j if x.get('e2e_ms'))
        joins = sorted(r['join_s'] for r in rs if r.get('join_s') is not None)
        reloads = sum(1 for r in rs for e in r['events'] if e[1] == 'freeze_reload')
        out.append(f"        {browser:14s} {cls:12s} sessions {len(rs):3d} judged {len(j):3d} {mins:6.1f} min  "
                   f"freezes/min {frz / mins if mins else 0:5.2f}  longest {longest:5.2f} s  "
                   f"dropped {100 * statistics.median(drops) if drops else 0:4.1f}%  "
                   f"e2e {e2e[len(e2e) // 2] if e2e else '-'}/{e2e95[len(e2e95) // 2] if e2e95 else '-'} ms  "
                   f"join {joins[len(joins) // 2] if joins else '-'} s  freeze_reload {reloads}")
    for r in run.desktop_sessions:
        evs = {e[1] for e in r['events']}
        j = r.get('judged', {})
        if 'no_picture' in evs and r.get('stream_live_at_join') and r['cls'] != 'terrible':
            out.append(f"WARN  desktop {r['id']}: no picture ({r['stream']} {r['rendition']} {r['cls']}) although the stream was live")
        if 'freeze_reload' in evs:
            out.append(f"WARN  desktop {r['id']}: frozen 10 s, reloaded ({r['stream']} {r['rendition']} {r['cls']})")
        if 'oracle_killed' in evs:
            out.append(f"WARN  desktop {r['id']}: oracle did not stop; its window may still be open on {r['browser']}")
        if r['cls'] in ('fiber', 'residential') and j.get('ok') and j['longest_freeze_s'] >= 2:
            out.append(f"WARN  desktop {r['id']}: {j['longest_freeze_s']} s freeze on {r['cls']} ({r['stream']} {r['rendition']})")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--duration-s', type=float, default=1800)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--viewers', type=float, default=12, help='target mean concurrent viewers')
    ap.add_argument('--max-viewers', type=int, default=24)
    ap.add_argument('--warmup-s', type=float, default=300, help='traffic before the warm baseline quiesce')
    ap.add_argument('--live-min-s', type=float, default=60)
    ap.add_argument('--live-max-s', type=float, default=600)
    ap.add_argument('--off-min-s', type=float, default=10)
    ap.add_argument('--off-max-s', type=float, default=120)
    ap.add_argument('--stream-ready-s', type=float, default=30, help='a live publisher\'s stream must be listed by then')
    ap.add_argument('--ingest-dead-limit-s', type=float, default=60,
                    help='a stream whose uplink died silently must be gone by then')
    ap.add_argument('--leak-threads', type=int, default=4)
    ap.add_argument('--leak-fds', type=int, default=16)
    ap.add_argument('--leak-rss-mb', type=float, default=100)
    ap.add_argument('--no-gpu', action='store_true', help='skip the transcoded livegpu slot')
    ap.add_argument('--stop-weights', type=lambda v: [float(x) for x in v.split(',')], default=[5, 3, 2],
                    help='relative weights of publisher stops neat,kill,uplink_dead')
    ap.add_argument('--rtmp-silence-ms', type=int,
                    help='run a copy of conf/ with <PacketSilenceTimeoutMs> on every app\'s RTMP provider '
                         '(conf/ mirrors production: 10000 since 2026-10-24)')
    ap.add_argument('--rtmp-first-media-ms', type=int,
                    help='likewise <FirstMediaWaitTimeoutMs>: the wait for an encoder\'s first media, which '
                         'PacketSilenceTimeoutMs governs when unset')
    ap.add_argument('--playout-delay', metavar='MIN:MAX',
                    help='run a copy of conf/ with <PlayoutDelay> on every app\'s WebRTC publisher (production: 50:200)')
    ap.add_argument('--adaptive-playout', action='store_true', help='with --playout-delay: <Adaptive>true</Adaptive>')
    ap.add_argument('--no-switches', action='store_true',
                    help='run a copy of conf/ without <JitterBufferDelay>, <SendPacing> and <VideoShedding> (for --bin of a '
                         'build that predates them, such as production\'s pacercap2410)')
    ap.add_argument('--desktop', action='append', default=[], metavar='NAME=ENDPOINT',
                    help='a real browser that watches back to back, judged per session: Chrome http://host:port (CDP) '
                         'or Firefox ws://host:port (WebDriver BiDi), e.g. deskA-chrome=http://<desktop-ip>:19222. Repeatable')
    ap.add_argument('--allow-production-live', action='store_true')
    ap.add_argument('--ome-nice', type=int, default=int(os.environ.get('OME_IMPAIR_NICE', '0')))
    ap.add_argument('--bin', default=str(REPO / 'build' / 'Release' / 'bin' / 'OvenMediaEngine'))
    ap.add_argument('--out')
    ap.add_argument('--keep-client-logs', action='store_true', help='also keep logs of clients that played fine')
    ap.add_argument('-v', '--verbose', action='store_true', help='print every event')
    sys.exit(asyncio.run(main_async(ap.parse_args())))


if __name__ == '__main__':
    main()
