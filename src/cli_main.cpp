#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "FatReader.h"
#include "MFTReader.h"
#include "exFatReader.h"
#include <algorithm>
#include <cwctype>
#include <iostream>
#include <locale.h>
#include <regex>
#include <string>
#include <vector>
#include <wchar.h>
#include <windows.h>

static void PrintHelp() {
  std::wcout << L"FastFileSearch CLI - Search files using regex patterns\n"
             << L"\n"
             << L"Usage: ffs <pattern> [drive|folder]\n"
             << L"\n"
             << L"Arguments:\n"
             << L"  <pattern>       Regex pattern to match file names\n"
             << L"  [drive|folder]  Drive (e.g. C:) or folder path to "
                L"search\n"
             << L"                  Default: current directory\n"
             << L"\n"
             << L"Note: Administrator privileges required to scan raw volumes.\n"
             << L"\n"
             << L"Options:\n"
             << L"  -h, --help      Show this help message\n"
             << L"  --              Treat the following arguments as positional\n"
             << L"\n"
             << L"Examples:\n"
             << L"  ffs \".*\\\\.cpp$\"                Search current folder for "
                L".cpp files\n"
             << L"  ffs \".*\\\\.exe$\" D:             Search D: drive for .exe "
                L"files\n"
             << L"  ffs \"test_.*\" C:\\Projects       Search C:\\Projects for "
                L"files starting with test_\n";
}

// Converts a std::regex_error message (ASCII) to a wide string for wcerr.
static std::wstring WidenMessage(const char *msg) {
  int len = MultiByteToWideChar(CP_ACP, 0, msg, -1, nullptr, 0);
  if (len <= 1)
    return L"invalid regular expression";
  std::wstring out(len - 1, L'\0');
  MultiByteToWideChar(CP_ACP, 0, msg, -1, &out[0], len);
  return out;
}

// Normalizes the target into an absolute "X:\..." path with a validated drive.
// Returns false (after printing a diagnostic) if the target is unusable.
static bool NormalizeTarget(const std::wstring &raw, std::wstring &target,
                            wchar_t &drive) {
  if (raw.empty()) {
    std::wcerr << L"Error: target path is empty" << std::endl;
    return false;
  }

  std::wstring work = raw;
  // "C:" alone means the root of that drive, not its current directory.
  if (work.size() == 2 && work[1] == L':')
    work.push_back(L'\\');

  DWORD needed = GetFullPathNameW(work.c_str(), 0, nullptr, nullptr);
  if (needed == 0) {
    std::wcerr << L"Error: cannot resolve target '" << raw << L"' ("
               << GetLastError() << L")" << std::endl;
    return false;
  }
  std::vector<wchar_t> buf(needed + 1, L'\0');
  DWORD written = GetFullPathNameW(work.c_str(), (DWORD)buf.size(), buf.data(),
                                   nullptr);
  if (written == 0 || written >= buf.size()) {
    std::wcerr << L"Error: cannot resolve target '" << raw << L"'" << std::endl;
    return false;
  }
  target.assign(buf.data(), written);

  // Drop a trailing separator unless this is a drive root ("X:\").
  if (target.size() > 3 && target.back() == L'\\')
    target.pop_back();

  drive = (wchar_t)towupper(target[0]);
  if (target.size() < 3 || target[1] != L':' || drive < L'A' || drive > L'Z') {
    std::wcerr << L"Error: Invalid drive in target '" << raw << L"'"
               << std::endl;
    return false;
  }
  return true;
}

