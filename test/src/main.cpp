#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "audiorw.hpp"
#include "doctest.h"

static const auto TEST_MP3 = std::filesystem::path{ASSETS_DIR} / "test.mp3";
static const auto TEST_WAV = std::filesystem::path{ASSETS_DIR} / "test.wav";

TEST_CASE("stream::item::from(mp3).get_header()") {
	auto in = audiorw::stream::item::from(TEST_MP3, audiorw::format_hint::try_mp3_only);
	in.get_header();
}

TEST_CASE("stream::item::from(wav).get_header()") {
	auto in = audiorw::stream::item::from(TEST_WAV, audiorw::format_hint::try_wav_only);
	const auto header = in.get_header();
	REQUIRE(header.format == audiorw::format::wav);
	REQUIRE(header.SR == 8000);
	REQUIRE(header.bit_depth == 32);
	REQUIRE(header.channel_count == 2);
}
