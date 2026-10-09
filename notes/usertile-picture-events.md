# Account picture change events

Why the tray user tile listens to `Windows.System.UserWatcher` and to three
folders, rather than to the one registry key it used to watch.

## The shell has no broadcast

Checked in the Win10 x64 `shell32.dll`:

- `_NotifyUserPictureChange` (`0x1802D2B5C`) is the only user picture change
  notification in the binary, and it has exactly one caller,
  `SHSetUserPicturePath` (`0x1802D3340`)
- it runs `CNotifyUserPictureChangeTask::_Notify` (`0x1802D2974`), which
  enumerates REG_SZ values under
  `HKLM\Software\Microsoft\Windows\CurrentVersion\UserPictureChange`, parses
  each as a CLSID, `CoCreateInstance`s it with CLSCTX 5 and IID
  `{a561e69a-b4b8-4113-91a5-64c6bcca3430}`, then calls vtable slot 3 with the
  user name
- there is no `SHChangeNotify`, no `BroadcastSystemMessage`, no
  `WM_SETTINGCHANGE` and no WNF publish anywhere on the picture path
- `shell32.dll` contains no `AccountPicture` string at all, so it is not what
  writes the modern store, and the Settings app change never reaches
  `SHSetUserPicturePath`

So that extensibility point runs in the setter's process, on a path nothing
modern uses. It is not usable from explorer.

## The registry key does not move

Measured on a Win10 19044 box:

| what | last write |
| --- | --- |
| `HKLM\...\CurrentVersion\AccountPicture\Users\<sid>` | 2026-03-14 01:48:35 |
| `%ProgramData%\AccountPictures\<sid>\ImageNN.jpg` | 2026-08-08 07:16:43 |

Changing the picture rewrites the JPGs and leaves the key alone, so
`RegNotifyChangeKeyValue` on that key can never report a picture change. It is
still waited on, at the `Users` parent with the subtree flag, because it does
fire when the per SID subkey is first created.

`%ProgramData%\Microsoft\User Account Pictures\<user>.dat` is 0 bytes, so the
legacy store is dead and `SHGetUserPictureBytes` is answering from the account
property store instead.

## Windows.System.UserWatcher, tried and removed

On paper this is the right event. `UserWatcher.Updated` carries a
`ChangedPropertyKinds` vector and `UserWatcherUpdateKind_Picture` is 1. It was
built, it started cleanly, and it never once fired. The code is gone, and the
detail is kept here so nobody spends the same two days on it again.

Interfaces and slot indices are from `windows.system.h` in the 10.0.19041.0
SDK:

| interface | IID | slots used |
| --- | --- | --- |
| `IUserStatics` | `155eb23b-242a-45e0-a2e9-3171fc6a7fdd` | 6 `CreateWatcher` |
| `IUserWatcher` | `155eb23b-242a-45e0-a2e9-3171fc6a7fbb` | 7 `Start`, 8 `Stop`, 13 `add_Updated`, 14 `remove_Updated` |
| `IUserChangedEventArgs2` | `6b2ccb44-6f01-560c-97ad-fc7f32ec581f` | 6 `get_ChangedPropertyKinds` |
| `IVectorView<T>` | parameterized | 6 `GetAt`, 7 `get_Size` |

`IInspectable` puts the first declared method of a WinRT interface at slot 6.
`ITypedEventHandler_impl` derives from `IUnknown`, not `IInspectable`
(`windows.foundation.collections.h:1126`), so `Invoke` is slot 3.

It is bound by hand because this project builds with no exceptions, no RTTI
and `IgnoreAllDefaultLibraries`, which rules out both C++/WinRT and WRL.

### Start needs an Added handler

`Start` returns `E_ILLEGAL_METHOD_CALL` (`0x8000000E`) when only `Updated` is
registered. Observed live, with `get_Status` reading `Created` (0) both before
and after, which is exactly the state `Start` is supposed to accept.

Read out of `usermgr.dll` (`Windows::System::UserWatcher::Start`,
`0x180095F00`), the first thing it does after taking the lock is:

