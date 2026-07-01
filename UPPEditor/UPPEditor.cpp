// UPPEditor.cpp
// Unity WebPlayer PlayerPrefs (.upp) Editor.
// Part of UFToolkit: https://github.com/mtdcmz/UFToolkit/

#define _WIN32_WINNT  0x0600
#define WINVER        0x0600
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <wchar.h>
#include <wctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <vector>
#include <string>
#include <algorithm>

#include "resource.h"

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------
#define APP_NAME     L"UPPEditor"
#define APP_VERSION  L"1.0"
#define GITHUB_URL   L"https://github.com/mtdcmz/UFToolkit/"

#define WM_POSTINIT (WM_APP + 1)

static const int TOOLBAR_H = 34;

// ---------------------------------------------------------------------------
//  .upp binary format
//
//  Header: "UnityPrf" + uint32 LE 0x00010000 + uint32 LE 0x00100000
//  Records:
//    u8  keyLen  -> key bytes (UTF-8)
//    u8  type
//      < 0x80   = short string (length = type)
//      0x80     = long string (u32 LE length, then data)
//      0xFD     = float32 LE
//      0xFE     = int32 LE
// ---------------------------------------------------------------------------
static const char     UPP_HEADER[8] = {'U','n','i','t','y','P','r','f'};
static const uint32_t UPP_V1        = 0x00010000;
static const uint32_t UPP_V2        = 0x00100000;
static const uint8_t  UPP_T_STRING  = 0x80;
static const uint8_t  UPP_T_FLOAT   = 0xFD;
static const uint8_t  UPP_T_INT     = 0xFE;

enum class PrefType : uint8_t { String, Int, Float };

struct PrefEntry {
    std::string key;
    PrefType    type = PrefType::String;
    std::string strValue;
    int32_t     intValue = 0;
    float       floatValue = 0.f;
};

// Parse a .upp buffer. Returns false + error message on failure.
// Duplicate keys: last occurrence wins.
static bool ParseUpp(const std::vector<uint8_t>& buf, std::vector<PrefEntry>& out, std::wstring& errMsg)
{
    out.clear();
    if (buf.size() < 16 || memcmp(buf.data(), UPP_HEADER, 8) != 0) {
        errMsg = L"Not a valid .upp file (bad header).";
        return false;
    }
    uint32_t v1, v2;
    memcpy(&v1, buf.data() + 8, 4);
    memcpy(&v2, buf.data() + 12, 4);
    if (v1 != UPP_V1 || v2 != UPP_V2) {
        errMsg = L"Not a valid .upp file (unexpected version marker).";
        return false;
    }

    size_t offset = 16;
    while (offset < buf.size()) {
        if (buf.size() - offset < 1) { errMsg = L"Truncated file (key length)."; return false; }
        uint8_t keyLen = buf[offset]; offset += 1;

        if (buf.size() - offset < keyLen) { errMsg = L"Truncated file (key data)."; return false; }
        std::string key(reinterpret_cast<const char*>(&buf[offset]), keyLen);
        offset += keyLen;

        if (buf.size() - offset < 1) { errMsg = L"Truncated file (type byte)."; return false; }
        uint8_t type = buf[offset]; offset += 1;

        PrefEntry e;
        e.key = key;

        if (type < UPP_T_STRING) {
            if (buf.size() - offset < (size_t)type) { errMsg = L"Truncated file (short string)."; return false; }
            e.type = PrefType::String;
            e.strValue.assign(reinterpret_cast<const char*>(&buf[offset]), type);
            offset += type;
        } else if (type == UPP_T_STRING) {
            if (buf.size() - offset < 4) { errMsg = L"Truncated file (string length)."; return false; }
            uint32_t len; memcpy(&len, &buf[offset], 4); offset += 4;
            if (buf.size() - offset < len) { errMsg = L"Truncated file (long string)."; return false; }
            e.type = PrefType::String;
            e.strValue.assign(reinterpret_cast<const char*>(&buf[offset]), len);
            offset += len;
        } else if (type == UPP_T_FLOAT) {
            if (buf.size() - offset < 4) { errMsg = L"Truncated file (float)."; return false; }
            float f; memcpy(&f, &buf[offset], 4); offset += 4;
            e.type = PrefType::Float; e.floatValue = f;
        } else if (type == UPP_T_INT) {
            if (buf.size() - offset < 4) { errMsg = L"Truncated file (int)."; return false; }
            int32_t n; memcpy(&n, &buf[offset], 4); offset += 4;
            e.type = PrefType::Int; e.intValue = n;
        } else {
            wchar_t msg[128];
            swprintf(msg, 128, L"Unknown value type 0x%02X at offset %zu.", (unsigned)type, offset - 1);
            errMsg = msg;
            return false;
        }

        bool replaced = false;
        for (auto& existing : out) {
            if (existing.key == e.key) { existing = std::move(e); replaced = true; break; }
        }
        if (!replaced) out.push_back(std::move(e));
    }
    return true;
}

