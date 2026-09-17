//==============================================================================
//
//  OvenMediaEngine - Scheduled segment cache (sample index)
//
//==============================================================================
#pragma once

#include <base/info/media_track.h>
#include <base/ovlibrary/ovlibrary.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace segment_cache
{
	struct SampleRef
	{
		int64_t file_offset = -1;
		uint32_t size = 0;
		int64_t pts = 0;  // track timebase
		int64_t dts = 0;
		int64_t duration = 0;
		bool keyframe = false;
	};

	struct TrackIndex
	{
		uint32_t track_id = 0;
		int ffmpeg_stream_index = -1;
		std::shared_ptr<MediaTrack> media_track;
		std::vector<SampleRef> samples;
	};

	// Container-agnostic index of an MP4 (or similar) source for later materialize.
	class SampleIndex
	{
	public:
		const ov::String &GetSourcePath() const { return _source_path; }
		int64_t GetSourceSize() const { return _source_size; }
		int64_t GetSourceMtimeNs() const { return _source_mtime_ns; }

		void SetSourceIdentity(const ov::String &path, int64_t size, int64_t mtime_ns)
		{
			_source_path = path;
			_source_size = size;
			_source_mtime_ns = mtime_ns;
		}

		const std::vector<TrackIndex> &GetTracks() const { return _tracks; }
		std::vector<TrackIndex> &GetTracks() { return _tracks; }

		const TrackIndex *FindTrackByMediaType(cmn::MediaType type) const
		{
			for (const auto &track : _tracks)
			{
				if (track.media_track != nullptr && track.media_track->GetMediaType() == type)
				{
					return &track;
				}
			}
			return nullptr;
		}

		TrackIndex *FindTrackByFfmpegIndex(int stream_index)
		{
			for (auto &track : _tracks)
			{
				if (track.ffmpeg_stream_index == stream_index)
				{
					return &track;
				}
			}
			return nullptr;
		}

		const TrackIndex *FindTrackByFfmpegIndex(int stream_index) const
		{
			for (const auto &track : _tracks)
			{
				if (track.ffmpeg_stream_index == stream_index)
				{
					return &track;
				}
			}
			return nullptr;
		}

	private:
		ov::String _source_path;
		int64_t _source_size = 0;
		int64_t _source_mtime_ns = 0;
		std::vector<TrackIndex> _tracks;
	};
}  // namespace segment_cache
