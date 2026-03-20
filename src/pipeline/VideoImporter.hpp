#pragma once
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>

class AsyncJob;
struct AppState;

class VideoImporter {
public:
    static VideoImporter& instance();

    VideoImporter(const VideoImporter&)            = delete;
    VideoImporter& operator=(const VideoImporter&) = delete;

    // Starts the full video → splat pipeline on a background thread:
    //   FrameExtractor (0–30%) → ColmapRunner (30–65%) → SplatTrainer (65–100%)
    // On success, updates state.gaussianModel under state.gaussianMutex.
    // No-op if a pipeline is already running.
    void importAsync(const std::filesystem::path& videoPath, AppState& state);

    // --- Status queries (main thread) ---
    bool        isLoading()  const;
    bool        isDone()     const;
    float       progress()   const;
    std::string statusText() const;

    // --- Cancel ---
    void requestCancel();

    // Cancel any running pipeline and block until the thread exits.
    void cancelAndJoin();

    // If the pipeline is done, handle result (set error status if exception),
    // reset internal state, and return true. Otherwise return false.
    bool finalize(AppState& state);

private:
    VideoImporter() = default;

    std::shared_ptr<AsyncJob>   m_job;
    std::optional<std::jthread> m_thread;
};
