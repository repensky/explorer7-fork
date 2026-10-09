# Tray notify callbacks on the 7850 explorer

The wrapper puts its own sink between the tray and any client that registers
for notification icon callbacks, and it owns the cookie the tray hands back.
This note covers why the sink exists and the crash that came from losing the
cookie, diagnosed on 2026-09-07 from `Win11Restore\dumps\explorer.exe(1).9780.dmp`.

## Which explorer serves which interface

`CTrayNotifyFactory::CreateInstance` picks the wrapper by asking the real
object for `ITrayNotify8`, the six method interface.

| explorer | interface | wrapper | unregister path |
| --- | --- | --- | --- |
| 7601 | `ITrayNotify7`, three methods | `CTrayNotifyWrapper` | `RegisterCallback(NULL)` |
| 7850 | `ITrayNotify8`, six methods | `CTrayNotify8Wrapper` | `UnregisterCallback(cookie)` |

7850 is a pre release Windows 8 build, which is why it already serves the
newer interface. Read from the 7601 database, `CTrayNotify::RegisterCallback`
there takes a single `INotificationCB *`, there is no `UnregisterCallback`
symbol at all, and the body stores the sink and guards the enumeration with
`if (a2 != nullptr)`, so passing null is the supported way to unregister and
cannot fault. Everything below is therefore 7850 only.

## Why the sink exists

7850 sends `NOTIFYITEM` callbacks that the Windows 10 and 11 `actxprxy` cannot
marshal, so a client in another process faults inside `rpcrt4`. The wrapper
registers a `CTrayNotificationCallback` of its own, lets it forward during the
enumeration the tray performs inside the register call, then calls
`StopForwarding` so the live updates are dropped instead of marshalled.

## The cookie, and the crash

The two mangled names settle the signatures, read from
`UltimateThemeSwitcher\exports\explorer7850.exe.i64`:

| symbol | signature |
| --- | --- |
| `?RegisterCallback@CTrayNotify@@UEAAJPEAUINotificationCB@@PEAK@Z` | `(INotificationCB *, unsigned long *)` |
| `?UnregisterCallback@CTrayNotify@@UEAAJK@Z` | `(unsigned long)` |

The trailing `K` is `unsigned long` **by value**, not `PEAK`, so the cookie
goes back in by value even though the wrapper's header declares the parameter
as `ULONG *`.

`RegisterCallback` fails fast with `__int2c` if either argument is null, then
allocates a sixteen byte node holding `{IUnknown *punk; DWORD cookie}`. The
cookie comes from `_InterlockedIncrement(this + 0x448)`, which returns the new
value, so **cookies start at one and zero is never issued**. The node goes in
the DPA at `this+0x440` and the cookie is written to `*pCookie`.

`UnregisterCallback` is five lines and never checks the lookup:

```c
v3 = *((struct _DPA **)this + 136);   // this+0x440
v7[2] = a2;                            // the cookie, used by value as the key
v4 = DPA_Search(v3, v7, 0, _CompareNotifyCBNode, 0, 0);   // -1 when not found
v5 = DPA_DeletePtr(*((HDPA *)this + 136), v4);            // null when i is -1
(*(...)(*(_QWORD *)*v5 + 16))(*v5);    // faults here, no null check
```

The wrapper used to call `m_notify8->UnregisterCallback(NULL)` from its
destructor and from the re-register path, which asks the tray to drop cookie
zero. That never matches, so `DPA_Search` returns -1, `DPA_DeletePtr` returns
null, and the next instruction faults. In the dump `rdx` was `0xffffffff`, the
DPA held two live nodes with cookies 2 and 3, and both of their objects were
`wrp64!CTrayNotificationCallback`.

The trigger was a COM rundown rather than anything the user did. A client that
had marshalled the tray object went away, so combase ran the object down
through `CRemoteUnknown::RundownOid` into `CStdIdentity::ReleaseCtrlUnk`,
which dropped the wrapper's last reference on an RPC worker thread. Because
that client never called `UnregisterCallback` itself, the sink was still held
and the destructor took the faulting branch. Nothing about this is tied to the
host build, the same sequence crashes on 19041.

## The fix

`CTrayNotify8Wrapper` now keeps `m_cookie`, the cookie the tray issued for the
one registration a wrapper instance can hold, and `DropRegistration` hands
exactly that cookie back, skipping the call when it is zero. Three further
fast fail paths were closed at the same time:

- the register call passes a local `ULONG` and copies it out, so a client that
  passes a null cookie pointer no longer reaches the fast fail
- a null sink now returns `S_OK` after dropping the registration instead of
  being forwarded to a function that fast fails on it
- `UnregisterCallback` ignores the caller's cookie and uses `m_cookie`, so a
  client that passes a pointer where 7850 wants a value cannot make it search
  for a bogus cookie

The value is passed as `(ULONG*)(ULONG_PTR)m_cookie` to match the by value ABI
through the declared pointer parameter.

Read from the two IDA databases and the crash dump, not inferred. The dump and
the 7850 database agree, `UnregisterCallback` sits at RVA `0x5EEA4` in both and
the reported fault offset `0x5EEF6` is that plus `0x52`. Built but not yet
retested on the VM.
