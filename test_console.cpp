#include "FatReader.h"
#include "MFTReader.h"
#include "exFatReader.h"
#include <functional> // Added for std::function
#include <iostream>
#include <locale.h>
#include <vector> // Added for std::vector
#include <windows.h>

int wmain(int argc, wchar_t *argv[]) {
  setlocale(LC_ALL, ""); // Use system locale
  std::wcout << L"Starting Test Program..." << std::endl;

  std::vector<std::wstring> args;
  for (int i = 1; i < argc; i++)
    args.push_back(argv[i]);

  bool verbose = false;
  bool trace = false;
  std::wstring target = L"D:";
  std::wstring query = L"ws";

  // Extract flags
  for (auto it = args.begin(); it != args.end();) {
    if (*it == L"-v") {
      verbose = true;
      it = args.erase(it);
    } else if (*it == L"-t") {
      trace = true;
      it = args.erase(it);
    } else {
      ++it;
    }
  }

  // Parse positional
  if (args.size() > 0)
    target = args[0];
  if (args.size() > 1)
    query = args[1];
  else if (args.size() == 1 && target.find(L":") == std::wstring::npos) {
    query = target;
    target = L"D:";
  }

  std::wcout << L"Target: " << target << L"\nQuery: " << query << L"\nVerbose: "
             << (verbose ? L"Yes" : L"No") << L"\nTrace: "
             << (trace ? L"Yes" : L"No") << std::endl;

  wchar_t drive = towupper(target[0]);

  std::wcout << L"Initializing Drive " << drive << L"..." << std::endl;

  wchar_t driveRoot[] = {drive, L':', L'\\', L'\0'};
  wchar_t fsName[MAX_PATH];
  GetVolumeInformationW(driveRoot, NULL, 0, NULL, NULL, NULL, fsName, MAX_PATH);

  std::wcout << L"Detected File System: " << fsName << std::endl;

  std::wcout << L"Detected File System: " << fsName << std::endl;

  std::vector<FileResult> searchResults;

  // Lambda for search/print
  auto PerformSearch = [&](auto &rdr) {
    if (trace) {
      rdr.SetTraceCallback([](const std::wstring &msg) {
        std::wcout << L"[TRACE] " << msg << std::endl;
      });
    }

    if (!rdr.Initialize(drive)) {
      std::wcout << L"Failed to initialize: " << rdr.GetLastErrorMessage()
                 << std::endl;
      return false;
    }

    auto callback = [](int percent, int max, void *userData) {
      if (percent % 10 == 0)
        std::wcout << L"Scan Progress: " << percent << L"%" << std::endl;
    };

    // FAT specific Scan signature check
    // For generic template usage we might need `if constexpr` or specialized
    // override But here we can manually call.
    return true; // placeholder
  };

  auto callback = [](int percent, int max, void *userData) {
    (void)max;
    (void)userData;
    if (percent % 10 == 0)
      std::wcout << L"Scan Progress: " << percent << L"%" << std::endl;
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
      r.SetTraceCallback([](const std::wstring &msg) {
        std::wcout << L"[TRACE] " << msg << std::endl;
      });
    if (!r.Initialize(drive)) {
      std::wcout << L"Init Failed: " << r.GetLastErrorMessage() << std::endl;
      return 1;
    }

    if (!r.Scan(callback, nullptr, verboseCb)) {
      std::wcout << L"Scan Failed: " << r.GetLastErrorMessage() << std::endl;
      return 1;
    }
    searchResults = r.Search(query, target);

  } else if (wcscmp(fsName, L"FAT") == 0 || wcscmp(fsName, L"FAT32") == 0) {
    FatReader r;
    if (trace)
      r.SetTraceCallback([](const std::wstring &msg) {
        std::wcout << L"[TRACE] " << msg << std::endl;
      });
    if (!r.Initialize(drive)) {
      std::wcout << L"Init Failed: " << r.GetLastErrorMessage() << std::endl;
      return 1;
    }

    // Use default Code Page (OEM)
    if (!r.Scan(CP_OEMCP, callback, nullptr, verboseCb)) {
      std::wcout << L"Scan Failed: " << r.GetLastErrorMessage() << std::endl;
      return 1;
    }
    searchResults = r.Search(query, target);

  } else if (wcscmp(fsName, L"exFAT") == 0) {
    exFatReader r;
    if (trace)
      r.SetTraceCallback([](const std::wstring &msg) {
        std::wcout << L"[TRACE] " << msg << std::endl;
      });
    if (!r.Initialize(drive)) {
      std::wcout << L"Init Failed: " << r.GetLastErrorMessage() << std::endl;
      return 1;
    }

    if (!r.Scan(callback, nullptr, verboseCb)) {
      std::wcout << L"Scan Failed: " << r.GetLastErrorMessage() << std::endl;
      return 1;
    }
    searchResults = r.Search(query, target);
  } else {
    std::wcout << L"Unsupported File System." << std::endl;
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
    std::wcout << L"Listing results (limit 100):" << std::endl;
    for (size_t i = 0; i < min(searchResults.size(), 100); ++i) {
      std::wcout << searchResults[i].FullPath << L"\n";
    }
  }

  return 0;
}
