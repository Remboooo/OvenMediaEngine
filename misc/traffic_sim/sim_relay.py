#!/usr/bin/env python3
"""Per-client network relay for the traffic simulator (userspace, no root / netem).

One process carries every path between the simulated clients and the sim OME:

  sig     LAN:33333 tcp       -> OME 33333   WebRTC signalling websocket (tagged per viewer)
  ice     LAN:34000-34005 udp -> OME same    ICE/DTLS/SRTP (tagged per viewer)
  ingest  127.0.0.1:31936 tcp -> OME 31935   RTMP from the publishers (tagged per publisher)
  http    127.0.0.1:30340+i   -> OME 33333   LL-HLS/HLS, one port per network class

Each connection or flow is tagged with a client id and a network class from
classes.json. The simulator says which client comes next over a control socket
(JSON lines) before it starts that client. Clients are started one at a time,
so the next new connection or flow belongs to that client:

  {"cmd": "expect_viewer", "id": "v17", "cls": "wifi"}   next sig connection; ICE flows until the next expect
  {"cmd": "expect_ingest", "id": "p0", "cls": "uplink_jitter"}
  {"cmd": "blackhole", "id": "v17"}    network vanishes: drop everything both ways, never close toward OME
  {"cmd": "release", "id": "v17"}      network comes back (a temporary outage)
  {"cmd": "forget", "id": "v17"}       client is gone; drop its stats
  {"cmd": "stats"} / {"cmd": "ports"}
  {"cmd": "set_class", "cls": "lim", "net": {"down": {...}, "up": {...}, "tcp": {...}}}
                                       replace a class's impairment live (until classes.json changes)
  {"cmd": "trace", "id": "o3", "path": "/x/trace.csv"}   log every UDP packet of a client (see TRACE_FIELDS)
  {"cmd": "trace_stop", "id": "o3"}
  {"cmd": "hold", "id": "o3", "ms": 500, "dir": "d"}   queue (not drop) a client's UDP for ms from now,
                                       like a bufferbloated link that stalls; dir d, u or both

A black-holed client that then closes its side (the user closes the dead tab)
must look like nothing at all to OME: the OME-side socket stays open and
silent until OME itself gives up, which is exactly what the simulator checks.

UDP impairment reuses webrtc_impair's Shaper (loss, bursts, FIFO jitter,
reorder, rate, stalls). TCP is a delay line: chunks keep their order and the
throughput, but get delay/jitter/stalls, and a "lost" chunk costs a 200 ms
retransmission stall, like TCP over a lossy link.
"""
import argparse
import asyncio
import json
import os
import random
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'webrtc_impair'))
from impair_relay import Shaper, big_rcvbuf, parse_range  # noqa: E402

FLOW_IDLE_S = 60  # close an ICE flow's upstream socket after this long without packets

# One CSV line per UDP packet of a traced client, written when the relay receives it.
# t: wall clock (s since the epoch, so it lines up with a browser's timestamps on this host); dir: d = OME -> client, u = client -> OME;
# fate: ok, drop (impairment), bh (black-holed); due_ms: delay applied (first copy);
# kind: rtp, rtcp, stun, dtls or other. SRTP leaves the RTP header in the clear, so for RTP
# ssrc/pt/seq/ts/marker are what OME (or the client) put on the wire. For RTCP only the first
# packet's type is in the clear (SRTCP encrypts the rest): pt is 200 SR, 201 RR, 205 RTPFB, 206 PSFB,
# and seq holds its FMT/count field (205/1 NACK, 205/15 transport-cc, 206/1 PLI, 206/4 FIR, 206/15 REMB).
TRACE_FIELDS = 't,dir,fate,due_ms,kind,size,ssrc,pt,seq,ts,marker'
STUN_DUMP = os.environ.get('SIM_RELAY_STUN_DUMP')  # also append each traced STUN message, in hex, to this file


