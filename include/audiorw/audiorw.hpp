#pragma once

#include <ads.hpp>
#include <boost/container/small_vector.hpp>
#include <corecrt_wio.h>
#include <filesystem>
#include <fstream>
#include <miniaudio.h>
#include <wavpack.h>

namespace audiorw::concepts {

template <typename Fn> concept should_abort_fn = requires (Fn fn) { { fn() } -> std::same_as<bool>; };

} // audiorw::concepts

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
	scope_wavpack_reader(WavpackStreamReader64* stream, void* user_data);
	~scope_wavpack_reader();
	auto context() { return context_; }
private:
	WavpackContext* context_;
};

struct scope_wavpack_writer {
	scope_wavpack_writer(audiorw::header header, storage_type type, WavpackBlockOutput blockout, void* user_data);
	~scope_wavpack_writer();
	auto context() { return context_; }
private:
	WavpackContext* context_;
};

struct ma_read_user_data_t {
	// TODO: don't do this. use ifstream instead
	const std::vector<std::byte>* bytes;
	ma_int64 pos = 0;
};

struct ma_write_user_data_t {
	atomic_file_writer file;
};

struct wavpack_read_user_data_t {
	// TODO: don't do this. use ifstream instead
	const std::vector<std::byte>* bytes;
	int64_t pos = 0;
};

struct wavpack_write_user_data_t {
	atomic_file_writer file;
};

enum class wavpack_chunk_write_result { aborted, succeeded };

[[nodiscard]] auto get_formats_to_try(format_hint hint) -> formats_to_try;
[[nodiscard]] auto to_ma_encoding_format(audiorw::format format) -> ma_encoding_format;
[[nodiscard]] auto to_ma_format(int bit_depth, storage_type type) -> ma_format;
[[nodiscard]] auto to_std_origin(ma_seek_origin) -> std::ios_base::seekdir;
[[nodiscard]] auto make_wavpack_config(audiorw::header header, storage_type type) -> WavpackConfig;
[[nodiscard]] auto make_wavpack_stream_reader() -> WavpackStreamReader64;
[[nodiscard]] auto wavpack_write_blockout(void* puserdata, void* data, int64_t bcount) -> int;
auto wavpack_write_float_chunk(WavpackContext* context, ads::interleaved<float>* frames, size_t chunk_size) -> void;
auto wavpack_write_int_chunk(WavpackContext* context, ads::channel_count chs, std::vector<int32_t>* int_sample_buf, int int_scale, ads::interleaved<float>* frames, size_t chunk_size) -> void;

