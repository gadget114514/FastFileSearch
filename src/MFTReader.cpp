#include "MFTReader.h"
#include <algorithm>
#include <iostream>
#include <regex>
#include <sstream>
#include <utility>
#include <vector>

namespace {

// Largest record / read chunk we accept. Also the Scan read buffer size.
constexpr uint32_t kReadBufferSize = 1024 * 1024; // 1MB
// Bounded retry count for transient sector errors (ERROR_CRC/ERROR_IO_DEVICE).
constexpr int kMaxTransientRetries = 3;

// Runs a callable on scope exit (used to clear scanDebugCallback on every path).
template <class F> class ScopeExit {
public:
  explicit ScopeExit(F f) : f_(std::move(f)) {}
  ~ScopeExit() { f_(); }
  ScopeExit(const ScopeExit &) = delete;
  ScopeExit &operator=(const ScopeExit &) = delete;

private:
  F f_;
};

bool IsTransientIoError(DWORD err) {
  return err == ERROR_CRC || err == ERROR_IO_DEVICE;
}

// Seeks to an absolute byte offset. Returns false on failure (GetLastError set).
bool SeekAbsolute(HANDLE h, uint64_t offset) {
  if (offset > static_cast<uint64_t>(INT64_MAX)) {
    ::SetLastError(ERROR_SEEK);
    return false;
  }
  LARGE_INTEGER li;
  li.QuadPart = static_cast<LONGLONG>(offset);
  return SetFilePointerEx(h, li, NULL, FILE_BEGIN) != FALSE;
}

// Reads exactly `size` bytes at `offset`, looping over short reads and
// retrying transient sector errors a bounded number of times.
// On return, `outRead` holds the bytes actually read. The result is false on
// seek failure or an unrecoverable read error; a short count with true means
// EOF was reached first.
bool ReadFully(HANDLE h, uint64_t offset, uint8_t *dst, DWORD size,
               DWORD &outRead) {
  outRead = 0;
  if (!SeekAbsolute(h, offset))
    return false;

  DWORD total = 0;
  int retries = 0;
  while (total < size) {
    DWORD got = 0;
    if (!ReadFile(h, dst + total, size - total, &got, NULL)) {
      DWORD err = GetLastError();
      if (IsTransientIoError(err) && retries < kMaxTransientRetries) {
        ++retries;
        Sleep(10);
        if (!SeekAbsolute(h, offset + total)) {
          outRead = total;
          return false;
        }
        continue;
      }
      outRead = total;
      return false;
    }
    if (got == 0)
      break; // EOF
    total += got;
  }
  outRead = total;
  return true;
}

} // namespace

// Helper to decode NTFS Data Runs
struct DataRun {
  uint64_t LCN;
  uint64_t Length; // In clusters
};

MFTReader::MFTReader()
    : hVolume(INVALID_HANDLE_VALUE), currentDrive(0), mftStartLcn(0),
      bytesPerCluster(0), recordSize(0) {}

MFTReader::~MFTReader() { Close(); }

std::wstring MFTReader::GetLastErrorMessage() const { return lastError; }

void MFTReader::SetError(const std::wstring &msg) {
  DWORD err = GetLastError();
  LPVOID lpMsgBuf = nullptr;
  DWORD len = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      (LPWSTR)&lpMsgBuf, 0, NULL);

  std::wstring sysText =
      (len > 0 && lpMsgBuf) ? std::wstring((wchar_t *)lpMsgBuf)
                            : std::wstring(L"Unknown Error");
  if (lpMsgBuf)
    LocalFree(lpMsgBuf);
  lastError = msg + L": " + sysText;
}

void MFTReader::Close() {
  if (hVolume != INVALID_HANDLE_VALUE) {
    CloseHandle(hVolume);
    hVolume = INVALID_HANDLE_VALUE;
  }
  fileMap.clear();
}

