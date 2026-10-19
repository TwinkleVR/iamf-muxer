#include "ffmpeg_iamf_track_adapter.h"

#include "iamf_mp4_file_writer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>
#include <vector>

namespace immersive_audio_muxer {
namespace {

constexpr uint64_t kMaxMoovBytes = 256ULL * 1024ULL * 1024ULL;
constexpr uint64_t kCopyBufferBytes = 1024ULL * 1024ULL;

struct BoxView {
    uint64_t offset = 0;
    uint64_t size = 0;
    uint32_t header_size = 0;
    std::array<char, 4> type{};
};

void SetError(std::string* error, const std::string& message) {
    if (error)
        *error = message;
}

bool IsType(const BoxView& box, std::string_view type) {
    return type.size() == 4 && std::equal(type.begin(), type.end(), box.type.begin());
}

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

void AppendU32(std::vector<uint8_t>* bytes, uint32_t value) {
    bytes->push_back(static_cast<uint8_t>(value >> 24));
    bytes->push_back(static_cast<uint8_t>(value >> 16));
    bytes->push_back(static_cast<uint8_t>(value >> 8));
    bytes->push_back(static_cast<uint8_t>(value));
}

void AppendU64(std::vector<uint8_t>* bytes, uint64_t value) {
    for (int i = 7; i >= 0; --i)
        bytes->push_back(static_cast<uint8_t>(value >> (i * 8)));
}

void AppendType(std::vector<uint8_t>* bytes, std::string_view type) {
    bytes->insert(bytes->end(), type.begin(), type.end());
}

bool ParseBox(const std::vector<uint8_t>& bytes, uint64_t offset, uint64_t end, BoxView* box,
              std::string* error) {
    if (box == nullptr || offset > end || end - offset < 8 || offset > std::numeric_limits<size_t>::max()) {
        SetError(error, "truncated MP4 box header");
        return false;
    }
    const size_t start = static_cast<size_t>(offset);
    if (start + 8 > bytes.size()) {
        SetError(error, "truncated MP4 box header bytes");
        return false;
    }
    const uint32_t size32 = ReadU32(bytes, start);
    const uint32_t header_size = size32 == 1 ? 16 : 8;
    if (end - offset < header_size || offset + header_size > bytes.size()) {
        SetError(error, "truncated MP4 extended box header");
        return false;
    }
    uint64_t size = size32;
    if (size32 == 1)
        size = ReadU64(bytes, start + 8);
    else if (size32 == 0)
        size = end - offset;
    if (size < header_size || size > end - offset) {
        SetError(error, "MP4 box size is outside its parent");
        return false;
    }
    box->offset = offset;
    box->size = size;
    box->header_size = header_size;
    std::copy(bytes.begin() + start + 4, bytes.begin() + start + 8, box->type.begin());
    return true;
}

bool ParseChildren(const std::vector<uint8_t>& bytes, const BoxView& parent, std::vector<BoxView>* children,
                   std::string* error) {
    if (children == nullptr || parent.header_size > parent.size ||
        parent.offset > std::numeric_limits<uint64_t>::max() - parent.size) {
        SetError(error, "invalid MP4 parent box");
        return false;
    }
    children->clear();
    uint64_t offset = parent.offset + parent.header_size;
    const uint64_t end = parent.offset + parent.size;
    while (offset < end) {
        BoxView child;
        if (!ParseBox(bytes, offset, end, &child, error))
            return false;
        children->push_back(child);
        if (child.size == 0)
            return false;
        offset += child.size;
    }
    return offset == end;
}

bool ReadTopLevelBoxes(const std::filesystem::path& path, uint64_t file_size, std::vector<BoxView>* boxes,
                       std::string* error) {
    if (boxes == nullptr) {
        SetError(error, "MP4 top-level box output is null");
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        SetError(error, "cannot open FFmpeg placeholder MP4");
        return false;
    }
    boxes->clear();
    uint64_t offset = 0;
    while (offset < file_size) {
        std::array<uint8_t, 16> header{};
        input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!input || !input.read(reinterpret_cast<char*>(header.data()), 8)) {
            SetError(error, "truncated MP4 top-level box header");
            return false;
        }
        const uint32_t size32 = (static_cast<uint32_t>(header[0]) << 24) |
                                (static_cast<uint32_t>(header[1]) << 16) |
                                (static_cast<uint32_t>(header[2]) << 8) | header[3];
        const uint32_t header_size = size32 == 1 ? 16 : 8;
        if (header_size == 16 && !input.read(reinterpret_cast<char*>(header.data() + 8), 8)) {
            SetError(error, "truncated MP4 extended top-level box header");
            return false;
        }
        uint64_t size = size32;
        if (size32 == 1) {
            size = 0;
            for (size_t i = 0; i < 8; ++i)
                size = (size << 8) | header[8 + i];
        } else if (size32 == 0) {
            size = file_size - offset;
        }
        if (size < header_size || size > file_size - offset) {
            SetError(error, "MP4 top-level box size is invalid");
            return false;
        }
        BoxView box;
        box.offset = offset;
        box.size = size;
        box.header_size = header_size;
        std::copy(header.begin() + 4, header.begin() + 8, box.type.begin());
        boxes->push_back(box);
        offset += size;
    }
    return offset == file_size;
}

bool ReadRange(const std::filesystem::path& path, uint64_t offset, uint64_t size, std::vector<uint8_t>* bytes,
               std::string* error) {
    if (bytes == nullptr || size > std::numeric_limits<size_t>::max()) {
        SetError(error, "MP4 range is too large to inspect");
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        SetError(error, "cannot open FFmpeg placeholder MP4 for inspection");
        return false;
    }
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input) {
        SetError(error, "cannot seek FFmpeg placeholder MP4");
        return false;
    }
    bytes->assign(static_cast<size_t>(size), 0);
    if (size != 0)
        input.read(reinterpret_cast<char*>(bytes->data()), static_cast<std::streamsize>(size));
    if (!input && input.gcount() != static_cast<std::streamsize>(size)) {
        SetError(error, "cannot read FFmpeg placeholder MP4 range");
        return false;
    }
    return true;
}