static std::vector<uint8_t> SerializeUpp(const std::vector<PrefEntry>& entries)
{
    std::vector<uint8_t> buf;
    buf.insert(buf.end(), UPP_HEADER, UPP_HEADER + 8);

    auto pushU32 = [&](uint32_t v) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
        buf.insert(buf.end(), p, p + 4);
    };
    pushU32(UPP_V1);
    pushU32(UPP_V2);

    for (const auto& e : entries) {
        uint8_t klen = (uint8_t)std::min<size_t>(e.key.size(), 255);
        buf.push_back(klen);
        buf.insert(buf.end(), e.key.begin(), e.key.begin() + klen);

        if (e.type == PrefType::String) {
            if (e.strValue.size() < UPP_T_STRING) {
                buf.push_back((uint8_t)e.strValue.size());
                buf.insert(buf.end(), e.strValue.begin(), e.strValue.end());
            } else {
                buf.push_back(UPP_T_STRING);
                pushU32((uint32_t)e.strValue.size());
                buf.insert(buf.end(), e.strValue.begin(), e.strValue.end());
            }
        } else if (e.type == PrefType::Int) {
            buf.push_back(UPP_T_INT);
            int32_t v = e.intValue;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
            buf.insert(buf.end(), p, p + 4);
        } else { // Float
            buf.push_back(UPP_T_FLOAT);
            float v = e.floatValue;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
            buf.insert(buf.end(), p, p + 4);
        }
    }
    return buf;
}

// ---------------------------------------------------------------------------
//  UTF-8 <-> UTF-16 conversion
// ---------------------------------------------------------------------------
static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring w(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], len);
    return w;
}
static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string s(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], len, nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------------------
//  File I/O (wide paths – handles non-ASCII folders)
// ---------------------------------------------------------------------------
static bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out)
{
    HANDLE h = CreateFile(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size)) { CloseHandle(h); return false; }
    out.resize((size_t)size.QuadPart);
    DWORD got = 0;
    BOOL ok = out.empty() ? TRUE : ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr);
    CloseHandle(h);
    return ok && (size_t)got == out.size();
}
static bool WriteFileBytes(const std::wstring& path, const std::vector<uint8_t>& bytes)
{
    HANDLE h = CreateFile(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = bytes.empty() ? TRUE : WriteFile(h, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
    CloseHandle(h);
    return ok && (size_t)written == bytes.size();
}

// ---------------------------------------------------------------------------
//  AppData\Unity\WebPlayerPrefs helpers
// ---------------------------------------------------------------------------
static std::wstring GetAppDataDir()
{
    wchar_t buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPath(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf)))
        return buf;
    wchar_t* env = _wgetenv(L"APPDATA");
    return env ? env : L"";
}
static std::wstring GetWebPlayerPrefsDir()
{
    std::wstring a = GetAppDataDir();
    if (a.empty()) return L"";
    return a + L"\\Unity\\WebPlayerPrefs";
}

// Find every .upp under WebPlayerPrefs\<domain>\*.upp
static std::vector<std::wstring> ScanAppDataSaves()
{
    std::vector<std::wstring> results;
    std::wstring base = GetWebPlayerPrefsDir();
    if (base.empty()) return results;

    std::wstring wild = base + L"\\*";
    WIN32_FIND_DATA fd = {};
    HANDLE h = FindFirstFile(wild.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return results;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

        std::wstring domainDir = base + L"\\" + fd.cFileName;
        std::wstring wild2 = domainDir + L"\\*.upp";
        WIN32_FIND_DATA fd2 = {};
        HANDLE h2 = FindFirstFile(wild2.c_str(), &fd2);
        if (h2 != INVALID_HANDLE_VALUE) {
            do {
                if (fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                results.push_back(domainDir + L"\\" + fd2.cFileName);
            } while (FindNextFile(h2, &fd2));
            FindClose(h2);
        }
    } while (FindNextFile(h, &fd));
    FindClose(h);
    return results;
}

// ---------------------------------------------------------------------------
//  Unity PlayerPrefs filename codec
//
//  Encodes a game src path into the .upp filename Unity expects.
//  Decode is approximate (uppercase info lost), used only for display.
// ---------------------------------------------------------------------------
static std::string EncodeUnityPrefFilenameUtf8(const std::wstring& wpath)
{
    std::string utf8 = WideToUtf8(wpath);
    std::string out = "pref";
    for (unsigned char b : utf8) {
        if (b == '\\' || b == '/')     out += '-';
        else if (b == ' ')             out += ' ';
        else if (b >= 'a' && b <= 'z') out += (char)b;
        else if (b >= 'A' && b <= 'Z') out += (char)(b + 32);
        else if (b >= '0' && b <= '9') out += (char)b;
        else {
            char hex[16];
            unsigned int uval = (unsigned int)(int)(signed char)b;
            snprintf(hex, sizeof(hex), "_%x", uval);
            out += hex;
        }
    }
    out += ".upp";
    return out;
}

static std::wstring DecodeUppFilename(const std::wstring& filenameNoExt)
{
    std::wstring s = filenameNoExt;
    if (s.size() >= 4 && s.compare(0, 4, L"pref") == 0) s = s.substr(4);

    auto isHex = [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); };
    auto hexVal = [](wchar_t c) -> int { return (c >= L'0' && c <= L'9') ? (c - L'0') : (c - L'a' + 10); };

    std::string bytes;
    size_t i = 0;
    while (i < s.size()) {
        wchar_t c = s[i];
        if (c == L'-') { bytes.push_back('\\'); i++; }
        else if (c == L'_') {
            i++;
            bool highForm = (i + 6 <= s.size() && s.compare(i, 6, L"ffffff") == 0);
            if (highForm) i += 6;
            int v = 0, taken = 0;
            while (taken < 2 && i < s.size() && isHex(s[i])) { v = v * 16 + hexVal(s[i]); i++; taken++; }
            bytes.push_back((char)(unsigned char)v);
        } else {
            bytes.push_back((char)c);
            i++;
        }
    }
    return Utf8ToWide(bytes);
}

