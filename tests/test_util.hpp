#pragma once

#include <chrono>
#include <thread>

namespace eme::test {

template <class Pred>
bool waitFor(Pred pred, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!pred()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

}  // namespace eme::test

#include <cstddef>
#include <vector>

namespace eme::test {

// NAL unit types in an Annex-B byte stream (3- or 4-byte start codes).
inline std::vector<int> nalTypes(const std::vector<std::byte>& au) {
  std::vector<int> types;
  for (std::size_t i = 0; i + 3 < au.size(); ++i) {
    if (au[i] == std::byte{0} && au[i + 1] == std::byte{0} && au[i + 2] == std::byte{1}) {
      types.push_back(std::to_integer<int>(au[i + 3]) & 0x1f);
      i += 3;
    }
  }
  return types;
}

}  // namespace eme::test
