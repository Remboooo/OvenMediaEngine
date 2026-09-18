#!/usr/bin/env bash
# Start localhost dogfood OME (SegmentCache soak iteration).
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
REPO="$(CDPATH= cd -- "${ROOT}/../../.." && pwd)"
CONF="${ROOT}/conf"
RUN="${ROOT}/run"
LOG="${ROOT}/logs"
BIN="${OME_DOGFOOD_BIN:-${REPO}/build/Release/bin/OvenMediaEngine}"
PIDFILE="${RUN}/ome.pid"

mkdir -p "${RUN}" "${LOG}"

# Fresh logs per start so soak journal checks aren't polluted by prior runs.
: > "${LOG}/ovenmediaengine.log" 2>/dev/null || true
: > "${LOG}/stdout.log" 2>/dev/null || true
: > "${LOG}/stderr.log" 2>/dev/null || true

if [[ ! -x "${BIN}" ]]; then
	echo "Missing binary: ${BIN}" >&2
	echo "Build first: (cd ${REPO} && ./rebuild.sh --target OvenMediaEngine)" >&2
	exit 1
fi

if [[ -f "${PIDFILE}" ]] && kill -0 "$(cat "${PIDFILE}")" 2>/dev/null; then
	echo "Already running pid=$(cat "${PIDFILE}")" >&2
	exit 0
fi

# Keep short-clip join near item start (see schedules/_filler.sch comments).
if [[ "${OME_DOGFOOD_STAMP_SCHEDULE:-1}" == "1" ]] && [[ -x "${ROOT}/stamp_schedule.sh" ]]; then
	"${ROOT}/stamp_schedule.sh"
fi

# Foreground by default for soak; use OME_DOGFOOD_DAEMON=1 for -d
ARGS=(-c "${CONF}")
if [[ "${OME_DOGFOOD_DAEMON:-0}" == "1" ]]; then
	ARGS+=(-d)
fi

echo "Starting dogfood OME"
echo "  bin:  ${BIN}"
echo "  conf: ${CONF}"
echo "  LLHLS/HLS/WS: http://127.0.0.1:13333/_filler/_filler/..."
echo "  API:  http://127.0.0.1:18081/"

cd "${ROOT}"
if [[ "${OME_DOGFOOD_DAEMON:-0}" == "1" ]]; then
	"${BIN}" "${ARGS[@]}"
	# daemon writes its own pid sometimes; track via pgrep on conf path
	sleep 1
	pgrep -n -f "OvenMediaEngine.*-c ${CONF}" > "${PIDFILE}" || true
	echo "pid=$(cat "${PIDFILE}" 2>/dev/null || echo unknown)"
else
	# background ourselves so scripts can return
	nohup "${BIN}" "${ARGS[@]}" >"${LOG}/stdout.log" 2>"${LOG}/stderr.log" &
	echo $! > "${PIDFILE}"
	echo "pid=$!"
	sleep 2
	if ! kill -0 "$(cat "${PIDFILE}")" 2>/dev/null; then
		echo "Failed to start — see ${LOG}/stderr.log" >&2
		tail -50 "${LOG}/stderr.log" >&2 || true
		exit 1
	fi
fi

curl -sf -o /dev/null -w "llhls_preflight:%{http_code}\n" \
	"http://127.0.0.1:13333/_filler/_filler/llhls.m3u8" || echo "llhls_preflight:not_ready_yet"