// ---------------------------------------------------------------------------
//  UFunPlayer integration – auto-load the most recent save
//
//  If UFunPlayer.exe is running and has Recent Files history, compute
//  the expected .upp path and silently open it if it exists.
// ---------------------------------------------------------------------------
static bool IsProcessRunning(const wchar_t* exeName)
{
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32 pe = {};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32First(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) { found = true; break; }
        } while (Process32Next(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return found;
}

static std::wstring GetUFunPlayerMostRecentFile()
{
    HKEY hk = nullptr;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\UFunPlayer\\RecentFiles",
                      0, KEY_READ, &hk) != ERROR_SUCCESS)
        return L"";

    wchar_t buf[MAX_PATH * 2] = {};
    DWORD sz = sizeof(buf), type = 0;
    std::wstring result;
    if (RegQueryValueEx(hk, L"0", nullptr, &type, (BYTE*)buf, &sz) == ERROR_SUCCESS && type == REG_SZ)
        result = buf;
    RegCloseKey(hk);
    return result;
}

static std::wstring ToLowerW(const std::wstring& s)
{
    std::wstring r = s;
    for (auto& c : r) c = towlower(c);
    return r;
}

static std::wstring GetPrefsDomain(const std::wstring& gameSrc)
{
    std::wstring lower = ToLowerW(gameSrc);
    bool isHttp  = lower.compare(0, 7, L"http://") == 0;
    bool isHttps = lower.compare(0, 8, L"https://") == 0;
    if (!isHttp && !isHttps) return L"localhost";

    size_t schemeLen = isHttps ? 8 : 7;
    size_t hostEnd = gameSrc.find_first_of(L"/:", schemeLen);
    return (hostEnd == std::wstring::npos)
        ? gameSrc.substr(schemeLen)
        : gameSrc.substr(schemeLen, hostEnd - schemeLen);
}

static std::wstring ComputeExpectedSavePath(const std::wstring& gameSrc)
{
    std::wstring base = GetWebPlayerPrefsDir();
    if (base.empty() || gameSrc.empty()) return L"";
    std::wstring domain  = GetPrefsDomain(gameSrc);
    std::wstring encoded = Utf8ToWide(EncodeUnityPrefFilenameUtf8(gameSrc));
    return base + L"\\" + domain + L"\\" + encoded;
}

// ---------------------------------------------------------------------------
//  Value parsing / formatting
// ---------------------------------------------------------------------------
static std::wstring TrimW(const std::wstring& s)
{
    size_t start = s.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) return L"";
    size_t end = s.find_last_not_of(L" \t\r\n");
    return s.substr(start, end - start + 1);
}

static bool TryParseInt32(const std::wstring& textRaw, int32_t& out)
{
    std::wstring text = TrimW(textRaw);
    if (text.empty()) return false;
    wchar_t* endPtr = nullptr;
    errno = 0;
    long val = wcstol(text.c_str(), &endPtr, 10);
    if (endPtr != text.c_str() + text.size()) return false;
    if (errno == ERANGE || val > INT32_MAX || val < INT32_MIN) return false;
    out = (int32_t)val;
    return true;
}
static bool TryParseFloat(const std::wstring& textRaw, float& out)
{
    std::wstring text = TrimW(textRaw);
    if (text.empty()) return false;
    wchar_t* endPtr = nullptr;
    float val = wcstof(text.c_str(), &endPtr);
    if (endPtr != text.c_str() + text.size()) return false;
    out = val;
    return true;
}

static std::wstring TypeToString(PrefType t)
{
    switch (t) {
        case PrefType::String: return L"String";
        case PrefType::Int:    return L"Int";
        case PrefType::Float:  return L"Float";
    }
    return L"?";
}
static std::wstring FormatValue(const PrefEntry& e)
{
    wchar_t buf[64];
    switch (e.type) {
        case PrefType::Int:
            swprintf(buf, 64, L"%d", e.intValue);
            return buf;
        case PrefType::Float:
            swprintf(buf, 64, L"%g", e.floatValue);
            return buf;
        case PrefType::String:
        default:
            return Utf8ToWide(e.strValue);
    }
}

// ---------------------------------------------------------------------------
//  Global state
// ---------------------------------------------------------------------------
static HINSTANCE g_hInst    = nullptr;
static HWND      g_hwndMain = nullptr;
static HACCEL    g_hAccel   = nullptr;

static std::vector<PrefEntry> g_entries;
static std::vector<size_t>    g_filteredIndices; // ListView row -> g_entries index
static std::wstring           g_currentFile;
static std::wstring           g_searchText;
static bool                   g_dirty = false;
static std::wstring           g_pendingFile;      // command-line file

// ---------------------------------------------------------------------------
//  Forward declarations
// ---------------------------------------------------------------------------
LRESULT CALLBACK MainWndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK AddKeyDlgProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK EditValueDlgProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK BrowseSavesDlgProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK AboutDlgProc(HWND, UINT, WPARAM, LPARAM);

