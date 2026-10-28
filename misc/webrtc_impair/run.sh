#!/usr/bin/env bash
# WebRTC impairment harness. Never touches the production OME.
#
#   ./run.sh start [viewer_profile] [ingest_profile]   e.g. ./run.sh start real_world_mix ingest_jitter
#   ./run.sh profile <name>        switch viewer impairment live (no reconnect needed)
#   ./run.sh ingest <name>         switch ingest impairment live
#   ./run.sh status | stop
#
# Viewer URL (LAN):  ws://<lan-ip>:23333/live/test/auto   (OvenPlayer source type "webrtc")
# LAN address: OME_TEST_LAN_IP (default: the source address of the default route).
# Source: OME_IMPAIR_SRC (default: dogfood/media/sim_2160p60_baseline.ts, see misc/traffic_sim/make_sources.sh), looped
# with -re into RTMP.
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
REPO="$(CDPATH= cd -- "${ROOT}/../.." && pwd)"
BIN="${OME_IMPAIR_BIN:-${REPO}/build/Release/bin/OvenMediaEngine}"
LAN_IP="${OME_TEST_LAN_IP:-${OME_IMPAIR_LAN_IP:-$(ip -4 route get 192.0.2.1 2>/dev/null | grep -oP "src \K\S+")}}"
export OME_TEST_LAN_IP="${LAN_IP}"  # conf/Server.xml's ICE candidates
RUN="${ROOT}/run"
LOG="${ROOT}/logs"
PROFILES="${ROOT}/profiles"
mkdir -p "${RUN}" "${LOG}"

start_bg() {
	local name="$1"
	shift
	nohup "$@" >"${LOG}/${name}.log" 2>&1 &
	echo $! >"${RUN}/${name}.pid"
	echo "  ${name}: pid $!"
}

stop_bg() {
	local pidfile="${RUN}/$1.pid"
	if [[ -f "${pidfile}" ]]; then
		local pid
		pid="$(cat "${pidfile}")"
		kill "${pid}" 2>/dev/null || true
		# Wait until it is gone: a new OME started while the old one still holds its ports fails to bind
		for _ in $(seq 1 150); do kill -0 "${pid}" 2>/dev/null || break; sleep 0.1; done
		kill -9 "${pid}" 2>/dev/null || true
		rm -f "${pidfile}"
	fi
}

set_profile() {
	local target="$1" name="$2"
	[[ -f "${PROFILES}/${name}.json" ]] || { echo "no such profile: ${name}" >&2; ls "${PROFILES}" >&2; exit 1; }
	cp "${PROFILES}/${name}.json" "${PROFILES}/${target}"
	echo "${target} <- ${name}"
}

case "${1:-}" in
start)
	set_profile active.json "${2:-clean}"
	set_profile active_ingest.json "${3:-ingest_clean}"
	SRC="${OME_IMPAIR_SRC:-${REPO}/misc/segcache_soak/dogfood/media/sim_2160p60_baseline.ts}"
	[[ -f "${SRC}" ]] || { echo "no source .ts (set OME_IMPAIR_SRC)" >&2; exit 1; }
	[[ -x "${BIN}" ]] || { echo "missing ${BIN}" >&2; exit 1; }

	echo "starting impairment harness (src=${SRC})"
	# conf/ turns this fork's opt-in WebRTC features on; OME_IMPAIR_WEBRTC_FEATURES=none runs a copy without them
	# (upstream OME, and builds from before the switches, reject the elements)
	CONF="${ROOT}/conf"
	if [[ ${OME_IMPAIR_WEBRTC_FEATURES:-all} == none ]]; then
		CONF="${ROOT}/conf_nofeatures"
		mkdir -p "${CONF}"
		cp "${ROOT}/conf/Logger.xml" "${CONF}/"
		sed -E '/<(SendPacing|VideoShedding|JitterBufferDelay)>/d; s#<Adaptive>[a-z]+</Adaptive>##' "${ROOT}/conf/Server.xml" >"${CONF}/Server.xml"
	fi
	# Logger.xml's ../logs is resolved against the working directory.
	(cd "${CONF}" && start_bg ome ${OME_IMPAIR_NICE:+nice -n "${OME_IMPAIR_NICE}"} "${BIN}" -c "${CONF}")
	for _ in $(seq 1 60); do	# until the API answers (RTMP and signalling are up by then)
		curl -s -m1 -o /dev/null http://127.0.0.1:28081/v1/vhosts && break
		sleep 0.5
	done
	start_bg relay_ice env ${IMPAIR_TRACE:+IMPAIR_TRACE="${IMPAIR_TRACE}"} ${OME_IMPAIR_NICE:+nice -n "${OME_IMPAIR_NICE}"} python3 -u "${ROOT}/impair_relay.py" udp --listen "${LAN_IP}:24000-24005" \
		--target 127.0.0.1:24000-24005 --profile "${PROFILES}/active.json"
	start_bg relay_sig python3 -u "${ROOT}/impair_relay.py" tcp --listen "${LAN_IP}:23333" \
		--target 127.0.0.1:23333 --profile "${PROFILES}/ingest_clean.json"
	start_bg relay_ingest python3 -u "${ROOT}/impair_relay.py" tcp --listen 127.0.0.1:21936 \
		--target 127.0.0.1:21935 --profile "${PROFILES}/active_ingest.json"
	sleep 1
	start_bg source nice -n 10 ffmpeg -hide_banner -nostdin -loglevel warning -re -stream_loop -1 -i "${SRC}" \
		-c copy -f flv rtmp://127.0.0.1:21936/live/test
	echo "viewer: ws://${LAN_IP}:23333/live/test/auto"
	;;
profile)
	set_profile active.json "${2:?profile name}"
	;;
ingest)
	set_profile active_ingest.json "${2:?profile name}"
	;;
status)
	for p in ome relay_ice relay_sig relay_ingest source; do
		if [[ -f "${RUN}/${p}.pid" ]] && kill -0 "$(cat "${RUN}/${p}.pid")" 2>/dev/null; then
			echo "${p}: running (pid $(cat "${RUN}/${p}.pid"))"
		else
			echo "${p}: stopped"
		fi
	done
	echo "viewer profile: $(head -c 200 "${PROFILES}/active.json" 2>/dev/null)"
	;;
stop)
	for p in source relay_ingest relay_sig relay_ice ome; do stop_bg "${p}"; done
	echo "stopped"
	;;
*)
	sed -n '2,10p' "$0"
	exit 1
	;;
esac
