//==============================================================================
//
//  OvenMediaEngine - Shared soft throughput cap for segment-cache I/O
//
//==============================================================================
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace segment_cache
{
		// Soft cap on aggregate bytes/time. 0 mbps = unlimited.
		// Safe to share across hydrate worker threads (Account is atomic).
		// Counts compressed media bytes (index demux packet sizes / materialized outputs).
		class ThroughputLimiter
		{
	public:
		explicit ThroughputLimiter(double max_throughput_mbps)
			: _max_throughput_mbps(max_throughput_mbps),
			  _start(std::chrono::steady_clock::now())
		{
		}

		void Account(uint64_t bytes)
		{
			if (_max_throughput_mbps <= 0.0 || bytes == 0)
			{
				return;
			}

			const uint64_t total = _bytes.fetch_add(bytes) + bytes;
			const double min_seconds =
				(static_cast<double>(total) / (1024.0 * 1024.0)) / _max_throughput_mbps;
			const auto elapsed = std::chrono::steady_clock::now() - _start;
			const auto min_duration = std::chrono::duration<double>(min_seconds);
			if (elapsed < min_duration)
			{
				std::this_thread::sleep_for(min_duration - elapsed);
			}
		}

		double GetMaxThroughputMbps() const { return _max_throughput_mbps; }

	private:
		double _max_throughput_mbps = 0.0;
		std::atomic<uint64_t> _bytes{0};
		std::chrono::steady_clock::time_point _start;
	};
}  // namespace segment_cache
