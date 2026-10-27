# Third-party code in EMF

EMF itself is under the MIT licence (`LICENSE`). It includes, or links, the libraries below, each under its own
licence. Their full licence texts are next to their code in `third_party/`. The release zip carries them in
`licenses\`.

| Library | Used for | Licence | Shipped in the release |
|---|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) (`third_party/imgui`) | the in-game console and Mods tab | MIT | yes, inside `emf_core.dll` |
| [Lua 5.4](https://www.lua.org) (`third_party/lua`) | Lua mods | MIT | yes, inside the `lua` mod |
| [MinHook](https://github.com/TsudaKageyu/minhook) (`third_party/minhook`), with Hacker Disassembler Engine | function hooks | BSD 2-Clause | yes, inside `emf_core.dll` |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) (`third_party/vulkan-headers`) | the overlay's Vulkan layer (headers only) | Apache-2.0 | headers compiled in |
| [Microsoft WebView2 SDK](https://www.nuget.org/packages/Microsoft.Web.WebView2) 1.0.4258.31 (`third_party/webview2`) | EMF.exe's window (static loader library) | BSD 3-Clause style (Microsoft) | yes, inside `EMF.exe` |
| [doctest](https://github.com/doctest/doctest) (`third_party/doctest`) | unit tests | MIT | no (tests only) |

`third_party/imgui_emf` holds EMF's own ImGui configuration (MIT, like EMF).

EMF uses Windows' own components at run time: WinHTTP, DPAPI, BCrypt, `tar.exe`, and the WebView2 Runtime that
Windows installs. It does not ship them.

## The game

Enshrouded is a game by Keen Games. EMF is an unofficial, fan-made framework and is not affiliated with or endorsed
by Keen Games. The repository contains no game files. The engine notes in `docs/` describe how the game's engine
works (names, offsets, byte patterns), as found by analysing a legally owned copy, to make mods interoperate with it.
