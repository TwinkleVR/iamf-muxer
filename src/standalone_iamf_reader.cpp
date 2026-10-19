#include "standalone_iamf_reader.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <set>
#include <utility>

namespace immersive_audio_muxer {
namespace {
constexpr uint8_t kObuCodecConfig = 0;
constexpr uint8_t kObuAudioElement = 1;
constexpr uint8_t kObuMixPresentation = 2;
constexpr uint8_t kObuParameterBlock = 3;
constexpr uint8_t kObuTemporalDelimiter = 4;
constexpr uint8_t kObuAudioFrame = 5;
constexpr uint8_t kObuAudioFrameId0 = 6;
constexpr uint8_t kObuAudioFrameId17 = 23;
constexpr uint8_t kObuIaSequenceHeader = 31;
constexpr uint32_t kOpusCodecId = 0x4f707573;
constexpr uint32_t kLpcmCodecId = 0x6970636d;
constexpr uint32_t kAacLcCodecId = 0x6d703461;
constexpr uint64_t kMaxTemporalUnitReadBytes = 64 * 1024 * 1024;
constexpr uint64_t kMaxObuHeaderBytes = 1024 * 1024;
constexpr uint64_t kMaxDescriptorObuBytes = 16 * 1024 * 1024;
constexpr uint64_t kMaxDescriptorBytes = 64 * 1024 * 1024;
constexpr uint64_t kMaxIndexedTemporalUnits = 16 * 1024 * 1024;
constexpr uint32_t kFlacCodecId = 0x664c6143;
void SetError(std::string* error, const std::string& message) {
    if (error)
        *error = message;
}
uint32_t ReadU32Be(const std::vector<uint8_t>& b, size_t o) {
    return (static_cast<uint32_t>(b[o]) << 24) | (static_cast<uint32_t>(b[o + 1]) << 16) | (static_cast<uint32_t>(b[o + 2]) << 8) |
           static_cast<uint32_t>(b[o + 3]);
}
uint16_t ReadU16Be(const std::vector<uint8_t>& b, size_t o) {
    return static_cast<uint16_t>((b[o] << 8) | b[o + 1]);
}
uint32_t ReadU24Be(const std::vector<uint8_t>& b, size_t o) {
    return (static_cast<uint32_t>(b[o]) << 16) | (static_cast<uint32_t>(b[o + 1]) << 8) |
           static_cast<uint32_t>(b[o + 2]);
}
uint64_t ReadU64Be(const std::vector<uint8_t>& b, size_t o) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i)
        value = (value << 8) | b[o + i];
    return value;
}
uint32_t AacSampleRate(uint8_t i) {
    static constexpr uint32_t k[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    return i < sizeof(k) / sizeof(k[0]) ? k[i] : 0;
}
bool IsFlacSampleRate(uint32_t sample_rate) {
    switch (sample_rate) {
        case 8000:
        case 16000:
        case 22050:
        case 24000:
        case 32000:
        case 44100:
        case 48000:
        case 88200:
        case 96000:
        case 176400:
        case 192000:
            return true;
        default:
            return false;
    }
}

bool ReadHeaderUleb(const std::vector<uint8_t>& bytes, size_t* offset, uint64_t* value, bool* truncated) {
    if (truncated)
        *truncated = false;
    if (offset == nullptr || value == nullptr)
        return false;
    *value = 0;
    for (int i = 0; i < 10; ++i) {
        if (*offset >= bytes.size()) {
            if (truncated)
                *truncated = true;
            return false;
        }
        const uint8_t byte = bytes[(*offset)++];
        if (i == 9 && (byte & 0x7e))
            return false;
        *value |= static_cast<uint64_t>(byte & 0x7f) << (i * 7);
        if ((byte & 0x80) == 0)
            return true;
    }
    return false;
}
}  // namespace

