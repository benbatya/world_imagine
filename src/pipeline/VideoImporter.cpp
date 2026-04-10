#include "VideoImporter.hpp"
#include "FrameExtractor.hpp"
#include "InstantSplatRunner.hpp"
#include "app/AppState.hpp"
#include "io/PlyParser.hpp"
#include "model/GaussianModel.hpp"
#include "util/AsyncJob.hpp"

// --- COLMAP+OpenSplat headers (kept as fallback — entry point commented out) ---
// #include "ColmapRunner.hpp"
// #include "SplatTrainer.hpp"

#include <imgui.h>
#include <nfd.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <stdexcept>

#include <unistd.h>

namespace fs = std::filesystem;

// Generate a default run directory name: run_YYYYMMDDHHMMSS
static std::string defaultRunDirName() {
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&tt, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "run_%Y%m%d%H%M%S", &tm);
    return buf;
}

// ---------------------------------------------------------------------------
// config.yaml helpers — minimal hand-rolled read/write (no external YAML lib)
// ---------------------------------------------------------------------------
static fs::path configPath(const fs::path& runRoot) { return runRoot / "config.yaml"; }

static void writeConfig(const fs::path& runRoot, const fs::path& videoPath) {
    std::ofstream f(configPath(runRoot));
    if (f)
        f << "video_path: " << videoPath.string() << "\n";
}

// Returns empty path if not found or unreadable.
static fs::path readConfigVideoPath(const fs::path& runRoot) {
    std::ifstream f(configPath(runRoot));
    if (!f)
        return {};
    std::string line;
    while (std::getline(f, line)) {
        const std::string key = "video_path:";
        if (line.rfind(key, 0) == 0) {
            std::string val = line.substr(key.size());
            auto pos = val.find_first_not_of(" \t");
            if (pos != std::string::npos)
                val = val.substr(pos);
            return fs::path(val);
        }
    }
    return {};
}

// ---------------------------------------------------------------------------

VideoImporter& VideoImporter::instance() {
    static VideoImporter s;
    return s;
}

void VideoImporter::beginImport(AppState& /*state*/) {
    if (m_state != State::Idle)
        return; // already in a workflow

    m_videoPath       = fs::path{};
    m_runExtract      = true;
    m_runInstantSplat = true;
    m_prereqError.clear();
    m_prereqLogPath.clear();

    // Default run root: CWD / run_YYYYMMDDHHMMSS
    fs::path    defaultDir = fs::current_path() / defaultRunDirName();
    std::string dirStr     = defaultDir.string();
    std::strncpy(m_dirBuf, dirStr.c_str(), sizeof(m_dirBuf) - 1);
    m_dirBuf[sizeof(m_dirBuf) - 1] = '\0';

    m_state = State::PickDirectory;
}

