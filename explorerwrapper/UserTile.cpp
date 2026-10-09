#include "UserTile.h"
#include <objbase.h>
#include <wincodec.h>
#include <uxtheme.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <shellapi.h>
#include <sddl.h>
#include "MinHook.h"

// StrokeBackground is BGTYPE BORDERFILL with BORDERSIZE 0, so it is a plain
// fill at the frame rect and the picture covers all but a 1px ring of it
#define STROKE_FILL_COLOUR RGB(255, 255, 255)

// Shared with the flyout, set the first time a hook sees the tile object
void* g_pUserTileObj = nullptr;

// Explorer\Advanced, CompactUserTile. Read on every open so a change lands
// without restarting the shell
bool IsCompactUserTile()
{
	DWORD dwCompact = 0;
	g_registry.QueryValue(L"CompactUserTile", (LPBYTE)&dwCompact, sizeof(DWORD));
	return dwCompact == 1;
}

//---Cached picture state-----------------------------------

static HBITMAP g_srcBmp = nullptr;      // our decoded source, we own it
static HBITMAP g_blitBmp = nullptr;     // last handle we wrote to UT_PIC_BLIT

static int     g_blitW = 0;
static int     g_blitH = 0;
static HBITMAP g_blitSrc = nullptr;     // source the blit was built from

//---Theme handle-------------------------------------------

// explorer7 hooks uxtheme OpenThemeData and forces tray thread callers onto
// its bundled aero, which has TrayNotify but no UserTile. Opening from any
// other thread bypasses that hook and gets the real active theme. Handles are
// process wide so the result stays valid on the tray thread
struct ThemeReq { HWND hwnd; LPCWSTR cls; HTHEME out; };

static DWORD WINAPI ThemeWorker(LPVOID p)
{
    ThemeReq* r = (ThemeReq*)p;
    r->out = OpenThemeData(r->hwnd, r->cls);
    return 0;
}

static HTHEME OpenThemeOffThread(HWND hwnd, LPCWSTR cls)
{
    ThemeReq* r = new ThemeReq{ hwnd, cls, nullptr };
    HANDLE t = CreateThread(nullptr, 0, ThemeWorker, r, 0, nullptr);
    if (!t) { delete r; return nullptr; }

    HTHEME result = nullptr;
    if (WaitForSingleObject(t, 2000) == WAIT_OBJECT_0)
    {
        result = r->out;
        delete r;
    }
    // On timeout leak the request, the worker may still write to it
    CloseHandle(t);
    return result;
}

//---WIC decoding-------------------------------------------

// Pulls frame 0, converts to PBGRA, scales to w by h, copies into a DIB
static HBITMAP DecodeDecoder(IWICImagingFactory* factory,
                             IWICBitmapDecoder* decoder, int w, int h)
{
    HBITMAP result = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    if (FAILED(decoder->GetFrame(0, &frame)))
        return nullptr;

    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(factory->CreateFormatConverter(&conv)))
    {
        if (SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
                                       WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom)))
        {
            IWICBitmapScaler* scaler = nullptr;
            if (SUCCEEDED(factory->CreateBitmapScaler(&scaler)))
            {
                if (SUCCEEDED(scaler->Initialize(conv, w, h,
                                                 WICBitmapInterpolationModeFant)))
                {
                    BITMAPINFO bi = {};
                    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                    bi.bmiHeader.biWidth = w;
                    bi.bmiHeader.biHeight = -h;     // top down
                    bi.bmiHeader.biPlanes = 1;
                    bi.bmiHeader.biBitCount = 32;
                    bi.bmiHeader.biCompression = BI_RGB;

                    void* bits = nullptr;
                    HDC screen = GetDC(nullptr);
                    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS,
                                                   &bits, nullptr, 0);
                    ReleaseDC(nullptr, screen);

                    if (dib && bits)
                    {
                        UINT stride = (UINT)w * 4;
                        if (SUCCEEDED(scaler->CopyPixels(nullptr, stride,
                                                         stride * (UINT)h,
                                                         (BYTE*)bits)))
                        {
                            result = dib;
                            dib = nullptr;
                        }
                    }
                    if (dib) DeleteObject(dib);
                }
                if (scaler) scaler->Release();
            }
        }
        conv->Release();
    }
    frame->Release();
    return result;
}

static HBITMAP DecodeBytes(const BYTE* bytes, DWORD size, int w, int h)
{
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool uninit = (hrInit == S_OK);

    HBITMAP result = nullptr;
    IWICImagingFactory* factory = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
    {
        IWICStream* stream = nullptr;
        if (SUCCEEDED(factory->CreateStream(&stream)))
        {
            if (SUCCEEDED(stream->InitializeFromMemory((BYTE*)bytes, size)))
            {
                IWICBitmapDecoder* dec = nullptr;
                if (SUCCEEDED(factory->CreateDecoderFromStream(
                        stream, nullptr, WICDecodeMetadataCacheOnLoad, &dec)))
                {
                    result = DecodeDecoder(factory, dec, w, h);
                    dec->Release();
                }
            }
            stream->Release();
        }
        factory->Release();
    }
    if (uninit) CoUninitialize();
    return result;
}

static HBITMAP DecodeFile(LPCWSTR path, int w, int h)
{
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool uninit = (hrInit == S_OK);

    HBITMAP result = nullptr;
    IWICImagingFactory* factory = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
    {
        IWICBitmapDecoder* dec = nullptr;
        if (SUCCEEDED(factory->CreateDecoderFromFilename(
                path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)))
        {
            result = DecodeDecoder(factory, dec, w, h);
            dec->Release();
        }
        factory->Release();
    }
    if (uninit) CoUninitialize();
    return result;
}

//---Account picture lookup---------------------------------

typedef HRESULT(WINAPI* SHGetUserPictureBytes_t)(LPCWSTR, LPCWSTR, DWORD,
                                                 LPVOID*, DWORD*, LPWSTR, DWORD);
