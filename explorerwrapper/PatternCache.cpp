#include "dbgprint.h"
#include <psapi.h>

//---Pattern cache------------------------------------------

// Lookup results kept in a text file beside wrp64.dll, one line per pattern
// A line only counts while Windows, wrp64 and the scanned module are the same builds

static const char c_szCacheHeader[] = "# wrp64 pattern cache v1";
static const WCHAR c_szCacheFile[] = L"wrp64.patterns.cache";

// Stored as the result of a pattern the module does not contain
static const DWORD c_missRva = 0xFFFFFFFF;

// Larger than any cache this can produce, a bigger file is not ours
static const DWORD c_maxCacheBytes = 4 * 1024 * 1024;

// The same search can run twice in one start after its first hit was patched
// The sequence number keeps each of those lookups on its own line
struct CacheEntry
{
	const char* module;
	const char* token;
	const char* signature;
	DWORD after;
	DWORD seq;
	DWORD result;
	bool dead;
};

struct CacheModule
{
	uintptr_t base;
	char name[64];
	char token[192];
};

// How many times this process has made each search so far
struct RunCount
{
	const CacheModule* module;
	const char* signature;
	DWORD after;
	DWORD count;
};

static RunCount* g_runCounts;
static int g_runCountUsed;
static int g_runCountCap;

static SRWLOCK g_cacheLock = SRWLOCK_INIT;
static bool g_cacheLoaded;
static bool g_cacheDirty;
static bool g_cacheWriteFailed;
static WCHAR g_cachePath[MAX_PATH];
static char* g_fileText;
static CacheEntry* g_entries;
static int g_entryCount;
static int g_entryCap;
static CacheModule g_modules[48];
static int g_moduleCount;

// Raised for the whole of DllMain attach, before anything else has patched code
static volatile LONG g_patternCacheAttach;

static IMAGE_NT_HEADERS* ImageHeaders(uintptr_t base)
{
	auto dos = (IMAGE_DOS_HEADER*)base;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return nullptr;

	auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
	return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
}

//---Build token--------------------------------------------

// The servicing revision, the part of the build KUSER_SHARED_DATA does not carry
static DWORD ReadUbr()
{
	static DWORD ubr = 0xFFFFFFFF;
	if (ubr == 0xFFFFFFFF)
	{
		DWORD value = 0;
		DWORD cb = sizeof(value);
		if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
			L"UBR", RRF_RT_REG_DWORD, nullptr, &value, &cb) != ERROR_SUCCESS)
			value = 0;
		ubr = value;
	}
	return ubr;
}

// Read from the image in memory, the swapped explorer.exe disagrees with its file on disk
static void ReadFileVersion(HMODULE module, DWORD* ms, DWORD* ls)
{
	*ms = 0;
	*ls = 0;

	HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(1), RT_VERSION);
	if (!res)
		return;

	HGLOBAL data = LoadResource(module, res);
	const BYTE* p = data ? (const BYTE*)LockResource(data) : nullptr;
	DWORD size = SizeofResource(module, res);
	if (!p)
		return;

	// VS_FIXEDFILEINFO starts with this signature a few dwords into the block
	for (DWORD off = 0; off + 16 <= size && off < 256; off += 4)
	{
		if (*(const DWORD*)(p + off) == 0xFEEF04BD)
		{
			*ms = *(const DWORD*)(p + off + 8);
			*ls = *(const DWORD*)(p + off + 12);
			return;
		}
	}
}

static void BuildModuleToken(uintptr_t base, char* out, size_t cch)
{
	IMAGE_NT_HEADERS* self = ImageHeaders((uintptr_t)g_hInstance);
	IMAGE_NT_HEADERS* mod = ImageHeaders(base);

	DWORD verMs = 0;
	DWORD verLs = 0;
	ReadFileVersion((HMODULE)base, &verMs, &verLs);

	// wsprintf caps at 1024 characters, far above the longest token this can make
	char text[1024];
	wsprintfA(text,
		"os=%u.%u.%u.%u wrp=%08X-%08X mod=%08X-%08X-%08X ver=%u.%u.%u.%u",
		*(volatile DWORD*)0x7FFE026C, *(volatile DWORD*)0x7FFE0270,
		*(volatile DWORD*)0x7FFE0260, ReadUbr(),
		self ? self->FileHeader.TimeDateStamp : 0, self ? self->OptionalHeader.SizeOfImage : 0,
		mod ? mod->FileHeader.TimeDateStamp : 0, mod ? mod->OptionalHeader.SizeOfImage : 0,
		mod ? mod->OptionalHeader.CheckSum : 0,
		HIWORD(verMs), LOWORD(verMs), HIWORD(verLs), LOWORD(verLs));
	StringCchCopyA(out, cch, text);
}

