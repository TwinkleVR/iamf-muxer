extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
}

#include <cstddef>
#include <cstring>
#include <iostream>

namespace {

template <std::size_t N>
bool CheckDecoders(const char* const (&names)[N]) {
    bool passed = true;
    for (const char* name : names) {
        if (avcodec_find_decoder_by_name(name) == nullptr) {
            std::cerr << "missing FFmpeg decoder: " << name << '\n';
            passed = false;
        }
    }
    return passed;
}

template <std::size_t N>
bool CheckDemuxers(const char* const (&names)[N]) {
    bool passed = true;
    for (const char* name : names) {
        if (av_find_input_format(name) == nullptr) {
            std::cerr << "missing FFmpeg demuxer: " << name << '\n';
            passed = false;
        }
    }
    return passed;
}

template <std::size_t N>
bool CheckParsers(const AVCodecID (&codec_ids)[N]) {
    bool passed = true;
    for (AVCodecID codec_id : codec_ids) {
        AVCodecParserContext* parser = av_parser_init(codec_id);
        if (parser == nullptr) {
            std::cerr << "missing FFmpeg parser for codec id "
                      << static_cast<int>(codec_id) << '\n';
            passed = false;
        } else {
            av_parser_close(parser);
        }
    }
    return passed;
}

bool CheckBitstreamFilter(const char* name) {
    if (av_bsf_get_by_name(name) != nullptr)
        return true;
    std::cerr << "missing FFmpeg bitstream filter: " << name << '\n';
    return false;
}

bool CheckMuxer(const char* name) {
    if (av_guess_format(name, nullptr, nullptr) != nullptr)
        return true;
    std::cerr << "missing FFmpeg muxer: " << name << '\n';
    return false;
}

bool CheckLicenseConfiguration() {
    const char* configuration = avcodec_configuration();
    if (configuration != nullptr &&
        std::strstr(configuration, "--enable-gpl") == nullptr &&
        std::strstr(configuration, "--enable-nonfree") == nullptr &&
        std::strstr(configuration, "--enable-version3") == nullptr) {
        return true;
    }
    std::cerr << "FFmpeg build unexpectedly enables a restricted license flag\n";
    return false;
}

}  // namespace

int main() {
    // These are public AVCodec names; several differ from configure component names.
    constexpr const char* kRequiredDecoders[] = {
        "aac", "aac_fixed", "aac_latm", "ac3", "ac3_fixed", "alac",
        "amrnb", "amrwb", "ape", "dca", "flac", "gsm", "gsm_ms",
        "mlp", "mp1", "mp1float", "mp2", "mp2float", "mp3", "mp3float",
        "mpc7", "mpc8", "opus", "speex", "tak", "truehd", "tta",
        "vorbis", "wavpack", "wmalossless", "wmapro", "wmav1", "wmav2",
        "wmavoice", "g722", "g726", "g726le",
        "adpcm_ima_qt", "adpcm_ima_wav", "adpcm_ms", "adpcm_yamaha",
        "pcm_alaw", "pcm_bluray", "pcm_dvd", "pcm_f16le", "pcm_f24le",
        "pcm_f32be", "pcm_f32le", "pcm_f64be", "pcm_f64le", "pcm_mulaw",
        "pcm_s8", "pcm_s8_planar", "pcm_s16be", "pcm_s16be_planar",
        "pcm_s16le", "pcm_s16le_planar", "pcm_s24be", "pcm_s24daud",
        "pcm_s24le", "pcm_s24le_planar", "pcm_s32be", "pcm_s32le",
        "pcm_s32le_planar", "pcm_s64be", "pcm_s64le", "pcm_u8",
        "pcm_u16be", "pcm_u16le", "pcm_u24be", "pcm_u24le", "pcm_u32be",
        "pcm_u32le", "mov_text"};
    constexpr const char* kRequiredDemuxers[] = {
        "aac", "ac3", "aiff", "amr", "ape", "asf", "avi", "caf", "dts",
        "dtshd", "eac3", "flac", "flv", "h264", "hevc", "ivf", "matroska",
        "mov", "mp4", "m4a", "webm", "mp3", "mpc", "mpc8", "mpeg",
        "mpegts", "mpegvideo", "mxf", "ogg", "tta", "wav", "w64", "wv", "m4v"};
    constexpr AVCodecID kRequiredParserCodecIds[] = {
        AV_CODEC_ID_AAC,       AV_CODEC_ID_AAC_LATM, AV_CODEC_ID_AC3,
        AV_CODEC_ID_EAC3,      AV_CODEC_ID_DTS,      AV_CODEC_ID_FLAC,
        AV_CODEC_ID_GSM,       AV_CODEC_ID_GSM_MS,   AV_CODEC_ID_MLP,
        AV_CODEC_ID_TRUEHD,    AV_CODEC_ID_MP1,      AV_CODEC_ID_MP2,
        AV_CODEC_ID_MP3,       AV_CODEC_ID_MP3ADU,   AV_CODEC_ID_OPUS,
        AV_CODEC_ID_TAK,       AV_CODEC_ID_VORBIS,   AV_CODEC_ID_AV1,
        AV_CODEC_ID_H264,      AV_CODEC_ID_HEVC,     AV_CODEC_ID_MPEG4,
        AV_CODEC_ID_MPEG1VIDEO, AV_CODEC_ID_MPEG2VIDEO, AV_CODEC_ID_VP8,
        AV_CODEC_ID_VP9};

    bool passed = CheckDecoders(kRequiredDecoders);
    passed = CheckDemuxers(kRequiredDemuxers) && passed;
    passed = CheckMuxer("mp4") && passed;
    passed = CheckParsers(kRequiredParserCodecIds) && passed;
    passed = CheckBitstreamFilter("text2movsub") && passed;
    passed = CheckLicenseConfiguration() && passed;
    if (!passed)
        return 1;

    std::cout << "FFmpeg component availability check passed\n";
    return 0;
}
