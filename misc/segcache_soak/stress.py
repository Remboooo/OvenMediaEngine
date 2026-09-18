#!/usr/bin/env python3
"""
Randomized multi-client stress for segment-cache dogfood OME.

Phases:
  1. cold_start  — restart OME, connect clients immediately while index/hydrate runs
  2. churn       — random LLHLS/HLS/WebRTC connect/disconnect for a duration
  3. idle_io     — after grace, assert nearly-zero disk reads and low CPU

Env (SOAK_DOGFOOD=1 recommended):
  OME_STRESS_DURATION_S=180     # churn phase length
  OME_STRESS_MAX_CLIENTS=6
  OME_STRESS_SEED=1
  OME_STRESS_SKIP_RESTART=0     # 1 = use already-running dogfood (no cold_start)
  OME_STRESS_IDLE_GRACE_S=25    # wait after last client before I/O sample
  OME_STRESS_IDLE_SAMPLE_S=20
  OME_STRESS_MAX_IDLE_READ_BPS=65536   # allow tiny noise
  OME_STRESS_MAX_IDLE_CPU_PCT=15
  OME_STRESS_MAX_SERVE_CPU_PCT=80      # while clients active, post-hydrate
"""

from __future__ import annotations

import json
import os
import random
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Optional, Tuple

ROOT = Path(__file__).resolve().parent
DOGFOOD = ROOT / "dogfood"
LOG_DIR = ROOT / "logs"
RTC_BIN = ROOT.parent / "oven_rtc_tester" / "OvenRtcTester"


def env(name: str, default: str) -> str:
	return os.environ.get(name, default)


def env_int(name: str, default: int) -> int:
	return int(env(name, str(default)))


def env_float(name: str, default: float) -> float:
	return float(env(name, str(default)))


def env_bool(name: str, default: bool = False) -> bool:
	v = env(name, "1" if default else "0").lower()
	return v in ("1", "true", "yes", "on")


if env_bool("SOAK_DOGFOOD", True):
	os.environ.setdefault("OME_SOAK_HOST", "127.0.0.1")
	os.environ.setdefault("OME_SOAK_PORT", "13333")


@dataclass
class StressConfig:
	host: str = field(default_factory=lambda: env("OME_SOAK_HOST", "127.0.0.1"))
	port: int = field(default_factory=lambda: env_int("OME_SOAK_PORT", 13333))
	app: str = field(default_factory=lambda: env("OME_SOAK_APP", "_filler"))
	stream: str = field(default_factory=lambda: env("OME_SOAK_STREAM", "_filler"))
	duration_s: int = field(default_factory=lambda: env_int("OME_STRESS_DURATION_S", 180))
	max_clients: int = field(default_factory=lambda: env_int("OME_STRESS_MAX_CLIENTS", 6))
	seed: int = field(default_factory=lambda: env_int("OME_STRESS_SEED", 1))
	skip_restart: bool = field(default_factory=lambda: env_bool("OME_STRESS_SKIP_RESTART", False))
	idle_grace_s: int = field(default_factory=lambda: env_int("OME_STRESS_IDLE_GRACE_S", 25))
	idle_sample_s: int = field(default_factory=lambda: env_int("OME_STRESS_IDLE_SAMPLE_S", 20))
	max_idle_read_bps: float = field(
		default_factory=lambda: env_float("OME_STRESS_MAX_IDLE_READ_BPS", 65536.0)
	)
	max_idle_cpu_pct: float = field(
		default_factory=lambda: env_float("OME_STRESS_MAX_IDLE_CPU_PCT", 15.0)
	)
	max_serve_cpu_pct: float = field(
		default_factory=lambda: env_float("OME_STRESS_MAX_SERVE_CPU_PCT", 80.0)
	)
	min_client_s: float = field(default_factory=lambda: env_float("OME_STRESS_MIN_CLIENT_S", 8.0))
	max_client_s: float = field(default_factory=lambda: env_float("OME_STRESS_MAX_CLIENT_S", 35.0))
	stall_s: float = field(default_factory=lambda: env_float("OME_SOAK_STALL_S", 12.0))

	@property
	def base(self) -> str:
		return f"http://{self.host}:{self.port}/{self.app}/{self.stream}"

	@property
	def llhls_url(self) -> str:
		return f"{self.base}/llhls.m3u8"

	@property
	def hls_url(self) -> str:
		return f"{self.base}/ts:playlist.m3u8"

	@property
	def webrtc_url(self) -> str:
		return f"ws://{self.host}:{self.port}/{self.app}/{self.stream}"


