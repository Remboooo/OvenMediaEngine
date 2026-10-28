//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================
#include <config/config.h>
#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "monitoring.h"
#include "monitoring_private.h"

//  Covers the session counts `Monitoring` keeps per stream, application, host and server,
//  when sessions connect and disconnect around the deletion of their stream.
//
//  A publisher reports its sessions against the output stream. The input stream is deleted first,
//  and its publishers keep reporting until the output stream is deleted too.
//  Every session must be subtracted exactly once, or the counts drift away from zero for good.

namespace
{
	class TestApplicationInfo : public info::Application
	{
	public:
		TestApplicationInfo(const info::Host &host_info, const info::VHostAppName &vhost_app_name)
			: info::Application(host_info, 1, vhost_app_name, cfg::vhost::app::Application(), false)
		{
		}
	};

	class MonitoringSessionCount : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			server_config = std::make_shared<cfg::Server>();
			MonitorInstance->OnServerStarted(server_config);

			host_info		 = std::make_shared<info::Host>("test-server", "test-server-id", cfg::vhost::VirtualHost());
			application_info = std::make_shared<TestApplicationInfo>(*host_info, info::VHostAppName("default", "app"));

			ASSERT_TRUE(MonitorInstance->OnHostCreated(*host_info));
			ASSERT_TRUE(MonitorInstance->OnApplicationCreated(*application_info));

			input = std::make_shared<info::Stream>(*application_info, 100, StreamSourceType::Rtmp);
			input->SetName("stream");

			output = std::make_shared<info::Stream>(*application_info, 101, StreamSourceType::Transcoder);
			output->SetName("stream");
			output->LinkInputStream(input);

			ASSERT_TRUE(MonitorInstance->OnStreamCreated(*input));
			ASSERT_TRUE(MonitorInstance->OnStreamCreated(*output));
		}

		void TearDown() override
		{
			MonitorInstance->Release();
		}

		uint64_t AppConnections(PublisherType type)
		{
			return MonitorInstance->GetApplicationMetrics(*application_info)->GetConnections(type);
		}

		uint64_t ServerConnections(PublisherType type)
		{
			return MonitorInstance->GetServerMetrics()->GetConnections(type);
		}

		uint64_t InputConnections(PublisherType type)
		{
			auto metrics = MonitorInstance->GetStreamMetrics(*input);
			return (metrics != nullptr) ? metrics->GetConnections(type) : 0;
		}

		std::shared_ptr<cfg::Server> server_config;
		std::shared_ptr<info::Host> host_info;
		std::shared_ptr<TestApplicationInfo> application_info;
		std::shared_ptr<info::Stream> input;
		std::shared_ptr<info::Stream> output;
	};
}  // namespace

TEST_F(MonitoringSessionCount, SessionsOnTheOutputStreamCountOnEveryLevel)
{
	MonitorInstance->OnSessionConnected(*output, PublisherType::Webrtc);
	MonitorInstance->OnSessionConnected(*output, PublisherType::Webrtc);
	MonitorInstance->OnSessionConnected(*output, PublisherType::LLHls);

	EXPECT_EQ(AppConnections(PublisherType::Webrtc), 2u);
	EXPECT_EQ(AppConnections(PublisherType::LLHls), 1u);
	EXPECT_EQ(ServerConnections(PublisherType::Webrtc), 2u);
	EXPECT_EQ(InputConnections(PublisherType::Webrtc), 2u);

	MonitorInstance->OnSessionDisconnected(*output, PublisherType::Webrtc);
	MonitorInstance->OnSessionsDisconnected(*output, PublisherType::LLHls, 1);

	EXPECT_EQ(AppConnections(PublisherType::Webrtc), 1u);
	EXPECT_EQ(AppConnections(PublisherType::LLHls), 0u);
	EXPECT_EQ(InputConnections(PublisherType::Webrtc), 1u);
}

TEST_F(MonitoringSessionCount, DeletingTheStreamSubtractsItsSessionsOnce)
{
	MonitorInstance->OnSessionConnected(*output, PublisherType::Webrtc);
	MonitorInstance->OnSessionConnected(*output, PublisherType::Webrtc);

	ASSERT_TRUE(MonitorInstance->OnStreamDeleted(*input));
	ASSERT_TRUE(MonitorInstance->OnStreamDeleted(*output));

	EXPECT_EQ(AppConnections(PublisherType::Webrtc), 0u);
	EXPECT_EQ(ServerConnections(PublisherType::Webrtc), 0u);
}

