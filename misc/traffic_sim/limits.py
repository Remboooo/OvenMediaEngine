#!/usr/bin/env python3
"""Limits test: does each viewer-protection guard absorb exactly what it should, and fail gracefully beyond?

For one stream and rendition, one impairment dimension at a time (random loss, burst loss, RTT, jitter,
reordering, downlink queueing stalls, outages, bandwidth caps, encoder-uplink stalls and jitter), this
steps the impairment from well below what the guards should absorb to well past it. Every step:

  clean  (--clean-s)    a fresh oracle viewer connects; its baseline (RTT, jitter buffer, bitrate,
                        frame rate, GOP, send-to-display delay) sizes the prediction
  impair (--impair-s)   the impairment runs (outages and holds fire once, mid-phase)
  recover (--recover-s) clean again: the viewer must come back to its baseline

and judges what the viewer saw against a prediction made from the guards' parameters
(the model is in predict(); README.md explains it):

  below   the guards should absorb it: no freeze, no stutter, no keyframe wait. Any is a FAIL:
          a guard falls short of its own theory.
  zone    near the predicted limit: reported, never judged (the model is not that exact).
  beyond  harm is expected, but bounded: no freeze longer than the model's worst case, and recovery
          within the bound once the impairment ends. Beyond the bound is a FAIL: degradation is not graceful.

  always  no decode errors (corruption is never a network's fault: Chrome must not decode broken
          frames, so a decode error means the server sent wrong data); the server sends no RTP
          sequence gaps, keeps every frame's packets contiguous and marked; nothing in the OME log
          outside the allow-list; OME alive.

The viewer is a headless Chromium (segcache_soak/hlsjs_soak/webrtc_oracle.mjs) that decodes and
renders, behind sim_relay.py's "lim" class. The relay traces the viewer's packets, so each displayed
frame's send-to-display delay is known on one clock. Uses the traffic simulator's OME, conf and ports;
refuses to run while production is live.

  ./limits.py                                   all WebRTC dimensions, s720 Bypass+opus
  ./limits.py --dims loss,outage --quick        a subset, fewer steps
  ./limits.py --stream gpu4k --rendition 720p   a transcoded rung (GPU)
"""
import argparse
import asyncio
import bisect
import collections
import json
import math
import os
import re
import signal
import statistics
import subprocess
import sys
import time
from pathlib import Path

import traffic_sim as ts

ROOT = ts.ROOT
ORACLE = ts.REPO / 'misc' / 'segcache_soak' / 'hlsjs_soak' / 'webrtc_oracle.mjs'
DECODE_CHECK = ts.REPO / 'misc' / 'segcache_soak' / 'hlsjs_soak' / 'h264_decode_check.sh'
STREAMS = {s['stream']: s for s in ts.SLOTS}

# Guard parameters the predictions use (where each comes from):
CHROME_NACK_LIST_MAX = 1000   # libwebrtc NackRequester kMaxNackPackets: a bigger gap clears the list, requests a keyframe
OME_RTX_HISTORY = 8192        # src/publishers/webrtc/rtc_common_types.h MAX_RTP_RECORDS (stream history and session sent log)
OME_PACING_FACTOR = 2.5       # RtcSession::kPacingFactor: past one keyframe's worth, a burst is sent at this x the media rate
WEBRTC_FEATURES = ('pacing', 'shedding', 'adaptive-playout')  # --webrtc-features: this fork's opt-in switches
OME_WEBRTC_TIMEOUT_S = 30     # Publishers/WebRTC/Timeout: a longer outage ends the session
PATH_RTT_MS = 30             # the 15 ms each way the impairment profiles add (see DIMS)
CHROME_DEFAULT_RTT_MS = 100   # libwebrtc's RTT before it has measured one (NACK resend interval seen: ~100 ms)
NACK_REACTION_MS = 20         # gap seen -> RTX sent, measured: repair median 48 ms on a 30 ms RTT path (OME coalesces NACKs 10 ms)
# OME ignores PLI/FIR from viewers (RtcSession::OnRtcpReceived), so a viewer that lost a frame for good
# waits for the source's next natural keyframe: up to one GOP.

FREEZE_MIN_MS = 150           # W3C freeze definition: interval > max(3 x average, average + 150 ms)
STUTTER_FRAMES = 3            # a render gap of >= 3 frame periods is a visible stutter (below freeze size)


# ===================================================================== dimensions
def D(name, values, unit, make, kind='net', doc=''):
    return dict(name=name, values=values, unit=unit, make=make, kind=kind, doc=doc)


DIMS = [
    D('loss', [0.2, 0.5, 1, 2, 5, 10, 20, 35], '%',
      lambda v: {'down': {'loss': v / 100, 'delay_ms': 15}, 'up': {'delay_ms': 15}},
      doc='independent random loss toward the viewer, 30 ms RTT'),
    D('burst', [2, 4, 8, 16, 32, 64, 128], 'pkts',
      lambda v: {'down': {'burst': {'p_enter': 0.01 / v, 'p_exit': 1 / v, 'loss': 1.0}, 'delay_ms': 15}, 'up': {'delay_ms': 15}},
      doc='Gilbert-Elliott bursts of mean length v packets, about 1% loss overall, 30 ms RTT'),
    D('rtt', [20, 50, 100, 200, 400, 800], 'ms',
      lambda v: {'down': {'loss': 0.01, 'delay_ms': v / 2}, 'up': {'delay_ms': v / 2}},
      doc='round-trip time with 1% loss: retransmissions take one RTT'),
    D('jitter', [5, 15, 30, 60, 120, 240], 'ms',
      lambda v: {'down': {'delay_ms': 20 + 2 * v, 'jitter_ms': v}, 'up': {'delay_ms': 15}},
      doc='queueing jitter (standard deviation, order kept) toward the viewer'),
    D('reorder', [10, 25, 50, 100, 200], 'ms',
      lambda v: {'down': {'reorder': 0.02, 'reorder_ms': v, 'delay_ms': 15}, 'up': {'delay_ms': 15}},
      doc='2% of packets held back by v ms, so later ones overtake them'),
    D('hold', [100, 250, 500, 1000, 2000, 4000], 'ms', None, kind='hold',
      doc='one queueing stall toward the viewer: nothing lost, everything arrives late at once'),
    D('outage', [100, 250, 500, 1000, 2000, 4000, 8000], 'ms', None, kind='outage',
      doc='one outage both ways: everything sent meanwhile is lost'),
    D('rate', [3.0, 2.0, 1.5, 1.2, 1.0, 0.8, 0.5], 'x bitrate',
      lambda v, ctx: {'down': {'rate_mbps': v * ctx['bitrate_bps'] / 1e6, 'queue_ms': 300, 'delay_ms': 15}, 'up': {'delay_ms': 15}},
      doc='bottleneck at v times the rendition\'s average bitrate, 300 ms drop-tail queue'),
    D('uplink_hold', [100, 250, 500, 1000, 2000, 4000], 'ms', None, kind='uplink_hold',
      doc='one stall of the encoder\'s uplink (RTMP over TCP: late, never lost)'),
    D('uplink_jitter', [10, 30, 60, 120, 250, 500], 'ms',
      lambda v: {'tcp': {'delay_ms': 20 + 2 * v, 'jitter_ms': v}}, kind='uplink_net',
      doc='queueing jitter on the encoder\'s uplink'),
]
QUICK = {'loss': [0.5, 2, 10, 35], 'burst': [4, 32, 128], 'rtt': [50, 200, 800], 'jitter': [15, 60, 240],
         'reorder': [25, 100, 200], 'hold': [250, 1000, 4000], 'outage': [250, 1000, 4000], 'rate': [2.0, 1.2, 0.8],
         'uplink_hold': [250, 1000, 4000], 'uplink_jitter': [30, 120, 500]}