@dataclass
class ClientResult:
	transport: str
	ok: bool
	detail: str
	metrics: dict = field(default_factory=dict)


def dogfood_pid() -> Optional[int]:
	pidfile = DOGFOOD / "run" / "ome.pid"
	if pidfile.exists():
		try:
			pid = int(pidfile.read_text().strip())
			os.kill(pid, 0)
			return pid
		except (ValueError, OSError, ProcessLookupError):
			pass
	try:
		out = subprocess.check_output(
			["pgrep", "-n", "-f", "OvenMediaEngine.*dogfood/conf"],
			text=True,
		).strip()
		return int(out) if out else None
	except (subprocess.CalledProcessError, ValueError):
		return None


def read_proc_io(pid: int) -> Dict[str, int]:
	vals: Dict[str, int] = {}
	path = Path(f"/proc/{pid}/io")
	for line in path.read_text().splitlines():
		if ":" in line:
			k, v = line.split(":", 1)
			vals[k.strip()] = int(v.strip())
	return vals


def read_proc_cpu_times(pid: int) -> Tuple[float, float]:
	"""Return (process_cpu_seconds, wall_seconds) using /proc and time.monotonic."""
	stat = Path(f"/proc/{pid}/stat").read_text().split()
	# utime, stime are fields 14,15 (1-indexed) → indices 13,14
	clk = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
	utime = int(stat[13]) / clk
	stime = int(stat[14]) / clk
	return utime + stime, time.monotonic()


def sample_cpu_pct(pid: int, window_s: float = 2.0) -> float:
	c0, t0 = read_proc_cpu_times(pid)
	time.sleep(window_s)
	c1, t1 = read_proc_cpu_times(pid)
	dt = max(t1 - t0, 1e-3)
	return 100.0 * (c1 - c0) / dt


def http_get(url: str, timeout: float = 5.0) -> Tuple[int, bytes]:
	try:
		req = urllib.request.Request(url, method="GET")
		with urllib.request.urlopen(req, timeout=timeout) as resp:
			return int(resp.getcode()), resp.read()
	except urllib.error.HTTPError as e:
		try:
			body = e.read()
		except Exception:
			body = b""
		return int(e.code), body
	except Exception:
		return 0, b""


def http_code(url: str, timeout: float = 5.0) -> int:
	code, _ = http_get(url, timeout=timeout)
	return code


def playlist_playable(body: bytes) -> bool:
	"""True when body looks like a real HLS master/media playlist (not empty 201)."""
	text = body.decode("utf-8", errors="replace")
	if "#EXTM3U" not in text:
		return False
	# Master or media: need at least one media reference / segment.
	return bool(
		re.search(r"#EXTINF:|#EXT-X-STREAM-INF:|#EXT-X-MEDIA:|chunklist_|\.m4s|\.ts", text)
	)


def wait_http(url: str, timeout_s: float, accept: Tuple[int, ...] = (200,)) -> int:
	deadline = time.monotonic() + timeout_s
	last = 0
	while time.monotonic() < deadline:
		last = http_code(url, timeout=2.0)
		if last in accept:
			return last
		time.sleep(0.25)
	return last


def wait_playable_playlist(url: str, timeout_s: float) -> Tuple[int, float]:
	"""
	Wait until OME serves a real playlist (HTTP 200 + EXTM3U).
	Note: HTTP 201 means stream created but not started yet (empty body) — not ready.
	Returns (http_code, seconds_waited).
	"""
	deadline = time.monotonic() + timeout_s
	t0 = time.monotonic()
	last = 0
	while time.monotonic() < deadline:
		last, body = http_get(url, timeout=2.0)
		if last == 200 and playlist_playable(body):
			return last, time.monotonic() - t0
		time.sleep(0.2)
	return last, time.monotonic() - t0


