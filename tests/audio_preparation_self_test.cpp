#include "app/audio_preparation_service.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace immersive_audio_muxer::app;

#define CHECK(expression)                                                                        \
    do {                                                                                         \
        if (!(expression)) {                                                                     \
            std::cerr << "check failed: " #expression " at line " << __LINE__ << '\n';          \
            return false;                                                                        \
        }                                                                                        \
    } while (false)

namespace {

void WriteU16(std::ostream* output, uint16_t value) {
    output->put(static_cast<char>(value & 0xff));
    output->put(static_cast<char>((value >> 8) & 0xff));
}

void WriteU32(std::ostream* output, uint32_t value) {
    for (int byte = 0; byte < 4; ++byte)
        output->put(static_cast<char>((value >> (byte * 8)) & 0xff));
}

uint16_t ReadU16(const std::vector<uint8_t>& bytes, std::size_t offset) {
    return static_cast<uint16_t>(bytes[offset]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[offset + 1]) << 8);
}

uint32_t ReadU32(const std::vector<uint8_t>& bytes, std::size_t offset) {
    uint32_t value = 0;
    for (int byte = 0; byte < 4; ++byte)
        value |= static_cast<uint32_t>(bytes[offset + byte]) << (byte * 8);
    return value;
}

bool WriteInputWav(const std::filesystem::path& path, int bit_depth,
                   const std::vector<int32_t>& samples, int sample_rate = 48000,
                   int channels = 1) {
    const int bytes_per_sample = bit_depth / 8;
    const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * bytes_per_sample);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;
    output.write("RIFF", 4);
    WriteU32(&output, 36 + data_bytes);
    output.write("WAVEfmt ", 8);
    WriteU32(&output, 16);
    WriteU16(&output, 1);
    WriteU16(&output, static_cast<uint16_t>(channels));
    WriteU32(&output, static_cast<uint32_t>(sample_rate));
    WriteU32(&output, static_cast<uint32_t>(sample_rate * channels * bytes_per_sample));
    WriteU16(&output, static_cast<uint16_t>(channels * bytes_per_sample));
    WriteU16(&output, static_cast<uint16_t>(bit_depth));
    output.write("data", 4);
    WriteU32(&output, data_bytes);
    for (const int32_t sample : samples) {
        const uint32_t raw_sample = static_cast<uint32_t>(sample);
        for (int byte = 0; byte < bytes_per_sample; ++byte)
            output.put(static_cast<char>((raw_sample >> (byte * 8)) & 0xff));
    }
    return output.good();
}

bool WriteFloatInputWav(const std::filesystem::path& path, const std::vector<float>& samples) {
    const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * sizeof(float));
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;
    output.write("RIFF", 4);
    WriteU32(&output, 36 + data_bytes);
    output.write("WAVEfmt ", 8);
    WriteU32(&output, 16);
    WriteU16(&output, 3);
    WriteU16(&output, 1);
    WriteU32(&output, 48000);
    WriteU32(&output, 48000 * sizeof(float));
    WriteU16(&output, sizeof(float));
    WriteU16(&output, 32);
    output.write("data", 4);
    WriteU32(&output, data_bytes);
    for (const float sample : samples) {
        uint32_t bits = 0;
        std::memcpy(&bits, &sample, sizeof(bits));
        WriteU32(&output, bits);
    }
    return output.good();
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>());
}

