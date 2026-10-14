//==============================================================================
//
//  OvenMediaEngine
//
//==============================================================================
#include "pace_group_map.h"

#include <set>
#include <unordered_map>

std::map<uint32_t, uint32_t> BuildPaceGroupMap(
	const std::vector<uint32_t> &video_track_ids,
	const std::vector<uint32_t> &audio_track_ids,
	const std::vector<std::pair<std::optional<uint32_t>, std::optional<uint32_t>>> &renditions)
{
	std::map<uint32_t, uint32_t> track_to_group;

	for (uint32_t video_id : video_track_ids)
	{
		track_to_group[video_id] = video_id;
	}

	// audio_id -> set of video_ids it appears with in renditions
	std::unordered_map<uint32_t, std::set<uint32_t>> audio_to_videos;
	for (const auto &[video_id, audio_id] : renditions)
	{
		if (!audio_id.has_value())
		{
			continue;
		}
		if (video_id.has_value())
		{
			audio_to_videos[*audio_id].insert(*video_id);
		}
		else
		{
			// Audio-only rendition: ensure the audio is present with an empty
			// video set so it does not accidentally join a video group.
			audio_to_videos.try_emplace(*audio_id);
		}
	}

	for (uint32_t audio_id : audio_track_ids)
	{
		auto it = audio_to_videos.find(audio_id);
		if (it != audio_to_videos.end() && it->second.size() == 1)
		{
			track_to_group[audio_id] = *it->second.begin();
		}
		else
		{
			track_to_group[audio_id] = audio_id;
		}
	}

	return track_to_group;
}
