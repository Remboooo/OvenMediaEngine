//==============================================================================
//
//  OvenMediaEngine - SampleIndex sidecar persistence
//
//==============================================================================
#pragma once

#include "sample_index.h"

namespace segment_cache
{
	struct PackagerFingerprint
	{
		uint32_t format_id = 1;           // 1 = MPEG-TS HLS, 2 = LLHLS fMP4
		uint32_t target_duration_ms = 6000;
		uint32_t chunk_duration_ms = 0;   // LLHLS part target; 0 = whole-segment only
		uint32_t schema_version = 2;  // v2: LLHLS parts always start on keyframes (B-frame safe)

		bool operator==(const PackagerFingerprint &other) const
		{
			return format_id == other.format_id &&
				   target_duration_ms == other.target_duration_ms &&
				   chunk_duration_ms == other.chunk_duration_ms &&
				   schema_version == other.schema_version;
		}
	};

	class SampleIndexSidecar
	{
	public:
		// Encodes format_id / target_duration / chunk_duration / schema into the
		// filename so HLS and LLHLS (or different durations) do not collide:
		//   source.mp4.ome-segcache.f2.td2000.cd500.sv2
		static ov::String DefaultPathForSource(const ov::String &source_path,
											   const PackagerFingerprint &fingerprint);

		// Atomic write (tmp + rename). Returns false on I/O error.
		static bool Save(const ov::String &sidecar_path,
						 const SampleIndex &index,
						 const PackagerFingerprint &fingerprint);

		// Returns nullptr if missing, corrupt, version mismatch, source identity
		// mismatch, or fingerprint mismatch.
		static std::shared_ptr<SampleIndex> Load(const ov::String &sidecar_path,
												 const ov::String &expected_source_path,
												 int64_t expected_size,
												 int64_t expected_mtime_ns,
												 const PackagerFingerprint &fingerprint);

		// Load sidecar if valid (persist mode); otherwise Build() from MP4 and
		// optionally Save() when persist_sidecar is true.
		// max_throughput_mbps applies to the Build demux path (0 = unlimited).
		static std::shared_ptr<SampleIndex> LoadOrBuild(const ov::String &source_path,
														const PackagerFingerprint &fingerprint,
														bool persist_sidecar = true,
														double max_throughput_mbps = 0.0,
														ov::String *sidecar_path_out = nullptr);
	};
}  // namespace segment_cache
