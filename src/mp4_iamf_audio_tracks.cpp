#include "mp4_iamf_audio_tracks.h"

#include "iamf_tools_probe_api.h"
#include "path_utils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <system_error>
#include <string_view>

namespace immersive_audio_muxer {
namespace {

constexpr uint64_t kMaxMovieBoxSize = 256ULL * 1024ULL * 1024ULL;
constexpr uint64_t kCopyBufferSize = 1024ULL * 1024ULL;
constexpr uint32_t kMaxSampleCount = 16U * 1024U * 1024U;

struct Box {
    uint64_t offset = 0;
    uint64_t size = 0;
    uint32_t header_size = 0;
    std::array<char, 4> type{};
};

struct ByteRange {
    uint64_t offset = 0;
    uint64_t size = 0;
};

struct SampleToChunk {
    uint32_t first_chunk = 0;
    uint32_t samples_per_chunk = 0;
    uint32_t sample_description_index = 0;
};

void SetError(std::string* error, const std::string& message) {
    if (error != nullptr)
        *error = message;
}

uint32_t ReadU32(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
}

uint64_t ReadU64(const uint8_t* bytes) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i)
        value = (value << 8) | bytes[i];
    return value;
}

bool IsType(const Box& box, std::string_view type) {
    return type.size() == box.type.size() &&
           std::equal(type.begin(), type.end(), box.type.begin());
}

bool ReadBox(const std::vector<uint8_t>& bytes, uint64_t offset, uint64_t end,
             Box* box) {
    if (box == nullptr || offset > end || end - offset < 8 ||
        offset > bytes.size() || bytes.size() - static_cast<size_t>(offset) < 8)
        return false;
    const size_t start = static_cast<size_t>(offset);
    const uint32_t size32 = ReadU32(bytes.data() + start);
    uint64_t size = size32;
    uint32_t header_size = 8;
    if (size32 == 1) {
        if (end - offset < 16 || bytes.size() - start < 16)
            return false;
        size = ReadU64(bytes.data() + start + 8);
        header_size = 16;
    } else if (size32 == 0) {
        size = end - offset;
    }
    if (size < header_size || size > end - offset)
        return false;
    box->offset = offset;
    box->size = size;
    box->header_size = header_size;
    std::copy_n(reinterpret_cast<const char*>(bytes.data() + start + 4),
                4, box->type.begin());
    return true;
}

bool FindChild(const std::vector<uint8_t>& bytes, const Box& parent,
               std::string_view type, Box* result) {
    if (result == nullptr || parent.offset > UINT64_MAX - parent.size)
        return false;
    uint64_t cursor = parent.offset + parent.header_size;
    const uint64_t end = parent.offset + parent.size;
    while (cursor < end) {
        Box child;
        if (!ReadBox(bytes, cursor, end, &child))
            return false;
        if (IsType(child, type)) {
            *result = child;
            return true;
        }
        cursor += child.size;
    }
    return false;
}

bool ReadExactAt(std::ifstream* input, uint64_t offset, uint8_t* destination,
                 size_t size) {
    if (input == nullptr || destination == nullptr ||
        offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()))
        return false;
    input->clear();
    input->seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!*input)
        return false;
    input->read(reinterpret_cast<char*>(destination),
                static_cast<std::streamsize>(size));
    return input->gcount() == static_cast<std::streamsize>(size);
}

