#!/usr/bin/env bash
# Concurrent multi-rung WebRTC reception check via OvenRtcTester (no browser decode).
# Fails if any client drops below min fps or exceeds packet-loss budget.
#
# Usage:
#   bash webrtc_multi_recv.sh
#   OME_SOAK_ICE_RELAY=1 LIFE=40 bash webrtc_multi_recv.sh
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
RTC="${ROOT}/oven_rtc_tester/OvenRtcTester"
# Repo layout: misc/oven_rtc_tester next to misc/segcache_soak
if [[ ! -x "$RTC" ]]; then
	RTC="${ROOT}/../oven_rtc_tester/OvenRtcTester"
fi

HOST=${OME_SOAK_HOST:-127.0.0.1}
PORT=${OME_SOAK_PORT:-3333}
APP=${OME_SOAK_APP:-app}
STREAM=${OME_SOAK_STREAM:-stream}
LIFE=${LIFE:-30}
SINT=${SINT:-5000}
MIN_FPS_60=${MIN_FPS_60:-55}
MIN_FPS_30=${MIN_FPS_30:-27}
MAX_LOSS=${MAX_LOSS:-50}
RELAY=${OME_SOAK_ICE_RELAY:-0}

if [[ ! -x "$RTC" ]]; then
	echo "missing OvenRtcTester at $RTC" >&2
	exit 2
fi

q=""
if [[ "$RELAY" == "1" || "$RELAY" == "true" || "$RELAY" == "yes" ]]; then
	q="?transport=relay"
fi

# Playlist URLs (OvenRtcTester has no change_rendition). Prefer dedicated
# FileName playlists; webrtc_default ≈ first auto rendition (usually Bypass).
declare -a NAMES=(bypass 1080 720)
declare -a URLS=(
	"ws://${HOST}:${PORT}/${APP}/${STREAM}/webrtc_default${q}"
	"ws://${HOST}:${PORT}/${APP}/${STREAM}/1080p${q}"
	"ws://${HOST}:${PORT}/${APP}/${STREAM}/720p${q}"
)
declare -a MIN_FPS=("$MIN_FPS_60" "$MIN_FPS_60" "$MIN_FPS_60")

LOGDIR=$(mktemp -d /tmp/webrtc_multi_recv.XXXXXX)
echo "webrtc_multi_recv life=${LIFE}s relay=${RELAY} logdir=${LOGDIR}"
echo "clients: ${NAMES[*]}"

pids=()
for i in "${!NAMES[@]}"; do
	name=${NAMES[$i]}
	url=${URLS[$i]}
	echo "  start [$name] $url"
	timeout $((LIFE + 10)) "$RTC" -url "$url" -n 1 -life "$LIFE" -sint "$SINT" \
		>"${LOGDIR}/${name}.log" 2>&1 &
	pids+=($!)
done

ec=0
for pid in "${pids[@]}"; do
	wait "$pid" || ec=1
done

fail=0
for i in "${!NAMES[@]}"; do
	name=${NAMES[$i]}
	log=${LOGDIR}/${name}.log
	min=${MIN_FPS[$i]}
	line=$(grep 'total_video_frames' "$log" | tail -1 || true)
	if [[ -z "$line" ]]; then
		echo "FAIL [$name] no summary (not connected?)"
		tail -20 "$log" || true
		fail=1
		continue
	fi
	fps=$(echo "$line" | grep -oP 'avg_fps\(\K[0-9.]+' || echo 0)
	loss=$(echo "$line" | grep -oP 'packet_loss\(\K[0-9]+' || echo 999)
	frames=$(echo "$line" | grep -oP 'total_video_frames\(\K[0-9]+' || echo 0)
	# packet_loss is on the previous metrics line sometimes
	loss_line=$(grep 'packet_loss(' "$log" | tail -1 || true)
	if [[ -n "$loss_line" ]]; then
		loss=$(echo "$loss_line" | grep -oP 'packet_loss\(\K[0-9]+' || echo "$loss")
	fi
	ok=1
	awk -v fps="$fps" -v min="$min" 'BEGIN{exit !(fps+0 >= min+0)}' || ok=0
	if [[ "$loss" -gt "$MAX_LOSS" ]]; then ok=0; fi
	if [[ "$ok" -eq 1 ]]; then
		echo "OK [$name] frames=$frames avg_fps=$fps packet_loss=$loss (min_fps=$min)"
	else
		echo "FAIL [$name] frames=$frames avg_fps=$fps packet_loss=$loss (min_fps=$min max_loss=$MAX_LOSS)"
		fail=1
	fi
done

if [[ "$fail" -eq 0 ]]; then
	echo "PASS all reception clients"
	exit 0
fi
echo "FAIL see $LOGDIR"
exit 1
