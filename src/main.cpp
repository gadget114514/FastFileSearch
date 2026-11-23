#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "FatReader.h"
#include "Localization.h"
#include "MFTReader.h"
#include "exFatReader.h"
#include "resource.h"
#include <atomic>
#include <commctrl.h>
#include <commdlg.h>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <sstream>

#if 0
#include <iomanip>
#endif
#include <algorithm>
#include <exception>
#include <memory>
#include <process.h>
#include <regex>
#include <set>
#include <shlobj.h>
#include <string>
#include <vector>
#include <windows.h>
#include <shellapi.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

// ---------------------------------------------------------------------------
// Messages and tunables
// ---------------------------------------------------------------------------
// Worker -> UI messages. LPARAM of the two result messages is a heap-allocated
// ScanOutcome* owned by the receiver.
constexpr UINT WM_APP_SEARCH_DONE = WM_APP + 1;
constexpr UINT WM_APP_SEARCH_ERROR = WM_APP + 2;
constexpr UINT WM_APP_PROGRESS = WM_APP + 3; // WPARAM = percent

constexpr UINT_PTR kStartupGuardTimerId = 1;
constexpr UINT kStartupGuardMs = 1000;
constexpr size_t kMaxSearchResults = 50000;
constexpr int kMaxQueryChars = 256;
constexpr int kMaxConfiguredTargets = 1000;
constexpr int kIniValueChars = 64;
constexpr int kDateBufChars = 64;

// ---------------------------------------------------------------------------
// Application state. Only the UI thread touches this, except for the atomics
// and the ScanRequest/ScanOutcome snapshots passed to the worker.
// ---------------------------------------------------------------------------
struct ScanRequest {
  HWND hDlg = nullptr;
  std::wstring query;
  std::vector<std::wstring> targets;
  SearchOptions options;
  int codePage = CP_OEMCP;
};

struct ScanOutcome {
  std::vector<FileResult> results;
  std::wstring message; // Error text (ERROR) or per-drive warnings (DONE).
};

struct AppState {
  HINSTANCE hInstance = nullptr;
  HWND hList = nullptr;
  HWND hTargetList = nullptr;
  int currentLang = APP_LANG_ENGLISH;
  int currentCodePage = CP_OEMCP;
  std::vector<std::wstring> searchTargets;
  SearchOptions options;
  std::vector<FileResult> searchResults; // UI thread only
  int sortColumn = -1;
  bool sortAscending = true;

  std::atomic<bool> isSearching{false};
  std::atomic<bool> scanCancelRequested{false};
  HANDLE scanThread = nullptr; // UI thread only

  // Startup guard subclassing (restored once the guard is released).
  WNDPROC oldEditProc = nullptr;
  WNDPROC oldListProc = nullptr;
  WNDPROC oldTargetListProc = nullptr;
  bool isStartupGuarded = true;
};

AppState g_app;
MFTReader mftReader;
FatReader fatReader;
exFatReader exFatReaderObj;

// Prototypes
void PopulateList();
bool SaveConfig(HWND hDlg);
void LoadConfig(HWND hDlg);
void ResizeLayout(HWND hDlg, int cx, int cy);
int CALLBACK CompareFunc(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort);
std::wstring FormatSize(uint64_t size);
INT_PTR CALLBACK ConfigDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam,
                               LPARAM lParam);

