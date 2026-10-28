#!/usr/bin/env python3
"""Minimal RTMP publisher: sends an FLV file's tags at their own timestamps, looping, like an encoder
that emits each frame on time.

  rtmp_pub.py SRC.flv rtmp://127.0.0.1:31936/app/stream [--offset-s 16]

Why not ffmpeg: ffmpeg -re paces in its demuxer, and its output jitters by +-10 ms (FLV source) up to
+-40 ms (MPEG-TS source, whose demuxer hands out frames in bursts); its muxer adds the rest even when
fed on time. OME's send-side pacer passes ingest timing on unchanged, so that jitter reached the
viewers as judder and made a poor baseline. This writes every tag within about a millisecond of its
timestamp (TCP_NODELAY, one send per tag).

--offset-s starts at the first keyframe at or after that point. Timestamps start at 0 and keep
increasing across loops. Exits 1 when the connection fails or OME closes it.
"""
import argparse
import os
import socket
import struct
import sys
import threading
import time
import urllib.parse

CHUNK = 65536


# ------------------------------------------------------------ AMF0
def amf(v):
    if v is None:
        return b'\x05'
    if isinstance(v, bool):
        return b'\x01' + bytes([v])
    if isinstance(v, (int, float)):
        return b'\x00' + struct.pack('>d', float(v))
    if isinstance(v, str):
        b = v.encode()
        return b'\x02' + struct.pack('>H', len(b)) + b
    if isinstance(v, dict):
        out = b'\x03'
        for k, x in v.items():
            kb = k.encode()
            out += struct.pack('>H', len(kb)) + kb + amf(x)
        return out + b'\x00\x00\x09'
    raise TypeError(type(v))


def amf_read(b, i):
    """One AMF0 value from b at i: (value, next index). Only what OME's replies use."""
    t = b[i]
    if t == 0x00:
        return struct.unpack('>d', b[i + 1:i + 9])[0], i + 9
    if t == 0x01:
        return bool(b[i + 1]), i + 2
    if t == 0x02:
        n = struct.unpack('>H', b[i + 1:i + 3])[0]
        return b[i + 3:i + 3 + n].decode(errors='replace'), i + 3 + n
    if t in (0x03, 0x08):
        i += 1 if t == 0x03 else 5
        obj = {}
        while True:
            n = struct.unpack('>H', b[i:i + 2])[0]
            if n == 0 and b[i + 2] == 0x09:
                return obj, i + 3
            k = b[i + 2:i + 2 + n].decode(errors='replace')
            obj[k], i = amf_read(b, i + 2 + n)
    if t in (0x05, 0x06):
        return None, i + 1
    raise ValueError(f'AMF type {t}')


# ------------------------------------------------------------ RTMP
class Rtmp:
    def __init__(self, url):
        u = urllib.parse.urlparse(url)
        self.host, self.port = u.hostname, u.port or 1935
        parts = u.path.strip('/').split('/')
        self.app, self.stream = '/'.join(parts[:-1]), parts[-1]
        self.tc_url = f'rtmp://{self.host}:{self.port}/{self.app}'
        self.sock = socket.create_connection((self.host, self.port), timeout=10)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.in_chunk = 128
        self.rbuf = b''
        self.last = {}  # inbound csid -> (timestamp, length, type, stream id)

    def recv_exact(self, n):
        while len(self.rbuf) < n:
            d = self.sock.recv(65536)
            if not d:
                raise ConnectionError('closed by server')
            self.rbuf += d
        out, self.rbuf = self.rbuf[:n], self.rbuf[n:]
        return out

    def handshake(self):
        c1 = struct.pack('>II', int(time.time()) & 0xFFFFFFFF, 0) + os.urandom(1528)
        self.sock.sendall(b'\x03' + c1)
        self.recv_exact(1)
        s1 = self.recv_exact(1536)
        self.recv_exact(1536)  # S2
        self.sock.sendall(s1)  # C2 echoes S1

    def send(self, csid, mtype, sid, ts, payload):
        """One message, fmt 0 then fmt 3 continuation chunks."""
        ext = ts >= 0xFFFFFF
        hdr = bytes([csid]) + (min(ts, 0xFFFFFF)).to_bytes(3, 'big') + len(payload).to_bytes(3, 'big') + bytes([mtype]) \
            + struct.pack('<I', sid) + (struct.pack('>I', ts) if ext else b'')
        out = [hdr, payload[:CHUNK]]
        for i in range(CHUNK, len(payload), CHUNK):
            out.append(bytes([0xC0 | csid]) + (struct.pack('>I', ts) if ext else b''))
            out.append(payload[i:i + CHUNK])
        self.sock.sendall(b''.join(out))

    def read_message(self):
        """Reassembles one inbound message: (type, stream id, payload)."""
        partial = {}
        while True:
            b0 = self.recv_exact(1)[0]
            fmt, csid = b0 >> 6, b0 & 0x3F
            if csid == 0:
                csid = 64 + self.recv_exact(1)[0]
            elif csid == 1:
                b = self.recv_exact(2)
                csid = 64 + b[0] + 256 * b[1]
            ts, length, mtype, sid = self.last.get(csid, (0, 0, 0, 0))
            if fmt == 0:
                h = self.recv_exact(11)
                ts, length, mtype, sid = int.from_bytes(h[:3], 'big'), int.from_bytes(h[3:6], 'big'), h[6], struct.unpack('<I', h[7:11])[0]
            elif fmt == 1:
                h = self.recv_exact(7)
                ts, length, mtype = int.from_bytes(h[:3], 'big'), int.from_bytes(h[3:6], 'big'), h[6]
            elif fmt == 2:
                ts = int.from_bytes(self.recv_exact(3), 'big')
            if fmt < 3 and ts == 0xFFFFFF:
                self.recv_exact(4)
            self.last[csid] = (ts, length, mtype, sid)
            buf = partial.get(csid, b'')
            buf += self.recv_exact(min(self.in_chunk, length - len(buf)))
            if len(buf) < length:
                partial[csid] = buf
                continue
            partial.pop(csid, None)
            if mtype == 1:
                self.in_chunk = struct.unpack('>I', buf[:4])[0] & 0x7FFFFFFF
                continue
            return mtype, sid, buf

    def command(self, name, txn, *args, sid=0, csid=3):
        self.send(csid, 20, sid, 0, amf(name) + amf(txn) + b''.join(amf(a) for a in args))

    def wait_for(self, want):
        """Reads until a command named in want; returns (name, values)."""
        while True:
            mtype, _, b = self.read_message()
            if mtype != 20:
                continue
            vals, i = [], 0
            while i < len(b):
                v, i = amf_read(b, i)
                vals.append(v)
            if vals and vals[0] in want:
                return vals[0], vals
            if vals and vals[0] == '_error':
                raise ConnectionError(f'server error: {vals}')

    def publish(self):
        self.handshake()
        self.send(2, 1, 0, 0, struct.pack('>I', CHUNK))
        self.command('connect', 1, {'app': self.app, 'type': 'nonprivate', 'flashVer': 'FMLE/3.0 (compatible; rtmp_pub)',
                                    'tcUrl': self.tc_url})
        self.wait_for({'_result'})
        self.command('releaseStream', 2, None, self.stream)
        self.command('FCPublish', 3, None, self.stream)
        self.command('createStream', 4, None)
        while True:
            name, vals = self.wait_for({'_result'})
            if vals[1] == 4:
                break
        self.sid = int(vals[3])
        self.command('publish', 5, None, self.stream, 'live', sid=self.sid, csid=4)
        # Drain whatever OME sends from here on (onStatus, acknowledgements) so its writes never block
        threading.Thread(target=self.drain, daemon=True).start()

    def drain(self):
        try:
            while self.sock.recv(65536):
                pass
        except OSError:
            pass
        self.closed = True