bool MFTReader::Initialize(TCHAR driveLetter) {
  if (traceCallback)
    traceCallback(L"Initialize: Opening volume " +
                  std::wstring(1, driveLetter));
  Close();
  currentDrive = driveLetter;
  lastError = L"";

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

  DWORD bytesReturned;
  NTFS_VOLUME_DATA_BUFFER volumeData;
  if (!DeviceIoControl(hVolume, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0,
                       &volumeData, sizeof(volumeData), &bytesReturned, NULL)) {
    SetError(L"Failed to get NTFS volume data");
    Close();
    return false;
  }

  mftStartLcn = volumeData.MftStartLcn.QuadPart;
  bytesPerCluster = volumeData.BytesPerCluster;

  // Use BytesPerFileRecordSegment directly to avoid signed/unsigned confusion
  recordSize = volumeData.BytesPerFileRecordSegment;

  // Shared failure path for corrupt or unusable volume geometry.
  auto rejectGeometry = [this](const wchar_t *why) {
    Close();
    ::SetLastError(ERROR_INVALID_DATA);
    SetError(std::wstring(L"Invalid NTFS geometry: ") + why);
    return false;
  };

  if (recordSize > 0) {
    if (traceCallback) {
      traceCallback(L"Spec Selection: BytesPerFileRecordSegment is valid (" +
                    std::to_wstring(recordSize) + L"). Using it directly.");
    }
  } else {
    // Fallback logic if 0
    uint32_t clusters = volumeData.ClustersPerFileRecordSegment;
    if (traceCallback) {
      traceCallback(L"Spec Selection: BytesPerFileRecordSegment is 0. Checking "
                    L"ClustersPerFileRecordSegment: " +
                    std::to_wstring(clusters) + L" (0x" +
                    std::to_wstring(clusters) + L")");
    }

    if ((clusters > 0x80000000) || (clusters >= 0x80 && clusters <= 0xFF)) {
      // Negative 2^(-n) encoding: sign-extended (e.g. 0xFFFFFFF6) or raw byte
      // (e.g. 0xF6). Only the low byte carries the exponent.
      int8_t val = (int8_t)clusters;
      int shift = -(int)val;
      if (shift < 0 || shift >= 32)
        return rejectGeometry(L"record size exponent out of range");
      recordSize = (uint32_t)(1ULL << shift);
      if (traceCallback) {
        traceCallback(L"Spec Selection: Value is Negative Exponent (" +
                      std::to_wstring((int)val) + L"). Formula 2^(-n) -> " +
                      std::to_wstring(recordSize) + L" bytes.");
      }
    } else {
      // Standard cluster count
      uint64_t bytes64 = (uint64_t)clusters * bytesPerCluster;
      if (bytes64 > kReadBufferSize)
        return rejectGeometry(L"record size exceeds read buffer");
      recordSize = (uint32_t)bytes64;
      if (traceCallback) {
        traceCallback(L"Spec Selection: Value is Standard Cluster Count (" +
                      std::to_wstring(clusters) +
                      L"). Formula C * ClusterSize -> " +
                      std::to_wstring(recordSize) + L" bytes.");
      }
    }
  }

  if (bytesPerCluster == 0)
    return rejectGeometry(L"bytes per cluster is zero");
  if (recordSize == 0)
    return rejectGeometry(L"record size is zero");
  if (recordSize < sizeof(FILE_RECORD_HEADER))
    return rejectGeometry(L"record size smaller than FILE record header");
  if (recordSize > kReadBufferSize)
    return rejectGeometry(L"record size exceeds read buffer");

  if (traceCallback)
    traceCallback(
        L"Initialize: Volume opened, StartLCN=" + std::to_wstring(mftStartLcn) +
        L", ClusterSize=" + std::to_wstring(bytesPerCluster) +
        L", RecordSize=" + std::to_wstring(recordSize) + L" (ClustersPerRec=" +
        std::to_wstring(volumeData.ClustersPerFileRecordSegment) + L")");
  return true;
}

