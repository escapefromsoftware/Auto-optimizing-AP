#pragma once

#include "wifiopt/core.hpp"

#include <span>
#include <string>
#include <vector>

namespace wifiopt {

struct PhysicalApAssessment {
  bool possibly_same{};
  std::vector<std::string> evidence;
  std::string explanation;
};

PhysicalApAssessment assess_same_physical_ap(const AccessPoint& first,
                                             const AccessPoint& second);
std::string build_ap_recommendation(std::span<const RankedAccessPoint> access_points);

} // namespace wifiopt