# ===================================================================== predictions
def predict(dim, v, ctx):
    """What the guards should do with impairment value v, from their parameters and the step's baseline.

    A frame late by less than slack = (viewer buffer + 2 frame periods) shows nothing: the buffer covers
    it, or the frame before it stays up for less than 3 periods, below the stutter size. Each dimension's model gives the
    number of frames per step that arrive later than that (`events`):

      below   events <= 0.1   the guards must absorb it: any freeze or stutter is a FAIL
      zone    in between      reported, not judged (the model is first-order)
      beyond  events >= 3     harm is expected; no freeze may be longer than max_freeze_s

    Returns dict(verdict, events, max_freeze_s, recover_s, why).
    """
    # The baseline is measured on the clean path; most dimensions add 15 ms each way while impaired
    rtt = ctx['rtt_ms'] + (0 if dim in ('rtt', 'uplink_hold', 'uplink_jitter') else PATH_RTT_MS)
    jb = ctx['jb_ms']             # the viewer's buffer beyond assembly (Chrome's jitter buffer delay)
    frame_ms = ctx['frame_ms']
    slack = jb + 2 * frame_ms
    pps = ctx['pps']
    pkt_ms = 1000 / pps
    gop = ctx['gop_s']
    steps_pkts = pps * ctx['impair_s']
    # A packet lost for good (every NACK round failed) leaves the frame undecodable; OME ignores the
    # viewer's PLI, so the viewer waits for the source's next keyframe: at most one GOP.
    keyframe_wait = gop + (rtt + NACK_REACTION_MS) / 1000 + 0.5

    def verdict(events):
        return 'below' if events <= 0.1 else 'beyond' if events >= 3 else 'zone'

    def out(events, max_freeze_s, recover_s, why):
        return dict(verdict=verdict(events), events=round(events, 3), max_freeze_s=None if max_freeze_s is None else round(max_freeze_s, 2),
                    recover_s=None if recover_s is None else round(recover_s, 1), why=why)

    if dim in ('loss', 'rtt'):
        # The first NACK -> RTX round takes RTT + reaction. Chrome resends a NACK after its RTT estimate;
        # measured (30 ms path) it re-NACKs after ~100 ms, libwebrtc's default, consistent with a
        # receive-only peer that gets no RTT from OME (no RTCP XR): each further round costs max(RTT, 100 ms). A packet needing n rounds is late by
        # first + (n - 1) * resend; it is invisible while that fits the slack.
        p = v / 100 if dim == 'loss' else 0.01
        path_rtt = v if dim == 'rtt' else rtt
        first_ms = path_rtt + NACK_REACTION_MS
        resend_ms = max(path_rtt, CHROME_DEFAULT_RTT_MS)
        fit = 0 if first_ms > slack else 1 + math.floor((slack - first_ms) / resend_ms)
        events = p ** (fit + 1) * steps_pkts
        return out(events, keyframe_wait, keyframe_wait + 3,
                   f'first round {first_ms:.0f} ms, resend {resend_ms:.0f} ms, slack {slack:.0f} ms: {fit} round(s) fit, '
                   f'so a packet lost {fit + 1}x shows ({events:.2g} expected); FEC not credited')
    if dim == 'burst':
        # A burst of n packets is noticed when the first packet after it arrives; its first frame is then
        # late by the burst's length plus one round. Lengths are geometric with mean v.
        round_ms = rtt + NACK_REACTION_MS
        bursts = 0.01 * steps_pkts / v
        n_visible = max(0.0, (slack - round_ms) / pkt_ms)
        events = bursts * (1 - 1 / v) ** n_visible if v > 1 else 0.0
        # Retransmissions cross the same lossy path: about 1% of them are lost too, and a second round
        # does not fit the slack when the first barely did
        if 2 * round_ms > slack:
            events += 0.01 * 0.01 * steps_pkts
        lost_max = v * 5
        bound = keyframe_wait if lost_max > CHROME_NACK_LIST_MAX else (v * 5 * pkt_ms + 2 * round_ms) / 1000 + 0.3
        return out(events, max(bound, keyframe_wait), keyframe_wait + 3,
                   f'{bursts:.1f} bursts; one longer than {n_visible:.0f} packets ({n_visible * pkt_ms:.0f} ms) is late past the '
                   f'{slack:.0f} ms slack ({events:.2g} expected)')
    if dim in ('jitter', 'uplink_jitter'):
        # Queueing jitter (order kept, nothing lost). OME has no guard for it: toward the viewer there is
        # nothing in between, and upstream the pacer holds each frame a fixed time from its arrival (DTS
        # pacing caused 4K+ULPFEC freezes), so ingest jitter reaches the viewer as it came. Chrome's
        # estimator grows its buffer to cover it, but only after late frames have shown: absorbed while
        # the tail (3 sigma) fits the slack, harm once sigma exceeds 2 frame periods. After the step Chrome
        # shrinks the buffer at its own pace (judged as Chrome's delay, see judge()).
        where = 'ingest jitter, passed through by the pacer (fixed hold from arrival)' if dim == 'uplink_jitter' else 'FIFO jitter'
        events = 0.0 if 3 * v <= slack else (3.0 if v >= 2 * frame_ms else 1.0)
        return out(events, 6 * v / 1000 + 0.3, 10, f'{where}: tail ~{3 * v:.0f} ms vs slack {slack:.0f} ms, Chrome adapts its buffer after')
    if dim == 'reorder':
        # An overtaken packet is v ms late: nothing lost (a spurious NACK at worst). Visible when v > slack.
        n = 0.02 * steps_pkts
        events = n if v > slack else 0.0
        return out(events, v / 1000 + 0.3, 5, f'packets {v} ms late vs slack {slack:.0f} ms')
    if dim in ('hold', 'uplink_hold'):
        # Nothing lost: frames arrive late at once, so the viewer freezes for (stall - buffer) and must
        # then return to its old delay. Upstream, OME holds frames a fixed time from arrival and does
        # not re-space a backlog, so an ingest stall reaches the viewer the same way.
        events = 1.0 * 3 if v > slack * 1.5 else (0.0 if v <= slack else 1.0)
        return out(events, v / 1000 + 0.3, v / 1000 + 5,
                   f'stall {v} ms vs slack {slack:.0f} ms; nothing lost, so no keyframe wait')
    if dim == 'outage':
        # Everything sent during the outage is lost and must be retransmitted (one round after the end),
        # unless more packets are missing than Chrome's NACK list holds: then it waits for a keyframe.
        lost = v / 1000 * pps
        if v / 1000 >= OME_WEBRTC_TIMEOUT_S:
            return out(3, None, None, 'outage outlives the WebRTC timeout: the session ends')
        late = v + rtt + NACK_REACTION_MS
        events = 3.0 if late > slack * 1.5 else (0.0 if late <= slack else 1.0)
        if lost <= min(CHROME_NACK_LIST_MAX, OME_RTX_HISTORY):
            # OME paces the repair: one keyframe's worth at once, the rest at its pacing rate
            lost_bytes = v / 1000 * ctx['bitrate_bps'] / 8
            pace_ms = max(0.0, lost_bytes - ctx['kf_bytes']) / (OME_PACING_FACTOR * max(ctx['bitrate_bps'], 1) / 8) * 1000
            bound = (late + rtt + NACK_REACTION_MS + pace_ms) / 1000 + 0.3
            why = (f'{lost:.0f} packets lost fit the NACK list ({CHROME_NACK_LIST_MAX}): repaired (paced over {pace_ms:.0f} ms), '
                   f'frozen ~{late:.0f} ms vs slack {slack:.0f} ms')
        else:
            bound = v / 1000 + keyframe_wait
            why = (f'{lost:.0f} packets lost > NACK list {CHROME_NACK_LIST_MAX}: the viewer needs a keyframe, '
                   f'OME ignores its PLI, so it waits for the next one (GOP {gop:.1f} s)')
        return out(events, bound, bound + 3, why)
    if dim == 'rate':
        # The bottleneck must carry the average plus each keyframe within the queue (300 ms) and slack.
        kf_bits = ctx['kf_bytes'] * 8
        avg = ctx['bitrate_bps']
        cap = v * avg
        # Time to push a keyframe through, on top of the steady flow
        kf_ms = 1000 * kf_bits / max(1.0, cap - avg) if cap > avg else float('inf')
        if v < 1:
            events, why = 3.0, f'{v}x the average bitrate cannot carry the stream (fixed rendition, no ABR to fall back on)'
        else:
            events = 3.0 if kf_ms > (300 + slack) * 1.5 else (0.0 if kf_ms <= min(300, slack) else 1.0)
            why = f'keyframe ({ctx["kf_bytes"] / 1000:.0f} kB) takes {kf_ms:.0f} ms on the spare {(cap - avg) / 1e6:.2f} Mbps vs queue 300 ms / slack {slack:.0f} ms'
        return out(events, None if v < 1 else keyframe_wait, keyframe_wait + 5, why)
    raise ValueError(dim)


