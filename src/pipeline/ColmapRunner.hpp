#pragma once
#include <filesystem>

class AsyncJob;

struct ColmapConfig {
    std::filesystem::path imagePath;     // directory of extracted frames
    std::filesystem::path workspacePath; // COLMAP will write database + sparse/ here
    std::string colmapBin{"colmap"};     // path/name of colmap executable
};

class ColmapRunner {
public:
    // Runs COLMAP feature extraction → matching → mapping.
    // Returns path to the sparse model (workspacePath/sparse/0/).
    // Returns empty path if cancelled.
    // Throws std::runtime_error on failure.
    // Reports progress scaled to [progressLo, progressHi] on the job.
    std::filesystem::path run(const ColmapConfig& cfg,
                              AsyncJob& job,
                              float progressLo = 0.f,
                              float progressHi = 1.f);
};