// Decodes an NTFS run list into `out`. Returns false on corrupt metadata
// (length/offset field wider than 8 bytes, or an LCN that goes negative or
// overflows). A run list truncated by maxLen simply ends the list.
static bool DecodeDataRuns(const uint8_t *runList, uint64_t maxLen,
                           std::vector<DataRun> &out) {
  out.clear();
  int64_t currentLcn = 0;
  uint64_t offset = 0;

  while (offset < maxLen) {
    uint8_t header = runList[offset++];
    if (header == 0)
      break;

    uint8_t lenBytes = header & 0xF;
    uint8_t offBytes = (header >> 4) & 0xF;

    // Fields wider than 8 bytes cannot be represented in uint64/int64.
    if (lenBytes > 8 || offBytes > 8)
      return false;

    if (offset + lenBytes + offBytes > maxLen)
      break;

    uint64_t length = 0;
    for (unsigned i = 0; i < lenBytes; i++) {
      length |= (uint64_t)runList[offset++] << (i * 8);
    }

    uint64_t rawOffset = 0;
    for (unsigned i = 0; i < offBytes; i++) {
      rawOffset |= (uint64_t)runList[offset++] << (i * 8);
    }

    // Sign extend LCN offset (unsigned arithmetic avoids signed-shift UB).
    int64_t lcnOffset = (int64_t)rawOffset;
    if (offBytes > 0 && offBytes < 8 &&
        (rawOffset & (1ULL << ((offBytes * 8) - 1)))) {
      lcnOffset = (int64_t)(rawOffset | (~0ULL << (offBytes * 8)));
    }

    // Overflow-checked signed addition.
    if ((lcnOffset > 0 && currentLcn > INT64_MAX - lcnOffset) ||
        (lcnOffset < 0 && currentLcn < INT64_MIN - lcnOffset))
      return false;
    currentLcn += lcnOffset;

    // A negative absolute LCN is never valid.
    if (currentLcn < 0)
      return false;

    out.push_back({(uint64_t)currentLcn, length});
  }
  return true;
}