def classify(data):
    """RFC 7983 demultiplexing, then RTP vs RTCP by the second byte."""
    b0 = data[0] if data else 0
    if b0 < 4:
        return 'stun', None
    if 20 <= b0 < 64:
        return 'dtls', None
    if 128 <= b0 < 192 and len(data) >= 12:
        b1 = data[1]
        if 192 <= b1 <= 223:  # RTCP packet types 192-223 (RFC 5761)
            return 'rtcp', (int.from_bytes(data[4:8], 'big'), b1, b0 & 0x1f, 0, 0)
        return 'rtp', (int.from_bytes(data[8:12], 'big'), b1 & 0x7f, int.from_bytes(data[2:4], 'big'),
                       int.from_bytes(data[4:8], 'big'), b1 >> 7)
    return 'other', None


def log(msg):
    print(f"{time.strftime('%H:%M:%S')} {msg}", flush=True)


class Classes:
    def __init__(self, path):
        self.path, self.mtime, self.data = path, None, {}
        self.reload()

    def reload(self):
        m = os.stat(self.path).st_mtime
        if m != self.mtime:
            self.mtime = m
            with open(self.path) as f:
                self.data = {k: v for k, v in json.load(f).items() if not k.startswith('_')}
            log(f'classes loaded: {", ".join(self.data)}')

    def get(self, cls, part):
        return self.data.get(cls, {}).get(part, {})


class Tag:
    def __init__(self, cid, cls):
        self.id, self.cls = cid, cls
        self.blackholed = False
        self.trace = None  # open file while traced
        self.hold_until = {'d': 0.0, 'u': 0.0}  # loop time until which UDP is queued
        self.stats = dict(udp_flows=0, down_pkts=0, down_drop=0, up_pkts=0, up_drop=0, bh_drop=0,
                          tcp_conns=0, tcp_open=0, tcp_bytes_down=0, tcp_bytes_up=0)


class TcpShaper:
    """Due times for one direction of a TCP connection; FIFO by construction."""

    def __init__(self, rng, started):
        self.rng, self.started = rng, started
        self.last_due = 0.0
        self.link_free_at = 0.0

    def due(self, cfg, size, now):
        delay = cfg.get('delay_ms', 0) / 1000
        if cfg.get('jitter_ms'):
            delay = max(0.0, delay + self.rng.gauss(0, cfg['jitter_ms'] / 1000))
        if self.rng.random() < cfg.get('loss', 0.0):
            delay += 0.2
        every = cfg.get('stall_every_s', 0)
        if every:
            phase = (now - self.started) % every
            stall = cfg.get('stall_ms', 0) / 1000
            if phase < stall:
                delay += stall - phase
        due = now + delay
        rate = cfg.get('rate_mbps', 0)
        if rate:
            self.link_free_at = max(now, self.link_free_at) + size * 8 / (rate * 1e6)
            due = max(due, self.link_free_at)
        due = max(due, self.last_due + 1e-6)
        self.last_due = due
        return due


