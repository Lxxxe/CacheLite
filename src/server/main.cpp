#include "cachelite/base/BuildInfo.h"

#include <iostream>

int main() {
    std::cout << cachelite::base::projectName()
              << " "
              << cachelite::base::projectVersion()
              << "\n";
    std::cout << "M0 engineering skeleton is ready.\n";
    return 0;
}