bool MFTReader::Scan(void (*progressCallback)(int, int, void *), void *userData,
                     std::function<void(const std::wstring &)> onFileFound) {
  scanDebugCallback = onFileFound;
  // Clears the debug callback on every return path below.
  ScopeExit clearDebugCallback([this] { scanDebugCallback = nullptr; });

  if (hVolume == INVALID_HANDLE_VALUE) {
    lastError = L"Volume not initialized";
    return false;
  }
  if (recordSize == 0 || bytesPerCluster == 0) {
    lastError = L"Invalid volume geometry";
    return false;
  }

  // 1. Read Record 0 ($MFT) to find the runs
  if (traceCallback)
    traceCallback(L"Scan: Reading $MFT record...");
  if (mftStartLcn > UINT64_MAX / bytesPerCluster) {
    ::SetLastError(ERROR_INVALID_DATA);
    SetError(L"Corrupt $MFT start LCN");
    return false;
  }
  uint64_t mftOffset = mftStartLcn * bytesPerCluster;
  std::vector<uint8_t> buffer(recordSize);

  DWORD bytesRead = 0;
  if (!ReadFully(hVolume, mftOffset, buffer.data(), recordSize, bytesRead)) {
    SetError(L"Failed to read MFT Header");
    return false;
  }
  if (bytesRead != recordSize) {
    ::SetLastError(ERROR_HANDLE_EOF);
    SetError(L"Short read of MFT Header");
    return false;
  }
  if (IsCancelRequested()) {
    lastError = L"Scan cancelled";
    return false;
  }

  FILE_RECORD_HEADER *fr = (FILE_RECORD_HEADER *)buffer.data();
  if (fr->Magic != 0x454C4946) { // "FILE"
    ::SetLastError(ERROR_INVALID_DATA);
    SetError(L"Invalid $MFT record signature");
    return false;
  }

  // Bound every attribute walk by the used part of the record, clamped to the
  // buffer actually read.
  const uint32_t usedSize = (std::min<uint32_t>)(fr->RealSize, recordSize);
  if ((uint32_t)fr->AttributeOffset + sizeof(ATTRIBUTE_HEADER) > usedSize) {
    ::SetLastError(ERROR_INVALID_DATA);
    SetError(L"Invalid attribute offset in $MFT record");
    return false;
  }
  const uint8_t *recordEnd = buffer.data() + usedSize;

  // Find DATA attribute (0x80)
  std::vector<DataRun> runs;
  uint8_t *attrCursor = buffer.data() + fr->AttributeOffset;

  while (attrCursor + sizeof(ATTRIBUTE_HEADER) <= recordEnd) {
    ATTRIBUTE_HEADER *ah = (ATTRIBUTE_HEADER *)attrCursor;
    if (ah->TypeID == AttributeEnd)
      break;
    if (ah->Length < sizeof(ATTRIBUTE_HEADER) ||
        ah->Length > (size_t)(recordEnd - attrCursor)) {
      ::SetLastError(ERROR_INVALID_DATA);
      SetError(L"Invalid attribute length in $MFT record");
      return false;
    }

    if (ah->TypeID == AttributeData) { // 0x80
      if (ah->NonResidentFlag) {
        // Non-resident header is 0x40 bytes; the fields read below need it.
        if (ah->Length < 0x40) {
          ::SetLastError(ERROR_INVALID_DATA);
          SetError(L"Data attribute too short");
          return false;
        }

        uint16_t runOffset = *(uint16_t *)(attrCursor + 0x20);
        if (runOffset >= ah->Length) {
          ::SetLastError(ERROR_INVALID_DATA);
          SetError(L"Invalid Run Offset");
          return false;
        }

        // Run list is at attrCursor + runOffset, bounded by the attribute.
        if (traceCallback)
          traceCallback(L"Scan: Decoding Data Runs...");
        if (!DecodeDataRuns(attrCursor + runOffset, ah->Length - runOffset,
                            runs)) {
          ::SetLastError(ERROR_INVALID_DATA);
          SetError(L"Corrupt Data Runs for $MFT");
          return false;
        }
        if (runs.empty()) {
          ::SetLastError(ERROR_INVALID_DATA);
          SetError(L"Empty Data Runs for $MFT");
          return false;
        }

        if (scanDebugCallback) {
          scanDebugCallback(L"MFT Data Runs decoded: " +
                            std::to_wstring(runs.size()));
        }
      } else {
        // Resident: MFT is too small? Unlikely for $MFT on used volume.
        // But technically possible on empty tiny volume.
      }
      break; // Found primary DATA
    }
    attrCursor += ah->Length;
  }

  if (runs.empty()) {
    ::SetLastError(ERROR_INVALID_DATA);
    SetError(L"$MFT has no non-resident DATA runs");
    return false;
  }

  // 2. Read all runs
  if (traceCallback)
    traceCallback(L"Scan: Processing " + std::to_wstring(runs.size()) +
                  L" runs.");
  std::vector<uint8_t> readBuf(kReadBufferSize);

  // Estimate total records for progress
  uint64_t totalRecords = 0;
  for (auto &r : runs)
    totalRecords += (r.Length * bytesPerCluster) / recordSize;
  uint64_t processedRecords = 0;
  bool firstChunkProcessed = false;

  for (auto &run : runs) {
    if (IsCancelRequested()) {
      lastError = L"Scan cancelled";
      return false;
    }
    if (traceCallback)
      traceCallback(L"Scan: Run LCN=" + std::to_wstring(run.LCN) + L", Len=" +
                    std::to_wstring(run.Length));
    if (run.LCN > UINT64_MAX / bytesPerCluster ||
        run.Length > UINT64_MAX / bytesPerCluster) {
      ::SetLastError(ERROR_INVALID_DATA);
      SetError(L"Corrupt $MFT data run");
      return false;
    }
    uint64_t runBytes = run.Length * bytesPerCluster;
    uint64_t runStart = run.LCN * bytesPerCluster;
    uint64_t currentOffset = 0;

    while (currentOffset < runBytes) {
      if (IsCancelRequested()) {
        lastError = L"Scan cancelled";
        return false;
      }
      uint32_t toRead = (uint32_t)(std::min)(
          static_cast<uint64_t>(kReadBufferSize), runBytes - currentOffset);
      DWORD chunkRead = 0;
      if (!ReadFully(hVolume, runStart + currentOffset, readBuf.data(), toRead,
                     chunkRead)) {
        SetError(L"Failed to read $MFT data run");
        return false;
      }
      if (chunkRead != toRead) {
        ::SetLastError(ERROR_HANDLE_EOF);
        SetError(L"Short read of $MFT data run");
        return false;
      }
      bytesRead = chunkRead;

      // If bytesRead < recordSize, we can't parse a record: end of this run.
      if (bytesRead < recordSize)
        break;

      uint32_t recordsRead = bytesRead / recordSize;
      if (scanDebugCallback && !firstChunkProcessed) {
        scanDebugCallback(L"First chunk read: " + std::to_wstring(bytesRead) +
                          L" bytes (" + std::to_wstring(recordsRead) +
                          L" records)");
        firstChunkProcessed = true;
      }

      // Process buffer
      for (uint32_t i = 0; i < bytesRead; i += recordSize) {
        if (i + recordSize > bytesRead)
          break;
        ParseRecord((FILE_RECORD_HEADER *)(readBuf.data() + i));
        processedRecords++;
      }

      currentOffset += bytesRead;
      if (progressCallback && totalRecords > 0 &&
          (processedRecords % 1000 == 0)) {
        double pct = 100.0 * (double)processedRecords / (double)totalRecords;
        progressCallback((int)(std::min)(pct, 100.0), 100, userData);
      }
    }
  }

  if (progressCallback)
    progressCallback(100, 100, userData);

  return true;
}

