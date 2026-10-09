#include "ExplorerLauncher.h"
#include "dbgprint.h"

//---Launcher wrapper---------------------------------------

CExplorerLauncherWrapper::CExplorerLauncherWrapper(IExplorerLauncher10* pLauncher)
{
	m_cref = 1;
	launcher = pLauncher;
}

HRESULT __stdcall CExplorerLauncherWrapper::QueryInterface(REFIID riid, void** ppvObject)
{
	if (!ppvObject)
		return E_POINTER;

	if (riid == IID_IUnknown || riid == IID_IExplorerLauncher7 || riid == IID_IExplorerLauncher8)
	{
		*ppvObject = static_cast<IUnknown*>(this);
		AddRef();
		return S_OK;
	}
	return launcher->QueryInterface(riid, ppvObject);
}

ULONG __stdcall CExplorerLauncherWrapper::AddRef(void)
{
	return InterlockedIncrement(&m_cref);
}

ULONG __stdcall CExplorerLauncherWrapper::Release(void)
{
	long cref = InterlockedDecrement(&m_cref);
	if (cref == 0)
	{
		launcher->Release();
		delete this;
	}
	return cref;
}

// The three new arguments are null, as the 26100 explorer passes them
HRESULT STDMETHODCALLTYPE CExplorerLauncherWrapper::ShowWindow(REFCLSID clsidHost, PCIDLIST_ABSOLUTE pidl, DWORD flags, POINT pt, int nShow)
{
	HRESULT hr = launcher->ShowWindow(clsidHost, pidl, flags, pt, nShow, nullptr, nullptr, nullptr);
	dbgprintf(L"explorer7: launcher ShowWindow flags %X show %d, hr %08X", flags, nShow, hr);
	return hr;
}

//---Host creator logger------------------------------------

CExplorerHostCreatorLogger::CExplorerHostCreatorLogger(IExplorerHostCreator* pCreator)
{
	m_cref = 1;
	creator = pCreator;
}

HRESULT __stdcall CExplorerHostCreatorLogger::QueryInterface(REFIID riid, void** ppvObject)
{
	if (!ppvObject)
		return E_POINTER;

	if (riid == IID_IUnknown || riid == IID_IExplorerHostCreator)
	{
		*ppvObject = static_cast<IExplorerHostCreator*>(this);
		AddRef();
		return S_OK;
	}
	return creator->QueryInterface(riid, ppvObject);
}

ULONG __stdcall CExplorerHostCreatorLogger::AddRef(void)
{
	return InterlockedIncrement(&m_cref);
}

ULONG __stdcall CExplorerHostCreatorLogger::Release(void)
{
	long cref = InterlockedDecrement(&m_cref);
	if (cref == 0)
	{
		creator->Release();
		delete this;
	}
	return cref;
}

HRESULT __stdcall CExplorerHostCreatorLogger::CreateHost(REFCLSID clsidHost)
{
	HRESULT hr = creator->CreateHost(clsidHost);

	WCHAR szHost[40] = L"";
	StringFromGUID2(clsidHost, szHost, ARRAYSIZE(szHost));
	dbgprintf(L"explorer7: factory CreateHost %s, hr %08X", szHost, hr);
	return hr;
}

// Runs the message loop until the last window in this host closes
HRESULT __stdcall CExplorerHostCreatorLogger::RunHost(void)
{
	dbgprintf(L"explorer7: factory RunHost starting");
	HRESULT hr = creator->RunHost();
	dbgprintf(L"explorer7: factory RunHost returned %08X", hr);
	return hr;
}

//---CoCreateInstance hook----------------------------------

HRESULT WrapExplorerLauncher(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext, REFIID riid, LPVOID* ppv, HRESULT result)
{
	if (rclsid == CLSID_ExplorerLauncher && (riid == IID_IExplorerLauncher7 || riid == IID_IExplorerLauncher8))
	{
		// A frame that still answers the old interface is left alone
		if (result != S_OK)
		{
			IExplorerLauncher10* launcher = nullptr;
			result = CoCreateInstance(rclsid, pUnkOuter, dwClsContext, IID_IExplorerLauncher10, (void**)&launcher);
			if (SUCCEEDED(result))
				*ppv = static_cast<IUnknown*>(new CExplorerLauncherWrapper(launcher));
		}
		dbgprintf(L"explorer7: launcher for %.300s, hr %08X", GetCommandLineW(), result);
	}
	else if (rclsid == CLSID_ExplorerHostCreator && riid == IID_IExplorerHostCreator)
	{
		// The shell creates one for its own desktop host, only a factory launch is wrapped
		bool factory = StrStrIW(GetCommandLineW(), L"/factory") != nullptr;
		if (factory && result == S_OK && *ppv)
			*ppv = static_cast<IExplorerHostCreator*>(new CExplorerHostCreatorLogger((IExplorerHostCreator*)*ppv));
		dbgprintf(L"explorer7: host creator for %.300s, hr %08X", GetCommandLineW(), result);
	}
	return result;
}
