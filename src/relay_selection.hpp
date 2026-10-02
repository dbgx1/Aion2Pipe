#pragma once
namespace aion {
// Keep the native protocol route at a stable WinDivert priority. Chat HTTPS is
// forwarded explicitly to port 18080 and does not use process-level redirectors.
inline constexpr short defaultProxyPriority=2000;
}
