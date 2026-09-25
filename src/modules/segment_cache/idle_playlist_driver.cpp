//==============================================================================
//
//  OvenMediaEngine - Idle playlist driver (playhead → virtual segment window)
//
//==============================================================================
#include "idle_playlist_driver.h"

#include <algorithm>
#include <cstdint>

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
		return _session->GetSequenceOrigin().msn_base +
			   static_cast<int64_t>(_playhead.loop_count) * static_cast<int64_t>(n) +
			   static_cast<int64_t>(plan_ordinal);
	}

	void IdlePlaylistDriver::RebuildWindow()
	{
		_window.clear();
		_media_sequence_start = 0;
		_discontinuity_sequence = 0;
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
		const size_t want = std::min(_config.window_segments, n);
		const auto &origin = _session->GetSequenceOrigin();

		// Walk backward from the playhead so the window bridges a loop wrap
		// (previous loop's tail + new loop's head) instead of shrinking to 1.
		struct Cursor
		{
			int64_t loop = 0;
			int64_t ord = 0;
		};
		Cursor cur{static_cast<int64_t>(_playhead.loop_count),
				   static_cast<int64_t>(_playhead.segment_ordinal)};

		std::vector<IdlePlaylistEntry> newest_first;
		newest_first.reserve(want);

		for (size_t i = 0; i < want; i++)
		{
			const auto &seg = plan.segments[static_cast<size_t>(cur.ord)];
			IdlePlaylistEntry entry;
			const int64_t own = cur.loop * static_cast<int64_t>(n) + cur.ord;
			entry.plan_ordinal = static_cast<size_t>(cur.ord);
			entry.media_sequence = origin.msn_base + own;
			entry.loop = cur.loop;
			entry.start_dts = seg.start_dts;
			entry.end_dts = seg.end_dts;
			entry.duration_ms = static_cast<double>(seg.end_dts - seg.start_dts) / 90.0;
			entry.part_count = seg.parts.size();
			entry.discontinuity =
				IsWrapDiscontinuity(own, entry.plan_ordinal, n) || (origin.item_boundary && own == 0);
			newest_first.push_back(entry);

			if (cur.ord > 0)
			{
				cur.ord--;
			}
			else if (cur.loop > 0)
			{
				cur.loop--;
				cur.ord = static_cast<int64_t>(n) - 1;
			}
			else
			{
				break;
			}
		}

		_window.assign(newest_first.rbegin(), newest_first.rend());
		if (_window.empty() == false)
		{
			_media_sequence_start = _window.front().media_sequence;
			const int64_t first_own = _media_sequence_start - origin.msn_base;
			_discontinuity_sequence = origin.disc_base +
									  WrapDiscontinuitySequenceBefore(first_own, n) +
									  ((origin.item_boundary && first_own > 0) ? 1 : 0);
		}
	}
}  // namespace segment_cache
