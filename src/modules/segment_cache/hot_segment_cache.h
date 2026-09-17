//==============================================================================
//
//  OvenMediaEngine - Short-lived materialized segment byte cache
//
//==============================================================================
#pragma once

#include <base/ovlibrary/ovlibrary.h>

#include <cstdint>
#include <list>
#include <mutex>
#include <unordered_map>

namespace segment_cache
{
	struct HotSegmentKey
	{
		ov::String source_path;
		int64_t start_dts = 0;
		uint32_t target_duration_ms = 0;
		uint32_t format_id = 1;

		bool operator==(const HotSegmentKey &other) const
		{
			return start_dts == other.start_dts &&
				   target_duration_ms == other.target_duration_ms &&
				   format_id == other.format_id &&
				   source_path == other.source_path;
		}
	};

	struct HotSegmentKeyHash
	{
		size_t operator()(const HotSegmentKey &key) const
		{
			size_t h = std::hash<std::string>{}(key.source_path.CStr());
			h ^= std::hash<int64_t>{}(key.start_dts) + 0x9e3779b9 + (h << 6) + (h >> 2);
			h ^= std::hash<uint32_t>{}(key.target_duration_ms) + 0x9e3779b9 + (h << 6) + (h >> 2);
			h ^= std::hash<uint32_t>{}(key.format_id) + 0x9e3779b9 + (h << 6) + (h >> 2);
			return h;
		}
	};

	class HotSegmentCache
	{
	public:
		struct Config
		{
			size_t max_entries = 16;
			uint64_t max_bytes = 64ull * 1024ull * 1024ull;  // 64 MB
		};

		explicit HotSegmentCache(const Config &config);

		std::shared_ptr<const ov::Data> Get(const HotSegmentKey &key);
		void Put(const HotSegmentKey &key, const std::shared_ptr<const ov::Data> &data);
		void Clear();

		size_t GetEntryCount() const;
		uint64_t GetTotalBytes() const;
		uint64_t GetHitCount() const;
		uint64_t GetMissCount() const;

	private:
		void EvictIfNeeded_l();

		mutable std::mutex _mutex;
		Config _config;
		std::list<HotSegmentKey> _lru;
		struct Entry
		{
			std::shared_ptr<const ov::Data> data;
			std::list<HotSegmentKey>::iterator lru_it;
		};
		std::unordered_map<HotSegmentKey, Entry, HotSegmentKeyHash> _map;
		uint64_t _total_bytes = 0;
		uint64_t _hits = 0;
		uint64_t _misses = 0;
	};
}  // namespace segment_cache
