#include "UserTile.h"
#include <windowsx.h>
#include <uxtheme.h>
#include <commctrl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <objbase.h>
#include <wincodec.h>
#include <sddl.h>
#include <wtsapi32.h>
#define SECURITY_WIN32
#include <security.h>
#include <lm.h>
#include "resource.h"
#include "FlyoutFix.h"

// The tile subclass, the tooltip and the press state all live in
// UserTile.cpp now, this file owns the flyout window and nothing else

// The wrapper runs on minCRT with a custom entry point, so the CRT is never
// initialised and its string helpers are not safe to call. These map the
// few the flyout uses onto the shlwapi ones the project already links.
// Done as shims rather than edits so the lifted code stays untouched
static inline void UT_wcscpy_s(wchar_t* dst, size_t cch, const wchar_t* src)
{
	StrCpyNW(dst, src, (int)cch);
}

#define wcscpy_s      UT_wcscpy_s
#define swprintf_s    wnsprintfW
#define _wcsicmp      lstrcmpiW
#define _wtoi         StrToIntW
#define wcsrchr(s, c) StrRChrW((s), NULL, (c))

//---Localized text-----------------------------------------

// Text lives in wrapper.rc so muirct can split it into wrp64.dll.mui
// The literals below are only used when the resource is missing

enum {
	UTS_LOGGED_ON_AS = 0,
	UTS_ADMINISTRATOR,
	UTS_GUEST_ACCOUNT,
	UTS_STANDARD_USER,
	UTS_PASSWORD_PROT,
	UTS_LOCK_COMPUTER,
	UTS_LOG_OFF,
	UTS_SWITCH_USER,
	UTS_OPEN_USER_ACCT,
	UTS_LOG_OFF_FMT,
	UTS_LOCK,
	UTS_COUNT
};

static const struct { UINT id; const wchar_t* fallback; } kUtText[UTS_COUNT] = {
	{ IDS_UT_LOGGED_ON_AS,   L"Currently logged on as:" },
	{ IDS_UT_ADMINISTRATOR,  L"Administrator"           },
	{ IDS_UT_GUEST_ACCOUNT,  L"Guest account"           },
	{ IDS_UT_STANDARD_USER,  L"Standard user"           },
	{ IDS_UT_PASSWORD_PROT,  L"Password protected"      },
	{ IDS_UT_LOCK_COMPUTER,  L"Lock computer"           },
	{ IDS_UT_LOG_OFF,        L"Log off"                 },
	{ IDS_UT_SWITCH_USER,    L"Switch user"             },
	{ IDS_UT_OPEN_USER_ACCT, L"Open User Accounts"      },
	{ IDS_UT_LOG_OFF_FMT,    L"Log off %s..."           },
	{ IDS_UT_LOCK,           L"Lock"                    },
};

static wchar_t g_utText[UTS_COUNT][96];
static bool    g_utTextReady = false;

// Loaded once, so the returned pointers stay valid for the whole process
static const wchar_t* UT_Text(int idx)
{
	if (!g_utTextReady)
	{
		for (int i = 0; i < UTS_COUNT; ++i)
		{
			if (LoadStringW(g_hInstance, kUtText[i].id, g_utText[i],
			                ARRAYSIZE(g_utText[i])) <= 0)
				StrCpyNW(g_utText[i], kUtText[i].fallback, ARRAYSIZE(g_utText[i]));
		}
		g_utTextReady = true;
	}
	return g_utText[idx];
}

//---Subclass ids-------------------------------------------

// Arbitrary unique ids for SetWindowSubclass
#define TOOLTIP_SUBCLASS_ID  0x7785cf14u
#define TILE_SUBCLASS_ID     0x7785cf15u

// Forward declarations, the show helpers are defined further down.
// UserTile_DismissTooltip is not one of these, it lives in UserTile.cpp
// and is declared in the header
static void ShowFlyoutForTile(HWND tileHwnd);
static void ShowUserTileContextMenu(HWND tileHwnd);
static void RefreshUserName();

// Flyout state, declared here so the tile subclass can read it from
// WM_MOUSEACTIVATE. Initialization happens in EnsureFlyoutCreated below
static HWND   g_flyoutHwnd     = nullptr;
static HTHEME g_flyoutTheme    = nullptr;  // "Flyout" theme, body bg and label colors
static HTHEME g_chinTheme      = nullptr;  // "TrayNotifyFlyout" theme, chin bg
static HTHEME g_listViewTheme  = nullptr;  // "Explorer::ListView", task button hover

// Compact-mode toggle (Windhawk setting). Read at Wh_ModInit and on
// Wh_ModSettingsChanged. Drives layout-variable selection below.
static bool g_compactMode = false;

// Set by Wh_ModSettingsChanged when compactMode flips. Picked up by
// ShowFlyoutForTile (which runs on the tray thread) to tear down the
// flyout window safely. Wh_ModSettingsChanged is not guaranteed to run
// on the tray thread, and DestroyWindow / UnregisterClass fail silently
// across threads — so we defer those to the tray thread.
static bool g_flyoutNeedsRebuild = false;

// Phase 5 — cached user picture, decoded once via WIC at flyout creation.
// HBITMAP is a 32bpp PARGB DIB section sized exactly USERPIC_W × USERPIC_H,
// so WM_PAINT is a single AlphaBlend with no per-paint scaling.
//
// Both modes: picture is centered inside the aero frame's display rect
// and inset by k*PicShrinkPct so the bevel/glass overlay falls outside
// the photo. RecomputeLayout fills these in from AERO_* + the shrink
// percentage; values below are placeholders overwritten on first init.
static int USERPIC_X = 18;
static int USERPIC_Y = 46;
static int USERPIC_W = 104;
static int USERPIC_H = 104;
static HBITMAP  g_userPictureBmp                 = nullptr;
static wchar_t  g_userPictureSrcPath[MAX_PATH]   = L"";  // last loaded source path
static FILETIME g_userPictureSrcMtime            = {};   // last loaded source mtime

// Phase 6, cached aero glass frame borders, one per mode
// Both come from BITMAP resources, see notes/usertile-frame-resources.md

// Placeholders, RecomputeLayout sets the real values on init
static int AERO_X = -8;
static int AERO_Y = 20;
static int AERO_W = 156;
static int AERO_H = 156;
static HBITMAP g_aeroFrameBmp        = nullptr;
static HBITMAP g_aeroFrameSmallBmp   = nullptr;
static bool    g_aeroFrameTried      = false;
static bool    g_aeroFrameSmallTried = false;

// Bypass for explorer7's OpenThemeData hook. The wrapper installs a
// MinHook over uxtheme!OpenThemeData that, *when called from the tray
// thread*, redirects to its own bundled aero msstyles (light) — and
// passes through to the original uxtheme call (system active style,
// e.g. aerodark) for any *other* thread. See
//   exports/explorer7-wrapper/explorerwrapper/MinhookImports.h:11-26
// HTHEME handles returned from another thread remain valid and usable
// on the tray thread (they're process-wide opaque pointers), so we can
// simply open on a one-shot worker and use the result here.
struct ThemeOpenReq {
    HWND     hwnd;
    LPCWSTR  classList;
    HTHEME   result;
};

static DWORD WINAPI OpenThemeWorker(LPVOID lp) {
    auto* req = static_cast<ThemeOpenReq*>(lp);
    req->result = OpenThemeData(req->hwnd, req->classList);
    return 0;
}

static HTHEME OpenSystemTheme(HWND hwnd, LPCWSTR classList) {
    // Heap-allocate the request so a worker that's still running after
    // we time out can keep writing to it without trampling the caller's
    // stack. During an OS theme transition (Personalization apply),
    // OpenThemeData has been observed to hang for many seconds — long
    // enough to lock up the tray thread if we WaitForSingleObject(INFINITE).
    auto* req = new ThemeOpenReq{ hwnd, classList, nullptr };
    HANDLE t = CreateThread(nullptr, 0, OpenThemeWorker, req, 0, nullptr);
    if (!t) { delete req; return nullptr; }
    HTHEME result = nullptr;
    if (WaitForSingleObject(t, 2000) == WAIT_OBJECT_0) {
        result = req->result;
        delete req;
    } else {
        // Timeout: leak req + thread. Worker eventually returns; small
        // one-time cost beats deadlocking the tray thread.
        dbgprintf(L"OpenSystemTheme(%s) timed out — falling back to null",
               classList);
    }
    CloseHandle(t);
    return result;
}

// Idempotent — opens any theme handle that's currently null. Used by
// EnsureFlyoutCreated on first creation, and by ShowFlyoutForTile to
// reopen after WM_THEMECHANGED dropped the handles.
static void EnsureFlyoutThemes() {
    if (!g_flyoutHwnd || !IsWindow(g_flyoutHwnd)) return;
    if (!g_flyoutTheme)
        g_flyoutTheme = OpenSystemTheme(g_flyoutHwnd, L"Flyout");
    if (!g_chinTheme)
        g_chinTheme = OpenSystemTheme(g_flyoutHwnd, L"TrayNotifyFlyout");
    if (!g_listViewTheme)
        g_listViewTheme = OpenSystemTheme(g_flyoutHwnd, L"Explorer::ListView");
}

// Mirrors timedate.cpl's FlyoutSheet (UIFILE 305) theming exactly:
//   - body bg:   DrawThemeBackground(L"Flyout",          FLYOUT_WINDOW=6, 0, ...)
//   - chin bg:   DrawThemeBackground(L"TrayNotifyFlyout", part=2,         0, ...)
//   - body text: GetThemeColor      (L"Flyout",          FLYOUT_BODY=2,   0, TMT_TEXTCOLOR)
//   - link text: GetThemeColor      (L"Flyout",          FLYOUT_LINK=4,   1/2, TMT_TEXTCOLOR)
// (Reverse-engineered from exports/timedate-cpl/uifile_305.bin "FlyoutSheet" stylesheet.)

// The FLYOUT_ part ids and the TMT_ property ids all come from vsstyle.h,
// which the wrapper already pulls in through uxtheme. It spells the link
// states FLYOUTLINK_ rather than FLYOUT_LINK_, so alias those two
#define FLYOUT_LINK_NORMAL FLYOUTLINK_NORMAL
#define FLYOUT_LINK_HOVER  FLYOUTLINK_HOVER

// Pulls a color from the Flyout theme; returns fallback if theme/part not set.
static COLORREF GetFlyoutColor(int part, int state, int propId,
                               COLORREF fallback) {
    if (!g_flyoutTheme) return fallback;
    COLORREF c = 0;
    if (SUCCEEDED(GetThemeColor(g_flyoutTheme, part, state, propId, &c))) {
        return c;
    }
    return fallback;
}

// Theme handle for the universal "TextStyle" class. Part TEXT_MAININSTRUCTION
// (= 1) carries the iconic dark-blue heading color used by Control Panel
// page titles, Task Dialog main instructions, and balloon-tip headers —
// classic #003399-ish blue under Win7/Win8 Aero light, and whatever the
// active theme defines (e.g. lighter blue under aerolite, near-black under
// high-contrast white) on the system's other styles.
//
// Opened via OpenSystemTheme so the worker thread bypasses explorer7's
// tray-thread OpenThemeData hook (which would otherwise pin us to the
// bundled aero light msstyles regardless of the user's active theme).
// Cleared on WM_THEMECHANGED so a theme switch picks up the new color.
static HTHEME g_textStyleTheme = nullptr;
static bool   g_textStyleTried = false;

static COLORREF GetMainInstructionColor(COLORREF fallback) {
    if (!g_textStyleTried) {
        g_textStyleTried = true;
        g_textStyleTheme = OpenSystemTheme(g_flyoutHwnd, L"TextStyle");
    }
    if (!g_textStyleTheme) return fallback;
    COLORREF c = fallback;
    if (FAILED(GetThemeColor(g_textStyleTheme, 1 /*TEXT_MAININSTRUCTION*/,
                              0, TMT_TEXTCOLOR, &c))) {
        return fallback;
    }
    return c;
}

// -----------------------------------------------------------------
// Phase 5 — user picture loading (WIC)
// -----------------------------------------------------------------
// Mirrors UserTile.Helper.Paths.EnsureUserTileFromPublicAccountPictures
// search priority:
//   1. C:\ProgramData\AccountPictures\<SID>\*       (newest matching)
//   2. C:\Users\Public\AccountPictures\<SID>\*Image192*
//   3. C:\ProgramData\Microsoft\User Account Pictures\user.{png,bmp}
// WIC is preferred over GDI+ to avoid pulling gdiplus.dll into explorer7's
// address space — windowscodecs.dll is already loaded by the shell.

static LPWSTR GetCurrentUserSidString() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return nullptr;
    }
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) { CloseHandle(token); return nullptr; }
    auto* tu = static_cast<TOKEN_USER*>(LocalAlloc(LPTR, needed));
    if (!tu) { CloseHandle(token); return nullptr; }
    LPWSTR sidStr = nullptr;
    if (GetTokenInformation(token, TokenUser, tu, needed, &needed)) {
        ConvertSidToStringSidW(tu->User.Sid, &sidStr);
    }
    LocalFree(tu);
    CloseHandle(token);
    return sidStr;  // caller frees via LocalFree
}

