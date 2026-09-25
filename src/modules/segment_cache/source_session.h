//==============================================================================
//
//  OvenMediaEngine - Per-source segment cache session (index + plan + hot bytes)
//
//==============================================================================
#pragma once

#include "boundary_planner.h"
#include "hot_segment_cache.h"
#include "options.h"
#include "sample_index.h"
#include "sample_index_sidecar.h"

#include <base/ovlibrary/ovlibrary.h>

#include <memory>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <thread>

namespace segment_cache
{
	struct IdlePlayhead
	{
		size_t segment_ordinal = 0;
		size_t part_ordinal = 0;
		int64_t media_dts = 0;
		uint32_t loop_count = 0;
		double segment_duration_ms = 0;
	};

	// Owns sample index, boundary plan, and hot byte cache for one MP4 source.
	class SourceSession : public std::enable_shared_from_this<SourceSession>
	{
	public:
		static std::shared_ptr<SourceSession> Open(const ov::String &source_path,
												  const PackagerFingerprint &fingerprint,
												  const HotSegmentCache::Config &hot_config = {},
												  bool persist_sidecar = true,
												  double max_throughput_mbps = 0.0);

		const SampleIndex &GetIndex() const { return *_index; }
		const BoundaryPlan &GetPlan() const { return *_plan; }
		const PackagerFingerprint &GetFingerprint() const { return _fingerprint; }
		const ov::String &GetSourcePath() const { return _source_path; }

		// HLS muxed TS by plan ordinal (0 .. segments-1). Uses hot cache.
		std::shared_ptr<const ov::Data> GetHlsTsSegment(size_t segment_ordinal);

		std::shared_ptr<const ov::Data> GetFmp4Init(cmn::MediaType media_type);
		std::shared_ptr<const ov::Data> GetFmp4Segment(cmn::MediaType media_type, size_t segment_ordinal);
		std::shared_ptr<const ov::Data> GetFmp4Part(cmn::MediaType media_type,
													size_t segment_ordinal,
													size_t part_ordinal);

		// Map wall-clock elapsed ms (looping) onto the boundary plan.
		IdlePlayhead ResolvePlayhead(int64_t elapsed_ms) const;

		// Numbering across items. Within a session a segment's own sequence is
		// loop * plan_size + ordinal; the published MEDIA-SEQUENCE is
		// msn_base + own, so the stream keeps counting up when the next item's
		// session replaces this one. disc_base counts discontinuities before
		// own sequence 0; item_boundary marks own sequence 0 as one.
		struct SequenceOrigin
		{
			int64_t msn_base = 0;
			int64_t disc_base = 0;
			bool item_boundary = false;
		};
		// Set before the session is published (SessionRegistry::Put).
		void SetSequenceOrigin(const SequenceOrigin &origin) { _sequence_origin = origin; }
		const SequenceOrigin &GetSequenceOrigin() const { return _sequence_origin; }
		// Origin for a session that replaces this one at the current playhead.
		SequenceOrigin NextSequenceOrigin() const;
		// Published MEDIA-SEQUENCE -> plan ordinal. False if it precedes this session.
		bool ResolveSequence(int64_t media_sequence, size_t &plan_ordinal) const;

		// Shared playhead for idle playlist sync / pump-off mode (ms into looping item).
		void SetElapsedMs(int64_t elapsed_ms);
		int64_t GetElapsedMs() const;

		int64_t GetItemDurationMs() const;

		// Materialize every planned segment/part into the hot cache (blocking).
		// Returns the number of HLS TS segments successfully cached.
		size_t HydrateAll(const HydrateOptions &options = {});

		// Ensure segments around the current playhead are resident in the hot
		// cache (HLS TS + fMP4 segments). Safe to call frequently; cheap on hit.
		void WarmPlayheadWindow(size_t radius_segments = 24);
		// Same as WarmPlayheadWindow but no-ops unless min_interval_ms has elapsed.
		void MaybeWarmPlayheadWindow(size_t radius_segments = 24, int64_t min_interval_ms = 2000);

		// Fire-and-forget hydrate using options (threads + throughput caps).
		void StartGreedyHydrateAsync(const HydrateOptions &options);
		bool IsHydrateDone() const { return _hydrate_done.load(); }

		HotSegmentCache &GetHotCache() { return _hot; }

	private:
		SourceSession(ov::String source_path,
					 PackagerFingerprint fingerprint,
					 std::shared_ptr<SampleIndex> index,
					 std::shared_ptr<BoundaryPlan> plan,
					 HotSegmentCache::Config hot_config);

		HotSegmentKey MakeKey(int64_t start_dts, uint32_t format_id) const;
		void WarmSegmentRange(size_t begin_ordinal, size_t end_ordinal);

		ov::String _source_path;
		PackagerFingerprint _fingerprint;
		std::shared_ptr<SampleIndex> _index;
		std::shared_ptr<BoundaryPlan> _plan;
		mutable HotSegmentCache _hot;
		mutable std::mutex _init_mutex;
		std::shared_ptr<const ov::Data> _video_init;
		std::shared_ptr<const ov::Data> _audio_init;
		std::atomic<int64_t> _elapsed_ms{0};
		std::atomic<bool> _hydrate_running{false};
		std::atomic<bool> _hydrate_done{false};
		std::atomic<int64_t> _last_warm_mono_ms{0};
		SequenceOrigin _sequence_origin;
		// Highest own sequence (loop * plan_size + ordinal) the playhead reached.
		std::atomic<int64_t> _max_own_sequence{-1};
	};

	// Registry: stream_name → SourceSession for GET overrides.
	// Cache-backed playlist/segment serving is gated by SetCacheServeEnabled —
	// set true when entering PlayFileIdle and kept true while demux runs for
	// WebRTC/OVT so HLS/LLHLS stay on the cache (packager playlist writes are
	// suppressed in those publishers while this flag is set).
	class SessionRegistry
	{
	public:
		static SessionRegistry &GetInstance();

		// True when OME_SEGMENT_CACHE env forces enable (XML remains primary).
		static bool IsEnvEnabled();

		void Put(const ov::String &stream_name, const std::shared_ptr<SourceSession> &session);
		void Remove(const ov::String &stream_name);
		std::shared_ptr<SourceSession> Find(const ov::String &stream_name) const;
		void Clear();

		// When true, HLS/LLHLS rewrite playlists and serve segments from cache.
		// Remains true across idle→WebRTC pump resume; cleared on item exit.
		void SetCacheServeEnabled(const ov::String &stream_name, bool enabled);
		bool IsCacheServeEnabled(const ov::String &stream_name) const;

	private:
		mutable std::mutex _mutex;
		std::unordered_map<std::string, std::shared_ptr<SourceSession>> _sessions;
		std::unordered_map<std::string, bool> _cache_serve_enabled;
		// Where the next session's numbering continues, kept across Remove() so a
		// non-cached item in between does not restart MEDIA-SEQUENCE at 0.
		std::unordered_map<std::string, SourceSession::SequenceOrigin> _next_origin;
	};
}  // namespace segment_cache
