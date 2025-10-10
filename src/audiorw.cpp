#include "audiorw.hpp"
#include <stdexcept>

namespace audiorw::detail {

[[nodiscard]] static
auto make_tmp_file_path(std::filesystem::path path) -> std::filesystem::path {
    path += ".tmp";
    return path;
}

atomic_file_writer::atomic_file_writer(const std::filesystem::path& path)
    : path_{path}
    , tmp_path_{make_tmp_file_path(path)}
    , file_{tmp_path_, std::ios::binary}
{
}

atomic_file_writer::~atomic_file_writer() {
    try {
        if (!commit_flag_) {
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

auto to_ma_encoding_format(audiorw::format format) -> ma_encoding_format {
	switch (format) {
		case audiorw::format::flac: { return ma_encoding_format_flac; }
		case audiorw::format::mp3:  { return ma_encoding_format_mp3; }
		case audiorw::format::wav:  { return ma_encoding_format_wav; }
		default: { throw std::runtime_error{"Invalid audio format"}; }
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

} // audiorw::detail
