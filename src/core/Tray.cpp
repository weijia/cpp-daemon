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
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
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

// 从嵌入资源读取 appicon_tray.png，用 GDI+ 解码为带 alpha 的 HICON。
// （与 go-daemon 共用同一张 PNG；GDI+ 能正确处理 32 位带透明通道 PNG，
//   比手动把 PNG 包成 ICO 再 CreateIconFromResourceEx 可靠得多。）
HICON loadTrayIcon() {
  HMODULE hMod = GetModuleHandleA(nullptr);
  HRSRC hRes = FindResourceA(hMod, "IDR_TRAY_PNG", RT_RCDATA);
  if (!hRes) return nullptr;
  HGLOBAL hGlob = LoadResource(hMod, hRes);
  if (!hGlob) return nullptr;
  DWORD pngSize = SizeofResource(hMod, hRes);
  const BYTE* png = (const BYTE*)LockResource(hGlob);
  if (!png || pngSize == 0) return nullptr;

  // 把资源内存包成 IStream 交给 GDI+ 解码
  HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, pngSize);
  if (!hMem) return nullptr;
  void* pMem = GlobalLock(hMem);
  memcpy(pMem, png, pngSize);
  GlobalUnlock(hMem);
  IStream* pStream = nullptr;
  if (FAILED(CreateStreamOnHGlobal(hMem, TRUE, &pStream)) || !pStream) {
    GlobalFree(hMem);
    return nullptr;
  }

  Gdiplus::GdiplusStartupInput gsi;
  ULONG_PTR token = 0;
  Gdiplus::GdiplusStartup(&token, &gsi, nullptr);

  Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromStream(pStream);
  pStream->Release();
  HICON hIcon = nullptr;
  if (bmp && bmp->GetLastStatus() == Gdiplus::Ok) {
    // 缩放到 32x32，托盘显示更清晰
    Gdiplus::Bitmap scaled(32, 32, PixelFormat32bppARGB);
    Gdiplus::Graphics g(&scaled);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.DrawImage(bmp, 0, 0, 32, 32);
    scaled.GetHICON(&hIcon);
  }
  delete bmp;
  Gdiplus::GdiplusShutdown(token);
  return hIcon;
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
      else if (id == kIdmQuit) self->app_->requestQuit();
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
