#!/usr/bin/env python3
"""Minimal LL-HLS client: master → A/V chunklists with CAN-BLOCK-RELOAD, fetch parts.

Mirrors how Safari / hls.js LL mode behave (Delivery Directives _HLS_msn/_HLS_part).
Exit 0 if it sustains playable media for --duration-s without stall/404.
"""
from __future__ import annotations

import argparse
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from typing import List, Optional, Tuple


def http_get(url: str, timeout: float) -> Tuple[int, bytes, float]:
	t0 = time.monotonic()
	req = urllib.request.Request(url, headers={"User-Agent": "OME-LLHLS-Client/1.0", "Accept": "*/*"})
	try:
		with urllib.request.urlopen(req, timeout=timeout) as resp:
			body = resp.read()
			return int(resp.status), body, time.monotonic() - t0
	except urllib.error.HTTPError as e:
		return int(e.code), e.read() if e.fp else b"", time.monotonic() - t0
	except Exception as e:
		return 0, str(e).encode(), time.monotonic() - t0


def abs_url(base: str, ref: str) -> str:
	return urllib.parse.urljoin(base if base.endswith("/") else base + "/", ref)


@dataclass
class Part:
	msn: int
	psn: int
	duration: float
	uri: str
	independent: bool = False


@dataclass
class MediaPlaylist:
	url: str
	target_duration: float = 6.0
	part_target: float = 1.0
	part_hold_back: float = 3.0
	media_sequence: int = 0
	parts: List[Part] = field(default_factory=list)
	init_uri: Optional[str] = None
	can_block: bool = False
	raw: str = ""

	@property
	def last_msn_psn(self) -> Tuple[int, int]:
		if not self.parts:
			return self.media_sequence, -1
		p = self.parts[-1]
		return p.msn, p.psn


_PART_RE = re.compile(
	r'#EXT-X-PART:DURATION=(?P<dur>[0-9.]+),URI="(?P<uri>[^"]+)"(?P<rest>[^\n]*)'
)
_MAP_RE = re.compile(r'#EXT-X-MAP:URI="([^"]+)"')
_MSN_RE = re.compile(r"#EXT-X-MEDIA-SEQUENCE:(\d+)")
_TARGET_RE = re.compile(r"#EXT-X-TARGETDURATION:(\d+)")
_PART_INF_RE = re.compile(r"#EXT-X-PART-INF:PART-TARGET=([0-9.]+)")
_CTRL_RE = re.compile(
	r"#EXT-X-SERVER-CONTROL:.*?CAN-BLOCK-RELOAD=(YES|NO).*?PART-HOLD-BACK=([0-9.]+)"
)
_SEG_RE = re.compile(r'#EXTINF:[0-9.]+,\s*\n([^\n#]+)')


def parse_media(url: str, text: str) -> MediaPlaylist:
	pl = MediaPlaylist(url=url, raw=text)
	m = _MSN_RE.search(text)
	if m:
		pl.media_sequence = int(m.group(1))
	m = _TARGET_RE.search(text)
	if m:
		pl.target_duration = float(m.group(1))
	m = _PART_INF_RE.search(text)
	if m:
		pl.part_target = float(m.group(1))
	m = _CTRL_RE.search(text)
	if m:
		pl.can_block = m.group(1) == "YES"
		pl.part_hold_back = float(m.group(2))
	m = _MAP_RE.search(text)
	if m:
		pl.init_uri = m.group(1)

	# Infer msn from part URI: part_<track>_<msn>_<psn>_...
	for m in _PART_RE.finditer(text):
		uri = m.group("uri")
		dur = float(m.group("dur"))
		indep = "INDEPENDENT=YES" in m.group("rest")
		pm = re.search(r"part_\d+_(\d+)_(\d+)_", uri)
		if not pm:
			# fallback: sequential within unknown msn
			msn, psn = pl.media_sequence, len(pl.parts)
		else:
			msn, psn = int(pm.group(1)), int(pm.group(2))
		pl.parts.append(Part(msn=msn, psn=psn, duration=dur, uri=uri, independent=indep))
	return pl


