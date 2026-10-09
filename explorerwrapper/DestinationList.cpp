#include "DestinationList.h"
#include "dbgprint.h"
#include <shlwapi.h>

//---Jump list item icons-----------------------------------

// PKEY_AppUserModel_DestListLogoUri and PKEY_AppUserModel_ID
static const PROPERTYKEY kLogoUri = { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 29 };
static const PROPERTYKEY kAumid   = { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 5 };

// Vista and later accept a PNG inside an icon file, so no re-encoding is needed
static bool WritePngAsIcon(LPCWSTR pngPath, LPCWSTR icoPath)
{
	HANDLE hIn = CreateFileW(pngPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (hIn == INVALID_HANDLE_VALUE)
		return false;

	bool ok = false;
	DWORD size = GetFileSize(hIn, nullptr);
	if (size >= 24 && size < 4 * 1024 * 1024)
	{
		BYTE* png = (BYTE*)LocalAlloc(LPTR, size);
		DWORD read = 0;
		if (png && ReadFile(hIn, png, size, &read, nullptr) && read == size && png[0] == 0x89)
		{
			DWORD w = (png[16] << 24) | (png[17] << 16) | (png[18] << 8) | png[19];
			DWORD h = (png[20] << 24) | (png[21] << 16) | (png[22] << 8) | png[23];

			BYTE hdr[22] = {};
			hdr[2] = 1;                                  // type, icon
			hdr[4] = 1;                                  // one image
			hdr[6] = (w >= 256) ? 0 : (BYTE)w;
			hdr[7] = (h >= 256) ? 0 : (BYTE)h;
			hdr[10] = 1;                                 // planes
			hdr[12] = 32;                                // bit count
			*(DWORD*)(hdr + 14) = size;
			*(DWORD*)(hdr + 18) = sizeof(hdr);

			HANDLE hOut = CreateFileW(icoPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
			if (hOut != INVALID_HANDLE_VALUE)
			{
				DWORD wrote = 0;
				ok = (WriteFile(hOut, hdr, sizeof(hdr), &wrote, nullptr) != 0)
				  && (WriteFile(hOut, png, size, &wrote, nullptr) != 0);
				CloseHandle(hOut);
			}
		}
		if (png)
			LocalFree(png);
	}
	CloseHandle(hIn);
	return ok;
}

// Package assets carry theme or scale qualifiers, so gaming.png may only exist as gaming.theme-light.png
static bool ResolveAssetVariant(LPWSTR path, int cch)
{
	if (PathFileExistsW(path))
		return true;

	WCHAR dir[MAX_PATH] = {};
	lstrcpynW(dir, path, ARRAYSIZE(dir));
	PathRemoveFileSpecW(dir);

	WCHAR base[MAX_PATH] = {};
	lstrcpynW(base, PathFindFileNameW(path), ARRAYSIZE(base));

	WCHAR ext[16] = {};
	LPWSTR dot = PathFindExtensionW(base);
	lstrcpynW(ext, dot, ARRAYSIZE(ext));
	*dot = 0;

	// The Windows 7 jump list is light, so prefer the light theme artwork
	static const LPCWSTR suffixes[] = { L".theme-light", L".theme-dark", L".scale-200", L".scale-100" };
	for (int i = 0; i < ARRAYSIZE(suffixes); i++)
	{
		wnsprintfW(path, cch, L"%s\\%s%s%s", dir, base, suffixes[i], ext);
		if (PathFileExistsW(path))
			return true;
	}

	// Last resort, accept whatever qualified variant is present
	WCHAR pattern[MAX_PATH] = {};
	wnsprintfW(pattern, ARRAYSIZE(pattern), L"%s\\%s.*%s", dir, base, ext);

	WIN32_FIND_DATAW fd = {};
	HANDLE hf = FindFirstFileW(pattern, &fd);
	if (hf != INVALID_HANDLE_VALUE)
	{
		wnsprintfW(path, cch, L"%s\\%s", dir, fd.cFileName);
		FindClose(hf);
		return true;
	}

	return false;
}

// Turn the package family name in an app id into its data folder
static bool FamilyFromAumid(LPCWSTR aumid, LPWSTR out, int cch)
{
	if (!aumid || !*aumid)
		return false;

	lstrcpynW(out, aumid, cch);
	LPWSTR bang = StrChrW(out, L'!');
	if (bang)
		*bang = 0;

	return out[0] != 0;
}

// ms-appdata maps to the package data folders, ms-appx to the install folder
static bool ResolveLogoUri(LPCWSTR uri, LPCWSTR aumid, LPWSTR out, int cch)
{
	WCHAR family[256] = {};
	if (!FamilyFromAumid(aumid, family, ARRAYSIZE(family)))
		return false;

	WCHAR rel[MAX_PATH] = {};
	if (StrCmpNIW(uri, L"ms-appdata:///", 14) == 0)
	{
		LPCWSTR rest = uri + 14;
		LPCWSTR sub = nullptr;
		if (StrCmpNIW(rest, L"local/", 6) == 0)        { rest += 6; sub = L"LocalState"; }
		else if (StrCmpNIW(rest, L"roaming/", 8) == 0) { rest += 8; sub = L"RoamingState"; }
		else if (StrCmpNIW(rest, L"temp/", 5) == 0)    { rest += 5; sub = L"TempState"; }
		else return false;

		lstrcpynW(rel, rest, ARRAYSIZE(rel));
		for (LPWSTR p = rel; *p; p++)
			if (*p == L'/') *p = L'\\';

		WCHAR local[MAX_PATH] = {};
		if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, ARRAYSIZE(local)))
			return false;

		wnsprintfW(out, cch, L"%s\\Packages\\%s\\%s\\%s", local, family, sub, rel);
		return ResolveAssetVariant(out, cch);
	}

	if (StrCmpNIW(uri, L"ms-appx:///", 11) == 0)
	{
		lstrcpynW(rel, uri + 11, ARRAYSIZE(rel));
		for (LPWSTR p = rel; *p; p++)
			if (*p == L'/') *p = L'\\';

		// GetPackagePathByFullName is Windows 8 and later, so bind it late
		typedef LONG (WINAPI *PFN_FIND)(PCWSTR, UINT32, UINT32*, PWSTR*, UINT32*, PWSTR, UINT32*);
		typedef LONG (WINAPI *PFN_PATH)(PCWSTR, UINT32*, PWSTR);
		static PFN_FIND pFind = (PFN_FIND)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "FindPackagesByPackageFamily");
		static PFN_PATH pPath = (PFN_PATH)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetPackagePathByFullName");
		if (!pFind || !pPath)
			return false;

		const UINT32 filter = 0x00000010; // PACKAGE_FILTER_HEAD
		UINT32 count = 0, bufLen = 0;
		if (pFind(family, filter, &count, nullptr, &bufLen, nullptr, nullptr) != ERROR_INSUFFICIENT_BUFFER || count == 0)
			return false;

		PWSTR* names = (PWSTR*)LocalAlloc(LPTR, count * sizeof(PWSTR));
		PWSTR buf = (PWSTR)LocalAlloc(LPTR, bufLen * sizeof(WCHAR));
		UINT32* props = (UINT32*)LocalAlloc(LPTR, count * sizeof(UINT32));
		bool ok = false;
		if (names && buf && props
			&& pFind(family, filter, &count, names, &bufLen, buf, props) == ERROR_SUCCESS && count > 0)
		{
			WCHAR pkgPath[MAX_PATH] = {};
			UINT32 cchPath = ARRAYSIZE(pkgPath);
			if (pPath(names[0], &cchPath, pkgPath) == ERROR_SUCCESS)
			{
				wnsprintfW(out, cch, L"%s\\%s", pkgPath, rel);
				ok = ResolveAssetVariant(out, cch);
			}
		}
		if (names) LocalFree(names);
		if (buf) LocalFree(buf);
		if (props) LocalFree(props);
		return ok;
	}

	return false;
}

