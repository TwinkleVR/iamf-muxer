#include "avformat_input.h"

#include "runtime_logging.h"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace immersive_audio_muxer::avformat_input {
namespace {

constexpr int kIoBufferSize = 32 * 1024;
constexpr uint64_t kMaxMovieBoxSize = 256ULL * 1024ULL * 1024ULL;
std::once_flag g_install_log_callback;
thread_local bool g_suppress_ffmpeg_logs = false;

void FilterFfmpegLog(void* context, int level, const char* format, va_list arguments) {
    try {
        std::array<char, 8192> message_buffer{};
        va_list file_arguments;
        va_copy(file_arguments, arguments);
        int prefix = 1;
        const int formatted_size = av_log_format_line2(context, level, format, file_arguments,
                                                       message_buffer.data(),
                                                       static_cast<int>(message_buffer.size()), &prefix);
        va_end(file_arguments);

        if (formatted_size >= 0) {
            std::string message(message_buffer.data());
            while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
                message.pop_back();
            if (formatted_size >= static_cast<int>(message_buffer.size() - 1))
                message += " [... FFmpeg log line truncated]";
            const char* context_name = context == nullptr ? "global" : av_default_item_name(context);
            if (auto logger = logging::GetLogger()) {
                spdlog::level::level_enum log_level = spdlog::level::trace;
                if (level <= AV_LOG_ERROR)
                    log_level = spdlog::level::err;
                else if (level <= AV_LOG_WARNING)
                    log_level = spdlog::level::warn;
                else if (level <= AV_LOG_INFO)
                    log_level = spdlog::level::info;
                else if (level <= AV_LOG_DEBUG)
                    log_level = spdlog::level::debug;
                logger->log(spdlog::source_loc{__FILE__, __LINE__, __func__}, log_level,
                            "[FFmpeg][{}] {}", context_name, message);
            }
        }
    } catch (...) {
    }

    if (!g_suppress_ffmpeg_logs)
        av_log_default_callback(context, level, format, arguments);
}

class QuietFfmpegLogs final {
public:
    explicit QuietFfmpegLogs(bool suppress) : previous_suppression_(g_suppress_ffmpeg_logs) {
        InitializeFfmpegLogging();
        if (suppress) {
            g_suppress_ffmpeg_logs = true;
        }
    }
    ~QuietFfmpegLogs() { g_suppress_ffmpeg_logs = previous_suppression_; }

    QuietFfmpegLogs(const QuietFfmpegLogs&) = delete;
    QuietFfmpegLogs& operator=(const QuietFfmpegLogs&) = delete;

private:
    bool previous_suppression_;
};

struct Box {
    size_t offset = 0;
    size_t size = 0;
    size_t header_size = 0;
    std::array<char, 4> type{};
};

struct Mp4View {
    std::ifstream source;
    int64_t file_size = 0;
    int64_t position = 0;
    int64_t movie_offset = 0;
    std::vector<uint8_t> movie_box;
};

bool IsType(const Box& box, const char (&type)[5]) {
    return std::memcmp(box.type.data(), type, 4) == 0;
}

bool ReadExactAt(std::ifstream* input, uint64_t offset, uint8_t* destination, size_t size) {
    if (input == nullptr || destination == nullptr || offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()))
        return false;
    input->clear();
    input->seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!*input)
        return false;
    input->read(reinterpret_cast<char*>(destination), static_cast<std::streamsize>(size));
    return input->gcount() == static_cast<std::streamsize>(size);
}

bool ReadBox(const std::vector<uint8_t>& data, size_t limit, size_t* cursor, Box* box) {
    if (cursor == nullptr || box == nullptr || *cursor > limit || limit - *cursor < 8)
        return false;
    const size_t start = *cursor;
    const uint32_t short_size = AV_RB32(data.data() + start);
    std::memcpy(box->type.data(), data.data() + start + 4, 4);
    uint64_t box_size = short_size;
    size_t header_size = 8;
    if (short_size == 1) {
        if (limit - start < 16)
            return false;
        box_size = AV_RB64(data.data() + start + 8);
        header_size = 16;
    } else if (short_size == 0) {
        box_size = limit - start;
    }
    if (box_size < header_size || box_size > limit - start || box_size > std::numeric_limits<size_t>::max())
        return false;
    box->offset = start;
    box->size = static_cast<size_t>(box_size);
    box->header_size = header_size;
    *cursor = start + box->size;
    return true;
}

