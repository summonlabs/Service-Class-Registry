// Entry point for the scr command line tool.
//
// On Windows the command line is taken from GetCommandLineW and converted to
// UTF-8, so that non-ASCII paths and identifiers survive; the console output
// code page is set to UTF-8 for the same reason. On other platforms argv is
// used directly.

#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

int run_cli(const std::vector<std::string>& args);

#ifdef _WIN32
namespace {

std::string wide_to_utf8(const wchar_t* text) {
  if (text == nullptr) {
    return std::string();
  }
  const int length = static_cast<int>(wcslen(text));
  if (length == 0) {
    return std::string();
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return std::string();
  }
  std::string utf8(static_cast<std::size_t>(size), '\0');
  static_cast<void>(WideCharToMultiByte(CP_UTF8, 0, text, length, utf8.data(), size, nullptr, nullptr));
  return utf8;
}

}  // namespace
#endif

int main(int argc, char** argv) {
  std::vector<std::string> args;
#ifdef _WIN32
  static_cast<void>(argc);
  static_cast<void>(argv);
  static_cast<void>(SetConsoleOutputCP(CP_UTF8));
  int wide_count = 0;
  LPWSTR* wide_arguments = CommandLineToArgvW(GetCommandLineW(), &wide_count);
  if (wide_arguments != nullptr) {
    // Element zero is the program path; the command line starts at one.
    for (int index = 1; index < wide_count; ++index) {
      args.push_back(wide_to_utf8(wide_arguments[index]));
    }
    LocalFree(wide_arguments);
  }
#else
  args.assign(argv, argv + argc);
#endif
  return run_cli(args);
}