static bool PickNewestFile(LPCWSTR dir, LPCWSTR mask,
                           bool requireImageExt,
                           wchar_t outPath[MAX_PATH]) {
    wchar_t search[MAX_PATH];
    swprintf_s(search, MAX_PATH, L"%s\\%s", dir, mask);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    FILETIME bestTime = {};
    wchar_t bestName[MAX_PATH] = L"";
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (requireImageExt) {
            const wchar_t* ext = wcsrchr(fd.cFileName, L'.');
            if (!ext) continue;
            if (_wcsicmp(ext, L".png")  != 0 &&
                _wcsicmp(ext, L".jpg")  != 0 &&
                _wcsicmp(ext, L".jpeg") != 0 &&
                _wcsicmp(ext, L".bmp")  != 0) continue;
        }
        if (CompareFileTime(&fd.ftLastWriteTime, &bestTime) > 0) {
            bestTime = fd.ftLastWriteTime;
            wcscpy_s(bestName, MAX_PATH, fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!bestName[0]) return false;
    swprintf_s(outPath, MAX_PATH, L"%s\\%s", dir, bestName);
    return true;
}

// Primary path resolver — reads the same registry key the shell writes
// when the system caches a user picture. This is what
// `SHGetUserPicturePath` (the API 7850's _CreateUserPicture wraps) reads
// under the hood: HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\
// AccountPicture\Users\<SID>, which has REG_SZ values "Image1080",
// "Image448", "Image192", "Image96"... pointing at the actual cached
// PNG paths the shell knows about.
//
// Win10 stores those files under %LOCALAPPDATA% and %ProgramData% in
// places our directory enumeration didn't always cover, which is why
// the previous "find newest mtime in well-known dirs" approach
// returned stale results. Going through the registry gives us the
// path the shell itself uses.
//
// Selection strategy: pick the *freshest-mtime* file among all Image*
// values, tie-broken by largest resolution. The shell's update path
// doesn't always refresh every Image* simultaneously — a "biggest
// first" strategy could pin to a stale Image1080 while Image192 had
// the latest content (the v2.8.26 "second picture change doesn't
// reflect" bug). Freshest-first guarantees we always grab whichever
// size the most recent shell update actually rewrote.
static bool ResolveUserPicturePathFromRegistry(wchar_t outPath[MAX_PATH]) {
    LPWSTR sidStr = GetCurrentUserSidString();
    if (!sidStr) return false;

    wchar_t keyPath[256];
    swprintf_s(keyPath, ARRAYSIZE(keyPath),
               L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\"
               L"AccountPicture\\Users\\%s", sidStr);
    LocalFree(sidStr);

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0,
                      KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    static const wchar_t* const kImageValues[] = {
        L"Image1080", L"Image448", L"Image240",
        L"Image192",  L"Image96",  L"Image64",
        L"Image48",   L"Image40",  L"Image32",
    };

    wchar_t  bestPath[MAX_PATH] = L"";
    FILETIME bestMtime          = {};
    int      bestSize           = 0;

    for (const auto* valName : kImageValues) {
        DWORD type = 0;
        wchar_t buf[MAX_PATH] = L"";
        DWORD cb = sizeof(buf);
        if (RegQueryValueExW(hKey, valName, nullptr, &type,
                             reinterpret_cast<BYTE*>(buf),
                             &cb) != ERROR_SUCCESS) {
            continue;
        }
        if ((type != REG_SZ && type != REG_EXPAND_SZ) || !buf[0]) continue;
        if (type == REG_EXPAND_SZ) {
            wchar_t expanded[MAX_PATH];
            if (ExpandEnvironmentStringsW(buf, expanded, MAX_PATH) > 0) {
                wcscpy_s(buf, MAX_PATH, expanded);
            }
        }

        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        if (!GetFileAttributesExW(buf, GetFileExInfoStandard, &fad)) continue;
        if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        // Parse "ImageNNNN" -> NNNN for the tie-breaker.
        int sz = _wtoi(valName + 5);  // skip "Image"

        dbgprintf(L"  registry candidate %s: mtime=%08X:%08X size=%d -> %s",
               valName, fad.ftLastWriteTime.dwHighDateTime,
               fad.ftLastWriteTime.dwLowDateTime, sz, buf);

        int cmp = bestPath[0]
            ? CompareFileTime(&fad.ftLastWriteTime, &bestMtime)
            : 1;  // first valid entry always wins
        bool fresher    = (cmp >  0);
        bool sameButBig = (cmp == 0 && sz > bestSize);
        if (fresher || sameButBig) {
            wcscpy_s(bestPath, MAX_PATH, buf);
            bestMtime = fad.ftLastWriteTime;
            bestSize  = sz;
        }
    }

    RegCloseKey(hKey);
    if (!bestPath[0]) return false;

    wcscpy_s(outPath, MAX_PATH, bestPath);
    dbgprintf(L"User picture: registry resolved Image%d -> %s mtime=%08X:%08X",
           bestSize, bestPath,
           bestMtime.dwHighDateTime, bestMtime.dwLowDateTime);
    return true;
}

static bool ResolveUserPicturePath(wchar_t outPath[MAX_PATH]) {
    // The registry is the *primary* signal — that's where the shell
    // declares the canonical picture path. But on Win10 we've observed
    // that the shell's update path doesn't always refresh the registry
    // when Settings → Accounts changes the picture: subsequent changes
    // can write a new file to the same directory (with a different
    // filename) and update only some Image* values, leaving Image1080
    // pointing at the original first-change file. Net result: the
    // registry-resolved path becomes stale even though `_UpdateUserInfo`
    // fires correctly.
    //
    // Defense: gather candidates from both registry AND directory
    // enumeration (incl. AppData), then return the freshest-mtime one
    // across all of them. Adds the AppData/LocalAppData paths because
    // some Win10 update paths write per-user account-picture caches
    // there instead of (or in addition to) ProgramData.
    wchar_t  bestPath[MAX_PATH] = L"";
    FILETIME bestTime           = {};
    const wchar_t* bestSource   = L"none";

    auto consider = [&](LPCWSTR path, const wchar_t* sourceLabel) {
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return;
        if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return;
        if (!bestPath[0] ||
            CompareFileTime(&fad.ftLastWriteTime, &bestTime) > 0) {
            bestTime = fad.ftLastWriteTime;
            bestSource = sourceLabel;
            wcscpy_s(bestPath, MAX_PATH, path);
        }
    };

    // Source: registry (canonical declaration). The resolver itself
    // logs each Image* candidate it considers; we just take its winner.
    wchar_t regPath[MAX_PATH] = L"";
    if (ResolveUserPicturePathFromRegistry(regPath)) {
        consider(regPath, L"registry");
    }

    LPWSTR sidStr = GetCurrentUserSidString();
    if (sidStr) {
        wchar_t dir[MAX_PATH], cand[MAX_PATH];

        // ProgramData\AccountPictures\<SID>\ — newest *anything* in the
        // dir (catches new SourceId-named files the shell adds without
        // updating the Image* values).
        wchar_t base[MAX_PATH];
        if (ExpandEnvironmentStringsW(L"%ProgramData%", base, MAX_PATH))
            swprintf_s(dir, MAX_PATH, L"%s\\AccountPictures\\%s", base, sidStr);
        else
            dir[0] = L'\0';
        if (PickNewestFile(dir, L"*.*", true, cand))
            consider(cand, L"ProgramData\\AccountPictures dir");

        // Public\AccountPictures\<SID>\*Image192* — legacy-style cache.
        if (ExpandEnvironmentStringsW(L"%PUBLIC%", base, MAX_PATH))
            swprintf_s(dir, MAX_PATH, L"%s\\AccountPictures\\%s", base, sidStr);
        else
            dir[0] = L'\0';
        if (PickNewestFile(dir, L"*Image192*", true, cand))
            consider(cand, L"Public\\AccountPictures dir");

        LocalFree(sidStr);
    }

    // %APPDATA%\Microsoft\Windows\AccountPictures and
    // %LOCALAPPDATA%\Microsoft\Windows\AccountPictures — per-user
    // caches for local accounts (some Win10 update paths land here
    // instead of ProgramData).
    wchar_t expanded[MAX_PATH], cand[MAX_PATH];
    DWORD len = ExpandEnvironmentStringsW(
        L"%APPDATA%\\Microsoft\\Windows\\AccountPictures",
        expanded, MAX_PATH);
    if (len > 0 && len <= MAX_PATH &&
        PickNewestFile(expanded, L"*.*", true, cand)) {
        consider(cand, L"AppData\\AccountPictures dir");
    }
    len = ExpandEnvironmentStringsW(
        L"%LOCALAPPDATA%\\Microsoft\\Windows\\AccountPictures",
        expanded, MAX_PATH);
    if (len > 0 && len <= MAX_PATH &&
        PickNewestFile(expanded, L"*.*", true, cand)) {
        consider(cand, L"LocalAppData\\AccountPictures dir");
    }

    // Machine-wide default fallback (user.{png,bmp}).
    if (ExpandEnvironmentStringsW(
            L"%ProgramData%\\Microsoft\\User Account Pictures\\user.png",
            expanded, MAX_PATH))
        consider(expanded, L"default user.png");
    if (ExpandEnvironmentStringsW(
            L"%ProgramData%\\Microsoft\\User Account Pictures\\user.bmp",
            expanded, MAX_PATH))
        consider(expanded, L"default user.bmp");

    if (!bestPath[0]) return false;

    wcscpy_s(outPath, MAX_PATH, bestPath);
    dbgprintf(L"User picture: best across all sources [%s] -> %s mtime=%08X:%08X",
           bestSource, bestPath,
           bestTime.dwHighDateTime, bestTime.dwLowDateTime);
    return true;
}

// Any bitmap source to a premultiplied BGRA bitmap at the paint size
// Caller owns the returned HBITMAP
static HBITMAP WICSourceToDIB(IWICImagingFactory* factory,
                               IWICBitmapSource* source,
                               int w, int h) {
    HBITMAP outBmp = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(factory->CreateFormatConverter(&conv))) {
        if (SUCCEEDED(conv->Initialize(
                source, GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0,
                WICBitmapPaletteTypeCustom))) {
            IWICBitmapScaler* scaler = nullptr;
            if (SUCCEEDED(factory->CreateBitmapScaler(&scaler))) {
                if (SUCCEEDED(scaler->Initialize(
                        conv, w, h,
                        WICBitmapInterpolationModeFant))) {
                    BITMAPINFO bi = {};
                    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
                    bi.bmiHeader.biWidth       = w;
                    bi.bmiHeader.biHeight      = -h;  // top-down
                    bi.bmiHeader.biPlanes      = 1;
                    bi.bmiHeader.biBitCount    = 32;
                    bi.bmiHeader.biCompression = BI_RGB;

                    void* pixels = nullptr;
                    HDC screen = GetDC(nullptr);
                    HBITMAP dib = CreateDIBSection(
                        screen, &bi, DIB_RGB_COLORS,
                        &pixels, nullptr, 0);
                    ReleaseDC(nullptr, screen);

                    if (dib && pixels) {
                        UINT stride = w * 4;
                        UINT bufSize = stride * h;
                        if (SUCCEEDED(scaler->CopyPixels(
                                nullptr, stride, bufSize,
                                static_cast<BYTE*>(pixels)))) {
                            outBmp = dib;
                            dib = nullptr;
                        }
                    }
                    if (dib) DeleteObject(dib);
                }
                scaler->Release();
            }
        }
        conv->Release();
    }
    return outBmp;
}

// Same pipeline, starting from a decoder's first frame
static HBITMAP DecodeWICDecoderToDIB(IWICImagingFactory* factory,
                                      IWICBitmapDecoder* decoder,
                                      int w, int h) {
    HBITMAP outBmp = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    if (SUCCEEDED(decoder->GetFrame(0, &frame))) {
        outBmp = WICSourceToDIB(factory, frame, w, h);
        frame->Release();
    }
    return outBmp;
}

// Decode a memory blob (BMP/PNG/JPG bytes) into a W×H 32bpp PARGB DIB
// section via WIC. Used by both the file-source path and the shell-API
// path (SHGetUserPictureBytes returns BMP bytes which we feed here).
static HBITMAP DecodeUserPictureBytesWIC(const BYTE* bytes, DWORD size,
                                          int w, int h) {
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needUninit = (hrInit == S_OK);

    HBITMAP outBmp = nullptr;
    IWICImagingFactory* factory = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory)))) {
        IWICStream* stream = nullptr;
        if (SUCCEEDED(factory->CreateStream(&stream))) {
            if (SUCCEEDED(stream->InitializeFromMemory(
                    const_cast<BYTE*>(bytes), size))) {
                IWICBitmapDecoder* decoder = nullptr;
                if (SUCCEEDED(factory->CreateDecoderFromStream(
                        stream, nullptr, WICDecodeMetadataCacheOnLoad,
                        &decoder))) {
                    outBmp = DecodeWICDecoderToDIB(factory, decoder, w, h);
                    decoder->Release();
                }
            }
            stream->Release();
        }
        factory->Release();
    }

    if (needUninit) CoUninitialize();
    return outBmp;
}

// shell32!SHGetUserPictureBytes — the canonical Win10 API for fetching
// the current user's account picture. Reads from the SAM property store
// (PKEY_SAM_UserPicture) or, on fallback, from a TLV blob in
// %LOCALAPPDATA%\Microsoft\Windows\AccountPictures\<acct>.dat. This is
// what 7850's CUserPane::_CreateUserPicture wraps, and what
// SHGetUserPicture (the simpler HBITMAP-returning sibling) calls
// internally with format=L"bmp".
//
// Decompilation: exports/shell32/decompilation.c:22-155.
//
//   pszUser:    NULL = current logged-on user (resolved via GetUserNameExW)
//   pszFormat:  L"bmp" / L"jpg" / L"png" — selects which element to
//               extract from the multi-format blob
//   dwFlags:    1 = SAM-then-.dat fallback (the path SHGetUserPicture uses)
//               2 = always default tile
//               3 = SAM only, no fallback
//   ppvBytes:   out — caller LocalFree's
//   pcbBytes:   out — byte count
//   pszFile/cchFile: out optional — source filename
typedef HRESULT (WINAPI *SHGetUserPictureBytes_t)(
    LPCWSTR pszUser, LPCWSTR pszFormat, DWORD dwFlags,
    LPVOID *ppvBytes, DWORD *pcbBytes,
    LPWSTR pszFile, DWORD cchFile);

static SHGetUserPictureBytes_t g_pSHGetUserPictureBytes = nullptr;
static bool                    g_shellApiTried          = false;

static SHGetUserPictureBytes_t LookupShellPictureAPI() {
    if (g_shellApiTried) return g_pSHGetUserPictureBytes;
    g_shellApiTried = true;

    HMODULE shell32 = GetModuleHandleW(L"shell32.dll");
    if (!shell32) shell32 = LoadLibraryW(L"shell32.dll");
    if (!shell32) {
        dbgprintf(L"shell32.dll not loaded — falling back to file-based picture resolver");
        return nullptr;
    }
    g_pSHGetUserPictureBytes = reinterpret_cast<SHGetUserPictureBytes_t>(
        GetProcAddress(shell32, "SHGetUserPictureBytes"));
    if (g_pSHGetUserPictureBytes) {
        dbgprintf(L"shell32!SHGetUserPictureBytes resolved at %p",
               g_pSHGetUserPictureBytes);
    } else {
        dbgprintf(L"shell32!SHGetUserPictureBytes export not found "
               L"(GetLastError=%lu) — falling back to file-based resolver",
               GetLastError());
    }
    return g_pSHGetUserPictureBytes;
}

// Decode the current user's picture via shell32!SHGetUserPictureBytes.
// Returns an HBITMAP at w×h or nullptr if the API isn't available or
// returned no picture. Tries L"bmp" first (what SHGetUserPicture uses
// internally), then L"png" / L"jpg" as a defensive fallback in case
// the .dat blob doesn't carry a BMP element.
static HBITMAP DecodeUserPictureViaShellAPI(int w, int h) {
    auto fn = LookupShellPictureAPI();
    if (!fn) return nullptr;

    static const wchar_t* const kFormats[] = { L"bmp", L"png", L"jpg" };
    for (auto* fmt : kFormats) {
        LPVOID bytes = nullptr;
        DWORD  size  = 0;
        HRESULT hr = fn(nullptr, fmt, 1, &bytes, &size, nullptr, 0);
        if (FAILED(hr) || !bytes || size == 0) {
            if (bytes) LocalFree(bytes);
            continue;
        }
        HBITMAP outBmp = DecodeUserPictureBytesWIC(
            static_cast<BYTE*>(bytes), size, w, h);
        LocalFree(bytes);
        if (outBmp) {
            dbgprintf(L"User picture: shell API returned %lu %s bytes -> "
                   L"%dx%d HBITMAP", size, fmt, w, h);
            return outBmp;
        }
    }
    return nullptr;
}

// Decode an image file from disk to a W×H 32bpp PARGB DIB section.
static HBITMAP DecodeUserPictureWIC(LPCWSTR path, int w, int h) {
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needUninit = (hrInit == S_OK);  // S_FALSE = thread already STA

    HBITMAP outBmp = nullptr;
    IWICImagingFactory* factory = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory)))) {
        IWICBitmapDecoder* decoder = nullptr;
        if (SUCCEEDED(factory->CreateDecoderFromFilename(
                path, nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnLoad, &decoder))) {
            outBmp = DecodeWICDecoderToDIB(factory, decoder, w, h);
            decoder->Release();
        }
        factory->Release();
    }

    if (needUninit) CoUninitialize();
    return outBmp;
}

// shell32!SHGetUserPicturePath (ordinal 261). Older, more widely-available
// API than SHGetUserPictureBytes — present on most Win10 builds where
// the latter has been removed. Returns the path to the user's account
// picture file (the bmp under <COMMON_APPDATA>\Microsoft\User Account
// Pictures\, or the .dat under AccountPictures, depending on flags).
//
// Decompilation: exports/shell32/decompilation.c:196-202 (Ordinal_261)
// → forwards to Ordinal_810 with default flags.
typedef HRESULT (WINAPI *SHGetUserPicturePath_t)(
    LPCWSTR pszUser, DWORD dwFlags,
    LPWSTR pszFile, DWORD cchFile,
    LPWSTR pszPath, DWORD cchPath);

static SHGetUserPicturePath_t g_pSHGetUserPicturePath = nullptr;
static bool                   g_shellApiPathTried     = false;

static SHGetUserPicturePath_t LookupShellPicturePathAPI() {
    if (g_shellApiPathTried) return g_pSHGetUserPicturePath;
    g_shellApiPathTried = true;

    HMODULE shell32 = GetModuleHandleW(L"shell32.dll");
    if (!shell32) shell32 = LoadLibraryW(L"shell32.dll");
    if (!shell32) return nullptr;

    // Try by name first, then by ordinal 261 (the documented value
    // confirmed in the decomp).
    g_pSHGetUserPicturePath = reinterpret_cast<SHGetUserPicturePath_t>(
        GetProcAddress(shell32, "SHGetUserPicturePath"));
    if (!g_pSHGetUserPicturePath) {
        g_pSHGetUserPicturePath = reinterpret_cast<SHGetUserPicturePath_t>(
            GetProcAddress(shell32, MAKEINTRESOURCEA(261)));
    }
    if (g_pSHGetUserPicturePath) {
        dbgprintf(L"shell32!SHGetUserPicturePath resolved at %p",
               g_pSHGetUserPicturePath);
    } else {
        dbgprintf(L"shell32!SHGetUserPicturePath not found by name or ord261 "
               L"(GetLastError=%lu)", GetLastError());
    }
    return g_pSHGetUserPicturePath;
}

// Decode the user picture by asking SHGetUserPicturePath for the
// canonical file path, then WIC-decoding that file. Less precise than
// SHGetUserPictureBytes (we depend on SHGetUserPicturePath returning
// the *current* file, not a stale cache) but available on more Win10
// builds.
static HBITMAP DecodeUserPictureViaShellPath(int w, int h) {
    auto fn = LookupShellPicturePathAPI();
    if (!fn) return nullptr;
    wchar_t pathBuf[MAX_PATH] = L"";
    HRESULT hr = fn(nullptr, 0, pathBuf, MAX_PATH, nullptr, 0);
    if (FAILED(hr) || !pathBuf[0]) {
        dbgprintf(L"SHGetUserPicturePath failed hr=0x%08X", hr);
        return nullptr;
    }
    dbgprintf(L"SHGetUserPicturePath -> %s", pathBuf);
    HBITMAP outBmp = DecodeUserPictureWIC(pathBuf, w, h);
    if (outBmp) {
        dbgprintf(L"User picture: SHGetUserPicturePath path decoded -> "
               L"%dx%d HBITMAP", w, h);
    }
    return outBmp;
}

