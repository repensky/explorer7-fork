#include "StartMenuResolver.h"
#include "StartMenuPin.h"
#include "dbgprint.h"
#include "PinnedList.h"
#pragma function(memset)

extern "C" HRESULT WINAPI Explorer_CoCreateInstance(
	__in   REFCLSID rclsid,
	__in   LPUNKNOWN pUnkOuter,
	__in   DWORD dwClsContext,
	__in   REFIID riid,
	__out  LPVOID* ppv
);

// A fresh profile has no start menu app cache, so EnumItems returns nothing
// 0x4 avoids E_ACCESSDENIED before init, 0x8 forces the folder scan to run
#define REFRESH_CACHE_FORCE_FULL 0x4C

//constructor
CStartMenuResolver::CStartMenuResolver(IAppResolver8* newresolver)
{
	m_cRef = 0; //?
	m_resolver8 = newresolver;
	m_startmenuitemscache8 = nullptr;
	m_startmenuitemscache10 = nullptr;
	m_cacheRefreshed = false;
}

CStartMenuResolver::CStartMenuResolver(IStartMenuItemsCache8 *newcache)
{
	m_cRef = 0;
	m_startmenuitemscache8 = newcache;
	m_startmenuitemscache10 = nullptr;
	m_cacheRefreshed = false;
	CoCreateInstance(
		CLSID_StartMenuCacheAndAppResolver,
		nullptr,
		CLSCTX_INPROC_SERVER,
		IID_IAppResolver8,
		(LPVOID *)&m_resolver8
	);
	EnsureAppCache(true);
}

CStartMenuResolver::CStartMenuResolver(IStartMenuItemsCache10 *newcache)
{
	m_cRef = 0;
	m_startmenuitemscache10 = newcache;
	m_startmenuitemscache8 = nullptr;
	m_cacheRefreshed = false;
	CoCreateInstance(
		CLSID_StartMenuCacheAndAppResolver,
		nullptr,
		CLSCTX_INPROC_SERVER,
		IID_IAppResolver8,
		(LPVOID*)&m_resolver8
	);
	EnsureAppCache(true);
}

// How many items appresolver's start menu cache holds right now
UINT CStartMenuResolver::GetAppCacheCount()
{
	UINT count = 0;
	if (!m_resolver8)
		return 0;

	IStartMenuAppItems8* items = nullptr;
	if (SUCCEEDED(m_resolver8->QueryInterface(IID_IStartMenuAppItems8, (LPVOID*)&items)))
	{
		IObjectCollection* collection = nullptr;
		if (SUCCEEDED(items->EnumItems(0, IID_IObjectCollection, (PVOID*)&collection)))
		{
			collection->GetCount(&count);
			collection->Release();
		}
		items->Release();
	}
	return count;
}

// Only explorer is allowed to build the cache, this DLL runs inside it
HRESULT CStartMenuResolver::EnsureAppCache(bool onlyIfEmpty)
{
	if (m_cacheRefreshed)
		return S_FALSE;

	// An established profile already has a cache, a rebuild would be wasted work
	// The flag stays clear so a later genuine emptiness can still force one
	if (onlyIfEmpty)
	{
		if (GetAppCacheCount() != 0)
			return S_FALSE;
	}
	m_cacheRefreshed = true;

	HRESULT rslt = E_FAIL;
	if (m_startmenuitemscache10)
		rslt = m_startmenuitemscache10->RefreshCache(REFRESH_CACHE_FORCE_FULL);
	else if (m_startmenuitemscache8)
		rslt = m_startmenuitemscache8->RefreshCache(REFRESH_CACHE_FORCE_FULL);

	dbgprintf(L"CStartMenuResolver::EnsureAppCache RefreshCache = %p", rslt);
	return rslt;
}

CStartMenuResolver::~CStartMenuResolver()
{
	if (m_resolver8)
		m_resolver8->Release();

	if (m_startmenuitemscache8)
		m_startmenuitemscache8->Release();

	if (m_startmenuitemscache10)
		m_startmenuitemscache10->Release();
}

