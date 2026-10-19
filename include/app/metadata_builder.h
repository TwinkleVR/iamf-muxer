#pragma once

#include "app/project_types.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace immersive_audio_muxer::app {

enum class GeneratedCodec {
    kOpus,
    kLpcm,
    kAacLc,
    kFlac,
};

struct MetadataBuildOptions {
    ProjectAudioInput ambisonics;
    ProjectAudioInput head_locked_stereo;
    ProjectAudioInput channel_based;
    std::filesystem::path output_path;
    uint32_t mix_id = 42;
    int sample_rate = 48000;
    int samples_per_frame = 960;
    int opus_bitrate_per_channel = 64000;
    bool overwrite_existing_files = true;
    GeneratedCodec generated_codec = GeneratedCodec::kOpus;
    std::string mix_annotation = "Generated Mix Presentation";
};

class MetadataBuilder final {
public:
    static bool WriteAmbisonicsAndHeadLockedStereo(const MetadataBuildOptions& options,
                                                   std::string* error);
    static bool WriteChannelBased(const MetadataBuildOptions& options, std::string* error);
};

}  // namespace immersive_audio_muxer::app
