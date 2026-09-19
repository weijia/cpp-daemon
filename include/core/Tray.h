#pragma once

#include <memory>
#include <string>

namespace cppdaemon {

class App;  // 前向声明

// 系统托盘（Win32 NOTIFYICONDATA + 隐藏窗口消息循环）。
// HEADLESS 构建下为 no-op，由 App 以信号驱动退出。
class Tray {
public:
  explicit Tray(App* app);
  ~Tray();

  // 启动托盘并阻塞，直到用户退出或 App::quit 被调用。
  void run();
  // 供外部（如 App::quit）唤醒消息循环退出。
  void quit();

  // 注册一个插件托盘菜单项（label -> 点击回调由 App 转发到插件）。
  void addPluginItem(const std::string& itemId, const std::string& label,
                     const std::string& tooltip);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cppdaemon
