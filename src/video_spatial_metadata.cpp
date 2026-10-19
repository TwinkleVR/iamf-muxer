#include "video_mapping.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <string_view>

namespace immersive_audio_muxer {
namespace {

void U32(std::vector<uint8_t>* output, uint32_t value) {
    output->push_back(static_cast<uint8_t>(value >> 24));
    output->push_back(static_cast<uint8_t>(value >> 16));
    output->push_back(static_cast<uint8_t>(value >> 8));
    output->push_back(static_cast<uint8_t>(value));
}

void Type(std::vector<uint8_t>* output, const char* type) {
    output->insert(output->end(), type, type + 4);
}

void Append(std::vector<uint8_t>* output, const std::vector<uint8_t>& value) {
    output->insert(output->end(), value.begin(), value.end());
}

std::vector<uint8_t> Box(const char* type, const std::vector<uint8_t>& payload) {
    if (payload.size() > 0xffffffffu - 8u)
        return {};
    std::vector<uint8_t> result;
    U32(&result, static_cast<uint32_t>(payload.size() + 8));
    Type(&result, type);
    Append(&result, payload);
    return result;
}

std::vector<uint8_t> FullBox(const char* type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> full;
    U32(&full, 0);
    Append(&full, payload);
    return Box(type, full);
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

uint8_t StereoModeForPreset(VideoMappingPreset preset) {
    switch (preset) {
        case VideoMappingPreset::kVr180Mono:
        case VideoMappingPreset::kVr360Mono:
            return 0;
        case VideoMappingPreset::kVr180TopBottom:
        case VideoMappingPreset::kVr360TopBottom:
            return 1;
        case VideoMappingPreset::kVr180SideBySide:
        case VideoMappingPreset::kVr360SideBySide:
            return 2;
        case VideoMappingPreset::kPreserveSource:
        case VideoMappingPreset::kNone:
            return 0;
    }
    return 0;
}

bool IsVr180(VideoMappingPreset preset) {
    return preset == VideoMappingPreset::kVr180Mono || preset == VideoMappingPreset::kVr180SideBySide ||
           preset == VideoMappingPreset::kVr180TopBottom;
}

struct RawBoxView {
    size_t offset = 0;
    size_t size = 0;
    size_t header_size = 0;
    std::array<char, 4> type{};
};

uint32_t ReadU32(const std::vector<uint8_t>& bytes, size_t offset) {
    return (static_cast<uint32_t>(bytes[offset]) << 24) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<uint32_t>(bytes[offset + 3]);
}

uint64_t ReadU64(const std::vector<uint8_t>& bytes, size_t offset) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i)
        value = (value << 8) | bytes[offset + i];
    return value;
}

bool ParseRawBox(const std::vector<uint8_t>& bytes, size_t offset, size_t end, RawBoxView* box) {
    if (box == nullptr || offset > end || end - offset < 8)
        return false;
    const uint32_t size32 = ReadU32(bytes, offset);
    const size_t header_size = size32 == 1 ? 16 : 8;
    if (end - offset < header_size)
        return false;
    uint64_t size = size32;
    if (size32 == 1)
        size = ReadU64(bytes, offset + 8);
    else if (size32 == 0)
        size = end - offset;
    if (size < header_size || size > end - offset || size > std::numeric_limits<size_t>::max())
        return false;
    box->offset = offset;
    box->size = static_cast<size_t>(size);
    box->header_size = header_size;
    std::copy(bytes.begin() + static_cast<ptrdiff_t>(offset + 4),
              bytes.begin() + static_cast<ptrdiff_t>(offset + 8), box->type.begin());
    return true;
}

bool IsRawBoxType(const RawBoxView& box, std::string_view type) {
    return type.size() == box.type.size() && std::equal(type.begin(), type.end(), box.type.begin());
}

void CopyRawBox(const std::vector<uint8_t>& bytes, const RawBoxView& box, std::vector<uint8_t>* output) {
    output->insert(output->end(), bytes.begin() + static_cast<ptrdiff_t>(box.offset),
                   bytes.begin() + static_cast<ptrdiff_t>(box.offset + box.size));
}

bool RewriteVexuPack(const std::vector<uint8_t>& bytes, const RawBoxView& pack,
                     uint8_t stereo_mode, std::vector<uint8_t>* rewritten, std::string* error) {
    if (rewritten == nullptr || pack.header_size > pack.size) {
        if (error)
            *error = "invalid VEXU pack box";
        return false;
    }
    std::vector<uint8_t> payload;
    size_t offset = pack.offset + pack.header_size;
    const size_t end = pack.offset + pack.size;
    while (offset < end) {
        RawBoxView child;
        if (!ParseRawBox(bytes, offset, end, &child)) {
            if (error)
                *error = "invalid child box in VEXU pack";
            return false;
        }
        if (IsRawBoxType(child, "pkin")) {
            if (stereo_mode == 1 || stereo_mode == 2) {
                std::vector<uint8_t> raw;
                CopyRawBox(bytes, child, &raw);
                if (child.size == 16) {
                    const char* packing = stereo_mode == 2 ? "side" : "over";
                    std::copy(packing, packing + 4, raw.begin() + 12);
                }
                Append(&payload, raw);
            }
        } else {
            CopyRawBox(bytes, child, &payload);
        }
        offset += child.size;
    }
    if (offset != end) {
        if (error)
            *error = "VEXU pack children do not fill their parent box";
        return false;
    }
    *rewritten = Box("pack", payload);
    if (rewritten->empty()) {
        if (error)
            *error = "rewritten VEXU pack box is too large";
        return false;
    }
    return true;
}

bool BuildVexuBox(const VideoSpatialMetadata& metadata, std::vector<uint8_t>* vexu,
                  std::string* error) {
    if (vexu == nullptr) {
        if (error)
            *error = "VEXU box output is null";
        return false;
    }
    std::vector<uint8_t> payload;
    for (const auto& raw_box : metadata.source_sample_entry_spatial_boxes) {
        RawBoxView outer;
        if (!ParseRawBox(raw_box, 0, raw_box.size(), &outer)) {
            if (error)
                *error = "invalid source spatial sample-entry box";
            return false;
        }
        if (!IsRawBoxType(outer, "vexu"))
            continue;
        size_t offset = outer.offset + outer.header_size;
        const size_t end = outer.offset + outer.size;
        while (offset < end) {
            RawBoxView child;
            if (!ParseRawBox(raw_box, offset, end, &child)) {
                if (error)
                    *error = "invalid child box in source VEXU box";
                return false;
            }
            if (IsRawBoxType(child, "proj")) {
                // Projection is always rebuilt from the selected mapping.
            } else if (IsRawBoxType(child, "eyes") &&
                       (!metadata.has_stereo || metadata.stereo_mode == 0)) {
                // A mono mapping cannot retain source binocular eye views.
            } else if (IsRawBoxType(child, "pack")) {
                std::vector<uint8_t> pack;
                if (!RewriteVexuPack(raw_box, child,
                                     metadata.has_stereo ? metadata.stereo_mode : 0,
                                     &pack, error))
                    return false;
                if (pack.size() > 8)
                    Append(&payload, pack);
            } else {
                CopyRawBox(raw_box, child, &payload);
            }
            offset += child.size;
        }
        if (offset != end) {
            if (error)
                *error = "source VEXU children do not fill their parent box";
            return false;
        }
    }
    const std::vector<uint8_t> projection = BuildVexuProjectionChildBox(metadata);
    if (!projection.empty())
        Append(&payload, projection);
    *vexu = Box("vexu", payload);
    if (vexu->empty()) {
        if (error)
            *error = "rewritten VEXU box is too large";
        return false;
    }
    return true;
}

bool IsSampleEntrySpatialBox(const RawBoxView& box) {
    return IsRawBoxType(box, "st3d") || IsRawBoxType(box, "sv3d") ||
           IsRawBoxType(box, "vexu") || IsRawBoxType(box, "hfov");
}

}  // namespace