bool FindChild(const std::vector<uint8_t>& bytes, const BoxView& parent, std::string_view type, BoxView* result,
               std::string* error) {
    std::vector<BoxView> children;
    if (!ParseChildren(bytes, parent, &children, error))
        return false;
    for (const BoxView& child : children) {
        if (IsType(child, type)) {
            if (result)
                *result = child;
            return true;
        }
    }
    return false;
}

bool ReadTrackId(const std::vector<uint8_t>& moov, const BoxView& trak, uint32_t* track_id) {
    if (track_id == nullptr)
        return false;
    BoxView tkhd;
    if (!FindChild(moov, trak, "tkhd", &tkhd, nullptr) || tkhd.size < tkhd.header_size + 20)
        return false;
    const size_t base = static_cast<size_t>(tkhd.offset + tkhd.header_size);
    const uint8_t version = moov[base];
    if (version != 0 && version != 1)
        return false;
    const size_t id_offset = base + (version == 0 ? 12 : 20);
    if (id_offset + 4 > tkhd.offset + tkhd.size)
        return false;
    *track_id = ReadU32(moov, id_offset);
    return *track_id != 0;
}

bool HasHandlerName(const std::vector<uint8_t>& moov, const BoxView& trak, std::string_view marker) {
    BoxView mdia;
    BoxView hdlr;
    if (!FindChild(moov, trak, "mdia", &mdia, nullptr) || !FindChild(moov, mdia, "hdlr", &hdlr, nullptr) ||
        hdlr.size < hdlr.header_size + 24 || marker.empty()) {
        return false;
    }
    const size_t name_offset = static_cast<size_t>(hdlr.offset + hdlr.header_size + 24);
    const size_t end = static_cast<size_t>(hdlr.offset + hdlr.size);
    size_t name_end = name_offset;
    while (name_end < end && moov[name_end] != 0)
        ++name_end;
    if (name_end == name_offset)
        return false;
    const std::string_view name(reinterpret_cast<const char*>(moov.data() + name_offset), name_end - name_offset);
    return name.find(marker) != std::string_view::npos;
}

bool ReadMovieTimescale(const std::vector<uint8_t>& moov, uint32_t* timescale, std::string* error) {
    if (timescale == nullptr) {
        SetError(error, "movie timescale output is null");
        return false;
    }
    BoxView moov_view;
    if (!ParseBox(moov, 0, moov.size(), &moov_view, error) || !IsType(moov_view, "moov") ||
        moov_view.size != moov.size()) {
        SetError(error, "FFmpeg output has an invalid movie box");
        return false;
    }
    BoxView mvhd;
    if (!FindChild(moov, moov_view, "mvhd", &mvhd, error) || mvhd.size < mvhd.header_size + 20) {
        SetError(error, "FFmpeg output has no usable movie header");
        return false;
    }
    const size_t base = static_cast<size_t>(mvhd.offset + mvhd.header_size);
    const uint8_t version = moov[base];
    if (version != 0 && version != 1) {
        SetError(error, "unsupported FFmpeg movie header version");
        return false;
    }
    const size_t offset = base + (version == 0 ? 12 : 20);
    if (offset + 4 > mvhd.offset + mvhd.size) {
        SetError(error, "truncated FFmpeg movie header");
        return false;
    }
    *timescale = ReadU32(moov, offset);
    if (*timescale == 0) {
        SetError(error, "FFmpeg movie header has a zero timescale");
        return false;
    }
    return true;
}

bool ReadSampleSizes(const std::vector<uint8_t>& moov, const BoxView& stsz, std::vector<uint64_t>* sizes,
                     std::string* error) {
    if (sizes == nullptr || stsz.size < stsz.header_size + 12) {
        SetError(error, "invalid placeholder stsz box");
        return false;
    }
    const size_t base = static_cast<size_t>(stsz.offset + stsz.header_size);
    const uint32_t sample_size = ReadU32(moov, base + 4);
    const uint32_t sample_count = ReadU32(moov, base + 8);
    const uint64_t available = stsz.size - stsz.header_size - 12;
    if (sample_size != 0) {
        sizes->assign(sample_count, sample_size);
        return true;
    }
    if (available < static_cast<uint64_t>(sample_count) * 4)
        return false;
    sizes->clear();
    sizes->reserve(sample_count);
    for (uint32_t i = 0; i < sample_count; ++i)
        sizes->push_back(ReadU32(moov, base + 12 + static_cast<size_t>(i) * 4));
    return true;
}

struct StscEntry {
    uint32_t first_chunk = 0;
    uint32_t samples_per_chunk = 0;
};

