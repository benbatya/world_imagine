#pragma once
#include <array>

struct AppState;

class MenuOverlay {
  public:
    void draw(AppState& state);

  private:
    bool m_visible{false};

    // Import dialog state
    bool m_showImport{false};
    std::array<char, 512> m_importPath{};

    // Export dialog state
    bool m_showExport{false};
    std::array<char, 512> m_exportPath{};
};
