#include <flutter/dart_project.h>
#include <flutter/flutter_view_controller.h>
#include <windows.h>
#include <string>

#include "flutter_window.h"
#include "utils.h"

std::wstring GetExecutableDir() {
  wchar_t buffer[MAX_PATH];
  DWORD size = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  if (size == 0 || size == MAX_PATH) {
    return L"";
  }
  std::wstring path(buffer, size);
  size_t pos = path.find_last_of(L"\\/");
  if (pos == std::wstring::npos) {
    return L"";
  }
  return path.substr(0, pos);
}

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE prev,
                      _In_ wchar_t *command_line, _In_ int show_command) {
  // Attach to console when present (e.g., 'flutter run') or create a
  // new console when running with a debugger.
  if (!::AttachConsole(ATTACH_PARENT_PROCESS) && ::IsDebuggerPresent()) {
    CreateAndAttachConsole();
  }

  std::wstring exe_dir = GetExecutableDir();
  if (!exe_dir.empty()) {
    SetCurrentDirectoryW(exe_dir.c_str());
    SetDllDirectoryW(exe_dir.c_str());
  }

  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  flutter::DartProject project(L"data");

  std::vector<std::string> command_line_arguments =
      GetCommandLineArguments();

  project.set_dart_entrypoint_arguments(std::move(command_line_arguments));

  FlutterWindow window(project);
  Win32Window::Point origin(10, 10);
  Win32Window::Size size(1280, 720);
  if (!window.Create(L"gamemapstool", origin, size)) {
    return EXIT_FAILURE;
  }
  window.SetQuitOnClose(true);

  ::MSG msg;
  while (::GetMessage(&msg, nullptr, 0, 0)) {
    ::TranslateMessage(&msg);
    ::DispatchMessage(&msg);
  }

  ::CoUninitialize();
  return EXIT_SUCCESS;
}
