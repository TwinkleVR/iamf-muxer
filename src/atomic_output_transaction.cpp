#include "atomic_output_transaction.h"

#include "path_utils.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <system_error>
#include <utility>

namespace immersive_audio_muxer {
namespace {

std::filesystem::path AbsoluteNormalized(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(path, error);
    return (error ? path : absolute).lexically_normal();
}

std::string UniqueStem() {
    static std::atomic_uint64_t sequence{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return ".iamf_muxer-" + std::to_string(static_cast<std::uint64_t>(now)) + "-" +
           std::to_string(sequence.fetch_add(1, std::memory_order_relaxed) + 1);
}

void SetError(std::string* error, const std::string& message) {
    if (error)
        *error = message;
}

}  // namespace

AtomicOutputTransaction::~AtomicOutputTransaction() {
    Abort();
}

AtomicOutputTransaction::AtomicOutputTransaction(AtomicOutputTransaction&& other) noexcept
    : destination_path_(std::move(other.destination_path_)),
      staging_directory_(std::move(other.staging_directory_)),
      temporary_path_(std::move(other.temporary_path_)),
      backup_path_(std::move(other.backup_path_)),
      overwrite_existing_(other.overwrite_existing_),
      destination_staged_(other.destination_staged_),
      committed_(other.committed_) {
    other.Reset();
}

AtomicOutputTransaction& AtomicOutputTransaction::operator=(AtomicOutputTransaction&& other) noexcept {
    if (this == &other)
        return *this;
    Abort();
    destination_path_ = std::move(other.destination_path_);
    staging_directory_ = std::move(other.staging_directory_);
    temporary_path_ = std::move(other.temporary_path_);
    backup_path_ = std::move(other.backup_path_);
    overwrite_existing_ = other.overwrite_existing_;
    destination_staged_ = other.destination_staged_;
    committed_ = other.committed_;
    other.Reset();
    return *this;
}

bool AtomicOutputTransaction::Create(const std::filesystem::path& destination, bool overwrite_existing,
                                     AtomicOutputTransaction* transaction, std::string* error) {
    if (transaction == nullptr || destination.empty()) {
        SetError(error, "output transaction requires a destination path");
        return false;
    }
    transaction->Abort();
    const std::filesystem::path normalized_destination = AbsoluteNormalized(destination);
    const std::filesystem::path parent = normalized_destination.parent_path();
    if (parent.empty()) {
        SetError(error, "output transaction has no parent directory: " + path_utils::ToUtf8(normalized_destination));
        return false;
    }

    std::error_code filesystem_error;
    if (!std::filesystem::is_directory(parent, filesystem_error) || filesystem_error) {
        SetError(error, "output directory does not exist: " + path_utils::ToUtf8(parent));
        return false;
    }
    const bool destination_exists = std::filesystem::exists(normalized_destination, filesystem_error);
    if (filesystem_error) {
        SetError(error, "cannot inspect output path: " + filesystem_error.message());
        return false;
    }
    if (destination_exists && !overwrite_existing) {
        SetError(error, "output file already exists and overwrite is disabled");
        return false;
    }
    if (destination_exists && !std::filesystem::is_regular_file(normalized_destination, filesystem_error)) {
        SetError(error, "output path is not a regular file: " + path_utils::ToUtf8(normalized_destination));
        return false;
    }
    if (filesystem_error) {
        SetError(error, "cannot inspect output file type: " + filesystem_error.message());
        return false;
    }

    for (int attempt = 0; attempt < 64; ++attempt) {
        const std::filesystem::path staging = parent / UniqueStem();
        filesystem_error.clear();
        if (!std::filesystem::create_directory(staging, filesystem_error)) {
            if (!filesystem_error)
                continue;
            if (filesystem_error == std::errc::file_exists)
                continue;
            SetError(error, "cannot create private output staging directory: " + filesystem_error.message());
            return false;
        }
        transaction->destination_path_ = normalized_destination;
        transaction->staging_directory_ = staging;
        transaction->temporary_path_ = staging / normalized_destination.filename();
        transaction->backup_path_ = staging / (path_utils::ToUtf8(normalized_destination.filename()) + ".backup");
        transaction->overwrite_existing_ = overwrite_existing;
        transaction->destination_staged_ = false;
        transaction->committed_ = false;
        return true;
    }
    SetError(error, "cannot allocate a unique output staging directory");
    return false;
}

bool AtomicOutputTransaction::Commit(std::string* error) {
    if (!valid() || committed_) {
        SetError(error, "output transaction is not active");
        return false;
    }
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(temporary_path_, filesystem_error) || filesystem_error) {
        SetError(error, "staged output file is missing: " + path_utils::ToUtf8(temporary_path_));
        return false;
    }
    const bool destination_exists = std::filesystem::exists(destination_path_, filesystem_error);
    if (filesystem_error) {
        SetError(error, "cannot inspect output before commit: " + filesystem_error.message());
        return false;
    }
    if (destination_exists) {
        if (!overwrite_existing_) {
            SetError(error, "output file appeared while overwrite was disabled");
            return false;
        }
        filesystem_error.clear();
        std::filesystem::rename(destination_path_, backup_path_, filesystem_error);
        if (filesystem_error) {
            SetError(error, "cannot stage existing output for replacement: " + filesystem_error.message());
            return false;
        }
        destination_staged_ = true;
    }

    filesystem_error.clear();
    std::filesystem::rename(temporary_path_, destination_path_, filesystem_error);
    if (filesystem_error) {
        std::string restore_message;
        if (destination_staged_) {
            std::error_code restore_error;
            std::filesystem::rename(backup_path_, destination_path_, restore_error);
            if (restore_error)
                restore_message = "; original output restore failed: " + restore_error.message() +
                                  " (backup: " + path_utils::ToUtf8(backup_path_) + ")";
            else
                destination_staged_ = false;
        }
        SetError(error, "cannot commit MP4 output: " + filesystem_error.message() + restore_message);
        return false;
    }
    if (destination_staged_) {
        std::error_code backup_error;
        std::filesystem::remove(backup_path_, backup_error);
        if (backup_error) {
            SetError(error, "output committed, but backup cleanup failed: " + backup_error.message() +
                             " (backup: " + path_utils::ToUtf8(backup_path_) + ")");
            committed_ = true;
            return true;
        }
        destination_staged_ = false;
    }
    committed_ = true;
    std::error_code cleanup_error;
    std::filesystem::remove(staging_directory_, cleanup_error);
    return true;
}

void AtomicOutputTransaction::Abort() noexcept {
    if (staging_directory_.empty()) {
        Reset();
        return;
    }
    std::error_code cleanup_error;
    if (destination_staged_ && !committed_) {
        std::filesystem::rename(backup_path_, destination_path_, cleanup_error);
        if (!cleanup_error)
            destination_staged_ = false;
    }
    // If restoration failed, keep the private staging directory and backup so
    // the original output remains recoverable instead of deleting it during
    // best-effort cleanup.
    if (!committed_ && !destination_staged_)
        std::filesystem::remove_all(staging_directory_, cleanup_error);
    Reset();
}

void AtomicOutputTransaction::Reset() noexcept {
    destination_path_.clear();
    staging_directory_.clear();
    temporary_path_.clear();
    backup_path_.clear();
    overwrite_existing_ = false;
    destination_staged_ = false;
    committed_ = false;
}

}  // namespace immersive_audio_muxer
