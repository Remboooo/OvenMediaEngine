//==============================================================================
//
//  OvenMediaEngine - Scheduled SegmentCache config (Server.xml)
//
//==============================================================================
#pragma once

namespace cfg
{
	namespace vhost
	{
		namespace app
		{
			namespace pvd
			{
				// Nested under Providers/Schedule/SegmentCache/Hydrate
				struct SegmentCacheHydrate : public Item
				{
				protected:
					ov::String _mode = "lazy";  // lazy | greedy
					int _max_threads = 1;
					// Soft cap on aggregate materialize throughput across all hydrate
					// workers (and on sample-index Build). 0 = unlimited.
					double _max_throughput_mbps = 0.0;

				public:
					CFG_DECLARE_CONST_REF_GETTER_OF(GetMode, _mode)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetMaxThreads, _max_threads)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetMaxThroughputMbps, _max_throughput_mbps)

					bool IsGreedy() const
					{
						return _mode.LowerCaseString() == "greedy";
					}

				protected:
					void MakeList() override
					{
						Register<Optional>("Mode", &_mode, nullptr, [=]() -> std::shared_ptr<ConfigError> {
							const auto mode = _mode.LowerCaseString();
							if (mode != "lazy" && mode != "greedy")
							{
								return CreateConfigErrorPtr(
									"SegmentCache/Hydrate/Mode must be 'lazy' or 'greedy' (got: %s)",
									_mode.CStr());
							}
							return nullptr;
						});
						Register<Optional>("MaxThreads", &_max_threads, nullptr, [=]() -> std::shared_ptr<ConfigError> {
							if (_max_threads < 1 || _max_threads > 64)
							{
								return CreateConfigErrorPtr(
									"SegmentCache/Hydrate/MaxThreads must be between 1 and 64 (got: %d)",
									_max_threads);
							}
							return nullptr;
						});
						Register<Optional>("MaxThroughputMbps", &_max_throughput_mbps, nullptr, [=]() -> std::shared_ptr<ConfigError> {
							if (_max_throughput_mbps < 0.0)
							{
								return CreateConfigErrorPtr(
									"SegmentCache/Hydrate/MaxThroughputMbps must be >= 0 (0 = unlimited)");
							}
							return nullptr;
						});
					}
				};

				// Nested under Providers/Schedule
				//
				// Segment cache targets bypass H.264+AAC MP4. Enabling it with
				// BypassTranscoder=false or unsupported file:// media logs a warning
				// and falls back to demux — schedules are not rejected.
				struct SegmentCache : public Item
				{
				protected:
					bool _enable = false;
					// memory = index in RAM only; persist = also write .ome-segcache.* sidecars
					ov::String _mode = "persist";
					SegmentCacheHydrate _hydrate;
					int _idle_grace_period_ms = 30000;
					// Classic HLS only: advertise this many segments past the wall-clock
					// playhead so players that join ~2–3 segments behind the live edge
					// land near LLHLS / schedule time. 0 = edge at playhead (default).
					int _hls_lookahead_segments = 0;

				public:
					CFG_DECLARE_CONST_REF_GETTER_OF(IsEnable, _enable)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetMode, _mode)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetHydrate, _hydrate)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetIdleGracePeriodMs, _idle_grace_period_ms)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetHlsLookaheadSegments, _hls_lookahead_segments)

					bool IsPersist() const
					{
						return _mode.LowerCaseString() != "memory";
					}

				protected:
					void MakeList() override
					{
						Register<Optional>("Enable", &_enable);
						Register<Optional>("Mode", &_mode, nullptr, [=]() -> std::shared_ptr<ConfigError> {
							const auto mode = _mode.LowerCaseString();
							if (mode != "persist" && mode != "memory")
							{
								return CreateConfigErrorPtr(
									"SegmentCache/Mode must be 'persist' or 'memory' (got: %s)",
									_mode.CStr());
							}
							return nullptr;
						});
						Register<Optional>("Hydrate", &_hydrate);
						Register<Optional>("IdleGracePeriodMs", &_idle_grace_period_ms, nullptr, [=]() -> std::shared_ptr<ConfigError> {
							if (_idle_grace_period_ms < 0 || _idle_grace_period_ms > 600000)
							{
								return CreateConfigErrorPtr(
									"SegmentCache/IdleGracePeriodMs must be between 0 and 600000 (got: %d)",
									_idle_grace_period_ms);
							}
							return nullptr;
						});
						Register<Optional>("HlsLookaheadSegments", &_hls_lookahead_segments, nullptr, [=]() -> std::shared_ptr<ConfigError> {
							if (_hls_lookahead_segments < 0 || _hls_lookahead_segments > 100)
							{
								return CreateConfigErrorPtr(
									"SegmentCache/HlsLookaheadSegments must be between 0 and 100 (got: %d)",
									_hls_lookahead_segments);
							}
							return nullptr;
						});
					}
				};
			}  // namespace pvd
		}  // namespace app
	}  // namespace vhost
}  // namespace cfg
