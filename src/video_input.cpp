#include "video_input.h"

#include "avformat_input.h"
#include "mp4_iamf_audio_tracks.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/spherical.h>
#include <libavutil/stereo3d.h>
}

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>
#include <utility>

namespace immersive_audio_muxer {
namespace {
std::string AvError(int error_code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error_code, buffer, sizeof(buffer));
    return buffer;
}

constexpr uint64_t kMaxSourceMoovBytes = 256ULL * 1024ULL * 1024ULL;

struct Mp4BoxView {
    size_t offset = 0;
    size_t size = 0;
    size_t header_size = 0;
    std::array<char, 4> type{};
};

uint32_t ReadMp4U32(const std::vector<uint8_t>& bytes, size_t offset) {
    return (static_cast<uint32_t>(bytes[offset]) << 24) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<uint32_t>(bytes[offset + 3]);
}

uint64_t ReadMp4U64(const std::vector<uint8_t>& bytes, size_t offset) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i)
        value = (value << 8) | bytes[offset + i];
    return value;
}

bool ParseMp4Box(const std::vector<uint8_t>& bytes, size_t offset, size_t end, Mp4BoxView* box) {
    if (box == nullptr || offset > end || end - offset < 8 || end > bytes.size())
        return false;
    const uint32_t size32 = ReadMp4U32(bytes, offset);
    const size_t header_size = size32 == 1 ? 16 : 8;
    if (end - offset < header_size)
        return false;
    uint64_t size = size32;
    if (size32 == 1)
        size = ReadMp4U64(bytes, offset + 8);
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

bool IsMp4BoxType(const Mp4BoxView& box, std::string_view type) {
    return type.size() == box.type.size() && std::equal(type.begin(), type.end(), box.type.begin());
}

bool FindMp4Child(const std::vector<uint8_t>& bytes, const Mp4BoxView& parent,
                  std::string_view type, Mp4BoxView* child) {
    if (child == nullptr || parent.header_size > parent.size)
        return false;
    size_t offset = parent.offset + parent.header_size;
    const size_t end = parent.offset + parent.size;
    while (offset < end) {
        Mp4BoxView candidate;
        if (!ParseMp4Box(bytes, offset, end, &candidate))
            return false;
        if (IsMp4BoxType(candidate, type)) {
            *child = candidate;
            return true;
        }
        offset += candidate.size;
    }
    return false;
}

bool ParseMp4Children(const std::vector<uint8_t>& bytes, const Mp4BoxView& parent,
                      std::vector<Mp4BoxView>* children) {
    if (children == nullptr || parent.header_size > parent.size)
        return false;
    children->clear();
    size_t offset = parent.offset + parent.header_size;
    const size_t end = parent.offset + parent.size;
    while (offset < end) {
        Mp4BoxView child;
        if (!ParseMp4Box(bytes, offset, end, &child))
            return false;
        children->push_back(child);
        offset += child.size;
    }
    return offset == end;
}

bool ReadSourceMoov(const std::string& path, std::vector<uint8_t>* moov, std::string* error) {
    if (moov == nullptr) {
        if (error)
            *error = "source MP4 movie-box output is null";
        return false;
    }
    std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
    if (!input) {
        if (error)
            *error = "cannot open source MP4 to inspect spatial boxes";
        return false;
    }
    input.seekg(0, std::ios::end);
    const std::streamoff end_position = input.tellg();
    if (end_position < 0) {
        if (error)
            *error = "cannot determine source MP4 size";
        return false;
    }
    const uint64_t file_size = static_cast<uint64_t>(end_position);
    uint64_t offset = 0;
    bool found_moov = false;
    while (offset < file_size) {
        if (file_size - offset < 8) {
            if (error)
                *error = "truncated source MP4 top-level box";
            return false;
        }
        std::array<uint8_t, 16> header{};
        input.clear();
        input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        input.read(reinterpret_cast<char*>(header.data()), 8);
        if (input.gcount() != 8) {
            if (error)
                *error = "cannot read source MP4 top-level box";
            return false;
        }
        const uint32_t size32 = (static_cast<uint32_t>(header[0]) << 24) |
                                (static_cast<uint32_t>(header[1]) << 16) |
                                (static_cast<uint32_t>(header[2]) << 8) | header[3];
        const bool is_moov = std::equal("moov", "moov" + 4, header.begin() + 4);
        const size_t header_size = size32 == 1 ? 16 : 8;
        if (size32 == 1) {
            if (file_size - offset < 16) {
                if (error)
                    *error = "truncated extended source MP4 box header";
                return false;
            }
            input.read(reinterpret_cast<char*>(header.data() + 8), 8);
            if (input.gcount() != 8) {
                if (error)
                    *error = "cannot read extended source MP4 box size";
                return false;
            }
        }
        uint64_t box_size = size32;
        if (size32 == 1) {
            box_size = 0;
            for (size_t index = 8; index < 16; ++index)
                box_size = (box_size << 8) | header[index];
        } else if (size32 == 0) {
            box_size = file_size - offset;
        }
        if (box_size < header_size || box_size > file_size - offset) {
            if (error)
                *error = "source MP4 box size is invalid";
            return false;
        }
        if (is_moov) {
            if (found_moov || box_size > kMaxSourceMoovBytes || box_size > std::numeric_limits<size_t>::max()) {
                if (error)
                    *error = "source MP4 movie box is duplicated or exceeds the supported size";
                return false;
            }
            moov->resize(static_cast<size_t>(box_size));
            input.clear();
            input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            input.read(reinterpret_cast<char*>(moov->data()), static_cast<std::streamsize>(moov->size()));
            if (input.gcount() != static_cast<std::streamsize>(moov->size())) {
                if (error)
                    *error = "cannot read complete source MP4 movie box";
                return false;
            }
            found_moov = true;
        }
        offset += box_size;
    }
    if (!found_moov) {
        if (error)
            *error = "source MP4 has no movie box";
        return false;
    }
    return true;
}

bool ReadTrackId(const std::vector<uint8_t>& moov, const Mp4BoxView& trak, uint32_t* track_id) {
    Mp4BoxView tkhd;
    if (track_id == nullptr || !FindMp4Child(moov, trak, "tkhd", &tkhd) ||
        tkhd.size < tkhd.header_size + 4)
        return false;
    const size_t payload = tkhd.offset + tkhd.header_size;
    const uint8_t version = moov[payload];
    const size_t track_id_offset = payload + (version == 1 ? 20 : 12);
    if (version > 1 || track_id_offset + 4 > tkhd.offset + tkhd.size)
        return false;
    *track_id = ReadMp4U32(moov, track_id_offset);
    return true;
}

bool IsSpatialSampleEntryBox(const Mp4BoxView& box) {
    return IsMp4BoxType(box, "st3d") || IsMp4BoxType(box, "sv3d") ||
           IsMp4BoxType(box, "vexu") || IsMp4BoxType(box, "hfov");
}

bool ReadSampleEntrySpatialBoxes(const std::vector<uint8_t>& moov, const Mp4BoxView& stsd,
                                 std::vector<std::vector<uint8_t>>* boxes, std::string* error) {
    if (boxes == nullptr || stsd.size < stsd.header_size + 8) {
        if (error)
            *error = "source MP4 sample-description box is truncated";
        return false;
    }
    const size_t payload = stsd.offset + stsd.header_size;
    const uint32_t entry_count = ReadMp4U32(moov, payload + 4);
    if (entry_count == 0) {
        if (error)
            *error = "source MP4 video track has no sample entry";
        return false;
    }
    std::vector<std::vector<uint8_t>> first_entry_boxes;
    size_t entry_offset = payload + 8;
    for (uint32_t index = 0; index < entry_count; ++index) {
        Mp4BoxView entry;
        if (!ParseMp4Box(moov, entry_offset, stsd.offset + stsd.size, &entry) ||
            entry.size < entry.header_size + 78) {
            if (error)
                *error = "source MP4 video sample entry is malformed";
            return false;
        }
        const size_t children_start = entry.offset + entry.header_size + 78;
        const size_t entry_end = entry.offset + entry.size;
        std::vector<std::vector<uint8_t>> entry_boxes;
        size_t child_offset = children_start;
        while (child_offset < entry_end) {
            Mp4BoxView child;
            if (!ParseMp4Box(moov, child_offset, entry_end, &child)) {
                if (error)
                    *error = "source MP4 video sample-entry child box is malformed";
                return false;
            }
            if (IsSpatialSampleEntryBox(child)) {
                entry_boxes.emplace_back(moov.begin() + static_cast<ptrdiff_t>(child.offset),
                                         moov.begin() + static_cast<ptrdiff_t>(child.offset + child.size));
            }
            child_offset += child.size;
        }
        if (index == 0) {
            first_entry_boxes = std::move(entry_boxes);
        } else if (entry_boxes != first_entry_boxes) {
            if (error)
                *error = "source MP4 video sample descriptions have different spatial boxes";
            return false;
        }
        entry_offset += entry.size;
    }
    if (entry_offset != stsd.offset + stsd.size) {
        if (error)
            *error = "source MP4 sample descriptions do not fill the stsd box";
        return false;
    }
    *boxes = std::move(first_entry_boxes);
    return true;
}

bool ReadSourceMp4SpatialBoxes(const std::string& path, int stream_id,
                               std::vector<std::vector<uint8_t>>* boxes, std::string* error) {
    if (stream_id <= 0 || boxes == nullptr) {
        if (error)
            *error = "source MP4 video track has no valid track ID";
        return false;
    }
    std::vector<uint8_t> moov;
    if (!ReadSourceMoov(path, &moov, error))
        return false;
    Mp4BoxView root;
    if (!ParseMp4Box(moov, 0, moov.size(), &root) || !IsMp4BoxType(root, "moov") || root.size != moov.size()) {
        if (error)
            *error = "source MP4 movie box is malformed";
        return false;
    }
    std::vector<Mp4BoxView> children;
    if (!ParseMp4Children(moov, root, &children)) {
        if (error)
            *error = "source MP4 movie-box children are malformed";
        return false;
    }
    unsigned matched_tracks = 0;
    std::vector<std::vector<uint8_t>> selected_boxes;
    for (const Mp4BoxView& child : children) {
        if (!IsMp4BoxType(child, "trak"))
            continue;
        uint32_t track_id = 0;
        if (!ReadTrackId(moov, child, &track_id) || track_id != static_cast<uint32_t>(stream_id))
            continue;
        Mp4BoxView mdia;
        if (!FindMp4Child(moov, child, "mdia", &mdia)) {
            if (error)
                *error = "selected source MP4 track has no media box";
            return false;
        }
        Mp4BoxView hdlr;
        if (!FindMp4Child(moov, mdia, "hdlr", &hdlr) || hdlr.size < hdlr.header_size + 12) {
            if (error)
                *error = "selected source MP4 track has no valid handler box";
            return false;
        }
        const size_t handler_offset = hdlr.offset + hdlr.header_size + 8;
        if (!std::equal("vide", "vide" + 4,
                        moov.begin() + static_cast<ptrdiff_t>(handler_offset))) {
            continue;
        }
        ++matched_tracks;
        Mp4BoxView minf, stbl, stsd;
        if (!FindMp4Child(moov, mdia, "minf", &minf) ||
            !FindMp4Child(moov, minf, "stbl", &stbl) ||
            !FindMp4Child(moov, stbl, "stsd", &stsd) ||
            !ReadSampleEntrySpatialBoxes(moov, stsd, &selected_boxes, error))
            return false;
    }
    if (matched_tracks != 1) {
        if (error)
            *error = matched_tracks == 0 ? "cannot match selected FFmpeg stream to an MP4 video track"
                                         : "source MP4 contains duplicate IDs for the selected video track";
        return false;
    }
    *boxes = std::move(selected_boxes);
    return true;
}

}  // namespace

