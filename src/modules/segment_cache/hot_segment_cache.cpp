//==============================================================================
//
//  OvenMediaEngine - Short-lived materialized segment byte cache
//
//==============================================================================
#include "hot_segment_cache.h"

#define OV_LOG_TAG "SegmentCache.Hot"

namespace segment_cache
{
	HotSegmentCache::HotSegmentCache(const Config &config)
		: _config(config)
	{
		if (_config.max_entries == 0)
		{
			_config.max_entries = 1;
		}
	}

	std::shared_ptr<const ov::Data> HotSegmentCache::Get(const HotSegmentKey &key)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		auto it = _map.find(key);
		if (it == _map.end())
		{
			_misses++;
			return nullptr;
		}

		_lru.splice(_lru.begin(), _lru, it->second.lru_it);
		it->second.lru_it = _lru.begin();
		_hits++;
		return it->second.data;
	}

	void HotSegmentCache::Put(const HotSegmentKey &key, const std::shared_ptr<const ov::Data> &data)
	{
		if (data == nullptr || data->GetLength() == 0)
		{
			return;
		}

		std::lock_guard<std::mutex> lock(_mutex);
		auto it = _map.find(key);
		if (it != _map.end())
		{
			_total_bytes -= it->second.data->GetLength();
			_lru.erase(it->second.lru_it);
			_map.erase(it);
		}

		_lru.push_front(key);
		Entry entry;
		entry.data = data;
		entry.lru_it = _lru.begin();
		_map.emplace(key, entry);
		_total_bytes += data->GetLength();
		EvictIfNeeded_l();
	}

	void HotSegmentCache::Clear()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_map.clear();
		_lru.clear();
		_total_bytes = 0;
	}

	size_t HotSegmentCache::GetEntryCount() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _map.size();
	}

	uint64_t HotSegmentCache::GetTotalBytes() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _total_bytes;
	}

	uint64_t HotSegmentCache::GetHitCount() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _hits;
	}

	uint64_t HotSegmentCache::GetMissCount() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _misses;
	}

	void HotSegmentCache::EvictIfNeeded_l()
	{
		while ((_map.size() > _config.max_entries) || (_total_bytes > _config.max_bytes))
		{
			if (_lru.empty())
			{
				break;
			}
			const HotSegmentKey &victim = _lru.back();
			auto it = _map.find(victim);
			if (it != _map.end())
			{
				_total_bytes -= it->second.data->GetLength();
				_map.erase(it);
			}
			_lru.pop_back();
		}
	}
}  // namespace segment_cache
