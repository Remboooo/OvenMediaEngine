//==============================================================================
//
//  ScheduledStream
//
//  Created by Getroot
//  Copyright (c) 2023 AirenSoft. All rights reserved.
//
//==============================================================================

#include "scheduled_stream.h"

#include "schedule_private.h"

#include <base/provider/application.h>
#include <modules/segment_cache/mp4_sample_indexer.h>
#include <modules/segment_cache/options.h>
#include <modules/segment_cache/source_session.h>

#include <cstdlib>
#include <cstring>
#include <cmath>
#include <strings.h>
#include <thread>

namespace pvd
{
    // Implementation of ScheduledStream
    std::shared_ptr<ScheduledStream> ScheduledStream::Create(const std::shared_ptr<Application> &application, const info::Stream &stream_info, const std::shared_ptr<Schedule> &schedule)
    {
        auto stream = std::make_shared<pvd::ScheduledStream>(application, stream_info, schedule);
        return stream;
    }

    ScheduledStream::ScheduledStream(const std::shared_ptr<Application> &application, const info::Stream &info, const std::shared_ptr<Schedule> &schedule)
        : Stream(application, info), _schedule(schedule), _channel_info(schedule->GetStream())
    {
    }

    ScheduledStream::~ScheduledStream()
    {
        Stop();
    }

    bool ScheduledStream::Start()
    {
        // Create Worker
        _worker_thread_running.store(true);
        _worker_thread = std::thread(&ScheduledStream::WorkerThread, this);
        pthread_setname_np(_worker_thread.native_handle(), "Scheduled");

        return Stream::Start();
    }

    bool ScheduledStream::Stop()
    {
        if (_worker_thread_running.load() == false)
        {
            return true;
        }

        _worker_thread_running.store(false);
        _schedule_updated.SetEvent();

        if (_worker_thread.joinable())
        {
            _worker_thread.join();
        }

		segment_cache::SessionRegistry::GetInstance().Remove(GetName());

        return Stream::Stop();
    }

    bool ScheduledStream::Terminate()
    {
        return Stream::Terminate();
    }

    std::shared_ptr<const Schedule> ScheduledStream::PeekSchedule() const
    {
        std::shared_lock<std::shared_mutex> lock(_schedule_mutex);
        return _schedule;
    }

    std::shared_ptr<Schedule> ScheduledStream::GetSchedule() const
    {
        std::shared_lock<std::shared_mutex> lock(_schedule_mutex);
        _schedule_updated.Reset();
        return _schedule;
    }

    bool ScheduledStream::GetCurrentProgram(std::shared_ptr<Schedule::Program> &curr_program, std::shared_ptr<Schedule::Item> &curr_item, int64_t &curr_item_pos) const
    {
        std::shared_lock<std::shared_mutex> lock(_current_mutex);
        curr_program = _current_program;
        curr_item = _current_item;
        curr_item_pos = _current_item_position_ms;

        return true;
    }

    bool ScheduledStream::IsMaxFallbackDurationExceeded() const
    {
        auto schedule = PeekSchedule();
        if (schedule == nullptr)
        {
            return false;
        }

        auto max_fallback_duration_ms = schedule->GetStream()._max_fallback_duration_ms;
        if (max_fallback_duration_ms <= 0)
        {
            return false;
        }

        auto fallback_entered_at_ms = _fallback_entered_at_ms.load();
        if (fallback_entered_at_ms == -1)
        {
            return false;
        }

        return (ov::Time::GetMonotonicTimestamp() - fallback_entered_at_ms) >= max_fallback_duration_ms;
    }

    uint32_t ScheduledStream::GetPumpDemandCount() const
    {
		auto self = const_cast<ScheduledStream *>(this)->GetSharedPtrAs<pvd::Stream>();
		if (self == nullptr)
		{
			return 0;
		}

		auto info = std::static_pointer_cast<const info::Stream>(self);
		auto orch = ocst::Orchestrator::GetInstance();
		uint32_t count = 0;

		for (auto type : {PublisherType::Webrtc, PublisherType::Ovt})
		{
			auto pub_stream = orch->GetPublisherStream(type, info);
			if (pub_stream != nullptr)
			{
				count += pub_stream->GetSessionCount();
			}
		}

		return count;
    }

    int64_t ScheduledStream::GetPumpGracePeriodMs() const
    {
		// Env overrides Server.xml for tests / local enable.
		const char *env = ::getenv("OME_SEGMENT_CACHE_PUMP_GRACE_MS");
		if (env != nullptr && env[0] != '\0')
		{
			char *end = nullptr;
			const long long value = ::strtoll(env, &end, 10);
			if (end != env && value >= 0 && value <= 600000)
			{
				return static_cast<int64_t>(value);
			}
		}

		return ResolveSegmentCacheOptions().idle_grace_period_ms;
    }

	void ScheduledStream::AnchorCachePlayhead(int64_t elapsed_ms)
	{
		_cache_playhead_anchor_mono_ms = ov::Time::GetMonotonicTimestamp();
		_cache_playhead_anchor_elapsed_ms = std::max<int64_t>(0, elapsed_ms);
	}

	void ScheduledStream::TickCachePlayhead(const std::shared_ptr<segment_cache::SourceSession> &session,
											int64_t item_duration_ms)
	{
		if (session == nullptr || _cache_playhead_anchor_mono_ms < 0)
		{
			return;
		}

		const int64_t now = ov::Time::GetMonotonicTimestamp();
		// Keep absolute elapsed (may exceed one loop). ResolvePlayhead computes
		// loop_count / in-loop position — do not modulo here or MSN never wraps.
		int64_t elapsed_ms =
			_cache_playhead_anchor_elapsed_ms + (now - _cache_playhead_anchor_mono_ms);
		if (elapsed_ms < 0)
		{
			elapsed_ms = 0;
		}

		int64_t position_ms = elapsed_ms;
		if (item_duration_ms > 0)
		{
			position_ms = elapsed_ms % item_duration_ms;
		}

		{
			std::lock_guard<std::shared_mutex> lock(_current_mutex);
			_current_item_position_ms = static_cast<double>(position_ms);
		}
		session->SetElapsedMs(elapsed_ms);
		session->MaybeWarmPlayheadWindow(24, 2000);
		NotifyCachePlayheadToPublishers();
	}

	void ScheduledStream::NotifyCachePlayheadToPublishers()
	{
		auto self = GetSharedPtrAs<pvd::Stream>();
		if (self == nullptr)
		{
			return;
		}
		auto info = std::static_pointer_cast<const info::Stream>(self);
		auto pub = ocst::Orchestrator::GetInstance()->GetPublisherStream(PublisherType::LLHls, info);
		if (pub != nullptr)
		{
			pub->OnSegmentCachePlayheadTick();
		}
	}

	segment_cache::Options ScheduledStream::ResolveSegmentCacheOptions() const
	{
		segment_cache::Options options;

		// Env master switch still enables without XML.
		const char *env = ::getenv("OME_SEGMENT_CACHE");
		const bool env_on = (env != nullptr) && (env[0] != '\0') &&
							(std::strcmp(env, "0") != 0) &&
							(strcasecmp(env, "false") != 0) &&
							(strcasecmp(env, "off") != 0);

		cfg::vhost::app::pvd::SegmentCache cfg_cache;
		if (GetApplication() != nullptr)
		{
			cfg_cache = GetApplication()->GetConfig().GetProviders().GetScheduledProvider().GetSegmentCache();
		}

		const bool xml_on = cfg_cache.IsEnable();
		const bool stream_on = _channel_info._segment_cache_enable.value_or(xml_on || env_on);

		if (stream_on == false)
		{
			return options;
		}

		options.mode = cfg_cache.IsPersist() ? segment_cache::CacheMode::Persist
											: segment_cache::CacheMode::Memory;
		// Env without XML defaults to persist.
		if (xml_on == false && env_on)
		{
			options.mode = segment_cache::CacheMode::Persist;
		}

		options.idle_grace_period_ms = cfg_cache.GetIdleGracePeriodMs();
		options.hls_lookahead_segments =
			static_cast<size_t>(std::max(0, cfg_cache.GetHlsLookaheadSegments()));

		const auto &hydrate = cfg_cache.GetHydrate();
		options.hydrate.mode = hydrate.IsGreedy() ? segment_cache::HydrateMode::Greedy
												  : segment_cache::HydrateMode::Lazy;
		options.hydrate.max_threads = static_cast<uint32_t>(std::max(1, hydrate.GetMaxThreads()));
		options.hydrate.max_throughput_mbps = hydrate.GetMaxThroughputMbps();

		// Env hydrate override for tests / local enable.
		const char *hydrate_env = ::getenv("OME_SEGMENT_CACHE_HYDRATE");
		if (hydrate_env != nullptr && hydrate_env[0] != '\0')
		{
			if (std::strcmp(hydrate_env, "greedy") == 0 || std::strcmp(hydrate_env, "1") == 0 ||
				strcasecmp(hydrate_env, "true") == 0)
			{
				options.hydrate.mode = segment_cache::HydrateMode::Greedy;
			}
			else if (std::strcmp(hydrate_env, "lazy") == 0)
			{
				options.hydrate.mode = segment_cache::HydrateMode::Lazy;
			}
		}

		return options;
	}

