#pragma once
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>

class AsyncJob;
class GaussianModel;
struct AppState;

class SplatIO {
public:
    static SplatIO& instance();

    SplatIO(const SplatIO&)            = delete;
    SplatIO& operator=(const SplatIO&) = delete;

    // --- Async import ---
    // Starts a background thread that loads the PLY file.
    // On success, updates state.gaussianModel under state.gaussianMutex.
    // No-op if a load is already in progress.
    void loadAsync(const std::filesystem::path& path, AppState& state);

    // --- Status queries (main thread) ---
    bool        isLoading()  const;
    bool        isDone()     const;
    float       progress()   const;
    std::string statusText() const;

    // --- Cancel ---
    void requestCancel();

    // Cancels any in-progress load and blocks until the thread exits.
    // Call before destroying the AppState the load was started with.
    void cancelAndJoin();

    // --- Completion handling ---
    // If the job is done, handles the result (sets error status if exception),
    // resets internal state, and returns true. Otherwise returns false.
    bool finalize(AppState& state);

    // --- Synchronous export (unchanged) ---
    void exportPLY(const GaussianModel& model, const std::filesystem::path& path);

private:
    SplatIO() = default;

    std::shared_ptr<AsyncJob>   m_job;
    std::optional<std::jthread> m_thread;
};
