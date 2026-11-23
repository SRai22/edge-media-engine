#pragma once
// Annex-B H.264 helpers for encoders that emit SPS/PPS in-band (Media Foundation MFTs).

#include <cstddef>
#include <span>
#include <vector>

namespace eme::h264 {

// Calls fn(nalType, nalWithStartCode) for every NAL unit in an Annex-B byte stream (3- or 4-byte start codes).
template <class Fn>
void forEachNal(std::span<const std::byte> au, Fn&& fn) {
  auto startCode = [&](std::size_t i) -> std::size_t {  // length of the start code at i, or 0
    if (i + 3 <= au.size() && au[i] == std::byte{0} && au[i + 1] == std::byte{0}) {
      if (au[i + 2] == std::byte{1}) return 3;
      if (i + 4 <= au.size() && au[i + 2] == std::byte{0} && au[i + 3] == std::byte{1}) return 4;
    }
    return 0;
  };
  std::size_t begin = au.size();
  for (std::size_t i = 0; i < au.size(); ++i) {
    if (startCode(i)) {
      begin = i;
      break;
    }
  }
  while (begin < au.size()) {
    const std::size_t header = begin + startCode(begin);
    std::size_t end = au.size();
    for (std::size_t i = header + 1; i < au.size(); ++i) {
      if (startCode(i)) {
        end = i;
        break;
      }
    }
    if (header < end) fn(std::to_integer<int>(au[header]) & 0x1f, au.subspan(begin, end - begin));
    begin = end;
  }
}

inline bool containsIdr(std::span<const std::byte> au) {
  bool idr = false;
  forEachNal(au, [&](int type, auto) { idr |= type == 5; });
  return idr;
}

// The SPS (7) and PPS (8) NAL units of an access unit, start codes kept; empty if it has none.
inline std::vector<std::byte> parameterSets(std::span<const std::byte> au) {
  std::vector<std::byte> out;
  forEachNal(au, [&](int type, std::span<const std::byte> nal) {
    if (type == 7 || type == 8) out.insert(out.end(), nal.begin(), nal.end());
  });
  return out;
}

}  // namespace eme::h264