bool ReadMovieAndMediaRanges(const std::string& path,
                             std::vector<uint8_t>* movie,
                             std::vector<ByteRange>* media_ranges) {
    if (movie == nullptr || media_ranges == nullptr)
        return false;
    std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
    if (!input)
        return false;
    input.seekg(0, std::ios::end);
    const std::streamoff end = input.tellg();
    if (end < 16)
        return false;
    const uint64_t file_size = static_cast<uint64_t>(end);
    uint64_t cursor = 0;
    bool found_movie = false;
    media_ranges->clear();
    while (cursor + 8 <= file_size) {
        std::array<uint8_t, 16> header{};
        if (!ReadExactAt(&input, cursor, header.data(), 8))
            return false;
        const uint32_t size32 = ReadU32(header.data());
        uint64_t box_size = size32;
        uint32_t header_size = 8;
        if (size32 == 1) {
            if (cursor + 16 > file_size || !ReadExactAt(&input, cursor, header.data(), 16))
                return false;
            box_size = ReadU64(header.data() + 8);
            header_size = 16;
        } else if (size32 == 0) {
            box_size = file_size - cursor;
        }
        if (box_size < header_size || box_size > file_size - cursor)
            return false;
        const char* type = reinterpret_cast<const char*>(header.data() + 4);
        if (std::memcmp(type, "mdat", 4) == 0) {
            media_ranges->push_back({cursor + header_size, box_size - header_size});
        } else if (std::memcmp(type, "moov", 4) == 0 && !found_movie) {
            if (box_size > kMaxMovieBoxSize || box_size > std::numeric_limits<size_t>::max())
                return false;
            movie->resize(static_cast<size_t>(box_size));
            if (!ReadExactAt(&input, cursor, movie->data(), movie->size()))
                return false;
            found_movie = true;
        }
        cursor += box_size;
    }
    return found_movie && !media_ranges->empty();
}

bool IsAudioTrack(const std::vector<uint8_t>& movie, const Box& track) {
    Box mdia;
    Box handler;
    if (!FindChild(movie, track, "mdia", &mdia) ||
        !FindChild(movie, mdia, "hdlr", &handler) ||
        handler.size < handler.header_size + 12)
        return false;
    const size_t handler_type_offset =
        static_cast<size_t>(handler.offset + handler.header_size + 8);
    return std::memcmp(movie.data() + handler_type_offset, "soun", 4) == 0;
}

bool ReadIamfDescriptorBytes(const std::vector<uint8_t>& movie,
                             const Box& track,
                             std::vector<uint8_t>* descriptor_bytes) {
    if (descriptor_bytes == nullptr)
        return false;
    Box mdia, minf, stbl, stsd;
    if (!FindChild(movie, track, "mdia", &mdia) ||
        !FindChild(movie, mdia, "minf", &minf) ||
        !FindChild(movie, minf, "stbl", &stbl) ||
        !FindChild(movie, stbl, "stsd", &stsd) ||
        stsd.size < stsd.header_size + 8)
        return false;
    const size_t stsd_body = static_cast<size_t>(stsd.offset + stsd.header_size);
    const uint32_t entry_count = ReadU32(movie.data() + stsd_body + 4);
    if (entry_count != 1)
        return false;
    Box sample_entry;
    if (!ReadBox(movie, stsd.offset + stsd.header_size + 8,
                 stsd.offset + stsd.size, &sample_entry) ||
        !IsType(sample_entry, "iamf") ||
        sample_entry.size < sample_entry.header_size + 28)
        return false;

    uint64_t child_cursor = sample_entry.offset + sample_entry.header_size + 28;
    const uint64_t sample_end = sample_entry.offset + sample_entry.size;
    while (child_cursor < sample_end) {
        Box child;
        if (!ReadBox(movie, child_cursor, sample_end, &child))
            return false;
        if (IsType(child, "iacb")) {
            if (child.size < child.header_size + 2)
                return false;
            const size_t payload = static_cast<size_t>(child.offset + child.header_size);
            if (movie[payload] != 1)
                return false;
            size_t offset = payload + 1;
            uint64_t descriptor_size = 0;
            bool complete = false;
            for (int i = 0; i < 10 && offset < child.offset + child.size; ++i) {
                const uint8_t value = movie[offset++];
                if (i == 9 && (value & 0x7e) != 0)
                    return false;
                descriptor_size |= static_cast<uint64_t>(value & 0x7f) << (i * 7);
                if ((value & 0x80) == 0) {
                    complete = true;
                    break;
                }
            }
            if (!complete || descriptor_size == 0 ||
                descriptor_size != child.offset + child.size - offset ||
                descriptor_size > std::numeric_limits<size_t>::max())
                return false;
            descriptor_bytes->assign(movie.begin() + static_cast<ptrdiff_t>(offset),
                                     movie.begin() + static_cast<ptrdiff_t>(offset + descriptor_size));
            return true;
        }
        child_cursor += child.size;
    }
    return false;
}

