#include "app/metadata_builder.h"

#include "atomic_output_transaction.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

namespace immersive_audio_muxer::app {
namespace {

void SetError(std::string* error, const std::string& message) {
    if (error)
        *error = message;
}

std::string ProtoString(const std::string& value) {
    std::ostringstream escaped;
    escaped << '"';
    for (const unsigned char character : value) {
        switch (character) {
            case '\\':
                escaped << "\\\\";
                break;
            case '"':
                escaped << "\\\"";
                break;
            case '\n':
                escaped << "\\n";
                break;
            case '\r':
                escaped << "\\r";
                break;
            case '\t':
                escaped << "\\t";
                break;
            default:
                if (character < 0x20) {
                    escaped << "\\x" << std::hex << std::setw(2) << std::setfill('0')
                            << static_cast<unsigned int>(character) << std::dec << std::setfill(' ');
                } else {
                    escaped << character;
                }
                break;
        }
    }
    escaped << '"';
    return escaped.str();
}

bool IsSupportedAmbisonics(AudioRole role, int channel_count, int* order) {
    if (order == nullptr)
        return false;
    switch (role) {
        case AudioRole::kFirstOrderAmbisonics:
            *order = 1;
            return channel_count == 4;
        case AudioRole::kSecondOrderAmbisonics:
            *order = 2;
            return channel_count == 9;
        case AudioRole::kThirdOrderAmbisonics:
            *order = 3;
            return channel_count == 16;
        default:
            return false;
    }
}

void AppendChannelLabels(std::ostringstream* output, int channel_count, uint32_t audio_element_id,
                         const std::string& wav_filename, bool ambisonics) {
    *output << "audio_frame_metadata {\n"
            << "  wav_filename: " << ProtoString(wav_filename) << "\n"
            << "  samples_to_trim_at_end_includes_padding: false\n"
            << "  samples_to_trim_at_start_includes_codec_delay: false\n"
            << "  samples_to_trim_at_end: 0\n"
            << "  samples_to_trim_at_start: 0\n"
            << "  audio_element_id: " << audio_element_id << "\n"
            << "  channel_metadatas: [\n";
    for (int channel = 0; channel < channel_count; ++channel) {
        if (ambisonics) {
            *output << "    { channel_id: " << channel << " channel_label: CHANNEL_LABEL_A_" << channel
                     << " }";
        } else {
            *output << "    { channel_id: " << channel << " channel_label: CHANNEL_LABEL_"
                    << (channel == 0 ? "L_2" : "R_2") << " }";
        }
        if (channel + 1 < channel_count)
            *output << ",";
        *output << "\n";
    }
    *output << "  ]\n}\n\n";
}

std::vector<const char*> ChannelBasedLabels(ChannelLayout layout) {
    switch (layout) {
        case ChannelLayout::kMono:
            return {"CHANNEL_LABEL_MONO"};
        case ChannelLayout::kStereo:
            return {"CHANNEL_LABEL_L_2", "CHANNEL_LABEL_R_2"};
        case ChannelLayout::k3_1_2:
            return {"CHANNEL_LABEL_L_3", "CHANNEL_LABEL_R_3", "CHANNEL_LABEL_CENTRE",
                    "CHANNEL_LABEL_LFE", "CHANNEL_LABEL_LTF_3", "CHANNEL_LABEL_RTF_3"};
        case ChannelLayout::k5_1:
            return {"CHANNEL_LABEL_L_5", "CHANNEL_LABEL_R_5", "CHANNEL_LABEL_CENTRE",
                    "CHANNEL_LABEL_LFE", "CHANNEL_LABEL_LS_5", "CHANNEL_LABEL_RS_5"};
        case ChannelLayout::k5_1_2:
            return {"CHANNEL_LABEL_L_5", "CHANNEL_LABEL_R_5", "CHANNEL_LABEL_CENTRE",
                    "CHANNEL_LABEL_LFE", "CHANNEL_LABEL_LS_5", "CHANNEL_LABEL_RS_5",
                    "CHANNEL_LABEL_LTF_2", "CHANNEL_LABEL_RTF_2"};
        case ChannelLayout::k5_1_4:
            return {"CHANNEL_LABEL_L_5", "CHANNEL_LABEL_R_5", "CHANNEL_LABEL_CENTRE",
                    "CHANNEL_LABEL_LFE", "CHANNEL_LABEL_LS_5", "CHANNEL_LABEL_RS_5",
                    "CHANNEL_LABEL_LTF_4", "CHANNEL_LABEL_RTF_4", "CHANNEL_LABEL_LTB_4",
                    "CHANNEL_LABEL_RTB_4"};
        case ChannelLayout::k7_1:
            return {"CHANNEL_LABEL_L_7", "CHANNEL_LABEL_R_7", "CHANNEL_LABEL_CENTRE",
                    "CHANNEL_LABEL_LFE", "CHANNEL_LABEL_LSS_7", "CHANNEL_LABEL_RSS_7",
                    "CHANNEL_LABEL_LRS_7", "CHANNEL_LABEL_RRS_7"};
        case ChannelLayout::k7_1_2:
            return {"CHANNEL_LABEL_L_7", "CHANNEL_LABEL_R_7", "CHANNEL_LABEL_CENTRE",
                    "CHANNEL_LABEL_LFE", "CHANNEL_LABEL_LSS_7", "CHANNEL_LABEL_RSS_7",
                    "CHANNEL_LABEL_LRS_7", "CHANNEL_LABEL_RRS_7", "CHANNEL_LABEL_LTF_2",
                    "CHANNEL_LABEL_RTF_2"};
        case ChannelLayout::k7_1_4:
            return {"CHANNEL_LABEL_L_7", "CHANNEL_LABEL_R_7", "CHANNEL_LABEL_CENTRE",
                    "CHANNEL_LABEL_LFE", "CHANNEL_LABEL_LSS_7", "CHANNEL_LABEL_RSS_7",
                    "CHANNEL_LABEL_LRS_7", "CHANNEL_LABEL_RRS_7", "CHANNEL_LABEL_LTF_4",
                    "CHANNEL_LABEL_RTF_4", "CHANNEL_LABEL_LTB_4", "CHANNEL_LABEL_RTB_4"};
        case ChannelLayout::kUnset:
            return {};
    }
    return {};
}

int ChannelBasedSubstreamCount(ChannelLayout layout) {
    switch (layout) {
        case ChannelLayout::kMono:
        case ChannelLayout::kStereo:
            return 1;
        case ChannelLayout::k3_1_2:
        case ChannelLayout::k5_1:
            return 4;
        case ChannelLayout::k5_1_2:
            return 5;
        case ChannelLayout::k5_1_4:
        case ChannelLayout::k7_1_2:
            return 6;
        case ChannelLayout::k7_1:
            return 5;
        case ChannelLayout::k7_1_4:
            return 7;
        case ChannelLayout::kUnset:
            return 0;
    }
    return 0;
}

int ChannelBasedCoupledSubstreamCount(ChannelLayout layout) {
    switch (layout) {
        case ChannelLayout::kMono:
            return 0;
        case ChannelLayout::kStereo:
            return 1;
        case ChannelLayout::k3_1_2:
        case ChannelLayout::k5_1:
            return 2;
        case ChannelLayout::k5_1_2:
            return 3;
        case ChannelLayout::k5_1_4:
        case ChannelLayout::k7_1_2:
            return 4;
        case ChannelLayout::k7_1:
            return 3;
        case ChannelLayout::k7_1_4:
            return 5;
        case ChannelLayout::kUnset:
            return 0;
    }
    return 0;
}

void AppendChannelBasedFrameLabels(std::ostringstream* output,
                                   const std::vector<const char*>& labels,
                                   uint32_t audio_element_id,
                                   const std::string& wav_filename) {
    *output << "audio_frame_metadata {\n"
            << "  wav_filename: " << ProtoString(wav_filename) << "\n"
            << "  samples_to_trim_at_end_includes_padding: false\n"
            << "  samples_to_trim_at_start_includes_codec_delay: false\n"
            << "  samples_to_trim_at_end: 0\n"
            << "  samples_to_trim_at_start: 0\n"
            << "  audio_element_id: " << audio_element_id << "\n"
            << "  channel_metadatas: [\n";
    for (std::size_t channel = 0; channel < labels.size(); ++channel) {
        *output << "    { channel_id: " << channel << " channel_label: " << labels[channel] << " }";
        if (channel + 1 < labels.size())
            *output << ",";
        *output << "\n";
    }
    *output << "  ]\n}\n\n";
}

void AppendGain(std::ostringstream* output, int parameter_id, int parameter_rate) {
    *output << "        param_definition {\n"
            << "          parameter_id: " << parameter_id << "\n"
            << "          parameter_rate: " << parameter_rate << "\n"
            << "          param_definition_mode: 1\n"
            << "          reserved: 0\n"
            << "        }\n"
            << "        default_mix_gain_db { q7_dot8: 0 }\n";
}

const char* AdditionalChannelBasedSoundSystem(ChannelLayout layout) {
    switch (layout) {
        case ChannelLayout::kMono:
            return "SOUND_SYSTEM_12_0_1_0";
        case ChannelLayout::kStereo:
            return nullptr;
        case ChannelLayout::k3_1_2:
            return "SOUND_SYSTEM_11_2_3_0";
        case ChannelLayout::k5_1:
            return "SOUND_SYSTEM_B_0_5_0";
        case ChannelLayout::k5_1_2:
            return "SOUND_SYSTEM_C_2_5_0";
        case ChannelLayout::k5_1_4:
            return "SOUND_SYSTEM_D_4_5_0";
        case ChannelLayout::k7_1:
            return "SOUND_SYSTEM_I_0_7_0";
        case ChannelLayout::k7_1_2:
            return "SOUND_SYSTEM_10_2_7_0";
        case ChannelLayout::k7_1_4:
            return "SOUND_SYSTEM_J_4_7_0";
        case ChannelLayout::kUnset:
            return nullptr;
    }
    return nullptr;
}

void AppendLoudnessLayout(std::ostringstream* output, const char* sound_system) {
    *output << "    layouts {\n"
            << "      loudness_layout {\n"
            << "        layout_type: LAYOUT_TYPE_LOUDSPEAKERS_SS_CONVENTION\n"
            << "        ss_layout {\n"
            << "          sound_system: " << sound_system << "\n"
            << "        }\n"
            << "      }\n"
            << "      loudness {\n"
            << "        info_type_bit_masks: []\n"
            << "      }\n"
            << "    }\n";
}

void AppendSubMix(std::ostringstream* output, uint32_t audio_element_id, const char* annotation,
                  const char* rendering_mode, int gain_parameter_id, int parameter_rate) {
    *output << "    audio_elements {\n"
            << "      audio_element_id: " << audio_element_id << "\n"
            << "      localized_element_annotations: [" << ProtoString(annotation) << "]\n"
            << "      rendering_config {\n"
            << "        headphones_rendering_mode: " << rendering_mode << "\n"
            << "      }\n"
            << "      element_mix_gain {\n";
    AppendGain(output, gain_parameter_id, parameter_rate);
    *output << "      }\n"
            << "    }\n";
}

const char* AacSampleFrequencyIndexName(int sample_rate) {
    switch (sample_rate) {
        case 96000:
            return "AAC_SAMPLE_FREQUENCY_INDEX_96000";
        case 88200:
            return "AAC_SAMPLE_FREQUENCY_INDEX_88200";
        case 64000:
            return "AAC_SAMPLE_FREQUENCY_INDEX_64000";
        case 48000:
            return "AAC_SAMPLE_FREQUENCY_INDEX_48000";
        case 44100:
            return "AAC_SAMPLE_FREQUENCY_INDEX_44100";
        case 32000:
            return "AAC_SAMPLE_FREQUENCY_INDEX_32000";
        case 24000:
            return "AAC_SAMPLE_FREQUENCY_INDEX_24000";
        case 22050:
            return "AAC_SAMPLE_FREQUENCY_INDEX_22050";
        case 16000:
            return "AAC_SAMPLE_FREQUENCY_INDEX_16000";
        case 12000:
            return "AAC_SAMPLE_FREQUENCY_INDEX_12000";
        case 11025:
            return "AAC_SAMPLE_FREQUENCY_INDEX_11025";
        case 8000:
            return "AAC_SAMPLE_FREQUENCY_INDEX_8000";
        case 7350:
            return "AAC_SAMPLE_FREQUENCY_INDEX_7350";
        default:
            return nullptr;
    }
}

}  // namespace

bool MetadataBuilder::WriteAmbisonicsAndHeadLockedStereo(const MetadataBuildOptions& options,
                                                             std::string* error) {
    if (options.output_path.empty() || options.ambisonics.media.path.empty()) {
        SetError(error, "metadata requires one Ambisonics input and an output path");
        return false;
    }
    const bool has_head_locked_stereo = !options.head_locked_stereo.media.path.empty();
    if (!options.ambisonics.role_confirmed ||
        (has_head_locked_stereo && !options.head_locked_stereo.role_confirmed)) {
        SetError(error, "metadata requires assigned audio roles");
        return false;
    }
    const auto channel_count_for = [](const ProjectAudioInput& input) {
        const auto stream = std::find_if(input.media.streams.begin(), input.media.streams.end(),
                                         [](const MediaStreamInfo& value) {
                                             return value.kind == MediaStreamKind::kAudio;
                                         });
        return stream == input.media.streams.end() ? 0 : stream->channel_count;
    };
    const int ambisonics_input_channels = channel_count_for(options.ambisonics);
    int ambisonics_order = 0;
    if (!IsSupportedAmbisonics(options.ambisonics.role, ambisonics_input_channels, &ambisonics_order)) {
        SetError(error, "ambisonics role and channel count do not match");
        return false;
    }
    const int ambisonics_channels = (ambisonics_order + 1) * (ambisonics_order + 1);
    const int stereo_channels = has_head_locked_stereo ? channel_count_for(options.head_locked_stereo) : 0;
    if (has_head_locked_stereo && stereo_channels != 2) {
        SetError(error, "head-locked stereo input must contain exactly two channels");
        return false;
    }
    if (options.sample_rate <= 0 || options.samples_per_frame <= 0 ||
        (options.generated_codec == GeneratedCodec::kOpus && options.opus_bitrate_per_channel <= 0)) {
        SetError(error, "metadata requires valid sample rate, frame size, and codec settings");
        return false;
    }

    std::error_code filesystem_error;
    const auto parent = options.output_path.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent, filesystem_error)) {
        SetError(error, "metadata output directory does not exist: " + parent.u8string());
        return false;
    }
    const std::string ambisonics_wav = options.ambisonics.media.path.filename().u8string();
    const std::string stereo_wav = has_head_locked_stereo
                                        ? options.head_locked_stereo.media.path.filename().u8string()
                                        : std::string();
    if (ambisonics_wav.empty() || (has_head_locked_stereo && stereo_wav.empty())) {
        SetError(error, "audio input filename cannot be empty");
        return false;
    }

    const bool use_lpcm = options.generated_codec == GeneratedCodec::kLpcm;
    const bool use_aac_lc = options.generated_codec == GeneratedCodec::kAacLc;
    const bool use_flac = options.generated_codec == GeneratedCodec::kFlac;
    const char* aac_sample_frequency_index = use_aac_lc
                                                 ? AacSampleFrequencyIndexName(options.sample_rate)
                                                 : nullptr;
    if (use_aac_lc && aac_sample_frequency_index == nullptr) {
        SetError(error, "AAC-LC requires a supported IAMF sample rate");
        return false;
    }
    int codec_samples_per_frame = options.samples_per_frame;
    if (use_aac_lc) {
        codec_samples_per_frame = 1024;
    }

    std::ostringstream text;
    text << "# proto-file: iamf/cli/proto/user_metadata.proto\n"
         << "# proto-message: UserMetadata\n\n"
         << "test_vector_metadata {\n"
         << "  human_readable_description: \"Generated Ambisonics and Head-Locked Stereo IAMF\"\n"
         << "  file_name_prefix: \"generated_iamf\"\n"
         << "  is_valid: true\n"
         << "}\n\n"
         << "ia_sequence_header_metadata {\n"
         << "  primary_profile: PROFILE_VERSION_BASE\n"
         << "  additional_profile: PROFILE_VERSION_BASE\n"
         << "}\n\n"
         << "codec_config_metadata {\n"
         << "  codec_config_id: 200\n"
         << "  codec_config {\n"
         << "    num_samples_per_frame: " << codec_samples_per_frame << "\n";
    if (use_lpcm) {
        text << "    decoder_config_lpcm {\n"
             << "      sample_format_flags: LPCM_LITTLE_ENDIAN\n"
             << "      sample_size: 16\n"
             << "      sample_rate: " << options.sample_rate << "\n"
             << "    }\n";
    } else if (use_aac_lc) {
        text << "    decoder_config_aac {\n"
             << "      buffer_size_db: 0\n"
             << "      max_bitrate: 0\n"
             << "      average_bit_rate: 0\n"
             << "      decoder_specific_info {\n"
             << "        sample_frequency_index: " << aac_sample_frequency_index << "\n"
             << "      }\n"
             << "      aac_encoder_metadata {\n"
             << "        bitrate_mode: 0\n"
             << "        enable_afterburner: true\n"
             << "        signaling_mode: 2\n"
             << "      }\n"
             << "    }\n";
    } else if (use_flac) {
        text << "    decoder_config_flac {\n"
             << "      metadata_blocks {\n"
             << "        header { block_type: FLAC_BLOCK_TYPE_STREAMINFO }\n"
             << "        stream_info {\n"
             << "          sample_rate: " << options.sample_rate << "\n"
             << "          bits_per_sample: 15\n"
             << "          total_samples_in_stream: 0\n"
             << "        }\n"
             << "      }\n"
             << "      flac_encoder_metadata { compression_level: 5 }\n"
             << "    }\n";
    } else {
        text << "    decoder_config_opus {\n"
             << "      version: 1\n"
             << "      input_sample_rate: " << options.sample_rate << "\n"
             << "      opus_encoder_metadata {\n"
             << "        target_bitrate_per_channel: " << options.opus_bitrate_per_channel << "\n"
             << "        application: APPLICATION_AUDIO\n"
             << "        use_float_api: false\n"
             << "      }\n"
             << "    }\n";
    }
    text << "  }\n"
         << "}\n\n";
    AppendChannelLabels(&text, ambisonics_channels, 300, ambisonics_wav, true);
    if (has_head_locked_stereo)
        AppendChannelLabels(&text, stereo_channels, 301, stereo_wav, false);

    text << "audio_element_metadata {\n"
         << "  audio_element_id: 300\n"
         << "  audio_element_type: AUDIO_ELEMENT_SCENE_BASED\n"
         << "  reserved: 0\n"
         << "  codec_config_id: 200\n"
         << "  audio_substream_ids: [";
    for (int channel = 0; channel < ambisonics_channels; ++channel)
        text << (channel == 0 ? "" : ", ") << channel;
    text << "]\n  ambisonics_config {\n"
         << "    ambisonics_mono_config {\n"
         << "      output_channel_count: " << ambisonics_channels << "\n"
         << "      substream_count: " << ambisonics_channels << "\n"
         << "      channel_mapping: [";
    for (int channel = 0; channel < ambisonics_channels; ++channel)
        text << (channel == 0 ? "" : ", ") << channel;
    text << "]\n    }\n  }\n}\n\n";
    if (has_head_locked_stereo) {
        text << "audio_element_metadata {\n"
             << "  audio_element_id: 301\n"
             << "  audio_element_type: AUDIO_ELEMENT_CHANNEL_BASED\n"
             << "  reserved: 0\n"
             << "  codec_config_id: 200\n"
             << "  audio_substream_ids: [" << ambisonics_channels << "]\n"
             << "  scalable_channel_layout_config {\n"
             << "    reserved: 0\n"
             << "    channel_audio_layer_configs: [{\n"
             << "      loudspeaker_layout: LOUDSPEAKER_LAYOUT_STEREO\n"
             << "      output_gain_is_present_flag: 0\n"
             << "      recon_gain_is_present_flag: 0\n"
             << "      reserved_a: 0\n"
             << "    }]\n"
             << "  }\n"
             << "}\n\n";
    }
    text << "mix_presentation_metadata {\n"
         << "  mix_presentation_id: " << options.mix_id << "\n"
         << "  annotations_language: [\"en-us\"]\n"
         << "  localized_presentation_annotations: ["
         << ProtoString(options.mix_annotation.empty() ? "Generated Mix Presentation" : options.mix_annotation)
         << "]\n"
         << "  sub_mixes {\n";
    AppendSubMix(&text, 300, "Ambisonics", "HEADPHONES_RENDERING_MODE_BINAURAL_WORLD_LOCKED",
                 998, options.sample_rate);
    if (has_head_locked_stereo)
        // The official base/enhanced profiles reject BINAURAL_HEAD_LOCKED.
        // Stereo is the required direct, non-OBR path for this two-channel input.
        AppendSubMix(&text, 301, "Head-Locked Stereo",
                     "HEADPHONES_RENDERING_MODE_STEREO", 999, options.sample_rate);
    text << "    output_mix_gain {\n";
    AppendGain(&text, 997, options.sample_rate);
    text << "    }\n"
         << "    layouts {\n"
         << "      loudness_layout {\n"
         << "        layout_type: LAYOUT_TYPE_LOUDSPEAKERS_SS_CONVENTION\n"
         << "        ss_layout { sound_system: SOUND_SYSTEM_A_0_2_0 reserved: 0 }\n"
         << "      }\n"
         << "      loudness { info_type_bit_masks: [] }\n"
         << "    }\n"
         << "  }\n"
         << "}\n\n"
         << "temporal_delimiter_metadata { enable_temporal_delimiters: false }\n";

    immersive_audio_muxer::AtomicOutputTransaction transaction;
    std::string transaction_error;
    if (!immersive_audio_muxer::AtomicOutputTransaction::Create(options.output_path, options.overwrite_existing_files,
                                                         &transaction, &transaction_error)) {
        SetError(error, "cannot stage metadata textproto: " + transaction_error);
        return false;
    }
    std::ofstream output(transaction.temporary_path(), std::ios::binary | std::ios::trunc);
    if (!output) {
        SetError(error, "cannot create metadata textproto: " + options.output_path.u8string());
        return false;
    }
    const std::string serialized = text.str();
    output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
    if (!output.good()) {
        SetError(error, "failed to write metadata textproto");
        return false;
    }
    output.close();
    if (!transaction.Commit(&transaction_error)) {
        transaction.Abort();
        SetError(error, "cannot commit metadata textproto: " + transaction_error);
        return false;
    }
    return true;
}

