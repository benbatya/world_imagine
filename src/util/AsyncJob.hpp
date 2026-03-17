#pragma once
#include <atomic>
#include <exception>
#include <mutex>
#include <string>

// Thread-safe job handle shared between a background worker and the UI.
// Worker calls setProgress/setStatusText/markDone.
// UI reads progress/statusText/isDone; Cancel button calls requestCancel.
class AsyncJob {
public:
    // ---- Writer API (background thread) ------------------------------------

    void setProgress(float p) { m_progress.store(p, std::memory_order_relaxed); }

    void setStatusText(std::string s) {
        std::lock_guard lock{m_mu};
        m_statusText = std::move(s);
    }

    // Must be called exactly once when the worker exits (success or failure).
    void markDone(std::exception_ptr ex = nullptr) {
        if (ex) {
            std::lock_guard lock{m_mu};
            m_exception = ex;
        }
        m_done.store(true, std::memory_order_release);
    }

    // ---- Reader API (main / UI thread) ------------------------------------

    float progress() const { return m_progress.load(std::memory_order_relaxed); }

    std::string statusText() const {
        std::lock_guard lock{m_mu};
        return m_statusText;
    }

    bool isDone() const { return m_done.load(std::memory_order_acquire); }

    // Returns null if the job finished without error.
    std::exception_ptr exception() const {
        std::lock_guard lock{m_mu};
        return m_exception;
    }

    // ---- Cancel (either thread) -------------------------------------------

    void requestCancel() { m_cancelRequested.store(true, std::memory_order_relaxed); }
    bool cancelRequested() const { return m_cancelRequested.load(std::memory_order_relaxed); }

private:
    std::atomic<float> m_progress{0.f};
    std::atomic<bool>  m_cancelRequested{false};
    std::atomic<bool>  m_done{false};

    mutable std::mutex m_mu;
    std::string        m_statusText;
    std::exception_ptr m_exception;
};
