cmake_minimum_required(VERSION 3.24)
file(STRINGS VERSION EMF_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS VERSION)  # a VERSION change reconfigures and rebuilds
project(emf VERSION ${EMF_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so our DLLs load with nothing else installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /EHsc /Zi)
  # No incremental linking: it kept stale code from emf_core_lib.lib in emf_core.dll. /OPT:REF drops functions and data
  # nothing uses (/DEBUG alone turns that off). /OPT:REF also turns on /OPT:ICF unless told otherwise, and folding
  # identical functions into one address breaks code that compares function pointers (hook tables refusing a detour
  # twice, callback registries; test_api_patterns caught it): /OPT:NOICF, always.
  add_link_options(/DEBUG:FULL /INCREMENTAL:NO /OPT:REF /OPT:NOICF)
endif()

# --- MinHook (vendored, v1.3.4) ---
add_library(minhook STATIC
  third_party/minhook/src/buffer.c
  third_party/minhook/src/hook.c
  third_party/minhook/src/trampoline.c
  third_party/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)
if(MSVC)
  target_compile_options(minhook PRIVATE /W0)
endif()

# --- Shared library code ---
add_library(emf_common STATIC
  src/common/sigscan.cpp
  src/common/pattern_find.cpp
  src/common/pattern_file.cpp
  src/common/pe_image.cpp
  src/common/keen_map.cpp
  src/common/seh.cpp
  src/common/signatures.cpp
  src/common/code_offsets.cpp
  src/common/world_view.cpp
  src/common/log.cpp
  src/common/hooks.cpp
  src/common/sha256.cpp)
target_link_libraries(emf_common PUBLIC minhook bcrypt)
target_include_directories(emf_common PUBLIC src)

# --- Lua 5.4.9 (vendored, MIT) and the Lua host library ---
file(GLOB LUA_SOURCES ${CMAKE_SOURCE_DIR}/third_party/lua/*.c)
# Compiled as C++: a Lua error is then a C++ exception, so it unwinds the host's C++ frames (locks, strings)
# instead of jumping over them with longjmp.
set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE CXX)
# Lua opens files with fopen (ANSI code page); mod paths are UTF-8: lua_files.h sends them to the wide functions.
if(MSVC)
  set_source_files_properties(${LUA_SOURCES} PROPERTIES COMPILE_OPTIONS "/FI${CMAKE_SOURCE_DIR}/src/lua_host/lua_files.h")
endif()
add_library(lua54 STATIC ${LUA_SOURCES} src/lua_host/lua_files.cpp)
target_include_directories(lua54 PUBLIC third_party/lua)
if(MSVC)
  target_compile_options(lua54 PRIVATE /W0 /TP /EHsc)
endif()
add_library(emf_lua_lib STATIC src/lua_host/fields.cpp src/lua_host/host.cpp src/lua_host/bindings.cpp
  src/core/json.cpp src/core/manifest.cpp)
target_include_directories(emf_lua_lib PUBLIC src sdk/include)
target_link_libraries(emf_lua_lib PUBLIC lua54)

# --- Probe DLL (installed as dinput8.dll next to enshrouded.exe) ---
add_library(emf_probe SHARED
  src/probe/dllmain.cpp
  src/proxy/dinput8_proxy.cpp
  src/probe/probe.cpp
  src/probe/report.cpp
  src/proxy/dinput8.def)
target_link_libraries(emf_probe PRIVATE emf_common)
target_include_directories(emf_probe PRIVATE src)
set_target_properties(emf_probe PROPERTIES OUTPUT_NAME dinput8 RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/probe
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/probe)

# --- Stand-in for a closed DLL mod that installs itself as dinput8.dll (chain-loading tests only; not shipped) ---
add_library(emf_standin_mod SHARED
  tools/standin_mod/dllmain.cpp
  tools/standin_mod/standin.rc
  src/proxy/dinput8_proxy.cpp
  src/proxy/dinput8.def)
set_target_properties(emf_standin_mod PROPERTIES OUTPUT_NAME dinput8 RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/standin
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/standin)

# --- Offline signature check ---
add_executable(emf_sigcheck tools/sigcheck/main.cpp)
target_link_libraries(emf_sigcheck PRIVATE emf_common)

# --- EMF core library (testable part) ---
# --- Dear ImGui v1.91.9 + Vulkan headers v1.3.296 (vendored) for the in-game console ---
add_library(imgui STATIC
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_vulkan.cpp
  third_party/imgui_emf/imgui_emf_assert.cpp)
target_include_directories(imgui PUBLIC third_party/imgui third_party/imgui/backends third_party/vulkan-headers/include
  third_party/imgui_emf)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES VK_NO_PROTOTYPES IMGUI_DISABLE_DEMO_WINDOWS
  IMGUI_USER_CONFIG="imgui_emf_config.h")
target_compile_options(imgui PRIVATE /W0)

add_library(emf_core_lib STATIC
  src/core/dispatcher.cpp
  src/core/known_tools.cpp
  src/core/mods.cpp
  src/core/file_poll.cpp
  src/core/json.cpp
  src/core/manifest.cpp
  src/core/mod_order.cpp
  src/core/settings.cpp
  src/core/hook_table.cpp
  src/core/api.cpp
  src/core/world_tracker.cpp
  src/core/programs.cpp
  src/core/program_registry.cpp
  src/core/system_programs.cpp
  src/core/program_access.cpp
  src/core/inventory.cpp
  src/core/teleport.cpp
  src/core/spawn.cpp
  src/core/save_events.cpp
  src/core/fault_report.cpp
  src/core/world_jobs.cpp
  src/core/events.cpp
  src/core/event_relay.cpp
  src/core/items.cpp
  src/core/commands.cpp
  src/core/builtin_commands.cpp
  src/core/daytime.cpp
  src/core/console.cpp
  src/core/bridge/json_out.cpp
  src/core/bridge/protocol.cpp
  src/core/bridge/service.cpp
  src/core/bridge/console_runner.cpp
  src/core/bridge/pipe_server.cpp
  src/core/bridge/screens.cpp
  src/core/bridge/state.cpp
  src/core/bridge/pixels.cpp
  src/core/bridge/runtime_parts.cpp
  src/core/bridge/runtime.cpp
  src/core/chat_commands.cpp
  src/core/server_chat.cpp
  src/core/server_chat_hooks.cpp
  src/core/overlay/console_model.cpp
  src/core/overlay/wheel_model.cpp
  src/core/overlay/hud_model.cpp
  src/core/overlay/cursor_model.cpp
  src/core/overlay/mods_ui.cpp
  src/core/overlay/wheel_gate.cpp
  src/core/overlay/wheel_ui.cpp
  src/core/overlay/submit.cpp
  src/core/overlay/input_router.cpp
  src/core/overlay/input_hook.cpp
  src/core/overlay/injected_keys.cpp
  src/core/overlay/console_ui.cpp
  src/core/overlay/swapchain_book.cpp
  src/core/overlay/vk_capture.cpp
  src/core/overlay/vk_overlay.cpp
  src/core/overlay/vk_layer.cpp
  src/core/overlay/overlay.cpp)
target_include_directories(emf_core_lib PUBLIC src sdk/include)
target_link_libraries(emf_core_lib PUBLIC emf_common imgui)
target_compile_definitions(emf_core_lib PUBLIC EMF_VERSION="${EMF_VERSION}")
# --- emf_bridge_fake: the debug bridge's protocol with made-up game data, for testing bridge clients
# without the game (scripts/bridge_clients_test.ps1; not installed) ---
add_executable(emf_bridge_fake tools/bridge/fake_game.cpp)
target_link_libraries(emf_bridge_fake PRIVATE emf_core_lib)
set_target_properties(emf_bridge_fake PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

# --- Nexus Mods' public API (used by the app): requests and replies, archive classifier, BBCode to HTML,
# WinHTTP client; with the player's key: nxm:// links, the REST API v1, downloads, single sign-on.
# emf_nexus is its command-line tool. ---
add_library(emf_nexus_lib STATIC src/nexus/api.cpp src/nexus/classify.cpp src/nexus/bbcode.cpp src/nexus/http.cpp
  src/nexus/client.cpp src/nexus/nxm.cpp src/nexus/v1.cpp src/nexus/download.cpp src/nexus/sso.cpp src/nexus/updates.cpp
  src/core/json.cpp)
target_include_directories(emf_nexus_lib PUBLIC src)
target_link_libraries(emf_nexus_lib PUBLIC winhttp bcrypt)
add_executable(emf_nexus tools/nexus_cli/main.cpp)
target_link_libraries(emf_nexus PRIVATE emf_nexus_lib)
target_compile_definitions(emf_nexus PRIVATE EMF_VERSION="${EMF_VERSION}")

# --- EMF framework: loader (dinput8.dll) + core (emf/emf_core.dll); dist/ mirrors the install layout ---
add_library(emf_loader SHARED
  src/loader/dllmain.cpp
  src/proxy/dinput8_proxy.cpp
  src/proxy/dinput8.def)
set_target_properties(emf_loader PROPERTIES OUTPUT_NAME dinput8 RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/dist
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/loader)
add_library(emf_core SHARED src/core/core.cpp)
target_link_libraries(emf_core PRIVATE emf_core_lib)
set_target_properties(emf_core PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/dist/emf)
# The Vulkan layer manifest (emf_core.dll is the layer library; the loader enables it for the game process).
configure_file(src/core/overlay/VkLayer_EMF_console.json ${CMAKE_BINARY_DIR}/dist/emf/vk/VkLayer_EMF_console.json
  COPYONLY)
# The loaders and DLL mods EMF recognises next to the game (read by the core at start-up and by the installer).
configure_file(src/core/known_tools.txt ${CMAKE_BINARY_DIR}/dist/emf/known_tools.txt COPYONLY)

# --- Shipped mods: dist/emf/mods/<id>/<id>.dll next to its mod.json (folder mods) ---
function(emf_mod id)
  set(json ${CMAKE_BINARY_DIR}/dist/emf/mods/${id}/mod.json)
  add_custom_command(OUTPUT ${json}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/mods/${id}/mod.json ${json}
    DEPENDS ${CMAKE_SOURCE_DIR}/mods/${id}/mod.json)
  add_library(${id} SHARED ${ARGN} ${json})
  target_include_directories(${id} PRIVATE sdk/include)
  set_target_properties(${id} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/dist/emf/mods/${id}
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/${id})
endfunction()

# --- Sample mod (built only against sdk/include) ---
emf_mod(sample_mod mods/sample_mod/sample_mod.cpp)

# --- Companion mod (F6: a companion that follows and guards you) ---
emf_mod(companion mods/companion/companion.cpp)

# --- quick_stack mod (L: put what you carry into nearby chests that hold it) ---
emf_mod(quick_stack mods/quick_stack/quick_stack.cpp)

# --- clock mod (the time of day on screen; read-only) ---
emf_mod(clock mods/clock/clock.cpp)

# --- raids mod (Night Raids: waves of enemies march on a base at night) ---
emf_mod(raids mods/raids/raids.cpp)

# --- loot_magnet mod (corpses of kills near a player are looted into them, for every player) ---
emf_mod(loot_magnet mods/loot_magnet/loot_magnet.cpp)

# --- sleep mod (Sleep Through the Day: sleeping by day skips to the evening) ---
emf_mod(sleep mods/sleep/sleep.cpp)

# --- teleports mod (Waypoint Teleports: /unstuck, /home, admin tp/bring; API v20 teleport) ---
emf_mod(teleports mods/teleports/teleports.cpp)
# --- bulk_craft mod (Bulk Craft: repeat a hand-crafted recipe N times through the game's own craft) ---
emf_mod(bulk_craft mods/bulk_craft/bulk_craft.cpp)

# --- examples/hello: the first mod of docs/modding/README.md (built to prove it compiles; not installed) ---
add_library(hello SHARED examples/hello/hello.cpp)
target_include_directories(hello PRIVATE sdk/include)
set_target_properties(hello PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/hello)

# --- template/: the release package's mod template (built here so it keeps compiling; not installed) ---
add_library(template_mymod SHARED template/src/mymod.cpp)
target_include_directories(template_mymod PRIVATE sdk/include)
set_target_properties(template_mymod PROPERTIES OUTPUT_NAME mymod RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/template_mymod)

# --- examples/cpp/waypoints: a C++ example with its own CMake setup, built here too so it keeps compiling;
# shipped in the package's examples\, not installed. The Lua examples (examples/lua/*) run in tests/test_lua_examples.cpp. ---
add_library(waypoints SHARED examples/cpp/waypoints/src/waypoints.cpp)
target_include_directories(waypoints PRIVATE sdk/include)
set_target_properties(waypoints PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples/waypoints
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/waypoints)

# --- villager_jobs mod (villagers craft from a chest; radial wheel) ---
emf_mod(villager_jobs mods/villager_jobs/villager_jobs.cpp)

# --- event_log mod (on_event sample: logs gameplay events on the host) ---
emf_mod(event_log mods/event_log/event_log.cpp)

# --- big_stacks mod (item definitions: bigger stacks, in memory) ---
emf_mod(big_stacks mods/big_stacks/big_stacks.cpp)

# --- lua mod (runs Lua mods) and the Lua mods kill_counter, life_skills and champion (mod.json + main.lua) ---
emf_mod(lua mods/lua/lua_mod.cpp)
target_link_libraries(lua PRIVATE emf_lua_lib)
foreach(m kill_counter life_skills champion grave bounty)
  foreach(f mod.json main.lua)
    configure_file(mods/${m}/${f} ${CMAKE_BINARY_DIR}/dist/emf/mods/${m}/${f} COPYONLY)
  endforeach()
endforeach()

# --- sim_rate mod (developer example: hook_pattern on a function EMF also hooks; off by default) ---
emf_mod(sim_rate mods/sim_rate/sim_rate.cpp)
# --- loot_scale mod (hook_pattern on the game's loot amount getter; off by default) ---
emf_mod(loot_scale mods/loot_scale/loot_scale.cpp)
# --- write_test mod (developer: API v16 writes that reach the client world; off by default, not packaged) ---
emf_mod(write_test mods/write_test/write_test.cpp)
# --- Internal test mods in tools/spikes (not packaged; that folder is not in the public source, so these targets
# exist only where it is). Each lands in build/<preset>/spikes/<id>/ (DLL + mod.json). ---
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/spikes/teleport/teleport_spike.cpp)
  # tps: teleport through the job command buffer (docs/engine_parts/re_teleport.md section 6)
  add_library(tps SHARED tools/spikes/teleport/teleport_spike.cpp)
  target_include_directories(tps PRIVATE sdk/include)
  set_target_properties(tps PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/spikes/tps
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/tps)
  configure_file(tools/spikes/teleport/mod.json ${CMAKE_BINARY_DIR}/spikes/tps/mod.json COPYONLY)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/spikes/bulk_craft/b1_spike.cpp)
  # b1: CraftRecipeEvent with a player operator (Bulk Craft)
  add_library(b1 SHARED tools/spikes/bulk_craft/b1_spike.cpp)
  target_include_directories(b1 PRIVATE sdk/include)
  set_target_properties(b1 PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/spikes/b1
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/b1)
  configure_file(tools/spikes/bulk_craft/mod.json ${CMAKE_BINARY_DIR}/spikes/b1/mod.json COPYONLY)
endif()

# --- Test mods (loaded by tests/test_core.cpp from <tests>/test_mods) ---
foreach(m emf_test_mod emf_test_mod_crash emf_test_mod_reject)
  string(REPLACE "emf_test_mod" "tests/test_mod/test_mod" src ${m})
  add_library(${m} SHARED ${src}.cpp)
  target_include_directories(${m} PRIVATE sdk/include)
  set_target_properties(${m} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test_mods)
endforeach()

# --- Folder-mod test DLL (copied into folder mods by tests/test_mod_loading.cpp; not in test_mods, which the loose-DLL
# tests scan) ---
add_library(emf_test_folder_mod SHARED tests/test_mod/test_folder_mod.cpp)
target_include_directories(emf_test_folder_mod PRIVATE sdk/include)
set_target_properties(emf_test_folder_mod PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test_folder_mod)
add_library(emf_test_helper SHARED tests/test_mod/test_helper.cpp)
set_target_properties(emf_test_helper PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test_folder_mod
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/emf_test_helper)
add_library(emf_test_needs_helper SHARED tests/test_mod/test_needs_helper.cpp)
target_include_directories(emf_test_needs_helper PRIVATE sdk/include)
target_link_libraries(emf_test_needs_helper PRIVATE emf_test_helper)
set_target_properties(emf_test_needs_helper PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test_folder_mod)

# --- EMF.exe: the desktop mod manager, a WebView2 page in a Win32 window (app/, docs/app.md) ---
# The mod list and the page bridge are a library, so emf_tests covers them; they share the core's manifest, settings,
# known-tools table and the loader's chain-file parser.
add_library(emf_app_lib STATIC app/game_folder.cpp app/locate.cpp app/bridge.cpp app/archive.cpp app/install.cpp
  app/browse.cpp app/account_store.cpp app/nxm_handler.cpp app/args.cpp app/updates.cpp app/bridge_updates.cpp
  app/servers.cpp app/server_process.cpp app/server_bridge.cpp
  src/core/json.cpp src/core/manifest.cpp src/core/settings.cpp src/core/known_tools.cpp src/core/mod_order.cpp)
target_include_directories(emf_app_lib PUBLIC ${CMAKE_SOURCE_DIR} src sdk/include)
# Browse and install: the Nexus library, and bcrypt for the SHA-256 of installed files
target_link_libraries(emf_app_lib PUBLIC emf_nexus_lib bcrypt)
# The Nexus account: DPAPI for the stored key, the registry and Explorer's notice for the nxm:// handler
target_link_libraries(emf_app_lib PUBLIC crypt32 advapi32 shell32)
target_compile_definitions(emf_app_lib PRIVATE EMF_VERSION="${EMF_VERSION}")
# EMF.exe itself: build/msvc/app/EMF.exe with the page in app/ui/ next to it. It links WebView2's static loader (no
# WebView2Loader.dll) and must import none of the proxy DLL names mods use (scripts/app_smoke.ps1 checks).
add_executable(emf_app WIN32 app/main.cpp app/app.rc)
target_include_directories(emf_app PRIVATE app third_party/webview2/include)
target_link_libraries(emf_app PRIVATE emf_app_lib ${CMAKE_SOURCE_DIR}/third_party/webview2/x64/WebView2LoaderStatic.lib
  dwmapi ole32 shell32 user32 advapi32)
target_compile_definitions(emf_app PRIVATE EMF_VERSION="${EMF_VERSION}" EMF_VER_MAJOR=${PROJECT_VERSION_MAJOR}
  EMF_VER_MINOR=${PROJECT_VERSION_MINOR} EMF_VER_PATCH=${PROJECT_VERSION_PATCH})
set_target_properties(emf_app PROPERTIES OUTPUT_NAME EMF RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/app)
foreach(f index.html app.css app.js saves.js)
  configure_file(app/ui/${f} ${CMAKE_BINARY_DIR}/app/ui/${f} COPYONLY)
endforeach()
configure_file(src/core/known_tools.txt ${CMAKE_BINARY_DIR}/app/known_tools.txt COPYONLY)  # for folders without EMF

# --- Stand-in for the dedicated server (EMF.exe's server tests only; not shipped): enshrouded_server.exe that stops
# cleanly on Ctrl+C, Ctrl+Break or its window closing, like the real one (tools/standin_server/main.cpp) ---
add_executable(emf_standin_server tools/standin_server/main.cpp)
set_target_properties(emf_standin_server PROPERTIES OUTPUT_NAME enshrouded_server
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/standin_server)

# --- Stand-in for tar.exe (EMF.exe's archive tests only; not shipped): an archive with no size in its header that
# unpacks to far more than its listing says (tools/standin_tar/main.cpp) ---
add_executable(emf_standin_tar tools/standin_tar/main.cpp)
set_target_properties(emf_standin_tar PROPERTIES OUTPUT_NAME tar RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/standin_tar)

# --- Tests ---
enable_testing()
add_executable(emf_tests
  tests/test_main.cpp
  tests/test_sigscan.cpp
  tests/test_fnv.cpp
  tests/test_log_limiter.cpp
  tests/test_keen_map.cpp
  tests/test_pe_image.cpp
  tests/test_signatures.cpp
  tests/test_code_offsets.cpp
  tests/test_world_view.cpp
  tests/test_runtime.cpp
  tests/test_report.cpp
  tests/test_generated.cpp
  tests/test_core.cpp
  tests/test_api.cpp
  tests/test_emf_hpp.cpp
  tests/test_sample_mod.cpp
  tests/test_time_skip.cpp
  tests/test_programs.cpp
  tests/test_program_registry.cpp
  tests/test_system_programs.cpp
  tests/test_inventory.cpp
  tests/test_teleport.cpp
  tests/test_api_teleport.cpp
  tests/test_wheel_model.cpp
  tests/test_hud_model.cpp
  tests/test_wheel_gate.cpp
  tests/test_aim.cpp
  tests/test_keys.cpp
  tests/test_sdk_helpers.cpp
  tests/test_quick_stack.cpp
  tests/test_quick_stack_glue.cpp
  tests/test_quick_stack_moves.cpp
  tests/test_clock.cpp
  tests/test_version.cpp
  tests/test_json.cpp
  tests/test_nexus_api.cpp
  tests/test_nexus_classify.cpp
  tests/test_nexus_bbcode.cpp
  tests/test_nexus_client.cpp
  tests/test_nexus_nxm.cpp
  tests/test_nexus_v1.cpp
  tests/test_nexus_download.cpp
  tests/test_nexus_sso.cpp
  tests/test_nexus_http_local.cpp
  tests/test_nexus_updates.cpp
  tests/test_manifest.cpp
  tests/test_version_range.cpp
  tests/test_setting_controls.cpp
  tests/test_mod_order.cpp
  tests/test_settings.cpp
  tests/test_mod_loading.cpp
  tests/test_api_settings.cpp
  tests/test_mods_ui.cpp
  tests/test_pattern_find.cpp
  tests/test_hook_table.cpp
  tests/test_hook_chain_real.cpp
  tests/test_api_patterns.cpp
  tests/test_api_sender.cpp
  tests/test_player_commands.cpp
  tests/test_pattern_file.cpp
  tests/test_loot_scale.cpp
  tests/test_lua_fields.cpp
  tests/test_lua_host.cpp
  tests/test_lua_bindings.cpp
  tests/test_lua_template.cpp
  tests/test_lua_wheel_group.cpp
  tests/test_beastmaster.cpp
  tests/test_necromancer.cpp
  tests/test_lua_examples.cpp
  tests/test_villager_jobs.cpp
  tests/test_spawn.cpp
  tests/test_spawn_track.cpp
  tests/test_save_events.cpp
  tests/test_npc_command.cpp
  tests/test_events.cpp
  tests/test_companion.cpp
  tests/test_raids.cpp
  tests/test_raids_glue.cpp
  tests/test_loot_magnet.cpp
  tests/test_loot_magnet_glue.cpp
  tests/test_sleep.cpp
  tests/test_teleports.cpp
  tests/test_teleports_glue.cpp
  tests/test_bulk_craft.cpp
  tests/test_bulk_craft_glue.cpp
  tests/test_lua_round2.cpp
  tests/test_bounty.cpp
  tests/test_lua_take_back.cpp
  tests/test_event_relay.cpp
  tests/test_event_log.cpp
  tests/test_items.cpp
  tests/test_item_dump.cpp
  tests/test_commands.cpp
  tests/test_builtin_commands.cpp
  tests/test_console.cpp
  tests/test_test_scratch.cpp
  tests/test_bridge_log.cpp
  tests/test_bridge_console.cpp
  tests/test_bridge_protocol.cpp
  tests/test_bridge_service.cpp
  tests/test_bridge_pipe.cpp
  tests/test_bridge_keys.cpp
  tests/test_bridge_screens.cpp
  tests/test_bridge_capture.cpp
  tests/test_bridge_state.cpp
  tests/test_bridge_overlay.cpp
  tests/test_bridge_pixels.cpp
  tests/test_bridge_runtime.cpp
  tests/test_bridge_swapchain.cpp
  tests/test_big_stacks.cpp
  tests/test_console_model.cpp
  tests/test_overlay_submit.cpp
  tests/test_input_router.cpp
  tests/test_console_ui.cpp
  tests/test_swapchain_book.cpp
  tests/test_overlay_present.cpp
  tests/test_vk_layer.cpp
  tests/test_layer_env.cpp
  tests/test_chat_commands.cpp
  tests/test_server_chat.cpp
  tests/test_server_chat_hooks.cpp
  tests/test_line_queue.cpp
  tests/test_ready_gate.cpp
  tests/test_world_jobs.cpp
  tests/test_entry_hold.cpp
  tests/test_chain_list.cpp
  tests/test_chain_load.cpp
  tests/test_known_tools.cpp
  tests/test_vk_overlay_device.cpp
  tests/test_app_game_folder.cpp
  tests/test_app_locate.cpp
  tests/test_app_bridge.cpp
  tests/test_app_archive.cpp
  tests/test_app_install.cpp
  tests/test_app_browse.cpp
  tests/test_app_account.cpp
  tests/test_app_nexus.cpp
  tests/test_app_nxm_registry.cpp
  tests/test_app_servers.cpp
  tests/test_app_server_process.cpp
  tests/test_app_server_bridge.cpp
  tests/test_app_updates.cpp
  src/probe/report.cpp)
target_include_directories(emf_tests PRIVATE tests sdk/include)
target_compile_definitions(emf_tests PRIVATE EMF_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(emf_tests PRIVATE emf_common emf_core_lib emf_lua_lib emf_app_lib)
target_link_libraries(emf_tests PRIVATE emf_nexus_lib)
add_dependencies(emf_tests emf_app emf_standin_server emf_standin_tar emf_test_folder_mod emf_test_needs_helper sim_rate loot_scale emf_test_mod emf_test_mod_crash emf_test_mod_reject sample_mod companion event_log big_stacks villager_jobs quick_stack clock raids loot_magnet sleep teleports bulk_craft)
target_include_directories(emf_tests PRIVATE third_party/doctest src)
add_test(NAME emf_tests COMMAND emf_tests)

# --- Mod profiles (docs/app.md "Profiles"): src/core/profiles.cpp is the file format, the store and the
# rules, in EMF.exe and in the core (the console's "profile" command). ---
# app/profiles.cpp reads the game folder with EMF.exe's own scan (app/game_folder.cpp, which the core builds too; the
# install records stay in EMF.exe: app/game_folder_installs.cpp) and switches through set_mod_enabled.
target_sources(emf_core_lib PRIVATE src/core/profiles.cpp app/game_folder.cpp app/profiles.cpp src/core/profile_commands.cpp
  src/core/overlay/profiles_ui.cpp)
target_include_directories(emf_core_lib PRIVATE ${CMAKE_SOURCE_DIR})
target_sources(emf_app_lib PRIVATE src/core/profiles.cpp app/game_folder_installs.cpp app/profiles.cpp app/bridge_profiles.cpp)
target_sources(emf_tests PRIVATE tests/test_profiles.cpp tests/test_app_profiles.cpp tests/test_profile_commands.cpp
  tests/test_app_profiles_bridge.cpp tests/test_profile_bar.cpp)

# --- examples/cpp/waypoints' book and guide logic ---
target_sources(emf_tests PRIVATE tests/test_waypoints.cpp)

# --- docs/engine_parts/re_attributes_menu.md: attributes and damage in the API, the bridge's load_world ---
target_sources(emf_core_lib PRIVATE src/core/attributes.cpp src/core/menu_start.cpp)
target_sources(emf_tests PRIVATE tests/test_attributes.cpp tests/test_api_attributes.cpp tests/test_menu_start.cpp)

# --- API v19: chat lines, players' lines, request_save ---
target_sources(emf_tests PRIVATE tests/test_api_chat.cpp)

# --- Hand-over (mods/handover, Lua) ---
foreach(f mod.json main.lua)
  configure_file(mods/handover/${f} ${CMAKE_BINARY_DIR}/dist/emf/mods/handover/${f} COPYONLY)
endforeach()
target_sources(emf_tests PRIVATE tests/test_handover.cpp)

# --- Server Herald (mods/herald) ---
emf_mod(herald mods/herald/herald.cpp)
target_link_libraries(herald PRIVATE winhttp)
target_sources(emf_tests PRIVATE tests/test_herald.cpp tests/test_herald_dll.cpp)
add_dependencies(emf_tests herald)

# --- Save backups (docs/saves.md). src/saves/ is shared by EMF.exe (the Saves tab, the starter backup) and
# the core (automatic backups at game start, the console's saves.* commands, save-info.json from the game's menu). ---
target_sources(emf_core_lib PRIVATE src/saves/backup.cpp src/saves/save_info.cpp src/core/save_slots.cpp src/core/save_commands.cpp)
target_sources(emf_app_lib PRIVATE src/saves/backup.cpp src/saves/save_info.cpp app/bridge_saves.cpp)
target_sources(emf_tests PRIVATE tests/test_save_backup.cpp tests/test_save_info.cpp tests/test_app_saves.cpp tests/test_save_slots.cpp tests/test_save_commands.cpp tests/test_save_fixes.cpp)
