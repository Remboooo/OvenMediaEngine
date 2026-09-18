#!/usr/bin/env bash
set -euo pipefail
ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
PIDFILE="${ROOT}/run/ome.pid"
CONF="${ROOT}/conf"

stop_pid() {
	local pid="$1"
	if kill -0 "${pid}" 2>/dev/null; then
		kill "${pid}" 2>/dev/null || true
		for _ in $(seq 1 30); do
			kill -0 "${pid}" 2>/dev/null || return 0
			sleep 0.2
		done
		kill -9 "${pid}" 2>/dev/null || true
	fi
}

if [[ -f "${PIDFILE}" ]]; then
	stop_pid "$(cat "${PIDFILE}")"
	rm -f "${PIDFILE}"
fi

# Also sweep any dogfood instance started with this conf
while read -r pid; do
	[[ -n "${pid}" ]] || continue
	stop_pid "${pid}"
done < <(pgrep -f "OvenMediaEngine.*-c ${CONF}" || true)

echo "dogfood OME stopped"