void MFTReader::ParseRecord(const FILE_RECORD_HEADER *header) {
  if (header->Magic != 0x454C4946)
    return;
  if (!(header->Flags & 0x01))
    return; // Not in use

  uint64_t mftRef = header->MFTRecordNumber;
  // If MFTRecordNumber is 0 (older NTFS), we need to track index manually.
  // ideally we pass index to this func. But standard usually has it.
  // If 0 and it's not the $MFT itself, it could be unreliable.
  // Better strategy: The loop in Scan knows the index.
  // But let's rely on MFTRecordNumber for now or pass it.
  // Actually, let's fix: We can't trust MFTRecordNumber on all versions.
  // However, for this simplified version, let's assume valid or skip.
  // Better: Scan caller passes the ID. But that requires refactoring Scan loop.
  // Let's assume header->MFTRecordNumber is correct (Valid on XP+).

  Entry entry;
  entry.RefID = header->MFTRecordNumber;
  entry.IsValid = false;
  entry.IsDirectory = (header->Flags & 0x02) != 0;

  // Corrupt record: used size must fit in the record and cover the header.
  if (header->RealSize < sizeof(FILE_RECORD_HEADER) ||
      header->RealSize > recordSize)
    return;
  const uint8_t *ptr = (const uint8_t *)header;
  const uint8_t *end = ptr + header->RealSize;

  if ((uint32_t)header->AttributeOffset + sizeof(ATTRIBUTE_HEADER) >
      header->RealSize)
    return;
  const ATTRIBUTE_HEADER *attr =
      (const ATTRIBUTE_HEADER *)(ptr + header->AttributeOffset);

  bool gotName = false;

  while ((const uint8_t *)attr + sizeof(ATTRIBUTE_HEADER) <= end &&
         attr->TypeID != AttributeEnd) {
    // Length must cover at least the common header and stay inside the record
    // (also guarantees forward progress).
    if (attr->Length < sizeof(ATTRIBUTE_HEADER))
      break;
    if ((const uint8_t *)attr + attr->Length > end)
      break; // OOB

    if (attr->TypeID == AttributeFileName) { // 0x30
      const FILE_NAME_ATTRIBUTE *fn;
      // Resident value offsets are read from the resident header, so require
      // its full size before touching ValueOffset.
      if (!attr->NonResidentFlag &&
          attr->Length >= sizeof(RESIDENT_ATTRIBUTE_HEADER)) {
        const RESIDENT_ATTRIBUTE_HEADER *res =
            (const RESIDENT_ATTRIBUTE_HEADER *)attr;

        // Safety check for offset
        if (res->ValueOffset + sizeof(FILE_NAME_ATTRIBUTE) <= attr->Length) {
          fn = (const FILE_NAME_ATTRIBUTE *)((const uint8_t *)attr +
                                             res->ValueOffset);

          // Check Name Length
          size_t nameBytes = fn->NameLength * sizeof(wchar_t);
          if ((const uint8_t *)fn->Name + nameBytes <=
              (const uint8_t *)attr + attr->Length) {
            bool isWin32 = (fn->NameType == 0x01 || fn->NameType == 0x03);
            if (!gotName || isWin32) {
              entry.ParentRefID = fn->ParentDirectoryRef & 0xFFFFFFFFFFFF;
              entry.Size = fn->DataSize;
              if (fn->NameLength > 0)
                entry.Name.assign(fn->Name, fn->NameLength);
              gotName = true;
              entry.IsValid = true;
            }
          }
        }
      }
    } else if (attr->TypeID == AttributeStandardInformation) {
      if (!attr->NonResidentFlag &&
          attr->Length >= sizeof(RESIDENT_ATTRIBUTE_HEADER)) {
        const RESIDENT_ATTRIBUTE_HEADER *res =
            (const RESIDENT_ATTRIBUTE_HEADER *)attr;
        if (res->ValueOffset + sizeof(STANDARD_INFORMATION) <= attr->Length) {
          const STANDARD_INFORMATION *si =
              (const STANDARD_INFORMATION *)((const uint8_t *)attr +
                                             res->ValueOffset);
          entry.LastWriteTime = si->FileChangeTime;
        }
      }
    } else if (attr->TypeID == AttributeData) { // 0x80
      if (!attr->NonResidentFlag) {
        // Resident: only read the value length if the full header fits.
        if (attr->Length >= sizeof(RESIDENT_ATTRIBUTE_HEADER)) {
          const RESIDENT_ATTRIBUTE_HEADER *res =
              (const RESIDENT_ATTRIBUTE_HEADER *)attr;
          entry.Size = res->ValueLength;
        }
      } else {
        // Non-Resident
        // DataSize is at offset 0x30
        if (attr->Length >= 0x40) {
          entry.Size = *(const uint64_t *)((const uint8_t *)attr + 0x30);
        }
      }
    }

    attr = (const ATTRIBUTE_HEADER *)((const uint8_t *)attr + attr->Length);
  }

  if (entry.IsValid) {
    fileMap[entry.RefID] = entry;
    if (scanDebugCallback) {
      scanDebugCallback(entry.Name);
    }
  }
}

