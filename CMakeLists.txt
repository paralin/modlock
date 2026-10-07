cmake_minimum_required(VERSION 3.24)
project(Modlock VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# scripts/lint-cc.ts reads the compile database.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(APPLE)
  set(CMAKE_INSTALL_RPATH "@loader_path;@loader_path/../lib")
elseif(UNIX)
  set(CMAKE_INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib")
endif()
set(BUILD_SHARED_LIBS OFF)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

set(ABSL_PROPAGATE_CXX_STD ON)
set(ABSL_BUILD_TESTING OFF)
set(ABSL_ENABLE_INSTALL ON)
set(ABSL_MSVC_STATIC_RUNTIME OFF)
set(protobuf_BUILD_TESTS OFF)
option(protobuf_BUILD_PROTOC_BINARIES "Build the optional native schema compiler" OFF)
if(CMAKE_CROSSCOMPILING)
  set(protobuf_BUILD_PROTOC_BINARIES OFF CACHE BOOL "" FORCE)
endif()
set(protobuf_BUILD_LIBUPB ${protobuf_BUILD_PROTOC_BINARIES})
set(protobuf_BUILD_SHARED_LIBS ON)
set(protobuf_INSTALL ON)
set(protobuf_MSVC_STATIC_RUNTIME OFF)
set(protobuf_ABSL_PROVIDER "module")

function(modlock_resolve_module name out)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/vendor/${name})
    set(${out} ${CMAKE_CURRENT_SOURCE_DIR}/vendor/${name} PARENT_SCOPE)
    return()
  endif()
  find_program(GO_EXECUTABLE go)
  if(NOT GO_EXECUTABLE)
    return()
  endif()
  execute_process(
    COMMAND ${GO_EXECUTABLE} list -m -f {{.Dir}} ${name}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE resolved
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE result
  )
  if(result EQUAL 0 AND EXISTS ${resolved})
    set(${out} ${resolved} PARENT_SCOPE)
  endif()
endfunction()

modlock_resolve_module(github.com/aperturerobotics/abseil-cpp MODLOCK_ABSEIL_DIR)
modlock_resolve_module(github.com/aperturerobotics/protobuf MODLOCK_PROTOBUF_DIR)
if(NOT MODLOCK_ABSEIL_DIR OR NOT MODLOCK_PROTOBUF_DIR)
  message(FATAL_ERROR
    "Cannot locate the aperturerobotics protobuf fork / abseil mirror. "
    "Run 'go mod download github.com/aperturerobotics/protobuf "
    "github.com/aperturerobotics/abseil-cpp' (Go required), or populate "
    "vendor/.")
endif()
message(STATUS "abseil: ${MODLOCK_ABSEIL_DIR}")
message(STATUS "protobuf fork: ${MODLOCK_PROTOBUF_DIR}")

if(WIN32)
  set(BUILD_SHARED_LIBS ON)
  set(ABSL_BUILD_MONOLITHIC_SHARED_LIBS ON)
endif()
add_subdirectory(${MODLOCK_ABSEIL_DIR} build-deps/abseil)
set(BUILD_SHARED_LIBS OFF)
if(MINGW)
  # The upstream monolithic list includes this Windows input only under MSVC.
  target_sources(abseil_dll PRIVATE cmake/zig-dll-exports.cc
    ${MODLOCK_ABSEIL_DIR}/absl/time/internal/cctz/src/time_zone_name_win.cc)
endif()
set(utf8_range_ENABLE_TESTS OFF)
set(utf8_range_ENABLE_INSTALL ON)
file(READ ${MODLOCK_PROTOBUF_DIR}/version.json protobuf_versions)
string(JSON protobuf_series MEMBER "${protobuf_versions}" 0)
string(JSON protobuf_VERSION GET "${protobuf_versions}" ${protobuf_series} protoc_version)
add_subdirectory(${MODLOCK_PROTOBUF_DIR}/third_party/utf8_range build-deps/utf8_range)
target_include_directories(utf8_range PUBLIC
  $<BUILD_INTERFACE:${MODLOCK_PROTOBUF_DIR}/third_party/utf8_range>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
add_subdirectory(${MODLOCK_PROTOBUF_DIR} build-deps/protobuf)
include(cmake/wasmtime.cmake)

# The SDK embeds the game data file so every program starts with its
# signatures; modlock-host replaces them with the copy installed beside it.
file(READ data/game_signatures.txtpb MODLOCK_GAME_SIGNATURES)
configure_file(cmake/game_signatures_data.cc.in src/gameinterop/game_signatures_data.cc @ONLY)
configure_file(data/game_signatures.txtpb game_signatures.txtpb COPYONLY)

add_library(modlock_sdk SHARED
  src/camera/camera_path.cc
  src/engine_host.cc
  src/plugin_library.cc
  src/gameinterop/ability_definitions.cc
  src/gameinterop/ability_input_hook.cc
  src/gameinterop/ability_modifier.cc
  src/gameinterop/ability_slots.cc
  src/gameinterop/projectile_impact_hook.cc
  src/gameinterop/movement_hook.cc
  src/gameinterop/bot_creation.cc
  src/gameinterop/client_command_hook.cc
  src/gameinterop/server_addons.cc
  src/gameinterop/combat_events.cc
  src/gameinterop/connection_tracker.cc
  src/gameinterop/console_variables.cc
  src/gameinterop/engine_server.cc
  src/gameinterop/entity_abi.cc
  src/gameinterop/frame_hook.cc
  src/gameinterop/game_rules.cc
  src/gameinterop/game_signatures.cc
  ${CMAKE_CURRENT_BINARY_DIR}/src/gameinterop/game_signatures_data.cc
  src/gameinterop/game_symbols.cc
  src/gameinterop/hero_definitions.cc
  src/gameinterop/hero_initialization.cc
  src/gameinterop/keyvalues.cc
  src/gameinterop/match_clock.cc
  src/gameinterop/native_chat_hook.cc
  src/gameinterop/native_damage.cc
  src/gameinterop/native_sound.cc
  src/gameinterop/native_trace.cc
  src/gameinterop/native_user_messages.cc
  src/gameinterop/modifier_states.cc
  src/gameinterop/pawn_observer.cc
  src/gameinterop/player_selection.cc
  src/gameinterop/respawn_guard.cc
  src/gameinterop/server_clock.cc
  src/gameinterop/setpawn_observe.cc
  src/gameinterop/signature.cc
  src/gameinterop/stamina_observer.cc
  src/gameinterop/startup_server_probe.cc
  src/gameinterop/teleport_pawn_control.cc
  src/gameinterop/vtable_slot_hook.cc
  src/gameinterop/thunk_owner.cc
  src/gameinterop/world_entities.cc
  src/host.cc
  src/host_app/game_paths.cc
  src/host_app/stdio_guard.cc
  src/net/listen_boot.cc
  src/quake/bsp_world.cc
  src/quake/collision_hull.cc
  src/quake/collision_scene.cc
  src/quake/movement.cc
  src/quake/player_movement.cc
  src/render/world_effect_game_factory.cc
  src/render/world_text_game_factory.cc
  src/wasm/instance.cc
  src/wasm/runtime.cc
  src/wasm/ability_tuning.cc
  src/wasm/game.cc
  src/wasm/hero_restore.cc
  src/wasm/npc_restore.cc
  src/wasm/ui.cc
  src/wasm/settings.cc
  src/wasm/metrics.cc
  src/wasm/file_settings.cc
  src/wasm/wasm_host.cc
  proto/modlock/camera.pb.cc
  proto/modlock/chat.pb.cc
  proto/modlock/game_signatures.pb.cc
  proto/modlock/hud.pb.cc
  proto/modlock/stamina_consumed.pb.cc
  proto/modlock/types.pb.cc
  proto/modlock/ui.pb.cc
  proto/modlock/wasm.pb.cc
)
add_library(modlock::sdk ALIAS modlock_sdk)
if(MINGW)
  # Imported DLL data cannot initialize protobuf's constinit parse tables.
  # Keep the same dynamic-initialization mode in the runtime and its consumers.
  target_compile_options(libprotobuf PUBLIC -U__cpp_constinit)
  target_sources(libprotobuf PRIVATE cmake/zig-dll-exports.cc)
  target_sources(modlock_sdk PRIVATE cmake/zig-dll-exports.cc)
endif()
set_target_properties(modlock_sdk PROPERTIES EXPORT_NAME sdk)
target_include_directories(modlock_sdk PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(modlock_sdk PUBLIC protobuf::libprotobuf PRIVATE wasmtime::wasmtime)
target_compile_features(modlock_sdk PUBLIC cxx_std_23)
target_compile_definitions(modlock_sdk PRIVATE MODLOCK_BUILDING_SDK)
if(MSVC)
  target_compile_options(modlock_sdk PUBLIC /FImodlock/export.h)
  target_compile_options(modlock_sdk PRIVATE /EHs-c-)
else()
  target_compile_options(modlock_sdk PUBLIC -include modlock/export.h)
  target_compile_options(modlock_sdk PRIVATE -fno-exceptions)
endif()

if(WIN32)
  target_sources(modlock_sdk PRIVATE
    src/gameinterop/windows/mapped_module_image.cc
    src/gameinterop/windows/native_memory.cc
    src/host_app/windows/engine_log.cc
    src/host_app/windows/import_patch.cc
    src/host_app/windows/vconsole_port.cc
    src/host_app/windows/windows_module_loader.cc
    third_party/safetyhook/safetyhook.cpp
    third_party/safetyhook/Zydis.c)
  target_include_directories(modlock_sdk SYSTEM PRIVATE third_party/safetyhook)
  target_compile_definitions(modlock_sdk PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(modlock_sdk PRIVATE ws2_32 psapi)
else()
  target_sources(modlock_sdk PRIVATE
    src/gameinterop/unsupported/mapped_module_image.cc
    src/gameinterop/unsupported/native_memory.cc)
endif()

target_link_libraries(modlock_sdk PRIVATE ${CMAKE_DL_LIBS})

# The server and each predicting client must compute identical Quake movement,
# so its arithmetic is strict IEEE with no fused multiply-add.
set(modlock_quake_sources
  src/quake/bsp_world.cc
  src/quake/collision_hull.cc
  src/quake/collision_scene.cc
  src/quake/movement.cc
  src/quake/player_movement.cc)
if(MSVC)
  set_source_files_properties(${modlock_quake_sources} PROPERTIES COMPILE_OPTIONS /fp:strict)
else()
  set_source_files_properties(${modlock_quake_sources} PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
endif()

add_executable(modlock-host
  src/host_app/control_link.cc
  src/host_app/game_dump.cc
  src/host_app/main.cc
  proto/modlock/control/control.pb.cc
  proto/modlock/dump/dump.pb.cc)
target_include_directories(modlock-host PRIVATE src)
target_link_libraries(modlock-host PRIVATE modlock::sdk)
if(WIN32)
  target_link_libraries(modlock-host PRIVATE ws2_32)
endif()
install(TARGETS modlock-host RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/game_signatures.txtpb DESTINATION ${CMAKE_INSTALL_BINDIR})
if(WIN32)
  install(FILES ${MODLOCK_WASMTIME_LIBRARY} DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
  install(FILES ${MODLOCK_WASMTIME_LIBRARY} DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

# Luau mods run on Luau, compiled for WASI by wasi-sdk because Luau raises its
# errors as C++ exceptions, which need the WebAssembly exception handling
# proposal and its unwinder.
include(FetchContent)
cmake_host_system_information(RESULT host_os QUERY OS_NAME)
string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" host_arch)
if(host_arch MATCHES "^(arm64|aarch64)$")
  set(host_arch arm64)
else()
  set(host_arch x86_64)
endif()
if(host_os STREQUAL "macOS")
  set(wasi_sdk_platform ${host_arch}-macos)
elseif(host_os STREQUAL "Windows")
  set(wasi_sdk_platform ${host_arch}-windows)
else()
  set(wasi_sdk_platform ${host_arch}-linux)
endif()
set(wasi_sdk_hashes
  arm64-linux f7e243dff54d60bcc576e94d6166b69f410f2500ae4a9ceef34315be10e77971
  arm64-macos 9c59398106b417f8f14913380fdf0097a8cc0ff4af9eb3ce0065a859e88d49e9
  arm64-windows 45e1c71f3e965621e7b98ebe1d37b0e4b1f77f3e8072113ffb4534e67b1a4b7c
  x86_64-linux b761e3a0721dbae9c09a0059e5fdb2bf917d1b4a8a7b430fb3b5aafb0984b2c4
  x86_64-macos 87d27fa8adc68dee59bfbf2e22a6d34ef717c34d6bf1d8af2a56fc929d9ce0eb
  x86_64-windows cccb5c323a9b34f0349a9b09e8804a0a7632c68c3310f4b5f437ed57d7e71d8f)
list(FIND wasi_sdk_hashes ${wasi_sdk_platform} wasi_sdk_index)
math(EXPR wasi_sdk_index "${wasi_sdk_index} + 1")
list(GET wasi_sdk_hashes ${wasi_sdk_index} wasi_sdk_hash)
FetchContent_Declare(wasi_sdk
  URL https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-34/wasi-sdk-34.0-${wasi_sdk_platform}.tar.gz
  URL_HASH SHA256=${wasi_sdk_hash}
  SOURCE_SUBDIR none)
FetchContent_Declare(luau
  URL https://github.com/luau-lang/luau/archive/refs/tags/0.741.tar.gz
  URL_HASH SHA256=deee653d0f5971b809ef67f50dbe8109ad369d7575e344cfe2013774fed2f4c6
  SOURCE_SUBDIR none)
FetchContent_MakeAvailable(wasi_sdk luau)
set(luau_parts Common Ast Bytecode Compiler VM)
set(luau_sources ${CMAKE_CURRENT_SOURCE_DIR}/runtimes/luau/runtime.cc)
set(luau_includes)
foreach(part IN LISTS luau_parts)
  file(GLOB part_sources ${luau_SOURCE_DIR}/${part}/src/*.cpp)
  list(APPEND luau_sources ${part_sources})
  list(APPEND luau_includes -I${luau_SOURCE_DIR}/${part}/include)
endforeach()
set(luau_wasm ${CMAKE_CURRENT_BINARY_DIR}/luau.wasm)
add_custom_command(OUTPUT ${luau_wasm}
  COMMAND ${wasi_sdk_SOURCE_DIR}/bin/clang++ --target=wasm32-wasip1 -std=c++20 -O2 -s
    -fwasm-exceptions -mllvm -wasm-use-legacy-eh=false -mexec-model=reactor
    -D_WASI_EMULATED_PROCESS_CLOCKS -Wno-c23-extensions ${luau_includes}
    -Wl,-z,stack-size=2097152
    -o ${luau_wasm} ${luau_sources} -lunwind -lwasi-emulated-process-clocks
  DEPENDS ${luau_sources} ${CMAKE_CURRENT_SOURCE_DIR}/runtimes/boundary.h
    ${CMAKE_CURRENT_SOURCE_DIR}/runtimes/luau/prelude.luau
  COMMENT "Compiling the Luau runtime")
add_custom_target(luau_runtime ALL DEPENDS ${luau_wasm})
install(FILES ${luau_wasm} DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${luau_SOURCE_DIR}/LICENSE.txt DESTINATION ${CMAKE_INSTALL_BINDIR}
  RENAME luau-LICENSE)

# TypeScript and JavaScript mods run on QuickJS, compiled for WASI by the same
# wasi-sdk. modlock-host loads it from beside the executable, where the build
# and the install both put it.
FetchContent_Declare(quickjs_ng
  URL https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.17.0.tar.gz
  URL_HASH SHA256=559bc4c420475e55c7ab4510adbc562f55d7524d75e8e89d79ce4bb02f5687d9
  SOURCE_SUBDIR none)
FetchContent_MakeAvailable(quickjs_ng)
set(quickjs_sources
  ${quickjs_ng_SOURCE_DIR}/dtoa.c
  ${quickjs_ng_SOURCE_DIR}/libregexp.c
  ${quickjs_ng_SOURCE_DIR}/libunicode.c
  ${quickjs_ng_SOURCE_DIR}/quickjs.c
  ${CMAKE_CURRENT_SOURCE_DIR}/runtimes/quickjs/runtime.c)
set(quickjs_wasm ${CMAKE_CURRENT_BINARY_DIR}/quickjs.wasm)
add_custom_command(OUTPUT ${quickjs_wasm}
  COMMAND ${wasi_sdk_SOURCE_DIR}/bin/clang --target=wasm32-wasip1 -Oz -s -mexec-model=reactor
    -D_GNU_SOURCE -I${quickjs_ng_SOURCE_DIR} -Wl,-z,stack-size=2097152
    -o ${quickjs_wasm} ${quickjs_sources}
  DEPENDS ${quickjs_sources} ${CMAKE_CURRENT_SOURCE_DIR}/runtimes/boundary.h
  COMMENT "Compiling the QuickJS runtime")
add_custom_target(quickjs_runtime ALL DEPENDS ${quickjs_wasm})
install(FILES ${quickjs_wasm} DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${quickjs_ng_SOURCE_DIR}/LICENSE DESTINATION ${CMAKE_INSTALL_BINDIR}
  RENAME quickjs-LICENSE)

# Python mods run on CPython for WASI, linked from its published WASI build
# and snapshotted by Wizer after the interpreter starts, so an instance begins
# with Python initialized and reads no files.
if(host_os STREQUAL "Windows")
  set(wasmtime_cli_archive zip)
else()
  set(wasmtime_cli_archive tar.xz)
endif()
string(REPLACE arm64 aarch64 wasmtime_cli_platform ${wasi_sdk_platform})
set(wasmtime_cli_hashes
  aarch64-linux ca14988c6da3d92512bd9c3bad6cbd06b1f5743b6911a9e709f1a6fa30637d7f
  aarch64-macos ea5a79261caa39819b863fd850879428cf90548353e1e5cf9bcbcc4e7addf2c1
  aarch64-windows e3a5cf6a18b9af5e922f5e3434711278311b58da010765d9bb5b7885a1bf2095
  x86_64-linux a4d6e9e3a5a60f527cf7793d674c48930c80c2e8977995b8a275cad3254b9322
  x86_64-macos b24513efff6a5dbeb196e2a01097ba632a8b0ea1563ba297ada26ceb3aa067d9
  x86_64-windows c95a3925acde6398808baabe5ad13d0b1df02b004bc256c00cf8e2a36daccea8)
list(FIND wasmtime_cli_hashes ${wasmtime_cli_platform} wasmtime_cli_index)
math(EXPR wasmtime_cli_index "${wasmtime_cli_index} + 1")
list(GET wasmtime_cli_hashes ${wasmtime_cli_index} wasmtime_cli_hash)
FetchContent_Declare(wasmtime_cli
  URL https://github.com/bytecodealliance/wasmtime/releases/download/v${MODLOCK_WASMTIME_VERSION}/wasmtime-v${MODLOCK_WASMTIME_VERSION}-${wasmtime_cli_platform}.${wasmtime_cli_archive}
  URL_HASH SHA256=${wasmtime_cli_hash}
  SOURCE_SUBDIR none)
set(python_version 3.14.7)
set(python_wasi https://github.com/brettcannon/cpython-wasi-build/releases/download/v${python_version})
FetchContent_Declare(cpython
  URL https://www.python.org/ftp/python/${python_version}/Python-${python_version}.tgz
  URL_HASH SHA256=62859805f6fdf25e2bcbf3fa3217801e1996887ca33e6a2af80674bdfa2dbe07
  SOURCE_SUBDIR none)
FetchContent_Declare(cpython_wasi_build
  URL ${python_wasi}/_build-python-${python_version}-wasi_sdk-24.zip
  URL_HASH SHA256=a2a9fca77dc47bb9d2b0dd7d04538f8aa188e5f0a87b362fd569c0d024039100
  SOURCE_SUBDIR none)
FetchContent_Declare(cpython_wasi
  URL ${python_wasi}/python-${python_version}-wasi_sdk-24.zip
  URL_HASH SHA256=2e064d3fb8172471d39d741348efa722349c40b96301f69968dff714999c584b
  SOURCE_SUBDIR none)
FetchContent_MakeAvailable(wasmtime_cli cpython cpython_wasi_build cpython_wasi)
set(python_build ${cpython_wasi_build_SOURCE_DIR})
file(GLOB python_hacl ${python_build}/Modules/_hacl/*.a)
set(python_libraries ${python_build}/libpython3.14.a ${python_build}/Modules/_decimal/libmpdec/libmpdec.a
  ${python_hacl} ${python_build}/Modules/expat/libexpat.a)
set(python_sources ${CMAKE_CURRENT_SOURCE_DIR}/runtimes/python/runtime.c)
set(python_raw_wasm ${CMAKE_CURRENT_BINARY_DIR}/python-uninitialized.wasm)
set(python_wasm ${CMAKE_CURRENT_BINARY_DIR}/python.wasm)
add_custom_command(OUTPUT ${python_raw_wasm}
  COMMAND ${wasi_sdk_SOURCE_DIR}/bin/clang --target=wasm32-wasip1 -std=c23 -O2 -s
    -mexec-model=reactor -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_GETPID
    -D_WASI_EMULATED_PROCESS_CLOCKS -I${cpython_SOURCE_DIR}/Include -I${python_build}
    -Wl,-z,stack-size=8388608 -Wl,--stack-first
    -o ${python_raw_wasm} ${python_sources} ${python_libraries} -ldl -lm
    -lwasi-emulated-signal -lwasi-emulated-getpid -lwasi-emulated-process-clocks
  DEPENDS ${python_sources} ${python_libraries} ${CMAKE_CURRENT_SOURCE_DIR}/runtimes/boundary.h
    ${CMAKE_CURRENT_SOURCE_DIR}/runtimes/python/prelude.py
  COMMENT "Compiling the Python runtime")
add_custom_command(OUTPUT ${python_wasm}
  COMMAND ${wasmtime_cli_SOURCE_DIR}/wasmtime wizer -S cli=y -f modlock_initialize
    --dir ${cpython_wasi_SOURCE_DIR}/lib::/lib
    --preload modlock=${CMAKE_CURRENT_SOURCE_DIR}/runtimes/python/host.wat
    -o ${python_wasm} ${python_raw_wasm}
  DEPENDS ${python_raw_wasm} ${CMAKE_CURRENT_SOURCE_DIR}/runtimes/python/host.wat
  COMMENT "Initializing the Python runtime")
add_custom_target(python_runtime ALL DEPENDS ${python_wasm})
install(FILES ${python_wasm} DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${cpython_SOURCE_DIR}/LICENSE DESTINATION ${CMAKE_INSTALL_BINDIR}
  RENAME python-LICENSE)

# modlock-sigcheck checks the recorded signatures against game binaries on
# disk; it runs on any host, so it carries its own copy of the decoder.
add_executable(modlock-sigcheck src/sigcheck/main.cc third_party/safetyhook/Zydis.c)
target_include_directories(modlock-sigcheck SYSTEM PRIVATE third_party/safetyhook)
target_link_libraries(modlock-sigcheck PRIVATE modlock::sdk)
install(TARGETS modlock-sigcheck RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(NOT MODLOCK_REVISION)
  execute_process(COMMAND git rev-parse HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE MODLOCK_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
endif()
string(LENGTH "${MODLOCK_REVISION}" MODLOCK_REVISION_LENGTH)
if(NOT MODLOCK_REVISION_LENGTH EQUAL 40 OR NOT MODLOCK_REVISION MATCHES "^[0-9a-f]+$")
  message(FATAL_ERROR "MODLOCK_REVISION must identify the resolved source commit")
endif()
file(GLOB_RECURSE MODLOCK_HEADERS CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h ${CMAKE_CURRENT_SOURCE_DIR}/proto/*.pb.h)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${MODLOCK_HEADERS})
set(MODLOCK_INTERFACE_INPUT "")
foreach(header IN LISTS MODLOCK_HEADERS)
  file(SHA256 ${header} digest)
  string(APPEND MODLOCK_INTERFACE_INPUT "${digest}\n")
endforeach()
string(SHA256 MODLOCK_INTERFACE_HASH "${MODLOCK_INTERFACE_INPUT}")
configure_file(cmake/build.h.in include/modlock/build.h @ONLY)

install(TARGETS modlock_sdk EXPORT ModlockTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY tests/ DESTINATION ${CMAKE_INSTALL_DATADIR}/modlock/testing FILES_MATCHING PATTERN "*.h")
install(DIRECTORY proto/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/proto FILES_MATCHING PATTERN "*.h")
install(DIRECTORY proto/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/github.com/paralin/modlock/proto FILES_MATCHING PATTERN "*.h")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/modlock/build.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/modlock)
install(EXPORT ModlockTargets NAMESPACE modlock:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Modlock)
configure_package_config_file(cmake/ModlockConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/ModlockConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Modlock)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/ModlockConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ModlockConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/ModlockConfigVersion.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Modlock)

if(BUILD_TESTING)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)
  add_executable(modifier_states_test tests/modifier_states_test.cc)
  target_link_libraries(modifier_states_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME modifier_states_test COMMAND modifier_states_test)
  add_executable(camera_path_test tests/camera_path_test.cc)
  target_link_libraries(camera_path_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME camera_path_test COMMAND camera_path_test)
  add_executable(ability_definitions_test tests/ability_definitions_test.cc)
  target_link_libraries(ability_definitions_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME ability_definitions_test COMMAND ability_definitions_test)
  add_executable(ability_input_hook_test tests/ability_input_hook_test.cc)
  target_link_libraries(ability_input_hook_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME ability_input_hook_test COMMAND ability_input_hook_test)
  add_executable(combat_events_test tests/combat_events_test.cc)
  target_link_libraries(combat_events_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME combat_events_test COMMAND combat_events_test)
  add_executable(console_variables_test tests/console_variables_test.cc)
  target_link_libraries(console_variables_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME console_variables_test COMMAND console_variables_test)
  add_executable(engine_server_test tests/engine_server_test.cc)
  target_link_libraries(engine_server_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME engine_server_test COMMAND engine_server_test)
  add_executable(gameinterop_connection_tracker_test tests/gameinterop_connection_tracker_test.cc)
  target_link_libraries(gameinterop_connection_tracker_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME gameinterop_connection_tracker_test COMMAND gameinterop_connection_tracker_test)
  add_executable(gameinterop_frame_hook_test tests/gameinterop_frame_hook_test.cc)
  target_link_libraries(gameinterop_frame_hook_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME gameinterop_frame_hook_test COMMAND gameinterop_frame_hook_test)
  add_executable(gameinterop_hero_definitions_test tests/gameinterop_hero_definitions_test.cc)
  target_link_libraries(gameinterop_hero_definitions_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME gameinterop_hero_definitions_test COMMAND gameinterop_hero_definitions_test)
  add_executable(gameinterop_hero_initialization_test tests/gameinterop_hero_initialization_test.cc)
  target_link_libraries(gameinterop_hero_initialization_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME gameinterop_hero_initialization_test COMMAND gameinterop_hero_initialization_test)
  add_executable(gameinterop_keyvalues_test tests/gameinterop_keyvalues_test.cc)
  target_link_libraries(gameinterop_keyvalues_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME gameinterop_keyvalues_test COMMAND gameinterop_keyvalues_test)
  add_executable(gameinterop_resource_service_test tests/gameinterop_resource_service_test.cc)
  target_link_libraries(gameinterop_resource_service_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME gameinterop_resource_service_test COMMAND gameinterop_resource_service_test)
  add_executable(gameinterop_schema_abi_test tests/gameinterop_schema_abi_test.cc tests/schema_enum_test.cc)
  target_link_libraries(gameinterop_schema_abi_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME gameinterop_schema_abi_test COMMAND gameinterop_schema_abi_test)
  add_executable(gameinterop_server_clock_test tests/gameinterop_server_clock_test.cc)
  target_link_libraries(gameinterop_server_clock_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME gameinterop_server_clock_test COMMAND gameinterop_server_clock_test)
  add_executable(gameinterop_signature_test tests/gameinterop_signature_test.cc)
  target_link_libraries(gameinterop_signature_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME gameinterop_signature_test COMMAND gameinterop_signature_test)
  add_executable(mapped_module_image_test tests/mapped_module_image_test.cc)
  target_link_libraries(mapped_module_image_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME mapped_module_image_test COMMAND mapped_module_image_test)
  add_executable(game_symbols_test tests/game_symbols_test.cc)
  target_link_libraries(game_symbols_test PRIVATE modlock::sdk gtest_main)
  target_compile_definitions(game_symbols_test PRIVATE
    MODLOCK_GAME_SIGNATURES_FILE="${CMAKE_CURRENT_SOURCE_DIR}/data/game_signatures.txtpb")
  add_test(NAME game_symbols_test COMMAND game_symbols_test)
  add_executable(gameinterop_world_entities_test tests/gameinterop_world_entities_test.cc)
  target_link_libraries(gameinterop_world_entities_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME gameinterop_world_entities_test COMMAND gameinterop_world_entities_test)
  add_executable(gameinterop_teleport_test tests/gameinterop_teleport_test.cc)
  target_link_libraries(gameinterop_teleport_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME gameinterop_teleport_test COMMAND gameinterop_teleport_test)
  add_executable(gameinterop_thunk_owner_test tests/gameinterop_thunk_owner_test.cc)
  target_link_libraries(gameinterop_thunk_owner_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME gameinterop_thunk_owner_test COMMAND gameinterop_thunk_owner_test)
  add_executable(native_chat_hook_test tests/native_chat_hook_test.cc)
  target_link_libraries(native_chat_hook_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME native_chat_hook_test COMMAND native_chat_hook_test)
  add_executable(net_listen_boot_test tests/net_listen_boot_test.cc)
  target_link_libraries(net_listen_boot_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME net_listen_boot_test COMMAND net_listen_boot_test)
  add_executable(pawn_observer_test tests/pawn_observer_test.cc tests/preparation_movement_test.cc)
  target_link_libraries(pawn_observer_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME pawn_observer_test COMMAND pawn_observer_test)
  add_executable(callbacks_test tests/callbacks_test.cc)
  target_include_directories(callbacks_test PRIVATE src)
  target_link_libraries(callbacks_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME callbacks_test COMMAND callbacks_test)
  add_executable(plugin_host_test tests/plugin_host_test.cc)
  target_link_libraries(plugin_host_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME plugin_host_test COMMAND plugin_host_test)
  add_executable(setpawn_observe_test tests/setpawn_observe_test.cc)
  target_link_libraries(setpawn_observe_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME setpawn_observe_test COMMAND setpawn_observe_test)
  add_executable(quake_movement_test tests/quake_movement_test.cc ${modlock_quake_sources})
  target_include_directories(quake_movement_test PRIVATE src)
  target_link_libraries(quake_movement_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME quake_movement_test COMMAND quake_movement_test)
  add_executable(stdio_guard_test tests/stdio_guard_test.cc)
  target_link_libraries(stdio_guard_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME stdio_guard_test COMMAND stdio_guard_test)
  add_executable(world_effect_game_factory_test tests/world_effect_game_factory_test.cc)
  target_link_libraries(world_effect_game_factory_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME world_effect_game_factory_test COMMAND world_effect_game_factory_test)
  # wasm_instance_test runs the Go example mod, so it builds it with Go, and
  # the Luau example on the Luau runtime.
  set(hello_go_wasm ${CMAKE_CURRENT_BINARY_DIR}/hello-go.wasm)
  file(GLOB hello_go_sources CONFIGURE_DEPENDS mod/*.go examples/hello-go/*.go proto/modlock/wasm/*.go)
  add_custom_command(OUTPUT ${hello_go_wasm}
    COMMAND ${CMAKE_COMMAND} -E env GOOS=wasip1 GOARCH=wasm
      ${GO_EXECUTABLE} build -buildmode=c-shared -o ${hello_go_wasm} ./examples/hello-go
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS ${hello_go_sources} go.mod)
  add_custom_target(hello_go_wasm DEPENDS ${hello_go_wasm})
  add_executable(wasm_instance_test tests/wasm_instance_test.cc src/wasm/instance.cc
    src/wasm/runtime.cc)
  add_dependencies(wasm_instance_test hello_go_wasm luau_runtime python_runtime)
  target_include_directories(wasm_instance_test PRIVATE src)
  target_compile_definitions(wasm_instance_test PRIVATE HELLO_GO_WASM="${hello_go_wasm}"
    LUAU_WASM="${luau_wasm}"
    HELLO_LUAU_MAIN="${CMAKE_CURRENT_SOURCE_DIR}/examples/hello-luau/main.luau"
    LUAU_LIBRARY="${CMAKE_CURRENT_SOURCE_DIR}/luau/modlock" PYTHON_WASM="${python_wasm}"
    HELLO_PYTHON_MAIN="${CMAKE_CURRENT_SOURCE_DIR}/examples/hello-python/main.py"
    PYTHON_LIBRARY="${CMAKE_CURRENT_SOURCE_DIR}/python/modlock")
  target_link_libraries(wasm_instance_test PRIVATE modlock::sdk wasmtime::wasmtime gtest_main)
  add_test(NAME wasm_instance_test COMMAND wasm_instance_test)
  # wasm_host_test runs a Go mod that serves and calls extension services.
  set(extension_go_wasm ${CMAKE_CURRENT_BINARY_DIR}/extension-go.wasm)
  file(GLOB extension_go_sources CONFIGURE_DEPENDS mod/*.go tests/wasm/extension-go/*.go
    proto/modlock/wasm/*.go)
  add_custom_command(OUTPUT ${extension_go_wasm}
    COMMAND ${CMAKE_COMMAND} -E env GOOS=wasip1 GOARCH=wasm
      ${GO_EXECUTABLE} build -buildmode=c-shared -o ${extension_go_wasm} ./tests/wasm/extension-go
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS ${extension_go_sources} go.mod)
  add_custom_target(extension_go_wasm DEPENDS ${extension_go_wasm})
  add_executable(wasm_host_test tests/wasm_host_test.cc)
  add_dependencies(wasm_host_test extension_go_wasm)
  target_compile_definitions(wasm_host_test PRIVATE EXTENSION_GO_WASM="${extension_go_wasm}")
  target_link_libraries(wasm_host_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME wasm_host_test COMMAND wasm_host_test)
  add_executable(wasm_ui_test tests/wasm_ui_test.cc src/wasm/ui.cc)
  target_include_directories(wasm_ui_test PRIVATE src)
  target_link_libraries(wasm_ui_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME wasm_ui_test COMMAND wasm_ui_test)
  add_executable(wasm_settings_test tests/wasm_settings_test.cc src/wasm/settings.cc)
  target_include_directories(wasm_settings_test PRIVATE src)
  target_link_libraries(wasm_settings_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME wasm_settings_test COMMAND wasm_settings_test)
  add_executable(wasm_metrics_test tests/wasm_metrics_test.cc src/wasm/metrics.cc)
  target_include_directories(wasm_metrics_test PRIVATE src)
  target_link_libraries(wasm_metrics_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME wasm_metrics_test COMMAND wasm_metrics_test)
  add_executable(wasm_hero_restore_test tests/wasm_hero_restore_test.cc src/wasm/hero_restore.cc)
  target_include_directories(wasm_hero_restore_test PRIVATE src)
  target_link_libraries(wasm_hero_restore_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME wasm_hero_restore_test COMMAND wasm_hero_restore_test)
  add_executable(wasm_npc_restore_test tests/wasm_npc_restore_test.cc src/wasm/npc_restore.cc)
  target_include_directories(wasm_npc_restore_test PRIVATE src)
  target_link_libraries(wasm_npc_restore_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME wasm_npc_restore_test COMMAND wasm_npc_restore_test)
  add_executable(world_text_game_factory_test tests/world_text_game_factory_test.cc)
  target_link_libraries(world_text_game_factory_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME world_text_game_factory_test COMMAND world_text_game_factory_test)
endif()
