#pragma once

#include "cachelite/protocol/RespValue.h"

#include <string>

namespace cachelite::protocol {

class RespEncoder {
public:
    [[nodiscard]] static std::string encode(const RespValue& value);
};

}  // namespace cachelite::protocol
