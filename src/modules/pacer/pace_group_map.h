//==============================================================================
//
//  OvenMediaEngine
//
//==============================================================================
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

// Assign each paced track to a pace group (option B):
//   - every video track owns group_id == video_track_id
//   - an audio track joins a video's group only when it is paired with exactly
//     one video across all renditions; otherwise it owns group_id == audio_track_id
//
// Rendition pairs are (video_track_id, audio_track_id); either side may be nullopt
// for audio-only / video-only renditions.
std::map<uint32_t, uint32_t> BuildPaceGroupMap(
	const std::vector<uint32_t> &video_track_ids,
	const std::vector<uint32_t> &audio_track_ids,
	const std::vector<std::pair<std::optional<uint32_t>, std::optional<uint32_t>>> &renditions);