bool ReadSampleOffsets(const std::vector<uint8_t>& moov, const BoxView& trak, std::vector<uint64_t>* offsets,
                       std::vector<uint64_t>* sizes, std::string* error) {
    BoxView mdia, minf, stbl, stsz, stsc, chunk_offsets;
    if (!FindChild(moov, trak, "mdia", &mdia, error) ||
        !FindChild(moov, mdia, "minf", &minf, error) ||
        !FindChild(moov, minf, "stbl", &stbl, error) ||
        !FindChild(moov, stbl, "stsz", &stsz, error) ||
        !FindChild(moov, stbl, "stsc", &stsc, error)) {
        SetError(error, "placeholder track is missing a sample table");
        return false;
    }
    if (!FindChild(moov, stbl, "stco", &chunk_offsets, nullptr) &&
        !FindChild(moov, stbl, "co64", &chunk_offsets, nullptr)) {
        SetError(error, "placeholder track is missing chunk offsets");
        return false;
    }
    if (!ReadSampleSizes(moov, stsz, sizes, error) || stsc.size < stsc.header_size + 8 ||
        chunk_offsets.size < chunk_offsets.header_size + 8) {
        SetError(error, "invalid placeholder sample table");
        return false;
    }
    const size_t stsc_base = static_cast<size_t>(stsc.offset + stsc.header_size);
    const uint32_t stsc_count = ReadU32(moov, stsc_base + 4);
    if (stsc_count == 0 || stsc.size - stsc.header_size - 8 < static_cast<uint64_t>(stsc_count) * 12) {
        SetError(error, "invalid placeholder stsc table");
        return false;
    }
    std::vector<StscEntry> stsc_entries;
    stsc_entries.reserve(stsc_count);
    for (uint32_t i = 0; i < stsc_count; ++i) {
        const size_t entry = stsc_base + 8 + static_cast<size_t>(i) * 12;
        StscEntry value{ReadU32(moov, entry), ReadU32(moov, entry + 4)};
        if (value.first_chunk == 0 || value.samples_per_chunk == 0 ||
            (!stsc_entries.empty() && value.first_chunk <= stsc_entries.back().first_chunk)) {
            SetError(error, "placeholder stsc entries are not monotonic");
            return false;
        }
        stsc_entries.push_back(value);
    }
    const size_t chunk_base = static_cast<size_t>(chunk_offsets.offset + chunk_offsets.header_size);
    const uint32_t chunk_count = ReadU32(moov, chunk_base + 4);
    const bool co64 = IsType(chunk_offsets, "co64");
    const uint64_t entry_bytes = co64 ? 8 : 4;
    if (chunk_offsets.size - chunk_offsets.header_size - 8 < static_cast<uint64_t>(chunk_count) * entry_bytes) {
        SetError(error, "placeholder chunk-offset table is truncated");
        return false;
    }
    offsets->clear();
    offsets->reserve(sizes->size());
    size_t size_index = 0;
    for (uint32_t chunk = 1; chunk <= chunk_count && size_index < sizes->size(); ++chunk) {
        size_t entry_index = 0;
        while (entry_index + 1 < stsc_entries.size() && stsc_entries[entry_index + 1].first_chunk <= chunk)
            ++entry_index;
        const uint32_t samples_per_chunk = stsc_entries[entry_index].samples_per_chunk;
        const size_t chunk_entry = chunk_base + 8 + static_cast<size_t>(chunk - 1) * static_cast<size_t>(entry_bytes);
        uint64_t cursor = co64 ? ReadU64(moov, chunk_entry) : ReadU32(moov, chunk_entry);
        for (uint32_t sample = 0; sample < samples_per_chunk && size_index < sizes->size(); ++sample) {
            offsets->push_back(cursor);
            if ((*sizes)[size_index] > std::numeric_limits<uint64_t>::max() - cursor) {
                SetError(error, "placeholder sample offset overflows");
                return false;
            }
            cursor += (*sizes)[size_index++];
        }
    }
    if (size_index != sizes->size()) {
        SetError(error, "placeholder chunk table does not cover every sample");
        return false;
    }
    return true;
}

bool BuildBox(std::string_view type, const std::vector<uint8_t>& payload, std::vector<uint8_t>* box,
              std::string* error) {
    if (box == nullptr || payload.size() > std::numeric_limits<uint32_t>::max() - 8) {
        SetError(error, "rewritten MP4 box exceeds 32-bit size");
        return false;
    }
    box->clear();
    AppendU32(box, static_cast<uint32_t>(payload.size() + 8));
    AppendType(box, type);
    box->insert(box->end(), payload.begin(), payload.end());
    return true;
}

bool RebuildBoxWithChildReplacement(const std::vector<uint8_t>& bytes, const BoxView& parent,
                                    const BoxView& replaced_child,
                                    const std::vector<uint8_t>& replacement,
                                    std::vector<uint8_t>* rebuilt, std::string* error) {
    if (rebuilt == nullptr || replaced_child.offset < parent.offset + parent.header_size ||
        replaced_child.offset + replaced_child.size > parent.offset + parent.size) {
        SetError(error, "invalid nested MP4 box while updating video metadata");
        return false;
    }
    std::vector<uint8_t> payload;
    const size_t payload_start = static_cast<size_t>(parent.offset + parent.header_size);
    const size_t child_start = static_cast<size_t>(replaced_child.offset);
    const size_t child_end = static_cast<size_t>(replaced_child.offset + replaced_child.size);
    const size_t parent_end = static_cast<size_t>(parent.offset + parent.size);
    payload.insert(payload.end(), bytes.begin() + static_cast<ptrdiff_t>(payload_start),
                   bytes.begin() + static_cast<ptrdiff_t>(child_start));
    payload.insert(payload.end(), replacement.begin(), replacement.end());
    payload.insert(payload.end(), bytes.begin() + static_cast<ptrdiff_t>(child_end),
                   bytes.begin() + static_cast<ptrdiff_t>(parent_end));
    return BuildBox(std::string_view(parent.type.data(), parent.type.size()), payload, rebuilt, error);
}

