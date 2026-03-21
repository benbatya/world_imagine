#include "VideoImporter.hpp"
#include "ColmapRunner.hpp"
#include "FrameExtractor.hpp"
#include "SplatTrainer.hpp"
#include "app/AppState.hpp"
#include "io/PlyParser.hpp"
#include "model/GaussianModel.hpp"
#include "util/AsyncJob.hpp"

#include <imgui.h>
#include <nfd.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <stdexcept>

#include <unistd.h>

namespace fs = std::filesystem;

static fs::path exeDirectory() {
    char buf[4096]{};
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0)
        return fs::path(buf).parent_path();
    return fs::current_path();
}

static fs::path findOpensplat() {
    fs::path next2exe = exeDirectory() / "opensplat";
    if (fs::exists(next2exe))
        return next2exe;
    fs::path buildTree =
        exeDirectory().parent_path() / "third_party/opensplat/build/opensplat";
    if (fs::exists(buildTree))
        return buildTree;
    return "opensplat";
}

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

VideoImporter& VideoImporter::instance() {
    static VideoImporter s;
    return s;
}

void VideoImporter::beginImport(const fs::path& videoPath, AppState& /*state*/) {
    if (m_state != State::Idle)
        return; // already in a workflow

    m_videoPath  = videoPath;
    m_runExtract = true;
    m_runColmap  = true;
    m_runTrainer = true;

    // Default run root: CWD / run_YYYYMMDDHHMMSS
    fs::path defaultDir = fs::current_path() / defaultRunDirName();
    std::string dirStr  = defaultDir.string();
    std::strncpy(m_dirBuf, dirStr.c_str(), sizeof(m_dirBuf) - 1);
    m_dirBuf[sizeof(m_dirBuf) - 1] = '\0';

    m_state = State::PickDirectory;
    std::cout << "m_state=" << (int)m_state << std::endl;

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
                nfdchar_t* outPath  = nullptr;
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
                advanceState(state);
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

    case State::AskColmap: {
        ImGui::OpenPopup("COLMAP Data Exists");
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {0.5f, 0.5f});

        if (ImGui::BeginPopupModal("COLMAP Data Exists", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped(
                "The directory '%s/colmap/' already exists.\n\n"
                "Do you want to re-run COLMAP?",
                m_runRoot.string().c_str());
            ImGui::Spacing();

            if (ImGui::Button("Yes, re-run", {160, 0})) {
                m_runColmap = true;
                ImGui::CloseCurrentPopup();
                advanceState(state);
            }
            ImGui::SameLine();
            if (ImGui::Button("No, skip", {160, 0})) {
                m_runColmap = false;
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

    case State::AskTrainer: {
        ImGui::OpenPopup("Output PLY Exists");
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {0.5f, 0.5f});

        if (ImGui::BeginPopupModal("Output PLY Exists", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped(
                "The file '%s/output.ply' already exists.\n\n"
                "Do you want to re-run splat training?",
                m_runRoot.string().c_str());
            ImGui::Spacing();

            if (ImGui::Button("Yes, re-train", {160, 0})) {
                m_runTrainer = true;
                ImGui::CloseCurrentPopup();
                advanceState(state);
            }
            ImGui::SameLine();
            if (ImGui::Button("No, skip", {160, 0})) {
                m_runTrainer = false;
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
    }
    return false;
}

void VideoImporter::advanceState(AppState& state) {
    // Walk through checks in order: frames → colmap → trainer → launch
    // We enter this after PickDirectory or after a user answered a question.

    std::cout << "advanceState: state=" << (int)m_state << std::endl;

    if (m_state == State::PickDirectory || m_state == State::AskFrames) {
        // Check colmap/ next (if we haven't checked it yet)
        if (m_state == State::PickDirectory) {
            // First check: does frames/ exist?
            if (fs::exists(m_runRoot / "frames")) {
                m_state = State::AskFrames;
                std::cout << "advanceState: moving to state=" << (int)m_state << std::endl;
                return;
            }
            m_runExtract = true; // directory doesn't exist → auto-extract
        }
        // Now check colmap/
        if (fs::exists(m_runRoot / "colmap")) {
            m_state = State::AskColmap;
            std::cout << "advanceState: moving to state=" << (int)m_state << std::endl;
            return;
        }
        m_runColmap = true; // doesn't exist → auto-run
    }

    if (m_state == State::PickDirectory || m_state == State::AskFrames ||
        m_state == State::AskColmap) {
        // Check output.ply
        if (fs::exists(m_runRoot / "output.ply")) {
            m_state = State::AskTrainer;
            std::cout << "advanceState: moving to state=" << (int)m_state << std::endl;
            return;
        }
        m_runTrainer = true; // doesn't exist → auto-run
    }

    // All checks done — launch the pipeline
    launchPipeline(state);
}

void VideoImporter::launchPipeline(AppState& state) {
    m_state = State::Running;
    m_job   = std::make_shared<AsyncJob>();

    fs::path framesDir  = m_runRoot / "frames";
    fs::path colmapDir  = m_runRoot / "colmap";
    fs::path outputPly  = m_runRoot / "output.ply";
    fs::path videoPath  = m_videoPath;
    std::string opensplat = findOpensplat().string();

    bool doExtract = m_runExtract;
    bool doColmap  = m_runColmap;
    bool doTrainer = m_runTrainer;

    auto job = m_job;

    m_thread.emplace(
        [job, videoPath, framesDir, colmapDir, outputPly, opensplat, doExtract, doColmap,
         doTrainer, &state]() {
            try {
                // --- Stage 1: extract frames (0 – 30%) ---
                if (doExtract) {
                    job->setStatusText("Extracting frames…");
                    auto frames = FrameExtractor{}.run(
                        {.videoPath = videoPath, .outputDir = framesDir, .everyNthFrame = 5},
                        *job, 0.f, 0.30f);

                    if (job->cancelRequested()) {
                        job->markDone();
                        return;
                    }
                    if (frames.empty())
                        throw std::runtime_error("No frames extracted from video");

                    job->setStatusText(std::format("Extracted {} frames", frames.size()));
                } else {
                    job->setProgress(0.30f);
                    job->setStatusText("Skipped frame extraction");
                }

                // --- Stage 2: COLMAP SfM (30 – 65%) ---
                fs::path modelPath;
                if (doColmap) {
                    modelPath = ColmapRunner{}.run(
                        {.imagePath     = framesDir,
                         .workspacePath = colmapDir,
                         .colmapBin     = "colmap"},
                        *job, 0.30f, 0.65f);

                    if (job->cancelRequested()) {
                        job->markDone();
                        return;
                    }
                    if (modelPath.empty())
                        throw std::runtime_error("COLMAP produced no model");
                } else {
                    job->setProgress(0.65f);
                    job->setStatusText("Skipped COLMAP");
                    modelPath = colmapDir / "sparse" / "0";
                }

                // Ensure colmap/images/ points to the frames dir so OpenSplat can find
                // the images at the conventional location regardless of where we stored them.
                fs::path imagesLink = colmapDir / "images";
                if (!fs::exists(imagesLink) && !fs::is_symlink(imagesLink))
                    fs::create_directory_symlink(framesDir, imagesLink);

                // --- Stage 3: train splats (65 – 100%) ---
                if (doTrainer) {
                    auto model = SplatTrainer{}.run(
                        {.colmapPath   = modelPath,
                         .outputPly    = outputPly,
                         .opensplatBin = opensplat,
                         .iterations   = 7000},
                        *job, 0.65f, 1.f);

                    if (job->cancelRequested()) {
                        job->markDone();
                        return;
                    }
                    if (!model)
                        throw std::runtime_error("SplatTrainer returned no model");

                    // Publish result to AppState
                    {
                        std::lock_guard lock{state.gaussianMutex};
                        state.gaussianModel = std::move(model);
                        state.committedSplatCount.store(state.gaussianModel->numSplats(),
                                                       std::memory_order_release);
                    }
                } else {
                    job->setStatusText("Loading existing PLY…");

                    // Load existing output.ply via PlyParser
                    AsyncJob loadJob;
                    auto model = PlyParser{}.loadAsync(outputPly, loadJob);
                    if (!model)
                        throw std::runtime_error("Failed to load existing output.ply");

                    job->setProgress(1.f);

                    {
                        std::lock_guard lock{state.gaussianMutex};
                        state.gaussianModel = std::move(model);
                        state.committedSplatCount.store(state.gaussianModel->numSplats(),
                                                       std::memory_order_release);
                    }
                }

                job->setStatusText("Done");
                job->markDone();

            } catch (...) {
                job->markDone(std::current_exception());
            }
        });
}

bool VideoImporter::isLoading() const {
    return m_state == State::Running && m_job && !m_job->isDone();
}
bool VideoImporter::isDone() const { return m_job && m_job->isDone(); }

float VideoImporter::progress() const {
    return m_job ? m_job->progress() : 0.f;
}

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
            state.setStatus(std::string("Video import failed: ") + e.what());
        }
    } else {
        std::lock_guard lock{state.gaussianMutex};
        if (state.gaussianModel) {
            state.setStatus(std::format("Imported {} splats", state.gaussianModel->numSplats()));
        }
    }

    m_thread.reset();
    m_job.reset();
    m_state = State::Idle;
    return true;
}
