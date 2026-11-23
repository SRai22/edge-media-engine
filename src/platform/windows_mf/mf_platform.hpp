#pragma once
// Internal: process-wide Media Foundation setup shared by the MF camera source and the MF H.264 encoder.
#include <windows.h>
#include <objbase.h>
#include <mfapi.h>

#include <cstdint>
#include <format>
#include <string>

namespace eme::mf {

inline std::string hr(HRESULT h) { return std::format("0x{:08X}", static_cast<std::uint32_t>(h)); }

inline std::string utf8(const wchar_t* w) {
  if (!w || !*w) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

// One-time process setup. CoIncrementMTAUsage keeps the multithreaded apartment alive for the whole process, so any
// thread that has not chosen an apartment (callers of enumerateCameras/open/encode) is implicitly MTA, and MF objects
// (all free-threaded) can be used and released from any of them. Neither is undone: tearing COM/MF down while static
// destructors or MF work-queue threads still run is the classic exit-time crash.
// The MF DLLs are delay-loaded (CMakeLists.txt): Media Foundation is a removable package on Windows IoT and absent on
// N editions and Server without the feature, and there the process must still run, just without MF cameras/encoders.
inline HRESULT platform() {
  static const HRESULT h = [] {
    for (const wchar_t* dll : {L"mfplat.dll", L"mf.dll", L"mfreadwrite.dll"}) {
      if (!LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) return HRESULT_FROM_WIN32(GetLastError());  // kept loaded
    }
    CO_MTA_USAGE_COOKIE cookie{};
    if (const HRESULT c = CoIncrementMTAUsage(&cookie); FAILED(c)) return c;
    return MFStartup(MF_VERSION, MFSTARTUP_LITE);  // lite: no sockets needed
  }();
  return h;
}

}  // namespace eme::mf