bool BuildSpatiallyRewrittenStsd(const std::vector<uint8_t>& moov, const BoxView& stsd,
                                 const VideoSpatialMetadata& metadata,
                                 std::vector<uint8_t>* rewritten_stsd, std::string* error) {
    if (rewritten_stsd == nullptr || stsd.size < stsd.header_size + 8) {
        SetError(error, "video sample-description box is truncated");
        return false;
    }
    const uint64_t payload_start = stsd.offset + stsd.header_size;
    const uint32_t entry_count = ReadU32(moov, static_cast<size_t>(payload_start + 4));
    std::vector<uint8_t> payload(moov.begin() + static_cast<ptrdiff_t>(payload_start),
                                 moov.begin() + static_cast<ptrdiff_t>(payload_start + 8));
    uint64_t entry_offset = payload_start + 8;
    for (uint32_t index = 0; index < entry_count; ++index) {
        BoxView entry;
        if (!ParseBox(moov, entry_offset, stsd.offset + stsd.size, &entry, error))
            return false;
        if (entry.size < entry.header_size + 78) {
            SetError(error, "video sample entry is too short to contain its visual header");
            return false;
        }
        const uint64_t entry_payload = entry.offset + entry.header_size;
        const uint64_t children_offset = entry_payload + 78;
        std::vector<uint8_t> sample_entry_payload(
            moov.begin() + static_cast<ptrdiff_t>(entry_payload),
            moov.begin() + static_cast<ptrdiff_t>(children_offset));
        uint64_t child_offset = children_offset;
        while (child_offset < entry.offset + entry.size) {
            BoxView child;
            if (!ParseBox(moov, child_offset, entry.offset + entry.size, &child, error))
                return false;
            if (!IsType(child, "st3d") && !IsType(child, "sv3d") &&
                !IsType(child, "vexu") && !IsType(child, "hfov")) {
                sample_entry_payload.insert(
                    sample_entry_payload.end(),
                    moov.begin() + static_cast<ptrdiff_t>(child.offset),
                    moov.begin() + static_cast<ptrdiff_t>(child.offset + child.size));
            }
            child_offset += child.size;
        }
        std::vector<std::vector<uint8_t>> spatial_boxes;
        if (!BuildVideoSampleEntrySpatialBoxes(metadata, &spatial_boxes, error))
            return false;
        for (const auto& spatial_box : spatial_boxes)
            sample_entry_payload.insert(sample_entry_payload.end(), spatial_box.begin(), spatial_box.end());
        std::vector<uint8_t> rewritten_entry;
        if (!BuildBox(std::string_view(entry.type.data(), entry.type.size()), sample_entry_payload,
                      &rewritten_entry, error))
            return false;
        payload.insert(payload.end(), rewritten_entry.begin(), rewritten_entry.end());
        entry_offset += entry.size;
    }
    if (entry_offset != stsd.offset + stsd.size) {
        SetError(error, "video sample-description entries do not fill their box");
        return false;
    }
    return BuildBox("stsd", payload, rewritten_stsd, error);
}

bool BuildSpatiallyRewrittenVideoTrack(const std::vector<uint8_t>& moov, const BoxView& trak,
                                       const VideoSpatialMetadata& metadata,
                                       std::vector<uint8_t>* rewritten_trak, bool* is_video,
                                       std::string* error) {
    if (rewritten_trak == nullptr || is_video == nullptr) {
        SetError(error, "invalid video track metadata rewrite arguments");
        return false;
    }
    BoxView mdia, hdlr;
    if (!FindChild(moov, trak, "mdia", &mdia, error) || !FindChild(moov, mdia, "hdlr", &hdlr, error))
        return false;
    if (hdlr.size < hdlr.header_size + 12) {
        SetError(error, "video handler box is truncated");
        return false;
    }
    const size_t handler_offset = static_cast<size_t>(hdlr.offset + hdlr.header_size + 8);
    *is_video = std::equal("vide", "vide" + 4, moov.begin() + static_cast<ptrdiff_t>(handler_offset));
    if (!*is_video) {
        rewritten_trak->assign(moov.begin() + static_cast<ptrdiff_t>(trak.offset),
                               moov.begin() + static_cast<ptrdiff_t>(trak.offset + trak.size));
        return true;
    }

    BoxView minf, stbl, stsd;
    if (!FindChild(moov, mdia, "minf", &minf, error) ||
        !FindChild(moov, minf, "stbl", &stbl, error) ||
        !FindChild(moov, stbl, "stsd", &stsd, error))
        return false;
    std::vector<uint8_t> rewritten_stsd, rewritten_stbl, rewritten_minf, rewritten_mdia;
    if (!BuildSpatiallyRewrittenStsd(moov, stsd, metadata, &rewritten_stsd, error) ||
        !RebuildBoxWithChildReplacement(moov, stbl, stsd, rewritten_stsd, &rewritten_stbl, error) ||
        !RebuildBoxWithChildReplacement(moov, minf, stbl, rewritten_stbl, &rewritten_minf, error) ||
        !RebuildBoxWithChildReplacement(moov, mdia, minf, rewritten_minf, &rewritten_mdia, error) ||
        !RebuildBoxWithChildReplacement(moov, trak, mdia, rewritten_mdia, rewritten_trak, error))
        return false;
    return true;
}

