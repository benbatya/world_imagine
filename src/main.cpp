#include "app/Application.hpp"

#include <cstdio>
#include <exception>

int main(int argc, char* argv[]) {
  try {
    Application app{argc, argv};
    app.run();
  } catch (const std::exception& e) {
    fprintf(stderr, "[Fatal] %s\n", e.what());
    return 1;
  }
  return 0;
}
