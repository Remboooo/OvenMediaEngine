#!/usr/bin/env python3
"""Userspace network impairment relays for OME WebRTC reproduction (no root / netem).

udp  — ICE/DTLS/SRTP relay. OME advertises the relay's ports as its only ICE
       candidates; each listen port forwards to the matching OME ICE port.
       Impairment applies per direction (down = OME -> viewer, up = viewer -> OME).
tcp  — RTMP ingest relay (encoder -> OME) with stalls / rate cap, to mimic an
       encoder on a jittery uplink.

Impairment profiles are JSON, re-read whenever the file changes, so conditions
can be switched mid-session without reconnecting. Every random decision comes
from a seeded RNG, so a run is reproducible for the same packet sequence.

Profile keys (all optional; per direction under "down"/"up" for udp, top level for tcp):
  loss            iid drop probability (0..1)
  burst           {"p_enter": .., "p_exit": .., "loss": ..}  Gilbert-Elliott bursts
  delay_ms        base one-way delay
  jitter_ms       stddev of normal jitter added to delay; FIFO-preserving (a queue,
                  not a reorderer) unless jitter_reorders is true
  jitter_reorders let jitter reorder packets (netem-style iid delay; unrealistic at 4K pps)
  reorder         probability a packet is held back an extra reorder_ms
  reorder_ms      extra hold for reordered packets (default 20)
  duplicate       probability a packet is sent twice
  rate_mbps       bottleneck rate; packets queue behind it
  queue_ms        drop-tail once queueing delay would exceed this (default 200)
  stall_every_s   every N seconds hold everything ...
  stall_ms        ... for this long, then release the backlog at once
  start_after_s   impairment off for the first N seconds of each flow (let ICE/DTLS settle)

Usage:
  impair_relay.py udp --listen 0.0.0.0:24000-24005 --target 127.0.0.1:22000-22005 --profile profiles/clean.json
  impair_relay.py tcp --listen 127.0.0.1:21936 --target 127.0.0.1:21935 --profile profiles/ingest_clean.json
"""
import argparse
import asyncio
import json
import os
import random
import socket
import sys
import time


def parse_range(spec):
    host, ports = spec.rsplit(':', 1)
    if '-' in ports:
        a, b = (int(x) for x in ports.split('-'))
        return host, list(range(a, b + 1))
    return host, [int(ports)]


class Profile:
    def __init__(self, path):
        self.path, self.mtime, self.data = path, None, {}
        self.reload()

    def reload(self):
        try:
            m = os.stat(self.path).st_mtime
        except FileNotFoundError:
            return
        if m == self.mtime:
            return
        self.mtime = m
        with open(self.path) as f:
            self.data = json.load(f)
        log(f'profile loaded {self.path}: {json.dumps(self.data)}')

    def get(self, direction=None):
        return self.data.get(direction, {}) if direction else self.data


class NoImpair:
    def get(self, direction=None):
        return {}


def log(msg):
    print(f"{time.strftime('%H:%M:%S')} {msg}", flush=True)


TRACE = open(os.environ['IMPAIR_TRACE'], 'a', buffering=1 << 16) if os.environ.get('IMPAIR_TRACE') else None


def trace(direction, data, delays):
    """RTP header fields are cleartext under SRTP: log them for offline analysis (wall clock, so viewer_check.py
    can line them up with what the viewer showed)."""
    if TRACE is None or len(data) < 12 or not (0x80 <= data[0] <= 0xBF) or 64 <= (data[1] & 0x7F) <= 95:
        return  # not RTP (STUN/DTLS) or RTCP
    pt = data[1] & 0x7F
    seq = (data[2] << 8) | data[3]
    ts = int.from_bytes(data[4:8], 'big')
    ssrc = int.from_bytes(data[8:12], 'big')
    TRACE.write(f"{time.time():.6f} {direction} {ssrc} {pt} {seq} {ts} {len(data)} {0 if delays else 1}\n")


