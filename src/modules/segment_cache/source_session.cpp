//==============================================================================
//
//  OvenMediaEngine - Per-source segment cache session (index + plan + hot bytes)
//
//==============================================================================
#include "source_session.h"

#include "fmp4_materializer.h"
#include "mpegts_materializer.h"
#include "throughput_limiter.h"

#include <base/ovlibrary/time.h>

#include <cstdlib>
#include <cstring>
#include <cinttypes>
#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

#define OV_LOG_TAG "SegmentCache.Session"

namespace segment_cache
{
	SourceSession::SourceSession(ov::String source_path,
							   PackagerFingerprint fingerprint,
							   std::shared_ptr<SampleIndex> index,
							   std::shared_ptr<BoundaryPlan> plan,
							   HotSegmentCache::Config hot_config)
		: _source_path(std::move(source_path)),
		  _fingerprint(fingerprint),
		  _index(std::move(index)),
		  _plan(std::move(plan)),
		  _hot(hot_config)
	{
	}

	std::shared_ptr<SourceSession> SourceSession::Open(const ov::String &source_path,
													  const PackagerFingerprint &fingerprint,
													  const HotSegmentCache::Config &hot_config,
													  bool persist_sidecar,
													  double max_throughput_mbps)
	{
		auto index = SampleIndexSidecar::LoadOrBuild(
			source_path, fingerprint, persist_sidecar, max_throughput_mbps);
		if (index == nullptr)
		{
			return nullptr;
		}

		auto plan = BoundaryPlanner::Build(*index, fingerprint);
		if (plan == nullptr)
		{
			return nullptr;
		}

		HotSegmentCache::Config hot = hot_config;
		// Live window budget: ~64 segments × (TS + fMP4 A/V) under HD bitrates.
		// Full multi-hour plans do not fit in RAM; WarmPlayheadWindow keeps the
		// playhead resident after greedy hydrate.
		const size_t needed = std::max(plan->segments.size() * 4 + 8, size_t(256));
		hot.max_entries = std::max(hot.max_entries, std::min(needed, size_t(512)));
		hot.max_bytes = std::max(hot.max_bytes, static_cast<uint64_t>(1024ull * 1024ull * 1024ull));

		return std::shared_ptr<SourceSession>(
			new SourceSession(source_path, fingerprint, index, plan, hot));
	}

	void SourceSession::WarmSegmentRange(size_t begin_ordinal, size_t end_ordinal)
	{
		if (_plan == nullptr || _plan->segments.empty())
		{
			return;
		}
		end_ordinal = std::min(end_ordinal, _plan->segments.size());
		begin_ordinal = std::min(begin_ordinal, end_ordinal);

		(void)GetFmp4Init(cmn::MediaType::Video);
		(void)GetFmp4Init(cmn::MediaType::Audio);

		for (size_t i = begin_ordinal; i < end_ordinal; i++)
		{
			(void)GetHlsTsSegment(i);
			if (_fingerprint.format_id == 2)
			{
				(void)GetFmp4Segment(cmn::MediaType::Video, i);
				(void)GetFmp4Segment(cmn::MediaType::Audio, i);
			}
		}
	}

	void SourceSession::WarmPlayheadWindow(size_t radius_segments)
	{
		if (_plan == nullptr || _plan->segments.empty())
		{
			return;
		}
		// Stamp first so nested Get* → MaybeWarmPlayheadWindow cannot re-enter.
		_last_warm_mono_ms.store(ov::Time::GetMonotonicTimestamp());
		const auto head = ResolvePlayhead(GetElapsedMs());
		const size_t center = head.segment_ordinal;
		const size_t begin = (center > radius_segments) ? (center - radius_segments) : 0;
		const size_t end = std::min(_plan->segments.size(), center + radius_segments + 1);
		WarmSegmentRange(begin, end);
	}

	void SourceSession::MaybeWarmPlayheadWindow(size_t radius_segments, int64_t min_interval_ms)
	{
		const int64_t now = ov::Time::GetMonotonicTimestamp();
		const int64_t last = _last_warm_mono_ms.load();
		if (last > 0 && (now - last) < min_interval_ms)
		{
			return;
		}
		WarmPlayheadWindow(radius_segments);
	}

