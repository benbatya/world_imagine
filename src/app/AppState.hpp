#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

// Forward-declared to avoid pulling in torch headers everywhere
class GaussianModel;

struct AppState {
    // Loaded 3DGS model — null until imported or trained.
    // Write only from the main thread (or under gaussianMutex from bg thread).
    std::shared_ptr<GaussianModel> gaussianModel;
    std::mutex gaussianMutex;

    // Number of splats committed to gaussianModel so far.
    // Written by the background load thread (atomic); read by Viewport3D each
    // frame to detect mid-load growth without locking gaussianMutex.
    std::atomic<size_t> committedSplatCount{0};

    // Status bar text (safe to write from any thread)
    std::string statusMessage{"Ready"};
    std::mutex statusMutex;

    void setStatus(std::string msg) {
        std::lock_guard lock{statusMutex};
        statusMessage = std::move(msg);
    }

    std::string getStatus() {
        std::lock_guard lock{statusMutex};
        return statusMessage;
    }
};
