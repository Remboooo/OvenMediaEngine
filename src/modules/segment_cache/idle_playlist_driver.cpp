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
		const size_t want = std::min(_config.window_segments, n);

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
		newest_first.reserve(want + _config.lookahead_segments);

		auto MakeEntry = [&](const Cursor &c) {
			const auto &seg = plan.segments[static_cast<size_t>(c.ord)];
			IdlePlaylistEntry entry;
			entry.plan_ordinal = static_cast<size_t>(c.ord);
			entry.media_sequence = c.loop * static_cast<int64_t>(n) + c.ord;
			entry.start_dts = seg.start_dts;
			entry.end_dts = seg.end_dts;
			entry.duration_ms = static_cast<double>(seg.end_dts - seg.start_dts) / 90.0;
			entry.part_count = seg.parts.size();
			entry.discontinuity =
				IsWrapDiscontinuity(entry.media_sequence, entry.plan_ordinal, n);
			return entry;
		};

		for (size_t i = 0; i < want; i++)
		{
			newest_first.push_back(MakeEntry(cur));

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

		// Optional forward walk past the playhead (scheduled HLS buffer-ahead).
		// Do NOT cross into the next file loop: advertising EXT-X-DISCONTINUITY
		// and next-loop segments before wall-clock wrap lets players consume the
		// wrap early, then stall when the tip freezes until the real playhead
		// catches up. Mid-file lookahead is enough for join-depth alignment.
		Cursor fwd{static_cast<int64_t>(_playhead.loop_count),
				   static_cast<int64_t>(_playhead.segment_ordinal)};
		for (size_t i = 0; i < _config.lookahead_segments; i++)
		{
			if (fwd.ord + 1 >= static_cast<int64_t>(n))
			{
				break;
			}
			fwd.ord++;
			_window.push_back(MakeEntry(fwd));
		}

		if (_window.empty() == false)
		{
			_media_sequence_start = _window.front().media_sequence;
		}
	}
}  // namespace segment_cache
