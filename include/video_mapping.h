#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace immersive_audio_muxer {

enum class VideoMappingPreset : int {
    kPreserveSource = 0,
    kNone = 1,
    kVr180Mono = 2,
    kVr180SideBySide = 3,
    kVr180TopBottom = 4,
    kVr360Mono = 5,
    kVr360SideBySide = 6,
    kVr360TopBottom = 7,
};

struct VideoMappingOptions {
    VideoMappingPreset preset = VideoMappingPreset::kPreserveSource;
};

enum class VideoProjectionType {
    kEquirectangular,
    kCubemap,
};

enum class VexuProjectionType {
    kRectilinear,
    kEquirectangular,
    kHalfEquirectangular,
    kFisheye,
    kParametricImmersive,
};

enum class SourceMp4SpatialMetadataState {
    kNotInspected,
    kNotMp4,
    kParsed,
    kFailed,
};

struct VideoSpatialMetadata {
    bool has_stereo = false;
    uint8_t stereo_mode = 0;
    bool has_spherical = false;
    VideoProjectionType projection = VideoProjectionType::kEquirectangular;
    int32_t yaw = 0;
    int32_t pitch = 0;
    int32_t roll = 0;
    uint32_t bound_top = 0;
    uint32_t bound_bottom = 0;
    uint32_t bound_left = 0;
    uint32_t bound_right = 0;
    uint32_t cubemap_layout = 0;
    uint32_t cubemap_padding = 0;
    bool has_vexu_projection = false;
    VexuProjectionType vexu_projection = VexuProjectionType::kEquirectangular;
    SourceMp4SpatialMetadataState source_mp4_metadata_state =
        SourceMp4SpatialMetadataState::kNotInspected;
    bool preserve_source_sample_entry_boxes = false;
    std::string source_mp4_metadata_error;
    std::vector<std::vector<uint8_t>> source_sample_entry_spatial_boxes;
};

const char* VideoMappingPresetLabel(VideoMappingPreset preset);
bool ParseVideoMappingPreset(const std::string& value, VideoMappingPreset* preset);

bool BuildVideoSpatialMetadata(const VideoMappingOptions& options,
                               const VideoSpatialMetadata& source,
                               VideoSpatialMetadata* output,
                               std::string* error);

std::vector<uint8_t> BuildStereo3DBox(const VideoSpatialMetadata& metadata);
std::vector<uint8_t> BuildSphericalVideoBox(const VideoSpatialMetadata& metadata);
std::vector<uint8_t> BuildVexuProjectionChildBox(const VideoSpatialMetadata& metadata);
bool BuildVideoSampleEntrySpatialBoxes(const VideoSpatialMetadata& metadata,
                                      std::vector<std::vector<uint8_t>>* boxes,
                                      std::string* error);

}  // namespace immersive_audio_muxer
