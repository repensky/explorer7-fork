#include "OSVersion.h"

typedef NTSTATUS (NTAPI *RtlGetVersion_t)(PRTL_OSVERSIONINFOEXW);

void COSVersion::_FillVersionInfo()
{
	// read the real Windows version from the registry, not the spoofable process version
	HKEY hKey;
	BOOL gotBuild = FALSE;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
		L"Software\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
	{
		DWORD val = 0, sz = sizeof(DWORD);
		if (RegQueryValueExW(hKey, L"CurrentMajorVersionNumber", 0, NULL, (LPBYTE)&val, &sz) == ERROR_SUCCESS)
			m_osvi.dwMajorVersion = val;
		val = 0; sz = sizeof(DWORD);
		if (RegQueryValueExW(hKey, L"CurrentMinorVersionNumber", 0, NULL, (LPBYTE)&val, &sz) == ERROR_SUCCESS)
			m_osvi.dwMinorVersion = val;
		WCHAR bld[32] = {}; DWORD bsz = sizeof(bld);
		if (RegQueryValueExW(hKey, L"CurrentBuildNumber", 0, NULL, (LPBYTE)bld, &bsz) == ERROR_SUCCESS && bld[0])
		{
			ULONG b = 0;
			for (int i = 0; bld[i] >= L'0' && bld[i] <= L'9'; ++i)
				b = b * 10 + (bld[i] - L'0');
			m_osvi.dwBuildNumber = b;
			gotBuild = TRUE;
		}
		RegCloseKey(hKey);
	}

	// fall back to RtlGetVersion only if the registry did not provide the build
	if (!gotBuild || !m_osvi.dwMajorVersion)
	{
		HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
		RtlGetVersion_t RtlGetVersion = (RtlGetVersion_t)GetProcAddress(hNtDll, "RtlGetVersion");
		if (RtlGetVersion)
			RtlGetVersion(&m_osvi);
	}
}

COSVersion::COSVersion()
{
	ZeroMemory(&m_osvi, sizeof(RTL_OSVERSIONINFOEXW));
	m_osvi.dwOSVersionInfoSize = sizeof(RTL_OSVERSIONINFOEXW);
}

ULONG COSVersion::BuildNumber()
{
	if (!m_osvi.dwBuildNumber)
		_FillVersionInfo();
	return m_osvi.dwBuildNumber;
}

ULONG COSVersion::MajorVersion()
{
	if (!m_osvi.dwMajorVersion)
		_FillVersionInfo();
	return m_osvi.dwMajorVersion;
}

ULONG COSVersion::MinorVersion()
{
	if (!m_osvi.dwMinorVersion)
		_FillVersionInfo();
	return m_osvi.dwMinorVersion;
}

// https://stackoverflow.com/questions/47926094/detecting-windows-10-os-build-minor-version
ULONG COSVersion::BuildRevision()
{
    DWORD ubr = 0, ubr_size = sizeof(DWORD);
    HKEY hKey;
    LONG lRes = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        L"Software\\Microsoft\\Windows NT\\CurrentVersion",
        0,
        KEY_READ,
        &hKey
    );
    if (lRes == ERROR_SUCCESS)
    {
        RegQueryValueExW(
            hKey,
            UNIFIEDBUILDREVISION_VALUE,
            0,
            NULL,
            (LPBYTE)&ubr,
            (LPDWORD)&ubr_size
        );
    }
    return ubr;
}