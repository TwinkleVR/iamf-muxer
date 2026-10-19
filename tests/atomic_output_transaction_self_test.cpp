#include "atomic_output_transaction.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <string>
#include <iterator>

namespace {

bool WriteText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    return output.good();
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

}  // namespace

int main() {
    std::error_code error;
    const auto unique_suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root = std::filesystem::temp_directory_path(error) /
                                       ("iamf_muxer_atomic_output_test-" + std::to_string(unique_suffix));
    if (error) {
        std::cerr << "cannot locate temporary directory\n";
        return 1;
    }
    std::filesystem::remove_all(root, error);
    error.clear();
    if (!std::filesystem::create_directories(root, error) || error) {
        std::cerr << "cannot create test directory\n";
        return 1;
    }

    const std::filesystem::path destination = root / "output.mp4";
    const std::filesystem::path partial_sentinel = root / "output.mp4.partial";
    const std::filesystem::path backup_sentinel = root / "output.mp4.backup";
    if (!WriteText(destination, "old") || !WriteText(partial_sentinel, "partial-sentinel") ||
        !WriteText(backup_sentinel, "backup-sentinel")) {
        std::cerr << "cannot create test fixtures\n";
        return 1;
    }

    immersive_audio_muxer::AtomicOutputTransaction transaction;
    std::string transaction_error;
    if (!immersive_audio_muxer::AtomicOutputTransaction::Create(destination, true, &transaction, &transaction_error) ||
        !WriteText(transaction.temporary_path(), "new") || !transaction.Commit(&transaction_error)) {
        std::cerr << transaction_error << '\n';
        return 1;
    }
    if (ReadText(destination) != "new" || ReadText(partial_sentinel) != "partial-sentinel" ||
        ReadText(backup_sentinel) != "backup-sentinel") {
        std::cerr << "transaction changed an unrelated sidecar\n";
        return 1;
    }

    if (immersive_audio_muxer::AtomicOutputTransaction::Create(destination, false, &transaction, &transaction_error)) {
        std::cerr << "overwrite-disabled transaction unexpectedly succeeded\n";
        return 1;
    }
    std::filesystem::remove_all(root, error);
    return error ? 1 : 0;
}
