#include "core/Tray.h"
#include "core/App.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef CPPDAEMON_HEADLESS
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <shellapi.h>
#endif
#endif

namespace cppdaemon {

namespace {

constexpr int kIdmOpen = 1;
constexpr int kIdmQuit = 2;
constexpr int kIdmPluginBase = 100;

struct Item {
  std::string itemId;
  std::string label;
  std::string tooltip;
};

// UTF-8 -> UTF-16（源码以 /utf-8 编译，字面量为 UTF-8）
std::wstring utf8ToWide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  if (n == 0) return {};
  std::wstring w(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
  return w;
}

// 从嵌入资源读取 appicon_tray.png，包成 ICO 容器，取 HICON
// （与 go-daemon 的 pngToICO 完全一致的 ICO 布局）
HICON loadTrayIcon() {
  HMODULE hMod = GetModuleHandleA(nullptr);
  HRSRC hRes = FindResourceA(hMod, "IDR_TRAY_PNG", RT_RCDATA);
  if (!hRes) return nullptr;
  HGLOBAL hGlob = LoadResource(hMod, hRes);
  if (!hGlob) return nullptr;
  DWORD pngSize = SizeofResource(hMod, hRes);
  const BYTE* png = (const BYTE*)LockResource(hGlob);
  if (!png || pngSize == 0) return nullptr;

  std::vector<BYTE> ico;
  ico.reserve(22 + pngSize);
  auto put16 = [&](uint16_t v) {
    ico.push_back((BYTE)(v & 0xFF));
    ico.push_back((BYTE)((v >> 8) & 0xFF));
  };
  auto put32 = [&](uint32_t v) {
    ico.push_back((BYTE)(v & 0xFF));
    ico.push_back((BYTE)((v >> 8) & 0xFF));
    ico.push_back((BYTE)((v >> 16) & 0xFF));
    ico.push_back((BYTE)((v >> 24) & 0xFF));
  };
  put16(0);     // reserved
  put16(1);     // type = icon
  put16(1);     // count = 1
  ico.push_back(0);  // width
  ico.push_back(0);  // height
  ico.push_back(0);  // colors
  ico.push_back(0);  // reserved
  put16(1);     // planes
  put16(32);    // bpp
  put32(pngSize);  // image size
  put32(22);    // image data offset = 6(header) + 16(direntry)
  ico.insert(ico.end(), png, png + pngSize);

  return CreateIconFromResourceEx(ico.data(), (DWORD)ico.size(), TRUE,
                                  0x00030000, 0, 0, LR_DEFAULTCOLOR);
}

}  // namespace

class Tray::Impl {
public:
  explicit Impl(App* app) : app_(app) {}
  ~Impl() = default;

  void addItem(const std::string& itemId, const std::string& label,
               const std::string& tooltip) {
    items_.push_back({itemId, label, tooltip});
#ifdef _WIN32
#ifndef CPPDAEMON_HEADLESS
    if (hwnd_) addMenuItem(items_.size() - 1);
#endif
#endif
  }

  void run() {
#ifdef _WIN32
#ifndef CPPDAEMON_HEADLESS
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = &WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CppDaemonTrayClass";
    RegisterClassW(&wc);
    hwnd_ = CreateWindowW(L"CppDaemonTrayClass", L"cpp-daemon", 0, 0, 0, 0, 0,
                          nullptr, nullptr, wc.hInstance, this);
    if (!hwnd_) {
      std::fprintf(stderr, "[tray] 创建窗口失败\n");
      return;
    }
    NOTIFYICONDATAW nid = {0};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP + 2;
    nid.hIcon = loadTrayIcon();
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    std::wstring tip = utf8ToWide("cpp-daemon 插件化上报代理");
    wcsncpy(nid.szTip, tip.c_str(), 127);
    nid.szTip[127] = 0;
    Shell_NotifyIconW(NIM_ADD, &nid);

    buildMenu();
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
      if (msg.message == WM_APP + 1) break;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (nid.hIcon) DestroyIcon(nid.hIcon);
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
#endif
#endif
  }

  void quit() {
#ifdef _WIN32
#ifndef CPPDAEMON_HEADLESS
    if (hwnd_) PostMessageW(hwnd_, WM_APP + 1, 0, 0);
#endif
#endif
  }

private:
  App* app_ = nullptr;
#ifdef _WIN32
#ifndef CPPDAEMON_HEADLESS
  HWND hwnd_ = nullptr;
  HMENU menu_ = nullptr;
  std::vector<Item> items_;

  void buildMenu() {
    menu_ = CreatePopupMenu();
    AppendMenuW(menu_, MF_STRING, kIdmOpen, utf8ToWide("打开配置").c_str());
    AppendMenuW(menu_, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < items_.size(); ++i) addMenuItem(i);
    AppendMenuW(menu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu_, MF_STRING, kIdmQuit, utf8ToWide("退出").c_str());
  }

  void addMenuItem(size_t idx) {
    if (!menu_) return;
    AppendMenuW(menu_, MF_STRING, kIdmPluginBase + (UINT)idx,
               utf8ToWide(items_[idx].label).c_str());
  }

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Impl* self = nullptr;
    if (msg == WM_NCCREATE) {
      self = (Impl*)((CREATESTRUCTW*)lp)->lpCreateParams;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
      self = (Impl*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_APP + 2) {  // 托盘图标消息
      if (lp == WM_RBUTTONUP) {
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd);
        TrackPopupMenu(self->menu_, TPM_RIGHTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y,
                       0, hwnd, nullptr);
      } else if (lp == WM_LBUTTONDBLCLK) {
        self->openConfig();
      }
      return 0;
    }
    if (msg == WM_COMMAND) {
      int id = LOWORD(wp);
      if (id == kIdmOpen) self->openConfig();
      else if (id == kIdmQuit) self->app_->quit();
      else if (id >= kIdmPluginBase) {
        size_t idx = (size_t)id - kIdmPluginBase;
        if (idx < self->items_.size())
          self->app_->forwardTrayClick(self->items_[idx].itemId);
      }
      return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  void openConfig() {
    std::string addr = app_->config().http.listen;
    std::wstring url = utf8ToWide("http://" + addr + "/");
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
#endif
#endif
};

Tray::Tray(App* app) : impl_(std::make_unique<Impl>(app)) {}
Tray::~Tray() = default;

void Tray::run() { impl_->run(); }
void Tray::quit() { impl_->quit(); }
void Tray::addPluginItem(const std::string& itemId, const std::string& label,
                         const std::string& tooltip) {
  impl_->addItem(itemId, label, tooltip);
}

}  // namespace cppdaemon