// Parse a shell32 _USER_PICTURE_ELEMENTS TLV blob and return the bytes
// of the LAST type-1 (data) element. This re-implements what shell32's
// internal _GetPicture does: the .dat file holds multiple format groups
// (BMP/PNG/JPG) as TLV elements, each {type=0 format-string, type=1 data,
// type=2 extra}. The last data element is what the shell returns for
// the requested format.
//
// Format (from exports/shell32/decompilation.c:505-619 _GetPicture):
//   offset 0:  uint32 version (must be 1)
//   offset 4:  uint32 element_count
//   offset 8:  first element
//     +0:  uint32 type (0=format string, 1=data, 2=extra)
//     +4:  uint32 size
//     +8:  size bytes payload, padded to 4-byte alignment
static bool ExtractDatLastDataElement(const BYTE* buf, DWORD bufSize,
                                       const BYTE** outBytes,
                                       DWORD* outSize) {
    if (bufSize < 12) return false;
    uint32_t version = *reinterpret_cast<const uint32_t*>(buf);
    uint32_t count   = *reinterpret_cast<const uint32_t*>(buf + 4);
    if (version != 1 || count == 0) return false;

    const BYTE* bestData = nullptr;
    DWORD bestSize = 0;
    DWORD off = 8;

    for (uint32_t i = 0; i < count; ++i) {
        if (off + 8 > bufSize) break;
        uint32_t type = *reinterpret_cast<const uint32_t*>(buf + off);
        uint32_t size = *reinterpret_cast<const uint32_t*>(buf + off + 4);
        if (off + 8 > bufSize || size > bufSize - (off + 8)) break;
        if (type == 1) {
            // Last-wins matches shell32's behavior: _GetPicture overwrites
            // its candidate vars on each iteration and uses whichever
            // type-1 was seen last paired with the matching type-0.
            bestData = buf + off + 8;
            bestSize = size;
        }
        // Advance past this element's header + payload, aligned to 4.
        DWORD next = off + 8 + size;
        next = (next + 3) & ~3u;
        if (next <= off) break;  // safety: no progress / overflow
        off = next;
    }

    if (!bestData) return false;
    *outBytes = bestData;
    *outSize  = bestSize;
    return true;
}

// Locate the most recent user-picture .dat file across the well-known
// AppData locations. The shell's SHGetPictureFromDataFileForUser
// (exports/shell32/decompilation.c:361) reads <picture-path>\<acct>.dat
// where <picture-path> comes from SHAcctGetUserPicturePath; in practice
// this lands under %APPDATA% or %LOCALAPPDATA%\Microsoft\Windows\
// AccountPictures\.
static bool FindUserPictureDatFile(wchar_t outPath[MAX_PATH]) {
    wchar_t bestPath[MAX_PATH] = L"";
    FILETIME bestTime = {};

    auto considerDir = [&](LPCWSTR dir) {
        wchar_t search[MAX_PATH];
        swprintf_s(search, MAX_PATH, L"%s\\*.dat", dir);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(search, &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (CompareFileTime(&fd.ftLastWriteTime, &bestTime) > 0) {
                bestTime = fd.ftLastWriteTime;
                swprintf_s(bestPath, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    };

    wchar_t expanded[MAX_PATH];
    if (ExpandEnvironmentStringsW(
            L"%LOCALAPPDATA%\\Microsoft\\Windows\\AccountPictures",
            expanded, MAX_PATH) > 0) {
        considerDir(expanded);
    }
    if (ExpandEnvironmentStringsW(
            L"%APPDATA%\\Microsoft\\Windows\\AccountPictures",
            expanded, MAX_PATH) > 0) {
        considerDir(expanded);
    }

    if (!bestPath[0]) return false;
    wcscpy_s(outPath, MAX_PATH, bestPath);
    dbgprintf(L"User picture .dat: %s mtime=%08X:%08X", bestPath,
           bestTime.dwHighDateTime, bestTime.dwLowDateTime);
    return true;
}

// Read the .dat file ourselves and decode the last-encoded data element
// — direct re-implementation of shell32's SHGetPictureFromDataFileForUser
// + _GetPicture pipeline. This is what `_UpdateUserInfo` ultimately
// reads: the .dat is the master that always reflects the current
// picture, even when the Image*.jpg thumbnails lag behind on subsequent
// changes.
static HBITMAP DecodeUserPictureFromDat(int w, int h) {
    wchar_t datPath[MAX_PATH] = L"";
    if (!FindUserPictureDatFile(datPath)) return nullptr;

    HANDLE hFile = CreateFileW(datPath, GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        dbgprintf(L"User picture .dat: open failed (LE=%lu)", GetLastError());
        return nullptr;
    }

    LARGE_INTEGER fsize;
    if (!GetFileSizeEx(hFile, &fsize) || fsize.QuadPart == 0 ||
        fsize.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(hFile);
        return nullptr;
    }

    DWORD size = static_cast<DWORD>(fsize.QuadPart);
    BYTE* buf = static_cast<BYTE*>(LocalAlloc(LPTR, size));
    if (!buf) {
        CloseHandle(hFile);
        return nullptr;
    }

    DWORD got = 0;
    BOOL ok = ReadFile(hFile, buf, size, &got, nullptr);
    CloseHandle(hFile);

    HBITMAP outBmp = nullptr;
    if (ok && got == size) {
        const BYTE* dataPtr = nullptr;
        DWORD dataSize = 0;
        if (ExtractDatLastDataElement(buf, size, &dataPtr, &dataSize)) {
            outBmp = DecodeUserPictureBytesWIC(dataPtr, dataSize, w, h);
            if (outBmp) {
                dbgprintf(L"User picture .dat parsed: %lu data bytes -> "
                       L"%dx%d HBITMAP", dataSize, w, h);
            } else {
                dbgprintf(L"User picture .dat: WIC decode failed "
                       L"(%lu data bytes)", dataSize);
            }
        } else {
            dbgprintf(L"User picture .dat: TLV parse failed "
                   L"(file %lu bytes, may be wrong format)", size);
        }
    } else {
        dbgprintf(L"User picture .dat: ReadFile failed (got %lu / %lu)", got, size);
    }
    LocalFree(buf);
    return outBmp;
}

// Tracks whether g_userPictureBmp reflects the current shell-known
// picture. Cleared by hooked_UpdateUserInfo (the dirty-flag trigger
// that fires every time the shell's picture pipeline updates), so the
// next RefreshUserPicture call re-fetches via the shell API.
static bool g_userPictureCacheValid = false;

// Idempotent loader / reloader. Tries multiple sources in descending
// order of "freshness reliability":
//
//   1. shell32!SHGetUserPictureBytes — best (canonical, reads SAM/.dat).
//      Often unavailable on Win10 LTSC (export removed in some builds).
//   2. Direct .dat parsing — re-implements SHGetPictureFromDataFileForUser
//      + _GetPicture from exports/shell32/decompilation.c. The .dat blob
//      is the *master* the shell rewrites on every picture change; the
//      Image*.jpg thumbnails are derived caches that lag behind on the
//      2nd+ change (the bug we're working around).
//   3. shell32!SHGetUserPicturePath (ordinal 261) — older API. Returns
//      a path, we WIC-decode that file.
//   4. File-based fallback — scans registry Image* values + AccountPictures
//      directories. Last resort; suffers from thumbnail-lag.
//
// Cached HBITMAP stays valid until hooked_UpdateUserInfo invalidates it.
static void RefreshUserPicture() {
    if (g_userPictureCacheValid && g_userPictureBmp) {
        return;  // up-to-date; only invalidated by hooked_UpdateUserInfo
    }

    // Tier 1: shell32!SHGetUserPictureBytes
    HBITMAP shellBmp = DecodeUserPictureViaShellAPI(USERPIC_W, USERPIC_H);
    if (shellBmp) {
        if (g_userPictureBmp) DeleteObject(g_userPictureBmp);
        g_userPictureBmp = shellBmp;
        g_userPictureCacheValid = true;
        g_userPictureSrcPath[0] = L'\0';
        g_userPictureSrcMtime = {};
        return;
    }

    // Tier 2: direct .dat parse — the master the shell rewrites every
    // picture change. This works when SHGetUserPictureBytes export is
    // missing (Win10 LTSC) AND the Image*.jpg thumbnails lag behind.
    shellBmp = DecodeUserPictureFromDat(USERPIC_W, USERPIC_H);
    if (shellBmp) {
        if (g_userPictureBmp) DeleteObject(g_userPictureBmp);
        g_userPictureBmp = shellBmp;
        g_userPictureCacheValid = true;
        g_userPictureSrcPath[0] = L'\0';
        g_userPictureSrcMtime = {};
        return;
    }

    // Tier 3: SHGetUserPicturePath → WIC-decode the returned path.
    shellBmp = DecodeUserPictureViaShellPath(USERPIC_W, USERPIC_H);
    if (shellBmp) {
        if (g_userPictureBmp) DeleteObject(g_userPictureBmp);
        g_userPictureBmp = shellBmp;
        g_userPictureCacheValid = true;
        g_userPictureSrcPath[0] = L'\0';
        g_userPictureSrcMtime = {};
        return;
    }

    // Fallback: file-based resolver.
    wchar_t path[MAX_PATH] = L"";
    bool resolved = ResolveUserPicturePath(path);

    FILETIME mtime = {};
    if (resolved) {
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
            mtime = fad.ftLastWriteTime;
        }
    }

    bool samePath  = (wcscmp(path, g_userPictureSrcPath) == 0);
    bool sameMtime = (CompareFileTime(&mtime, &g_userPictureSrcMtime) == 0);

    if (!resolved) {
        if (g_userPictureBmp) {
            DeleteObject(g_userPictureBmp);
            g_userPictureBmp = nullptr;
            g_userPictureSrcPath[0] = L'\0';
            g_userPictureSrcMtime = {};
            dbgprintf(L"User picture: source disappeared (file fallback), cleared cache");
        }
        return;
    }

    if (samePath && sameMtime && g_userPictureBmp) {
        dbgprintf(L"User picture (file fallback): cached (path+mtime unchanged): %s "
               L"mtime=%08X:%08X", path,
               mtime.dwHighDateTime, mtime.dwLowDateTime);
        g_userPictureCacheValid = true;
        return;
    }

    HBITMAP bmp = DecodeUserPictureWIC(path, USERPIC_W, USERPIC_H);
    if (!bmp) {
        dbgprintf(L"User picture (file fallback): WIC decode failed for %s", path);
        return;
    }

    if (g_userPictureBmp) DeleteObject(g_userPictureBmp);
    g_userPictureBmp = bmp;
    wcscpy_s(g_userPictureSrcPath, MAX_PATH, path);
    g_userPictureSrcMtime = mtime;
    g_userPictureCacheValid = true;
    dbgprintf(L"User picture (file fallback) %s: %s mtime=%08X:%08X",
           samePath ? L"updated" : L"loaded", path,
           mtime.dwHighDateTime, mtime.dwLowDateTime);
}

// Frame size in compact mode, the normal-mode one is 155
constexpr int AERO_SMALL_SIZE = 64;

// Nudge the compact picture off the frame centre, up and left
constexpr int kCompactPicNudgeX = -2;
constexpr int kCompactPicNudgeY = -2;

// Pixels the compact label block rides above the picture midpoint
constexpr int kCompactLabelLift = 2;

// Picture-inside-frame shrink, expressed as a percentage of the frame's
// display size. The aero frame asset's bevel ring + glass overlay
// occupies the outer pixels; a centered picture smaller than the frame
// keeps the photo content inside the bevel rather than letting the
// overlay clip across the photo edges. Tune these to taste — smaller
// percentage = picture grows toward the frame edge, larger = more inset.
//
//   compact: 64 px frame, ~10 px shrink ≈ 15.625%  →  picture 54×54
//   normal:  104 px frame, 25% shrink              →  picture 78×78
static float kCompactPicShrinkPct = 25.0f;
static float kNormalPicShrinkPct  = 33.0f;

// The aero-frame PNG ships with a soft drop-shadow baked into its
// alpha channel: pixels around the bevel ring are pure black with
// falloff alpha. Out of the box that shadow reads as too dark; this
// scale is applied to those pixels' alpha after WIC decode.
//
// Detection is cheap and safe in PBGRA: a "shadow" pixel has RGB == 0
// (premultiplied black). Bevel/glass pixels carry a non-zero color
// component, so they're left untouched — the bevel outlines stay at
// the asset's original strength. Tunable: 1.0 = original, 0.5 = half
// as dark, 0.0 = no shadow.
static float kAeroShadowAlphaScale = 0.5f;

// Walk the cached big-frame DIB's pixel buffer and reduce alpha on
// shadow-only pixels (RGB == 0, A > 0). Called once after LoadAeroFrame
// populates g_aeroFrameBmp at AERO_W × AERO_H.
//
// To avoid creating a visible step at the bevel/shadow boundary, the
// scale is *ramped* with a smoothstep curve over kRampRadius pixels.
// The ramp uses t = (d-1) / (kRampRadius-1) so:
//
//   - distance 1 (touching bevel): t=0, smoothstep=0, scale=1.0
//     → pixel kept at full original alpha. The bevel (un-touched at
//       100% alpha) and its immediate shadow neighbor are now both
//       at 100%, eliminating the boundary step entirely.
//   - distance kRampRadius:        t=1, smoothstep=1, scale=target
//     → full lightening applied.
//   - in between: smoothstep (3t² - 2t³) — derivative is zero at both
//     ends, so the ease-in at the bevel boundary AND the ease-out where
//     the ramp meets the flat-scale interior region are both
//     imperceptible. A linear ramp at d=1 still drops alpha by
//     (1-target)/kRampRadius right next to the bevel, which reads as
//     a thin halo; smoothstep@d=1 is 0% drop.
static void LightenAeroFrameShadow() {
    if (!g_aeroFrameBmp) return;
    if (kAeroShadowAlphaScale >= 1.0f - 1e-6f) return;  // no-op

    BITMAP bm = {};
    if (!GetObject(g_aeroFrameBmp, sizeof(bm), &bm)) return;
    if (!bm.bmBits || bm.bmBitsPixel != 32) {
        dbgprintf(L"LightenAeroFrameShadow: not a 32bpp DIB section, skipping");
        return;
    }

    int   w     = bm.bmWidth;
    int   h     = bm.bmHeight;       // BITMAP.bmHeight is always positive
    LONG  pitch = bm.bmWidthBytes;   // already row-stride in bytes
    BYTE* px    = static_cast<BYTE*>(bm.bmBits);

    constexpr int kRampRadius = 6;   // px buffer of smoothstepped alpha around bevel
    int touched = 0;

    // RGB > 0 in PBGRA means "this pixel carries bevel/glass color, not
    // pure shadow". Anti-aliased outline pixels (faded bevel near the
    // edge) carry small but non-zero RGB and are correctly classified
    // here as bevel — so they're never written.
    auto isBevel = [](BYTE r, BYTE g, BYTE b) -> bool {
        return r > 0 || g > 0 || b > 0;
    };

    for (int y = 0; y < h; ++y) {
        BYTE* row = px + y * pitch;
        for (int x = 0; x < w; ++x) {
            BYTE b = row[x*4+0], g = row[x*4+1], r = row[x*4+2], a = row[x*4+3];
            if (a == 0 || isBevel(r, g, b)) continue;  // skip transparent + bevel

            // Find Chebyshev distance to nearest bevel pixel within
            // kRampRadius. Stop early if we hit distance 1 (closest
            // possible non-self).
            int closest = kRampRadius + 1;
            int x0 = (x - kRampRadius > 0)     ? x - kRampRadius : 0;
            int x1 = (x + kRampRadius < w)     ? x + kRampRadius : w - 1;
            int y0 = (y - kRampRadius > 0)     ? y - kRampRadius : 0;
            int y1 = (y + kRampRadius < h)     ? y + kRampRadius : h - 1;
            for (int ny = y0; ny <= y1 && closest > 1; ++ny) {
                const BYTE* nrow = px + ny * pitch;
                for (int nx = x0; nx <= x1; ++nx) {
                    BYTE nb = nrow[nx*4+0], ng = nrow[nx*4+1],
                         nr = nrow[nx*4+2];
                    if (!isBevel(nr, ng, nb)) continue;
                    int dx = (nx > x) ? (nx - x) : (x - nx);
                    int dy = (ny > y) ? (ny - y) : (y - ny);
                    int d  = (dx > dy) ? dx : dy;
                    if (d > 0 && d < closest) closest = d;
                }
            }

            float scale;
            if (closest > kRampRadius) {
                scale = kAeroShadowAlphaScale;            // far from bevel
            } else {
                // Smoothstep ramp: closest=1 → t=0 → scale=1.0 (fully
                // preserved, matches un-touched bevel),
                // closest=kRampRadius → t=1 → scale=target.
                // Derivative-zero at both endpoints removes the
                // perceptual edge a linear ramp leaves at d=1.
                float t = static_cast<float>(closest - 1) /
                          static_cast<float>(kRampRadius - 1);
                float s = t * t * (3.0f - 2.0f * t);  // smoothstep(t)
                scale = 1.0f - s * (1.0f - kAeroShadowAlphaScale);
            }
            row[x*4+3] = static_cast<BYTE>(a * scale + 0.5f);
            ++touched;
        }
    }
}

//---Frame border resources---------------------------------

// Both frame borders live in wrp64.dll as plain BITMAP resources
// Swap them in Resource Hacker to theme the user tile, see notes below

// Hand back the raw DIB bytes of one of our own BITMAP resources
// The compiler strips the file header, so this starts at the info header
static const BYTE* LockFrameResource(int resId, DWORD* outSize) {
    HRSRC found = FindResourceW(g_hInstance, MAKEINTRESOURCEW(resId),
                                RT_BITMAP);
    if (!found) return nullptr;
    HGLOBAL loaded = LoadResource(g_hInstance, found);
    if (!loaded) return nullptr;
    const BYTE* bytes = static_cast<const BYTE*>(LockResource(loaded));
    if (!bytes) return nullptr;
    *outSize = SizeofResource(g_hInstance, found);
    return bytes;
}

// Read one frame border resource and scale it to the size we paint at
// Hand-walks the DIB to keep alpha, see notes/usertile-frame-resources.md
static HBITMAP LoadFrameBitmap(int resId, int w, int h) {
    DWORD resSize = 0;
    const BYTE* res = LockFrameResource(resId, &resSize);
    if (!res || resSize < sizeof(BITMAPINFOHEADER)) {
        dbgprintf(L"Frame bitmap %d: resource missing or truncated", resId);
        return nullptr;
    }

    BITMAPINFOHEADER bih = {};
    memcpy(&bih, res, sizeof(bih));

    bool topDown = (bih.biHeight < 0);
    int  srcW    = bih.biWidth;
    int  srcH    = topDown ? -bih.biHeight : bih.biHeight;

    if (srcW <= 0 || srcH <= 0 ||
        (bih.biBitCount != 32 && bih.biBitCount != 24) ||
        (bih.biCompression != BI_RGB && bih.biCompression != BI_BITFIELDS)) {
        dbgprintf(L"Frame bitmap %d: want an uncompressed 24 or 32 bit BMP, "
                  L"got %ux%u at %u bpp compression %u",
                  resId, bih.biWidth, bih.biHeight,
                  bih.biBitCount, bih.biCompression);
        return nullptr;
    }

    // Pixels sit after the header, the colour masks and any palette
    DWORD pixOff = bih.biSize;
    if (bih.biCompression == BI_BITFIELDS) pixOff += 3 * sizeof(DWORD);
    pixOff += bih.biClrUsed * sizeof(RGBQUAD);

    DWORD srcBpp    = bih.biBitCount / 8;
    DWORD srcStride = ((DWORD)srcW * srcBpp + 3) & ~3u;
    DWORD dstStride = (DWORD)srcW * 4;
    if (pixOff > resSize ||
        srcStride * (DWORD)srcH > resSize - pixOff) {
        dbgprintf(L"Frame bitmap %d: pixel data runs past the resource", resId);
        return nullptr;
    }

    BYTE* buf = static_cast<BYTE*>(
        HeapAlloc(GetProcessHeap(), 0, dstStride * (DWORD)srcH));
    if (!buf) return nullptr;

    // Flip to top-down and widen 24bpp rows out to BGRA on the way
    bool anyAlpha = false;
    for (int y = 0; y < srcH; ++y) {
        int srcRow = topDown ? y : (srcH - 1 - y);
        const BYTE* s = res + pixOff + (DWORD)srcRow * srcStride;
        BYTE* d = buf + (DWORD)y * dstStride;
        for (int x = 0; x < srcW; ++x, s += srcBpp, d += 4) {
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = (srcBpp == 4) ? s[3] : 255;
            if (d[3]) anyAlpha = true;
        }
    }

    // A 32bpp BMP with the alpha bytes left at zero means no alpha, not
    // fully invisible, so treat that case as solid
    if (!anyAlpha) {
        for (DWORD i = 3; i < dstStride * (DWORD)srcH; i += 4) buf[i] = 255;
    }

    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needUninit = (hrInit == S_OK);

    HBITMAP outBmp = nullptr;
    IWICImagingFactory* factory = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory)))) {
        IWICBitmap* source = nullptr;
        if (SUCCEEDED(factory->CreateBitmapFromMemory(
                (UINT)srcW, (UINT)srcH, GUID_WICPixelFormat32bppBGRA,
                dstStride, dstStride * (DWORD)srcH, buf, &source))) {
            outBmp = WICSourceToDIB(factory, source, w, h);
            source->Release();
        }
        factory->Release();
    }

    if (needUninit) CoUninitialize();
    HeapFree(GetProcessHeap(), 0, buf);

    if (!outBmp) {
        dbgprintf(L"Frame bitmap %d: WIC conversion failed", resId);
        return nullptr;
    }
    dbgprintf(L"Frame bitmap %d loaded: %dx%d source scaled to %dx%d",
              resId, srcW, srcH, w, h);
    return outBmp;
}