def blocking_reload(pl: MediaPlaylist, msn: int, psn: int, timeout: float) -> MediaPlaylist:
	parsed = urllib.parse.urlparse(pl.url)
	q = urllib.parse.parse_qs(parsed.query, keep_blank_values=True)
	# keep session= etc, set delivery directives
	flat = {k: v[0] for k, v in q.items() if not k.startswith("_HLS_")}
	flat["_HLS_msn"] = str(msn)
	flat["_HLS_part"] = str(psn)
	new_q = urllib.parse.urlencode(flat)
	url = urllib.parse.urlunparse(parsed._replace(query=new_q))
	code, body, dt = http_get(url, timeout=timeout)
	if code != 200:
		raise RuntimeError(f"blocking reload HTTP {code} after {dt:.2f}s url={url} body={body[:200]!r}")
	# Server may return a playlist that still doesn't contain the part (bug) —
	# treat as soft fail for the caller to detect.
	out = parse_media(pl.url, body.decode("utf-8", "replace"))
	out._block_dt = dt  # type: ignore[attr-defined]
	return out


def fetch_bytes(base_playlist_url: str, rel: str, timeout: float = 10.0) -> Tuple[int, int, float]:
	url = abs_url(base_playlist_url, rel)
	code, body, dt = http_get(url, timeout=timeout)
	return code, len(body), dt