bool StandaloneIamfReader::ReadUleb(const std::vector<uint8_t>& b, size_t* o, uint64_t* v) {
    if (!o || !v)
        return false;
    *v = 0;
    for (int i = 0; i < 10; ++i) {
        if (*o >= b.size())
            return false;
        const uint8_t x = b[(*o)++];
        if (i == 9 && (x & 0x7e))
            return false;
        *v |= static_cast<uint64_t>(x & 0x7f) << (i * 7);
        if (!(x & 0x80))
            return true;
    }
    return false;
}
bool StandaloneIamfReader::ReadString(const std::vector<uint8_t>& bytes,
                                       size_t* offset, std::string* value) {
    constexpr size_t kIamfMaxStringSize = 128;
    if (!offset || !value)
        return false;

    value->clear();
    for (size_t i = 0; i < kIamfMaxStringSize; ++i) {
        if (*offset >= bytes.size())
            return false;
        const uint8_t byte = bytes[(*offset)++];
        if (byte == 0)
            return true;
        value->push_back(static_cast<char>(byte));
    }
    return false;
}

bool StandaloneIamfReader::IsAudioFrameObuType(uint8_t t) {
    return t == kObuAudioFrame || (t >= kObuAudioFrameId0 && t <= kObuAudioFrameId17);
}
bool StandaloneIamfReader::IsTemporalUnitObuType(uint8_t t) {
    return t == kObuParameterBlock || t == kObuTemporalDelimiter || IsAudioFrameObuType(t);
}
bool StandaloneIamfReader::ReadRange(uint64_t o, uint64_t n, std::vector<uint8_t>* out, std::string* e) const {
    if (!out || o > file_size_ || n > file_size_ - o || n > std::numeric_limits<size_t>::max()) {
        SetError(e, "IAMF byte range is invalid");
        return false;
    }
    out->resize(static_cast<size_t>(n));
    if (!n)
        return true;
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(o), std::ios::beg);
    file_.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(n));
    if (file_.gcount() != static_cast<std::streamsize>(n)) {
        SetError(e, "failed to read IAMF byte range");
        return false;
    }
    return true;
}
bool StandaloneIamfReader::ReadObuMetadata(uint64_t o, ObuMetadata* m, std::string* e) const {
    if (!m || o >= file_size_) {
        SetError(e, "IAMF OBU offset is outside the file");
        return false;
    }
    *m = {};
    std::vector<uint8_t> h;
    uint64_t requested = std::min<uint64_t>(file_size_ - o, 32);
    while (true) {
        if (!ReadRange(o, requested, &h, e) || h.empty())
            return false;
        m->type = h[0] >> 3;
        m->flags = h[0] & 7;
        size_t p = 1;
        uint64_t n = 0;
        bool truncated = false;
        if (!ReadHeaderUleb(h, &p, &n, &truncated)) {
            if (truncated && requested < std::min<uint64_t>(file_size_ - o, kMaxObuHeaderBytes)) {
                requested = std::min<uint64_t>(file_size_ - o, std::min<uint64_t>(kMaxObuHeaderBytes, requested * 2));
                continue;
            }
            SetError(e, "invalid IAMF OBU size");
            return false;
        }
        if (p > file_size_ - o || n > file_size_ - o - p) {
            SetError(e, "invalid IAMF OBU size");
            return false;
        }
        const size_t size_end = p;
        if (IsAudioFrameObuType(m->type) && (m->flags & 2)) {
            uint64_t v = 0;
            if (!ReadHeaderUleb(h, &p, &v, &truncated)) {
                if (truncated && requested < std::min<uint64_t>(file_size_ - o, kMaxObuHeaderBytes)) {
                    requested = std::min<uint64_t>(file_size_ - o, std::min<uint64_t>(kMaxObuHeaderBytes, requested * 2));
                    continue;
                }
                SetError(e, "invalid IAMF audio frame end trim");
                return false;
            }
            if (v > UINT32_MAX) {
                SetError(e, "invalid IAMF audio frame end trim");
                return false;
            }
            m->trim_at_end = static_cast<uint32_t>(v);
            if (!ReadHeaderUleb(h, &p, &v, &truncated)) {
                if (truncated && requested < std::min<uint64_t>(file_size_ - o, kMaxObuHeaderBytes)) {
                    requested = std::min<uint64_t>(file_size_ - o, std::min<uint64_t>(kMaxObuHeaderBytes, requested * 2));
                    continue;
                }
                SetError(e, "invalid IAMF audio frame start trim");
                return false;
            }
            if (v > UINT32_MAX) {
                SetError(e, "invalid IAMF audio frame start trim");
                return false;
            }
            m->trim_at_start = static_cast<uint32_t>(v);
        }
        if (m->flags & 1) {
            uint64_t x = 0;
            if (!ReadHeaderUleb(h, &p, &x, &truncated)) {
                if (truncated && requested < std::min<uint64_t>(file_size_ - o, kMaxObuHeaderBytes)) {
                    requested = std::min<uint64_t>(file_size_ - o, std::min<uint64_t>(kMaxObuHeaderBytes, requested * 2));
                    continue;
                }
                SetError(e, "invalid IAMF OBU extension header");
                return false;
            }
            if (x > n || x > file_size_ - o - p || x > std::numeric_limits<size_t>::max()) {
                SetError(e, "invalid IAMF OBU extension header");
                return false;
            }
            p += static_cast<size_t>(x);
        }
        if (p > file_size_ - o || n < p - 1) {
            SetError(e, "invalid IAMF OBU payload size");
            return false;
        }
        const uint64_t header_fields = p - size_end;
        if (header_fields > n) {
            SetError(e, "invalid IAMF OBU header fields");
            return false;
        }
        m->total_size = size_end + n;
        m->payload_offset = o + p;
        m->payload_size = n - header_fields;
        if (m->total_size > file_size_ - o) {
            SetError(e, "IAMF OBU exceeds file size");
            return false;
        }
        return true;
    }
}

