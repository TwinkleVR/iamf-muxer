#include "iamf_box_writer.h"
#include "video_mapping.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace immersive_audio_muxer;

#define CHECK(expression)                                                                                              \
    do {                                                                                                               \
        if (!(expression)) {                                                                                           \
            std::cerr << "check failed: " #expression << " at line " << __LINE__ << '\n';                       \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (false)

int main() {
    const std::vector<uint8_t> descriptor = {0x01, 0x02, 0x03};
    std::vector<uint8_t> configuration;
    std::string error;
    CHECK(IamfBoxWriter::BuildConfigurationBox(descriptor, &configuration, &error));
    CHECK(configuration.size() == 10 + descriptor.size());
    CHECK(configuration[0] == 0 && configuration[1] == 0 && configuration[2] == 0 && configuration[3] == configuration.size());
    CHECK(configuration[4] == 'i' && configuration[5] == 'a' && configuration[6] == 'c' && configuration[7] == 'b');
    CHECK(configuration[8] == 1);
    CHECK(configuration[9] == descriptor.size());
    CHECK(configuration[10] == descriptor[0]);

    VideoMappingOptions mapping_options;
    mapping_options.preset = VideoMappingPreset::kVr180SideBySide;
    VideoSpatialMetadata mapping;
    CHECK(BuildVideoSpatialMetadata(mapping_options, {}, &mapping, &error));
    CHECK(mapping.has_stereo && mapping.stereo_mode == 2);
    CHECK(mapping.has_spherical && mapping.bound_left == 0x40000000 &&
          mapping.bound_right == 0x40000000);

    const auto contains_box = [](const std::vector<uint8_t>& bytes, const char* type) {
        return std::search(bytes.begin(), bytes.end(), type, type + 4) != bytes.end();
    };
    const auto make_box = [](const char* type, const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> box;
        const uint32_t size = static_cast<uint32_t>(payload.size() + 8);
        box.push_back(static_cast<uint8_t>(size >> 24));
        box.push_back(static_cast<uint8_t>(size >> 16));
        box.push_back(static_cast<uint8_t>(size >> 8));
        box.push_back(static_cast<uint8_t>(size));
        box.insert(box.end(), type, type + 4);
        box.insert(box.end(), payload.begin(), payload.end());
        return box;
    };
    const auto make_full_box = [&make_box](const char* type, const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> fullbox_payload = {0, 0, 0, 0};
        fullbox_payload.insert(fullbox_payload.end(), payload.begin(), payload.end());
        return make_box(type, fullbox_payload);
    };
    const auto append = [](std::vector<uint8_t>* destination, const std::vector<uint8_t>& source) {
        destination->insert(destination->end(), source.begin(), source.end());
    };
    const auto trailing_fourcc = [](const std::vector<uint8_t>& bytes, const char* tag) {
        return bytes.size() >= 4 && std::equal(tag, tag + 4, bytes.end() - 4);
    };
    const std::vector<uint8_t> stereo_box = BuildStereo3DBox(mapping);
    CHECK(stereo_box.size() == 13 && stereo_box[4] == 's' && stereo_box[5] == 't' &&
          stereo_box[6] == '3' && stereo_box[7] == 'd' && stereo_box[12] == 2);
    const std::vector<uint8_t> spherical_box = BuildSphericalVideoBox(mapping);
    CHECK(contains_box(spherical_box, "sv3d"));
    CHECK(contains_box(spherical_box, "svhd"));
    CHECK(contains_box(spherical_box, "proj"));
    CHECK(contains_box(spherical_box, "equi"));

    std::vector<std::vector<uint8_t>> spatial_boxes;
    CHECK(BuildVideoSampleEntrySpatialBoxes(mapping, &spatial_boxes, &error));
    CHECK(spatial_boxes.size() == 3);
    CHECK(contains_box(spatial_boxes[2], "vexu"));
    CHECK(contains_box(spatial_boxes[2], "proj"));
    CHECK(contains_box(spatial_boxes[2], "prji"));
    CHECK(trailing_fourcc(spatial_boxes[2], "hequ"));

    mapping_options.preset = VideoMappingPreset::kVr360TopBottom;
    CHECK(BuildVideoSpatialMetadata(mapping_options, {}, &mapping, &error));
    CHECK(mapping.has_vexu_projection &&
          mapping.vexu_projection == VexuProjectionType::kEquirectangular);
    CHECK(BuildVideoSampleEntrySpatialBoxes(mapping, &spatial_boxes, &error));
    CHECK(spatial_boxes.size() == 3 && trailing_fourcc(spatial_boxes[2], "equi"));

    mapping_options.preset = VideoMappingPreset::kVr360SideBySide;
    CHECK(BuildVideoSpatialMetadata(mapping_options, {}, &mapping, &error));
    CHECK(mapping.has_stereo && mapping.stereo_mode == 2 && mapping.bound_left == 0 &&
          mapping.bound_right == 0 && mapping.vexu_projection == VexuProjectionType::kEquirectangular);
    CHECK(BuildVideoSampleEntrySpatialBoxes(mapping, &spatial_boxes, &error));
    CHECK(spatial_boxes.size() == 3 && trailing_fourcc(spatial_boxes[2], "equi"));

    mapping_options.preset = VideoMappingPreset::kVr360Mono;
    CHECK(BuildVideoSpatialMetadata(mapping_options, {}, &mapping, &error));
    CHECK(mapping.has_stereo && mapping.stereo_mode == 0 && mapping.bound_left == 0 &&
          mapping.bound_right == 0 && mapping.vexu_projection == VexuProjectionType::kEquirectangular);
    CHECK(BuildVideoSampleEntrySpatialBoxes(mapping, &spatial_boxes, &error));
    CHECK(spatial_boxes.size() == 3 && trailing_fourcc(spatial_boxes[2], "equi"));

    mapping_options.preset = VideoMappingPreset::kNone;
    CHECK(BuildVideoSpatialMetadata(mapping_options, {}, &mapping, &error));
    CHECK(!mapping.has_stereo && !mapping.has_spherical && mapping.has_vexu_projection &&
          mapping.vexu_projection == VexuProjectionType::kRectilinear);
    CHECK(BuildVideoSampleEntrySpatialBoxes(mapping, &spatial_boxes, &error));
    CHECK(spatial_boxes.size() == 1 && trailing_fourcc(spatial_boxes[0], "rect"));

    VideoSpatialMetadata source_spatial;
    source_spatial.has_stereo = true;
    source_spatial.has_spherical = true;
    source_spatial.source_mp4_metadata_state = SourceMp4SpatialMetadataState::kParsed;
    source_spatial.source_sample_entry_spatial_boxes = {
        {0, 0, 0, 13, 's', 't', '3', 'd', 0, 0, 0, 0, 1}};
    mapping_options.preset = VideoMappingPreset::kPreserveSource;
    CHECK(BuildVideoSpatialMetadata(mapping_options, source_spatial, &mapping, &error));
    CHECK(mapping.preserve_source_sample_entry_boxes);
    CHECK(BuildVideoSampleEntrySpatialBoxes(mapping, &spatial_boxes, &error));
    CHECK(spatial_boxes == source_spatial.source_sample_entry_spatial_boxes);

    source_spatial.source_sample_entry_spatial_boxes.clear();
    CHECK(BuildVideoSpatialMetadata(mapping_options, source_spatial, &mapping, &error));
    CHECK(BuildVideoSampleEntrySpatialBoxes(mapping, &spatial_boxes, &error));
    CHECK(spatial_boxes.empty());

    std::vector<uint8_t> old_prji = make_full_box("prji", {'r', 'e', 'c', 't'});
    std::vector<uint8_t> old_proj = make_box("proj", old_prji);
    std::vector<uint8_t> pkin = make_full_box("pkin", {'s', 'i', 'd', 'e'});
    std::vector<uint8_t> pack = make_box("pack", pkin);
    std::vector<uint8_t> custom = make_box("cust", {0x01, 0x02});
    std::vector<uint8_t> old_vexu_payload;
    append(&old_vexu_payload, old_proj);
    append(&old_vexu_payload, pack);
    append(&old_vexu_payload, custom);
    source_spatial.source_sample_entry_spatial_boxes = {make_box("vexu", old_vexu_payload)};
    mapping_options.preset = VideoMappingPreset::kVr180TopBottom;
    CHECK(BuildVideoSpatialMetadata(mapping_options, source_spatial, &mapping, &error));
    CHECK(BuildVideoSampleEntrySpatialBoxes(mapping, &spatial_boxes, &error));
    CHECK(spatial_boxes.size() == 3);
    CHECK(contains_box(spatial_boxes[2], "cust"));
    CHECK(contains_box(spatial_boxes[2], "over"));
    CHECK(trailing_fourcc(spatial_boxes[2], "hequ"));
    const auto count_box_type = [](const std::vector<uint8_t>& bytes, const char* type) {
        size_t count = 0;
        for (auto it = bytes.begin(); it + 4 <= bytes.end(); ++it)
            count += std::equal(type, type + 4, it) ? 1 : 0;
        return count;
    };
    CHECK(count_box_type(spatial_boxes[2], "proj") == 1);

    std::vector<uint8_t> eyes = make_box("eyes", {0, 0, 0, 0});
    old_vexu_payload.clear();
    append(&old_vexu_payload, eyes);
    append(&old_vexu_payload, pack);
    append(&old_vexu_payload, custom);
    source_spatial.source_sample_entry_spatial_boxes = {make_box("vexu", old_vexu_payload)};
    mapping_options.preset = VideoMappingPreset::kVr180Mono;
    CHECK(BuildVideoSpatialMetadata(mapping_options, source_spatial, &mapping, &error));
    CHECK(BuildVideoSampleEntrySpatialBoxes(mapping, &spatial_boxes, &error));
    CHECK(spatial_boxes.size() == 3);
    CHECK(!contains_box(spatial_boxes[2], "eyes"));
    CHECK(!contains_box(spatial_boxes[2], "pkin"));
    CHECK(contains_box(spatial_boxes[2], "cust"));

    VideoMappingPreset parsed = VideoMappingPreset::kNone;
    CHECK(ParseVideoMappingPreset("vr360-ou", &parsed));
    CHECK(parsed == VideoMappingPreset::kVr360TopBottom);
    return 0;
}

#undef CHECK
