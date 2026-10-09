#include "UserAssist.h"

IUserAssist* g_UserAssist;

extern "C" HRESULT WINAPI Explorer_CoCreateInstance(
	__in   REFCLSID rclsid,
	__in   LPUNKNOWN pUnkOuter,
	__in   DWORD dwClsContext,
	__in   REFIID riid,
	__out  LPVOID * ppv
);

HRESULT WINAPI UAQueryEntry(REFIID iid, LPWSTR parsingname, PUEMINFO uem)
{	
	if (!g_UserAssist) //bugbug shpindllofclsid?
		Explorer_CoCreateInstance(CLSID_UserAssist,NULL,CLSCTX_INPROC_SERVER || CLSCTX_INPROC_HANDLER || CLSCTX_NO_CODE_DOWNLOAD,IID_IUserAssist7,(PVOID*)&g_UserAssist);
	if ( g_UserAssist )
		return g_UserAssist->QueryEntry(iid,parsingname,uem);
	else
		return E_NOINTERFACE;
}

HRESULT WINAPI UAQueryShortcut(LPITEMIDLIST pidl, PUEMINFO uem)
{
	HRESULT result;
	IShellItem* shellitem;
	result = SHCreateItemFromIDList(pidl,IID_IShellItem,(LPVOID*)&shellitem);
	if ( SUCCEEDED(result) )
	{
		LPWSTR parsingname;
		result = shellitem->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING,&parsingname);
		if ( SUCCEEDED(result) )
		{
			uem->cbSize = sizeof(UEMINFO);
			uem->dwMask = 0x31;			
			result = UAQueryEntry(UAIID_SHORTCUTS,parsingname,uem);		
			CoTaskMemFree(parsingname);
		}
	}
	return result;
}

HRESULT WINAPI UAQueryApp(LPCWSTR appId, PUEMINFO uem)
{
	uem->cbSize = sizeof(UEMINFO);
	uem->dwMask = 0x31;
	return UAQueryEntry(UAIID_APPLICATIONS, (LPWSTR)appId, uem);
}

// Windows 7 ranks the list by app id usage alone, read from shell32 7601 s_CombineUAInfoCB
BOOL WINAPI UAQueryMFUUsage(LPITEMIDLIST pidl, LPCWSTR appId, PUEMINFO uem)
{
	HRESULT hr = appId ? UAQueryApp(appId, uem) : E_INVALIDARG;

	// Only an item without an app id falls back to the usage of the shortcut itself
	if (FAILED(hr))
		hr = UAQueryShortcut(pidl, uem);
	return SUCCEEDED(hr) && uem->R && !uem->fExcludeFromMFU;
}

LPWSTR WINAPI CoAllocString(LPWSTR src)
{	
	LPWSTR dst = (LPWSTR)CoTaskMemAlloc(((lstrlenW(src)+1)*sizeof(WCHAR)));
	lstrcpyW(dst,src);
	return dst;
}