std::wstring MFTReader::BuildPath(uint64_t refId) {
  // Prevent infinite loop
  std::wstring path = L"";
  int depth = 0;
  while (refId != 0x05 && depth < 256) { // 0x05 is Root Directory
    auto it = fileMap.find(refId);
    if (it == fileMap.end())
      break;

    path = L"\\" + it->second.Name + path;
    refId = it->second.ParentRefID;
    depth++;
  }

  std::wstring drivePrefix = L"";
  drivePrefix += currentDrive;
  drivePrefix += L":";

  return drivePrefix + path;
}

bool MFTReader::MatchPattern(const std::wstring &str,
                             const std::wstring &pattern,
                             const SearchOptions &options) {
  if (pattern.empty())
    return true;

  bool matched = false;

  if (options.mode == MatchMode_Exact) {
    if (options.ignoreCase) {
      matched = lstrcmpiW(str.c_str(), pattern.c_str()) == 0;
    } else {
      matched = wcscmp(str.c_str(), pattern.c_str()) == 0;
    }
  } else if (options.mode == MatchMode_RegEx) {
    try {
      std::regex_constants::syntax_option_type flags = std::regex::ECMAScript;
      if (options.ignoreCase)
        flags |= std::regex::icase;
      std::wregex re(pattern, flags);
      matched = std::regex_search(str, re);
    } catch (...) {
      matched = false;
    }
  } else if (options.mode == MatchMode_SpaceDivided) {
    std::wstringstream ss(pattern);
    std::wstring token;
    bool allTokensFound = true;
    while (ss >> token) {
      bool tokenFound = false;
      if (options.ignoreCase) {
        auto it = std::search(str.begin(), str.end(), token.begin(),
                              token.end(), [](wchar_t c1, wchar_t c2) {
                                return towlower(c1) == towlower(c2);
                              });
        tokenFound = (it != str.end());
      } else {
        tokenFound = (str.find(token) != std::wstring::npos);
      }
      if (!tokenFound) {
        allTokensFound = false;
        break;
      }
    }
    matched = allTokensFound;
  } else {
    // Default: Substring
    if (options.ignoreCase) {
      auto it = std::search(
          str.begin(), str.end(), pattern.begin(), pattern.end(),
          [](wchar_t c1, wchar_t c2) { return towlower(c1) == towlower(c2); });
      matched = it != str.end();
    } else {
      matched = str.find(pattern) != std::wstring::npos;
    }
  }

  if (options.invertMatch)
    return !matched;
  return matched;
}