//---Cache file---------------------------------------------

static bool AddEntry(const char* module, const char* token, const char* signature,
	DWORD after, DWORD seq, DWORD result)
{
	if (g_entryCount == g_entryCap)
	{
		int cap = g_entryCap ? g_entryCap * 2 : 128;
		void* grown = realloc(g_entries, sizeof(CacheEntry) * cap);
		if (!grown)
			return false;
		g_entries = (CacheEntry*)grown;
		g_entryCap = cap;
	}

	CacheEntry* e = &g_entries[g_entryCount++];
	e->module = module;
	e->token = token;
	e->signature = signature;
	e->after = after;
	e->seq = seq;
	e->result = result;
	e->dead = false;
	return true;
}

// Splits one line in place, false for anything that is not six tab separated fields
static bool ParseLine(char* line)
{
	char* fields[6] = {};
	int count = 0;
	fields[count++] = line;
	for (char* p = line; *p && count < 6; ++p)
	{
		if (*p == '\t')
		{
			*p = 0;
			fields[count++] = p + 1;
		}
	}
	if (count != 6 || !fields[0][0] || !fields[1][0] || !fields[5][0])
		return false;

	char* end = nullptr;
	DWORD after = strtoulCUSTOM(fields[2], &end, 16);
	if (end == fields[2])
		return false;

	DWORD seq = strtoulCUSTOM(fields[3], &end, 16);
	if (end == fields[3])
		return false;

	DWORD result = c_missRva;
	if (strcmp(fields[4], "-") != 0)
	{
		result = strtoulCUSTOM(fields[4], &end, 16);
		if (end == fields[4])
			return false;
	}

	return AddEntry(fields[0], fields[1], fields[5], after, seq, result);
}

//---Cache location-----------------------------------------

// Cuts a file path back to its folder in place
static bool CutToFolder(WCHAR* path)
{
	WCHAR* slash = nullptr;
	for (WCHAR* p = path; *p; ++p)
	{
		if (*p == L'\\')
			slash = p;
	}
	if (!slash)
		return false;
	*slash = 0;
	return true;
}