static bool ConfirmDiscardChanges(HWND hwnd);
static bool LoadUppFile(HWND hwnd, const std::wstring& path);
static bool DoSave(HWND hwnd);
static bool DoSaveAs(HWND hwnd);
static void DoClose(HWND hwnd);
static void DoBrowseSaves(HWND hwnd);
static void DoAddKey(HWND hwnd);
static void DoDeleteSelected(HWND hwnd);
static void DoEditSelected(HWND hwnd, int row);
static void RevealInExplorer(const std::wstring& path);

static void RefreshListView(HWND hwnd);
static void UpdateWindowTitle(HWND hwnd);
static void UpdateMenuStates(HWND hwnd);
static void UpdateMainViewVisibility(HWND hwnd);
static void PaintPlaceholder(HDC hdc, const RECT& rc);

// ---------------------------------------------------------------------------
//  Core actions
// ---------------------------------------------------------------------------
static bool ConfirmDiscardChanges(HWND hwnd)
{
    if (!g_dirty) return true;
    int r = MessageBox(hwnd,
        L"You have unsaved changes. Save before continuing?",
        L"Unsaved Changes", MB_YESNOCANCEL | MB_ICONWARNING);
    if (r == IDCANCEL) return false;
    if (r == IDYES) return DoSave(hwnd);
    return true;
}

static bool LoadUppFile(HWND hwnd, const std::wstring& path)
{
    if (!ConfirmDiscardChanges(hwnd)) return false;

    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(path, bytes)) {
        MessageBox(hwnd, (L"Failed to read file:\n" + path).c_str(), L"Open Failed", MB_ICONERROR);
        return false;
    }
    std::vector<PrefEntry> entries;
    std::wstring err;
    if (!ParseUpp(bytes, entries, err)) {
        MessageBox(hwnd, (L"Not a valid .upp file:\n\n" + err + L"\n\n" + path).c_str(),
                   L"Open Failed", MB_ICONERROR);
        return false;
    }

    g_entries = std::move(entries);
    g_currentFile = path;
    g_dirty = false;
    g_searchText.clear();
    SetDlgItemText(hwnd, IDC_SEARCH_EDIT, L"");

    UpdateMainViewVisibility(hwnd);
    RefreshListView(hwnd);
    UpdateWindowTitle(hwnd);
    UpdateMenuStates(hwnd);
    return true;
}

static bool DoSave(HWND hwnd)
{
    if (g_currentFile.empty()) return DoSaveAs(hwnd);
    std::vector<uint8_t> bytes = SerializeUpp(g_entries);
    if (!WriteFileBytes(g_currentFile, bytes)) {
        MessageBox(hwnd, (L"Failed to write file:\n" + g_currentFile).c_str(), L"Save Failed", MB_ICONERROR);
        return false;
    }
    g_dirty = false;
    UpdateWindowTitle(hwnd);
    return true;
}

static bool DoSaveAs(HWND hwnd)
{
    wchar_t file[MAX_PATH] = {};
    if (!g_currentFile.empty()) {
        size_t slash = g_currentFile.find_last_of(L"\\/");
        std::wstring fname = (slash == std::wstring::npos) ? g_currentFile : g_currentFile.substr(slash + 1);
        wcsncpy(file, fname.c_str(), MAX_PATH - 1);
    }

    OPENFILENAME ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd;
    ofn.lpstrFilter = L"Unity PlayerPrefs (*.upp)\0*.upp\0All Files\0*.*\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrDefExt = L"upp";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle  = L"Save Unity PlayerPrefs File";
    if (!GetSaveFileName(&ofn)) return false;

    std::vector<uint8_t> bytes = SerializeUpp(g_entries);
    if (!WriteFileBytes(file, bytes)) {
        MessageBox(hwnd, L"Failed to write file.", L"Save Failed", MB_ICONERROR);
        return false;
    }
    g_currentFile = file;
    g_dirty = false;
    UpdateWindowTitle(hwnd);
    return true;
}

static void DoClose(HWND hwnd)
{
    if (!ConfirmDiscardChanges(hwnd)) return;
    g_entries.clear();
    g_filteredIndices.clear();
    g_currentFile.clear();
    g_dirty = false;
    g_searchText.clear();
    UpdateMainViewVisibility(hwnd);
    RefreshListView(hwnd);
    UpdateWindowTitle(hwnd);
    UpdateMenuStates(hwnd);
}

static void RevealInExplorer(const std::wstring& path)
{
    std::wstring param = L"/select,\"" + path + L"\"";
    ShellExecute(nullptr, L"open", L"explorer.exe", param.c_str(), nullptr, SW_SHOWNORMAL);
}

struct SaveFileInfo { std::wstring realPath; std::wstring translatedPath; };
struct BrowseSavesContext {
    std::vector<SaveFileInfo> list;
    std::wstring chosenPath;
    bool openRequested = false;
};

static void DoBrowseSaves(HWND hwnd)
{
    BrowseSavesContext ctx;
    for (auto& path : ScanAppDataSaves()) {
        SaveFileInfo info;
        info.realPath = path;
        size_t slash = path.find_last_of(L"\\/");
        std::wstring fname = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
        size_t dot = fname.find_last_of(L'.');
        std::wstring fnameNoExt = (dot == std::wstring::npos) ? fname : fname.substr(0, dot);
        info.translatedPath = DecodeUppFilename(fnameNoExt);
        ctx.list.push_back(info);
    }

    DialogBoxParam(g_hInst, MAKEINTRESOURCE(IDD_BROWSE_SAVES), hwnd,
                   BrowseSavesDlgProc, (LPARAM)&ctx);

    if (ctx.openRequested) LoadUppFile(hwnd, ctx.chosenPath);
}

