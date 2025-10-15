#pragma once

#include <ads.hpp>
#include <algorithm>
#include <boost/container/small_vector.hpp>
#include <filesystem>
#include <fstream>
#include <miniaudio.h>
#include <stdexcept>
#include <variant>
#include <wavpack.h>

namespace audiorw {

enum class format {
	flac,
	mp3,
	wav,
	wavpack
};

struct header {
	audiorw::format format;
	ads::channel_count channel_count;
	ads::frame_count frame_count;
	int SR        = 44100;
	int bit_depth = 32;
};

} // audiorw

namespace audiorw::detail {

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
	auto get_header() const -> header;
	auto get_header(audiorw::format format) const -> header;
	auto read_pcm_frames(void* frames, ma_uint64 frame_count) -> ma_uint64;
	auto seek_to_pcm_frame(ma_uint64 frame) -> ma_result;
private:
	ma_decoder decoder_;
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

using decoder = std::variant<scope_ma_decoder, scope_wavpack_reader>;

} // audiorw::detail

namespace audiorw {

struct item;

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

enum class operation_result { abort, fail, success };

struct ads_frame_input_stream {
	ads_frame_input_stream(ads::fully_dynamic<float>* frames);
	auto read_frames(std::span<float> buffer) -> ads::frame_count;
private:
	ads::fully_dynamic<float>* frames_;
	size_t pos_ = 0;
};

struct byte_input_stream {
	byte_input_stream(std::span<const std::byte> bytes);
	auto close() -> bool;
	auto get_length() -> std::optional<size_t>;
	auto get_pos() -> size_t;
	auto push_back_byte(std::byte v) -> bool;
	auto read_bytes(std::span<std::byte> buffer) -> size_t;
	auto seek(int64_t offset, std::ios::seekdir mode) -> bool;
private:
	std::span<const std::byte> bytes_;
	size_t pos_ = 0;
};

struct byte_item_input_stream {
	byte_item_input_stream(std::span<const std::byte> bytes, format_hint hint);
	// NOTE: For mp3s get_header() will have to decode the entire file immediately.
    auto get_header() const -> header;
	auto read_frames(float* buffer, ads::frame_count frames_to_read) -> ads::frame_count;
	auto seek(ads::frame_idx pos) -> bool;
private:
	byte_input_stream in_;
	detail::decoder decoder_;
};

struct file_byte_input_stream {
	file_byte_input_stream(const std::filesystem::path& path);
	auto close() -> bool;
	auto get_length() -> std::optional<size_t>;
	auto get_pos() -> size_t;
	auto push_back_byte(std::byte v) -> bool;
	auto read_bytes(std::span<std::byte> buffer) -> size_t;
	auto seek(int64_t offset, std::ios::seekdir mode) -> bool;
private:
	std::ifstream file_;
};

struct file_byte_output_stream {
	file_byte_output_stream(const std::filesystem::path& path);
	auto commit() -> void;
	auto seek(int64_t offset, std::ios::seekdir mode) -> bool;
	auto write_bytes(std::span<const std::byte> buffer) -> size_t;
private:
	detail::atomic_file_writer writer_;
};

struct file_item_input_stream {
	file_item_input_stream(const std::filesystem::path& path, format_hint hint);
	// NOTE: For mp3s get_header() will have to decode the entire file immediately.
    auto get_header() const -> header;
	auto read_frames(float* buffer, ads::frame_count frames_to_read) -> ads::frame_count;
	auto seek(ads::frame_idx pos) -> bool;
private:
	file_byte_input_stream in_;
	detail::decoder decoder_;
};

struct item_frame_input_stream {
	item_frame_input_stream(audiorw::item* item);
	auto read_frames(std::span<float> buffer) -> ads::frame_count;
private:
	ads_frame_input_stream stream_;
};

struct item_item_output_stream {
	item_item_output_stream(audiorw::item* item);
	auto commit() -> void {}
	auto seek(ads::frame_idx pos) -> bool;
	auto write_header(audiorw::header header) -> void;
	auto write_frames(std::span<const float> buffer) -> ads::frame_count;
private:
	item* item_;
	size_t pos_ = 0;
};

} // audiorw

