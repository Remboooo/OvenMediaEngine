#!/usr/bin/env python3
"""Theoretical best case for a WebRTC viewer under one simulated network condition, with this deployment's protocol
and configuration fixed: WebRTC (SRTP over UDP), NACK -> RTX, ULPFEC, the playout-delay extension starting at
<PlayoutDelay> Min 50 ms, H.264 passed through from the encoder (Bypass: the server cannot make a keyframe) with a
1 s GOP.

The "ideal" server and receiver behind these numbers:
  - the receiver notices a hole when the next packet arrives and NACKs at once; the server answers at once, so a
    repair round costs one RTT plus one packet interval, and lost repairs are asked again every RTT;
  - the receiver holds frames for exactly the buffer B the condition needs (repair rounds, jitter tail, reordering,
    stall), chosen per session (the playout-delay extension allows it), at most B_CAP;
  - a packet still missing at its deadline costs a freeze until the next source keyframe (Bypass: on average half a
    GOP away, as no keyframe can be requested); ULPFEC is not credited (it only matters where no repair round fits);
  - a link slower than the stream (pinned rendition, no ABR) can at best show the share of GOPs it carries.

So the best case is a pair: the freezes nothing can avoid, and the latency that buys it (one-way delay + B + the
receiver's decode/render pipeline). Formulas are first-order; each result says how it was reached.

    best_case.py            prints the smoke profiles and the limits quick matrix as JSON
"""
import json
import math

PLAYOUT_MIN_MS = 50
B_CAP_MS = 1000            # longest per-session buffer a live viewer is given
GOP_S = 1.0
PIPE_MS = 10               # decode + render after the jitter buffer: clean runs e2e - jitter buffer, Chrome ~18 ms, Firefox ~5 ms
FREEZE_GAP_MS = 150        # W3C: a gap > max(3 frames, frame + 150 ms)
NACK_LIST = 1000           # libwebrtc NackRequester: a longer hole is given up (keyframe needed)


def _keyframe_freezes(p_unrec, pps, window_s):
    """Freezes and freeze time when each packet is lost for good with probability p_unrec: one freeze per GOP that
    has such a packet, lasting until the next keyframe (half a GOP on average)."""
    per_gop = 1 - math.exp(-pps * GOP_S * p_unrec)
    n = window_s / GOP_S * per_gop
    return n, n * GOP_S / 2


def best(net, ctx, window_s):
    """net: impairment (impair_relay / sim_relay profile 'down'/'up' keys); ctx: stream (pps, bitrate_bps,
    kf_bytes, frame_ms) and path (base_rtt_ms). Returns dict(freezes, freeze_s, shown, e2e_ms, buffer_ms, why)."""
    down, up = net.get('down', {}), net.get('up', {})
    owd = down.get('delay_ms', 0) + ctx.get('base_rtt_ms', 1) / 2
    rtt = down.get('delay_ms', 0) + up.get('delay_ms', 0) + ctx.get('base_rtt_ms', 1)
    pps, frame_ms = ctx['pps'], ctx['frame_ms']
    pkt_ms = 1000 / pps
    sigma = down.get('jitter_ms', 0)
    p = down.get('loss', 0)
    burst = down.get('burst')
    if burst:
        # Gilbert-Elliott: stationary loss share; retransmissions come much later, so they see that share too
        pi_bad = burst['p_enter'] / (burst['p_enter'] + burst['p_exit'])
        p = p + pi_bad * burst.get('loss', 1.0)
        mean_burst = 1 / burst['p_exit']
    else:
        mean_burst = 1
    needs = {'min': PLAYOUT_MIN_MS}
    why = []
    freezes = freeze_s = 0.0
    # Jitter: FIFO queueing, a buffer of 3 sigma covers all but 0.1% of packets
    if sigma:
        needs['jitter'] = 3 * sigma
    if down.get('reorder'):
        needs['reorder'] = down.get('reorder_ms', 0)
    # Loss: one repair round (the hole is seen after the burst ends), then resends every RTT
    if p:
        first = rtt + pkt_ms * mean_burst + 3 * sigma
        needs['repair'] = first
        rounds = 1 + max(0, math.floor((B_CAP_MS - first) / max(rtt, 1)))
        # Use as many rounds as keep the residual under one lost-for-good packet per hour; more only add delay
        k = 1
        while k < rounds and pps * 3600 * p ** (k + 1) > 1:
            k += 1
        needs['repair'] = first + (k - 1) * rtt
        fits = first <= B_CAP_MS
        p_unrec = p ** (k + 1) if fits else p
        n, s = _keyframe_freezes(p_unrec, pps, window_s)
        freezes += n
        freeze_s += s
        why.append(f'loss {p:.2%}: {k if fits else 0} repair round(s) of {rtt:.0f} ms, {p_unrec * pps * window_s:.2g} packet(s) lost for good')
    # Stalls: everything late by the stall at once; a buffer as long as the stall hides it
    if down.get('stall_ms'):
        st = down['stall_ms']
        needs['stall'] = st
        if st > B_CAP_MS:
            n = window_s / down['stall_every_s']
            freezes += n
            freeze_s += n * (st - B_CAP_MS) / 1000
        why.append(f"stall {st} ms every {down['stall_every_s']} s: hidden by a {st} ms buffer")
    # Bottleneck: below the stream's rate only whole GOPs can be dropped; above it, a keyframe drains on the spare rate
    if down.get('rate_mbps'):
        cap, avg = down['rate_mbps'] * 1e6, ctx['bitrate_bps']
        if cap < avg:
            share = cap / avg
            freezes += (1 - share) * window_s / GOP_S
            freeze_s += (1 - share) * window_s
            why.append(f'link {cap / 1e6:.1f} < stream {avg / 1e6:.1f} Mbps: at best {share:.0%} of GOPs shown')
        else:
            # The sender paces the keyframe at the spare rate (no queue overflow); the viewer buffers that long
            kf_ms = 1000 * ctx['kf_bytes'] * 8 / max(cap - avg, 1)
            needs['keyframe drain'] = kf_ms
            why.append(f"keyframe drains in {kf_ms:.0f} ms on the spare {(cap - avg) / 1e6:.1f} Mbps")
    # One outage of T ms (limits.py 'outage'): frozen T; the hole is repaired after it unless longer than the NACK list
    if net.get('outage_ms'):
        t = net['outage_ms']
        lost = t / 1000 * pps
        extra = rtt if lost <= NACK_LIST else GOP_S * 500
        freezes += 1 if t + extra > FREEZE_GAP_MS else 0
        freeze_s += (t + extra) / 1000 if t + extra > FREEZE_GAP_MS else 0
        why.append(f'outage {t} ms: {lost:.0f} packets, ' + ('repaired one RTT later' if lost <= NACK_LIST else 'more than the NACK list: wait for a keyframe'))
    if net.get('hold_ms'):
        needs['stall'] = net['hold_ms']
    buf = max(needs.values())
    if buf > B_CAP_MS:
        buf = B_CAP_MS
    shown = max(0.0, 1 - freeze_s / window_s)
    return dict(freezes=round(freezes, 2), freeze_s=round(freeze_s, 2), shown=round(shown, 4),
                buffer_ms=round(buf), e2e_ms=round(owd + buf + PIPE_MS), buffer_for=max(needs, key=needs.get), why='; '.join(why))


