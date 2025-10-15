#define NOMINMAX
#define MINIAUDIO_IMPLEMENTATION
#include "audiorw.hpp"
#include "miniaudio.h"

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
	table[size_t(format::flac)]    = { .format = format::flac,    .ext = ".FLAC", .hint_only = format_hint::try_flac_only,    .hint_all = format_hint::try_flac_first };
	table[size_t(format::mp3)]     = { .format = format::mp3,     .ext = ".MP3",  .hint_only = format_hint::try_mp3_only,     .hint_all = format_hint::try_mp3_first };
	table[size_t(format::wav)]     = { .format = format::wav,     .ext = ".WAV",  .hint_only = format_hint::try_wav_only,     .hint_all = format_hint::try_wav_first };
	table[size_t(format::wavpack)] = { .format = format::wavpack, .ext = ".WV",   .hint_only = format_hint::try_wavpack_only, .hint_all = format_hint::try_wavpack_first };
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

scope_wavpack_writer::scope_wavpack_writer(const audiorw::header& header, storage_type type, WavpackBlockOutput blockout, void* user_data)
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

auto ma_to_std_seek_mode(ma_seek_origin origin) -> std::ios_base::seekdir {
	switch (origin) {
		case ma_seek_origin_start:   { return std::ios_base::beg; }
		case ma_seek_origin_current: { return std::ios_base::cur; }
		case ma_seek_origin_end:     { return std::ios_base::end; }
		default:                     { throw std::runtime_error{"Invalid seek origin"}; }
	}
}

auto wavpack_to_std_seek_mode(int mode) -> std::ios_base::seekdir {
	switch (mode) {
		case SEEK_SET: { return std::ios_base::beg; }
		case SEEK_CUR: { return std::ios_base::cur; }
		case SEEK_END: { return std::ios_base::end; }
		default:       { throw std::runtime_error{"Invalid seek mode"}; }
	}
}

auto make_wavpack_config(const audiorw::header& header, storage_type type) -> WavpackConfig {
    auto config = WavpackConfig{0};
	config.bytes_per_sample = header.bit_depth / 8;
	config.bits_per_sample  = header.bit_depth;
	config.channel_mask     = get_wavpack_channel_mask(header.channel_count);
	config.num_channels     = header.channel_count.value;
	config.sample_rate      = header.SR;
	config.float_norm_exp   = get_wavpack_float_norm_exp(type);
	return config;
}

[[nodiscard]] static
auto to_upper(std::string str) -> std::string {
	std::transform(str.begin(), str.end(), str.begin(), [](char c) { return std::toupper(c); });
	return str;
}

[[nodiscard]] static
auto prepend_dot(std::string str) -> std::string {
	return std::string{"."} + str;
}

[[nodiscard]] static
auto make_search_ext(std::string str) -> std::string {
	str = to_upper(str);
	if (str[0] != '.') {
		str = prepend_dot(str);
	}
	return str;
}

[[nodiscard]] static
auto find_format_info(std::string_view ext) -> std::optional<format_info> {
	if (ext.empty()) {
		return std::nullopt;
	}
	const auto search_ext = make_search_ext(std::string{ext});
	auto match = [search_ext](const format_info& info) {
		return info.ext == search_ext;
	};
	if (const auto pos = std::ranges::find_if(FORMAT_INFO, match); pos != FORMAT_INFO.end()) {
		return *pos;
	}
    return std::nullopt;
}

} // audiorw::detail

namespace audiorw {

auto get_known_file_extensions() -> std::array<std::string_view, 4> {
	std::array<std::string_view, 4> out;
	std::ranges::transform(detail::FORMAT_INFO, std::begin(out), &detail::format_info::ext);
	return out;
}

auto make_format_hint(const std::filesystem::path& file_path, bool try_all) -> std::optional<format_hint> {
	const auto ext = file_path.extension();
	if (const auto info = detail::find_format_info(ext.string())) {
		return try_all ? info->hint_all : info->hint_only;
	}
	return std::nullopt;
}

} // audiorw