// Normal-mode frame border, cached at the size WM_PAINT blits it
static void LoadAeroFrame() {
    if (g_aeroFrameTried) return;
    g_aeroFrameTried = true;

    g_aeroFrameBmp = LoadFrameBitmap(IDB_AEROFRAME, AERO_W, AERO_H);
    if (g_aeroFrameBmp) LightenAeroFrameShadow();
}

// Compact-mode frame border, same path as the normal-mode one
static void LoadAeroFrameSmall() {
    if (g_aeroFrameSmallTried) return;
    g_aeroFrameSmallTried = true;

    g_aeroFrameSmallBmp = LoadFrameBitmap(IDB_AEROFRAME_SMALL,
                                          AERO_W, AERO_H);
}

// Captured at WM_MOUSEACTIVATE on the tile (which fires *before* the
// activation cycle deactivates and hides the flyout). Used by
// WM_LBUTTONDOWN to decide whether the click is a toggle-close (don't
// reopen) or a fresh open. Reset to false at end of WM_LBUTTONDOWN.
static bool g_flyoutWasVisibleAtMouseDown = false;

// Tracks whether the flyout is currently displayed on-screen. The window
// itself is always WS_VISIBLE (positioned far off-screen when "hidden")
// so that DWM keeps a presentation surface and ShowFlyoutForTile is a
// fast SetWindowPos move with no first-paint gap. We can't rely on
// IsWindowVisible since it's always TRUE; this flag is the source of
// truth for "is the user currently looking at the flyout".
static bool g_flyoutShown = false;

// Hide the flyout by making it fully transparent (alpha 0) via the layered-
// window mechanism. The window stays at its current on-screen position so
// DWM keeps composing it — this is what eliminates the first-show flash.
// (Naming kept as "Offscreen" to minimize churn against existing call sites,
//  even though the window is no longer literally moved off-screen.)
static void HideFlyoutOffscreen(HWND hwnd) {
    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
    g_flyoutShown = false;
}

// DWM drop-shadow extents around the visible window frame, measured once
// at flyout creation. On Win10 with DWM composition + WS_BORDER + DWMWA_
// NCRENDERING_POLICY=ENABLED, the shadow extends ~7px outside GetWindowRect
// on every side. We shift the SetWindowPos position inward by these
// amounts so the *visible-with-shadow* bounds (DWMWA_EXTENDED_FRAME_BOUNDS)
// honor FLYOUT_MARGIN from the work-area edges, instead of the logical
// window rect honoring it (which leaves the shadow flush with the edges).
static int g_shadowLeft   = 0;
static int g_shadowTop    = 0;
static int g_shadowRight  = 0;
static int g_shadowBottom = 0;

// -----------------------------------------------------------------
// Flyout content state (Phase 2) — username, admin/standard, password.
// Refreshed every time the flyout is shown, drawn in WM_PAINT.
// Logic mirrors the .NET TaskPopup helpers (UserType.cs, UserPassword.cs).
// -----------------------------------------------------------------
static WCHAR g_userName[256]  = L"User";
static bool  g_isAdmin        = false;
static bool  g_isUser         = false;  // member of BUILTIN\Users
static bool  g_isGuest        = false;
static bool  g_hasPassword    = true;   // conservative default

static HFONT g_fontRegular           = nullptr;  // Segoe UI 9pt
static HFONT g_fontRegularUnderline  = nullptr;  // Segoe UI 9pt underlined (link hover)
static HFONT g_fontHeader            = nullptr;  // Segoe UI 12pt — username + header label
static HFONT g_fontHeaderUnderline   = nullptr;  // Segoe UI 12pt underlined (username hover)

// Flyout window dimensions. FLYOUT_H drops by 110 px in compact mode
// (the slot occupied by the larger frame + header). FOOTER_TOP and the
// button Y positions are derived from FLYOUT_H, so they all shift up
// together — re-evaluated by RecomputeLayout whenever compactMode flips.
static const wchar_t* FLYOUT_CLASS_NAME = L"WH_UserTileFlyout";
static int FLYOUT_W       = 276;        // 290 - 14 (left edge pulled inward; bottom-right anchored)
static int FLYOUT_H       = 336;        // 350 - 14 (top edge pulled inward); compact: 226
static const int FLYOUT_MARGIN  = 7;    // gap from work-area edges (matches .NET)
static const int FOOTER_H       = 43;   // bottom panel height (Phase 4)
static int FOOTER_TOP     = 336 - 43;   // recomputed from FLYOUT_H

// Phase 4 — "Open User Accounts" link in the footer panel.
#define LINK_TEXT (UT_Text(UTS_OPEN_USER_ACCT))
static bool g_linkHovered  = false;
static bool g_linkTracking = false;  // also gates username TrackMouseEvent — single tracker per popup hwnd

// Username hover state — clicking opens %USERPROFILE% in Explorer.
// Underline + hover-link color appear only while the cursor is over the
// rendered text; rect computed from GetTextExtentPoint32W on g_userName.
static bool g_userNameHovered = false;

static void RefreshUserName() {
    // The "display name" Windows shows in Settings → Accounts → Your
    // info / on the lock screen lives in different stores depending on
    // account type:
    //
    //   - Local account: SAM "Full name" field (set via lusrmgr.msc or
    //     the rename flow in Settings). NetUserGetInfo level 2's
    //     usri2_full_name reads this. GetUserNameExW(NameDisplay)
    //     typically returns ERROR_NONE_MAPPED or echoes the SAM login
    //     id here — it doesn't see the Full name field.
    //   - Domain / Microsoft-account-linked: the directory's
    //     displayName. GetUserNameExW(NameDisplay) is authoritative.
    //
    // Try the local SAM lookup first because that's where the display
    // name lives for the common (local-account) case. Fall through to
    // NameDisplay for domain/MSA, then to the SAM account name as the
    // last resort.
    g_userName[0] = L'\0';

    wchar_t samName[256] = L"";
    DWORD samLen = ARRAYSIZE(samName);
    if (!GetUserNameW(samName, &samLen)) {
        wcscpy_s(g_userName, ARRAYSIZE(g_userName), L"User");
        return;
    }

    // Tier 1 — local SAM Full name.
    USER_INFO_2* info = nullptr;
    NET_API_STATUS status = NetUserGetInfo(
        nullptr, samName, 2, reinterpret_cast<LPBYTE*>(&info));
    if (status == NERR_Success && info) {
        if (info->usri2_full_name && info->usri2_full_name[0]) {
            wcscpy_s(g_userName, ARRAYSIZE(g_userName),
                     info->usri2_full_name);
            NetApiBufferFree(info);
            return;
        }
        NetApiBufferFree(info);
    }

    // Tier 2 — domain/MSA display name.
    ULONG dispLen = ARRAYSIZE(g_userName);
    if (GetUserNameExW(NameDisplay, g_userName, &dispLen) &&
        g_userName[0] != L'\0') {
        return;
    }

    // Last resort: the SAM login id.
    wcscpy_s(g_userName, ARRAYSIZE(g_userName), samName);
}

// Localized names for BUILTIN\Administrators, BUILTIN\Users, and
// BUILTIN\Guests — resolved once via LookupAccountSidW against the
// well-known SIDs, then used for case-insensitive name comparison
// against NetUserGetLocalGroups results.
//
// This mirrors usercpl2.dll's CLogonUser::_GetAccountType (Ghidra
// decomp exports/usercpl2.raw:64064-64257), which keeps a static table
// of {well-known-SID-template, localized-name-ptr} tuples populated at
// DllMain init time and compares via lstrcmpiW. Sidesteps a quirk we
// observed where LookupAccountNameW + EqualSid (the symmetric reverse
// direction) fails on certain restricted sessions even though the
// group name string is in hand from NetUserGetLocalGroups — those
// sessions still let LookupAccountSidW resolve well-known SIDs to
// their localized names, so caching once and string-comparing later
// is the more reliable path.
static wchar_t g_adminGroupName[256] = L"";
static wchar_t g_usersGroupName[256] = L"";
static wchar_t g_guestGroupName[256] = L"";
static bool    g_groupNamesResolved  = false;

static void ResolveLocalizedGroupName(DWORD rid, wchar_t* out, size_t outCch,
                                       const wchar_t* fallback) {
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    PSID sid = nullptr;
    if (!AllocateAndInitializeSid(&ntAuth, 2,
            SECURITY_BUILTIN_DOMAIN_RID, rid,
            0, 0, 0, 0, 0, 0, &sid)) {
        wcscpy_s(out, outCch, fallback);
        return;
    }
    wchar_t nameBuf[256] = L"";
    DWORD nameLen = ARRAYSIZE(nameBuf);
    wchar_t domainBuf[256] = L"";
    DWORD domainLen = ARRAYSIZE(domainBuf);
    SID_NAME_USE use;
    if (LookupAccountSidW(nullptr, sid, nameBuf, &nameLen,
                           domainBuf, &domainLen, &use) &&
        nameBuf[0] != L'\0') {
        wcscpy_s(out, outCch, nameBuf);
    } else {
        wcscpy_s(out, outCch, fallback);
    }
    FreeSid(sid);
}

static void EnsureLocalizedGroupNames() {
    if (g_groupNamesResolved) return;
    g_groupNamesResolved = true;
    ResolveLocalizedGroupName(DOMAIN_ALIAS_RID_ADMINS,
                              g_adminGroupName, ARRAYSIZE(g_adminGroupName),
                              L"Administrators");
    ResolveLocalizedGroupName(DOMAIN_ALIAS_RID_USERS,
                              g_usersGroupName, ARRAYSIZE(g_usersGroupName),
                              L"Users");
    ResolveLocalizedGroupName(DOMAIN_ALIAS_RID_GUESTS,
                              g_guestGroupName, ARRAYSIZE(g_guestGroupName),
                              L"Guests");
    dbgprintf(L"Localized group names: admin='%s' users='%s' guest='%s'",
           g_adminGroupName, g_usersGroupName, g_guestGroupName);
}

