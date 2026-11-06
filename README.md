# appx-wine

Runs Windows 8/8.1 and Windows 10 (UWP) Store apps (`.appx`) under Wine by providing the Windows Runtime pieces Wine lacks:
`Windows.UI.Xaml` startup, `CoreWindow`, the dispatcher, and mocks for everything else.

It targets C++/CX DirectX games, which typically use XAML only to host a swapchain
(`SwapChainBackgroundPanel` or `SwapChainPanel`) and draw everything with Direct3D 11. Apps with real XAML user
interfaces (buttons, lists, text) and C#/.NET apps will not work: no XAML controls are rendered. An invisible
`TextBox` that a game focuses to read typed text does receive keyboard input.

Tested (x86):
- Asphalt 8: Airborne 1.0.0.10 (Windows 8). Menus, races, saving and keyboard/mouse input all work.
- Asphalt 8: Airborne 4.5.0n (Windows 10 UWP, VCLibs 120). Menus, races and saving across restarts work. Run it
  without network access (for example inside `bwrap --unshare-net`), otherwise it asks for an update.

The changes made for 4.5.0n have not been re-tested with 1.0.0.10.

## Usage
```sh
nix-shell shell.nix   # pinned toolchain: wine-11.0 (wow64), mingw32 gcc, 7-Zip, python + dnfile
./appx-wine install ~/Games/foo Foo_1.0.0.0_x86__abc.appx Microsoft.VCLibs.110.00_11.0.51106.1_x86.appx
./appx-wine run ~/Games/foo
```
Without Nix, install these yourself and run `./appx-wine` directly:
- Wine with 32-bit support (a WoW64 build or a multilib install); tested with 11.0. Older versions may lack
  WinRT pieces the shim relies on, such as `Windows.Foundation.PropertyValue`.
- The i686 MinGW-w64 cross compiler `i686-w64-mingw32-gcc` (Debian/Ubuntu: `gcc-mingw-w64-i686`,
  Fedora: `mingw32-gcc`, Arch: `mingw-w64-gcc`), plus `make`.
