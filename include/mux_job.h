#pragma once

#include "in_process_iamf_encoder.h"
#include "standalone_iamf_reader.h"
#include "video_mapping.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace immersive_audio_muxer {

enum class MuxJobStage {
    kNone,
    kPreparingEncoder,
    kOpeningVideo,
    kReadingIamf,
    kValidatingIamf,
    kWritingOutput,
    kCompleted,
    kCancelled,
};

struct MuxJobEvent {
    MuxJobStage stage = MuxJobStage::kNone;
    std::string message;
};

struct MuxJobRequest {
    std::filesystem::path iamf_path;
    std::optional<InProcessIamfEncoderOptions> encoder_options;
    std::filesystem::path video_path;
    std::filesystem::path output_path;
    int video_stream_index = -1;
    bool keep_mp4_audio = false;
    VideoMappingOptions video_mapping;
    bool overwrite_existing_files = true;
    bool dry_run = false;
    std::function<bool()> cancellation_requested;
    std::function<void(const MuxJobEvent&)> event_callback;
};

struct MuxJobResult {
    MuxJobStage failed_stage = MuxJobStage::kNone;
    std::filesystem::path iamf_path;
    IamfIndexSummary iamf_summary{};
    std::size_t configuration_box_size = 0;
    bool wrote_output = false;
    bool iamf_only_output = false;
};

class MuxJob final {
public:
    static bool Run(const MuxJobRequest& request, MuxJobResult* result, std::string* error);
};

}  // namespace immersive_audio_muxer
