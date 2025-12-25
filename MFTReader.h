#pragma once
#include "NtfsStructs.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

#include <functional>

struct FileResult {
  std::wstring Name;
  std::wstring FullPath;
  uint64_t Size;
  uint64_t LastWriteTime;
  bool IsDirectory;
};

class MFTReader {
public:
  MFTReader();
  ~MFTReader();

  bool Initialize(TCHAR driveLetter);
  void Close();
  bool Scan(void (*progressCallback)(int, int, void *), void *userData,
            std::function<void(const std::wstring &)> onFileFound = nullptr);
  void SetTraceCallback(std::function<void(const std::wstring &)> callback) {
    traceCallback = callback;
  }
  std::vector<FileResult> Search(const std::wstring &query,
                                 const std::wstring &targetFolder,
                                 bool caseSensitive = false,
                                 int maxResults = -1);
  std::wstring GetLastErrorMessage() const;

private:
  std::wstring lastError;
  void SetError(const std::wstring &msg);
  struct Entry {
    uint64_t RefID;
    uint64_t ParentRefID;
    std::wstring Name;
    uint64_t Size;
    uint64_t LastWriteTime;
    bool IsDirectory;
    bool IsValid;
  };

  HANDLE hVolume;
  TCHAR currentDrive;
  std::unordered_map<uint64_t, Entry> fileMap;
  std::function<void(const std::wstring &)> scanDebugCallback; // For -v (files)
  std::function<void(const std::wstring &)> traceCallback; // For -t (stages)

  // NTFS Volume Data
  uint64_t mftStartLcn;
  uint64_t totalClusters;
  uint32_t clustersPerRecord; // often < 0 if bytes.
  uint32_t bytesPerCluster;
  uint32_t recordSize;

  void ProcessBuffer(const uint8_t *buffer, size_t size);
  void ParseRecord(const FILE_RECORD_HEADER *record);
  std::wstring BuildPath(uint64_t refId);
  bool MatchPattern(const std::wstring &str, const std::wstring &pattern);
};
