#pragma once

#include <ads.hpp>
#include <filesystem>
#include <fstream>
#include <miniaudio.h>

namespace audiorw {

enum class format_hint {
	// Audio format will be deduced by trying to read
	// the header as each supported type, starting
	// with the one specified
	try_flac_first,
	try_mp3_first,
	try_wav_first,
	try_wavpack_first,

	// Only the specified type will be tried.
	try_flac_only,
	try_mp3_only,
	try_wav_only,
	try_wavpack_only,
};

enum class format {
	flac,
	mp3,
	wav,
	wavpack
};

enum class storage_type {
	default_,
	int_,
	float_,
	normalized_float_,
};

struct header {
	audiorw::format format;
	ads::channel_count channel_count;
	ads::frame_count frame_count;
	int SR        = 44100;
	int bit_depth = 32;
};

using frames = ads::fully_dynamic<float>;

struct item {
	audiorw::header header;
	audiorw::frames frames;
};

namespace detail {

struct atomic_file_writer {
	atomic_file_writer(const std::filesystem::path& path);
	~atomic_file_writer();
	auto commit() -> void;
	auto stream() -> std::ofstream&;
private:
	std::filesystem::path path_;
	std::filesystem::path tmp_path_;
	std::ofstream file_;
	bool commit_flag_ = false;
};

struct scope_ma_encoder {
	scope_ma_encoder(ma_encoder_write_proc on_write, ma_encoder_seek_proc on_seek, void* user_data, const ma_encoder_config& config);
	~scope_ma_encoder();
	auto write_pcm_frames(const void* frames, ma_uint64 frame_count) -> ma_uint64;
private:
	ma_encoder encoder_;
};

[[nodiscard]] auto to_ma_encoding_format(audiorw::format format) -> ma_encoding_format;
[[nodiscard]] auto to_ma_format(int bit_depth, storage_type type) -> ma_format;
[[nodiscard]] auto to_std_origin(ma_seek_origin) -> std::ios_base::seekdir;

template <typename ShouldAbortFn>
auto ma_write(const audiorw::item& item, std::filesystem::path path, storage_type type, ShouldAbortFn should_abort) -> void {
	static constexpr auto CHUNK_SIZE = 1 << 14;
	auto config = ma_encoder_config_init(to_ma_encoding_format(item.header.format), to_ma_format(item.header.bit_depth, type), item.header.channel_count.value, item.header.SR);
	struct user_data_t {
		atomic_file_writer file;
	};
	auto user_data = user_data_t{};
	user_data.file = {path};
	auto on_write = [](ma_encoder* encoder, const void* buffer, size_t bytes_to_write, size_t* bytes_written) -> ma_result {
		const auto user_data = reinterpret_cast<user_data_t*>(encoder->pUserData);
		const auto char_data = reinterpret_cast<const char*>(buffer);
		user_data.file.write(char_data, bytes_to_write);
		return MA_SUCCESS;
	};
	auto on_seek = [](ma_encoder* encoder, ma_int64 offset, ma_seek_origin origin) -> ma_result {
		const auto user_data = reinterpret_cast<user_data_t*>(encoder->pUserData);
		user_data.file.seekp(offset, to_std_origin(origin));
		return MA_SUCCESS;
	};
	auto interleaved_frames = ads::interleaved<float>{item.header.channel_count, item.header.frame_count};
	ads::interleave(item.frames, interleaved_frames);
	auto encoder = scope_ma_encoder{on_write, on_seek, &user_data, config};
	auto frames_remaining = item.header.frame_count;
	auto pos              = 0;
	while (frames_remaining > 0UL) {
		if (should_abort()) {
			return;
		}
		const auto chunk_size     = std::min(frames_remaining, CHUNK_SIZE);
		const auto frames_written = encoder.write_pcm_frames(interleaved_frames.data() + pos, chunk_size);
		frames_remaining -= frames_written;
		pos              += frames_written;
	}
	user_data.file.commit();
}

template <typename ShouldAbortFn>
auto wavpack_write(const audiorw::item& item, std::filesystem::path path, storage_type type, ShouldAbortFn should_abort) -> void {
	// TODO:
}

} // detail

[[nodiscard]] auto make_format_hint(const std::filesystem::path& file_path) -> format_hint;

template <typename ShouldAbortFn>
auto read(std::filesystem::path path, audiorw::format_hint hint, ShouldAbortFn should_abort) -> item {
}

template <typename ShouldAbortFn>
auto write(const audiorw::item& item, std::filesystem::path path, storage_type type, ShouldAbortFn should_abort) -> void {
	switch (item.header.format) {
		case format::wavpack: { wavpack_write(item, path, type, should_abort); return; }
		default:              { ma_write(item, path, type, should_abort); return; }
	}
}

} // audiorw