auto ma_write(const audiorw::item& item, std::filesystem::path path, storage_type type, concepts::should_abort_fn auto should_abort) -> void {
	auto config = ma_encoder_config_init(to_ma_encoding_format(item.header.format), to_ma_format(item.header.bit_depth, type), item.header.channel_count.value, item.header.SR);
	auto user_data = ma_write_user_data_t{};
	user_data.file = {path};
	auto on_write = [](ma_encoder* encoder, const void* buffer, size_t bytes_to_write, size_t* bytes_written) -> ma_result {
		auto& user_data = *reinterpret_cast<ma_write_user_data_t*>(encoder->pUserData);
		const auto char_data = reinterpret_cast<const char*>(buffer);
		user_data.file.stream().write(char_data, bytes_to_write);
		return user_data.file.stream().fail() ? MA_ERROR : MA_SUCCESS;
	};
	auto on_seek = [](ma_encoder* encoder, ma_int64 offset, ma_seek_origin origin) -> ma_result {
		auto& user_data = *reinterpret_cast<ma_write_user_data_t*>(encoder->pUserData);
		user_data.file.stream().seekp(offset, to_std_origin(origin));
		return MA_SUCCESS;
	};
	auto interleaved_frames = ads::interleaved<float>{item.header.channel_count, item.header.frame_count};
	ads::interleave(item.frames, interleaved_frames.begin());
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
	auto int_sample_buf     = std::vector<int32_t>{};
	auto interleaved_frames = ads::interleaved<float>{item.header.channel_count, item.header.frame_count};
	ads::interleave(item.frames, interleaved_frames.begin());
    auto frames_remaining = item.header.frame_count;
	auto pos              = 0;
	while (frames_remaining > 0UL) {
		if (should_abort()) {
			return wavpack_chunk_write_result::aborted;
		}
		const auto chunk_size = std::min(CHUNK_SIZE, frames_remaining);
		wavpack_write_int_chunk(context, item.header.channel_count, &int_sample_buf, int_scale, &interleaved_frames, chunk_size);
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
	auto user_data = wavpack_write_user_data_t{};
	user_data.file = {path};
	auto blockout = [](void* puserdata, void* data, int64_t bcount) -> int { return wavpack_write_blockout(puserdata, data, bcount); };
	auto writer = scope_wavpack_writer{item.header, type, blockout, &user_data};
	if (wavpack_write_chunks(writer.context(), item, type, should_abort) == wavpack_chunk_write_result::succeeded) {
		if (!WavpackFlushSamples(writer.context())) {
			throw std::runtime_error("Write error");
		}
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

auto ma_try_read(const std::vector<std::byte>& bytes, audiorw::format format, concepts::should_abort_fn auto should_abort) -> std::optional<item> {
	auto user_data = ma_read_user_data_t{};
	user_data.bytes = &bytes;
	auto on_read = [](ma_decoder* decoder, void* buffer, size_t bytes_to_read, size_t* bytes_read) -> ma_result {
		auto& user_data = *reinterpret_cast<ma_read_user_data_t*>(decoder->pUserData);
		const auto byte_data = reinterpret_cast<std::byte*>(buffer);
		const auto bytes_available = user_data.bytes->size() - user_data.pos;
		bytes_to_read = std::min(bytes_available, bytes_to_read);
		const auto beg = user_data.bytes->data() + user_data.pos;
		const auto end = beg + bytes_to_read;
		std::copy(beg, end, byte_data);
		user_data.pos += bytes_to_read;
		*bytes_read = bytes_to_read;
		return MA_SUCCESS;
	};
	auto on_seek = [](ma_decoder* decoder, ma_int64 offset, ma_seek_origin origin) -> ma_result {
		auto& user_data = *reinterpret_cast<ma_read_user_data_t*>(decoder->pUserData);
		switch (origin) {
			case ma_seek_origin_start:   { user_data.pos = offset; return MA_SUCCESS; }
			case ma_seek_origin_current: { user_data.pos += offset; return MA_SUCCESS; }
			default:                     { throw std::runtime_error{"Invalid decoder seek origin"}; }
		}
	};
	try         { return ma_try_read(on_read, on_seek, user_data, should_abort); }
	catch (...) { return std::nullopt; }
}

auto wavpack_read(const std::vector<std::byte>& bytes, concepts::should_abort_fn auto should_abort) -> item {
	auto user_data = wavpack_read_user_data_t{};
	user_data.bytes = &bytes;
	auto stream = make_wavpack_stream_reader();
	auto writer = scope_wavpack_reader{&stream, &user_data};
	/*
	frame_size_ = sizeof(float);
	num_channels_ = WavpackGetNumChannels(context_);
	num_frames_ = uint64_t(WavpackGetNumSamples64(context_));
	sample_rate_ = WavpackGetSampleRate(context_);
	bit_depth_ = WavpackGetBitsPerSample(context_);

	const auto mode = WavpackGetMode(context_);

	if ((mode & MODE_FLOAT) == MODE_FLOAT)
	{
		chunk_reader_ = [this](float* buffer, uint32_t read_size)
		{
			return WavpackUnpackSamples(context_, reinterpret_cast<int32_t*>(buffer), read_size);
		};
	}
	else
	{
		chunk_reader_ = [this](float* buffer, uint32_t read_size)
		{
			const auto divisor = (1 << (bit_depth_ - 1)) - 1;

			unpacked_samples_buffer_.resize(size_t(num_channels_) * read_size);

			const auto frames_read = WavpackUnpackSamples(context_, unpacked_samples_buffer_.data(), read_size);

			unpacked_samples_buffer_.resize(frames_read * num_channels_);

			for (uint32_t i = 0; i < frames_read * num_channels_; i++)
			{
				buffer[i] = float(unpacked_samples_buffer_[i]) / divisor;
			}

			return frames_read;
		};
	}
	*/
}

auto try_read(std::filesystem::path path, audiorw::format format, concepts::should_abort_fn auto should_abort) -> std::optional<item> {
	switch (format) {
		case format::wavpack: { return detail::wavpack_read(read_file_bytes(path), should_abort); }
		default:              { return detail::ma_try_read(read_file_bytes(path), format, should_abort); }
	}
}

} // detail

[[nodiscard]] auto make_format_hint(const std::filesystem::path& file_path) -> format_hint;

auto read(std::filesystem::path path, audiorw::format_hint hint, concepts::should_abort_fn auto should_abort) -> item {
	auto formats_to_try = detail::get_formats_to_try(hint);
	for (auto format : formats_to_try) {
		if (auto item = detail::try_read(path, format, should_abort)) {
			return std::move(item).value();
		}
	}
	throw std::runtime_error{"Invalid audio format"};
}

auto write(const audiorw::item& item, std::filesystem::path path, storage_type type, concepts::should_abort_fn auto should_abort) -> void {
	switch (item.header.format) {
		case format::wavpack: { detail::wavpack_write(item, path, type, should_abort); return; }
		default:              { detail::ma_write(item, path, type, should_abort); return; }
	}
}

} // audiorw
