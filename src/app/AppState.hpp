#pragma once
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

  // Status bar text (safe to write from any thread)
  std::string statusMessage{"Ready"};
  std::mutex statusMutex;

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