bool VideoImporter::drawUI(AppState& state) {
    switch (m_state) {
    case State::Idle:
    case State::Running:
        return false;

    case State::PickDirectory: {
        ImGui::OpenPopup("Select Run Directory");
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({500, 0}, ImGuiCond_Appearing);

        if (ImGui::BeginPopupModal("Select Run Directory", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("Choose the output directory for this pipeline run.");
            ImGui::Spacing();

            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90);
            ImGui::InputText("##rundir", m_dirBuf, sizeof(m_dirBuf));
            ImGui::SameLine();
            if (ImGui::Button("Browse…")) {
                nfdchar_t* outPath = nullptr;
                nfdresult_t nfdRes = NFD_PickFolder(&outPath, nullptr);
                if (nfdRes == NFD_OKAY) {
                    std::strncpy(m_dirBuf, outPath, sizeof(m_dirBuf) - 1);
                    m_dirBuf[sizeof(m_dirBuf) - 1] = '\0';
                    NFD_FreePath(outPath);
                }
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::Button("OK", {120, 0})) {
                m_runRoot = fs::path(m_dirBuf);
                fs::create_directories(m_runRoot);
                ImGui::CloseCurrentPopup();

                // Check config.yaml for a previously stored video path
                fs::path storedVideo = readConfigVideoPath(m_runRoot);
                if (!storedVideo.empty() && fs::exists(storedVideo)) {
                    m_videoPath = storedVideo;
                    advanceState(state);
                } else {
                    // No valid config — ask the user to pick a video
                    m_videoBuf[0] = '\0';
                    m_state       = State::PickVideo;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", {120, 0})) {
                ImGui::CloseCurrentPopup();
                m_state = State::Idle;
            }
            ImGui::EndPopup();
        }
        return true;
    }

    case State::PickVideo: {
        ImGui::OpenPopup("Select Video File");
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({520, 0}, ImGuiCond_Appearing);

        if (ImGui::BeginPopupModal("Select Video File", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("Choose the video file to import into '%s'.",
                               m_runRoot.string().c_str());
            ImGui::Spacing();

            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90);
            ImGui::InputText("##videopath", m_videoBuf, sizeof(m_videoBuf));
            ImGui::SameLine();
            if (ImGui::Button("Browse…")) {
                nfdchar_t*      outPath  = nullptr;
                nfdfilteritem_t filters[1] = {{"Video files", "mp4,avi,mov,mkv"}};
                nfdresult_t     nfdRes   = NFD_OpenDialog(&outPath, filters, 1, nullptr);
                if (nfdRes == NFD_OKAY) {
                    std::strncpy(m_videoBuf, outPath, sizeof(m_videoBuf) - 1);
                    m_videoBuf[sizeof(m_videoBuf) - 1] = '\0';
                    NFD_FreePath(outPath);
                }
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const bool hasPath = m_videoBuf[0] != '\0';
            if (!hasPath)
                ImGui::BeginDisabled();
            if (ImGui::Button("OK", {120, 0})) {
                m_videoPath = fs::path(m_videoBuf);
                writeConfig(m_runRoot, m_videoPath);
                ImGui::CloseCurrentPopup();
                advanceState(state);
            }
            if (!hasPath)
                ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", {120, 0})) {
                ImGui::CloseCurrentPopup();
                m_state = State::Idle;
            }
            ImGui::EndPopup();
        }
        return true;
    }

    case State::PrereqError: {
        ImGui::OpenPopup("Prerequisite Error");
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({600, 0}, ImGuiCond_Appearing);

        if (ImGui::BeginPopupModal("Prerequisite Error", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextColored({1.f, 0.4f, 0.4f, 1.f}, "InstantSplat prerequisites not met");
            ImGui::Spacing();
            ImGui::TextWrapped("%s", m_prereqError.c_str());
            ImGui::Spacing();
            if (!m_prereqLogPath.empty()) {
                ImGui::TextWrapped("Error details written to: %s", m_prereqLogPath.c_str());
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            if (ImGui::Button("OK", {120, 0})) {
                ImGui::CloseCurrentPopup();
                m_state = State::Idle;
            }
            ImGui::EndPopup();
        }
        return true;
    }

    case State::AskFrames: {
        ImGui::OpenPopup("Frames Exist");
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {0.5f, 0.5f});

        if (ImGui::BeginPopupModal("Frames Exist", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped(
                "The directory '%s/frames/' already exists.\n\n"
                "Do you want to re-extract frames from the video?",
                m_runRoot.string().c_str());
            ImGui::Spacing();

            if (ImGui::Button("Yes, re-extract", {160, 0})) {
                m_runExtract = true;
                ImGui::CloseCurrentPopup();
                advanceState(state);
            }
            ImGui::SameLine();
            if (ImGui::Button("No, skip", {160, 0})) {
                m_runExtract = false;
                ImGui::CloseCurrentPopup();
                advanceState(state);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", {80, 0})) {
                ImGui::CloseCurrentPopup();
                m_state = State::Idle;
            }
            ImGui::EndPopup();
        }
        return true;
    }

    case State::AskInstantSplat: {
        ImGui::OpenPopup("InstantSplat Output Exists");
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {0.5f, 0.5f});

        if (ImGui::BeginPopupModal("InstantSplat Output Exists", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped(
                "The directory '%s/instantsplat/' already exists.\n\n"
                "Do you want to re-run InstantSplat?",
                m_runRoot.string().c_str());
            ImGui::Spacing();

            if (ImGui::Button("Yes, re-run", {160, 0})) {
                m_runInstantSplat = true;
                ImGui::CloseCurrentPopup();
                launchPipeline(state);
            }
            ImGui::SameLine();
            if (ImGui::Button("No, load existing", {160, 0})) {
                m_runInstantSplat = false;
                ImGui::CloseCurrentPopup();
                launchPipeline(state);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", {80, 0})) {
                ImGui::CloseCurrentPopup();
                m_state = State::Idle;
            }
            ImGui::EndPopup();
        }
        return true;
    }

    // --- COLMAP+OpenSplat dialogs (commented out — kept as fallback) ---
    // case State::AskColmap: { ... }
    // case State::AskTrainer: { ... }
    }
    return false;
}

void VideoImporter::advanceState(AppState& state) {
    // Check frames/ first
    if (m_state == State::PickDirectory || m_state == State::PickVideo) {
        if (fs::exists(m_runRoot / "frames")) {
            m_state = State::AskFrames;
            return;
        }
        m_runExtract = true; // directory doesn't exist → auto-extract
    }

    if (m_state == State::AskFrames) {
        // Fall through to prereq check
    }

    // Check prerequisites
    {
        auto prereq = InstantSplatRunner::checkPrerequisites();
        if (!prereq.ok) {
            m_prereqError   = prereq.error;
            m_prereqLogPath = InstantSplatRunner::writeErrorLog(prereq.error).string();
            m_state         = State::PrereqError;
            return;
        }
    }

    // Check instantsplat/ output directory
    if (fs::exists(m_runRoot / "instantsplat")) {
        m_state = State::AskInstantSplat;
        return;
    }
    m_runInstantSplat = true;

    launchPipeline(state);
}

void VideoImporter::launchPipeline(AppState& state) {
    m_state = State::Running;
    m_job   = std::make_shared<AsyncJob>();

    fs::path framesDir       = m_runRoot / "frames";
    fs::path instantSplatDir = m_runRoot / "instantsplat";
    fs::path videoPath       = m_videoPath;
    std::string dockerImage  = "world_imagine/instantsplat:latest";

    bool doExtract      = m_runExtract;
    bool doInstantSplat = m_runInstantSplat;

    auto job = m_job;

    m_thread.emplace([job, videoPath, framesDir, instantSplatDir, dockerImage, doExtract,
                      doInstantSplat, &state]() {
        try {
            // --- Stage 1: extract frames (0 – 20%) ---
            if (doExtract) {
                job->setStatusText("Extracting frames…");
                auto frames = FrameExtractor{}.run(
                    {.videoPath = videoPath, .outputDir = framesDir, .everyNthFrame = 5}, *job,
                    0.f, 0.20f);

                if (job->cancelRequested()) {
                    job->markDone();
                    return;
                }
                if (frames.empty())
                    throw std::runtime_error("No frames extracted from video");

                job->setStatusText(std::format("Extracted {} frames", frames.size()));
            } else {
                job->setProgress(0.20f);
                job->setStatusText("Skipped frame extraction");
            }

            // --- Stage 2: InstantSplat (20 – 100%) ---
            fs::path plyPath;
            if (doInstantSplat) {
                InstantSplatRunner runner;
                InstantSplatConfig cfg;
                cfg.framesDir     = framesDir;
                cfg.outputDir     = instantSplatDir;
                cfg.dockerImage   = dockerImage;
                cfg.trainIterations = 7000;

                plyPath = runner.run(cfg, *job, 0.20f, 1.0f);

                if (job->cancelRequested()) {
                    job->markDone();
                    return;
                }
                if (plyPath.empty())
                    throw std::runtime_error("InstantSplat produced no PLY file");
            } else {
                // Load existing PLY from instantsplat output directory
                job->setStatusText("Loading existing InstantSplat PLY…");
                fs::path existingPly;
                for (auto& entry : fs::recursive_directory_iterator(instantSplatDir)) {
                    if (entry.path().extension() == ".ply") {
                        existingPly = entry.path();
                        break;
                    }
                }
                if (existingPly.empty())
                    throw std::runtime_error(
                        "No PLY file found in " + instantSplatDir.string());
                plyPath = existingPly;
                job->setProgress(0.50f);
            }

            // Load the PLY into a GaussianModel
            job->setStatusText("Loading PLY into GPU…");
            AsyncJob loadJob;
            auto model = PlyParser{}.loadAsync(plyPath, loadJob);
            if (!model)
                throw std::runtime_error("Failed to load PLY: " + plyPath.string());

            job->setProgress(1.f);

            {
                std::lock_guard lock{state.gaussianMutex};
                state.gaussianModel = std::move(model);
                state.committedSplatCount.store(state.gaussianModel->numSplats(),
                                                std::memory_order_release);
            }

            job->setStatusText("Done");
            job->markDone();

        } catch (...) {
            job->markDone(std::current_exception());
        }
    });

    // --- COLMAP+OpenSplat pipeline (commented out — kept as fallback) ---
    // Uncomment and swap above block to re-enable the COLMAP+OpenSplat path:
    //
    // m_thread.emplace([job, videoPath, framesDir, colmapDir, outputPly, opensplat, doExtract,
    //                   doColmap, doTrainer, &state]() {
    //     // Stage 1: extract frames (0 – 30%)
    //     // Stage 2: COLMAP SfM (30 – 65%)
    //     // Stage 3: OpenSplat training (65 – 100%)
    // });
}

bool VideoImporter::isLoading() const {
    return m_state == State::Running && m_job && !m_job->isDone();
}
bool VideoImporter::isDone() const { return m_job && m_job->isDone(); }

float VideoImporter::progress() const { return m_job ? m_job->progress() : 0.f; }

std::string VideoImporter::statusText() const {
    return m_job ? m_job->statusText() : std::string{};
}

void VideoImporter::requestCancel() {
    if (m_job)
        m_job->requestCancel();
}

void VideoImporter::cancelAndJoin() {
    if (m_job)
        m_job->requestCancel();
    m_thread.reset(); // jthread destructor joins
    m_job.reset();
    m_state = State::Idle;
}

bool VideoImporter::finalize(AppState& state) {
    if (!m_job || !m_job->isDone())
        return false;

    if (auto ex = m_job->exception()) {
        try {
            std::rethrow_exception(ex);
        } catch (const std::exception& e) {
            std::string errMsg = std::string("Video import failed: ") + e.what();
            // Write error log
            InstantSplatRunner::writeErrorLog(errMsg);
            state.setStatus(errMsg);
        }
    } else {
        std::lock_guard lock{state.gaussianMutex};
        if (state.gaussianModel) {
            state.setStatus(
                std::format("Imported {} splats", state.gaussianModel->numSplats()));
        }
    }

    m_thread.reset();
    m_job.reset();
    m_state = State::Idle;
    return true;
}