// Guarded Procedures to ignore disruptive messages during startup
LRESULT CALLBACK GuardedEditProc(HWND hWnd, UINT uMsg, WPARAM wParam,
                                 LPARAM lParam) {
  if (g_app.isStartupGuarded) {
    if (uMsg == WM_KILLFOCUS)
      return 0; // Ignore focus loss during guard
  }
  return CallWindowProc(g_app.oldEditProc, hWnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK GuardedListProc(HWND hWnd, UINT uMsg, WPARAM wParam,
                                 LPARAM lParam) {
  if (g_app.isStartupGuarded) {
    // Ignore all mouse messages and focus/capture during guard to prevent
    // "dragging mode"
    if (uMsg >= WM_MOUSEFIRST && uMsg <= WM_MOUSELAST)
      return 0;
    if (uMsg == WM_SETFOCUS || uMsg == WM_KILLFOCUS)
      return 0;
    if (uMsg == WM_CAPTURECHANGED || uMsg == WM_CANCELMODE)
      return 0;
    if (uMsg == WM_LBUTTONDOWN || uMsg == WM_LBUTTONUP || uMsg == WM_MOUSEMOVE)
      return 0;
  }
  return CallWindowProc(g_app.oldListProc, hWnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK TargetListProc(HWND hWnd, UINT uMsg, WPARAM wParam,
                                LPARAM lParam) {
  if (uMsg == WM_KEYDOWN && wParam == VK_DELETE) {
    SendMessage(GetParent(hWnd), WM_COMMAND, IDC_BTN_REMOVE, 0);
    return 0;
  }
  return CallWindowProc(g_app.oldTargetListProc, hWnd, uMsg, wParam, lParam);
}

// Restores the original window procedures of the startup-guarded controls and
// releases the guard. Called from the guard timer, or immediately if the timer
// could not be created (so input can never be swallowed indefinitely).
void ReleaseStartupGuard(HWND hDlg) {
  g_app.isStartupGuarded = false;
  HWND hEdit = GetDlgItem(hDlg, IDC_EDIT_QUERY);
  if (g_app.oldEditProc && hEdit) {
    SetWindowLongPtr(hEdit, GWLP_WNDPROC, (LONG_PTR)g_app.oldEditProc);
    g_app.oldEditProc = nullptr;
  }
  if (g_app.oldListProc && g_app.hList) {
    SetWindowLongPtr(g_app.hList, GWLP_WNDPROC, (LONG_PTR)g_app.oldListProc);
    g_app.oldListProc = nullptr;
  }
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
std::wstring FormatWin32Error(DWORD code) {
  LPWSTR buf = nullptr;
  DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                 FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                             (LPWSTR)&buf, 0, NULL);
  std::wstring text;
  if (len && buf)
    text.assign(buf, len);
  if (buf)
    LocalFree(buf);
  while (!text.empty() &&
         (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
    text.pop_back();
  if (text.empty())
    text = L"error " + std::to_wstring(code);
  return text;
}

// Win32 buffers passed to controls that may write to them must be mutable;
// never cast away const on c_str().
std::vector<wchar_t> MutableCopy(const std::wstring &s) {
  std::vector<wchar_t> buf(s.begin(), s.end());
  buf.push_back(L'\0');
  return buf;
}

std::wstring Utf8ToWide(const char *text) {
  if (!text)
    return L"";
  int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
  if (len <= 0)
    return L"";
  std::vector<wchar_t> buf(static_cast<size_t>(len));
  MultiByteToWideChar(CP_UTF8, 0, text, -1, buf.data(), len);
  return buf.data();
}

std::wstring JoinLines(const std::vector<std::wstring> &lines) {
  std::wstring out;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i)
      out += L"\n";
    out += lines[i];
  }
  return out;
}

// Builds a per-drive error. |detail| is the failing reader's own message (or a
// short description); |win32Error| must be captured immediately after the
// failing call. Access denial gets an "run as administrator" hint.
std::wstring BuildDriveError(wchar_t drive, const std::wstring &detail,
                             DWORD win32Error) {
  std::wstring msg = L"Drive ";
  msg += drive;
  msg += L": ";
  msg += detail.empty() ? FormatWin32Error(win32Error) : detail;
  if (win32Error != 0 && !detail.empty())
    msg += L" (" + FormatWin32Error(win32Error) + L")";
  if (win32Error == ERROR_ACCESS_DENIED) {
    msg += L"\n";
    msg += Localization::GetString(IDS_ERR_ACCESS_DENIED_HINT);
  }
  return msg;
}

bool IsSupportedCodePage(int cp) {
  return cp == CP_OEMCP || cp == 932 || cp == 1252 || cp == 936 ||
         cp == 950 || cp == CP_UTF8;
}

// Reads a SearchOptions size/date value; negative or malformed -> 0 (disabled).
uint64_t ReadNonNegativeSetting(const wchar_t *key, const std::wstring &iniPath) {
  wchar_t valBuf[kIniValueChars] = {0};
  GetPrivateProfileStringW(L"SearchOptions", key, L"0", valBuf, kIniValueChars,
                           iniPath.c_str());
  long long value = _wtoi64(valBuf);
  return value < 0 ? 0 : static_cast<uint64_t>(value);
}

std::wstring FormatDate(uint64_t fileTime) {
  FILETIME ft;
  ft.dwLowDateTime = (DWORD)fileTime;
  ft.dwHighDateTime = (DWORD)(fileTime >> 32);

  SYSTEMTIME stUTC, stLocal;
  if (!FileTimeToSystemTime(&ft, &stUTC) ||
      !SystemTimeToTzSpecificLocalTime(NULL, &stUTC, &stLocal))
    return L"";

  wchar_t buf[kDateBufChars];
  swprintf_s(buf, L"%04d/%02d/%02d %02d:%02d:%02d", stLocal.wYear,
             stLocal.wMonth, stLocal.wDay, stLocal.wHour, stLocal.wMinute,
             stLocal.wSecond);
  return buf;
}

std::wstring FormatSize(uint64_t size) {
  if (size < 1024)
    return std::to_wstring(size) + L" B";
  if (size < 1024 * 1024)
    return std::to_wstring(size / 1024) + L" KB";
  if (size < 1024 * 1024 * 1024)
    return std::to_wstring(size / (1024 * 1024)) + L" MB";
  return std::to_wstring(size / (1024 * 1024 * 1024)) + L" GB";
}

void AdjustButtonToText(HWND hBtn) {
  if (!hBtn)
    return;
  wchar_t buf[256];
  GetWindowTextW(hBtn, buf, 256);
  HDC hdc = GetDC(hBtn);
  HFONT hFont = (HFONT)SendMessage(hBtn, WM_GETFONT, 0, 0);
  HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

  SIZE sz;
  GetTextExtentPoint32W(hdc, buf, (int)wcslen(buf), &sz);

  SelectObject(hdc, hOldFont);
  ReleaseDC(hBtn, hdc);

  // Padding: 25px (covers button padding or checkbox icon)
  int width = sz.cx + 25;
  if (width < 60)
    width = 60; // Minimum width

  RECT rc;
  GetWindowRect(hBtn, &rc);
  SetWindowPos(hBtn, NULL, 0, 0, width, rc.bottom - rc.top,
               SWP_NOMOVE | SWP_NOZORDER);
}

void UpdateLanguage(HWND hDlg) {
  SetDlgItemTextW(hDlg, IDC_BTN_SEARCH, Localization::GetString(IDS_SEARCH));
  SetDlgItemTextW(hDlg, IDC_STATIC_FILENAME,
                  Localization::GetString(IDS_FILENAME_LABEL));
  SetDlgItemTextW(hDlg, IDC_BTN_SAVE, Localization::GetString(IDS_BTN_SAVE));
  SetDlgItemTextW(hDlg, IDC_STATIC_TARGETS,
                  Localization::GetString(IDS_LBL_TARGETS));
  SetDlgItemTextW(hDlg, IDC_BTN_ADD, Localization::GetString(IDS_BTN_ADD));
  SetDlgItemTextW(hDlg, IDC_BTN_REMOVE,
                  Localization::GetString(IDS_BTN_REMOVE));
  SetDlgItemTextW(hDlg, IDC_CHECK_NOT, Localization::GetString(IDS_CHK_NOT));

  // Update List Columns (LVM_SETCOLUMN copies the text; it is not written to)
  LVCOLUMNW lvc;
  lvc.mask = LVCF_TEXT;
  lvc.pszText = (LPWSTR)Localization::GetString(IDS_COL_NAME);
  SendMessage(g_app.hList, LVM_SETCOLUMNW, 0, (LPARAM)&lvc);
  lvc.pszText = (LPWSTR)Localization::GetString(IDS_COL_PATH);
  SendMessage(g_app.hList, LVM_SETCOLUMNW, 1, (LPARAM)&lvc);
  lvc.pszText = (LPWSTR)Localization::GetString(IDS_COL_DATE);
  SendMessage(g_app.hList, LVM_SETCOLUMNW, 2, (LPARAM)&lvc);
  lvc.pszText = (LPWSTR)Localization::GetString(IDS_COL_SIZE);
  SendMessage(g_app.hList, LVM_SETCOLUMNW, 3, (LPARAM)&lvc);

  SetDlgItemTextW(hDlg, IDC_STATIC_CP,
                  Localization::GetString(IDS_LBL_CODEPAGE));

  // Update Menu
  HMENU hMenu = GetMenu(hDlg);
  if (hMenu) {
    // Top Level
    ModifyMenuW(hMenu, 0, MF_BYPOSITION | MF_STRING | MF_POPUP,
                (UINT_PTR)GetSubMenu(hMenu, 0),
                Localization::GetString(IDS_MENU_CONFIG));
    ModifyMenuW(hMenu, 1, MF_BYPOSITION | MF_STRING | MF_POPUP,
                (UINT_PTR)GetSubMenu(hMenu, 1),
                Localization::GetString(IDS_MENU_LANGUAGE));
    ModifyMenuW(hMenu, 2, MF_BYPOSITION | MF_STRING | MF_POPUP,
                (UINT_PTR)GetSubMenu(hMenu, 2),
                Localization::GetString(IDS_MENU_HELP));

    // Items
    ModifyMenuW(hMenu, ID_CONFIG_OPTIONS, MF_BYCOMMAND | MF_STRING,
                ID_CONFIG_OPTIONS,
                Localization::GetString(IDS_MENU_SEARCH_OPTIONS));
    ModifyMenuW(hMenu, ID_HELP_ABOUT, MF_BYCOMMAND | MF_STRING, ID_HELP_ABOUT,
                Localization::GetString(IDS_MENU_ABOUT));

    DrawMenuBar(hDlg);
  }

  // Adjust sizes based on new text
  AdjustButtonToText(GetDlgItem(hDlg, IDC_BTN_SEARCH));
  AdjustButtonToText(GetDlgItem(hDlg, IDC_BTN_SAVE));
  AdjustButtonToText(GetDlgItem(hDlg, IDC_BTN_ADD));
  AdjustButtonToText(GetDlgItem(hDlg, IDC_BTN_REMOVE));
  AdjustButtonToText(GetDlgItem(hDlg, IDC_CHECK_NOT));

  // Determine Min Width
  int m = 10;

  // 1. Top Row: Label + Edit(150) + Not + Search
  RECT rcS, rcNot, rcLbl;
  GetWindowRect(GetDlgItem(hDlg, IDC_BTN_SEARCH), &rcS);
  GetWindowRect(GetDlgItem(hDlg, IDC_CHECK_NOT), &rcNot);

  // Measure Label explicitly (as AdjustButtonToText doesn't run on Label)
  {
    wchar_t buf[256];
    GetDlgItemTextW(hDlg, IDC_STATIC_FILENAME, buf, 256);
    HDC hdc = GetDC(GetDlgItem(hDlg, IDC_STATIC_FILENAME));
    HFONT hFont = (HFONT)SendDlgItemMessage(hDlg, IDC_STATIC_FILENAME,
                                            WM_GETFONT, 0, 0);
    HFONT hOld = (HFONT)SelectObject(hdc, hFont);
    SIZE sz;
    GetTextExtentPoint32W(hdc, buf, (int)wcslen(buf), &sz);
    SelectObject(hdc, hOld);
    ReleaseDC(GetDlgItem(hDlg, IDC_STATIC_FILENAME), hdc);
    // Resize Label Control
    SetWindowPos(GetDlgItem(hDlg, IDC_STATIC_FILENAME), NULL, 0, 0, sz.cx + 5,
                 sz.cy, SWP_NOMOVE | SWP_NOZORDER);
    GetWindowRect(GetDlgItem(hDlg, IDC_STATIC_FILENAME), &rcLbl);
  }

  int lblW = rcLbl.right - rcLbl.left;
  int notW = rcNot.right - rcNot.left;
  int searchW = rcS.right - rcS.left;

  int minW_Top = m + lblW + m + 150 + m + notW + m + searchW + m;

  // 2. Middle: List(100) + Btns (Add/Rem)
  RECT rcAdd, rcRem, rcTLabel;
  GetWindowRect(GetDlgItem(hDlg, IDC_BTN_ADD), &rcAdd);
  GetWindowRect(GetDlgItem(hDlg, IDC_BTN_REMOVE), &rcRem);

  // Measure "Search Candidates" Label
  {
    wchar_t buf[256];
    GetDlgItemTextW(hDlg, IDC_STATIC_TARGETS, buf, 256);
    HDC hdc = GetDC(GetDlgItem(hDlg, IDC_STATIC_TARGETS));
    HFONT hFont = (HFONT)SendDlgItemMessage(hDlg, IDC_STATIC_TARGETS,
                                            WM_GETFONT, 0, 0);
    HFONT hOld = (HFONT)SelectObject(hdc, hFont);
    SIZE sz;
    GetTextExtentPoint32W(hdc, buf, (int)wcslen(buf), &sz);
    SelectObject(hdc, hOld);
    ReleaseDC(GetDlgItem(hDlg, IDC_STATIC_TARGETS), hdc);
    SetWindowPos(GetDlgItem(hDlg, IDC_STATIC_TARGETS), NULL, 0, 0, sz.cx + 5,
                 sz.cy, SWP_NOMOVE | SWP_NOZORDER);
    GetWindowRect(GetDlgItem(hDlg, IDC_STATIC_TARGETS), &rcTLabel);
  }

  int addW = rcAdd.right - rcAdd.left;
  int remW = rcRem.right - rcRem.left;
  int tLblW = rcTLabel.right - rcTLabel.left;

  int maxBtnW = (addW > remW) ? addW : remW;
  int minW_Mid = m + 100 + m + maxBtnW + m;
  int minW_Lbl = m + tLblW + m;

  int reqWidth = minW_Top;
  if (minW_Mid > reqWidth)
    reqWidth = minW_Mid;
  if (minW_Lbl > reqWidth)
    reqWidth = minW_Lbl;

  // Current Client Width
  RECT rcClient;
  GetClientRect(hDlg, &rcClient);

  if (rcClient.right < reqWidth) {
    // Resize Window
    RECT rcWin;
    GetWindowRect(hDlg, &rcWin);
    int diff = reqWidth - rcClient.right;
    SetWindowPos(hDlg, NULL, 0, 0, (rcWin.right - rcWin.left) + diff,
                 rcWin.bottom - rcWin.top, SWP_NOMOVE | SWP_NOZORDER);
    GetClientRect(hDlg, &rcClient); // Update
  }

  // Refresh Layout
  ResizeLayout(hDlg, rcClient.right, rcClient.bottom);
}

// ---------------------------------------------------------------------------
// Search worker
//
// The worker never touches window or dialog state. It works only on the
// ScanRequest snapshot and the reader objects (used exclusively by this
// thread), and reports back by posting a heap-allocated ScanOutcome.
// ---------------------------------------------------------------------------

// Progress callback used by the readers. Runs on the worker thread: it only
// posts a message to the dialog.
void ScanProgress(int percent, int /*max*/, void *userData) {
  PostMessageW((HWND)userData, WM_APP_PROGRESS, (WPARAM)percent, 0);
}

// Scans every drive named by the request. Returns true if at least one drive
// scanned successfully. Per-drive problems are collected into outcome.message.
bool RunScan(const ScanRequest &req, ScanOutcome &outcome) {
  std::set<wchar_t> drivesToScan;
  for (const auto &target : req.targets) {
    if (target.length() >= 3 && target[1] == L':')
      drivesToScan.insert(towupper(target[0]));
  }

  bool anySuccess = false;
  std::vector<std::wstring> problems;

  for (wchar_t drive : drivesToScan) {
    if (g_app.scanCancelRequested.load())
      break;

    wchar_t driveRoot[] = {drive, L':', L'\\', L'\0'};
    wchar_t fsName[MAX_PATH] = {0};
    if (!GetVolumeInformationW(driveRoot, NULL, 0, NULL, NULL, NULL, fsName,
                               MAX_PATH)) {
      problems.push_back(BuildDriveError(
          drive, L"cannot read volume information", GetLastError()));
      continue;
    }

    const bool isNtfs = (wcscmp(fsName, L"NTFS") == 0);
    const bool isFat =
        (wcscmp(fsName, L"FAT") == 0 || wcscmp(fsName, L"FAT32") == 0);
    const bool isExFat = (wcscmp(fsName, L"exFAT") == 0);
    if (!isNtfs && !isFat && !isExFat) {
      problems.push_back(BuildDriveError(
          drive, std::wstring(L"unsupported file system (") + fsName + L")",
          0));
      continue;
    }

    // Reset progress for this drive.
    ScanProgress(0, 100, req.hDlg);

    // Capture the reader's own message and GetLastError() immediately after a
    // failure, before anything else can overwrite them.
    bool scanSuccess = false;
    DWORD failCode = 0;
    std::wstring failDetail;
    if (isNtfs) {
      if (mftReader.Initialize(drive) &&
          mftReader.Scan(ScanProgress, req.hDlg)) {
        scanSuccess = true;
      } else {
        failCode = GetLastError();
        failDetail = mftReader.GetLastErrorMessage();
      }
    } else if (isFat) {
      if (fatReader.Initialize(drive) &&
          fatReader.Scan(req.codePage, ScanProgress, req.hDlg)) {
        scanSuccess = true;
      } else {
        failCode = GetLastError();
        failDetail = fatReader.GetLastErrorMessage();
      }
    } else {
      if (exFatReaderObj.Initialize(drive) &&
          exFatReaderObj.Scan(ScanProgress, req.hDlg)) {
        scanSuccess = true;
      } else {
        failCode = GetLastError();
        failDetail = exFatReaderObj.GetLastErrorMessage();
      }
    }

    ScanProgress(100, 100, req.hDlg); // Ensure 100% at end

    if (!scanSuccess) {
      // A cancelled MFT scan also reports failure; don't report it as an error.
      if (g_app.scanCancelRequested.load())
        break;
      problems.push_back(BuildDriveError(
          drive, failDetail.empty() ? L"scan failed" : failDetail, failCode));
      continue;
    }

    for (const auto &targetFolder : req.targets) {
      if (targetFolder.length() < 3 || towupper(targetFolder[0]) != drive)
        continue;
      if (g_app.scanCancelRequested.load())
        break;

      std::vector<FileResult> results;
      const int maxResults = static_cast<int>(kMaxSearchResults);
      if (isNtfs)
        results = mftReader.Search(req.query, targetFolder, req.options,
                                   maxResults);
      else if (isFat)
        results = fatReader.Search(req.query, targetFolder, req.codePage,
                                   req.options, maxResults);
      else
        results = exFatReaderObj.Search(req.query, targetFolder, req.options,
                                        maxResults);

      outcome.results.insert(outcome.results.end(), results.begin(),
                             results.end());
      if (outcome.results.size() >= kMaxSearchResults)
        break;
    }
    anySuccess = true;
  }

  outcome.message = JoinLines(problems);
  return anySuccess;
}

unsigned __stdcall ScanThreadMain(void *param) {
  std::unique_ptr<ScanRequest> req(static_cast<ScanRequest *>(param));
  auto outcome = std::make_unique<ScanOutcome>();
  UINT doneMessage = WM_APP_SEARCH_DONE;

  // Exception boundary: every path posts exactly one result message, so the
  // UI always gets to clear isSearching.
  try {
    if (!RunScan(*req, *outcome))
      doneMessage = WM_APP_SEARCH_ERROR;
  } catch (const std::exception &e) {
    outcome->results.clear();
    outcome->message = Utf8ToWide(e.what());
    doneMessage = WM_APP_SEARCH_ERROR;
  } catch (...) {
    outcome->results.clear();
    outcome->message = Localization::GetString(IDS_ERR_UNKNOWN_SCAN);
    doneMessage = WM_APP_SEARCH_ERROR;
  }

  ScanOutcome *raw = outcome.release();
  if (!PostMessageW(req->hDlg, doneMessage, 0, reinterpret_cast<LPARAM>(raw)))
    delete raw; // Dialog is gone; nobody will consume the result.
  return 0;
}

// Waits for the worker to exit and closes its handle (UI thread only).
void JoinScanThread() {
  if (!g_app.scanThread)
    return;
  WaitForSingleObject(g_app.scanThread, INFINITE);
  CloseHandle(g_app.scanThread);
  g_app.scanThread = nullptr;
}

void StartScan(HWND hDlg) {
  auto req = std::make_unique<ScanRequest>();
  req->hDlg = hDlg;

  // Snapshot everything the worker needs while still on the UI thread.
  wchar_t queryBuf[kMaxQueryChars] = {0};
  GetDlgItemTextW(hDlg, IDC_EDIT_QUERY, queryBuf, kMaxQueryChars);
  req->query = queryBuf;
  req->targets = g_app.searchTargets;
  req->options = g_app.options;
  req->codePage = g_app.currentCodePage;

  g_app.searchResults.clear();
  ListView_DeleteAllItems(g_app.hList);
  SetDlgItemTextW(hDlg, IDC_STATUS, Localization::GetString(IDS_STATUS_BUSY));

  mftReader.ResetCancel();
  g_app.scanCancelRequested = false;
  g_app.isSearching = true;

  uintptr_t thread =
      _beginthreadex(nullptr, 0, ScanThreadMain, req.get(), 0, nullptr);
  if (thread == 0) {
    g_app.isSearching = false;
    SetDlgItemTextW(hDlg, IDC_STATUS, Localization::GetString(IDS_STATUS_FAILED));
    MessageBoxW(hDlg, Localization::GetString(IDS_ERR_UNKNOWN_SCAN),
                Localization::GetString(IDS_TITLE_ERROR),
                MB_ICONERROR | MB_OK);
    return; // req is still owned here and is freed.
  }
  req.release(); // The worker now owns the request.
  g_app.scanThread = reinterpret_cast<HANDLE>(thread);
}

// Requests cancellation and waits for the worker before the dialog closes, so
// no worker can post to a destroyed window or outlive the UI state it reads.
// Results from the abandoned scan are discarded.
void CancelScan(HWND hDlg) {
  if (!g_app.scanThread)
    return;
  g_app.scanCancelRequested = true;
  mftReader.RequestCancel(); // Stops MFT scans; FAT/exFAT scans finish their
                             // current drive before the flag is observed.
  JoinScanThread();
  g_app.isSearching = false;

  MSG msg;
  while (PeekMessageW(&msg, hDlg, WM_APP_SEARCH_DONE, WM_APP_SEARCH_ERROR,
                      PM_REMOVE)) {
    delete reinterpret_cast<ScanOutcome *>(msg.lParam);
  }
}

// Handles WM_APP_SEARCH_DONE / WM_APP_SEARCH_ERROR on the UI thread. Takes
// ownership of the posted ScanOutcome.
void HandleScanFinished(HWND hDlg, UINT msg, LPARAM lParam) {
  std::unique_ptr<ScanOutcome> outcome(
      reinterpret_cast<ScanOutcome *>(lParam));
  JoinScanThread();
  g_app.isSearching = false;

  if (msg == WM_APP_SEARCH_DONE) {
    g_app.searchResults = std::move(outcome->results);
    PopulateList();
    SetDlgItemTextW(
        hDlg, IDC_STATUS,
        Localization::FormatCount(IDS_STATUS_FOUND_FMT, g_app.searchResults.size())
            .c_str());
    if (!outcome->message.empty()) {
      MessageBoxW(hDlg, outcome->message.c_str(),
                  Localization::GetString(IDS_TITLE_ERROR),
                  MB_ICONWARNING | MB_OK);
    }
  } else {
    std::wstring err = outcome->message.empty()
                           ? Localization::GetString(IDS_ERR_UNKNOWN_SCAN)
                           : outcome->message;
    MessageBoxW(hDlg, err.c_str(), Localization::GetString(IDS_TITLE_ERROR),
                MB_ICONERROR | MB_OK);
    SetDlgItemTextW(hDlg, IDC_STATUS, Localization::GetString(IDS_STATUS_FAILED));
  }
}

// ---------------------------------------------------------------------------
// Results list, file dialogs and clipboard
// ---------------------------------------------------------------------------
void PopulateList() {
  ListView_DeleteAllItems(g_app.hList);
  SendMessage(g_app.hList, WM_SETREDRAW, FALSE, 0);

  for (size_t i = 0; i < g_app.searchResults.size(); ++i) {
    const FileResult &res = g_app.searchResults[i];

    LVITEM lvI = {};
    lvI.mask = LVIF_TEXT | LVIF_PARAM | LVIF_STATE;
    lvI.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    lvI.state = 0;
    lvI.iItem = (int)i;
    lvI.iSubItem = 0;
    lvI.lParam = (LPARAM)i;
    auto nameBuf = MutableCopy(res.Name);
    lvI.pszText = nameBuf.data();
    int idx = ListView_InsertItem(g_app.hList, &lvI);

    auto pathBuf = MutableCopy(res.FullPath);
    ListView_SetItemText(g_app.hList, idx, 1, pathBuf.data());

    auto dateBuf = MutableCopy(FormatDate(res.LastWriteTime));
    ListView_SetItemText(g_app.hList, idx, 2, dateBuf.data());

    auto sizeBuf = MutableCopy(FormatSize(res.Size));
    ListView_SetItemText(g_app.hList, idx, 3, sizeBuf.data());
  }

  SendMessage(g_app.hList, WM_SETREDRAW, TRUE, 0);

  // 1. Force focus away immediately
  SetFocus(GetDlgItem(GetParent(g_app.hList), IDC_EDIT_QUERY));

  // 2. Clear state strictly
  ListView_SetItemState(g_app.hList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
  ListView_SetSelectionMark(g_app.hList, -1);

  // 3. Force Repaint
  InvalidateRect(g_app.hList, NULL, TRUE);
  UpdateWindow(g_app.hList);
}

void SaveResults(HWND hDlg) {
  OPENFILENAMEW ofn;
  wchar_t fileName[MAX_PATH] = L"scan_result.txt";

  ZeroMemory(&ofn, sizeof(ofn));
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hDlg;
  ofn.lpstrFilter = L"Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
  ofn.lpstrFile = fileName;
  ofn.nMaxFile = MAX_PATH;
  // New file names must be allowed, so OFN_FILEMUSTEXIST is not set.
  ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT;
  ofn.lpstrDefExt = L"txt";

  if (!GetSaveFileNameW(&ofn))
    return; // Cancelled (or dialog failed): nothing to save.

  std::wofstream outfile(fileName);
  if (!outfile) {
    MessageBoxW(hDlg, Localization::GetString(IDS_ERR_SAVE_FAILED),
                Localization::GetString(IDS_TITLE_ERROR), MB_ICONERROR | MB_OK);
    return;
  }
  outfile << L"Name\tPath\tDate\tSize\n"; // Header
  for (const auto &res : g_app.searchResults) {
    outfile << res.Name << L"\t" << res.FullPath << L"\t"
            << FormatDate(res.LastWriteTime) << L"\t" << res.Size << L"\n";
  }
  outfile.close();
  if (outfile.fail()) {
    MessageBoxW(hDlg, Localization::GetString(IDS_ERR_SAVE_FAILED),
                Localization::GetString(IDS_TITLE_ERROR), MB_ICONERROR | MB_OK);
  }
}

void AddFolder(HWND hDlg) {
  BROWSEINFOW bi = {0};
  bi.hwndOwner = hDlg;
  bi.lpszTitle = L"Select Folder to Search";
  bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

  LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
  if (pidl != 0) {
    wchar_t path[MAX_PATH];
    if (SHGetPathFromIDListW(pidl, path)) {
      // Check duplicates
      bool found = false;
      for (const auto &t : g_app.searchTargets) {
        if (t == path)
          found = true;
      }
      if (!found) {
        g_app.searchTargets.push_back(path);
        SendMessage(g_app.hTargetList, LB_ADDSTRING, 0, (LPARAM)path);
      }
    }
    CoTaskMemFree(pidl);
  }
}

void RemoveFolder(HWND hDlg) {
  int idx = (int)SendMessage(g_app.hTargetList, LB_GETCURSEL, 0, 0);
  if (idx != LB_ERR) {
    wchar_t buf[MAX_PATH];
    SendMessage(g_app.hTargetList, LB_GETTEXT, idx, (LPARAM)buf);
    SendMessage(g_app.hTargetList, LB_DELETESTRING, idx, 0);

    // Remove from vector
    for (auto it = g_app.searchTargets.begin(); it != g_app.searchTargets.end();
         ++it) {
      if (*it == buf) {
        g_app.searchTargets.erase(it);
        break;
      }
    }
  }
}

std::wstring GetConfigPath() {
  wchar_t path[MAX_PATH];
  if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL,
                                 SHGFP_TYPE_CURRENT, path))) {
    std::wstring dir = std::wstring(path) + L"\\FastFileSearch";
    return dir + L"\\FastFileSearch.ini";
  }
  // Fallback to local
  return L"FastFileSearch.ini";
}

// Creates the directory that holds |iniPath|, if it has one. Returns false if
// it could not be created.
bool EnsureConfigDirectory(const std::wstring &iniPath) {
  const size_t sep = iniPath.find_last_of(L'\\');
  if (sep == std::wstring::npos)
    return true; // Relative path in the working directory.
  const std::wstring dir = iniPath.substr(0, sep);
  if (CreateDirectoryW(dir.c_str(), NULL))
    return true;
  return GetLastError() == ERROR_ALREADY_EXISTS;
}

bool SaveConfig(HWND hDlg) {
  const std::wstring iniPath = GetConfigPath();
  bool ok = EnsureConfigDirectory(iniPath);

  auto put = [&](const std::wstring &section, const std::wstring &key,
                 const std::wstring &value) {
    if (!WritePrivateProfileStringW(section.c_str(), key.c_str(), value.c_str(),
                                    iniPath.c_str()))
      ok = false;
  };

  // Save Window Size
  RECT rc;
  if (GetWindowRect(hDlg, &rc)) {
    put(L"Settings", L"Width", std::to_wstring(rc.right - rc.left));
    put(L"Settings", L"Height", std::to_wstring(rc.bottom - rc.top));
  }

  put(L"Settings", L"Language", std::to_wstring(g_app.currentLang));
  put(L"Settings", L"CodePage", std::to_wstring(g_app.currentCodePage));

  // Search Options
  const SearchOptions &opt = g_app.options;
  put(L"SearchOptions", L"Mode", std::to_wstring((int)opt.mode));
  put(L"SearchOptions", L"IgnoreCase", std::to_wstring(opt.ignoreCase ? 1 : 0));
  put(L"SearchOptions", L"MinSize", std::to_wstring(opt.minSize));
  put(L"SearchOptions", L"MaxSize", std::to_wstring(opt.maxSize));
  put(L"SearchOptions", L"MinDate", std::to_wstring(opt.minDate));
  put(L"SearchOptions", L"MaxDate", std::to_wstring(opt.maxDate));
  put(L"SearchOptions", L"IncludeFiles",
      std::to_wstring(opt.includeFiles ? 1 : 0));
  put(L"SearchOptions", L"IncludeFolders",
      std::to_wstring(opt.includeFolders ? 1 : 0));
  put(L"SearchOptions", L"ExtensionFilter", opt.extensionFilter);
  put(L"SearchOptions", L"MatchFullPath",
      std::to_wstring(opt.matchFullPath ? 1 : 0));
  put(L"SearchOptions", L"ExcludePattern", opt.excludePattern);
  put(L"SearchOptions", L"InvertMatch", std::to_wstring(opt.invertMatch ? 1 : 0));

  // Clear old Targets (NULL key deletes the whole section)
  if (!WritePrivateProfileStringW(L"Targets", NULL, NULL, iniPath.c_str()))
    ok = false;

  put(L"Targets", L"Count", std::to_wstring(g_app.searchTargets.size()));
  for (size_t i = 0; i < g_app.searchTargets.size(); ++i) {
    put(L"Targets", L"Target" + std::to_wstring(i), g_app.searchTargets[i]);
  }
  return ok;
}

void LoadConfig(HWND hDlg) {
  std::wstring iniPath = GetConfigPath();

  // Load Window Size
  int w = GetPrivateProfileIntW(L"Settings", L"Width", 0, iniPath.c_str());
  int h = GetPrivateProfileIntW(L"Settings", L"Height", 0, iniPath.c_str());

  int screenW = GetSystemMetrics(SM_CXSCREEN);
  int screenH = GetSystemMetrics(SM_CYSCREEN);

  if (w > 300 && w <= screenW && h > 200 && h <= screenH) {
    SetWindowPos(hDlg, NULL, 0, 0, w, h,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }

  // Load Language
  g_app.currentLang =
      GetPrivateProfileIntW(L"Settings", L"Language", 0, iniPath.c_str());
  if (g_app.currentLang < 0 || g_app.currentLang >= APP_LANG_COUNT)
    g_app.currentLang = 0;
  Localization::SetLanguage((LanguageID)g_app.currentLang);

  // Load Code Page (unsupported values fall back to the OEM code page)
  int codePage = GetPrivateProfileIntW(L"Settings", L"CodePage", CP_OEMCP,
                                       iniPath.c_str());
  g_app.currentCodePage = IsSupportedCodePage(codePage) ? codePage : CP_OEMCP;

  // Load Search Options (enum range validated)
  int mode = GetPrivateProfileIntW(L"SearchOptions", L"Mode", 0, iniPath.c_str());
  if (mode < MatchMode_Substring || mode > MatchMode_RegEx)
    mode = MatchMode_Substring;
  g_app.options.mode = (MatchMode)mode;
  g_app.options.ignoreCase = GetPrivateProfileIntW(L"SearchOptions", L"IgnoreCase",
                                                   1, iniPath.c_str()) != 0;

  g_app.options.minSize = ReadNonNegativeSetting(L"MinSize", iniPath);
  g_app.options.maxSize = ReadNonNegativeSetting(L"MaxSize", iniPath);
  g_app.options.minDate = ReadNonNegativeSetting(L"MinDate", iniPath);
  g_app.options.maxDate = ReadNonNegativeSetting(L"MaxDate", iniPath);

  g_app.options.includeFiles =
      GetPrivateProfileIntW(L"SearchOptions", L"IncludeFiles", 1,
                            iniPath.c_str()) != 0;
  g_app.options.includeFolders =
      GetPrivateProfileIntW(L"SearchOptions", L"IncludeFolders", 1,
                            iniPath.c_str()) != 0;

  wchar_t extBuf[256];
  GetPrivateProfileStringW(L"SearchOptions", L"ExtensionFilter", L"", extBuf,
                           256, iniPath.c_str());
  g_app.options.extensionFilter = extBuf;

  g_app.options.matchFullPath =
      GetPrivateProfileIntW(L"SearchOptions", L"MatchFullPath", 0,
                            iniPath.c_str()) != 0;

  wchar_t exBuf[256];
  GetPrivateProfileStringW(L"SearchOptions", L"ExcludePattern", L"", exBuf, 256,
                           iniPath.c_str());
  g_app.options.excludePattern = exBuf;

  g_app.options.invertMatch =
      GetPrivateProfileIntW(L"SearchOptions", L"InvertMatch", 0,
                            iniPath.c_str()) != 0;
  CheckDlgButton(hDlg, IDC_CHECK_NOT,
                 g_app.options.invertMatch ? BST_CHECKED : BST_UNCHECKED);

  // Load Targets (Count is clamped so a corrupt value cannot loop forever)
  g_app.searchTargets.clear();
  SendMessage(g_app.hTargetList, LB_RESETCONTENT, 0, 0);

  int count = GetPrivateProfileIntW(L"Targets", L"Count", 0, iniPath.c_str());
  count = std::clamp(count, 0, kMaxConfiguredTargets);
  if (count > 0) {
    wchar_t buf[MAX_PATH];
    for (int i = 0; i < count; ++i) {
      std::wstring key = L"Target" + std::to_wstring(i);
      GetPrivateProfileStringW(L"Targets", key.c_str(), L"", buf, MAX_PATH,
                               iniPath.c_str());
      if (wcslen(buf) > 0) {
        g_app.searchTargets.push_back(buf);
        SendMessage(g_app.hTargetList, LB_ADDSTRING, 0, (LPARAM)buf);
      }
    }
  }

  SendDlgItemMessage(hDlg, IDC_COMBO_LANG, CB_SETCURSEL, g_app.currentLang, 0);
}

int CALLBACK CompareFunc(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort) {
  if (lParam1 >= (LPARAM)g_app.searchResults.size() ||
      lParam2 >= (LPARAM)g_app.searchResults.size())
    return 0;

  const FileResult &a = g_app.searchResults[lParam1];
  const FileResult &b = g_app.searchResults[lParam2];

  int cmp = 0;
  switch (g_app.sortColumn) {
  case 0: // Name
    cmp = lstrcmpiW(a.Name.c_str(), b.Name.c_str());
    break;
  case 1: // Path
    cmp = lstrcmpiW(a.FullPath.c_str(), b.FullPath.c_str());
    break;
  case 2: // Date
    if (a.LastWriteTime < b.LastWriteTime)
      cmp = -1;
    else if (a.LastWriteTime > b.LastWriteTime)
      cmp = 1;
    break;
  case 3: // Size
    if (a.Size < b.Size)
      cmp = -1;
    else if (a.Size > b.Size)
      cmp = 1;
    break;
  }

  return g_app.sortAscending ? cmp : -cmp;
}

void ResizeLayout(HWND hDlg, int cx, int cy) {
  if (cx == 0 || cy == 0)
    return;

  // Quick helper to move
  auto Move = [&](int id, int x, int y, int w, int h, bool size) {
    SetWindowPos(GetDlgItem(hDlg, id), NULL, x, y, w, h,
                 SWP_NOZORDER | (size ? 0 : SWP_NOSIZE));
  };

  // Margins (in pixels approx)
  int m = 10;

  // Get Button Sizes
  RECT rcBtnS, rcBtnAdd, rcBtnRem;
  GetWindowRect(GetDlgItem(hDlg, IDC_BTN_SEARCH), &rcBtnS);
  GetWindowRect(GetDlgItem(hDlg, IDC_BTN_ADD), &rcBtnAdd);
  GetWindowRect(GetDlgItem(hDlg, IDC_BTN_REMOVE), &rcBtnRem);

  int btnW = rcBtnS.right - rcBtnS.left;
  int addW = rcBtnAdd.right - rcBtnAdd.left;
  int remW = rcBtnRem.right - rcBtnRem.left;

  int maxRightColW = btnW;
  if (addW > maxRightColW)
    maxRightColW = addW;
  if (remW > maxRightColW)
    maxRightColW = remW;

  // Right Edge for buttons
  int rightEdge = cx - m;

  // Top Row
  // Buttons align to Right
  int btnX = rightEdge - btnW;

  // Search Button
  RECT rcS;
  GetWindowRect(GetDlgItem(hDlg, IDC_BTN_SEARCH), &rcS);
  POINT ptS = {rcS.left, rcS.top};
  ScreenToClient(hDlg, &ptS);
  Move(IDC_BTN_SEARCH, btnX, ptS.y, 0, 0, false);

  // Not Checkbox
  RECT rcNot;
  GetWindowRect(GetDlgItem(hDlg, IDC_CHECK_NOT), &rcNot);
  int notW = rcNot.right - rcNot.left;
  int notX = btnX - m - notW;
  Move(IDC_CHECK_NOT, notX, ptS.y + 2, 0, 0, false);

  // Filename Label
  RECT rcLbl;
  GetWindowRect(GetDlgItem(hDlg, IDC_STATIC_FILENAME), &rcLbl);
  int lblW = rcLbl.right - rcLbl.left;
  Move(IDC_STATIC_FILENAME, m, ptS.y + 2, lblW, rcLbl.bottom - rcLbl.top, true);

  // Edit Query
  // Starts after Label + m, ends before Not - m
  int editX = m + lblW + m;
  int editW = notX - m - editX;
  if (editW < 50)
    editW = 50; // Safety

  RECT rcE;
  GetWindowRect(GetDlgItem(hDlg, IDC_EDIT_QUERY), &rcE);
  Move(IDC_EDIT_QUERY, editX, ptS.y, editW, rcE.bottom - rcE.top, true);

  // Target List Buttons
  // Align right edge to rightEdge
  RECT rcAddT;
  GetWindowRect(GetDlgItem(hDlg, IDC_BTN_ADD), &rcAddT);
  POINT ptAdd = {rcAddT.left, rcAddT.top};
  ScreenToClient(hDlg, &ptAdd);
  // x = rightEdge - addW
  Move(IDC_BTN_ADD, rightEdge - addW, ptAdd.y, 0, 0, false);

  RECT rcRemT;
  GetWindowRect(GetDlgItem(hDlg, IDC_BTN_REMOVE), &rcRemT);
  POINT ptRem = {rcRemT.left, rcRemT.top};
  ScreenToClient(hDlg, &ptRem);
  // x = rightEdge - remW
  Move(IDC_BTN_REMOVE, rightEdge - remW, ptRem.y, 0, 0, false);

  // Target List
  RECT rcT;
  GetWindowRect(GetDlgItem(hDlg, IDC_LIST_TARGETS), &rcT);
  POINT ptT = {rcT.left, rcT.top};
  ScreenToClient(hDlg, &ptT);
  // Width = rightEdge - maxRightColW - m - m
  Move(IDC_LIST_TARGETS, m, ptT.y, cx - m - maxRightColW - m - m,
       rcT.bottom - rcT.top, true);

  // Result List
  RECT rcL;
  GetWindowRect(GetDlgItem(hDlg, IDC_LIST_RESULTS), &rcL);
  POINT ptL = {rcL.left, rcL.top};
  ScreenToClient(hDlg, &ptL);
  // Height: cy - m - BottomRow - ptL.y
  int bottomRowH = 40;
  Move(IDC_LIST_RESULTS, m, ptL.y, cx - m - m, cy - bottomRowH - ptL.y, true);

  // Bottom Row
  int bottomY = cy - 35;
  Move(IDC_BTN_SAVE, m, bottomY, 0, 0, false);

  // Status and Progress
  RECT rcSave;
  GetWindowRect(GetDlgItem(hDlg, IDC_BTN_SAVE), &rcSave);
  int statusX = m + (rcSave.right - rcSave.left) + m;
  int availableWidth = cx - statusX - m;

  // Split width: Status gets 60%, Progress gets 40% (approx)
  int progressW = 150;
  if (availableWidth > 300)
    progressW = 200;

  int statusW = availableWidth - progressW - m;
  if (statusW < 50)
    statusW = 50; // Min width

  Move(IDC_STATUS, statusX, bottomY + 5, statusW, 20, true);
  Move(IDC_PROGRESS, statusX + statusW + m, bottomY + 2, progressW, 20, true);
}

LRESULT DrawItemWithHighlight(LPNMLVCUSTOMDRAW lplvcd, HWND hDlg) {
  int iItem = (int)lplvcd->nmcd.dwItemSpec;
  int iSubItem = lplvcd->iSubItem;
  HDC hdc = lplvcd->nmcd.hdc;

  // Get Text
  wchar_t textBuf[1024];
  ListView_GetItemText(lplvcd->nmcd.hdr.hwndFrom, iItem, iSubItem, textBuf,
                       1024);
  std::wstring text = textBuf;

  // Get Query
  wchar_t queryBuf[kMaxQueryChars];
  GetDlgItemTextW(hDlg, IDC_EDIT_QUERY, queryBuf, kMaxQueryChars);
  std::wstring query = queryBuf;

  if (query.empty() || text.empty() || iSubItem != 0) {
    return CDRF_DODEFAULT;
  }

  struct Range {
    size_t start, len;
  };
  std::vector<Range> ranges;

  std::wstring textLower = text;
  std::wstring queryLower = query;
  if (g_app.options.ignoreCase) {
    for (auto &c : textLower)
      c = towlower(c);
    for (auto &c : queryLower)
      c = towlower(c);
  }

  // Collect Ranges
  if (g_app.options.mode == MatchMode_Exact) {
    bool match = g_app.options.ignoreCase
                     ? (lstrcmpiW(text.c_str(), query.c_str()) == 0)
                     : (text == query);
    if (match)
      ranges.push_back({0, text.length()});
  } else if (g_app.options.mode == MatchMode_SpaceDivided) {
    std::wstringstream ss(queryLower);
    std::wstring token;
    while (ss >> token) {
      size_t pos = textLower.find(token);
      while (pos != std::wstring::npos) {
        ranges.push_back({pos, token.length()});
        pos = textLower.find(token, pos + 1);
      }
    }
  } else if (g_app.options.mode == MatchMode_RegEx) {
    try {
      std::wregex re(query,
                     (g_app.options.ignoreCase
                          ? std::regex_constants::icase
                          : (std::regex_constants::syntax_option_type)0) |
                         std::regex_constants::optimize);
      auto it = std::wsregex_iterator(text.begin(), text.end(), re);
      auto end = std::wsregex_iterator();
      for (; it != end; ++it) {
        ranges.push_back({(size_t)it->position(), (size_t)it->length()});
      }
    } catch (const std::regex_error &) {
      // Invalid pattern while typing: simply no highlight.
    }
  } else { // Substring
    size_t pos = textLower.find(queryLower);
    while (pos != std::wstring::npos) {
      ranges.push_back({pos, query.length()});
      pos = textLower.find(queryLower, pos + 1);
    }
  }

  if (ranges.empty())
    return CDRF_DODEFAULT;

  // Merge Ranges
  std::sort(ranges.begin(), ranges.end(),
            [](const Range &a, const Range &b) { return a.start < b.start; });
  std::vector<Range> merged;
  for (auto &r : ranges) {
    if (merged.empty() || r.start > merged.back().start + merged.back().len) {
      merged.push_back(r);
    } else {
      size_t end =
          std::max(merged.back().start + merged.back().len, r.start + r.len);
      merged.back().len = end - merged.back().start;
    }
  }

  // Draw
  RECT rc;
  ListView_GetSubItemRect(lplvcd->nmcd.hdr.hwndFrom, iItem, iSubItem,
                          LVIR_LABEL, &rc);

  bool isSelected =
      (ListView_GetItemState(lplvcd->nmcd.hdr.hwndFrom, iItem, LVIS_SELECTED) &
       LVIS_SELECTED) != 0;
  COLORREF clrText = isSelected ? GetSysColor(COLOR_HIGHLIGHTTEXT)
                                : GetSysColor(COLOR_WINDOWTEXT);
  COLORREF clrMatchBk = RGB(0, 255, 255); // Cyan
  COLORREF clrMatchText = RGB(0, 0, 0);   // Black

  HBRUSH hFillBr = isSelected ? GetSysColorBrush(COLOR_HIGHLIGHT)
                              : GetSysColorBrush(COLOR_WINDOW);
  FillRect(hdc, &rc, hFillBr);

  int oldBkMode = SetBkMode(hdc, TRANSPARENT);
  HFONT hFont = (HFONT)SendMessage(lplvcd->nmcd.hdr.hwndFrom, WM_GETFONT, 0, 0);
  HGDIOBJ oldFont = SelectObject(hdc, hFont);

  int currentX = rc.left + 6;
  auto DrawSegment = [&](const std::wstring &seg, bool highlight) {
    if (seg.empty())
      return;
    SIZE sz;
    GetTextExtentPoint32W(hdc, seg.c_str(), (int)seg.length(), &sz);
    RECT segRect = {currentX, rc.top, currentX + sz.cx, rc.bottom};
    if (segRect.left < rc.right) {
      if (segRect.right > rc.right)
        segRect.right = rc.right;
      if (highlight) {
        HBRUSH hBr = CreateSolidBrush(clrMatchBk);
        FillRect(hdc, &segRect, hBr);
        DeleteObject(hBr);
        SetTextColor(hdc, clrMatchText);
      } else {
        SetTextColor(hdc, clrText);
      }
      DrawTextW(hdc, seg.c_str(), (int)seg.length(), &segRect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    currentX += sz.cx;
  };

  size_t lastPos = 0;
  for (auto &r : merged) {
    if (r.start > lastPos)
      DrawSegment(text.substr(lastPos, r.start - lastPos), false);
    DrawSegment(text.substr(r.start, r.len), true);
    lastPos = r.start + r.len;
  }
  if (lastPos < text.length())
    DrawSegment(text.substr(lastPos), false);

  SelectObject(hdc, oldFont);
  SetBkMode(hdc, oldBkMode);
  return CDRF_SKIPDEFAULT;
}

// Copies |text| to the clipboard. Returns false (and frees the allocation) if
// any step fails. SetClipboardData is called exactly once; on success the
// system owns the memory.
bool CopyTextToClipboard(HWND owner, const std::wstring &text) {
  if (!OpenClipboard(owner))
    return false;

  bool ok = false;
  EmptyClipboard();
  const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (hMem) {
    void *dst = GlobalLock(hMem);
    if (dst) {
      memcpy(dst, text.c_str(), bytes);
      GlobalUnlock(hMem);
      ok = SetClipboardData(CF_UNICODETEXT, hMem) != NULL;
    }
    if (!ok)
      GlobalFree(hMem); // Still ours: the system did not take ownership.
  }
  CloseClipboard();
  return ok;
}

// Shows the result context menu for |item| and performs the chosen action.
void ShowResultContextMenu(HWND hDlg, int item) {
  HMENU hMenu = LoadMenu(GetModuleHandle(NULL), MAKEINTRESOURCE(IDR_MENU1));
  if (!hMenu)
    return;

  int cmd = 0;
  HMENU hSubMenu = GetSubMenu(hMenu, 0);
  if (hSubMenu) {
    POINT pt;
    GetCursorPos(&pt);
    cmd = TrackPopupMenu(hSubMenu, TPM_RETURNCMD, pt.x, pt.y, 0, hDlg, NULL);
  }
  DestroyMenu(hMenu); // Also destroys the submenu.

  if (cmd == ID_POPUP_COPYPATH) {
    wchar_t buf[MAX_PATH];
    ListView_GetItemText(g_app.hList, item, 1, buf, MAX_PATH);
    CopyTextToClipboard(hDlg, buf);
  } else if (cmd == ID_POPUP_OPENFOLDER) {
    wchar_t buf[MAX_PATH];
    ListView_GetItemText(g_app.hList, item, 1, buf, MAX_PATH);

    std::wstring param = L"/select,\"" + std::wstring(buf) + L"\"";
    ShellExecuteW(NULL, L"open", L"explorer.exe", param.c_str(), NULL, SW_SHOW);
  }
}

// ---------------------------------------------------------------------------
// Main dialog
// ---------------------------------------------------------------------------
INT_PTR CALLBACK DialogProc(HWND hDlg, UINT uMsg, WPARAM wParam,
                            LPARAM lParam) {
  switch (uMsg) {
  case WM_SIZE:
    ResizeLayout(hDlg, LOWORD(lParam), HIWORD(lParam));
    break;
  case WM_INITDIALOG: {
    // Stage 1 Fix: Ensure no capture immediately and LOCKDOWN the list view
    ReleaseCapture();

    // Set Icon
    HICON hIconBig =
        (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP_ICON),
                         IMAGE_ICON, 32, 32, 0);
    HICON hIconSmall =
        (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP_ICON),
                         IMAGE_ICON, 16, 16, 0);
    SendMessage(hDlg, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
    SendMessage(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);

    g_app.isStartupGuarded = true; // Start guarded

    g_app.hInstance = GetModuleHandle(NULL);
    g_app.hList = GetDlgItem(hDlg, IDC_LIST_RESULTS);
    HWND hEdit = GetDlgItem(hDlg, IDC_EDIT_QUERY);

    // Subclass controls to ignore disruptive messages during startup
    g_app.oldEditProc = (WNDPROC)SetWindowLongPtr(hEdit, GWLP_WNDPROC,
                                                  (LONG_PTR)GuardedEditProc);
    g_app.oldListProc = (WNDPROC)SetWindowLongPtr(g_app.hList, GWLP_WNDPROC,
                                                  (LONG_PTR)GuardedListProc);

    // Settle and enforce focus after a short delay. If the timer cannot be
    // created, release the guard now rather than swallowing input forever.
    if (SetTimer(hDlg, kStartupGuardTimerId, kStartupGuardMs, NULL) == 0)
      ReleaseStartupGuard(hDlg);

    // Temporarily disable the list to ignore double-click artifacts from launch
    EnableWindow(g_app.hList, FALSE);

    ListView_SetExtendedListViewStyle(g_app.hList,
                                      LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    g_app.hTargetList = GetDlgItem(hDlg, IDC_LIST_TARGETS);
    g_app.oldTargetListProc =
        (WNDPROC)SetWindowLongPtr(g_app.hTargetList, GWLP_WNDPROC,
                                  (LONG_PTR)TargetListProc);

    // Setup Columns
    LVCOLUMN lvc;
    lvc.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
    lvc.iSubItem = 0;
    lvc.pszText = (LPWSTR)L"Name";
    lvc.cx = 200;
    lvc.fmt = LVCFMT_LEFT;
    ListView_InsertColumn(g_app.hList, 0, &lvc);

    lvc.iSubItem = 1;
    lvc.pszText = (LPWSTR)L"Path";
    lvc.cx = 300;
    ListView_InsertColumn(g_app.hList, 1, &lvc);

    lvc.iSubItem = 2;
    lvc.pszText = (LPWSTR)L"Modified Date";
    lvc.cx = 140;
    ListView_InsertColumn(g_app.hList, 2, &lvc);

    lvc.iSubItem = 3;
    lvc.pszText = (LPWSTR)L"Size";
    lvc.cx = 100;
    ListView_InsertColumn(g_app.hList, 3, &lvc);

    // Check Current Language Menu Item
    HMENU hMenu = GetMenu(hDlg);
    CheckMenuItem(hMenu, ID_LANG_ENGLISH + g_app.currentLang,
                  MF_BYCOMMAND | MF_CHECKED);

    // Setup Code Pages
    HWND hCP = GetDlgItem(hDlg, IDC_COMBO_CP);
    SendMessage(hCP, CB_ADDSTRING, 0, (LPARAM)L"Auto (OEM)");
    SendMessage(hCP, CB_SETITEMDATA, 0, (LPARAM)CP_OEMCP);
    SendMessage(hCP, CB_ADDSTRING, 0, (LPARAM)L"Japanese (932)");
    SendMessage(hCP, CB_SETITEMDATA, 1, (LPARAM)932);
    SendMessage(hCP, CB_ADDSTRING, 0, (LPARAM)L"Western (1252)");
    SendMessage(hCP, CB_SETITEMDATA, 2, (LPARAM)1252);
    SendMessage(hCP, CB_ADDSTRING, 0, (LPARAM)L"Simplified Chinese (936)");
    SendMessage(hCP, CB_SETITEMDATA, 3, (LPARAM)936);
    SendMessage(hCP, CB_ADDSTRING, 0, (LPARAM)L"Traditional Chinese (950)");
    SendMessage(hCP, CB_SETITEMDATA, 4, (LPARAM)950);
    SendMessage(hCP, CB_ADDSTRING, 0, (LPARAM)L"UTF-8 (65001)");
    SendMessage(hCP, CB_SETITEMDATA, 5, (LPARAM)CP_UTF8);

    // Select current CP
    SendMessage(hCP, CB_SETCURSEL, 0, 0); // Default to Auto
    const LRESULT cpCount = SendMessage(hCP, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < (int)cpCount; ++i) {
      LRESULT data = SendMessage(hCP, CB_GETITEMDATA, i, 0);
      if (data != CB_ERR && (int)data == g_app.currentCodePage) {
        SendMessage(hCP, CB_SETCURSEL, i, 0);
        break;
      }
    }

    // Load Config
    LoadConfig(hDlg);

    // Apply Language (including Menu)
    UpdateLanguage(hDlg);

    // Check Configured Language Menu Item
    hMenu = GetMenu(hDlg);
    for (int i = 0; i < APP_LANG_COUNT; i++) {
      CheckMenuItem(hMenu, ID_LANG_ENGLISH + i,
                    MF_BYCOMMAND |
                        (i == g_app.currentLang ? MF_CHECKED : MF_UNCHECKED));
    }

    if (g_app.searchTargets.empty()) {
      wchar_t defaultPath[] = L"C:\\";
      g_app.searchTargets.push_back(defaultPath);
      SendMessage(g_app.hTargetList, LB_ADDSTRING, 0, (LPARAM)defaultPath);
    }

    ListView_SetExtendedListViewStyle(g_app.hList,
                                      LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    UpdateLanguage(hDlg);

    // FIX: Set focus to the search field and return FALSE as requested.
    // This is the definitive way to set focus in a dialog during
    // initialization.
    SetFocus(GetDlgItem(hDlg, IDC_EDIT_QUERY));
    return FALSE;
  }
  case WM_APP_PROGRESS: {
    SendDlgItemMessage(hDlg, IDC_PROGRESS, PBM_SETPOS, (WPARAM)wParam, 0);
    return TRUE;
  }
  case WM_TIMER: {
    if (wParam == kStartupGuardTimerId) {
      KillTimer(hDlg, kStartupGuardTimerId);

      // Release the interactive guard and restore the original procedures
      ReleaseStartupGuard(hDlg);

      // 1. Re-enable the list view first so it can process the cleanup messages
      EnableWindow(g_app.hList, TRUE);

      // 2. Clear any stray mouse messages that arrived during
      // startup/initialization
      MSG msg;
      while (PeekMessage(&msg, NULL, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE))
        ;

      // 3. Abort all internal modes (drags, marquees, mouse capture)
      ReleaseCapture();
      SendMessage(hDlg, WM_CANCELMODE, 0, 0);
      SendMessage(g_app.hList, WM_CANCELMODE, 0, 0);

      // 4. Force selection/focus reset on the list
      ListView_SetItemState(g_app.hList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
      ListView_SetSelectionMark(g_app.hList, -1);

      // 5. Force focus to the query edit box as the absolute final action
      HWND hEdit = GetDlgItem(hDlg, IDC_EDIT_QUERY);
      SetFocus(hEdit);
      SendMessage(hEdit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    }
    break;
  }
  case WM_COMMAND: {
    int id = LOWORD(wParam);
    if (id == IDC_BTN_SEARCH) {
      if (!g_app.isSearching) {
        if (g_app.searchTargets.empty()) {
          MessageBoxW(hDlg, Localization::GetString(IDS_MSG_NEED_CANDIDATE),
                      Localization::GetString(IDS_TITLE_INFO), MB_OK);
          break;
        }

        // Update Options
        g_app.options.invertMatch =
            (IsDlgButtonChecked(hDlg, IDC_CHECK_NOT) == BST_CHECKED);

        StartScan(hDlg);
      }
    } else if (id == IDC_BTN_SAVE) {
      SaveResults(hDlg);
    } else if (id == IDC_BTN_ADD) {
      AddFolder(hDlg);
    } else if (id == IDC_BTN_REMOVE) {
      RemoveFolder(hDlg);
    } else if (id == IDC_COMBO_CP && HIWORD(wParam) == CBN_SELCHANGE) {
      LRESULT idx = SendDlgItemMessage(hDlg, IDC_COMBO_CP, CB_GETCURSEL, 0, 0);
      if (idx != CB_ERR) {
        LRESULT data =
            SendDlgItemMessage(hDlg, IDC_COMBO_CP, CB_GETITEMDATA, idx, 0);
        if (data != CB_ERR)
          g_app.currentCodePage = (int)data;
      }
    } else if (id == ID_POPUP_COPYPATH) {
    } else if (id >= ID_LANG_ENGLISH && id <= ID_LANG_PORTUGUESE) {
      int newLang = id - ID_LANG_ENGLISH;
      Localization::SetLanguage((LanguageID)newLang);
      g_app.currentLang = newLang;
      UpdateLanguage(hDlg);

      // Update Menu Checks
      HMENU hMenu = GetMenu(hDlg);
      for (int i = 0; i < APP_LANG_COUNT; i++) {
        CheckMenuItem(hMenu, ID_LANG_ENGLISH + i,
                      MF_BYCOMMAND |
                          (i == g_app.currentLang ? MF_CHECKED : MF_UNCHECKED));
      }
    } else if (id == ID_CONFIG_OPTIONS) {
      if (DialogBox(g_app.hInstance, MAKEINTRESOURCE(IDD_CONFIG), hDlg,
                    ConfigDlgProc) == IDOK) {
        // Options updated
      }
      break;
    } else if (id == ID_HELP_ABOUT) {
      MessageBoxW(
          hDlg,
          L"Fast File Search v1.1\n\nHigh-performance File Search Tool.\n\n"
          L"Supports:\n"
          L"- NTFS (Direct MFT Access)\n"
          L"- FAT16/FAT32 (Direct Volume Access)\n"
          L"- exFAT (Direct Volume Access)\n\n"
          L"Scans raw disk structures for maximum speed.",
          L"About", MB_OK | MB_ICONINFORMATION);
    } else if (id == IDCANCEL) {
      CancelScan(hDlg); // Stop and join any running scan before closing.
      if (!SaveConfig(hDlg)) {
        MessageBoxW(hDlg, Localization::GetString(IDS_ERR_CONFIG_SAVE_FAILED),
                    Localization::GetString(IDS_TITLE_ERROR),
                    MB_ICONERROR | MB_OK);
      }
      EndDialog(hDlg, 0);
    }
    break;
  }
  case WM_ACTIVATE: {
    // Focus handled by Timer and Tab Order
    break;
  }
  case WM_NOTIFY: {
    LPNMHDR pnm = (LPNMHDR)lParam;
    if (pnm->idFrom == IDC_LIST_RESULTS) {
      if (pnm->code == NM_CUSTOMDRAW) {
        LPNMLVCUSTOMDRAW lplvcd = (LPNMLVCUSTOMDRAW)lParam;
        if (lplvcd->nmcd.dwDrawStage == CDDS_PREPAINT) {
          SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
          return TRUE;
        }
        if (lplvcd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
          SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_NOTIFYSUBITEMDRAW);
          return TRUE;
        }
        if (lplvcd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
          int col = lplvcd->iSubItem;
          if (col == 0 || col == 1) { // Name or Path
            LRESULT ret = DrawItemWithHighlight(lplvcd, hDlg);
            SetWindowLongPtr(hDlg, DWLP_MSGRESULT, ret);
            return TRUE;
          }
          SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_DODEFAULT);
          return TRUE;
        }
      } else if (pnm->code == LVN_COLUMNCLICK) {
        LPNMLISTVIEW pnmv = (LPNMLISTVIEW)lParam;
        if (g_app.sortColumn == pnmv->iSubItem) {
          g_app.sortAscending = !g_app.sortAscending;
        } else {
          g_app.sortColumn = pnmv->iSubItem;
          g_app.sortAscending = true;
        }
        ListView_SortItems(g_app.hList, CompareFunc, 0);
      } else if (pnm->code == NM_RCLICK) {
        LPNMITEMACTIVATE pnmitem = (LPNMITEMACTIVATE)pnm;
        if (pnmitem->iItem != -1)
          ShowResultContextMenu(hDlg, pnmitem->iItem);
      }
    }
    break;
  }
  case WM_APP_SEARCH_DONE:
  case WM_APP_SEARCH_ERROR:
    HandleScanFinished(hDlg, uMsg, lParam);
    break;
  }
  return FALSE;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR lpCmdLine,
                   int nShowCmd) {
  INITCOMMONCONTROLSEX icex;
  icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
  icex.dwICC = ICC_WIN95_CLASSES | ICC_DATE_CLASSES;
  if (!InitCommonControlsEx(&icex)) {
    MessageBoxW(NULL, L"Failed to initialize common controls.",
                L"FastFileSearch Error", MB_ICONERROR | MB_OK);
    return 1;
  }

  // Init COM for SHBrowseForFolder. S_FALSE means already initialized on this
  // thread; both success codes must be balanced by CoUninitialize.
  const HRESULT hrCom = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  if (FAILED(hrCom)) {
    MessageBoxW(NULL, L"Failed to initialize COM.", L"FastFileSearch Error",
                MB_ICONERROR | MB_OK);
    return 1;
  }
  const INT_PTR ret =
      DialogBox(hInstance, MAKEINTRESOURCE(IDD_MAINDIALOG), NULL, DialogProc);
  CoUninitialize();
  return static_cast<int>(ret);
}

INT_PTR CALLBACK ConfigDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam,
                               LPARAM lParam) {
  auto UpdateUIState = [hDlg]() {
    BOOL sizeEnabled = IsDlgButtonChecked(hDlg, IDC_CHKS_SIZE) == BST_CHECKED;
    EnableWindow(GetDlgItem(hDlg, IDC_EDIT_MINSIZE), sizeEnabled);
    EnableWindow(GetDlgItem(hDlg, IDC_EDIT_MAXSIZE), sizeEnabled);
    BOOL dateEnabled = IsDlgButtonChecked(hDlg, IDC_CHKS_DATE) == BST_CHECKED;
    EnableWindow(GetDlgItem(hDlg, IDC_DATE_MIN), dateEnabled);
    EnableWindow(GetDlgItem(hDlg, IDC_DATE_MAX), dateEnabled);
  };

  switch (uMsg) {
  case WM_INITDIALOG: {
    // Localize Dialog
    SetWindowTextW(hDlg, Localization::GetString(IDS_CONFIG_TITLE));
    SetDlgItemTextW(hDlg, IDC_GRP_MATCHMODE,
                    Localization::GetString(IDS_GRP_MATCHMODE));

    SetDlgItemTextW(hDlg, IDC_RADIO_SUBSTRING,
                    Localization::GetString(IDS_RAD_SUBSTRING));
    SetDlgItemTextW(hDlg, IDC_RADIO_EXACT,
                    Localization::GetString(IDS_RAD_EXACT));
    SetDlgItemTextW(hDlg, IDC_RADIO_SPACED,
                    Localization::GetString(IDS_RAD_SPACED));
    SetDlgItemTextW(hDlg, IDC_RADIO_REGEX,
                    Localization::GetString(IDS_RAD_REGEX));
    SetDlgItemTextW(hDlg, IDC_CHKS_IGNORECASE,
                    Localization::GetString(IDS_CHK_IGNORECASE));

    SetDlgItemTextW(hDlg, IDC_GRP_SIZE, Localization::GetString(IDS_GRP_SIZE));
    SetDlgItemTextW(hDlg, IDC_CHKS_SIZE, Localization::GetString(IDS_CHK_SIZE));
    SetDlgItemTextW(hDlg, IDC_STATIC_MIN, Localization::GetString(IDS_LBL_MIN));
    SetDlgItemTextW(hDlg, IDC_STATIC_MAX, Localization::GetString(IDS_LBL_MAX));

    SetDlgItemTextW(hDlg, IDC_GRP_DATE, Localization::GetString(IDS_GRP_DATE));
    SetDlgItemTextW(hDlg, IDC_CHKS_DATE, Localization::GetString(IDS_CHK_DATE));
    SetDlgItemTextW(hDlg, IDC_STATIC_FROM,
                    Localization::GetString(IDS_LBL_FROM));
    SetDlgItemTextW(hDlg, IDC_STATIC_TO, Localization::GetString(IDS_LBL_TO));

    SetDlgItemTextW(hDlg, IDC_GRP_INCLUDE,
                    Localization::GetString(IDS_GRP_INCLUDE));
    SetDlgItemTextW(hDlg, IDC_CHKS_FILES,
                    Localization::GetString(IDS_CHK_FILES));
    SetDlgItemTextW(hDlg, IDC_CHKS_FOLDERS,
                    Localization::GetString(IDS_CHK_FOLDERS));

    SetDlgItemTextW(hDlg, IDC_GRP_TYPE, Localization::GetString(IDS_GRP_TYPE));
    SetDlgItemTextW(hDlg, IDC_STATIC_EXT, Localization::GetString(IDS_LBL_EXT));

    SetDlgItemTextW(hDlg, IDC_GRP_ADVANCED,
                    Localization::GetString(IDS_GRP_ADVANCED));
    SetDlgItemTextW(hDlg, IDC_CHKS_FULLPATH,
                    Localization::GetString(IDS_CHK_FULLPATH));
    SetDlgItemTextW(hDlg, IDC_STATIC_EXCLUDE,
                    Localization::GetString(IDS_LBL_EXCLUDE));

    SetDlgItemTextW(hDlg, IDOK, Localization::GetString(IDS_BTN_OK));
    SetDlgItemTextW(hDlg, IDCANCEL, Localization::GetString(IDS_BTN_CANCEL));

    // Mode
    int id = IDC_RADIO_SUBSTRING;
    if (g_app.options.mode == MatchMode_Exact)
      id = IDC_RADIO_EXACT;
    else if (g_app.options.mode == MatchMode_SpaceDivided)
      id = IDC_RADIO_SPACED;
    else if (g_app.options.mode == MatchMode_RegEx)
      id = IDC_RADIO_REGEX;
    CheckRadioButton(hDlg, IDC_RADIO_SUBSTRING, IDC_RADIO_REGEX, id);
    CheckDlgButton(hDlg, IDC_CHKS_IGNORECASE,
                   g_app.options.ignoreCase ? BST_CHECKED : BST_UNCHECKED);

    // Size
    CheckDlgButton(hDlg, IDC_CHKS_SIZE,
                   (g_app.options.minSize > 0 || g_app.options.maxSize > 0)
                       ? BST_CHECKED
                       : BST_UNCHECKED);
    SetDlgItemInt(hDlg, IDC_EDIT_MINSIZE,
                  (UINT)(g_app.options.minSize / 1024), FALSE);
    SetDlgItemInt(hDlg, IDC_EDIT_MAXSIZE,
                  (UINT)(g_app.options.maxSize / 1024), FALSE);

    // Date
    CheckDlgButton(hDlg, IDC_CHKS_DATE,
                   (g_app.options.minDate > 0 || g_app.options.maxDate > 0)
                       ? BST_CHECKED
                       : BST_UNCHECKED);
    SYSTEMTIME stMin, stMax;
    FILETIME ft;
    if (g_app.options.minDate > 0) {
      ULARGE_INTEGER uli;
      uli.QuadPart = g_app.options.minDate;
      ft.dwLowDateTime = uli.LowPart;
      ft.dwHighDateTime = uli.HighPart;
      FileTimeToSystemTime(&ft, &stMin);
    } else
      GetLocalTime(&stMin);
    if (g_app.options.maxDate > 0) {
      ULARGE_INTEGER uli;
      uli.QuadPart = g_app.options.maxDate;
      ft.dwLowDateTime = uli.LowPart;
      ft.dwHighDateTime = uli.HighPart;
      FileTimeToSystemTime(&ft, &stMax);
    } else
      GetLocalTime(&stMax);
    DateTime_SetSystemtime(GetDlgItem(hDlg, IDC_DATE_MIN), GDT_VALID, &stMin);
    DateTime_SetSystemtime(GetDlgItem(hDlg, IDC_DATE_MAX), GDT_VALID, &stMax);

    // Include
    CheckDlgButton(hDlg, IDC_CHKS_FILES,
                   g_app.options.includeFiles ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_CHKS_FOLDERS,
                   g_app.options.includeFolders ? BST_CHECKED : BST_UNCHECKED);

    // Extension
    SetDlgItemTextW(hDlg, IDC_EDIT_EXT,
                    g_app.options.extensionFilter.c_str());

    // Advanced
    CheckDlgButton(hDlg, IDC_CHKS_FULLPATH,
                   g_app.options.matchFullPath ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemTextW(hDlg, IDC_EDIT_EXCLUDE,
                    g_app.options.excludePattern.c_str());

    UpdateUIState();
    return TRUE;
  }
  case WM_COMMAND: {
    int id = LOWORD(wParam);
    if (id == IDOK) {
      // Mode
      if (IsDlgButtonChecked(hDlg, IDC_RADIO_SUBSTRING))
        g_app.options.mode = MatchMode_Substring;
      else if (IsDlgButtonChecked(hDlg, IDC_RADIO_EXACT))
        g_app.options.mode = MatchMode_Exact;
      else if (IsDlgButtonChecked(hDlg, IDC_RADIO_SPACED))
        g_app.options.mode = MatchMode_SpaceDivided;
      else if (IsDlgButtonChecked(hDlg, IDC_RADIO_REGEX))
        g_app.options.mode = MatchMode_RegEx;
      g_app.options.ignoreCase =
          (IsDlgButtonChecked(hDlg, IDC_CHKS_IGNORECASE) == BST_CHECKED);

      // Size
      if (IsDlgButtonChecked(hDlg, IDC_CHKS_SIZE) == BST_CHECKED) {
        g_app.options.minSize =
            (uint64_t)GetDlgItemInt(hDlg, IDC_EDIT_MINSIZE, NULL, FALSE) * 1024;
        g_app.options.maxSize =
            (uint64_t)GetDlgItemInt(hDlg, IDC_EDIT_MAXSIZE, NULL, FALSE) * 1024;
      } else {
        g_app.options.minSize = 0;
        g_app.options.maxSize = 0;
      }

      // Date
      if (IsDlgButtonChecked(hDlg, IDC_CHKS_DATE) == BST_CHECKED) {
        SYSTEMTIME st;
        FILETIME ft;
        ULARGE_INTEGER uli;
        DateTime_GetSystemtime(GetDlgItem(hDlg, IDC_DATE_MIN), &st);
        SystemTimeToFileTime(&st, &ft);
        uli.LowPart = ft.dwLowDateTime;
        uli.HighPart = ft.dwHighDateTime;
        g_app.options.minDate = uli.QuadPart;

        DateTime_GetSystemtime(GetDlgItem(hDlg, IDC_DATE_MAX), &st);
        st.wHour = 23;
        st.wMinute = 59;
        st.wSecond = 59; // End of day
        SystemTimeToFileTime(&st, &ft);
        uli.LowPart = ft.dwLowDateTime;
        uli.HighPart = ft.dwHighDateTime;
        g_app.options.maxDate = uli.QuadPart;
      } else {
        g_app.options.minDate = 0;
        g_app.options.maxDate = 0;
      }

      // Include
      g_app.options.includeFiles =
          (IsDlgButtonChecked(hDlg, IDC_CHKS_FILES) == BST_CHECKED);
      g_app.options.includeFolders =
          (IsDlgButtonChecked(hDlg, IDC_CHKS_FOLDERS) == BST_CHECKED);

      // Extension
      wchar_t extBuf[256];
      GetDlgItemTextW(hDlg, IDC_EDIT_EXT, extBuf, 256);
      g_app.options.extensionFilter = extBuf;

      // Advanced
      g_app.options.matchFullPath =
          (IsDlgButtonChecked(hDlg, IDC_CHKS_FULLPATH) == BST_CHECKED);
      wchar_t exBuf[256];
      GetDlgItemTextW(hDlg, IDC_EDIT_EXCLUDE, exBuf, 256);
      g_app.options.excludePattern = exBuf;

      EndDialog(hDlg, IDOK);
      return TRUE;
    } else if (id == IDCANCEL) {
      EndDialog(hDlg, IDCANCEL);
      return TRUE;
    } else if (id == IDC_CHKS_SIZE || id == IDC_CHKS_DATE) {
      UpdateUIState();
    }
    break;
  }
  }
  return FALSE;
}