bool BuildSpatiallyRewrittenMoov(const std::vector<uint8_t>& moov,
                                 uint32_t target_track_id,
                                 const VideoSpatialMetadata& metadata,
                                 std::vector<uint8_t>* rewritten_moov, std::string* error) {
    BoxView root;
    if (rewritten_moov == nullptr || !ParseBox(moov, 0, moov.size(), &root, error) ||
        !IsType(root, "moov") || root.size != moov.size()) {
        SetError(error, "MP4 has an invalid movie box while updating video metadata");
        return false;
    }
    std::vector<BoxView> children;
    if (!ParseChildren(moov, root, &children, error))
        return false;
    std::vector<uint8_t> payload;
    bool found_target_video = false;
    for (const BoxView& child : children) {
        if (!IsType(child, "trak")) {
            payload.insert(payload.end(), moov.begin() + static_cast<ptrdiff_t>(child.offset),
                           moov.begin() + static_cast<ptrdiff_t>(child.offset + child.size));
            continue;
        }
        uint32_t track_id = 0;
        if (!ReadTrackId(moov, child, &track_id)) {
            SetError(error, "MP4 track has no readable ID while updating video metadata");
            return false;
        }
        if (track_id != target_track_id) {
            payload.insert(payload.end(), moov.begin() + static_cast<ptrdiff_t>(child.offset),
                           moov.begin() + static_cast<ptrdiff_t>(child.offset + child.size));
            continue;
        }
        std::vector<uint8_t> track;
        bool is_video = false;
        if (!BuildSpatiallyRewrittenVideoTrack(moov, child, metadata, &track, &is_video, error))
            return false;
        if (!is_video) {
            SetError(error, "selected MP4 track is not a video track");
            return false;
        }
        found_target_video = true;
        payload.insert(payload.end(), track.begin(), track.end());
    }
    if (!found_target_video) {
        SetError(error, "MP4 has no selected video track to receive spatial metadata");
        return false;
    }
    return BuildBox("moov", payload, rewritten_moov, error);
}

bool BuildRewrittenMoov(const std::vector<uint8_t>& moov,
                        const std::vector<FfmpegIamfPlaceholderTrack>& iamf_tracks,
                        std::vector<uint8_t>* rewritten, std::string* error) {
    if (iamf_tracks.empty()) {
        SetError(error, "no FFmpeg IAMF placeholder tracks were provided");
        return false;
    }
    BoxView moov_view;
    if (!ParseBox(moov, 0, moov.size(), &moov_view, error) || !IsType(moov_view, "moov") ||
        moov_view.size != moov.size()) {
        SetError(error, "FFmpeg output has an invalid movie box");
        return false;
    }
    std::vector<BoxView> children;
    if (!ParseChildren(moov, moov_view, &children, error))
        return false;
    uint32_t movie_timescale = 0;
    if (!ReadMovieTimescale(moov, &movie_timescale, error))
        return false;
    std::vector<uint8_t> payload;
    std::vector<bool> replaced(iamf_tracks.size(), false);
    std::vector<uint32_t> readable_track_ids;
    for (const BoxView& child : children) {
        if (IsType(child, "trak")) {
            uint32_t track_id = 0;
            if (!ReadTrackId(moov, child, &track_id)) {
                SetError(error, "placeholder output contains a track without a readable track ID");
                return false;
            }
            readable_track_ids.push_back(track_id);
            // movenc may renumber track IDs when a source contains a sparse
            // or zero-valued stream ID (for example, an attached picture).
            // The marker is written only on the temporary stream and lets us
            // recover the intended track without guessing from stream order.
            size_t replacement_index = iamf_tracks.size();
            for (size_t index = 0; index < iamf_tracks.size(); ++index) {
                const std::string marker = "IAMF placeholder " + std::to_string(iamf_tracks[index].track_id);
                if (track_id == iamf_tracks[index].track_id || HasHandlerName(moov, child, marker)) {
                    replacement_index = index;
                    break;
                }
            }
            if (replacement_index != iamf_tracks.size()) {
                if (replaced[replacement_index]) {
                    SetError(error, "FFmpeg output contains a duplicate IAMF placeholder track");
                    return false;
                }
                const FfmpegIamfPlaceholderTrack& replacement = iamf_tracks[replacement_index];
                if (replacement.reader == nullptr || replacement.reader->temporal_units().empty()) {
                    SetError(error, "IAMF placeholder has no indexed temporal units");
                    return false;
                }
                std::vector<uint64_t> offsets;
                std::vector<uint64_t> sizes;
                if (!ReadSampleOffsets(moov, child, &offsets, &sizes, error) ||
                    offsets.size() != replacement.reader->temporal_units().size()) {
                    SetError(error, "placeholder IAMF sample count does not match the standalone IAMF input");
                    return false;
                }
                for (size_t i = 0; i < offsets.size(); ++i) {
                    if (sizes[i] != replacement.reader->temporal_units()[i].index.byte_size) {
                        SetError(error, "FFmpeg changed a raw IAMF temporal-unit sample while interleaving");
                        return false;
                    }
                }
                std::vector<uint8_t> iamf_track;
                if (!IamfMp4FileWriter::BuildIamfTrackBoxWithSampleOffsets(
                        *replacement.reader, track_id, offsets, &iamf_track, error, movie_timescale))
                    return false;
                payload.insert(payload.end(), iamf_track.begin(), iamf_track.end());
                replaced[replacement_index] = true;
                continue;
            }
        }
        payload.insert(payload.end(), moov.begin() + static_cast<size_t>(child.offset),
                       moov.begin() + static_cast<size_t>(child.offset + child.size));
    }
    for (size_t index = 0; index < replaced.size(); ++index) {
        if (!replaced[index]) {
            std::string ids;
            for (uint32_t track_id : readable_track_ids) {
                if (!ids.empty())
                    ids += ",";
                ids += std::to_string(track_id);
            }
            SetError(error, "cannot find FFmpeg IAMF placeholder track id " +
                               std::to_string(iamf_tracks[index].track_id) + " (tracks: " + ids + ")");
            return false;
        }
    }
    return BuildBox("moov", payload, rewritten, error);
}