bool FindChild(const std::vector<uint8_t>& data, const Box& parent,
               const char (&type)[5], Box* result) {
    if (result == nullptr || parent.offset > data.size() ||
        parent.size > data.size() - parent.offset)
        return false;
    const size_t end = parent.offset + parent.size;
    size_t cursor = parent.offset + parent.header_size;
    while (cursor < end) {
        Box child;
        if (!ReadBox(data, end, &cursor, &child))
            return false;
        if (IsType(child, type)) {
            *result = child;
            return true;
        }
    }
    return false;
}

bool HasAudioHandler(const std::vector<uint8_t>& movie, const Box& track) {
    const size_t track_end = track.offset + track.size;
    size_t track_cursor = track.offset + track.header_size;
    Box child;
    while (track_cursor < track_end) {
        if (!ReadBox(movie, track_end, &track_cursor, &child))
            return false;
        if (!IsType(child, "mdia"))
            continue;
        const size_t media_end = child.offset + child.size;
        size_t media_cursor = child.offset + child.header_size;
        Box media_child;
        while (media_cursor < media_end) {
            if (!ReadBox(movie, media_end, &media_cursor, &media_child))
                return false;
            if (!IsType(media_child, "hdlr"))
                continue;
            const size_t handler_offset = media_child.offset + media_child.header_size;
            // hdlr contains version/flags, pre_defined, then handler_type.
            if (media_child.size < media_child.header_size + 12)
                return false;
            return std::memcmp(movie.data() + handler_offset + 8, "soun", 4) == 0;
        }
    }
    return false;
}

bool HasIamfSampleEntry(const std::vector<uint8_t>& movie, const Box& track) {
    Box mdia, minf, stbl, stsd;
    if (!FindChild(movie, track, "mdia", &mdia) ||
        !FindChild(movie, mdia, "minf", &minf) ||
        !FindChild(movie, minf, "stbl", &stbl) ||
        !FindChild(movie, stbl, "stsd", &stsd) ||
        stsd.size < stsd.header_size + 8)
        return false;
    const size_t body = stsd.offset + stsd.header_size;
    const uint32_t entry_count = AV_RB32(movie.data() + body + 4);
    size_t cursor = body + 8;
    const size_t end = stsd.offset + stsd.size;
    for (uint32_t index = 0; index < entry_count; ++index) {
        Box entry;
        if (!ReadBox(movie, end, &cursor, &entry))
            return false;
        if (IsType(entry, "iamf"))
            return true;
    }
    return false;
}

bool HideIamfAudioTracks(std::vector<uint8_t>* movie, size_t* count) {
    if (movie == nullptr || count == nullptr || movie->size() < 8)
        return false;
    Box root;
    size_t root_cursor = 0;
    if (!ReadBox(*movie, movie->size(), &root_cursor, &root) || !IsType(root, "moov") || root.size != movie->size())
        return false;

    size_t found = 0;
    size_t cursor = root.header_size;
    Box child;
    while (cursor < root.size) {
        if (!ReadBox(*movie, root.size, &cursor, &child))
            return false;
        if (IsType(child, "trak") && HasAudioHandler(*movie, child) &&
            HasIamfSampleEntry(*movie, child)) {
            std::memcpy(movie->data() + child.offset + 4, "free", 4);
            ++found;
        }
    }
    *count = found;
    return found > 0;
}