	size_t SourceSession::HydrateAll(const HydrateOptions &options)
	{
		size_t hydrated = 0;
		if (_plan == nullptr || _plan->segments.empty())
		{
			_hydrate_done.store(true);
			return 0;
		}

		(void)GetFmp4Init(cmn::MediaType::Video);
		(void)GetFmp4Init(cmn::MediaType::Audio);

		const size_t segment_count = _plan->segments.size();
		// Do not try to pin an entire multi-hour HD plan in RAM — that overflows
		// the hot cache and LRU-evicts the live window (GET rematerialize stalls).
		// Warm a playhead-centered window instead; TickCachePlayhead refreshes it.
		constexpr size_t kMaxWarmSegments = 64;
		const size_t warm_count = std::min(segment_count, kMaxWarmSegments);
		const auto head = ResolvePlayhead(GetElapsedMs());
		const size_t center = head.segment_ordinal;
		const size_t radius = warm_count / 2;
		const size_t begin = (center > radius) ? (center - radius) : 0;
		const size_t end = std::min(segment_count, begin + warm_count);

		const uint32_t thread_count =
			std::max<uint32_t>(1, std::min<uint32_t>(options.max_threads, static_cast<uint32_t>(end - begin)));

		auto limiter = std::make_shared<ThroughputLimiter>(options.max_throughput_mbps);

		std::atomic<size_t> next_ordinal{begin};
		std::atomic<size_t> hydrated_count{0};
		std::vector<std::thread> workers;
		workers.reserve(thread_count);

		for (uint32_t t = 0; t < thread_count; t++)
		{
			workers.emplace_back([this, &next_ordinal, &hydrated_count, limiter, end]() {
				while (true)
				{
					const size_t i = next_ordinal.fetch_add(1);
					if (i >= end)
					{
						break;
					}

					uint64_t produced = 0;
					if (auto ts = GetHlsTsSegment(i))
					{
						produced += ts->GetLength();
						hydrated_count.fetch_add(1);
					}

					if (_fingerprint.format_id == 2)
					{
						if (auto v = GetFmp4Segment(cmn::MediaType::Video, i))
						{
							produced += v->GetLength();
						}
						if (auto a = GetFmp4Segment(cmn::MediaType::Audio, i))
						{
							produced += a->GetLength();
						}
						// Parts rematerialize cheaply on demand — skipping them here
						// keeps the hot cache for full segments around the playhead.
					}

					limiter->Account(produced);
				}
			});
		}

		for (auto &worker : workers)
		{
			worker.join();
		}

		hydrated = hydrated_count.load();
		WarmPlayheadWindow(std::max<size_t>(24, radius));
		_hydrate_done.store(true);
		logti("Greedy hydrate complete for %s: warmed %zu/%zu HLS segments around playhead "
			  "(ordinal %zu), threads=%u, cap=%.1f Mbps, hot entries=%zu bytes=%" PRIu64,
			  _source_path.CStr(), hydrated, segment_count, center, thread_count,
			  options.max_throughput_mbps,
			  _hot.GetEntryCount(), _hot.GetTotalBytes());
		return hydrated;
	}

	void SourceSession::StartGreedyHydrateAsync(const HydrateOptions &options)
	{
		if (options.IsGreedy() == false)
		{
			return;
		}
		bool expected = false;
		if (_hydrate_running.compare_exchange_strong(expected, true) == false)
		{
			return;
		}

		logti("Starting greedy hydrate for %s (threads=%u, max_throughput=%.1f Mbps)",
			  _source_path.CStr(), options.max_threads, options.max_throughput_mbps);

		auto self = shared_from_this();
		std::thread([self, options]() {
			self->HydrateAll(options);
			self->_hydrate_running.store(false);
		}).detach();
	}

	HotSegmentKey SourceSession::MakeKey(int64_t start_dts, uint32_t format_id) const
	{
		HotSegmentKey key;
		key.source_path = _source_path;
		key.start_dts = start_dts;
		key.target_duration_ms = _fingerprint.target_duration_ms;
		key.format_id = format_id;
		return key;
	}

