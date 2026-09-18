#!/usr/bin/env bash
set -euo pipefail
ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
REPO="$(CDPATH= cd -- "${ROOT}/../../.." && pwd)"

"${ROOT}/stop.sh"
# Rebuild binary if requested
if [[ "${1:-}" == "--rebuild" ]]; then
	(cd "${REPO}" && ./rebuild.sh --target OvenMediaEngine)
fi
OME_DOGFOOD_BIN="${OME_DOGFOOD_BIN:-${REPO}/build/Release/bin/OvenMediaEngine}" \
	"${ROOT}/start.sh"
