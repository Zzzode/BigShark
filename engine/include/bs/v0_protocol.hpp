#pragma once

#include <bs/decision.hpp>
#include <string>

namespace bs::v0 {

Ctx parseRequest(const std::string& json);
std::string serializeResponse(const Decision& decision);

}  // namespace bs::v0