bool StandaloneIamfReader::ParseSequenceHeader(const ObuMetadata& m, std::string* e) {
    if (m.type != kObuIaSequenceHeader) {
        SetError(e, "invalid IAMF IA Sequence Header metadata");
        return false;
    }
    if (m.payload_size > kMaxDescriptorObuBytes) {
        SetError(e, "IAMF sequence header OBU is too large");
        return false;
    }
    std::vector<uint8_t> payload;
    if (!ReadRange(m.payload_offset, m.payload_size, &payload, e) || payload.size() < 6) {
        SetError(e, "invalid IAMF IA Sequence Header OBU");
        return false;
    }
    if (payload[0] != 'i' || payload[1] != 'a' || payload[2] != 'm' || payload[3] != 'f') {
        SetError(e, "IAMF IA Sequence Header has an invalid ia_code FourCC");
        return false;
    }
    summary_.primary_profile = payload[4];
    summary_.additional_profile = payload[5];
    summary_.has_sequence_header = true;
    return true;
}
bool StandaloneIamfReader::ParseCodecConfig(const ObuMetadata& m, std::string* e) {
    if (m.payload_size > kMaxDescriptorObuBytes) {
        SetError(e, "IAMF codec config OBU is too large");
        return false;
    }
    std::vector<uint8_t> p;
    if (!ReadRange(m.payload_offset, m.payload_size, &p, e))
        return false;
    size_t c = 0;
    uint64_t ignored = 0, frame = 0;
    if (!ReadUleb(p, &c, &ignored) || c + 6 > p.size()) {
        SetError(e, "invalid IAMF Codec Config OBU");
        return false;
    }
    codec_id_ = ReadU32Be(p, c);
    c += 4;
    if (!ReadUleb(p, &c, &frame) || !frame || frame > UINT32_MAX || c + 2 > p.size()) {
        SetError(e, "invalid IAMF Codec Config frame size");
        return false;
    }
    audio_roll_distance_ = static_cast<int16_t>(ReadU16Be(p, c));
    c += 2;
    summary_.frame_size = static_cast<uint32_t>(frame);
    if (codec_id_ == kOpusCodecId) {
        summary_.sample_rate = 48000;
        return true;
    }
    if (codec_id_ == kLpcmCodecId) {
        if (c > p.size() || p.size() - c < 6) {
            SetError(e, "invalid IAMF LPCM decoder config");
            return false;
        }
        const uint8_t sample_format_flags = p[c++];
        const uint8_t sample_size = p[c++];
        const uint32_t sample_rate = ReadU32Be(p, c);
        if (sample_format_flags != 0 && sample_format_flags != 1) {
            SetError(e, "unsupported IAMF LPCM sample format flags");
            return false;
        }
        if (sample_size != 16 && sample_size != 24 && sample_size != 32) {
            SetError(e, "unsupported IAMF LPCM sample size");
            return false;
        }
        if (sample_rate != 16000 && sample_rate != 32000 && sample_rate != 44100 && sample_rate != 48000 && sample_rate != 96000) {
            SetError(e, "unsupported IAMF LPCM sample rate");
            return false;
        }
        if (audio_roll_distance_ != 0) {
            SetError(e, "invalid IAMF LPCM audio roll distance");
            return false;
        }
        summary_.sample_rate = sample_rate;
        return true;
    }
    if (codec_id_ == kFlacCodecId) {
        if (audio_roll_distance_ != 0 || c >= p.size()) {
            SetError(e, "invalid IAMF FLAC audio roll distance or decoder config");
            return false;
        }

        bool first_block = true;
        bool last_block = false;
        for (int block_count = 0; !last_block && block_count < 128; ++block_count) {
            if (p.size() - c < 4) {
                SetError(e, "truncated IAMF FLAC metadata block header");
                return false;
            }
            const uint8_t block_header = p[c++];
            last_block = (block_header & 0x80) != 0;
            const uint8_t block_type = block_header & 0x7f;
            const uint32_t block_length = ReadU24Be(p, c);
            c += 3;
            if (block_length > p.size() - c) {
                SetError(e, "truncated IAMF FLAC metadata block");
                return false;
            }
            if (first_block) {
                if (block_type != 0 || block_length != 34 || p.size() - c < 34) {
                    SetError(e, "IAMF FLAC decoder config must start with a 34-byte STREAMINFO block");
                    return false;
                }
                const uint16_t minimum_block_size = ReadU16Be(p, c);
                const uint16_t maximum_block_size = ReadU16Be(p, c + 2);
                const uint64_t packed = ReadU64Be(p, c + 10);
                const uint32_t sample_rate = static_cast<uint32_t>(packed >> 44);
                const uint8_t number_of_channels = static_cast<uint8_t>((packed >> 41) & 0x7);
                const uint8_t bits_per_sample = static_cast<uint8_t>((packed >> 36) & 0x1f);
                if (minimum_block_size != frame || maximum_block_size != frame) {
                    SetError(e, "IAMF FLAC STREAMINFO block size does not match samples per frame");
                    return false;
                }
                if (number_of_channels != 1 ||
                    (bits_per_sample != 15 && bits_per_sample != 23 && bits_per_sample != 31) ||
                    !IsFlacSampleRate(sample_rate)) {
                    SetError(e, "unsupported IAMF FLAC STREAMINFO values");
                    return false;
                }
                summary_.sample_rate = sample_rate;
                first_block = false;
            }
            c += block_length;
        }
        if (first_block || !last_block) {
            SetError(e, "IAMF FLAC metadata blocks are incomplete");
            return false;
        }
        return true;
    }
    if (codec_id_ != kAacLcCodecId) {
        SetError(e, "unsupported IAMF codec for independent MP4 writer");
        return false;
    }
    if (c >= p.size()) {
        SetError(e, "invalid IAMF AAC decoder config");
        return false;
    }
    ++c;
    uint64_t ds = 0;
    if (!ReadUleb(p, &c, &ds) || ds < 13 || ds > p.size() - c) {
        SetError(e, "invalid IAMF AAC decoder config descriptor");
        return false;
    }
    const size_t decoder_config_end = c + static_cast<size_t>(ds);
    c += 13;
    if (c >= decoder_config_end) {
        SetError(e, "missing IAMF AAC DecoderSpecificInfo");
        return false;
    }
    ++c;
    uint64_t ss = 0;
    if (!ReadUleb(p, &c, &ss) || c > decoder_config_end || ss > decoder_config_end - c || ss < 2) {
        SetError(e, "invalid IAMF AAC DecoderSpecificInfo");
        return false;
    }
    const uint8_t sample_frequency_index = static_cast<uint8_t>(((p[c] & 0x07) << 1) | (p[c + 1] >> 7));
    summary_.sample_rate = AacSampleRate(sample_frequency_index);
    if (!summary_.sample_rate) {
        SetError(e, "unsupported IAMF AAC sample rate");
        return false;
    }
    if (audio_roll_distance_ != -1) {
        SetError(e, "invalid IAMF AAC-LC audio roll distance");
        return false;
    }
    return true;
}
bool StandaloneIamfReader::ParseMixPresentationAnnotation(
    const ObuMetadata& metadata, std::string* error) {
    if (metadata.type != kObuMixPresentation) {
        SetError(error, "invalid Mix Presentation OBU metadata");
        return false;
    }
    if (metadata.payload_size > kMaxDescriptorObuBytes) {
        SetError(error, "IAMF Mix Presentation OBU is too large");
        return false;
    }

    std::vector<uint8_t> payload;
    if (!ReadRange(metadata.payload_offset, metadata.payload_size, &payload,
                   error)) {
        return false;
    }

    size_t offset = 0;
    uint64_t ignored_mix_presentation_id = 0;
    uint64_t count_label = 0;
    if (!ReadUleb(payload, &offset, &ignored_mix_presentation_id) ||
        !ReadUleb(payload, &offset, &count_label) || count_label > 256) {
        SetError(error, "invalid IAMF Mix Presentation annotation header");
        return false;
    }

    for (uint64_t i = 0; i < count_label; ++i) {
        std::string ignored_language;
        if (!ReadString(payload, &offset, &ignored_language)) {
            SetError(error, "invalid IAMF Mix Presentation annotation language");
            return false;
        }
    }

    std::string first_annotation;
    for (uint64_t i = 0; i < count_label; ++i) {
        std::string annotation;
        if (!ReadString(payload, &offset, &annotation)) {
            SetError(error, "invalid IAMF Mix Presentation annotation");
            return false;
        }
        if (i == 0)
            first_annotation = std::move(annotation);
    }

    if (!mix_presentation_annotation_set_) {
        summary_.mix_presentation_annotation = std::move(first_annotation);
        mix_presentation_annotation_set_ = true;
    }
    return true;
}

