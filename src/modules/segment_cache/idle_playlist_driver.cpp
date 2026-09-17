//==============================================================================
//
//  OvenMediaEngine - Idle playlist driver (playhead → virtual segment window)
//
//==============================================================================
#include "idle_playlist_driver.h"

#include <algorithm>

#define OV_LOG_TAG "SegmentCache.IdlePlaylist"

namespace segment_cache
{
	IdlePlaylistDriver::IdlePlaylistDriver(std::shared_ptr<const SourceSession> session, Config config)
		: _session(std::move(session)),
		  _config(config)
	{
		if (_config.window_segments == 0)
		{
			_config.window_segments = 1;
		}
		RebuildWindow();
	}

	void IdlePlaylistDriver::SetEpochElapsedMs(int64_t elapsed_ms)
	{
		if (elapsed_ms < 0)
		{
			elapsed_ms = 0;
		}
		_elapsed_ms = elapsed_ms;
		RebuildWindow();
	}

	int64_t IdlePlaylistDriver::MediaSequenceForOrdinal(size_t plan_ordinal) const
	{
		if (_session == nullptr || _session->GetPlan().segments.empty())
		{
			return 0;
		}
		const size_t n = _session->GetPlan().segments.size();
		return static_cast<int64_t>(_playhead.loop_count) * static_cast<int64_t>(n) +
			   static_cast<int64_t>(plan_ordinal);
	}

	void IdlePlaylistDriver::RebuildWindow()
	{
		_window.clear();
		_media_sequence_start = 0;
		_playhead = {};

		if (_session == nullptr)
		{
			return;
		}

		const auto &plan = _session->GetPlan();
		if (plan.segments.empty())
		{
			return;
		}

		_playhead = _session->ResolvePlayhead(_elapsed_ms);
		const size_t n = plan.segments.size();
		const size_t window = std::min(_config.window_segments, n);

		// Advertise the last `window` completed segments ending at the current one.
		// Current segment is included so players can request the live edge.
		const size_t end_ord = _playhead.segment_ordinal;
		size_t start_ord = 0;
		if (end_ord + 1 >= window)
		{
			start_ord = end_ord + 1 - window;
		}

		// Absolute sequence accounts for prior full loops.
		const int64_t loop_base =
			static_cast<int64_t>(_playhead.loop_count) * static_cast<int64_t>(n);

		_media_sequence_start = loop_base + static_cast<int64_t>(start_ord);

		for (size_t ord = start_ord; ord <= end_ord; ord++)
		{
			const auto &seg = plan.segments[ord];
			IdlePlaylistEntry entry;
			entry.plan_ordinal = ord;
			entry.media_sequence = loop_base + static_cast<int64_t>(ord);
			entry.start_dts = seg.start_dts;
			entry.end_dts = seg.end_dts;
			entry.duration_ms = static_cast<double>(seg.end_dts - seg.start_dts) / 90.0;
			entry.part_count = seg.parts.size();
			_window.push_back(entry);
		}
	}
}  // namespace segment_cache
