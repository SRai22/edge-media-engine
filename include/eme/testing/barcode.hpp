#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace eme::mock {

// 64-bit value drawn into the top 64 luma rows as two bands of 64 cells: the value, then its bitwise
// inverse as a check. Cells are width/64 px wide and 32 px tall, large enough to survive H.264 at
// ~1 Mbps. Used to measure capture->decode latency across processes (value = monotonic µs).
inline constexpr int kBarcodeRows = 64;

inline bool barcodeFits(int width, int height) { return width >= 64 * 8 && height >= kBarcodeRows * 2; }

inline void writeBarcode(std::uint8_t* luma, int width, int stride, std::uint64_t value) {
  const int cell = width / 64;
  for (int band = 0; band < 2; ++band) {
    for (int bit = 0; bit < 64; ++bit) {
      const bool one = ((value >> (63 - bit)) & 1) != (band == 1);
      const std::uint8_t v = one ? 235 : 16;
      for (int y = band * 32; y < band * 32 + 32; ++y) {
        std::uint8_t* row = luma + static_cast<std::ptrdiff_t>(y) * stride + bit * cell;
        for (int x = 0; x < cell; ++x) row[x] = v;
      }
    }
  }
}

inline std::optional<std::uint64_t> readBarcode(const std::uint8_t* luma, int width, int stride) {
  const int cell = width / 64;
  auto sample = [&](int band, int bit) {
    int sum = 0;  // 4x4 average at the cell centre
    for (int dy = -2; dy < 2; ++dy)
      for (int dx = -2; dx < 2; ++dx)
        sum += luma[static_cast<std::ptrdiff_t>(band * 32 + 16 + dy) * stride + bit * cell + cell / 2 + dx];
    return sum / 16;
  };
  std::uint64_t value = 0;
  for (int bit = 0; bit < 64; ++bit) {
    const int a = sample(0, bit), b = sample(1, bit);
    const bool one = a > 128;
    if (one == (b > 128) || (a > 96 && a < 160) || (b > 96 && b < 160)) return std::nullopt;  // ambiguous or check failed
    value = (value << 1) | (one ? 1u : 0u);
  }
  return value;
}

}  // namespace eme::mock