# ===================================================================== analysis
def jb_delay(stats, t_from, t_to):
    """Chrome's mean video jitter-buffer delay (ms) over a window, from its cumulative counters."""
    w = [s for s in stats if t_from <= s['t'] < t_to and s.get('video', {}).get('jitterBufferEmittedCount')]
    if len(w) < 2:
        return None
    de = w[-1]['video']['jitterBufferEmittedCount'] - w[0]['video']['jitterBufferEmittedCount']
    dd = w[-1]['video']['jitterBufferDelay'] - w[0]['video']['jitterBufferDelay']
    return 1000 * dd / de if de > 0 else None


def load_jsonl(path):
    try:
        with open(path) as f:
            return [json.loads(l) for l in f if l.strip()]
    except FileNotFoundError:
        return []


def load_trace(path):
    """Relay trace rows (see sim_relay.TRACE_FIELDS)."""
    rows = []
    try:
        with open(path) as f:
            next(f, None)
            for line in f:
                p = line.rstrip('\n').split(',')
                rows.append((float(p[0]), p[1], p[2], p[4], int(p[5]),
                             int(p[6]) if p[6] else None, int(p[7]) if p[7] else None,
                             int(p[8]) if p[8] else None, int(p[9]) if p[9] else None, p[10] == '1'))
    except FileNotFoundError:
        pass
    return rows


def window(recs, t_from, t_to, key='t'):
    return [r for r in recs if t_from <= r[key] < t_to]


def render_events(presented, frame_ms, t_from, t_to):
    """Freezes and stutters in what the compositor showed between t_from and t_to (ms epoch)."""
    pts = [r for r in presented if t_from <= r['when'] < t_to]
    freezes, stutters = [], []
    # A freeze still going on at either edge of the window counts too: from the last frame shown
    # before the window, and up to the window's end
    before = [r for r in presented if r['when'] < t_from]
    edges = ([before[-1]] if before else [dict(when=t_from)]) + pts + [dict(when=t_to)]
    for a, b in zip(edges, edges[1:]):
        gap = b['when'] - a['when']
        if gap > max(3 * frame_ms, frame_ms + FREEZE_MIN_MS):
            freezes.append((a['when'], gap))
        elif gap >= STUTTER_FRAMES * frame_ms:
            stutters.append((a['when'], gap))
    # Skipped frames: consecutive presented frames more than 1.5 frame periods apart in RTP time
    skipped = 0
    for a, b in zip(pts, pts[1:]):
        if a.get('rtp') is not None and b.get('rtp') is not None:
            d = ((b['rtp'] - a['rtp']) & 0xFFFFFFFF) / 90
            if 1.5 * frame_ms < d < 10000:
                skipped += round(d / frame_ms) - 1
    return dict(frames=len(pts), freezes=freezes, stutters=stutters, skipped=skipped,
                fps=len(pts) / ((t_to - t_from) / 1000) if t_to > t_from else 0)


def send_times(trace, video_ssrc):
    """RTP timestamp -> wall time (ms) the relay saw the frame's last packet from OME."""
    out = {}
    for t, d, fate, kind, size, ssrc, pt, seq, ts_, marker in trace:
        if d == 'd' and kind == 'rtp' and ssrc == video_ssrc and marker:
            out.setdefault(ts_, t * 1000)
    return out


def socket_drops(pid=None):
    """Datagrams the kernel dropped on a process's UDP sockets (receive buffer full), summed; all
    sockets on the machine without a pid."""
    inodes = None
    if pid is not None:
        try:
            inodes = {os.readlink(f'/proc/{pid}/fd/{fd}')[8:-1] for fd in os.listdir(f'/proc/{pid}/fd')}
        except OSError:
            return 0
    total = 0
    for table in ('/proc/net/udp', '/proc/net/udp6'):
        try:
            with open(table) as f:
                for line in list(f)[1:]:
                    col = line.split()
                    if inodes is None or col[9] in inodes:
                        total += int(col[-1])
        except OSError:
            pass
    return total


def protocol_checks(trace, ssrcs):
    """What OME put on the wire toward this viewer, before any impairment.

    Sequence numbers must run without gaps, repeats or reversals per SSRC, and every video frame
    (RTP timestamp) must carry a marker. Packets after a frame's marker with its timestamp are its
    RED-wrapped ULPFEC parity, which OME's FEC worker sends asynchronously, so they may also land in
    the middle of a later frame: SRTP hides the RED header that would tell parity from media, so frame
    contiguity is not checkable here.
    """
    issues = collections.Counter()
    last_seq = {}
    marked = collections.OrderedDict()  # video timestamp -> marker seen
    for t, d, fate, kind, size, ssrc, pt, seq, ts_, marker in trace:
        if d != 'd' or kind != 'rtp' or ssrc not in ssrcs:
            continue
        prev = last_seq.get(ssrc)
        if prev is not None:
            step = (seq - prev) & 0xFFFF
            if step == 0:
                issues[f'{ssrcs[ssrc]}: repeated sequence number'] += 1
            elif step > 0x8000:
                issues[f'{ssrcs[ssrc]}: sequence number went backwards'] += 1
            elif step > 1:
                issues[f'{ssrcs[ssrc]}: sequence gap (numbers OME never sent)'] += step - 1
        last_seq[ssrc] = seq
        if ssrcs[ssrc] == 'video':
            marked[ts_] = marked.get(ts_, False) or marker
    # A session's first packet can be the middle of a frame; the last frames may still be open
    for ts_, m in list(marked.items())[1:-3]:
        if not m:
            issues['video frame without a marker packet'] += 1
    return issues