// Cache converted icons under the temp folder, keyed on the uri
static void CacheNameForUri(LPCWSTR uri, LPWSTR out, int cch)
{
	DWORD hash = 2166136261u;
	for (LPCWSTR p = uri; *p; p++)
		hash = (hash ^ (DWORD)*p) * 16777619u;

	WCHAR tmp[MAX_PATH] = {};
	GetTempPathW(ARRAYSIZE(tmp), tmp);
	wnsprintfW(out, cch, L"%sexplorer7_jl", tmp);
	CreateDirectoryW(out, nullptr);
	wnsprintfW(out, cch, L"%sexplorer7_jl\\%X.ico", tmp, hash);
}

// Give an immersive jump list item a real icon file it can display
static void GiveItemAnIcon(IShellLinkW* link)
{
	IPropertyStore* ps = nullptr;
	if (FAILED(link->QueryInterface(IID_PPV_ARGS(&ps))))
		return;

	PROPVARIANT pvUri, pvId;
	PropVariantInit(&pvUri);
	PropVariantInit(&pvId);

	if (SUCCEEDED(ps->GetValue(kLogoUri, &pvUri)) && pvUri.vt == VT_LPWSTR && pvUri.pwszVal
		&& SUCCEEDED(ps->GetValue(kAumid, &pvId)) && pvId.vt == VT_LPWSTR && pvId.pwszVal)
	{
		WCHAR ico[MAX_PATH] = {};
		CacheNameForUri(pvUri.pwszVal, ico, ARRAYSIZE(ico));

		bool ready = PathFileExistsW(ico) != FALSE;
		if (!ready)
		{
			WCHAR png[MAX_PATH] = {};
			if (ResolveLogoUri(pvUri.pwszVal, pvId.pwszVal, png, ARRAYSIZE(png)))
				ready = WritePngAsIcon(png, ico);
		}

		if (ready)
			link->SetIconLocation(ico, 0);
	}

	PropVariantClear(&pvUri);
	PropVariantClear(&pvId);
	ps->Release();
}

