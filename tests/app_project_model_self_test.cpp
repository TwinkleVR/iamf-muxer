#include "app/project_types.h"
#include "app/metadata_builder.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <tuple>
#include <utility>
#include <vector>

using namespace immersive_audio_muxer::app;

#define CHECK(expression)                                                                                              \
    do {                                                                                                               \
        if (!(expression)) {                                                                                           \
            std::cerr << "check failed: " #expression << " at line " << __LINE__ << '\n';                       \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (false)

namespace {

MediaProbeInfo MakeInput(const char* name, InputKind kind) {
    MediaProbeInfo input;
    input.path = name;
    input.kind = kind;
    if (kind == InputKind::kAudioCandidate) {
        MediaStreamInfo stream;
        stream.index = 0;
        stream.kind = MediaStreamKind::kAudio;
        stream.channel_count = std::string(name) == "stereo.wav" ? 2 : 16;
        input.streams.push_back(stream);
    }
    return input;
}

MediaProbeInfo MakeChannelInput(const std::filesystem::path& path, int channel_count,
                                std::vector<SpeakerPosition> positions = {},
                                bool positions_complete = false) {
    MediaProbeInfo input;
    input.path = path;
    input.kind = InputKind::kAudioCandidate;
    MediaStreamInfo stream;
    stream.index = 0;
    stream.kind = MediaStreamKind::kAudio;
    stream.channel_count = channel_count;
    stream.channel_positions = std::move(positions);
    stream.channel_positions_complete = positions_complete;
    input.streams.push_back(stream);
    input.channel_layout_candidates = ChannelLayoutsForChannelCount(channel_count);
    return input;
}

}  // namespace

int main() {
    std::string error;
    MuxProject mux_project;
    CHECK(mux_project.AddInput(MakeInput("video.mp4", InputKind::kVideoContainer), &error));
    CHECK(mux_project.AddInput(MakeInput("mix.iamf", InputKind::kStandaloneIamf), &error));
    CHECK(mux_project.mode == ProjectMode::kMuxExistingIamf);
    CHECK(mux_project.ValidateForExistingIamfMux(&error));
    CHECK(!mux_project.AddInput(MakeInput("audio.wav", InputKind::kAudioCandidate), &error));

    MuxProject encoding_project;
    CHECK(encoding_project.AddInput(MakeInput("ambisonics.wav", InputKind::kAudioCandidate), &error));
    CHECK(encoding_project.AddInput(MakeInput("stereo.wav", InputKind::kAudioCandidate), &error));
    CHECK(!encoding_project.ValidateForAudioEncoding(&error));
    CHECK(encoding_project.AssignAudioRole("ambisonics.wav", AudioRole::kThirdOrderAmbisonics, true, &error));
    CHECK(encoding_project.AssignAudioRole("stereo.wav", AudioRole::kHeadLockedStereo, true, &error));
    CHECK(encoding_project.ValidateForAudioEncoding(&error));
    CHECK(!encoding_project.AssignAudioRole("stereo.wav", AudioRole::kThirdOrderAmbisonics, true, &error));

    CHECK(ChannelLayoutsForChannelCount(6).size() == 2);
    CHECK(ChannelLayoutsForChannelCount(8).size() == 2);
    CHECK(ChannelLayoutsForChannelCount(10).size() == 2);
    CHECK(DetectChannelLayout(6,
                             {SpeakerPosition::kFrontLeft, SpeakerPosition::kFrontCenter,
                              SpeakerPosition::kFrontRight, SpeakerPosition::kSideLeft,
                              SpeakerPosition::kSideRight, SpeakerPosition::kLowFrequency},
                             true) == ChannelLayout::k5_1);
    CHECK(DetectChannelLayout(6,
                             {SpeakerPosition::kFrontLeft, SpeakerPosition::kFrontCenter,
                              SpeakerPosition::kFrontRight, SpeakerPosition::kTopFrontLeft,
                              SpeakerPosition::kTopFrontRight, SpeakerPosition::kLowFrequency},
                             true) == ChannelLayout::k3_1_2);
    CHECK(DetectChannelLayout(10,
                             {SpeakerPosition::kFrontLeft, SpeakerPosition::kFrontCenter,
                              SpeakerPosition::kFrontRight, SpeakerPosition::kSideLeft,
                              SpeakerPosition::kSideRight, SpeakerPosition::kBackLeft,
                              SpeakerPosition::kBackRight, SpeakerPosition::kTopMiddleLeft,
                              SpeakerPosition::kTopMiddleRight, SpeakerPosition::kLowFrequency},
                             true) == ChannelLayout::k7_1_2);

    const std::vector<std::tuple<ChannelLayout, const char*, std::vector<int>>> channel_orders = {
        {ChannelLayout::kMono, "C", {0}},
        {ChannelLayout::kStereo, "L / R", {0, 1}},
        {ChannelLayout::k3_1_2, "L / R / C / LFE / Ltf / Rtf", {0, 1, 2, 3, 4, 5}},
        {ChannelLayout::k5_1, "L / R / C / LFE / Ls / Rs", {0, 1, 2, 3, 4, 5}},
        {ChannelLayout::k5_1_2, "L / R / C / LFE / Ls / Rs / Ltf / Rtf",
         {0, 1, 2, 3, 4, 5, 6, 7}},
        {ChannelLayout::k5_1_4, "L / R / C / LFE / Ls / Rs / Ltf / Rtf / Ltr / Rtr",
         {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}},
        {ChannelLayout::k7_1, "L / R / C / LFE / Lrs / Rrs / Lss / Rss",
         {0, 1, 2, 3, 6, 7, 4, 5}},
        {ChannelLayout::k7_1_2, "L / R / C / LFE / Lrs / Rrs / Lss / Rss / Ltf / Rtf",
         {0, 1, 2, 3, 6, 7, 4, 5, 8, 9}},
        {ChannelLayout::k7_1_4,
         "L / R / C / LFE / Lrs / Rrs / Lss / Rss / Ltf / Rtf / Ltb / Rtb",
         {0, 1, 2, 3, 6, 7, 4, 5, 8, 9, 10, 11}}};
    std::vector<int> channel_order;
    for (const auto& [layout, input_order, expected_mapping] : channel_orders) {
        CHECK(BuildChannelOrderForIamf(layout, &channel_order, &error));
        CHECK(channel_order == expected_mapping);
        CHECK(InputChannelOrderDescription(layout) == input_order);
    }

    const std::vector<SpeakerPosition> five_one_source_order = {
        SpeakerPosition::kFrontLeft, SpeakerPosition::kFrontRight,
        SpeakerPosition::kFrontCenter, SpeakerPosition::kLowFrequency,
        SpeakerPosition::kSideLeft, SpeakerPosition::kSideRight};
    MediaProbeInfo detected_five_one = MakeChannelInput(
        "detected_5_1.wav", 6, five_one_source_order, true);
    detected_five_one.suggested_channel_layout = ChannelLayout::k5_1;
    detected_five_one.channel_layout_detection_exact = true;
    MuxProject override_project;
    CHECK(override_project.AddInput(detected_five_one, &error));
    CHECK(override_project.AssignChannelLayout("detected_5_1.wav", ChannelLayout::k3_1_2,
                                               true, &error));
    CHECK(override_project.ValidateForChannelBasedEncoding(&error));
    CHECK(BuildChannelOrderForIamf(override_project.audio_inputs.front().channel_layout,
                                  &channel_order, &error));
    CHECK(channel_order == std::vector<int>({0, 1, 2, 3, 4, 5}));

    MediaProbeInfo detected_seven_one = MakeChannelInput(
        "detected_7_1.wav", 8,
        {SpeakerPosition::kFrontLeft, SpeakerPosition::kFrontRight,
         SpeakerPosition::kFrontCenter, SpeakerPosition::kLowFrequency,
         SpeakerPosition::kBackLeft, SpeakerPosition::kBackRight,
         SpeakerPosition::kSideLeft, SpeakerPosition::kSideRight}, true);
    detected_seven_one.suggested_channel_layout = ChannelLayout::k7_1;
    detected_seven_one.channel_layout_detection_exact = true;
    MuxProject relabeled_project;
    CHECK(relabeled_project.AddInput(detected_seven_one, &error));
    CHECK(relabeled_project.AssignChannelLayout("detected_7_1.wav", ChannelLayout::k5_1_2,
                                                true, &error));
    CHECK(relabeled_project.ValidateForChannelBasedEncoding(&error));

    MuxProject channel_project;
    const std::filesystem::path channel_path = "unknown_10ch.wav";
    CHECK(channel_project.AddInput(MakeChannelInput(channel_path, 10), &error));
    CHECK(channel_project.AssignChannelLayout(channel_path, ChannelLayout::k7_1_2,
                                              false, &error));
    CHECK(!channel_project.ValidateForChannelBasedEncoding(&error));
    CHECK(channel_project.AssignChannelLayout(channel_path, ChannelLayout::k7_1_2,
                                              true, &error));
    CHECK(channel_project.ValidateForChannelBasedEncoding(&error));

    const auto unique_id = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path metadata_directory =
        std::filesystem::current_path() / ("iamf_channel_metadata_test_" + std::to_string(unique_id));
    std::filesystem::create_directories(metadata_directory);
    const std::vector<std::tuple<ChannelLayout, int, int, std::vector<std::string>,
                                 std::vector<std::string>>>
        supported_layouts = {
            {ChannelLayout::kMono, 1, 0, {"CHANNEL_LABEL_MONO"},
             {"SOUND_SYSTEM_A_0_2_0", "SOUND_SYSTEM_12_0_1_0"}},
            {ChannelLayout::kStereo, 1, 1, {"CHANNEL_LABEL_L_2", "CHANNEL_LABEL_R_2"},
             {"SOUND_SYSTEM_A_0_2_0"}},
            {ChannelLayout::k3_1_2, 4, 2,
             {"CHANNEL_LABEL_L_3", "CHANNEL_LABEL_R_3", "CHANNEL_LABEL_CENTRE",
              "CHANNEL_LABEL_LFE", "CHANNEL_LABEL_LTF_3", "CHANNEL_LABEL_RTF_3"},
             {"SOUND_SYSTEM_A_0_2_0", "SOUND_SYSTEM_11_2_3_0"}},
            {ChannelLayout::k5_1, 4, 2,
             {"CHANNEL_LABEL_L_5", "CHANNEL_LABEL_R_5", "CHANNEL_LABEL_CENTRE",
              "CHANNEL_LABEL_LFE", "CHANNEL_LABEL_LS_5", "CHANNEL_LABEL_RS_5"},
             {"SOUND_SYSTEM_A_0_2_0", "SOUND_SYSTEM_B_0_5_0"}},
            {ChannelLayout::k5_1_2, 5, 3,
             {"CHANNEL_LABEL_L_5", "CHANNEL_LABEL_R_5", "CHANNEL_LABEL_CENTRE",
              "CHANNEL_LABEL_LFE", "CHANNEL_LABEL_LS_5", "CHANNEL_LABEL_RS_5",
              "CHANNEL_LABEL_LTF_2", "CHANNEL_LABEL_RTF_2"},
             {"SOUND_SYSTEM_A_0_2_0", "SOUND_SYSTEM_C_2_5_0"}},
            {ChannelLayout::k5_1_4, 6, 4,
             {"CHANNEL_LABEL_L_5", "CHANNEL_LABEL_R_5", "CHANNEL_LABEL_CENTRE",
              "CHANNEL_LABEL_LFE", "CHANNEL_LABEL_LS_5", "CHANNEL_LABEL_RS_5",
              "CHANNEL_LABEL_LTF_4", "CHANNEL_LABEL_RTF_4", "CHANNEL_LABEL_LTB_4",
              "CHANNEL_LABEL_RTB_4"},
             {"SOUND_SYSTEM_A_0_2_0", "SOUND_SYSTEM_D_4_5_0"}},
            {ChannelLayout::k7_1, 5, 3,
             {"CHANNEL_LABEL_L_7", "CHANNEL_LABEL_R_7", "CHANNEL_LABEL_CENTRE",
              "CHANNEL_LABEL_LFE", "CHANNEL_LABEL_LSS_7", "CHANNEL_LABEL_RSS_7",
              "CHANNEL_LABEL_LRS_7", "CHANNEL_LABEL_RRS_7"},
             {"SOUND_SYSTEM_A_0_2_0", "SOUND_SYSTEM_I_0_7_0"}},
            {ChannelLayout::k7_1_2, 6, 4,
             {"CHANNEL_LABEL_L_7", "CHANNEL_LABEL_R_7", "CHANNEL_LABEL_CENTRE",
              "CHANNEL_LABEL_LFE", "CHANNEL_LABEL_LSS_7", "CHANNEL_LABEL_RSS_7",
              "CHANNEL_LABEL_LRS_7", "CHANNEL_LABEL_RRS_7", "CHANNEL_LABEL_LTF_2",
              "CHANNEL_LABEL_RTF_2"},
             {"SOUND_SYSTEM_A_0_2_0", "SOUND_SYSTEM_10_2_7_0"}},
            {ChannelLayout::k7_1_4, 7, 5,
             {"CHANNEL_LABEL_L_7", "CHANNEL_LABEL_R_7", "CHANNEL_LABEL_CENTRE",
              "CHANNEL_LABEL_LFE", "CHANNEL_LABEL_LSS_7", "CHANNEL_LABEL_RSS_7",
              "CHANNEL_LABEL_LRS_7", "CHANNEL_LABEL_RRS_7", "CHANNEL_LABEL_LTF_4",
              "CHANNEL_LABEL_RTF_4", "CHANNEL_LABEL_LTB_4", "CHANNEL_LABEL_RTB_4"},
             {"SOUND_SYSTEM_A_0_2_0", "SOUND_SYSTEM_J_4_7_0"}}};
    for (const auto& [layout, substream_count, coupled_substream_count, expected_labels,
                      expected_sound_systems] : supported_layouts) {
        MetadataBuildOptions metadata;
        metadata.channel_based.media.path = metadata_directory / "prepared.wav";
        metadata.channel_based.channel_layout = layout;
        metadata.channel_based.channel_layout_confirmed = true;
        MediaStreamInfo stream;
        stream.kind = MediaStreamKind::kAudio;
        stream.channel_count = ChannelCount(layout);
        metadata.channel_based.media.streams.push_back(stream);
        metadata.output_path = metadata_directory / (std::string(ToString(layout)) + ".textproto");
        if (!MetadataBuilder::WriteChannelBased(metadata, &error)) {
            std::cerr << "channel layout metadata failed for " << ToString(layout) << ": " << error << '\n';
            return 1;
        }
        std::ifstream metadata_file(metadata.output_path, std::ios::binary);
        const std::string serialized((std::istreambuf_iterator<char>(metadata_file)),
                                     std::istreambuf_iterator<char>());
        CHECK(serialized.find("primary_profile: PROFILE_VERSION_SIMPLE") != std::string::npos);
        CHECK(serialized.find("additional_profile: PROFILE_VERSION_SIMPLE") != std::string::npos);
        CHECK(serialized.find("PROFILE_VERSION_BASE") == std::string::npos);
        CHECK(serialized.find(std::string("loudspeaker_layout: ") + IamfLayoutName(layout)) !=
              std::string::npos);
        CHECK(serialized.find("audio_element_type: AUDIO_ELEMENT_CHANNEL_BASED") != std::string::npos);
        CHECK(serialized.find("HEAD-LOCKED") == std::string::npos);
        CHECK(serialized.find("audio_element_metadata {") == serialized.rfind("audio_element_metadata {"));
        CHECK(serialized.find("      substream_count: " + std::to_string(substream_count)) !=
              std::string::npos);
        CHECK(serialized.find("      coupled_substream_count: " +
                              std::to_string(coupled_substream_count)) != std::string::npos);
        std::vector<std::string> actual_labels;
        std::size_t label_position = 0;
        const std::string label_marker = "channel_label: ";
        while ((label_position = serialized.find(label_marker, label_position)) != std::string::npos) {
            const std::size_t label_start = label_position + label_marker.size();
            const std::size_t label_end = serialized.find_first_of(" }", label_start);
            CHECK(label_end != std::string::npos);
            actual_labels.push_back(serialized.substr(label_start, label_end - label_start));
            label_position = label_end;
        }
        CHECK(actual_labels == expected_labels);

        std::vector<std::string> actual_sound_systems;
        std::size_t sound_system_position = 0;
        const std::string sound_system_marker = "sound_system: ";
        while ((sound_system_position =
                    serialized.find(sound_system_marker, sound_system_position)) != std::string::npos) {
            const std::size_t sound_system_start =
                sound_system_position + sound_system_marker.size();
            const std::size_t sound_system_end =
                serialized.find_first_of(" \r\n}", sound_system_start);
            CHECK(sound_system_end != std::string::npos);
            actual_sound_systems.push_back(
                serialized.substr(sound_system_start, sound_system_end - sound_system_start));
            sound_system_position = sound_system_end;
        }
        CHECK(actual_sound_systems == expected_sound_systems);
        CHECK(serialized.find("info_type_bit_masks: []") != std::string::npos);
        CHECK(serialized.find("integrated_loudness:") == std::string::npos);
        CHECK(serialized.find("digital_peak:") == std::string::npos);
    }
    std::filesystem::remove_all(metadata_directory);
    return 0;
}

#undef CHECK
