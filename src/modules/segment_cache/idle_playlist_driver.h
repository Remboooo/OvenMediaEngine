//==============================================================================
//
//  OvenMediaEngine - Idle playlist driver (playhead → virtual segment window)
//
//==============================================================================
#pragma once

#include "source_session.h"

#include <vector>

namespace segment_cache
{
	struct IdlePlaylistEntry
	{
		size_t plan_ordinal = 0;
		int64_t media_sequence = 0;
		int64_t start_dts = 0;
		int64_t end_dts = 0;
		double duration_ms = 0;
		size_t part_count = 0;
		// True when this entry is the first segment of loop k>=1 (file wrap).
		bool discontinuity = false;
	};

	// Wrap discontinuities sit at media_sequence = k * plan_size for k >= 1.
	// EXT-X-DISCONTINUITY-SEQUENCE is the count of those with msn < first_msn.
	inline int64_t WrapDiscontinuitySequenceBefore(int64_t first_media_sequence, size_t plan_size)
	{
		if (plan_size == 0 || first_media_sequence <= 0)
		{
			return 0;
		}
		return (first_media_sequence - 1) / static_cast<int64_t>(plan_size);
	}

	inline bool IsWrapDiscontinuity(int64_t media_sequence, size_t plan_ordinal, size_t plan_size)
	{
		return plan_size > 0 && plan_ordinal == 0 &&
			   media_sequence >= static_cast<int64_t>(plan_size);
	}

	// Advances a sliding playlist window from wall-clock elapsed time alone.
	// No demux / no packager — used to prove idle HLS/LLHLS playlist motion.
	class IdlePlaylistDriver
	{
	public:
		struct Config
		{
			// How many segments to advertise ending at the wall-clock playhead
			// (oldest → playhead), walking backward across loop wraps.
			size_t window_segments = 3;
			// Extra segments past the playhead (classic HLS only). Future media is
			// deterministic under SegmentCache; 0 keeps the edge at the playhead.
			size_t lookahead_segments = 0;
		};

		IdlePlaylistDriver(std::shared_ptr<const SourceSession> session, Config config);

		void SetEpochElapsedMs(int64_t elapsed_ms);

		IdlePlayhead GetPlayhead() const { return _playhead; }

		// Media sequence of the oldest segment still in the window.
		int64_t GetMediaSequenceStart() const { return _media_sequence_start; }

		// Segments currently in the playlist window (oldest → newest).
		const std::vector<IdlePlaylistEntry> &GetWindow() const { return _window; }

		// Absolute media sequence for a plan ordinal at the current loop.
		int64_t MediaSequenceForOrdinal(size_t plan_ordinal) const;

	private:
		void RebuildWindow();

		std::shared_ptr<const SourceSession> _session;
		Config _config;
		IdlePlayhead _playhead;
		int64_t _elapsed_ms = 0;
		int64_t _media_sequence_start = 0;
		std::vector<IdlePlaylistEntry> _window;
	};
}  // namespace segment_cache
