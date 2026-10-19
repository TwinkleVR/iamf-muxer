#pragma once

#include <functional>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
}

namespace immersive_audio_muxer::avformat_input {

// Installs the process-wide FFmpeg callback once and routes its messages to
// the runtime log while preserving the existing default output behavior.
void InitializeFfmpegLogging();

// Opens a media file and reads its stream metadata. If the normal open fails,
// an MP4/MOV file is retried through a same-size in-memory view with IAMF audio
// tracks hidden. The source file is never modified or copied.
int Open(AVFormatContext** context, const std::string& path,
         const std::function<bool()>& cancellation_requested = {},
         bool* audio_tracks_suppressed = nullptr, bool force_suppress_audio_tracks = false,
         bool find_stream_info = true);

// Closes a context returned by Open, including any custom AVIO state used for
// the IAMF-suppressed MP4/MOV view.
void Close(AVFormatContext** context);

}  // namespace immersive_audio_muxer::avformat_input
