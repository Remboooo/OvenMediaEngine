#!/usr/bin/env bash
# Makes the traffic sim's three publisher sources (and the .flv next to each, for limits.py's paced publisher) in
# misc/segcache_soak/dogfood/media, from ffmpeg's test pattern with noise and a tone. Real footage is better: the
# encoder's keyframe sizes and bitrate swings are what the WebRTC send path has to absorb. For the 4K slot, put a
# recording there and point OME_SIM_4K_SRC at it.
#
#   sim_2160p60_baseline.ts  4K60 H.264 Constrained Baseline, 1 s GOP, ~19 Mbps (a hardware encoder / OBS NVENC)
#   sim_1080p60_high.ts      1080p60 High with B-frames, 2 s GOP, ~6.5 Mbps (OBS x264 defaults)
#   sim_720p30_main.ts       720p30 Main, 8 s GOP, ~2.8 Mbps (a long-GOP encoder)
#
#   ./make_sources.sh                 all three (the 4K one takes a while on the CPU)
#   SIM_4K_S=900 ./make_sources.sh    longer 4K source (default 300 s; traffic_sim.py starts publishers up to
#                                     OME_SIM_4K_SPAN_S into it, default 840: set it to the length minus 60)
set -euo pipefail

MEDIA="${OME_SIM_MEDIA:-$(dirname "$0")/../segcache_soak/dogfood/media}"
mkdir -p "${MEDIA}"
SIM_4K_S="${SIM_4K_S:-300}"

make()
{
	local name="$1" size="$2" rate="$3" secs="$4"
	shift 4
	if [[ -f "${MEDIA}/${name}.ts" ]]; then
		echo "${name}.ts exists, kept"
	else
		echo "making ${name}.ts (${size}@${rate}, ${secs} s)"
		ffmpeg -hide_banner -nostdin -loglevel error -y \
			-f lavfi -i "testsrc2=size=${size}:rate=${rate},noise=alls=10:allf=t" \
			-f lavfi -i "sine=frequency=440:sample_rate=48000,aformat=channel_layouts=stereo" \
			-t "${secs}" -c:v libx264 -preset veryfast "$@" -pix_fmt yuv420p \
			-c:a aac -b:a 128k -f mpegts "${MEDIA}/${name}.ts"
	fi
	[[ -f "${MEDIA}/${name}.flv" ]] ||
		ffmpeg -hide_banner -nostdin -loglevel error -y -i "${MEDIA}/${name}.ts" -c copy -bsf:a aac_adtstoasc \
			-f flv "${MEDIA}/${name}.flv"
}

make sim_2160p60_baseline 3840x2160 60 "${SIM_4K_S}" -profile:v baseline -g 60 -keyint_min 60 -sc_threshold 0 \
	-b:v 19M -maxrate 22M -bufsize 19M
make sim_1080p60_high 1920x1080 60 180 -profile:v high -bf 2 -g 120 -keyint_min 120 -sc_threshold 0 \
	-b:v 6500k -maxrate 8M -bufsize 6500k
make sim_720p30_main 1280x720 30 180 -profile:v main -bf 0 -g 240 -keyint_min 240 -sc_threshold 0 \
	-b:v 2800k -maxrate 3500k -bufsize 2800k

if (( SIM_4K_S < 900 )); then
	echo "4K source is ${SIM_4K_S} s: run traffic_sim.py with OME_SIM_4K_SPAN_S=$((SIM_4K_S - 60))"
fi