HRESULT STDMETHODCALLTYPE CStartMenuResolver::QueryInterface(REFIID riid, void** ppvObject)
{
	if (riid == IID_IAppResolver7)
	{
		//dbgprintf(L"IID_IAppResolver7\n");
		*ppvObject = static_cast<IAppResolver7*>(this);
		AddRef();
		return S_OK;
	}
	if (riid == IID_IStartMenuItemsCache7)
	{
		HRESULT ret = E_NOINTERFACE;
		if (m_startmenuitemscache8)
		{
			ret = m_startmenuitemscache8->QueryInterface(IID_IStartMenuItemsCache8, (PVOID *)&m_startmenuitemscache8);
			if (ret == S_OK)
			{
				*ppvObject = static_cast<IStartMenuItemsCache7 *>(this);
				AddRef();
			}
		}
		else if (m_startmenuitemscache10)
		{
			ret = m_startmenuitemscache10->QueryInterface(IID_IStartMenuItemsCache10, (PVOID*)&m_startmenuitemscache10);
			if (ret == S_OK)
			{
				*ppvObject = static_cast<IStartMenuItemsCache7 *>(this);
				AddRef();
			}
		}
		return ret;
	}
	return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE CStartMenuResolver::AddRef(void)
{
	return InterlockedIncrement(&m_cRef);
}

ULONG STDMETHODCALLTYPE CStartMenuResolver::Release(void)
{
	if (InterlockedDecrement(&m_cRef) == 0)
	{
		delete this;
		return 0;
	}
	return m_cRef;
}

//IAppResolver7
HRESULT STDMETHODCALLTYPE CStartMenuResolver::GetAppIDForShortcut(IShellItem* p1, LPWSTR* p2)
{
	return m_resolver8->GetAppIDForShortcut(p1, p2);
}

HRESULT STDMETHODCALLTYPE CStartMenuResolver::GetAppIDForWindow(HWND p1, LPWSTR* p2, int* p3, int* p4, int* p5)
{
	return m_resolver8->GetAppIDForWindow(p1, p2, p3, p4, p5);
}

HRESULT STDMETHODCALLTYPE CStartMenuResolver::GetAppIDForProcess(ULONG_PTR p1, LPWSTR* p2, int* p3, int* p4, int* p5)
{
	return m_resolver8->GetAppIDForProcess(p1, p2, p3, p4, p5);
}

HRESULT STDMETHODCALLTYPE CStartMenuResolver::GetShortcutForProcess(ULONG_PTR p1, IShellItem** p2)
{
	return m_resolver8->GetShortcutForProcess(p1, p2);
}

HRESULT STDMETHODCALLTYPE CStartMenuResolver::GetBestShortcutForAppID(LPWSTR p1, IShellItem** p2)
{
	// Ittr: Immersive apps normally fail here, Settings would wrongly succeed
	// Failing it keeps Settings consistent with every other immersive app
	if (lstrcmp(p1, L"windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel") == 0)
	{
		return E_OUTOFMEMORY;
	}

	return m_resolver8->GetBestShortcutForAppID(p1, p2);
}

HRESULT STDMETHODCALLTYPE CStartMenuResolver::GetBestShortcutAndAppIDForAppPath(LPWSTR p1, IShellItem** p2, LPWSTR* p3)
{
	return m_resolver8->GetBestShortcutAndAppIDForAppPath(p1, p2, p3);
}

HRESULT STDMETHODCALLTYPE CStartMenuResolver::CanPinApp(IShellItem* p1)
{
	return m_resolver8->CanPinApp(p1);
}

HRESULT STDMETHODCALLTYPE CStartMenuResolver::GetRelaunchProperties(HWND p1, LPWSTR* p2, LPWSTR* p3, LPWSTR* p4, LPWSTR* p5, LPWSTR* p6)
{
	//dbgprintf(L"GetRelaunchProperties");
	return m_resolver8->GetRelaunchProperties(p1, p2, p3, p4, p5, p6, nullptr);
}

HRESULT STDMETHODCALLTYPE CStartMenuResolver::GenerateShortcutFromWindowProperties(HWND p1, IShellItem** p2)
{
	return m_resolver8->GenerateShortcutFromWindowProperties(p1, p2);
}

HRESULT STDMETHODCALLTYPE CStartMenuResolver::GenerateShortcutFromItemProperties(IShellItem2* p1, IShellItem** p2)
{
	return m_resolver8->GenerateShortcutFromItemProperties(p1, p2);
}

//IStartMenuItemsCache7
HRESULT STDMETHODCALLTYPE CStartMenuResolver::OnChangeNotify(unsigned int p1, long p2, PVOID* p3, PVOID* p4)
{
	HRESULT rslt;
	if (m_startmenuitemscache8)
		rslt = m_startmenuitemscache8->OnChangeNotify(p1, p2, p3, p4);
	else if (m_startmenuitemscache10)
		rslt = m_startmenuitemscache10->OnChangeNotify(p1, p2, p3, p4);
	return rslt;
}

HRESULT STDMETHODCALLTYPE CStartMenuResolver::PinListChanged(void)
{
	//we need to clear MFU cache, but we don't have one!
	return S_OK;
}

HRESULT STDMETHODCALLTYPE CStartMenuResolver::GetPinnedItemsCount(int* pCount)
{
	*pCount = 0;
	IPinnedList2* pinList2 = 0;
	HRESULT rslt = Explorer_CoCreateInstance(CLSID_StartMenuPin, NULL, CLSCTX_INPROC_SERVER, IID_IPinnedList2, (PVOID*)&pinList2);
	if (SUCCEEDED(rslt))
	{
		IEnumFullIDList* enumidlist;
		rslt = pinList2->EnumObjects(&enumidlist);
		if (SUCCEEDED(rslt))
		{
			LPITEMIDLIST pidl;
			ULONG wat;
			while (enumidlist->Next(1, &pidl, &wat) == S_OK)
			{
				(*pCount)++;
				CoTaskMemFree(pidl);
			}
			enumidlist->Release();
		}
		pinList2->Release();
	}
	return rslt;
}

// UserAssist stores its value names in rot13
static void Rot13(LPWSTR s)
{
	for (; *s; s++)
	{
		if (*s >= L'a' && *s <= L'z')
			*s = L'a' + (*s - L'a' + 13) % 26;
		else if (*s >= L'A' && *s <= L'Z')
			*s = L'A' + (*s - L'A' + 13) % 26;
	}
}

// The app cache holds only shortcuts, so used Games folder items such as Solitaire are read from UserAssist
void CStartMenuResolver::AddUsedGames(CEnumStartMenu* startenum, IPinnedList2* startpinnedlist, IPinnedList2* taskbarpinnedlist)
{
	// The name 7850's first logon seeding stored for Solitaire starts with this
	static const WCHAR c_gamesPrefix[] = L"::{ED228FDF-9EA8-4870-83B1-96B02CFE0D52}\\";
	const int prefixLen = ARRAYSIZE(c_gamesPrefix) - 1;

	HKEY key;
	if (RegOpenKeyExW(HKEY_CURRENT_USER,
		L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\UserAssist\\{F4E57C4B-2036-45F0-A9AB-443BCFE33D9F}\\Count",
		0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
		return;

	PIDLIST_ABSOLUTE gamesRoot = nullptr;
	IShellFolder* games = nullptr;
	WCHAR name[1024];
	for (DWORD i = 0;; i++)
	{
		DWORD cch = ARRAYSIZE(name);
		LSTATUS st = RegEnumValueW(key, i, name, &cch, nullptr, nullptr, nullptr, nullptr);
		if (st == ERROR_NO_MORE_ITEMS)
			break;
		if (st != ERROR_SUCCESS)
			continue;
		Rot13(name);
		if (StrCmpNIW(name, c_gamesPrefix, prefixLen) != 0)
			continue;

		// Parsed through the Games folder the way the seeding made it, not from the full name
		if (!games)
		{
			if (FAILED(SHGetKnownFolderIDList(FOLDERID_Games, 0, nullptr, &gamesRoot))
				|| FAILED(SHBindToObject(nullptr, gamesRoot, nullptr, IID_IShellFolder, (void**)&games)))
				break;
		}
		PIDLIST_RELATIVE child = nullptr;
		if (FAILED(games->ParseDisplayName(nullptr, nullptr, name + prefixLen, nullptr, &child, nullptr)))
		{
			dbgprintf(L"CStartMenuResolver::AddUsedGames could not parse %s", name);
			continue;
		}
		PIDLIST_ABSOLUTE pidl = ILCombine(gamesRoot, child);
		ILFree(child);
		if (!pidl)
			continue;

		bool added = false;
		IShellItem* shellitem;
		if (SUCCEEDED(SHCreateItemFromIDList(pidl, IID_IShellItem, (LPVOID*)&shellitem)))
		{
			// The game's own app id ranks it like any other app and lets RemoveDuplicates fold it into a shortcut to the same exe
			STARTMENUITEM startitem = { 0 };
			if (FAILED(m_resolver8->GetAppIDForShortcut(shellitem, &startitem.pszAppID)))
				startitem.pszAppID = nullptr;
			shellitem->Release();
			if (UAQueryMFUUsage(pidl, startitem.pszAppID, &startitem.ueminfo)
				&& startpinnedlist->IsPinned(pidl) == S_FALSE && taskbarpinnedlist->IsPinned(pidl) == S_FALSE)
			{
				if (!startitem.pszAppID)
					startitem.pszAppID = CoAllocString(name);
				startitem.pidlRelative = pidl;
				startitem.iPinPos = -1;
				startenum->AddItem(&startitem);
				added = true;
			}
			else
				CoTaskMemFree(startitem.pszAppID);
		}
		if (!added)
			ILFree(pidl);
	}

	if (games)
		games->Release();
	ILFree(gamesRoot);
	RegCloseKey(key);
}

HRESULT STDMETHODCALLTYPE CStartMenuResolver::GetStartMenuMFUList(unsigned int limit, IEnumStartMenuItem** penumStart, IEnumString** penumStrings, FILETIME* pNewFileTime)
{
	unsigned int cnt = 0;
	CEnumStartMenu* startenum = new CEnumStartMenu;
	*penumStart = (IEnumStartMenuItem*)startenum;
	*penumStrings = (IEnumString*)new CEnumStartMenu;
	//add pinned items
	IPinnedList2* startpinnedlist;
	IPinnedList2* taskbarpinnedlist;
	HRESULT rslt = Explorer_CoCreateInstance(CLSID_StartMenuPin, NULL, CLSCTX_INPROC_SERVER, IID_IPinnedList2, (PVOID*)&startpinnedlist);
	if (FAILED(rslt)) return rslt;
	rslt = Explorer_CoCreateInstance(CLSID_TaskbarPin, NULL, CLSCTX_INPROC_SERVER, IID_IPinnedList2, (PVOID*)&taskbarpinnedlist);
	if (FAILED(rslt)) return rslt;
	IEnumFullIDList* enumidlist;
	rslt = startpinnedlist->EnumObjects(&enumidlist);
	if (SUCCEEDED(rslt))
	{
		STARTMENUITEM startitem = { 0 };
		while (enumidlist->Next(1, &startitem.pidlRelative, NULL) == S_OK)
		{
			IShellItem* shellitem;
			rslt = SHCreateItemFromIDList(startitem.pidlRelative, IID_IShellItem, (LPVOID*)&shellitem);
			if (SUCCEEDED(rslt))
			{
				rslt = m_resolver8->GetAppIDForShortcut(shellitem, &startitem.pszAppID);
				if (FAILED(rslt))
				{
					dbgprintf(L"GetAppIDForShortcut failed %p (shortcut broken?!)", rslt);
					rslt = shellitem->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &startitem.pszAppID);
				}
				shellitem->Release();
				startitem.iPinPos = cnt;
				startenum->AddItem(&startitem);
				cnt++;
			}
		}
		enumidlist->Release();
	}
	//add separator
	STARTMENUITEM startitem = { 0 };
	startitem.iPinPos = -2;
	startenum->AddItem(&startitem);
	//add MFU items if needed
	if (limit > cnt)
	{
		//from start menu
		IStartMenuAppItems8* startitems;
		if (FAILED(m_resolver8->QueryInterface(IID_IStartMenuAppItems8, (LPVOID*)&startitems))) return S_FALSE;
		IObjectCollection* collection;
		if (FAILED(startitems->EnumItems(0, IID_IObjectCollection, (PVOID*)&collection))) return S_FALSE;
		UINT iLauncherCount = 0;
		UINT iLauncherItem;
		collection->GetCount(&iLauncherCount);

		// An empty cache means no MFU candidates at all, so build it and retry
		if (iLauncherCount == 0 && SUCCEEDED(EnsureAppCache()))
		{
			collection->Release();
			collection = nullptr;
			if (SUCCEEDED(startitems->EnumItems(0, IID_IObjectCollection, (PVOID*)&collection)))
				collection->GetCount(&iLauncherCount);
			dbgprintf(L"CStartMenuResolver: cache was empty, after refresh %d items", iLauncherCount);
			if (!collection)
			{
				startitems->Release();
				return S_FALSE;
			}
		}
		for (iLauncherItem = 0; iLauncherItem < iLauncherCount; iLauncherItem++)
		{
			IPropertyStore* propstore;
			if (SUCCEEDED(collection->GetAt(iLauncherItem, IID_IPropertyStore, (PVOID*)&propstore)))
			{
				PROPVARIANT pvPidl;
				PROPVARIANT pvAppId;
				PROPVARIANT pvMetro;
				PROPVARIANT pvDual;
				propstore->GetValue(PKEY_AppUserModel_BestShortcut, &pvPidl);
				propstore->GetValue(PKEY_AppUserModel_ID, &pvAppId);
				propstore->GetValue(PKEY_AppUserModel_HostEnvironment, &pvMetro);
				propstore->GetValue(PKEY_AppUserModel_IsDualMode, &pvDual);
				//we're accepting only non-metro or dualmode shortcuts
				if (!pvMetro.intVal || pvDual.intVal)
				{
					STARTMENUITEM startitem = { 0 };
					LPCWSTR appId = (pvAppId.vt == VT_LPWSTR || pvAppId.vt == VT_BSTR) ? pvAppId.pwszVal : nullptr;
					if (UAQueryMFUUsage((LPITEMIDLIST)pvPidl.caub.pElems, appId, &startitem.ueminfo))
						if (startpinnedlist->IsPinned((LPITEMIDLIST)pvPidl.caub.pElems) == S_FALSE) //IsPinned checks are VERY slow, at least under VMWare
							if (taskbarpinnedlist->IsPinned((LPITEMIDLIST)pvPidl.caub.pElems) == S_FALSE) //...why?!
							{
								startitem.pidlRelative = ILClone((LPITEMIDLIST)pvPidl.caub.pElems);
								startitem.pszAppID = CoAllocString(pvAppId.bstrVal);
								startitem.iPinPos = -1;
								startenum->AddItem(&startitem);
							}
				}
				propstore->Release();
			}
		}
		collection->Release();
		startitems->Release();
		//from desktop
		LPITEMIDLIST pidlitem;
		IShellFolder* dsf;
		IEnumIDList* enumdesktop;
		SHGetDesktopFolder(&dsf);
		dsf->EnumObjects(NULL, SHCONTF_NONFOLDERS | SHCONTF_FASTITEMS, &enumdesktop);

		while (enumdesktop->Next(1, &pidlitem, NULL) == S_OK)
		{
			SFGAOF attrs = SFGAO_LINK;
			dsf->GetAttributesOf(1, (LPCITEMIDLIST*)&pidlitem, &attrs);

			if (attrs & SFGAO_LINK)
			{
				STARTMENUITEM startitem = { 0 };
				SHGetRealIDL(dsf, pidlitem, &startitem.pidlRelative);

				if (SUCCEEDED(UAQueryShortcut(startitem.pidlRelative, &startitem.ueminfo)) &&
					startitem.ueminfo.R && !startitem.ueminfo.fExcludeFromMFU)
					if (taskbarpinnedlist->IsPinned(startitem.pidlRelative) == S_FALSE)
						if (startpinnedlist->IsPinned(startitem.pidlRelative) == S_FALSE)
						{
							IShellItem* shellitem;
							rslt = SHCreateItemFromIDList(startitem.pidlRelative, IID_IShellItem, (LPVOID*)&shellitem);
							if (SUCCEEDED(rslt))
							{
								rslt = m_resolver8->GetAppIDForShortcut(shellitem, &startitem.pszAppID);
								if (FAILED(rslt))
								{
									dbgprintf(L"GetAppIDForShortcut failed %p (shortcut broken?!)", rslt);
									rslt = shellitem->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &startitem.pszAppID);
								}
								else
								{
									// Ranked by app id usage on the same scale as the items from the cache
									UEMINFO byApp = { 0 };
									if (SUCCEEDED(UAQueryApp(startitem.pszAppID, &byApp)) && byApp.R)
										startitem.ueminfo = byApp;
								}
								shellitem->Release();
								startitem.iPinPos = -1;
								startenum->AddItem(&startitem);
							}
						}
			}

			ILFree(pidlitem);
		}
		enumdesktop->Release();
		dsf->Release();

		//from the games folder
		AddUsedGames(startenum, startpinnedlist, taskbarpinnedlist);
	}
	startpinnedlist->Release();
	taskbarpinnedlist->Release();
	startenum->Sort();
	startenum->SetLimit(limit);
	startenum->RemoveDuplicates();
	GetSystemTimeAsFileTime(pNewFileTime);
	return S_OK;
}

HRESULT STDMETHODCALLTYPE CStartMenuResolver::RegisterSMNotify(IUnknown* p1)
{
	return S_OK;
}

HRESULT STDMETHODCALLTYPE CStartMenuResolver::RegisterARNotify(IUnknown* p1)
{
	if (m_startmenuitemscache8)
		return m_startmenuitemscache8->RegisterARNotify(new CAppResolverNotify8((IAppResolverNotify7*)p1));
	else if (m_startmenuitemscache10)
		return m_startmenuitemscache10->RegisterARNotify(new CAppResolverNotify8((IAppResolverNotify7*)p1));

	return E_ABORT;
}

HRESULT STDMETHODCALLTYPE CStartMenuResolver::SetAltName(PVOID* p1, DWORD* p2, PVOID* p3)
{
	dbgprintf(L"CStartMenuResolver::SetAltName %p %p %p", p1, p2, p3);
	return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE CStartMenuResolver::GetAltName(PVOID* p1, DWORD* p2)
{
	dbgprintf(L"CStartMenuResolver::GetAltName %p %p", p1, p2);
	return E_NOTIMPL;
}