typedef HRESULT(WINAPI* SHGetUserPicturePath_t)(LPCWSTR, DWORD, LPWSTR, DWORD,
                                                LPWSTR, DWORD);

// Current user SID as a string, caller frees with LocalFree
static LPWSTR CurrentUserSid()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return nullptr;

    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    LPWSTR sid = nullptr;
    if (needed)
    {
        TOKEN_USER* tu = (TOKEN_USER*)LocalAlloc(LPTR, needed);
        if (tu)
        {
            if (GetTokenInformation(token, TokenUser, tu, needed, &needed))
                ConvertSidToStringSidW(tu->User.Sid, &sid);
            LocalFree(tu);
        }
    }
    CloseHandle(token);
    return sid;
}

// Freshest Image* value the shell recorded for this account
static bool PictureFromRegistry(WCHAR out[MAX_PATH])
{
    LPWSTR sid = CurrentUserSid();
    if (!sid) return false;

    WCHAR key[320];
    wnsprintfW(key, ARRAYSIZE(key),
               L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AccountPicture\\Users\\%s",
               sid);
    LocalFree(sid);

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;

    static const LPCWSTR kNames[] = {
        L"Image1080", L"Image448", L"Image240", L"Image192",
        L"Image96", L"Image64", L"Image48", L"Image40", L"Image32",
    };

    WCHAR best[MAX_PATH] = L"";
    FILETIME bestTime = {};

    for (int i = 0; i < ARRAYSIZE(kNames); i++)
    {
        WCHAR buf[MAX_PATH] = L"";
        DWORD cb = sizeof(buf), type = 0;
        if (RegQueryValueExW(hKey, kNames[i], nullptr, &type,
                             (BYTE*)buf, &cb) != ERROR_SUCCESS)
            continue;
        if ((type != REG_SZ && type != REG_EXPAND_SZ) || !buf[0])
            continue;

        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        if (!GetFileAttributesExW(buf, GetFileExInfoStandard, &fad))
            continue;
        if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        if (!best[0] || CompareFileTime(&fad.ftLastWriteTime, &bestTime) > 0)
        {
            bestTime = fad.ftLastWriteTime;
            StrCpyNW(best, buf, MAX_PATH);
        }
    }
    RegCloseKey(hKey);

    if (!best[0]) return false;
    StrCpyNW(out, best, MAX_PATH);
    return true;
}

// Tries every source the shell uses, in the order the shell uses them
// ProgramData is not always on C, so ask the shell where it is
static bool ProgramDataPath(WCHAR out[MAX_PATH])
{
    out[0] = L'\0';
    return SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA,
                                      nullptr, SHGFP_TYPE_CURRENT, out))
           && out[0];
}

static HBITMAP LoadAccountPicture(int size)
{
    HMODULE shell32 = GetModuleHandleW(L"shell32.dll");

    if (shell32)
    {
        auto pBytes = (SHGetUserPictureBytes_t)
            GetProcAddress(shell32, "SHGetUserPictureBytes");
        if (pBytes)
        {
            static const LPCWSTR kFormats[] = { L"bmp", L"png", L"jpg" };
            for (int i = 0; i < ARRAYSIZE(kFormats); i++)
            {
                LPVOID data = nullptr;
                DWORD cb = 0;
                if (SUCCEEDED(pBytes(nullptr, kFormats[i], 1, &data, &cb, nullptr, 0))
                    && data && cb)
                {
                    HBITMAP bmp = DecodeBytes((BYTE*)data, cb, size, size);
                    LocalFree(data);
                    if (bmp)
                    {
                        dbgprintf(L"picture from SHGetUserPictureBytes (%s)", kFormats[i]);
                        return bmp;
                    }
                }
                else if (data)
                {
                    LocalFree(data);
                }
            }
        }

        auto pPath = (SHGetUserPicturePath_t)
            GetProcAddress(shell32, "SHGetUserPicturePath");
        if (!pPath)
            pPath = (SHGetUserPicturePath_t)GetProcAddress(shell32, MAKEINTRESOURCEA(261));
        if (pPath)
        {
            WCHAR path[MAX_PATH] = L"";
            if (SUCCEEDED(pPath(nullptr, 0, path, MAX_PATH, nullptr, 0)) && path[0])
            {
                HBITMAP bmp = DecodeFile(path, size, size);
                if (bmp)
                {
                    dbgprintf(L"picture from SHGetUserPicturePath: %s", path);
                    return bmp;
                }
            }
        }
    }

    WCHAR regPath[MAX_PATH] = L"";
    if (PictureFromRegistry(regPath))
    {
        HBITMAP bmp = DecodeFile(regPath, size, size);
        if (bmp)
        {
            dbgprintf(L"picture from registry: %s", regPath);
            return bmp;
        }
    }

    WCHAR progData[MAX_PATH];
    if (ProgramDataPath(progData))
    {
        static const LPCWSTR kDefaultNames[] = { L"user.png", L"user.bmp" };
        for (int i = 0; i < ARRAYSIZE(kDefaultNames); i++)
        {
            WCHAR cand[MAX_PATH];
            wnsprintfW(cand, ARRAYSIZE(cand),
                       L"%s\\Microsoft\\User Account Pictures\\%s",
                       progData, kDefaultNames[i]);
            HBITMAP bmp = DecodeFile(cand, size, size);
            if (bmp)
            {
                dbgprintf(L"picture from default: %s", cand);
                return bmp;
            }
        }
    }

    dbgprintf(L"no account picture found from any source");
    return nullptr;
}

