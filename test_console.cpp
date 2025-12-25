#include "MFTReader.h"
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

  MFTReader reader;

  // Set Trace Callback
  if (trace) {
    reader.SetTraceCallback([](const std::wstring &msg) {
      std::wcout << L"[TRACE] " << msg << std::endl;
    });
  }

  wchar_t drive = towupper(target[0]);

  std::wcout << L"Initializing Drive " << drive << L"..." << std::endl;
  if (!reader.Initialize(drive)) {
    std::wcout << L"Failed to initialize (Administrator privileges required): "
               << reader.GetLastErrorMessage() << std::endl;
    return 1;
  }
  std::wcout << L"Initialization efficient." << std::endl;

  auto callback = [](int percent, int max, void *userData) {
    if (percent % 10 == 0)
      std::wcout << L"Scan Progress: " << percent << L"%" << std::endl;
  };

  // Verbose callback
  std::function<void(const std::wstring &)> verboseCb = nullptr;
  if (verbose) {
    verboseCb = [](const std::wstring &name) {
      std::wcout << L"File: " << name << L"\n";
    };
  }

  std::wcout << L"Scanning MFT..." << std::endl;
  if (!reader.Scan(callback, nullptr, verboseCb)) {
    std::wcout << L"Scan failed: " << reader.GetLastErrorMessage() << std::endl;
    return 1;
  }
  std::wcout << L"Scan Complete." << std::endl;
  if (verbose) {
    std::wcout << L"--------------------------------------------------\n";
  }

  std::wcout << L"Searching for '" << query << L"' in '" << target << L"'..."
             << std::endl;
  auto results = reader.Search(query, target);

  std::wcout << L"Found " << results.size() << L" results." << std::endl;

  if (results.empty()) {
    std::wcout << L"No items found matching the query." << std::endl;
  } else {
    std::wcout << L"Listing results (limit 100):" << std::endl;
    for (size_t i = 0; i < min(results.size(), 100); ++i) {
      std::wcout << results[i].FullPath << L"\n";
    }
  }

  return 0;
}
