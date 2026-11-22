#include <gtest/gtest.h>

#include "eme/core/frame_pool.hpp"

using eme::FramePool;

TEST(FramePool, BoundedAndNonBlocking) {
  FramePool pool(64, 2);
  auto a = pool.acquire();
  auto b = pool.acquire();
  ASSERT_TRUE(a && b);
  EXPECT_EQ(a->cpuData().size(), 64u);
  EXPECT_EQ(pool.acquire(), nullptr);
  EXPECT_EQ(pool.outstanding(), 2u);
}

TEST(FramePool, RecyclesReleasedBuffers) {
  FramePool pool(64, 2);
  const eme::CpuBuffer* first = nullptr;
  {
    auto a = pool.acquire();
    first = a.get();
  }
  EXPECT_EQ(pool.outstanding(), 0u);
  auto again = pool.acquire();
  EXPECT_EQ(again.get(), first);
  EXPECT_EQ(pool.allocated(), 1u);
}

TEST(FramePool, BufferMayOutlivePool) {
  std::shared_ptr<eme::CpuBuffer> survivor;
  {
    FramePool pool(64, 1);
    survivor = pool.acquire();
  }
  survivor->data()[0] = std::byte{1};  // still valid; freed when survivor drops (ASan checks this)
  survivor.reset();
}