bool PrepareAndCheckInteger(const std::filesystem::path& directory, int bit_depth,
                            const std::vector<int32_t>& samples) {
    const auto input_path = directory / ("source_s" + std::to_string(bit_depth) + ".wav");
    const auto output_path = directory / ("prepared_s" + std::to_string(bit_depth) + ".wav");
    CHECK(WriteInputWav(input_path, bit_depth, samples));
    AudioPreparationOptions options;
    options.input_path = input_path;
    options.stream_index = 0;
    options.output_wav_path = output_path;
    options.output_sample_rate = 48000;
    PreparedAudioInfo prepared;
    std::string error;
    CHECK(AudioPreparationService::PrepareToWav(options, &prepared, &error));
    CHECK(prepared.bit_depth == bit_depth);
    CHECK(prepared.sample_format == (bit_depth == 16 ? PreparedAudioSampleFormat::kPcmS16
                                   : bit_depth == 24 ? PreparedAudioSampleFormat::kPcmS24
                                                     : PreparedAudioSampleFormat::kPcmS32));

    const std::vector<uint8_t> bytes = ReadFile(output_path);
    CHECK(bytes.size() == 80 + samples.size() * static_cast<std::size_t>(bit_depth / 8));
    CHECK(std::string(bytes.begin(), bytes.begin() + 4) == "RIFF");
    CHECK(ReadU16(bytes, 56) == 1);
    CHECK(ReadU16(bytes, 68) == bit_depth / 8);
    CHECK(ReadU16(bytes, 70) == bit_depth);
    CHECK(ReadU32(bytes, 76) == samples.size() * static_cast<std::size_t>(bit_depth / 8));
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const uint32_t raw_sample = static_cast<uint32_t>(samples[index]);
        for (int byte = 0; byte < bit_depth / 8; ++byte) {
            const uint8_t expected = static_cast<uint8_t>((raw_sample >> (byte * 8)) & 0xff);
            CHECK(bytes[80 + index * static_cast<std::size_t>(bit_depth / 8) + byte] == expected);
        }
    }
    return true;
}

bool PrepareAndCheckForcedS16(const std::filesystem::path& directory, int source_bit_depth,
                              const std::vector<int32_t>& samples) {
    const auto input_path = directory / ("source_s" + std::to_string(source_bit_depth) + "_aac.wav");
    const auto output_path = directory / ("prepared_s16_from_s" + std::to_string(source_bit_depth) + ".wav");
    CHECK(WriteInputWav(input_path, source_bit_depth, samples));
    AudioPreparationOptions options;
    options.input_path = input_path;
    options.stream_index = 0;
    options.output_wav_path = output_path;
    options.output_sample_rate = 48000;
    options.precision_policy = AudioPreparationPrecisionPolicy::kForcePcmS16;
    PreparedAudioInfo prepared;
    std::string error;
    CHECK(AudioPreparationService::PrepareToWav(options, &prepared, &error));
    CHECK(prepared.bit_depth == 16);
    CHECK(prepared.sample_format == PreparedAudioSampleFormat::kPcmS16);

    const std::vector<uint8_t> bytes = ReadFile(output_path);
    CHECK(bytes.size() == 80 + samples.size() * sizeof(int16_t));
    CHECK(ReadU16(bytes, 56) == 1);
    CHECK(ReadU16(bytes, 68) == sizeof(int16_t));
    CHECK(ReadU16(bytes, 70) == 16);
    CHECK(ReadU32(bytes, 76) == samples.size() * sizeof(int16_t));
    bool has_nonzero_sample = false;
    for (std::size_t index = 80; index < bytes.size(); ++index)
        has_nonzero_sample = has_nonzero_sample || bytes[index] != 0;
    CHECK(has_nonzero_sample);
    return true;
}

