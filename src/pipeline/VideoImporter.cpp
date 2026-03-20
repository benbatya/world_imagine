#include "VideoImporter.hpp"
#include "ColmapRunner.hpp"
#include "FrameExtractor.hpp"
#include "SplatTrainer.hpp"
#include "app/AppState.hpp"
#include "model/GaussianModel.hpp"
#include "util/AsyncJob.hpp"

#include <chrono>
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
    // Prefer opensplat next to our own executable (set up by CMake install or copy)
    fs::path next2exe = exeDirectory() / "opensplat";
    if (fs::exists(next2exe)) return next2exe;
    // Fall back to build tree location (development)
    fs::path buildTree = exeDirectory().parent_path().parent_path() /
                         "third_party/opensplat/build/opensplat";
    if (fs::exists(buildTree)) return buildTree;
    // Fall back to PATH
    return "opensplat";
}

VideoImporter& VideoImporter::instance() {
    static VideoImporter s;
    return s;
}

void VideoImporter::importAsync(const fs::path& videoPath, AppState& state) {
    if (m_job && !m_job->isDone()) return; // already running

    m_job = std::make_shared<AsyncJob>();

    // Create a unique temp directory for this run
    auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    fs::path tmpDir = fs::temp_directory_path() / std::format("world_imagine_{}", stamp);
    fs::create_directories(tmpDir);

    fs::path framesDir   = tmpDir / "frames";
    fs::path colmapDir   = tmpDir / "colmap";
    fs::path outputPly   = tmpDir / "output.ply";
    std::string opensplat = findOpensplat().string();

    auto job = m_job; // capture shared_ptr

    m_thread.emplace([job, videoPath, framesDir, colmapDir, outputPly, opensplat, &state]() {
        try {
            // --- Stage 1: extract frames (0 – 30%) ---
            job->setStatusText("Extracting frames…");
            auto frames = FrameExtractor{}.run(
                {.videoPath = videoPath, .outputDir = framesDir, .everyNthFrame = 5},
                *job, 0.f, 0.30f);

            if (job->cancelRequested()) { job->markDone(); return; }
            if (frames.empty())
                throw std::runtime_error("No frames extracted from video");

            job->setStatusText(std::format("Extracted {} frames", frames.size()));

            // --- Stage 2: COLMAP SfM (30 – 65%) ---
            fs::path modelPath = ColmapRunner{}.run(
                {.imagePath     = framesDir,
                 .workspacePath = colmapDir,
                 .colmapBin     = "colmap"},
                *job, 0.30f, 0.65f);

            if (job->cancelRequested()) { job->markDone(); return; }
            if (modelPath.empty())
                throw std::runtime_error("COLMAP produced no model");

            // --- Stage 3: train splats (65 – 100%) ---
            auto model = SplatTrainer{}.run(
                {.colmapPath   = modelPath,
                 .outputPly    = outputPly,
                 .opensplatBin = opensplat,
                 .iterations   = 7000},
                *job, 0.65f, 1.f);

            if (job->cancelRequested()) { job->markDone(); return; }
            if (!model)
                throw std::runtime_error("SplatTrainer returned no model");

            // --- Publish result to AppState ---
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
}

bool VideoImporter::isLoading() const { return m_job && !m_job->isDone(); }
bool VideoImporter::isDone()    const { return m_job &&  m_job->isDone(); }

float VideoImporter::progress() const {
    return m_job ? m_job->progress() : 0.f;
}

std::string VideoImporter::statusText() const {
    return m_job ? m_job->statusText() : std::string{};
}

void VideoImporter::requestCancel() {
    if (m_job) m_job->requestCancel();
}

void VideoImporter::cancelAndJoin() {
    if (m_job) m_job->requestCancel();
    m_thread.reset(); // jthread destructor joins
    m_job.reset();
}

bool VideoImporter::finalize(AppState& state) {
    if (!m_job || !m_job->isDone()) return false;

    if (auto ex = m_job->exception()) {
        try {
            std::rethrow_exception(ex);
        } catch (const std::exception& e) {
            state.setStatus(std::string("Video import failed: ") + e.what());
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
    return true;
}