def run(url: str, duration_s: float, stall_s: float) -> int:
	print(f"LLHLS client start url={url} duration={duration_s}s stall_limit={stall_s}s", flush=True)
	code, body, dt = http_get(url, timeout=15)
	if code != 200:
		print(f"FAIL master HTTP {code} ({dt:.2f}s)", flush=True)
		return 1
	master = body.decode("utf-8", "replace")
	print(f"OK master ({dt:.2f}s, {len(body)} bytes)", flush=True)
	print(master, flush=True)

	# Collect media playlist URIs (video STREAM-INF line + AUDIO URI=)
	media_uris: List[Tuple[str, str]] = []
	for m in re.finditer(r'URI="([^"]+\.m3u8[^"]*)"', master):
		media_uris.append(("audio", m.group(1)))
	# STREAM-INF URI is the next non-tag line
	lines = master.splitlines()
	for i, line in enumerate(lines):
		if line.startswith("#EXT-X-STREAM-INF:"):
			for j in range(i + 1, len(lines)):
				if lines[j] and not lines[j].startswith("#"):
					media_uris.append(("video", lines[j].strip()))
					break

	if not media_uris:
		print("FAIL no media playlists in master", flush=True)
		return 1

	playlists: dict[str, MediaPlaylist] = {}
	for kind, ref in media_uris:
		mu = abs_url(url, ref)
		code, body, dt = http_get(mu, timeout=15)
		if code != 200:
			print(f"FAIL {kind} playlist HTTP {code} ({dt:.2f}s) {mu}", flush=True)
			return 1
		pl = parse_media(mu, body.decode("utf-8", "replace"))
		playlists[kind] = pl
		print(
			f"OK {kind} playlist ({dt:.2f}s) parts={len(pl.parts)} "
			f"edge={pl.last_msn_psn} can_block={pl.can_block} part_target={pl.part_target} "
			f"hold_back={pl.part_hold_back}",
			flush=True,
		)
		if pl.init_uri:
			c, n, d = fetch_bytes(mu, pl.init_uri)
			ok = c == 200 and n > 0
			print(("OK" if ok else "FAIL") + f" {kind} init HTTP {c} size={n} ({d:.2f}s)", flush=True)
			if not ok:
				return 1

	# Require at least one part on video
	video = playlists.get("video") or next(iter(playlists.values()))
	if not video.parts:
		print("FAIL video playlist has no EXT-X-PART entries (not LL-HLS edge)", flush=True)
		print(video.raw[-800:], flush=True)
		return 1

	# Download existing trailing parts (join near live edge)
	join_from = max(0, len(video.parts) - 3)
	media_s = 0.0
	bytes_total = 0
	t_end = time.monotonic() + duration_s
	last_media_at = time.monotonic()
	failures = 0

	def ingest_part(pl: MediaPlaylist, part: Part, label: str) -> bool:
		nonlocal media_s, bytes_total, last_media_at, failures
		c, n, d = fetch_bytes(pl.url, part.uri, timeout=max(10.0, stall_s))
		if c != 200 or n < 32:
			print(f"FAIL {label} part msn={part.msn} psn={part.psn} HTTP {c} size={n} ({d:.2f}s)", flush=True)
			failures += 1
			return False
		media_s += part.duration
		bytes_total += n
		last_media_at = time.monotonic()
		print(
			f"OK {label} part msn={part.msn} psn={part.psn} size={n} dur={part.duration:.3f}s "
			f"({d:.2f}s) media_total={media_s:.1f}s",
			flush=True,
		)
		return True

	seen = set()
	for part in video.parts[join_from:]:
		key = (part.msn, part.psn)
		if key in seen:
			continue
		seen.add(key)
		if not ingest_part(video, part, "video"):
			return 1

	# Live loop: blocking reload for next part, fetch it (and audio if present)
	audio = playlists.get("audio")
	cur_msn, cur_psn = video.last_msn_psn
	next_msn, next_psn = cur_msn, cur_psn + 1

	while time.monotonic() < t_end:
		# Stall watchdog
		idle = time.monotonic() - last_media_at
		if idle > stall_s:
			print(f"FAIL stall: no media for {idle:.1f}s > {stall_s}s", flush=True)
			return 1

		block_timeout = max(video.part_hold_back * 3.0, stall_s, 15.0)
		try:
			t0 = time.monotonic()
			video = blocking_reload(video, next_msn, next_psn, timeout=block_timeout)
			block_dt = getattr(video, "_block_dt", time.monotonic() - t0)
			playlists["video"] = video
		except Exception as e:
			print(f"FAIL blocking reload msn={next_msn} part={next_psn}: {e}", flush=True)
			return 1

		# Find requested part (or later)
		found = None
		for part in video.parts:
			if part.msn > next_msn or (part.msn == next_msn and part.psn >= next_psn):
				found = part
				break
		if found is None:
			print(
				f"FAIL blocking returned in {block_dt:.2f}s but msn={next_msn} part>={next_psn} missing; "
				f"edge={video.last_msn_psn}",
				flush=True,
			)
			print(video.raw[-600:], flush=True)
			return 1

		print(
			f"OK block msn={next_msn} part={next_psn} → got {found.msn}/{found.psn} in {block_dt:.2f}s",
			flush=True,
		)

		# Fetch any unseen parts up through found
		for part in video.parts:
			key = (part.msn, part.psn)
			if key in seen:
				continue
			if part.msn < found.msn or (part.msn == found.msn and part.psn <= found.psn):
				seen.add(key)
				if not ingest_part(video, part, "video"):
					return 1

		# Keep audio roughly in sync via blocking reload too
		if audio is not None and audio.can_block:
			a_msn, a_psn = audio.last_msn_psn
			try:
				audio = blocking_reload(audio, max(a_msn, found.msn), 0 if found.msn > a_msn else a_psn + 1, timeout=block_timeout)
				playlists["audio"] = audio
				for part in audio.parts[-2:]:
					key = ("a", part.msn, part.psn)
					if key in seen:
						continue
					seen.add(key)
					ingest_part(audio, part, "audio")
			except Exception as e:
				print(f"WARN audio block: {e}", flush=True)

		# Advance cursor to just after found
		next_msn, next_psn = found.msn, found.psn + 1

	print(
		f"DONE ok media={media_s:.1f}s bytes={bytes_total} parts={len(seen)} failures={failures}",
		flush=True,
	)
	return 0 if failures == 0 and media_s >= min(5.0, duration_s * 0.5) else 1


def main() -> int:
	ap = argparse.ArgumentParser(description=__doc__)
	ap.add_argument("url", nargs="?", default="http://127.0.0.1:3333/_filler/_filler/llhls.m3u8")
	ap.add_argument("--duration-s", type=float, default=30.0)
	ap.add_argument("--stall-s", type=float, default=12.0)
	args = ap.parse_args()
	return run(args.url, args.duration_s, args.stall_s)


if __name__ == "__main__":
	sys.exit(main())