const char* VideoMappingPresetLabel(VideoMappingPreset preset) {
    switch (preset) {
        case VideoMappingPreset::kPreserveSource:
            return "Keep source mapping";
        case VideoMappingPreset::kNone:
            return "None - Regular video";
        case VideoMappingPreset::kVr180Mono:
            return "VR180 - Mono";
        case VideoMappingPreset::kVr180SideBySide:
            return "VR180 - Side-by-Side";
        case VideoMappingPreset::kVr180TopBottom:
            return "VR180 - Top-Bottom";
        case VideoMappingPreset::kVr360Mono:
            return "VR360 - Mono";
        case VideoMappingPreset::kVr360SideBySide:
            return "VR360 - Side-by-Side";
        case VideoMappingPreset::kVr360TopBottom:
            return "VR360 - Top-Bottom";
    }
    return "Unknown";
}

bool ParseVideoMappingPreset(const std::string& value, VideoMappingPreset* preset) {
    if (preset == nullptr)
        return false;
    const std::string normalized = Lower(value);
    if (normalized == "preserve" || normalized == "preserve-source" || normalized == "source") {
        *preset = VideoMappingPreset::kPreserveSource;
        return true;
    }
    if (normalized == "none" || normalized == "regular") {
        *preset = VideoMappingPreset::kNone;
        return true;
    }
    if (normalized == "vr180-mono") {
        *preset = VideoMappingPreset::kVr180Mono;
        return true;
    }
    if (normalized == "vr180-sbs" || normalized == "vr180-side-by-side") {
        *preset = VideoMappingPreset::kVr180SideBySide;
        return true;
    }
    if (normalized == "vr180-tb" || normalized == "vr180-top-bottom" || normalized == "vr180-ou") {
        *preset = VideoMappingPreset::kVr180TopBottom;
        return true;
    }
    if (normalized == "vr360-mono") {
        *preset = VideoMappingPreset::kVr360Mono;
        return true;
    }
    if (normalized == "vr360-sbs" || normalized == "vr360-side-by-side") {
        *preset = VideoMappingPreset::kVr360SideBySide;
        return true;
    }
    if (normalized == "vr360-tb" || normalized == "vr360-top-bottom" || normalized == "vr360-ou") {
        *preset = VideoMappingPreset::kVr360TopBottom;
        return true;
    }
    return false;
}

