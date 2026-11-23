#pragma once
#include <string>

enum LanguageID {
  APP_LANG_ENGLISH = 0,
  APP_LANG_JAPANESE,
  APP_LANG_CHINESE_SIMP,
  APP_LANG_CHINESE_TRAD,
  APP_LANG_SPANISH,
  APP_LANG_FRENCH,
  APP_LANG_GERMAN,
  APP_LANG_PORTUGUESE,
  APP_LANG_COUNT
};

enum StringID {
  IDS_SEARCH = 0,
  IDS_FILENAME_LABEL,
  IDS_STATUS_READY,
  IDS_STATUS_BUSY,
  IDS_STATUS_FOUND_FMT, // Contains a "%d" placeholder; see
                        // Localization::FormatCount.
  IDS_COL_NAME,
  IDS_COL_PATH,
  IDS_COL_DATE,
  IDS_COL_SIZE,
  IDS_BTN_SAVE,
  IDS_LBL_TARGETS,
  IDS_BTN_ADD,
  IDS_BTN_REMOVE,
  IDS_CHK_NOT,
  IDS_LBL_CODEPAGE,

  // Search Options Dialog Strings
  IDS_CONFIG_TITLE,
  IDS_GRP_MATCHMODE,
  IDS_RAD_SUBSTRING,
  IDS_RAD_EXACT,
  IDS_RAD_SPACED,
  IDS_RAD_REGEX,
  IDS_CHK_IGNORECASE,
  IDS_GRP_SIZE,
  IDS_CHK_SIZE,
  IDS_LBL_MIN,
  IDS_LBL_MAX,
  IDS_GRP_DATE,
  IDS_CHK_DATE,
  IDS_LBL_FROM,
  IDS_LBL_TO,
  IDS_GRP_INCLUDE,
  IDS_CHK_FILES,
  IDS_CHK_FOLDERS,
  IDS_GRP_TYPE,
  IDS_LBL_EXT,
  IDS_GRP_ADVANCED,
  IDS_CHK_FULLPATH,
  IDS_LBL_EXCLUDE,
  IDS_BTN_OK,
  IDS_BTN_CANCEL,

  // Menu Strings
  IDS_MENU_FILE,
  IDS_MENU_CONFIG,
  IDS_MENU_LANGUAGE,
  IDS_MENU_HELP,
  IDS_MENU_SEARCH_OPTIONS,
  IDS_MENU_ABOUT,
  IDS_MENU_CONTEXT,
  IDS_MENU_COPY_PATH,

  // Error / message strings
  IDS_ERR_ACCESS_DENIED_HINT,
  IDS_ERR_UNKNOWN_SCAN,
  IDS_MSG_NEED_CANDIDATE,
  IDS_TITLE_INFO,
  IDS_TITLE_ERROR,
  IDS_STATUS_FAILED,
  IDS_ERR_SAVE_FAILED,
  IDS_ERR_CONFIG_SAVE_FAILED,

  IDS_STRING_COUNT
};

class Localization {
public:
  static void SetLanguage(LanguageID lang);
  static LanguageID GetLanguage();
  // Returns a display string for the current language. The result is a
  // translation for display only: never pass it as a printf format string.
  // Out-of-range IDs return L"" (and assert in debug builds); missing
  // translations fall back to English.
  static const wchar_t *GetString(StringID id);
  static const wchar_t *GetLanguageName(LanguageID lang);
  static int GetLanguageCount();
  // Replaces the first "%d" in a localized template with |count|. Does not
  // interpret the template as a format string.
  static std::wstring FormatCount(StringID id, unsigned long long count);

private:
  static LanguageID s_currentLang;
};
