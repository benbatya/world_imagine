#pragma once
#include <filesystem>
#include <vector>

class AsyncJob;

struct FrameExtractorConfig {
    std::filesystem::path videoPath;
    std::filesystem::path outputDir;
    int everyNthFrame = 5; // at 30 fps → ~6 frames/sec captured
};

class FrameExtractor {
public:
    // Extracts JPEG frames from a video file.
    // Returns paths to extracted frames on success.
    // Returns empty vector if cancelled (job.cancelRequested()).
    // Throws std::runtime_error on hard failure.
    // Reports progress scaled to [progressLo, progressHi] on the job.
    std::vector<std::filesystem::path> run(const FrameExtractorConfig& cfg,
                                           AsyncJob& job,
                                           float progressLo = 0.f,
                                           float progressHi = 1.f);
};
