#pragma once

#include <ads.hpp>
#include <boost/container/small_vector.hpp>
#include <filesystem>
#include <fstream>
#include <miniaudio.h>
#include <stdexcept>
#include <wavpack.h>

namespace audiorw::concepts {

template <typename Fn> concept should_abort_fn = requires (Fn fn) { { fn() } -> std::same_as<bool>; };

} // audiorw::concepts

namespace audiorw::detail {

struct streamer_impl;

} // audiorw::detail

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

struct streamer {
	streamer(const std::filesystem::path& path, format_hint hint);
	auto read_frames(float* buffer, ads::frame_count frames_to_read) -> ads::frame_count;
	auto seek(ads::frame_idx pos) -> bool;
private:
	std::unique_ptr<detail::streamer_impl> impl_;
};

namespace detail {

static constexpr auto CHUNK_SIZE = 1 << 14;

using formats_to_try = boost::container::small_vector<format, 4>;

struct atomic_file_writer {
	atomic_file_writer() = default;
	atomic_file_writer(const std::filesystem::path& path);
	atomic_file_writer& operator=(atomic_file_writer&& rhs) noexcept = default;
	~atomic_file_writer();
	auto commit() -> void;
	auto stream() -> std::ofstream&;
private:
	std::filesystem::path path_;
	std::filesystem::path tmp_path_;
	std::ofstream file_;
	bool commit_flag_ = false;
};

struct scope_ma_decoder {
	scope_ma_decoder(ma_decoder_read_proc on_read, ma_decoder_seek_proc on_seek, void* user_data);
	~scope_ma_decoder();
    auto get_header(audiorw::format format) const -> header;
	auto read_pcm_frames(void* frames, ma_uint64 frame_count) -> ma_uint64;
	auto seek_to_pcm_frame(ma_uint64 frame) -> ma_result;
private:
	ma_decoder decoder_;
};

struct scope_ma_encoder {
	scope_ma_encoder(ma_encoder_write_proc on_write, ma_encoder_seek_proc on_seek, void* user_data, const ma_encoder_config& config);
	~scope_ma_encoder();
	auto write_pcm_frames(const void* frames, ma_uint64 frame_count) -> ma_uint64;
private:
	ma_encoder encoder_;
};

struct scope_wavpack_reader {
	scope_wavpack_reader(WavpackStreamReader64 stream, void* user_data);
	~scope_wavpack_reader();
    auto get_header() const -> const header& { return header_; }
	auto context() { return context_; }
	auto mode() const { return mode_; }
private:
	WavpackStreamReader64 stream_reader_;
	WavpackContext* context_;
	header header_;
	int mode_ = 0;
};

struct scope_wavpack_writer {
	scope_wavpack_writer(audiorw::header header, storage_type type, WavpackBlockOutput blockout, void* user_data);
	~scope_wavpack_writer();
	auto context() { return context_; }
private:
	WavpackContext* context_;
};

enum class wavpack_chunks_read_result { aborted, succeeded };
enum class wavpack_chunk_write_result { aborted, succeeded };

[[nodiscard]] auto get_formats_to_try(format_hint hint) -> formats_to_try;
[[nodiscard]] auto to_ma_encoding_format(audiorw::format format) -> ma_encoding_format;
[[nodiscard]] auto to_ma_format(int bit_depth, storage_type type) -> ma_format;
[[nodiscard]] auto to_std_origin(ma_seek_origin) -> std::ios_base::seekdir;
[[nodiscard]] auto make_wavpack_config(audiorw::header header, storage_type type) -> WavpackConfig;
[[nodiscard]] auto make_wavpack_stream_reader() -> WavpackStreamReader64;
[[nodiscard]] auto wavpack_write_blockout(void* puserdata, void* data, int64_t bcount) -> int;
auto ma_on_read(ma_decoder* decoder, void* buffer, size_t bytes_to_read, size_t* bytes_read) -> ma_result;
auto ma_on_seek(ma_decoder* decoder, ma_int64 offset, ma_seek_origin origin) -> ma_result;
auto wavpack_write_float_chunk(WavpackContext* context, ads::interleaved<float>* frames, size_t chunk_size) -> void;
auto wavpack_write_int_chunk(WavpackContext* context, ads::channel_count chs, int int_scale, ads::interleaved<float>* frames, size_t chunk_size) -> void;

auto ma_write(const audiorw::item& item, std::filesystem::path path, storage_type type, concepts::should_abort_fn auto should_abort) -> void {
	auto config = ma_encoder_config_init(to_ma_encoding_format(item.header.format), to_ma_format(item.header.bit_depth, type), item.header.channel_count.value, item.header.SR);
	auto writer = atomic_file_writer{path};
	auto on_write = [](ma_encoder* encoder, const void* buffer, size_t bytes_to_write, size_t* bytes_written) -> ma_result {
		auto& writer = *reinterpret_cast<atomic_file_writer*>(encoder->pUserData);
		const auto char_data = reinterpret_cast<const char*>(buffer);
		writer.stream().write(char_data, bytes_to_write);
		return writer.stream().fail() ? MA_ERROR : MA_SUCCESS;
	};
	auto on_seek = [](ma_encoder* encoder, ma_int64 offset, ma_seek_origin origin) -> ma_result {
		auto& writer = *reinterpret_cast<atomic_file_writer*>(encoder->pUserData);
		writer.stream().seekp(offset, to_std_origin(origin));
		return MA_SUCCESS;
	};
	auto interleaved_frames = ads::interleaved<float>{item.header.channel_count, item.header.frame_count};
	ads::interleave(item.frames, interleaved_frames.begin());
	auto encoder = scope_ma_encoder{on_write, on_seek, &writer, config};
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
	writer.commit();
}

[[nodiscard]]
auto wavpack_write_float_chunks(WavpackContext* context, const audiorw::item& item, concepts::should_abort_fn auto should_abort) -> wavpack_chunk_write_result {
	auto interleaved_frames = ads::interleaved<float>{item.header.channel_count, item.header.frame_count};
	ads::interleave(item.frames, interleaved_frames.begin());
    auto frames_remaining = item.header.frame_count;
	auto pos              = 0;
	while (frames_remaining > 0UL) {
		if (should_abort()) {
			return wavpack_chunk_write_result::aborted;
		}
		const auto chunk_size = std::min(CHUNK_SIZE, frames_remaining);
		wavpack_write_float_chunk(context, &interleaved_frames, chunk_size);
		frames_remaining -= chunk_size;
		pos              += chunk_size;
	}
	return wavpack_chunk_write_result::succeeded;
}

[[nodiscard]]
auto wavpack_write_int_chunks(WavpackContext* context, const audiorw::item& item, concepts::should_abort_fn auto should_abort) -> wavpack_chunk_write_result {
	const auto int_scale = (1 << (item.header.bit_depth - 1)) - 1;
	auto interleaved_frames = ads::interleaved<float>{item.header.channel_count, item.header.frame_count};
	ads::interleave(item.frames, interleaved_frames.begin());
    auto frames_remaining = item.header.frame_count;
	auto pos              = 0;
	while (frames_remaining > 0UL) {
		if (should_abort()) {
			return wavpack_chunk_write_result::aborted;
		}
		const auto chunk_size = std::min(CHUNK_SIZE, frames_remaining);
		wavpack_write_int_chunk(context, item.header.channel_count, int_scale, &interleaved_frames, chunk_size);
		frames_remaining -= chunk_size;
		pos              += chunk_size;
	}
	return wavpack_chunk_write_result::succeeded;
}

[[nodiscard]]
auto wavpack_write_chunks(WavpackContext* context, const audiorw::item& item, storage_type type, concepts::should_abort_fn auto should_abort) -> wavpack_chunk_write_result {
	switch (type) {
		case storage_type::float_:            { return wavpack_write_float_chunks(context, item, should_abort); }
		case storage_type::normalized_float_: { return wavpack_write_float_chunks(context, item, should_abort); }
		case storage_type::int_:              { return wavpack_write_int_chunks(context, item, should_abort); }
		default:                              { throw std::runtime_error{"Invalid storage type"}; }
	}
}

auto wavpack_write(const audiorw::item& item, std::filesystem::path path, storage_type type, concepts::should_abort_fn auto should_abort) -> void {
	auto file_writer = atomic_file_writer{path};
	auto blockout = [](void* puserdata, void* data, int64_t bcount) -> int { return wavpack_write_blockout(puserdata, data, bcount); };
	auto writer = scope_wavpack_writer{item.header, type, blockout, &file_writer};
	if (wavpack_write_chunks(writer.context(), item, type, should_abort) == wavpack_chunk_write_result::succeeded) {
		if (!WavpackFlushSamples(writer.context())) {
			throw std::runtime_error("Write error");
		}
		file_writer.commit();
	}
}

auto ma_try_read(audiorw::format format, ma_decoder_read_proc on_read, ma_decoder_seek_proc on_seek, void* user_data, concepts::should_abort_fn auto should_abort) -> std::optional<item> {
	auto decoder = scope_ma_decoder{on_read, on_seek, &user_data};
	auto item    = audiorw::item{};
	// NOTE: For mp3s get_header() will decode the entire file immediately.
	item.header  = decoder.get_header(format);
	item.frames  = ads::make<float>(item.header.channel_count, item.header.frame_count);
	auto interleaved_frames = ads::interleaved<float>{item.header.channel_count, item.header.frame_count};
	auto frames_remaining = item.header.frame_count;
	auto pos              = 0;
	while (frames_remaining > 0UL) {
		if (should_abort()) {
			return std::nullopt;
		}
		const auto chunk_size  = std::min(frames_remaining, CHUNK_SIZE);
		const auto frames_read = decoder.read_pcm_frames(interleaved_frames.data() + pos, chunk_size);
		frames_remaining -= frames_read;
		pos              += frames_read;
	}
	ads::deinterleave(interleaved_frames, item.frames.begin());
	return item;
}

auto ma_try_read(const std::filesystem::path& path, audiorw::format format, concepts::should_abort_fn auto should_abort) -> std::optional<item> {
	auto file = std::ifstream{path, std::ios::binary};
	try         { return ma_try_read(ma_on_read, ma_on_seek, &file, should_abort); }
	catch (...) { return std::nullopt; }
}

auto wavpack_read_float_chunks(WavpackContext* context, const audiorw::header& header, ads::interleaved<float>* buffer, concepts::should_abort_fn auto should_abort) -> wavpack_chunks_read_result {
	auto frames_remaining = header.frame_count;
	auto pos              = 0;
	while (frames_remaining > 0UL) {
		if (should_abort()) {
			return wavpack_chunks_read_result::aborted;
		}
		auto buffer_pos     = buffer->data() + pos;
		auto buffer_as_ints = reinterpret_cast<int32_t*>(buffer_pos);
		const auto chunk_size  = std::min(frames_remaining, CHUNK_SIZE);
		const auto frames_read = WavpackUnpacksamples(context, buffer_as_ints, chunk_size);
		frames_remaining -= frames_read;
		pos              += frames_read;
	}
	return wavpack_chunks_read_result::succeeded;
}

auto wavpack_read_int_chunks(WavpackContext* context, const audiorw::header& header, ads::interleaved<float>* buffer, concepts::should_abort_fn auto should_abort) -> wavpack_chunks_read_result {
	static_assert (sizeof(float) == sizeof(int32_t));
	const auto divisor    = (1 << (header.bit_depth - 1)) - 1;
	auto frames_remaining = header.frame_count;
	auto pos              = 0;
	while (frames_remaining > 0UL) {
		if (should_abort()) {
			return wavpack_chunks_read_result::aborted;
		}
		auto buffer_pos = buffer->data() + pos;
		auto buffer_as_ints = reinterpret_cast<int32_t*>(buffer_pos);
		const auto chunk_size  = std::min(frames_remaining, CHUNK_SIZE);
		const auto frames_read = WavpackUnpacksamples(context, buffer_as_ints, chunk_size);
		for (auto i = 0; i < frames_read * header.channel_count.value; i++) {
			buffer_pos[i] = static_cast<float>(buffer_as_ints[i]) / divisor;
		}
		frames_remaining -= frames_read;
		pos              += frames_read;
	}
	return wavpack_chunks_read_result::succeeded;
}

auto wavpack_read(const std::filesystem::path& path, concepts::should_abort_fn auto should_abort) -> std::optional<item> {
	auto file = std::ifstream{path, std::ios::binary};
	auto stream = make_wavpack_stream_reader();
	auto reader = scope_wavpack_reader{stream, &file};
	auto item   = audiorw::item{};
	item.header = reader.get_header();
	item.frames = ads::make<float>(item.header.channel_count, item.header.frame_count);
	auto interleaved_frames = ads::interleaved<float>{item.header.channel_count, item.header.frame_count};
	const auto float_mode = (reader.mode() & MODE_FLOAT) == MODE_FLOAT;
	if (float_mode) { wavpack_read_float_chunks(reader.context(), item.header, &interleaved_frames, should_abort); }
	else            { wavpack_read_int_chunks(reader.context(), item.header, &interleaved_frames, should_abort); }
	ads::deinterleave(interleaved_frames, item.frames.begin());
	return item;
}

auto try_read(std::filesystem::path path, audiorw::format format, concepts::should_abort_fn auto should_abort) -> std::optional<item> {
	switch (format) {
		case format::wavpack: { return detail::wavpack_read(path, should_abort); }
		default:              { return detail::ma_try_read(path, format, should_abort); }
	}
}

} // detail

