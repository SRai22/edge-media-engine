#include <gtest/gtest.h>

#include <vector>

#include "eme/processing/h264_annexb.hpp"

using namespace eme;

namespace {

std::vector<std::byte> bytes(std::initializer_list<int> v) {
  std::vector<std::byte> out;
  for (int x : v) out.push_back(static_cast<std::byte>(x));
  return out;
}

}  // namespace

TEST(H264AnnexB, ExtractsSpsAndPpsWithMixedStartCodes) {
  // AUD (9), SPS (7), PPS (8), IDR (5); 4- and 3-byte start codes as MFTs emit them.
  const auto au = bytes({0, 0, 0, 1, 0x09, 0xf0,                    //
                         0, 0, 0, 1, 0x67, 0x42, 0xc0, 0x1f, 0x00,  // SPS ends in a zero byte
                         0, 0, 1, 0x68, 0xce, 0x3c, 0x80,           //
                         0, 0, 1, 0x65, 0x88, 0x84});
  EXPECT_EQ(h264::parameterSets(au), bytes({0, 0, 0, 1, 0x67, 0x42, 0xc0, 0x1f, 0x00, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80}));
  EXPECT_TRUE(h264::containsIdr(au));
}

TEST(H264AnnexB, NonIdrAndGarbage) {
  const auto p = bytes({0, 0, 0, 1, 0x41, 0x9a, 0x02});
  EXPECT_TRUE(h264::parameterSets(p).empty());
  EXPECT_FALSE(h264::containsIdr(p));
  EXPECT_TRUE(h264::parameterSets(bytes({})).empty());
  EXPECT_FALSE(h264::containsIdr(bytes({0x65, 0, 0})));  // no start code: nothing parsed
  EXPECT_FALSE(h264::containsIdr(bytes({0, 0, 1})));     // start code with no NAL header
}
