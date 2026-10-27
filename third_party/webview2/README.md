# WebView2 SDK (vendored, minimal)

Microsoft Edge WebView2 SDK, NuGet package `Microsoft.Web.WebView2` version **1.0.4258.31**
(https://www.nuget.org/packages/Microsoft.Web.WebView2), BSD-3-Clause (`LICENSE.txt`).

Only what EMF.exe (`app/`) needs is kept:

- `include/WebView2.h`, `include/WebView2EnvironmentOptions.h` (from `build/native/include/`);
- `x64/WebView2LoaderStatic.lib` (from `build/native/x64/`): the loader linked into EMF.exe, so no
  `WebView2Loader.dll` is shipped. It imports only kernel32, advapi32 and ole32 functions.

The browser itself is the WebView2 Runtime that Windows 11 and current Windows 10 already have
(https://developer.microsoft.com/microsoft-edge/webview2/).
