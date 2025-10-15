#include <stdexcept>
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

scope_ma_decoder::scope_ma_decoder(ma_decoder_read_proc on_read, ma_decoder_seek_proc on_seek, void* user_data, audiorw::format format)
	: decoder_{std::make_unique<ma_decoder>().release(), &ma_decoder_uninit}
{
	auto config = ma_decoder_config_init(ma_format_f32, 0, 0);
	config.encodingFormat = to_ma_encoding_format(format);
	if (ma_decoder_init(on_read, on_seek, user_data, &config, decoder_.get()) != MA_SUCCESS) {
		throw std::runtime_error{"Failed to initialize decoder"};
	}
}

auto scope_ma_decoder::get_header(audiorw::format format) const -> header {
	ma_format dec_format;
	ma_uint32 dec_channels;
	ma_uint32 dec_SR;
	ma_uint64 dec_length;
	if (ma_decoder_get_data_format(decoder_.get(), &dec_format, &dec_channels, &dec_SR, nullptr, 0) != MA_SUCCESS) {
		throw std::runtime_error{"Failed to get data format from decoder"};
	}
	// NOTE: For MP3s this will have to decode the entire file at this point
	if (ma_decoder_get_length_in_pcm_frames(decoder_.get(), &dec_length) != MA_SUCCESS) {
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
	return get_header(get_format(*decoder_));
}

auto scope_ma_decoder::read_pcm_frames(void* frames, ma_uint64 frame_count) -> ma_uint64 {
	ma_uint64 frames_read = 0;
	switch (ma_decoder_read_pcm_frames(decoder_.get(), frames, frame_count, &frames_read)) {
		case MA_SUCCESS: { return frames_read; }
		case MA_AT_END:  { return frames_read; }
		default:         { throw std::runtime_error{"Failed to read PCM frames"}; }
	}
}

auto scope_ma_decoder::seek_to_pcm_frame(ma_uint64 frame) -> ma_result {
	return ma_decoder_seek_to_pcm_frame(decoder_.get(), frame);
}

scope_ma_encoder::scope_ma_encoder(ma_encoder_write_proc on_write, ma_encoder_seek_proc on_seek, void* user_data, const ma_encoder_config& config)
	: encoder_{std::make_unique<ma_encoder>().release(), &ma_encoder_uninit}
{
	if (ma_encoder_init(on_write, on_seek, user_data, &config, encoder_.get()) != MA_SUCCESS) {
		throw std::runtime_error{"Failed to initialize encoder"};
	}
}

scope_ma_encoder::~scope_ma_encoder() {
	ma_encoder_uninit(encoder_.get());
}

auto scope_ma_encoder::write_pcm_frames(const void* frames, ma_uint64 frame_count) -> ma_uint64 {
	ma_uint64 frames_written = 0;
	if (ma_encoder_write_pcm_frames(encoder_.get(), frames, frame_count, &frames_written) != MA_SUCCESS) {
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
	if (context_) {
		WavpackCloseFile(context_);
	}
}

scope_wavpack_reader::scope_wavpack_reader(scope_wavpack_reader&& rhs) noexcept
	: stream_reader_{std::exchange(rhs.stream_reader_, {})}
	, context_{std::exchange(rhs.context_, {})}
	, header_{std::exchange(rhs.header_, {})}
	, mode_{std::exchange(rhs.mode_, {})}
{
}

scope_wavpack_reader& scope_wavpack_reader::operator=(scope_wavpack_reader&& rhs) noexcept {
	stream_reader_ = std::exchange(rhs.stream_reader_, {});
	context_ = std::exchange(rhs.context_, {});
	header_ = std::exchange(rhs.header_, {});
	mode_ = std::exchange(rhs.mode_, {});
	return *this;
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

auto to_operation_result(try_read_result r) -> operation_result {
	switch (r) {
		case try_read_result::abort:   { return operation_result::abort; }
		case try_read_result::success: { return operation_result::success; }
		default:                       { throw std::runtime_error{"Invalid try read result"}; }
	}
}

auto to_try_read_result(operation_result r) -> try_read_result {
	switch (r) {
		case operation_result::abort:   { return try_read_result::abort; }
		case operation_result::success: { return try_read_result::success; }
		default:                        { throw std::runtime_error{"Invalid operation result"}; }
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

auto stream_read_float_frames(scope_wavpack_reader* stream, float* buffer, ads::frame_count frames_to_read) -> ads::frame_count {
	auto buffer_as_ints = reinterpret_cast<int32_t*>(buffer);
	return {WavpackUnpackSamples(stream->context(), buffer_as_ints, frames_to_read.value)};
}

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

auto read_frames(scope_wavpack_reader* decoder, float* buffer, ads::frame_count frames_to_read) -> ads::frame_count {
	const auto float_mode = (decoder->mode() & MODE_FLOAT) == MODE_FLOAT;
	if (float_mode) { return stream_read_float_frames(decoder, buffer, frames_to_read); }
	else            { return stream_read_int_frames(decoder, buffer, frames_to_read); }
}

auto seek(scope_wavpack_reader* decoder, ads::frame_idx pos) -> bool {
	return WavpackSeekSample64(decoder->context(), pos.value) == 1;
}

auto get_header(const scope_wavpack_reader* decoder) -> header {
	return decoder->get_header();
}

auto get_header(const scope_ma_decoder* decoder) -> header {
	return decoder->get_header();
}

auto read_frames(scope_ma_decoder* decoder, float* buffer, ads::frame_count frames_to_read) -> ads::frame_count {
	return {decoder->read_pcm_frames(buffer, frames_to_read.value)};
}

auto seek(scope_ma_decoder* decoder, ads::frame_idx pos) -> bool {
	return decoder->seek_to_pcm_frame(pos.value) == MA_SUCCESS;
}

auto get_header(const detail::decoder* decoder) -> header {
	return std::visit([](auto& decoder){ return get_header(&decoder); }, *decoder);
}

auto read_frames(detail::decoder* decoder, float* buffer, ads::frame_count frames_to_read) -> ads::frame_count {
	return std::visit([buffer, frames_to_read](auto& decoder){ return read_frames(&decoder, buffer, frames_to_read); }, *decoder);
}

auto seek(detail::decoder* decoder, ads::frame_idx pos) -> bool {
	return std::visit([pos](auto& decoder){ return seek(&decoder, pos); }, *decoder);
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

byte_input_stream::byte_input_stream(std::span<const std::byte> bytes)
	: bytes_{bytes}
{
}

auto byte_input_stream::close() -> bool {
	return true;
}

auto byte_input_stream::get_length() -> std::optional<size_t> {
	return bytes_.size();
}

auto byte_input_stream::get_pos() -> size_t {
	return pos_;
}

auto byte_input_stream::push_back_byte(std::byte v) -> bool {
	pos_--;
	return true;
}

auto byte_input_stream::read_bytes(std::span<std::byte> buffer) -> size_t {
	const auto remaining_bytes = bytes_.size() - pos_;
	const auto n = std::min(buffer.size(), remaining_bytes);
	const auto beg = bytes_.data() + pos_;
	const auto end = beg + n;
	std::copy(beg, end, buffer.data());
	pos_ += n;
	return n;
}

auto byte_input_stream::seek(int64_t offset, std::ios::seekdir mode) -> bool {
	switch (mode) {
		case std::ios::beg: { pos_ = offset; return true; }
		case std::ios::cur: { pos_ += offset; return true; }
		case std::ios::end: { pos_ = bytes_.size() + offset; return true; }
	}
	return false;
}

//########################################################################################

file_byte_input_stream::file_byte_input_stream(const std::filesystem::path& path)
	: file_{path, std::ios::binary}
{
}

auto file_byte_input_stream::close() -> bool {
	file_.close();
	return true;
}

auto file_byte_input_stream::get_length() -> std::optional<size_t> {
	const auto pos = file_.tellg();
	file_.seekg(0, std::ios::end);
	const auto length = file_.tellg();
	file_.seekg(pos, std::ios::beg);
	return length;
}

auto file_byte_input_stream::get_pos() -> size_t {
	return file_.tellg();
}

auto file_byte_input_stream::push_back_byte(std::byte v) -> bool {
	file_.putback(static_cast<char>(v));
	return !file_.fail();
}

auto file_byte_input_stream::read_bytes(std::span<std::byte> buffer) -> size_t {
	if (buffer.size() < 1) return 0;
	auto char_buffer = reinterpret_cast<char*>(buffer.data());
	file_.read(char_buffer, buffer.size());
	return file_.fail() ? 0 : file_.gcount();
}

auto file_byte_input_stream::seek(int64_t offset, std::ios::seekdir mode) -> bool {
	file_.seekg(offset, mode);
	return !file_.fail();
}

//########################################################################################

byte_item_input_stream::byte_item_input_stream(std::span<const std::byte> bytes, format_hint hint)
	: in_{std::make_unique<byte_input_stream>(bytes)}
	, decoder_{detail::make_decoder(in_.get(), hint)}
{
}

auto byte_item_input_stream::get_header() const -> header {
	return detail::get_header(&decoder_);
}

auto byte_item_input_stream::read_frames(float* buffer, ads::frame_count frames_to_read) -> ads::frame_count {
	return detail::read_frames(&decoder_, buffer, frames_to_read);
}

auto byte_item_input_stream::seek(ads::frame_idx pos) -> bool {
	return detail::seek(&decoder_, pos);
}

//########################################################################################

file_item_input_stream::file_item_input_stream(const std::filesystem::path& path, format_hint hint)
	: in_{path}
	, decoder_{detail::make_decoder(&in_, hint)}
{
}

auto file_item_input_stream::get_header() const -> header {
	return detail::get_header(&decoder_);
}

auto file_item_input_stream::read_frames(float* buffer, ads::frame_count frames_to_read) -> ads::frame_count {
	return detail::read_frames(&decoder_, buffer, frames_to_read);
}

auto file_item_input_stream::seek(ads::frame_idx pos) -> bool {
	return detail::seek(&decoder_, pos);
}

//########################################################################################
ads_frame_input_stream::ads_frame_input_stream(const ads::fully_dynamic<float>* frames)
	: frames_{frames}
{
}

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
item_frame_input_stream::item_frame_input_stream(const audiorw::item* item)
	: stream_{&item->frames}
{
}

auto item_frame_input_stream::read_frames(std::span<float> buffer) -> ads::frame_count {
	return stream_.read_frames(buffer);
}

//########################################################################################

item_item_output_stream::item_item_output_stream(audiorw::item* item)
	: item_{item}
{
}

auto item_item_output_stream::seek(ads::frame_idx pos) -> bool {
	pos_ = 0;
	return true;
}

auto item_item_output_stream::write_header(audiorw::header header) -> void {
	item_->header = header;
	item_->frames = ads::make<float>(header.channel_count, header.frame_count);
}

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

file_byte_output_stream::file_byte_output_stream(const std::filesystem::path& path)
	: writer_{path}
{
}

auto file_byte_output_stream::commit() -> void {
	writer_.commit();
}

auto file_byte_output_stream::seek(int64_t offset, std::ios::seekdir mode) -> bool {
	auto& file = writer_.stream();
	file.seekp(offset, mode);
	return !file.fail();
}

auto file_byte_output_stream::write_bytes(std::span<const std::byte> buffer) -> size_t {
	const auto buffer_as_chars = reinterpret_cast<const char*>(buffer.data());
	auto& file = writer_.stream();
	file.write(buffer_as_chars, buffer.size());
	return file.fail() ? 0 : buffer.size();
}

//########################################################################################

} // audiorw