// Rescales the cached source into a fresh DIB of exactly w by h
static HBITMAP ScaleSource(int w, int h)
{
    if (!g_srcBmp || w <= 0 || h <= 0) return nullptr;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib)
    {
        ReleaseDC(nullptr, screen);
        return nullptr;
    }

    HDC dstDC = CreateCompatibleDC(screen);
    HDC srcDC = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);

    if (dstDC && srcDC)
    {
        HGDIOBJ oldDst = SelectObject(dstDC, dib);
        HGDIOBJ oldSrc = SelectObject(srcDC, g_srcBmp);
        SetStretchBltMode(dstDC, HALFTONE);
        SetBrushOrgEx(dstDC, 0, 0, nullptr);
        StretchBlt(dstDC, 0, 0, w, h, srcDC, 0, 0, UT_SRC_SIZE, UT_SRC_SIZE, SRCCOPY);
        SelectObject(srcDC, oldSrc);
        SelectObject(dstDC, oldDst);
    }
    if (dstDC) DeleteDC(dstDC);
    if (srcDC) DeleteDC(srcDC);
    return dib;
}

//---Tooltip------------------------------------------------

// 7850 builds the tooltip in _OnCreate, but that sits after the user tile
// store call, which fails on Windows 10, so the whole block is skipped and
// the tile is left with no tooltip window at all.
//
// Everything that drives it still runs though. _OnMouseMove arms a timer,
// _OnTimer shows and later auto hides, WM_MOUSELEAVE cancels, and the
// TTN_SHOW handler positions it against the tray edge. All of that just
// posts to a null handle today. Building the window and filling the object
// members is enough to hand the native code something to talk to

typedef HRESULT (WINAPI* GetUserDisplayName_t)(LPWSTR, DWORD*);

// The object pointer, so the tile subclass can reach the tooltip members


static LRESULT CALLBACK TooltipSubclassProc(HWND, UINT, WPARAM, LPARAM,
                                            UINT_PTR, DWORD_PTR);

// Same source 7850 uses, shell32 ordinal 241, with the login name as the
// fallback in place of its resource string
static LPWSTR AllocUserDisplayName()
{
    WCHAR buf[513] = L"";
    DWORD cch = ARRAYSIZE(buf);

    HMODULE shell32 = GetModuleHandleW(L"shell32.dll");
    auto pName = shell32 ? (GetUserDisplayName_t)
        GetProcAddress(shell32, MAKEINTRESOURCEA(241)) : nullptr;

    if (!pName || FAILED(pName(buf, &cch)) || !buf[0])
    {
        cch = ARRAYSIZE(buf);
        if (!GetUserNameW(buf, &cch) || !buf[0])
            return nullptr;
    }

    // CoTaskMem because _UpdateUserInfo frees this member that way
    size_t cb = (wcslen(buf) + 1) * sizeof(WCHAR);
    LPWSTR out = (LPWSTR)CoTaskMemAlloc(cb);
    if (out) memcpy(out, buf, cb);
    return out;
}

static void EnsureTooltip(void* thisPtr)
{
    HWND* pTip = (HWND*)UT_Field(thisPtr, UT_TOOLTIP_HWND);
    if (*pTip && IsWindow(*pTip))
        return;

    HWND tile = *(HWND*)UT_Field(thisPtr, UT_TILE_HWND);
    if (!tile)
        return;

    // The tool info points straight at this buffer, so it has to outlive us
    LPWSTR* pName = (LPWSTR*)UT_Field(thisPtr, UT_NAME_PTR);
    if (!*pName)
        *pName = AllocUserDisplayName();
    if (!*pName)
        return;

    // 7850 passes no extended style here. Shell_TrayWnd is topmost, and being
    // owned by it does not put us in the same band, so the tip lands under
    // the taskbar. Asking for topmost up front is what keeps it above,
    // rather than setting it per show and letting the control demote us
    HWND tip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                               TOOLTIPS_CLASSW, nullptr,
                               WS_POPUP | TTS_ALWAYSTIP,
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               tile, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    if (!tip)
    {
        dbgprintf(L"tooltip window creation failed (LE=%lu)", GetLastError());
        return;
    }
    *pTip = tip;

    // Real 7850 tips carry a drop shadow that falls across the taskbar. That
    // comes from CS_DROPSHADOW, which lives on the window class rather than
    // the window, so nothing set on the hwnd can produce it. Folding it into
    // the class is the only route, and it is a no-op when already present.
    // The system SPI_SETDROPSHADOW setting still has the final say
    ULONG_PTR clsStyle = GetClassLongPtrW(tip, GCL_STYLE);
    if (!(clsStyle & CS_DROPSHADOW))
    {
        SetClassLongPtrW(tip, GCL_STYLE, clsStyle | CS_DROPSHADOW);
        dbgprintf(L"added CS_DROPSHADOW to the tooltip class");
    }

    // Track plus absolute is what lets the TTN_SHOW handler place it at the
    // tray corner instead of following the pointer
    TOOLINFOW* ti = (TOOLINFOW*)UT_Field(thisPtr, UT_TOOLINFO);
    ti->cbSize   = sizeof(TOOLINFOW);
    ti->uFlags   = TTF_IDISHWND | TTF_TRACK | TTF_ABSOLUTE;
    ti->hwnd     = tile;
    ti->uId      = (UINT_PTR)tile;
    ti->lpszText = *pName;
    SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)ti);

    SetWindowSubclass(tip, TooltipSubclassProc, 2, 0);

    dbgprintf(L"tooltip created for \"%s\"", *pName);
}

