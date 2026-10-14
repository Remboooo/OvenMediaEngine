#!/usr/bin/env bash
# Deploy WebRTC multi-rung send-path fixes to production OME. Run as root.
#   - per-session send thread, demand packetize, per-rung pacers
#   - larger ICE UDP SNDBUF + sysctl wmem (4K+ULPFEC keyframe bursts)
#   - inline UDP sendmsg (no epoll dispatch mutex on ICE RTP)
# Rollback: sudo bash deploy_webrtcfix.sh --rollback
#
# Diagnostic flag files (toggle Ulpfec / JitterBuffer via systemd drop-in):
#   .disable_ulpfec          → OME_WEBRTC_DISABLE_ULPFEC=1
#   .disable_jitterbuffer    → OME_WEBRTC_DISABLE_JITTERBUFFER=1
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)"
DIR="${OME_INSTALL_DIR:-/usr/share/ovenmediaengine}"
SRC="${OME_BIN:-${ROOT}/build/Release/bin/OvenMediaEngine}"
PREV=OvenMediaEngine-0.21.0-segcache-playlistfix
NEW=OvenMediaEngine-0.21.0-segcache-webrtcfix
DROPIN_DIR=/etc/systemd/system/ovenmediaengine.service.d
DROPIN_FILE="${DROPIN_DIR}/99-ome-webrtc-debug.conf"
SCRIPT_DIR="$(dirname "$0")"
FLAG_ULPFEC="${SCRIPT_DIR}/.disable_ulpfec"
FLAG_JB="${SCRIPT_DIR}/.disable_jitterbuffer"

status()
{
	sleep 8
	systemctl --no-pager --lines=0 status ovenmediaengine
	echo "exe: $(readlink "/proc/$(systemctl show -p MainPID --value ovenmediaengine)/exe")"
	local env_line
	env_line=$(systemctl show -p Environment --value ovenmediaengine)
	echo "env: $(echo "$env_line" | tr ' ' '\n' | grep -E '^OME_WEBRTC_' | tr '\n' ' ')"
	journalctl -u ovenmediaengine --since '-1 min' --no-pager | grep -E ' C \[|Unknown item|load config|DISABLE_|SO_SNDBUF|JitterBuffer' | tail -8 || true
}

write_dropin()
{
	mkdir -p "${DROPIN_DIR}"
	local lines=()
	if [[ -f "${FLAG_ULPFEC}" ]]; then
		lines+=("Environment=OME_WEBRTC_DISABLE_ULPFEC=1")
	fi
	if [[ -f "${FLAG_JB}" ]]; then
		lines+=("Environment=OME_WEBRTC_DISABLE_JITTERBUFFER=1")
	fi
	if ((${#lines[@]} > 0)); then
		{
			echo "[Service]"
			printf '%s\n' "${lines[@]}"
		} >"${DROPIN_FILE}"
		echo "drop-in: ${lines[*]}"
	else
		rm -f "${DROPIN_FILE}"
		echo "drop-in: cleared (Ulpfec on, JitterBuffer=config)"
	fi
	systemctl daemon-reload
}

if [[ "${1:-}" == "--rollback" ]]; then
	ln -sfn "${PREV}" "${DIR}/OvenMediaEngine"
	rm -f "${FLAG_ULPFEC}" "${FLAG_JB}"
	write_dropin
	systemctl restart ovenmediaengine
	status
	exit 0
fi

sysctl -w \
	net.core.wmem_max=33554432 \
	net.core.wmem_default=16777216 \
	net.core.rmem_max=33554432 \
	net.core.rmem_default=16777216 \
	>/dev/null
echo "sysctl: wmem_max=$(sysctl -n net.core.wmem_max) wmem_default=$(sysctl -n net.core.wmem_default)"

install -m 0755 -o root -g root "${SRC}" "${DIR}/${NEW}"
ln -sfn "${NEW}" "${DIR}/OvenMediaEngine"
write_dropin
systemctl restart ovenmediaengine
status
