#pragma once
// Internal: minimal blocking HTTP for WHIP over libcurl.

#include <chrono>
#include <expected>
#include <string>

#include "eme/core/types.hpp"

namespace eme::detail {

struct HttpResponse {
  long status = 0;
  std::string body;
  std::string location;  // Location header, if any
};

std::expected<HttpResponse, Error> httpRequest(const std::string& method, const std::string& url,
                                               const std::string& bearerToken, const std::string& body,
                                               const std::string& contentType, std::chrono::milliseconds timeout);

// scheme://host[:port], lower-cased; empty if not an absolute URL.
std::string origin(const std::string& url);

// Resolves a Location header against the request URL (absolute, absolute-path or relative).
std::string resolveUrl(const std::string& base, const std::string& location);

}  // namespace eme::detail
