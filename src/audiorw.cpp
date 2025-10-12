#define NOMINMAX
#define MINIAUDIO_IMPLEMENTATION
#include "audiorw.hpp"
#include "miniaudio.h"
#include <variant>

namespace audiorw::detail {

struct format_info {
    audiorw::format format;
    std::string_view ext;
    audiorw::format_hint hint_only;
    audiorw::format_hint hint_all;
};

using format_info_table = std::array<format_info, 4>;

[[nodiscard]] constexpr
auto make_format_info_table() -> format_info_table {
    format_info_table table;
    table[size_t(format::flac)]    =  { .format = format::flac,    .ext = ".FLAC", .hint_only = format_hint::try_flac_only,    .hint_all = format_hint::try_flac_first };
    table[size_t(format::mp3)]     =  { .format = format::mp3,     .ext = ".MP3",  .hint_only = format_hint::try_mp3_only,     .hint_all = format_hint::try_mp3_first };
    table[size_t(format::wav)]     =  { .format = format::wav,     .ext = ".WAV",  .hint_only = format_hint::try_wav_only,     .hint_all = format_hint::try_wav_first };
    table[size_t(format::wavpack)] =  { .format = format::wavpack, .ext = ".WV",   .hint_only = format_hint::try_wavpack_only, .hint_all = format_hint::try_wavpack_first };
    return table;
}

static constexpr auto FORMAT_INFO = make_format_info_table();

[[nodiscard]] static
auto get_bit_depth(ma_format format) -> int {
	switch (format) {
		case ma_format_f32: { return 32; }
		case ma_format_s16: { return 16; }
		case ma_format_s24: { return 24; }
		case ma_format_s32: { return 32; }
		case ma_format_u8:  { return 8; }
		default:            { throw std::runtime_error{"Invalid audio format"}; }
	}
}

[[nodiscard]] static
auto make_tmp_file_path(std::filesystem::path path) -> std::filesystem::path {
	path += ".tmp";
	return path;
}

[[nodiscard]] static
auto get_format(const ma_decoder& decoder) -> format {
	if (decoder.pBackendVTable == &g_ma_decoding_backend_vtable_flac ) { return format::flac; }
	if (decoder.pBackendVTable == &g_ma_decoding_backend_vtable_mp3)   { return format::mp3; }
	if (decoder.pBackendVTable == &g_ma_decoding_backend_vtable_wav)   { return format::wav; }
	throw std::runtime_error{"Invalid audio format"};
}

atomic_file_writer::atomic_file_writer(const std::filesystem::path& path)
	: path_{path}
	, tmp_path_{make_tmp_file_path(path)}
	, file_{tmp_path_, std::ios::binary}
{
}

atomic_file_writer::~atomic_file_writer() {
	try {
		if (file_ && !commit_flag_) {
			file_.close();
			std::filesystem::remove(tmp_path_);
		}
	}
	catch (...) {}
}

auto atomic_file_writer::commit() -> void {
	if (!commit_flag_) {
		file_.flush();
		file_.close();
		std::filesystem::rename(tmp_path_, path_);
		commit_flag_ = true;
	}
}

auto atomic_file_writer::stream() -> std::ofstream& {
	return file_;
}

scope_ma_decoder::scope_ma_decoder(ma_decoder_read_proc on_read, ma_decoder_seek_proc on_seek, void* user_data) {
	if (ma_decoder_init(on_read, on_seek, user_data, nullptr, &decoder_) != MA_SUCCESS) {
		throw std::runtime_error{"Failed to initialize decoder"};
	}
}

scope_ma_decoder::~scope_ma_decoder() {
	ma_decoder_uninit(&decoder_);
}

auto scope_ma_decoder::get_header(audiorw::format format) const -> header {
	ma_format dec_format;
	ma_uint32 dec_channels;
	ma_uint32 dec_SR;
	ma_uint64 dec_length;
	if (ma_decoder_get_data_format(const_cast<ma_decoder*>(&decoder_), &dec_format, &dec_channels, &dec_SR, nullptr, 0) != MA_SUCCESS) {
		throw std::runtime_error{"Failed to get data format from decoder"};
	}
	// NOTE: For MP3s this will have to decode the entire file at this point
	if (ma_decoder_get_length_in_pcm_frames(const_cast<ma_decoder*>(&decoder_), &dec_length) != MA_SUCCESS) {
		throw std::runtime_error{"Failed to get frame count from decoder"};
	}
	header out;
	out.SR            = dec_SR;
	out.bit_depth     = get_bit_depth(dec_format);
	out.format        = format;
	out.channel_count = {dec_channels};
	out.frame_count   = {dec_length};
	return out;
}

auto scope_ma_decoder::get_header() const -> header {
	return get_header(get_format(decoder_));
}

auto scope_ma_decoder::read_pcm_frames(void* frames, ma_uint64 frame_count) -> ma_uint64 {
	ma_uint64 frames_read = 0;
	if (ma_decoder_read_pcm_frames(&decoder_, frames, frame_count, &frames_read) != MA_SUCCESS) {
		throw std::runtime_error{"Failed to read PCM frames"};
	}
	return frames_read;
}

auto scope_ma_decoder::seek_to_pcm_frame(ma_uint64 frame) -> ma_result {
	return ma_decoder_seek_to_pcm_frame(&decoder_, frame);
}

scope_ma_encoder::scope_ma_encoder(ma_encoder_write_proc on_write, ma_encoder_seek_proc on_seek, void* user_data, const ma_encoder_config& config) {
	if (ma_encoder_init(on_write, on_seek, user_data, &config, &encoder_) != MA_SUCCESS) {
		throw std::runtime_error{"Failed to initialize encoder"};
	}
}

scope_ma_encoder::~scope_ma_encoder() {
	ma_encoder_uninit(&encoder_);
}

auto scope_ma_encoder::write_pcm_frames(const void* frames, ma_uint64 frame_count) -> ma_uint64 {
	ma_uint64 frames_written = 0;
	if (ma_encoder_write_pcm_frames(&encoder_, frames, frame_count, &frames_written) != MA_SUCCESS) {
		throw std::runtime_error{"Failed to write PCM frames"};
	}
	return frames_written;
}

scope_wavpack_reader::scope_wavpack_reader(WavpackStreamReader64 stream, void* user_data)
	: stream_reader_{stream}
{
	int flags = OPEN_2CH_MAX;
	char error[80];
	context_ = WavpackOpenFileInputEx64(&stream_reader_, user_data, nullptr, error, flags, 0);
	if (!context_) {
		throw std::runtime_error{error};
	}
	header_.format        = format::wavpack;
	header_.bit_depth     = WavpackGetBitsPerSample(context_);
	header_.channel_count = {static_cast<uint64_t>(WavpackGetNumChannels(context_))};
	header_.frame_count   = {static_cast<uint64_t>(WavpackGetNumSamples64(context_))};
	header_.SR            = WavpackGetSampleRate(context_);
	mode_                 = WavpackGetMode(context_);
}

scope_wavpack_reader::~scope_wavpack_reader() {
	WavpackCloseFile(context_);
}

scope_wavpack_writer::scope_wavpack_writer(audiorw::header header, storage_type type, WavpackBlockOutput blockout, void* user_data)
	: context_{WavpackOpenFileOutput(blockout, user_data, nullptr)}
{
    auto config = make_wavpack_config(header, type);
	if (!WavpackSetConfiguration64(context_, &config, header.frame_count.value, nullptr)) {
		throw std::runtime_error(WavpackGetErrorMessage(context_));
	}
	if (!WavpackPackInit(context_)) {
		throw std::runtime_error(WavpackGetErrorMessage(context_));
	}
}

scope_wavpack_writer::~scope_wavpack_writer() {
	WavpackCloseFile(context_);
}

using stream = std::variant<scope_ma_decoder, scope_wavpack_reader>;

[[nodiscard]] static
auto ma_try_stream_open(std::ifstream* file) -> std::optional<stream> {
	try         { return scope_ma_decoder{ma_on_read, ma_on_seek, file}; }
	catch (...) { return std::nullopt; }
}

[[nodiscard]] static
auto wavpack_try_stream_open(std::ifstream* file) -> std::optional<stream> {
	try         { return scope_wavpack_reader{make_wavpack_stream_reader(), file}; }
	catch (...) { return std::nullopt; }
}

[[nodiscard]] static
auto try_stream_open(std::ifstream* file, audiorw::format format) -> std::optional<stream> {
	switch (format) {
		case audiorw::format::wavpack: { return wavpack_try_stream_open(file); }
		default:                       { return ma_try_stream_open(file); }
	}
}

[[nodiscard]] static
auto stream_open(std::ifstream* file, format_hint hint) -> stream {
	const auto formats_to_try = detail::get_formats_to_try(hint);
	for (auto format : formats_to_try) {
		if (auto stream = try_stream_open(file, format)) {
			return std::move(stream).value();
		}
	}
	throw std::runtime_error{"Failed to open stream"};
}

[[nodiscard]] static
auto get_header(scope_ma_decoder* stream) -> header {
	return stream->get_header();
}

[[nodiscard]] static
auto stream_read_frames(scope_ma_decoder* stream, float* buffer, ads::frame_count frames_to_read) -> ads::frame_count {
	return {stream->read_pcm_frames(buffer, frames_to_read.value)};
}

[[nodiscard]] static
auto stream_seek(scope_ma_decoder* stream, ads::frame_idx pos) -> bool {
	return stream->seek_to_pcm_frame(pos.value) == MA_SUCCESS;
}

[[nodiscard]] static
auto stream_read_float_frames(scope_wavpack_reader* stream, float* buffer, ads::frame_count frames_to_read) -> ads::frame_count {
	auto buffer_as_ints = reinterpret_cast<int32_t*>(buffer);
	return {WavpackUnpackSamples(stream->context(), buffer_as_ints, frames_to_read.value)};
}

[[nodiscard]] static
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

[[nodiscard]] static
auto get_header(scope_wavpack_reader* stream) -> header {
	return stream->get_header();
}

[[nodiscard]] static
auto stream_read_frames(scope_wavpack_reader* stream, float* buffer, ads::frame_count frames_to_read) -> ads::frame_count {
	const auto float_mode = (stream->mode() & MODE_FLOAT) == MODE_FLOAT;
	if (float_mode) { return stream_read_float_frames(stream, buffer, frames_to_read); }
	else            { return stream_read_int_frames(stream, buffer, frames_to_read); }
}

[[nodiscard]] static
auto stream_seek(scope_wavpack_reader* stream, ads::frame_idx pos) -> bool {
	return WavpackSeekSample64(stream->context(), pos.value) == 1;
}

struct streamer_impl {
	streamer_impl(const std::filesystem::path& path, format_hint hint);
    auto get_header() -> header;
	auto read_frames(float* buffer, ads::frame_count frames_to_read) -> ads::frame_count;
	auto seek(ads::frame_idx pos) -> bool;
private:
	std::ifstream file_;
	stream stream_;
};

streamer_impl::streamer_impl(const std::filesystem::path& path, format_hint hint)
	: file_{path, std::ios::binary}
	, stream_{stream_open(&file_, hint)}
{
}

auto streamer_impl::get_header() -> header {
	return std::visit([](auto& stream){ return detail::get_header(&stream); }, stream_);
}

auto streamer_impl::read_frames(float* buffer, ads::frame_count frames_to_read) -> ads::frame_count {
	return std::visit([buffer, frames_to_read](auto& stream){ return stream_read_frames(&stream, buffer, frames_to_read); }, stream_);
}

auto streamer_impl::seek(ads::frame_idx pos) -> bool {
	return std::visit([pos](auto& stream){ return stream_seek(&stream, pos); }, stream_);
}

auto get_formats_to_try(format_hint hint) -> formats_to_try {
	auto formats = formats_to_try{};
	switch (hint) {
		case format_hint::try_flac_first:    { return { format::flac, format::wav, format::mp3, format::wavpack }; }
		case format_hint::try_mp3_first:     { return { format::mp3, format::wav, format::flac, format::wavpack }; }
		case format_hint::try_wav_first:     { return { format::wav, format::mp3, format::flac, format::wavpack }; }
		case format_hint::try_wavpack_first: { return { format::wavpack, format::wav, format::mp3, format::flac }; }
		case format_hint::try_flac_only:     { return { format::flac }; }
		case format_hint::try_mp3_only:      { return { format::mp3 }; }
		case format_hint::try_wav_only:      { return { format::wav }; }
		case format_hint::try_wavpack_only:  { return { format::wavpack }; }
		default:                             { throw std::runtime_error{"Invalid audio format"}; }
	}
	return formats;
}

[[nodiscard]] static
auto get_wavpack_channel_mask(ads::channel_count chs) -> int {
	static constexpr auto CFG_MONO   = 4;
	static constexpr auto CFG_STEREO = 3;
	return chs == 1 ? CFG_MONO : CFG_STEREO;
}

[[nodiscard]] static
auto get_wavpack_float_norm_exp(storage_type type) -> int {
	static constexpr auto NORMALIZED_FLOAT   = 127;
	static constexpr auto UNNORMALIZED_FLOAT = 128;
	switch (type) {
		case storage_type::float_:            { return UNNORMALIZED_FLOAT; }
		case storage_type::normalized_float_: { return NORMALIZED_FLOAT; }
		default:                              { return 0; }
	}
}

auto to_ma_encoding_format(audiorw::format format) -> ma_encoding_format {
	switch (format) {
		case audiorw::format::flac: { return ma_encoding_format_flac; }
		case audiorw::format::mp3:  { return ma_encoding_format_mp3; }
		case audiorw::format::wav:  { return ma_encoding_format_wav; }
		default:                    { throw std::runtime_error{"Invalid audio format"}; }
	}
}

auto to_ma_format(int bit_depth, storage_type type) -> ma_format {
	switch (bit_depth) {
		case 8:  { return ma_format_u8; }
		case 16: { return ma_format_s16; }
		case 24: { return ma_format_s24; }
		case 32: { return type == storage_type::int_ ? ma_format_s32 : ma_format_f32; }
		default: { throw std::runtime_error{"Invalid audio format"}; }
	}
}

auto to_std_origin(ma_seek_origin origin) -> std::ios_base::seekdir {
	switch (origin) {
		case ma_seek_origin_start:   { return std::ios_base::beg; }
		case ma_seek_origin_current: { return std::ios_base::cur; }
		case ma_seek_origin_end:     { return std::ios_base::end; }
		default:                     { throw std::runtime_error{"Invalid seek origin"}; }
	}
}

auto wavpack_to_std_origin(int mode) -> std::ios_base::seekdir {
	switch (mode) {
		case SEEK_SET: { return std::ios_base::beg; }
		case SEEK_CUR: { return std::ios_base::cur; }
		case SEEK_END: { return std::ios_base::end; }
		default:       { throw std::runtime_error{"Invalid seek mode"}; }
	}
}

auto wavpack_write_float_chunk(WavpackContext* context, ads::interleaved<float>* frames, size_t chunk_size) -> void {
	if (!WavpackPackSamples(context, reinterpret_cast<int32_t*>(frames->data()), chunk_size)) {
		throw std::runtime_error("Write error");
	}
}

auto wavpack_write_int_chunk(WavpackContext* context, ads::channel_count chs, int int_scale, ads::interleaved<float>* frames, size_t chunk_size) -> void {
	static_assert (sizeof(float) == sizeof(int32_t));
	auto buffer_as_ints = reinterpret_cast<int32_t*>(frames->data());
	for (int i = 0; i < chs.value * chunk_size; i++) {
		buffer_as_ints[i] = static_cast<int32_t>(double(frames->at(i)) * int_scale);
	}
	if (!WavpackPackSamples(context, buffer_as_ints, chunk_size)) {
		throw std::runtime_error("Write error");
	}
}

auto make_wavpack_config(audiorw::header header, storage_type type) -> WavpackConfig {
    auto config = WavpackConfig{0};
	config.bytes_per_sample = header.bit_depth / 8;
	config.bits_per_sample  = header.bit_depth;
	config.channel_mask     = get_wavpack_channel_mask(header.channel_count);
	config.num_channels     = header.channel_count.value;
	config.sample_rate      = header.SR;
	config.float_norm_exp   = get_wavpack_float_norm_exp(type);
	return config;
}

auto wavpack_write_blockout(void* puserdata, void* data, int32_t bcount) -> int {
	auto& writer = *reinterpret_cast<atomic_file_writer*>(puserdata);
	const auto char_data = reinterpret_cast<const char*>(data);
	writer.stream().write(char_data, bcount);
	return writer.stream().fail() ? 0 : 1;
}

auto make_wavpack_stream_reader() -> WavpackStreamReader64 {
	WavpackStreamReader64 sr;
	sr.can_seek = [](void* puserdata) -> int {
		if (!puserdata) return 0;
		const auto& file = *reinterpret_cast<std::ifstream*>(puserdata);
		return file.fail() ? 0 : 1;
	};
	sr.close = [](void* puserdata) -> int {
		auto& file = *reinterpret_cast<std::ifstream*>(puserdata);
		file.close();
		return 1;
	};
	sr.get_length = [](void* puserdata) -> int64_t {
		auto& file = *reinterpret_cast<std::ifstream*>(puserdata);
		const auto pos = file.tellg();
		file.seekg(0, std::ios::end);
		const auto length = file.tellg();
		file.seekg(pos, std::ios::beg);
		return length;
	};
	sr.get_pos = [](void* puserdata) -> int64_t {
		auto& file = *reinterpret_cast<std::ifstream*>(puserdata);
		return file.tellg();
	};
	sr.push_back_byte = [](void* puserdata, int c) -> int {
		auto& file = *reinterpret_cast<std::ifstream*>(puserdata);
		file.putback(c);
		return file.fail() ? 0 : c;
	};
	sr.read_bytes = [](void* puserdata, void* data, int32_t bcount) -> int32_t {
		if (bcount < 1) return 0;
		auto& file = *reinterpret_cast<std::ifstream*>(puserdata);
		auto char_data = reinterpret_cast<char*>(data);
		file.read(char_data, bcount);
		return file.fail() ? 0 : file.gcount();
	};
	sr.set_pos_abs = [](void* puserdata, int64_t pos) -> int {
		auto& file = *reinterpret_cast<std::ifstream*>(puserdata);
		file.seekg(pos, std::ios_base::beg);
		return 0;
	};
	sr.set_pos_rel = [](void* puserdata, int64_t delta, int mode) -> int {
		auto& file = *reinterpret_cast<std::ifstream*>(puserdata);
		file.seekg(delta, wavpack_to_std_origin(mode));
		return 0;
	};
	return sr;
}

[[nodiscard]] static
auto find_format_info(std::string_view extension) -> std::optional<format_info> {
    auto match = [extension](const format_info& info) { return info.ext == extension; };
    if (const auto pos = std::ranges::find_if(FORMAT_INFO, match); pos != FORMAT_INFO.end()) {
        return *pos;
    }
    return std::nullopt;
}

auto ma_on_read(ma_decoder* decoder, void* buffer, size_t bytes_to_read, size_t* bytes_read) -> ma_result {
	auto& file = *reinterpret_cast<std::ifstream*>(decoder->pUserData);
	const auto char_data = reinterpret_cast<char*>(buffer);
	file.read(char_data, bytes_to_read);
	*bytes_read = file.gcount();
	return file.fail() ? MA_ERROR : MA_SUCCESS;
}

auto ma_on_seek(ma_decoder* decoder, ma_int64 offset, ma_seek_origin origin) -> ma_result {
	auto& file = *reinterpret_cast<std::ifstream*>(decoder->pUserData);
	file.seekg(offset, to_std_origin(origin));
	return file.fail() ? MA_ERROR : MA_SUCCESS;
}

} // audiorw::detail

namespace audiorw {

streamer::streamer(const std::filesystem::path& path, format_hint hint)
    : impl_{std::make_unique<detail::streamer_impl>(path, hint)}
{
}

auto streamer::get_header() const -> header {
    return impl_->get_header();
}

auto streamer::read_frames(float* buffer, ads::frame_count frames_to_read) -> ads::frame_count {
    return impl_->read_frames(buffer, frames_to_read);
}

auto streamer::seek(ads::frame_idx pos) -> bool {
    return impl_->seek(pos);
}

auto make_format_hint(const std::filesystem::path& file_path, bool try_all) -> format_hint {
    const auto ext = file_path.extension();
    if (const auto info = detail::find_format_info(ext.string())) {
        return try_all ? info->hint_all : info->hint_only;
    }
    return format_hint::try_wav_only;
}

} // audiorw