bool ReadSampleSizes(const std::vector<uint8_t>& movie, const Box& stsz,
                     std::vector<uint32_t>* sizes) {
    if (sizes == nullptr || stsz.size < stsz.header_size + 12)
        return false;
    const size_t body = static_cast<size_t>(stsz.offset + stsz.header_size);
    const uint32_t fixed_size = ReadU32(movie.data() + body + 4);
    const uint32_t sample_count = ReadU32(movie.data() + body + 8);
    if (sample_count == 0 || sample_count > kMaxSampleCount)
        return false;
    sizes->clear();
    sizes->reserve(sample_count);
    if (fixed_size != 0) {
        sizes->assign(sample_count, fixed_size);
        return true;
    }
    if (stsz.size - stsz.header_size - 12 < static_cast<uint64_t>(sample_count) * 4)
        return false;
    for (uint32_t i = 0; i < sample_count; ++i) {
        const uint32_t sample_size = ReadU32(movie.data() + body + 12 + static_cast<size_t>(i) * 4);
        if (sample_size == 0)
            return false;
        sizes->push_back(sample_size);
    }
    return true;
}

bool ReadSampleOffsets(const std::vector<uint8_t>& movie, const Box& track,
                       std::vector<uint64_t>* offsets,
                       std::vector<uint32_t>* sizes) {
    Box mdia, minf, stbl, stsz, stsc, chunk_offsets;
    if (!FindChild(movie, track, "mdia", &mdia) ||
        !FindChild(movie, mdia, "minf", &minf) ||
        !FindChild(movie, minf, "stbl", &stbl) ||
        !FindChild(movie, stbl, "stsz", &stsz) ||
        !FindChild(movie, stbl, "stsc", &stsc) ||
        (!FindChild(movie, stbl, "stco", &chunk_offsets) &&
         !FindChild(movie, stbl, "co64", &chunk_offsets)) ||
        !ReadSampleSizes(movie, stsz, sizes) ||
        stsc.size < stsc.header_size + 8 ||
        chunk_offsets.size < chunk_offsets.header_size + 8)
        return false;

    const size_t stsc_body = static_cast<size_t>(stsc.offset + stsc.header_size);
    const uint32_t stsc_count = ReadU32(movie.data() + stsc_body + 4);
    if (stsc_count == 0 ||
        stsc.size - stsc.header_size - 8 < static_cast<uint64_t>(stsc_count) * 12)
        return false;
    std::vector<SampleToChunk> mappings;
    mappings.reserve(stsc_count);
    for (uint32_t i = 0; i < stsc_count; ++i) {
        const size_t entry = stsc_body + 8 + static_cast<size_t>(i) * 12;
        SampleToChunk mapping{ReadU32(movie.data() + entry), ReadU32(movie.data() + entry + 4),
                              ReadU32(movie.data() + entry + 8)};
        if (mapping.first_chunk == 0 || mapping.samples_per_chunk == 0 ||
            mapping.sample_description_index != 1 ||
            (!mappings.empty() && mapping.first_chunk <= mappings.back().first_chunk))
            return false;
        mappings.push_back(mapping);
    }

    const size_t chunks_body = static_cast<size_t>(chunk_offsets.offset + chunk_offsets.header_size);
    const uint32_t chunk_count = ReadU32(movie.data() + chunks_body + 4);
    const bool use_co64 = IsType(chunk_offsets, "co64");
    const uint64_t chunk_entry_size = use_co64 ? 8 : 4;
    if (chunk_count == 0 ||
        chunk_offsets.size - chunk_offsets.header_size - 8 <
            static_cast<uint64_t>(chunk_count) * chunk_entry_size)
        return false;

    offsets->clear();
    offsets->reserve(sizes->size());
    size_t sample_index = 0;
    size_t mapping_index = 0;
    for (uint32_t chunk = 1; chunk <= chunk_count && sample_index < sizes->size(); ++chunk) {
        while (mapping_index + 1 < mappings.size() &&
               mappings[mapping_index + 1].first_chunk <= chunk)
            ++mapping_index;
        const size_t entry = chunks_body + 8 + static_cast<size_t>(chunk - 1) * chunk_entry_size;
        uint64_t offset = use_co64 ? ReadU64(movie.data() + entry) : ReadU32(movie.data() + entry);
        for (uint32_t in_chunk = 0;
             in_chunk < mappings[mapping_index].samples_per_chunk && sample_index < sizes->size();
             ++in_chunk) {
            offsets->push_back(offset);
            if ((*sizes)[sample_index] > std::numeric_limits<uint64_t>::max() - offset)
                return false;
            offset += (*sizes)[sample_index++];
        }
    }
    return sample_index == sizes->size();
}