```c
if ( HeapEventStore::GetSize(&Context[14]) == 0 )
{
    v2 = -2147483634;                     // 0x8000000E
    ... userwatcher.cpp line 57 ...
}
```

`&Context[14]` is `this + 112`. The event sources sit at a stride of 0x40:

| offset | source | proof |
| --- | --- | --- |
| `this + 72` | Added | `add_Added` and `RaiseAddedInternal` both use it |
| `this + 136` | Removed | `add_Removed` |
| `this + 200` | Updated | `add_Updated` |

`112` falls inside the Added source, 40 bytes into it, so `Start` is asking
whether anything is listening for `Added`. A watcher with no `Added` handler
has nothing to enumerate to, and refuses to start.

So the tile registers a second handler on `add_Added` (slot 9) purely to
satisfy that check. It shares the vtable and the PIID with the real one, and a
`notify` field makes its `Invoke` return immediately.

### The handler IID

A parameterized interface has no IID in the headers. The one for
`ITypedEventHandler<UserWatcher, UserChangedEventArgs>` is the WinRT PIID of

```
pinterface({9de1c534-6ae1-11e0-84e1-18a905bcc53f};
           rc(Windows.System.UserWatcher;{155eb23b-242a-45e0-a2e9-3171fc6a7fbb});
           rc(Windows.System.UserChangedEventArgs;{086459dc-18c6-48db-bc99-724fb9203ccc}))
```

which is `eb9d0454-25db-5620-98b8-be4c5d0dbc67`. That is not a derivation
taken on trust. The same 16 bytes appear verbatim in `audiosrv.dll`,
`cdp.dll`, `diagtrack.dll`, `CBDHSvc.dll`, `AudioEndpointBuilder.dll`,
`modernexecserver.dll`, `NotificationControllerPS.dll` and `tellib.dll`, all of
which subscribe to this event.

### Why it was removed

Once the Added sink was in place it started every time and reported
`UserWatcher started, status was 0`. It then never delivered a single
callback. Every picture change, including ones made through the Settings app,
reached the tile through the folder watches instead.

The decisive test was logging the handler itself rather than the event. `Start`
runs an enumeration that raises `Added`, so a sink that logs on entry should
print once at startup before anything is touched. It never printed. The
callback path was dead end to end, which means a successful `add_Updated` and a
successful `Start` prove nothing on their own.

Two lessons worth keeping. A WinRT call returning `S_OK` says the registration
was accepted, not that anything will ever call you. And a silent no-op handler
cannot be told apart from a handler that is never reached, so give every sink a
log line on entry before drawing any conclusion from its silence.

Whether the fault was in the hand-rolled binding or in what the User Manager
raises was never established. The picture chain on the OS side is
`UserManagerStatics::PictureCallback` to `UserStatics::FireUserPictureUpdated`
to the watcher's `RaiseUpdated`, driven by the User Manager service rather than
by any file watch, so a picture written without going through that service
would not raise it either way.

## The folder watches

Which store Windows writes depends on how the picture was set, so all three
are watched, each armed independently:

| folder | subtree | holds |
| --- | --- | --- |
| `%ProgramData%\AccountPictures` | yes | the scaled `ImageNN.jpg` per SID |
| `%ProgramData%\Microsoft\User Account Pictures` | no | the legacy `<user>.dat` |
| `%AppData%\Microsoft\Windows\AccountPictures` | no | the picked `.accountpicture-ms` |

The first is watched at the parent on purpose. The per SID folder does not
exist until a picture is first set, and watching the leaf meant that the
folder appearing was missed for the rest of the session.

## What the old watcher got wrong

1. It opened the per SID registry key first and returned when that failed, and
   the folder watch was only created afterwards, so a machine where no picture
   had ever been set lost the whole watcher at startup
2. Its only folder was the per SID leaf, with no retry when it did not exist
3. Its filter was `LAST_WRITE | SIZE`, which never reports create, delete or
   rename, and is only raised once the change reaches disk
4. Its `AccountPictureStale` backstop stamped the same registry key that never
   moves, so it could not recover either