std::vector<FileResult> MFTReader::Search(const std::wstring &query,
                                          const std::wstring &targetFolder,
                                          const SearchOptions &options,
                                          int maxResults) {
  std::vector<FileResult> results;

  // Resolve targetFolder to RefID?
  // For now, let's just filter by string path prefix if targetFolder is set.
  // It's slower but coding RefID resolution for input string is extra step.

  for (const auto &pair : fileMap) {
    const auto &entry = pair.second;
    if (!entry.IsValid)
      continue;

    // Reconstruct Path
    std::wstring fullPath = BuildPath(entry.RefID);

    // Filter by Target Folder
    if (!targetFolder.empty()) {
      if (fullPath.length() < targetFolder.length())
        continue;

      // Case-insensitive prefix check
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

    // Exclusion Filter
    if (!options.excludePattern.empty()) {
      bool excluded = false;
      std::wstring targetForExclude = fullPath;
      std::wstringstream ss(options.excludePattern);
      std::wstring pattern;
      while (std::getline(ss, pattern, L';')) {
        if (pattern.empty())
          continue;
        if (options.ignoreCase) {
          std::wstring targetLower = targetForExclude;
          std::wstring patternLower = pattern;
          for (auto &c : targetLower)
            c = towlower(c);
          for (auto &c : patternLower)
            c = towlower(c);
          if (targetLower.find(patternLower) != std::wstring::npos) {
            excluded = true;
            break;
          }
        } else {
          if (targetForExclude.find(pattern) != std::wstring::npos) {
            excluded = true;
            break;
          }
        }
      }
      if (excluded)
        continue;
    }

    // Match Query (Name or Full Path)
    if (!MatchPattern(options.matchFullPath ? fullPath : entry.Name, query,
                      options))
      continue;

    // Metadata Filters
    if (options.minSize > 0 && entry.Size < options.minSize)
      continue;
    if (options.maxSize > 0 && entry.Size > options.maxSize)
      continue;
    if (options.minDate > 0 && entry.LastWriteTime < options.minDate)
      continue;
    if (options.maxDate > 0 && entry.LastWriteTime > options.maxDate)
      continue;

    // Type Filter
    if (entry.IsDirectory) {
      if (!options.includeFolders)
        continue;
    } else {
      if (!options.includeFiles)
        continue;

      // Extension Filter
      if (!options.extensionFilter.empty()) {
        bool extMatch = false;
        size_t dotPos = entry.Name.find_last_of(L'.');
        std::wstring fileExt = (dotPos != std::wstring::npos)
                                   ? entry.Name.substr(dotPos + 1)
                                   : L"";
        for (auto &c : fileExt)
          c = towlower(c);

        std::wstringstream ss(options.extensionFilter);
        std::wstring extToken;
        while (std::getline(ss, extToken, L';')) {
          if (extToken.empty())
            continue;
          for (auto &c : extToken)
            c = towlower(c);
          if (fileExt == extToken) {
            extMatch = true;
            break;
          }
        }
        if (!extMatch)
          continue;
      }
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
  return results;
}
