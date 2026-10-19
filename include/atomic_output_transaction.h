#pragma once

#include <filesystem>
#include <string>

namespace immersive_audio_muxer {

// Owns a private, same-directory staging directory and commits one output
// file without deleting arbitrary stale sidecar names. The staging directory
// is created with create_directory(), which provides the exclusivity needed
// to avoid two jobs selecting the same temporary name.
class AtomicOutputTransaction final {
public:
    AtomicOutputTransaction() = default;
    ~AtomicOutputTransaction();

    AtomicOutputTransaction(const AtomicOutputTransaction&) = delete;
    AtomicOutputTransaction& operator=(const AtomicOutputTransaction&) = delete;
    AtomicOutputTransaction(AtomicOutputTransaction&& other) noexcept;
    AtomicOutputTransaction& operator=(AtomicOutputTransaction&& other) noexcept;

    static bool Create(const std::filesystem::path& destination, bool overwrite_existing,
                       AtomicOutputTransaction* transaction, std::string* error);

    const std::filesystem::path& destination_path() const { return destination_path_; }
    const std::filesystem::path& temporary_path() const { return temporary_path_; }
    bool Commit(std::string* error);
    void Abort() noexcept;
    bool valid() const { return !temporary_path_.empty(); }

private:
    void Reset() noexcept;

    std::filesystem::path destination_path_;
    std::filesystem::path staging_directory_;
    std::filesystem::path temporary_path_;
    std::filesystem::path backup_path_;
    bool overwrite_existing_ = false;
    bool destination_staged_ = false;
    bool committed_ = false;
};

}  // namespace immersive_audio_muxer