[[nodiscard]] auto make_format_hint(const std::filesystem::path& file_path, bool try_all = false) -> format_hint;
[[nodiscard]] auto open(const std::filesystem::path& file_path, audiorw::format_hint hint) -> streamer;

auto read(std::filesystem::path path, audiorw::format_hint hint, concepts::should_abort_fn auto should_abort) -> std::optional<item> {
	const auto formats_to_try = detail::get_formats_to_try(hint);
	for (auto format : formats_to_try) {
		if (auto item = detail::try_read(path, format, should_abort)) {
			return std::move(item).value();
		}
	}
	throw std::runtime_error{"Invalid audio format"};
}

auto read(std::filesystem::path path, audiorw::format_hint hint) -> std::optional<item> {
	return read(path, hint, []{ return false; });
}

auto write(const audiorw::item& item, std::filesystem::path path, storage_type type, concepts::should_abort_fn auto should_abort) -> void {
	switch (item.header.format) {
		case format::wavpack: { detail::wavpack_write(item, std::move(path), type, std::move(should_abort)); return; }
		default:              { detail::ma_write(item, std::move(path), type, std::move(should_abort)); return; }
	}
}

auto write(const audiorw::item& item, std::filesystem::path path, storage_type type) -> void {
	return write(item, std::move(path), type, []{ return false; });
}

} // audiorw
