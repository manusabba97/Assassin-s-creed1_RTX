#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace ac1rtx::log {

namespace {
FILE* s_file = nullptr;
std::mutex s_mutex;
}

void open(const wchar_t* path) {
  std::lock_guard lock { s_mutex };
  s_file = _wfopen(path, L"a");
  if (s_file) {
    fprintf(s_file, "\n===== session pid %lu =====\n", GetCurrentProcessId());
  }
}

void line(const char* fmt, ...) {
  std::lock_guard lock { s_mutex };
  if (!s_file) {
    return;
  }
  SYSTEMTIME t;
  GetLocalTime(&t);
  fprintf(s_file, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  va_list args;
  va_start(args, fmt);
  vfprintf(s_file, fmt, args);
  va_end(args);
  fputc('\n', s_file);
  fflush(s_file);
}

} // namespace ac1rtx::log