bool PrepareAndCheckTargetBitDepth(const std::filesystem::path& directory, int source_bit_depth,
                                   int target_bit_depth, const std::vector<int32_t>& samples) {
    const auto input_path = directory / ("source_s" + std::to_string(source_bit_depth) +
                                         "_target_s" + std::to_string(target_bit_depth) + ".wav");
    const auto output_path = directory / ("prepared_s" + std::to_string(target_bit_depth) +
                                          "_from_s" + std::to_string(source_bit_depth) + ".wav");
    CHECK(WriteInputWav(input_path, source_bit_depth, samples));
    AudioPreparationOptions options;
    options.input_path = input_path;
    options.stream_index = 0;
    options.output_wav_path = output_path;
    options.output_sample_rate = 48000;
    options.target_pcm_bit_depth = target_bit_depth;
    PreparedAudioInfo prepared;
    std::string error;
    CHECK(AudioPreparationService::PrepareToWav(options, &prepared, &error));
    CHECK(prepared.bit_depth == target_bit_depth);
    CHECK(prepared.sample_format == (target_bit_depth == 16 ? PreparedAudioSampleFormat::kPcmS16
                                   : target_bit_depth == 24 ? PreparedAudioSampleFormat::kPcmS24
                                                            : PreparedAudioSampleFormat::kPcmS32));

    const std::vector<uint8_t> bytes = ReadFile(output_path);
    const int bytes_per_sample = target_bit_depth / 8;
    CHECK(bytes.size() == 80 + samples.size() * static_cast<std::size_t>(bytes_per_sample));
    CHECK(ReadU16(bytes, 56) == 1);
    CHECK(ReadU16(bytes, 68) == bytes_per_sample);
    CHECK(ReadU16(bytes, 70) == target_bit_depth);
    CHECK(ReadU32(bytes, 76) == samples.size() * static_cast<std::size_t>(bytes_per_sample));
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const uint32_t expected_sample = static_cast<uint32_t>(samples[index]) <<
                                         (target_bit_depth - source_bit_depth);
        for (int byte = 0; byte < bytes_per_sample; ++byte) {
            const uint8_t expected = static_cast<uint8_t>((expected_sample >> (byte * 8)) & 0xff);
            CHECK(bytes[80 + index * static_cast<std::size_t>(bytes_per_sample) + byte] == expected);
        }
    }
    return true;
}

bool PrepareAndCheckFloat32(const std::filesystem::path& directory) {
    const auto input_path = directory / "source_f32.wav";
    const auto output_path = directory / "prepared_f32.wav";
    const std::vector<float> samples = {-0.5f, -0.125f, 0.25f, 0.5f};
    CHECK(WriteFloatInputWav(input_path, samples));
    AudioPreparationOptions options;
    options.input_path = input_path;
    options.stream_index = 0;
    options.output_wav_path = output_path;
    options.output_sample_rate = 48000;
    PreparedAudioInfo prepared;
    std::string error;
    CHECK(AudioPreparationService::PrepareToWav(options, &prepared, &error));
    CHECK(prepared.bit_depth == 32);
    CHECK(prepared.sample_format == PreparedAudioSampleFormat::kFloat32);

    const std::vector<uint8_t> bytes = ReadFile(output_path);
    CHECK(bytes.size() == 80 + samples.size() * sizeof(float));
    CHECK(ReadU16(bytes, 56) == 3);
    CHECK(ReadU16(bytes, 68) == sizeof(float));
    CHECK(ReadU16(bytes, 70) == 32);
    CHECK(ReadU32(bytes, 76) == samples.size() * sizeof(float));
    for (std::size_t index = 0; index < samples.size(); ++index) {
        uint32_t expected = 0;
        std::memcpy(&expected, &samples[index], sizeof(expected));
        CHECK(ReadU32(bytes, 80 + index * sizeof(float)) == expected);
    }

    options.output_wav_path = directory / "prepared_s32_from_f32.wav";
    options.target_pcm_bit_depth = 32;
    PreparedAudioInfo prepared_s32;
    CHECK(AudioPreparationService::PrepareToWav(options, &prepared_s32, &error));
    CHECK(prepared_s32.bit_depth == 32);
    CHECK(prepared_s32.sample_format == PreparedAudioSampleFormat::kPcmS32);
    const std::vector<uint8_t> s32_bytes = ReadFile(options.output_wav_path);
    CHECK(s32_bytes.size() == 80 + samples.size() * sizeof(int32_t));
    CHECK(ReadU16(s32_bytes, 56) == 1);
    CHECK(ReadU16(s32_bytes, 68) == sizeof(int32_t));
    CHECK(ReadU16(s32_bytes, 70) == 32);
    CHECK(ReadU32(s32_bytes, 76) == samples.size() * sizeof(int32_t));
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const int64_t expected = static_cast<int64_t>(samples[index] * 2147483648.0);
        const int64_t actual = static_cast<int32_t>(ReadU32(s32_bytes, 80 + index * sizeof(int32_t)));
        const int64_t difference = actual > expected ? actual - expected : expected - actual;
        CHECK(difference <= 1);
    }

    options.output_wav_path = directory / "prepared_s16_from_f32.wav";
    options.target_pcm_bit_depth = 0;
    options.precision_policy = AudioPreparationPrecisionPolicy::kForcePcmS16;
    PreparedAudioInfo prepared_s16;
    CHECK(AudioPreparationService::PrepareToWav(options, &prepared_s16, &error));
    CHECK(prepared_s16.bit_depth == 16);
    CHECK(prepared_s16.sample_format == PreparedAudioSampleFormat::kPcmS16);
    const std::vector<uint8_t> s16_bytes = ReadFile(options.output_wav_path);
    CHECK(s16_bytes.size() == 80 + samples.size() * sizeof(int16_t));
    CHECK(ReadU16(s16_bytes, 56) == 1);
    CHECK(ReadU16(s16_bytes, 68) == sizeof(int16_t));
    CHECK(ReadU16(s16_bytes, 70) == 16);
    CHECK(ReadU32(s16_bytes, 76) == samples.size() * sizeof(int16_t));
    bool has_nonzero_sample = false;
    for (std::size_t index = 80; index < s16_bytes.size(); ++index)
        has_nonzero_sample = has_nonzero_sample || s16_bytes[index] != 0;
    CHECK(has_nonzero_sample);
    return true;
}

