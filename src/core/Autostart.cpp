#include "core/Autostart.h"

#include <cstdio>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace cppdaemon {

bool setAutostart(bool enabled) {
#ifdef _WIN32
  HKEY key;
  if (RegOpenKeyExA(HKEY_CURRENT_USER,
                    "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                    KEY_SET_VALUE, &key) != ERROR_SUCCESS)
    return false;
  // 取当前 exe 路径（带引号避免空格问题）
  char buf[MAX_PATH] = {0};
  std::string path;
  if (GetModuleFileNameA(nullptr, buf, MAX_PATH)) path = std::string("\"") + buf + "\"";
  LONG r;
  if (enabled)
    r = RegSetValueExA(key, "CppDaemon", 0, REG_SZ, (const BYTE*)path.c_str(),
                       (DWORD)(path.size() + 1));
  else
    r = RegDeleteValueA(key, "CppDaemon");
  RegCloseKey(key);
  return r == ERROR_SUCCESS;
#else
  (void)enabled;
  return true;  // 非 Windows no-op
#endif
}

bool isAutostartEnabled() {
#ifdef _WIN32
  HKEY key;
  if (RegOpenKeyExA(HKEY_CURRENT_USER,
                    "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                    KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
    return false;
  char buf[512] = {0};
  DWORD sz = sizeof(buf);
  LONG r = RegQueryValueExA(key, "CppDaemon", 0, nullptr, (BYTE*)buf, &sz);
  RegCloseKey(key);
  return r == ERROR_SUCCESS;
#else
  return false;
#endif
}

}  // namespace cppdaemon