bool EnsureIamfBrand(std::vector<uint8_t>* ftyp, std::string* error) {
    if (ftyp == nullptr || ftyp->size() < 16 || std::string(reinterpret_cast<const char*>(ftyp->data() + 4), 4) != "ftyp") {
        SetError(error, "FFmpeg output has no usable ftyp box");
        return false;
    }
    const uint32_t header_size = (*ftyp)[0] == 0 && (*ftyp)[1] == 0 && (*ftyp)[2] == 0 && (*ftyp)[3] == 1 ? 16 : 8;
    if (ftyp->size() < static_cast<size_t>(header_size) + 8 ||
        (ftyp->size() - static_cast<size_t>(header_size) - 8) % 4 != 0) {
        SetError(error, "FFmpeg output ftyp compatible brands are truncated");
        return false;
    }
    const std::string major(reinterpret_cast<const char*>(ftyp->data() + header_size), 4);
    const size_t start = static_cast<size_t>(header_size) + 8;
    size_t replacement = std::numeric_limits<size_t>::max();
    for (size_t offset = start; offset < ftyp->size(); offset += 4) {
        const std::string brand(reinterpret_cast<const char*>(ftyp->data() + offset), 4);
        if (brand == "iamf")
            return true;
        if (replacement == std::numeric_limits<size_t>::max() && brand == major)
            replacement = offset;
    }
    if (replacement == std::numeric_limits<size_t>::max()) {
        SetError(error, "FFmpeg output ftyp has no compatible-brand slot for IAMF");
        return false;
    }
    std::copy_n("iamf", 4, ftyp->begin() + static_cast<ptrdiff_t>(replacement));
    return true;
}

bool WriteMoovIntoReservedArea(std::fstream* output, const std::vector<BoxView>& top_level,
                               const BoxView& current_moov,
                               const std::vector<uint8_t>& rewritten_moov,
                               std::string* error) {
    if (output == nullptr || rewritten_moov.empty()) {
        SetError(error, "cannot install an empty MP4 movie box");
        return false;
    }
    size_t moov_index = top_level.size();
    uint64_t first_mdat_offset = std::numeric_limits<uint64_t>::max();
    for (size_t index = 0; index < top_level.size(); ++index) {
        if (top_level[index].offset == current_moov.offset && IsType(top_level[index], "moov"))
            moov_index = index;
        if (IsType(top_level[index], "mdat"))
            first_mdat_offset = std::min(first_mdat_offset, top_level[index].offset);
    }
    if (moov_index == top_level.size() || current_moov.offset >= first_mdat_offset) {
        SetError(error, "MP4 movie box is not reserved before the media data");
        return false;
    }

    uint64_t reserved_end = current_moov.offset + current_moov.size;
    for (size_t index = moov_index + 1; index < top_level.size() && top_level[index].offset < first_mdat_offset;
         ++index) {
        const BoxView& box = top_level[index];
        if (!IsType(box, "free") || box.offset != reserved_end) {
            SetError(error, "MP4 front-moov reservation is interrupted before the media data");
            return false;
        }
        if (box.size > std::numeric_limits<uint64_t>::max() - reserved_end) {
            SetError(error, "MP4 front-moov reservation size overflows");
            return false;
        }
        reserved_end += box.size;
    }
    if (reserved_end > first_mdat_offset || rewritten_moov.size() > reserved_end - current_moov.offset) {
        SetError(error, "rewritten MP4 movie box exceeds the reserved front-moov space");
        return false;
    }
    const uint64_t free_size = reserved_end - current_moov.offset - rewritten_moov.size();
    if (free_size > 0 && free_size < 8) {
        SetError(error, "front-moov reservation leaves too little space for a valid free box");
        return false;
    }

    output->seekp(static_cast<std::streamoff>(current_moov.offset), std::ios::beg);
    output->write(reinterpret_cast<const char*>(rewritten_moov.data()),
                  static_cast<std::streamsize>(rewritten_moov.size()));
    if (free_size > 0) {
        std::array<uint8_t, 8> free_header{};
        free_header[0] = static_cast<uint8_t>(free_size >> 24);
        free_header[1] = static_cast<uint8_t>(free_size >> 16);
        free_header[2] = static_cast<uint8_t>(free_size >> 8);
        free_header[3] = static_cast<uint8_t>(free_size);
        free_header[4] = 'f';
        free_header[5] = 'r';
        free_header[6] = 'e';
        free_header[7] = 'e';
        output->write(reinterpret_cast<const char*>(free_header.data()),
                      static_cast<std::streamsize>(free_header.size()));
        std::vector<char> zeros(static_cast<size_t>(kCopyBufferBytes), 0);
        uint64_t bytes_left = free_size - free_header.size();
        while (bytes_left > 0) {
            const size_t bytes_to_write = static_cast<size_t>(
                std::min<uint64_t>(bytes_left, static_cast<uint64_t>(zeros.size())));
            output->write(zeros.data(), static_cast<std::streamsize>(bytes_to_write));
            bytes_left -= bytes_to_write;
        }
    }
    output->flush();
    if (!*output) {
        SetError(error, "cannot install the rewritten MP4 movie box in its front reservation");
        return false;
    }
    return true;
}

uint32_t ReadU32(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

bool HasUnsupportedPlaceholderLayout(const std::filesystem::path& path,
                                const std::vector<BoxView>& boxes,
                                std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        SetError(error, "cannot open MP4 before replacing IAMF placeholder tracks");
        return true;
    }
    for (const BoxView& box : boxes) {
        if (IsType(box, "moof") || IsType(box, "mfra")) {
            SetError(error, "fragmented MP4 cannot be extended with a flat IAMF track");
            return true;
        }
        std::array<uint8_t, 4> size_bytes{};
        input.seekg(static_cast<std::streamoff>(box.offset), std::ios::beg);
        if (!input || !input.read(reinterpret_cast<char*>(size_bytes.data()), 4)) {
            SetError(error, "cannot inspect MP4 box header before replacing IAMF placeholder tracks");
            return true;
        }
        if (ReadU32(size_bytes.data()) == 0) {
            SetError(error, "MP4 has a size-to-end box that cannot be followed by IAMF data");
            return true;
        }
    }
    return false;
}

}  // namespace

