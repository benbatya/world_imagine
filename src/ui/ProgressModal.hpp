#pragma once

struct AppState;

// Phase 4: Modal dialog shown during long-running async pipeline jobs.
// Polls AsyncJob::progress() and AsyncJob::statusText() each frame.
class ProgressModal {
public:
    void draw(AppState& state);
};