// What _ToggleToolTip(false) does. The native call is unreachable here
// because it sits behind the null flyout object test in _OnShowFlyout
static void DismissTooltip()
{
    if (!g_pUserTileObj)
        return;

    HWND tip  = *(HWND*)UT_Field(g_pUserTileObj, UT_TOOLTIP_HWND);
    HWND tile = *(HWND*)UT_Field(g_pUserTileObj, UT_TILE_HWND);
    if (!tip || !tile)
        return;

    KillTimer(tile, 1);
    *(BYTE*)UT_Field(g_pUserTileObj, UT_TIP_PENDING) = 0;
    KillTimer(tile, 2);
    *(BYTE*)UT_Field(g_pUserTileObj, UT_TIP_EXPIRED) = 0;

    // The 7785 mod's three step dismissal. TTM_POP alone is a no-op against
    // a tracking tooltip, and the hide stops the user seeing it fade out
    // from behind whatever just took focus
    SendMessageW(tip, TTM_TRACKACTIVATE, FALSE,
                 (LPARAM)UT_Field(g_pUserTileObj, UT_TOOLINFO));
    SendMessageW(tip, TTM_POP, 0, 0);
    ShowWindow(tip, SW_HIDE);
}

// The flyout and the context menu both hand off from a visible tooltip
void UserTile_DismissTooltip(void)
{
    DismissTooltip();
}

