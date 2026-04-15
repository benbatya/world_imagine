#include "InstantSplatRunner.hpp"
#include "util/AsyncJob.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Subprocess helper — fork/exec with stdout/stderr capture and cancel polling
// ---------------------------------------------------------------------------
static int runDockerCommand(const std::vector<std::string>& args, AsyncJob& job,
                            std::function<void(const std::string&)> onLine = nullptr) {
    int pipefd[2];
    if (pipe(pipefd) < 0)
        throw std::runtime_error(std::format("pipe failed: {}", strerror(errno)));

    // Log the command
    std::string cmdLine;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0)
            cmdLine += ' ';
        cmdLine += args[i];
    }
    std::fprintf(stderr, "[InstantSplat] Running: %s\n", cmdLine.c_str());

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        throw std::runtime_error(std::format("fork failed: {}", strerror(errno)));
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(pipefd[1]);

    // Set read end non-blocking
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    std::string buf;
    while (true) {
        // Drain available output
        char tmp[512];
        ssize_t n;
        while ((n = read(pipefd[0], tmp, sizeof(tmp))) > 0) {
            buf.append(tmp, static_cast<size_t>(n));
            size_t pos;
            while ((pos = buf.find('\n')) != std::string::npos) {
                auto line = buf.substr(0, pos);
                std::fprintf(stderr, "[instantsplat] %s\n", line.c_str());
                if (onLine)
                    onLine(line);
                buf.erase(0, pos + 1);
            }
        }

        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            close(pipefd[0]);
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        if (r < 0) {
            close(pipefd[0]);
            throw std::runtime_error(std::format("waitpid failed: {}", strerror(errno)));
        }

        if (job.cancelRequested()) {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
            close(pipefd[0]);
            return -1;
        }

        std::this_thread::sleep_for(100ms);
    }
}

// Run a command and capture its stdout into a string (blocking, no AsyncJob).
static std::pair<int, std::string> runCapture(const std::vector<std::string>& args) {
    int pipefd[2];
    if (pipe(pipefd) < 0)
        return {-1, std::format("pipe failed: {}", strerror(errno))};

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return {-1, std::format("fork failed: {}", strerror(errno))};
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(pipefd[1]);

    std::string output;
    char tmp[512];
    ssize_t n;
    while ((n = ::read(pipefd[0], tmp, sizeof(tmp))) > 0)
        output.append(tmp, static_cast<size_t>(n));

    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return {rc, output};
}

// ---------------------------------------------------------------------------
// Prerequisite checks
// ---------------------------------------------------------------------------
PrerequisiteResult
InstantSplatRunner::checkPrerequisites(const std::string& dockerImage) {
    // 1. Check docker is available
    auto [rcDocker, outDocker] = runCapture({"docker", "--version"});
    if (rcDocker != 0) {
        return {false,
                "Docker is not installed or not in PATH.\n"
                "Install Docker Engine: https://docs.docker.com/engine/install/\n"
                "Output: " +
                    outDocker};
    }

    // 2. Check nvidia GPU access inside docker
    auto [rcGpu, outGpu] = runCapture({"docker", "run", "--rm", "--gpus", "all",
                                       "nvidia/cuda:12.6.0-base-ubuntu24.04", "nvidia-smi"});
    if (rcGpu != 0) {
        return {false,
                "nvidia-container-toolkit is not configured or GPU not accessible in Docker.\n"
                "Install nvidia-container-toolkit: "
                "https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/install-guide.html\n"
                "Output: " +
                    outGpu};
    }

    // 3. Check the InstantSplat image exists
    auto [rcImg, outImg] = runCapture({"docker", "image", "inspect", dockerImage});
    if (rcImg != 0) {
        return {false,
                std::format("Docker image '{}' not found.\n"
                            "Build or pull the InstantSplat image first (see PRD step 2).\n"
                            "Output: {}",
                            dockerImage, outImg)};
    }

    return {true, ""};
}

// ---------------------------------------------------------------------------
// Error log writing
// ---------------------------------------------------------------------------
fs::path InstantSplatRunner::writeErrorLog(const std::string& errorDetails) {
    fs::create_directories("logs");

    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&tt, &tm);
    char timeBuf[64];
    std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d_%H%M%S", &tm);

    fs::path logPath = fs::path("logs") / std::format("import_error_{}.log", timeBuf);

    std::ofstream f(logPath);
    if (f) {
        f << "World Imagine — Import Error Log\n";
        f << "Timestamp: " << timeBuf << "\n";
        f << "---\n\n";
        f << errorDetails << "\n";
    }

    return logPath;
}

// ---------------------------------------------------------------------------
// Parse training iteration progress from InstantSplat output
// ---------------------------------------------------------------------------
static std::pair<int, int> parseTrainProgress(const std::string& line) {
    // InstantSplat train.py prints lines like: [ITER 100/7000] loss=0.123
    // or step 100/7000 or similar patterns with iter/total
    auto slash = line.find('/');
    if (slash == std::string::npos)
        return {0, 0};

    // Walk backwards from slash to find the start of the number
    size_t numStart = slash;
    while (numStart > 0 && std::isdigit(line[numStart - 1]))
        --numStart;
    if (numStart == slash)
        return {0, 0};

    // Walk forward from slash to find the end of the total number
    size_t numEnd = slash + 1;
    while (numEnd < line.size() && std::isdigit(line[numEnd]))
        ++numEnd;
    if (numEnd == slash + 1)
        return {0, 0};

    try {
        int iter = std::stoi(line.substr(numStart, slash - numStart));
        int total = std::stoi(line.substr(slash + 1, numEnd - slash - 1));
        return {iter, total};
    } catch (...) {
        return {0, 0};
    }
}