bool SampleRangesAreInMediaData(const std::vector<ByteRange>& media_ranges,
                                const std::vector<uint64_t>& offsets,
                                const std::vector<uint32_t>& sizes) {
    if (offsets.size() != sizes.size())
        return false;
    for (size_t i = 0; i < offsets.size(); ++i) {
        const uint64_t offset = offsets[i];
        const uint64_t size = sizes[i];
        const bool contained = std::any_of(media_ranges.begin(), media_ranges.end(),
            [offset, size](const ByteRange& range) {
                return offset >= range.offset && offset <= range.offset + range.size &&
                       size <= range.offset + range.size - offset;
            });
        if (!contained)
            return false;
    }
    return true;
}

std::filesystem::path NewTemporaryIamfPath() {
    static std::atomic<uint64_t> sequence{0};
    std::error_code ec;
    const auto directory = std::filesystem::temp_directory_path(ec);
    if (ec)
        return {};
    std::random_device random;
    for (unsigned int attempt = 0; attempt < 32; ++attempt) {
        const uint64_t nonce = (static_cast<uint64_t>(random()) << 32) ^ random() ^
                               sequence.fetch_add(1, std::memory_order_relaxed) ^
                               static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto candidate = directory / ("twinkle-iamf-audio-" + std::to_string(nonce) + ".iamf");
        ec.clear();
        if (!std::filesystem::exists(candidate, ec) && !ec)
            return candidate;
    }
    return {};
}

bool ExtractOneTrack(const std::string& mp4_path,
                     const std::vector<uint8_t>& movie,
                     const std::vector<ByteRange>& media_ranges,
                     const Box& track,
                     const std::function<bool()>& cancellation_requested,
                     std::string* validated_path,
                     std::string* extraction_error) {
    if (validated_path == nullptr)
        return false;
    std::vector<uint8_t> descriptor_bytes;
    std::vector<uint64_t> sample_offsets;
    std::vector<uint32_t> sample_sizes;
    if (!ReadIamfDescriptorBytes(movie, track, &descriptor_bytes) ||
        !ReadSampleOffsets(movie, track, &sample_offsets, &sample_sizes) ||
        !SampleRangesAreInMediaData(media_ranges, sample_offsets, sample_sizes))
        return false;

    std::filesystem::path temporary_path;
    try {
        temporary_path = NewTemporaryIamfPath();
    } catch (const std::exception& exception) {
        SetError(extraction_error, std::string("cannot allocate temporary IAMF validation file: ") + exception.what());
        return false;
    }
    if (temporary_path.empty()) {
        SetError(extraction_error, "cannot allocate temporary IAMF validation file");
        return false;
    }
    const std::string temporary_path_utf8 = path_utils::ToUtf8(temporary_path);
    bool keep_file = false;
    struct RemoveOnFailure {
        const std::filesystem::path& path;
        bool& keep;
        ~RemoveOnFailure() {
            if (!keep) {
                std::error_code ec;
                std::filesystem::remove(path, ec);
            }
        }
    } remove_on_failure{temporary_path, keep_file};

    std::ifstream source(std::filesystem::u8path(mp4_path), std::ios::binary);
    std::ofstream output(temporary_path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!source || !output) {
        SetError(extraction_error, "cannot open MP4 audio data or temporary IAMF validation file");
        return false;
    }
    output.write(reinterpret_cast<const char*>(descriptor_bytes.data()),
                 static_cast<std::streamsize>(descriptor_bytes.size()));
    std::vector<uint8_t> buffer(static_cast<size_t>(kCopyBufferSize));
    for (size_t sample = 0; sample < sample_offsets.size(); ++sample) {
        uint64_t offset = sample_offsets[sample];
        uint64_t remaining = sample_sizes[sample];
        while (remaining != 0) {
            if (cancellation_requested && cancellation_requested()) {
                SetError(extraction_error, "operation cancelled while extracting MP4 IAMF audio");
                return false;
            }
            const size_t count = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
            if (!ReadExactAt(&source, offset, buffer.data(), count)) {
                SetError(extraction_error, "failed to read an IAMF sample from the MP4 input");
                return false;
            }
            output.write(reinterpret_cast<const char*>(buffer.data()),
                         static_cast<std::streamsize>(count));
            if (!output) {
                SetError(extraction_error, "failed to write the temporary IAMF validation file");
                return false;
            }
            offset += count;
            remaining -= count;
        }
    }
    output.flush();
    if (!output) {
        SetError(extraction_error, "failed to flush the temporary IAMF validation file");
        return false;
    }
    output.close();

#if defined(IMMERSIVE_AUDIO_MUXER_HAVE_INPROCESS_IAMF_TOOLS)
    std::array<char, 1024> probe_error{};
    uint32_t temporal_units = 0;
    const int probe_result = iamf_tools_probe_file(
        temporary_path_utf8.c_str(), descriptor_bytes.size(), &temporal_units,
        probe_error.data(), probe_error.size());
    if (probe_result == IAMF_TOOLS_PROBE_INVALID_IAMF)
        return false;
    if (probe_result != IAMF_TOOLS_PROBE_SUCCESS) {
        const std::string detail = probe_error[0] == '\0'
                                       ? std::string()
                                       : ": " + std::string(probe_error.data());
        SetError(extraction_error,
                 "iamf-tools could not complete embedded IAMF validation" + detail);
        return false;
    }
    if (temporal_units == 0)
        return false;
    keep_file = true;
    *validated_path = temporary_path_utf8;
    return true;
#else
    SetError(extraction_error, "iamf-tools Probe support is unavailable for embedded IAMF validation");
    return false;
#endif
}

}  // namespace