// Clamp, lifted from the 7785 mod. The native code uses TTF_TRACK with
// absolute coordinates and never clamps, so the tip can fall under the
// taskbar or off screen. rcWork excludes every reserved band, not just the
// taskbar, so this is also what keeps it clear of a second appbar
static LRESULT CALLBACK TooltipSubclassProc(HWND hwnd, UINT msg, WPARAM wp,
                                            LPARAM lp, UINT_PTR id, DWORD_PTR)
{
    if (msg == WM_WINDOWPOSCHANGING)
    {
        WINDOWPOS* wpos = (WINDOWPOS*)lp;

        // Force above the taskbar here rather than at creation. Every move
        // the control makes on its way to showing comes through this message,
        // including the one that would otherwise drop us back under the tray,
        // and the drop shadow follows whatever band its owner ends up in
        if (wpos)
        {
            wpos->hwndInsertAfter = HWND_TOPMOST;
            wpos->flags &= ~SWP_NOZORDER;
        }

        if (wpos && !(wpos->flags & SWP_NOMOVE))
        {
            int tipW = wpos->cx;
            int tipH = wpos->cy;
            if (wpos->flags & SWP_NOSIZE)
            {
                RECT cur;
                if (GetWindowRect(hwnd, &cur))
                {
                    tipW = cur.right - cur.left;
                    tipH = cur.bottom - cur.top;
                }
            }
            POINT proposed = { wpos->x, wpos->y };
            MONITORINFO mi = {};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(MonitorFromPoint(proposed, MONITOR_DEFAULTTONEAREST),
                                &mi))
            {
                if (wpos->x + tipW > mi.rcWork.right)  wpos->x = mi.rcWork.right - tipW;
                if (wpos->x < mi.rcWork.left)          wpos->x = mi.rcWork.left;
                if (wpos->y + tipH > mi.rcWork.bottom) wpos->y = mi.rcWork.bottom - tipH;
                if (wpos->y < mi.rcWork.top)           wpos->y = mi.rcWork.top;
            }
        }
    }
    else if (msg == WM_NCDESTROY)
    {
        RemoveWindowSubclass(hwnd, TooltipSubclassProc, id);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// Anchor from ReCalcToolTip, kept because the stock handler only refreshes
// its cached point after the tile has moved and shows at 0,0 before that.
// The lift out of the taskbar's topmost band is ours too, the stock
// SetWindowPos passes SWP_NOZORDER so it can only ever move the window
static void PositionTooltip(HWND tip)
{
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    RECT tr = {}, tipRc = {};
    if (!tray || !GetWindowRect(tray, &tr) || !GetWindowRect(tip, &tipRc))
        return;

    int w = tipRc.right - tipRc.left;
    int h = tipRc.bottom - tipRc.top;

    // Ask the control for the bubble it is about to draw. The window itself
    // has not always been sized to the text yet when TTN_SHOW arrives, and a
    // stale height throws the bottom edge off the taskbar by that much
    if (g_pUserTileObj)
    {
        DWORD bubble = (DWORD)SendMessageW(tip, TTM_GETBUBBLESIZE, 0,
                                           (LPARAM)UT_Field(g_pUserTileObj, UT_TOOLINFO));
        if (bubble)
        {
            w = LOWORD(bubble);
            h = HIWORD(bubble);
        }
    }

    APPBARDATA abd = {};
    abd.cbSize = sizeof(abd);
    UINT edge = SHAppBarMessage(ABM_GETTASKBARPOS, &abd) ? abd.uEdge : ABE_BOTTOM;

    int x, y;
    switch (edge)
    {
    case ABE_LEFT:
        x = tr.right + UT_TOOLTIP_GAP;
        y = tr.bottom - h;
        break;
    case ABE_TOP:
        x = tr.right - w;
        y = tr.bottom + UT_TOOLTIP_GAP;
        break;
    case ABE_RIGHT:
        x = tr.left - w - UT_TOOLTIP_GAP;
        y = tr.bottom - h;
        break;
    default:                                     // ABE_BOTTOM
        x = tr.right - w;
        y = tr.top - h - UT_TOOLTIP_GAP;
        break;
    }

    // Keep the track point itself inside the work area, so the answer does
    // not depend on the subclass clamp catching it afterwards
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromWindow(tray, MONITOR_DEFAULTTONEAREST), &mi))
    {
        if (x + w > mi.rcWork.right)  x = mi.rcWork.right - w;
        if (x < mi.rcWork.left)       x = mi.rcWork.left;
        if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
        if (y < mi.rcWork.top)        y = mi.rcWork.top;
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    // A TTF_TRACK tooltip positions itself from the point it was last given,
    // not from wherever the window was moved to, so a bare SetWindowPos gets
    // overwritten on the next show. TTF_ABSOLUTE makes this the top left
    SendMessageW(tip, TTM_TRACKPOSITION, 0, MAKELPARAM(x, y));
    SetWindowPos(tip, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

// The native _OnTimer sends TTM_TRACKACTIVATE right after this message, so
// refreshing the track point here lands it before every show
static void RefreshTooltipPosition()
{
    if (!g_pUserTileObj)
        return;
    HWND tip = *(HWND*)UT_Field(g_pUserTileObj, UT_TOOLTIP_HWND);
    if (tip && IsWindow(tip))
        PositionTooltip(tip);
}

//---Hooks--------------------------------------------------

using CalcTileSize_t = long(__thiscall*)(void*, RECT*);
using Paint_t = void(__thiscall*)(void*, HDC, RECT*);

static CalcTileSize_t CalcTileSize_Orig = nullptr;
static Paint_t Paint_Orig = nullptr;

long __thiscall CalcTileSize_Hook(void* thisPtr, RECT* prc);
static void EnsureTileSubclassed(HWND tile);

//---Account picture change events--------------------------

// Posted by the watcher thread when the shell rewrites the picture values
#define UT_MSG_PICCHANGED (WM_APP + 0x21)

static HANDLE g_picThread  = nullptr;
static HANDLE g_picStopEvt = nullptr;

// Frees every cached copy so the next paint reloads from disk
static void DropCachedPicture(void* obj, HWND tile)
{
    if (obj)
    {
        HBITMAP* pSource = (HBITMAP*)UT_Field(obj, UT_PIC_SOURCE);
        if (g_srcBmp && *pSource == g_srcBmp)
            *pSource = nullptr;
    }
    if (g_srcBmp)
    {
        DeleteObject(g_srcBmp);
        g_srcBmp = nullptr;
    }
    // The blit handle lives on the object, clear both or the paint keeps it
    if (obj)
    {
        HBITMAP* pBlit = (HBITMAP*)UT_Field(obj, UT_PIC_BLIT);
        // Only drop it when it is ours, the shell frees its own on rebuild
        if (g_blitBmp && *pBlit == g_blitBmp)
            *pBlit = nullptr;
    }
    if (g_blitBmp)
    {
        DeleteObject(g_blitBmp);
        g_blitBmp = nullptr;
    }
    g_blitW = 0;
    g_blitH = 0;
    UserTile_InvalidatePictureCache();
    if (tile)
        InvalidateRect(tile, nullptr, FALSE);
    dbgprintf(L"account picture changed, tile reloaded");
}

//---Watcher thread-----------------------------------------

// Every source below is an event, nothing here wakes on its own
// Each is armed on its own, so one missing store cannot silence the rest
static DWORD WINAPI PictureWatchThread(LPVOID param)
{
    HWND tile = (HWND)param;

    // Which store Windows writes depends on how the picture was set, so
    // watch all three rather than betting on one
    static const DWORD kFilter =
        FILE_NOTIFY_CHANGE_FILE_NAME  | FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
        FILE_NOTIFY_CHANGE_CREATION   | FILE_NOTIFY_CHANGE_ATTRIBUTES;

    HANDLE dirs[3] = { nullptr, nullptr, nullptr };
    int    nDirs = 0;
    WCHAR  root[MAX_PATH];
    WCHAR  path[MAX_PATH];

    if (ProgramDataPath(root))
    {
        // Watched at the parent with the subtree flag, on purpose
        // The per SID folder does not exist until a picture is first set
        wnsprintfW(path, ARRAYSIZE(path), L"%s\\AccountPictures", root);
        HANDLE h = FindFirstChangeNotificationW(path, TRUE, kFilter);
        if (h != INVALID_HANDLE_VALUE) dirs[nDirs++] = h;

        // The legacy per user .dat, still what SHGetUserPictureBytes falls
        // back to when the account property store has nothing
        wnsprintfW(path, ARRAYSIZE(path), L"%s\\Microsoft\\User Account Pictures", root);
        h = FindFirstChangeNotificationW(path, FALSE, kFilter);
        if (h != INVALID_HANDLE_VALUE) dirs[nDirs++] = h;
    }
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr,
                                   SHGFP_TYPE_CURRENT, root)) && root[0])
    {
        // Where the picture the user picked is dropped before it is scaled
        wnsprintfW(path, ARRAYSIZE(path), L"%s\\Microsoft\\Windows\\AccountPictures", root);
        HANDLE h = FindFirstChangeNotificationW(path, FALSE, kFilter);
        if (h != INVALID_HANDLE_VALUE) dirs[nDirs++] = h;
    }

    // Opened at the Users parent, so a missing per SID subkey is survivable
    // and its creation is itself an event
    HKEY   hKey   = nullptr;
    HANDLE regEvt = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AccountPicture\\Users",
                      0, KEY_NOTIFY, &hKey) == ERROR_SUCCESS)
    {
        regEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!regEvt)
        {
            RegCloseKey(hKey);
            hKey = nullptr;
        }
    }

    dbgprintf(L"account picture watch started, folders=%d key=%d",
              nDirs, regEvt ? 1 : 0);
    if (!nDirs && !regEvt)
        dbgprintf(L"account picture watch has no source, the tile will not refresh");

    for (;;)
    {
        HANDLE waits[5];
        DWORD  n = 0;
        waits[n++] = g_picStopEvt;

        if (regEvt)
        {
            ResetEvent(regEvt);
            if (RegNotifyChangeKeyValue(hKey, TRUE,
                                        REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME,
                                        regEvt, TRUE) == ERROR_SUCCESS)
                waits[n++] = regEvt;
        }
        for (int i = 0; i < nDirs; i++)
            waits[n++] = dirs[i];

        DWORD r = WaitForMultipleObjects(n, waits, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0 || r == WAIT_FAILED)
            break;                       // asked to stop, or the wait broke

        // Refresh on the first notice, nothing here waits on a settle timer
        PostMessageW(tile, UT_MSG_PICCHANGED, 0, 0);

        // Then swallow the rest of the burst, one change writes many files
        // Each store landing on its own would reload the tile again
        if (WaitForSingleObject(g_picStopEvt, 1000) == WAIT_OBJECT_0)
            break;

        // Drain until quiet, a notice raised while a handle was unarmed is
        // queued and signals again the moment FindNext re-arms it
        for (int pass = 0; pass < 8; pass++)
        {
            bool again = false;
            for (int i = 0; i < nDirs; i++)
                if (WaitForSingleObject(dirs[i], 0) == WAIT_OBJECT_0)
                {
                    FindNextChangeNotification(dirs[i]);
                    again = true;
                }
            if (!again)
                break;
        }
    }

    for (int i = 0; i < nDirs; i++)
        FindCloseChangeNotification(dirs[i]);
    if (regEvt) CloseHandle(regEvt);
    if (hKey)   RegCloseKey(hKey);
    dbgprintf(L"account picture watch stopped");
    return 0;
}

