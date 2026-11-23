#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "FatReader.h"
#include "MFTReader.h"
#include "exFatReader.h"
#include <algorithm>
#include <cwctype>
#include <functional>
#include <iostream>
#include <locale.h>
#include <string>
#include <vector>
#include <wchar.h>
#include <windows.h>

namespace {

constexpr size_t kListLimit = 100;

bool IsLeapYear(unsigned year) {
  return (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
}

unsigned DaysInMonth(unsigned year, unsigned month) {
  static const unsigned kDays[] = {31, 28, 31, 30, 31, 30,
                                   31, 31, 30, 31, 30, 31};
  if (month == 2 && IsLeapYear(year))
    return 29;
  return kDays[month - 1];
}

// Parses "YYYY-MM-DD" into a FILETIME (as uint64). When endOfDay is set the
// time is 23:59:59 so that a max-date filter includes the whole day.
// Returns false with no output on any malformed or out-of-range input.
bool ParseDate(const std::wstring &text, bool endOfDay, uint64_t &outTime) {
  unsigned year = 0, month = 0, day = 0;
  int consumed = -1;
  int fields = swscanf(text.c_str(), L"%u-%u-%u%n", &year, &month, &day,
                       &consumed);
  if (fields != 3 || consumed < 0 || (size_t)consumed != text.size())
    return false;
  if (year < 1601 || year > 30827)
    return false;
  if (month < 1 || month > 12)
    return false;
  if (day < 1 || day > DaysInMonth(year, month))
    return false;

  SYSTEMTIME st = {};
  st.wYear = (WORD)year;
  st.wMonth = (WORD)month;
  st.wDay = (WORD)day;
  if (endOfDay) {
    st.wHour = 23;
    st.wMinute = 59;
    st.wSecond = 59;
  }
  FILETIME ft = {};
  if (!SystemTimeToFileTime(&st, &ft))
    return false;
  outTime = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  return true;
}

} // namespace

int wmain(int argc, wchar_t *argv[]) {
  setlocale(LC_ALL, "");
  std::wcout << L"Starting Test Program..." << std::endl;

  bool verbose = false;
  bool trace = false;
  SearchOptions options;
  options.ignoreCase = true; // Case-insensitive by default, matching the GUI.

  std::vector<std::wstring> positionals;

  // Single pass over the arguments. Value-taking flags consume the next
  // argument; a missing value is an error.
  for (int i = 1; i < argc; ++i) {
    const std::wstring flag = argv[i];
    auto nextValue = [&](std::wstring &value) -> bool {
      if (i + 1 >= argc) {
        std::wcerr << L"Error: option '" << flag << L"' requires a value"
                   << std::endl;
        return false;
      }
      value = argv[++i];
      return true;
    };

    std::wstring value;
    if (flag == L"-v") {
      verbose = true;
    } else if (flag == L"-t") {
      trace = true;
    } else if (flag == L"-e") {
      options.mode = MatchMode_Exact;
    } else if (flag == L"-s") {
      options.mode = MatchMode_SpaceDivided;
    } else if (flag == L"-r") {
      options.mode = MatchMode_RegEx;
    } else if (flag == L"-i") {
      options.ignoreCase = true;
    } else if (flag == L"-I") {
      options.ignoreCase = false;
    } else if (flag == L"--min-size") {
      if (!nextValue(value))
        return 1;
      options.minSize = (uint64_t)_wtoi64(value.c_str());
    } else if (flag == L"--max-size") {
      if (!nextValue(value))
        return 1;
      options.maxSize = (uint64_t)_wtoi64(value.c_str());
    } else if (flag == L"--min-date" || flag == L"--max-date") {
      if (!nextValue(value))
        return 1;
      const bool isMin = (flag == L"--min-date");
      uint64_t parsed = 0;
      if (!ParseDate(value, !isMin, parsed)) {
        std::wcerr << L"Error: invalid " << flag << L" '" << value
                   << L"' (expected a valid date as YYYY-MM-DD)" << std::endl;
        return 1;
      }
      if (isMin)
        options.minDate = parsed;
      else
        options.maxDate = parsed;
    } else if (flag == L"--files-only") {
      options.includeFiles = true;
      options.includeFolders = false;
    } else if (flag == L"--folders-only") {
      options.includeFiles = false;
      options.includeFolders = true;
    } else if (flag == L"--ext") {
      if (!nextValue(value))
        return 1;
      options.extensionFilter = value;
    } else if (flag == L"--full-path") {
      options.matchFullPath = true;
    } else if (flag == L"--exclude") {
      if (!nextValue(value))
        return 1;
      options.excludePattern = value;
    } else if (flag.size() > 1 && flag[0] == L'-') {
      std::wcerr << L"Error: unknown option '" << flag << L"'" << std::endl;
      return 1;
    } else {
      positionals.push_back(flag);
    }
  }

  // Positionals: "<query>" or "<target> <query>".
  if (positionals.empty()) {
    std::wcerr << L"Error: missing <query> argument" << std::endl;
    return 1;
  }
  if (positionals.size() > 2) {
    std::wcerr << L"Error: unexpected argument '" << positionals[2] << L"'"
               << std::endl;
    return 1;
  }

  const std::wstring query = positionals.back();
  std::wstring target;
  if (positionals.size() == 2) {
    target = positionals[0];
  } else {
    // No target given: search the whole volume of the current directory.
    wchar_t cwd[MAX_PATH] = {};
    if (GetCurrentDirectoryW(MAX_PATH, cwd) < 3) {
      std::wcerr << L"Error: cannot determine current directory ("
                 << GetLastError() << L")" << std::endl;
      return 1;
    }
    target = std::wstring(cwd, 2) + L"\\";
  }

  if (target.size() == 2 && target[1] == L':')
    target.push_back(L'\\');
  if (target.size() < 2 || target[1] != L':' || !iswalpha(target[0])) {
    std::wcerr << L"Error: target must start with a drive letter (e.g. D:): '"
               << target << L"'" << std::endl;
    return 1;
  }

  std::wcout << L"Verbose: " << (verbose ? L"Yes" : L"No") << L"\nTrace: "
             << (trace ? L"Yes" : L"No") << L"\nMode: "
             << (options.mode == MatchMode_Exact ? L"Exact" :
                 options.mode == MatchMode_SpaceDivided ? L"SpaceDivided" :
                 options.mode == MatchMode_RegEx ? L"RegEx" : L"Substring")
             << L"\nIgnoreCase: " << (options.ignoreCase ? L"Yes" : L"No")
             << L"\nMinSize: " << options.minSize << L" bytes"
             << L"\nMaxSize: " << options.maxSize << L" bytes"
             << L"\nMinDate: " << options.minDate
             << L"\nMaxDate: " << options.maxDate
             << L"\nIncludeFiles: " << (options.includeFiles ? L"Yes" : L"No")
             << L"\nIncludeFolders: " << (options.includeFolders ? L"Yes" : L"No")
             << L"\nExtensionFilter: " << options.extensionFilter
             << L"\nMatchFullPath: " << (options.matchFullPath ? L"Yes" : L"No")
             << L"\nExcludePattern: " << options.excludePattern
             << std::endl;

  wchar_t drive = (wchar_t)towupper(target[0]);

  std::wcout << L"Initializing Drive " << drive << L"..." << std::endl;

  wchar_t driveRoot[] = {drive, L':', L'\\', L'\0'};
  wchar_t fsName[MAX_PATH] = {};
  if (!GetVolumeInformationW(driveRoot, NULL, 0, NULL, NULL, NULL, fsName,
                             MAX_PATH)) {
    std::wcerr << L"Error: Cannot access volume '" << driveRoot << L"' ("
               << GetLastError() << L")" << std::endl;
    return 1;
  }

  std::wcout << L"Detected File System: " << fsName << std::endl;

  std::vector<FileResult> searchResults;

  auto callback = [](int percent, int max, void *userData) {
    (void)max;
    (void)userData;
    if (percent % 10 == 0)
      std::wcout << L"Scan Progress: " << percent << L"%" << std::endl;
  };

  auto traceCb = [](const std::wstring &msg) {
    std::wcout << L"[TRACE] " << msg << std::endl;
  };

  std::function<void(const std::wstring &)> verboseCb = nullptr;
  if (verbose) {
    verboseCb = [](const std::wstring &name) {
      std::wcout << L"File: " << name << L"\n";
    };
  }

  if (wcscmp(fsName, L"NTFS") == 0) {
    MFTReader r;
    if (trace)
      r.SetTraceCallback(traceCb);
    if (!r.Initialize(drive)) {
      std::wcerr << L"Init Failed: " << r.GetLastErrorMessage() << std::endl;
      return 1;
    }

    if (!r.Scan(callback, nullptr, verboseCb)) {
      std::wcerr << L"Scan Failed: " << r.GetLastErrorMessage() << std::endl;
      return 1;
    }
    searchResults = r.Search(query, target, options);

  } else if (wcscmp(fsName, L"FAT") == 0 || wcscmp(fsName, L"FAT32") == 0) {
    FatReader r;
    if (trace)
      r.SetTraceCallback(traceCb);
    if (!r.Initialize(drive)) {
      std::wcerr << L"Init Failed: " << r.GetLastErrorMessage() << std::endl;
      return 1;
    }

    // Use default Code Page (OEM)
    if (!r.Scan(CP_OEMCP, callback, nullptr, verboseCb)) {
      std::wcerr << L"Scan Failed: " << r.GetLastErrorMessage() << std::endl;
      return 1;
    }
    searchResults = r.Search(query, target, CP_OEMCP, options);

  } else if (wcscmp(fsName, L"exFAT") == 0) {
    exFatReader r;
    if (trace)
      r.SetTraceCallback(traceCb);
    if (!r.Initialize(drive)) {
      std::wcerr << L"Init Failed: " << r.GetLastErrorMessage() << std::endl;
      return 1;
    }

    if (!r.Scan(callback, nullptr, verboseCb)) {
      std::wcerr << L"Scan Failed: " << r.GetLastErrorMessage() << std::endl;
      return 1;
    }
    searchResults = r.Search(query, target, options);
  } else {
    std::wcerr << L"Unsupported File System: '" << fsName << L"'" << std::endl;
    return 1;
  }

  std::wcout << L"Scan Complete." << std::endl;
  if (verbose) {
    std::wcout << L"--------------------------------------------------\n";
  }

  std::wcout << L"Searching for '" << query << L"' in '" << target << L"'..."
             << std::endl;

  std::wcout << L"Found " << searchResults.size() << L" results." << std::endl;

  if (searchResults.empty()) {
    std::wcout << L"No items found matching the query." << std::endl;
  } else {
    std::wcout << L"Listing results (limit " << kListLimit << L"):" << std::endl;
    const size_t shown = (std::min)(searchResults.size(), kListLimit);
    for (size_t i = 0; i < shown; ++i) {
      std::wcout << searchResults[i].FullPath << L"\n";
    }
  }

  return 0;
}
