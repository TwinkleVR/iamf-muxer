#include "iamf_mp4_file_writer.h"

#include "iamf_box_writer.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <limits>
#include <vector>

namespace immersive_audio_muxer {
namespace {
void U16(std::vector<uint8_t>* b, uint16_t v) {
    b->push_back(static_cast<uint8_t>(v >> 8));
    b->push_back(static_cast<uint8_t>(v));
}
void U32(std::vector<uint8_t>* b, uint32_t v) {
    b->push_back(static_cast<uint8_t>(v >> 24));
    b->push_back(static_cast<uint8_t>(v >> 16));
    b->push_back(static_cast<uint8_t>(v >> 8));
    b->push_back(static_cast<uint8_t>(v));
}
void U64(std::vector<uint8_t>* b, uint64_t v) {
    U32(b, static_cast<uint32_t>(v >> 32));
    U32(b, static_cast<uint32_t>(v));
}
void Type(std::vector<uint8_t>* b, const char* type) {
    b->insert(b->end(), type, type + 4);
}
void Append(std::vector<uint8_t>* dst, const std::vector<uint8_t>& src) {
    dst->insert(dst->end(), src.begin(), src.end());
}
bool CheckedAdd(uint64_t value, uint64_t* total) {
    if (total == nullptr || value > std::numeric_limits<uint64_t>::max() - *total)
        return false;
    *total += value;
    return true;
}
std::vector<uint8_t> Box(const char* type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> result;
    if (payload.size() > std::numeric_limits<uint32_t>::max() - 8)
        return {};
    U32(&result, static_cast<uint32_t>(payload.size() + 8));
    Type(&result, type);
    Append(&result, payload);
    return result;
}
std::vector<uint8_t> FullBox(const char* type, uint8_t version, uint32_t flags, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> full;
    full.push_back(version);
    full.push_back(static_cast<uint8_t>(flags >> 16));
    full.push_back(static_cast<uint8_t>(flags >> 8));
    full.push_back(static_cast<uint8_t>(flags));
    Append(&full, payload);
    return Box(type, full);
}
void Matrix(std::vector<uint8_t>* b) {
    U32(b, 0x00010000);
    U32(b, 0);
    U32(b, 0);
    U32(b, 0);
    U32(b, 0x00010000);
    U32(b, 0);
    U32(b, 0);
    U32(b, 0);
    U32(b, 0x40000000);
}
std::vector<uint8_t> Mvhd(uint64_t duration, uint32_t next_track_id) {
    std::vector<uint8_t> p;
    U32(&p, 0);
    U32(&p, 0);
    U32(&p, 1000);
    U32(&p, static_cast<uint32_t>(std::min<uint64_t>(duration, 0xffffffffu)));
    U32(&p, 0x00010000);
    U16(&p, 0x0100);
    U16(&p, 0);
    p.insert(p.end(), 8, 0);
    Matrix(&p);
    p.insert(p.end(), 24, 0);
    U32(&p, next_track_id);
    return FullBox("mvhd", 0, 0, p);
}
std::vector<uint8_t> Tkhd(uint32_t track_id, uint64_t duration, uint32_t width, uint32_t height, uint16_t volume) {
    std::vector<uint8_t> p;
    U32(&p, 0);  // creation_time
    U32(&p, 0);  // modification_time
    U32(&p, track_id);
    U32(&p, 0);  // reserved
    U32(&p, static_cast<uint32_t>(std::min<uint64_t>(duration, 0xffffffffu)));
    p.insert(p.end(), 8, 0);
    U16(&p, volume);
    U16(&p, 0);
    U16(&p, 0);
    U16(&p, 0);
    Matrix(&p);
    U32(&p, width << 16);
    U32(&p, height << 16);
    return FullBox("tkhd", 0, 3, p);
}
std::vector<uint8_t> Mdhd(uint32_t timescale, uint64_t duration) {
    std::vector<uint8_t> p;
    U32(&p, 0);
    U32(&p, 0);
    U32(&p, timescale);
    U32(&p, static_cast<uint32_t>(std::min<uint64_t>(duration, 0xffffffffu)));
    U16(&p, 0x55c4);
    U16(&p, 0);
    return FullBox("mdhd", 0, 0, p);
}
std::vector<uint8_t> Handler(const char* type, const char* name) {
    std::vector<uint8_t> p;
    U32(&p, 0);
    Type(&p, type);
    p.insert(p.end(), 12, 0);
    p.insert(p.end(), name, name + std::strlen(name) + 1);
    return FullBox("hdlr", 0, 0, p);
}
std::vector<uint8_t> Dinf() {
    std::vector<uint8_t> dref_payload;
    U32(&dref_payload, 1);
    Append(&dref_payload, FullBox("url ", 0, 1, {}));
    return Box("dinf", FullBox("dref", 0, 0, dref_payload));
}

// iamf-tools CodecConfig::CodecId values are IAMF codec FourCCs.
constexpr uint32_t kCodecOpus = 0x4f707573;
constexpr uint32_t kCodecMp4a = 0x6d703461;

void AppendRollSampleGroups(std::vector<uint8_t>* stbl, uint32_t codec_id, int32_t roll_distance, uint32_t sample_count) {
    if (stbl == nullptr || sample_count == 0 || (codec_id != kCodecOpus && codec_id != kCodecMp4a)) {
        return;
    }
    const int16_t roll = static_cast<int16_t>(
        std::max<int32_t>(std::numeric_limits<int16_t>::min(), std::min<int32_t>(std::numeric_limits<int16_t>::max(), roll_distance)));
    std::vector<uint8_t> sgpd;
    Type(&sgpd, "roll");
    U32(&sgpd, 2);
    U32(&sgpd, 1);
    U16(&sgpd, static_cast<uint16_t>(roll));
    Append(stbl, FullBox("sgpd", 1, 0, sgpd));

    std::vector<uint8_t> sbgp;
    Type(&sbgp, "roll");
    U32(&sbgp, 1);
    U32(&sbgp, sample_count);
    U32(&sbgp, 1);
    Append(stbl, FullBox("sbgp", 0, 0, sbgp));
}

void AppendChunkTables(std::vector<uint8_t>* stbl, uint64_t fallback_offset, size_t sample_count,
                       const std::vector<uint64_t>* chunk_offsets) {
    const bool per_sample = chunk_offsets != nullptr && chunk_offsets->size() == sample_count && sample_count != 0;
    std::vector<uint8_t> stsc;
    U32(&stsc, 1);
    U32(&stsc, 1);
    U32(&stsc, per_sample ? 1 : static_cast<uint32_t>(sample_count));
    U32(&stsc, 1);
    Append(stbl, FullBox("stsc", 0, 0, stsc));

    if (per_sample) {
        const bool use_co64 = std::any_of(chunk_offsets->begin(), chunk_offsets->end(),
                                          [](uint64_t offset) { return offset > std::numeric_limits<uint32_t>::max(); });
        std::vector<uint8_t> offsets;
        U32(&offsets, static_cast<uint32_t>(chunk_offsets->size()));
        for (uint64_t offset : *chunk_offsets) {
            if (use_co64)
                U64(&offsets, offset);
            else
                U32(&offsets, static_cast<uint32_t>(offset));
        }
        Append(stbl, FullBox(use_co64 ? "co64" : "stco", 0, 0, offsets));
        return;
    }

    if (fallback_offset > std::numeric_limits<uint32_t>::max()) {
        std::vector<uint8_t> co64;
        U32(&co64, 1);
        U64(&co64, fallback_offset);
        Append(stbl, FullBox("co64", 0, 0, co64));
    } else {
        std::vector<uint8_t> stco;
        U32(&stco, 1);
        U32(&stco, static_cast<uint32_t>(fallback_offset));
        Append(stbl, FullBox("stco", 0, 0, stco));
    }
}
struct IamfTrimInfo {
    uint64_t total_samples = 0;
    uint64_t trim_start = 0;
    uint64_t trim_end = 0;
};

bool GetIamfTrimInfo(const StandaloneIamfReader& iamf, IamfTrimInfo* info, std::string* error) {
    if (info == nullptr || iamf.temporal_units().empty() || iamf.summary().sample_rate == 0) {
        if (error)
            *error = "IAMF trim metadata is unavailable";
        return false;
    }
    info->total_samples = 0;
    info->trim_start = 0;
    info->trim_end = 0;
    for (size_t i = 0; i < iamf.temporal_units().size(); ++i) {
        const auto& unit = iamf.temporal_units()[i];
        const auto& entry = unit.index;
        if (entry.num_samples > std::numeric_limits<uint32_t>::max() || unit.index.byte_size > std::numeric_limits<uint32_t>::max()) {
            if (error)
                *error = "IAMF sample exceeds current MP4 32-bit table limits";
            return false;
        }
        if (entry.num_samples > std::numeric_limits<uint64_t>::max() - info->total_samples) {
            if (error)
                *error = "IAMF duration overflow";
            return false;
        }
        if (static_cast<uint64_t>(entry.trim_at_start) + entry.trim_at_end > entry.num_samples) {
            if (error)
                *error = "IAMF trim exceeds temporal unit sample duration";
            return false;
        }
        info->total_samples += entry.num_samples;
    }

    // An encoder may distribute codec delay across several leading temporal
    // units. MP4 represents this as one aggregate empty edit, so accept a
    // contiguous leading trim region and preserve the original IAMF samples.
    bool in_leading_trim_region = true;
    for (const auto& unit : iamf.temporal_units()) {
        const uint64_t trim_at_start = unit.index.trim_at_start;
        if (trim_at_start != 0) {
            if (!in_leading_trim_region || trim_at_start > std::numeric_limits<uint64_t>::max() - info->trim_start) {
                if (error) {
                    *error = "IAMF start trim is present outside the leading boundary region";
                }
                return false;
            }
            info->trim_start += trim_at_start;
        } else {
            in_leading_trim_region = false;
        }
    }

    // Likewise, allow a contiguous trailing trim region and aggregate it into
    // the media duration used by the MP4 edit list.
    bool in_trailing_trim_region = true;
    for (auto it = iamf.temporal_units().rbegin(); it != iamf.temporal_units().rend(); ++it) {
        const uint64_t trim_at_end = it->index.trim_at_end;
        if (trim_at_end != 0) {
            if (!in_trailing_trim_region || trim_at_end > std::numeric_limits<uint64_t>::max() - info->trim_end) {
                if (error) {
                    *error = "IAMF end trim is present outside the trailing boundary region";
                }
                return false;
            }
            info->trim_end += trim_at_end;
        } else {
            in_trailing_trim_region = false;
        }
    }
    if (info->total_samples > std::numeric_limits<uint32_t>::max() || info->trim_start > info->total_samples ||
        info->trim_end > info->total_samples - info->trim_start) {
        if (error)
            *error = "IAMF trim exceeds the indexed sample duration";
        return false;
    }
    return true;
}

uint64_t SamplesToMovieTimeScale(uint64_t samples, uint32_t sample_rate, uint32_t movie_timescale) {
    if (sample_rate == 0 || movie_timescale == 0)
        return 0;
    const __int128 numerator = static_cast<__int128>(samples) * movie_timescale + sample_rate / 2;
    const __int128 scaled = numerator / sample_rate;
    return scaled > std::numeric_limits<uint64_t>::max() ? std::numeric_limits<uint64_t>::max()
                                                         : static_cast<uint64_t>(scaled);
}

std::vector<uint8_t> EditList(const IamfTrimInfo& trim, uint32_t sample_rate, uint32_t movie_timescale = 1000) {
    if (trim.trim_start == 0 && trim.trim_end == 0)
        return {};
    std::vector<uint8_t> entries;
    uint32_t entry_count = 0;
    const uint64_t start_duration = SamplesToMovieTimeScale(trim.trim_start, sample_rate, movie_timescale);
    const uint64_t media_duration = SamplesToMovieTimeScale(trim.total_samples - trim.trim_start - trim.trim_end,
                                                             sample_rate, movie_timescale);
    if (start_duration != 0) {
        U32(&entries, static_cast<uint32_t>(std::min<uint64_t>(start_duration, 0xffffffffu)));
        U32(&entries, 0xffffffffu);
        U32(&entries, 0x00010000);
        ++entry_count;
    }
    U32(&entries, static_cast<uint32_t>(std::min<uint64_t>(media_duration, 0xffffffffu)));
    U32(&entries, static_cast<uint32_t>(std::min<uint64_t>(trim.trim_start, 0x7fffffff)));
    U32(&entries, 0x00010000);
    ++entry_count;

    std::vector<uint8_t> payload;
    U32(&payload, entry_count);
    Append(&payload, entries);
    return Box("edts", FullBox("elst", 0, 0, payload));
}

std::vector<uint8_t> BuildIamfAudioTrack(const StandaloneIamfReader& iamf, const std::vector<uint8_t>& entry, uint32_t track_id,
                                         uint64_t data_offset,
                                         std::string* error, const std::vector<uint64_t>* chunk_offsets = nullptr,
                                         uint32_t movie_timescale = 1000) {
    IamfTrimInfo trim;
    if (!GetIamfTrimInfo(iamf, &trim, error))
        return {};
    const auto& units = iamf.temporal_units();
    std::vector<uint8_t> stbl;
    std::vector<uint8_t> stsd;
    U32(&stsd, 1);
    Append(&stsd, entry);
    Append(&stbl, FullBox("stsd", 0, 0, stsd));
    std::vector<uint8_t> stts;
    U32(&stts, units.size());
    for (const auto& unit : units) {
        U32(&stts, 1);
        U32(&stts, unit.index.num_samples);
    }
    Append(&stbl, FullBox("stts", 0, 0, stts));
    AppendChunkTables(&stbl, data_offset, units.size(), chunk_offsets);
    std::vector<uint8_t> stsz;
    U32(&stsz, 0);
    U32(&stsz, units.size());
    for (const auto& unit : units) U32(&stsz, unit.index.byte_size);
    Append(&stbl, FullBox("stsz", 0, 0, stsz));
    AppendRollSampleGroups(&stbl, iamf.codec_id(), iamf.audio_roll_distance(), static_cast<uint32_t>(units.size()));
    stbl = Box("stbl", stbl);

    std::vector<uint8_t> minf;
    Append(&minf, FullBox("smhd", 0, 0, {0, 0, 0, 0}));
    Append(&minf, Dinf());
    Append(&minf, stbl);
    minf = Box("minf", minf);
    std::vector<uint8_t> mdia;
    Append(&mdia, Mdhd(iamf.summary().sample_rate, trim.total_samples));
    Append(&mdia, Handler("soun", "IAMF SoundHandler"));
    Append(&mdia, minf);
    mdia = Box("mdia", mdia);
    std::vector<uint8_t> trak;
    Append(&trak, Tkhd(track_id,
                       SamplesToMovieTimeScale(trim.total_samples - trim.trim_end, iamf.summary().sample_rate,
                                               movie_timescale),
                       0, 0, 0x0100));
    if (auto edit = EditList(trim, iamf.summary().sample_rate, movie_timescale); !edit.empty()) {
        Append(&trak, edit);
    }
    Append(&trak, mdia);
    return Box("trak", trak);
}
std::vector<uint8_t> Ftyp() {
    std::vector<uint8_t> ftyp;
    U32(&ftyp, 0x1c);
    Type(&ftyp, "ftyp");
    Type(&ftyp, "isom");
    U32(&ftyp, 0x200);
    Type(&ftyp, "isom");
    Type(&ftyp, "iso6");
    Type(&ftyp, "iamf");
    return ftyp;
}

bool WriteIamfOnlyFile(const std::string& path, const std::vector<uint8_t>& ftyp,
                       const std::vector<uint8_t>& moov, const StandaloneIamfReader& iamf,
                       std::string* error, const std::function<bool()>& cancellation_requested) {
    uint64_t mdat_size = 8;
    if (!CheckedAdd(iamf.summary().temporal_unit_bytes, &mdat_size) ||
        mdat_size > std::numeric_limits<uint64_t>::max() - 8) {
        if (error)
            *error = "output mdat size overflow";
        return false;
    }

    std::ofstream output(std::filesystem::u8path(path), std::ios::binary);
    if (!output) {
        if (error)
            *error = "cannot create output MP4";
        return false;
    }
    output.write(reinterpret_cast<const char*>(ftyp.data()), static_cast<std::streamsize>(ftyp.size()));
    output.write(reinterpret_cast<const char*>(moov.data()), static_cast<std::streamsize>(moov.size()));
    if (mdat_size <= std::numeric_limits<uint32_t>::max()) {
        std::vector<uint8_t> header;
        U32(&header, static_cast<uint32_t>(mdat_size));
        Type(&header, "mdat");
        output.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    } else {
        std::vector<uint8_t> header;
        U32(&header, 1);
        Type(&header, "mdat");
        U64(&header, mdat_size + 8);
        output.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    }
    if (!output) {
        if (error)
            *error = "failed to write MP4 header";
        return false;
    }

    std::vector<uint8_t> chunk;
    for (size_t index = 0; index < iamf.temporal_units().size(); ++index) {
        if (cancellation_requested && cancellation_requested()) {
            if (error)
                *error = "operation cancelled while copying IAMF samples";
            return false;
        }
        if (!iamf.ReadTemporalUnit(index, &chunk, error))
            return false;
        output.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
    }
    if (!output) {
        if (error)
            *error = "failed to write IAMF payload";
        return false;
    }
    return true;
}
}  // namespace

bool IamfMp4FileWriter::WriteIamfOnly(const std::string& output_path, const StandaloneIamfReader& iamf, std::string* error,
                                      const std::function<bool()>& cancellation_requested) {
    if (iamf.temporal_units().empty() || iamf.summary().sample_rate == 0) {
        if (error)
            *error = "IAMF track has no samples or sample rate";
        return false;
    }
    IamfTrimInfo trim;
    if (!GetIamfTrimInfo(iamf, &trim, error))
        return false;
    std::vector<uint8_t> config, entry;
    if (!IamfBoxWriter::BuildConfigurationBox(iamf.descriptor_bytes(), &config, error) ||
        !IamfBoxWriter::BuildSampleEntry(config, &entry, error))
        return false;

    const std::vector<uint8_t> ftyp = Ftyp();
    std::vector<uint8_t> moov;
    uint64_t data_offset = 0;
    for (int iteration = 0; iteration < 4; ++iteration) {
        const auto track = BuildIamfAudioTrack(iamf, entry, 2, data_offset, error);
        if (track.empty())
            return false;
        std::vector<uint8_t> payload;
        Append(&payload, Mvhd(SamplesToMovieTimeScale(trim.total_samples - trim.trim_end,
                                                      iamf.summary().sample_rate, 1000), 3));
        Append(&payload, track);
        moov = Box("moov", payload);
        const uint64_t header_size = iamf.summary().temporal_unit_bytes + 8 <= std::numeric_limits<uint32_t>::max() ? 8 : 16;
        const uint64_t next_offset = ftyp.size() + moov.size() + header_size;
        if (next_offset == data_offset)
            break;
        data_offset = next_offset;
    }
    return WriteIamfOnlyFile(output_path, ftyp, moov, iamf, error, cancellation_requested);
}

bool IamfMp4FileWriter::BuildIamfTrackBoxWithSampleOffsets(
    const StandaloneIamfReader& iamf, uint32_t track_id, const std::vector<uint64_t>& sample_offsets,
    std::vector<uint8_t>* track_box, std::string* error, uint32_t movie_timescale) {
    if (track_box == nullptr || track_id == 0 || sample_offsets.size() != iamf.temporal_units().size()) {
        if (error)
            *error = "IAMF sample-offset table does not match the temporal-unit count";
        return false;
    }
    std::vector<uint8_t> configuration_box;
    std::vector<uint8_t> sample_entry;
    if (!IamfBoxWriter::BuildConfigurationBox(iamf.descriptor_bytes(), &configuration_box, error) ||
        !IamfBoxWriter::BuildSampleEntry(configuration_box, &sample_entry, error))
        return false;
    *track_box = BuildIamfAudioTrack(iamf, sample_entry, track_id, 0, error, &sample_offsets, movie_timescale);
    if (track_box->empty())
        return false;
    return true;
}

}  // namespace immersive_audio_muxer