// ---------------------------------------------------------------------------
// Main pipeline
// ---------------------------------------------------------------------------
fs::path InstantSplatRunner::run(const InstantSplatConfig& cfg, AsyncJob& job, float progressLo,
                                 float progressHi) {
    const float range = progressHi - progressLo;

    // Ensure output and workspace directories exist.
    // workspace/ is mounted as /data/frames so init_geo.py can write sparse_0/,
    // confidence_dsp.npy, etc. there — and those files persist across docker run
    // invocations (each stage is a separate --rm container).
    // The actual frame images are layered on top via a second bind-mount at
    // /data/frames/images, which is what init_geo.py / train.py expect.
    fs::create_directories(cfg.outputDir);
    fs::path absWorkspace = fs::absolute(cfg.outputDir / "workspace");
    fs::create_directories(absWorkspace);
    fs::create_directories(absWorkspace / "images"); // ensure mount point exists

    // Absolute paths for Docker mounts
    fs::path absFrames = fs::absolute(cfg.framesDir);
    fs::path absOutput = fs::absolute(cfg.outputDir);

    // Container paths
    const std::string containerSourcePath = "/data/frames";          // workspace (writable, persists)
    const std::string containerImages     = "/data/frames/images";   // frames (bind-mounted on top)
    const std::string containerOutput     = "/data/output";
    const std::string containerWorkdir    = "/opt/instantsplat";

    // Common docker run prefix — workspace first, then images on top
    auto dockerBase = [&]() -> std::vector<std::string> {
        return {"docker",  "run",
                "--rm",    "--gpus",  "all",
                "-v",      absWorkspace.string() + ":" + containerSourcePath,
                "-v",      absFrames.string()    + ":" + containerImages,
                "-v",      absOutput.string()    + ":" + containerOutput,
                "-w",      containerWorkdir,
                cfg.dockerImage};
    };

    // --- Stage 1: init_geo.py — geometry initialisation via MASt3R (0–40%) ---
    job.setStatusText("InstantSplat: initialising geometry (MASt3R)...");
    job.setProgress(progressLo);

    {
        auto args = dockerBase();
        // init_geo.py expects:
        //   --source_path <dir_containing_images/> --model_path <output_dir> --n_views <num>
        args.insert(args.end(),
                    {"python", "init_geo.py", "--source_path", containerSourcePath, "--model_path",
                     containerOutput, "--n_views", std::to_string(cfg.nViews)});

        int rc = runDockerCommand(args, job, [&](const std::string& line) {
            // Report status updates from init_geo
            if (line.find("Processing") != std::string::npos ||
                line.find("Matching") != std::string::npos) {
                job.setStatusText("InstantSplat: " + line);
            }
        });
        if (rc == -1)
            return {}; // cancelled
        if (rc != 0)
            throw std::runtime_error(
                std::format("InstantSplat init_geo.py failed (exit {})", rc));
    }

    job.setProgress(progressLo + range * 0.4f);

    // --- Stage 2: train.py — Gaussian splatting training (40–85%) ---
    job.setStatusText("InstantSplat: training Gaussian splats...");

    {
        auto args = dockerBase();
        args.insert(args.end(),
                    {"python", "train.py", "--source_path", containerSourcePath, "--model_path",
                     containerOutput, "--iterations", std::to_string(cfg.trainIterations),
                     "--n_views", std::to_string(cfg.nViews)});

        float trainLo = progressLo + range * 0.4f;
        float trainHi = progressLo + range * 0.85f;

        int rc = runDockerCommand(args, job, [&](const std::string& line) {
            auto [iter, total] = parseTrainProgress(line);
            if (total > 0) {
                float frac = static_cast<float>(iter) / static_cast<float>(total);
                job.setProgress(trainLo + frac * (trainHi - trainLo));
                job.setStatusText(std::format("InstantSplat training: {}/{}", iter, total));
            }
        });
        if (rc == -1)
            return {}; // cancelled
        if (rc != 0)
            throw std::runtime_error(
                std::format("InstantSplat train.py failed (exit {})", rc));
    }

    job.setProgress(progressLo + range * 0.85f);

    // --- Stage 3: render.py — render + export point cloud (85–100%) ---
    job.setStatusText("InstantSplat: rendering and exporting PLY...");

    {
        auto args = dockerBase();
        args.insert(args.end(),
                    {"python", "render.py", "--source_path", containerSourcePath, "--model_path",
                     containerOutput, "--iteration", std::to_string(cfg.trainIterations),
                     "--n_views", std::to_string(cfg.nViews)});

        int rc = runDockerCommand(args, job, [&](const std::string& line) {
            job.setStatusText("InstantSplat render: " + line);
        });
        if (rc == -1)
            return {}; // cancelled
        if (rc != 0)
            throw std::runtime_error(
                std::format("InstantSplat render.py failed (exit {})", rc));
    }

    job.setProgress(progressHi);

    // Find the output PLY file — InstantSplat writes point_cloud/iteration_N/point_cloud.ply
    fs::path plyPath =
        cfg.outputDir / "point_cloud" /
        std::format("iteration_{}", cfg.trainIterations) / "point_cloud.ply";

    if (!fs::exists(plyPath)) {
        // Fallback: search for any .ply file in the output directory
        for (auto& entry : fs::recursive_directory_iterator(cfg.outputDir)) {
            if (entry.path().extension() == ".ply") {
                plyPath = entry.path();
                break;
            }
        }
    }

    if (!fs::exists(plyPath))
        throw std::runtime_error(
            "InstantSplat did not produce a PLY file in " + cfg.outputDir.string());

    return plyPath;
}