	bool ScheduledStream::AreEgressPublishersStarted() const
	{
		auto self = const_cast<ScheduledStream *>(this)->GetSharedPtrAs<pvd::Stream>();
		if (self == nullptr)
		{
			return false;
		}

		auto info = std::static_pointer_cast<const info::Stream>(self);
		auto orch = ocst::Orchestrator::GetInstance();

		// At least one configured egress publisher must have left CREATED.
		// Until then we must keep demuxing so MediaRouter can prepare tracks.
		bool any_publisher = false;
		for (auto type : {PublisherType::LLHls, PublisherType::Hls, PublisherType::Webrtc, PublisherType::Ovt})
		{
			auto pub_stream = orch->GetPublisherStream(type, info);
			if (pub_stream == nullptr)
			{
				continue;
			}
			any_publisher = true;
			if (pub_stream->GetState() != pub::Stream::State::STARTED &&
				pub_stream->GetState() != pub::Stream::State::STOPPED)
			{
				return false;
			}
		}

		// No publishers attached yet — keep pumping briefly rather than idling blind.
		return any_publisher;
	}

    bool ScheduledStream::ShouldRunMediaPump() const
    {
		if (_channel_info._bypass_transcoder == false)
		{
			return true;
		}
		auto session = segment_cache::SessionRegistry::GetInstance().Find(GetName());
		if (session == nullptr)
		{
			return true;
		}

		// Hold demux until the cache session matches the file currently playing
		// (async Open may finish for a different item, or still be indexing).
		{
			std::shared_lock<std::shared_mutex> lock(_current_mutex);
			if (_current_item == nullptr || _current_item->_file_path.IsEmpty() ||
				session->GetSourcePath() != _current_item->_file_path)
			{
				return true;
			}
		}

		// Never idle before egress publishers have started — otherwise LLHLS/WebRTC
		// stay forever in "created but not started" (no MediaRouter prepare).
		if (AreEgressPublishersStarted() == false)
		{
			return true;
		}

		if (GetPumpDemandCount() > 0)
		{
			_pump_demand_zero_since_ms.store(-1);
			return true;
		}

		// No WebRTC/OVT demand. Enter idle immediately unless the demux pump is
		// already running — then wait out the grace period to avoid thrashing.
		if (_media_pump_running.load() == false)
		{
			return false;
		}

		const int64_t grace_ms = GetPumpGracePeriodMs();
		if (grace_ms <= 0)
		{
			return false;
		}

		const int64_t now = ov::Time::GetMonotonicTimestamp();
		int64_t zero_since = _pump_demand_zero_since_ms.load();
		if (zero_since < 0)
		{
			_pump_demand_zero_since_ms.store(now);
			return true;
		}

		return (now - zero_since) < grace_ms;
    }