struct AddKeyResult { bool confirmed = false; PrefEntry entry; };

static void DoAddKey(HWND hwnd)
{
    AddKeyResult result;
    if (DialogBoxParam(g_hInst, MAKEINTRESOURCE(IDD_ADD_KEY), hwnd,
                        AddKeyDlgProc, (LPARAM)&result) != IDOK || !result.confirmed)
        return;

    for (auto& e : g_entries) {
        if (e.key == result.entry.key) {
            MessageBox(hwnd, L"A key with this name already exists.", L"Add Key", MB_ICONWARNING);
            return;
        }
    }
    g_entries.push_back(result.entry);
    g_dirty = true;
    g_searchText.clear();
    SetDlgItemText(hwnd, IDC_SEARCH_EDIT, L"");
    RefreshListView(hwnd);
    UpdateWindowTitle(hwnd);
}

static void DoDeleteSelected(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_LISTVIEW);
    int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= (int)g_filteredIndices.size()) return;

    size_t idx = g_filteredIndices[sel];
    std::wstring keyW = Utf8ToWide(g_entries[idx].key);
    std::wstring msg = L"Delete key \"" + keyW + L"\"?";
    if (MessageBox(hwnd, msg.c_str(), L"Delete Key", MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    g_entries.erase(g_entries.begin() + idx);
    g_dirty = true;
    RefreshListView(hwnd);
    UpdateWindowTitle(hwnd);
}

static void DoEditSelected(HWND hwnd, int row)
{
    if (row < 0 || row >= (int)g_filteredIndices.size()) return;
    size_t idx = g_filteredIndices[row];
    PrefEntry* entry = &g_entries[idx];

    if (DialogBoxParam(g_hInst, MAKEINTRESOURCE(IDD_EDIT_VALUE), hwnd,
                        EditValueDlgProc, (LPARAM)entry) == IDOK) {
        g_dirty = true;
        RefreshListView(hwnd);
        UpdateWindowTitle(hwnd);
    }
}

// ---------------------------------------------------------------------------
//  UI sync helpers
// ---------------------------------------------------------------------------
static void RefreshListView(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_LISTVIEW);
    ListView_DeleteAllItems(hList);
    g_filteredIndices.clear();

    std::wstring searchLower = ToLowerW(g_searchText);

    for (size_t i = 0; i < g_entries.size(); i++) {
        std::wstring keyW = Utf8ToWide(g_entries[i].key);
        if (!searchLower.empty()) {
            std::wstring keyLower = ToLowerW(keyW);
            if (keyLower.find(searchLower) == std::wstring::npos) continue;
        }
        int row = (int)g_filteredIndices.size();
        g_filteredIndices.push_back(i);

        LVITEM item = {};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.iSubItem = 0;
        item.pszText = &keyW[0];
        ListView_InsertItem(hList, &item);

        std::wstring typeText = TypeToString(g_entries[i].type);
        ListView_SetItemText(hList, row, 1, &typeText[0]);

        std::wstring valueText = FormatValue(g_entries[i]);
        ListView_SetItemText(hList, row, 2, &valueText[0]);
    }
    EnableWindow(GetDlgItem(hwnd, IDC_DELETE_BTN), FALSE);
}

static void UpdateWindowTitle(HWND hwnd)
{
    std::wstring title = std::wstring(APP_NAME) + L" " + APP_VERSION;
    if (!g_currentFile.empty()) {
        size_t pos = g_currentFile.find_last_of(L"\\/");
        std::wstring fname = (pos == std::wstring::npos) ? g_currentFile : g_currentFile.substr(pos + 1);
        title += L" - " + fname;
        if (g_dirty) title += L" *";
    }
    SetWindowText(hwnd, title.c_str());
}

static void UpdateMenuStates(HWND hwnd)
{
    HMENU hFile = GetSubMenu(GetMenu(hwnd), 0);
    UINT ena = g_currentFile.empty() ? MF_GRAYED : MF_ENABLED;
    EnableMenuItem(hFile, IDM_FILE_RELOAD, MF_BYCOMMAND | ena);
    EnableMenuItem(hFile, IDM_FILE_SAVE,   MF_BYCOMMAND | ena);
    EnableMenuItem(hFile, IDM_FILE_SAVEAS, MF_BYCOMMAND | ena);
    EnableMenuItem(hFile, IDM_FILE_CLOSE,  MF_BYCOMMAND | ena);
}

static void UpdateMainViewVisibility(HWND hwnd)
{
    bool loaded = !g_currentFile.empty();
    int showCmd = loaded ? SW_SHOW : SW_HIDE;
    ShowWindow(GetDlgItem(hwnd, IDC_SEARCH_LABEL), showCmd);
    ShowWindow(GetDlgItem(hwnd, IDC_SEARCH_EDIT),  showCmd);
    ShowWindow(GetDlgItem(hwnd, IDC_ADD_BTN),      showCmd);
    ShowWindow(GetDlgItem(hwnd, IDC_DELETE_BTN),   showCmd);
    ShowWindow(GetDlgItem(hwnd, IDC_LISTVIEW),     showCmd);
    InvalidateRect(hwnd, nullptr, TRUE);
}

