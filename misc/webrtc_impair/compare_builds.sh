#!/usr/bin/env bash
# Upstream OME vs this branch, same config (production's: Rtx, Ulpfec, JitterBuffer, PlayoutDelay 50:200), judged by
# real desktop browsers on a remote desktop (misc/remote_viewer). Never touches the production OME.
#
#   ./compare_builds.sh smoke  [reps]   smoke_test.sh harness profiles, 4K Bypass, Chrome + Firefox + the probe at once
#   ./compare_builds.sh limits          limits.py quick matrix, one browser at a time (LIM_STREAM gpu4k or s1080)
#
# Env: UPSTREAM_BIN (default ../OME-upstream/build/Release/bin/OvenMediaEngine, upstream eeb875103, the commit this
#   branch is based on), PATCHED_BIN (build/Release), CDP (http://<desktop-ip>:19222), BIDI (ws://<desktop-ip>:19224), both required.
#   CMP_BUILDS (default "patched upstream"): any other name <name> runs the binary in BIN_<name>, e.g.
#   CMP_BUILDS="capped patched" BIN_capped=... to try a change against the patched build.
#   This fork's opt-in WebRTC features (<SendPacing>, <VideoShedding>, <PlayoutDelay><Adaptive>) are on for every build
#   but upstream; FEAT_<name>=none turns them off for one (needed for binaries built before the switches existed).
# Results: logs/cmp_<build>_r<N>/ (smoke) and ../traffic_sim/logs/cmp_lim[s1080]_<build>_<browser>/ (limits); report_data.py
# collects both.
set -uo pipefail

ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
REPO="$(CDPATH= cd -- "${ROOT}/../.." && pwd)"
UPSTREAM_BIN="${UPSTREAM_BIN:-$(CDPATH= cd -- "${REPO}/.." && pwd)/OME-upstream/build/Release/bin/OvenMediaEngine}"
PATCHED_BIN="${PATCHED_BIN:-${REPO}/build/Release/bin/OvenMediaEngine}"
CDP="${CDP:?set CDP=http://<desktop-ip>:19222}"
BIDI="${BIDI:?set BIDI=ws://<desktop-ip>:19224}"
PROFILES="${CMP_PROFILES:-clean,loss_1pct,burst_loss,jitter_reorder,stalls,real_world_mix}"
DIMS="${CMP_DIMS:-loss,burst,rtt,jitter,reorder,outage,rate}"

BUILDS="${CMP_BUILDS:-patched upstream}"

bin_of() {
	case $1 in
	upstream) echo "${UPSTREAM_BIN}" ;;
	patched) echo "${PATCHED_BIN}" ;;
	*) local v="BIN_$1"; echo "${!v:-}" ;;
	esac
}
for b in ${BUILDS}; do [[ -x "$(bin_of "${b}")" ]] || { echo "missing $(bin_of "${b}")" >&2; exit 2; }; done

features_of() {
	local v="FEAT_$1"
	if [[ -n ${!v:-} ]]; then echo "${!v}"; elif [[ $1 == upstream ]]; then echo none; else echo all; fi
}

case "${1:-}" in
smoke)
	# Builds interleaved per repetition, so drift (the desktop, this machine's load) hits both alike
	for rep in $(seq 1 "${2:-3}"); do
		for b in ${BUILDS}; do
			out="${ROOT}/logs/cmp_${b}_r${rep}"
			# A repetition with every profile done is kept (rerun after an interruption)
			if [[ $(wc -l <"${out}/results.jsonl" 2>/dev/null || echo 0) -ge $(tr ',' '\n' <<<"${PROFILES}" | wc -l) ]]; then
				echo "=== smoke ${b} rep ${rep}: done already"
				continue
			fi
			rm -rf "${out}"
			echo "=== smoke ${b} rep ${rep} ($(date +%H:%M))"
			OME_IMPAIR_BIN="$(bin_of "${b}")" OME_IMPAIR_WEBRTC_FEATURES="$(features_of "${b}")" SMOKE_PROFILES="${PROFILES}" SMOKE_VIEWER_CDP="${CDP}" SMOKE_VIEWER_BIDI="${BIDI}" \
				SMOKE_BASELINE=/dev/null SMOKE_OUT="${out}" "${ROOT}/smoke_test.sh" harness
		done
	done
	;;
limits)
	# LIM_STREAM: gpu4k (the 1080p60 rung, transcoded on the GPU; upstream 0.21 has no NVENC/NVDEC and falls behind
	# there) or s1080 (1080p60 passed through: the delivery path alone)
	stream="${LIM_STREAM:-gpu4k}"
	tag=$([[ ${stream} == gpu4k ]] && echo lim || echo "lim${stream}")
	playlist=$([[ ${stream} == gpu4k ]] && echo 1080p || echo auto)
	for browser in ${LIM_BROWSERS:-chrome firefox}; do
		for b in ${LIM_BUILDS:-${BUILDS}}; do
			# LIM_RUN: suffix for a repeat run's directory; LIM_ARGS: extra limits.py arguments (e.g. --keep-trace)
			out="${REPO}/misc/traffic_sim/logs/cmp_${tag}_${b}_${browser}${LIM_RUN:+_${LIM_RUN}}"
			if [[ -f "${out}/results.json" ]]; then echo "=== limits ${stream} ${b} ${browser}: done already"; continue; fi
			echo "=== limits ${stream} ${b} ${browser} ($(date +%H:%M))"
			if [[ ${browser} == chrome ]]; then remote=(--remote-viewer "${CDP}"); else remote=(--remote-firefox "${BIDI}"); fi
			python3 -u "${REPO}/misc/traffic_sim/limits.py" --stream "${stream}" --playlist "${playlist}" --rendition auto --quick \
				--dims "${DIMS}" --playout-delay 50:200 "${remote[@]}" --bin "$(bin_of "${b}")" --allow-production-live \
				$([[ $(features_of "${b}") == none ]] && echo --webrtc-features none) \
				${LIM_ARGS:-} --out "${out}"
		done
	done
	;;
*)
	sed -n '2,15p' "$0"
	exit 2
	;;
esac