static void StartPictureWatch(HWND tile)
{
    if (g_picThread || !tile)
        return;

    g_picStopEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_picStopEvt)
        return;

    g_picThread = CreateThread(nullptr, 0, PictureWatchThread, tile, 0, nullptr);
    if (!g_picThread)
    {
        CloseHandle(g_picStopEvt);
        g_picStopEvt = nullptr;
    }
}

static void StopPictureWatch(void)
{
    if (!g_picThread)
        return;

    SetEvent(g_picStopEvt);
    WaitForSingleObject(g_picThread, 2000);
    CloseHandle(g_picThread);
    CloseHandle(g_picStopEvt);
    g_picThread  = nullptr;
    g_picStopEvt = nullptr;
}

static void EnsureTileReady(void* thisPtr)
{
    g_pUserTileObj = thisPtr;
    EnsureTileSubclassed(*(HWND*)UT_Field(thisPtr, UT_TILE_HWND));
    EnsureTooltip(thisPtr);

    HTHEME* pTheme = (HTHEME*)UT_Field(thisPtr, UT_THEME);
    if (!*pTheme)
    {
        static bool tried = false;
        HTHEME th = OpenThemeOffThread(*(HWND*)UT_Field(thisPtr, UT_TILE_HWND),
                                       L"TrayNotify::UserTile");
        if (th)
        {
            *pTheme = th;
            dbgprintf(L"opened TrayNotify::UserTile off thread");
        }
        else if (!tried)
        {
            tried = true;
            dbgprintf(L"TrayNotify::UserTile missing from the active theme, "
                   L"the tile will fall back to a flat fill");
        }
    }

    HBITMAP* pSource = (HBITMAP*)UT_Field(thisPtr, UT_PIC_SOURCE);
    if (*pSource)
        return;                          // store worked, nothing to do

    if (!g_srcBmp)
        g_srcBmp = LoadAccountPicture(UT_SRC_SIZE);
    if (!g_srcBmp)
        return;

    // The class does not free this member, so one shared handle is fine
    *pSource = g_srcBmp;
    *(BYTE*)UT_Field(thisPtr, UT_LOADED_FLAG) = 1;
    dbgprintf(L"supplied source picture to the tile");
}

long __thiscall CalcTileSize_Hook(void* thisPtr, RECT* prc)
{
    // _Paint calls us before it reads the theme handle and the bitmaps, so
    // this is the one place that can repair a tile that already existed
    // before the mod loaded
    EnsureTileReady(thisPtr);

    long hr = CalcTileSize_Orig(thisPtr, prc);

    if (!g_srcBmp)
        return hr;

    int w = *(int*)UT_Field(thisPtr, UT_RECT_RIGHT) - *(int*)UT_Field(thisPtr, UT_RECT_LEFT);
    int h = *(int*)UT_Field(thisPtr, UT_RECT_BOTTOM) - *(int*)UT_Field(thisPtr, UT_RECT_TOP);
    if (w <= 0 || h <= 0)
        return hr;

    HBITMAP* pBlit = (HBITMAP*)UT_Field(thisPtr, UT_PIC_BLIT);

    // The shell scales this with WIC, ours is a GDI stretch and looks
    // washed out, so leave the handle to the shell and let it rebuild
    if (*pBlit && *pBlit != g_blitBmp)
        return hr;
    if (*pBlit == g_blitBmp && g_blitSrc == g_srcBmp &&
        w == g_blitW && h == g_blitH)
        return hr;

    HBITMAP scaled = ScaleSource(w, h);
    if (!scaled)
        return hr;

    if (g_blitBmp && *pBlit == g_blitBmp)
        DeleteObject(g_blitBmp);         // only ever our own handle

    g_blitBmp = scaled;
    g_blitSrc = g_srcBmp;
    g_blitW = w;
    g_blitH = h;
    *pBlit = scaled;
    dbgprintf(L"rebuilt blit bitmap at %dx%d", w, h);
    return hr;
}

//---Paint fallback-----------------------------------------

//---Borrowed tray theme------------------------------------

// With no UserTile class we borrow the Clock theme's part 1 for hover
// Clock is bound to TrayClockWClass, so it must be opened on that hwnd
static HTHEME g_clockTheme = nullptr;
static bool   g_clockThemeTried = false;

static HTHEME GetClockTheme(HWND tile)
{
    if (g_clockThemeTried)
        return g_clockTheme;
    g_clockThemeTried = true;

    HWND parent = GetParent(tile);
    HWND clock = parent ? FindWindowExW(parent, nullptr, L"TrayClockWClass", nullptr)
                        : nullptr;
    if (!clock)
    {
        dbgprintf(L"no TrayClockWClass sibling, hover will be unstyled");
        return nullptr;
    }

    g_clockTheme = OpenThemeData(clock, L"Clock");
    dbgprintf(L"Clock theme %s via TrayClockWClass",
           g_clockTheme ? L"opened" : L"failed");
    return g_clockTheme;
}