static void PaintPlaceholder(HDC hdc, const RECT& rc)
{
    HBRUSH hbr = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(hdc, &rc, hbr);
    DeleteObject(hbr);

    const wchar_t* text = L"Open a .upp file via File > Open or File > Browse Saves,\n"
                           L"or drag one onto this window.";
    LOGFONT lf = {};
    lf.lfHeight = -16;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy(lf.lfFaceName, L"Segoe UI", LF_FACESIZE - 1);
    HFONT hFont = CreateFontIndirect(&lf);
    HFONT hOld = (HFONT)SelectObject(hdc, hFont);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(140, 140, 140));
    RECT r = rc;
    DrawText(hdc, text, -1, &r, DT_CENTER | DT_VCENTER);

    SelectObject(hdc, hOld);
    DeleteObject(hFont);
}

// ---------------------------------------------------------------------------
//  Dialog procedures
// ---------------------------------------------------------------------------
INT_PTR CALLBACK AddKeyDlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG:
        SetWindowLongPtr(hDlg, GWLP_USERDATA, (LONG_PTR)lp);
        CheckRadioButton(hDlg, IDC_ADDKEY_TYPE_STR, IDC_ADDKEY_TYPE_FLOAT, IDC_ADDKEY_TYPE_STR);
        return TRUE;

    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            AddKeyResult* res = (AddKeyResult*)GetWindowLongPtr(hDlg, GWLP_USERDATA);

            wchar_t keyBuf[256] = {};
            GetDlgItemText(hDlg, IDC_ADDKEY_NAME, keyBuf, 256);
            std::wstring keyW = keyBuf;
            if (keyW.empty()) {
                MessageBox(hDlg, L"Key name cannot be empty.", L"Add Key", MB_ICONWARNING);
                return TRUE;
            }
            std::string keyUtf8 = WideToUtf8(keyW);
            if (keyUtf8.size() > 255) {
                MessageBox(hDlg, L"Key name is too long (max 255 bytes in UTF-8).", L"Add Key", MB_ICONWARNING);
                return TRUE;
            }

            HWND hVal = GetDlgItem(hDlg, IDC_ADDKEY_VALUE);
            int len = GetWindowTextLength(hVal);
            std::wstring valW(len, L'\0');
            if (len > 0) GetWindowText(hVal, &valW[0], len + 1);

            PrefEntry e;
            e.key = keyUtf8;
            if (IsDlgButtonChecked(hDlg, IDC_ADDKEY_TYPE_INT) == BST_CHECKED) {
                int32_t v;
                if (!TryParseInt32(valW, v)) {
                    MessageBox(hDlg, L"Value must be a valid integer (e.g. 42 or -7).", L"Add Key", MB_ICONWARNING);
                    return TRUE;
                }
                e.type = PrefType::Int; e.intValue = v;
            } else if (IsDlgButtonChecked(hDlg, IDC_ADDKEY_TYPE_FLOAT) == BST_CHECKED) {
                float v;
                if (!TryParseFloat(valW, v)) {
                    MessageBox(hDlg, L"Value must be a valid number (e.g. 3.14).", L"Add Key", MB_ICONWARNING);
                    return TRUE;
                }
                e.type = PrefType::Float; e.floatValue = v;
            } else {
                e.type = PrefType::String; e.strValue = WideToUtf8(valW);
            }

            res->entry = e;
            res->confirmed = true;
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(hDlg, IDCANCEL); return TRUE; }
        return TRUE;
    }
    return FALSE;
}

INT_PTR CALLBACK EditValueDlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowLongPtr(hDlg, GWLP_USERDATA, (LONG_PTR)lp);
        PrefEntry* e = (PrefEntry*)lp;
        SetDlgItemText(hDlg, IDC_EDITVAL_KEYLABEL, Utf8ToWide(e->key).c_str());
        SetDlgItemText(hDlg, IDC_EDITVAL_TYPELABEL, TypeToString(e->type).c_str());
        SetDlgItemText(hDlg, IDC_EDITVAL_VALUE, FormatValue(*e).c_str());
        SetFocus(GetDlgItem(hDlg, IDC_EDITVAL_VALUE));
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            PrefEntry* e = (PrefEntry*)GetWindowLongPtr(hDlg, GWLP_USERDATA);
            HWND hVal = GetDlgItem(hDlg, IDC_EDITVAL_VALUE);
            int len = GetWindowTextLength(hVal);
            std::wstring valW(len, L'\0');
            if (len > 0) GetWindowText(hVal, &valW[0], len + 1);

            if (e->type == PrefType::Int) {
                int32_t v;
                if (!TryParseInt32(valW, v)) {
                    MessageBox(hDlg, L"Value must be a valid integer.", L"Edit Value", MB_ICONWARNING);
                    return TRUE;
                }
                e->intValue = v;
            } else if (e->type == PrefType::Float) {
                float v;
                if (!TryParseFloat(valW, v)) {
                    MessageBox(hDlg, L"Value must be a valid number.", L"Edit Value", MB_ICONWARNING);
                    return TRUE;
                }
                e->floatValue = v;
            } else {
                e->strValue = WideToUtf8(valW);
            }
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(hDlg, IDCANCEL); return TRUE; }
        return TRUE;
    }
    return FALSE;
}