class Shaper:
    """Decides drop/delay for one direction of one flow."""

    def __init__(self, rng, started):
        self.rng, self.started = rng, started
        self.bad = False
        self.link_free_at = 0.0
        self.last_due = 0.0
        self.stats = dict(pkts=0, bytes=0, dropped=0, queue_drop=0, dup=0, reordered=0, stalled=0)

    def plan(self, cfg, size, now):
        """Returns a list of send delays in seconds (empty = drop)."""
        self.stats['pkts'] += 1
        self.stats['bytes'] += size
        if now - self.started < cfg.get('start_after_s', 0):
            return [0.0]
        burst = cfg.get('burst')
        if burst:
            if self.bad:
                self.bad = self.rng.random() >= burst.get('p_exit', 0.3)
            else:
                self.bad = self.rng.random() < burst.get('p_enter', 0.0)
            if self.bad and self.rng.random() < burst.get('loss', 1.0):
                self.stats['dropped'] += 1
                return []
        if self.rng.random() < cfg.get('loss', 0.0):
            self.stats['dropped'] += 1
            return []

        delay = cfg.get('delay_ms', 0) / 1000
        jitter = cfg.get('jitter_ms', 0) / 1000
        if jitter:
            delay = max(0.0, delay + self.rng.gauss(0, jitter))
        reorder = self.rng.random() < cfg.get('reorder', 0.0)
        if reorder:
            delay += cfg.get('reorder_ms', 20) / 1000
            self.stats['reordered'] += 1

        every = cfg.get('stall_every_s', 0)
        if every:
            phase = (now - self.started) % every
            stall = cfg.get('stall_ms', 0) / 1000
            if phase < stall:
                delay += stall - phase
                self.stats['stalled'] += 1

        rate = cfg.get('rate_mbps', 0)
        if rate:
            tx = size * 8 / (rate * 1e6)
            start = max(now, self.link_free_at)
            if start - now > cfg.get('queue_ms', 200) / 1000:
                self.stats['queue_drop'] += 1
                return []
            self.link_free_at = start + tx
            delay += self.link_free_at - now

        if not reorder and not (jitter and cfg.get('jitter_reorders', False)):
            # A path is a queue: never overtake the previous packet, also not when the delay
            # drops (a profile change mid-flow would otherwise reorder by the difference, far
            # past SRTP's replay window). On the total delay, once: the bottleneck's queueing is
            # already in order, and clamping before adding it would stack each packet's queueing
            # onto the previous one's. +1us so equal due times stay ordered (asyncio's timer heap
            # does not keep FIFO).
            delay = max(delay, self.last_due - now + 1e-6)

        if not reorder:
            self.last_due = max(self.last_due, now + delay)
        out = [delay]
        if self.rng.random() < cfg.get('duplicate', 0.0):
            out.append(delay + 0.001)
            self.stats['dup'] += 1
        return out


# --------------------------------------------------------------------- UDP
# A keyframe, a released backlog or a NACK answer reaches the relay at loopback speed. The relay must
# not be the network's bottleneck (the profile's rate/queue is), so its sockets hold seconds of video:
# the kernel caps this at net.core.rmem_max (the 208 KB default drops 4K keyframes).
RCVBUF = 8 << 20


def big_rcvbuf(transport):
    sock = transport.get_extra_info('socket')
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, RCVBUF)
    got = sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
    if got < RCVBUF:
        log(f'udp receive buffer {got} bytes: raise net.core.rmem_max, or bursts drop in the relay')
    return got


class Downstream(asyncio.DatagramProtocol):
    """Socket toward OME for one viewer address; OME's replies go back down."""

    def __init__(self, relay, client):
        self.relay, self.client = relay, client

    def datagram_received(self, data, addr):
        self.relay.forward('d', self.relay.down[self.client], self.relay.profile.get('down'), data,
                           lambda d: self.relay.listen_transport.sendto(d, self.client))