namespace audiorw::concepts {

template <typename Fn> concept close_stream_fn      = requires(Fn fn) { { fn() } -> std::same_as<bool>; };
template <typename Fn> concept commit_fn            = requires(Fn fn) { { fn() } -> std::same_as<void>; };
template <typename Fn> concept get_stream_length_fn = requires(Fn fn) { { fn() } -> std::same_as<std::optional<size_t>>; };
template <typename Fn> concept get_stream_pos_fn    = requires(Fn fn) { { fn() } -> std::same_as<size_t>; };
template <typename Fn> concept push_back_byte_fn    = requires(Fn fn, std::byte v) { { fn(v) } -> std::same_as<bool>; };
template <typename Fn> concept read_bytes_fn        = requires(Fn fn, std::span<std::byte> buffer) { { fn(buffer) } -> std::same_as<size_t>; };
template <typename Fn> concept read_frames_fn       = requires(Fn fn, std::span<float> buffer) { { fn(buffer) } -> std::same_as<ads::frame_count>; };
template <typename Fn> concept seek_bytes_fn        = requires(Fn fn) { { fn(int64_t{}, std::ios::seekdir{}) } -> std::same_as<bool>; };
template <typename Fn> concept seek_frames_fn       = requires(Fn fn) { { fn(ads::frame_idx{}, std::ios::seekdir{}) } -> std::same_as<bool>; };
template <typename Fn> concept should_abort_fn      = requires(Fn fn) { { fn() } -> std::same_as<bool>; };
template <typename Fn> concept write_bytes_fn       = requires(Fn fn, std::span<const std::byte> buffer) { { fn(buffer) } -> std::same_as<size_t>; };
template <typename Fn> concept write_header_fn      = requires(Fn fn, audiorw::header v) { { fn(v) } -> std::same_as<void>; };
template <typename Fn> concept write_frames_fn      = requires(Fn fn, std::span<const float> buffer) { { fn(buffer) } -> std::same_as<ads::frame_count>; };

template <typename T>
concept byte_input_stream =
requires(T x, std::span<std::byte> buffer, std::byte b) {
	{ x.close() } -> std::same_as<bool>;
	{ x.get_length() } -> std::same_as<std::optional<size_t>>;
	{ x.get_pos() } -> std::same_as<size_t>;
	{ x.push_back_byte(b) } -> std::same_as<bool>;
	{ x.read_bytes(buffer) } -> std::same_as<size_t>;
	{ x.seek(int64_t{}, std::ios::seekdir{}) } -> std::same_as<bool>;
};

template <typename T>
concept frame_input_stream =
requires(T x, std::span<float> buffer) {
	{ x.read_frames(buffer) } -> std::same_as<ads::frame_count>;
};

template <typename T>
concept byte_output_stream =
requires(T x, std::span<const std::byte> buffer) {
	{ x.commit() } -> std::same_as<void>;
	{ x.seek(int64_t{}, std::ios::seekdir{}) } -> std::same_as<bool>;
	{ x.write_bytes(buffer) } -> std::same_as<size_t>;
};

template <typename T>
concept item_output_stream =
requires(T x, std::span<const float> buffer, audiorw::header header) {
	{ x.commit() } -> std::same_as<void>;
	{ x.seek(ads::frame_idx{}) } -> std::same_as<bool>;
	{ x.write_frames(buffer) } -> std::same_as<ads::frame_count>;
	{ x.write_header(header) } -> std::same_as<void>;
};

} // audiorw::concepts

