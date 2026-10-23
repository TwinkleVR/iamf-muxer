#include "app/audio_preparation_service.h"

#include "runtime_logging.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cstdint>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <vector>

namespace immersive_audio_muxer::app {
namespace {

std::string AvError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

void SetError(std::string* error, const std::string& message) {
    if (error)
        *error = message;
    if (message.find("cancelled") != std::string::npos)
        IAMF_MUX_LOG_WARN("[AudioPreparation] {}", message);
    else
        IAMF_MUX_LOG_ERROR("[AudioPreparation] {}", message);
}

bool IsCancelled(const AudioPreparationOptions& options) {
    return options.cancellation_requested && options.cancellation_requested();
}

int AudioInputInterrupt(void* opaque) {
    const auto* callback = static_cast<const std::function<bool()>*>(opaque);
    return callback != nullptr && *callback && (*callback)() ? 1 : 0;
}

bool WriteU16(std::ofstream* output, uint16_t value) {
    const char bytes[2] = {static_cast<char>(value & 0xff), static_cast<char>(value >> 8)};
    output->write(bytes, sizeof(bytes));
    return output->good();
}

bool WriteU32(std::ofstream* output, uint32_t value) {
    const char bytes[4] = {static_cast<char>(value & 0xff), static_cast<char>((value >> 8) & 0xff),
                           static_cast<char>((value >> 16) & 0xff), static_cast<char>(value >> 24)};
    output->write(bytes, sizeof(bytes));
    return output->good();
}

bool WriteU64(std::ofstream* output, uint64_t value) {
    const char bytes[8] = {static_cast<char>(value & 0xff),
                           static_cast<char>((value >> 8) & 0xff),
                           static_cast<char>((value >> 16) & 0xff),
                           static_cast<char>((value >> 24) & 0xff),
                           static_cast<char>((value >> 32) & 0xff),
                           static_cast<char>((value >> 40) & 0xff),
                           static_cast<char>((value >> 48) & 0xff),
                           static_cast<char>((value >> 56) & 0xff)};
    output->write(bytes, sizeof(bytes));
    return output->good();
}

constexpr uint64_t kWavHeaderRiffBytes = 72;
constexpr uint32_t kReservedDs64ChunkBytes = 28;
constexpr std::streamoff kWavDataSizeOffset = 76;

struct WavSampleFormatInfo {
    AVSampleFormat swr_format = AV_SAMPLE_FMT_S16;
    uint16_t wav_format_code = 1;
    int bit_depth = 16;
    int bytes_per_sample = 2;
    int converted_bytes_per_sample = 2;
    const char* name = "s16";
};

WavSampleFormatInfo GetWavSampleFormatInfo(PreparedAudioSampleFormat format) {
    switch (format) {
        case PreparedAudioSampleFormat::kPcmS16:
            return {AV_SAMPLE_FMT_S16, 1, 16, 2, 2, "s16"};
        case PreparedAudioSampleFormat::kPcmS24:
            // libswresample has no packed S24 sample format. Convert through
            // S32 and write the most-significant 24 bits to the WAV payload.
            return {AV_SAMPLE_FMT_S32, 1, 24, 3, 4, "s24"};
        case PreparedAudioSampleFormat::kPcmS32:
            return {AV_SAMPLE_FMT_S32, 1, 32, 4, 4, "s32"};
        case PreparedAudioSampleFormat::kFloat32:
            return {AV_SAMPLE_FMT_FLT, 3, 32, 4, 4, "flt"};
    }
    return {};
}

bool IsFloatPcmCodec(AVCodecID codec_id) {
    return codec_id == AV_CODEC_ID_PCM_F32LE || codec_id == AV_CODEC_ID_PCM_F32BE ||
           codec_id == AV_CODEC_ID_PCM_F64LE || codec_id == AV_CODEC_ID_PCM_F64BE;
}

bool SelectPreparedSampleFormat(const AVCodecContext* decoder, const AVCodecParameters* parameters,
                                AudioPreparationPrecisionPolicy precision_policy,
                                int target_pcm_bit_depth,
                                PreparedAudioSampleFormat* format, std::string* error) {
    if (decoder == nullptr || parameters == nullptr || format == nullptr) {
        SetError(error, "cannot determine prepared WAV sample format");
        return false;
    }
    auto format_for_bit_depth = [](int bit_depth) {
        switch (bit_depth) {
            case 16:
                return PreparedAudioSampleFormat::kPcmS16;
            case 24:
                return PreparedAudioSampleFormat::kPcmS24;
            case 32:
                return PreparedAudioSampleFormat::kPcmS32;
            default:
                return PreparedAudioSampleFormat::kPcmS16;
        }
    };
    const AVSampleFormat packed_input_format = av_get_packed_sample_fmt(decoder->sample_fmt);
    if (IsFloatPcmCodec(parameters->codec_id)) {
        if (parameters->codec_id == AV_CODEC_ID_PCM_F64LE || parameters->codec_id == AV_CODEC_ID_PCM_F64BE) {
            SetError(error, "64-bit float PCM input is not supported by the prepared WAV precision policy");
            return false;
        }
        if (packed_input_format != AV_SAMPLE_FMT_FLT) {
            SetError(error, "32-bit float PCM decoder returned an unexpected sample format");
            return false;
        }
        if (target_pcm_bit_depth != 0) {
            *format = format_for_bit_depth(target_pcm_bit_depth);
        } else {
            *format = precision_policy == AudioPreparationPrecisionPolicy::kForcePcmS16
                          ? PreparedAudioSampleFormat::kPcmS16
                          : PreparedAudioSampleFormat::kFloat32;
        }
        return true;
    }

    int source_bit_depth = std::max(parameters->bits_per_raw_sample, parameters->bits_per_coded_sample);
    if (source_bit_depth <= 0)
        source_bit_depth = av_get_bits_per_sample(parameters->codec_id);
    if (source_bit_depth <= 0) {
        // Lossy codecs generally have no meaningful PCM source bit depth. Keep
        // the established S16 preparation behavior for those inputs.
        *format = PreparedAudioSampleFormat::kPcmS16;
    } else if (source_bit_depth <= 16) {
        *format = PreparedAudioSampleFormat::kPcmS16;
    } else if (source_bit_depth <= 24) {
        *format = PreparedAudioSampleFormat::kPcmS24;
    } else if (source_bit_depth <= 32) {
        *format = PreparedAudioSampleFormat::kPcmS32;
    } else {
        SetError(error, "audio source bit depth exceeds the supported 32-bit prepared WAV formats");
        return false;
    }
    if (precision_policy == AudioPreparationPrecisionPolicy::kForcePcmS16)
        *format = PreparedAudioSampleFormat::kPcmS16;
    if (target_pcm_bit_depth != 0)
        *format = format_for_bit_depth(target_pcm_bit_depth);
    return true;
}

bool WriteWavHeader(std::ofstream* output, int sample_rate, int channels,
                    const WavSampleFormatInfo& sample_format) {
    const uint64_t byte_rate_64 = static_cast<uint64_t>(sample_rate) * static_cast<uint64_t>(channels) *
                                  static_cast<uint64_t>(sample_format.bytes_per_sample);
    const uint32_t block_align_32 = static_cast<uint32_t>(channels) *
                                    static_cast<uint32_t>(sample_format.bytes_per_sample);
    if (byte_rate_64 > std::numeric_limits<uint32_t>::max() ||
        block_align_32 > std::numeric_limits<uint16_t>::max())
        return false;
    const uint32_t byte_rate = static_cast<uint32_t>(byte_rate_64);
    const uint16_t block_align = static_cast<uint16_t>(block_align_32);
    output->write("RIFF", 4);
    if (!WriteU32(output, 0))
        return false;
    output->write("WAVEJUNK", 8);
    if (!WriteU32(output, kReservedDs64ChunkBytes))
        return false;
    // Reserve a ds64-sized JUNK chunk so the header can be finalized as RF64.
    const char reserved_ds64[kReservedDs64ChunkBytes] = {};
    output->write(reserved_ds64, sizeof(reserved_ds64));
    output->write("fmt ", 4);
    if (!WriteU32(output, 16) || !WriteU16(output, sample_format.wav_format_code) ||
        !WriteU16(output, static_cast<uint16_t>(channels)) ||
        !WriteU32(output, static_cast<uint32_t>(sample_rate)) || !WriteU32(output, byte_rate) ||
        !WriteU16(output, block_align) || !WriteU16(output, static_cast<uint16_t>(sample_format.bit_depth)))
        return false;
    output->write("data", 4);
    return WriteU32(output, 0);
}

bool FinalizeWavHeader(std::ofstream* output, int channels, int bytes_per_sample, uint64_t data_bytes) {
    const uint64_t bytes_per_sample_frame = static_cast<uint64_t>(channels) *
                                            static_cast<uint64_t>(bytes_per_sample);
    if (channels <= 0 || bytes_per_sample <= 0 || bytes_per_sample_frame == 0 ||
        data_bytes % bytes_per_sample_frame != 0 ||
        data_bytes > std::numeric_limits<uint64_t>::max() - kWavHeaderRiffBytes) {
        return false;
    }

    const uint64_t riff_size = kWavHeaderRiffBytes + data_bytes;
    const uint64_t sample_count = data_bytes / bytes_per_sample_frame;
    const uint64_t max_classic_data_bytes =
        static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) - kWavHeaderRiffBytes;
    if (data_bytes <= max_classic_data_bytes) {
        if (!output->seekp(0, std::ios::beg))
            return false;
        output->write("RIFF", 4);
        if (!output->good() || !WriteU32(output, static_cast<uint32_t>(riff_size)) ||
            !output->seekp(kWavDataSizeOffset, std::ios::beg))
            return false;
        return WriteU32(output, static_cast<uint32_t>(data_bytes));
    }

