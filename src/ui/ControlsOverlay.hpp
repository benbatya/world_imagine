#pragma once

struct AppState;
class Viewport3D;

// Top-right overlay showing camera controls for the active mode.
class ControlsOverlay {
public:
    void draw(AppState& state, Viewport3D& viewport);

private:
    bool m_visible{false};
};
