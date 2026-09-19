#pragma once

namespace cppdaemon {

// 开机自启：Windows 注册表 HKCU\...\Run；非 Windows / HEADLESS 为 no-op。
bool setAutostart(bool enabled);
bool isAutostartEnabled();

}  // namespace cppdaemon