bool ExtractValidatedIamfAudioTracks(
    const std::string& mp4_path, std::vector<std::string>* iamf_paths,
    const std::function<bool()>& cancellation_requested, std::string* error) {
    if (iamf_paths == nullptr || mp4_path.empty()) {
        SetError(error, "invalid MP4 IAMF extraction request");
        return false;
    }
    iamf_paths->clear();
    if (cancellation_requested && cancellation_requested()) {
        SetError(error, "operation cancelled while inspecting MP4 IAMF tracks");
        return false;
    }

    std::vector<uint8_t> movie;
    std::vector<ByteRange> media_ranges;
    if (!ReadMovieAndMediaRanges(mp4_path, &movie, &media_ranges))
        return true;  // The video open path handles container errors separately.

    Box root;
    if (!ReadBox(movie, 0, movie.size(), &root) || !IsType(root, "moov"))
        return true;
    uint64_t cursor = root.offset + root.header_size;
    const uint64_t end = root.offset + root.size;
    while (cursor < end) {
        if (cancellation_requested && cancellation_requested()) {
            RemoveTemporaryIamfAudioTracks(iamf_paths);
            SetError(error, "operation cancelled while inspecting MP4 IAMF tracks");
            return false;
        }
        Box child;
        if (!ReadBox(movie, cursor, end, &child))
            break;
        cursor += child.size;
        if (!IsType(child, "trak") || !IsAudioTrack(movie, child))
            continue;
        std::string validated_path;
        std::string extraction_error;
        if (ExtractOneTrack(mp4_path, movie, media_ranges, child,
                            cancellation_requested, &validated_path,
                            &extraction_error)) {
            iamf_paths->push_back(std::move(validated_path));
        } else if (!extraction_error.empty()) {
            RemoveTemporaryIamfAudioTracks(iamf_paths);
            SetError(error, extraction_error);
            return false;
        }
        if (cancellation_requested && cancellation_requested()) {
            RemoveTemporaryIamfAudioTracks(iamf_paths);
            SetError(error, "operation cancelled while validating MP4 IAMF tracks");
            return false;
        }
    }
    return true;
}

void RemoveTemporaryIamfAudioTracks(std::vector<std::string>* iamf_paths) {
    if (iamf_paths == nullptr)
        return;
    for (const std::string& path : *iamf_paths) {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::u8path(path), ec);
    }
    iamf_paths->clear();
}

}  // namespace immersive_audio_muxer
