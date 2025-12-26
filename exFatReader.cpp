#include "exFatReader.h"
#include <algorithm>

exFatReader::exFatReader() : hVolume(INVALID_HANDLE_VALUE), currentDrive(0) {}
exFatReader::~exFatReader() { Close(); }

void exFatReader::SetError(const std::wstring &msg) {
  DWORD err = GetLastError();
  LPVOID lpMsgBuf;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                 (LPWSTR)&lpMsgBuf, 0, NULL);
  lastError = msg + L": " + (lpMsgBuf ? (wchar_t *)lpMsgBuf : L"Unknown Error");
  if (lpMsgBuf)
    LocalFree(lpMsgBuf);
}

void exFatReader::Close() {
  if (hVolume != INVALID_HANDLE_VALUE) {
    CloseHandle(hVolume);
    hVolume = INVALID_HANDLE_VALUE;
  }
  fileMap.clear();
}

std::wstring exFatReader::GetLastErrorMessage() const { return lastError; }

bool exFatReader::Initialize(TCHAR driveLetter) {
  Close();
  currentDrive = driveLetter;

  std::wstring path = L"\\\\.\\";
  path += driveLetter;
  path += L":";

  hVolume = CreateFileW(path.c_str(), GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                        0, NULL);
  if (hVolume == INVALID_HANDLE_VALUE) {
    SetError(L"Failed to open volume");
    return false;
  }

  EXFAT_BOOT_SECTOR bs;
  DWORD bytesRead;
  if (!ReadFile(hVolume, &bs, sizeof(bs), &bytesRead, NULL) ||
      bytesRead != sizeof(bs)) {
    SetError(L"Failed to read exFAT boot sector");
    return false;
  }

  if (memcmp(bs.FileSystemName, "EXFAT   ", 8) != 0) {
    SetError(L"Not an exFAT volume");
    return false;
  }

  bytesPerSector = 1 << bs.BytesPerSectorShift;
  sectorsPerCluster = 1 << bs.SectorsPerClusterShift;
  clusterHeapOffset = bs.ClusterHeapOffset;
  fatOffset = bs.FatOffset;
  fatLength = bs.FatLength;
  rootDirectoryCluster = bs.RootDirectoryCluster;

  return true;
}

uint32_t exFatReader::GetNextCluster(uint32_t cluster) {
  uint64_t fatPos =
      (uint64_t)fatOffset * bytesPerSector + (uint64_t)cluster * 4;
  LARGE_INTEGER li;
  li.QuadPart = fatPos;
  SetFilePointerEx(hVolume, li, NULL, FILE_BEGIN);

  uint32_t next;
  DWORD bytesRead;
  if (!ReadFile(hVolume, &next, 4, &bytesRead, NULL))
    return 0xFFFFFFFF;
  return next;
}

uint64_t exFatReader::ClusterToSector(uint32_t cluster) {
  return clusterHeapOffset + (uint64_t)(cluster - 2) * sectorsPerCluster;
}

bool exFatReader::Scan(void (*progressCallback)(int, int, void *),
                       void *userData,
                       std::function<void(const std::wstring &)> onFileFound) {
  if (hVolume == INVALID_HANDLE_VALUE)
    return false;

  // Start from Root Directory
  ProcessDirectory(rootDirectoryCluster, false, 0, 0, L"");

  return true;
}