bool StandaloneIamfReader::ParseAudioElementSubstreamIds(
    const ObuMetadata& metadata, std::set<uint32_t>* ids, std::string* e) const {
    if (!ids || metadata.type != kObuAudioElement) {
        SetError(e, "invalid Audio Element OBU metadata");
        return false;
    }
    if (metadata.payload_size > kMaxDescriptorObuBytes) {
        SetError(e, "IAMF Audio Element OBU is too large");
        return false;
    }
    std::vector<uint8_t> payload;
    if (!ReadRange(metadata.payload_offset, metadata.payload_size, &payload, e))
        return false;
    size_t p = 0;
    uint64_t ignored_audio_element_id = 0;
    if (!ReadUleb(payload, &p, &ignored_audio_element_id) || p >= payload.size()) {
        SetError(e, "invalid Audio Element ID");
        return false;
    }
    // audio_element_type is 3 bits followed by 5 reserved bits.
    ++p;
    uint64_t ignored_codec_config_id = 0;
    uint64_t num_substreams = 0;
    if (!ReadUleb(payload, &p, &ignored_codec_config_id) ||
        !ReadUleb(payload, &p, &num_substreams) || num_substreams == 0 ||
        num_substreams > UINT32_MAX) {
        SetError(e, "invalid Audio Element descriptor header");
        return false;
    }
    for (uint64_t i = 0; i < num_substreams; ++i) {
        uint64_t substream_id = 0;
        if (!ReadUleb(payload, &p, &substream_id) || substream_id > UINT32_MAX) {
            SetError(e, "invalid Audio Element substream ID");
            return false;
        }
        ids->insert(static_cast<uint32_t>(substream_id));
    }
    return true;
}

