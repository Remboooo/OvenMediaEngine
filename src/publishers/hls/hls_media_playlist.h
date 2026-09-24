//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Getroot
//  Copyright (c) 2024 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include <base/info/stream.h>

#include <base/ovlibrary/ovlibrary.h>
#include <base/info/media_track.h>
#include <modules/containers/mpegts/mpegts_packager.h>

#include <atomic>
#include <shared_mutex>
#include <vector>

class HlsMediaPlaylist
{
public:
	struct HlsMediaPlaylistConfig
	{
		uint8_t version = 3;
		size_t segment_count = 10; // It shows the number of segments in the playlist if rewind is disabled
		size_t target_duration = 6;
		bool event_playlist_type = false;
	};

	HlsMediaPlaylist(const ov::String &variant_name, const ov::String &playlist_file_name, const HlsMediaPlaylistConfig &config);

	void AddMediaTrackInfo(const std::shared_ptr<const MediaTrack> &track);
	// Replace the current track of the same id at runtime, so master attributes
	// (resolution, framerate, subtitle name/language) follow a configuration change
	void UpdateMediaTrackInfo(const std::shared_ptr<const MediaTrack> &track);
	bool HasTrack(uint32_t track_id) const;

	// A rendition whose current codec is unsupported stops being advertised
	void SetExcluded(bool excluded) { _excluded = excluded; }
	bool IsExcluded() const { return _excluded; }

	int64_t GetWallclockOffset() const { return _wallclock_offset_ms; }
	void SetWallclockOffset(int64_t offset_ms) { _wallclock_offset_ms = offset_ms; }

	bool OnSegmentCreated(const std::shared_ptr<base::modules::Segment> &segment);
	bool OnSegmentDeleted(const std::shared_ptr<base::modules::Segment> &segment);

	ov::String GetVariantName() const { return _variant_name; }
	ov::String GetPlaylistFileName() const { return _playlist_file_name; }

	bool HasVideo() const;
	bool HasAudio() const;
	bool HasSubtitle() const;
	std::shared_ptr<const MediaTrack> GetSubtitleTrack() const;

	uint32_t GetBitrates() const;
	uint32_t GetAverageBitrate() const;
	// EXT-X-STREAM-INF BANDWIDTH (peak). The last-second measurement is only
	// meaningful while packets flow (0 in SegmentCache idle, huge during a
	// demux burst), so never advertise less than AVERAGE-BANDWIDTH.
	uint32_t GetBandwidth() const;
	bool GetResolution(uint32_t &width, uint32_t &height) const;
	// <width>x<height>
	ov::String GetResolutionString() const;
	double GetFramerate() const;
	ov::String GetCodecsString() const;

	ov::String ToString(bool rewind) const;
	ov::String MakeSegmentString(const std::shared_ptr<base::modules::Segment> &segment) const;
	std::shared_ptr<base::modules::Segment> GetLatestSegment() const;

	void SetEndList();

	std::size_t GetSegmentCount() const;
	bool HasSegment(int64_t number) const;
	void ClearSegments();

	// Replace the live window for SegmentCache idle serve. Sets
	// EXT-X-DISCONTINUITY-SEQUENCE accounting from disc_sequence_before_first
	// (wraps that fully precede the first listed segment) instead of wiping it.
	void ReplaceIdleWindow(const std::vector<std::shared_ptr<base::modules::Segment>> &segments,
						   int64_t disc_sequence_before_first);

	// Floor for EXT-X-TARGETDURATION (seconds). Only ever raised, so the value
	// stays stable across item changes. SegmentCache segments are cut by the
	// cache plan, not by this publisher's SegmentDuration.
	void RaiseTargetDuration(size_t seconds);

private:
	// Recompute the cached CODECS union. Caller must hold _segments_mutex exclusively.
	void RebuildCodecsParameter();

	// EXT-X-TARGETDURATION: max of config, RaiseTargetDuration() floor and the
	// rounded EXTINF of retained segments. Caller must hold _segments_mutex.
	size_t GetTargetDurationLocked() const;

	HlsMediaPlaylistConfig _config;
	std::atomic<size_t> _min_target_duration{0};
	ov::String _variant_name;
	ov::String _playlist_file_name;

	// Media track ID : Media track
	// Protected by _tracks_mutex because a runtime track change (worker thread)
	// updates them while the master playlist reads them (HTTP thread)
	std::map<uint32_t, std::shared_ptr<const MediaTrack>> _media_tracks;
	std::shared_ptr<const MediaTrack> _first_video_track = nullptr;
	std::shared_ptr<const MediaTrack> _first_audio_track = nullptr;
	std::shared_ptr<const MediaTrack> _subtitle_track = nullptr;	// The subtitle track is used alone in MediaPlaylist
	mutable std::shared_mutex _tracks_mutex;

	std::atomic<bool> _excluded { false };

	// Segment number : Segment
	std::map<uint64_t, std::shared_ptr<base::modules::Segment>> _segments;
	mutable std::shared_mutex _segments_mutex;
	// Discontinuity accounting for EXT-X-DISCONTINUITY-SEQUENCE, guarded by _segments_mutex
	int64_t _total_discontinuity_count = 0;		// discontinuities ever added to this playlist
	int64_t _removed_discontinuity_count = 0;	// discontinuities that have scrolled out entirely
	// Cached CODECS union of the retained segments, rebuilt on add/remove, guarded by _segments_mutex
	ov::String _codecs_parameter;
	int64_t _wallclock_offset_ms = INT64_MIN;

	bool _end_list = false;
};