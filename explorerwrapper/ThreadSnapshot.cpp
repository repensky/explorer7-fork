#include "dbgprint.h"
#include <tlhelp32.h>

//---MinHook thread snapshot--------------------------------

// MinHook freezes threads with a toolhelp snapshot of every thread on the machine
// Only MinHook imports toolhelp here, so its snapshot is answered with this process alone

typedef LONG (NTAPI *NtGetNextThread_t)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);
typedef HANDLE (WINAPI *CreateToolhelp32Snapshot_t)(DWORD, DWORD);
typedef BOOL (WINAPI *Thread32Walk_t)(HANDLE, LPTHREADENTRY32);

static NtGetNextThread_t g_ntGetNextThread;
static CreateToolhelp32Snapshot_t g_realCreateSnapshot;
static Thread32Walk_t g_realThread32First;
static Thread32Walk_t g_realThread32Next;

// One list per open snapshot, the handle is a real event so CloseHandle still works on it
struct OwnThreadList
{
	HANDLE handle;
	DWORD* ids;
	int count;
	int next;
};

static OwnThreadList g_lists[8];
static SRWLOCK g_listLock = SRWLOCK_INIT;

// Every thread of this process, false when the walk itself cannot be started
static bool CollectOwnThreads(DWORD** idsOut, int* countOut)
{
	DWORD* ids = nullptr;
	int count = 0;
	int cap = 0;
	HANDLE thread = nullptr;
	bool started = false;

	for (;;)
	{
		HANDLE next = nullptr;
		if (g_ntGetNextThread(GetCurrentProcess(), thread, THREAD_QUERY_LIMITED_INFORMATION, 0, 0, &next) < 0)
			break;
		started = true;
		if (thread)
			CloseHandle(thread);
		thread = next;

		if (count == cap)
		{
			int grown = cap ? cap * 2 : 32;
			void* bigger = realloc(ids, sizeof(DWORD) * grown);
			if (!bigger)
			{
				free(ids);
				CloseHandle(thread);
				return false;
			}
			ids = (DWORD*)bigger;
			cap = grown;
		}
		ids[count++] = GetThreadId(thread);
	}
	if (thread)
		CloseHandle(thread);

	if (!started)
	{
		free(ids);
		return false;
	}

	*idsOut = ids;
	*countOut = count;
	return true;
}

static OwnThreadList* FindList(HANDLE handle)
{
	for (int i = 0; i < ARRAYSIZE(g_lists); ++i)
	{
		if (g_lists[i].handle && g_lists[i].handle == handle)
			return &g_lists[i];
	}
	return nullptr;
}

// The slot goes back once the walk has run dry, MinHook closes the handle itself
static BOOL NextOwnThread(OwnThreadList* list, LPTHREADENTRY32 entry)
{
	if (list->next >= list->count)
	{
		free(list->ids);
		ZeroMemory(list, sizeof(*list));
		SetLastError(ERROR_NO_MORE_FILES);
		return FALSE;
	}

	DWORD size = entry->dwSize;
	if (size < FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(DWORD))
	{
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}

	ZeroMemory(entry, size < sizeof(THREADENTRY32) ? size : sizeof(THREADENTRY32));
	entry->dwSize = size;
	entry->th32ThreadID = list->ids[list->next++];
	entry->th32OwnerProcessID = GetCurrentProcessId();
	return TRUE;
}