	std::shared_ptr<const ov::Data> SourceSession::GetHlsTsSegment(size_t segment_ordinal)
	{
		if (segment_ordinal >= _plan->segments.size())
		{
			return nullptr;
		}

		const auto &seg = _plan->segments[segment_ordinal];
		auto key = MakeKey(seg.start_dts, 1);
		if (auto hit = _hot.Get(key))
		{
			return hit;
		}

		auto result = MpegTsMaterializer::Materialize(_index, seg.start_dts, _fingerprint.target_duration_ms);
		if (result == nullptr || result->data == nullptr)
		{
			return nullptr;
		}

		_hot.Put(key, result->data);
		return result->data;
	}

	std::shared_ptr<const ov::Data> SourceSession::GetFmp4Init(cmn::MediaType media_type)
	{
		std::lock_guard lock(_init_mutex);
		if (media_type == cmn::MediaType::Video)
		{
			if (_video_init == nullptr)
			{
				const auto *track = _index->FindTrackByMediaType(cmn::MediaType::Video);
				if (track == nullptr)
				{
					return nullptr;
				}
				_video_init = Fmp4Materializer::MaterializeInit(track->media_track);
			}
			return _video_init;
		}
		if (media_type == cmn::MediaType::Audio)
		{
			if (_audio_init == nullptr)
			{
				const auto *track = _index->FindTrackByMediaType(cmn::MediaType::Audio);
				if (track == nullptr)
				{
					return nullptr;
				}
				_audio_init = Fmp4Materializer::MaterializeInit(track->media_track);
			}
			return _audio_init;
		}
		return nullptr;
	}

	std::shared_ptr<const ov::Data> SourceSession::GetFmp4Segment(cmn::MediaType media_type,
																 size_t segment_ordinal)
	{
		if (segment_ordinal >= _plan->segments.size())
		{
			return nullptr;
		}

		const auto &seg = _plan->segments[segment_ordinal];
		// Encode media type into format_id high nibble so video/audio don't collide.
		const uint32_t format_id = 2u + (media_type == cmn::MediaType::Audio ? 100u : 0u);
		auto key = MakeKey(seg.start_dts, format_id);
		if (auto hit = _hot.Get(key))
		{
			return hit;
		}

		auto result = Fmp4Materializer::MaterializeSampleRange(
			_index, media_type, seg.video_start, seg.video_end);
		if (result == nullptr || result->data == nullptr)
		{
			return nullptr;
		}

		_hot.Put(key, result->data);
		return result->data;
	}

	std::shared_ptr<const ov::Data> SourceSession::GetFmp4Part(cmn::MediaType media_type,
															  size_t segment_ordinal,
															  size_t part_ordinal)
	{
		if (segment_ordinal >= _plan->segments.size())
		{
			return nullptr;
		}

		const auto &seg = _plan->segments[segment_ordinal];
		if (seg.parts.empty() || part_ordinal >= seg.parts.size())
		{
			return nullptr;
		}

		const auto &part = seg.parts[part_ordinal];
		const uint32_t format_id = 3u + (media_type == cmn::MediaType::Audio ? 100u : 0u);
		// Parts are uniquely identified by their start_dts within a source.
		auto key = MakeKey(part.start_dts, format_id);
		if (auto hit = _hot.Get(key))
		{
			return hit;
		}

		auto result = Fmp4Materializer::MaterializeSampleRange(
			_index, media_type, part.video_start, part.video_end);
		if (result == nullptr || result->data == nullptr)
		{
			return nullptr;
		}

		_hot.Put(key, result->data);
		return result->data;
	}

	int64_t SourceSession::GetItemDurationMs() const
	{
		if (_plan->segments.empty())
		{
			return 0;
		}
		const auto &last = _plan->segments.back();
		return (last.end_dts - _plan->segments.front().start_dts) / 90;
	}

