#include "http.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>

namespace eme::detail {
namespace {

std::size_t onBody(char* data, std::size_t size, std::size_t n, void* user) {
  auto* out = static_cast<std::string*>(user);
  constexpr std::size_t kMax = 256 * 1024;  // SDP answers are a few KB; bound untrusted input
  if (out->size() + size * n > kMax) return 0;
  out->append(data, size * n);
  return size * n;
}

std::size_t onHeader(char* data, std::size_t size, std::size_t n, void* user) {
  std::string line(data, size * n);
  if (line.size() > 9) {
    std::string key = line.substr(0, 9);
    for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (key == "location:") {
      auto v = line.substr(9);
      const auto b = v.find_first_not_of(" \t");
      const auto e = v.find_last_not_of(" \t\r\n");
      *static_cast<std::string*>(user) = b == std::string::npos ? "" : v.substr(b, e - b + 1);
    }
  }
  return size * n;
}

}  // namespace

std::expected<HttpResponse, Error> httpRequest(const std::string& method, const std::string& url,
                                               const std::string& bearerToken, const std::string& body,
                                               const std::string& contentType, std::chrono::milliseconds timeout) {
  static std::once_flag init;
  std::call_once(init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
  if (!curl) return fail(ErrorCode::Internal, "curl init failed");

  HttpResponse res;
  curl_slist* headers = nullptr;
  const std::string auth = "Authorization: Bearer " + bearerToken;
  const std::string type = "Content-Type: " + contentType;
  headers = curl_slist_append(headers, auth.c_str());
  headers = curl_slist_append(headers, "Expect:");  // no 100-continue stall on SDP-sized bodies
  if (!body.empty()) headers = curl_slist_append(headers, type.c_str());
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headerGuard(headers, &curl_slist_free_all);

  CURL* h = curl.get();
  curl_easy_setopt(h, CURLOPT_URL, url.c_str());
  curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method.c_str());
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
  if (!body.empty()) {
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  }
  curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout.count()));
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(std::min<long long>(timeout.count(), 3000)));
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, onBody);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &res.body);
  curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, onHeader);
  curl_easy_setopt(h, CURLOPT_HEADERDATA, &res.location);
  if (const auto rc = curl_easy_perform(h); rc != CURLE_OK) {
    return fail(ErrorCode::Unsupported, std::string("HTTP ") + method + " failed: " + curl_easy_strerror(rc));
  }
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &res.status);
  return res;
}

std::string origin(const std::string& url) {
  const auto scheme = url.find("://");
  if (scheme == std::string::npos) return {};
  const auto pathStart = url.find('/', scheme + 3);
  std::string o = pathStart == std::string::npos ? url : url.substr(0, pathStart);
  for (auto& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return o;
}

std::string resolveUrl(const std::string& base, const std::string& location) {
  if (location.starts_with("http://") || location.starts_with("https://")) return location;
  const auto scheme = base.find("://");
  const auto pathStart = scheme == std::string::npos ? std::string::npos : base.find('/', scheme + 3);
  const std::string origin = pathStart == std::string::npos ? base : base.substr(0, pathStart);
  if (location.starts_with('/')) return origin + location;
  const auto lastSlash = base.rfind('/');
  return (lastSlash == std::string::npos || lastSlash < scheme + 3 ? base + "/" : base.substr(0, lastSlash + 1)) + location;
}

}  // namespace eme::detail