bool ReplaceFfmpegPlaceholderTracksWithIamf(
    const std::string& path, const std::vector<FfmpegIamfPlaceholderTrack>& iamf_tracks,
    std::string* error, const std::function<bool()>& cancellation_requested) {
    if (path.empty() || iamf_tracks.empty()) {
        SetError(error, "invalid FFmpeg IAMF placeholder replacement request");
        return false;
    }
    for (const FfmpegIamfPlaceholderTrack& track : iamf_tracks) {
        if (track.track_id == 0 || track.reader == nullptr || track.reader->temporal_units().empty()) {
            SetError(error, "invalid FFmpeg IAMF placeholder track");
            return false;
        }
    }
    const std::filesystem::path input_path = std::filesystem::u8path(path);
    std::error_code filesystem_error;
    const uintmax_t file_size_value = std::filesystem::file_size(input_path, filesystem_error);
    if (filesystem_error || file_size_value > std::numeric_limits<uint64_t>::max()) {
        SetError(error, "cannot inspect FFmpeg placeholder MP4 size");
        return false;
    }
    const uint64_t file_size = static_cast<uint64_t>(file_size_value);
    std::vector<BoxView> top_level;
    if (!ReadTopLevelBoxes(input_path, file_size, &top_level, error))
        return false;
    const BoxView* ftyp_box = nullptr;
    const BoxView* moov_box = nullptr;
    bool has_mdat = false;
    for (const BoxView& box : top_level) {
        if (IsType(box, "ftyp")) {
            if (ftyp_box != nullptr) {
                SetError(error, "FFmpeg output contains more than one ftyp box");
                return false;
            }
            ftyp_box = &box;
        } else if (IsType(box, "moov")) {
            if (moov_box != nullptr) {
                SetError(error, "FFmpeg output contains more than one moov box");
                return false;
            }
            moov_box = &box;
        } else if (IsType(box, "mdat")) {
            has_mdat = true;
        } else if (IsType(box, "moof") || IsType(box, "mfra")) {
            SetError(error, "fragmented FFmpeg output cannot be patched with a flat IAMF trak");
            return false;
        }
    }
    if (ftyp_box == nullptr || moov_box == nullptr || !has_mdat ||
        moov_box->size > kMaxMoovBytes) {
        SetError(error, "FFmpeg output is missing a bounded flat ftyp/moov pair");
        return false;
    }
    if (HasUnsupportedPlaceholderLayout(input_path, top_level, error))
        return false;
    std::vector<uint8_t> ftyp;
    std::vector<uint8_t> moov;
    if (!ReadRange(input_path, ftyp_box->offset, ftyp_box->size, &ftyp, error) ||
        !ReadRange(input_path, moov_box->offset, moov_box->size, &moov, error))
        return false;
    if (!EnsureIamfBrand(&ftyp, error))
        return false;
    std::vector<uint8_t> rewritten_moov;
    if (!BuildRewrittenMoov(moov, iamf_tracks, &rewritten_moov, error))
        return false;

    std::fstream output(input_path, std::ios::binary | std::ios::in | std::ios::out);
    if (!output) {
        SetError(error, "cannot open FFmpeg output for in-place IAMF track patching");
        return false;
    }
    if (cancellation_requested && cancellation_requested()) {
        SetError(error, "operation cancelled before installing the IAMF track");
        return false;
    }
    output.seekp(static_cast<std::streamoff>(ftyp_box->offset), std::ios::beg);
    output.write(reinterpret_cast<const char*>(ftyp.data()), static_cast<std::streamsize>(ftyp.size()));
    if (!output) {
        SetError(error, "cannot update the FFmpeg MP4 file type box");
        return false;
    }
    return WriteMoovIntoReservedArea(&output, top_level, *moov_box, rewritten_moov, error);
}

bool PatchMp4VideoSpatialMetadata(const std::string& path,
                                  uint32_t target_track_id,
                                  const VideoSpatialMetadata& metadata,
                                  std::string* error) {
    if (path.empty() || target_track_id == 0) {
        SetError(error, "MP4 path and selected video track ID are required when patching spatial metadata");
        return false;
    }
    const std::filesystem::path input_path = std::filesystem::u8path(path);
    std::error_code filesystem_error;
    const uintmax_t file_size_value = std::filesystem::file_size(input_path, filesystem_error);
    if (filesystem_error || file_size_value > std::numeric_limits<uint64_t>::max()) {
        SetError(error, "cannot inspect MP4 size while patching video spatial metadata");
        return false;
    }
    std::vector<BoxView> top_level;
    if (!ReadTopLevelBoxes(input_path, static_cast<uint64_t>(file_size_value), &top_level, error))
        return false;
    const BoxView* moov_box = nullptr;
    for (const BoxView& box : top_level) {
        if (!IsType(box, "moov"))
            continue;
        if (moov_box != nullptr) {
            SetError(error, "MP4 contains more than one active movie box while patching spatial metadata");
            return false;
        }
        moov_box = &box;
    }
    if (moov_box == nullptr || moov_box->size > kMaxMoovBytes) {
        SetError(error, "MP4 has no bounded movie box for spatial metadata patching");
        return false;
    }
    std::vector<uint8_t> moov, rewritten_moov;
    if (!ReadRange(input_path, moov_box->offset, moov_box->size, &moov, error) ||
        !BuildSpatiallyRewrittenMoov(moov, target_track_id, metadata, &rewritten_moov, error))
        return false;
    std::fstream output(input_path, std::ios::binary | std::ios::in | std::ios::out);
    if (!output) {
        SetError(error, "cannot open MP4 to patch video spatial metadata");
        return false;
    }
    return WriteMoovIntoReservedArea(&output, top_level, *moov_box, rewritten_moov, error);
}

