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

#include <cstdint>
#include <array>
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

bool WriteWavHeader(std::ofstream* output, int sample_rate, int channels, uint32_t data_bytes) {
    const uint32_t byte_rate = static_cast<uint32_t>(sample_rate) * static_cast<uint32_t>(channels) * 2;
    const uint16_t block_align = static_cast<uint16_t>(channels * 2);
    output->write("RIFF", 4);
    if (!WriteU32(output, 36u + data_bytes))
        return false;
    output->write("WAVEfmt ", 8);
    if (!WriteU32(output, 16) || !WriteU16(output, 1) || !WriteU16(output, static_cast<uint16_t>(channels)) ||
        !WriteU32(output, static_cast<uint32_t>(sample_rate)) || !WriteU32(output, byte_rate) ||
        !WriteU16(output, block_align) || !WriteU16(output, 16))
        return false;
    output->write("data", 4);
    return WriteU32(output, data_bytes);
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

bool WriteSamples(std::ofstream* output, const uint8_t* data, int samples, int channels, uint64_t* bytes_written,
                  std::string* error) {
    if (samples <= 0)
        return true;
    const uint64_t bytes = static_cast<uint64_t>(samples) * static_cast<uint64_t>(channels) * 2;
    if (bytes > std::numeric_limits<uint32_t>::max() - *bytes_written) {
        SetError(error, "prepared WAV exceeds the classic RIFF 4 GiB limit");
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
                               const std::vector<int>& output_channel_to_input,
                               std::vector<uint8_t>* reordered, std::string* error) {
    if (source == nullptr || reordered == nullptr || samples < 0 || channels <= 0 ||
        output_channel_to_input.size() != static_cast<std::size_t>(channels)) {
        SetError(error, "invalid channel reorder configuration");
        return false;
    }
    reordered->resize(static_cast<std::size_t>(samples) * static_cast<std::size_t>(channels) * 2);
    for (int sample = 0; sample < samples; ++sample) {
        for (int output_channel = 0; output_channel < channels; ++output_channel) {
            const int input_channel = output_channel_to_input[static_cast<std::size_t>(output_channel)];
            const std::size_t source_offset =
                (static_cast<std::size_t>(sample) * static_cast<std::size_t>(channels) +
                 static_cast<std::size_t>(input_channel)) * 2;
            const std::size_t output_offset =
                (static_cast<std::size_t>(sample) * static_cast<std::size_t>(channels) +
                 static_cast<std::size_t>(output_channel)) * 2;
            (*reordered)[output_offset] = source[source_offset];
            (*reordered)[output_offset + 1] = source[source_offset + 1];
        }
    }
    return true;
}

}  // namespace

bool AudioPreparationService::PrepareToWav(const AudioPreparationOptions& options, PreparedAudioInfo* info,
                                            std::string* error) {
    IAMF_MUX_LOG_INFO(
        "[AudioPreparation] starting: input={} requested_stream={} output={} output_sample_rate={} preserve_unspecified_channel_order={} reorder_channels={}",
        options.input_path.u8string(), options.stream_index, options.output_wav_path.u8string(),
        options.output_sample_rate, options.preserve_unspecified_channel_order,
        options.output_channel_to_input.size());
    if (info == nullptr || options.input_path.empty() || options.output_wav_path.empty()) {
        SetError(error, "audio preparation requires input, output, and result arguments");
        return false;
    }
    if (options.output_sample_rate <= 0 || options.output_sample_rate > 384000) {
        SetError(error, "invalid output sample rate");
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
        "[AudioPreparation] selected stream: index={} codec={} sample_rate={} channels={} sample_format={} channel_layout={} time_base={}/{} bitrate={}",
        stream_index, codec->name, decoder->sample_rate, decoder->ch_layout.nb_channels,
        av_get_sample_fmt_name(decoder->sample_fmt) == nullptr
            ? "unknown"
            : av_get_sample_fmt_name(decoder->sample_fmt),
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
    status = swr_alloc_set_opts2(&raw_swr, &output_layout, AV_SAMPLE_FMT_S16, options.output_sample_rate,
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
    if (!output || !WriteWavHeader(&output, options.output_sample_rate, output_channels, 0)) {
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
    uint64_t data_bytes = 0;
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
            converted.resize(static_cast<size_t>(output_capacity) * static_cast<size_t>(output_channels) * 2);
            uint8_t* output_data[1] = {converted.data()};
            const int converted_samples = swr_convert(resampler.get(), output_data, output_capacity,
                                                       const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
            if (converted_samples < 0) {
                SetError(error, "failed while converting audio samples: " + AvError(converted_samples));
                return false;
            }
            const uint8_t* samples_to_write = converted.data();
            if (!output_channel_to_input.empty()) {
                if (!ReorderInterleavedSamples(converted.data(), converted_samples, output_channels,
                                               output_channel_to_input, &reordered, error))
                    return false;
                samples_to_write = reordered.data();
            }
            if (!WriteSamples(&output, samples_to_write, converted_samples, output_channels, &data_bytes, error))
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
        converted.resize(static_cast<size_t>(flushed_capacity) * static_cast<size_t>(output_channels) * 2);
        uint8_t* output_data[1] = {converted.data()};
        const int flushed_samples = swr_convert(resampler.get(), output_data, flushed_capacity, nullptr, 0);
        if (flushed_samples < 0) {
            SetError(error, "failed while flushing converted audio samples: " + AvError(flushed_samples));
            return false;
        }
        const uint8_t* samples_to_write = converted.data();
        if (!output_channel_to_input.empty()) {
            if (!ReorderInterleavedSamples(converted.data(), flushed_samples, output_channels,
                                           output_channel_to_input, &reordered, error))
                return false;
            samples_to_write = reordered.data();
        }
        if (!WriteSamples(&output, samples_to_write, flushed_samples, output_channels, &data_bytes, error))
            return false;
    }
    if (!output.seekp(4, std::ios::beg) || !WriteU32(&output, 36u + static_cast<uint32_t>(data_bytes)) ||
        !output.seekp(40, std::ios::beg) || !WriteU32(&output, static_cast<uint32_t>(data_bytes))) {
        SetError(error, "failed to finalize prepared WAV header");
        return false;
    }
    output.close();
    info->wav_path = options.output_wav_path;
    const uint64_t sample_count = data_bytes / (static_cast<uint64_t>(output_channels) * 2);
    const double duration_seconds = static_cast<double>(sample_count) / options.output_sample_rate;
    IAMF_MUX_LOG_INFO(
        "[AudioPreparation] completed: input={} stream={} output={} output_sample_rate={} channels={} sample_format=s16 samples={} duration_seconds={:.3f} data_bytes={}",
        options.input_path.u8string(), stream_index, options.output_wav_path.u8string(),
        options.output_sample_rate, output_channels, sample_count, duration_seconds, data_bytes);
    return true;
}

}  // namespace immersive_audio_muxer::app