// Detect Administrators / Users / Guests membership in a single SAM
// query — directly enumerates the user's "Member of" list (the same
// list lusrmgr.msc shows in the user-properties dialog).
//
// Primary path mirrors usercpl2.dll's CLogonUser::_GetAccountType
// (exports/usercpl2.raw:64064-64257): NetUserGetLocalGroups with
// LG_INCLUDE_INDIRECT enumerates direct + nested membership, then each
// returned group name is compared via lstrcmpiW against the cached
// localized BUILTIN\Administrators / BUILTIN\Users / BUILTIN\Guests
// names. Locale-portable (cache uses LookupAccountSidW which returns
// whatever the active language pack calls those groups) and dodges
// LookupAccountNameW permission quirks on restricted sessions.
//
// Fallback path: when NetUserGetLocalGroups fails (e.g. domain user
// whose DC isn't reachable), drop back to CheckTokenMembership against
// the current process's linked token. That trades direct SAM accuracy
// for runtime-token-effective groups, which is the right behavior for
// domain accounts.
//
// Label rule at the WM_PAINT call site:
//   Administrator  if g_isAdmin
//   Standard user  if g_isUser (and not admin)
//   Guest account  otherwise — explicit Guests membership OR a stripped
//                  account that's been removed from Users
static void RefreshUserGroupMembership() {
    g_isAdmin = false;
    g_isUser  = false;
    g_isGuest = false;

    EnsureLocalizedGroupNames();

    bool samResolved = false;
    wchar_t samName[256] = L"";
    DWORD samLen = ARRAYSIZE(samName);
    if (GetUserNameW(samName, &samLen)) {
        LOCALGROUP_USERS_INFO_0* groups = nullptr;
        DWORD entriesRead = 0, totalEntries = 0;
        NET_API_STATUS status = NetUserGetLocalGroups(
            nullptr, samName, 0, LG_INCLUDE_INDIRECT,
            reinterpret_cast<LPBYTE*>(&groups),
            MAX_PREFERRED_LENGTH, &entriesRead, &totalEntries);
        dbgprintf(L"NetUserGetLocalGroups('%s'): status=%lu entriesRead=%lu "
               L"totalEntries=%lu groups=%p",
               samName, status, entriesRead, totalEntries, groups);
        // Status alone is authoritative — NetUserGetLocalGroups can
        // legitimately return NERR_Success with `groups == nullptr`
        // when the user is in zero groups (no buffer needed). Don't
        // gate samResolved on the buffer pointer, or we'll fall
        // through to the token fallback and pick up a stale
        // Administrators SID from when the process started, mislabeling
        // a stripped account as "Administrator".
        if (status == NERR_Success) {
            samResolved = true;
            if (groups) {
                for (DWORD i = 0; i < entriesRead; ++i) {
                    const wchar_t* gname = groups[i].lgrui0_name;
                    if (!gname) continue;
                    bool mAdmin = (lstrcmpiW(gname, g_adminGroupName) == 0);
                    bool mUser  = (lstrcmpiW(gname, g_usersGroupName) == 0);
                    bool mGuest = (lstrcmpiW(gname, g_guestGroupName) == 0);
                    if (mAdmin) g_isAdmin = true;
                    if (mUser)  g_isUser  = true;
                    if (mGuest) g_isGuest = true;
                    dbgprintf(L"  group[%lu] = '%s' admin=%d user=%d guest=%d",
                           i, gname,
                           mAdmin ? 1 : 0, mUser ? 1 : 0, mGuest ? 1 : 0);
                }
                NetApiBufferFree(groups);
            }
        }
    }

    // Token-groups enumeration — covers cases SAM can't or won't show:
    //
    //   1. Guest+Admin. The user IS in BUILTIN\Administrators, but
    //      because they're also in BUILTIN\Guests the SAM ACL on
    //      "Administrators" denies the user themselves read access
    //      to the membership list — NetUserGetLocalGroups silently
    //      returns the Guests entry only. usercpl2.dll dodges this
    //      because its "Change account type" page requires a UAC
    //      shield and runs elevated; we don't have that luxury here
    //      (running inline in Guest's own explorer).
    //
    //      LSA still tracks the membership: at logon it puts the
    //      Admin SID in the user's token with the attribute
    //      SE_GROUP_USE_FOR_DENY_ONLY set ("present but demoted by
    //      restricted-user policy"). Iterating TokenGroups and
    //      flagging on DENY_ONLY Admin captures this case without
    //      elevation. CheckTokenMembership() can't be used here —
    //      it deliberately reports DENY_ONLY groups as "not a member"
    //      because they fail access checks.
    //
    //   2. Domain account where NetUserGetLocalGroups can't reach a
    //      DC (status != NERR_Success). No SAM ground truth at all,
    //      so flag from token unconditionally for this branch.
    //
    // Critical guard: when SAM resolved successfully (samResolved =
    // true), we DON'T flag from ENABLED token entries. A user stripped
    // from Administrators after logon will still have Admin in their
    // token as ENABLED (token doesn't refresh in-session) while SAM
    // correctly reflects the empty membership. Treating that ENABLED
    // entry as truth would re-introduce the v2.8.68 stripped-account
    // bug. DENY_ONLY is the safe discriminator: that attribute is
    // recomputed by LSA from current SAM state at logon and means
    // "the user is in this group right now, just under restricted
    // policy" — exactly the Guest+Admin signal.
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    PSID adminSid = nullptr;
    PSID usersSid = nullptr;
    PSID guestSid = nullptr;
    AllocateAndInitializeSid(&ntAuth, 2,
        SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
        0, 0, 0, 0, 0, 0, &adminSid);
    AllocateAndInitializeSid(&ntAuth, 2,
        SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_USERS,
        0, 0, 0, 0, 0, 0, &usersSid);
    AllocateAndInitializeSid(&ntAuth, 2,
        SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_GUESTS,
        0, 0, 0, 0, 0, 0, &guestSid);

    HANDLE processToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(),
                         TOKEN_QUERY | TOKEN_DUPLICATE,
                         &processToken)) {
        TOKEN_LINKED_TOKEN linked = {};
        DWORD needed = 0;
        HANDLE linkedToken = nullptr;
        if (GetTokenInformation(processToken, TokenLinkedToken,
                                &linked, sizeof(linked), &needed)) {
            linkedToken = linked.LinkedToken;
        }
        HANDLE tokenToScan = linkedToken ? linkedToken : processToken;

        DWORD tgSize = 0;
        GetTokenInformation(tokenToScan, TokenGroups, nullptr, 0, &tgSize);
        if (tgSize > 0) {
            auto* tg = static_cast<TOKEN_GROUPS*>(LocalAlloc(LPTR, tgSize));
            if (tg && GetTokenInformation(tokenToScan, TokenGroups,
                                           tg, tgSize, &tgSize)) {
                int adminMatches = 0, adminDenyOnly = 0;
                int userMatches  = 0, guestMatches = 0;
                for (DWORD i = 0; i < tg->GroupCount; ++i) {
                    PSID sid = tg->Groups[i].Sid;
                    if (!sid) continue;
                    DWORD attrs = tg->Groups[i].Attributes;
                    bool denyOnly =
                        (attrs & SE_GROUP_USE_FOR_DENY_ONLY) != 0;
                    if (adminSid && EqualSid(sid, adminSid)) {
                        ++adminMatches;
                        if (denyOnly) ++adminDenyOnly;
                        // DENY_ONLY Admin is always trustworthy —
                        // it's LSA's runtime signal that the user is
                        // in Admin per current SAM state.
                        if (denyOnly) g_isAdmin = true;
                        // ENABLED Admin is only trustworthy when SAM
                        // failed entirely (domain fallback). Skipped
                        // when samResolved to avoid stale stripped
                        // memberships.
                        else if (!samResolved) g_isAdmin = true;
                    }
                    if (usersSid && EqualSid(sid, usersSid)) {
                        ++userMatches;
                        if (!samResolved) g_isUser = true;
                    }
                    if (guestSid && EqualSid(sid, guestSid)) {
                        ++guestMatches;
                        if (!samResolved) g_isGuest = true;
                    }
                }
                dbgprintf(L"TokenGroups scan: count=%lu admin=%d (deny-only=%d) "
                       L"user=%d guest=%d samResolved=%d",
                       tg->GroupCount, adminMatches, adminDenyOnly,
                       userMatches, guestMatches, samResolved ? 1 : 0);
            }
            if (tg) LocalFree(tg);
        }

        if (linkedToken) CloseHandle(linkedToken);
        CloseHandle(processToken);
    }

    if (adminSid) FreeSid(adminSid);
    if (usersSid) FreeSid(usersSid);
    if (guestSid) FreeSid(guestSid);

    // SAM privilege-class fallback. NetUserGetInfo level 1 returns the
    // user's stored "privilege class" (USER_PRIV_GUEST=0,
    // USER_PRIV_USER=1, USER_PRIV_ADMIN=2). Critically, this read is
    // typically permitted for the user themselves even when
    // NetUserGetLocalGroups is ACL-denied — covers the case observed
    // in the wild where a Guest who has been added to BUILTIN\
    // Administrators (via an elevated helper like usercpl-restore's
    // AdminHelper) gets status=ERROR_ACCESS_DENIED on the group
    // enumeration but can still see their own usri1_priv. SAM
    // promotes usri1_priv to USER_PRIV_ADMIN when the user is added
    // to a privileged group.
    //
    // We only ELEVATE flags here (set g_isAdmin if priv=ADMIN, never
    // clear it). usri1_priv can lag behind group changes on some
    // configurations, so it's not authoritative — but a positive
    // result is trustworthy.
    {
        wchar_t userSam[256] = L"";
        DWORD userSamLen = ARRAYSIZE(userSam);
        if (GetUserNameW(userSam, &userSamLen)) {
            USER_INFO_1* info1 = nullptr;
            NET_API_STATUS s1 = NetUserGetInfo(
                nullptr, userSam, 1, reinterpret_cast<LPBYTE*>(&info1));
            if (s1 == NERR_Success && info1) {
                dbgprintf(L"NetUserGetInfo('%s', level=1): usri1_priv=%lu "
                       L"(0=guest, 1=user, 2=admin)",
                       userSam, info1->usri1_priv);
                if (info1->usri1_priv == USER_PRIV_ADMIN) g_isAdmin = true;
                NetApiBufferFree(info1);
            } else {
                dbgprintf(L"NetUserGetInfo('%s', level=1) failed: status=%lu",
                       userSam, s1);
            }
        }
    }
}

// Mirrors UserPassword.CheckCurrentUserHasPassword — interactive logon
// with empty password; success means no password, ERROR_LOGON_FAILURE
// means password exists, ERROR_ACCOUNT_RESTRICTION means blank-password
// policy fired (counts as "no password"). Falls back to network logon
// if the interactive attempt is blocked by policy.
static void RefreshHasPassword() {
    g_hasPassword = true;  // conservative default

    HANDLE token = nullptr;
    BOOL ok = LogonUserW(g_userName, L".", L"",
                          LOGON32_LOGON_INTERACTIVE,
                          LOGON32_PROVIDER_DEFAULT, &token);
    if (ok) { CloseHandle(token); g_hasPassword = false; return; }

    DWORD err = GetLastError();
    if (err == ERROR_LOGON_FAILURE)        { g_hasPassword = true;  return; }
    if (err == ERROR_ACCOUNT_RESTRICTION)  { g_hasPassword = false; return; }

    // Console-only blank-password policy may have blocked interactive —
    // try network.
    ok = LogonUserW(g_userName, L".", L"",
                     LOGON32_LOGON_NETWORK,
                     LOGON32_PROVIDER_DEFAULT, &token);
    if (ok) { CloseHandle(token); g_hasPassword = false; return; }

    err = GetLastError();
    if (err == ERROR_LOGON_FAILURE)        { g_hasPassword = true;  return; }
    if (err == ERROR_ACCOUNT_RESTRICTION)  { g_hasPassword = false; return; }
    // Otherwise keep the conservative default (true).
}

static void RefreshUserState() {
    RefreshUserName();
    RefreshUserGroupMembership();
    RefreshHasPassword();
    dbgprintf(L"User state: name=%s admin=%d user=%d guest=%d password=%d",
           g_userName, g_isAdmin, g_isUser, g_isGuest, g_hasPassword);
}

// Convert points → pixels at the given HDC's DPI.
static int PtToPx(int pt, HDC hdc) {
    return -MulDiv(pt, GetDeviceCaps(hdc, LOGPIXELSY), 72);
}