void exFatReader::ProcessDirectory(uint32_t cluster, bool noFatChain,
                                   uint64_t dataLength, uint32_t parentCluster,
                                   const std::wstring &parentPath) {
  uint32_t current = cluster;
  uint32_t clusterBytes = bytesPerSector * sectorsPerCluster;
  std::vector<uint8_t> buffer(clusterBytes);

  uint64_t bytesProcessed = 0;

  while (current >= 2 && current < 0xFFFFFFF8) {
    uint64_t sector = ClusterToSector(current);
    LARGE_INTEGER li;
    li.QuadPart = sector * bytesPerSector;
    SetFilePointerEx(hVolume, li, NULL, FILE_BEGIN);

    DWORD bytesRead;
    if (!ReadFile(hVolume, buffer.data(), clusterBytes, &bytesRead, NULL))
      break;

    for (uint32_t i = 0; i < bytesRead; i += 32) {
      uint8_t entryType = buffer[i];
      if (entryType == EXFAT_ENTRY_TYPE_END)
        return;
      if (!(entryType & 0x80))
        continue; // Inactive entry

      if (entryType == EXFAT_ENTRY_TYPE_FILE) {
        EXFAT_FILE_ENTRY *fe = (EXFAT_FILE_ENTRY *)&buffer[i];
        uint8_t secondaryCount = fe->SecondaryCount;

        // Peek at next entries
        if (i + 32 * secondaryCount >= bytesRead) {
          // This case is complex (entry set crosses cluster boundary).
          // For now, let's assume it fits or we'd need to buffer across
          // clusters. Simplified: just skip if across boundary for now, but in
          // reality we should handle it.
          continue;
        }

        EXFAT_STREAM_EXTENSION_ENTRY *se =
            (EXFAT_STREAM_EXTENSION_ENTRY *)&buffer[i + 32];
        if (se->EntryType != EXFAT_ENTRY_TYPE_STREAM_EXT)
          continue;

        std::wstring fileName = L"";
        for (int n = 0; n < secondaryCount - 1; n++) {
          EXFAT_FILENAME_ENTRY *ne =
              (EXFAT_FILENAME_ENTRY *)&buffer[i + 64 + n * 32];
          if (ne->EntryType != EXFAT_ENTRY_TYPE_FILENAME)
            break;

          wchar_t part[16] = {0};
          memcpy(part, ne->FileName, 30);
          fileName += part;
        }

        Entry entry;
        entry.FirstCluster = se->FirstCluster;
        entry.ParentFirstCluster = cluster;
        entry.Name = fileName;
        entry.Size = se->DataLength;
        entry.IsDirectory = (fe->FileAttributes & 0x10) != 0;
        entry.LastWriteTime = FatTimestampToWin32(
            fe->LastModifiedTimestamp, fe->LastModified10msIncrement);
        entry.IsValid = true;

        uint32_t id = entry.FirstCluster;
        if (id == 0)
          id = 0x80000000 + (uint32_t)fileMap.size();

        fileMap[id] = entry;

        // Move index forward
        i += 32 * secondaryCount;

        if (entry.IsDirectory && entry.FirstCluster != 0) {
          bool nextNoFatChain =
              (se->GeneralSecondaryFlags & EXFAT_FLAG_NO_FAT_CHAIN) != 0;
          ProcessDirectory(entry.FirstCluster, nextNoFatChain, entry.Size,
                           cluster, parentPath + L"\\" + entry.Name);
        }
      }
    }

    bytesProcessed += bytesRead;
    if (noFatChain && dataLength > 0 && bytesProcessed >= dataLength)
      break;

    if (noFatChain)
      current++;
    else
      current = GetNextCluster(current);
  }
}

uint64_t exFatReader::FatTimestampToWin32(uint32_t timestamp, uint8_t tenMs) {
  // exFAT uses a single 32-bit field for date/time + increment
  // Bits 0-4: Double seconds (0-29)
  // Bits 5-10: Minute (0-59)
  // Bits 11-15: Hour (0-23)
  // Bits 16-20: Day (1-31)
  // Bits 21-24: Month (1-12)
  // Bits 25-31: Year (0-127, relative to 1980)

  SYSTEMTIME st = {0};
  st.wYear = (WORD)(1980 + (timestamp >> 25));
  st.wMonth = (WORD)((timestamp >> 21) & 0x0F);
  st.wDay = (WORD)((timestamp >> 16) & 0x1F);
  st.wHour = (WORD)((timestamp >> 11) & 0x1F);
  st.wMinute = (WORD)((timestamp >> 5) & 0x3F);
  st.wSecond = (WORD)((timestamp & 0x1F) * 2);
  st.wMilliseconds = (WORD)(tenMs * 10);

  FILETIME ft;
  SystemTimeToFileTime(&st, &ft);
  return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

bool exFatReader::MatchPattern(const std::wstring &str,
                               const std::wstring &pattern) {
  std::wstring s = str;
  std::wstring p = pattern;
  for (auto &c : s)
    c = towlower(c);
  for (auto &c : p)
    c = towlower(c);
  return s.find(p) != std::wstring::npos;
}

std::vector<FileResult> exFatReader::Search(const std::wstring &query,
                                            const std::wstring &targetFolder,
                                            bool caseSensitive,
                                            int maxResults) {
  std::vector<FileResult> results;
  // Similar to FatReader::Search
  // We can't use BuildPath directly without ParentFirstCluster logic
  // Let's implement a simple path builder or use recursion.
  // Given the map-based approach, it's easier to reconstruct upwards.

  auto BuildPathLocal = [&](const Entry &entry) {
    std::wstring path = entry.Name;
    uint32_t pc = entry.ParentFirstCluster;
    while (pc != 0 && pc != rootDirectoryCluster) {
      auto it = fileMap.find(pc);
      if (it == fileMap.end())
        break;
      path = it->second.Name + L"\\" + path;
      pc = it->second.ParentFirstCluster;
    }
    std::wstring drive = L"";
    drive += currentDrive;
    drive += L":\\";
    return drive + path;
  };

  for (auto const &[id, entry] : fileMap) {
    if (MatchPattern(entry.Name, query)) {
      std::wstring fullPath = BuildPathLocal(entry);
      if (!targetFolder.empty()) {
        if (fullPath.length() < targetFolder.length())
          continue;
        bool match = true;
        for (size_t i = 0; i < targetFolder.length(); ++i) {
          if (towlower(fullPath[i]) != towlower(targetFolder[i])) {
            match = false;
            break;
          }
        }
        if (!match)
          continue;
      }

      FileResult res;
      res.Name = entry.Name;
      res.FullPath = fullPath;
      res.Size = entry.Size;
      res.LastWriteTime = entry.LastWriteTime;
      res.IsDirectory = entry.IsDirectory;
      results.push_back(res);
      if (maxResults > 0 && (int)results.size() >= maxResults)
        break;
    }
  }
  return results;
}
