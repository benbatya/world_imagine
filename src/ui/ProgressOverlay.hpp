#pragma once

struct AppState;

// Non-modal overlay shown at the bottom of the window during long-running async jobs.
// Polls AsyncJob::progress() and AsyncJob::statusText() each frame.
// Disappears automatically when the job finishes; exposes a Cancel button.
class ProgressOverlay {
public:
  void draw(AppState& state);
};