std::unique_ptr<Mp4View> MakeIamfSuppressedView(const std::string& path) {
    std::unique_ptr<Mp4View> view(new (std::nothrow) Mp4View());
    if (!view)
        return {};
    const std::filesystem::path filesystem_path = std::filesystem::u8path(path);
    view->source.open(filesystem_path, std::ios::binary);
    if (!view->source)
        return {};
    view->source.seekg(0, std::ios::end);
    const std::streamoff end = view->source.tellg();
    if (end < 0 || static_cast<uint64_t>(end) > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        return {};
    view->file_size = static_cast<int64_t>(end);
    if (view->file_size < 16)
        return {};

    bool found_ftyp = false;
    bool found_moov = false;
    uint64_t offset = 0;
    while (offset + 8 <= static_cast<uint64_t>(view->file_size)) {
        std::array<uint8_t, 16> header{};
        if (!ReadExactAt(&view->source, offset, header.data(), 8))
            return {};
        const uint32_t short_size = AV_RB32(header.data());
        uint64_t box_size = short_size;
        uint64_t header_size = 8;
        if (short_size == 1) {
            if (offset + 16 > static_cast<uint64_t>(view->file_size) || !ReadExactAt(&view->source, offset, header.data(), 16))
                return {};
            box_size = AV_RB64(header.data() + 8);
            header_size = 16;
        } else if (short_size == 0) {
            box_size = static_cast<uint64_t>(view->file_size) - offset;
        }
        if (box_size < header_size || box_size > static_cast<uint64_t>(view->file_size) - offset)
            return {};
        const bool is_ftyp = std::memcmp(header.data() + 4, "ftyp", 4) == 0;
        const bool is_moov = std::memcmp(header.data() + 4, "moov", 4) == 0;
        found_ftyp = found_ftyp || is_ftyp;
        if (is_moov) {
            if (box_size > kMaxMovieBoxSize || box_size > std::numeric_limits<size_t>::max())
                return {};
            view->movie_box.resize(static_cast<size_t>(box_size));
            if (!ReadExactAt(&view->source, offset, view->movie_box.data(), view->movie_box.size()))
                return {};
            size_t hidden_tracks = 0;
            if (!HideIamfAudioTracks(&view->movie_box, &hidden_tracks))
                return {};
            view->movie_offset = static_cast<int64_t>(offset);
            found_moov = true;
            break;
        }
        offset += box_size;
    }
    if (!found_ftyp || !found_moov)
        return {};
    view->source.clear();
    return view;
}

int ReadPacket(void* opaque, uint8_t* buffer, int buffer_size) {
    auto* view = static_cast<Mp4View*>(opaque);
    if (view == nullptr || buffer == nullptr || buffer_size <= 0)
        return AVERROR(EINVAL);
    if (view->position >= view->file_size)
        return AVERROR_EOF;
    const int64_t available = view->file_size - view->position;
    const size_t requested = static_cast<size_t>(std::min<int64_t>(buffer_size, available));
    view->source.clear();
    view->source.seekg(static_cast<std::streamoff>(view->position), std::ios::beg);
    if (!view->source)
        return AVERROR(EIO);
    view->source.read(reinterpret_cast<char*>(buffer), static_cast<std::streamsize>(requested));
    const std::streamsize received = view->source.gcount();
    if (received <= 0)
        return AVERROR_EOF;

    const int64_t read_start = view->position;
    const int64_t read_end = read_start + received;
    const int64_t overlay_start = view->movie_offset;
    const int64_t overlay_end = overlay_start + static_cast<int64_t>(view->movie_box.size());
    const int64_t overlap_start = std::max(read_start, overlay_start);
    const int64_t overlap_end = std::min(read_end, overlay_end);
    if (overlap_start < overlap_end) {
        const size_t destination_offset = static_cast<size_t>(overlap_start - read_start);
        const size_t source_offset = static_cast<size_t>(overlap_start - overlay_start);
        const size_t overlap_size = static_cast<size_t>(overlap_end - overlap_start);
        std::memcpy(buffer + destination_offset, view->movie_box.data() + source_offset, overlap_size);
    }
    view->position = read_end;
    return static_cast<int>(received);
}

int64_t Seek(void* opaque, int64_t offset, int whence) {
    auto* view = static_cast<Mp4View*>(opaque);
    if (view == nullptr)
        return AVERROR(EINVAL);
    if ((whence & AVSEEK_SIZE) != 0)
        return view->file_size;
    whence &= ~AVSEEK_FORCE;
    int64_t base = 0;
    switch (whence) {
        case SEEK_SET:
            base = 0;
            break;
        case SEEK_CUR:
            base = view->position;
            break;
        case SEEK_END:
            base = view->file_size;
            break;
        default:
            return AVERROR(EINVAL);
    }
    if ((offset > 0 && base > std::numeric_limits<int64_t>::max() - offset) ||
        (offset < 0 && base < std::numeric_limits<int64_t>::min() - offset))
        return AVERROR(EINVAL);
    const int64_t target = base + offset;
    if (target < 0 || target > view->file_size)
        return AVERROR(EINVAL);
    view->position = target;
    return target;
}

void FreeCustomIo(AVIOContext** io, Mp4View* view) {
    if (io != nullptr && *io != nullptr) {
        av_freep(&(*io)->buffer);
        avio_context_free(io);
    }
    delete view;
}

void SetInterrupt(AVFormatContext* context, const std::function<bool()>& cancellation_requested) {
    if (context == nullptr || !cancellation_requested)
        return;
    context->interrupt_callback.callback = [](void* opaque) -> int {
        const auto* callback = static_cast<const std::function<bool()>*>(opaque);
        return callback != nullptr && *callback && (*callback)() ? 1 : 0;
    };
    context->interrupt_callback.opaque = const_cast<std::function<bool()>*>(&cancellation_requested);
}

bool IsMovContainerPath(const std::string& path) {
    std::string extension = std::filesystem::u8path(path).extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return extension == ".mp4" || extension == ".mov" || extension == ".m4v" || extension == ".m4a" ||
           extension == ".3gp" || extension == ".3g2" || extension == ".mj2";
}

int OpenOnce(AVFormatContext** context, const std::string& path,
             const std::function<bool()>& cancellation_requested, Mp4View* view,
             bool find_stream_info, AVIOContext** custom_io) {
    AVFormatContext* raw = avformat_alloc_context();
    if (raw == nullptr)
        return AVERROR(ENOMEM);
    SetInterrupt(raw, cancellation_requested);
    if (view != nullptr) {
        uint8_t* buffer = static_cast<uint8_t*>(av_malloc(kIoBufferSize));
        if (buffer == nullptr) {
            avformat_free_context(raw);
            return AVERROR(ENOMEM);
        }
        AVIOContext* io = avio_alloc_context(buffer, kIoBufferSize, 0, view, ReadPacket, nullptr, Seek);
        if (io == nullptr) {
            av_free(buffer);
            avformat_free_context(raw);
            return AVERROR(ENOMEM);
        }
        io->seekable = AVIO_SEEKABLE_NORMAL;
        raw->pb = io;
        raw->flags |= AVFMT_FLAG_CUSTOM_IO;
        if (custom_io != nullptr)
            *custom_io = io;
    }
    int status = avformat_open_input(&raw, path.c_str(), nullptr, nullptr);
    if (status >= 0 && find_stream_info)
        status = avformat_find_stream_info(raw, nullptr);
    if (status < 0) {
        if (raw != nullptr) {
            if (view != nullptr)
                avformat_close_input(&raw);
            else
                Close(&raw);
        }
        return status;
    }
    *context = raw;
    return 0;
}

}  // namespace

