#pragma once
#include <Windows.h>
#include <Shlwapi.h>

// Resolve the Explorer7 base dir: HKLM Explorer\Advanced\WrpPath if set,
// otherwise the folder of the host executable (as before)
inline void GetExplorer7BaseDir(LPWSTR szDir, DWORD cchDir)
{
	DWORD cb = cchDir * sizeof(WCHAR);
	szDir[0] = L'\0';
	// accept REG_SZ and REG_EXPAND_SZ, RegGetValue expands %SystemRoot% and returns REG_SZ
	LSTATUS res = RegGetValueW(
		HKEY_LOCAL_MACHINE,
		L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
		L"WrpPath",
		RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
		nullptr,
		szDir,
		&cb);
	if (res == ERROR_SUCCESS && szDir[0])
	{
		DWORD n = 0;
		while (szDir[n]) ++n;
		if (n && szDir[n - 1] == L'\\')
			szDir[n - 1] = L'\0';
		return;
	}

	GetModuleFileNameW(NULL, szDir, cchDir);
	WCHAR *bs = StrRChrW(szDir, NULL, L'\\');
	if (bs && *bs == L'\\')
		*bs = L'\0';
}