TEST_F(MonitoringSessionCount, SessionLeavingAfterItsInputStreamIsDeletedIsNotSubtractedAgain)
{
	MonitorInstance->OnSessionConnected(*output, PublisherType::Webrtc);
	MonitorInstance->OnSessionConnected(*output, PublisherType::Webrtc);

	ASSERT_TRUE(MonitorInstance->OnStreamDeleted(*input));
	EXPECT_EQ(AppConnections(PublisherType::Webrtc), 0u);

	// The publisher stops its sessions only when the output stream goes
	MonitorInstance->OnSessionDisconnected(*output, PublisherType::Webrtc);
	MonitorInstance->OnSessionDisconnected(*output, PublisherType::Webrtc);
	ASSERT_TRUE(MonitorInstance->OnStreamDeleted(*output));

	EXPECT_EQ(AppConnections(PublisherType::Webrtc), 0u);
	EXPECT_EQ(ServerConnections(PublisherType::Webrtc), 0u);
}

TEST_F(MonitoringSessionCount, SessionJoiningAfterItsInputStreamIsDeletedIsNotCounted)
{
	ASSERT_TRUE(MonitorInstance->OnStreamDeleted(*input));

	// An LL-HLS request still finds the output stream, and is never subtracted otherwise
	MonitorInstance->OnSessionConnected(*output, PublisherType::LLHls);
	EXPECT_EQ(AppConnections(PublisherType::LLHls), 0u);

	ASSERT_TRUE(MonitorInstance->OnStreamDeleted(*output));
	MonitorInstance->OnSessionsDisconnected(*output, PublisherType::LLHls, 1);

	EXPECT_EQ(AppConnections(PublisherType::LLHls), 0u);
	EXPECT_EQ(ServerConnections(PublisherType::LLHls), 0u);
}

TEST_F(MonitoringSessionCount, DeletingOnlyTheOutputStreamSubtractsItsSessionsFromTheInputStream)
{
	MonitorInstance->OnSessionConnected(*output, PublisherType::Webrtc);

	ASSERT_TRUE(MonitorInstance->OnStreamDeleted(*output));
	MonitorInstance->OnSessionDisconnected(*output, PublisherType::Webrtc);

	EXPECT_EQ(AppConnections(PublisherType::Webrtc), 0u);
	EXPECT_EQ(InputConnections(PublisherType::Webrtc), 0u);
}

TEST_F(MonitoringSessionCount, SessionsChurningWhileTheStreamIsDeletedEndAtZero)
{
	constexpr int THREADS	  = 4;
	constexpr int ROUNDS	  = 20000;
	constexpr int MAX_OPEN	  = 4;

	std::atomic<int> rounds_done{0};
	std::vector<std::thread> threads;

	for (int i = 0; i < THREADS; i++)
	{
		threads.emplace_back([&]() {
			// Each thread keeps a few sessions open, so some are open when the stream goes
			int open = 0;

			for (int round = 0; round < ROUNDS; round++)
			{
				MonitorInstance->OnSessionConnected(*output, PublisherType::Webrtc);
				open++;

				if (open > ((round % MAX_OPEN) + 1))
				{
					MonitorInstance->OnSessionDisconnected(*output, PublisherType::Webrtc);
					open--;
				}

				rounds_done++;
			}

			for (; open > 0; open--)
			{
				MonitorInstance->OnSessionDisconnected(*output, PublisherType::Webrtc);
			}
		});
	}

	while (rounds_done.load() < (THREADS * ROUNDS) / 4)
	{
		std::this_thread::yield();
	}

	ASSERT_TRUE(MonitorInstance->OnStreamDeleted(*input));
	ASSERT_TRUE(MonitorInstance->OnStreamDeleted(*output));

	for (auto &thread : threads)
	{
		thread.join();
	}

	EXPECT_EQ(AppConnections(PublisherType::Webrtc), 0u);
	EXPECT_EQ(ServerConnections(PublisherType::Webrtc), 0u);
}
