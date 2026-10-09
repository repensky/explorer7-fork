#pragma once
#define INITGUID
#include "common.h"

#pragma region GUID definitions
DEFINE_GUID(CLSID_ExplorerLauncher, 0x1F849CCE, 0x2546, 0x4B9F, 0xB0, 0x3E, 0x40, 0x04, 0x78, 0x1B, 0xDC, 0x40); //1F849CCE-2546-4B9F-B03E-4004781BDC40
DEFINE_GUID(IID_IExplorerLauncher7, 0x578E4660, 0xE403, 0x4E8F, 0x9F, 0xD4, 0x6D, 0x55, 0x9F, 0x7A, 0x0E, 0xDC); //578E4660-E403-4E8F-9FD4-6D559F7A0EDC, asked for by 7601
DEFINE_GUID(IID_IExplorerLauncher8, 0x5AC8C8F7, 0x1CC7, 0x46CB, 0x8D, 0x7D, 0x3C, 0xF1, 0x4B, 0x64, 0x86, 0x8C); //5AC8C8F7-1CC7-46CB-8D7D-3CF14B64868C, asked for by 7850
DEFINE_GUID(IID_IExplorerLauncher10, 0x9B25C299, 0x03B6, 0x4A14, 0x82, 0x7D, 0x09, 0x54, 0x85, 0xD0, 0xC0, 0x22); //9B25C299-03B6-4A14-827D-095485D0C022, the only one 19041 and 26100 answer
DEFINE_GUID(CLSID_ExplorerHostCreator, 0xAB0B37EC, 0x56F6, 0x4A0E, 0xA8, 0xFD, 0x7A, 0x8B, 0xF7, 0xC2, 0xDA, 0x96); //AB0B37EC-56F6-4A0E-A8FD-7A8BF7C2DA96
DEFINE_GUID(IID_IExplorerHostCreator, 0xC4DE032A, 0xD902, 0x450A, 0xBC, 0x43, 0xD9, 0xDF, 0x6D, 0x0F, 0xD4, 0x8C); //C4DE032A-D902-450A-BC43-D9DF6D0FD48C, same in 7601, 7850 and 26100
#pragma endregion

// Slot 3 as the 19041 and 26100 ExplorerFrame symbols declare it
MIDL_INTERFACE("9B25C299-03B6-4A14-827D-095485D0C022")
IExplorerLauncher10 : public IUnknown
{
public:
	STDMETHOD(ShowWindow)(REFCLSID clsidHost, PCIDLIST_ABSOLUTE pidl, DWORD flags, POINT pt, int nShow, HWND hwnd, IUnknown* site, IUnknown* handshake) PURE;
};

// Slots 3 and 4, the only two 7601, 7850 and 26100 call
MIDL_INTERFACE("C4DE032A-D902-450A-BC43-D9DF6D0FD48C")
IExplorerHostCreator : public IUnknown
{
public:
	STDMETHOD(CreateHost)(REFCLSID clsidHost) PURE;
	STDMETHOD(RunHost)(void) PURE;
};

// The older launcher, slot 3 takes the same arguments minus the last three
class CExplorerLauncherWrapper : public IUnknown
{
public:
	CExplorerLauncherWrapper(IExplorerLauncher10*);

	// IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject);
	STDMETHODIMP_(ULONG) AddRef(void);
	STDMETHODIMP_(ULONG) Release(void);

	// Slot 3 of the 7601 and 7850 launcher
	virtual HRESULT STDMETHODCALLTYPE ShowWindow(REFCLSID clsidHost, PCIDLIST_ABSOLUTE pidl, DWORD flags, POINT pt, int nShow);
private:
	IExplorerLauncher10* launcher;
	long m_cref;
};

// Passes the host creator through and logs what a factory launch did
class CExplorerHostCreatorLogger : public IExplorerHostCreator
{
public:
	CExplorerHostCreatorLogger(IExplorerHostCreator*);

	// IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject);
	STDMETHODIMP_(ULONG) AddRef(void);
	STDMETHODIMP_(ULONG) Release(void);

	// IExplorerHostCreator
	STDMETHODIMP CreateHost(REFCLSID clsidHost);
	STDMETHODIMP RunHost(void);
private:
	IExplorerHostCreator* creator;
	long m_cref;
};

// Folder launches and factory launches from a second explorer process
HRESULT WrapExplorerLauncher(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext, REFIID riid, LPVOID* ppv, HRESULT result);