class Relay:
    def __init__(self, args):
        self.args = args
        self.loop = None  # set in run(), inside asyncio.run's loop
        self.classes = Classes(args.classes)
        self.rng = random.Random(args.seed)
        self.tags = {}
        self.pending_sig = None
        self.pending_ingest = None
        self.current_ice = None
        self.flows = {}  # (listen_port, client addr) -> Flow
        self.http_ports = {}
        self.lag = []  # event-loop lag samples (s), recent window
        self.lag_max = 0.0

    def tag(self, cid, cls):
        t = self.tags.get(cid)
        if t is None:
            t = self.tags[cid] = Tag(cid, cls)
        t.cls = cls
        return t

    # ------------------------------------------------------------ control
    async def control(self, reader, writer):
        while True:
            line = await reader.readline()
            if not line:
                break
            try:
                req = json.loads(line)
                writer.write((json.dumps(self.command(req)) + '\n').encode())
            except Exception as e:  # keep the relay up whatever the simulator sends
                writer.write((json.dumps({'ok': False, 'error': str(e)}) + '\n').encode())
            await writer.drain()
        writer.close()

    def command(self, req):
        cmd = req['cmd']
        if cmd == 'expect_viewer':
            t = self.tag(req['id'], req['cls'])
            self.pending_sig = t
            self.current_ice = t
            return {'ok': True}
        if cmd == 'expect_ingest':
            self.pending_ingest = self.tag(req['id'], req['cls'])
            return {'ok': True}
        if cmd in ('blackhole', 'release'):
            t = self.tags.get(req['id'])
            if t is None:
                return {'ok': False, 'error': 'unknown id'}
            t.blackholed = cmd == 'blackhole'
            log(f"{cmd} {t.id} ({t.cls})")
            return {'ok': True}
        if cmd == 'forget':
            t = self.tags.get(req['id'])
            # Keep a black-holed client's tag while OME still holds its sockets open.
            if t is not None and not (t.blackholed and t.stats['tcp_open']):
                del self.tags[req['id']]
            return {'ok': True}
        if cmd == 'stats':
            lag = sorted(self.lag)
            return {'ok': True,
                    'clients': {k: dict(t.stats, cls=t.cls, blackholed=t.blackholed) for k, t in self.tags.items()},
                    'flows': len(self.flows),
                    'loop_lag_p99_ms': round(1000 * lag[int(len(lag) * 0.99)] if lag else 0, 1),
                    'loop_lag_max_ms': round(1000 * self.lag_max, 1)}
        if cmd == 'ports':
            return {'ok': True, 'http': self.http_ports}
        if cmd == 'set_class':
            self.classes.data[req['cls']] = req['net']
            log(f"class {req['cls']} set to {json.dumps(req['net'])}")
            return {'ok': True}
        if cmd == 'trace':
            t = self.tags.get(req['id'])
            if t is None:
                return {'ok': False, 'error': 'unknown id'}
            if t.trace is None:
                t.trace = open(req['path'], 'w', buffering=1 << 20)
                t.trace.write(TRACE_FIELDS + '\n')
            return {'ok': True}
        if cmd == 'hold':
            t = self.tags.get(req['id'])
            if t is None:
                return {'ok': False, 'error': 'unknown id'}
            until = self.loop.time() + req['ms'] / 1000
            for d in ('d', 'u') if req.get('dir', 'both') == 'both' else (req['dir'],):
                t.hold_until[d] = max(t.hold_until[d], until)
            log(f"hold {t.id} {req['ms']} ms dir={req.get('dir', 'both')}")
            return {'ok': True}
        if cmd == 'trace_stop':
            t = self.tags.get(req['id'])
            if t is not None and t.trace is not None:
                t.trace.close()
                t.trace = None
            return {'ok': True}
        return {'ok': False, 'error': f'unknown cmd {cmd}'}

    # ------------------------------------------------------------ UDP (ICE)
    def forward(self, tag, direction, shaper, data, send):
        if tag.blackholed:
            tag.stats['bh_drop'] += 1
            if tag.trace is not None:
                self.write_trace(tag, direction, 'bh', None, data)
            return
        cfg = self.classes.get(tag.cls, 'down' if direction == 'd' else 'up')
        now = self.loop.time()
        delays = shaper.plan(cfg, len(data), now)
        held = tag.hold_until[direction] - now
        if held > 0 and delays:
            # Everything queued behind the stall leaves when it ends, in arrival order
            # (equal due times would not keep FIFO in asyncio's timer heap)
            first = max(delays[0], held, shaper.last_due - now + 1e-6)
            delays = [first] + [first + 1e-6 * i for i in range(1, len(delays))]
            shaper.last_due = now + delays[-1]
        if tag.trace is not None:
            self.write_trace(tag, direction, 'ok' if delays else 'drop', delays[0] if delays else None, data)
        key = 'down' if direction == 'd' else 'up'
        tag.stats[key + '_pkts'] += 1
        if not delays:
            tag.stats[key + '_drop'] += 1
        for d in delays:
            if d <= 0:
                send(data)
            else:
                self.loop.call_at(now + d, send, data)

    def write_trace(self, tag, direction, fate, delay, data):
        kind, h = classify(data)
        due = '' if delay is None else f'{delay * 1000:.2f}'
        fields = ','.join(map(str, h)) if h else ',,,,'
        tag.trace.write(f'{time.time():.6f},{direction},{fate},{due},{kind},{len(data)},{fields}\n')
        if kind == 'stun' and STUN_DUMP:
            # The whole message, to check ICE checks against the SDP's credentials offline
            with open(STUN_DUMP, 'a') as f:
                f.write(f'{time.time():.6f} {direction} {tag.id} {data.hex()}\n')

    async def gc_flows(self):
        while True:
            await asyncio.sleep(10)
            now = self.loop.time()
            for key, flow in list(self.flows.items()):
                if flow.transport is not None and now - flow.last_seen > FLOW_IDLE_S:
                    flow.transport.close()
                    del self.flows[key]

    # ------------------------------------------------------------ TCP
    async def tcp_accept(self, reader, writer, target_port, tag):
        stats = tag.stats
        stats['tcp_conns'] += 1
        stats['tcp_open'] += 1
        try:
            up_reader, up_writer = await asyncio.open_connection(self.args.ome, target_port)
        except OSError:
            stats['tcp_open'] -= 1
            writer.close()
            return
        now = self.loop.time()
        done = asyncio.Event()  # OME closed its side: tear everything down
        await asyncio.gather(
            self.tcp_pipe(reader, up_writer, tag, 'up', TcpShaper(self.rng, now), done, toward_ome=True),
            self.tcp_pipe(up_reader, writer, tag, 'down', TcpShaper(self.rng, now), done, toward_ome=False),
        )
        stats['tcp_open'] -= 1

    async def tcp_pipe(self, reader, writer, tag, part, shaper, done, toward_ome):
        q = asyncio.Queue(maxsize=64)

        async def rx():
            try:
                while True:
                    data = await reader.read(65536)
                    if not data:
                        break
                    if tag.blackholed:
                        tag.stats['bh_drop'] += 1
                        continue
                    cfg = self.classes.get(tag.cls, 'tcp')
                    due = shaper.due(cfg, len(data), self.loop.time())
                    hold = tag.hold_until['u' if toward_ome else 'd']
                    if hold > due:
                        due = shaper.last_due = hold
                    await q.put((due, data))
            except (ConnectionError, OSError):
                pass
            await q.put(None)

        async def tx():
            try:
                while True:
                    item = await q.get()
                    if item is None:
                        break
                    due, data = item
                    wait = due - self.loop.time()
                    if wait > 0:
                        await asyncio.sleep(wait)
                    if tag.blackholed:
                        tag.stats['bh_drop'] += 1
                        continue
                    writer.write(data)
                    tag.stats['tcp_bytes_' + part] += len(data)
                    await writer.drain()
            except (ConnectionError, OSError):
                pass
            if toward_ome:
                # The client closed. If its network is gone, OME must not hear about it:
                # hold the OME-side socket open until OME closes it (or the network returns).
                while tag.blackholed and not done.is_set():
                    await asyncio.sleep(0.5)
            else:
                done.set()
                if tag.blackholed:
                    return  # OME gave up; a client behind a dead network never sees the FIN
            try:
                writer.close()
            except Exception:
                pass

        await asyncio.gather(rx(), tx())

    # ------------------------------------------------------------ main
    async def lag_monitor(self):
        while True:
            t0 = self.loop.time()
            await asyncio.sleep(0.05)
            lag = self.loop.time() - t0 - 0.05
            self.lag.append(lag)
            if len(self.lag) > 1200:  # last ~60 s
                del self.lag[:200]
            self.lag_max = max(self.lag_max, lag)

    async def run(self):
        a = self.args
        loop = self.loop = asyncio.get_running_loop()
        default = Tag('?', a.default_class)

        def sig_handler(r, w):
            tag, self.pending_sig = self.pending_sig or default, None
            return self.tcp_accept(r, w, a.sig, tag)

        def ingest_handler(r, w):
            tag, self.pending_ingest = self.pending_ingest or Tag('?', 'uplink_clean'), None
            return self.tcp_accept(r, w, a.ingest_target, tag)

        async def listen(handler, host, port):
            # A port can be held for a moment by a connection's ephemeral source port (the simulator's
            # own API polls): retry instead of failing the run
            for attempt in range(20):
                try:
                    return await asyncio.start_server(handler, host, port)
                except OSError:
                    if attempt == 19:
                        raise
                    await asyncio.sleep(0.25)

        await listen(sig_handler, a.lan, a.sig)
        ihost, iport = a.ingest_listen.rsplit(':', 1)
        await listen(ingest_handler, ihost, int(iport))
        for i, cls in enumerate(sorted(self.classes.data)):
            if cls.startswith('uplink'):
                continue
            port = a.http_base + len(self.http_ports)
            # limits.py addresses its HTTP client as "lim_http" (hold); other classes' connections are anonymous
            await listen(
                lambda r, w, c=cls: self.tcp_accept(r, w, a.sig, self.tag('lim_http', c) if c == 'lim' else Tag(None, c)),
                '127.0.0.1', port)
            self.http_ports[cls] = port
        _, ice_ports = parse_range(f'x:{a.ice}')
        for port in ice_ports:
            await loop.create_datagram_endpoint(lambda p=port: IceListen(self, p), local_addr=(a.lan, port))
        server = await asyncio.start_unix_server(self.control, a.control)
        log(f'sim relay up: sig {a.lan}:{a.sig} ice {a.lan}:{a.ice} ingest {a.ingest_listen} '
            f'http {self.http_ports} control {a.control}')
        loop.create_task(self.gc_flows())
        loop.create_task(self.lag_monitor())
        async with server:
            while True:
                await asyncio.sleep(5)
                try:
                    self.classes.reload()
                except (OSError, ValueError) as e:
                    log(f'classes reload failed: {e}')