def wait_llhls_media_ready(master_url: str, timeout_s: float) -> bool:
	"""Wait until the LLHLS video chunklist lists at least one segment or part."""
	deadline = time.monotonic() + timeout_s
	while time.monotonic() < deadline:
		code, body = http_get(master_url, timeout=2.0)
		if code != 200:
			time.sleep(0.25)
			continue
		text = body.decode("utf-8", errors="replace")
		m = re.search(r'(/[^"\s]+chunklist_0_video[^"\s]*)', text)
		if not m:
			time.sleep(0.25)
			continue
		chunk_path = m.group(1)
		if chunk_path.startswith("http"):
			chunk_url = chunk_path
		else:
			host = re.match(r"(https?://[^/]+)", master_url)
			chunk_url = (host.group(1) if host else "") + chunk_path
		cc, cb = http_get(chunk_url, timeout=3.0)
		if cc == 200 and re.search(r"#EXTINF:|#EXT-X-PART:", cb.decode("utf-8", errors="replace")):
			return True
		time.sleep(0.25)
	return False


def hydrate_underway() -> bool:
	"""True if greedy hydrate started and has not finished yet."""
	if log_has(r"Greedy hydrate complete"):
		return False
	return log_has(r"Starting greedy hydrate|SegmentCache indexing|SegmentCache ready")


def ome_log_path() -> Path:
	return DOGFOOD / "logs" / "ovenmediaengine.log"


def log_has(pattern: str) -> bool:
	path = ome_log_path()
	if not path.exists():
		return False
	try:
		return re.search(pattern, path.read_text(encoding="utf-8", errors="replace")) is not None
	except OSError:
		return False


def restart_dogfood() -> None:
	script = DOGFOOD / "restart.sh"
	print("  restarting dogfood OME…", flush=True)
	subprocess.check_call([str(script)], cwd=str(DOGFOOD))
	# pidfile may lag a moment
	for _ in range(20):
		if dogfood_pid():
			return
		time.sleep(0.25)
	raise RuntimeError("dogfood failed to start")


def _read_progress(path: Path) -> Tuple[Optional[int], Optional[int]]:
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
			try:
				out_us = int(line.split("=", 1)[1])
			except ValueError:
				pass
	return frame, out_us


def run_ffmpeg_client(
	url: str,
	duration_s: float,
	log_path: Path,
	stall_s: float,
	label: str,
	first_media_s: float = 20.0,
) -> ClientResult:
	progress = log_path.with_suffix(".progress")
	try:
		progress.write_text("", encoding="utf-8")
	except OSError:
		pass
	cmd = [
		"ffmpeg",
		"-hide_banner",
		"-nostdin",
		"-loglevel",
		"warning",
		"-progress",
		str(progress),
		"-y",
		"-re",
		"-rw_timeout",
		"20000000",
		"-timeout",
		"20000000",
		"-i",
		url,
		"-t",
		str(max(1, int(duration_s))),
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
	last_media = -1.0
	last_adv = t0
	proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=stderr_f)
	try:
		while True:
			rc = proc.poll()
			now = time.monotonic()
			frame, out_us = _read_progress(progress)
			media = (out_us / 1_000_000.0) if out_us is not None else -1.0
			if media >= 0 and media > last_media + 0.05:
				last_media = media
				last_adv = now
			elapsed = now - t0
			if elapsed > first_media_s and last_media < 0:
				proc.kill()
				return ClientResult(
					label,
					False,
					f"no media within {first_media_s:.0f}s",
					{"elapsed_s": elapsed},
				)
			if last_media >= 0 and (now - last_adv) > stall_s:
				proc.kill()
				return ClientResult(
					label,
					False,
					f"stalled media={last_media:.1f}s for >{stall_s:.0f}s",
					{"media_s": last_media, "elapsed_s": elapsed},
				)
			if rc is not None:
				break
			time.sleep(0.2)
	finally:
		stderr_f.close()
		if proc.poll() is None:
			proc.kill()

	err = log_path.read_text(encoding="utf-8", errors="replace")
	hard = re.findall(
		r"(?i)(404 Not Found|Server returned 4\d\d|Server returned 5\d\d|"
		r"Packet corrupt|skipping \d+ segments ahead|"
		r"timestamp discontinuity|Invalid data found|Error opening input|"
		r"Connection refused|Conversion failed)",
		err,
	)
	# Packet corrupt / discontinuity are hard failures for this stress.
	if hard:
		return ClientResult(label, False, f"ffmpeg errors: {hard[:6]}", {"media_s": last_media})
	if proc.returncode not in (0, 255) and "Immediate exit" not in err:
		return ClientResult(
			label, False, f"ffmpeg exit={proc.returncode}", {"media_s": last_media}
		)
	if last_media < duration_s * 0.7:
		return ClientResult(
			label,
			False,
			f"short media={last_media:.1f}s (wanted ~{duration_s:.0f}s)",
			{"media_s": last_media},
		)
	return ClientResult(
		label,
		True,
		f"ok media={last_media:.1f}s",
		{"media_s": last_media, "elapsed_s": time.monotonic() - t0},
	)