# ------------------------------------------------------------ FLV
class Tag:
    """One FLV tag: its payload is read from the file when sent (a 4K recording is gigabytes)."""
    __slots__ = ('typ', 'ts', 'pos', 'size', 'head')

    def __init__(self, typ, ts, pos, size, head):
        self.typ, self.ts, self.pos, self.size, self.head = typ, ts, pos, size, head


def flv_tags(f):
    f.seek(0)
    h = f.read(9)
    pos = int.from_bytes(h[5:9], 'big') + 4
    end = f.seek(0, 2)
    while pos + 11 <= end:
        f.seek(pos)
        h = f.read(13)
        typ = h[0]
        size = int.from_bytes(h[1:4], 'big')
        ts = int.from_bytes(h[4:7], 'big') | (h[7] << 24)
        if pos + 11 + size > end:
            break
        yield Tag(typ, ts, pos + 11, size, h[11:13])
        pos += 11 + size + 4


def payload(f, tag):
    f.seek(tag.pos)
    return f.read(tag.size)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('src')
    ap.add_argument('url')
    ap.add_argument('--offset-s', type=float, default=0)
    args = ap.parse_args()

    f = open(args.src, 'rb')
    tags = list(flv_tags(f))
    # AVC/AAC sequence headers (packet type 0) and metadata go once, before the media
    is_header = lambda t: (t.typ == 9 and t.head[1] == 0) or (t.typ == 8 and (t.head[0] >> 4) == 10 and t.head[1] == 0)
    headers = [t for t in tags if t.typ == 18 or is_header(t)]
    media = [t for t in tags if t.typ in (8, 9) and not is_header(t)]
    keys = [i for i, t in enumerate(media) if t.typ == 9 and (t.head[0] >> 4) == 1]
    start_i = next((i for i in keys if media[i].ts >= args.offset_s * 1000), keys[0])
    span = media[-1].ts - media[0].ts + 33

    r = Rtmp(args.url)
    r.closed = False
    r.publish()
    for t in headers:
        data = payload(f, t)
        if t.typ == 18:
            data = amf('@setDataFrame') + data
        r.send(5 if t.typ == 18 else 6 if t.typ == 8 else 7, t.typ, r.sid, 0, data)

    # One continuous schedule from the starting keyframe: item i plays at its file time, plus one
    # file length per completed loop, so timestamps keep increasing without gaps
    n = len(media)
    base = media[start_i].ts
    t0 = time.monotonic()
    i = start_i
    try:
        while not r.closed:
            tag = media[i % n]
            data = payload(f, tag)  # read before waiting, so the send itself is on time
            t = tag.ts - base + (i // n) * span
            wait = t0 + t / 1000 - time.monotonic()
            if wait > 0:
                time.sleep(wait)
            r.send(6 if tag.typ == 8 else 7, tag.typ, r.sid, max(0, t), data)
            i += 1
    except (OSError, ConnectionError) as e:
        print(f'rtmp_pub: {e}', file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main())
