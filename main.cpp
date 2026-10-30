#include <cstdio>
#include <string>
#include <windows.h>

#include "bench.h"
#include "ui_main.h"

int main(int argc, char** argv) {
    std::string which = argc > 1 ? argv[1] : "";
    if (which == "bench" || which == "bench4") {
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        Bench::run(which);
        return 0;
    }
    return UiRun();
}
