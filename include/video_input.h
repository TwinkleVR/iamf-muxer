#pragma once

#include "video_mapping.h"

#include <functional>
#include <string>
#include <vector>

namespace immersive_audio_muxer {

struct VideoTrack {
    std::string source_path;
    std::string format_name;
    int stream_index = -1;
    VideoSpatialMetadata spatial_metadata;
};

class VideoInput final {
public:
    VideoInput() = default;
    ~VideoInput();
    VideoInput(const VideoInput&) = delete;
    VideoInput& operator=(const VideoInput&) = delete;

    using CancellationCallback = std::function<bool()>;
    // Reads the selected video stream's container metadata before
    // normalization or FFmpeg interleaved muxing.
    bool OpenMetadataOnly(const std::string& path, int requested_stream_index, std::string* error,
                          const CancellationCallback& cancellation_requested = {},
                          bool preserve_iamf_audio_tracks = false);
    const VideoTrack& track() const { return track_; }
    bool IsMp4Like() const;
    const std::vector<std::string>& validated_iamf_audio_paths() const {
        return validated_iamf_audio_paths_;
    }

private:
    VideoTrack track_;
    std::vector<std::string> validated_iamf_audio_paths_;
};
}  // namespace immersive_audio_muxer
