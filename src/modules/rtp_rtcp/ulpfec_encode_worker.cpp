#include "ulpfec_encode_worker.h"

#include <pthread.h>

#include <base/ovlibrary/ovlibrary.h>

#define OV_LOG_TAG "RtpRtcp"

UlpfecEncodeWorker::UlpfecEncodeWorker(const std::string &thread_name)
{
	_thread = std::thread(&UlpfecEncodeWorker::Run, this, thread_name);
}

UlpfecEncodeWorker::~UlpfecEncodeWorker()
{
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_stop = true;
	}
	_cv.notify_one();

	if (_thread.joinable())
	{
		_thread.join();
	}
}

void UlpfecEncodeWorker::Submit(uint32_t timestamp, uint32_t track_id, std::vector<std::shared_ptr<RedRtpPacket>> &&media)
{
	if (media.empty())
	{
		return;
	}

	uint64_t dropped = 0;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (_jobs.size() >= kMaxPendingJobs)
		{
			_jobs.pop_front();
			dropped = ++_dropped;
		}
		_jobs.push_back(Job{timestamp, track_id, std::move(media)});
	}
	_cv.notify_one();

	// Powers of two: visible when it starts, quiet when it persists.
	if (dropped != 0 && (dropped & (dropped - 1)) == 0)
	{
		logtw("ULPFEC worker behind on track %u: dropped parity for %" PRIu64 " frame(s) so far", track_id, dropped);
	}
}

std::vector<UlpfecEncodeWorker::Result> UlpfecEncodeWorker::TakeResults()
{
	std::lock_guard<std::mutex> lock(_mutex);
	std::vector<Result> out(std::make_move_iterator(_ready.begin()), std::make_move_iterator(_ready.end()));
	_ready.clear();
	return out;
}

bool UlpfecEncodeWorker::WaitIdle(std::chrono::microseconds timeout)
{
	std::unique_lock<std::mutex> lock(_mutex);
	return _idle_cv.wait_for(lock, timeout, [this] { return _jobs.empty() && _busy == false; });
}

uint64_t UlpfecEncodeWorker::GetDroppedCount() const
{
	std::lock_guard<std::mutex> lock(_mutex);
	return _dropped;
}

void UlpfecEncodeWorker::Run(std::string thread_name)
{
	ov::logger::ThreadHelper thread_helper;
	// pthread names are limited to 15 characters + NUL.
	::pthread_setname_np(pthread_self(), thread_name.substr(0, 15).c_str());

	while (true)
	{
		Job job;
		{
			std::unique_lock<std::mutex> lock(_mutex);
			_cv.wait(lock, [this] { return _stop || _jobs.empty() == false; });
			if (_stop)
			{
				return;
			}
			job = std::move(_jobs.front());
			_jobs.pop_front();
			_busy = true;
		}

		auto generator = std::make_shared<UlpfecGenerator>();
		generator->AdoptMediaPackets(std::move(job.media));
		generator->EncodePending();

		std::lock_guard<std::mutex> lock(_mutex);
		if (_ready.size() >= kMaxReadyResults)
		{
			// The packetizer has not drained for a while (stream idle or stalled);
			// parity for the oldest frame is stale by now.
			_ready.pop_front();
			_dropped++;
		}
		_ready.push_back(Result{job.timestamp, job.track_id, std::move(generator)});
		_busy = false;
		_idle_cv.notify_all();
	}
}