bool PatchMp4TimecodeTrackFlags(const std::string& path,
                               const std::vector<Mp4TimecodeTrackFlags>& tracks,
                               std::string* error) {
    if (tracks.empty())
        return true;
    if (path.empty()) {
        SetError(error, "MP4 path is required when patching timecode track flags");
        return false;
    }
    const std::filesystem::path input_path = std::filesystem::u8path(path);
    std::error_code filesystem_error;
    const uintmax_t file_size_value = std::filesystem::file_size(input_path, filesystem_error);
    if (filesystem_error || file_size_value > std::numeric_limits<uint64_t>::max()) {
        SetError(error, "cannot inspect MP4 size while patching timecode flags");
        return false;
    }
    std::vector<BoxView> top_level;
    if (!ReadTopLevelBoxes(input_path, static_cast<uint64_t>(file_size_value), &top_level, error))
        return false;
    const BoxView* moov_box = nullptr;
    for (const BoxView& box : top_level) {
        if (!IsType(box, "moov"))
            continue;
        if (moov_box != nullptr) {
            SetError(error, "MP4 contains more than one active movie box while patching timecode flags");
            return false;
        }
        moov_box = &box;
    }
    if (moov_box == nullptr || moov_box->size > kMaxMoovBytes) {
        SetError(error, "MP4 has no bounded movie box for timecode flag patching");
        return false;
    }
    std::vector<uint8_t> moov;
    if (!ReadRange(input_path, moov_box->offset, moov_box->size, &moov, error))
        return false;
    BoxView root;
    if (!ParseBox(moov, 0, moov.size(), &root, error) || !IsType(root, "moov"))
        return false;
    std::vector<BoxView> children;
    if (!ParseChildren(moov, root, &children, error))
        return false;

    std::vector<uint32_t> patched_track_ids;
    for (const Mp4TimecodeTrackFlags& patch : tracks) {
        if (patch.track_id == 0 ||
            std::find(patched_track_ids.begin(), patched_track_ids.end(), patch.track_id) != patched_track_ids.end()) {
            SetError(error, "invalid or duplicate timecode track ID while patching flags");
            return false;
        }
        bool found_track = false;
        bool patched_entry = false;
        for (const BoxView& child : children) {
            if (!IsType(child, "trak"))
                continue;
            uint32_t track_id = 0;
            if (!ReadTrackId(moov, child, &track_id)) {
                SetError(error, "MP4 contains a track without a valid ID while patching timecode flags");
                return false;
            }
            if (track_id != patch.track_id)
                continue;
            found_track = true;
            BoxView mdia, minf, stbl, stsd;
            if (!FindChild(moov, child, "mdia", &mdia, error) ||
                !FindChild(moov, mdia, "minf", &minf, error) ||
                !FindChild(moov, minf, "stbl", &stbl, error) ||
                !FindChild(moov, stbl, "stsd", &stsd, error))
                return false;
            const uint64_t entries_offset = stsd.offset + stsd.header_size;
            if (stsd.size < stsd.header_size + 8) {
                SetError(error, "timecode sample description box is truncated");
                return false;
            }
            const uint32_t entry_count = ReadU32(moov, static_cast<size_t>(entries_offset + 4));
            uint64_t entry_offset = entries_offset + 8;
            const uint64_t stsd_end = stsd.offset + stsd.size;
            for (uint32_t entry_index = 0; entry_index < entry_count; ++entry_index) {
                BoxView entry;
                if (!ParseBox(moov, entry_offset, stsd_end, &entry, error))
                    return false;
                if (IsType(entry, "tmcd")) {
                    if (entry.size < 24) {
                        SetError(error, "timecode sample entry is too short to contain flags");
                        return false;
                    }
                    const size_t flags_offset = static_cast<size_t>(entry.offset + 20);
                    moov[flags_offset] = static_cast<uint8_t>(patch.flags >> 24);
                    moov[flags_offset + 1] = static_cast<uint8_t>(patch.flags >> 16);
                    moov[flags_offset + 2] = static_cast<uint8_t>(patch.flags >> 8);
                    moov[flags_offset + 3] = static_cast<uint8_t>(patch.flags);
                    patched_entry = true;
                }
                entry_offset += entry.size;
            }
            if (entry_offset != stsd_end) {
                SetError(error, "timecode sample description sizes do not match their box");
                return false;
            }
            break;
        }
        if (!found_track || !patched_entry) {
            SetError(error, "cannot find tmcd sample entry for output track " + std::to_string(patch.track_id));
            return false;
        }
        patched_track_ids.push_back(patch.track_id);
    }

    std::fstream output(input_path, std::ios::binary | std::ios::in | std::ios::out);
    if (!output) {
        SetError(error, "cannot open MP4 to patch timecode track flags");
        return false;
    }
    output.seekp(static_cast<std::streamoff>(moov_box->offset), std::ios::beg);
    output.write(reinterpret_cast<const char*>(moov.data()), static_cast<std::streamsize>(moov.size()));
    output.flush();
    if (!output) {
        SetError(error, "cannot write patched timecode track flags");
        return false;
    }
    return true;
}

}  // namespace immersive_audio_muxer
