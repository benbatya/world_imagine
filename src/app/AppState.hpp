#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// Forward-declared to avoid pulling in torch headers everywhere
class GaussianModel;

enum class CameraMode : int { Orbit, Fly };

struct AppState {
    // Loaded 3DGS model — null until imported or trained.
    // Write only from the main thread (or under gaussianMutex from bg thread).
    std::shared_ptr<GaussianModel> gaussianModel;
    std::mutex gaussianMutex;

    // Active camera mode — written and read on the main thread only.
    std::atomic<CameraMode> cameraMode{CameraMode::Orbit};

    // Number of splats committed to gaussianModel so far.
    // Written by the background load thread (atomic); read by Viewport3D each
    // frame to detect mid-load growth without locking gaussianMutex.
    std::atomic<size_t> committedSplatCount{0};

    // Camera pose to apply on next model load (set by VideoImporter from cameras.json).
    // Protected by camPoseMutex. Consumed once by Viewport3D on isNewModel.
    struct CamPose {
        glm::vec3 position{0.f, 0.f, 5.f};
        glm::quat orientation{1.f, 0.f, 0.f, 0.f};
    };
    CamPose    pendingCamPose;
    std::mutex camPoseMutex;
    std::atomic<bool> hasPendingCamPose{false};

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