    if (!output->seekp(0, std::ios::beg))
        return false;
    output->write("RF64", 4);
    if (!output->good() || !WriteU32(output, std::numeric_limits<uint32_t>::max()) ||
        !output->seekp(12, std::ios::beg))
        return false;
    output->write("ds64", 4);
    if (!output->good() || !WriteU32(output, kReservedDs64ChunkBytes) || !WriteU64(output, riff_size) ||
        !WriteU64(output, data_bytes) || !WriteU64(output, sample_count) || !WriteU32(output, 0) ||
        !output->seekp(kWavDataSizeOffset, std::ios::beg))
        return false;
    return WriteU32(output, std::numeric_limits<uint32_t>::max());
}

struct FormatContextCloser {
    void operator()(AVFormatContext* context) const { avformat_close_input(&context); }
};

struct CodecContextDeleter {
    void operator()(AVCodecContext* context) const { avcodec_free_context(&context); }
};

struct FrameDeleter {
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};

struct PacketDeleter {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};

struct SwrDeleter {
    void operator()(SwrContext* context) const { swr_free(&context); }
};

bool WriteSamples(std::ofstream* output, const uint8_t* data, int samples, int channels, int bytes_per_sample,
                  uint64_t* bytes_written, std::string* error) {
    if (samples <= 0)
        return true;
    const uint64_t bytes = static_cast<uint64_t>(samples) * static_cast<uint64_t>(channels) *
                           static_cast<uint64_t>(bytes_per_sample);
    if (bytes > std::numeric_limits<uint64_t>::max() - *bytes_written ||
        bytes > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        SetError(error, "prepared WAV exceeds the supported RF64 size limit");
        return false;
    }
    output->write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    if (!output->good()) {
        SetError(error, "failed to write prepared WAV");
        return false;
    }
    *bytes_written += bytes;
    return true;
}

bool ReorderInterleavedSamples(const uint8_t* source, int samples, int channels,
                               int bytes_per_sample,
                               const std::vector<int>& output_channel_to_input,
                               std::vector<uint8_t>* reordered, std::string* error) {
    if (source == nullptr || reordered == nullptr || samples < 0 || channels <= 0 || bytes_per_sample <= 0 ||
        output_channel_to_input.size() != static_cast<std::size_t>(channels)) {
        SetError(error, "invalid channel reorder configuration");
        return false;
    }
    reordered->resize(static_cast<std::size_t>(samples) * static_cast<std::size_t>(channels) *
                      static_cast<std::size_t>(bytes_per_sample));
    for (int sample = 0; sample < samples; ++sample) {
        for (int output_channel = 0; output_channel < channels; ++output_channel) {
            const int input_channel = output_channel_to_input[static_cast<std::size_t>(output_channel)];
            const std::size_t source_offset =
                (static_cast<std::size_t>(sample) * static_cast<std::size_t>(channels) +
                 static_cast<std::size_t>(input_channel)) * static_cast<std::size_t>(bytes_per_sample);
            const std::size_t output_offset =
                (static_cast<std::size_t>(sample) * static_cast<std::size_t>(channels) +
                 static_cast<std::size_t>(output_channel)) * static_cast<std::size_t>(bytes_per_sample);
            std::copy_n(source + source_offset, bytes_per_sample, reordered->data() + output_offset);
        }
    }
    return true;
}

bool PackS24Samples(const uint8_t* source, int samples, int channels, std::vector<uint8_t>* packed,
                    std::string* error) {
    if (source == nullptr || packed == nullptr || samples < 0 || channels <= 0) {
        SetError(error, "invalid 24-bit PCM packing configuration");
        return false;
    }
    const std::size_t sample_count = static_cast<std::size_t>(samples) * static_cast<std::size_t>(channels);
    packed->resize(sample_count * 3);
    for (std::size_t index = 0; index < sample_count; ++index) {
        int32_t sample = 0;
        std::memcpy(&sample, source + index * sizeof(sample), sizeof(sample));
        const uint32_t left_justified = static_cast<uint32_t>(sample);
        const uint32_t sample_24 = left_justified >> 8;
        (*packed)[index * 3] = static_cast<uint8_t>(sample_24 & 0xff);
        (*packed)[index * 3 + 1] = static_cast<uint8_t>((sample_24 >> 8) & 0xff);
        (*packed)[index * 3 + 2] = static_cast<uint8_t>((sample_24 >> 16) & 0xff);
    }
    return true;
}

}  // namespace