bool CheckSharedPcmPreparationPlans() {
    const PreparedAudioInfo s16{ {}, PreparedAudioSampleFormat::kPcmS16, 16 };
    const PreparedAudioInfo s24{ {}, PreparedAudioSampleFormat::kPcmS24, 24 };
    const PreparedAudioInfo s32{ {}, PreparedAudioSampleFormat::kPcmS32, 32 };
    const PreparedAudioInfo f32{ {}, PreparedAudioSampleFormat::kFloat32, 32 };

    const SharedPcmPreparationPlan integer_plan = PlanSharedPcmPreparation(s16, s24);
    CHECK(integer_plan.target_bit_depth == 24);
    CHECK(integer_plan.reprepare_first);
    CHECK(!integer_plan.reprepare_second);
    const SharedPcmPreparationPlan reversed_integer_plan = PlanSharedPcmPreparation(s24, s16);
    CHECK(reversed_integer_plan.target_bit_depth == 24);
    CHECK(!reversed_integer_plan.reprepare_first);
    CHECK(reversed_integer_plan.reprepare_second);

    for (const auto& pair : {std::pair<PreparedAudioInfo, PreparedAudioInfo>{f32, s16}, {s16, f32}}) {
        const SharedPcmPreparationPlan float_plan = PlanSharedPcmPreparation(pair.first, pair.second);
        CHECK(float_plan.target_bit_depth == 32);
        CHECK(float_plan.reprepare_first);
        CHECK(float_plan.reprepare_second);
    }

    const SharedPcmPreparationPlan float_and_s24_plan = PlanSharedPcmPreparation(f32, s24);
    CHECK(float_and_s24_plan.target_bit_depth == 32);
    CHECK(float_and_s24_plan.reprepare_first);
    CHECK(float_and_s24_plan.reprepare_second);
    const SharedPcmPreparationPlan float_pair_plan = PlanSharedPcmPreparation(f32, f32);
    CHECK(float_pair_plan.target_bit_depth == 32);
    CHECK(float_pair_plan.reprepare_first);
    CHECK(float_pair_plan.reprepare_second);

    const SharedPcmPreparationPlan float_and_s32_plan = PlanSharedPcmPreparation(f32, s32);
    CHECK(float_and_s32_plan.target_bit_depth == 32);
    CHECK(float_and_s32_plan.reprepare_first);
    CHECK(!float_and_s32_plan.reprepare_second);

    const SharedPcmPreparationPlan s32_and_float_plan = PlanSharedPcmPreparation(s32, f32);
    CHECK(s32_and_float_plan.target_bit_depth == 32);
    CHECK(!s32_and_float_plan.reprepare_first);
    CHECK(s32_and_float_plan.reprepare_second);
    return true;
}