int wmain(int argc, wchar_t *argv[]) {
  setlocale(LC_ALL, "");

  for (int i = 1; i < argc; i++) {
    std::wstring arg = argv[i];
    if (arg == L"-h" || arg == L"--help") {
      PrintHelp();
      return 0;
    }
  }

  if (argc < 2) {
    PrintHelp();
    return 1;
  }

  // Collect positionals. Anything starting with '-' is an option, and unknown
  // options are rejected rather than being taken as a regex. "--" ends options.
  std::vector<std::wstring> positionals;
  bool optionsEnded = false;
  for (int i = 1; i < argc; i++) {
    std::wstring arg = argv[i];
    if (!optionsEnded && arg == L"--") {
      optionsEnded = true;
      continue;
    }
    if (!optionsEnded && arg.size() > 1 && arg[0] == L'-') {
      std::wcerr << L"Error: unknown option '" << arg << L"'" << std::endl;
      std::wcerr << L"Try 'ffs --help' for usage." << std::endl;
      return 1;
    }
    positionals.push_back(arg);
  }

  if (positionals.empty()) {
    std::wcerr << L"Error: missing <pattern>" << std::endl;
    std::wcerr << L"Try 'ffs --help' for usage." << std::endl;
    return 1;
  }
  if (positionals.size() > 2) {
    std::wcerr << L"Error: unexpected argument '" << positionals[2] << L"'"
               << std::endl;
    std::wcerr << L"Try 'ffs --help' for usage." << std::endl;
    return 1;
  }

  const std::wstring pattern = positionals[0];

  // Validate the regex once, up front. The readers compile it per entry and
  // treat syntax errors as "no match", so a bad pattern must be caught here.
  const auto regexFlags = std::regex::ECMAScript | std::regex::icase;
  try {
    std::wregex probe(pattern, regexFlags);
    (void)probe;
  } catch (const std::regex_error &e) {
    std::wcerr << L"Error: invalid regex '" << pattern << L"': "
               << WidenMessage(e.what()) << std::endl;
    return 1;
  }

  std::wstring rawTarget;
  if (positionals.size() >= 2) {
    rawTarget = positionals[1];
  } else {
    wchar_t cwd[MAX_PATH];
    DWORD cwdLen = GetCurrentDirectoryW(MAX_PATH, cwd);
    if (cwdLen == 0 || cwdLen >= MAX_PATH) {
      std::wcerr << L"Error: cannot determine current directory ("
                 << GetLastError() << L")" << std::endl;
      return 1;
    }
    rawTarget = cwd;
  }

  std::wstring target;
  wchar_t drive = 0;
  if (!NormalizeTarget(rawTarget, target, drive))
    return 1;

  wchar_t driveRoot[] = {drive, L':', L'\\', L'\0'};
  wchar_t fsName[MAX_PATH] = {};
  if (!GetVolumeInformationW(driveRoot, NULL, 0, NULL, NULL, NULL, fsName,
                             MAX_PATH)) {
    std::wcerr << L"Error: Cannot access volume '" << driveRoot << L"' ("
               << GetLastError() << L")" << std::endl;
    return 1;
  }

  bool isNtfs = (wcscmp(fsName, L"NTFS") == 0);
  bool isFat =
      (wcscmp(fsName, L"FAT") == 0 || wcscmp(fsName, L"FAT32") == 0);
  bool isExFat = (wcscmp(fsName, L"exFAT") == 0);

  if (!isNtfs && !isFat && !isExFat) {
    std::wcerr << L"Error: Unsupported filesystem '" << fsName << L"'" << std::endl;
    return 1;
  }

  SearchOptions options;
  options.mode = MatchMode_RegEx;
  options.ignoreCase = true;

  auto callback = [](int, int, void *) {};

  std::vector<FileResult> results;

  if (isNtfs) {
    MFTReader reader;
    if (!reader.Initialize(drive)) {
      std::wcerr << L"Error: " << reader.GetLastErrorMessage() << std::endl;
      return 1;
    }
    if (!reader.Scan(callback, nullptr)) {
      std::wcerr << L"Error: Scan failed - " << reader.GetLastErrorMessage() << std::endl;
      return 1;
    }
    results = reader.Search(pattern, target, options, -1);
  } else if (isFat) {
    FatReader reader;
    if (!reader.Initialize(drive)) {
      std::wcerr << L"Error: " << reader.GetLastErrorMessage() << std::endl;
      return 1;
    }
    if (!reader.Scan(CP_OEMCP, callback, nullptr)) {
      std::wcerr << L"Error: Scan failed - " << reader.GetLastErrorMessage() << std::endl;
      return 1;
    }
    results = reader.Search(pattern, target, CP_OEMCP, options, -1);
  } else if (isExFat) {
    exFatReader reader;
    if (!reader.Initialize(drive)) {
      std::wcerr << L"Error: " << reader.GetLastErrorMessage() << std::endl;
      return 1;
    }
    if (!reader.Scan(callback, nullptr)) {
      std::wcerr << L"Error: Scan failed - " << reader.GetLastErrorMessage() << std::endl;
      return 1;
    }
    results = reader.Search(pattern, target, options, -1);
  }

  for (const auto &r : results) {
    std::wcout << r.FullPath << L"\n";
  }

  return 0;
}