def run_webrtc_client(url: str, duration_s: float, log_path: Path) -> ClientResult:
	if not RTC_BIN.exists():
		return ClientResult("webrtc", False, f"missing {RTC_BIN}")
	life = max(5, int(duration_s))
	try:
		proc = subprocess.run(
			[str(RTC_BIN), "-url", url, "-n", "1", "-life", str(life), "-sint", "5000"],
			cwd=str(RTC_BIN.parent),
			capture_output=True,
			text=True,
			timeout=life + 45,
		)
	except subprocess.TimeoutExpired:
		return ClientResult("webrtc", False, "OvenRtcTester timed out")
	out = proc.stdout + "\n" + proc.stderr
	log_path.write_text(out, encoding="utf-8")
	frames = [int(m.group(1)) for m in re.finditer(r"total_video_frames\((\d+)\)", out)]
	video_frames = frames[-1] if frames else 0
	connected = bool(
		re.search(r"connection_state\(connected\)", out, re.I)
		or re.search(r"connection state has changed connected", out, re.I)
	)
	if not connected:
		return ClientResult("webrtc", False, "not connected", {"log_tail": out[-500:]})
	min_frames = max(20, life * 10)
	if video_frames < min_frames:
		return ClientResult(
			"webrtc",
			False,
			f"low frames={video_frames} (want >= {min_frames})",
			{"frames": video_frames},
		)
	return ClientResult(
		"webrtc", True, f"ok frames={video_frames}", {"frames": video_frames}
	)