# Streams as measured: 4K Bypass (smoke test) and the gpu4k 1080p60 rung (limits.py)
STREAM_4K = dict(pps=1650, bitrate_bps=19e6, kf_bytes=320e3, frame_ms=1000 / 59.94, base_rtt_ms=1)
STREAM_1080 = dict(pps=600, bitrate_bps=6e6, kf_bytes=110e3, frame_ms=1000 / 60, base_rtt_ms=1)


def smoke_profiles(profiles_dir):
    import pathlib
    out = {}
    for name in ('clean', 'loss_1pct', 'burst_loss', 'jitter_reorder', 'stalls', 'real_world_mix'):
        net = json.loads((pathlib.Path(profiles_dir) / f'{name}.json').read_text())
        out[name] = best(net, STREAM_4K, 52)
    return out


def limits_steps(ctx=STREAM_1080, impair_s=30):
    """The quick matrix of limits.py, its nets rebuilt here (keep in step with limits.DIMS)."""
    nets = {
        'loss': lambda v: {'down': {'loss': v / 100, 'delay_ms': 15}, 'up': {'delay_ms': 15}},
        'burst': lambda v: {'down': {'burst': {'p_enter': 0.01 / v, 'p_exit': 1 / v, 'loss': 1.0}, 'delay_ms': 15}, 'up': {'delay_ms': 15}},
        'rtt': lambda v: {'down': {'loss': 0.01, 'delay_ms': v / 2}, 'up': {'delay_ms': v / 2}},
        'jitter': lambda v: {'down': {'delay_ms': 20 + 2 * v, 'jitter_ms': v}, 'up': {'delay_ms': 15}},
        'reorder': lambda v: {'down': {'reorder': 0.02, 'reorder_ms': v, 'delay_ms': 15}, 'up': {'delay_ms': 15}},
        'outage': lambda v: {'down': {'delay_ms': 15}, 'up': {'delay_ms': 15}, 'outage_ms': v},
        'rate': lambda v: {'down': {'rate_mbps': v * ctx['bitrate_bps'] / 1e6, 'queue_ms': 300, 'delay_ms': 15}, 'up': {'delay_ms': 15}},
    }
    quick = {'loss': [0.5, 2, 10, 35], 'burst': [4, 32, 128], 'rtt': [50, 200, 800], 'jitter': [15, 60, 240],
             'reorder': [25, 100, 200], 'outage': [250, 1000, 4000], 'rate': [2.0, 1.2, 0.8]}
    return {f'{d}={v}': best(nets[d](v), ctx, impair_s) for d, vs in quick.items() for v in vs}


if __name__ == '__main__':
    import pathlib
    here = pathlib.Path(__file__).resolve().parent
    print(json.dumps(dict(smoke=smoke_profiles(here / 'profiles'), limits=limits_steps()), indent=1))
