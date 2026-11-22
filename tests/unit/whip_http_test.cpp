#include <gtest/gtest.h>

#include "eme/transport/whip_publisher.hpp"
#include "transport/whip/http.hpp"

using eme::detail::resolveUrl;

TEST(WhipHttp, ResolvesLocationHeader) {
  EXPECT_EQ(resolveUrl("http://127.0.0.1:8085/w", "/w/abc"), "http://127.0.0.1:8085/w/abc");
  EXPECT_EQ(resolveUrl("http://127.0.0.1:8085/w", "abc"), "http://127.0.0.1:8085/abc");
  EXPECT_EQ(resolveUrl("http://127.0.0.1:8085/whip/endpoint", "res/1"), "http://127.0.0.1:8085/whip/res/1");
  EXPECT_EQ(resolveUrl("http://a:1/w", "https://b:2/x"), "https://b:2/x");
  EXPECT_EQ(resolveUrl("http://a:1", "/x"), "http://a:1/x");
}

TEST(WhipPublisher, StartValidatesConfig) {
  eme::Distributor<eme::EncodedVideoFrame> d;
  eme::WhipPublisher none("whip", {});
  EXPECT_EQ(none.start().error().code, eme::ErrorCode::InvalidState);
  eme::WhipPublisher badUrl("whip", {.endpointUrl = "ftp://x", .bearerToken = "t"});
  ASSERT_TRUE(badUrl.bindInput(d));
  EXPECT_EQ(badUrl.start().error().code, eme::ErrorCode::InvalidArgument);
  eme::WhipPublisher noToken("whip", {.endpointUrl = "http://127.0.0.1:9/w"});
  ASSERT_TRUE(noToken.bindInput(d));
  EXPECT_EQ(noToken.start().error().code, eme::ErrorCode::InvalidArgument);
  eme::WhipPublisher refused("whip", {.endpointUrl = "http://127.0.0.1:9/w", .bearerToken = "t",
                                      .setupTimeout = std::chrono::seconds(3)});
  ASSERT_TRUE(refused.bindInput(d));
  EXPECT_EQ(refused.start().error().code, eme::ErrorCode::Unsupported);  // nothing listens on port 9
}

TEST(WhipHttp, OriginComparisonGuardsTheToken) {
  EXPECT_EQ(eme::detail::origin("HTTP://Host:8085/w/x"), "http://host:8085");
  EXPECT_NE(eme::detail::origin("http://evil:1/x"), eme::detail::origin("http://127.0.0.1:8085/w"));
  EXPECT_NE(eme::detail::origin("http://host/x"), eme::detail::origin("https://host/w"));  // downgrade
  EXPECT_EQ(eme::detail::origin("no-scheme"), "");
}
