#include "app/Application.hpp"

#include <cstdio>
#include <exception>

int main() {
    try {
        Application app;
        app.run();
    } catch (const std::exception& e) {
        fprintf(stderr, "[Fatal] %s\n", e.what());
        return 1;
    }
    return 0;
}
