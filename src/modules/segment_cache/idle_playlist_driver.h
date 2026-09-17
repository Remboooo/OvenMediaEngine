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
	};

	// Advances a sliding playlist window from wall-clock elapsed time alone.
	// No demux / no packager — used to prove idle HLS/LLHLS playlist motion.
	class IdlePlaylistDriver
	{
	public:
		struct Config
		{
			size_t window_segments = 3;  // how many completed segments to advertise
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
