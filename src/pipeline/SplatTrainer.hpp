#pragma once
#include <filesystem>
#include <memory>

class AsyncJob;
class GaussianModel;

struct TrainConfig {
    std::filesystem::path colmapPath;   // path to COLMAP sparse/0/ directory
    std::filesystem::path outputPly;    // where opensplat writes the result
    std::string opensplatBin;           // path to opensplat executable
    int iterations = 7000;
};

class SplatTrainer {
public:
    // Runs opensplat on the COLMAP output and loads the result into a GaussianModel.
    // Returns nullptr if cancelled.
    // Throws std::runtime_error on failure.
    // Reports progress scaled to [progressLo, progressHi] on the job.
    std::shared_ptr<GaussianModel> run(const TrainConfig& cfg,
                                       AsyncJob& job,
                                       float progressLo = 0.f,
                                       float progressHi = 1.f);
};
