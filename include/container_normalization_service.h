#pragma once

#include <functional>
#include <string>

namespace immersive_audio_muxer {

// Converts the input into a temporary flat MP4. Video and compatible streams
// are copied; SubRip subtitles are adapted to MOV_TEXT. Source audio is
// included only when requested, while attachments are skipped.
class ContainerNormalizationService final {
public:
    static bool NormalizeToMp4(const std::string& input_path, const std::string& output_path, int requested_video_stream_index,
                               bool keep_source_audio, int* normalized_video_stream_index, std::string* error,
                               const std::function<bool()>& cancellation_requested = {});
};

}  // namespace immersive_audio_muxer
