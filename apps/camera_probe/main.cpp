// Lists cameras and the modes eme can capture from (PLAN §5.2 / M0 capability report). Prints JSON.
#include <iostream>
#include <nlohmann/json.hpp>

#include "eme/sources/camera.hpp"

int main() {
  nlohmann::json out = nlohmann::json::array();
  for (const auto& cam : eme::enumerateCameras()) {
    nlohmann::json modes = nlohmann::json::array();
    for (const auto& m : cam.modes) modes.push_back({{"format", m.fourcc}, {"width", m.width}, {"height", m.height}, {"fps", m.fps}});
    out.push_back({{"id", cam.id}, {"name", cam.name}, {"bus", cam.bus}, {"modes", modes}});
  }
  std::cout << out.dump(2) << std::endl;
}
