```c++
#include <audiorw.hpp>

auto fn_should_abort(std::stop_token stop) {
  return [stop] { return stop.stop_requested(); }
}
```

## Read a WAV file into memory
```c++
auto example(std::filesystem::path path, std::stop_token stop) -> void {
  if (const auto item = audiorw::read(path, audiorw::format_hint::try_wav_only, fn_should_abort(stop))) {
    const auto header = item->header;
    const auto& frames = item->frames;
    // ...
  }
}

```

## Try to read an audio file into memory but you don't know for sure what type of audio file it is
```c++
auto example(std::filesystem::path path, std::stop_token stop) -> void {
  if (const auto hint = audiorw::make_format_hint(path)) {
    if (const auto item = audiorw::read(path, *hint, fn_should_abort(stop))) {
      const auto header = item->header;
      const auto& frames = item->frames;
      // ...
    }
  }
}

```

# Read an MP3 from other bytes that you already have in memory and write it to a WAV file
```c++
auto example(std::span<const std::byte> mp3_bytes, std::filesystem::path out_file, std::stop_token stop) -> void {
  auto in  = audiorw::stream::item::from(mp3_bytes, audiorw::format_hint::try_mp3_only);
  auto out = audiorw::stream::bytes::to(out_file);
  auto header = in.get_header();
  header.format = audiorw::format::wav;
  const auto result = audiorw::write(header, &in, &out, audiorw::storage_type::float_, fn_should_abort(stop));
  if (result == operation_result::abort) {
    // Writing was aborted.
  }
  else {
    assert (result == operation_result::success);
  }
}
```

# Encode audio frames as WavPack and stream to a vector of bytes
```c++
auto example(const ads::fully_dynamic<float>* frames, std::stop_token stop) -> std::vector<std::byte> {
  auto bytes = std::vector<std::byte>{};
  auto in    = audiorw::stream::frames::from(frames);
  auto out   = audiorw::stream::bytes::to(&bytes);
  audiorw::header header;
  header.format        = audiorw::format::wavpack;
  header.channel_count = frames->get_channel_count();
  header.frame_count   = frames->get_frame_count();
  header.SR            = 44100;
  header.bit_depth     = 32;
  const auto result = audiorw::write(header, &in, &out, audiorw::storage_type::float_, fn_should_abort(stop));
  if (result == operation_result::abort) {
    // Writing was aborted.
    return {};
  }
  else {
    assert (result == operation_result::success);
    return bytes;
  }
}
```
