#include "SplatTrainer.hpp"
#include "io/PlyParser.hpp"
#include "util/AsyncJob.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace std::chrono_literals;

// Runs opensplat and calls onLine for each line written to its stdout/stderr.
// Returns the child exit code, or -1 if cancelled.
static int runOpensplat(const std::vector<std::string>& args,
                        AsyncJob& job,
                        std::function<void(const std::string&)> onLine) {
    int pipefd[2];
    if (pipe(pipefd) < 0)
        throw std::runtime_error(std::format("pipe failed: {}", strerror(errno)));

    // Print the command before invoking
    std::string cmdLine;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) cmdLine += ' ';
        cmdLine += args[i];
    }
    fprintf(stderr, "[SplatTrainer] Running: %s\n", cmdLine.c_str());

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
                fprintf(stderr, "[opensplat] %s\n", line.c_str());
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

// Parse "[iter/total]" from an opensplat output line.
// Returns {iter, total} or {0, 0} if not matched.
static std::pair<int, int> parseIterLine(const std::string& line) {
    // opensplat prints lines like: [1/7000] Loss 0.123
    auto lbrace = line.find('[');
    auto slash = line.find('/');
    auto rbrace = line.find(']');
    if (lbrace == std::string::npos || slash == std::string::npos ||
        rbrace == std::string::npos || slash < lbrace || rbrace < slash)
        return {0, 0};
    try {
        int iter  = std::stoi(line.substr(lbrace + 1, slash - lbrace - 1));
        int total = std::stoi(line.substr(slash + 1, rbrace - slash - 1));
        return {iter, total};
    } catch (...) {
        return {0, 0};
    }
}

std::shared_ptr<GaussianModel> SplatTrainer::run(const TrainConfig& cfg,
                                                  AsyncJob& job,
                                                  float progressLo,
                                                  float progressHi) {
    job.setStatusText("Training: starting opensplat…");
    job.setProgress(progressLo);

    // opensplat <colmapPath> -n <iters> -o <output.ply>
    // The colmap path should point to the workspace root (parent of sparse/)
    // opensplat expects the COLMAP project root, not sparse/0/ directly.
    // Pass the parent of sparse/0/ (the workspace).
    std::filesystem::path workspacePath = cfg.colmapPath.parent_path().parent_path();

    std::vector<std::string> args{
        cfg.opensplatBin,
        workspacePath.string(),
        "-n", std::to_string(cfg.iterations),
        "-o", cfg.outputPly.string()
    };

    int rc = runOpensplat(args, job, [&](const std::string& line) {
        auto [iter, total] = parseIterLine(line);
        if (total > 0) {
            float frac = static_cast<float>(iter) / static_cast<float>(total);
            job.setProgress(progressLo + frac * (progressHi - progressLo));
            job.setStatusText(std::format("Training: {}/{}", iter, total));
        }
    });

    if (rc == -1) return nullptr; // cancelled
    if (rc != 0)
        throw std::runtime_error(std::format("opensplat failed (exit {})", rc));

    if (!std::filesystem::exists(cfg.outputPly))
        throw std::runtime_error("opensplat did not produce output at " + cfg.outputPly.string());

    job.setStatusText("Training: loading result…");
    job.setProgress(progressHi);

    // Load the resulting PLY into a GaussianModel
    AsyncJob loadJob;
    PlyParser parser;
    auto model = parser.loadAsync(cfg.outputPly, loadJob);
    if (!model)
        throw std::runtime_error("Failed to load trained PLY: " + cfg.outputPly.string());

    return model;
}