def phase_cold_start(cfg: StressConfig, stamp: str) -> List[ClientResult]:
	print("\n======== PHASE cold_start ========", flush=True)
	results: List[ClientResult] = []
	if cfg.skip_restart:
		print("  skipped (OME_STRESS_SKIP_RESTART=1)", flush=True)
		return results

	restart_dogfood()
	pid = dogfood_pid()
	print(f"  pid={pid}", flush=True)

	# HTTP 201 = stream created but not started (empty). Wait for real 200 playlist.
	code, ready_s = wait_playable_playlist(cfg.llhls_url, timeout_s=45.0)
	print(f"  first playable LLHLS HTTP {code} after {ready_s:.2f}s", flush=True)
	if code != 200:
		results.append(
			ClientResult(
				"cold_llhls",
				False,
				f"no playable playlist within 45s (HTTP {code})",
			)
		)
		return results

	# Master can be 200 before the video chunklist has segments — wait briefly.
	t_chunk = time.monotonic()
	if not wait_llhls_media_ready(cfg.llhls_url, timeout_s=20.0):
		results.append(
			ClientResult("cold_llhls", False, "video chunklist never gained segments")
		)
		return results
	print(
		f"  video chunklist ready after +{time.monotonic() - t_chunk:.2f}s",
		flush=True,
	)

	during_hydrate = hydrate_underway()
	print(f"  indexing/hydrate still underway={during_hydrate}", flush=True)
	# Persist sidecar + playhead-window hydrate often finish before HTTP 200.
	# Success criterion: early playable connect after restart while SegmentCache ran.
	saw_cache = log_has(r"SegmentCache indexing|SegmentCache ready|Starting greedy hydrate")
	if ready_s <= 20.0 and saw_cache:
		detail = (
			f"playable at {ready_s:.2f}s while hydrate still running"
			if during_hydrate
			else f"playable at {ready_s:.2f}s (hydrate finished first; cache path used)"
		)
		results.append(ClientResult("cold_during_hydrate", True, detail))
		print(f"  OK early connect: {detail}", flush=True)
	else:
		results.append(
			ClientResult(
				"cold_during_hydrate",
				False,
				f"slow/missing cache startup (ready={ready_s:.2f}s saw_cache={saw_cache})",
			)
		)

	# Immediate short plays on all three transports overlapping hydrate.
	with ThreadPoolExecutor(max_workers=3) as pool:
		futs = {
			pool.submit(
				run_ffmpeg_client,
				cfg.llhls_url,
				12.0,
				LOG_DIR / f"{stamp}_cold_llhls.log",
				cfg.stall_s,
				"cold_llhls",
				18.0,
			): "llhls",
			pool.submit(
				run_ffmpeg_client,
				cfg.hls_url,
				12.0,
				LOG_DIR / f"{stamp}_cold_hls.log",
				cfg.stall_s,
				"cold_hls",
				18.0,
			): "hls",
			pool.submit(
				run_webrtc_client,
				cfg.webrtc_url,
				12.0,
				LOG_DIR / f"{stamp}_cold_webrtc.log",
			): "webrtc",
		}
		for fut in as_completed(futs):
			res = fut.result()
			results.append(res)
			print(
				("OK" if res.ok else "FAIL") + f" cold {res.transport}: {res.detail}",
				flush=True,
			)

	# LLHLS + HLS must both play during cold start; WebRTC should too once stream started.
	av = [r for r in results if r.transport in ("cold_llhls", "cold_hls")]
	if not all(r.ok for r in av) or not av:
		results.append(
			ClientResult("cold_start", False, "LLHLS/HLS did not both play during startup")
		)
	elif ready_s <= 20.0 and saw_cache:
		print("  OK connected during early startup / cache path", flush=True)

	return results


