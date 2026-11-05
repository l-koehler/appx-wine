# appx-wine

Runs Windows 8/8.1 Store apps (`.appx`) under Wine by providing the Windows Runtime pieces Wine lacks:
`Windows.UI.Xaml` startup, `CoreWindow`, the dispatcher, and mocks for everything else.

It targets C++/CX DirectX games, which typically use XAML only to host a swapchain
(`SwapChainBackgroundPanel`) and draw everything with Direct3D 11. Apps with real XAML user interfaces (buttons,
lists, text) and C#/.NET apps will not work: no XAML controls are rendered.

Tested: Asphalt 8: Airborne 1.0.0.10 (x86). Menus, races, saving and keyboard/mouse input all work.

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

## Environment variables for `run`
| Variable | Effect |
|---|---|
| `SHIM_QUIET=1` | don't log each mocked call (the log is `DIR/shim.log` plus stderr) |
| `SHIM_WIDTH`, `SHIM_HEIGHT` | window size, default 1280x720 |
| `SHIM_MOUSE=1` | report the mouse as a mouse pointer; by default it's a touch pointer, which games built for tablets handle better (with a mouse pointer, Asphalt 8 treats hovering in scrolling menus as swiping) |
| `SHIM_DUMP_EVERY=<s>` | write the back buffer to `DIR/frames/frame-NNN.bmp` every `<s>` seconds; F12 dumps one frame |
| `SHIM_TAPS="<s>:<x>,<y>;<s>:key=<vk>;..."` | scripted clicks/key presses (seconds after start, client coordinates), for testing without a person |
| `SHIM_WATCHDOG=<s>` | dump the main thread's stack when it makes no WinRT call for `<s>` seconds |

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
    loader (`x:Name`, `x:ConnectionId`), `Window`, `Loaded` events and the `CompositionTarget.Rendering` loop.
    `MediaElement`s report `MediaEnded` right away, so intro videos are skipped.
  - `dxgi.c`: composition/CoreWindow swapchains become `CreateSwapChainForHwnd`; frame dumps.
  - `collections.c`: `IVector`, `IMap`, iterators. `async.c`: async operations complete immediately.
  - `buffer.c`: `IBuffer`, hashing, `CryptographicBuffer`. `package.c`: identity from `AppxManifest.xml`,
    including the computed publisher ID.
  - `misc.c`: storage folders, display properties, a fake display adapter for PnP queries, and workarounds for
    Wine gaps.

### Wine gaps worked around
| Symptom | Cause | Workaround |
|---|---|---|
| crash executing heap memory in `_CxxThrowException` | Wine's builtin `msvcr110`/`msvcp110` are used instead of the packaged ones (`WINEDEBUG=+loaddll`) and disagree with `vccorlib110` on the WinRT exception layout | per-app DllOverrides `native` for packaged VC runtimes |
| abort in `RoCaptureErrorContext` | unimplemented in combase | import patched to a no-op |
| `fixme:ole:thread_context_callback_ContextCallback`, then fail-fast `c0000409` | PPL task continuations need `IContextCallback::ContextCallback` | vtable patched to call the callback directly |
| `E_NOTIMPL` from `NetworkInformation.GetHostNames` | Wine stub | class mocked |
| missing files whose names contain spaces | APPX stores names percent-encoded | `install` decodes them |

## Debugging a new app
Run without `SHIM_QUIET` and read the log from the end. A crash prints the faulting module+offset and a stack
scan. `C++ exception:` lines show where the app threw. `QI miss` names interfaces that an object lacks.
`WINEDEBUG=fixme+all` exposes Wine stubs that return `E_NOTIMPL`, and `WINEDEBUG=trace+file` shows missing
files. Fixes usually mean adding an `override(...)` with a real implementation in `src/`.

## Legal
This repository contains no Microsoft code or metadata. The SDK metadata is downloaded at install time, and
you provide the app package and its dependency packages yourself.