INT_PTR CALLBACK BrowseSavesDlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowLongPtr(hDlg, GWLP_USERDATA, (LONG_PTR)lp);
        BrowseSavesContext* ctx = (BrowseSavesContext*)lp;

        HWND hList = GetDlgItem(hDlg, IDC_BROWSE_LISTVIEW);
        ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        LVCOLUMN col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        wchar_t c0[] = L"Game (approximate)"; col.pszText = c0; col.cx = 250; ListView_InsertColumn(hList, 0, &col);
        wchar_t c1[] = L"Save File";          col.pszText = c1; col.cx = 180; ListView_InsertColumn(hList, 1, &col);

        for (size_t i = 0; i < ctx->list.size(); i++) {
            LVITEM item = {};
            item.mask = LVIF_TEXT;
            item.iItem = (int)i;
            item.pszText = &ctx->list[i].translatedPath[0];
            ListView_InsertItem(hList, &item);
            ListView_SetItemText(hList, (int)i, 1, &ctx->list[i].realPath[0]);
        }
        if (ctx->list.empty()) {
            EnableWindow(GetDlgItem(hDlg, IDC_BROWSE_OPEN), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BROWSE_OPENFOLDER), FALSE);
        }
        return TRUE;
    }
    case WM_NOTIFY: {
        LPNMHDR nm = (LPNMHDR)lp;
        if (nm->idFrom == IDC_BROWSE_LISTVIEW && nm->code == NM_DBLCLK) {
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(IDC_BROWSE_OPEN, 0), 0);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BROWSE_OPEN: {
            BrowseSavesContext* ctx = (BrowseSavesContext*)GetWindowLongPtr(hDlg, GWLP_USERDATA);
            HWND hList = GetDlgItem(hDlg, IDC_BROWSE_LISTVIEW);
            int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
            if (sel < 0 || sel >= (int)ctx->list.size()) return TRUE;
            ctx->chosenPath = ctx->list[sel].realPath;
            ctx->openRequested = true;
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        case IDC_BROWSE_OPENFOLDER: {
            BrowseSavesContext* ctx = (BrowseSavesContext*)GetWindowLongPtr(hDlg, GWLP_USERDATA);
            HWND hList = GetDlgItem(hDlg, IDC_BROWSE_LISTVIEW);
            int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
            if (sel < 0 || sel >= (int)ctx->list.size()) return TRUE;
            RevealInExplorer(ctx->list[sel].realPath);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        return TRUE;
    }
    return FALSE;
}

INT_PTR CALLBACK AboutDlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM)
{
    if (msg == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) EndDialog(hDlg, 0);
    return msg == WM_INITDIALOG ? TRUE : FALSE;
}