void InitializeFfmpegLogging() {
    std::call_once(g_install_log_callback, [] {
        av_log_set_level(AV_LOG_VERBOSE);
        av_log_set_callback(FilterFfmpegLog);
    });
}

int Open(AVFormatContext** context, const std::string& path,
         const std::function<bool()>& cancellation_requested,
         bool* audio_tracks_suppressed, bool force_suppress_audio_tracks,
         bool find_stream_info) {
    if (context == nullptr || path.empty())
        return AVERROR(EINVAL);
    QuietFfmpegLogs quiet_logs(IsMovContainerPath(path));
    *context = nullptr;
    if (audio_tracks_suppressed != nullptr)
        *audio_tracks_suppressed = false;

    int original_status = AVERROR_INVALIDDATA;
    if (!force_suppress_audio_tracks) {
        // FFmpeg has no per-context log callback. Keep expected IAMF parse
        // failures from leaking to the console while we retry without IAMF.
        original_status = OpenOnce(context, path, cancellation_requested, nullptr, find_stream_info, nullptr);
        if (original_status >= 0)
            return 0;
        if (cancellation_requested && cancellation_requested())
            return original_status;
    }

    std::unique_ptr<Mp4View> view = MakeIamfSuppressedView(path);
    if (!view)
        return original_status;
    Mp4View* view_raw = view.release();
    AVIOContext* custom_io = nullptr;
    const int fallback_status = OpenOnce(context, path, cancellation_requested, view_raw, find_stream_info, &custom_io);
    if (fallback_status >= 0) {
        if (audio_tracks_suppressed != nullptr)
            *audio_tracks_suppressed = true;
        return 0;
    }
    // OpenOnce closes a live format context on failure. If FFmpeg discarded
    // that context during avformat_open_input, custom IO remains ours to free.
    if (custom_io != nullptr)
        FreeCustomIo(&custom_io, view_raw);
    else
        delete view_raw;
    return force_suppress_audio_tracks ? fallback_status : original_status;
}

void Close(AVFormatContext** context) {
    if (context == nullptr || *context == nullptr)
        return;
    AVFormatContext* raw = *context;
    AVIOContext* custom_io = (raw->flags & AVFMT_FLAG_CUSTOM_IO) != 0 ? raw->pb : nullptr;
    Mp4View* view = custom_io == nullptr ? nullptr : static_cast<Mp4View*>(custom_io->opaque);
    avformat_close_input(context);
    if (custom_io != nullptr)
        FreeCustomIo(&custom_io, view);
}

}  // namespace immersive_audio_muxer::avformat_input