bool MetadataBuilder::WriteChannelBased(const MetadataBuildOptions& options, std::string* error) {
    const ProjectAudioInput& input = options.channel_based;
    if (options.output_path.empty() || input.media.path.empty() ||
        input.channel_layout == ChannelLayout::kUnset || !input.channel_layout_confirmed) {
        SetError(error, "metadata requires a confirmed Channel-based input and an output path");
        return false;
    }
    const auto audio_stream = std::find_if(input.media.streams.begin(), input.media.streams.end(),
                                           [](const MediaStreamInfo& stream) {
                                               return stream.kind == MediaStreamKind::kAudio;
                                           });
    const int channels = audio_stream == input.media.streams.end() ? 0 : audio_stream->channel_count;
    if (channels != ChannelCount(input.channel_layout)) {
        SetError(error, "Channel-based layout and source channel count do not match");
        return false;
    }
    const std::vector<const char*> labels = ChannelBasedLabels(input.channel_layout);
    const int substream_count = ChannelBasedSubstreamCount(input.channel_layout);
    const int coupled_substream_count = ChannelBasedCoupledSubstreamCount(input.channel_layout);
    if (static_cast<int>(labels.size()) != channels || substream_count <= 0 ||
        IamfLayoutName(input.channel_layout)[0] == '\0') {
        SetError(error, "the selected Channel-based layout has no IAMF metadata mapping");
        return false;
    }
    if (options.sample_rate <= 0 || options.samples_per_frame <= 0 ||
        (options.generated_codec == GeneratedCodec::kOpus && options.opus_bitrate_per_channel <= 0)) {
        SetError(error, "metadata requires valid sample rate, frame size, and codec settings");
        return false;
    }

    const bool use_lpcm = options.generated_codec == GeneratedCodec::kLpcm;
    const bool use_aac_lc = options.generated_codec == GeneratedCodec::kAacLc;
    const bool use_flac = options.generated_codec == GeneratedCodec::kFlac;
    const char* aac_sample_frequency_index = use_aac_lc
                                                 ? AacSampleFrequencyIndexName(options.sample_rate)
                                                 : nullptr;
    if (use_aac_lc && aac_sample_frequency_index == nullptr) {
        SetError(error, "AAC-LC requires a supported IAMF sample rate");
        return false;
    }
    const int codec_samples_per_frame = use_aac_lc ? 1024 : options.samples_per_frame;
    const std::string wav_filename = input.media.path.filename().u8string();
    if (wav_filename.empty()) {
        SetError(error, "audio input filename cannot be empty");
        return false;
    }

    std::ostringstream text;
    text << "# proto-file: iamf/cli/proto/user_metadata.proto\n"
         << "# proto-message: UserMetadata\n\n"
         << "test_vector_metadata {\n"
         << "  human_readable_description: \"Generated Channel-based IAMF\"\n"
         << "  file_name_prefix: \"generated_iamf\"\n"
         << "  is_valid: true\n"
         << "}\n\n"
         << "ia_sequence_header_metadata {\n"
         << "  primary_profile: PROFILE_VERSION_SIMPLE\n"
         << "  additional_profile: PROFILE_VERSION_SIMPLE\n"
         << "}\n\n"
         << "codec_config_metadata {\n"
         << "  codec_config_id: 200\n"
         << "  codec_config {\n"
         << "    num_samples_per_frame: " << codec_samples_per_frame << "\n";
    if (use_lpcm) {
        text << "    decoder_config_lpcm {\n"
             << "      sample_format_flags: LPCM_LITTLE_ENDIAN\n"
             << "      sample_size: 16\n"
             << "      sample_rate: " << options.sample_rate << "\n"
             << "    }\n";
    } else if (use_aac_lc) {
        text << "    decoder_config_aac {\n"
             << "      buffer_size_db: 0\n"
             << "      max_bitrate: 0\n"
             << "      average_bit_rate: 0\n"
             << "      decoder_specific_info {\n"
             << "        sample_frequency_index: " << aac_sample_frequency_index << "\n"
             << "      }\n"
             << "      aac_encoder_metadata {\n"
             << "        bitrate_mode: 0\n"
             << "        enable_afterburner: true\n"
             << "        signaling_mode: 2\n"
             << "      }\n"
             << "    }\n";
    } else if (use_flac) {
        text << "    decoder_config_flac {\n"
             << "      metadata_blocks {\n"
             << "        header { block_type: FLAC_BLOCK_TYPE_STREAMINFO }\n"
             << "        stream_info {\n"
             << "          sample_rate: " << options.sample_rate << "\n"
             << "          bits_per_sample: 15\n"
             << "          total_samples_in_stream: 0\n"
             << "        }\n"
             << "      }\n"
             << "      flac_encoder_metadata { compression_level: 5 }\n"
             << "    }\n";
    } else {
        text << "    decoder_config_opus {\n"
             << "      version: 1\n"
             << "      input_sample_rate: " << options.sample_rate << "\n"
             << "      opus_encoder_metadata {\n"
             << "        target_bitrate_per_channel: " << options.opus_bitrate_per_channel << "\n"
             << "        application: APPLICATION_AUDIO\n"
             << "        use_float_api: false\n"
             << "      }\n"
             << "    }\n";
    }
    text << "  }\n"
         << "}\n\n";
    AppendChannelBasedFrameLabels(&text, labels, 300, wav_filename);
    text << "audio_element_metadata {\n"
         << "  audio_element_id: 300\n"
         << "  audio_element_type: AUDIO_ELEMENT_CHANNEL_BASED\n"
         << "  reserved: 0\n"
         << "  codec_config_id: 200\n"
         << "  audio_substream_ids: [";
    for (int substream = 0; substream < substream_count; ++substream)
        text << (substream == 0 ? "" : ", ") << substream;
    text << "]\n"
         << "  scalable_channel_layout_config {\n"
         << "    reserved: 0\n"
         << "    channel_audio_layer_configs: [{\n"
         << "      loudspeaker_layout: " << IamfLayoutName(input.channel_layout) << "\n"
         << "      output_gain_is_present_flag: 0\n"
         << "      recon_gain_is_present_flag: 0\n"
         << "      reserved_a: 0\n"
         << "      substream_count: " << substream_count << "\n"
         << "      coupled_substream_count: " << coupled_substream_count << "\n"
         << "    }]\n"
         << "  }\n"
         << "}\n\n"
         << "mix_presentation_metadata {\n"
         << "  mix_presentation_id: " << options.mix_id << "\n"
         << "  annotations_language: [\"en-us\"]\n"
         << "  localized_presentation_annotations: ["
         << ProtoString(options.mix_annotation.empty() ? "Generated Mix Presentation" : options.mix_annotation)
         << "]\n"
         << "  sub_mixes {\n";
    AppendSubMix(&text, 300, ToString(input.channel_layout),
                 "HEADPHONES_RENDERING_MODE_BINAURAL_WORLD_LOCKED", 998, options.sample_rate);
    text << "    output_mix_gain {\n";
    AppendGain(&text, 997, options.sample_rate);
    text << "    }\n";
    AppendLoudnessLayout(&text, "SOUND_SYSTEM_A_0_2_0");
    if (const char* sound_system = AdditionalChannelBasedSoundSystem(input.channel_layout))
        AppendLoudnessLayout(&text, sound_system);
    text << "  }\n"
         << "}\n\n"
         << "temporal_delimiter_metadata { enable_temporal_delimiters: false }\n";

    immersive_audio_muxer::AtomicOutputTransaction transaction;
    std::string transaction_error;
    if (!immersive_audio_muxer::AtomicOutputTransaction::Create(
            options.output_path, options.overwrite_existing_files, &transaction, &transaction_error)) {
        SetError(error, "cannot stage metadata textproto: " + transaction_error);
        return false;
    }
    std::ofstream output(transaction.temporary_path(), std::ios::binary | std::ios::trunc);
    if (!output) {
        SetError(error, "cannot create metadata textproto: " + options.output_path.u8string());
        return false;
    }
    const std::string serialized = text.str();
    output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
    if (!output.good()) {
        SetError(error, "failed to write metadata textproto");
        return false;
    }
    output.close();
    if (!transaction.Commit(&transaction_error)) {
        transaction.Abort();
        SetError(error, "cannot commit metadata textproto: " + transaction_error);
        return false;
    }
    return true;
}

}  // namespace immersive_audio_muxer::app