CAutoDestWrapper::CAutoDestWrapper(IAutoDestinationList10* destlist)
{
	m_cRef = 1;
	m_dest10 = destlist;
}

CAutoDestWrapper::~CAutoDestWrapper()
{
	m_dest10->Release();
}

HRESULT __stdcall CAutoDestWrapper::QueryInterface(REFIID riid, void** ppvObject)
{
	return m_dest10->QueryInterface(riid, ppvObject);
}

ULONG __stdcall CAutoDestWrapper::AddRef(void)
{
	m_dest10->AddRef();
	return InterlockedIncrement(&m_cRef);
}

ULONG __stdcall CAutoDestWrapper::Release(void)
{
	m_dest10->Release();
	if (InterlockedDecrement(&m_cRef) == 0)
	{
		free((void*)this);
		return 0;
	}
	return m_cRef;
}

HRESULT __stdcall CAutoDestWrapper::Initialize(LPCWSTR p1, LPCWSTR p2, LPCWSTR p3)
{
	return m_dest10->Initialize(p1, p2, p3);
}

HRESULT __stdcall CAutoDestWrapper::HasList(int* p1)
{
	return m_dest10->HasList(p1);
}

HRESULT __stdcall CAutoDestWrapper::GetList(int p1, unsigned int p2, REFGUID p3, void** p4)
{
	return m_dest10->GetList(p1, p2, 0, p3, p4);
}

HRESULT __stdcall CAutoDestWrapper::AddUsagePoint(IUnknown* p1)
{
	return m_dest10->AddUsagePoint(p1);
}

HRESULT __stdcall CAutoDestWrapper::PinItem(IUnknown* p1, int p2)
{
	return m_dest10->PinItem(p1, p2);
}

HRESULT __stdcall CAutoDestWrapper::IsPinned(IUnknown* p1, int* p2)
{
	return m_dest10->IsPinned(p1, p2);
}

HRESULT __stdcall CAutoDestWrapper::RemoveDestination(IUnknown* p1)
{
	return m_dest10->RemoveDestination(p1);
}

HRESULT __stdcall CAutoDestWrapper::SetUsageData(IUnknown* p1, float* p2, FILETIME* p3)
{
	return m_dest10->SetUsageData(p1, p2, p3);
}

HRESULT __stdcall CAutoDestWrapper::GetUsageData(IUnknown* p1, float* p2, FILETIME* p3)
{
	return m_dest10->GetUsageData(p1, p2, p3);
}

HRESULT __stdcall CAutoDestWrapper::ResolveDestination(HWND p1, unsigned long p2, IShellItem* p3, REFGUID p4, void** p5)
{
	return m_dest10->ResolveDestination(p1, p2, p3, p4, p5);
}

HRESULT __stdcall CAutoDestWrapper::ClearList(int p1)
{
	return m_dest10->ClearList(p1);
}



CCustomDestWrapper::CCustomDestWrapper(IInternalCustomDestList10* custDest)
{
	m_cRef = 1;
	m_custDest10 = custDest;
}

CCustomDestWrapper::CCustomDestWrapper(IInternalCustomDestList1507* custDest)
{
	m_cRef = 1;
	m_custDest1507 = custDest;
}

CCustomDestWrapper::~CCustomDestWrapper()
{
	if (m_custDest10)
		m_custDest10->Release();
	if (m_custDest1507)
		m_custDest1507->Release();
}

HRESULT __stdcall CCustomDestWrapper::QueryInterface(REFIID riid, void** ppvObject)
{
	if (m_custDest10)
		return m_custDest10->QueryInterface(riid, ppvObject);
	if (m_custDest1507)
		return m_custDest1507->QueryInterface(riid, ppvObject);
	return S_OK;
}

