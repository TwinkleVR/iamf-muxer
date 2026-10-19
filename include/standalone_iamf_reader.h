#pragma once

#include <cstdint>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace immersive_audio_muxer {

struct IamfIndexEntry {
    uint64_t byte_offset = 0;
    uint64_t byte_size = 0;
    int64_t start_timestamp = 0;
    uint32_t num_samples = 0;
    uint32_t trim_at_start = 0;
    uint32_t trim_at_end = 0;
};

struct IamfIndexSummary {
    uint64_t descriptor_bytes = 0;
    uint64_t temporal_unit_bytes = 0;
    uint64_t total_samples = 0;
    uint32_t sample_rate = 0;
    uint32_t frame_size = 0;
    uint32_t temporal_unit_count = 0;
    uint32_t codec_config_count = 0;
    uint32_t audio_element_count = 0;
    uint32_t mix_presentation_count = 0;
    std::string mix_presentation_annotation;
    uint8_t primary_profile = 0;
    uint8_t additional_profile = 0;
    bool has_sequence_header = false;
    bool scan_reached_eof = false;
};

struct IamfTemporalUnitSample {
    IamfIndexEntry index{};
};

class StandaloneIamfReader final {
public:
    bool Open(const std::string& path, std::string* error);
    bool ValidateObjectAudioAndParameters(std::string* error) const;
    const std::vector<uint8_t>& descriptor_bytes() const { return descriptor_bytes_; }
    const std::vector<IamfTemporalUnitSample>& temporal_units() const { return temporal_units_; }
    const IamfIndexSummary& summary() const { return summary_; }
    bool ReadTemporalUnit(size_t index, std::vector<uint8_t>* bytes, std::string* error) const;
    uint32_t codec_id() const { return codec_id_; }
    int32_t audio_roll_distance() const { return audio_roll_distance_; }

private:
    struct ObuMetadata {
        uint8_t type = 0;
        uint8_t flags = 0;
        uint64_t total_size = 0;
        uint64_t payload_offset = 0;
        uint64_t payload_size = 0;
        uint32_t trim_at_start = 0;
        uint32_t trim_at_end = 0;
        uint32_t audio_frame_id = 0;
        bool has_audio_frame_id = false;
    };
    bool ReadObuMetadata(uint64_t offset, ObuMetadata* metadata, std::string* error) const;
    bool ParseSequenceHeader(const ObuMetadata& metadata, std::string* error);
    bool ParseCodecConfig(const ObuMetadata& metadata, std::string* error);
    bool ParseMixPresentationAnnotation(const ObuMetadata& metadata, std::string* error);
    bool ParseAudioElementSubstreamIds(const ObuMetadata& metadata, std::set<uint32_t>* ids, std::string* error) const;
    bool ParseAudioFrameId(ObuMetadata* metadata, std::string* error) const;
    bool ReadRange(uint64_t offset, uint64_t size, std::vector<uint8_t>* out, std::string* error) const;
    static bool ReadUleb(const std::vector<uint8_t>& bytes, size_t* offset, uint64_t* value);
    static bool ReadString(const std::vector<uint8_t>& bytes, size_t* offset, std::string* value);
    static bool IsTemporalUnitObuType(uint8_t type);
    static bool IsAudioFrameObuType(uint8_t type);
    mutable std::ifstream file_;
    uint64_t file_size_ = 0;
    std::vector<uint8_t> descriptor_bytes_;
    std::vector<IamfTemporalUnitSample> temporal_units_;
    IamfIndexSummary summary_{};
    uint32_t codec_id_ = 0;
    int32_t audio_roll_distance_ = 0;
    bool mix_presentation_annotation_set_ = false;
};
}  // namespace immersive_audio_muxer