static bool IsFolder(const WCHAR* path)
{
	DWORD attr = GetFileAttributesW(path);
	return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

// The Explorer7 folder setting, the same value GetExplorer7BaseDir reads
// RegGetValue expands the stored %SystemRoot% form on its own
static bool FolderFromWrpPath(WCHAR* out, DWORD cch)
{
	DWORD cb = cch * sizeof(WCHAR);
	if (RegGetValueW(HKEY_LOCAL_MACHINE,
		L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
		L"WrpPath", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, out, &cb) != ERROR_SUCCESS
		|| !out[0])
		return false;

	int len = lstrlenW(out);
	if (len && out[len - 1] == L'\\')
		out[len - 1] = 0;
	return IsFolder(out);
}

// The loader names its redirected wrp64 import after C:\Windows, the mapped file is the real one
static bool FolderFromMappedImage(WCHAR* out, DWORD cch)
{
	WCHAR device[MAX_PATH];
	DWORD n = K32GetMappedFileNameW(GetCurrentProcess(), (LPVOID)g_hInstance, device, ARRAYSIZE(device));
	if (!n || n >= ARRAYSIZE(device))
		return false;

	// The mapped name is a device path, so swap the device for its drive letter
	WCHAR drive[3] = L"A:";
	for (WCHAR letter = L'A'; letter <= L'Z'; ++letter)
	{
		drive[0] = letter;
		WCHAR target[MAX_PATH];
		if (!QueryDosDeviceW(drive, target, ARRAYSIZE(target)))
			continue;

		int len = lstrlenW(target);
		if (len <= 0 || (DWORD)len >= n || device[len] != L'\\')
			continue;
		if (CompareStringOrdinal(device, len, target, len, TRUE) != CSTR_EQUAL)
			continue;

		if (FAILED(StringCchCopyW(out, cch, drive)) || FAILED(StringCchCatW(out, cch, device + len)))
			return false;
		return CutToFolder(out);
	}
	return false;
}

static bool FolderFromModuleName(WCHAR* out, DWORD cch)
{
	DWORD n = GetModuleFileNameW(g_hInstance, out, cch);
	return n && n < cch && CutToFolder(out);
}

// WrpPath when it is set and exists, else wherever wrp64 really loaded from
static bool ResolveCachePath()
{
	WCHAR folder[MAX_PATH] = {};
	PCWSTR source = L"WrpPath";
	if (!FolderFromWrpPath(folder, ARRAYSIZE(folder)))
	{
		source = L"the loaded wrp64 file";
		if (!FolderFromMappedImage(folder, ARRAYSIZE(folder)))
		{
			source = L"the wrp64 module name";
			if (!FolderFromModuleName(folder, ARRAYSIZE(folder)))
				return false;
		}
	}

	if (FAILED(StringCchCopyW(g_cachePath, ARRAYSIZE(g_cachePath), folder))
		|| FAILED(StringCchCatW(g_cachePath, ARRAYSIZE(g_cachePath), L"\\"))
		|| FAILED(StringCchCatW(g_cachePath, ARRAYSIZE(g_cachePath), c_szCacheFile)))
	{
		g_cachePath[0] = 0;
		return false;
	}

	dbgprintf(L"pattern cache: %s, placed by %s", g_cachePath, source);
	return true;
}

static void LoadCacheFile()
{
	g_cacheLoaded = true;

	if (!ResolveCachePath())
		return;

	HANDLE h = CreateFileW(g_cachePath, GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return;

	DWORD size = GetFileSize(h, nullptr);
	if (size == INVALID_FILE_SIZE || size > c_maxCacheBytes)
	{
		CloseHandle(h);
		return;
	}

	g_fileText = (char*)malloc(size + 1);
	DWORD read = 0;
	if (g_fileText && !ReadFile(h, g_fileText, size, &read, nullptr))
		read = 0;
	CloseHandle(h);
	if (!g_fileText)
		return;
	g_fileText[read] = 0;

	// A file from another format version is ignored whole and written fresh
	char* line = g_fileText;
	bool first = true;
	int bad = 0;
	while (*line)
	{
		char* next = line;
		while (*next && *next != '\n')
			++next;
		if (*next)
			*next++ = 0;
		size_t len = strlen(line);
		if (len && line[len - 1] == '\r')
			line[len - 1] = 0;

		if (first)
		{
			first = false;
			if (strcmp(line, c_szCacheHeader) != 0)
			{
				g_cacheDirty = true;
				dbgprintf(L"pattern cache: %s has another format, starting over", g_cachePath);
				return;
			}
		}
		else if (line[0] && !ParseLine(line))
		{
			++bad;
		}
		line = next;
	}

	if (bad)
		g_cacheDirty = true;
	dbgprintf(L"pattern cache: %d lines read from %s, %d unreadable", g_entryCount, g_cachePath, bad);
}

// Live lines only, written to a side file and swapped in so a reader never sees half a file
static void FlushCacheFile()
{
	g_cacheDirty = false;
	if (!g_cachePath[0] || g_cacheWriteFailed)
		return;

	size_t cap = sizeof(c_szCacheHeader) + 4;
	int live = 0;
	for (int i = 0; i < g_entryCount; ++i)
	{
		const CacheEntry* e = &g_entries[i];
		if (e->dead)
			continue;
		cap += strlen(e->module) + strlen(e->token) + strlen(e->signature) + 32;
		++live;
	}

	char* text = (char*)malloc(cap);
	if (!text)
		return;

	StringCchCopyA(text, cap, c_szCacheHeader);
	StringCchCatA(text, cap, "\r\n");
	size_t len = strlen(text);
	for (int i = 0; i < g_entryCount; ++i)
	{
		const CacheEntry* e = &g_entries[i];
		if (e->dead)
			continue;

		// wsprintf stops at 1024 characters, a line that long is left out rather than cut
		if (strlen(e->module) + strlen(e->token) + strlen(e->signature) + 32 >= 1024)
			continue;

		char result[16];
		if (e->result == c_missRva)
			StringCchCopyA(result, ARRAYSIZE(result), "-");
		else
			wsprintfA(result, "%X", e->result);

		char line[1024];
		int n = wsprintfA(line, "%s\t%s\t%X\t%X\t%s\t%s\r\n",
			e->module, e->token, e->after, e->seq, result, e->signature);
		if (n > 0 && len + (size_t)n < cap)
		{
			memcpy(text + len, line, n);
			len += n;
			text[len] = 0;
		}
	}

	WCHAR temp[MAX_PATH + 16];
	wsprintfW(temp, L"%s.%lu", g_cachePath, GetCurrentProcessId());

	bool written = false;
	HANDLE h = CreateFileW(temp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE)
	{
		DWORD out = 0;
		BOOL ok = WriteFile(h, text, (DWORD)len, &out, nullptr) && out == len;
		CloseHandle(h);
		written = ok && MoveFileExW(temp, g_cachePath, MOVEFILE_REPLACE_EXISTING);
		if (!written)
			DeleteFileW(temp);
	}

	// A file another user created cannot be replaced, but it can still be rewritten
	if (!written)
	{
		h = CreateFileW(g_cachePath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h != INVALID_HANDLE_VALUE)
		{
			DWORD out = 0;
			written = WriteFile(h, text, (DWORD)len, &out, nullptr) && out == len;
			CloseHandle(h);
		}
	}

	free(text);

	if (written)
	{
		dbgprintf(L"pattern cache: %d lines written to %s", live, g_cachePath);
	}
	else
	{
		g_cacheWriteFailed = true;
		dbgprintf(L"pattern cache: cannot write %s, error %u", g_cachePath, GetLastError());
	}
}

//---Module lookup------------------------------------------

static CacheModule* GetCacheModule(uintptr_t base)
{
	for (int i = 0; i < g_moduleCount; ++i)
	{
		if (g_modules[i].base == base)
			return &g_modules[i];
	}
	if (g_moduleCount == ARRAYSIZE(g_modules))
		return nullptr;

	char path[MAX_PATH];
	DWORD n = GetModuleFileNameA((HMODULE)base, path, ARRAYSIZE(path));
	if (!n || n >= ARRAYSIZE(path))
		return nullptr;

	const char* leaf = path;
	for (const char* p = path; *p; ++p)
	{
		if (*p == '\\')
			leaf = p + 1;
	}
	if (strlen(leaf) >= sizeof(g_modules[0].name))
		return nullptr;

	CacheModule* m = &g_modules[g_moduleCount];
	size_t i = 0;
	for (; leaf[i]; ++i)
		m->name[i] = (leaf[i] >= 'A' && leaf[i] <= 'Z') ? (char)(leaf[i] + 32) : leaf[i];
	m->name[i] = 0;
	BuildModuleToken(base, m->token, sizeof(m->token));
	m->base = base;
	++g_moduleCount;

	// Lines this module left under any other build are void now
	int stale = 0;
	for (int e = 0; e < g_entryCount; ++e)
	{
		CacheEntry* entry = &g_entries[e];
		if (!entry->dead && strcmp(entry->module, m->name) == 0 && strcmp(entry->token, m->token) != 0)
		{
			entry->dead = true;
			++stale;
		}
	}
	if (stale)
	{
		g_cacheDirty = true;
		dbgprintf(L"pattern cache: %S changed build, %d old lines dropped", m->name, stale);
	}
	return m;
}

static CacheEntry* FindEntry(const CacheModule* m, const char* signature, DWORD after, DWORD seq)
{
	for (int i = 0; i < g_entryCount; ++i)
	{
		CacheEntry* e = &g_entries[i];
		if (!e->dead && e->after == after && e->seq == seq && strcmp(e->signature, signature) == 0
			&& strcmp(e->module, m->name) == 0 && strcmp(e->token, m->token) == 0)
			return e;
	}
	return nullptr;
}

static char* CopyString(const char* s)
{
	size_t len = strlen(s) + 1;
	char* copy = (char*)malloc(len);
	if (copy)
		memcpy(copy, s, len);
	return copy;
}

// Zero for the first time a search is made in this process, then one, and so on
static bool NextSeq(const CacheModule* m, const char* signature, DWORD after, DWORD* seq)
{
	for (int i = 0; i < g_runCountUsed; ++i)
	{
		RunCount* r = &g_runCounts[i];
		if (r->module == m && r->after == after && strcmp(r->signature, signature) == 0)
		{
			*seq = r->count++;
			return true;
		}
	}

	if (g_runCountUsed == g_runCountCap)
	{
		int cap = g_runCountCap ? g_runCountCap * 2 : 128;
		void* grown = realloc(g_runCounts, sizeof(RunCount) * cap);
		if (!grown)
			return false;
		g_runCounts = (RunCount*)grown;
		g_runCountCap = cap;
	}

	char* copy = CopyString(signature);
	if (!copy)
		return false;

	RunCount* r = &g_runCounts[g_runCountUsed++];
	r->module = m;
	r->signature = copy;
	r->after = after;
	r->count = 1;
	*seq = 0;
	return true;
}

//---Scanning-----------------------------------------------

static bool PatternMatchesAt(const unsigned char* at, const wiktorArray<int>* pattern)
{
	for (int j = 0; j < pattern->size; ++j)
	{
		if (pattern->data[j] != -1 && at[j] != pattern->data[j])
			return false;
	}
	return true;
}

// The whole image from the first byte past an earlier hit, first match wins
static uintptr_t ScanImage(uintptr_t base, DWORD sizeOfImage, const wiktorArray<int>* pattern, size_t first)
{
	const auto scanBytes = reinterpret_cast<unsigned char*>(base);
	const size_t s = pattern->size;
	const int* d = pattern->data;

	for (size_t i = first; i < sizeOfImage - s; ++i)
	{
		bool found = true;
		for (size_t j = 0; j < s; ++j)
		{
			if (scanBytes[i + j] != d[j] && d[j] != -1)
			{
				found = false;
				break;
			}
		}
		if (found)
			return reinterpret_cast<uintptr_t>(&scanBytes[i]);
	}
	return 0;
}

uintptr_t FindPatternCached(uintptr_t baseAddress, const char* signature, uintptr_t startAfter)
{
	if (!baseAddress)
		return 0;

	IMAGE_NT_HEADERS* nt = ImageHeaders(baseAddress);
	if (!nt)
		return 0;

	const DWORD sizeOfImage = nt->OptionalHeader.SizeOfImage;
	auto pattern = patternToByte(signature);

	size_t first = 0;
	if (startAfter >= baseAddress && startAfter < baseAddress + sizeOfImage)
		first = (size_t)(startAfter - baseAddress) + 1;

	AcquireSRWLockExclusive(&g_cacheLock);

	if (!g_cacheLoaded)
		LoadCacheFile();

	CacheModule* m = g_cachePath[0] ? GetCacheModule(baseAddress) : nullptr;

	// Without a sequence number the lookup cannot be told apart, so it is not cached
	DWORD seq = 0;
	if (m && !NextSeq(m, signature, (DWORD)first, &seq))
		m = nullptr;

	CacheEntry* e = m ? FindEntry(m, signature, (DWORD)first, seq) : nullptr;

	uintptr_t result = 0;
	bool known = false;
	if (e)
	{
		if (e->result == c_missRva)
		{
			known = true;
		}
		else if (e->result >= first && (size_t)e->result + pattern->size <= sizeOfImage
			&& PatternMatchesAt((const unsigned char*)baseAddress + e->result, pattern))
		{
			result = baseAddress + e->result;
			known = true;
		}
		else
		{
			// The bytes moved under a matching build, so this line is wrong and goes
			e->dead = true;
			g_cacheDirty = true;
		}
	}

	if (!known)
	{
		result = ScanImage(baseAddress, sizeOfImage, pattern, first);

		// A miss outside attach may be another hook over the bytes, so only hits are kept then
		if (m && (result || g_patternCacheAttach))
		{
			char* copy = CopyString(signature);
			if (copy && AddEntry(m->name, m->token, copy, (DWORD)first, seq,
				result ? (DWORD)(result - baseAddress) : c_missRva))
				g_cacheDirty = true;
		}
	}

	if (g_cacheDirty && !g_patternCacheAttach)
		FlushCacheFile();

	ReleaseSRWLockExclusive(&g_cacheLock);

	delete pattern;

	// Reported here rather than at the call sites, most of which do not check the result
	// A fallback chain expects the wrong build's variant to miss, so one miss is not a fault
	if (!result && !startAfter)
		dbgprintf(L"explorer7: pattern not found in %p: %S", (void*)baseAddress, signature);

	return result;
}

void PatternCacheBeginAttach()
{
	InterlockedExchange(&g_patternCacheAttach, 1);
}

// Everything attach found goes to disk in one write
void PatternCacheEndAttach()
{
	AcquireSRWLockExclusive(&g_cacheLock);
	InterlockedExchange(&g_patternCacheAttach, 0);
	if (g_cacheDirty)
		FlushCacheFile();
	ReleaseSRWLockExclusive(&g_cacheLock);
}