	void SourceSession::SetElapsedMs(int64_t elapsed_ms)
	{
		if (elapsed_ms < 0)
		{
			elapsed_ms = 0;
		}
		_elapsed_ms.store(elapsed_ms);
	}

	int64_t SourceSession::GetElapsedMs() const
	{
		return _elapsed_ms.load();
	}

	IdlePlayhead SourceSession::ResolvePlayhead(int64_t elapsed_ms) const
	{
		IdlePlayhead head;
		const int64_t item_ms = GetItemDurationMs();
		if (item_ms <= 0 || _plan->segments.empty())
		{
			return head;
		}

		if (elapsed_ms < 0)
		{
			elapsed_ms = 0;
		}

		head.loop_count = static_cast<uint32_t>(elapsed_ms / item_ms);
		const int64_t position_ms = elapsed_ms % item_ms;
		const int64_t position_dts =
			_plan->segments.front().start_dts + position_ms * 90;

		for (size_t i = 0; i < _plan->segments.size(); i++)
		{
			const auto &seg = _plan->segments[i];
			if (position_dts >= seg.start_dts && position_dts < seg.end_dts)
			{
				head.segment_ordinal = i;
				head.media_dts = position_dts;
				head.segment_duration_ms = static_cast<double>(seg.end_dts - seg.start_dts) / 90.0;

				if (seg.parts.empty() == false)
				{
					for (size_t p = 0; p < seg.parts.size(); p++)
					{
						if (position_dts >= seg.parts[p].start_dts &&
							position_dts < seg.parts[p].end_dts)
						{
							head.part_ordinal = p;
							break;
						}
						if (p + 1 == seg.parts.size())
						{
							head.part_ordinal = p;
						}
					}
				}
				return head;
			}
		}

		// Past last sample: clamp to final segment.
		head.segment_ordinal = _plan->segments.size() - 1;
		head.media_dts = _plan->segments.back().end_dts;
		head.segment_duration_ms =
			static_cast<double>(_plan->segments.back().end_dts - _plan->segments.back().start_dts) / 90.0;
		if (_plan->segments.back().parts.empty() == false)
		{
			head.part_ordinal = _plan->segments.back().parts.size() - 1;
		}
		return head;
	}

	SessionRegistry &SessionRegistry::GetInstance()
	{
		static SessionRegistry instance;
		return instance;
	}

	bool SessionRegistry::IsEnvEnabled()
	{
		const char *env = std::getenv("OME_SEGMENT_CACHE");
		return env != nullptr && env[0] == '1' && env[1] == '\0';
	}

	void SessionRegistry::Put(const ov::String &stream_name, const std::shared_ptr<SourceSession> &session)
	{
		std::lock_guard lock(_mutex);
		_sessions[stream_name.CStr()] = session;
		// New sessions default to packager-serve until Scheduled enters idle.
		_cache_serve_enabled.emplace(stream_name.CStr(), false);
	}

	void SessionRegistry::Remove(const ov::String &stream_name)
	{
		std::lock_guard lock(_mutex);
		_sessions.erase(stream_name.CStr());
		_cache_serve_enabled.erase(stream_name.CStr());
	}

	std::shared_ptr<SourceSession> SessionRegistry::Find(const ov::String &stream_name) const
	{
		std::lock_guard lock(_mutex);
		auto it = _sessions.find(stream_name.CStr());
		if (it == _sessions.end())
		{
			return nullptr;
		}
		return it->second;
	}

	void SessionRegistry::Clear()
	{
		std::lock_guard lock(_mutex);
		_sessions.clear();
		_cache_serve_enabled.clear();
	}

	void SessionRegistry::SetCacheServeEnabled(const ov::String &stream_name, bool enabled)
	{
		std::lock_guard lock(_mutex);
		_cache_serve_enabled[stream_name.CStr()] = enabled;
	}

	bool SessionRegistry::IsCacheServeEnabled(const ov::String &stream_name) const
	{
		std::lock_guard lock(_mutex);
		auto it = _cache_serve_enabled.find(stream_name.CStr());
		return it != _cache_serve_enabled.end() && it->second;
	}
}  // namespace segment_cache
