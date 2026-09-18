#!/usr/bin/env bash
# Run hypotheses 1–3 wrap soak: playlist invariants + hls.js player in parallel.
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
DURATION_S="${OME_WRAP_DURATION_S:-400}"

if [[ "${SOAK_DOGFOOD:-0}" != "1" ]]; then
	export SOAK_DOGFOOD=1
fi

echo "=== wrap soak (${DURATION_S}s) ==="
echo "Ensure dogfood is on short-clip schedule (_filler.sch) and restarted."

# Install hls.js soak deps once
if [[ ! -d "${ROOT}/hlsjs_soak/node_modules/hls.js" ]]; then
	echo "npm install in hlsjs_soak…"
	(cd "${ROOT}/hlsjs_soak" && npm install --no-fund --no-audit)
fi
if [[ ! -d "${ROOT}/hlsjs_soak/node_modules/playwright" ]]; then
	echo "npm install playwright…"
	(cd "${ROOT}/hlsjs_soak" && npm install --no-fund --no-audit)
fi
# Ensure Chromium for Playwright
(cd "${ROOT}/hlsjs_soak" && npx playwright install chromium >/dev/null)

INV_LOG="${ROOT}/logs/playlist_invariants_$(date -u +%Y%m%dT%H%M%SZ).log"
HLS_LOG="${ROOT}/logs/hlsjs_soak_$(date -u +%Y%m%dT%H%M%SZ).log"
mkdir -p "${ROOT}/logs"

python3 "${ROOT}/playlist_invariants.py" \
	--duration-s "${DURATION_S}" \
	--require-wrap \
	2>&1 | tee "${INV_LOG}" &
INV_PID=$!

node "${ROOT}/hlsjs_soak/play.mjs" \
	--duration-s "${DURATION_S}" \
	--stall-s "${OME_SOAK_STALL_S:-20}" \
	2>&1 | tee "${HLS_LOG}" &
HLS_PID=$!

EC=0
if ! wait "${INV_PID}"; then
	EC=1
	echo "playlist_invariants FAILED" >&2
fi
if ! wait "${HLS_PID}"; then
	EC=1
	echo "hlsjs_soak FAILED" >&2
fi

echo "=== logs ==="
echo "  ${INV_LOG}"
echo "  ${HLS_LOG}"
echo "=== wrap soak exit=${EC} ==="
exit "${EC}"
