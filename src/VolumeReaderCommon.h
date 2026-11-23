#pragma once
// Shared helpers for the raw-volume readers (FatReader, exFatReader).
// Everything here is header-only so no extra translation unit is needed.

#include "MFTReader.h" // FileResult, SearchOptions
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <functional>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

namespace VolumeCommon {

// Windows wchar_t is 16-bit UTF-16; the LFN / exFAT name decoders rely on it.
static_assert(sizeof(wchar_t) == 2, "UTF-16 name decoding requires 16-bit wchar_t");

constexpr size_t kDirEntrySize = 32;          // FAT and exFAT directory entries
constexpr uint32_t kMaxDirectoryDepth = 256;  // Recursion cap for directory traversal
constexpr size_t kMaxPathDepth = 512;         // Bound for parent-chain walks

struct VolumeEntry {
  uint32_t FirstCluster = 0;
  uint64_t ParentKey = 0; // Key of the parent directory entry in EntryMap; 0 = volume root
  std::wstring Name;
  uint64_t Size = 0;
  uint64_t LastWriteTime = 0; // FILETIME; 0 = unknown
  bool IsDirectory = false;
  bool IsValid = false;
};

// Keys are uint64. Real entries use their first cluster (< 2^32). Entries that
// have no usable cluster, or whose cluster is already taken, get a synthetic key
// from the 2^32+ range, which no cluster number can reach.
using EntryMap = std::unordered_map<uint64_t, VolumeEntry>;
constexpr uint64_t kSyntheticKeyBase = 1ULL << 32;

inline uint64_t AllocateSyntheticKey(uint64_t &counter) {
  return kSyntheticKeyBase + counter++;
}

// Seeks to `offset` and reads exactly `size` bytes, looping over short reads.
// The buffer is zeroed first so a partial read never exposes stale data.
// Returns false on seek failure, read failure, or end of device.
inline bool ReadVolumeExact(HANDLE hVolume, uint64_t offset, void *buffer,
                            DWORD size) {
  std::memset(buffer, 0, size);
  LARGE_INTEGER li;
  li.QuadPart = (LONGLONG)offset;
  if (!SetFilePointerEx(hVolume, li, NULL, FILE_BEGIN))
    return false;

  DWORD total = 0;
  while (total < size) {
    DWORD got = 0;
    if (!ReadFile(hVolume, static_cast<uint8_t *>(buffer) + total,
                  size - total, &got, NULL))
      return false;
    if (got == 0)
      return false; // EOF before the requested length
    total += got;
  }
  return true;
}

// Appends UTF-16LE code units from `bytes` (charCount units), stopping at the
// first NUL or 0xFFFF padding unit. Decoding is explicit so it does not depend
// on the in-memory layout of packed on-disk structs.
inline void AppendUtf16Le(std::wstring &out, const uint8_t *bytes,
                          size_t charCount) {
  for (size_t i = 0; i < charCount; ++i) {
    uint16_t unit =
        (uint16_t)(bytes[2 * i] | ((uint16_t)bytes[2 * i + 1] << 8));
    if (unit == 0x0000 || unit == 0xFFFF)
      return;
    out.push_back((wchar_t)unit);
  }
}

// Validated FILETIME conversion. Returns false for out-of-range fields.
inline bool MakeFileTime(uint32_t year, uint32_t month, uint32_t day,
                         uint32_t hour, uint32_t minute, uint32_t second,
                         uint32_t millis, uint64_t &out) {
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 ||
      minute > 59 || second > 59 || millis > 999)
    return false;
  SYSTEMTIME st = {};
  st.wYear = (WORD)year;
  st.wMonth = (WORD)month;
  st.wDay = (WORD)day;
  st.wHour = (WORD)hour;
  st.wMinute = (WORD)minute;
  st.wSecond = (WORD)second;
  st.wMilliseconds = (WORD)millis;
  FILETIME ft;
  if (!SystemTimeToFileTime(&st, &ft))
    return false;
  out = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  return true;
}

// FAT DOS date/time (2-second resolution). Returns 0 when the fields are invalid.
inline uint64_t DosDateTimeToFileTime(uint16_t date, uint16_t time) {
  uint64_t ft = 0;
  if (!MakeFileTime(1980u + (date >> 9), (date >> 5) & 0x0F, date & 0x1F,
                    time >> 11, (time >> 5) & 0x3F, (time & 0x1F) * 2u, 0,
                    ft))
    return 0;
  return ft;
}

// exFAT timestamp: 2-second units plus a 10 ms increment (0..199).
// Returns 0 when the fields are invalid.
inline uint64_t ExFatTimestampToFileTime(uint32_t ts, uint8_t tenMs) {
  if (tenMs > 199)
    return 0;
  uint64_t ft = 0;
  uint32_t second = (ts & 0x1F) * 2u + tenMs / 100;
  if (!MakeFileTime(1980u + (ts >> 25), (ts >> 21) & 0x0F, (ts >> 16) & 0x1F,
                    (ts >> 11) & 0x1F, (ts >> 5) & 0x3F, second,
                    (tenMs % 100) * 10u, ft))
    return 0;
  return ft;
}

// Full path for an entry: "X:\" + ancestors + name. The walk is bounded so a
// corrupt parent link cannot loop forever.
inline std::wstring BuildFullPath(const EntryMap &map, const VolumeEntry &entry,
                                  wchar_t drive) {
  std::wstring path = entry.Name;
  uint64_t key = entry.ParentKey;
  for (size_t hops = 0; key != 0 && hops < kMaxPathDepth; ++hops) {
    auto it = map.find(key);
    if (it == map.end())
      break;
    path = it->second.Name + L"\\" + path;
    key = it->second.ParentKey;
  }
  return std::wstring(1, drive) + L":\\" + path;
}

inline bool MatchPattern(const std::wstring &str, const std::wstring &pattern,
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

// Search over a populated EntryMap. Shared by FatReader and exFatReader.
inline std::vector<FileResult>
SearchEntries(const EntryMap &fileMap, wchar_t drive, const std::wstring &query,
              const std::wstring &targetFolder, const SearchOptions &options,
              int maxResults) {
  std::vector<FileResult> results;
  for (auto const &kv : fileMap) {
    const VolumeEntry &entry = kv.second;
    if (!MatchPattern(entry.Name, query, options))
      continue;

    std::wstring fullPath = BuildFullPath(fileMap, entry, drive);

    // Filter by Target Folder
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

    // Exclusion Filter
    if (!options.excludePattern.empty()) {
      bool excluded = false;
      std::wstringstream ss(options.excludePattern);
      std::wstring pattern;
      while (std::getline(ss, pattern, L';')) {
        if (pattern.empty())
          continue;
        if (options.ignoreCase) {
          std::wstring targetLower = fullPath;
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
          if (fullPath.find(pattern) != std::wstring::npos) {
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

      // Extension Filter (Logical AND with name match)
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

} // namespace VolumeCommon
