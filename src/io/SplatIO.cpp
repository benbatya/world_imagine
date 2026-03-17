#include "SplatIO.hpp"

#include "PlyParser.hpp"
#include "app/AppState.hpp"
#include "model/GaussianModel.hpp"
#include "util/AsyncJob.hpp"

SplatIO& SplatIO::instance() {
    static SplatIO s;
    return s;
}

void SplatIO::loadAsync(const std::filesystem::path& path, AppState& state) {
    if (isLoading())
        return;

    // Join any previous thread before starting a new one
    m_thread.reset();

    auto job = std::make_shared<AsyncJob>();
    m_job    = job;

    m_thread = std::jthread([path, job, &state]() {
        std::shared_ptr<GaussianModel> model;
        try {
            PlyParser parser;
            model = parser.loadAsync(path, *job);
        } catch (...) {
            job->markDone(std::current_exception());
            return;
        }

        if (model) {
            size_t n = model->numSplats();
            {
                std::lock_guard lock{state.gaussianMutex};
                state.gaussianModel = std::move(model);
            }
            state.setStatus("Loaded " + std::to_string(n) + " splats");
        } else {
            state.setStatus("Import cancelled");
        }

        job->setProgress(1.f);
        job->markDone();
    });
}

bool SplatIO::isLoading() const {
    return m_job && !m_job->isDone();
}

bool SplatIO::isDone() const {
    return m_job && m_job->isDone();
}

float SplatIO::progress() const {
    return m_job ? m_job->progress() : 0.f;
}

std::string SplatIO::statusText() const {
    return m_job ? m_job->statusText() : std::string{};
}

void SplatIO::requestCancel() {
    if (m_job)
        m_job->requestCancel();
}

bool SplatIO::finalize(AppState& state) {
    if (!m_job || !m_job->isDone())
        return false;

    if (auto ex = m_job->exception()) {
        try {
            std::rethrow_exception(ex);
        } catch (const std::exception& e) {
            state.setStatus(std::string("Error: ") + e.what());
        } catch (...) {
            state.setStatus("Unknown error during background job");
        }
    }

    m_job.reset();
    m_thread.reset();
    return true;
}

void SplatIO::exportPLY(const GaussianModel& model, const std::filesystem::path& path) {
    PlyParser parser;
    parser.save(model, path);
}
