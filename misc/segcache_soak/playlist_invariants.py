#!/usr/bin/env python3
"""
Poll classic HLS media playlists and assert live-playlist invariants that
hls.js is sensitive to (hypotheses 1–3 for long-run stalls).

Fails when:
  1) EXT-X-DISCONTINUITY-SEQUENCE decreases, or vanishes after it was > 0
  2) EXT-X-PROGRAM-DATE-TIME jumps backward across a non-discontinuity boundary
  3) Live window collapses below floor for sustained samples (wrap shrink)

Usage:
  SOAK_DOGFOOD=1 python3 playlist_invariants.py --duration-s 400
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import List, Optional, Tuple
from urllib.parse import urljoin


def env(name: str, default: str) -> str:
	return os.environ.get(name, default)


def dogfood_defaults() -> None:
	if env("SOAK_DOGFOOD", "0") not in ("1", "true", "yes"):
		return
	os.environ.setdefault("OME_SOAK_HOST", "127.0.0.1")
	os.environ.setdefault("OME_SOAK_PORT", "13333")


dogfood_defaults()


@dataclass
class PlaylistSnap:
	url: str
	media_sequence: int
	disc_sequence: Optional[int]
	target_duration: int
	segment_count: int
	has_discontinuity_tag: bool
	pdts: List[float]  # epoch seconds from PROGRAM-DATE-TIME, aligned to listed segs
	seg_urls: List[str]
	raw: str


_RE_MSN = re.compile(r"#EXT-X-MEDIA-SEQUENCE:(\d+)")
_RE_DISC_SEQ = re.compile(r"#EXT-X-DISCONTINUITY-SEQUENCE:(\d+)")
_RE_TARGET = re.compile(r"#EXT-X-TARGETDURATION:(\d+)")
_RE_PDT = re.compile(r"#EXT-X-PROGRAM-DATE-TIME:([^\r\n]+)")
_RE_INF = re.compile(r"#EXTINF:([0-9.]+)")


def http_get(url: str, timeout: float = 5.0) -> Tuple[int, str]:
	req = urllib.request.Request(url, headers={"User-Agent": "ome-playlist-invariants/1"})
	try:
		with urllib.request.urlopen(req, timeout=timeout) as resp:
			return resp.getcode(), resp.read().decode("utf-8", "replace")
	except urllib.error.HTTPError as e:
		return e.code, ""
	except Exception:
		return 0, ""


def parse_iso8601(s: str) -> Optional[float]:
	# 2024-01-01T12:00:00.000Z or with offset
	s = s.strip()
	try:
		from datetime import datetime

		if s.endswith("Z"):
			s = s[:-1] + "+00:00"
		return datetime.fromisoformat(s).timestamp()
	except Exception:
		return None


def parse_media_playlist(url: str, body: str) -> Optional[PlaylistSnap]:
	if "#EXTM3U" not in body:
		return None
	msn_m = _RE_MSN.search(body)
	if not msn_m:
		return None
	disc_m = _RE_DISC_SEQ.search(body)
	tgt_m = _RE_TARGET.search(body)

	pdts: List[float] = []
	seg_urls: List[str] = []
	has_disc = False
	pending_pdt: Optional[float] = None

	for line in body.splitlines():
		line = line.strip()
		if line == "#EXT-X-DISCONTINUITY":
			has_disc = True
			continue
		m = _RE_PDT.match(line)
		if m:
			pending_pdt = parse_iso8601(m.group(1))
			continue
		if line.startswith("#"):
			continue
		if not line:
			continue
		# segment URI
		seg_urls.append(urljoin(url, line))
		if pending_pdt is not None:
			pdts.append(pending_pdt)
			pending_pdt = None
		else:
			pdts.append(float("nan"))

	return PlaylistSnap(
		url=url,
		media_sequence=int(msn_m.group(1)),
		disc_sequence=int(disc_m.group(1)) if disc_m else None,
		target_duration=int(tgt_m.group(1)) if tgt_m else 0,
		segment_count=len(seg_urls),
		has_discontinuity_tag=has_disc,
		pdts=pdts,
		seg_urls=seg_urls,
		raw=body,
	)


def resolve_media_playlist_url(master_url: str) -> Optional[str]:
	code, body = http_get(master_url)
	if code != 200 or not body:
		return None
	# Already a media playlist
	if "#EXT-X-MEDIA-SEQUENCE:" in body or "#EXTINF:" in body:
		return master_url
	for line in body.splitlines():
		line = line.strip()
		if not line or line.startswith("#"):
			continue
		return urljoin(master_url, line)
	return None


@dataclass
class Tracker:
	seen_disc_seq_positive: bool = False
	max_disc_seq: int = -1
	last_msn: Optional[int] = None
	last_disc_seq: Optional[int] = None
	last_pdts: List[float] = field(default_factory=list)
	window_collapse_streak: int = 0
	wraps_seen: int = 0
	samples: int = 0
	failures: List[str] = field(default_factory=list)

	def fail(self, msg: str) -> None:
		print(f"FAIL {msg}", flush=True)
		self.failures.append(msg)


def check_snap(
	tr: Tracker,
	snap: PlaylistSnap,
	*,
	window_floor: int,
	collapse_grace: int,
) -> None:
	tr.samples += 1

	# Hypothesis 1: DISC-SEQ monotonic / must not vanish after observed > 0
	if snap.disc_sequence is not None:
		if snap.disc_sequence < tr.max_disc_seq:
			tr.fail(
				f"DISC-SEQ decreased: {tr.max_disc_seq} -> {snap.disc_sequence} "
				f"(msn={snap.media_sequence})"
			)
		tr.max_disc_seq = max(tr.max_disc_seq, snap.disc_sequence)
		if snap.disc_sequence > 0:
			tr.seen_disc_seq_positive = True
		tr.last_disc_seq = snap.disc_sequence
	else:
		if tr.seen_disc_seq_positive or tr.max_disc_seq >= 0:
			tr.fail(
				f"DISC-SEQ vanished after max={tr.max_disc_seq} "
				f"(msn={snap.media_sequence}, segs={snap.segment_count}, "
				f"has_EXT-X-DISCONTINUITY={snap.has_discontinuity_tag})"
			)

	# MEDIA-SEQUENCE should not jump backward on a live edge poll
	if tr.last_msn is not None and snap.media_sequence < tr.last_msn:
		tr.fail(f"MEDIA-SEQUENCE decreased: {tr.last_msn} -> {snap.media_sequence}")
	if tr.last_msn is not None and snap.media_sequence > tr.last_msn:
		# count likely wraps when MSN advances by more than a couple while
		# a discontinuity tag is present (soft signal only)
		if snap.has_discontinuity_tag:
			tr.wraps_seen += 1
	tr.last_msn = snap.media_sequence

	# Hypothesis 2: PDT should not go backward unless discontinuity precedes that seg
	# We only have playlist-level has_discontinuity_tag, so check adjacent PDTs:
	# non-decreasing within a contiguous run that has no disc tag in the playlist.
	if not snap.has_discontinuity_tag:
		prev = None
		for i, pdt in enumerate(snap.pdts):
			if pdt != pdt:  # nan
				continue
			if prev is not None and pdt + 0.001 < prev:
				tr.fail(
					f"PDT jumped backward without DISCONTINUITY in playlist: "
					f"seg[{i}] {prev} -> {pdt} (msn={snap.media_sequence})"
				)
			prev = pdt
	tr.last_pdts = snap.pdts

	# Hypothesis 3: window collapse below floor
	if snap.segment_count < window_floor:
		tr.window_collapse_streak += 1
		if tr.window_collapse_streak >= collapse_grace:
			tr.fail(
				f"window collapsed to {snap.segment_count} < floor {window_floor} "
				f"for {tr.window_collapse_streak} polls (msn={snap.media_sequence})"
			)
	else:
		tr.window_collapse_streak = 0


def main() -> int:
	ap = argparse.ArgumentParser(description=__doc__)
	ap.add_argument(
		"--url",
		default="",
		help="HLS master or media playlist URL (default: dogfood ts:hls)",
	)
	ap.add_argument("--duration-s", type=float, default=400.0)
	ap.add_argument("--interval-s", type=float, default=1.0)
	ap.add_argument(
		"--window-floor",
		type=int,
		default=int(env("OME_SOAK_WINDOW_FLOOR", "3")),
		help="Soft minimum segment count; below this for --collapse-grace polls → FAIL",
	)
	ap.add_argument(
		"--collapse-grace",
		type=int,
		default=3,
		help="Consecutive low-window polls before FAIL (allows brief wrap shrink)",
	)
	ap.add_argument(
		"--require-wrap",
		action="store_true",
		help="Fail if no discontinuity-tagged playlist was observed",
	)
	args = ap.parse_args()

	host = env("OME_SOAK_HOST", "127.0.0.1")
	port = env("OME_SOAK_PORT", "3333")
	app = env("OME_SOAK_APP", "_filler")
	stream = env("OME_SOAK_STREAM", "_filler")
	url = args.url or f"http://{host}:{port}/{app}/{stream}/ts:playlist.m3u8"

	print(f"playlist_invariants start url={url} duration={args.duration_s}s", flush=True)

	deadline = time.monotonic() + args.duration_s
	media_url: Optional[str] = None
	tr = Tracker()

	while time.monotonic() < deadline:
		if media_url is None:
			media_url = resolve_media_playlist_url(url)
			if media_url is None:
				print("waiting for playlist…", flush=True)
				time.sleep(args.interval_s)
				continue
			print(f"media playlist: {media_url}", flush=True)

		code, body = http_get(media_url)
		if code != 200 or not body:
			print(f"WARN http {code} on media playlist", flush=True)
			time.sleep(args.interval_s)
			continue

		snap = parse_media_playlist(media_url, body)
		if snap is None:
			print("WARN failed to parse media playlist", flush=True)
			time.sleep(args.interval_s)
			continue

		before = len(tr.failures)
		check_snap(
			tr,
			snap,
			window_floor=args.window_floor,
			collapse_grace=args.collapse_grace,
		)
		if len(tr.failures) == before:
			disc = snap.disc_sequence if snap.disc_sequence is not None else "-"
			print(
				f"ok msn={snap.media_sequence} disc_seq={disc} "
				f"segs={snap.segment_count} disc_tag={int(snap.has_discontinuity_tag)}",
				flush=True,
			)

		if tr.failures:
			# Keep going a bit so logs show pattern, but exit non-zero.
			pass

		time.sleep(args.interval_s)

	if args.require_wrap and not tr.seen_disc_seq_positive and tr.wraps_seen == 0:
		# Also accept seeing EXT-X-DISCONTINUITY without DISC-SEQ (still a wrap signal)
		tr.fail("require-wrap: never observed discontinuity signaling")

	print(
		f"done samples={tr.samples} max_disc_seq={tr.max_disc_seq} "
		f"failures={len(tr.failures)}",
		flush=True,
	)
	return 1 if tr.failures else 0


if __name__ == "__main__":
	sys.exit(main())
