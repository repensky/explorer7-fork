#include "PinnedList.h"
#include "dbgprint.h"
#include "OSVersion.h"
#include "StartMenuResolver.h"

// 26100 fails a Modify with no first pidl inside InternalModify, and the shim it prefers is a stub
// PinShellLink still runs the whole legacy add, backup shortcut, list append, save and notify
HRESULT PinNewItemViaShellLink(IPinnedList3* list, PCIDLIST_ABSOLUTE pidl)
{
	if (!list || !pidl)
		return E_INVALIDARG;

	IShellLinkW* link = NULL;
	HRESULT hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
	if (FAILED(hr))
		return hr;

	// A shortcut is loaded as it is, anything else gets a fresh link pointing at it
	WCHAR path[MAX_PATH] = L"";
	bool loaded = false;
	if (SHGetPathFromIDListW(pidl, path) && lstrcmpiW(PathFindExtensionW(path), L".lnk") == 0)
	{
		IPersistFile* file = NULL;
		if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))))
		{
			loaded = SUCCEEDED(file->Load(path, STGM_READ));
			file->Release();
		}
	}
	if (!loaded)
		hr = link->SetIDList(pidl);

	// The first argument becomes the backup shortcut's file name, so it is the item's display name
	// The app id is worked out by shell32 from the link itself once the shortcut exists
	LPWSTR name = NULL;
	if (SUCCEEDED(hr))
	{
		IShellItem* item = NULL;
		if (SUCCEEDED(SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&item))))
		{
			if (FAILED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)))
				name = NULL;
			item->Release();
		}

		// A bare file name is a good enough label when the shell will not name the item
		if (!name && path[0])
			SHStrDupW(PathFindFileNameW(path), &name);

		hr = name ? list->PinShellLink(name, link) : E_FAIL;
	}

	dbgprintf(L"PinNewItemViaShellLink [%s] name [%s] loaded=%d hr=%08X", path, name ? name : L"", (int)loaded, hr);
	if (name)
		CoTaskMemFree(name);
	link->Release();
	return hr;
}

CPinnedListWrapper::CPinnedListWrapper(IUnknown* flex, int build)
{
	m_build = build;
	if (build >= 10240 && build < 14393)
		m_pinnedList25 = (IPinnedList25*)flex;
	else if (build >= 14393 && build < 17763)
		m_flexList = (IFlexibleTaskbarPinnedList*)flex;
	else if (build >= 17763)
		m_pinnedList3 = (IPinnedList3*)flex;
}

CPinnedListWrapper::~CPinnedListWrapper()
{
	if (m_pinnedList25)
		m_pinnedList25->Release();
	if (m_flexList)
		m_flexList->Release();
	if (m_pinnedList3)
		m_pinnedList3->Release();
}

HRESULT __stdcall CPinnedListWrapper::QueryInterface(REFIID riid, void** ppvObject)
{
	if (m_pinnedList25)
		return m_pinnedList25->QueryInterface(riid, ppvObject);
	if (m_flexList)
		return m_flexList->QueryInterface(riid, ppvObject);
	if (m_pinnedList3)
		return m_pinnedList3->QueryInterface(riid, ppvObject);
	return S_OK;
}

ULONG __stdcall CPinnedListWrapper::AddRef(void)
{
	ULONG cref;
	if (m_pinnedList25)
		cref = m_pinnedList25->AddRef();
	if (m_flexList)
		cref = m_flexList->AddRef();
	if (m_pinnedList3)
		cref = m_pinnedList3->AddRef();
	return cref;
}

ULONG __stdcall CPinnedListWrapper::Release(void)
{
	ULONG cref;
	if (m_pinnedList25)
		cref = m_pinnedList25->Release();
	if (m_flexList)
		cref = m_flexList->Release();
	if (m_pinnedList3)
		cref = m_pinnedList3->Release();
	if (cref == 0)
		free((void*)this);
	return cref;
}

//.text:00007FF6BEB24439 explorer.exe:$94439 #93A39
HRESULT __stdcall CPinnedListWrapper::EnumObjects(IEnumFullIDList** p1)
{
	if (m_pinnedList25)
		return m_pinnedList25->EnumObjects(p1);
	if (m_flexList)
		return m_flexList->EnumObjects(p1);
	if (m_pinnedList3)
		return m_pinnedList3->EnumObjects(p1);
	return S_OK;
}

HRESULT __stdcall CPinnedListWrapper::Modify(PCIDLIST_ABSOLUTE p1, PCIDLIST_ABSOLUTE p2)
{
	if (m_pinnedList25)
		return m_pinnedList25->Modify(p1, p2);
	if (m_flexList)
		return m_flexList->Modify(p1, p2);
	if (m_pinnedList3)
	{
		// 26100 answers a brand new pin with a failure, so it goes in through the shortcut route
		if (!p1 && p2 && m_build >= 26100)
			return PinNewItemViaShellLink(m_pinnedList3, p2);

		HRESULT hr = m_pinnedList3->Modify(p1, p2, 18);
		dbgprintf(L"CPinnedListWrapper::Modify %p %p hr=%08X", p1, p2, hr);
		return hr;
	}
	return S_OK;
}

HRESULT __stdcall CPinnedListWrapper::GetChangeCount(ULONG* p1)
{
	if (m_pinnedList25)
		return m_pinnedList25->GetChangeCount(p1);
	if (m_flexList)
		return m_flexList->GetChangeCount(p1);
	if (m_pinnedList3)
		return m_pinnedList3->GetChangeCount(p1);
	return S_OK;
}