bool BuildVideoSpatialMetadata(const VideoMappingOptions& options,
                               const VideoSpatialMetadata& source,
                               VideoSpatialMetadata* output,
                               std::string* error) {
    if (output == nullptr) {
        if (error)
            *error = "video mapping output is null";
        return false;
    }
    *output = {};
    if (options.preset == VideoMappingPreset::kPreserveSource) {
        if (source.source_mp4_metadata_state == SourceMp4SpatialMetadataState::kFailed) {
            if (error)
                *error = "cannot preserve source MP4 spatial boxes: " + source.source_mp4_metadata_error;
            return false;
        }
        *output = source;
        output->preserve_source_sample_entry_boxes =
            source.source_mp4_metadata_state == SourceMp4SpatialMetadataState::kParsed;
        return true;
    }
    if (source.source_mp4_metadata_state == SourceMp4SpatialMetadataState::kFailed) {
        if (error)
            *error = "cannot safely rewrite source MP4 spatial boxes: " + source.source_mp4_metadata_error;
        return false;
    }
    output->source_mp4_metadata_state = source.source_mp4_metadata_state;
    output->source_mp4_metadata_error = source.source_mp4_metadata_error;
    output->source_sample_entry_spatial_boxes = source.source_sample_entry_spatial_boxes;
    if (options.preset == VideoMappingPreset::kNone) {
        output->has_vexu_projection = true;
        output->vexu_projection = VexuProjectionType::kRectilinear;
        return true;
    }

    switch (options.preset) {
        case VideoMappingPreset::kVr180Mono:
        case VideoMappingPreset::kVr180SideBySide:
        case VideoMappingPreset::kVr180TopBottom:
        case VideoMappingPreset::kVr360Mono:
        case VideoMappingPreset::kVr360SideBySide:
        case VideoMappingPreset::kVr360TopBottom:
            break;
        case VideoMappingPreset::kPreserveSource:
        case VideoMappingPreset::kNone:
            return true;
        default:
            if (error)
                *error = "unsupported video mapping preset";
            return false;
    }
    output->has_stereo = true;
    output->stereo_mode = StereoModeForPreset(options.preset);
    output->has_spherical = true;
    output->projection = VideoProjectionType::kEquirectangular;
    if (IsVr180(options.preset)) {
        output->bound_left = 0x40000000u;
        output->bound_right = 0x40000000u;
        output->has_vexu_projection = true;
        output->vexu_projection = VexuProjectionType::kHalfEquirectangular;
    } else {
        output->has_vexu_projection = true;
        output->vexu_projection = VexuProjectionType::kEquirectangular;
    }
    return true;
}

std::vector<uint8_t> BuildStereo3DBox(const VideoSpatialMetadata& metadata) {
    if (!metadata.has_stereo || metadata.stereo_mode > 4)
        return {};
    return FullBox("st3d", {metadata.stereo_mode});
}