- 7-Zip as `7zz` (the 7-Zip.org build; p7zip's `7z` can be symlinked as `7zz`). It must handle Zip64, which
  Info-ZIP `unzip` fails on for large packages.
- `curl`, `unzip`, `sha256sum`.
- Python 3 with [`dnfile`](https://pypi.org/project/dnfile/) (`pip install dnfile`).

`install` extracts the package to `DIR/app`, copies the DLLs of the given dependency packages (see the
"declared dependencies" it prints, usually just VCLibs) next to the executable, builds the shim into
`DIR/build`, and sets up the Wine prefix `DIR/prefix`. It can be re-run at any time, for example after changing
the shim. App data (`ApplicationData.LocalFolder` etc.) lives in
`DIR/prefix/drive_c/users/$USER/AppData/Local/Packages/<PackageFamilyName>`.

Only x86 packages are supported: the method thunks are written for the 32-bit stdcall ABI.

### Offline behaviour
The shim behaves like a Windows machine without internet: `NetworkInformation.GetInternetConnectionProfile`
returns null, push notification channels fail with `0x800704CF` (network unreachable), and
`DatagramSocket.GetEndpointPairsAsync` fails with `0x80072AF9` (host not found). This only covers the WinRT
APIs; Wine's own networking (Winsock, WinHTTP) still works. To keep an app off the network entirely, run it in a
network namespace, such as `bwrap --unshare-net`.

### App lifecycle and persistent data
Closing the window raises `Application.Suspending`, waits up to 5 s for `SuspendingOperation` deferrals to complete,
and then terminates the process, as Windows does with a suspended app. Apps that save on suspend need this; some
also hang in their own shutdown code after suspending.

`PasswordVault` credentials are saved to `shim-passwordvault` in the app data folder: one credential per line, with
hex-encoded UTF-16 fields, written through a temporary file on every `Add`/`Remove`. Some games keep their save data
there. `ApplicationData.LocalSettings` is only kept in memory.

## Environment variables for `run`
| Variable | Effect |
|---|---|
| `SHIM_QUIET=1` | don't log each mocked call (the log is `DIR/shim.log` plus stderr) |
| `SHIM_WIDTH`, `SHIM_HEIGHT` | window size, default 1280x720 |
| `SHIM_MOUSE=1` | report the mouse as a mouse pointer; by default it's a touch pointer, which games built for tablets handle better (with a mouse pointer, Asphalt 8 treats hovering in scrolling menus as swiping) |
| `SHIM_DUMP_EVERY=<s>` | write the back buffer to `DIR/frames/frame-NNN.bmp` every `<s>` seconds; F12 dumps one frame |
| `SHIM_TAPS="<s>:<x>,<y>;<s>:key=<vk>;..."` | scripted clicks/key presses (seconds after start, client coordinates), for testing without a person |
| `SHIM_WATCHDOG=<s>` | dump the main thread's stack when it makes no WinRT call for `<s>` seconds |
| `SHIM_WATCHDOG_ALL=1` | with `SHIM_WATCHDOG`, also dump every other thread's stack once, at the first idle report |

## How it works
- `tools/gen.py` reads the Windows SDK metadata (`.winmd` files from NuGet `Microsoft.Windows.SDK.Contracts`
  10.0.19041.1, downloaded and hash-checked on first install) plus the app's own `.winmd` files, and generates
  `winrt_meta.c`: every interface, including closed generic instances with IIDs from the WinRT
  parameterized-interface algorithm, with method order and x86 stack sizes. It also lists classes with their
  interfaces, statics and factories.
- `src/core.c` turns that into a generic COM object for any runtime class. Each method is a small thunk that logs the
  call and cleans up the correct number of stack bytes. By default, `put_X` stores into a property bag, `get_X` returns
  the stored value or an auto-created mock of the return type, `add_X`/`remove_X` keep event handlers, and
  factories create objects. Composable factories support XAML-style aggregation.
- `tools/gen_reg.py` registers the shim for every class Wine doesn't implement itself, plus a few where Wine's
  implementation is incomplete.
- Real implementations override specific methods:
  - `xaml.c`: `Application.Start`, a Win32 window as `CoreWindow`, input events, dispatcher, a minimal XAML
    loader (`x:Name`, `x:ConnectionId`), `Window`, `Loaded` and `SizeChanged` events, `ActualWidth`/`ActualHeight`
    (the window size) and the `CompositionTarget.Rendering` loop. `GeneralTransform`s are the identity, because
    every element covers the window. `Control.Focus` on a `TextBox` routes typed characters to it and fires
    `TextChanged`. `Suspending` on close. `MediaElement`s report `MediaEnded` right away, so intro videos are
    skipped.
  - `dxgi.c`: composition/CoreWindow swapchains become `CreateSwapChainForHwnd`. Frame dumps from `Present` and
    `Present1`.
  - `collections.c`: `IVector`, `IMap`, iterators.
  - `async.c`: async operations complete immediately. `ThreadPoolTimer`, including `TimerDestroyedHandler`:
    the UWP C runtime (`msvcr120_app`) builds its timer queues on it and waits for that handler after `Cancel`.
    Offline failures for network operations.
  - `buffer.c`: `IBuffer`, hashing, `CryptographicBuffer` (including base64/hex decoding), DES (`DES_ECB`,
    `DES_ECB_PKCS7`) through `SymmetricKeyAlgorithmProvider`/`CryptographicEngine`.
  - `package.c`: identity from `AppxManifest.xml`, including the computed publisher ID.
  - `misc.c`: storage folders, display properties (`DisplayProperties`, `DisplayInformation` at 96 dpi and
    100 %, `SwapChainPanel.CompositionScale` 1.0), `PasswordVault`, the offline network profile, and workarounds
    for Wine gaps. PnP queries see a display adapter and a HAL device, and `PnpObject.CreateFromIdAsync` returns the
    root container. The HAL device's `DriverVersion` (`10.0.19041.1`) is what some apps parse as the OS version. The
    values are from a Windows 10 machine.

### Wine gaps worked around
| Symptom | Cause | Workaround |
|---|---|---|
| crash executing heap memory in `_CxxThrowException` | Wine's builtin `msvcr110`/`msvcp110` are used instead of the packaged ones (`WINEDEBUG=+loaddll`) and disagree with `vccorlib110` on the WinRT exception layout | per-app DllOverrides `native` for packaged VC runtimes |
| abort in `RoCaptureErrorContext` | unimplemented in combase | import patched to a no-op |
| `fixme:ole:thread_context_callback_ContextCallback`, then fail-fast `c0000409` | PPL task continuations need `IContextCallback::ContextCallback` | vtable patched to call the callback directly |
| `E_NOTIMPL` from `NetworkInformation.GetHostNames` | Wine stub | class mocked |
| `E_NOTIMPL` from `EasClientDeviceInformation`, `ApplicationView`, `AnalyticsInfo`, `AdvertisingManager`, `DeviceAccessInformation` | Wine stubs (`WINEDEBUG=+relay`, then look for `fixme`/`stub` just before `RoOriginateLanguageException(80004001`) | classes mocked (`FORCE` in `tools/gen_reg.py`) |
| missing files whose names contain spaces | APPX stores names percent-encoded | `install` decodes them |

## Debugging a new app
Run without `SHIM_QUIET` and read the log from the end. A crash prints the faulting module+offset and a stack
scan. `C++ exception:` lines show where the app threw. The `frames:` line after them follows the EBP chain and
is exact when the app is built with frame pointers; the stack scan can show stale return addresses. `QI miss` names interfaces that an object lacks.
`WINEDEBUG=fixme+all` exposes Wine stubs that return `E_NOTIMPL`, and `WINEDEBUG=trace+file` shows missing
files. Fixes usually mean adding an `override(...)` with a real implementation in `src/`.

Other things that helped:
- Generic mocks return 0 or null for properties nobody set, which is a common cause of wrong layout or input. List
  what the app reads, for example
  `grep -oE 'call [^ ]*\.get_[A-Za-z]*(Dpi|Scale|Width|Height)[A-Za-z]*' DIR/shim.log | sort | uniq -c`.
- For hangs, get exact backtraces of every thread with winedbg while the app runs:
  `printf "attach 0x%x\nbt all\ndetach\nquit\n" $pid | wine winedbg` (`pid` from `wine tasklist`).
- `wine taskkill /im <exe>` sends `WM_CLOSE` like the window's close button, which is useful for testing shutdown.
- `SHIM_TAPS` together with `SHIM_DUMP_EVERY=1` checks UI flows without anyone at the screen.

## Legal
This repository contains no Microsoft code or metadata. The SDK metadata is downloaded at install time, and
you provide the app package and its dependency packages yourself.
