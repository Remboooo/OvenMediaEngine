//==============================================================================
//
//  OvenMediaEngine - Unit Tests
//
//==============================================================================
#include <gtest/gtest.h>

#include "pace_group_map.h"

TEST(PaceGroupMap, VideoOwnsOwnGroup)
{
	auto map = BuildPaceGroupMap({1, 2}, {}, {});
	EXPECT_EQ(map[1], 1u);
	EXPECT_EQ(map[2], 2u);
}

TEST(PaceGroupMap, AudioJoinsUniquelyPairedVideo)
{
	// bypass(1)+aac(10), 1080(2)+opus(20) — each audio unique to one video
	std::vector<std::pair<std::optional<uint32_t>, std::optional<uint32_t>>> renditions = {
		{1u, 10u},
		{2u, 20u},
	};
	auto map = BuildPaceGroupMap({1, 2}, {10, 20}, renditions);
	EXPECT_EQ(map[1], 1u);
	EXPECT_EQ(map[10], 1u);
	EXPECT_EQ(map[2], 2u);
	EXPECT_EQ(map[20], 2u);
}

TEST(PaceGroupMap, SharedAudioGetsOwnGroup)
{
	// opus(20) shared by 1080/720/480 — must not couple video rungs
	std::vector<std::pair<std::optional<uint32_t>, std::optional<uint32_t>>> renditions = {
		{1u, 10u},	 // bypass + aac
		{2u, 20u},	 // 1080 + opus
		{3u, 20u},	 // 720 + opus
		{4u, 20u},	 // 480 + opus
	};
	auto map = BuildPaceGroupMap({1, 2, 3, 4}, {10, 20}, renditions);
	EXPECT_EQ(map[1], 1u);
	EXPECT_EQ(map[10], 1u);
	EXPECT_EQ(map[2], 2u);
	EXPECT_EQ(map[3], 3u);
	EXPECT_EQ(map[4], 4u);
	EXPECT_EQ(map[20], 20u);
}

TEST(PaceGroupMap, AudioOnlyRenditionOwnsOwnGroup)
{
	std::vector<std::pair<std::optional<uint32_t>, std::optional<uint32_t>>> renditions = {
		{std::nullopt, 10u},
	};
	auto map = BuildPaceGroupMap({}, {10}, renditions);
	EXPECT_EQ(map[10], 10u);
}

TEST(PaceGroupMap, UnreferencedAudioOwnsOwnGroup)
{
	// Audio present as a track but not in any rendition
	auto map = BuildPaceGroupMap({1}, {10}, {{1u, std::nullopt}});
	EXPECT_EQ(map[1], 1u);
	EXPECT_EQ(map[10], 10u);
}