VideoInput::~VideoInput() {
    RemoveTemporaryIamfAudioTracks(&validated_iamf_audio_paths_);
}

bool VideoInput::OpenMetadataOnly(const std::string& path, int requested_stream_index, std::string* error,
                                  const CancellationCallback& cancellation_requested,
                                  bool preserve_iamf_audio_tracks) {
    RemoveTemporaryIamfAudioTracks(&validated_iamf_audio_paths_);
    track_ = {};
    int status = 0;
    AVFormatContext* context = nullptr;
    status = avformat_input::Open(&context, path, cancellation_requested, nullptr, false, true);
    if (status < 0) {
        if (error)
            *error = "cannot open or read video streams: " + AvError(status);
        return false;
    }

    if (preserve_iamf_audio_tracks) {
        std::string extraction_error;
        if (!ExtractValidatedIamfAudioTracks(path, &validated_iamf_audio_paths_,
                                             cancellation_requested, &extraction_error)) {
            if (error)
                *error = extraction_error;
            avformat_input::Close(&context);
            return false;
        }
    }

    int stream_index = requested_stream_index;
    if (stream_index < 0) {
        stream_index = av_find_best_stream(context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    }
    if (stream_index < 0 || stream_index >= static_cast<int>(context->nb_streams) ||
        context->streams[stream_index]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
        if (error)
            *error = "no valid video stream found";
        avformat_input::Close(&context);
        return false;
    }

    AVStream* stream = context->streams[stream_index];
    const AVCodecParameters* parameters = stream->codecpar;
    if (stream->time_base.num <= 0 || stream->time_base.den <= 0 || parameters->width <= 0 || parameters->height <= 0 ||
        parameters->width > 65535 || parameters->height > 65535) {
        if (error)
            *error = "video stream has invalid dimensions or time base";
        avformat_input::Close(&context);
        return false;
    }

    track_.source_path = path;
    track_.stream_index = stream_index;
    track_.format_name = context->iformat != nullptr && context->iformat->name != nullptr ? context->iformat->name : "";
    VideoSpatialMetadata& spatial = track_.spatial_metadata;
    const auto get_coded_side_data = [parameters](enum AVPacketSideDataType type, size_t* size) {
        const AVPacketSideData* side_data =
            av_packet_side_data_get(parameters->coded_side_data, parameters->nb_coded_side_data, type);
        if (size != nullptr)
            *size = side_data == nullptr ? 0 : side_data->size;
        return side_data == nullptr ? static_cast<const uint8_t*>(nullptr) : side_data->data;
    };

    size_t side_data_size = 0;
    const uint8_t* spherical_side_data =
        get_coded_side_data(AV_PKT_DATA_SPHERICAL, &side_data_size);
    if (spherical_side_data != nullptr && side_data_size >= sizeof(AVSphericalMapping)) {
        const auto* mapping = reinterpret_cast<const AVSphericalMapping*>(spherical_side_data);
        spatial.has_spherical = true;
        spatial.yaw = mapping->yaw;
        spatial.pitch = mapping->pitch;
        spatial.roll = mapping->roll;
        switch (mapping->projection) {
            case AV_SPHERICAL_EQUIRECTANGULAR:
                spatial.projection = VideoProjectionType::kEquirectangular;
                break;
            case AV_SPHERICAL_EQUIRECTANGULAR_TILE:
                spatial.projection = VideoProjectionType::kEquirectangular;
                spatial.bound_top = mapping->bound_top;
                spatial.bound_bottom = mapping->bound_bottom;
                spatial.bound_left = mapping->bound_left;
                spatial.bound_right = mapping->bound_right;
                break;
            case AV_SPHERICAL_HALF_EQUIRECTANGULAR:
                spatial.projection = VideoProjectionType::kEquirectangular;
                spatial.bound_left = 0x40000000;
                spatial.bound_right = 0x40000000;
                break;
            case AV_SPHERICAL_CUBEMAP:
                spatial.projection = VideoProjectionType::kCubemap;
                spatial.cubemap_padding = mapping->padding;
                break;
            default:
                spatial.has_spherical = false;
                break;
        }
    }

    const uint8_t* stereo_side_data =
        get_coded_side_data(AV_PKT_DATA_STEREO3D, &side_data_size);
    if (stereo_side_data != nullptr && side_data_size >= sizeof(AVStereo3D)) {
        const auto* stereo = reinterpret_cast<const AVStereo3D*>(stereo_side_data);
        switch (stereo->type) {
            case AV_STEREO3D_2D:
                spatial.stereo_mode = 0;
                spatial.has_stereo = true;
                break;
            case AV_STEREO3D_TOPBOTTOM:
                spatial.stereo_mode = 1;
                spatial.has_stereo = true;
                break;
            case AV_STEREO3D_SIDEBYSIDE:
                spatial.stereo_mode = 2;
                spatial.has_stereo = true;
                break;
            default:
                spatial.has_stereo = false;
                break;
        }
    }

    if (IsMp4Like()) {
        spatial.source_mp4_metadata_state = SourceMp4SpatialMetadataState::kFailed;
        std::string spatial_box_error;
        if (ReadSourceMp4SpatialBoxes(path, stream->id,
                                      &spatial.source_sample_entry_spatial_boxes,
                                      &spatial_box_error)) {
            spatial.source_mp4_metadata_state = SourceMp4SpatialMetadataState::kParsed;
        } else {
            spatial.source_mp4_metadata_error = std::move(spatial_box_error);
        }
    } else {
        spatial.source_mp4_metadata_state = SourceMp4SpatialMetadataState::kNotMp4;
    }

    avformat_input::Close(&context);
    return true;
}

bool VideoInput::IsMp4Like() const {
    const std::string& name = track_.format_name;
    return name.find("mov") != std::string::npos || name.find("mp4") != std::string::npos ||
           name.find("3gp") != std::string::npos || name.find("mj2") != std::string::npos;
}

}  // namespace immersive_audio_muxer
