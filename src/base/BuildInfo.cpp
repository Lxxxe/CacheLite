#include "cachelite/base/BuildInfo.h"

#ifndef CACHELITE_VERSION
#define CACHELITE_VERSION "0.0.0-dev"
#endif

namespace cachelite::base {

std::string_view projectName() noexcept {
    return "CacheLite";
}

std::string_view projectVersion() noexcept {
    return CACHELITE_VERSION;
}

}  // namespace cachelite::base

