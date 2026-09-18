#!/usr/bin/env bash
# Generate short H.264+AAC clips for wrap soaks (from trains4).
set -euo pipefail
ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
OUT="${ROOT}/dogfood/media"
SRC="${OME_SHORT_SRC:-/opt/ovenmediaengine/media/trains4.mp4}"
mkdir -p "${OUT}"

if [[ ! -f "${SRC}" ]]; then
	echo "Missing source: ${SRC}" >&2
	exit 1
fi

gen() {
	local name="$1" start="$2" dur="$3"
	local out="${OUT}/short_${name}.mp4"
	echo "Generating ${out} (ss=${start} t=${dur})…"
	ffmpeg -y -hide_banner -loglevel error -ss "${start}" -t "${dur}" -i "${SRC}" \
		-c:v libx264 -pix_fmt yuv420p -profile:v high -level 4.0 -preset veryfast -crf 23 \
		-g 48 -keyint_min 48 -sc_threshold 0 \
		-c:a aac -b:a 128k -ac 2 -ar 48000 \
		-movflags +faststart "${out}"
	# Publish into MediaRootDir used by dogfood Server.xml
	ln -sfr "${out}" "/opt/ovenmediaengine/media/short_${name}.mp4"
	ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 "${out}"
}

gen a 0 60
gen b 120 60
gen c 300 55
echo "Done. Symlinks in /opt/ovenmediaengine/media/short_*.mp4"
