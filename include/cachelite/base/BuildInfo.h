#pragma once

#include <string_view>

namespace cachelite::base {

[[nodiscard]] std::string_view projectName() noexcept;
[[nodiscard]] std::string_view projectVersion() noexcept;

}  // namespace cachelite::base

