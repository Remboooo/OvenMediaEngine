#!/usr/bin/env bash
# End-to-end WebRTC smoke test: push a real 4K60 source as a test stream, pull
# it back over WebRTC with the bitstream probe (one viewer per rendition, in
# parallel), and check what a Chrome receiver actually assembled plus the
# server's health. Prints PASS/FAIL per check; exit code 0 only if all pass.
#
#   ./smoke_test.sh harness      # before deploying: harness OME (build/Release), clean + loss profiles
#   ./smoke_test.sh production   # after deploying: live OME, test stream <SMOKE_APP>/e2etest, clean path only
# Procedure, baseline numbers and failure triage: README.md
#
# Env overrides: SMOKE_DURATION_S (60), SMOKE_RENDITIONS ("Bypass+opus,1080p"),
#   SMOKE_PROFILES (harness: "clean,loss_1pct,burst_loss,jitter_reorder,stalls,real_world_mix"),
#   SMOKE_SRC (default: misc/segcache_soak/dogfood/media/sim_2160p60_baseline.ts), SMOKE_APP (production: app),
#   OME_TEST_LAN_IP (default: the source address of the default route), SMOKE_SRC_OFFSET_S (240),
#   SMOKE_MIN_COMPLETE (default: from SMOKE_BASELINE, else clean 0.99 / impaired 0.97),
#   SMOKE_BASELINE (profiles/baseline.json: each profile's measured range, see smoke_baseline.py),
#   SMOKE_VIEWER_CDP / SMOKE_VIEWER_BIDI (a decoding viewer on a real desktop Chrome / Firefox, e.g.
#   http://<desktop-ip>:19222 / ws://<desktop-ip>:19224; judged by viewer_check.py),
#   OME_IMPAIR_BIN (harness: the OME binary, default build/Release), OME_IMPAIR_WEBRTC_FEATURES=none (harness: without
#   this fork's opt-in WebRTC features, for upstream OME), OME_TOKEN (production API token).
set -uo pipefail

MODE="${1:-}"
ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
REPO="$(CDPATH= cd -- "${ROOT}/../.." && pwd)"
PROBE_DIR="${REPO}/misc/segcache_soak/hlsjs_soak"
DURATION="${SMOKE_DURATION_S:-60}"
# Every profile replays the same stretch of the source: loss recovery depends on
# content (240-packet 4K keyframes recover far worse than 130-packet ones), so
# results are only comparable across profiles and runs on identical content.
SRC_OFFSET="${SMOKE_SRC_OFFSET_S:-240}"
SRC="${SMOKE_SRC:-${REPO}/misc/segcache_soak/dogfood/media/sim_2160p60_baseline.ts}"
OUT="${SMOKE_OUT:-${ROOT}/logs/smoke_$(date +%Y%m%d_%H%M%S)}"
BASELINE="${SMOKE_BASELINE:-${ROOT}/profiles/baseline.json}"
FAILS=0
VIEWERS=()
[[ -n "${SMOKE_VIEWER_CDP:-}" ]] && VIEWERS+=("chrome=${SMOKE_VIEWER_CDP}")
[[ -n "${SMOKE_VIEWER_BIDI:-}" ]] && VIEWERS+=("firefox=${SMOKE_VIEWER_BIDI}")
VIEWER_HOST="${SMOKE_VIEWER_HOST:-${OME_TEST_LAN_IP:-$(ip -4 route get 192.0.2.1 2>/dev/null | grep -oP "src \K\S+")}}"	# the desktop browsers reach OME over the LAN

case "${MODE}" in
harness)
	HOST="${VIEWER_HOST}" PORT=23333 RTMP=rtmp://127.0.0.1:21935 APP=live STREAM=e2etest
	API=http://127.0.0.1:28081/v1 TOKEN=webrtc-impair-local
	LOG="${ROOT}/logs/ovenmediaengine.log" DUMPS="${ROOT}/conf/dumps"
	RENDITIONS="${SMOKE_RENDITIONS:-Bypass+opus}"   # harness has no transcoded rungs
	PROFILES="${SMOKE_PROFILES:-clean,loss_1pct,burst_loss,jitter_reorder,stalls,real_world_mix}"
	;;