SharedPcmPreparationPlan PlanSharedPcmPreparation(const PreparedAudioInfo& first,
                                                  const PreparedAudioInfo& second) {
    const bool contains_float32 = first.sample_format == PreparedAudioSampleFormat::kFloat32 ||
                                  second.sample_format == PreparedAudioSampleFormat::kFloat32;
    const int target_bit_depth = contains_float32 ? 32 : std::max(first.bit_depth, second.bit_depth);
    if (contains_float32) {
        return {target_bit_depth,
                first.sample_format != PreparedAudioSampleFormat::kPcmS32 ||
                    first.bit_depth != target_bit_depth,
                second.sample_format != PreparedAudioSampleFormat::kPcmS32 ||
                    second.bit_depth != target_bit_depth};
    }
    return {target_bit_depth, first.bit_depth < target_bit_depth, second.bit_depth < target_bit_depth};
}

bool AudioPreparationService::PrepareToWav(const AudioPreparationOptions& options, PreparedAudioInfo* info,
                                            std::string* error) {
    IAMF_MUX_LOG_INFO(
        "[AudioPreparation] starting: input={} requested_stream={} output={} output_sample_rate={} precision_policy={} target_pcm_bit_depth={} preserve_unspecified_channel_order={} reorder_channels={}",
        options.input_path.u8string(), options.stream_index, options.output_wav_path.u8string(),
        options.output_sample_rate,
        options.precision_policy == AudioPreparationPrecisionPolicy::kForcePcmS16 ? "force_s16" : "preserve_source",
        options.target_pcm_bit_depth,
        options.preserve_unspecified_channel_order,
        options.output_channel_to_input.size());
    if (info == nullptr || options.input_path.empty() || options.output_wav_path.empty()) {
        SetError(error, "audio preparation requires input, output, and result arguments");
        return false;
    }
    if (options.output_sample_rate <= 0 || options.output_sample_rate > 384000) {
        SetError(error, "invalid output sample rate");
        return false;
    }
    if (options.target_pcm_bit_depth != 0 && options.target_pcm_bit_depth != 16 &&
        options.target_pcm_bit_depth != 24 && options.target_pcm_bit_depth != 32) {
        SetError(error, "target PCM bit depth must be 16, 24, or 32 bits");
        return false;
    }
    if (options.precision_policy == AudioPreparationPrecisionPolicy::kForcePcmS16 &&
        options.target_pcm_bit_depth != 0 && options.target_pcm_bit_depth != 16) {
        SetError(error, "forced 16-bit PCM policy conflicts with the requested target bit depth");
        return false;
    }
    std::error_code filesystem_error;
    if (!std::filesystem::exists(options.input_path, filesystem_error)) {
        SetError(error, "audio input does not exist: " + options.input_path.u8string());
        return false;
    }
    if (std::filesystem::absolute(options.input_path, filesystem_error).lexically_normal() ==
        std::filesystem::absolute(options.output_wav_path, filesystem_error).lexically_normal()) {
        SetError(error, "audio input and prepared WAV must be different files");
        return false;
    }

    AVFormatContext* raw_format = avformat_alloc_context();
    if (raw_format == nullptr) {
        SetError(error, "cannot allocate audio input context");
        return false;
    }
    raw_format->interrupt_callback.callback = AudioInputInterrupt;
    raw_format->interrupt_callback.opaque =
        const_cast<std::function<bool()>*>(&options.cancellation_requested);
    int status = avformat_open_input(&raw_format, options.input_path.u8string().c_str(), nullptr, nullptr);
    if (status < 0) {
        avformat_close_input(&raw_format);
        SetError(error, "cannot open audio input: " + AvError(status));
        return false;
    }
    std::unique_ptr<AVFormatContext, FormatContextCloser> format(raw_format);
    status = avformat_find_stream_info(format.get(), nullptr);
    if (status < 0) {
        SetError(error, "cannot read audio stream info: " + AvError(status));
        return false;
    }
    int stream_index = options.stream_index;
    if (stream_index < 0) {
        for (unsigned int i = 0; i < format->nb_streams; ++i) {
            if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                stream_index = static_cast<int>(i);
                break;
            }
        }
    }
    if (stream_index < 0 || static_cast<unsigned int>(stream_index) >= format->nb_streams ||
        format->streams[stream_index]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
        SetError(error, "audio input has no selected audio stream");
        return false;
    }
    const AVStream* stream = format->streams[stream_index];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
        SetError(error, "no FFmpeg decoder is available for the selected audio stream");
        return false;
    }
    AVCodecContext* raw_codec = avcodec_alloc_context3(codec);
    if (raw_codec == nullptr) {
        SetError(error, "cannot allocate FFmpeg audio decoder");
        return false;
    }
    std::unique_ptr<AVCodecContext, CodecContextDeleter> decoder(raw_codec);
    status = avcodec_parameters_to_context(decoder.get(), stream->codecpar);
    if (status < 0 || (status = avcodec_open2(decoder.get(), codec, nullptr)) < 0) {
        SetError(error, "cannot open FFmpeg audio decoder: " + AvError(status));
        return false;
    }
    if (decoder->sample_rate <= 0 || decoder->sample_rate > 384000) {
        SetError(error, "selected audio stream has an invalid sample rate");
        return false;
    }
    PreparedAudioSampleFormat prepared_format = PreparedAudioSampleFormat::kPcmS16;
    if (!SelectPreparedSampleFormat(decoder.get(), stream->codecpar, options.precision_policy,
                                    options.target_pcm_bit_depth,
                                    &prepared_format, error))
        return false;
    const WavSampleFormatInfo sample_format = GetWavSampleFormatInfo(prepared_format);

    AVChannelLayout input_layout{};
    if (decoder->ch_layout.nb_channels <= 0) {
        SetError(error, "selected audio stream has no usable channel count");
        return false;
    }
    std::array<char, 128> input_channel_layout{};
    if (av_channel_layout_describe(&decoder->ch_layout, input_channel_layout.data(),
                                   input_channel_layout.size()) < 0)
        input_channel_layout[0] = '\0';
    IAMF_MUX_LOG_INFO(
        "[AudioPreparation] selected stream: index={} codec={} sample_rate={} channels={} sample_format={} source_bits_per_sample={} prepared_format={} prepared_bits_per_sample={} channel_layout={} time_base={}/{} bitrate={}",
        stream_index, codec->name, decoder->sample_rate, decoder->ch_layout.nb_channels,
        av_get_sample_fmt_name(decoder->sample_fmt) == nullptr
            ? "unknown"
            : av_get_sample_fmt_name(decoder->sample_fmt),
        std::max(stream->codecpar->bits_per_raw_sample, stream->codecpar->bits_per_coded_sample),
        sample_format.name, sample_format.bit_depth,
        input_channel_layout[0] == '\0' ? "unknown" : input_channel_layout.data(),
        stream->time_base.num, stream->time_base.den, stream->codecpar->bit_rate);
    if (decoder->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC) {
        if (av_channel_layout_copy(&input_layout, &decoder->ch_layout) < 0) {
            SetError(error, "cannot copy input channel layout");
            return false;
        }
    } else if (options.preserve_unspecified_channel_order) {
        // Preserve discrete channel indices without assigning speaker meaning
        // based only on the number of channels.
        input_layout.order = AV_CHANNEL_ORDER_UNSPEC;
        input_layout.nb_channels = decoder->ch_layout.nb_channels;
    } else {
        av_channel_layout_default(&input_layout, decoder->ch_layout.nb_channels);
    }
    AVChannelLayout output_layout{};
    if (av_channel_layout_copy(&output_layout, &input_layout) < 0) {
        av_channel_layout_uninit(&input_layout);
        SetError(error, "cannot copy output channel layout");
        return false;
    }
    SwrContext* raw_swr = nullptr;
    status = swr_alloc_set_opts2(&raw_swr, &output_layout, sample_format.swr_format, options.output_sample_rate,
                                 &input_layout, decoder->sample_fmt, decoder->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&input_layout);
    av_channel_layout_uninit(&output_layout);
    if (status < 0 || raw_swr == nullptr) {
        SetError(error, "cannot configure audio resampler: " + AvError(status));
        return false;
    }
    std::unique_ptr<SwrContext, SwrDeleter> resampler(raw_swr);
    status = swr_init(resampler.get());
    if (status < 0) {
        SetError(error, "cannot initialize audio resampler: " + AvError(status));
        return false;
    }

    const int output_channels = decoder->ch_layout.nb_channels;
    if (output_channels <= 0 || output_channels > 255) {
        SetError(error, "selected audio stream has an invalid channel count");
        return false;
    }
    std::vector<int> output_channel_to_input = options.output_channel_to_input;
    if (!output_channel_to_input.empty()) {
        if (output_channel_to_input.size() != static_cast<std::size_t>(output_channels)) {
            SetError(error, "channel reorder map does not match the source channel count");
            return false;
        }
        std::vector<bool> seen(static_cast<std::size_t>(output_channels), false);
        for (const int input_channel : output_channel_to_input) {
            if (input_channel < 0 || input_channel >= output_channels ||
                seen[static_cast<std::size_t>(input_channel)]) {
                SetError(error, "channel reorder map must map every source channel exactly once");
                return false;
            }
            seen[static_cast<std::size_t>(input_channel)] = true;
        }
    }
    std::ofstream output(options.output_wav_path, std::ios::binary | std::ios::trunc);
    if (!output || !WriteWavHeader(&output, options.output_sample_rate, output_channels, sample_format)) {
        SetError(error, "cannot create prepared WAV: " + options.output_wav_path.u8string());
        return false;
    }
    std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
    std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
    if (!packet || !frame) {
        SetError(error, "cannot allocate FFmpeg audio buffers");
        return false;
    }
    std::vector<uint8_t> converted;
    std::vector<uint8_t> reordered;
    std::vector<uint8_t> packed_s24;
    uint64_t data_bytes = 0;
    auto write_converted_samples = [&](int converted_samples) -> bool {
        const uint8_t* samples_to_write = converted.data();
        if (!output_channel_to_input.empty()) {
            if (!ReorderInterleavedSamples(converted.data(), converted_samples, output_channels,
                                           sample_format.converted_bytes_per_sample,
                                           output_channel_to_input, &reordered, error))
                return false;
            samples_to_write = reordered.data();
        }
        if (prepared_format == PreparedAudioSampleFormat::kPcmS24) {
            if (!PackS24Samples(samples_to_write, converted_samples, output_channels, &packed_s24, error))
                return false;
            samples_to_write = packed_s24.data();
        }
        return WriteSamples(&output, samples_to_write, converted_samples, output_channels,
                            sample_format.bytes_per_sample, &data_bytes, error);
    };
    auto receive_frames = [&]() -> bool {
        while (true) {
            const int receive_status = avcodec_receive_frame(decoder.get(), frame.get());
            if (receive_status == AVERROR(EAGAIN) || receive_status == AVERROR_EOF)
                return true;
            if (receive_status < 0) {
                SetError(error, "audio decoder failed: " + AvError(receive_status));
                return false;
            }
            if (IsCancelled(options)) {
                SetError(error, "audio preparation cancelled");
                return false;
            }
            const int64_t delay = swr_get_delay(resampler.get(), decoder->sample_rate);
            const int64_t output_capacity_64 = av_rescale_rnd(
                delay + frame->nb_samples, options.output_sample_rate, decoder->sample_rate, AV_ROUND_UP);
            if (output_capacity_64 <= 0 || output_capacity_64 > std::numeric_limits<int>::max()) {
                SetError(error, "decoded audio frame is too large for the output buffer");
                return false;
            }
            const int output_capacity = static_cast<int>(output_capacity_64);
            converted.resize(static_cast<size_t>(output_capacity) * static_cast<size_t>(output_channels) *
                              static_cast<size_t>(sample_format.converted_bytes_per_sample));
            uint8_t* output_data[1] = {converted.data()};
            const int converted_samples = swr_convert(resampler.get(), output_data, output_capacity,
                                                       const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
            if (converted_samples < 0) {
                SetError(error, "failed while converting audio samples: " + AvError(converted_samples));
                return false;
            }
            if (!write_converted_samples(converted_samples))
                return false;
            av_frame_unref(frame.get());
        }
    };

    while ((status = av_read_frame(format.get(), packet.get())) >= 0) {
        if (IsCancelled(options)) {
            SetError(error, "audio preparation cancelled");
            return false;
        }
        if (packet->stream_index == stream_index) {
            status = avcodec_send_packet(decoder.get(), packet.get());
            if (status < 0) {
                SetError(error, "audio decoder rejected packet: " + AvError(status));
                return false;
            }
            if (!receive_frames())
                return false;
        }
        av_packet_unref(packet.get());
    }
    if (status != AVERROR_EOF) {
        SetError(error, "failed while reading audio input: " + AvError(status));
        return false;
    }
    status = avcodec_send_packet(decoder.get(), nullptr);
    if (status < 0 && status != AVERROR_EOF) {
        SetError(error, "failed to flush audio decoder: " + AvError(status));
        return false;
    }
    if (!receive_frames())
        return false;
    const int64_t flushed_capacity_64 = av_rescale_rnd(
        swr_get_delay(resampler.get(), decoder->sample_rate), options.output_sample_rate, decoder->sample_rate, AV_ROUND_UP);
    if (flushed_capacity_64 < 0 || flushed_capacity_64 > std::numeric_limits<int>::max()) {
        SetError(error, "flushed audio is too large for the output buffer");
        return false;
    }
    const int flushed_capacity = static_cast<int>(flushed_capacity_64);
    if (flushed_capacity > 0) {
        converted.resize(static_cast<size_t>(flushed_capacity) * static_cast<size_t>(output_channels) *
                          static_cast<size_t>(sample_format.converted_bytes_per_sample));
        uint8_t* output_data[1] = {converted.data()};
        const int flushed_samples = swr_convert(resampler.get(), output_data, flushed_capacity, nullptr, 0);
        if (flushed_samples < 0) {
            SetError(error, "failed while flushing converted audio samples: " + AvError(flushed_samples));
            return false;
        }
        if (!write_converted_samples(flushed_samples))
            return false;
    }
    if (!FinalizeWavHeader(&output, output_channels, sample_format.bytes_per_sample, data_bytes)) {
        SetError(error, "failed to finalize prepared WAV header");
        return false;
    }
    output.close();
    info->wav_path = options.output_wav_path;
    info->sample_format = prepared_format;
    info->bit_depth = sample_format.bit_depth;
    const uint64_t sample_count = data_bytes /
                                  (static_cast<uint64_t>(output_channels) *
                                   static_cast<uint64_t>(sample_format.bytes_per_sample));
    const double duration_seconds = static_cast<double>(sample_count) / options.output_sample_rate;
    IAMF_MUX_LOG_INFO(
        "[AudioPreparation] completed: input={} stream={} output={} output_sample_rate={} channels={} sample_format={} bit_depth={} samples={} duration_seconds={:.3f} data_bytes={}",
        options.input_path.u8string(), stream_index, options.output_wav_path.u8string(),
        options.output_sample_rate, output_channels, sample_format.name, sample_format.bit_depth,
        sample_count, duration_seconds, data_bytes);
    return true;
}

}  // namespace immersive_audio_muxer::app