HRESULT __stdcall CPinnedListWrapper::GetPinnableInfo(IDataObject* p1, int p2, IShellItem2** p3, IShellItem** p4, PWSTR* p5, INT* p6)
{
	if (!s_UseTaskbarPinning)
	{
		// in this situation, we don't take the information
		// this is to prevent potential issues arising with people trying to pin despite being in this mode
		return E_NOINTERFACE;
	}

	if (m_pinnedList25)
		return m_pinnedList25->GetPinnableInfo(p1, p2, p3, p4, p5, p6);
	if (m_flexList)
		return m_flexList->GetPinnableInfo(p1, p2, p3, p4, p5, p6);
	if (m_pinnedList3)
		return m_pinnedList3->GetPinnableInfo(p1, p2, p3, p4, p5, p6);
	return S_OK;
}

HRESULT __stdcall CPinnedListWrapper::IsPinnable(IDataObject* p1, int p2)
{
	if (m_pinnedList25)
		return m_pinnedList25->IsPinnable(p1, p2);
	if (m_flexList)
		return m_flexList->IsPinnable(p1, p2);
	if (m_pinnedList3)
		return m_pinnedList3->IsPinnable(p1, p2);
	return S_OK;
}

HRESULT __stdcall CPinnedListWrapper::Resolve(HWND p1, ULONG p2, PCIDLIST_ABSOLUTE p3, PIDLIST_ABSOLUTE* p4)
{
	if (m_pinnedList25)
		return m_pinnedList25->Resolve(p1, p2, p3, p4);
	if (m_flexList)
		return m_flexList->Resolve(p1, p2, p3, p4);
	if (m_pinnedList3)
		return m_pinnedList3->Resolve(p1, p2, p3, p4);
	return S_OK;
}

HRESULT __stdcall CPinnedListWrapper::IsPinned(PCIDLIST_ABSOLUTE p1)
{
	if (m_pinnedList25)
		return m_pinnedList25->IsPinned(p1);
	if (m_flexList)
		return m_flexList->IsPinned(p1);
	if (m_pinnedList3)
		return m_pinnedList3->IsPinned(p1);
	return S_OK;
}

HRESULT __stdcall CPinnedListWrapper::GetPinnedItem(PCIDLIST_ABSOLUTE p1, PIDLIST_ABSOLUTE* p2)
{
	if (m_pinnedList25)
		return m_pinnedList25->GetPinnedItem(p1, p2);
	if (m_flexList)
		return m_flexList->GetPinnedItem(p1, p2);
	if (m_pinnedList3)
		return m_pinnedList3->GetPinnedItem(p1, p2);
	return S_OK;
}

HRESULT __stdcall CPinnedListWrapper::GetAppIDForPinnedItem(PCIDLIST_ABSOLUTE p1, PWSTR* p2)
{
	// Ittr: Determine whether we should hide immersive items
	bool bHideImmersivePidl = false;

	// Only bother running the filtering code if the option to show store applications on the taskbar is disabled
	if (!s_ShowStoreAppsOnTaskbar)
	{
		ITEMIDLIST_ABSOLUTE* pidlApplicationFolder;
		if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_AppsFolder, KF_FLAG_DONT_VERIFY, nullptr, &pidlApplicationFolder)))
		{
			bHideImmersivePidl = ILIsParent(pidlApplicationFolder, p1, TRUE);
		}
		CoTaskMemFree(pidlApplicationFolder);
	}

	// Cause the interface to intentionally fail if pinning is disabled or in the cases where immersive items should be hidden
	if (!s_UseTaskbarPinning || bHideImmersivePidl)
	{
		return E_NOINTERFACE;
	}

	// Pass through to the appropriate PinnedList interface for the user's version of Windows
	if (m_pinnedList25)
		return m_pinnedList25->GetAppIDForPinnedItem(p1, p2);
	if (m_flexList)
		return m_flexList->GetAppIDForPinnedItem(p1, p2);
	if (m_pinnedList3)
		return m_pinnedList3->GetAppIDForPinnedItem(p1, p2);
	return S_OK;
}

HRESULT __stdcall CPinnedListWrapper::ItemChangeNotify(PCIDLIST_ABSOLUTE p1, PCIDLIST_ABSOLUTE p2)
{
	if (m_pinnedList25)
		return m_pinnedList25->ItemChangeNotify(p1, p2);
	if (m_flexList)
		return m_flexList->ItemChangeNotify(p1, p2);
	if (m_pinnedList3)
		return m_pinnedList3->ItemChangeNotify(p1, p2);
	return S_OK;
}

HRESULT __stdcall CPinnedListWrapper::UpdateForRemovedItemsAsNecessary(VOID)
{
	if (m_pinnedList25)
		return m_pinnedList25->UpdateForRemovedItemsAsNecessary();
	if (m_flexList)
		return m_flexList->UpdateForRemovedItemsAsNecessary();
	if (m_pinnedList3)
		return m_pinnedList3->UpdateForRemovedItemsAsNecessary();
	return S_OK;
}

// PinInitialItems asks for the pinned Explorer by app id, a missing slot here jumped to garbage and crashed the first logon
HRESULT __stdcall CPinnedListWrapper::GetPinnedItemForAppID(PCWSTR appId, PIDLIST_ABSOLUTE* ppidl)
{
	if (!ppidl)
		return E_POINTER;
	*ppidl = nullptr;

	HRESULT hr = E_NOTIMPL;
	if (m_flexList)
		hr = m_flexList->GetPinnedItemForAppID(appId, ppidl);
	else if (m_pinnedList3)
		hr = m_pinnedList3->GetPinnedItemForAppID(appId, ppidl);
	return hr;
}
