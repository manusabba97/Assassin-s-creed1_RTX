#pragma once

namespace ac1rtx::log {

void open(const wchar_t* path);
void line(const char* fmt, ...);

} // namespace ac1rtx::log