ULONG __stdcall CCustomDestWrapper::AddRef(void)
{
	if (m_custDest10)
		m_custDest10->AddRef(); 
	if (m_custDest1507)
		m_custDest1507->AddRef();
	return InterlockedIncrement(&m_cRef);
}

ULONG __stdcall CCustomDestWrapper::Release(void)
{
	ULONG cRef = 0;
	if (m_custDest10)
		cRef = m_custDest10->Release();
	if (m_custDest1507)
		cRef = m_custDest1507->Release();
	if (InterlockedDecrement(&m_cRef) == 0 || cRef == 0)
	{
		free((void*)this);
		return 0;
	}
	return cRef;
}

HRESULT __stdcall CCustomDestWrapper::SetMinItems(UINT p1)
{
	if (m_custDest10)
		return m_custDest10->SetMinItems(p1);
	if (m_custDest1507)
		return m_custDest1507->SetMinItems(p1);
	return S_OK;
}

HRESULT __stdcall CCustomDestWrapper::SetApplicationID(LPCWSTR p1)
{
	if (m_custDest10)
		return m_custDest10->SetApplicationID(p1);
	if (m_custDest1507)
		return m_custDest1507->SetApplicationID(p1);
	return S_OK;
}

HRESULT __stdcall CCustomDestWrapper::GetSlotCount(UINT* p1)
{
	if (m_custDest10)
		return m_custDest10->GetSlotCount(p1);
	if (m_custDest1507)
		return m_custDest1507->GetSlotCount(p1);
	return S_OK;
}

HRESULT __stdcall CCustomDestWrapper::GetCategoryCount(UINT* p1)
{
	if (m_custDest10)
		return m_custDest10->GetCategoryCount(p1);
	if (m_custDest1507)
		return m_custDest1507->GetCategoryCount(p1);
	return S_OK;
}

HRESULT __stdcall CCustomDestWrapper::GetCategory(UINT p1, int p2, PVOID p3)
{
	if (m_custDest10)
		return m_custDest10->GetCategory(p1, p2, p3);
	if (m_custDest1507)
		return m_custDest1507->GetCategory(p1, p2, p3);
	return S_OK;
}

HRESULT __stdcall CCustomDestWrapper::DeleteCategory(UINT p1, int p2)
{
	if (m_custDest10)
		return m_custDest10->DeleteCategory(p1, p2);
	if (m_custDest1507)
		return m_custDest1507->DeleteCategory(p1, p2);
	return S_OK;
}

HRESULT __stdcall CCustomDestWrapper::EnumerateCategoryDestinations(UINT p1, REFIID p2, void** p3)
{
	HRESULT hr;
	if (m_custDest10)
		hr = m_custDest10->EnumerateCategoryDestinations(p1, p2, p3);
	else if (m_custDest1507)
		hr = m_custDest1507->EnumerateCategoryDestinations(p1, p2, p3);
	else
		return S_OK;

	// Immersive apps leave their items without an icon, so supply one
	if (SUCCEEDED(hr) && p3 && *p3)
	{
		IObjectArray* arr = nullptr;
		if (SUCCEEDED(((IUnknown*)*p3)->QueryInterface(IID_PPV_ARGS(&arr))))
		{
			UINT count = 0;
			arr->GetCount(&count);

			for (UINT i = 0; i < count; i++)
			{
				IShellLinkW* link = nullptr;
				if (SUCCEEDED(arr->GetAt(i, IID_PPV_ARGS(&link))))
				{
					// Immersive items carry their logo as a package uri that Windows 7 cannot read
					WCHAR icon[MAX_PATH] = {};
					int idx = 0;
					link->GetIconLocation(icon, ARRAYSIZE(icon), &idx);
					if (icon[0] == 0)
						GiveItemAnIcon(link);

					link->Release();
				}
			}
			arr->Release();
		}
	}
	return hr;
}

HRESULT __stdcall CCustomDestWrapper::RemoveDestination(IUnknown* p1)
{
	if (m_custDest10)
		return m_custDest10->RemoveDestination(p1);
	if (m_custDest1507)
		return m_custDest1507->RemoveDestination(p1);
	return S_OK;
}

HRESULT __stdcall CCustomDestWrapper::ResolveDestination(HWND p1, ULONG p2, IShellItem* p3, REFIID p4, void** p5)
{
	if (m_custDest10)
		return m_custDest10->ResolveDestination(p1, p2, p3, p4, p5);
	if (m_custDest1507)
		return m_custDest1507->ResolveDestination(p1, p2, p3, p4, p5);
	return S_OK;
}
