#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "FatReader.h"
#include "MFTReader.h"
#include "exFatReader.h"
#include <algorithm>
#include <iostream>
#include <locale.h>
#include <string>
#include <vector>
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
             << L"\n"
             << L"Examples:\n"
             << L"  ffs \".*\\\\.cpp$\"                Search current folder for "
                L".cpp files\n"
             << L"  ffs \".*\\\\.exe$\" D:             Search D: drive for .exe "
                L"files\n"
             << L"  ffs \"test_.*\" C:\\Projects       Search C:\\Projects for "
                L"files starting with test_\n";
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

  std::wstring pattern = argv[1];
  std::wstring target;

  if (argc >= 3) {
    target = argv[2];
  } else {
    wchar_t cwd[MAX_PATH];
    if (GetCurrentDirectoryW(MAX_PATH, cwd)) {
      target = cwd;
    } else {
      target = L"C:\\";
    }
  }

  if (target.size() == 2 && target[1] == L':') {
    target.push_back(L'\\');
  }

  wchar_t drive = towupper(target[0]);
  if (drive < L'A' || drive > L'Z') {
    std::wcerr << L"Error: Invalid drive letter '" << target[0] << L"' from target '" << target << L"'" << std::endl;
    return 1;
  }

  wchar_t driveRoot[] = {drive, L':', L'\\', L'\0'};
  wchar_t fsName[MAX_PATH];
  if (!GetVolumeInformationW(driveRoot, NULL, 0, NULL, NULL, NULL, fsName,
                             MAX_PATH)) {
    std::wcerr << L"Error: Cannot access volume '" << driveRoot << L"'" << std::endl;
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