bool PrepareAndCheckReorderedS24(const std::filesystem::path& directory) {
    const auto input_path = directory / "source_s24_stereo.wav";
    const auto output_path = directory / "prepared_s24_stereo.wav";
    CHECK(WriteInputWav(input_path, 24, {0x123456, -0x234567, 1, -1}, 48000, 2));
    AudioPreparationOptions options;
    options.input_path = input_path;
    options.stream_index = 0;
    options.output_wav_path = output_path;
    options.output_sample_rate = 48000;
    options.output_channel_to_input = {1, 0};
    PreparedAudioInfo prepared;
    std::string error;
    CHECK(AudioPreparationService::PrepareToWav(options, &prepared, &error));
    CHECK(prepared.bit_depth == 24);

    const std::vector<uint8_t> bytes = ReadFile(output_path);
    CHECK(ReadU16(bytes, 58) == 2);
    CHECK(ReadU32(bytes, 76) == 12);
    const std::vector<int32_t> expected_samples = {-0x234567, 0x123456, -1, 1};
    for (std::size_t index = 0; index < expected_samples.size(); ++index) {
        const uint32_t raw_sample = static_cast<uint32_t>(expected_samples[index]);
        for (int byte = 0; byte < 3; ++byte) {
            const uint8_t expected = static_cast<uint8_t>((raw_sample >> (byte * 8)) & 0xff);
            CHECK(bytes[80 + index * 3 + byte] == expected);
        }
    }
    return true;
}

bool PrepareAndCheckResampledS24(const std::filesystem::path& directory) {
    const auto input_path = directory / "source_s24_resampled.wav";
    const auto output_path = directory / "prepared_s24_resampled.wav";
    std::vector<int32_t> samples(100, 0);
    samples.back() = 0x234567;
    CHECK(WriteInputWav(input_path, 24, samples, 44100));
    AudioPreparationOptions options;
    options.input_path = input_path;
    options.stream_index = 0;
    options.output_wav_path = output_path;
    options.output_sample_rate = 48000;
    PreparedAudioInfo prepared;
    std::string error;
    CHECK(AudioPreparationService::PrepareToWav(options, &prepared, &error));
    CHECK(prepared.bit_depth == 24);

    const std::vector<uint8_t> bytes = ReadFile(output_path);
    CHECK(ReadU32(bytes, 64) == 48000 * 3);
    CHECK(ReadU16(bytes, 70) == 24);
    const uint32_t data_bytes = ReadU32(bytes, 76);
    CHECK(data_bytes > 0);
    CHECK(data_bytes % 3 == 0);
    const uint32_t output_sample_count = data_bytes / 3;
    CHECK(output_sample_count >= 108 && output_sample_count <= 110);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: audio_preparation_self_test <output-directory>\n";
        return 2;
    }
    const std::filesystem::path directory = argv[1];
    std::error_code filesystem_error;
    std::filesystem::create_directories(directory, filesystem_error);
    if (filesystem_error) {
        std::cerr << "cannot create test directory: " << filesystem_error.message() << '\n';
        return 1;
    }

    if (!PrepareAndCheckInteger(directory, 16, {12000, -12000, 1, -1}) ||
        !PrepareAndCheckInteger(directory, 24, {0x123456, -0x123456, 1, -1}) ||
        !PrepareAndCheckInteger(directory, 32, {0x12345678, -0x12345678, 1, -1}) ||
        !PrepareAndCheckTargetBitDepth(directory, 16, 24, {12000, -12000, 1, -1}) ||
        !PrepareAndCheckTargetBitDepth(directory, 16, 32, {12000, -12000, 1, -1}) ||
        !PrepareAndCheckTargetBitDepth(directory, 24, 32, {0x123456, -0x123456, 1, -1}) ||
        !PrepareAndCheckForcedS16(directory, 16, {12000, -12000, 1, -1}) ||
        !PrepareAndCheckForcedS16(directory, 24, {0x123456, -0x123456, 1, -1}) ||
        !PrepareAndCheckForcedS16(directory, 32, {0x12345678, -0x12345678, 1, -1}) ||
        !PrepareAndCheckFloat32(directory) ||
        !CheckSharedPcmPreparationPlans() ||
        !PrepareAndCheckReorderedS24(directory) ||
        !PrepareAndCheckResampledS24(directory))
        return 1;
    return 0;
}

#undef CHECK
