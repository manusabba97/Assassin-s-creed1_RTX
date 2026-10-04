#include "screenshot.h"

#include "log.h"

#include <windows.h>

#include <algorithm>
#include <thread>

// gdiplus.h uses unqualified min/max, which NOMINMAX removes from windows.h.
using std::max;
using std::min;
#include <objidl.h>
#include <gdiplus.h>

namespace ac1rtx::screenshot {

namespace {

// Image/png encoder CLSID of GDI+.
const CLSID kPngEncoder = { 0x557CF406, 0x1A04, 0x11D3, { 0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E } };

bool foregroundIsOurs() {
  HWND hwnd = GetForegroundWindow();
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  return hwnd && pid == GetCurrentProcessId();
}

void capture(const std::wstring& directory) {
  HWND hwnd = GetForegroundWindow();
  RECT rect = {};
  if (!GetWindowRect(hwnd, &rect)) {
    return;
  }
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;

  HDC screen = GetDC(nullptr);
  HDC memory = CreateCompatibleDC(screen);
  HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
  HGDIOBJ previous = SelectObject(memory, bitmap);
  BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top, SRCCOPY | CAPTUREBLT);
  SelectObject(memory, previous);

  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t name[64];
  swprintf_s(name, L"\\%04u%02u%02u_%02u%02u%02u.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  const std::wstring path = directory + name;

  Gdiplus::Status status;
  {
    Gdiplus::Bitmap image(bitmap, nullptr);
    status = image.Save(path.c_str(), &kPngEncoder, nullptr);
  }
  DeleteObject(bitmap);
  DeleteDC(memory);
  ReleaseDC(nullptr, screen);
  log::line("screenshot %ls (%dx%d) -> %s", path.c_str(), width, height, status == Gdiplus::Ok ? "saved" : "FAILED");
}

void watch(int virtualKey, std::wstring directory) {
  Gdiplus::GdiplusStartupInput input;
  ULONG_PTR token = 0;
  if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
    log::line("GdiplusStartup failed; screenshots disabled");
    return;
  }
  CreateDirectoryW(directory.c_str(), nullptr);
  bool wasDown = false;
  for (;;) {
    Sleep(20);
    const bool down = (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    if (down && !wasDown && foregroundIsOurs()) {
      capture(directory);
    }
    wasDown = down;
  }
}

} // namespace

void start(int virtualKey, const std::wstring& directory) {
  std::thread(watch, virtualKey, directory).detach();
  log::line("screenshot key 0x%02X -> %ls", virtualKey, directory.c_str());
}

} // namespace ac1rtx::screenshot