def send_cadence(trace, video_ssrc, frame_ms, t_from, t_to):
    """Intervals between the ends of consecutive frames as OME sent them (pacer evenness)."""
    ends = sorted(v for k, v in send_times([r for r in trace if t_from <= r[0] * 1000 < t_to], video_ssrc).items())
    iv = [b - a for a, b in zip(ends, ends[1:])]
    if not iv:
        return {}
    iv.sort()
    return dict(p50=round(iv[len(iv) // 2], 1), p99=round(iv[int(len(iv) * 0.99)], 1), max=round(iv[-1], 1),
                late=sum(1 for x in iv if x > 3 * frame_ms))


def stat_delta(stats, t_from, t_to, kind, field):
    w = [s for s in stats if t_from <= s['t'] < t_to and s.get(kind) and field in s[kind]]
    if len(w) < 2:
        return None
    return w[-1][kind][field] - w[0][kind][field]


def baseline(oracle_dir, t_from, t_to, probe_dir=None):
    """The step's clean phase: what the prediction is sized from. Frame sizes and keyframes come from
    the capture probe when the viewer has no encoded transform (it sees the same stream)."""
    stats = load_jsonl(oracle_dir / 'stats.jsonl')
    assembled = load_jsonl(oracle_dir / 'assembled.jsonl')
    if not assembled and probe_dir is not None and (probe_dir / 'assembled.jsonl').exists():
        assembled = load_jsonl(probe_dir / 'assembled.jsonl')
    w = [s for s in stats if t_from <= s['t'] < t_to and s.get('video')]
    rtts = [s['rtt_ms'] for s in w if s.get('rtt_ms') is not None]
    jbs = []
    for a, b in zip(w, w[1:]):
        de = b['video'].get('jitterBufferEmittedCount', 0) - a['video'].get('jitterBufferEmittedCount', 0)
        dd = b['video'].get('jitterBufferDelay', 0) - a['video'].get('jitterBufferDelay', 0)
        if de > 0:
            jbs.append(1000 * dd / de)
    bw = (w[-1]['video']['bytesReceived'] - w[0]['video']['bytesReceived']) * 8 / ((w[-1]['t'] - w[0]['t']) / 1000) if len(w) > 1 else 0
    pk = (w[-1]['video']['packetsReceived'] - w[0]['video']['packetsReceived']) / ((w[-1]['t'] - w[0]['t']) / 1000) if len(w) > 1 else 0
    if not assembled:
        # No encoded transform: frame timing from what was displayed, keyframes from the decoder's count
        presented = load_jsonl(oracle_dir / 'presented.jsonl')
        assembled = [dict(t=r['when'], rtp=r['rtp'], type='delta', size=0) for r in presented if r.get('rtp') is not None]
        kfd = [(s['t'], s['video'].get('keyFramesDecoded', 0)) for s in stats if s.get('video')]
        for (ta, ka), (tb, kb) in zip(kfd, kfd[1:]):
            if kb > ka:
                assembled.append(dict(t=tb, rtp=None, type='key', size=0))
        assembled.sort(key=lambda r: r['t'])
    a = [r for r in window(assembled, t_from, t_to) if r['rtp'] is not None]
    keys = [r['t'] for r in assembled if r['type'] == 'key']
    gops = [b - a_ for a_, b in zip(keys, keys[1:])]
    sizes = [r['size'] for r in a]
    rtp = [r['rtp'] for r in a]
    d_rtp = sorted(((y - x) & 0xFFFFFFFF) for x, y in zip(rtp, rtp[1:]) if 0 < ((y - x) & 0xFFFFFFFF) < 90000)
    frame_ms = d_rtp[len(d_rtp) // 2] / 90 if d_rtp else 1000 / 30
    avg = statistics.mean(sizes) if sizes else 1
    kf = [r['size'] for r in assembled if r['type'] == 'key' and r['size']]
    return dict(rtt_ms=statistics.median(rtts) if rtts else 1, jb_ms=statistics.median(jbs) if jbs else 50,
                bitrate_bps=bw, pps=max(pk, 1), fps=1000 / frame_ms, frame_ms=frame_ms,
                gop_s=(max(gops) / 1000) if gops else 8.0,
                kf_bytes=statistics.median(kf) if kf else avg * 10, avg_frame_bytes=avg)


# ===================================================================== a step
class Limits:
    def __init__(self, args):
        self.args = args
        self.out = Path(args.out).resolve() if args.out else ROOT / 'logs' / time.strftime('limits_%Y%m%d_%H%M%S')
        self.out.mkdir(parents=True, exist_ok=True)
        self.results = []
        self.failures = []
        sim_args = argparse.Namespace(seed=args.seed, out=str(self.out / 'sim'), verbose=False)
        self.run = ts.Run(sim_args)
        self.pub_proc = None

    def say(self, msg):
        print(f'[{time.strftime("%H:%M:%S")}] {msg}', flush=True)

    def fail(self, step, what):
        self.failures.append((step, what))
        self.say(f'  FAIL {step}: {what}')

    async def relay(self, **req):
        r = await self.run.relay_cmd(**req)
        if not r.get('ok'):
            raise RuntimeError(f'relay {req}: {r}')
        return r

    async def set_viewer_net(self, net):
        if self.args.bottleneck:
            # The path's own bottleneck, under every step and phase (a step's own rate cap wins)
            mbps, queue_ms = (float(x) for x in self.args.bottleneck.split(':'))
            net = dict(net, down={'rate_mbps': mbps, 'queue_ms': queue_ms, **net.get('down', {})})
        await self.relay(cmd='set_class', cls='lim', net=net)

    async def set_uplink_net(self, net):
        await self.relay(cmd='set_class', cls='uplink_lim', net=net)

    async def publish(self):
        s = STREAMS[self.args.stream]
        await self.relay(cmd='expect_ingest', id='pub', cls='uplink_lim')
        src = ts.MEDIA / s['src'].replace('.ts', '.flv')
        if self.args.pub == 'ffmpeg' or not src.exists():
            # ffmpeg -re: its output jitters by +-20 ms (MPEG-TS source), which OME passes on to viewers
            self.pub_proc = await self.run.spawn(
                'pub', 'ffmpeg', '-hide_banner', '-nostdin', '-loglevel', 'warning', '-re', '-stream_loop', '-1',
                '-i', ts.MEDIA / s['src'], '-c', 'copy', '-bsf:a', 'aac_adtstoasc',
                '-f', 'flv', f"{ts.INGEST}/{s['app']}/{s['stream']}", log=self.out / 'publisher.log', nice=5)
        else:
            # Each frame on time, like a live encoder (rtmp_pub.py)
            self.pub_proc = await self.run.spawn(
                'pub', sys.executable, ROOT / 'rtmp_pub.py', src, f"{ts.INGEST}/{s['app']}/{s['stream']}",
                log=self.out / 'publisher.log', nice=-0)
        for _ in range(120):
            await asyncio.sleep(0.5)
            listed = await ts.app_streams(s['app'])
            if listed and s['stream'] in listed:
                await asyncio.sleep(3 if s['app'] == 'live' else 25)  # transcoder warm-up
                return
        raise RuntimeError('stream did not come up')

    def ws_url(self):
        s = STREAMS[self.args.stream]
        q = '' if self.args.rendition == 'auto' else f'?rendition={ts.urllib.request.quote(self.args.rendition)}'
        return f"ws://{ts.LAN}:{ts.SIG_PORT}/{s['app']}/{s['stream']}/{self.args.playlist}{q}"

    async def step(self, dim, v):
        name = f"{dim['name']}={v}{dim['unit'] if dim['unit'] != 'x bitrate' else 'x'}"
        d = self.out / 'steps' / name.replace('/', '_').replace(' ', '')
        d.mkdir(parents=True, exist_ok=True)
        a = self.args
        await self.set_viewer_net({'down': {}, 'up': {}, 'tcp': {}})
        await self.set_uplink_net({'tcp': {}})
        tag = f"o{len(self.results)}"
        self.relay_drops0 = socket_drops(self.run.relay.pid)
        self.all_drops0 = socket_drops()
        ptag = f"p{len(self.results)}"
        total = a.clean_s + a.impair_s + a.recover_s + 50
        # The probe: a second viewer on the same network class that only captures what Chrome assembles
        # (encoded transform, no decoder), for the offline decode check. It is not the viewer that is judged:
        # its transform delays frames on their way to Chrome's reference finder while FEC padding
        # notifications are not delayed, which in a burst breaks continuity that a real viewer keeps.
        # Reference probes on a clean path, connected first: what OME sent, frame by frame, to compare a
        # capture against byte for byte. One per rendition of an auto-ABR playlist (pinned), since the
        # probe's frames come from whichever rung it was on.
        refs = []
        if a.reference_probe:
            renditions = [x for x in a.reference_probe.split(',') if x]
            for k, rend in enumerate(renditions):
                rtag = f"r{len(self.results)}_{k}"
                url = self.ws_url() if rend == '-' else self.ws_url().split('?')[0] + f'?rendition={ts.urllib.request.quote(rend)}'
                async with self.run.connect_lock:
                    await self.relay(cmd='expect_viewer', id=rtag, cls='fiber')
                    refs.append((rtag, await self.run.spawn(f'ref_{rtag}', 'node', ORACLE, '--ws', url, '--out', d / f'reference_{k}',
                                                            '--duration-s', total, '--capture', '--no-decode', log=d / f'reference_{k}.log', nice=5)))
                    await asyncio.sleep(3)
        probe = None
        if not a.no_probe:
            async with self.run.connect_lock:
                await self.relay(cmd='expect_viewer', id=ptag, cls='lim')
                if a.keep_trace:
                    await self.relay(cmd='trace', id=ptag, path=str(d / 'probe_trace.csv'))
                probe = await self.run.spawn(f'probe_{ptag}', 'node', ORACLE, '--ws', self.ws_url(), '--out', d / 'probe',
                                             '--duration-s', total, '--capture', '--no-decode', log=d / 'probe.log', nice=5)
                await asyncio.sleep(3)
        async with self.run.connect_lock:
            await self.relay(cmd='expect_viewer', id=tag, cls='lim')
            await self.relay(cmd='trace', id=tag, path=str(d / 'trace.csv'))
            env = None
            if a.chrome_log:
                env = dict(os.environ, DEBUG='pw:browser', OME_SOAK_CHROME_ARGS='--enable-logging=stderr --v=0 --vmodule=' + ','.join(
                    f'*{m}*=3' for m in ('frame_buffer', 'video_stream_buffer_controller', 'video_receive_stream2',
                                         'rtp_video_stream_receiver2', 'nack_requester', 'packet_buffer', 'rtp_seq_num_only_ref_finder',
                                         'frame_decode_timing', 'vcm_timing', 'ulpfec_receiver', 'h264_decoder_impl', 'video_stream_decoder2', 'rtp_video_stream_receiver_frame_transformer_delegate', 'srtp_session', 'forward_error_correction', 'ulpfec_header_reader_writer', 'ulpfec_generator')))
            if a.remote_viewer:
                # A real desktop browser (GPU decode, real display); the oracle maps its clock onto ours
                env = dict(env or os.environ, OME_SOAK_CDP=a.remote_viewer)
            elif a.remote_firefox:
                env = dict(env or os.environ, OME_SOAK_BIDI=a.remote_firefox)
            t_spawn = time.time() * 1000
            # The viewer: decodes and renders exactly like a real one (no encoded transform)
            oracle = await self.run.spawn(f'oracle_{tag}', 'node', ORACLE, '--ws', self.ws_url(), '--out', d,
                                          '--duration-s', total, *([] if a.viewer_transform else ['--no-transform']),
                                          log=d / 'oracle.log', nice=0, env=env)
            await asyncio.sleep(3)
        # More viewers on the same network class, judged afterwards by viewers_report.py (steps/*/viewer_N/)
        extra = []
        for k in range(1, a.viewers):
            vtag = f"v{len(self.results)}_{k}"
            async with self.run.connect_lock:
                await self.relay(cmd='expect_viewer', id=vtag, cls='lim')
                # local: Chrome here, frames assembled but not decoded (judged on delivery), so one desktop's display
                # does not limit how many sessions OME serves
                vargs, venv = (['--no-decode'], None) if a.extra_viewers == 'local' else (['--no-transform'], env)
                extra.append((vtag, await self.run.spawn(f'oracle_{vtag}', 'node', ORACLE, '--ws', self.ws_url(), '--out', d / f'viewer_{k}',
                                                         '--duration-s', total, *vargs, log=d / f'viewer_{k}.log',
                                                         nice=0 if venv else 5, env=venv)))
                await asyncio.sleep(3)
        # A viewer that joins mid-GOP shows nothing until the next keyframe (OME ignores its PLI):
        # the baseline starts once it plays
        first_shown = await self.wait_playing(d / 'presented.jsonl', deadline=time.time() + 40)
        if first_shown is None:
            self.say('  viewer shows nothing after 40 s')
        t_connect = (first_shown or time.time() * 1000) - 2000  # judged windows start at t_connect + 3 s
        await asyncio.sleep(a.clean_s)
        t_imp = time.time() * 1000

        ctx = baseline(d, t_connect + 3000, t_imp, probe_dir=None if a.no_probe else d / 'probe')
        ctx['impair_s'] = a.impair_s
        ctx['join_s'] = round((first_shown - t_spawn) / 1000, 2) if first_shown else None
        ctx['pacer_ms'] = self.pacer_ms()
        if dim['kind'] in ('net', 'uplink_net'):
            make = dim['make']
            net = make(v, ctx) if make.__code__.co_argcount == 2 else make(v)
            if dim['kind'] == 'net':
                await self.set_viewer_net(net)
            else:
                await self.set_uplink_net(net)
            await asyncio.sleep(a.impair_s)
            event_at = None
        else:
            await asyncio.sleep(a.impair_s / 3)
            event_at = time.time() * 1000
            tags = [tag] + ([ptag] if probe else [])
            if dim['kind'] == 'hold':
                for t_ in tags:
                    await self.relay(cmd='hold', id=t_, ms=v, dir='d')
            elif dim['kind'] == 'outage':
                for t_ in tags:
                    await self.relay(cmd='blackhole', id=t_)
                await asyncio.sleep(v / 1000)
                for t_ in tags:
                    await self.relay(cmd='release', id=t_)
            elif dim['kind'] == 'uplink_hold':
                await self.relay(cmd='hold', id='pub', ms=v, dir='u')
            await asyncio.sleep(max(0, a.impair_s * 2 / 3 - (time.time() * 1000 - event_at) / 1000))
        t_rec = time.time() * 1000
        await self.set_viewer_net({'down': {}, 'up': {}, 'tcp': {}})
        await self.set_uplink_net({'tcp': {}})
        await asyncio.sleep(a.recover_s)
        t_end = time.time() * 1000
        for p_ in (oracle, probe, *[r for _, r in refs], *[v for _, v in extra]):
            self.run.signal(p_, signal.SIGTERM)
        await self.run.reap(oracle, 20)
        await self.run.reap(probe, 20)
        for _, r in refs + extra:
            await self.run.reap(r, 20)
        await self.relay(cmd='trace_stop', id=tag)
        await self.relay(cmd='forget', id=tag)
        if probe:
            if a.keep_trace:
                await self.relay(cmd='trace_stop', id=ptag)
            await self.relay(cmd='forget', id=ptag)
        for rtag, _ in refs + extra:
            await self.relay(cmd='forget', id=rtag)
        await asyncio.sleep(2)
        return await asyncio.to_thread(self.judge, dim, v, name, d, ctx, t_connect, t_imp, t_rec, t_end, event_at)

    async def wait_playing(self, presented, deadline, frames=30):
        """Wall time (ms) of the first displayed frame once `frames` have been displayed, or None."""
        while time.time() < deadline:
            await asyncio.sleep(0.5)
            try:
                with open(presented) as f:
                    lines = f.readlines()
            except FileNotFoundError:
                continue
            if len(lines) >= frames:
                return json.loads(lines[0])['when']
        return None

    def pacer_ms(self):
        """The send-side hold OME logged last for this stream (AdaptiveDelay a->b ms)."""
        try:
            with open(ROOT / 'logs' / 'ovenmediaengine.log', 'rb') as f:
                f.seek(max(0, self.log_size()), 0)
                tail = f.read().decode(errors='replace')
        except OSError:
            return 80
        val = 80
        for line in tail.splitlines():
            if 'AdaptiveDelay' in line and f'/{self.args.stream}#' in line and '→' in line:
                try:
                    val = int(line.split('→')[1].split('ms')[0])
                except ValueError:
                    pass
        return val

    def log_size(self):
        return self.log_offset

    # ------------------------------------------------------------ judging
    def judge(self, dim, v, name, d, ctx, t_connect, t_imp, t_rec, t_end, event_at):
        meta = json.loads((d / 'meta.json').read_text()) if (d / 'meta.json').exists() else {}
        presented = load_jsonl(d / 'presented.jsonl')
        assembled = load_jsonl(d / 'assembled.jsonl')
        stats = load_jsonl(d / 'stats.jsonl')
        lag = load_jsonl(d / 'lag.jsonl')
        trace = load_trace(d / 'trace.csv')
        pred = predict(dim['name'], v, ctx)
        res = dict(step=name, dim=dim['name'], value=v, ctx={k: round(x, 3) if isinstance(x, float) else x for k, x in ctx.items()},
                   pred=pred, fails=[], phases_ms=dict(connect=t_connect, impair=t_imp, recover=t_rec, end=t_end))
        if dim['name'] == 'rate':
            res['link_mbps'] = round(v * ctx['bitrate_bps'] / 1e6, 2)

        def fail(what):
            res['fails'].append(what)
            self.fail(name, what)

        # Joining: the first frame needs a keyframe, and OME ignores the viewer's PLI, so up to one GOP
        if ctx.get('join_s') is not None and ctx['join_s'] > ctx['gop_s'] + 2.5:
            fail(f"joining took {ctx['join_s']} s, more than one GOP ({ctx['gop_s']:.1f} s) plus connection setup")
        if not meta.get('connected_at'):
            fail(f"viewer never connected: {meta.get('error')}")
            return res
        frame_ms = ctx['frame_ms']
        # Phases: the impairment's effect is judged from its start to the recovery bound;
        # jitter's onset (the receiver's estimator ramping up) is excluded.
        settle = 5000 if dim['name'] in ('jitter', 'uplink_jitter') else 0
        clean = render_events(presented, frame_ms, t_connect + 3000, t_imp)
        imp = render_events(presented, frame_ms, t_imp + settle, t_rec + 2000)
        rec_bound = pred['recover_s'] if pred['recover_s'] is not None else 10
        tail_from = min(t_rec + rec_bound * 1000, t_end - 5000)
        tail = render_events(presented, frame_ms, tail_from, t_end - 500)
        res['clean'] = dict(freezes=len(clean['freezes']), stutters=len(clean['stutters']), skipped=clean['skipped'], fps=round(clean['fps'], 1))
        res['impair'] = dict(freezes=len(imp['freezes']), freeze_s=round(sum(g for _, g in imp['freezes']) / 1000, 2),
                             max_freeze_s=round(max([g for _, g in imp['freezes']], default=0) / 1000, 2),
                             stutters=len(imp['stutters']), skipped=imp['skipped'], fps=round(imp['fps'], 1))
        res['tail'] = dict(freezes=len(tail['freezes']), stutters=len(tail['stutters']), skipped=tail['skipped'], fps=round(tail['fps'], 1))

        # Oracle self-check: a freeze while this machine's timers ran late may be the oracle's own doing
        lag_bad = [r for r in lag if r['late_ms'] >= 100]
        def oracle_suspect(t, gap):
            return any(t - 200 <= r['t'] <= t + gap + 200 for r in lag_bad)
        res['oracle_lag_events'] = len(lag_bad)

        # Chrome's own counters over the impairment and recovery
        for f in ('freezeCount', 'totalFreezesDuration', 'nackCount', 'pliCount', 'firCount', 'packetsLost',
                  'retransmittedPacketsReceived', 'fecPacketsReceived', 'framesDropped', 'keyFramesDecoded'):
            res.setdefault('chrome', {})[f] = stat_delta(stats, t_imp, t_end, 'video', f)
        res['chrome']['audio_concealed_ratio'] = None
        cs = stat_delta(stats, t_imp, t_end, 'audio', 'concealedSamples')
        tot = stat_delta(stats, t_imp, t_end, 'audio', 'totalSamplesReceived')
        if cs is not None and tot:
            res['chrome']['audio_concealed_ratio'] = round(cs / tot, 4)

        # Send-to-display delay per frame: baseline vs end of recovery (the catch-up guard)
        st = send_times(trace, meta_video_ssrc(assembled, stats))
        def e2e(t_from, t_to):
            vals = [r['when'] - st[r['rtp']] for r in presented if t_from <= r['when'] < t_to and r.get('rtp') in st]
            return round(statistics.median(vals)) if vals else None
        res['e2e_ms'] = dict(clean=e2e(t_connect + 3000, t_imp), tail=e2e(t_end - 5000, t_end - 500),
                             impair=e2e(t_imp, t_rec))
        # Frames the renderer dropped while impaired (Chrome: getVideoPlaybackQuality; Firefox: decoded - presented)
        pb = [s['playback'] for s in stats if t_imp <= s['t'] < t_rec and s.get('playback')]
        if len(pb) > 1 and pb[-1]['total'] > pb[0]['total']:
            res['impair']['dropped'] = round((pb[-1]['dropped'] - pb[0]['dropped']) / (pb[-1]['total'] - pb[0]['total']), 4)
        else:
            dec = stat_delta(stats, t_imp, t_rec, 'video', 'framesDecoded')
            n = [r['n'] for r in presented if t_imp <= r['when'] < t_rec and r.get('n') is not None]
            if dec and len(n) > 1:
                res['impair']['dropped'] = round(max(0, dec - (n[-1] - n[0])) / dec, 4)
        # What the relay actually took away from this viewer's video during the impairment
        vssrc = meta_video_ssrc(assembled, stats)
        res['injected'] = dict(collections.Counter(
            fate for t, dd, fate, kind, size, ssrc, *_ in trace
            if dd == 'd' and kind == 'rtp' and ssrc == vssrc and fate != 'ok' and t_imp <= t * 1000 < t_rec))

        # A/V sync at playout (Chrome's estimate of when each track's current sample plays)
        av = [s['video']['estimatedPlayoutTimestamp'] - s['audio']['estimatedPlayoutTimestamp'] for s in stats
              if t_end - 5000 <= s['t'] < t_end and s.get('video', {}).get('estimatedPlayoutTimestamp') and s.get('audio', {}).get('estimatedPlayoutTimestamp')]
        res['av_offset_ms'] = round(statistics.median(av)) if av else None

        # ---- verdicts
        verdict = pred['verdict']
        real_freezes = [(t, g) for t, g in imp['freezes'] if not oracle_suspect(t, g)]
        real_stutters = [(t, g) for t, g in imp['stutters'] if not oracle_suspect(t, g)]
        if clean['freezes'] or clean['stutters']:
            res['note'] = 'the clean baseline already froze or stuttered: step not judged on the impairment'
        elif verdict == 'below':
            if real_freezes:
                fail(f"{len(real_freezes)} freeze(s) (longest {max(g for _, g in real_freezes) / 1000:.2f} s) where the guards "
                     f"should absorb it: {pred['why']}")
            elif real_stutters:
                fail(f"{len(real_stutters)} stutter(s) (longest {max(g for _, g in real_stutters):.0f} ms) where the guards "
                     f"should absorb it: {pred['why']}")
        elif verdict == 'beyond' and pred['max_freeze_s'] is not None:
            longest = max([g for _, g in real_freezes], default=0) / 1000
            if longest > pred['max_freeze_s']:
                fail(f"freeze of {longest:.2f} s exceeds the worst case {pred['max_freeze_s']} s: {pred['why']}")
        if tail['freezes'] or tail['stutters']:
            fail(f"not recovered {rec_bound} s after the impairment ended: {len(tail['freezes'])} freeze(s), "
                 f"{len(tail['stutters'])} stutter(s) still")
        # Latency and lip sync after recovery. Beyond the limit these are Chrome's to restore, at its own
        # pace (its video delay estimate and A/V sync converge over tens of seconds): reported, not judged.
        info = res.setdefault('info', [])
        late = res['e2e_ms']['clean'] and res['e2e_ms']['tail'] and res['e2e_ms']['tail'] > res['e2e_ms']['clean'] + max(80, res['e2e_ms']['clean'] * 0.5)
        if late:
            # Where the extra delay sits: Chrome's jitter buffer (its estimator, shrinking at its own pace)
            # or before it (a backlog in OME or the path)
            jb_clean, jb_tail = jb_delay(stats, t_connect + 3000, t_imp), jb_delay(stats, t_end - 5000, t_end - 500)
            extra = res['e2e_ms']['tail'] - res['e2e_ms']['clean']
            in_jb = jb_clean is not None and jb_tail is not None and extra - (jb_tail - jb_clean) <= 50
            msg = (f"delay did not come back: {res['e2e_ms']['clean']} ms before, {res['e2e_ms']['tail']} ms at the end"
                   + (f" (Chrome's jitter buffer {jb_clean:.0f} > {jb_tail:.0f} ms)" if jb_tail is not None else ''))
            fail(msg) if verdict == 'below' and not in_jb else info.append(msg)
        if res['av_offset_ms'] is not None and abs(res['av_offset_ms']) > 80:
            msg = f"audio and video {res['av_offset_ms']} ms apart at playout after recovery"
            fail(msg) if verdict == 'below' else info.append(msg)

        # ---- always: corruption and protocol
        pd = d / 'probe'
        if (pd / 'probe.h264').exists() and any(d.glob('reference_*/probe.h264')):
            # Byte for byte against clean references: authoritative. (The probe's encoded transform hands
            # over frames seconds out of order under heavy congestion and at ABR switches, which the
            # offline decoder cannot sort out.)
            import delivery_check
            dc = delivery_check.check(d)
            res['delivery'] = dc
            res['decode'] = (f"delivery: {dc['matched']} frames as sent, {dc['missing_optional_nals']} without AUD/SEI, "
                             f"{dc['corrupt']} differing, {dc['uncovered']} without a reference")
            if dc['corrupt']:
                fail(f"{dc['corrupt']} frame(s) differ from what OME sent: corrupt data reached the viewer ({'; '.join(dc['examples'])})")
            if not self.args.keep_capture:
                for f_ in [pd / 'probe.h264', *d.glob('reference_*/probe.h264')]:
                    f_.unlink(missing_ok=True)
        elif (pd / 'probe.h264').exists() and (pd / 'probe.h264').stat().st_size:
            (pd / 'frames.jsonl').write_text((pd / 'assembled.jsonl').read_text())
            r = subprocess.run(['bash', DECODE_CHECK, str(pd)], capture_output=True, text=True)
            line = (r.stdout.strip().splitlines() or [''])[0]
            res['decode'] = line
            try:
                bad = int(line.split('decoder errors:')[1].split()[0])
            except (IndexError, ValueError):
                bad = None
            if bad:
                fail(f'{bad} frame(s) with decoder errors: corrupt data reached the viewer ({pd / "decode_errors.txt"})')
            (pd / 'trimmed.h264').unlink(missing_ok=True)
            if not self.args.keep_capture:
                (pd / 'probe.h264').unlink(missing_ok=True)
        elif probe_expected := not self.args.no_probe:
            fail('the probe captured nothing: corruption not checked')
        ssrcs = {meta_video_ssrc(assembled, stats): 'video'}
        audio_ssrc = next((s['audio'].get('ssrc') for s in stats if s.get('audio')), None)
        if audio_ssrc:
            ssrcs[audio_ssrc] = 'audio'
        issues = protocol_checks(trace, ssrcs)
        res['protocol'] = dict(issues)
        # A burst (a released backlog) can overflow the relay's own socket buffer before it traces a packet:
        # gaps the relay's drops account for are the harness's, not numbers OME skipped
        res['relay_socket_drops'] = relay_drops = socket_drops(self.run.relay.pid) - self.relay_drops0
        # Everything else that dropped on this machine meanwhile: mostly the viewers' Chrome sockets
        # (also any other UDP receiver here, so a hint, not a verdict)
        other = socket_drops() - self.all_drops0 - relay_drops
        if other:
            res.setdefault('info', []).append(f'{other} datagram(s) dropped by full receive buffers outside the relay (the viewers\' Chrome?)')
        gap_keys = [k for k in issues if 'sequence gap' in k]
        if relay_drops and sum(issues[k] for k in gap_keys) <= relay_drops:
            for k in gap_keys + ['video frame without a marker packet']:
                issues.pop(k, None)
            print(f'  {name}: relay socket dropped {relay_drops} datagram(s) in a burst: sequence gaps not judged')
        for what, n in issues.items():
            fail(f'protocol: {what} x{n}')
        res['send_cadence_ms'] = send_cadence(trace, meta_video_ssrc(assembled, stats), frame_ms, t_connect + 3000, t_imp)
        if not self.args.keep_trace:
            (d / 'trace.csv').unlink(missing_ok=True)
        (d / 'result.json').write_text(json.dumps(res, indent=1))
        return res


def meta_video_ssrc(assembled, stats=()):
    c = collections.Counter(r.get('ssrc') for r in assembled)
    if c:
        return c.most_common(1)[0][0]
    return next((s['video'].get('ssrc') for s in stats if s.get('video', {}).get('ssrc')), None)


# ===================================================================== report
def report(lim, ome_alive, bad_log):
    lines = [f"limits test  stream={lim.args.stream} rendition={lim.args.rendition}  {lim.out}", '']
    by_dim = collections.defaultdict(list)
    for r in lim.results:
        by_dim[r['dim']].append(r)
    for dim in DIMS:
        rs = by_dim.get(dim['name'])
        if not rs:
            continue
        lines.append(f"{dim['name']}: {dim['doc']}")
        lines.append(f"  {'value':>9} {'join s':>6} {'predict':>7} {'freezes':>8} {'longest':>8} {'bound':>6} {'stutter':>7} {'skip':>5} "
                     f"{'nack':>5} {'pli':>4} {'drop':>6} {'e2e ms':>11} {'av':>5}  verdict")
        first_harm = None
        for r in rs:
            i, c = r.get('impair', {}), r.get('chrome', {})
            harm = bool(i.get('freezes') or i.get('stutters'))
            if harm and first_harm is None:
                first_harm = r['value']
            e2e = r.get('e2e_ms', {})
            lines.append(f"  {str(r['value']) + dim['unit'][:2]:>9} {str(r['ctx'].get('join_s')):>6} {r['pred']['verdict']:>7} {i.get('freezes', '-'):>8} "
                         f"{i.get('max_freeze_s', '-'):>8} {str(r['pred'].get('max_freeze_s') or '-'):>6} {i.get('stutters', '-'):>7} "
                         f"{i.get('skipped', '-'):>5} {str(c.get('nackCount')):>5} {str(c.get('pliCount')):>4} {sum(r.get('injected', {}).values()):>6} "
                         f"{str(e2e.get('clean')) + '>' + str(e2e.get('tail')):>11} {str(r.get('av_offset_ms')):>5}  "
                         f"{'FAIL' if r['fails'] else 'ok'}{' (' + r['note'] + ')' if r.get('note') else ''}")
        predicted = next((r['value'] for r in rs if r['pred']['verdict'] != 'below'), None)
        lines.append(f"  first harm at {first_harm}{dim['unit'][:2] if first_harm is not None else ''}; "
                     f"model said harm starts in {predicted}{dim['unit'][:2] if predicted is not None else ''}'s zone. "
                     f"{rs[0]['pred']['why']}")
        lines.append('')
    lines.append(f"server: {'alive' if ome_alive else 'DIED'}; log: {len(bad_log)} unexpected E/C line group(s)")
    for line in bad_log[:10]:
        lines.append(f'  {line}')
    lines.append('')
    if lim.failures:
        lines.append(f'LIMITS FAIL: {len(lim.failures)} failure(s)')
        for step, what in lim.failures:
            lines.append(f'  {step}: {what}')
    else:
        lines.append('LIMITS PASS')
    text = '\n'.join(lines)
    (lim.out / 'summary.txt').write_text(text + '\n')
    (lim.out / 'results.json').write_text(json.dumps(lim.results, indent=1))
    print(text)
    return 1 if lim.failures or not ome_alive or bad_log else 0


# ===================================================================== main
async def main_async(args):
    if not args.allow_production_live:
        live = await asyncio.to_thread(ts.production_live)
        if live:
            print(f'production has {live} live stream(s); refusing (use --allow-production-live)', file=sys.stderr)
            return 2
    if (await ts.api('/vhosts')).get('statusCode') != 0:
        print('something already answers on the sim API port 38081 (traffic_sim running?)', file=sys.stderr)
        return 2
    if args.stream not in STREAMS:
        print(f'--stream: one of {", ".join(STREAMS)}', file=sys.stderr)
        return 2
    dims = [d for d in DIMS if not args.dims or d['name'] in args.dims.split(',')]
    lim = Limits(args)
    run = lim.run
    ome_log = ROOT / 'logs' / 'ovenmediaengine.log'
    lim.log_offset = ome_log.stat().st_size if ome_log.exists() else 0
    conf = ts.make_conf(argparse.Namespace(rtmp_silence_ms=args.rtmp_silence_ms, rtmp_first_media_ms=None))
    features = set() if args.webrtc_features == 'none' else set(args.webrtc_features.split(','))
    unknown = features - set(WEBRTC_FEATURES)
    if unknown:
        sys.exit(f'--webrtc-features: unknown {",".join(sorted(unknown))} (known: {",".join(WEBRTC_FEATURES)}, or none)')
    if args.ulpfec == 'off' or args.playout_delay or features != set(WEBRTC_FEATURES):
        # A copy of whatever conf/ is in use (it turns this fork's opt-in WebRTC features on), with WebRTC ULPFEC off
        # (NACK/RTX stay on), a playout delay and/or some of those features off
        off = ROOT / 'conf_limits'
        off.mkdir(exist_ok=True)
        xml = (conf / 'Server.xml').read_text()
        if args.ulpfec == 'off':
            xml = xml.replace('<Ulpfec>true</Ulpfec>', '<Ulpfec>false</Ulpfec>')
        if args.playout_delay:
            lo, hi = (int(x) for x in args.playout_delay.split(':'))
            adaptive = '<Adaptive>true</Adaptive>' if 'adaptive-playout' in features else ''
            xml = xml.replace('<JitterBuffer>true</JitterBuffer>', '<JitterBuffer>true</JitterBuffer>\n'
                              f'\t\t\t\t\t\t\t<PlayoutDelay><Min>{lo}</Min><Max>{hi}</Max>{adaptive}</PlayoutDelay>')
        for name, element in (('pacing', 'SendPacing'), ('shedding', 'VideoShedding')):
            if name not in features:
                xml = re.sub(rf'\s*<{element}>true</{element}>', '', xml)
        if not features:
            # none: a conf upstream OME reads (it has no <JitterBufferDelay> either; its range is 20:500 ms)
            xml = re.sub(r'\s*<JitterBufferDelay>.*?</JitterBufferDelay>', '', xml)
        (off / 'Server.xml').write_text(xml)
        (off / 'Logger.xml').write_text((conf / 'Logger.xml').read_text())
        conf = off
    control = ROOT / 'logs' / 'relay.sock'
    os.chdir(conf)
    run.ome = await run.spawn('ome', args.bin, '-c', conf, log=lim.out / 'ome_stdout.log', nice=0)
    run.relay = await run.spawn('relay', sys.executable, '-u', ROOT / 'sim_relay.py', '--classes', ROOT / 'classes.json',
                                '--control', control, '--seed', args.seed, log=lim.out / 'relay.log', nice=0)
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set)
    try:
        for _ in range(60):
            await asyncio.sleep(0.5)
            if control.exists() and (await ts.api('/vhosts')).get('statusCode') == 200:
                break
        else:
            print('sim OME or relay did not come up', file=sys.stderr)
            return 2
        run.ctl = await asyncio.open_unix_connection(str(control))
        await lim.publish()
        lim.say(f'publishing {args.stream}; viewer rendition {args.rendition}; {len(dims)} dimension(s)')
        for dim in dims:
            values = QUICK.get(dim['name'], dim['values']) if args.quick else dim['values']
            if args.values:
                values = [type(dim['values'][0])(x) for x in args.values.split(',')]
            for v in values:
                if stop.is_set() or run.ome.returncode is not None:
                    break
                if not args.allow_production_live and await asyncio.to_thread(ts.production_live):
                    lim.say('production went live: stopping')
                    stop.set()
                    break
                lim.say(f"{dim['name']} = {v} {dim['unit']}")
                res = await lim.step(dim, v)
                lim.results.append(res)
                i = res.get('impair', {})
                lim.say(f"  predicted {res['pred']['verdict']}: freezes {i.get('freezes')} (longest {i.get('max_freeze_s')} s, "
                        f"bound {res['pred'].get('max_freeze_s')}), stutters {i.get('stutters')}, e2e {res.get('e2e_ms')}, "
                        f"{'FAIL' if res['fails'] else 'ok'}")
        ome_alive = run.ome.returncode is None
        bad, _ = ts.scan_log(run, lim.log_offset)
        bad_lines = [f'{n}x {k}' for k, n in bad.most_common(15)] if bad else []
        return report(lim, ome_alive, bad_lines)
    finally:
        for p in list(run.procs):
            run.signal(p, signal.SIGTERM if p is run.ome else signal.SIGKILL)
        for p in list(run.procs):
            await run.reap(p, 15)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--stream', default='s720', help='s720 (Bypass, no B-frames, 8 s GOP), s1080, gpu4k')
    ap.add_argument('--rendition', default='Bypass+opus', help="a rendition of the playlist, or 'auto' for OME's auto ABR")
    ap.add_argument('--reference-probe', metavar='RENDITIONS',
                    help="also capture on a clean path (steps/*/reference_N/), one per comma-separated rendition ('-' = the "
                         "viewer's own URL), to compare a capture with what OME sent byte for byte, e.g. 1080p,720p,480p")
    ap.add_argument('--viewers', type=int, default=1, help='judged viewers per step (the extra ones: steps/*/viewer_N/, see viewers_report.py)')
    ap.add_argument('--extra-viewers', choices=('same', 'local'), default='same',
                    help="where --viewers' extra viewers run: 'same' as the main viewer (decoded and shown), or 'local' "
                         "(Chrome here, not decoded, judged on delivery): one desktop shows only so many 60 fps videos")
    ap.add_argument('--remote-viewer', metavar='CDP_URL',
                    help='judge a Chrome on another machine (started with --remote-debugging-port), e.g. http://<desktop-ip>:19222; '
                         'the probe stays local. --chrome-log does not apply to it')
    ap.add_argument('--remote-firefox', metavar='BIDI_URL',
                    help='judge a Firefox on another machine (started with --remote-debugging-port; WebDriver BiDi), '
                         'e.g. ws://<desktop-ip>:19224. The extra viewers (--extra-viewers same) use it too')
    ap.add_argument('--playlist', default='auto', help="the playlist's file name (gpu4k also has 'abr': 1080p/720p/480p only)")
    ap.add_argument('--dims', help='comma-separated subset of: ' + ', '.join(d['name'] for d in DIMS))
    ap.add_argument('--quick', action='store_true', help='three or four values per dimension')
    ap.add_argument('--values', help='comma-separated values for the (single) dimension, instead of its sweep')
    ap.add_argument('--chrome-log', action='store_true',
                    help='oracle Chromium logs its WebRTC receive path (frame buffer, NACK, packet buffer) to oracle.log')
    ap.add_argument('--clean-s', type=float, default=12)
    ap.add_argument('--impair-s', type=float, default=30)
    ap.add_argument('--recover-s', type=float, default=20)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--rtmp-silence-ms', type=int, default=10000, help='as production after the zombie fix')
    ap.add_argument('--bottleneck', metavar='MBPS:QUEUE_MS',
                    help='a drop-tail bottleneck toward the viewer under every step, e.g. 10:100 (default: none)')
    ap.add_argument('--playout-delay', metavar='MIN:MAX',
                    help="WebRTC <PlayoutDelay> in ms (the playout-delay RTP extension), e.g. 50:200 (default: none, as production)")
    ap.add_argument('--webrtc-features', default=','.join(WEBRTC_FEATURES),
                    help="this fork's opt-in WebRTC features to turn on, comma-separated, or none (also drops <JitterBufferDelay>: "
                         "upstream OME and builds from before the switches reject these elements): pacing = <SendPacing>, shedding = <VideoShedding>, "
                         "adaptive-playout = <PlayoutDelay><Adaptive> (only with --playout-delay)")
    ap.add_argument('--ulpfec', choices=['on', 'off'], default='on', help='off: run with WebRTC <Ulpfec>false (NACK/RTX still on)')
    ap.add_argument('--pub', choices=['paced', 'ffmpeg'], default='paced',
                    help='paced: rtmp_pub.py sends each frame on time from the .flv next to the source (made with ffmpeg -c copy); '
                         'ffmpeg: ffmpeg -re from the .ts, whose output jitters')
    ap.add_argument('--viewer-transform', action='store_true',
                    help='give the judged viewer an encoded transform too (assembly record), which changes what Chrome does in bursts')
    ap.add_argument('--no-probe', action='store_true', help='no capture probe, so no decode check')
    ap.add_argument('--keep-capture', action='store_true')
    ap.add_argument('--keep-trace', action='store_true')
    ap.add_argument('--allow-production-live', action='store_true')
    ap.add_argument('--bin', default=str(ts.REPO / 'build' / 'Release' / 'bin' / 'OvenMediaEngine'))
    ap.add_argument('--out')
    sys.exit(asyncio.run(main_async(ap.parse_args())))


if __name__ == '__main__':
    main()