production)
	HOST=127.0.0.1 PORT=3333 RTMP=rtmp://127.0.0.1:1935 APP="${SMOKE_APP:-app}" STREAM=e2etest
	API="http://127.0.0.1:$(grep -zoP '<Managers>\s*<API>\s*<Port>\K\d+' /usr/share/ovenmediaengine/conf/Server.xml | tr -d '\0')/v1" TOKEN="${OME_TOKEN:-$(grep -oP '(?<=<AccessToken>)[^<]+' /usr/share/ovenmediaengine/conf/Server.xml | head -1)}"
	LOG=/var/log/ovenmediaengine/ovenmediaengine.log DUMPS=/usr/share/ovenmediaengine/dumps
	RENDITIONS="${SMOKE_RENDITIONS:-Bypass+opus,1080p}"
	PROFILES=clean
	;;
*)
	sed -n '2,15p' "$0"
	exit 2
	;;
esac

mkdir -p "${OUT}"
BIN_USED="${OME_IMPAIR_BIN:-${REPO}/build/Release/bin/OvenMediaEngine}"
[[ "${MODE}" == production ]] && BIN_USED=production
AUTH="Authorization: Basic $(echo -n "${TOKEN}" | base64)"
api() { curl -s -m3 -H "${AUTH}" "${API}$1"; }
check() {	# check <name> <ok:0|1> <detail>
	if [[ "$2" == 0 ]]; then echo "PASS  $1  $3"; else echo "FAIL  $1  $3"; FAILS=$((FAILS + 1)); fi
}
ome_pid() { pgrep -f "OvenMediaEngine.*-c ${ROOT}/conf" | head -1; }
[[ "${MODE}" == production ]] && ome_pid() { systemctl show -p MainPID --value ovenmediaengine; }

[[ -f "${SRC}" ]] || { echo "no source .ts (set SMOKE_SRC)" >&2; exit 2; }
echo "smoke test mode=${MODE} src=$(basename "${SRC}") renditions=${RENDITIONS} profiles=${PROFILES} out=${OUT}"