    ScheduledStream::PlaybackResult ScheduledStream::PlayFileIdle(const std::shared_ptr<Schedule::Item> &item, bool fallback_item)
    {
		_media_pump_running.store(false);
		_pump_demand_zero_since_ms.store(-1);
		segment_cache::SessionRegistry::GetInstance().SetCacheServeEnabled(GetName(), true);

		logti("Scheduled Channel : %s/%s: Idle cache playback for %s (no demux)",
			  GetApplicationName(), GetName().CStr(), item->_file_path.CStr());

		PlaybackResult result = PlaybackResult::PLAY_NEXT_ITEM;
		auto session = segment_cache::SessionRegistry::GetInstance().Find(GetName());
		if (session == nullptr)
		{
			segment_cache::SessionRegistry::GetInstance().SetCacheServeEnabled(GetName(), false);
			return PlaybackResult::ERROR;
		}

		const int64_t item_duration_ms = session->GetItemDurationMs();
		const int64_t start_offset_ms = std::max<int64_t>(0, static_cast<int64_t>(item->_start_time_ms));
		// Resume from the later of schedule join and any already-ticking playhead
		// so idle re-entry after WebRTC pump does not rewind MSN.
		int64_t resume_ms = start_offset_ms;
		if (session->GetElapsedMs() > resume_ms)
		{
			resume_ms = session->GetElapsedMs();
		}
		const int64_t entered_at = ov::Time::GetMonotonicTimestamp();
		AnchorCachePlayhead(resume_ms);
		session->SetElapsedMs(resume_ms);
		session->WarmPlayheadWindow(24);

		while (_worker_thread_running.load())
		{
			if (CheckCurrentProgramChanged() == true)
			{
				result = PlaybackResult::PLAY_NEXT_PROGRAM;
				break;
			}

			if (fallback_item)
			{
				if (CheckCurrentItemAvailable() == true)
				{
					result = PlaybackResult::FAILBACK;
					break;
				}
			}

			if (ShouldRunMediaPump() == true)
			{
				logti("Scheduled Channel : %s/%s: Pump demand appeared — leaving idle mode",
					  GetApplicationName(), GetName().CStr());
				// Keep cache-serve enabled so HLS/LLHLS stay on the segment cache
				// while demux runs only for WebRTC/OVT. Packager playlist writes are
				// suppressed while cache-serve is on.
				const int64_t elapsed = session->GetElapsedMs();
				// Freeze wall playhead at the seek point so demux bursts cannot jump MSN.
				AnchorCachePlayhead(elapsed);
				if (item->LoadContext() != nullptr && elapsed > 0)
				{
					auto *ctx = item->LoadContext().get();
					int video_stream = -1;
					for (unsigned i = 0; i < ctx->nb_streams; i++)
					{
						if (ctx->streams[i]->codecpar != nullptr &&
							ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
						{
							video_stream = static_cast<int>(i);
							break;
						}
					}

					// Seek to the preceding keyframe so WebRTC/LLHLS resume with
					// a decodable video frame (audio alone would otherwise continue).
					if (video_stream >= 0)
					{
						const AVRational ms_tb{1, 1000};
						const int64_t ts = av_rescale_q(elapsed, ms_tb, ctx->streams[video_stream]->time_base);
						if (::av_seek_frame(ctx, video_stream, ts, AVSEEK_FLAG_BACKWARD) < 0)
						{
							const int64_t seek_target = elapsed * 1000;
							::avformat_seek_file(ctx, -1, 0, seek_target,
												 (item_duration_ms > 0 ? item_duration_ms : elapsed) * 1000, 0);
						}
					}
					else
					{
						const int64_t seek_target = elapsed * 1000;
						::avformat_seek_file(ctx, -1, 0, seek_target,
											 (item_duration_ms > 0 ? item_duration_ms : elapsed) * 1000, 0);
					}
				}
				return PlaybackResult::RESUME_PUMP;
			}

			// Absolute elapsed for ResolvePlayhead (loop_count); UI uses in-loop position.
			const int64_t wall_elapsed = (ov::Time::GetMonotonicTimestamp() - entered_at) + resume_ms;
			int64_t position_ms = wall_elapsed;
			if (item_duration_ms > 0)
			{
				position_ms = wall_elapsed % item_duration_ms;
			}

			{
				std::lock_guard<std::shared_mutex> lock(_current_mutex);
				_current_item_position_ms = static_cast<double>(position_ms);
			}
			session->SetElapsedMs(wall_elapsed);
			// Do not WarmPlayheadWindow here: with no viewers, rematerialize would
			// keep media disk I/O alive. Hydrate + on-demand GET / demux Tick warm.
			NotifyCachePlayheadToPublishers();

			if (item->_duration_ms > 0 && (wall_elapsed - start_offset_ms) >= item->_duration_ms)
			{
				result = PlaybackResult::PLAY_NEXT_ITEM;
				break;
			}

			_schedule_updated.Wait(200);
		}

		segment_cache::SessionRegistry::GetInstance().SetCacheServeEnabled(GetName(), false);
		return result;
    }

    bool ScheduledStream::UpdateSchedule(const std::shared_ptr<Schedule> &schedule)
    {
        std::lock_guard<std::shared_mutex> lock(_schedule_mutex);

        // If MaxFallbackDurationMs is changed while in fallback, count from now
        if (_schedule != nullptr && schedule != nullptr &&
            _schedule->GetStream()._max_fallback_duration_ms != schedule->GetStream()._max_fallback_duration_ms &&
            _fallback_entered_at_ms.load() != -1)
        {
            _fallback_entered_at_ms.store(ov::Time::GetMonotonicTimestamp());
        }

        _schedule = schedule;

        _schedule_updated.SetEvent();
        return true;
    }

    bool ScheduledStream::CheckCurrentProgramChanged()
    {
        // Check if pointer is same
        std::shared_lock<std::shared_mutex> lock(_current_mutex);

        if (GetSchedule() == nullptr)
        {
            return true;
        }

        if (_current_program == GetSchedule()->GetCurrentProgram())
        {
            return false;
        }

        if (_current_program == nullptr && GetSchedule()->GetCurrentProgram() != nullptr)
        {
            return true;
        }
        else if (_current_program != nullptr && GetSchedule()->GetCurrentProgram() == nullptr)
        {
            return true;
        }

        if (*_current_program == *GetSchedule()->GetCurrentProgram())
        {
            return false;
        }

        return true;
    }

    bool ScheduledStream::CheckCurrentFallbackProgramChanged()
    {
        // Check if pointer is same
        std::shared_lock<std::shared_mutex> lock(_current_mutex);

        if (GetSchedule() == nullptr)
        {
            return true;
        }

        if (_fallback_program == GetSchedule()->GetFallbackProgram())
        {
            return false;
        }

        if (_fallback_program == nullptr && GetSchedule()->GetFallbackProgram() != nullptr)
        {
            return true;
        }
        else if (_fallback_program != nullptr && GetSchedule()->GetFallbackProgram() == nullptr)
        {
            return true;
        }

        // even if the pointer is changed, but the program is the same, it is not changed
        if (*_fallback_program == *GetSchedule()->GetFallbackProgram())
        {
            return false;
        }

        return true;
    }

    bool ScheduledStream::CheckCurrentItemAvailable(bool immediate)
    {
        if (_current_item == nullptr)
        {
            return false;
        }

		if (immediate == false)
		{
			// check every 1 seconds
			if (_failback_check_clock.IsStart() == false)
			{
				_failback_check_clock.Start();
			}

			if (_failback_check_clock.Elapsed() < 1000)
			{
				return false;
			}

			_failback_check_clock.Stop();
		}

        if (_current_item->_file == true)
        {
            return CheckFileItemAvailable(_current_item);
        }
        else
        {
            return CheckStreamItemAvailable(_current_item);
        }

        return false;
    }

	bool ScheduledStream::SetDurationToAllItems(const std::shared_ptr<Schedule::Program> &program)
	{
		if (program == nullptr)
		{
			return false;
		}

		int64_t total_duration_ms = 0;
		for (auto &item : program->_items)
		{
			if (item == nullptr)
			{
				logtw("Scheduled Channel %s/%s: Item is null in program %s", GetApplicationName(), GetName().CStr(), program->_name.CStr());
				continue;
			}

			if (item->_duration_ms > 0)
			{
				// logti("Scheduled Channel %s/%s: Item %s duration is already set to %" PRId64 " ms", GetApplicationName(), GetName().CStr(), item->url.CStr(), item->duration_ms);
				// already set
				total_duration_ms += item->_duration_ms;
				continue;
			}

			if (item->_file == true)
			{
				item->_duration_ms = GetFileItemDurationMS(item) - item->_start_time_ms_conf;
				total_duration_ms += item->_duration_ms;
			}
			else
			{
				// Live with no duration means it will be played until the end of the program
				item->_duration_ms = -1;
				program->_unlimited_duration = true;
			}

			logti("Scheduled Channel %s/%s: Item %s duration set to %" PRId64 " ms", GetApplicationName(), GetName().CStr(), item->_url.CStr(), item->_duration_ms);
		}

		program->_total_item_duration_ms = total_duration_ms;

		logti("Total item duration ms : %" PRId64 " ms", program->_total_item_duration_ms);

		return true;
	}

    void ScheduledStream::WorkerThread()
    {
		ov::logger::ThreadHelper thread_helper;

        while (_worker_thread_running.load())
        {
            // Schedule
            std::unique_lock<std::shared_mutex> guard(_current_mutex);
            _current_schedule = GetSchedule();
            _current_program = nullptr;
            _current_item = nullptr;
            _current_item_position_ms = 0;
            guard.unlock();

            if (_current_schedule == nullptr)
            {
                _realtime_clock.Pause();
                // Wait for schedule update
                _schedule_updated.Wait();
                continue;
            }

            // Programs
            guard.lock();
            _current_program = _current_schedule->GetCurrentProgram();
            _fallback_program = _current_schedule->GetFallbackProgram();
            guard.unlock();
            if (_current_program == nullptr)
            {
                PlayFallbackOrWait();
                continue;
            }
			else
			{
				SetDurationToAllItems(_current_program);
			}

            logti("Scheduled Channel %s/%s: Start %s program", GetApplicationName(), GetName().CStr(), _current_program->_name.CStr());

            // Items
			bool first_item = true;
            while (_worker_thread_running.load())
            {
                guard.lock();
                _current_item = nullptr;
                guard.unlock();
                
                if (CheckCurrentProgramChanged() == true)
                {
                    logti("Scheduled Channel %s/%s: Program changed", GetApplicationName(), GetName().CStr());
                    break;
                }

                guard.lock();
				if (first_item == true)
				{
					_current_item = _current_program->GetFirstItemWithPosition();
				}
                else
				{
					_current_item = _current_program->GetNextItem();
				}
                guard.unlock();

                if (_current_item == nullptr)
                {
                    logti("Scheduled Channel %s/%s: Program ended", GetApplicationName(), GetName().CStr());
                    break;
                }

				first_item = false;
				bool exit_loop = false;
                while (_worker_thread_running.load())
                {
                    auto result = PlayItem(_current_item);
                    if (result == PlaybackResult::ERROR)
                    {
                        if (_current_item->_fallback_on_err == true)
                        {
							auto fallback_result = PlayFallbackOrWait();
                            if (fallback_result == PlaybackResult::FAILBACK)
                            {
                                continue;
                            }
							else if (CheckCurrentFallbackProgramChanged() == true)
							{
								// Fallback program changed, should reset to _fallback_program
								exit_loop = true;
							}
                        }
                    }

					break;
                }

				if (exit_loop == true)
				{
					break;
				}
            }
        }
    }

    ScheduledStream::PlaybackResult ScheduledStream::PlayFallbackOrWait()
    {
        logti("Scheduled Channel %s/%s: Start fallback program", GetApplicationName(), GetName().CStr());

        // Keeps running until a scheduled item actually plays again
        if (_fallback_entered_at_ms.load() == -1)
        {
            _fallback_entered_at_ms.store(ov::Time::GetMonotonicTimestamp());
        }

        PlaybackResult result = PlaybackResult::PLAY_NEXT_ITEM;

        // Fallback
		bool fisrt = true;
        while (_worker_thread_running.load())
        {
            if (CheckCurrentProgramChanged() == true)
            {
                logti("Scheduled Channel %s/%s: Program changed", GetApplicationName(), GetName().CStr());
                break;
            }

            if (CheckCurrentFallbackProgramChanged() == true)
            {
                logti("Scheduled Channel %s/%s: Fallback program changed", GetApplicationName(), GetName().CStr());
                break;
            }

            if (_fallback_program == nullptr || _fallback_program->_items.empty() == true)
            {
                if (CheckCurrentItemAvailable(true) == true)
                {
                    return PlaybackResult::FAILBACK;
                }

                _realtime_clock.Pause();
                _schedule_updated.Wait(1000);

                continue;
            }

			std::shared_ptr<Schedule::Item> item = nullptr;
			if (fisrt == true)
			{
				item = _fallback_program->GetItem(0);
				fisrt = false;
			}
			else
			{
				item = _fallback_program->GetNextItem();
			}

            result = PlayItem(item, true);
            if (result == PlaybackResult::FAILBACK)
            {
                break;
            }
            else if (result == PlaybackResult::PLAY_NEXT_ITEM)
            {
                // Next fallback item
            }
            else if (result == PlaybackResult::PLAY_NEXT_PROGRAM)
            {
                break;
            }
            else if (result == PlaybackResult::ERROR)
            {
				logte("Scheduled Channel %s/%s: Playback error in fallback program. Try to play next item", GetApplicationName(), GetName().CStr());

				if (CheckCurrentItemAvailable(true) == true)
                {
                    return PlaybackResult::FAILBACK;
                }

                _realtime_clock.Pause();
                _schedule_updated.Wait(250);

                continue;
			}
			else
			{
				logtc("Scheduled Channel %s/%s: Unknown playback result %d", GetApplicationName(), GetName().CStr(), static_cast<int>(result));
				break;
            }
        }

        return result;
    }

    ScheduledStream::PlaybackResult ScheduledStream::PlayItem(const std::shared_ptr<Schedule::Item> &item, bool fallback_item)
    {
        if (item == nullptr)
        {
            return PlaybackResult::ERROR;
        }

        if (item->_file == true)
        {
            return PlayFile(item, fallback_item);
        }
        
        return PlayStream(item, fallback_item);
    }

    ScheduledStream::PlaybackResult ScheduledStream::PlayFile(const std::shared_ptr<Schedule::Item> &item, bool fallback_item)
    {
        logti("Scheduled Channel : %s/%s: Play file %s", GetApplicationName(), GetName().CStr(), item->_file_path.CStr());

        ScheduledStream::PlaybackResult result = PlaybackResult::PLAY_NEXT_ITEM;

		auto context = item->LoadContext();
		if (context == nullptr)
		{
			logte("Scheduled Channel : %s/%s: Format context is null. Try to play next item", GetApplicationName(), GetName().CStr());
			return PlaybackResult::ERROR;
		}

		if (PrepareFilePlayback(item) == false)
        {
            logte("Scheduled Channel : %s/%s: Failed to prepare file playback. Try to play next item", GetApplicationName(), GetName().CStr());
            return PlaybackResult::ERROR;
        }

		// Skip demux when cache can serve and no WebRTC/OVT demand.
		// RESUME_PUMP falls through into the demux loop without re-preparing.
		if (ShouldRunMediaPump() == false)
		{
			auto idle_result = PlayFileIdle(item, fallback_item);
			if (idle_result != PlaybackResult::RESUME_PUMP)
			{
				return idle_result;
			}
			logti("Scheduled Channel : %s/%s: Resuming demux after idle for %s",
				  GetApplicationName(), GetName().CStr(), item->_file_path.CStr());
		}

        if (_realtime_clock.IsStart() == false)
        {
            _realtime_clock.Start();
        }

        if (_realtime_clock.IsPaused() == true)
        {
            _realtime_clock.Resume();
        }

        // Play
        AVPacket packet = { 0 };
        std::map<int, bool> track_first_packet_map;
        std::map<int, int64_t> track_single_file_dts_offset_map;
        std::map<int, bool> end_of_track_map;

        bool is_mpegts { std::strncmp(context->iformat->name, "mpegts", 6) == 0 };

		_media_pump_running.store(true);
		// Keep / enable cache-serve for HLS/LLHLS even when demux runs for WebRTC.
		// Playhead was anchored in PrepareFilePlayback when SegmentCache is on.
		if (auto session = segment_cache::SessionRegistry::GetInstance().Find(GetName()))
		{
			segment_cache::SessionRegistry::GetInstance().SetCacheServeEnabled(GetName(), true);
			if (_cache_playhead_anchor_mono_ms < 0)
			{
				const int64_t elapsed = session->GetElapsedMs() > 0
											? session->GetElapsedMs()
											: std::max<int64_t>(0, static_cast<int64_t>(item->_start_time_ms));
				AnchorCachePlayhead(elapsed);
				session->SetElapsedMs(elapsed);
			}
		}

        while (_worker_thread_running.load())
        {
            if (CheckCurrentProgramChanged() == true)
            {
                result = PlaybackResult::PLAY_NEXT_PROGRAM;
                break;
            }

            if (fallback_item)
            {
                if (CheckCurrentItemAvailable() == true)
                {
                    result = PlaybackResult::FAILBACK;
                    break;
                }
            }

			// Drop to idle after grace when WebRTC/OVT demand clears.
			if (ShouldRunMediaPump() == false)
			{
				logti("Scheduled Channel : %s/%s: Pump demand cleared — entering idle for %s",
					  GetApplicationName(), GetName().CStr(), item->_file_path.CStr());
				auto idle_result = PlayFileIdle(item, fallback_item);
				if (idle_result != PlaybackResult::RESUME_PUMP)
				{
					result = idle_result;
					break;
				}
				_media_pump_running.store(true);
				continue;
			}

            int32_t ret = ::av_read_frame(context.get(), &packet);
            if (ret == AVERROR(EAGAIN))
            {
                logtw("Scheduled Channel : %s/%s: Failed to read frame. Error (%d, %s)", GetApplicationName(), GetName().CStr(), ret, "EAGAIN");
                continue;
            }
            else if (ret == AVERROR_EOF || ::avio_feof(context->pb))
            {
                // End of file
                logti("Scheduled Channel : %s/%s: End of file. Try to play next item", GetApplicationName(), GetName().CStr());
                result = PlaybackResult::PLAY_NEXT_ITEM;
                break;
            }
            else if (ret < 0)
            {
                char errbuf[AV_ERROR_MAX_STRING_SIZE] = { 0 };

                ::av_strerror(ret, errbuf, sizeof(errbuf));

                logte("%s/%s: Failed to read frame. Error (%d, %s). Try to play next item", GetApplicationName(), GetName().CStr(), ret, errbuf);

                result = PlaybackResult::PLAY_NEXT_ITEM;
                break;
            }

            auto track_id = FindTrackIdByOriginId(packet.stream_index);
            if (track_id < 0)
            {
                logtt("Scheduled Channel : %s/%s: Failed to find track %d", GetApplicationName(), GetName().CStr(), packet.stream_index);
                ::av_packet_unref(&packet);
                continue;
            }

            if (end_of_track_map.find(track_id) != end_of_track_map.end())
            {
                // End of track
                if (end_of_track_map.at(track_id) == true)
                {
                    ::av_packet_unref(&packet);
                    continue;
                }
            }
            else
            {
                end_of_track_map[track_id] = false;
            }

            auto track = GetTrack(track_id);
            if (track == nullptr)
            {
                logtw("Scheduled Channel : %s/%s: Failed to find track %d", GetApplicationName(), GetName().CStr(), track_id);
                ::av_packet_unref(&packet);
                continue;
            }

            cmn::BitstreamFormat bitstream_format = cmn::BitstreamFormat::Unknown;
			cmn::PacketType packet_type = cmn::PacketType::Unknown;
			switch (track->GetCodecId())
			{
				case cmn::MediaCodecId::H264:
					bitstream_format = (is_mpegts) ? cmn::BitstreamFormat::H264_ANNEXB : cmn::BitstreamFormat::H264_AVCC;
					packet_type = cmn::PacketType::NALU;
					break;
				case cmn::MediaCodecId::H265:
					bitstream_format = (is_mpegts) ? cmn::BitstreamFormat::H265_ANNEXB : cmn::BitstreamFormat::HVCC;
					packet_type = cmn::PacketType::NALU;
					break;
				case cmn::MediaCodecId::Aac:
					bitstream_format = (is_mpegts) ? cmn::BitstreamFormat::AAC_ADTS : cmn::BitstreamFormat::AAC_RAW;
					packet_type = cmn::PacketType::RAW;
					break;
				case cmn::MediaCodecId::Opus:
					bitstream_format = cmn::BitstreamFormat::OPUS;
					packet_type = cmn::PacketType::RAW;
					break;
                case cmn::MediaCodecId::Mp3:
                    bitstream_format = cmn::BitstreamFormat::MP3;
                    packet_type = cmn::PacketType::RAW;
                    break;
				default:
                    logtw("Scheduled Channel : %s/%s: Unsupported codec %s", GetApplicationName(), GetName().CStr(), cmn::GetCodecIdString(track->GetCodecId()));
					::av_packet_unref(&packet);
					continue;
			}

			auto media_packet = ffmpeg::compat::ToMediaPacket(track->GetId(), &packet, track->GetMediaType(), bitstream_format, packet_type);

            // Convert to fixed time base
            auto origin_tb = context->streams[packet.stream_index]->time_base;

            ::av_packet_unref(&packet);

            auto pts = media_packet->GetPts();
            auto dts = media_packet->GetDts();
            auto duration = media_packet->GetDuration();
			
            // origin timebase to track timebase
            // pts = static_cast<double>(pts) * (static_cast<double>(origin_tb.num) / static_cast<double>(origin_tb.den) * track->GetTimeBase().GetTimescale());
            // dts = static_cast<double>(dts) * (static_cast<double>(origin_tb.num) / static_cast<double>(origin_tb.den) * track->GetTimeBase().GetTimescale());
            // duration = static_cast<double>(duration) * (static_cast<double>(origin_tb.num) / static_cast<double>(origin_tb.den) * track->GetTimeBase().GetTimescale());

			pts = Rescale(pts, track->GetTimeBase().GetDen() * origin_tb.num, origin_tb.den * track->GetTimeBase().GetNum());
			dts = Rescale(dts, track->GetTimeBase().GetDen() * origin_tb.num, origin_tb.den * track->GetTimeBase().GetNum());
			duration = Rescale(duration, track->GetTimeBase().GetDen() * origin_tb.num, origin_tb.den * track->GetTimeBase().GetNum());

            if (track_first_packet_map.find(track_id) == track_first_packet_map.end())
            {
                track_first_packet_map[track_id] = true;
                track_single_file_dts_offset_map[track_id] = dts;
            }
            auto single_file_dts = dts - track_single_file_dts_offset_map[track_id];
           
            AdjustTimestampByBase(track_id, pts, dts, std::numeric_limits<int64_t>::max(), duration);
			logtt("Scheduled Channel Send Packet : %s/%s: Track %d, origin dts : %" PRId64 ", pts %" PRId64 ", dts %" PRId64 ", duration %" PRId64 ", tb %f", GetApplicationName(), GetName().CStr(), track_id, single_file_dts, pts, dts, duration, track->GetTimeBase().GetExpr());

			int64_t dts_us = Rescale(dts, 1000000 * track->GetTimeBase().GetNum(), track->GetTimeBase().GetDen());
			if (_global_track_offset_us_map.find(track_id) == _global_track_offset_us_map.end())
			{
				_global_track_offset_us_map[track_id] = dts_us;
			}

			int64_t global_zero_based_dts = dts_us - _global_track_offset_us_map[track_id];

            media_packet->SetPts(pts);
            media_packet->SetDts(dts);
            media_packet->SetDuration(-1); // Duration will be calculated in MediaRouter

			double time_ms = static_cast<double>(dts) * track->GetTimeBase().GetExpr() * 1000.0;

            int64_t dts_gap = 0;
            if (_last_packet_map.find(track_id) != _last_packet_map.end())
            {
                auto last_packet = _last_packet_map.at(track_id);
                dts_gap = media_packet->GetDts() - last_packet->GetDts();
            }

            logtt("Scheduled Channel Send Packet : %s/%s: Track %d, origin dts : %" PRId64 ", pts %" PRId64 ", dts %" PRId64 ", duration %" PRId64 ", tb %f, dts_ms %f, dts_gap %" PRId64 "", GetApplicationName(), GetName().CStr(), track_id, single_file_dts, pts, dts, duration, track->GetTimeBase().GetExpr(), time_ms, dts_gap);

            SendFrame(media_packet);

            if (fallback_item == false)
            {
                // A scheduled item is playing, fallback is over
                _fallback_entered_at_ms.store(-1);
            }

            _last_packet_map[track_id] = media_packet;

            // dts to real time (ms)
            auto single_file_dts_ms = static_cast<double>(single_file_dts) * track->GetTimeBase().GetExpr() * 1000.0;
			auto single_file_duration_ms = single_file_dts_ms + static_cast<double>(duration) * track->GetTimeBase().GetExpr() * 1000.0;

			std::unique_lock<std::shared_mutex> lock(_current_mutex);
            _current_item_position_ms = single_file_duration_ms;
            lock.unlock();

			if (auto session = segment_cache::SessionRegistry::GetInstance().Find(GetName()))
			{
				if (segment_cache::SessionRegistry::GetInstance().IsCacheServeEnabled(GetName()))
				{
					// Wall-clock playhead only — demux DTS can burst ahead after seek
					// and would otherwise yank HLS/LLHLS MEDIA-SEQUENCE by minutes.
					TickCachePlayhead(session, session->GetItemDurationMs());
				}
				else
				{
					session->SetElapsedMs(static_cast<int64_t>(single_file_duration_ms));
				}
			}

             // Get current play time
            if (item->_duration_ms >= 0)
            {
                if (single_file_duration_ms >= item->_duration_ms)
                {
                    end_of_track_map[track_id] = true;
                }

                // all tracks should be ended
                bool all_tracks_ended = true;
                for (auto &end_of_track : end_of_track_map)
                {
                    if (end_of_track.second == false)
                    {
                        all_tracks_ended = false;
                        break;
                    }
                }

                if (all_tracks_ended == true)
                {
                    // End of item
                    logti("Scheduled Channel : %s/%s: End of item (Current Pos : %.0f ms Duration : %" PRId64 " ms). Try to play next item", GetApplicationName(), GetName().CStr(), single_file_duration_ms, item->_duration_ms);
                    result = PlaybackResult::PLAY_NEXT_ITEM;
                    break;
                }
            }
            
            int64_t elapsed = _realtime_clock.ElapsedUs();
            if (elapsed < global_zero_based_dts)
            {
                int64_t wait_time = global_zero_based_dts - elapsed;
                // logti("Scheduled Channel : %s/%s: Track(%d) Current(%" PRId64 ") DTS(%" PRId64 ") Wait(%" PRId64 ")", GetApplicationName(), GetName().CStr(), track_id, elapsed, global_zero_based_dts, wait_time);
                std::this_thread::sleep_for(std::chrono::microseconds(wait_time));
            }
        }

        _current_item_position_ms = 0;

		_media_pump_running.store(false);

        logti("Scheduled Channel : %s/%s: Playback stopped", GetApplicationName(), GetName().CStr());

        return result;
    }

    bool ScheduledStream::CheckFileItemAvailable(const std::shared_ptr<Schedule::Item> &item)
    {
        if (item == nullptr || item->_file == false || item->_file_path.IsEmpty())
		{
			return false;
		}

		if (item->LoadContext() == nullptr)
		{
			return false;
		}

        bool video_track_needed = _channel_info._video_track;
        bool audio_track_needed = _channel_info._audio_track;

        for (uint32_t track_id = 0; track_id < item->LoadContext()->nb_streams; track_id++)
        {
            if (video_track_needed == false && audio_track_needed == false)
            {
                break;
            }

            auto stream = item->LoadContext()->streams[track_id];
            if (stream == nullptr)
            {
                continue;
            }

            if (ffmpeg::compat::ToMediaType(stream->codecpar->codec_type) == cmn::MediaType::Video &&
                video_track_needed == true)
            {
                video_track_needed = false;
            }
            else if (ffmpeg::compat::ToMediaType(stream->codecpar->codec_type) == cmn::MediaType::Audio &&
                audio_track_needed == true)
            {
                audio_track_needed = false;
            }
            else
            {
                continue;
            }
        }

        if (video_track_needed == true || audio_track_needed == true)
        {
            logte("%s/%s: Failed to find %s track(s) from file %s", GetApplicationName(), GetName().CStr(), 
                video_track_needed&& audio_track_needed == true ? "video and audio" :
                video_track_needed == true ? "video" : "audio", item->_file_path.CStr());
            return false;
        }

        return true;
    }

	int64_t ScheduledStream::GetFileItemDurationMS(const std::shared_ptr<Schedule::Item> &item) const
	{
		if (item == nullptr || item->_file == false || item->_file_path.IsEmpty())
		{
			return 0;
		}

		auto format_context = item->LoadContext();
		if (format_context == nullptr)
		{
			logte("%s/%s: Failed to load format context for file %s", GetApplicationName(), GetName().CStr(), item->_file_path.CStr());
			return 0;
		}

		int64_t duration_ms = format_context->duration / AV_TIME_BASE * 1000;

		return duration_ms;
	}

    bool ScheduledStream::PrepareFilePlayback(const std::shared_ptr<Schedule::Item> &item)
    {
		if (item == nullptr || item->LoadContext() == nullptr)
		{
			logte("%s/%s: Format context is null for file %s", GetApplicationName(), GetName().CStr(), item->_file_path.CStr());
			return false;
		}

        bool video_track_needed = _channel_info._video_track;
        bool audio_track_needed = _channel_info._audio_track;

		uint32_t audio_index = 0;
        int64_t total_duration_ms = 0;
        _origin_id_track_id_map.clear();
        for (uint32_t track_id = 0; track_id < item->LoadContext()->nb_streams; track_id++)
        {
            if (video_track_needed == false && audio_track_needed == false)
            {
                break;
            }

            auto stream = item->LoadContext()->streams[track_id];
            if (stream == nullptr)
            {
                continue;
            }

            if (ffmpeg::compat::ToMediaType(stream->codecpar->codec_type) == cmn::MediaType::Video &&
                video_track_needed == true)
            {
                auto new_track = ffmpeg::compat::CreateMediaTrack(stream);
                if (new_track == nullptr)
                {
                    continue;
                }

				auto old_track = GetTrack(kScheduledVideoTrackId);

                new_track->SetId(kScheduledVideoTrackId);
                new_track->SetTimeBase(1, kScheduledVideoTimebase);
				new_track->SetPublicName(old_track->GetPublicName());

				if (ChangeTrack(new_track) == false)
				{
					logte("%s/%s: Video track of item %s was rejected",
						  GetApplicationName(), GetName().CStr(), item->_file_path.CStr());
					return false;
				}

                _origin_id_track_id_map.emplace(stream->index, kScheduledVideoTrackId);

                if (total_duration_ms == 0)
                {
                    total_duration_ms = stream->duration * 1000 * ::av_q2d(stream->time_base);
                }
                else
                {  
                    total_duration_ms = std::min(total_duration_ms, (int64_t)(stream->duration * 1000 * ::av_q2d(stream->time_base)));
                }

                video_track_needed = false;
            }
            else if (ffmpeg::compat::ToMediaType(stream->codecpar->codec_type) == cmn::MediaType::Audio &&
                audio_track_needed == true)
            {
                auto new_track = ffmpeg::compat::CreateMediaTrack(stream);
                if (new_track == nullptr)
                {
                    continue;
                }

				auto audio_track_id = kScheduledAudioTrackId + audio_index;
				audio_index++;
				auto old_track = GetTrack(audio_track_id);

                new_track->SetId(audio_track_id);
                new_track->SetTimeBase(1, kScheduledAudioTimebase);
				new_track->SetPublicName(old_track->GetPublicName());
				new_track->SetLanguage(old_track->GetLanguage());
				new_track->SetCharacteristics(old_track->GetCharacteristics());

				if (ChangeTrack(new_track) == false)
				{
					logte("%s/%s: Audio track of item %s was rejected",
						  GetApplicationName(), GetName().CStr(), item->_file_path.CStr());
					return false;
				}

                _origin_id_track_id_map.emplace(stream->index, audio_track_id);

                if (total_duration_ms == 0)
                {
                    total_duration_ms = stream->duration * 1000 * ::av_q2d(stream->time_base);
                }
                else
                {  
                    total_duration_ms = std::min(total_duration_ms, (int64_t)(stream->duration * 1000 * ::av_q2d(stream->time_base)));
                }
				
				if (audio_index + 1 > _channel_info._audio_map.size())
				{
                	audio_track_needed = false;
				}
            }
            else
            {
                continue;
            }
        }

        if (video_track_needed == true || audio_track_needed == true)
        {
            logte("%s/%s: Failed to find %s track(s) from file %s", GetApplicationName(), GetName().CStr(), 
                video_track_needed && audio_track_needed == true ? "video and audio" :
                video_track_needed == true ? "video" : "audio", item->_file_path.CStr());
            return false;
        }

        // If there is no data track, add a dummy data track
		if(GetFirstTrackByType(cmn::MediaType::Data) == nullptr)
		{
			auto data_track = std::make_shared<MediaTrack>();
			data_track->SetId(kScheduledDataTrackId);
			data_track->SetMediaType(cmn::MediaType::Data);
			data_track->SetTimeBase(1, 1000); // Data track time base is always 1/1000 in
			data_track->SetOriginBitstream(cmn::BitstreamFormat::Unknown);

			AddTrack(data_track);
		}

        if (item->_duration_ms == 0)
        {
            item->_duration_ms = total_duration_ms;
        }

        // Seek to start position
        if (item->_start_time_ms >= 0)
        {
			// if stream_index is -1, in AV_TIME_BASE units
			// #define AV_TIME_BASE 1000000
            int64_t seek_target = item->_start_time_ms * 1000; // Convert to microseconds
            int64_t seek_min = 0;
            int64_t seek_max = total_duration_ms * 1000;

            int seek_ret = ::avformat_seek_file(item->LoadContext().get(), -1, seek_min, seek_target, seek_max, 0);
            if (seek_ret < 0)
            {
				logte("%s/%s: Failed to seek to start position %" PRId64 ", err:%d", GetApplicationName(), GetName().CStr(), item->_start_time_ms, seek_ret);
            }
        }

		logti("Scheduled Channel : %s/%s: File %s prepared. Start time %" PRId64 " ms, Duration %" PRId64 " ms",
			GetApplicationName(), GetName().CStr(), item->_file_path.CStr(), item->_start_time_ms, item->_duration_ms);

		// Publish track updates before (possibly slow) index build so MediaRouter
		// can prepare as soon as demux starts.
		OnSourceChanged();

		// Segment cache (Server.xml Providers/Schedule/SegmentCache, or OME_SEGMENT_CACHE=1).
		// Unsupported media / non-bypass → warn and fall back to demux (do not fail the item).
		const auto cache_options = ResolveSegmentCacheOptions();
		if (cache_options.IsEnabled() &&
			_channel_info._bypass_transcoder &&
			_channel_info._video_track &&
			item->_file)
		{
			const bool require_audio = _channel_info._audio_track;
			auto probe = segment_cache::Mp4SampleIndexer::Probe(item->_file_path, require_audio);
			if (probe.ok == false)
			{
				logtw("Scheduled Channel : %s/%s: SegmentCache skipped for %s — %s "
					  "(falling back to demux playback)",
					  GetApplicationName(), GetName().CStr(), item->_file_path.CStr(),
					  probe.error.CStr());
				segment_cache::SessionRegistry::GetInstance().SetCacheServeEnabled(GetName(), false);
				segment_cache::SessionRegistry::GetInstance().Remove(GetName());
			}
			else
			{
				segment_cache::PackagerFingerprint fp;
				fp.format_id = 2;  // plan parts for LLHLS; HLS TS still uses segment bounds
				fp.target_duration_ms = 2000;
				fp.chunk_duration_ms = 500;

				if (GetApplication() != nullptr)
				{
					const auto &pubs = GetApplication()->GetConfig().GetPublishers();
					if (pubs.GetLLHlsPublisher().IsParsed())
					{
						const auto &llhls = pubs.GetLLHlsPublisher();
						fp.target_duration_ms = static_cast<uint32_t>(std::lround(llhls.GetSegmentDuration() * 1000.0));
						fp.chunk_duration_ms = static_cast<uint32_t>(std::lround(llhls.GetChunkDuration() * 1000.0));
					}
					else if (pubs.GetHlsPublisher().IsParsed())
					{
						const auto &hls = pubs.GetHlsPublisher();
						fp.format_id = 1;
						fp.target_duration_ms = static_cast<uint32_t>(std::lround(hls.GetSegmentDuration() * 1000.0));
						fp.chunk_duration_ms = 0;
					}
				}

				if (const char *ms = std::getenv("OME_SEGMENT_CACHE_TARGET_MS"))
				{
					const int parsed = ::atoi(ms);
					if (parsed > 0)
					{
						fp.target_duration_ms = static_cast<uint32_t>(parsed);
					}
				}
				if (const char *ms = std::getenv("OME_SEGMENT_CACHE_CHUNK_MS"))
				{
					const int parsed = ::atoi(ms);
					if (parsed > 0)
					{
						fp.chunk_duration_ms = static_cast<uint32_t>(parsed);
					}
				}

				if (fp.target_duration_ms == 0)
				{
					fp.target_duration_ms = 2000;
				}

				// Index/sidecar Open can take a long time (esp. first hydrate). Do it off
				// the worker so demux starts immediately and publishers can leave CREATED.
				const ov::String stream_name = GetName();
				const ov::String app_name = GetApplicationName();
				const ov::String path = item->_file_path;
				const bool persist = cache_options.PersistSidecar();
				const auto hydrate = cache_options.hydrate;
				const double cap_mbps = cache_options.hydrate.max_throughput_mbps;

				logti("Scheduled Channel : %s/%s: SegmentCache indexing %s in background "
					  "(mode=%s, hydrate=%s, cap=%.1f Mbps)",
					  app_name.CStr(), stream_name.CStr(), path.CStr(),
					  persist ? "persist" : "memory",
					  hydrate.IsGreedy() ? "greedy" : "lazy",
					  cap_mbps);

				// Suppress packager playlist MSNs immediately. Otherwise demux (e.g. WebRTC
				// demand at startup) advertises MEDIA-SEQUENCE from 0, then cache-serve
				// rewrites to file-position MSNs (~join offset) and players skip hundreds
				// of segments. Until Open finishes, SyncIdle is a no-op (empty playlist).
				const int64_t start_ms = std::max<int64_t>(0, static_cast<int64_t>(item->_start_time_ms));
				AnchorCachePlayhead(start_ms);
				segment_cache::SessionRegistry::GetInstance().SetCacheServeEnabled(stream_name, true);

				std::thread([stream_name, app_name, path, fp, persist, hydrate, cap_mbps, start_ms]() {
					auto session = segment_cache::SourceSession::Open(
						path, fp, {}, persist, cap_mbps);
					if (session != nullptr)
					{
						session->SetElapsedMs(start_ms);
						segment_cache::SessionRegistry::GetInstance().Put(stream_name, session);
						session->WarmPlayheadWindow(24);
						session->StartGreedyHydrateAsync(hydrate);
						logti("Scheduled Channel : %s/%s: SegmentCache ready (%s, %zu segments, mode=%s, hydrate=%s, cap=%.1f Mbps)",
							  app_name.CStr(), stream_name.CStr(), path.CStr(),
							  session->GetPlan().segments.size(),
							  persist ? "persist" : "memory",
							  hydrate.IsGreedy() ? "greedy" : "lazy",
							  cap_mbps);
					}
					else
					{
						segment_cache::SessionRegistry::GetInstance().SetCacheServeEnabled(stream_name, false);
						logtw("Scheduled Channel : %s/%s: SegmentCache failed to open for %s "
							  "(need H264+AAC MP4) — falling back to demux; check SegmentCache.* logs",
							  app_name.CStr(), stream_name.CStr(), path.CStr());
					}
				}).detach();
			}
		}
		else if (cache_options.IsEnabled() && item->_file &&
				 _channel_info._bypass_transcoder == false)
		{
			logtw("Scheduled Channel : %s/%s: SegmentCache enabled but BypassTranscoder=false — "
				  "skipping cache for this item (demux/transcode path)",
				  GetApplicationName(), GetName().CStr());
		}
		else if (cache_options.IsEnabled() && item->_file &&
				 _channel_info._video_track == false)
		{
			logtw("Scheduled Channel : %s/%s: SegmentCache enabled but VideoTrack=false — "
				  "skipping cache for this item",
				  GetApplicationName(), GetName().CStr());
		}

        return true;
    }

    ScheduledStream::PlaybackResult ScheduledStream::PlayStream(const std::shared_ptr<Schedule::Item> &item, bool fallback_item)
    {
        logti("Scheduled Channel : %s/%s: Play stream %s", GetApplicationName(), GetName().CStr(), item->_url.CStr());

        ScheduledStream::PlaybackResult result = PlaybackResult::PLAY_NEXT_ITEM;

        auto stream_tap = PrepareStreamPlayback(item);
        if (stream_tap == nullptr)
        {
            logte("Scheduled Channel : %s/%s: Failed to prepare stream playback. Try to play next item", GetApplicationName(), GetName().CStr());
            return PlaybackResult::ERROR;
        }

        if (_realtime_clock.IsStart() == false)
        {
            _realtime_clock.Start();
        }

        if (_realtime_clock.IsPaused() == true)
        {
            _realtime_clock.Resume();
        }

        std::map<int, bool> track_first_packet_map;
        std::map<int, int64_t> track_single_file_dts_offset_map;
        std::map<int, bool> end_of_track_map;

		// bool sent_keyframe = false;

        // Play
        while (_worker_thread_running.load())
        {
            if (CheckCurrentProgramChanged() == true)
            {
                result = PlaybackResult::PLAY_NEXT_PROGRAM;
                break;
            }

            if (fallback_item)
            {
                if (CheckCurrentItemAvailable() == true)
                {
                    result = PlaybackResult::FAILBACK;
                    break;
                }
            }
			
            auto media_packet = stream_tap->Pop(_channel_info._error_tolerance_duration_ms);
            if (media_packet == nullptr)
            {
                if (CheckCurrentProgramChanged() == true)
                {
                    result = PlaybackResult::PLAY_NEXT_PROGRAM;
                    break;
                }
                else
                {
                    if (stream_tap->GetState() == MediaRouterStreamTap::State::Tapped)
                    {
                    	logtw("Scheduled Channel : %s/%s: Failed to pop packet until %" PRId64 " ms. Try to play next item", GetApplicationName(), GetName().CStr(), _channel_info._error_tolerance_duration_ms);
                    }
					else
					{
						logtw("Scheduled Channel : %s/%s: Stream tap state is %d. Try to play next item", GetApplicationName(), GetName().CStr(), static_cast<int>(stream_tap->GetState()));
					}
                    result = PlaybackResult::ERROR;
                    break;
                }
            }
			
			auto origin_track_id = media_packet->GetTrackId();
            auto track_id = FindTrackIdByOriginId(origin_track_id);
            if (track_id < 0)
            {
                logtt("Scheduled Channel : %s/%s: Failed to find track %d", GetApplicationName(), GetName().CStr(), media_packet->GetTrackId());
                continue;
            }

            if (end_of_track_map.find(track_id) != end_of_track_map.end())
            {
                // End of track
                if (end_of_track_map.at(track_id) == true)
                {
                    continue;
                }
            }
            else
            {
                end_of_track_map[track_id] = false;
            }

            // Transcoder will make bogus frame if it can't be decoded
			// if (GetRepresentationType() == StreamRepresentationType::Relay && sent_keyframe == false)
            // {
			// 	if (media_packet->GetMediaType() == cmn::MediaType::Video)
			// 	{
			// 		if (media_packet->GetFlag() != MediaPacketFlag::Key)
			// 		{
			// 			// Skip until key frame
			// 			continue;
			// 		}
			// 		else
			// 		{
			// 			sent_keyframe = true;
			// 		}
			// 	}
			// 	else 
			// 	{
			// 		continue; // Skip until key frame
			// 	}
			// }

            auto track = GetTrack(track_id);
            if (track == nullptr)
            {
                logtw("Scheduled Channel : %s/%s: Failed to find track %d", GetApplicationName(), GetName().CStr(), track_id);
                continue;
            }

            auto origin_tb = stream_tap->GetStreamInfo()->GetTrack(origin_track_id)->GetTimeBase();

            media_packet->SetTrackId(track_id);
            auto pts = media_packet->GetPts();
            auto dts = media_packet->GetDts();
            auto duration = media_packet->GetDuration();

            // origin timebase to track timebase
            //pts = (((double)pts * (double)origin_tb.GetNum()) / (double)origin_tb.GetDen()) * track->GetTimeBase().GetTimescale();
            //dts = (((double)dts * (double)origin_tb.GetNum()) / (double)origin_tb.GetDen()) * track->GetTimeBase().GetTimescale();
			//duration = static_cast<double>(duration) * (static_cast<double>(origin_tb.GetNum()) / static_cast<double>(origin_tb.GetDen()) * track->GetTimeBase().GetTimescale());
			
			// origin timebase to track timebase
			pts = Rescale(pts, track->GetTimeBase().GetDen() * origin_tb.GetNum(), origin_tb.GetDen() * track->GetTimeBase().GetNum());
			dts = Rescale(dts, track->GetTimeBase().GetDen() * origin_tb.GetNum(), origin_tb.GetDen() * track->GetTimeBase().GetNum());
			duration = Rescale(duration, track->GetTimeBase().GetDen() * origin_tb.GetNum(), origin_tb.GetDen() * track->GetTimeBase().GetNum());

			logtt("Scheduled Channel : %s/%s: Track %d, origin dts : %" PRId64 ", pts %" PRId64 ", dts %" PRId64 ", duration %" PRId64 ", tb %f", GetApplicationName(), GetName().CStr(), track_id, dts, pts, dts, duration, track->GetTimeBase().GetExpr());

            if (track_first_packet_map.find(track_id) == track_first_packet_map.end())
            {
                track_first_packet_map[track_id] = true;
                track_single_file_dts_offset_map[track_id] = dts;
            }
            auto single_file_dts = dts - track_single_file_dts_offset_map[track_id];

            AdjustTimestampByBase(track_id, pts, dts, std::numeric_limits<int64_t>::max(), duration);

			int64_t dts_us = Rescale(dts, 1000000 * track->GetTimeBase().GetNum(), track->GetTimeBase().GetDen());
			if (_global_track_offset_us_map.find(track_id) == _global_track_offset_us_map.end())
			{
				_global_track_offset_us_map[track_id] = dts_us;
			}
            media_packet->SetPts(pts);
            media_packet->SetDts(dts);
			media_packet->SetDuration(-1); // It will be calculated in MediaRouter

			double time_ms = (double)(dts * 1000.0 * track->GetTimeBase().GetExpr());

            logtt("Scheduled Channel Send Packet : %s/%s: Track %d, origin dts : %" PRId64 ", pts %" PRId64 ", dts %" PRId64 ", tb %f, dts_ms %f", GetApplicationName(), GetName().CStr(), track_id, single_file_dts, pts, dts, track->GetTimeBase().GetExpr(), time_ms);

            SendFrame(media_packet);

            if (fallback_item == false)
            {
                // A scheduled item is playing, fallback is over
                _fallback_entered_at_ms.store(-1);
            }

            // dts to real time (ms)
            auto single_file_dts_ms = static_cast<double>(single_file_dts) * track->GetTimeBase().GetExpr() * static_cast<double>(1000);

            std::unique_lock<std::shared_mutex> lock(_current_mutex);
            _current_item_position_ms = single_file_dts_ms;
            lock.unlock();

            // Get current play time
            if (item->_duration_ms >= 0)
            {
                if (single_file_dts_ms > item->_duration_ms)
                {
                    end_of_track_map[track_id] = true;
                }

                // all tracks should be ended
                bool all_tracks_ended = true;
                for (auto &end_of_track : end_of_track_map)
                {
                    if (end_of_track.second == false)
                    {
                        all_tracks_ended = false;
                        break;
                    }
                }

                if (all_tracks_ended == true)
                {
                    // End of item
                    logti("Scheduled Channel : %s/%s: End of item (Current Pos : %.0f ms Duration : %" PRId64 " ms). Try to play next item", GetApplicationName(), GetName().CStr(), single_file_dts_ms, item->_duration_ms);
                    result = PlaybackResult::PLAY_NEXT_ITEM;
                    break;
                }
            }
        }

        _current_item_position_ms = 0;

        ocst::Orchestrator::GetInstance()->UnmirrorStream(stream_tap);

        logti("Scheduled Channel : %s/%s: Playback stopped", GetApplicationName(), GetName().CStr());

        return result;
    }

    bool ScheduledStream::CheckStreamItemAvailable(const std::shared_ptr<Schedule::Item> &item)
    {
        auto stream_url = ov::Url::Parse(item->_url);
        if (stream_url == nullptr)
        {
            logte("Scheduled Channel : %s/%s: Failed to parse stream url %s", GetApplicationName(), GetName().CStr(), item->_url.CStr());
            return false;
        }

        auto vhost_app_name = info::VHostAppName(stream_url->Host(), stream_url->App());
        if (ocst::Orchestrator::GetInstance()->CheckIfStreamExist(vhost_app_name, stream_url->Stream()) == false)
        {
            logte("Scheduled Channel : %s/%s: Failed to find stream %s", GetApplicationName(), GetName().CStr(), item->_url.CStr());
            return false;
        }

        auto stream_tap = MediaRouterStreamTap::Create();

        auto result = ocst::Orchestrator::GetInstance()->MirrorStream(stream_tap, vhost_app_name, stream_url->Stream(), MediaRouterInterface::MirrorPosition::Inbound);
        if (result != CommonErrorCode::SUCCESS)
        {
            logte("Scheduled Channel : %s/%s: Failed to mirror stream %s (err : %d)", GetApplicationName(), GetName().CStr(), item->_url.CStr(), static_cast<int>(result));
            return false;
        }

        if (stream_tap->GetStreamInfo() == nullptr)
        {
            logte("Scheduled Channel : %s/%s: Failed to get stream info from stream tap %s", GetApplicationName(), GetName().CStr(), item->_url.CStr());
            return false;
        }

        bool video_track_needed = _channel_info._video_track;
        bool audio_track_needed = _channel_info._audio_track;
        for (const auto &[track_id, track] : stream_tap->GetStreamInfo()->GetTracks())
        {
            if (video_track_needed == false && audio_track_needed == false)
            {
                break;
            }

            if (track->GetMediaType() == cmn::MediaType::Video &&
                video_track_needed == true)
            {
                video_track_needed = false;
            }
            else if (track->GetMediaType() == cmn::MediaType::Audio &&
                audio_track_needed == true)
            {
                audio_track_needed = false;
            }
            else
            {
                continue;
            }
        }

        if (video_track_needed == true || audio_track_needed == true)
        {
            logte("%s/%s: Failed to find %s track(s) from stream %s", GetApplicationName(), GetName().CStr(),
                video_track_needed&& audio_track_needed == true ? "video and audio" :
                video_track_needed == true ? "video" : "audio", item->_url.CStr());

            ocst::Orchestrator::GetInstance()->UnmirrorStream(stream_tap);
            
            return false;
        }

        ocst::Orchestrator::GetInstance()->UnmirrorStream(stream_tap);
        return true;
    }

    std::shared_ptr<MediaRouterStreamTap> ScheduledStream::PrepareStreamPlayback(const std::shared_ptr<Schedule::Item> &item)
    {
        auto stream_tap = MediaRouterStreamTap::Create();
		// This is commented out because it is likely to cause problems every time a live item is switched. It seems that various tests will be needed.
		// stream_tap->SetNeedPastData(true);

        auto stream_url = ov::Url::Parse(item->_url);
        if (stream_url == nullptr)
        {
            logte("Scheduled Channel : %s/%s: Failed to parse stream url %s", GetApplicationName(), GetName().CStr(), item->_url.CStr());
            return nullptr;
        }

        auto vhost_app_name = info::VHostAppName(stream_url->Host(), stream_url->App());

        auto result = ocst::Orchestrator::GetInstance()->MirrorStream(stream_tap, vhost_app_name, stream_url->Stream(), MediaRouterInterface::MirrorPosition::Inbound);
        if (result != CommonErrorCode::SUCCESS)
        {
            logte("Scheduled Channel : %s/%s: Failed to mirror stream %s (err : %d)", GetApplicationName(), GetName().CStr(), item->_url.CStr(), static_cast<int>(result));
            return nullptr;
        }

        if (stream_tap->GetStreamInfo() == nullptr)
        {
            logte("Scheduled Channel : %s/%s: Failed to get stream info from stream tap %s", GetApplicationName(), GetName().CStr(), item->_url.CStr());
            ocst::Orchestrator::GetInstance()->UnmirrorStream(stream_tap);
            return nullptr;
        }

        _origin_id_track_id_map.clear();
        bool video_track_needed = _channel_info._video_track;
        bool audio_track_needed = _channel_info._audio_track;
        bool forward_data_needed = item->_forward_data;
		uint32_t audio_index = 0;
        for (const auto &[track_id, track] : stream_tap->GetStreamInfo()->GetTracks())
        {
            if (video_track_needed == false && audio_track_needed == false && forward_data_needed == false)
            {
                break;
            }

            if (track->GetMediaType() == cmn::MediaType::Video &&
                video_track_needed == true)
            {
                auto new_track = std::make_shared<MediaTrack>(*track);
                if (new_track == nullptr)
                {
                    continue;
                }

				auto old_track = GetTrack(kScheduledVideoTrackId);

                new_track->SetId(kScheduledVideoTrackId);
                new_track->SetTimeBase(1, kScheduledVideoTimebase);
				new_track->SetPublicName(old_track->GetPublicName());

				if (ChangeTrack(new_track) == false)
				{
					logte("%s/%s: Video track of the tapped stream was rejected",
						  GetApplicationName(), GetName().CStr());
					return nullptr;
				}

                _origin_id_track_id_map.emplace(track_id, kScheduledVideoTrackId);

                video_track_needed = false;
            }
            else if (track->GetMediaType() == cmn::MediaType::Audio &&
                audio_track_needed == true)
            {
                auto new_track = std::make_shared<MediaTrack>(*track);
                if (new_track == nullptr)
                {
                    continue;
                }
				
				auto audio_track_id = kScheduledAudioTrackId + audio_index;
				audio_index++;
				auto old_track = GetTrack(audio_track_id);

                new_track->SetId(audio_track_id);
                new_track->SetTimeBase(1, kScheduledAudioTimebase);
				new_track->SetPublicName(old_track->GetPublicName());
				new_track->SetLanguage(old_track->GetLanguage());
				new_track->SetCharacteristics(old_track->GetCharacteristics());

				if (ChangeTrack(new_track) == false)
				{
					logte("%s/%s: Audio track of the tapped stream was rejected",
						  GetApplicationName(), GetName().CStr());
					return nullptr;
				}

                _origin_id_track_id_map.emplace(track_id, audio_track_id);
				
				if (audio_index + 1 > _channel_info._audio_map.size())
				{
                	audio_track_needed = false;
				}
            }
            else if (track->GetMediaType() == cmn::MediaType::Data &&
                forward_data_needed == true)
            {
                auto new_track = std::make_shared<MediaTrack>(*track);
                if (new_track == nullptr)
                {
                    continue;
                }

				auto old_track = GetTrack(kScheduledDataTrackId);
				if (old_track == nullptr)
				{
					continue;
				}

                new_track->SetId(kScheduledDataTrackId);
                new_track->SetTimeBase(1, 1000);
				new_track->SetPublicName(old_track->GetPublicName());
                _origin_id_track_id_map.emplace(track_id, kScheduledDataTrackId);
                UpdateTrack(new_track);

                forward_data_needed = false;
            }
            else
            {
                continue;
            }
        }

        if (video_track_needed == true || audio_track_needed == true)
        {
            logte("%s/%s: Failed to find %s track(s) from stream %s", GetApplicationName(), GetName().CStr(),
                video_track_needed&& audio_track_needed == true ? "video and audio" :
                video_track_needed == true ? "video" : "audio", item->_url.CStr());
            ocst::Orchestrator::GetInstance()->UnmirrorStream(stream_tap);
            return nullptr;
        }

        if (forward_data_needed == true)
        {
            logtw("%s/%s: Failed to find data track from stream %s. Data forwarding will be skipped.", GetApplicationName(), GetName().CStr(), item->_url.CStr());
        }

        OnSourceChanged();

        stream_tap->Start();

        return stream_tap;
    }

    int ScheduledStream::FindTrackIdByOriginId(int origin_id) const
    {
        auto it = _origin_id_track_id_map.find(origin_id);
        if (it == _origin_id_track_id_map.end())
        {
            return -1;
        }

        return it->second;
    }
}

