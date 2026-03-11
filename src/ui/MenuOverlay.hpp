#pragma once

struct AppState;

class MenuOverlay {
public:
    void draw(AppState& state);

private:
    bool m_visible{false};
};
