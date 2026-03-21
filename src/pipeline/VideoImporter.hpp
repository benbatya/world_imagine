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

    // Begin the import workflow: shows directory selection, then confirmation dialogs
    // for each pipeline stage. Call after the user has picked a video file.
    void beginImport(const std::filesystem::path& videoPath, AppState& state);

    // Draw confirmation dialog UI (call every frame from the main loop).
    // Returns true if a dialog is currently visible.
    bool drawUI(AppState& state);

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

    // Advance to the next confirmation check or start the pipeline.
    void advanceState(AppState& state);

    // Actually launch the background pipeline thread with current decisions.
    void launchPipeline(AppState& state);

    enum class State {
        Idle,
        PickDirectory, // ImGui modal: edit run directory path
        AskFrames,     // frames/ exists — ask user
        AskColmap,     // colmap/ exists — ask user
        AskTrainer,    // output.ply exists — ask user
        Running,       // background thread active
    };

    State m_state{State::Idle};

    // Paths
    std::filesystem::path m_videoPath;
    std::filesystem::path m_runRoot;
    char                  m_dirBuf[1024]{};  // ImGui InputText buffer

    // Stage decisions (true = run that stage)
    bool m_runExtract{true};
    bool m_runColmap{true};
    bool m_runTrainer{true};

    std::shared_ptr<AsyncJob>   m_job;
    std::optional<std::jthread> m_thread;
};
