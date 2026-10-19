#include "iamf_box_writer.h"

#include <limits>

namespace immersive_audio_muxer {
namespace {
void PutType(std::vector<uint8_t>* out, const char type[4]) {
    out->insert(out->end(), type, type + 4);
}

bool PutUleb128(std::vector<uint8_t>* out, uint64_t value) {
    do {
        if (out->size() == std::numeric_limits<size_t>::max())
            return false;
        uint8_t byte = static_cast<uint8_t>(value & 0x7f);
        value >>= 7;
        if (value != 0)
            byte |= 0x80;
        out->push_back(byte);
    } while (value != 0);
    return true;
}

bool FinishBox(std::vector<uint8_t>* box, std::string* error) {
    if (box->size() > std::numeric_limits<uint32_t>::max()) {
        if (error)
            *error = "IAMF MP4 box exceeds 32-bit size";
        return false;
    }
    const uint32_t size = static_cast<uint32_t>(box->size());
    (*box)[0] = static_cast<uint8_t>(size >> 24);
    (*box)[1] = static_cast<uint8_t>(size >> 16);
    (*box)[2] = static_cast<uint8_t>(size >> 8);
    (*box)[3] = static_cast<uint8_t>(size);
    return true;
}
}  // namespace

bool IamfBoxWriter::BuildConfigurationBox(const std::vector<uint8_t>& descriptor_bytes, std::vector<uint8_t>* box, std::string* error) {
    if (box == nullptr || descriptor_bytes.empty()) {
        if (error)
            *error = "IAMF configuration requires non-empty descriptor bytes";
        return false;
    }
    box->clear();
    box->resize(4);
    PutType(box, "iacb");
    box->push_back(1);  // configurationVersion
    if (!PutUleb128(box, descriptor_bytes.size())) {
        if (error)
            *error = "failed to encode IAMF configuration size";
        return false;
    }
    box->insert(box->end(), descriptor_bytes.begin(), descriptor_bytes.end());
    return FinishBox(box, error);
}

bool IamfBoxWriter::BuildSampleEntry(const std::vector<uint8_t>& configuration_box, std::vector<uint8_t>* sample_entry,
                                     std::string* error) {
    if (sample_entry == nullptr || configuration_box.size() < 8) {
        if (error)
            *error = "IAMF sample entry requires a configuration box";
        return false;
    }
    sample_entry->clear();
    sample_entry->resize(4);
    PutType(sample_entry, "iamf");
    sample_entry->insert(sample_entry->end(), 6, 0);  // reserved
    sample_entry->push_back(0);
    sample_entry->push_back(1);                       // data_reference_index
    sample_entry->insert(sample_entry->end(), 8, 0);  // version/revision/vendor
    sample_entry->push_back(0);
    sample_entry->push_back(0);  // channel count
    sample_entry->push_back(0);
    sample_entry->push_back(16);  // sample size
    sample_entry->push_back(0);
    sample_entry->push_back(0);  // compression id
    sample_entry->push_back(0);
    sample_entry->push_back(0);                       // packet size
    sample_entry->insert(sample_entry->end(), 4, 0);  // IAMF sample rate is zero
    sample_entry->insert(sample_entry->end(), configuration_box.begin(), configuration_box.end());
    return FinishBox(sample_entry, error);
}

}  // namespace immersive_audio_muxer
