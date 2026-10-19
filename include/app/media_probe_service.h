#pragma once

#include "app/project_types.h"

#include <filesystem>
#include <string>

namespace immersive_audio_muxer::app {

class MediaProbeService final {
public:
    static bool Probe(const std::filesystem::path& path, MediaProbeInfo* info, std::string* error);
};

}  // namespace immersive_audio_muxer::app