if [[ "${MODE}" == harness ]]; then
	"${ROOT}/run.sh" stop >/dev/null 2>&1
	# The relay's RTP trace gives each frame's send time, for the viewers' send-to-display delay
	[[ ${#VIEWERS[@]} -gt 0 ]] && export IMPAIR_TRACE="${OUT}/relay_trace.txt"
	# run.sh also starts its own looping source as live/test; this test pushes live/e2etest.
	OME_IMPAIR_NICE="${OME_IMPAIR_NICE:-}" "${ROOT}/run.sh" start clean >/dev/null || exit 2
	sleep 3
fi

if api "/vhosts/default/apps/${APP}/streams" | grep -q "\"${STREAM}\""; then
	echo "stream ${APP}/${STREAM} already exists — refusing to interfere" >&2
	exit 2
fi

# --- server baseline --------------------------------------------------------
PID="$(ome_pid)"
RSS0=$(awk '/VmRSS/{print int($2/1024)}' "/proc/${PID}/status" 2>/dev/null || echo 0)
DUMPS0=$(ls "${DUMPS}" 2>/dev/null | wc -l)
LOG_OFFSET=$(stat -c %s "${LOG}" 2>/dev/null || echo 0)

# --- push the test stream ---------------------------------------------------
PUSH=
start_push() {
	nice -n 10 ffmpeg -hide_banner -nostdin -loglevel error -re -ss "${SRC_OFFSET}" -stream_loop -1 -i "${SRC}" \
		-c copy -bsf:a aac_adtstoasc -f flv "${RTMP}/${APP}/${STREAM}" >>"${OUT}/push.log" 2>&1 &
	PUSH=$!
	for _ in $(seq 1 30); do
		api "/vhosts/default/apps/${APP}/streams" | grep -q "\"${STREAM}\"" && break
		sleep 1
	done
	api "/vhosts/default/apps/${APP}/streams" | grep -q "\"${STREAM}\"" || return 1
	# A created stream is not playable until every output (incl. GPU transcoder
	# rungs) has started; signalling answers 404 until then. The LLHLS master
	# playlist only lists variants once the stream has started.
	for i in $(seq 1 90); do
		variants=$(curl -s -m3 "http://${HOST}:${PORT}/${APP}/${STREAM}/auto.m3u8" | grep -c STREAM-INF)
		[[ "${variants}" -gt 0 ]] && { STARTED_AFTER=$i; return 0; }
		sleep 1
	done
	return 1
}
stop_push() {
	[[ -n "${PUSH}" ]] && kill "${PUSH}" 2>/dev/null && wait "${PUSH}" 2>/dev/null
	PUSH=
	for _ in $(seq 1 20); do
		api "/vhosts/default/apps/${APP}/streams" | grep -q "\"${STREAM}\"" || return 0
		sleep 1
	done
}
trap 'stop_push; [[ "${MODE}" == harness ]] && "${ROOT}/run.sh" stop >/dev/null 2>&1' EXIT

# --- WebRTC probes ------------------------------------------------------------
for profile in ${PROFILES//,/ }; do
	[[ "${MODE}" == harness ]] && "${ROOT}/run.sh" profile "${profile}" >/dev/null
	start_push
	check "stream started (${profile})" $? "${APP}/${STREAM} from ${SRC_OFFSET}s, playable after ${STARTED_AFTER:-?}s"
	[[ -n "${STARTED_AFTER:-}" ]] || exit 1
	LOG_OFFSET=$(stat -c %s "${LOG}" 2>/dev/null || echo 0)	# judge the server from stream start on
	# The bar: this profile's lowest baseline run minus a point (runs on the same code vary by several
	# points under loss), else the fixed defaults
	read -r min bar_from < <(python3 -I - "${BASELINE}" "${profile}" <<'PY'
import json, sys
try:
    b = json.load(open(sys.argv[1])).get(sys.argv[2]) or {}
except (OSError, ValueError):
    b = {}
c = b.get('complete') or []
print(round(min(c) - 0.01, 4) if c else (0.99 if sys.argv[2] == 'clean' else 0.97), f"baseline({len(c)})" if c else 'default')
PY
)
	min="${SMOKE_MIN_COMPLETE:-${min}}"
	vpids=()
	t_start_ms=$(date +%s%3N)
	for v in "${VIEWERS[@]}"; do
		name="${v%%=*}" url="${v#*=}"
		vd="${OUT}/${profile}_viewer_${name}"
		if [[ ${name} == chrome ]]; then venv=(OME_SOAK_CDP="${url}"); else venv=(OME_SOAK_BIDI="${url}"); fi
		env "${venv[@]}" node "${PROBE_DIR}/webrtc_oracle.mjs" --ws "ws://${VIEWER_HOST}:${PORT}/${APP}/${STREAM}/auto" \
			--out "${vd}" --duration-s "${DURATION}" --no-transform >"${vd}.log" 2>&1 &
		vpids+=($!)
	done
	pids=()
	RESULTS=()
	IFS=, read -ra rends <<<"${RENDITIONS}"
	for r in "${rends[@]}"; do
		d="${OUT}/${profile}_${r//[^A-Za-z0-9]/_}"
		# Chrome's own log, for the probe's packet-buffer clears (an artifact of a probe that never decodes: README)
		DEBUG=pw:browser OME_SOAK_CHROME_ARGS="--enable-logging=stderr --v=0" \
		OME_SOAK_HOST="${HOST}" OME_SOAK_PORT="${PORT}" OME_SOAK_APP="${APP}" OME_SOAK_STREAM="${STREAM}" OME_SOAK_PLAYLIST=auto \
			nice -n 10 node "${PROBE_DIR}/webrtc_bitstream_probe.mjs" --duration-s "${DURATION}" --rendition "${r}" \
			--out "${d}" >"${d}.log" 2>&1 &
		pids+=($!)
	done
	wait "${pids[@]}" "${vpids[@]}"
	for r in "${rends[@]}"; do
		d="${OUT}/${profile}_${r//[^A-Za-z0-9]/_}"
		label="${profile}/${r}"
		if [[ ! -s "${d}/frames.jsonl" ]]; then
			check "webrtc ${label}" 1 "no frames captured (see ${d}.log)"
			continue
		fi
		res=$(python3 -I - "${d}" "${min}" <<'PY'
import json, statistics, sys
d, need = sys.argv[1], float(sys.argv[2])
fr = [json.loads(l) for l in open(f'{d}/frames.jsonl')]
fr = fr[len(fr) // 8:]  # skip connect / first-keyframe ramp
base = fr[0]['rtp']
u = sorted({(f['rtp'] - base) & 0xFFFFFFFF for f in fr})
deltas = [b - a for a, b in zip(u, u[1:])]
med = statistics.median(deltas)
# 59.94/60 fps alternates 1530/1440 ticks: average the normal deltas (gaps excluded).
step = statistics.mean(x for x in deltas if x < 1.5 * med)
# A jump of more than 5 s is a timestamp discontinuity (source loop, publisher restart), not missing frames
expected = 1 + sum(round(x / step) if x < 5 * 90000 else 1 for x in deltas)
ratio = len(u) / expected
st = (json.loads(open(f'{d}/stats.jsonl').read().splitlines()[-1]).get('stats') or {})
print(f"{0 if ratio >= need else 1} {ratio:.4f} complete={ratio:.1%} (need {need:.1%}) frames={len(u)} lost={st.get('packetsLost')} nack={st.get('nackCount')} rtx={st.get('retransmittedPacketsReceived')}")
PY
)
		read -r ok ratio detail <<<"${res}"
		clears=$(grep -c 'PacketBuffer is already at max size' "${d}.log")
		check "webrtc ${label} frames" "${ok}" "${detail} buffer_clears=${clears} bar=${bar_from}"
		RESULTS+=("\"probe_${r}\": {\"complete\": ${ratio}, \"buffer_clears\": ${clears}, \"stats\": $(python3 -I -c 'import json,sys; print(json.dumps((json.loads(open(sys.argv[1]).read().splitlines()[-1]).get("stats") or {})))' "${d}/stats.jsonl" 2>/dev/null || echo null)}")
		dec=$("${PROBE_DIR}/h264_decode_check.sh" "${d}" | head -1)
		errs=$(sed -nE 's/.*preceded by decoder errors: ([0-9]+).*/\1/p' <<<"${dec}")
		[[ "${errs:-1}" == 0 ]]
		check "webrtc ${label} decode" $? "${dec}"
		RESULTS+=("\"decode_errors_${r}\": ${errs:-null}")
		rm -f "${d}/trimmed.h264"	# large; probe.h264 is kept for re-checks
	done
	# Decoding viewers: what a real desktop browser showed, judged from 3 s after the impairment starts
	# (profiles start 5 s in) until 1 s before the end
	for v in "${VIEWERS[@]}"; do
		name="${v%%=*}"
		vd="${OUT}/${profile}_viewer_${name}"
		vres=$(python3 -I "${ROOT}/viewer_check.py" "${vd}" ${IMPAIR_TRACE:+--trace "${IMPAIR_TRACE}"} --from-ms $((t_start_ms + 8000)) --to-ms $((t_start_ms + DURATION * 1000 - 1000)))
		RESULTS+=("\"viewer_${name}\": ${vres}")
		read -r vok vline < <(python3 -I - "${vres}" "${profile}" "${name}" "${BASELINE}" <<'PY'
import json, sys
r, profile, name = json.loads(sys.argv[1]), sys.argv[2], sys.argv[3]
try:
    b = json.load(open(sys.argv[4])).get(profile) or {}
except (OSError, ValueError):
    b = {}
if not r.get('ok'):
    print(1, f"viewer failed: {r.get('error')}"); sys.exit()
fz = (b.get('freezes') or {}).get(name) or []
dr = (b.get('dropped') or {}).get(name) or []
# Bars: the baseline's worst run plus one freeze / one point of frames dropped (a desktop drops some frames on a
# clean path too: a desktop Chrome ~1.5% at 4K60). Without a baseline: clean needs 0 freezes and <= 2% dropped,
# impaired profiles are reported only.
fz_max = max(fz) + 1 if fz else (0 if profile == 'clean' else None)
dr_max = max(dr) + 0.01 if dr else (0.02 if profile == 'clean' else None)
ok = (fz_max is None or r['freezes'] <= fz_max) and (dr_max is None or (r['dropped_ratio'] or 0) <= dr_max)
bar = 'reported only' if fz_max is None and dr_max is None else \
      f"need <= {fz_max} freezes, <= {dr_max:.1%} dropped" + (' (baseline)' if fz else '')
e2e = r.get('e2e_ms') or {}
print(0 if ok else 1, f"freezes={r['freezes']} ({r['freeze_s']} s, longest {r['longest_freeze_s']} s) dropped={(r['dropped_ratio'] or 0):.1%} "
      f"shown={(r['shown_ratio'] or 0):.1%} e2e p50/p95={e2e.get('p50')}/{e2e.get('p95')} ms jb={r['jitter_buffer_ms']} ms plis={r['pliCount']} ({bar})")
PY
)
		check "viewer ${profile}/${name}" "${vok}" "${vline}"
	done
	( IFS=,; echo "{\"profile\": \"${profile}\", \"bin\": \"${BIN_USED}\", \"t\": ${t_start_ms}, ${RESULTS[*]}}" ) >>"${OUT}/results.jsonl"
	RESULTS=()
	tail -c +$((LOG_OFFSET + 1)) "${LOG}" >>"${OUT}/ome_during_test.log" 2>/dev/null
	stop_push
	STARTED_AFTER=
done

# --- server health -------------------------------------------------------------
[[ "$(ome_pid)" == "${PID}" ]]
check "server alive" $? "pid ${PID}"
DUMPS1=$(ls "${DUMPS}" 2>/dev/null | wc -l)
[[ "${DUMPS1}" == "${DUMPS0}" ]]
check "no crash dump" $? "dumps ${DUMPS0} -> ${DUMPS1}"
bad=$(grep -E ' [EC] \[|ULPFEC worker behind|RtcSendGap|took [0-9]{3,}ms' "${OUT}/ome_during_test.log" | grep -v 'HttpServer\|Cannot find stream' | head -5)
[[ -z "${bad}" ]]
check "server log" $? "${bad:-no errors, FEC backlog, send gaps or slow packetize}"
RSS1=$(awk '/VmRSS/{print int($2/1024)}' "/proc/${PID}/status" 2>/dev/null || echo 0)
echo "INFO  rss ${RSS0} -> ${RSS1} MB   RtcFec threads (now): $(cat /proc/"${PID}"/task/*/comm 2>/dev/null | grep -c '^RtcFec')"

echo
if [[ "${FAILS}" == 0 ]]; then echo "SMOKE TEST PASS (${OUT})"; else echo "SMOKE TEST FAIL: ${FAILS} check(s) (${OUT})"; fi
exit $((FAILS > 0))
