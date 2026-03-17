#pragma once

struct AppState;

// Modal dialog shown during long-running async jobs.
// Polls AsyncJob::progress() and AsyncJob::statusText() each frame.
// Closes automatically when the job finishes; exposes a Cancel button.
class ProgressModal {
public:
    void draw(AppState& state);

private:
    bool m_opened{false}; // tracks whether OpenPopup has been called this session
};
