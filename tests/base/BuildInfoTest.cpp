#include "cachelite/base/BuildInfo.h"

#include <iostream>

int main() {
    if (cachelite::base::projectName() != "CacheLite") {
        std::cerr << "unexpected project name\n";
        return 1;
    }

    if (cachelite::base::projectVersion().empty()) {
        std::cerr << "project version must not be empty\n";
        return 1;
    }

    return 0;
}