def phase_churn(cfg: StressConfig, stamp: str) -> Tuple[List[ClientResult], List[float]]:
	print("\n======== PHASE churn ========", flush=True)
	rng = random.Random(cfg.seed)
	transports = ["llhls", "hls", "webrtc"]
	results: List[ClientResult] = []
	cpu_samples: List[float] = []
	active = 0
	lock = threading.Lock()
	stop_at = time.monotonic() + cfg.duration_s
	client_id = 0
	hydrate_done = log_has(r"Greedy hydrate complete")

	def cpu_sampler() -> None:
		nonlocal hydrate_done
		while time.monotonic() < stop_at:
			pid = dogfood_pid()
			if pid and hydrate_done:
				try:
					cpu_samples.append(sample_cpu_pct(pid, 1.5))
				except OSError:
					pass
			elif not hydrate_done:
				hydrate_done = log_has(r"Greedy hydrate complete")
			time.sleep(2.0)

	sampler = threading.Thread(target=cpu_sampler, daemon=True)
	sampler.start()

	def one_client(cid: int, transport: str, life: float) -> ClientResult:
		nonlocal active
		with lock:
			active += 1
		tag = f"{stamp}_churn_{cid}_{transport}"
		try:
			if transport == "webrtc":
				return run_webrtc_client(cfg.webrtc_url, life, LOG_DIR / f"{tag}.log")
			url = cfg.llhls_url if transport == "llhls" else cfg.hls_url
			return run_ffmpeg_client(url, life, LOG_DIR / f"{tag}.log", cfg.stall_s, transport)
		finally:
			with lock:
				active -= 1

	futures = []
	with ThreadPoolExecutor(max_workers=cfg.max_clients) as pool:
		while time.monotonic() < stop_at:
			with lock:
				n_active = active
			if n_active >= cfg.max_clients:
				time.sleep(0.5)
				continue
			# Random pause before next spawn
			time.sleep(rng.uniform(0.2, 3.0))
			if time.monotonic() >= stop_at:
				break
			# Random subset: sometimes spawn 1, sometimes 2 overlapping
			batch = rng.randint(1, min(2, cfg.max_clients - n_active))
			for _ in range(batch):
				client_id += 1
				tr = rng.choice(transports)
				life = rng.uniform(cfg.min_client_s, cfg.max_client_s)
				life = min(life, max(5.0, stop_at - time.monotonic()))
				print(
					f"  spawn#{client_id} {tr} life={life:.1f}s (active~{n_active + 1})",
					flush=True,
				)
				futures.append(pool.submit(one_client, client_id, tr, life))
			# Occasionally wait for some to finish before spawning more
			if rng.random() < 0.3 and futures:
				done = [f for f in futures if f.done()]
				for f in done:
					res = f.result()
					results.append(res)
					print(
						("OK" if res.ok else "FAIL")
						+ f" churn {res.transport}: {res.detail}",
						flush=True,
					)
					futures.remove(f)

		for f in as_completed(futures):
			res = f.result()
			results.append(res)
			print(
				("OK" if res.ok else "FAIL") + f" churn {res.transport}: {res.detail}",
				flush=True,
			)

	sampler.join(timeout=5)
	if cpu_samples:
		print(
			f"  serve CPU samples n={len(cpu_samples)} "
			f"avg={sum(cpu_samples)/len(cpu_samples):.1f}% "
			f"max={max(cpu_samples):.1f}%",
			flush=True,
		)
	return results, cpu_samples


def phase_idle_io(cfg: StressConfig) -> List[ClientResult]:
	print("\n======== PHASE idle_io ========", flush=True)
	results: List[ClientResult] = []
	pid = dogfood_pid()
	if not pid:
		return [ClientResult("idle_io", False, "dogfood not running")]

	# Ensure hydrate finished before claiming 0 I/O.
	deadline = time.monotonic() + 120
	while time.monotonic() < deadline and not log_has(r"Greedy hydrate complete"):
		print("  waiting for greedy hydrate complete…", flush=True)
		time.sleep(3)
	if not log_has(r"Greedy hydrate complete|Idle cache playback"):
		results.append(
			ClientResult("idle_io", False, "hydrate/idle never observed in logs")
		)

	print(f"  grace {cfg.idle_grace_s}s (no clients)…", flush=True)
	time.sleep(cfg.idle_grace_s)

	# Confirm idle mode
	if not log_has(r"Idle cache playback|Pump demand cleared"):
		results.append(
			ClientResult("idle_log", False, "no Idle cache / Pump cleared log after grace")
		)
	else:
		print("  OK idle log present", flush=True)
		results.append(ClientResult("idle_log", True, "idle markers present"))

	try:
		io0 = read_proc_io(pid)
		c0, t0 = read_proc_cpu_times(pid)
	except OSError as e:
		return [ClientResult("idle_io", False, f"proc read failed: {e}")]

	time.sleep(cfg.idle_sample_s)

	try:
		io1 = read_proc_io(pid)
		c1, t1 = read_proc_cpu_times(pid)
	except OSError as e:
		return [ClientResult("idle_io", False, f"proc read failed: {e}")]

	read_delta = max(0, io1.get("read_bytes", 0) - io0.get("read_bytes", 0))
	# Prefer rchar for actual read() syscalls including page cache; also track read_bytes.
	rchar_delta = max(0, io1.get("rchar", 0) - io0.get("rchar", 0))
	dt = max(t1 - t0, 1e-3)
	read_bps = read_delta / dt
	rchar_bps = rchar_delta / dt
	cpu_pct = 100.0 * (c1 - c0) / dt

	print(
		f"  idle sample {cfg.idle_sample_s}s: read_bytes={read_delta} ({read_bps:.0f} B/s) "
		f"rchar={rchar_delta} ({rchar_bps:.0f} B/s) cpu={cpu_pct:.1f}%",
		flush=True,
	)

	# Disk I/O: use read_bytes (block layer). Allow small noise for journal/page accounting.
	if read_bps > cfg.max_idle_read_bps:
		results.append(
			ClientResult(
				"idle_io",
				False,
				f"idle read_bytes {read_bps:.0f} B/s > {cfg.max_idle_read_bps:.0f}",
				{"read_bps": read_bps, "read_delta": read_delta},
			)
		)
	else:
		results.append(
			ClientResult(
				"idle_io",
				True,
				f"idle read_bytes {read_bps:.0f} B/s",
				{"read_bps": read_bps, "rchar_bps": rchar_bps},
			)
		)

	if cpu_pct > cfg.max_idle_cpu_pct:
		results.append(
			ClientResult(
				"idle_cpu",
				False,
				f"idle cpu {cpu_pct:.1f}% > {cfg.max_idle_cpu_pct:.1f}%",
				{"cpu_pct": cpu_pct},
			)
		)
	else:
		results.append(
			ClientResult(
				"idle_cpu",
				True,
				f"idle cpu {cpu_pct:.1f}%",
				{"cpu_pct": cpu_pct},
			)
		)

	return results