namespace audiorw {

template <concepts::read_frames_fn ReadFramesFn>
struct generic_frame_input_stream {
	ReadFramesFn read_frames;
};

template <concepts::commit_fn CommitFn, concepts::seek_bytes_fn SeekBytesFn, concepts::write_bytes_fn WriteBytesFn>
struct generic_byte_output_stream {
	CommitFn commit;
	SeekBytesFn seek;
	WriteBytesFn write_bytes;
};

enum class storage_type {
	int_,
	float_,
	normalized_float_,
};

using frames = ads::fully_dynamic<float>;

struct item {
	audiorw::header header;
	audiorw::frames frames;
};

namespace detail {

static constexpr auto CHUNK_SIZE = 1 << 14;

using formats_to_try = boost::container::small_vector<format, 4>;

struct scope_ma_encoder {
	scope_ma_encoder(ma_encoder_write_proc on_write, ma_encoder_seek_proc on_seek, void* user_data, const ma_encoder_config& config);
	~scope_ma_encoder();
	auto write_pcm_frames(const void* frames, ma_uint64 frame_count) -> ma_uint64;
private:
	ma_encoder encoder_;
};

struct scope_wavpack_writer {
	scope_wavpack_writer(const audiorw::header& header, storage_type type, WavpackBlockOutput blockout, void* user_data);
	~scope_wavpack_writer();
	auto context() { return context_; }
private:
	WavpackContext* context_;
};

[[nodiscard]] auto get_formats_to_try(format_hint hint) -> formats_to_try;
[[nodiscard]] auto to_ma_encoding_format(audiorw::format format) -> ma_encoding_format;
[[nodiscard]] auto to_ma_format(int bit_depth, storage_type type) -> ma_format;
[[nodiscard]] auto ma_to_std_seek_mode(ma_seek_origin) -> std::ios_base::seekdir;
[[nodiscard]] auto wavpack_to_std_seek_mode(int mode) -> std::ios_base::seekdir;
[[nodiscard]] auto make_wavpack_config(const audiorw::header& header, storage_type type) -> WavpackConfig;

template <concepts::byte_input_stream Stream> [[nodiscard]]
auto ma_on_decoder_read(ma_decoder* decoder, void* buffer, size_t bytes_to_read, size_t* bytes_read) -> ma_result {
	auto& stream = *reinterpret_cast<Stream*>(decoder->pUserData);
	const auto buffer_as_bytes = reinterpret_cast<std::byte*>(buffer);
	*bytes_read = stream.read_bytes({buffer_as_bytes, bytes_to_read});
	return MA_SUCCESS;
}

template <concepts::byte_input_stream Stream> [[nodiscard]]
auto ma_on_decoder_seek(ma_decoder* decoder, ma_int64 offset, ma_seek_origin origin) -> ma_result {
	auto& stream = *reinterpret_cast<Stream*>(decoder->pUserData);
	return stream.seek(offset, ma_to_std_seek_mode(origin)) ? MA_SUCCESS : MA_ERROR;
}

template <concepts::byte_output_stream Stream> [[nodiscard]]
auto ma_on_encoder_write(ma_encoder* encoder, const void* buffer, size_t bytes_to_write, size_t* bytes_written) -> ma_result {
	auto& stream = *reinterpret_cast<Stream*>(encoder->pUserData);
	const auto buffer_as_bytes = reinterpret_cast<const std::byte*>(buffer);
	*bytes_written = stream.write_bytes({buffer_as_bytes, bytes_to_write});
	return MA_SUCCESS;
}

template <concepts::byte_output_stream Stream> [[nodiscard]]
auto ma_on_encoder_seek(ma_encoder* encoder, ma_int64 offset, ma_seek_origin origin) -> ma_result {
	auto& stream = *reinterpret_cast<Stream*>(encoder->pUserData);
	return stream.seek(offset, ma_to_std_seek_mode(origin)) ? MA_SUCCESS : MA_ERROR;
}

template <concepts::byte_output_stream Stream> [[nodiscard]]
auto wavpack_write_blockout(void* puserdata, void* data, int32_t bcount) -> int {
	auto& stream = *reinterpret_cast<Stream*>(puserdata);
	const auto data_as_bytes = reinterpret_cast<const std::byte*>(data);
	return stream.write_bytes({data_as_bytes, static_cast<size_t>(bcount)});
}

template <concepts::byte_input_stream Stream> [[nodiscard]]
auto make_wavpack_stream_reader() -> WavpackStreamReader64 {
	WavpackStreamReader64 sr;
	sr.can_seek = [](void* puserdata) -> int {
		return puserdata ? 1 : 0;
	};
	sr.close = [](void* puserdata) -> int {
		auto& stream = *reinterpret_cast<Stream*>(puserdata);
		return stream.close() ? 1 : 0;
	};
	sr.get_length = [](void* puserdata) -> int64_t {
		auto& stream = *reinterpret_cast<Stream*>(puserdata);
		const auto length = stream.get_length();
		return length ? *length : 0;
	};
	sr.get_pos = [](void* puserdata) -> int64_t {
		auto& stream = *reinterpret_cast<Stream*>(puserdata);
		return stream.get_pos();
	};
	sr.push_back_byte = [](void* puserdata, int c) -> int {
		auto& stream = *reinterpret_cast<Stream*>(puserdata);
		return stream.push_back_byte(static_cast<std::byte>(c)) ? c : 0;
	};
	sr.read_bytes = [](void* puserdata, void* buffer, int32_t bcount) -> int32_t {
		auto& stream = *reinterpret_cast<Stream*>(puserdata);
		const auto buffer_as_bytes = reinterpret_cast<std::byte*>(buffer);
		const auto beg             = buffer_as_bytes;
		const auto end             = buffer_as_bytes + bcount;
		const auto bytes           = std::span<std::byte>{beg, end};
		return stream.read_bytes(bytes);
	};
	sr.set_pos_abs = [](void* puserdata, int64_t pos) -> int {
		auto& stream = *reinterpret_cast<Stream*>(puserdata);
		stream.seek(pos, std::ios::beg);
		return 0;
	};
	sr.set_pos_rel = [](void* puserdata, int64_t delta, int mode) -> int {
		auto& stream = *reinterpret_cast<Stream*>(puserdata);
		stream.seek(delta, wavpack_to_std_seek_mode(mode));
		return 0;
	};
	return sr;
}

auto ma_write(const audiorw::header& header, concepts::frame_input_stream auto* in, concepts::byte_output_stream auto* out, storage_type type, concepts::should_abort_fn auto should_abort) -> operation_result {
	using OutStream = std::remove_reference_t<decltype(*out)>;
	auto config           = ma_encoder_config_init(to_ma_encoding_format(header.format), to_ma_format(header.bit_depth, type), header.channel_count.value, header.SR);
	auto encoder          = scope_ma_encoder{ma_on_encoder_write<OutStream>, ma_on_encoder_seek<OutStream>, out, config};
	auto sample_buffer    = std::vector<float>{};
	auto frames_remaining = header.frame_count;
	auto pos              = 0;
	while (frames_remaining > 0UL) {
		if (should_abort()) {
			return operation_result::abort;
		}
		const auto frames_to_process  = std::min(frames_remaining.value, uint64_t(CHUNK_SIZE));
		const auto samples_to_process = header.channel_count.value * frames_to_process;
		sample_buffer.resize(samples_to_process);
		const auto frames_read = in->read_frames(sample_buffer);
		if (frames_read != frames_to_process) {
			throw std::runtime_error{"Error reading frames"};
		}
		const auto frames_written = encoder.write_pcm_frames(sample_buffer.data(), frames_to_process);
		if (frames_written != frames_to_process) {
			throw std::runtime_error{"Error writing PCM frames"};
		}
		frames_remaining -= frames_written;
		pos              += frames_written;
	}
	out->commit();
	return operation_result::success;
}

[[nodiscard]]
auto wavpack_write_float_chunks(const audiorw::header& header, concepts::frame_input_stream auto* in, WavpackContext* context, concepts::should_abort_fn auto should_abort) -> operation_result {
	auto sample_buffer    = std::vector<float>{};
    auto frames_remaining = header.frame_count;
	auto pos              = 0;
	while (frames_remaining > 0UL) {
		if (should_abort()) {
			return operation_result::abort;
		}
		const auto frames_to_process  = std::min(frames_remaining.value, uint64_t(CHUNK_SIZE));
		const auto samples_to_process = header.channel_count.value * frames_to_process;
		sample_buffer.resize(samples_to_process);
		const auto frames_read = in->read_frames(sample_buffer);
		if (frames_read != frames_to_process) {
			throw std::runtime_error{"Error reading frames"};
		}
		const auto buffer_as_ints = reinterpret_cast<int32_t*>(sample_buffer.data());
		const auto frames_written = WavpackPackSamples(context, buffer_as_ints, frames_to_process);
		if (frames_written != frames_to_process) {
			throw std::runtime_error{"Error packing WavPack samples"};
		}
		frames_remaining -= frames_to_process;
		pos              += frames_to_process;
	}
	return operation_result::success;
}

[[nodiscard]]
auto wavpack_write_int_chunks(const audiorw::header& header, concepts::frame_input_stream auto* in, WavpackContext* context, concepts::should_abort_fn auto should_abort) -> operation_result {
	static_assert (sizeof(float) == sizeof(int32_t));
	const auto int_scale  = (1 << (header.bit_depth - 1)) - 1;
	auto sample_buffer    = std::vector<float>{};
    auto frames_remaining = header.frame_count;
	auto pos              = 0;
	while (frames_remaining > 0UL) {
		if (should_abort()) {
			return operation_result::abort;
		}
		const auto frames_to_process  = std::min(frames_remaining.value, uint64_t(CHUNK_SIZE));
		const auto samples_to_process = header.channel_count.value * frames_to_process;
		sample_buffer.resize(samples_to_process);
		const auto frames_read = in->read_frames(sample_buffer);
		if (frames_read != frames_to_process) {
			throw std::runtime_error{"Error reading frames"};
		}
		const auto buffer_as_ints = reinterpret_cast<int32_t*>(sample_buffer.data());
		for (int i = 0; i < sample_buffer.size(); i++) {
			buffer_as_ints[i] = static_cast<int32_t>(double(sample_buffer[i]) * int_scale);
		}
		const auto frames_written = WavpackPackSamples(context, buffer_as_ints, frames_to_process);
		if (frames_written != frames_to_process) {
			throw std::runtime_error{"Error packing WavPack samples"};
		}
		frames_remaining -= frames_to_process;
		pos              += frames_to_process;
	}
	return operation_result::success;
}

[[nodiscard]]
auto wavpack_write_chunks(const audiorw::header& header, concepts::frame_input_stream auto* in, WavpackContext* context, storage_type type, concepts::should_abort_fn auto should_abort) -> operation_result {
	switch (type) {
		case storage_type::float_:            { return wavpack_write_float_chunks(header, in, context, should_abort); }
		case storage_type::normalized_float_: { return wavpack_write_float_chunks(header, in, context, should_abort); }
		case storage_type::int_:              { return wavpack_write_int_chunks(header, in, context, should_abort); }
		default:                              { throw std::runtime_error{"Invalid storage type"}; }
	}
}

auto wavpack_write(const audiorw::header& header, concepts::frame_input_stream auto* in, concepts::byte_output_stream auto* out, storage_type type, concepts::should_abort_fn auto should_abort) -> operation_result {
	using OutStream = std::remove_reference_t<decltype(*out)>;
	auto writer = scope_wavpack_writer{header, type, wavpack_write_blockout<OutStream>, out};
	auto result = wavpack_write_chunks(header, in, writer.context(), type, should_abort);
	if (result == operation_result::success) {
		if (!WavpackFlushSamples(writer.context())) {
			throw std::runtime_error("Write error");
		}
		out->commit();
	}
	return result;
}

auto ma_try_read(concepts::item_output_stream auto* out, audiorw::format format, ma_decoder_read_proc on_read, ma_decoder_seek_proc on_seek, void* user_data, concepts::should_abort_fn auto should_abort) -> operation_result {
	auto decoder = scope_ma_decoder{on_read, on_seek, &user_data};
	// NOTE: For mp3s get_header() will decode the entire file immediately.
	const auto header = decoder.get_header(format);
	out->write_header(header);
	auto buffer           = std::vector<float>{};
	auto frames_remaining = header.frame_count;
	while (frames_remaining > 0UL) {
		if (should_abort()) {
			return operation_result::abort;
		}
		const auto frames_to_read  = std::min(frames_remaining.value, uint64_t(CHUNK_SIZE));
		const auto samples_to_read = header.channel_count.value * frames_to_read;
		buffer.resize(samples_to_read);
		const auto frames_read = decoder.read_pcm_frames(buffer.data(), frames_to_read);
		if (frames_read != frames_to_read) {
			throw std::runtime_error{"Error reading PCM frames"};
		}
		const auto frames_written = out->write_frames({buffer});
		if (frames_written != frames_to_read) {
			throw std::runtime_error{"Error reading frames"};
		}
		frames_remaining -= frames_read;
	}
	return operation_result::success;
}

inline
auto ma_try_read_header(audiorw::format format, ma_decoder_read_proc on_read, ma_decoder_seek_proc on_seek, void* user_data) -> header {
	auto decoder = scope_ma_decoder{on_read, on_seek, &user_data};
	// NOTE: For mp3s get_header() will decode the entire file immediately.
	return decoder.get_header(format);
}

auto ma_try_read(concepts::byte_input_stream auto* in, concepts::item_output_stream auto* out, audiorw::format format, concepts::should_abort_fn auto should_abort) -> operation_result {
	using InStream = std::remove_reference_t<decltype(*in)>;
	try         { return ma_try_read(out, format, ma_on_decoder_read<InStream>, ma_on_decoder_seek<InStream>, in, should_abort); }
	catch (...) { return operation_result::fail; }
}

auto ma_try_read_header(concepts::byte_input_stream auto* in, audiorw::format format) -> std::optional<header> {
	using InStream = std::remove_reference_t<decltype(*in)>;
	try         { return ma_try_read_header(format, ma_on_decoder_read<InStream>, ma_on_decoder_seek<InStream>, in); }
	catch (...) { return std::nullopt; }
}

auto wavpack_read_float_chunks(concepts::item_output_stream auto* out, WavpackContext* context, const audiorw::header& header, concepts::should_abort_fn auto should_abort) -> operation_result {
	auto buffer           = std::vector<float>{};
	auto frames_remaining = header.frame_count;
	while (frames_remaining > 0UL) {
		if (should_abort()) {
			return operation_result::abort;
		}
		const auto frames_to_read = std::min(frames_remaining.value, uint64_t(CHUNK_SIZE));
		const auto samples_to_read = header.channel_count.value * frames_to_read;
		buffer.resize(samples_to_read);
		auto buffer_as_ints = reinterpret_cast<int32_t*>(buffer.data());
		const auto frames_read = WavpackUnpackSamples(context, buffer_as_ints, frames_to_read);
		if (frames_read != frames_to_read) {
			throw std::runtime_error{"Error unpacking WavPack samples"};
		}
		const auto frames_written = out->write_frames({buffer});
		if (frames_written != frames_to_read) {
			throw std::runtime_error{"Error reading frames"};
		}
		frames_remaining -= frames_to_read;
	}
	return operation_result::success;
}

auto wavpack_read_int_chunks(concepts::item_output_stream auto* out, WavpackContext* context, const audiorw::header& header, concepts::should_abort_fn auto should_abort) -> operation_result {
	static_assert (sizeof(float) == sizeof(int32_t));
	const auto divisor    = (1 << (header.bit_depth - 1)) - 1;
	auto buffer           = std::vector<float>{};
	auto frames_remaining = header.frame_count;
	while (frames_remaining > 0UL) {
		if (should_abort()) {
			return operation_result::abort;
		}
		const auto frames_to_read = std::min(frames_remaining.value, uint64_t(CHUNK_SIZE));
		const auto samples_to_read = header.channel_count.value * frames_to_read;
		buffer.resize(samples_to_read);
		auto buffer_as_ints = reinterpret_cast<int32_t*>(buffer.data());
		const auto frames_read = WavpackUnpackSamples(context, buffer_as_ints, frames_to_read);
		if (frames_read != frames_to_read) {
			throw std::runtime_error{"Error unpacking WavPack samples"};
		}
		for (auto i = 0; i < buffer.size(); i++) {
			buffer.data()[i] = static_cast<float>(buffer_as_ints[i]) / divisor;
		}
		const auto frames_written = out->write_frames({buffer});
		if (frames_written != frames_to_read) {
			throw std::runtime_error{"Error reading frames"};
		}
		frames_remaining -= frames_to_read;
	}
	return operation_result::success;
}

[[nodiscard]]
auto wavpack_read(concepts::byte_input_stream auto* in, concepts::item_output_stream auto* out, concepts::should_abort_fn auto should_abort) -> operation_result {
	using InStream = std::remove_reference_t<decltype(*in)>;
	auto stream     = make_wavpack_stream_reader<InStream>();
	auto reader     = scope_wavpack_reader{stream, in};
	const auto header = reader.get_header();
	out->write_header(header);
	const auto float_mode = (reader.mode() & MODE_FLOAT) == MODE_FLOAT;
	if (float_mode) { return wavpack_read_float_chunks(out, reader.context(), header, should_abort); }
	else            { return wavpack_read_int_chunks(out, reader.context(), header, should_abort); }
}

[[nodiscard]]
auto wavpack_read_header(concepts::byte_input_stream auto* in) -> header {
	using InStream = std::remove_reference_t<decltype(*in)>;
	auto stream = make_wavpack_stream_reader<InStream>();
	auto reader = scope_wavpack_reader{stream, in};
	return reader.get_header();
}

[[nodiscard]]
auto try_read(concepts::byte_input_stream auto* in, concepts::item_output_stream auto* out, audiorw::format format, concepts::should_abort_fn auto should_abort) -> operation_result {
	switch (format) {
		case format::wavpack: { return detail::wavpack_read(in, out, should_abort); }
		default:              { return detail::ma_try_read(in, out, format, should_abort); }
	}
}

[[nodiscard]]
auto try_read_header(concepts::byte_input_stream auto* in, audiorw::format format) -> std::optional<header> {
	switch (format) {
		case format::wavpack: { return detail::wavpack_read_header(in); }
		default:              { return detail::ma_try_read_header(in, format); }
	}
}

[[nodiscard]]
auto read(concepts::byte_input_stream auto* in, concepts::item_output_stream auto* out, audiorw::format_hint hint, concepts::should_abort_fn auto should_abort) -> operation_result {
	const auto formats_to_try = get_formats_to_try(hint);
	for (auto format : formats_to_try) {
		if (const auto r = try_read(in, out, format, should_abort); r == operation_result::success) {
			return r;
		}
		in->seek(0, std::ios::beg);
		out->seek({0});
	}
	throw std::runtime_error{"Invalid audio format"};
}

[[nodiscard]]
auto read_header(concepts::byte_input_stream auto* in, audiorw::format_hint hint) -> header {
	const auto formats_to_try = get_formats_to_try(hint);
	for (auto format : formats_to_try) {
		if (const auto header = try_read_header(in, format)) {
			return header;
		}
	}
	throw std::runtime_error{"Invalid audio format"};
}

auto fn_always(auto value) { return [value]{ return value; }; }

[[nodiscard]]
auto try_make_wavpack_decoder(concepts::byte_input_stream auto* in) -> std::optional<detail::decoder> {
	using Stream = std::remove_reference_t<decltype(*in)>;
	try         { return detail::scope_wavpack_reader{detail::make_wavpack_stream_reader<std::remove_reference_t<Stream>>(), in}; }
	catch (...) { return std::nullopt; }
}

[[nodiscard]]
auto try_make_ma_decoder(concepts::byte_input_stream auto* in) -> std::optional<detail::decoder> {
	using Stream = std::remove_reference_t<decltype(*in)>;
	try         { return detail::scope_ma_decoder{detail::ma_on_decoder_read<Stream>, detail::ma_on_decoder_seek<Stream>, in}; }
	catch (...) { return std::nullopt; }
}

[[nodiscard]]
auto try_make_decoder(concepts::byte_input_stream auto* in, audiorw::format format) -> std::optional<detail::decoder> {
	switch (format) {
		case audiorw::format::wavpack: { return try_make_wavpack_decoder(in); }
		default:                       { return try_make_ma_decoder(in); }
	}
}

[[nodiscard]]
auto make_decoder(concepts::byte_input_stream auto* in, format_hint hint) -> detail::decoder {
	const auto formats_to_try = detail::get_formats_to_try(hint);
	for (auto format : formats_to_try) {
		if (auto decoder = try_make_decoder(in, format)) {
			return std::move(*decoder);
		}
		in->seek(0, std::ios::beg);
	}
	throw std::runtime_error{"Failed to make decoder"};
}

[[nodiscard]] inline
auto stream_read_float_frames(scope_wavpack_reader* stream, float* buffer, ads::frame_count frames_to_read) -> ads::frame_count {
	auto buffer_as_ints = reinterpret_cast<int32_t*>(buffer);
	return {WavpackUnpackSamples(stream->context(), buffer_as_ints, frames_to_read.value)};
}

[[nodiscard]] inline
auto stream_read_int_frames(scope_wavpack_reader* stream, float* buffer, ads::frame_count frames_to_read) -> ads::frame_count {
	auto buffer_as_ints = reinterpret_cast<int32_t*>(buffer);
	const auto& header     = stream->get_header();
	const auto frames_read = WavpackUnpackSamples(stream->context(), buffer_as_ints, frames_to_read.value);
	const auto chs         = header.channel_count.value;
	const auto divisor     = (1 < (header.bit_depth -1 )) - 1;
	for (auto i = 0; i < frames_read * chs; i++) {
		buffer[i] = static_cast<float>(buffer_as_ints[i]) / divisor;
	}
	return {frames_read};
}

[[nodiscard]] inline
auto read_frames(scope_wavpack_reader* decoder, float* buffer, ads::frame_count frames_to_read) -> ads::frame_count {
	const auto float_mode = (decoder->mode() & MODE_FLOAT) == MODE_FLOAT;
	if (float_mode) { return stream_read_float_frames(decoder, buffer, frames_to_read); }
	else            { return stream_read_int_frames(decoder, buffer, frames_to_read); }
}

[[nodiscard]] inline
auto seek(scope_wavpack_reader* decoder, ads::frame_idx pos) -> bool {
	return WavpackSeekSample64(decoder->context(), pos.value) == 1;
}

[[nodiscard]] inline
auto read_frames(scope_ma_decoder* decoder, float* buffer, ads::frame_count frames_to_read) -> ads::frame_count {
	return {decoder->read_pcm_frames(buffer, frames_to_read.value)};
}

[[nodiscard]] inline
auto seek(scope_ma_decoder* decoder, ads::frame_idx pos) -> bool {
	return decoder->seek_to_pcm_frame(pos.value) == MA_SUCCESS;
}

[[nodiscard]] inline
auto get_header(const detail::decoder* decoder) -> header {
	return std::visit([](auto& decoder){ return decoder.get_header(); }, *decoder);
}

[[nodiscard]] inline
auto read_frames(detail::decoder* decoder, float* buffer, ads::frame_count frames_to_read) -> ads::frame_count {
	return std::visit([buffer, frames_to_read](auto& decoder){ return read_frames(&decoder, buffer, frames_to_read); }, *decoder);
}

[[nodiscard]] inline
auto seek(detail::decoder* decoder, ads::frame_idx pos) -> bool {
	return std::visit([pos](auto& decoder){ return seek(&decoder, pos); }, *decoder);
}

} // detail

//########################################################################################

inline
byte_input_stream::byte_input_stream(std::span<const std::byte> bytes)
	: bytes_{bytes}
{
}

inline
auto byte_input_stream::close() -> bool {
	return true;
}

inline
auto byte_input_stream::get_length() -> std::optional<size_t> {
	return bytes_.size();
}

inline
auto byte_input_stream::get_pos() -> size_t {
	return pos_;
}

inline
auto byte_input_stream::push_back_byte(std::byte v) -> bool {
	pos_--;
	return true;
}

inline
auto byte_input_stream::read_bytes(std::span<std::byte> buffer) -> size_t {
	const auto remaining_bytes = bytes_.size() - pos_;
	const auto n = std::min(buffer.size(), remaining_bytes);
	const auto beg = bytes_.data();
	const auto end = bytes_.data() + n;
	std::copy(beg, end, buffer.data());
	pos_ += n;
	return n;
}

inline
auto byte_input_stream::seek(int64_t offset, std::ios::seekdir mode) -> bool {
	switch (mode) {
		case std::ios::beg: { pos_ = offset; return true; }
		case std::ios::cur: { pos_ += offset; return true; }
		case std::ios::end: { pos_ = bytes_.size() + offset; return true; }
	}
	return false;
}

//########################################################################################

inline
file_byte_input_stream::file_byte_input_stream(const std::filesystem::path& path)
	: file_{path, std::ios::binary}
{
}

inline
auto file_byte_input_stream::close() -> bool {
	file_.close();
	return true;
}

inline
auto file_byte_input_stream::get_length() -> std::optional<size_t> {
	const auto pos = file_.tellg();
	file_.seekg(0, std::ios::end);
	const auto length = file_.tellg();
	file_.seekg(pos, std::ios::beg);
	return length;
}

inline
auto file_byte_input_stream::get_pos() -> size_t {
	return file_.tellg();
}

inline
auto file_byte_input_stream::push_back_byte(std::byte v) -> bool {
	file_.putback(static_cast<char>(v));
	return !file_.fail();
}

inline
auto file_byte_input_stream::read_bytes(std::span<std::byte> buffer) -> size_t {
	if (buffer.size() < 1) return 0;
	auto char_buffer = reinterpret_cast<char*>(buffer.data());
	file_.read(char_buffer, buffer.size());
	return file_.fail() ? 0 : file_.gcount();
}

inline
auto file_byte_input_stream::seek(int64_t offset, std::ios::seekdir mode) -> bool {
	file_.seekg(offset, mode);
	return !file_.fail();
}

//########################################################################################

inline
byte_item_input_stream::byte_item_input_stream(std::span<const std::byte> bytes, format_hint hint)
	: in_{bytes}
	, decoder_{detail::make_decoder(&in_, hint)}
{
}

inline
auto byte_item_input_stream::get_header() const -> header {
	return detail::get_header(&decoder_);
}

inline
auto byte_item_input_stream::read_frames(float* buffer, ads::frame_count frames_to_read) -> ads::frame_count {
	return detail::read_frames(&decoder_, buffer, frames_to_read);
}

inline
auto byte_item_input_stream::seek(ads::frame_idx pos) -> bool {
	return detail::seek(&decoder_, pos);
}

//########################################################################################

inline
file_item_input_stream::file_item_input_stream(const std::filesystem::path& path, format_hint hint)
	: in_{path}
	, decoder_{detail::make_decoder(&in_, hint)}
{
}

inline
auto file_item_input_stream::get_header() const -> header {
	return detail::get_header(&decoder_);
}

inline
auto file_item_input_stream::read_frames(float* buffer, ads::frame_count frames_to_read) -> ads::frame_count {
	return detail::read_frames(&decoder_, buffer, frames_to_read);
}

inline
auto file_item_input_stream::seek(ads::frame_idx pos) -> bool {
	return detail::seek(&decoder_, pos);
}

//########################################################################################
inline
ads_frame_input_stream::ads_frame_input_stream(ads::fully_dynamic<float>* frames)
	: frames_{frames}
{
}

inline
auto ads_frame_input_stream::read_frames(std::span<float> buffer) -> ads::frame_count {
	const auto frames_remaining = frames_->get_frame_count().value - pos_;
	const auto frames_to_read   = std::min(frames_remaining, buffer.size() / frames_->get_channel_count().value);
	const auto beg              = frames_->begin();
	const auto end              = beg + frames_to_read;
	ads::interleave(std::ranges::subrange(beg, end), buffer.begin());
	pos_ += frames_to_read;
	return {frames_to_read};
}

//########################################################################################
inline
item_frame_input_stream::item_frame_input_stream(audiorw::item* item)
	: stream_{&item->frames}
{
}

inline
auto item_frame_input_stream::read_frames(std::span<float> buffer) -> ads::frame_count {
	return stream_.read_frames(buffer);
}

//########################################################################################

inline
item_item_output_stream::item_item_output_stream(audiorw::item* item)
	: item_{item}
{
}

inline
auto item_item_output_stream::seek(ads::frame_idx pos) -> bool {
	pos_ = 0;
	return true;
}

inline
auto item_item_output_stream::write_header(audiorw::header header) -> void {
	item_->header = header;
	item_->frames = ads::make<float>(header.channel_count, header.frame_count);
}

inline
auto item_item_output_stream::write_frames(std::span<const float> buffer) -> ads::frame_count {
	if (item_->header.channel_count == 0) {
		throw std::runtime_error{"Header not written yet"};
	}
	const auto space_remaining = item_->frames.get_frame_count().value - pos_;
	const auto frames_to_write = std::min(space_remaining, buffer.size() / item_->frames.get_channel_count().value);
	const auto beg             = buffer.begin();
	const auto end             = buffer.begin() + frames_to_write;
	const auto write_pos       = item_->frames.begin() + pos_;
	ads::deinterleave(std::ranges::subrange(beg, end), write_pos);
	pos_ += frames_to_write;
	return {frames_to_write};
}

//########################################################################################

inline
file_byte_output_stream::file_byte_output_stream(const std::filesystem::path& path)
	: writer_{path}
{
}

inline
auto file_byte_output_stream::commit() -> void {
	writer_.commit();
}

inline
auto file_byte_output_stream::seek(int64_t offset, std::ios::seekdir mode) -> bool {
	auto& file = writer_.stream();
	file.seekp(offset, mode);
	return !file.fail();
}

inline
auto file_byte_output_stream::write_bytes(std::span<const std::byte> buffer) -> size_t {
	const auto buffer_as_chars = reinterpret_cast<const char*>(buffer.data());
	auto& file = writer_.stream();
	file.write(buffer_as_chars, buffer.size());
	return file.fail() ? 0 : buffer.size();
}
//########################################################################################

[[nodiscard]] auto get_known_file_extensions() -> std::array<std::string_view, 4>;
[[nodiscard]] auto make_format_hint(const std::filesystem::path& file_path, bool try_all = false) -> std::optional<format_hint>;

[[nodiscard]]
auto read_header(concepts::byte_input_stream auto* in, audiorw::format_hint hint) -> header {
	return detail::read_header(in, hint);
}

auto read(concepts::byte_input_stream auto* in, concepts::item_output_stream auto* out, audiorw::format_hint hint, concepts::should_abort_fn auto should_abort) -> operation_result {
	return detail::read(in, out, hint, std::move(should_abort));
}

auto read(concepts::byte_input_stream auto* in, concepts::item_output_stream auto* out, audiorw::format_hint hint) -> operation_result {
	return read(in, out, hint, detail::fn_always(false));
}

[[nodiscard]]
auto read(const std::filesystem::path& path, audiorw::format_hint hint, concepts::should_abort_fn auto should_abort) -> std::optional<item> {
	auto item = audiorw::item{};
	auto in   = audiorw::file_byte_input_stream{path};
	auto out  = audiorw::item_item_output_stream{&item};
	auto result = audiorw::read(&in, &out, audiorw::format_hint::try_wav_only, should_abort);
	if (result == audiorw::operation_result::success) { return std::move(item); }
	else                                              { return std::nullopt; }
}

auto write(const audiorw::header& header, concepts::frame_input_stream auto* in, concepts::byte_output_stream auto* out, storage_type type, concepts::should_abort_fn auto should_abort) -> operation_result {
	switch (header.format) {
		case format::wavpack: { return detail::wavpack_write(header, in, out, type, std::move(should_abort)); }
		default:              { return detail::ma_write(header, in, out, type, std::move(should_abort)); }
	}
}

auto write(const audiorw::header& header, concepts::frame_input_stream auto* in, concepts::byte_output_stream auto* out, storage_type type) -> operation_result {
	return write(header, in, out, type, detail::fn_always(false));
}

auto write(const audiorw::item& item, const std::filesystem::path& path, storage_type type, concepts::should_abort_fn auto should_abort) -> operation_result {
	auto in  = audiorw::item_frame_input_stream{&item};
	auto out = audiorw::file_byte_output_stream{path};
	return write(item.header, &in, &out, type, should_abort);
}

} // audiorw
