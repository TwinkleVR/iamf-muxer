#pragma once

#include <filesystem>
#include <string>

namespace immersive_audio_muxer::path_utils {

inline std::string ToUtf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
#if defined(__cpp_char8_t)
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
#else
    return value;
#endif
}

}  // namespace immersive_audio_muxer::path_utils
