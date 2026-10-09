#include "AuthUI.h"
#include "dbgprint.h"

#ifndef REG_NOTIFY_THREAD_AGNOSTIC
#define REG_NOTIFY_THREAD_AGNOSTIC 0x10000000L
#endif

// Posted from the pool thread when the power key changes
#define WM_SHUTDOWNCHOICES_POWERKEY (WM_APP + 0x51)

static const WCHAR c_szListenerClass[] = L"Explorer7: Shutdown Choices Message Window";

CShutdownChoiceListener::CShutdownChoiceListener()
{
}

CShutdownChoiceListener::~CShutdownChoiceListener()
{
	StopListening();
}

HRESULT STDMETHODCALLTYPE CShutdownChoiceListener::QueryInterface(REFIID riid, void** ppvObject)
{
	if (!ppvObject)
		return E_POINTER;
	if (riid == IID_IUnknown)
	{
		*ppvObject = static_cast<IUnknown*>(this);
		AddRef();
		return S_OK;
	}
	*ppvObject = nullptr;
	return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE CShutdownChoiceListener::AddRef(void)
{
	return InterlockedIncrement(&m_cRef);
}

ULONG STDMETHODCALLTYPE CShutdownChoiceListener::Release(void)
{
	long cRef = InterlockedDecrement(&m_cRef);
	if (cRef == 0)
		delete this;
	return cRef;
}

HRESULT STDMETHODCALLTYPE CShutdownChoiceListener::SetNotifyWnd(HWND hwnd, UINT id)
{
	m_hwndNotify = hwnd;
	m_idNotify = id;
	return S_OK;
}

HRESULT STDMETHODCALLTYPE CShutdownChoiceListener::GetMessageWnd(HWND* phwnd)
{
	if (!phwnd)
		return E_POINTER;
	*phwnd = m_hwndMessage;
	return S_OK;
}

HRESULT STDMETHODCALLTYPE CShutdownChoiceListener::ScanForPassiveChanges(void)
{
	_SendClientNotification();
	return S_OK;
}

HRESULT STDMETHODCALLTYPE CShutdownChoiceListener::StartListening(void)
{
	if (m_hwndMessage)
		return S_OK;

	WNDCLASSW wc = {};
	wc.lpfnWndProc = s_MessageWndProc;
	wc.hInstance = g_hInstance;
	wc.lpszClassName = c_szListenerClass;
	if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
		return HRESULT_FROM_WIN32(GetLastError());

	// Hidden top level window rather than message only, so setting change broadcasts reach it
	m_hwndMessage = CreateWindowExW(0, c_szListenerClass, L"", 0, 0, 0, 0, 0, nullptr, nullptr, g_hInstance, nullptr);
	if (!m_hwndMessage)
		return HRESULT_FROM_WIN32(GetLastError());
	SetWindowLongPtrW(m_hwndMessage, GWLP_USERDATA, (LONG_PTR)this);

	// The same three settings Windows 7 authui listened for
	const GUID* settings[] = { &GUID_ACDC_POWER_SOURCE, &GUID_HIBERNATE_FASTS4_POLICY, &GUID_USERINTERFACEBUTTON_ACTION };
	for (int i = 0; i < ARRAYSIZE(settings); i++)
		m_powerNotify[i] = RegisterPowerSettingNotification(m_hwndMessage, settings[i], DEVICE_NOTIFY_WINDOW_HANDLE);

	// Watch the power key so hibernation turned on or off reaches the menu
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power", 0, KEY_NOTIFY, &m_powerKey) == ERROR_SUCCESS)
	{
		m_powerKeyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (m_powerKeyEvent)
		{
			_WatchPowerKey();
			RegisterWaitForSingleObject(&m_powerKeyWait, m_powerKeyEvent, s_PowerKeyChanged, this, INFINITE, WT_EXECUTEDEFAULT);
		}
	}

	dbgprintf(L"ShutdownChoiceListener: listening, window %p, power key watch %p", m_hwndMessage, m_powerKeyWait);
	return S_OK;
}

HRESULT STDMETHODCALLTYPE CShutdownChoiceListener::StopListening(void)
{
	// The wait goes first so no pool callback can still be running against this object
	if (m_powerKeyWait)
	{
		UnregisterWaitEx(m_powerKeyWait, INVALID_HANDLE_VALUE);
		m_powerKeyWait = nullptr;
	}
	if (m_powerKeyEvent)
	{
		CloseHandle(m_powerKeyEvent);
		m_powerKeyEvent = nullptr;
	}
	if (m_powerKey)
	{
		RegCloseKey(m_powerKey);
		m_powerKey = nullptr;
	}
	for (int i = 0; i < ARRAYSIZE(m_powerNotify); i++)
	{
		if (m_powerNotify[i])
		{
			UnregisterPowerSettingNotification(m_powerNotify[i]);
			m_powerNotify[i] = nullptr;
		}
	}
	if (m_hwndMessage)
	{
		DestroyWindow(m_hwndMessage);
		m_hwndMessage = nullptr;
	}
	return S_OK;
}

void CShutdownChoiceListener::_WatchPowerKey()
{
	RegNotifyChangeKeyValue(m_powerKey, FALSE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC, m_powerKeyEvent, TRUE);
}

// The logoff pane refreshes when a notify arrives from the window GetMessageWnd handed out
void CShutdownChoiceListener::_SendClientNotification()
{
	if (!m_hwndNotify)
		return;
	NMHDR nm = { m_hwndMessage, m_idNotify, 0 };
	SendMessageW(m_hwndNotify, WM_NOTIFY, m_idNotify, (LPARAM)&nm);
}

VOID CALLBACK CShutdownChoiceListener::s_PowerKeyChanged(PVOID context, BOOLEAN timedOut)
{
	// Pool thread, so hand the change over to the window thread
	CShutdownChoiceListener* self = (CShutdownChoiceListener*)context;
	PostMessageW(self->m_hwndMessage, WM_SHUTDOWNCHOICES_POWERKEY, 0, 0);
}

LRESULT CALLBACK CShutdownChoiceListener::s_MessageWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	CShutdownChoiceListener* self = (CShutdownChoiceListener*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
	if (self)
	{
		switch (uMsg)
		{
		case WM_SETTINGCHANGE:
			self->_SendClientNotification();
			return 0;
		case WM_POWERBROADCAST:
			if (wParam == PBT_POWERSETTINGCHANGE)
			{
				self->_SendClientNotification();
				return TRUE;
			}
			break;
		case WM_SHUTDOWNCHOICES_POWERKEY:
			// Rearm before refreshing so a change during the refresh is not lost
			self->_WatchPowerKey();
			dbgprintf(L"ShutdownChoiceListener: power key changed, refreshing the logoff pane");
			self->_SendClientNotification();
			return 0;
		}
	}
	return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

CAuthUIWrapper::CAuthUIWrapper(IUnknown* authui)
{
	m_cRef = 1;
	m_authui10 = (IShutdownChoices10*)authui;
}

CAuthUIWrapper::~CAuthUIWrapper()
{
	if (m_authui10)
		m_authui10->Release();
}

HRESULT STDMETHODCALLTYPE CAuthUIWrapper::QueryInterface(REFIID riid, void** ppvObject)
{
	if (m_authui10)
		return m_authui10->QueryInterface(riid, ppvObject);
	return S_OK;
}

ULONG STDMETHODCALLTYPE CAuthUIWrapper::AddRef(void)
{
	if (m_authui10)
		m_authui10->AddRef();
	return InterlockedIncrement(&m_cRef);
}

ULONG STDMETHODCALLTYPE CAuthUIWrapper::Release(void)
{
	if (m_authui10)
		m_authui10->Release();
	if (InterlockedDecrement(&m_cRef) == 0)
	{
		free((void*)this);
		return 0;
	}
	return m_cRef;
}

HRESULT STDMETHODCALLTYPE CAuthUIWrapper::Refresh()
{
	if (m_authui10)
		return m_authui10->Refresh();
	return S_OK;
}

HRESULT STDMETHODCALLTYPE CAuthUIWrapper::CreateListener(IUnknown** p1)
{
	// Windows 10 has no listener, so the logoff pane gets ours
	if (!p1)
		return E_POINTER;
	*p1 = new CShutdownChoiceListener();
	return *p1 ? S_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE CAuthUIWrapper::SetChoiceMask(ULONG p1)
{
	p1 = p1 & ~0x200000;
	if (m_authui10)
		return m_authui10->SetChoiceMask(p1);
	return S_OK;
}

HRESULT STDMETHODCALLTYPE CAuthUIWrapper::GetMessageWnd(HWND** p1)
{
	return E_NOTIMPL;
	//return m_authui->GetMessageWnd(p1);
}

HRESULT STDMETHODCALLTYPE CAuthUIWrapper::SetShowBadChoices(int p1)
{
	if (m_authui10)
		return m_authui10->SetShowBadChoices(p1);
	return S_OK;
}

HRESULT STDMETHODCALLTYPE CAuthUIWrapper::GetChoiceEnumerator(IUnknown** p1)
{
	if (m_authui10)
	{
		// The power menu is built from this list, and shutdownux only rereads sleep and hibernate on Refresh
		m_authui10->Refresh();
		return m_authui10->GetChoiceEnumerator(p1);
	}
	return S_OK;
}

HRESULT STDMETHODCALLTYPE CAuthUIWrapper::GetDefaultChoice(ULONG* p1)
{
	if (m_authui10)
		return m_authui10->GetDefaultChoice(p1);
	return S_OK;
}

HRESULT STDMETHODCALLTYPE CAuthUIWrapper::UserHasShutdownRights(void)
{
	if (m_authui10)
		return m_authui10->UserHasShutdownRights();
	return S_OK;
}

HRESULT STDMETHODCALLTYPE CAuthUIWrapper::GetChoiceName(ULONG p1, int p2, LPWSTR p3, UINT p4)
{
	if (m_authui10)
		return m_authui10->GetChoiceName(p1, p2, p3, p4);
	return S_OK;
}

HRESULT STDMETHODCALLTYPE CAuthUIWrapper::GetChoiceDesc(ULONG p1, LPWSTR p2, UINT p3)
{
	if (m_authui10)
		return  m_authui10->GetChoiceDesc(p1, p2, p3);
	return S_OK;
}