bool StandaloneIamfReader::ParseAudioFrameId(ObuMetadata* metadata,
                                             std::string* e) const {
    if (!metadata || !IsAudioFrameObuType(metadata->type)) {
        SetError(e, "invalid Audio Frame OBU metadata");
        return false;
    }
    if (metadata->type >= kObuAudioFrameId0 &&
        metadata->type <= kObuAudioFrameId17) {
        metadata->audio_frame_id = metadata->type - kObuAudioFrameId0;
        metadata->has_audio_frame_id = true;
        return true;
    }
    std::vector<uint8_t> payload;
    if (!ReadRange(metadata->payload_offset, metadata->payload_size, &payload, e))
        return false;
    size_t p = 0;
    uint64_t substream_id = 0;
    if (!ReadUleb(payload, &p, &substream_id) || substream_id > UINT32_MAX) {
        SetError(e, "invalid explicit Audio Frame substream ID");
        return false;
    }
    metadata->audio_frame_id = static_cast<uint32_t>(substream_id);
    metadata->has_audio_frame_id = true;
    return true;
}
bool StandaloneIamfReader::Open(const std::string& path, std::string* e) {
    file_.close();
    file_.clear();
    descriptor_bytes_.clear();
    temporal_units_.clear();
    file_size_ = 0;
    summary_ = {};
    codec_id_ = 0;
    audio_roll_distance_ = 0;
    mix_presentation_annotation_set_ = false;

    file_.open(std::filesystem::u8path(path), std::ios::binary);
    if (!file_) {
        SetError(e, "cannot open standalone IAMF file: " + path);
        return false;
    }
    file_.seekg(0, std::ios::end);
    const std::streamsize s = file_.tellg();
    if (s <= 0) {
        SetError(e, "standalone IAMF file is empty");
        return false;
    }
    file_size_ = static_cast<uint64_t>(s);
    file_.clear();

    std::set<uint32_t> expected_audio_frame_ids;
    uint64_t offset = 0;
    bool codec = false;
    bool has_explicit_delimiters = false;
    while (offset < file_size_) {
        ObuMetadata metadata;
        if (!ReadObuMetadata(offset, &metadata, e))
            return false;
        if (!metadata.total_size) {
            SetError(e, "IAMF OBU scan did not advance");
            return false;
        }
        if (metadata.type == kObuIaSequenceHeader && !summary_.has_sequence_header) {
            if (!ParseSequenceHeader(metadata, e))
                return false;
        }
        if (metadata.type == kObuCodecConfig) {
            ++summary_.codec_config_count;
            if (!codec) {
                if (!ParseCodecConfig(metadata, e))
                    return false;
                codec = true;
            }
        }
        if (metadata.type == kObuAudioElement) {
            ++summary_.audio_element_count;
            if (!ParseAudioElementSubstreamIds(metadata, &expected_audio_frame_ids, e))
                return false;
        }
        if (metadata.type == kObuMixPresentation) {
            ++summary_.mix_presentation_count;
            if (!ParseMixPresentationAnnotation(metadata, e))
                return false;
        }
        if (metadata.type == kObuTemporalDelimiter)
            has_explicit_delimiters = true;
        if (IsAudioFrameObuType(metadata.type) &&
            !ParseAudioFrameId(&metadata, e))
            return false;
        offset += metadata.total_size;
    }

    uint64_t first = 0;
    uint64_t current = 0;
    bool found = false;
    std::set<uint32_t> current_audio_frame_ids;

    auto commit_unit = [&](uint64_t end_offset) -> bool {
        if (!found || end_offset <= current) {
            SetError(e, "IAMF temporal-unit boundary is invalid");
            return false;
        }
        IamfTemporalUnitSample sample;
        sample.index.byte_offset = current;
        sample.index.byte_size = end_offset - current;
        sample.index.num_samples = summary_.frame_size;
        if (summary_.total_samples > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            SetError(e, "IAMF temporal-unit timestamp overflows the supported range");
            return false;
        }
        sample.index.start_timestamp = static_cast<int64_t>(summary_.total_samples);
        if (summary_.total_samples > std::numeric_limits<uint64_t>::max() - summary_.frame_size) {
            SetError(e, "IAMF temporal-unit sample count overflows");
            return false;
        }
        if (temporal_units_.size() >= kMaxIndexedTemporalUnits ||
            temporal_units_.size() >= std::numeric_limits<uint32_t>::max()) {
            SetError(e, "IAMF contains too many temporal units");
            return false;
        }
        temporal_units_.push_back(std::move(sample));
        summary_.total_samples += summary_.frame_size;
        return true;
    };

    offset = 0;
    while (offset < file_size_) {
        const uint64_t obu_offset = offset;
        if (!found && obu_offset > kMaxDescriptorBytes) {
            SetError(e, "IAMF descriptor section is too large");
            return false;
        }
        ObuMetadata metadata;
        if (!ReadObuMetadata(obu_offset, &metadata, e))
            return false;
        offset += metadata.total_size;
        if (IsAudioFrameObuType(metadata.type) &&
            !ParseAudioFrameId(&metadata, e))
            return false;
        if (!IsTemporalUnitObuType(metadata.type))
            continue;
        if (!found) {
            found = true;
            first = obu_offset;
            current = obu_offset;
        }

        if (has_explicit_delimiters) {
            if (metadata.type == kObuTemporalDelimiter && obu_offset != current) {
                if (!expected_audio_frame_ids.empty() &&
                    current_audio_frame_ids != expected_audio_frame_ids) {
                    SetError(e, "IAMF temporal unit is missing an audio frame");
                    return false;
                }
                if (!commit_unit(obu_offset))
                    return false;
                current = obu_offset;
                current_audio_frame_ids.clear();
            }
            if (metadata.has_audio_frame_id) {
                if (!expected_audio_frame_ids.empty() &&
                    expected_audio_frame_ids.find(metadata.audio_frame_id) == expected_audio_frame_ids.end()) {
                    SetError(e, "IAMF Audio Frame references an unknown substream ID");
                    return false;
                }
                if (!current_audio_frame_ids.insert(metadata.audio_frame_id).second) {
                    SetError(e, "IAMF temporal unit contains a duplicate audio frame");
                    return false;
                }
            }
            continue;
        }

        if (metadata.type == kObuParameterBlock &&
            !current_audio_frame_ids.empty() &&
            !expected_audio_frame_ids.empty() &&
            current_audio_frame_ids == expected_audio_frame_ids) {
            if (!commit_unit(obu_offset))
                return false;
            current = obu_offset;
            current_audio_frame_ids.clear();
        }
        if (!metadata.has_audio_frame_id)
            continue;
        if (!expected_audio_frame_ids.empty() &&
            expected_audio_frame_ids.find(metadata.audio_frame_id) == expected_audio_frame_ids.end()) {
            SetError(e, "IAMF Audio Frame references an unknown substream ID");
            return false;
        }
        if (!current_audio_frame_ids.insert(metadata.audio_frame_id).second) {
            if (expected_audio_frame_ids.empty())
                expected_audio_frame_ids = current_audio_frame_ids;
            if (current_audio_frame_ids != expected_audio_frame_ids) {
                SetError(e, "IAMF implicit temporal unit has an incomplete audio frame set");
                return false;
            }
            if (!commit_unit(obu_offset))
                return false;
            current = obu_offset;
            current_audio_frame_ids.clear();
            current_audio_frame_ids.insert(metadata.audio_frame_id);
        }
    }

    if (!summary_.has_sequence_header || !codec || !found || current >= file_size_) {
        SetError(e, "IAMF file has no valid IA Sequence Header, Codec Config, or temporal units");        return false;
    }
    if (!expected_audio_frame_ids.empty() &&
        current_audio_frame_ids != expected_audio_frame_ids) {
        SetError(e, "IAMF final temporal unit has an incomplete audio frame set");
        return false;
    }
    if (!commit_unit(file_size_))
        return false;

    for (auto& sample : temporal_units_) {
        uint64_t p = sample.index.byte_offset;
        const uint64_t end = p + sample.index.byte_size;
        while (p < end) {
            ObuMetadata metadata;
            if (!ReadObuMetadata(p, &metadata, e))
                return false;
            if (IsAudioFrameObuType(metadata.type)) {
                sample.index.trim_at_start = metadata.trim_at_start;
                sample.index.trim_at_end = metadata.trim_at_end;
                break;
            }
            p += metadata.total_size;
        }
    }

    summary_.descriptor_bytes = first;
    if (first > kMaxDescriptorBytes) {
        SetError(e, "IAMF descriptor section is too large");
        return false;
    }
    summary_.temporal_unit_bytes = file_size_ - first;
    summary_.temporal_unit_count = static_cast<uint32_t>(temporal_units_.size());
    summary_.scan_reached_eof = true;
    if (!ReadRange(0, first, &descriptor_bytes_, e))
        return false;
    return true;
}
bool StandaloneIamfReader::ValidateObjectAudioAndParameters(std::string* e) const {
    if (!summary_.has_sequence_header || descriptor_bytes_.empty() || temporal_units_.empty() ||
        !summary_.sample_rate || !summary_.frame_size || summary_.codec_config_count == 0 ||
        summary_.audio_element_count == 0) {
        SetError(e, "IAMF descriptor or temporal-unit index is incomplete");
        return false;
    }
    return true;
}
bool StandaloneIamfReader::ReadTemporalUnit(size_t i, std::vector<uint8_t>* b, std::string* e) const {
    if (!b || i >= temporal_units_.size()) {
        SetError(e, "temporal unit index out of range");
        return false;
    }
    const uint64_t size = temporal_units_[i].index.byte_size;
    if (size > kMaxTemporalUnitReadBytes) {
        SetError(e, "IAMF temporal unit is too large to buffer");
        return false;
    }
    return ReadRange(temporal_units_[i].index.byte_offset, size, b, e);
}
}  // namespace immersive_audio_muxer
