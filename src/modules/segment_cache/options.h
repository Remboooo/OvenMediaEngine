//==============================================================================
//
//  OvenMediaEngine - Runtime segment-cache options (resolved from Server.xml / env)
//
//==============================================================================
#pragma once

#include <base/ovlibrary/ovlibrary.h>

#include <cstdint>

namespace segment_cache
{
	enum class CacheMode
	{
		Off,
		Memory,
		Persist
	};

	enum class HydrateMode
	{
		Lazy,
		Greedy
	};

	struct HydrateOptions
	{
		HydrateMode mode = HydrateMode::Lazy;
		uint32_t max_threads = 1;
		// Soft cap on aggregate index+hydrate throughput (Mbps). 0 = unlimited.
		double max_throughput_mbps = 0.0;

		bool IsGreedy() const { return mode == HydrateMode::Greedy; }
	};

	struct Options
	{
		CacheMode mode = CacheMode::Off;
		HydrateOptions hydrate;
		int64_t idle_grace_period_ms = 30000;
		// Segments past wall-clock playhead listed in classic HLS idle playlists.
		size_t hls_lookahead_segments = 0;

		bool IsEnabled() const { return mode != CacheMode::Off; }
		bool PersistSidecar() const { return mode == CacheMode::Persist; }
	};
}  // namespace segment_cache