static HANDLE WINAPI CreateToolhelp32Snapshot_Own(DWORD flags, DWORD processId)
{
	// Anything but the plain all threads snapshot MinHook asks for goes to Windows
	if (flags != TH32CS_SNAPTHREAD || processId != 0)
		return g_realCreateSnapshot(flags, processId);

	DWORD* ids = nullptr;
	int count = 0;
	if (!CollectOwnThreads(&ids, &count))
		return g_realCreateSnapshot(flags, processId);

	HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!handle)
	{
		free(ids);
		return g_realCreateSnapshot(flags, processId);
	}

	AcquireSRWLockExclusive(&g_listLock);
	OwnThreadList* slot = nullptr;
	for (int i = 0; i < ARRAYSIZE(g_lists) && !slot; ++i)
	{
		if (!g_lists[i].handle)
			slot = &g_lists[i];
	}
	if (slot)
	{
		slot->handle = handle;
		slot->ids = ids;
		slot->count = count;
		slot->next = 0;
	}
	ReleaseSRWLockExclusive(&g_listLock);

	if (!slot)
	{
		CloseHandle(handle);
		free(ids);
		return g_realCreateSnapshot(flags, processId);
	}
	return handle;
}

static BOOL WINAPI Thread32First_Own(HANDLE snapshot, LPTHREADENTRY32 entry)
{
	AcquireSRWLockExclusive(&g_listLock);
	OwnThreadList* list = FindList(snapshot);
	BOOL result = FALSE;
	if (list)
	{
		list->next = 0;
		result = NextOwnThread(list, entry);
	}
	ReleaseSRWLockExclusive(&g_listLock);

	return list ? result : g_realThread32First(snapshot, entry);
}

static BOOL WINAPI Thread32Next_Own(HANDLE snapshot, LPTHREADENTRY32 entry)
{
	AcquireSRWLockExclusive(&g_listLock);
	OwnThreadList* list = FindList(snapshot);
	BOOL result = FALSE;
	if (list)
		result = NextOwnThread(list, entry);
	ReleaseSRWLockExclusive(&g_listLock);

	return list ? result : g_realThread32Next(snapshot, entry);
}

// Must run before the first MH_EnableHook, the walkers go in first so a half install stays harmless
void InstallOwnThreadSnapshot()
{
	HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	if (!kernel32 || !ntdll)
		return;

	g_ntGetNextThread = (NtGetNextThread_t)GetProcAddress(ntdll, "NtGetNextThread");
	g_realCreateSnapshot = (CreateToolhelp32Snapshot_t)GetProcAddress(kernel32, "CreateToolhelp32Snapshot");
	g_realThread32First = (Thread32Walk_t)GetProcAddress(kernel32, "Thread32First");
	g_realThread32Next = (Thread32Walk_t)GetProcAddress(kernel32, "Thread32Next");
	if (!g_ntGetNextThread || !g_realCreateSnapshot || !g_realThread32First || !g_realThread32Next)
	{
		dbgprintf(L"thread snapshot: exports missing, MinHook keeps the full snapshot");
		return;
	}

	BOOL first = ChangeImportedAddress(g_hInstance, "KERNEL32.dll", g_realThread32First, Thread32First_Own);
	BOOL next = ChangeImportedAddress(g_hInstance, "KERNEL32.dll", g_realThread32Next, Thread32Next_Own);
	BOOL create = first && next
		&& ChangeImportedAddress(g_hInstance, "KERNEL32.dll", g_realCreateSnapshot, CreateToolhelp32Snapshot_Own);

	dbgprintf(L"thread snapshot: MinHook lists this process only %d, walkers %d %d", create, first, next);
}

#ifdef EXPLORER7_SNAPSHOT_TEST
// Lets a test program call the replacements without patching anything
HANDLE TestOwnCreateSnapshot(DWORD flags, DWORD processId) { return CreateToolhelp32Snapshot_Own(flags, processId); }
BOOL TestOwnThread32First(HANDLE h, LPTHREADENTRY32 e) { return Thread32First_Own(h, e); }
BOOL TestOwnThread32Next(HANDLE h, LPTHREADENTRY32 e) { return Thread32Next_Own(h, e); }
void TestOwnResolve()
{
	g_ntGetNextThread = (NtGetNextThread_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtGetNextThread");
	g_realCreateSnapshot = CreateToolhelp32Snapshot;
	g_realThread32First = Thread32First;
	g_realThread32Next = Thread32Next;
}
#endif
