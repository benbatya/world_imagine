#include "ColmapRunner.hpp"
#include "util/AsyncJob.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// Forks and executes a command, polling every 100 ms for cancellation.
// Returns the child's exit code, or -1 if cancelled.
// Child inherits the parent's stdout/stderr.
static int runSubprocess(const std::vector<std::string>& args, AsyncJob& job) {
    // Log the command being run
    std::string cmdLine;
    for (const auto& a : args) {
        if (!cmdLine.empty()) cmdLine += ' ';
        cmdLine += a;
    }
    std::printf("[runSubprocess] %s\n", cmdLine.c_str());
    std::fflush(stdout);

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0)
        throw std::runtime_error(std::format("fork failed: {}", strerror(errno)));

    if (pid == 0) {
        // Child inherits parent's stdout/stderr — no redirect needed.
        setenv("QT_QPA_PLATFORM", "xcb", 1);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    // Parent: poll until child exits or cancel is requested
    while (true) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (r < 0)
            throw std::runtime_error(std::format("waitpid failed: {}", strerror(errno)));

        if (job.cancelRequested()) {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
            return -1;
        }

        std::this_thread::sleep_for(100ms);
    }
}

fs::path ColmapRunner::run(const ColmapConfig& cfg,
                           AsyncJob& job,
                           float progressLo,
                           float progressHi) {
    const float stageRange = (progressHi - progressLo) / 3.f;

    fs::create_directories(cfg.workspacePath);

    auto db = (cfg.workspacePath / "database.db").string();
    auto imgs = cfg.imagePath.string();
    auto sparse = (cfg.workspacePath / "sparse").string();
    fs::create_directories(sparse);

    // --- Stage 1: feature extraction ---
    job.setStatusText("COLMAP: extracting features…");
    job.setProgress(progressLo);

    int rc = runSubprocess({cfg.colmapBin,
                            "feature_extractor",
                            "--database_path", db,
                            "--image_path", imgs,
                            "--ImageReader.single_camera", "1"},
                           job);
    if (rc == -1) return {}; // cancelled
    if (rc != 0)
        throw std::runtime_error(std::format("colmap feature_extractor failed (exit {})", rc));

    job.setProgress(progressLo + stageRange);

    // --- Stage 2: matching ---
    job.setStatusText("COLMAP: matching features…");

    rc = runSubprocess({cfg.colmapBin,
                        "sequential_matcher",
                        "--database_path", db},
                       job);
    if (rc == -1) return {};
    if (rc != 0)
        throw std::runtime_error(std::format("colmap exhaustive_matcher failed (exit {})", rc));

    job.setProgress(progressLo + 2.f * stageRange);

    // --- Stage 3: sparse reconstruction ---
    job.setStatusText("COLMAP: reconstructing…");

    rc = runSubprocess({cfg.colmapBin,
                        "mapper",
                        "--database_path", db,
                        "--image_path", imgs,
                        "--output_path", sparse},
                       job);
    if (rc == -1) return {};
    if (rc != 0)
        throw std::runtime_error(std::format("colmap mapper failed (exit {})", rc));

    job.setProgress(progressHi);

    // COLMAP writes the first model to sparse/0/
    fs::path modelPath = cfg.workspacePath / "sparse" / "0";
    if (!fs::exists(modelPath))
        throw std::runtime_error("COLMAP mapper produced no model at " + modelPath.string());

    return modelPath;
}