class Flow:
    def __init__(self, tag, now, rng):
        self.tag, self.last_seen = tag, now
        self.up, self.down = Shaper(rng, now), Shaper(rng, now)
        self.transport = None


class IceListen(asyncio.DatagramProtocol):
    def __init__(self, relay, port):
        self.relay, self.port = relay, port
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport
        big_rcvbuf(transport)

    def datagram_received(self, data, addr):
        r = self.relay
        key = (self.port, addr)
        flow = r.flows.get(key)
        now = r.loop.time()
        if flow is None:
            tag = r.current_ice or Tag('?', r.args.default_class)
            flow = r.flows[key] = Flow(tag, now, r.rng)
            tag.stats['udp_flows'] += 1
            r.loop.create_task(self.open(flow, addr, data))
            return
        flow.last_seen = now
        if flow.transport is None:
            return  # upstream socket still opening
        r.forward(flow.tag, 'u', flow.up, data, flow.transport.sendto)

    async def open(self, flow, addr, first):
        r = self.relay
        listen = self

        class Down(asyncio.DatagramProtocol):
            def datagram_received(self, data, _):
                r.forward(flow.tag, 'd', flow.down, data, lambda d: listen.transport.sendto(d, addr))

        # From the LAN address, not loopback: OME echoes the source back in its STUN responses (XOR-MAPPED-ADDRESS),
        # and Firefox discards a check whose mapped address is a loopback one (ICE then fails); Chrome accepts it
        flow.transport, _ = await r.loop.create_datagram_endpoint(Down, local_addr=(r.args.lan, 0),
                                                                  remote_addr=(r.args.ome, self.port))
        big_rcvbuf(flow.transport)
        if not flow.tag.blackholed:
            flow.transport.sendto(first)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--lan', default=os.environ.get('OME_TEST_LAN_IP'), required='OME_TEST_LAN_IP' not in os.environ,
                    help='address the viewers reach (default: OME_TEST_LAN_IP, which traffic_sim.py sets)')
    ap.add_argument('--ome', default='127.0.0.1')
    ap.add_argument('--sig', type=int, default=33333)
    ap.add_argument('--ice', default='34000-34005')
    ap.add_argument('--ingest-listen', default='127.0.0.1:31936')
    ap.add_argument('--ingest-target', type=int, default=31935)
    # Below the ephemeral range (32768+), where a client connection's source port could take it first
    ap.add_argument('--http-base', type=int, default=30340)
    ap.add_argument('--classes', required=True)
    ap.add_argument('--default-class', default='fiber')
    ap.add_argument('--control', required=True)
    ap.add_argument('--seed', type=int, default=1)
    args = ap.parse_args()
    if os.path.exists(args.control):
        os.unlink(args.control)
    asyncio.run(Relay(args).run())


if __name__ == '__main__':
    main()