class UdpRelay(asyncio.DatagramProtocol):
    def __init__(self, listen_port, target, profile, seed):
        self.listen_port, self.target, self.profile = listen_port, target, profile
        self.rng = random.Random(seed + listen_port)
        self.loop = asyncio.get_event_loop()
        self.flows, self.up, self.down = {}, {}, {}
        self.listen_transport = None

    def connection_made(self, transport):
        self.listen_transport = transport
        big_rcvbuf(transport)

    def forward(self, direction, shaper, cfg, data, send):
        # Absolute due times on the loop clock: call_later() would re-read the
        # clock and let per-packet processing jitter reorder clamped packets.
        now = self.loop.time()
        delays = shaper.plan(cfg, len(data), now)
        trace(direction, data, delays)
        for d in delays:
            if d <= 0:
                send(data)
            else:
                self.loop.call_at(now + d, send, data)

    def datagram_received(self, data, addr):
        flow = self.flows.get(addr)
        if flow is None:
            self.flows[addr] = 'pending'
            now = self.loop.time()
            self.up[addr] = Shaper(self.rng, now)
            self.down[addr] = Shaper(self.rng, now)
            self.loop.create_task(self.open_flow(addr, data))
            return
        if flow == 'pending':
            return  # first-packet retransmits arrive while the upstream socket opens
        self.forward('u', self.up[addr], self.profile.get('up'), data, flow.sendto)

    async def open_flow(self, addr, first):
        # From the listen (LAN) address, not loopback: OME echoes the source back in its STUN responses
        # (XOR-MAPPED-ADDRESS), and Firefox discards a check whose mapped address is a loopback one (ICE fails)
        lan = self.listen_transport.get_extra_info('sockname')[0]
        transport, _ = await self.loop.create_datagram_endpoint(lambda: Downstream(self, addr), local_addr=(lan, 0),
                                                                remote_addr=self.target)
        big_rcvbuf(transport)
        self.flows[addr] = transport
        log(f'udp :{self.listen_port} new flow {addr[0]}:{addr[1]} -> {self.target[0]}:{self.target[1]}')
        transport.sendto(first)


# --------------------------------------------------------------------- TCP
async def tcp_pipe(reader, writer, shaper, profile, label):
    loop = asyncio.get_event_loop()
    try:
        while True:
            data = await reader.read(16384)
            if not data:
                break
            delays = shaper.plan(profile.get(), len(data), time.monotonic())
            # TCP never drops: a "lost" chunk becomes a retransmission-like 200ms stall.
            wait = delays[0] if delays else 0.2
            if wait > 0:
                await asyncio.sleep(wait)
            writer.write(data)
            await writer.drain()
    except (ConnectionResetError, BrokenPipeError):
        pass
    finally:
        writer.close()
        log(f'tcp {label} closed')


async def run_tcp(args, profile):
    host, ports = parse_range(args.listen)
    thost, tports = parse_range(args.target)
    rng = random.Random(args.seed)
    shapers = []

    async def handle(reader, writer):
        peer = writer.get_extra_info('peername')
        up_reader, up_writer = await asyncio.open_connection(thost, tports[0])
        log(f'tcp new connection {peer} -> {thost}:{tports[0]}')
        shaper = Shaper(rng, time.monotonic())
        shapers.append(shaper)
        await asyncio.gather(
            tcp_pipe(reader, up_writer, shaper, profile, 'ingest'),
            tcp_pipe(up_reader, writer, Shaper(rng, time.monotonic()), NoImpair(), 'return'),
        )

    server = await asyncio.start_server(handle, host, ports[0])
    log(f'tcp relay {host}:{ports[0]} -> {thost}:{tports[0]}')
    async with server:
        while True:
            await asyncio.sleep(5)
            profile.reload()
            for s in shapers:
                log(f"tcp ingest stats {s.stats}")


async def run_udp(args, profile):
    host, ports = parse_range(args.listen)
    thost, tports = parse_range(args.target)
    if len(ports) != len(tports):
        sys.exit('listen and target port ranges must be the same length')
    loop = asyncio.get_event_loop()
    relays = []
    for lp, tp in zip(ports, tports):
        relay = UdpRelay(lp, (thost, tp), profile, args.seed)
        await loop.create_datagram_endpoint(lambda r=relay: r, local_addr=(host, lp))
        relays.append(relay)
    log(f'udp relay {host}:{ports[0]}-{ports[-1]} -> {thost}:{tports[0]}-{tports[-1]}')
    while True:
        await asyncio.sleep(5)
        profile.reload()
        for r in relays:
            for addr in list(r.down):
                d, u = r.down[addr].stats, r.up[addr].stats
                log(f"udp :{r.listen_port} {addr[0]}:{addr[1]} down={d} up_pkts={u['pkts']} up_drop={u['dropped']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('mode', choices=['udp', 'tcp'])
    ap.add_argument('--listen', required=True)
    ap.add_argument('--target', required=True)
    ap.add_argument('--profile', required=True)
    ap.add_argument('--seed', type=int, default=1)
    args = ap.parse_args()
    profile = Profile(args.profile)
    asyncio.run(run_udp(args, profile) if args.mode == 'udp' else run_tcp(args, profile))


if __name__ == '__main__':
    main()
