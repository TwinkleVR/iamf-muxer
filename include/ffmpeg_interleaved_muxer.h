#pragma once

#include "video_mapping.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace immersive_audio_muxer {

class StandaloneIamfReader;

// Remuxes a source container and a standalone IAMF elementary stream through
// an explicit packet scheduler followed by one libavformat MP4 muxer. The
// scheduler keeps at most one pending packet per mapped stream and writes an
// already timestamp-ordered sequence; input payloads are stream-copied, and
// no decoder or encoder is opened by this class.
struct FfmpegInterleavedMuxOptions {
    std::string source_path;
    std::string iamf_path;
    std::string output_path;
    int requested_video_stream_index = -1;
    bool keep_source_audio = false;
    VideoMappingOptions video_mapping;
    VideoSpatialMetadata source_spatial_metadata;
    // Source IAMF tracks are filtered from FFmpeg, validated with iamf-tools,
    // then packetized through the same scheduler as every other output track.
    bool route_source_iamf_tracks_through_probe = false;
    std::vector<const StandaloneIamfReader*> source_iamf_tracks;
    // Retained for API compatibility and copied to the output format context.
    // The explicit scheduler above bounds its own pending state independently
    // of libavformat's internal interleave queue.
    int64_t max_interleave_delta = 500000;
    std::function<bool()> cancellation_requested;
};

class FfmpegInterleavedMuxer final {
public:
    static bool HasMp4Muxer();
    static bool Write(const FfmpegInterleavedMuxOptions& options, std::string* error);
};

}  // namespace immersive_audio_muxer
