#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class AsyncJob;
class GaussianModel;

struct InstantSplatConfig {
    std::filesystem::path framesDir;   // host directory with extracted JPEG frames
    std::filesystem::path outputDir;   // host directory for InstantSplat output (PLY goes here)
    std::string dockerImage{"world_imagine/instantsplat:latest"};
    int trainIterations = 7000;
};

// Checks whether the Docker prerequisites for InstantSplat are available.
struct PrerequisiteResult {
    bool ok = false;
    std::string error; // non-empty when ok==false
};

class InstantSplatRunner {
public:
    // Check whether Docker, nvidia-container-toolkit, and the InstantSplat image exist.
    static PrerequisiteResult checkPrerequisites(
        const std::string& dockerImage = "world_imagine/instantsplat:latest");

    // Write an error to a timestamped log file under logs/.
    // Returns the path to the log file.
    static std::filesystem::path writeErrorLog(const std::string& errorDetails);

    // Run the full InstantSplat pipeline inside Docker:
    //   1. init_geo.py  (geometry initialisation via MASt3R)
    //   2. train.py     (Gaussian splatting training)
    //   3. render.py    (render + export PLY)
    //
    // framesDir is mounted read-only into the container; outputDir is mounted read-write.
    // Returns the path to the output .ply file on the host.
    // Returns empty path if cancelled.
    // Throws std::runtime_error on failure.
    std::filesystem::path run(const InstantSplatConfig& cfg,
                              AsyncJob& job,
                              float progressLo = 0.f,
                              float progressHi = 1.f);
};
