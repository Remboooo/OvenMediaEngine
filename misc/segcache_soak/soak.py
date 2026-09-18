#!/usr/bin/env python3
"""
Segment-cache full-stack soak: watch → idle → watch cycles on live OME.

Transports:
  - llhls  : ffmpeg decode of llhls.m3u8 (requires A+V frames progressing)
  - hls    : ffmpeg decode of ts:playlist.m3u8
  - webrtc : optional OvenRtcTester if built
  - concurrent_hls_webrtc : HLS decode overlapping a WebRTC client

Env:
  OME_SOAK_HOST=127.0.0.1
  OME_SOAK_PORT=3333
  OME_SOAK_APP=_filler
  OME_SOAK_STREAM=_filler
  OME_SOAK_WATCH_S=60
  OME_SOAK_IDLE_S=60
  OME_SOAK_ITERS=3
  OME_SOAK_TRANSPORTS=llhls,hls,webrtc
  OME_SOAK_GRACE_S=35          # wait after watch for IdleGracePeriodMs (~30s)
  OME_SOAK_MIN_VIDEO_FPS=8     # average decoded video fps floor over watch window
  OME_SOAK_REQUIRE_IDLE_LOG=1  # fail idle phase if no Idle cache / Pump cleared log
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import List, Optional, Tuple

ROOT = Path(__file__).resolve().parent
LOG_DIR = ROOT / "logs"


def env(name: str, default: str) -> str:
	return os.environ.get(name, default)


def env_int(name: str, default: int) -> int:
	return int(env(name, str(default)))


def env_float(name: str, default: float) -> float:
	return float(env(name, str(default)))


def _dogfood_defaults() -> None:
	"""When SOAK_DOGFOOD=1, default host/port to the localhost dogfood instance."""
	if env("SOAK_DOGFOOD", "0") not in ("1", "true", "yes"):
		return
	os.environ.setdefault("OME_SOAK_HOST", "127.0.0.1")
	os.environ.setdefault("OME_SOAK_PORT", "13333")


_dogfood_defaults()


@dataclass
class Config:
	host: str = field(default_factory=lambda: env("OME_SOAK_HOST", "127.0.0.1"))
	port: int = field(default_factory=lambda: env_int("OME_SOAK_PORT", 3333))
	app: str = field(default_factory=lambda: env("OME_SOAK_APP", "_filler"))
	stream: str = field(default_factory=lambda: env("OME_SOAK_STREAM", "_filler"))
	watch_s: int = field(default_factory=lambda: env_int("OME_SOAK_WATCH_S", 60))
	idle_s: int = field(default_factory=lambda: env_int("OME_SOAK_IDLE_S", 60))
	iters: int = field(default_factory=lambda: env_int("OME_SOAK_ITERS", 3))
	grace_s: int = field(default_factory=lambda: env_int("OME_SOAK_GRACE_S", 35))
	min_video_fps: float = field(default_factory=lambda: env_float("OME_SOAK_MIN_VIDEO_FPS", 8.0))
	require_idle_log: bool = field(
		default_factory=lambda: env("OME_SOAK_REQUIRE_IDLE_LOG", "1") not in ("0", "false", "off")
	)
	transports: List[str] = field(
		default_factory=lambda: [
			t.strip()
			for t in env("OME_SOAK_TRANSPORTS", "llhls,hls,webrtc").split(",")
			if t.strip()
		]
	)

	@property
	def base(self) -> str:
		return f"http://{self.host}:{self.port}/{self.app}/{self.stream}"

	@property
	def llhls_url(self) -> str:
		return f"{self.base}/llhls.m3u8"

	@property
	def hls_url(self) -> str:
		# Bypass/relay scheduled streams use the HLS publisher default playlist
		# name ("playlist"), not OutputProfile FileName (that applies to
		# transcoded outputs only).
		return f"{self.base}/ts:playlist.m3u8"

	@property
	def webrtc_url(self) -> str:
		return f"ws://{self.host}:{self.port}/{self.app}/{self.stream}"


@dataclass
class PhaseResult:
	ok: bool
	detail: str
	metrics: dict = field(default_factory=dict)


def http_code(url: str, timeout: float = 5.0) -> int:
	try:
		req = urllib.request.Request(url, method="GET")
		with urllib.request.urlopen(req, timeout=timeout) as resp:
			return resp.getcode()
	except urllib.error.HTTPError as e:
		return e.code
	except Exception:
		return 0


def journal_since(seconds: int) -> str:
	# Dogfood writes to its own logfile (not systemd).
	if env("SOAK_DOGFOOD", "0") in ("1", "true", "yes"):
		log_path = Path(__file__).resolve().parent / "dogfood" / "logs" / "ovenmediaengine.log"
		if log_path.exists():
			try:
				# Keep enough tail that idle-enter lines survive hydrate spam.
				data = log_path.read_bytes()
				tail = data[-min(len(data), 2_000_000):].decode("utf-8", "replace")
				return tail
			except OSError as e:
				return f"<<dogfood log unavailable: {e}>>"
	try:
		out = subprocess.check_output(
			[
				"journalctl",
				"-u",
				"ovenmediaengine",
				"--since",
				f"{seconds} seconds ago",
				"--no-pager",
				"-o",
				"cat",
			],
			stderr=subprocess.DEVNULL,
			text=True,
			timeout=15,
		)
		return out
	except Exception as e:
		return f"<<journal unavailable: {e}>>"


def idle_log_ok(journal: str, stream: str) -> bool:
	patterns = [
		r"Idle cache playback for",
		r"Pump demand cleared — entering idle",
		r"Ready to play via idle segment cache",
	]
	if any(re.search(p, journal) for p in patterns):
		return True
	# Dogfood: also scan the full log for durable idle markers (hydrate can
	# push enter-idle lines out of the tail window).
	if env("SOAK_DOGFOOD", "0") in ("1", "true", "yes"):
		log_path = Path(__file__).resolve().parent / "dogfood" / "logs" / "ovenmediaengine.log"
		try:
			text = log_path.read_text(encoding="utf-8", errors="replace")
			return any(re.search(p, text) for p in patterns)
		except OSError:
			return False
	return False


def fail_log_hits(journal: str) -> List[str]:
	hits = []
	for line in journal.splitlines():
		# Startup race before publishers leave CREATED is expected once; ignore.
		if "created but not started" in line:
			continue
		if re.search(
			r"Empty video sample window|Failed to write moof|"
			r"OnSegmentDeleted - Failed to find|SegmentCache failed",
			line,
			re.I,
		):
			hits.append(line.strip())
	return hits[:20]


def run_ffmpeg_watch(url: str, duration_s: int, log_path: Path) -> PhaseResult:
	"""Decode A+V for duration_s; require progressing video frames."""
	progress_path = log_path.with_suffix(".progress")
	# Truncate so a prior run's high frame= does not freeze stall detection
	# (we only advance last_frame when frame > last_frame).
	try:
		progress_path.write_text("", encoding="utf-8")
	except OSError:
		pass
	cmd = [
		"ffmpeg",
		"-hide_banner",
		"-nostdin",
		"-loglevel",
		"warning",
		"-progress",
		str(progress_path),
		"-y",
		# Pace decode to wall clock. Without this, ffmpeg drains the LL-HLS
		# window in a burst then trips the stall detector at the live edge.
		"-re",
		"-rw_timeout",
		"20000000",
		"-timeout",
		"20000000",
		"-i",
		url,
		"-t",
		str(duration_s),
		"-map",
		"0:v:0",
		"-map",
		"0:a:0?",
		"-f",
		"null",
		"-",
	]
	stderr_f = open(log_path, "w", encoding="utf-8")
	t0 = time.monotonic()
	try:
		proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=stderr_f)
		# Poll progress: media time / frame count must keep rising.
		last_frame = -1
		last_media_s = -1.0
		last_advance = time.monotonic()
		stall_limit_s = env_float("OME_SOAK_STALL_S", 15.0)
		while True:
			rc = proc.poll()
			now = time.monotonic()
			frame, out_time_us = _read_progress(progress_path)
			media_s = _media_s_from_progress(out_time_us)
			advanced = False
			if frame is not None and frame < last_frame:
				# Progress file was truncated/restarted.
				last_frame = -1
			if frame is not None and frame > last_frame:
				last_frame = frame
				advanced = True
			if media_s >= 0 and media_s > last_media_s + 0.05:
				last_media_s = media_s
				advanced = True
			if advanced:
				last_advance = now
			elapsed = now - t0
			if elapsed > 5 and last_frame < 0 and last_media_s < 0:
				proc.kill()
				stderr_f.close()
				return PhaseResult(False, "no video frames decoded within 5s", {"frames": 0})
			if (last_frame >= 0 or last_media_s >= 0) and (now - last_advance) > stall_limit_s:
				proc.kill()
				stderr_f.close()
				return PhaseResult(
					False,
					f"video stalled at frame={last_frame} media={last_media_s:.1f}s "
					f"for >{stall_limit_s:.0f}s (elapsed={elapsed:.1f}s)",
					{"frames": last_frame, "elapsed_s": elapsed, "media_s": last_media_s},
				)
			if rc is not None:
				break
			time.sleep(0.25)
	finally:
		stderr_f.close()

	elapsed = time.monotonic() - t0
	frame, out_time_us = _read_progress(progress_path)
	err = log_path.read_text(encoding="utf-8", errors="replace")
	bad = re.findall(
		r"(?i)(404 Not Found|Server returned 4\d\d|Server returned 5\d\d|"
		r"Invalid data found|Error opening input|Connection refused|"
		r"Immediate exit requested|Conversion failed)",
		err,
	)
	if proc.returncode not in (0, 255) and "Immediate exit" not in err:
		# 255 sometimes on kill; treat non-zero carefully
		if proc.returncode != 0:
			return PhaseResult(
				False,
				f"ffmpeg exit={proc.returncode} bad={bad[:3]}",
				{"frames": frame or 0, "elapsed_s": elapsed, "stderr_tail": err[-800:]},
			)
	if bad:
		return PhaseResult(False, f"ffmpeg errors: {bad[:5]}", {"frames": frame or 0})

	frames = frame or 0
	fps = frames / max(elapsed, 0.1)
	min_fps = env_float("OME_SOAK_MIN_VIDEO_FPS", 8.0)
	# Require wall time / media time close to requested duration — early exit
	# usually means the live edge froze (playlist stopped advancing).
	media_s = max(0.0, _media_s_from_progress(out_time_us))
	if media_s < duration_s * 0.85 and elapsed < duration_s * 0.85:
		return PhaseResult(
			False,
			f"watch ended early: media={media_s:.1f}s wall={elapsed:.1f}s "
			f"(wanted {duration_s}s) — likely frozen playlist",
			{"frames": frames, "fps": fps, "elapsed_s": elapsed, "media_s": media_s},
		)
	if frames < max(10, int(min_fps * duration_s * 0.4)):
		return PhaseResult(
			False,
			f"too few video frames: {frames} over {elapsed:.1f}s (fps={fps:.2f})",
			{"frames": frames, "fps": fps, "elapsed_s": elapsed, "media_s": media_s},
		)
	if fps < min_fps * 0.5 and elapsed >= duration_s * 0.8:
		return PhaseResult(
			False,
			f"video fps too low: {fps:.2f} (min~{min_fps})",
			{"frames": frames, "fps": fps, "elapsed_s": elapsed, "media_s": media_s},
		)
	return PhaseResult(
		True,
		f"decoded {frames} video frames in {elapsed:.1f}s "
		f"(media={media_s:.1f}s, {fps:.1f} fps)",
		{"frames": frames, "fps": fps, "elapsed_s": elapsed, "media_s": media_s},
	)


def _read_progress(path: Path) -> Tuple[Optional[int], Optional[int]]:
	"""Return (frame, out_time_us). FFmpeg -progress mislabels microseconds as out_time_ms."""
	if not path.exists():
		return None, None
	try:
		text = path.read_text(encoding="utf-8", errors="replace")
	except OSError:
		return None, None
	frame = None
	out_us = None
	for line in text.splitlines():
		if line.startswith("frame="):
			try:
				frame = int(line.split("=", 1)[1])
			except ValueError:
				pass
		elif line.startswith("out_time_us="):
			try:
				out_us = int(line.split("=", 1)[1])
			except ValueError:
				pass
		elif line.startswith("out_time_ms=") and out_us is None:
			# FFmpeg writes microseconds into out_time_ms.
			try:
				out_us = int(line.split("=", 1)[1])
			except ValueError:
				pass
	return frame, out_us


def _media_s_from_progress(out_time_us: Optional[int]) -> float:
	if out_time_us is None:
		return -1.0
	return out_time_us / 1_000_000.0

def poll_playlist_advancing(master_url: str, duration_s: int, interval_s: float = 2.0) -> PhaseResult:
	"""During idle, playlist media-sequence / part count should still move."""
	t0 = time.monotonic()
	last_sig = None
	advances = 0
	fetches = 0
	stalls = 0
	while time.monotonic() - t0 < duration_s:
		try:
			body = urllib.request.urlopen(master_url, timeout=5).read().decode("utf-8", "replace")
		except Exception as e:
			return PhaseResult(False, f"playlist fetch failed: {e}")
		# Prefer video LL-HLS chunklist, else classic HLS media playlist.
		m = re.search(r"(/[^\s\"]*chunklist_[^\s\"]*video[^\s\"]*\.m3u8[^\s\"]*)", body)
		if not m:
			m = re.search(r"([^\s\"]*medialist_[^\s\"]+_hls\.m3u8[^\s\"]*)", body)
		target = master_url
		if m:
			rel = m.group(1)
			if rel.startswith("http"):
				target = rel
			elif rel.startswith("/"):
				from urllib.parse import urlparse

				p = urlparse(master_url)
				target = f"{p.scheme}://{p.netloc}{rel}"
			else:
				target = master_url.rsplit("/", 1)[0] + "/" + rel
		try:
			chunk = urllib.request.urlopen(target, timeout=5).read().decode("utf-8", "replace")
		except Exception as e:
			return PhaseResult(False, f"chunklist fetch failed: {e} target={target}")
		msn = None
		mm = re.search(r"#EXT-X-MEDIA-SEQUENCE:(\d+)", chunk)
		if mm:
			msn = int(mm.group(1))
		parts = len(re.findall(r"#EXT-X-PART:", chunk))
		segs = len(re.findall(r"#EXTINF:", chunk))
		sig = (msn, parts, segs, hash(chunk[-200:]))
		fetches += 1
		if last_sig is None:
			last_sig = sig
		elif sig != last_sig:
			advances += 1
			last_sig = sig
			stalls = 0
		else:
			stalls += 1
		time.sleep(interval_s)
	if advances < 1 and duration_s >= 10:
		return PhaseResult(
			False,
			f"playlist did not advance over {duration_s}s (fetches={fetches})",
			{"advances": advances, "fetches": fetches},
		)
	return PhaseResult(
		True,
		f"playlist advanced {advances}/{fetches} polls",
		{"advances": advances, "fetches": fetches},
	)


def run_webrtc(cfg: Config, duration_s: int, log_path: Path) -> PhaseResult:
	tester_dir = Path(__file__).resolve().parents[1] / "oven_rtc_tester"
	tester_bin = tester_dir / "OvenRtcTester"
	go = shutil.which("go")

	if tester_bin.exists():
		cmd = [
			str(tester_bin),
			"-url",
			cfg.webrtc_url,
			"-n",
			"1",
			"-life",
			str(duration_s),
			"-sint",
			"5000",
		]
		cwd = str(tester_dir)
	elif go is not None and (tester_dir / "OvenRtcTester.go").exists():
		cmd = [
			go,
			"run",
			"OvenRtcTester.go",
			"-url",
			cfg.webrtc_url,
			"-n",
			"1",
			"-life",
			str(duration_s),
			"-sint",
			"5000",
		]
		cwd = str(tester_dir)
	else:
		return PhaseResult(
			False,
			"webrtc skipped: build OvenRtcTester via "
			"`docker run --rm -v $PWD:/src -w /src golang:1.22-bookworm "
			"bash -c 'go mod tidy && go build -o OvenRtcTester .'`",
		)

	try:
		proc = subprocess.run(
			cmd,
			cwd=cwd,
			capture_output=True,
			text=True,
			timeout=duration_s + 45,
		)
	except subprocess.TimeoutExpired:
		return PhaseResult(False, "OvenRtcTester timed out")
	log_path.write_text(proc.stdout + "\n" + proc.stderr, encoding="utf-8")
	out = proc.stdout + proc.stderr
	video_frames = 0
	frame_matches = [int(m.group(1)) for m in re.finditer(r"total_video_frames\((\d+)\)", out)]
	if frame_matches:
		# Prefer the final report line (last), not the max of mid-run bursts.
		video_frames = frame_matches[-1]
	has_video = bool(re.search(r"track has started.*video", out, re.I)) or video_frames > 0
	has_audio = bool(re.search(r"track has started.*audio", out, re.I))
	connected = bool(re.search(r"connection state has changed connected", out, re.I)) or bool(
		re.search(r"connection_state\(connected\)", out, re.I)
	)
	if not connected:
		return PhaseResult(False, "WebRTC did not reach connected", {"log_tail": out[-600:]})
	min_frames = max(30, duration_s * 15)  # ~0.25x realtime at 60fps
	if video_frames < min_frames:
		return PhaseResult(
			False,
			f"WebRTC low video frames={video_frames} (want >= {min_frames})",
			{"frames": video_frames, "log_tail": out[-800:]},
		)
	# Bypass AAC often has no WebRTC audio (Opus expected) — warn only.
	detail = f"WebRTC connected video_frames={video_frames}"
	if not has_audio:
		detail += " (no audio track — ok for AAC bypass)"
	fps_vals = [float(x) for x in re.findall(r"Avg FPS[:= ]+(\d+(?:\.\d+)?)", out, re.I)]
	if fps_vals and max(fps_vals) < 1.0 and video_frames < 30:
		return PhaseResult(False, f"WebRTC Avg FPS too low: {fps_vals}", {"fps": fps_vals})
	return PhaseResult(True, detail, {"fps": fps_vals, "frames": video_frames})


def run_transport(cfg: Config, transport: str) -> int:
	print(f"\n======== TRANSPORT {transport} ========", flush=True)
	failures = 0
	for i in range(1, cfg.iters + 1):
		tag = f"{transport}_iter{i}"
		print(f"\n--- {tag}: WATCH {cfg.watch_s}s ---", flush=True)
		t_watch0 = time.time()

		if transport == "llhls":
			code = http_code(cfg.llhls_url)
			if code != 200:
				print(f"FAIL preflight LLHLS HTTP {code}", flush=True)
				failures += 1
				continue
			watch = run_ffmpeg_watch(cfg.llhls_url, cfg.watch_s, LOG_DIR / f"{tag}_watch.log")
		elif transport == "hls":
			code = http_code(cfg.hls_url)
			if code != 200:
				print(f"FAIL preflight HLS HTTP {code} (url={cfg.hls_url})", flush=True)
				failures += 1
				continue
			watch = run_ffmpeg_watch(cfg.hls_url, cfg.watch_s, LOG_DIR / f"{tag}_watch.log")
		elif transport == "webrtc":
			watch = run_webrtc(cfg, cfg.watch_s, LOG_DIR / f"{tag}_watch.log")
		else:
			print(f"unknown transport {transport}", flush=True)
			failures += 1
			continue

		print(("OK" if watch.ok else "FAIL") + f" watch: {watch.detail} {watch.metrics}", flush=True)
		if not watch.ok:
			failures += 1
			_dump_journal(tag + "_watch")
			continue

		# After watch: wait for grace so pump can idle (HLS/LLHLS do not hold demand).
		print(f"--- {tag}: settle grace {cfg.grace_s}s then IDLE {cfg.idle_s}s ---", flush=True)
		time.sleep(cfg.grace_s)
		j = journal_since(cfg.grace_s + 15)
		if cfg.require_idle_log and transport in ("llhls", "hls"):
			if not idle_log_ok(j, cfg.stream):
				print("FAIL idle: no Idle cache / Pump cleared log after grace", flush=True)
				failures += 1
				(LOG_DIR / f"{tag}_idle_journal.txt").write_text(j[-8000:], encoding="utf-8")
			else:
				print("OK idle log present", flush=True)

		if transport in ("llhls", "hls"):
			url = cfg.llhls_url if transport == "llhls" else cfg.hls_url
			idle = poll_playlist_advancing(url, cfg.idle_s)
		else:
			# WebRTC idle = no client; just wait and confirm no crash logs
			time.sleep(cfg.idle_s)
			idle = PhaseResult(True, "idle wait (no webrtc client)")

		print(("OK" if idle.ok else "FAIL") + f" idle: {idle.detail} {idle.metrics}", flush=True)
		if not idle.ok:
			failures += 1
			_dump_journal(tag + "_idle")

		hits = fail_log_hits(journal_since(int(time.time() - t_watch0) + 5))
		if hits:
			print(f"WARN journal hits ({len(hits)}):", flush=True)
			for h in hits[:8]:
				print(f"  {h}", flush=True)

	return failures


def run_concurrent_hls_webrtc(cfg: Config) -> int:
	"""HLS decode while a WebRTC client is connected (cache-serve + demux)."""
	print("\n======== TRANSPORT concurrent_hls_webrtc ========", flush=True)
	failures = 0
	for i in range(1, cfg.iters + 1):
		tag = f"concurrent_iter{i}"
		print(f"\n--- {tag}: HLS watch {cfg.watch_s}s + WebRTC overlap ---", flush=True)
		code = http_code(cfg.hls_url)
		if code != 200:
			print(f"FAIL preflight HLS HTTP {code}", flush=True)
			failures += 1
			continue

		hls_log = LOG_DIR / f"{tag}_hls.log"
		rtc_log = LOG_DIR / f"{tag}_webrtc.log"
		# Start HLS first so playhead is warm, then overlap WebRTC mid-watch.
		hls_proc = None
		progress_path = hls_log.with_suffix(".progress")
		try:
			progress_path.write_text("", encoding="utf-8")
		except OSError:
			pass

		cmd = [
			"ffmpeg",
			"-hide_banner",
			"-nostdin",
			"-loglevel",
			"warning",
			"-progress",
			str(progress_path),
			"-y",
			"-re",
			"-rw_timeout",
			"20000000",
			"-timeout",
			"20000000",
			"-i",
			cfg.hls_url,
			"-t",
			str(cfg.watch_s),
			"-map",
			"0:v:0",
			"-f",
			"null",
			"-",
		]
		stderr_f = open(hls_log, "w", encoding="utf-8")
		t0 = time.monotonic()
		last_media_s = -1.0
		last_advance = t0
		msn_samples: List[int] = []
		stall_limit_s = env_float("OME_SOAK_STALL_S", 15.0)
		try:
			hls_proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=stderr_f)
			# Overlap WebRTC after a few seconds of clean HLS.
			overlap_at = min(8.0, cfg.watch_s * 0.2)
			rtc_started = False
			rtc_thread_result: dict = {}

			def _rtc_worker() -> None:
				res = run_webrtc(cfg, max(15, cfg.watch_s - int(overlap_at)), rtc_log)
				rtc_thread_result["res"] = res

			rtc_thread = None
			while True:
				rc = hls_proc.poll()
				now = time.monotonic()
				elapsed = now - t0
				if (not rtc_started) and elapsed >= overlap_at:
					rtc_started = True
					print(f"  starting WebRTC overlap at t={elapsed:.1f}s", flush=True)
					import threading

					rtc_thread = threading.Thread(target=_rtc_worker, daemon=True)
					rtc_thread.start()

				frame, out_time_us = _read_progress(progress_path)
				media_s = _media_s_from_progress(out_time_us)
				if media_s >= 0 and media_s > last_media_s + 0.05:
					last_media_s = media_s
					last_advance = now

				# Sample HLS MEDIA-SEQUENCE occasionally to catch jumps.
				if int(elapsed) % 3 == 0:
					try:
						body = urllib.request.urlopen(cfg.hls_url, timeout=3).read().decode(
							"utf-8", "replace"
						)
						m = re.search(r"([^\s\"]*medialist_[^\s\"]+_hls\.m3u8)", body)
						if m:
							rel = m.group(1)
							media_url = (
								rel
								if rel.startswith("http")
								else cfg.hls_url.rsplit("/", 1)[0] + "/" + rel.lstrip("/")
							)
							chunk = urllib.request.urlopen(media_url, timeout=3).read().decode(
								"utf-8", "replace"
							)
							mm = re.search(r"#EXT-X-MEDIA-SEQUENCE:(\d+)", chunk)
							if mm:
								msn_samples.append(int(mm.group(1)))
					except Exception:
						pass

				if last_media_s >= 0 and (now - last_advance) > stall_limit_s:
					hls_proc.kill()
					print(
						f"FAIL concurrent: HLS stalled media={last_media_s:.1f}s "
						f"for >{stall_limit_s:.0f}s",
						flush=True,
					)
					failures += 1
					break
				if rc is not None:
					break
				time.sleep(0.25)

			if rtc_thread is not None:
				rtc_thread.join(timeout=cfg.watch_s + 60)
			stderr_f.close()

			err = hls_log.read_text(encoding="utf-8", errors="replace")
			bad = re.findall(
				r"(?i)(404 Not Found|skipping \d+ segments ahead|"
				r"timestamp discontinuity|Packet corrupt)",
				err,
			)
			# Allow a couple of transient corruptions; fail on segment skips / 404.
			hard = [b for b in bad if not re.search(r"(?i)Packet corrupt", b)]
			rtc_res: PhaseResult = rtc_thread_result.get(
				"res", PhaseResult(False, "WebRTC did not run")
			)

			msn_jump = 0
			if len(msn_samples) >= 2:
				for a, b in zip(msn_samples, msn_samples[1:]):
					msn_jump = max(msn_jump, b - a)

			ok = True
			if hls_proc.returncode not in (0, 255) and "Immediate exit" not in err:
				print(f"FAIL concurrent: ffmpeg exit={hls_proc.returncode}", flush=True)
				ok = False
			if last_media_s < cfg.watch_s * 0.7:
				print(
					f"FAIL concurrent: HLS media only {last_media_s:.1f}s "
					f"(wanted ~{cfg.watch_s}s)",
					flush=True,
				)
				ok = False
			if hard:
				print(f"FAIL concurrent: HLS errors {hard[:5]}", flush=True)
				ok = False
			if msn_jump > 5:
				print(
					f"FAIL concurrent: MEDIA-SEQUENCE jumped by {msn_jump} "
					f"between polls (samples={msn_samples[:12]})",
					flush=True,
				)
				ok = False
			if not rtc_res.ok:
				print(f"FAIL concurrent: {rtc_res.detail}", flush=True)
				ok = False

			if ok:
				print(
					f"OK concurrent: HLS media={last_media_s:.1f}s "
					f"msn_jump_max={msn_jump} webrtc={rtc_res.detail}",
					flush=True,
				)
			else:
				failures += 1
				_dump_journal(tag)
		finally:
			if hls_proc is not None and hls_proc.poll() is None:
				hls_proc.kill()
			stderr_f.close()

	return failures


def _dump_journal(tag: str) -> None:
	j = journal_since(180)
	(LOG_DIR / f"{tag}_journal.txt").write_text(j, encoding="utf-8")


def main() -> int:
	cfg = Config()
	LOG_DIR.mkdir(parents=True, exist_ok=True)
	stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
	summary_path = LOG_DIR / f"summary_{stamp}.json"
	print(
		f"soak start {stamp} host={cfg.host}:{cfg.port} "
		f"app={cfg.app}/{cfg.stream} watch={cfg.watch_s}s idle={cfg.idle_s}s "
		f"iters={cfg.iters} transports={cfg.transports}",
		flush=True,
	)

	total_fail = 0
	results = {}
	for t in cfg.transports:
		if t == "concurrent_hls_webrtc":
			nfail = run_concurrent_hls_webrtc(cfg)
		else:
			nfail = run_transport(cfg, t)
		results[t] = {"failures": nfail}
		total_fail += nfail

	summary = {
		"stamp": stamp,
		"config": cfg.__dict__,
		"results": results,
		"total_failures": total_fail,
	}
	# transports list etc not all json-friendly from dataclass with defaults already materialised
	summary["config"] = {
		"host": cfg.host,
		"port": cfg.port,
		"app": cfg.app,
		"stream": cfg.stream,
		"watch_s": cfg.watch_s,
		"idle_s": cfg.idle_s,
		"iters": cfg.iters,
		"transports": cfg.transports,
	}
	summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
	print(f"\n======== DONE failures={total_fail} summary={summary_path} ========", flush=True)
	return 1 if total_fail else 0


if __name__ == "__main__":
	sys.exit(main())