//---Pressed state------------------------------------------

// The tile wndproc opens the flyout on click but never moves its state field
// to 3, so without this the tile never darkens the way the clock does
static bool g_tileSubclassed = false;
static bool g_tilePressed = false;
static HWND g_tileHwnd = nullptr;


static LRESULT CALLBACK TileSubclassProc(HWND hwnd, UINT msg, WPARAM wp,
                                         LPARAM lp, UINT_PTR id, DWORD_PTR)
{
    switch (msg)
    {
    case UT_MSG_PICCHANGED:
        // The tick separates one change from two, the log carries no time
        dbgprintf(L"picture change posted at tick %u", GetTickCount());
        DropCachedPicture(g_pUserTileObj, hwnd);
        return 0;

    case WM_MOUSEACTIVATE:
        // Fires before the activation cycle, so this is the only place the
        // flyout's pristine shown state can be read. By WM_LBUTTONDOWN the
        // deactivation has already hidden it
        UserTile_SetFlyoutWasVisible(UserTile_FlyoutIsShown());
        return MA_ACTIVATE;

    case WM_LBUTTONDOWN:
        // Commit the press to screen before the flyout takes focus
        if (!g_tilePressed)
        {
            g_tilePressed = true;
            InvalidateRect(hwnd, nullptr, FALSE);
            UpdateWindow(hwnd);
        }
        DismissTooltip();                // 7850 does this from _OnShowFlyout

        // A click while it was already up is a toggle close, the deactivate
        // did the hiding, so only a fresh click opens it
        if (!UserTile_TakeFlyoutWasVisible())
        {
            UserTile_InvalidatePictureCache();
            UserTile_ShowFlyout(hwnd);
        }
        break;                           // native proc still runs, it no-ops

    case WM_RBUTTONDOWN:
        // Lock, switch user, log off, open user accounts. Shift suppresses
        // it, which is handy when debugging the tile
        if (!(GetAsyncKeyState(VK_SHIFT) & 0x8000))
            UserTile_ShowContextMenu(hwnd);
        return 0;

    case WM_TIMER:
        // Timer 1 is the hover delay. We run before the native handler, so
        // the track point is current by the time it activates the tip
        if (wp == 1)
            RefreshTooltipPosition();
        break;


    case WM_NOTIFY:
        // Claim the placement before the stock handler gets it, otherwise it
        // moves the tip back using a rect it only refreshes after the tile
        // has moved, which is still zero on the first show
        if (g_pUserTileObj)
        {
            NMHDR* nm = (NMHDR*)lp;
            if (nm && nm->code == TTN_SHOW &&
                nm->hwndFrom == *(HWND*)UT_Field(g_pUserTileObj, UT_TOOLTIP_HWND))
            {
                PositionTooltip(nm->hwndFrom);
                return TRUE;             // tells the control we placed it
            }
        }
        break;

    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
    case WM_KILLFOCUS:
        if (g_tilePressed)
        {
            g_tilePressed = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;

    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, TileSubclassProc, id);
        g_tileSubclassed = false;
        g_tilePressed = false;
        StopPictureWatch();
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

//---Account picture change notice--------------------------

static void EnsureTileSubclassed(HWND tile)
{
    if (g_tileSubclassed || !tile)
        return;
    if (SetWindowSubclass(tile, TileSubclassProc, 1, 0))
    {
        g_tileSubclassed = true;
        g_tileHwnd = tile;
        dbgprintf(L"subclassed the tile for press feedback");
        StartPictureWatch(tile);
    }
}

// Washes a colour over a rect at the given alpha, used for the hover state
// that part 2 would normally supply
static void BlendOver(HDC hdc, const RECT* rc, COLORREF colour, BYTE alpha)
{
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 1;
    bi.bmiHeader.biHeight = -1;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(hdc);
    if (!mem) return;

    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib && bits)
    {
        // Premultiplied, so scale each channel by the alpha
        BYTE* px = (BYTE*)bits;
        px[0] = (BYTE)(GetBValue(colour) * alpha / 255);
        px[1] = (BYTE)(GetGValue(colour) * alpha / 255);
        px[2] = (BYTE)(GetRValue(colour) * alpha / 255);
        px[3] = alpha;

        HGDIOBJ old = SelectObject(mem, dib);
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(hdc, rc->left, rc->top,
                   rc->right - rc->left, rc->bottom - rc->top,
                   mem, 0, 0, 1, 1, bf);
        SelectObject(mem, old);
    }
    if (dib) DeleteObject(dib);
    DeleteDC(mem);
}

// Stock _Paint fills the whole tile with COLOR_BTNFACE when the theme has no
// UserTile class, which is the white block. Win7 era msstyles do not carry
// that class, so paint the taskbar through instead and blit just the picture
void __thiscall Paint_Hook(void* thisPtr, HDC hdc, RECT* prc)
{
    if (*(HTHEME*)UT_Field(thisPtr, UT_THEME))
    {
        Paint_Orig(thisPtr, hdc, prc);   // theme has the class, stock path is right
        return;
    }

    CalcTileSize_Hook(thisPtr, prc);     // sizes the rects and builds the bitmaps

    HWND tile = *(HWND*)UT_Field(thisPtr, UT_TILE_HWND);
    if (FAILED(DrawThemeParentBackground(tile, hdc, prc)))
    {
        // No parent theme either, so fall back to the taskbar brush
        FillRect(hdc, prc, (HBRUSH)(COLOR_3DFACE + 1));
    }

    // Same hot test stock _Paint uses before it picks the part 2 state
    int stateId = *(int*)UT_Field(thisPtr, UT_STATE_ID);
    if (*(BYTE*)UT_Field(thisPtr, UT_HOT_FLAG) && stateId == 1)
    {
        if ((SendMessageW(tile, 0x129, 0, 0) & 1) == 0)
            stateId = 2;
    }

    // Pressed outranks hot, and only the subclass knows about it
    if (g_tilePressed)
        stateId = 3;

    // Interaction overlay, only for hot and pressed. State 1 draws nothing so
    // the tile stays transparent when the pointer is elsewhere
    if (stateId == 2 || stateId == 3)
    {
        HTHEME clock = GetClockTheme(tile);
        if (clock)
            DrawThemeBackground(clock, hdc, 1, stateId, prc, nullptr);
        else if (stateId == 2)
            BlendOver(hdc, prc, RGB(255, 255, 255), 60);
    }

    // Part 1, a BorderFill at the frame rect, the picture covers all but 1px
    HBRUSH fill = CreateSolidBrush(STROKE_FILL_COLOUR);
    if (fill)
    {
        FillRect(hdc, (RECT*)UT_Field(thisPtr, UT_FRAME_RECT), fill);
        DeleteObject(fill);
    }

    HBITMAP blit = *(HBITMAP*)UT_Field(thisPtr, UT_PIC_BLIT);
    if (!blit)
        return;

    HDC mem = CreateCompatibleDC(hdc);
    if (!mem)
        return;

    HGDIOBJ old = SelectObject(mem, blit);
    int l = *(int*)UT_Field(thisPtr, UT_RECT_LEFT);
    int t = *(int*)UT_Field(thisPtr, UT_RECT_TOP);
    int w = *(int*)UT_Field(thisPtr, UT_RECT_RIGHT) - l;
    int h = *(int*)UT_Field(thisPtr, UT_RECT_BOTTOM) - t;
    if (w > 0 && h > 0)
        BitBlt(hdc, l, t, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
}


//---Hook installation--------------------------------------

// Both patterns were taken from the 6.1.7850.0 explorer.exe and checked to
// match exactly once in .text. _UpdateUserInfo is deliberately not hooked,
// _OnCreate bails at the user tile store failure long before it would run
static const char* c_szPaintPattern =
	"48 8B C4 4C 89 40 18 48 89 50 10 48 89 48 08 53 55 56 57 41 55 48 83 EC";

static const char* c_szCalcTileSizePattern =
	"48 8B C4 48 89 58 18 48 89 70 20 48 89 50 10 48 89 48 08 55 57 41 54 41 55 41 57 48 8B EC 48 83";

// 7850 asserts on the null picture member inside _CalcTileSizeAndPosition
// and takes the shell down with it. Our hook fills that member in first, so
// this only bites when no account picture exists at all, but patching it
// means an unpatched explorer.exe works too.
//
// The branch opcode itself is wildcarded so this still matches a binary
// that was already patched by hand. Pinning it to 75 made the scan miss
// entirely on those, which reported as "pattern not found" rather than
// reaching the already patched check below. Still unique in .text either way
static const char* c_szAssertPattern =
	"BF C0 00 00 00 00 89 45 C4 89 45 C0 ?? 69 4C 8D";

static void PatchUserTileAssert(void)
{
	uintptr_t hit = FindPattern((uintptr_t)GetModuleHandle(NULL), c_szAssertPattern);
	if (!hit)
	{
		dbgprintf(L"usertile: assert pattern not found, leaving it alone");
		return;
	}

	BYTE* pBranch = (BYTE*)(hit + 12);
	if (*pBranch == 0xEB)
	{
		dbgprintf(L"usertile: assert already patched in the binary");
		return;
	}
	if (*pBranch != 0x75)
	{
		dbgprintf(L"usertile: assert site is not the expected jnz, leaving it");
		return;
	}

	DWORD dwOld = 0;
	if (VirtualProtect(pBranch, 1, PAGE_EXECUTE_READWRITE, &dwOld))
	{
		*pBranch = 0xEB;             // jnz becomes jmp, the trap is unreachable
		VirtualProtect(pBranch, 1, dwOld, &dwOld);
		FlushInstructionCache(GetCurrentProcess(), pBranch, 1);
		dbgprintf(L"usertile: assert patched out at %p", pBranch);
	}
}

void HookUserTile(void)
{
	// The offsets, both patterns and the assert site are 7850 only, so
	// anything else is left completely alone
	if (s_BuildRuntime != BUILDRUNTIME_7850)
	{
		dbgprintf(L"usertile: BuildRuntime is not 7850, skipping");
		return;
	}

	PatchUserTileAssert();

	uintptr_t base = (uintptr_t)GetModuleHandle(NULL);

	void* pCalc = (void*)FindPattern(base, c_szCalcTileSizePattern);
	if (pCalc)
	{
		MH_CreateHook(pCalc, (void*)CalcTileSize_Hook, (void**)&CalcTileSize_Orig);
		dbgprintf(L"usertile: _CalcTileSizeAndPosition hooked at %p", pCalc);
	}
	else
	{
		dbgprintf(L"usertile: _CalcTileSizeAndPosition pattern not found");
	}

	void* pPaint = (void*)FindPattern(base, c_szPaintPattern);
	if (pPaint)
	{
		MH_CreateHook(pPaint, (void*)Paint_Hook, (void**)&Paint_Orig);
		dbgprintf(L"usertile: _Paint hooked at %p", pPaint);
	}
	else
	{
		dbgprintf(L"usertile: _Paint pattern not found");
	}
}

void UnhookUserTile(void)
{
	if (g_tileSubclassed && g_tileHwnd)
	{
		RemoveWindowSubclass(g_tileHwnd, TileSubclassProc, 1);
		g_tileSubclassed = false;
		g_tileHwnd = nullptr;
	}

	UserTile_DestroyFlyout();

	// The tile keeps painting from whatever is in the member, so leave the
	// handle in place and only drop our own cache
	if (g_srcBmp)
	{
		DeleteObject(g_srcBmp);
		g_srcBmp = nullptr;
	}
	g_blitBmp = nullptr;
}
