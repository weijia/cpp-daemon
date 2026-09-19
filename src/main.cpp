#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "core/App.h"
#include "core/Config.h"

namespace {

std::string configPath() {
  std::string exeDir;
#ifdef _WIN32
  char buf[MAX_PATH] = {0};
  if (GetModuleFileNameA(nullptr, buf, MAX_PATH)) {
    std::string s(buf);
    exeDir = s.substr(0, s.find_last_of("/\\"));
  }
#else
  char buf[1024] = {0};
  if (readlink("/proc/self/exe", buf, sizeof(buf)) > 0) {
    std::string s(buf);
    exeDir = s.substr(0, s.find_last_of("/\\"));
  }
#endif
  if (exeDir.empty()) return "config.json";
  return exeDir + "/config.json";
}

}  // namespace

#ifdef _WIN32
#ifndef CPPDAEMON_HEADLESS
// GUI 子系统（WIN32_EXECUTABLE ON）需要 WinMain 入口；CRT 仍会填充 __argc/__argv
int WINAPI WinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int) {
  int argc = __argc;
  char** argv = __argv;
#else
int main(int argc, char** argv) {
#endif
#else
int main(int argc, char** argv) {
#endif
  std::string httpOverride;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-http" && i + 1 < argc) {
      httpOverride = argv[++i];
    } else if (a.rfind("-http=", 0) == 0) {
      httpOverride = a.substr(6);
    }
  }

  std::string path = configPath();
  cppdaemon::App app(path);
  if (!app.init()) {
    std::fprintf(stderr, "初始化失败\n");
    return 1;
  }
  if (!httpOverride.empty()) {
    app.setListen(httpOverride);
    std::printf("HTTP 监听地址已由命令行覆盖为 %s（仅本进程生效，不写盘）\n",
                httpOverride.c_str());
  }
  app.start();
  app.run();
  return 0;
}
