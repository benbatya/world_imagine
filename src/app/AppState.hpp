#pragma once
#include <memory>
#include <string>
#include <atomic>
#include <mutex>

// Forward declarations for types added in later phases
// class GaussianModel;
// class AsyncJob;

struct AppState {
    // Active status message shown in the UI (set from any thread)
    std::string     statusMessage{"Ready"};
    std::mutex      statusMutex;

    bool showProgressModal{false};

    void setStatus(std::string msg) {
        std::lock_guard lock{statusMutex};
        statusMessage = std::move(msg);
    }

    std::string getStatus() {
        std::lock_guard lock{statusMutex};
        return statusMessage;
    }
};