static void EnsureFonts(HWND hwnd) {
    if (g_fontRegular && g_fontHeader &&
        g_fontRegularUnderline && g_fontHeaderUnderline) return;
    HDC hdc = GetDC(hwnd);
    if (!hdc) return;
    if (!g_fontRegular) {
        g_fontRegular = CreateFontW(
            PtToPx(9, hdc), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");
    }
    if (!g_fontRegularUnderline) {
        g_fontRegularUnderline = CreateFontW(
            PtToPx(9, hdc), 0, 0, 0, FW_NORMAL,
            FALSE, TRUE /*underline*/, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");
    }
    if (!g_fontHeader) {
        g_fontHeader = CreateFontW(
            PtToPx(12, hdc), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");
    }
    if (!g_fontHeaderUnderline) {
        g_fontHeaderUnderline = CreateFontW(
            PtToPx(12, hdc), 0, 0, 0, FW_NORMAL,
            FALSE, TRUE /*underline*/, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");
    }
    ReleaseDC(hwnd, hdc);
}

// The clickable / hand-cursor rect for the aero glass frame.
// Starts from the visible frame bounds (in normal mode AERO_X is
// negative — the .NET overhang — so we clip to positive-x). In normal
// mode we also shrink ~25% and shift left so the hit hugs the picture
// inside the bevel; in compact mode the frame is already small and
// fully inside the popup, so we use the natural visible rect.
// Clicking opens User Accounts, same as the footer link.
static RECT GetAeroFrameRect() {
    int left  = (AERO_X < 0) ? 0 : AERO_X;
    int right = AERO_X + AERO_W;
    if (right > FLYOUT_W) right = FLYOUT_W;
    RECT rc = { left, AERO_Y, right, AERO_Y + AERO_H };

    if (g_compactMode) {
        // Compact: 5% inset (centered on the rect) so the hitbox hugs
        // the visible 64×64 frame slightly.
        constexpr float COMPACT_HITBOX_SCALE = 0.95f;
        int newW = static_cast<int>((rc.right  - rc.left) * COMPACT_HITBOX_SCALE);
        int newH = static_cast<int>((rc.bottom - rc.top)  * COMPACT_HITBOX_SCALE);
        int cx   = (rc.left + rc.right)  / 2;
        int cy   = (rc.top  + rc.bottom) / 2;
        rc.left   = cx - newW / 2;
        rc.right  = rc.left + newW;
        rc.top    = cy - newH / 2;
        rc.bottom = rc.top  + newH;
    } else {
        constexpr float HITBOX_SCALE    = 0.75f;  // 25% smaller per dimension
        constexpr int   HITBOX_OFFSET_X = -5;     // -7 baseline + 2 px right (v2.8.24)
        constexpr int   HITBOX_OFFSET_Y =  2;     // 2 px down  (v2.8.27)
        int newW = static_cast<int>((rc.right  - rc.left) * HITBOX_SCALE);
        int newH = static_cast<int>((rc.bottom - rc.top)  * HITBOX_SCALE);
        int cx   = (rc.left + rc.right)  / 2 + HITBOX_OFFSET_X;
        int cy   = (rc.top  + rc.bottom) / 2 + HITBOX_OFFSET_Y;
        rc.left   = cx - newW / 2;
        rc.right  = rc.left + newW;
        rc.top    = cy - newH / 2;
        rc.bottom = rc.top  + newH;
    }
    return rc;
}

// Compact-mode label stack — vertically centered against the picture
// rect like a vertical flow layout. When g_hasPassword is false, only
// username + type render, and the two-row group recenters to stay
// aligned with the PFP's vertical midpoint instead of leaving the
// "password" slot empty at the bottom. Heights / inter-row gaps below
// match the previously hand-tuned pixel layout; only the stack origin
// is computed.
struct CompactLabels {
    int usernameY;
    int typeY;
    int passwordY;  // valid only when g_hasPassword
};

static CompactLabels GetCompactLabelLayout() {
    constexpr int USERNAME_H        = 23;
    constexpr int TYPE_H            = 15;
    constexpr int PASSWORD_H        = 15;
    constexpr int GAP_USERNAME_TYPE = 0;
    constexpr int GAP_TYPE_PASSWORD = 1;

    int stackH = USERNAME_H + GAP_USERNAME_TYPE + TYPE_H;
    if (g_hasPassword) stackH += GAP_TYPE_PASSWORD + PASSWORD_H;

    int pfpTop    = USERPIC_Y;
    int pfpBottom = USERPIC_Y + USERPIC_H;
    int stackTop  = (pfpTop + pfpBottom - stackH) / 2 - kCompactLabelLift;

    CompactLabels L;
    L.usernameY = stackTop;
    L.typeY     = L.usernameY + USERNAME_H + GAP_USERNAME_TYPE;
    L.passwordY = L.typeY     + TYPE_H     + GAP_TYPE_PASSWORD;
    return L;
}

// The clickable rect of the username label. Hugs the actual rendered
// text (measured via GetTextExtentPoint32W on g_userName), capped to
// the container width that DT_END_ELLIPSIS would clip to. Coordinates
// match the WM_PAINT layout — different in compact mode (label sits
// alongside the small frame instead of below the header).
static RECT GetUserNameRect(HWND hwnd) {
    EnsureFonts(hwnd);
    HDC hdc = GetDC(hwnd);
    SIZE sz = { 0, 0 };
    if (hdc) {
        HFONT old = static_cast<HFONT>(SelectObject(hdc, g_fontHeader));
        GetTextExtentPoint32W(hdc, g_userName, lstrlenW(g_userName), &sz);
        SelectObject(hdc, old);
        ReleaseDC(hwnd, hdc);
    }
    const int containerX = g_compactMode ? 72 : 143;  // +1 px right in v2.8.31
    const int containerY = g_compactMode ? GetCompactLabelLayout().usernameY
                                         : 49;
    const int containerW = 130;  // both modes
    const int containerH = 23;
    if (sz.cx > containerW) sz.cx = containerW;
    int x = containerX;
    int y = containerY + (containerH - sz.cy) / 2;
    RECT rc = { x, y, x + sz.cx, y + sz.cy };
    return rc;
}

// Compute the bounding rect of the link text inside the footer.
// Used by both the painter and the hit-tester.
static RECT GetLinkRect(HWND hwnd) {
    EnsureFonts(hwnd);
    HDC hdc = GetDC(hwnd);
    SIZE sz = { 0, 0 };
    if (hdc) {
        HFONT old = static_cast<HFONT>(SelectObject(hdc, g_fontRegular));
        GetTextExtentPoint32W(hdc, LINK_TEXT, lstrlenW(LINK_TEXT), &sz);
        SelectObject(hdc, old);
        ReleaseDC(hwnd, hdc);
    }
    int x = (FLYOUT_W - sz.cx) / 2;
    int y = FOOTER_TOP + (FOOTER_H - sz.cy) / 2;
    RECT rc = { x, y, x + sz.cx, y + sz.cy };
    return rc;
}

// -----------------------------------------------------------------
// Flyout (Phase 1) — empty top-level popup that appears above the
// user tile when clicked. The native 7785 flyout uses CLSID_TrayUserTileFlyout
// which doesn't work on Win10 LTSC; this is the start of a native-C++
// replacement for the .NET TaskPopup the user has been running as a
// separate process.
//
// Phase 1 only registers the class, creates the window on first click,
// positions it above the tile (clamped to monitor work area), and
// hides it on deactivate. No content yet — confirms the trigger and
// positioning before we layout controls.
// (FLYOUT_W/H/MARGIN, FOOTER_H/TOP are declared near the top of the
//  file so helpers like GetLinkRect can see them.)
// -----------------------------------------------------------------

// Task-button geometry (Phase 3) — visually matched to the .NET
// TaskPopup at runtime, where AutoScaleMode=Font expands the buttons
// to nearly the full flyout width. Buttons stack upward from just
// above the reserved footer; first added (Switch user) ends up at the
// bottom-most slot.
// Button geometry — Y values track FOOTER_TOP, recomputed when FLYOUT_H
// changes (compact mode shifts the whole stack up by 110 px).
static int BTN_X                  = 3;                  // 3 px gap from popup left edge
static const int BTN_RIGHT_MARGIN = 5;                  // 5 px gap from popup right edge (3 base + 2 extra)
static int BTN_W                  = 276 - 3 - 6;        // FLYOUT_W - BTN_X - BTN_RIGHT_MARGIN
static const int BTN_H            = 32;
static const int BTN_BOTTOM_GAP   = 7;  // breathing room between bottom button and chin
static int BTN_SWITCH_Y           = (336 - 43) - 32 - 7;  // 270 in normal; recomputed in RecomputeLayout
static int BTN_LOG_OFF_Y          = (336 - 43) - 32 - 7 - 32;  // 238
static int BTN_LOCK_Y             = (336 - 43) - 32 - 7 - 32 - 32;  // 206

// Child-control IDs.
#define IDC_BTN_LOCK         101
#define IDC_BTN_LOG_OFF      102
#define IDC_BTN_SWITCH       103

// (g_flyoutHwnd is forward-declared near the top so the tile subclass
// can read it from WM_MOUSEACTIVATE.)
static ATOM  g_flyoutClass = 0;

// -----------------------------------------------------------------
// Custom "TaskButton" window class (Phase 3) — replicates the .NET
// TaskButton UserControl visual:
//
//   - Default state: just the Hyperlink-colored ("HotTrack") text on
//     a transparent background.
//   - Hover state:   Explorer::ListView Part 1 / State 2 (LISS_HOT)
//     drawn behind the text — soft cyan/blue gradient, the classic
//     Win7 task-pane hover effect.
//   - Pressed:       Explorer::ListView Part 1 / State 3 (LISS_SELECTED)
//   - Hand cursor on hover (matches the .NET AeroHandCursor).
//
// Click action is dispatched to the parent via WM_COMMAND with
// BN_CLICKED in HIWORD, so the existing WM_COMMAND handler keeps
// working unchanged.
// -----------------------------------------------------------------
static const wchar_t* TASKBTN_CLASS_NAME = L"WH_UserTileTaskButton";
static ATOM g_taskBtnClass = 0;

struct TaskBtnState {
    bool hovered;
    bool pressed;
    bool tracking;  // TrackMouseEvent armed?
};

static LRESULT CALLBACK TaskBtnWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    TaskBtnState* st = reinterpret_cast<TaskBtnState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_NCCREATE: {
            auto* fresh = new TaskBtnState{};
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(fresh));
            return DefWindowProcW(hwnd, msg, wp, lp);
        }

        case WM_NCDESTROY:
            if (st) {
                delete st;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            break;

        case WM_ERASEBKGND:
            // We do all painting in WM_PAINT; suppress default.
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);

            // Background — same DrawThemeBackground(Flyout, WINDOW=6) the
            // parent paints, so the button merges into the body bitmap.
            if (g_flyoutTheme && IsThemePartDefined(g_flyoutTheme,
                                                    FLYOUT_WINDOW, 0)) {
                DrawThemeBackground(g_flyoutTheme, hdc,
                                    FLYOUT_WINDOW, 0, &rc, nullptr);
            } else {
                FillRect(hdc, &rc,
                         reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
            }

            // Hover/pressed overlay — Explorer::ListView item theme. Use
            // the cached handle pre-opened by EnsureFlyoutCreated on a
            // worker thread, so it's the *system* aerodark schema rather
            // than explorer7's bundled-light one. Without that bypass,
            // the hover would be the light-blue Win7 ListView gradient
            // even on a dark theme — visibly mismatched with the system
            // wifi/clock flyouts the user sees.
            int themeState = 0;
            if (st && st->pressed)  themeState = 3;  // LISS_SELECTED
            else if (st && st->hovered) themeState = 2;  // LISS_HOT
            if (themeState && g_listViewTheme) {
                DrawThemeBackground(g_listViewTheme, hdc,
                                    1, themeState, &rc, nullptr);
            }

            // Title text — link color from the Flyout theme (orange under
            // aerodark, blue under aeronormal). Falls back to COLOR_HOTLIGHT.
            WCHAR text[128];
            int textLen = GetWindowTextW(hwnd, text, ARRAYSIZE(text));

            HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, g_fontRegular));
            SetBkMode(hdc, TRANSPARENT);
            int btnLinkState = (st && st->hovered) ? FLYOUT_LINK_HOVER
                                                   : FLYOUT_LINK_NORMAL;
            SetTextColor(hdc, GetFlyoutColor(FLYOUT_LINK, btnLinkState,
                                             3803 /*TMT_TEXTCOLOR*/,
                                             GetSysColor(COLOR_HOTLIGHT)));

            RECT textRc = rc;
            textRc.left += 10;  // left padding (matches .NET inset)
            DrawTextW(hdc, text, textLen, &textRc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            SelectObject(hdc, oldFont);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_MOUSEMOVE:
            if (st && !st->tracking) {
                TRACKMOUSEEVENT tme = {};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
                st->tracking = true;
                st->hovered = true;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;

        case WM_MOUSELEAVE:
            if (st) {
                st->tracking = false;
                if (st->hovered) {
                    st->hovered = false;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            break;

        case WM_LBUTTONDOWN:
            if (st) {
                SetCapture(hwnd);
                st->pressed = true;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONUP:
            if (st && st->pressed) {
                st->pressed = false;
                ReleaseCapture();
                POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                RECT rc;
                GetClientRect(hwnd, &rc);
                if (PtInRect(&rc, pt)) {
                    HWND parent = GetParent(hwnd);
                    int  id     = GetDlgCtrlID(hwnd);
                    SendMessageW(parent, WM_COMMAND,
                                 MAKEWPARAM(id, BN_CLICKED),
                                 reinterpret_cast<LPARAM>(hwnd));
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_CAPTURECHANGED:
            if (st && st->pressed) {
                st->pressed = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void RegisterTaskButtonClass() {
    if (g_taskBtnClass) return;
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = TaskBtnWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);  // default arrow (no hand cursor on hover)
    wc.hbrBackground = nullptr;
    wc.lpszClassName = TASKBTN_CLASS_NAME;
    g_taskBtnClass = RegisterClassExW(&wc);
    if (!g_taskBtnClass) {
        dbgprintf(L"TaskButton class register failed: %lu", GetLastError());
    }
}

// Phase 3: spawn three task-button child windows of our custom class,
// matching the .NET FlowLayoutPanel BottomUp ordering.
static void CreateFlyoutButtons(HWND hwnd) {
    RegisterTaskButtonClass();
    EnsureFonts(hwnd);
    HMODULE hInst = GetModuleHandleW(nullptr);

    struct ButtonSpec {
        const wchar_t* text;
        int            id;
        int            y;
    };
    const ButtonSpec specs[] = {
        { UT_Text(UTS_LOCK_COMPUTER), IDC_BTN_LOCK,    BTN_LOCK_Y },
        { UT_Text(UTS_LOG_OFF),       IDC_BTN_LOG_OFF, BTN_LOG_OFF_Y },
        { UT_Text(UTS_SWITCH_USER),   IDC_BTN_SWITCH,  BTN_SWITCH_Y },
    };
    for (const auto& s : specs) {
        CreateWindowExW(
            0, TASKBTN_CLASS_NAME, s.text,
            WS_CHILD | WS_VISIBLE,
            BTN_X, s.y, BTN_W, BTN_H,
            hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(s.id)),
            hInst, nullptr);
    }
}

static LRESULT CALLBACK FlyoutWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT rcClient;
            GetClientRect(hwnd, &rcClient);

            // Theme handles are pre-opened in EnsureFlyoutCreated and
            // re-opened in ShowFlyoutForTile after a WM_THEMECHANGED.
            // Don't call OpenSystemTheme from inside WM_PAINT — during an
            // OS theme transition (Personalization apply), OpenThemeData
            // can block on the originating thread's message pump, which
            // is exactly the thread we'd be on inside WM_PAINT, deadlocking
            // the tray. The drawing path below handles null handles via
            // its IsThemePartDefined / fallback branches.

            // Whole-window background — DrawThemeBackground(Flyout, WINDOW=6)
            // renders the theme part's *bitmap*, which is what carries the
            // dark/light visual in aerodark/aeronormal. Earlier we tried
            // FillRect(GetThemeColor(...,TMT_FILLCOLOR)) and got white because
            // FillColor metadata ≠ what the bitmap actually paints. Mirrors
            // FlyoutSheet's `dtb(Flyout, 6, 0)` background.
            if (g_flyoutTheme && IsThemePartDefined(g_flyoutTheme,
                                                    FLYOUT_WINDOW, 0)) {
                DrawThemeBackground(g_flyoutTheme, hdc,
                                    FLYOUT_WINDOW, 0, &rcClient, nullptr);
            } else {
                FillRect(hdc, &rcClient,
                         reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
                FrameRect(hdc, &rcClient,
                          reinterpret_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
            }

            // Picture under, alpha-aware frame border on top
            // Drawing that way is what gives the glass its reflection
            BLENDFUNCTION bfPa = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            HBITMAP frameBmp = g_compactMode ? g_aeroFrameSmallBmp
                                             : g_aeroFrameBmp;

            // Decoded by RefreshUserPicture from the path the shell
            // wrote to HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\
            // AccountPicture\Users\<SID>\Image1080 (or smaller, in
            // descending order). That registry-resolved path is the
            // same source 7850's _CreateUserPicture / SHGetUserPicture
            // pipeline reads from. We decode at full source resolution
            // and Fant-scale to USERPIC_W×H, so the picture stays sharp
            // regardless of display size — unlike the tile's tiny
            // ~24-32 px HBITMAP at +0xa0 which is too small to scale up.
            if (g_userPictureBmp) {
                HDC memDc = CreateCompatibleDC(hdc);
                if (memDc) {
                    HBITMAP oldBm = static_cast<HBITMAP>(
                        SelectObject(memDc, g_userPictureBmp));
                    AlphaBlend(hdc, USERPIC_X, USERPIC_Y,
                               USERPIC_W, USERPIC_H,
                               memDc, 0, 0, USERPIC_W, USERPIC_H, bfPa);
                    SelectObject(memDc, oldBm);
                    DeleteDC(memDc);
                }
            }
            if (frameBmp) {
                // Both frames are cached at AERO_W x AERO_H already
                // So this blit is src=dst and never resamples
                int srcW = AERO_W;
                int srcH = AERO_H;
                HDC memDc = CreateCompatibleDC(hdc);
                if (memDc) {
                    HBITMAP oldBm = static_cast<HBITMAP>(
                        SelectObject(memDc, frameBmp));
                    AlphaBlend(hdc, AERO_X, AERO_Y, AERO_W, AERO_H,
                               memDc, 0, 0, srcW, srcH, bfPa);
                    SelectObject(memDc, oldBm);
                    DeleteDC(memDc);
                }
            }

            // Phase 2: text labels. Colors come from the Flyout theme;
            // fallbacks match the .NET TaskPopup's original light-theme
            // values so classic theme still looks right. Label positions
            // shift up & left in compact mode — there's no header label,
            // and the labels sit alongside the smaller frame.
            EnsureFonts(hwnd);
            SetBkMode(hdc, TRANSPARENT);
            HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, g_fontRegular));

            if (!g_compactMode) {
                // Header line above the account name, skipped in compact mode
                SetTextColor(hdc, GetFlyoutColor(FLYOUT_BODY, 2, TMT_TEXTCOLOR,
                                                 RGB(0, 0, 0)));
                RECT r1 = { 13, 10, FLYOUT_W - 10, 30 };
                DrawTextW(hdc, UT_Text(UTS_LOGGED_ON_AS), -1, &r1,
                          DT_LEFT | DT_TOP | DT_SINGLELINE);
            }

            // label_UserName: classic dark-blue heading color from
            // TextStyle::TEXT_MAININSTRUCTION — the same blue Control
            // Panel page titles, Task Dialog main instructions, and
            // balloon-tip headers use. Same regular-weight Segoe UI 12pt
            // font, no semibold. Hover keeps the color and just adds an
            // underline as a subtle clickable affordance — clicking
            // opens %USERPROFILE% (handled in WM_LBUTTONUP).
            SelectObject(hdc, g_userNameHovered ? g_fontHeader
                                                : g_fontHeader);
            SetTextColor(hdc, GetMainInstructionColor(RGB(0, 51, 153)));
            // Compact: labels start at x=71 (1 px right of the 64×64
            // frame's right edge at x≈68), and the username/type/password
            // stack is vertically centered against the picture rect. See
            // GetCompactLabelLayout — height collapses to 2 rows when no
            // password is set so the visible group stays PFP-centered.
            CompactLabels CL = g_compactMode ? GetCompactLabelLayout()
                                             : CompactLabels{};

            RECT r2 = g_compactMode
                ? RECT{ 72, CL.usernameY, 72 + 130, CL.usernameY + 23 }
                : RECT{ 143, 49, 143 + 132, 49 + 23 };
            DrawTextW(hdc, g_userName, -1, &r2,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            // label_UserType + label2: normal body text (state 0)
            SelectObject(hdc, g_fontRegular);
            COLORREF bodyText = GetFlyoutColor(FLYOUT_BODY, 0, TMT_TEXTCOLOR,
                                               RGB(64, 64, 64));
            SetTextColor(hdc, bodyText);
            RECT r3 = g_compactMode
                ? RECT{ 73, CL.typeY, 73 + 130, CL.typeY + 15 }
                : RECT{ 144, 73, 144 + 130, 73 + 15 };
            // Label rule (priority: Administrator > Guest > Standard user):
            //   Administrator  if BUILTIN\Administrators membership is
            //                  detected through any source (SAM groups,
            //                  token DENY_ONLY, or usri1_priv=ADMIN).
            //   Guest account  if BUILTIN\Guests is present. This beats
            //                  Standard user because a Guest's token
            //                  usually carries BUILTIN\Users too via the
            //                  Authenticated Users → Users nesting that
            //                  LSA expands at logon — meaning "user=1"
            //                  from the token alone doesn't imply real
            //                  Users membership. When Guest is set,
            //                  trust that as the dominant classification.
            //                  Also covers explicit BUILTIN\Guests
            //                  membership for non-Guest accounts the
            //                  admin manually added.
            //   Standard user  otherwise, if BUILTIN\Users is detected.
            //                  By this branch we've already ruled out
            //                  admin/guest, so a positive Users signal
            //                  unambiguously means Standard.
            //   Guest account  otherwise (stripped account, no
            //                  memberships at all).
            const wchar_t* userTypeText =
                g_isAdmin ? UT_Text(UTS_ADMINISTRATOR)
                          : (g_isGuest ? UT_Text(UTS_GUEST_ACCOUNT)
                                       : (g_isUser ? UT_Text(UTS_STANDARD_USER)
                                                   : UT_Text(UTS_GUEST_ACCOUNT)));
            DrawTextW(hdc, userTypeText, -1, &r3,
                      DT_LEFT | DT_TOP | DT_SINGLELINE);

            if (g_hasPassword) {
                RECT r4 = g_compactMode
                    ? RECT{ 73, CL.passwordY, 73 + 130, CL.passwordY + 15 }
                    : RECT{ 144, 88, 144 + 130, 88 + 15 };
                DrawTextW(hdc, UT_Text(UTS_PASSWORD_PROT), -1, &r4,
                          DT_LEFT | DT_TOP | DT_SINGLELINE);
            }

            // 1px hairline divider above the task-button stack — same theme
            // part (FLYOUT_DIVIDER=5) timedate.cpl's notification panel uses
            // for its `<element height="2rp" background="dtb(Flyout, 5, 0)">`
            // separator. Picks up the dark-gray hairline color from the
            // active aerodark theme automatically.
            const int DIV_GAP_ABOVE_LISTVIEW = 7;  // px between divider top and first button
            RECT dividerRect = {
                0, BTN_LOCK_Y - DIV_GAP_ABOVE_LISTVIEW,
                FLYOUT_W, BTN_LOCK_Y - DIV_GAP_ABOVE_LISTVIEW + 1
            };
            if (g_flyoutTheme && IsThemePartDefined(g_flyoutTheme,
                                                    FLYOUT_DIVIDER, 0)) {
                DrawThemeBackground(g_flyoutTheme, hdc,
                                    FLYOUT_DIVIDER, 0, &dividerRect, nullptr);
            }

            // Phase 4: footer panel with the "Open User Accounts" link.
            // Chin background — TrayNotifyFlyout part 2 is what the system
            // clock flyout's settings-link strip uses (per timedate.cpl's
            // FlyoutSheet: `dtb(TrayNotifyFlyout, 2, 0)`). Different theme
            // class than the body — a dedicated tray-flyout-chin schema.
            // Anchor the chin to the *actual client rect*, not the
            // logical FLYOUT_W/FLYOUT_H. The window has WS_BORDER, which
            // eats 1 px on each side — so rcClient is FLYOUT_W-2 wide /
            // FLYOUT_H-2 tall. The body fill above uses rcClient too, so
            // matching here is what aligns the chin's top-edge highlight
            // (in the themed branch) with the body — otherwise the theme
            // lays its bitmap out for FLYOUT_W and the highlight's right
            // end gets clipped past where the body actually ends, leaving
            // the visible 1 px mismatch at the top-right corner.
            RECT footer = { 0, FOOTER_TOP, rcClient.right, rcClient.bottom };
            if (g_chinTheme && IsThemePartDefined(g_chinTheme, 2, 0)) {
                DrawThemeBackground(g_chinTheme, hdc, 2, 0, &footer, nullptr);
            } else {
                // No themed chin available (e.g. classic theme) — fall
                // back to the system's default 3D dialog face color
                // (COLOR_3DFACE) so the chin matches the rest of the
                // window frame instead of using a hardcoded aero-light
                // gradient that clashes under non-themed schemas.
                //
                // Anchored to the top: shrink L/R/B by 1 px (relative to
                // rcClient) so the body shows through a thin ring on
                // those sides and the chin doesn't bleed into the
                // WS_BORDER frame. Top edge stays flush with the divider.
                RECT chinRc = { 1, FOOTER_TOP,
                                rcClient.right  - 1,
                                rcClient.bottom - 1 };
                FillRect(hdc, &chinRc,
                         reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1));
            }

            // Centered link text — color from FLYOUT_LINK (orange in
            // aerodark, blue in classic, etc.), underlined on hover.
            HFONT linkFont = g_linkHovered ? g_fontRegularUnderline : g_fontRegular;
            SelectObject(hdc, linkFont);
            int linkState = g_linkHovered ? FLYOUT_LINK_HOVER
                                          : FLYOUT_LINK_NORMAL;
            SetTextColor(hdc, GetFlyoutColor(FLYOUT_LINK, linkState,
                                             TMT_TEXTCOLOR,
                                             GetSysColor(COLOR_HOTLIGHT)));
            DrawTextW(hdc, LINK_TEXT, -1, &footer,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            SelectObject(hdc, oldFont);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_MOUSEMOVE: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            RECT linkRc = GetLinkRect(hwnd);
            RECT nameRc = GetUserNameRect(hwnd);
            bool nowLinkHovered = PtInRect(&linkRc, pt) != FALSE;
            bool nowNameHovered = PtInRect(&nameRc, pt) != FALSE;

            if (!g_linkTracking) {
                TRACKMOUSEEVENT tme = {};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
                g_linkTracking = true;
            }
            if (nowLinkHovered != g_linkHovered) {
                g_linkHovered = nowLinkHovered;
                // Invalidate just the footer area to avoid flicker on the
                // upper labels.
                RECT footer = { 0, FOOTER_TOP, FLYOUT_W, FLYOUT_H };
                InvalidateRect(hwnd, &footer, FALSE);
            }
            if (nowNameHovered != g_userNameHovered) {
                g_userNameHovered = nowNameHovered;
                int yu = g_compactMode ? GetCompactLabelLayout().usernameY : 49;
                RECT inv = g_compactMode
                    ? RECT{ 72, yu, 72 + 130, yu + 23 }
                    : RECT{ 143, 49, 143 + 132, 49 + 23 };
                InvalidateRect(hwnd, &inv, FALSE);
            }
            break;
        }

        case WM_MOUSELEAVE:
            g_linkTracking = false;
            if (g_linkHovered) {
                g_linkHovered = false;
                RECT footer = { 0, FOOTER_TOP, FLYOUT_W, FLYOUT_H };
                InvalidateRect(hwnd, &footer, FALSE);
            }
            if (g_userNameHovered) {
                g_userNameHovered = false;
                int yu = g_compactMode ? GetCompactLabelLayout().usernameY : 49;
                RECT inv = g_compactMode
                    ? RECT{ 72, yu, 72 + 130, yu + 23 }
                    : RECT{ 143, 49, 143 + 132, 49 + 23 };
                InvalidateRect(hwnd, &inv, FALSE);
            }
            break;

        case WM_LBUTTONUP: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            RECT linkRc  = GetLinkRect(hwnd);
            RECT frameRc = GetAeroFrameRect();
            RECT nameRc  = GetUserNameRect(hwnd);
            if (PtInRect(&linkRc, pt) || PtInRect(&frameRc, pt)) {
                HideFlyoutOffscreen(hwnd);
                ShellExecuteW(nullptr, L"open", L"control.exe",
                              L"/name Microsoft.UserAccounts",
                              nullptr, SW_SHOWNORMAL);
            } else if (PtInRect(&nameRc, pt)) {
                HideFlyoutOffscreen(hwnd);
                wchar_t userProfile[MAX_PATH];
                DWORD len = ExpandEnvironmentStringsW(L"%USERPROFILE%",
                                                      userProfile, MAX_PATH);
                if (len > 0 && len <= MAX_PATH) {
                    ShellExecuteW(nullptr, L"open", userProfile,
                                  nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
            return 0;
        }

        case WM_DWMCOMPOSITIONCHANGED:
            // Drop or re-add the glass now instead of at the next open
            if (ApplyFlyoutFrame(hwnd)) {
                SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                             SWP_FRAMECHANGED | SWP_NOACTIVATE);
            }
            return 0;

        case WM_NCHITTEST:
            // The thick glass frame is ours now, so it must answer clicks
            // Collapsing to HTBORDER also stops it being dragged or resized
            return LockFrameHit(DefWindowProcW(hwnd, msg, wp, lp));

        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONDBLCLK:
        case WM_SYSCOMMAND:
            if (IsFrameDragMessage(msg, wp)) return 0;
            break;

        case WM_SETCURSOR: {
            // Hand cursor over the "Open User Accounts" link, the aero
            // glass frame / picture composite, AND the username label.
            // All three are clickable: link & frame open User Accounts,
            // username opens %USERPROFILE%. Matches the .NET AeroHandCursor.
            POINT pt;
            if (GetCursorPos(&pt) && ScreenToClient(hwnd, &pt)) {
                RECT linkRc  = GetLinkRect(hwnd);
                RECT frameRc = GetAeroFrameRect();
                RECT nameRc  = GetUserNameRect(hwnd);
                if (PtInRect(&linkRc, pt) || PtInRect(&frameRc, pt) ||
                    PtInRect(&nameRc, pt)) {
                    SetCursor(LoadCursor(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            break;  // fall through to DefWindowProc for default arrow
        }

        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                // The tile subclass captured shown-state at WM_MOUSEACTIVATE
                // *before* this fires, so we can move off-screen here
                // without breaking the toggle logic.
                HideFlyoutOffscreen(hwnd);
            }
            return 0;

        case WM_COMMAND:
            // BN_CLICKED notifications from our task buttons. Hide the
            // flyout first (so it's out of the way before the action
            // takes effect), then perform the action.
            //
            // "Switch user" calls WTSDisconnectSession (same syscall
            // tsdiscon.exe wraps), which jumps directly to LogonUI's
            // user-picker screen — one fewer click than the .NET
            // TaskPopup's LockWorkStation path (which lands on the lock
            // screen and forces the user to hit "Switch user" there).
            if (HIWORD(wp) == BN_CLICKED) {
                switch (LOWORD(wp)) {
                    case IDC_BTN_LOCK:
                        HideFlyoutOffscreen(hwnd);
                        LockWorkStation();
                        return 0;
                    case IDC_BTN_LOG_OFF:
                        HideFlyoutOffscreen(hwnd);
                        ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_MAJOR_OTHER);
                        return 0;
                    case IDC_BTN_SWITCH:
                        HideFlyoutOffscreen(hwnd);
                        if (!WTSDisconnectSession(WTS_CURRENT_SERVER_HANDLE,
                                                   WTS_CURRENT_SESSION,
                                                   FALSE)) {
                            // Privilege/policy may block the disconnect
                            // (rare on a normal interactive session, but
                            // possible under restricted SKUs). Fall back
                            // to the lock screen so the user still has a
                            // path to Switch User.
                            dbgprintf(L"WTSDisconnectSession failed (LE=%lu) — "
                                   L"falling back to LockWorkStation",
                                   GetLastError());
                            LockWorkStation();
                        }
                        return 0;
                }
            }
            break;

        case WM_THEMECHANGED:
            // Drop stale handles so the next show reopens against the new
            // active theme. Don't InvalidateRect — the flyout is hidden
            // (alpha=0) when this typically arrives, and a synchronous
            // repaint here would re-enter OpenThemeData during the OS
            // theme transition and could deadlock the tray thread.
            if (g_flyoutTheme)   { CloseThemeData(g_flyoutTheme);   g_flyoutTheme   = nullptr; }
            if (g_chinTheme)     { CloseThemeData(g_chinTheme);     g_chinTheme     = nullptr; }
            if (g_listViewTheme) { CloseThemeData(g_listViewTheme); g_listViewTheme = nullptr; }
            if (g_textStyleTheme) {
                CloseThemeData(g_textStyleTheme);
                g_textStyleTheme = nullptr;
            }
            g_textStyleTried = false;
            // Re-assert the layered "hidden" state. Some theme transitions
            // reset WS_EX_LAYERED / SetLayeredWindowAttributes on top-level
            // windows, which would briefly reveal the flyout at its last
            // on-screen position before WM_ACTIVATE fires WA_INACTIVE.
            if (!g_flyoutShown) {
                LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
                if (!(ex & WS_EX_LAYERED)) {
                    SetWindowLongW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
                }
                SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
            }
            return 0;  // consume — don't let DefWindowProc invalidate

        case WM_NCDESTROY:
            if (g_flyoutTheme)   { CloseThemeData(g_flyoutTheme);   g_flyoutTheme   = nullptr; }
            if (g_chinTheme)     { CloseThemeData(g_chinTheme);     g_chinTheme     = nullptr; }
            if (g_listViewTheme) { CloseThemeData(g_listViewTheme); g_listViewTheme = nullptr; }
            g_flyoutHwnd = nullptr;
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void EnsureFlyoutCreated() {
    if (g_flyoutHwnd && IsWindow(g_flyoutHwnd)) return;

    HMODULE hInst = GetModuleHandleW(nullptr);

    if (g_flyoutClass == 0) {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = FlyoutWndProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;  // we paint in WM_PAINT
        wc.lpszClassName = FLYOUT_CLASS_NAME;
        g_flyoutClass = RegisterClassExW(&wc);
        if (g_flyoutClass == 0) {
            dbgprintf(L"Flyout class register failed: %lu", GetLastError());
            return;
        }
    }

    // Style mirrors timedate.cpl _OpenUp's flyout window (0x80800000 =
    // WS_POPUP | WS_BORDER). WS_BORDER + DWMWA_NCRENDERING_POLICY=ENABLED
    // (set below) is what gives the system clock flyout its DWM-composited
    // dark frame.
    //
    // The window is created WS_VISIBLE + WS_EX_LAYERED at an on-screen
    // position with alpha=0 (invisible). DWM only allocates a redirection
    // bitmap / presentation surface for windows whose rect intersects a
    // monitor, so we *can't* park it at (-32000,-32000) and expect our
    // pre-paint to land in a usable surface — that's what was causing the
    // first-show flash. With WS_EX_LAYERED + alpha 0 the window is fully
    // transparent (and click-through, which is fine while "hidden") but
    // DWM still composes it, our RedrawWindow below reaches the real
    // surface, and toggling alpha to 255 on show is instant.
    g_flyoutHwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        FLYOUT_CLASS_NAME, L"User",
        WS_POPUP | WS_BORDER | WS_CLIPCHILDREN | WS_VISIBLE,
        0, 0, FLYOUT_W, FLYOUT_H,
        nullptr, nullptr, hInst, nullptr);

    if (!g_flyoutHwnd) {
        dbgprintf(L"Flyout CreateWindow failed: %lu", GetLastError());
        return;
    }
    dbgprintf(L"Flyout created: %p", g_flyoutHwnd);

    // Same thick glass frame every other system flyout gets
    // Without it DWM paints glass into a margin the window does not own
    ApplyFlyoutFrame(g_flyoutHwnd);

    // Created at FLYOUT_W by FLYOUT_H outer, so the thick frame leaves the
    // client short and every paint before the move lays out 16px too small
    RECT want = { 0, 0, FLYOUT_W, FLYOUT_H };
    AdjustWindowRectEx(&want,
                       (DWORD)GetWindowLongPtrW(g_flyoutHwnd, GWL_STYLE), FALSE,
                       (DWORD)GetWindowLongPtrW(g_flyoutHwnd, GWL_EXSTYLE));
    SetWindowPos(g_flyoutHwnd, nullptr, 0, 0,
                 want.right - want.left, want.bottom - want.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);

    // Make the window fully transparent immediately. WS_EX_LAYERED was set
    // at creation; alpha=0 means it occupies its on-screen position
    // (essential for DWM to compose into the redirection bitmap) but is
    // invisible and click-through to the user.
    SetLayeredWindowAttributes(g_flyoutHwnd, 0, 0, LWA_ALPHA);

    // Mirror timedate.cpl _OpenUp: when DWM composition is active, force
    // NC area to be DWM-rendered (DWMWA_NCRENDERING_POLICY=2,
    // DWMNCRP_ENABLED=2). Gives the flyout the dark-tinted Aero border that
    // matches the system clock flyout. Vista-era API, no immersive opt-in.
    BOOL composited = FALSE;
    if (SUCCEEDED(DwmIsCompositionEnabled(&composited)) && composited) {
        DWORD policy = 2;  // DWMNCRP_ENABLED
        HRESULT hr = DwmSetWindowAttribute(g_flyoutHwnd,
                                           2 /*DWMWA_NCRENDERING_POLICY*/,
                                           &policy, sizeof(policy));
        dbgprintf(L"DwmSetWindowAttribute(NCRENDERING_POLICY=ENABLED) hr=0x%08X", hr);

        // Disable DWM's open/close fade-in animation on this window. The
        // first-show flash is the system fading the popup in over a few
        // frames — that fade reveals whatever was in the un-composited
        // surface during the transition. Forcing transitions off makes
        // the SetWindowPos move appear instantly with the dark content
        // we already pre-painted off-screen.
        BOOL disableTransitions = TRUE;
        HRESULT hr2 = DwmSetWindowAttribute(
            g_flyoutHwnd,
            3 /*DWMWA_TRANSITIONS_FORCEDISABLED*/,
            &disableTransitions, sizeof(disableTransitions));
        dbgprintf(L"DwmSetWindowAttribute(TRANSITIONS_FORCEDISABLED) hr=0x%08X",
               hr2);
    }

    // Open theme handles BEFORE first paint so the user never sees the
    // light fallback. Worker thread bypasses explorer7's tray-thread hook
    // and gets the system aerodark schema instead.
    EnsureFlyoutThemes();
    dbgprintf(L"System themes pre-opened: Flyout=%p TrayNotifyFlyout=%p "
           L"Explorer::ListView=%p",
           g_flyoutTheme, g_chinTheme, g_listViewTheme);

    // Phase 5: decode the user picture into a cached PARGB DIB. Cached by
    // path + mtime so subsequent calls (per show) are no-ops unless the
    // user changed their picture in Settings — in which case we re-decode.
    RefreshUserPicture();

    // Phase 6: load both frame borders from our BITMAP resources
    LoadAeroFrame();
    LoadAeroFrameSmall();

    // Phase 3: spawn the task buttons as children of the flyout window.
    CreateFlyoutButtons(g_flyoutHwnd);

    // Now that themes are loaded and buttons exist, force a synchronous
    // paint into the off-screen presentation surface. By the time
    // ShowFlyoutForTile moves the window on-screen, the dark themed bg
    // and all child buttons are already rendered.
    RedrawWindow(g_flyoutHwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);

    // Measure the DWM drop-shadow extents while the window is parked
    // off-screen. Shadow size is constant for the lifetime of the
    // window (driven by style + DWM theme), so we cache once.
    RECT winRc = {};
    RECT dwmRc = {};
    GetWindowRect(g_flyoutHwnd, &winRc);
    if (SUCCEEDED(DwmGetWindowAttribute(g_flyoutHwnd,
                                        9 /*DWMWA_EXTENDED_FRAME_BOUNDS*/,
                                        &dwmRc, sizeof(dwmRc)))) {
        // The extended-frame rect is the visible-with-shadow rect; positive
        // values mean shadow extends outside GetWindowRect on that side.
        long sl = winRc.left   - dwmRc.left;
        long st = winRc.top    - dwmRc.top;
        long sr = dwmRc.right  - winRc.right;
        long sb = dwmRc.bottom - winRc.bottom;
        g_shadowLeft   = (sl > 0) ? (int)sl : 0;
        g_shadowTop    = (st > 0) ? (int)st : 0;
        g_shadowRight  = (sr > 0) ? (int)sr : 0;
        g_shadowBottom = (sb > 0) ? (int)sb : 0;
        dbgprintf(L"DWM shadow extents: L=%d T=%d R=%d B=%d",
               g_shadowLeft, g_shadowTop, g_shadowRight, g_shadowBottom);
    }
}

// Right-click context menu on the user tile. Items mirror repensky's
// baseline mod (Log off [name]... / Switch user / Lock / Open User
// Accounts) and the actions match the flyout's task-button handlers
// exactly so the two entry points are interchangeable.
static void ShowUserTileContextMenu(HWND tileHwnd) {
    if (!tileHwnd || !IsWindow(tileHwnd)) return;

    // Refresh the username so "Log off [name]..." reflects whoever's
    // logged in *now*, not whatever was cached at last flyout open.
    RefreshUserName();

    // Dismiss any visible tooltip — same hand-off as flyout open, so
    // the tooltip doesn't linger on top of the popup menu.
    UserTile_DismissTooltip();

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    wchar_t logoffText[ARRAYSIZE(g_userName) + 16];
    swprintf_s(logoffText, ARRAYSIZE(logoffText),
               UT_Text(UTS_LOG_OFF_FMT), g_userName);

    AppendMenuW(hMenu, MF_STRING, 1, logoffText);
    AppendMenuW(hMenu, MF_STRING, 2, UT_Text(UTS_SWITCH_USER));
    AppendMenuW(hMenu, MF_STRING, 3, UT_Text(UTS_LOCK));
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, 4, UT_Text(UTS_OPEN_USER_ACCT));

    POINT pt;
    GetCursorPos(&pt);

    // SetForegroundWindow before TrackPopupMenu — required so the menu
    // dismisses correctly when the user clicks outside it.
    SetForegroundWindow(tileHwnd);

    UINT cmd = TrackPopupMenu(hMenu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y, 0, tileHwnd, nullptr);

    DestroyMenu(hMenu);

    // Repensky's "menu sometimes doesn't dismiss" workaround — post a
    // no-op so the next message-pump cycle clears any stuck capture.
    PostMessageW(tileHwnd, WM_NULL, 0, 0);

    switch (cmd) {
        case 1:  // Log off [name]...
            ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_MAJOR_OTHER);
            break;
        case 2:  // Switch user
            if (!WTSDisconnectSession(WTS_CURRENT_SERVER_HANDLE,
                                       WTS_CURRENT_SESSION, FALSE)) {
                dbgprintf(L"Context menu: WTSDisconnectSession failed "
                       L"(LE=%lu) — falling back to LockWorkStation",
                       GetLastError());
                LockWorkStation();
            }
            break;
        case 3:  // Lock
            LockWorkStation();
            break;
        case 4:  // Open User Accounts
            ShellExecuteW(nullptr, L"open", L"control.exe",
                          L"/name Microsoft.UserAccounts",
                          nullptr, SW_SHOWNORMAL);
            break;
    }
}

static void ShowFlyoutForTile(HWND tileHwnd) {
    // Refresh user state (name / admin / guest / password) FIRST, before
    // any flyout-window operation. This way every paint downstream —
    // including the rebuild-path pre-paint inside EnsureFlyoutCreated —
    // sees current values. Without this ordering, a rebuild (e.g. after
    // compactMode toggled) would pre-paint with stale defaults; the
    // visible reveal still ends up correct because of the second
    // RedrawWindow below, but moving the refresh up makes the
    // refresh-on-open contract explicit and removes the wasted paint.
    RefreshUserState();

    // Settings-change rebuild happens here so the destroy + cache-clear
    // run on the tray thread (the thread that owns the flyout window).
    // Class registrations stay alive — EnsureFlyoutCreated reuses them,
    // so we don't trip ERROR_CLASS_ALREADY_EXISTS.
    if (g_flyoutNeedsRebuild) {
        g_flyoutNeedsRebuild = false;
        dbgprintf(L"Tearing down flyout for compactMode rebuild");
        if (g_flyoutHwnd && IsWindow(g_flyoutHwnd)) {
            DestroyWindow(g_flyoutHwnd);
        }
        g_flyoutHwnd = nullptr;
        // Drop user-picture cache so RefreshUserPicture re-decodes at the
        // new USERPIC_W × USERPIC_H.
        if (g_userPictureBmp) {
            DeleteObject(g_userPictureBmp);
            g_userPictureBmp = nullptr;
        }
        g_userPictureSrcPath[0] = L'\0';
        g_userPictureSrcMtime  = {};
        g_userPictureCacheValid = false;
        // Drop both frame caches, they are sized to the old AERO_W
        // Reusing one at the wrong size would read past the end in WM_PAINT
        if (g_aeroFrameBmp) {
            DeleteObject(g_aeroFrameBmp);
            g_aeroFrameBmp = nullptr;
        }
        g_aeroFrameTried = false;
        if (g_aeroFrameSmallBmp) {
            DeleteObject(g_aeroFrameSmallBmp);
            g_aeroFrameSmallBmp = nullptr;
        }
        g_aeroFrameSmallTried = false;
        // Theme handles are tied to the destroyed hwnd; reopen on rebuild.
        if (g_flyoutTheme)   { CloseThemeData(g_flyoutTheme);   g_flyoutTheme   = nullptr; }
        if (g_chinTheme)     { CloseThemeData(g_chinTheme);     g_chinTheme     = nullptr; }
        if (g_listViewTheme) { CloseThemeData(g_listViewTheme); g_listViewTheme = nullptr; }
    }

    EnsureFlyoutCreated();
    if (!g_flyoutHwnd) return;

    // Dismiss the native usertile tooltip if it's currently visible —
    // otherwise it lingers on top of (or behind) the flyout. The
    // native tooltip is a TTF_TRACK tooltip and the dismiss helper
    // sends TTM_TRACKACTIVATE FALSE + TTM_POP + SW_HIDE.
    UserTile_DismissTooltip();

    // RefreshUserState ran at the top of this function. Push the
    // refreshed display name into the native tile tooltip too — covers
    // the case where the user renamed the account in Control Panel
    // without triggering a _UpdateUserInfo flush.

    // Phase 5 refresh: cheap path-and-mtime check; only re-decodes if the
    // user changed their account picture (or the source file rotated).
    RefreshUserPicture();

    // Reopen any theme handles that WM_THEMECHANGED dropped while the
    // flyout was hidden. No-op if everything's still cached.
    EnsureFlyoutThemes();

    RedrawWindow(g_flyoutHwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);

    // Position with a 7px gap from the *work area* — exactly the .NET
    // TaskPopup formula (Screen.PrimaryScreen.WorkingArea.Right - W - 7).
    // mi.rcWork already excludes the taskbar AND any other reserved
    // edge bands (e.g. sidebar/gadget panels), which is the correct
    // semantic for "respect work area".
    APPBARDATA abd = {};
    abd.cbSize = sizeof(abd);
    UINT edge = ABE_BOTTOM;
    if (SHAppBarMessage(ABM_GETTASKBARPOS, &abd)) {
        edge = abd.uEdge;
    }

    HMONITOR hMon = MonitorFromWindow(tileHwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) return;

    // Start from mi.rcWork (which captures non-taskbar reserved bands like
    // a right-side sidebar/gadget panel), but override the taskbar-adjacent
    // edge with the live abd.rc value. mi.rcWork can lag behind a taskbar
    // resize (large/small icons toggle, taller taskbar drag), but
    // SHAppBarMessage(ABM_GETTASKBARPOS) always returns the current rect.
    RECT wa = mi.rcWork;
    switch (edge) {
        case ABE_BOTTOM: wa.bottom = abd.rc.top;    break;
        case ABE_TOP:    wa.top    = abd.rc.bottom; break;
        case ABE_LEFT:   wa.left   = abd.rc.right;  break;
        case ABE_RIGHT:  wa.right  = abd.rc.left;   break;
    }

    // Position calc accounts for DWM drop shadow: shift the GetWindowRect
    // inward by g_shadow* on each anchored edge so the *visible-with-shadow*
    // bounds (which the user perceives as the flyout's edge) end up
    // FLYOUT_MARGIN from the work-area edges.
    // This window is created once and reused, so it outlives a composition
    // toggle, recheck before measuring or the frame goes stale
    BOOL frameChanged = ApplyFlyoutFrame(g_flyoutHwnd);

    // FLYOUT_W and FLYOUT_H are the content box, the thick frame sits outside
    // Grow to the window size those styles need before placing anything
    RECT frameRc = { 0, 0, FLYOUT_W, FLYOUT_H };
    AdjustWindowRectEx(&frameRc,
                       (DWORD)GetWindowLongPtrW(g_flyoutHwnd, GWL_STYLE), FALSE,
                       (DWORD)GetWindowLongPtrW(g_flyoutHwnd, GWL_EXSTYLE));
    int winW = frameRc.right - frameRc.left;
    int winH = frameRc.bottom - frameRc.top;

    // Same float every other system flyout uses, see FlyoutFix.cpp
    int margin = NativeFlyoutGap();

    int x = 0, y = 0;
    switch (edge) {
        case ABE_BOTTOM:
            x = wa.right  - winW - margin - g_shadowRight;
            y = wa.bottom - winH - margin - g_shadowBottom;
            break;
        case ABE_LEFT:
            x = wa.left   + margin + g_shadowLeft;
            y = wa.bottom - winH - margin - g_shadowBottom;
            break;
        case ABE_RIGHT:
            x = wa.right  - winW - margin - g_shadowRight;
            y = wa.bottom - winH - margin - g_shadowBottom;
            break;
        case ABE_TOP:
            x = wa.right  - winW - margin - g_shadowRight;
            y = wa.top    + margin + g_shadowTop;
            break;
    }

    // No manual nudge here any more, NativeFlyoutGap carries the offset
    // Nudging on top of it pushed the corner one pixel past the native ones

    dbgprintf(L"Position calc: edge=%u wa=(%ld,%ld,%ld,%ld) "
           L"tb=(%ld,%ld,%ld,%ld) shadow=(L%d T%d R%d B%d) → (%d,%d)",
           edge, wa.left, wa.top, wa.right, wa.bottom,
           abd.rc.left, abd.rc.top, abd.rc.right, abd.rc.bottom,
           g_shadowLeft, g_shadowTop, g_shadowRight, g_shadowBottom, x, y);

    // Window is already WS_VISIBLE + WS_EX_LAYERED, pre-painted with the
    // dark theme into DWM's redirection bitmap. Move to the target
    // position, then bump alpha 0→255 to reveal the already-rendered
    // content. The alpha flip is instant (no DWM compose-cold-start).
    UINT swpFlags = SWP_NOACTIVATE;
    if (frameChanged) swpFlags |= SWP_FRAMECHANGED;
    SetWindowPos(g_flyoutHwnd, HWND_TOPMOST, x, y, winW, winH, swpFlags);
    SetLayeredWindowAttributes(g_flyoutHwnd, 0, 255, LWA_ALPHA);
    SetForegroundWindow(g_flyoutHwnd);
    g_flyoutShown = true;
    // Diagnostic: log the actual on-screen window rect after positioning
    // (after SetWindowPos commits) and the DWM extended frame bounds, so
    // we can verify the visible-frame gap is what we intended. The
    // difference between GetWindowRect and DWMWA_EXTENDED_FRAME_BOUNDS
    // is the invisible-margin / drop-shadow area.
    RECT winRc = {};
    GetWindowRect(g_flyoutHwnd, &winRc);
    RECT dwmRc = {};
    DwmGetWindowAttribute(g_flyoutHwnd, 9 /*DWMWA_EXTENDED_FRAME_BOUNDS*/,
                          &dwmRc, sizeof(dwmRc));
    dbgprintf(L"  After move: window=(%ld,%ld,%ld,%ld) "
           L"dwmExtFrame=(%ld,%ld,%ld,%ld)",
           winRc.left, winRc.top, winRc.right, winRc.bottom,
           dwmRc.left, dwmRc.top, dwmRc.right, dwmRc.bottom);
}


// -----------------------------------------------------------------
// Layout / settings: pick between normal and compact mode.
// -----------------------------------------------------------------
// Recompute layout variables from g_compactMode. Sets every dimension
// the flyout depends on (popup size, frame size + position, picture size
// + position, and the derived button stack Y values). WM_PAINT also
// branches on g_compactMode for content the layout vars don't cover
// (header label visibility, username/type/password label positions).
static void RecomputeLayout() {
    // Frame outer rect first; picture is the same rect inset by the
    // shrink-% on each side. Both modes follow 7850 CUserPane::_UpdateDC's
    // composite (BitBlt picture → AlphaBlend frame on top), but the
    // picture is smaller than the frame so the bevel ring overlays
    // empty background rather than clipping into the photo edges.
    if (g_compactMode) {
        FLYOUT_W   = 276;
        FLYOUT_H   = 226;       // 336 - 110
        AERO_W     = AERO_SMALL_SIZE;
        AERO_H     = AERO_SMALL_SIZE;
        AERO_X     = 6;
        AERO_Y     = 6;

        int shrunk = static_cast<int>(
            AERO_W * (1.0f - kCompactPicShrinkPct / 100.0f) + 0.5f);
        USERPIC_W = USERPIC_H = shrunk;
        USERPIC_X = AERO_X + (AERO_W - USERPIC_W) / 2 + kCompactPicNudgeX;
        USERPIC_Y = AERO_Y + (AERO_H - USERPIC_H) / 2 + kCompactPicNudgeY;
    } else {
        // Normal mode: 155×155 frame anchored at (-8, 20) — the .NET
        // TileWithFrameControl "overhang" position the original v2.8.0
        // design used. The negative AERO_X lets the frame extend past
        // the popup's left edge (the bevel's left half is fully
        // transparent), keeping the 142 px label column clear and
        // leaving vertical room above the lock button (BTN_LOCK_Y=190).
        FLYOUT_W   = 276;
        FLYOUT_H   = 336;
        AERO_W     = 155;
        AERO_H     = 155;
        AERO_X     = -8;
        AERO_Y     = 20;

        int shrunk = static_cast<int>(
            AERO_W * (1.0f - kNormalPicShrinkPct / 100.0f) + 0.5f);
        USERPIC_W = USERPIC_H = shrunk;
        USERPIC_X = AERO_X + (AERO_W - USERPIC_W) / 2;
        USERPIC_Y = AERO_Y + (AERO_H - USERPIC_H) / 2;
    }

    // Derived: button stack tracks FOOTER_TOP, which tracks FLYOUT_H.
    FOOTER_TOP    = FLYOUT_H - FOOTER_H;
    BTN_W         = FLYOUT_W - BTN_X - BTN_RIGHT_MARGIN;
    BTN_SWITCH_Y  = FOOTER_TOP - BTN_H - BTN_BOTTOM_GAP;
    BTN_LOG_OFF_Y = BTN_SWITCH_Y - BTN_H;
    BTN_LOCK_Y    = BTN_LOG_OFF_Y - BTN_H;
}

// Read the compactMode Windhawk setting and apply it to the layout vars.
// Called at Wh_ModInit and on Wh_ModSettingsChanged.
static void RefreshSettings() {
    g_compactMode = (IsCompactUserTile());
    RecomputeLayout();
    dbgprintf(L"Settings refreshed: compactMode=%d", g_compactMode ? 1 : 0);
}

//---Public entry points------------------------------------

// Everything above is the flyout window itself, lifted from the mod. These
// are the only things UserTile.cpp calls into

void UserTile_ShowFlyout(HWND tileHwnd)
{
	// Re-read CompactUserTile on every open so a registry change lands
	// without restarting the shell. A change of mode needs the window
	// rebuilt, and ShowFlyoutForTile does that on the tray thread where
	// DestroyWindow actually works
	bool wasCompact = g_compactMode;
	RefreshSettings();
	if (g_compactMode != wasCompact)
		g_flyoutNeedsRebuild = true;

	ShowFlyoutForTile(tileHwnd);
}

void UserTile_ShowContextMenu(HWND tileHwnd)
{
	RefreshSettings();
	ShowUserTileContextMenu(tileHwnd);
}

bool UserTile_FlyoutIsShown(void)
{
	return g_flyoutShown;
}

void UserTile_SetFlyoutWasVisible(bool wasVisible)
{
	g_flyoutWasVisibleAtMouseDown = wasVisible;
}

// Reads and consumes in one go, the caller only ever wants it once
bool UserTile_TakeFlyoutWasVisible(void)
{
	bool wasVisible = g_flyoutWasVisibleAtMouseDown;
	g_flyoutWasVisibleAtMouseDown = false;
	return wasVisible;
}

void UserTile_InvalidatePictureCache(void)
{
	g_userPictureCacheValid = false;
}

void UserTile_DestroyFlyout(void)
{
	if (g_flyoutHwnd && IsWindow(g_flyoutHwnd))
		DestroyWindow(g_flyoutHwnd);
	g_flyoutHwnd = nullptr;

	if (g_flyoutClass != 0)
	{
		UnregisterClassW(FLYOUT_CLASS_NAME, GetModuleHandleW(nullptr));
		g_flyoutClass = 0;
	}
	if (g_taskBtnClass != 0)
	{
		UnregisterClassW(TASKBTN_CLASS_NAME, GetModuleHandleW(nullptr));
		g_taskBtnClass = 0;
	}

	if (g_fontRegular)          { DeleteObject(g_fontRegular);          g_fontRegular = nullptr; }
	if (g_fontRegularUnderline) { DeleteObject(g_fontRegularUnderline); g_fontRegularUnderline = nullptr; }
	if (g_fontHeader)           { DeleteObject(g_fontHeader);           g_fontHeader = nullptr; }
	if (g_fontHeaderUnderline)  { DeleteObject(g_fontHeaderUnderline);  g_fontHeaderUnderline = nullptr; }

	if (g_textStyleTheme) { CloseThemeData(g_textStyleTheme); g_textStyleTheme = nullptr; }
	g_textStyleTried = false;

	if (g_userPictureBmp) { DeleteObject(g_userPictureBmp); g_userPictureBmp = nullptr; }
	g_userPictureCacheValid = false;

	if (g_aeroFrameBmp)      { DeleteObject(g_aeroFrameBmp);      g_aeroFrameBmp = nullptr; }
	g_aeroFrameTried = false;
	if (g_aeroFrameSmallBmp) { DeleteObject(g_aeroFrameSmallBmp); g_aeroFrameSmallBmp = nullptr; }
	g_aeroFrameSmallTried = false;
}