// ---------------------------------------------------------------------------
//  Main window procedure
// ---------------------------------------------------------------------------
LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {

    case WM_CREATE: {
        DragAcceptFiles(hwnd, TRUE);
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        HWND hSearchLabel = CreateWindow(L"STATIC", L"Search:", WS_CHILD | WS_VISIBLE,
            8, 9, 50, 18, hwnd, (HMENU)IDC_SEARCH_LABEL, g_hInst, nullptr);
        SendMessage(hSearchLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hSearchEdit = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            60, 6, 220, 22, hwnd, (HMENU)IDC_SEARCH_EDIT, g_hInst, nullptr);
        SendMessage(hSearchEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hAddBtn = CreateWindow(L"BUTTON", L"Add Key...", WS_CHILD | WS_VISIBLE,
            292, 5, 90, 24, hwnd, (HMENU)IDC_ADD_BTN, g_hInst, nullptr);
        SendMessage(hAddBtn, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hDelBtn = CreateWindow(L"BUTTON", L"Delete Key", WS_CHILD | WS_VISIBLE,
            388, 5, 90, 24, hwnd, (HMENU)IDC_DELETE_BTN, g_hInst, nullptr);
        SendMessage(hDelBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
        EnableWindow(hDelBtn, FALSE);

        HWND hList = CreateWindowEx(WS_EX_CLIENTEDGE, WC_LISTVIEW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, TOOLBAR_H, 100, 100, hwnd, (HMENU)IDC_LISTVIEW, g_hInst, nullptr);
        ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        SendMessage(hList, WM_SETFONT, (WPARAM)hFont, TRUE);

        LVCOLUMN col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        wchar_t c0[] = L"Key";   col.pszText = c0; col.cx = 240; ListView_InsertColumn(hList, 0, &col);
        wchar_t c1[] = L"Type";  col.pszText = c1; col.cx = 70;  ListView_InsertColumn(hList, 1, &col);
        wchar_t c2[] = L"Value"; col.pszText = c2; col.cx = 320; ListView_InsertColumn(hList, 2, &col);

        UpdateMainViewVisibility(hwnd);
        PostMessage(hwnd, WM_POSTINIT, 0, 0);
        return 0;
    }

    case WM_POSTINIT: {
        if (!g_pendingFile.empty()) {
            std::wstring f = g_pendingFile;
            g_pendingFile.clear();
            LoadUppFile(hwnd, f);
        } else if (IsProcessRunning(L"UFunPlayer.exe")) {
            std::wstring recentGame = GetUFunPlayerMostRecentFile();
            if (!recentGame.empty()) {
                std::wstring savePath = ComputeExpectedSavePath(recentGame);
                if (!savePath.empty() && PathFileExists(savePath.c_str()))
                    LoadUppFile(hwnd, savePath);
            }
        }
        return 0;
    }

    case WM_SIZE: {
        int w = LOWORD(lp), h = HIWORD(lp);
        int delW = 90, addW = 90, gap = 6;
        MoveWindow(GetDlgItem(hwnd, IDC_DELETE_BTN), w - delW - 8, 5, delW, 24, TRUE);
        MoveWindow(GetDlgItem(hwnd, IDC_ADD_BTN), w - delW - 8 - gap - addW, 5, addW, 24, TRUE);
        int searchW = w - delW - addW - gap - 8 - 8 - 60;
        if (searchW < 50) searchW = 50;
        MoveWindow(GetDlgItem(hwnd, IDC_SEARCH_EDIT), 60, 6, searchW, 22, TRUE);
        int listH = (h > TOOLBAR_H) ? (h - TOOLBAR_H) : 0;
        MoveWindow(GetDlgItem(hwnd, IDC_LISTVIEW), 0, TOOLBAR_H, w, listH, TRUE);
        return 0;
    }

    case WM_COMMAND: {
        WORD id = LOWORD(wp);
        if (id == IDC_SEARCH_EDIT && HIWORD(wp) == EN_CHANGE) {
            wchar_t buf[512] = {};
            GetDlgItemText(hwnd, IDC_SEARCH_EDIT, buf, 512);
            g_searchText = buf;
            RefreshListView(hwnd);
            return 0;
        }
        switch (id) {
        case IDM_FILE_OPEN: {
            wchar_t file[MAX_PATH] = {};
            OPENFILENAME ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"Unity PlayerPrefs (*.upp)\0*.upp\0All Files\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            ofn.lpstrTitle = L"Open Unity PlayerPrefs File";
            std::wstring initDir = GetWebPlayerPrefsDir();
            if (!initDir.empty() && PathFileExists(initDir.c_str())) ofn.lpstrInitialDir = initDir.c_str();
            if (GetOpenFileName(&ofn)) LoadUppFile(hwnd, file);
            break;
        }
        case IDM_FILE_BROWSE_SAVES: DoBrowseSaves(hwnd); break;
        case IDM_FILE_RELOAD: if (!g_currentFile.empty()) LoadUppFile(hwnd, g_currentFile); break;
        case IDM_FILE_SAVE:   DoSave(hwnd); break;
        case IDM_FILE_SAVEAS: DoSaveAs(hwnd); break;
        case IDM_FILE_CLOSE:  DoClose(hwnd); break;
        case IDM_FILE_EXIT:   PostMessage(hwnd, WM_CLOSE, 0, 0); break;
        case IDC_ADD_BTN:     DoAddKey(hwnd); break;
        case IDC_DELETE_BTN:  DoDeleteSelected(hwnd); break;
        case IDM_HELP_REPO:   ShellExecute(hwnd, L"open", GITHUB_URL, nullptr, nullptr, SW_SHOWNORMAL); break;
        case IDM_HELP_ABOUT:  DialogBox(g_hInst, MAKEINTRESOURCE(IDD_ABOUT), hwnd, AboutDlgProc); break;
        }
        return 0;
    }

    case WM_NOTIFY: {
        LPNMHDR nm = (LPNMHDR)lp;
        if (nm->idFrom == IDC_LISTVIEW) {
            if (nm->code == NM_DBLCLK) {
                NMITEMACTIVATE* nia = (NMITEMACTIVATE*)lp;
                if (nia->iItem >= 0) DoEditSelected(hwnd, nia->iItem);
                return 0;
            }
            if (nm->code == LVN_KEYDOWN) {
                NMLVKEYDOWN* kd = (NMLVKEYDOWN*)lp;
                if (kd->wVKey == VK_DELETE) DoDeleteSelected(hwnd);
                return 0;
            }
            if (nm->code == LVN_ITEMCHANGED) {
                int sel = ListView_GetSelectedCount(GetDlgItem(hwnd, IDC_LISTVIEW));
                EnableWindow(GetDlgItem(hwnd, IDC_DELETE_BTN), sel > 0);
                return 0;
            }
        }
        break;
    }

    case WM_DROPFILES: {
        HDROP hd = (HDROP)wp;
        wchar_t buf[MAX_PATH] = {};
        if (DragQueryFile(hd, 0, buf, MAX_PATH) > 0) LoadUppFile(hwnd, buf);
        DragFinish(hd);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (g_currentFile.empty()) {
            RECT rc; GetClientRect(hwnd, &rc);
            PaintPlaceholder(hdc, rc);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CLOSE:
        if (ConfirmDiscardChanges(hwnd)) DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
//  wWinMain  (Unicode entry point; link with -municode)
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow)
{
    g_hInst = hInst;

    if (__argc >= 2 && __wargv[1][0]) g_pendingFile = __wargv[1];

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEX wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIcon(hInst, MAKEINTRESOURCE(IDI_MAINICON));
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszMenuName  = MAKEINTRESOURCE(IDR_MAINMENU);
    wc.lpszClassName = L"UPPEditorWnd";
    wc.hIconSm       = LoadIcon(hInst, MAKEINTRESOURCE(IDI_SMALLICON));
    RegisterClassEx(&wc);

    g_hAccel = LoadAccelerators(hInst, MAKEINTRESOURCE(IDR_ACCEL));

    g_hwndMain = CreateWindowEx(
        WS_EX_ACCEPTFILES, L"UPPEditorWnd",
        (std::wstring(APP_NAME) + L" " + APP_VERSION).c_str(),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 720, 500,
        nullptr, nullptr, hInst, nullptr);
    if (!g_hwndMain) return 1;

    ShowWindow(g_hwndMain, nShow);
    UpdateWindow(g_hwndMain);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        if (!TranslateAccelerator(g_hwndMain, g_hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    return (int)msg.wParam;
}