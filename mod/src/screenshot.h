#pragma once

#include <string>

namespace ac1rtx::screenshot {

// Starts a watcher thread: pressing `virtualKey` while a window of this process is in the foreground saves that
// window's on-screen pixels (the Remix output) as <directory>\<yyyyMMdd_HHmmss>.png.
void start(int virtualKey, const std::wstring& directory);

} // namespace ac1rtx::screenshot