std::vector<uint8_t> BuildSphericalVideoBox(const VideoSpatialMetadata& metadata) {
    if (!metadata.has_spherical)
        return {};

    std::vector<uint8_t> svhd_payload;
    const char* source = "Immersive Audio Muxer";
    svhd_payload.insert(svhd_payload.end(), source, source + std::strlen(source) + 1);
    const auto svhd = FullBox("svhd", svhd_payload);

    std::vector<uint8_t> prhd_payload;
    U32(&prhd_payload, static_cast<uint32_t>(metadata.yaw));
    U32(&prhd_payload, static_cast<uint32_t>(metadata.pitch));
    U32(&prhd_payload, static_cast<uint32_t>(metadata.roll));
    const auto prhd = FullBox("prhd", prhd_payload);

    std::vector<uint8_t> projection_payload;
    Append(&projection_payload, prhd);
    if (metadata.projection == VideoProjectionType::kCubemap) {
        std::vector<uint8_t> cubemap_payload;
        U32(&cubemap_payload, metadata.cubemap_layout);
        U32(&cubemap_payload, metadata.cubemap_padding);
        Append(&projection_payload, FullBox("cbmp", cubemap_payload));
    } else {
        std::vector<uint8_t> equi_payload;
        U32(&equi_payload, metadata.bound_top);
        U32(&equi_payload, metadata.bound_bottom);
        U32(&equi_payload, metadata.bound_left);
        U32(&equi_payload, metadata.bound_right);
        Append(&projection_payload, FullBox("equi", equi_payload));
    }

    std::vector<uint8_t> sv3d_payload;
    Append(&sv3d_payload, svhd);
    Append(&sv3d_payload, Box("proj", projection_payload));
    return Box("sv3d", sv3d_payload);
}

std::vector<uint8_t> BuildVexuProjectionChildBox(const VideoSpatialMetadata& metadata) {
    if (!metadata.has_vexu_projection)
        return {};
    const char* tag = nullptr;
    switch (metadata.vexu_projection) {
        case VexuProjectionType::kRectilinear:
            tag = "rect";
            break;
        case VexuProjectionType::kEquirectangular:
            tag = "equi";
            break;
        case VexuProjectionType::kHalfEquirectangular:
            tag = "hequ";
            break;
        case VexuProjectionType::kFisheye:
            tag = "fish";
            break;
        case VexuProjectionType::kParametricImmersive:
            tag = "prim";
            break;
    }
    if (tag == nullptr)
        return {};
    std::vector<uint8_t> payload(tag, tag + 4);
    const std::vector<uint8_t> prji = FullBox("prji", payload);
    return Box("proj", prji);
}

bool BuildVideoSampleEntrySpatialBoxes(const VideoSpatialMetadata& metadata,
                                      std::vector<std::vector<uint8_t>>* boxes,
                                      std::string* error) {
    if (boxes == nullptr) {
        if (error)
            *error = "video spatial box output is null";
        return false;
    }
    boxes->clear();
    if (metadata.preserve_source_sample_entry_boxes) {
        *boxes = metadata.source_sample_entry_spatial_boxes;
        return true;
    }

    const std::vector<uint8_t> stereo = BuildStereo3DBox(metadata);
    if (!stereo.empty())
        boxes->push_back(stereo);
    const std::vector<uint8_t> spherical = BuildSphericalVideoBox(metadata);
    if (!spherical.empty())
        boxes->push_back(spherical);

    bool has_source_vexu = false;
    bool has_source_hfov = false;
    for (const auto& raw_box : metadata.source_sample_entry_spatial_boxes) {
        RawBoxView box;
        if (!ParseRawBox(raw_box, 0, raw_box.size(), &box) || !IsSampleEntrySpatialBox(box)) {
            if (error)
                *error = "invalid source spatial sample-entry box";
            return false;
        }
        has_source_vexu = has_source_vexu || IsRawBoxType(box, "vexu");
        has_source_hfov = has_source_hfov || IsRawBoxType(box, "hfov");
    }
    if (metadata.has_vexu_projection || has_source_vexu) {
        std::vector<uint8_t> vexu;
        if (!BuildVexuBox(metadata, &vexu, error))
            return false;
        boxes->push_back(std::move(vexu));
    }
    if (has_source_hfov) {
        for (const auto& raw_box : metadata.source_sample_entry_spatial_boxes) {
            RawBoxView box;
            ParseRawBox(raw_box, 0, raw_box.size(), &box);
            if (IsRawBoxType(box, "hfov"))
                boxes->push_back(raw_box);
        }
    }
    return true;
}

}  // namespace immersive_audio_muxer