def main() -> int:
	cfg = StressConfig()
	LOG_DIR.mkdir(parents=True, exist_ok=True)
	stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
	print(
		f"stress start {stamp} host={cfg.host}:{cfg.port} "
		f"duration={cfg.duration_s}s max_clients={cfg.max_clients} seed={cfg.seed}",
		flush=True,
	)

	all_results: List[ClientResult] = []
	serve_cpu: List[float] = []

	all_results.extend(phase_cold_start(cfg, stamp))
	churn_results, serve_cpu = phase_churn(cfg, stamp)
	all_results.extend(churn_results)
	all_results.extend(phase_idle_io(cfg))

	# Post-hydrate serve CPU ceiling (p95 — brief rematerialize spikes are OK)
	if serve_cpu:
		peak = max(serve_cpu)
		avg = sum(serve_cpu) / len(serve_cpu)
		ordered = sorted(serve_cpu)
		p95 = ordered[min(len(ordered) - 1, int(len(ordered) * 0.95))]
		print(f"  serve CPU samples n={len(serve_cpu)} avg={avg:.1f}% p95={p95:.1f}% max={peak:.1f}%", flush=True)
		if p95 > cfg.max_serve_cpu_pct:
			all_results.append(
				ClientResult(
					"serve_cpu",
					False,
					f"serve cpu p95 {p95:.1f}% > {cfg.max_serve_cpu_pct:.1f}%",
					{"peak": peak, "p95": p95, "avg": avg},
				)
			)
		else:
			all_results.append(
				ClientResult(
					"serve_cpu",
					True,
					f"serve cpu p95 {p95:.1f}% avg {avg:.1f}%",
					{"peak": peak, "p95": p95, "avg": avg},
				)
			)

	failures = [r for r in all_results if not r.ok]
	summary = {
		"stamp": stamp,
		"config": {
			"host": cfg.host,
			"port": cfg.port,
			"duration_s": cfg.duration_s,
			"max_clients": cfg.max_clients,
			"seed": cfg.seed,
		},
		"results": [
			{"transport": r.transport, "ok": r.ok, "detail": r.detail, "metrics": r.metrics}
			for r in all_results
		],
		"failures": len(failures),
	}
	out = LOG_DIR / f"stress_summary_{stamp}.json"
	out.write_text(json.dumps(summary, indent=2), encoding="utf-8")
	print(
		f"\n======== DONE failures={len(failures)}/{len(all_results)} summary={out} ========",
		flush=True,
	)
	for f in failures[:20]:
		print(f"  FAIL {f.transport}: {f.detail}", flush=True)
	return 1 if failures else 0


if __name__ == "__main__":
	sys.exit(main())
