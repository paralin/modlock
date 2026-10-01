cmake_minimum_required(VERSION 3.24)
project(Modlock VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
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


add_library(modlock_sdk SHARED
  src/camera/camera_path.cc
  src/engine_host.cc
  src/plugin_library.cc
  src/gameinterop/ability_definitions.cc
  src/gameinterop/ability_input_hook.cc
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
  src/render/world_effect_game_factory.cc
  src/render/world_text_game_factory.cc
  proto/modlock/camera.pb.cc
  proto/modlock/chat.pb.cc
  proto/modlock/hud.pb.cc
  proto/modlock/stamina_consumed.pb.cc
  proto/modlock/types.pb.cc
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
target_link_libraries(modlock_sdk PUBLIC protobuf::libprotobuf)
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
    src/host_app/windows/vconsole_port.cc
    src/host_app/windows/windows_module_loader.cc
    third_party/safetyhook/safetyhook.cpp
    third_party/safetyhook/Zydis.c)
  target_include_directories(modlock_sdk SYSTEM PRIVATE third_party/safetyhook)
  target_compile_definitions(modlock_sdk PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(modlock_sdk PRIVATE ws2_32 psapi)
else()
  target_sources(modlock_sdk PRIVATE src/gameinterop/unsupported/mapped_module_image.cc)
endif()

target_link_libraries(modlock_sdk PRIVATE ${CMAKE_DL_LIBS})
add_executable(modlock-host src/host_app/main.cc)
target_link_libraries(modlock-host PRIVATE modlock::sdk)
install(TARGETS modlock-host RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

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
  add_executable(stdio_guard_test tests/stdio_guard_test.cc)
  target_link_libraries(stdio_guard_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME stdio_guard_test COMMAND stdio_guard_test)
  add_executable(world_effect_game_factory_test tests/world_effect_game_factory_test.cc)
  target_link_libraries(world_effect_game_factory_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME world_effect_game_factory_test COMMAND world_effect_game_factory_test)
  add_executable(world_text_game_factory_test tests/world_text_game_factory_test.cc)
  target_link_libraries(world_text_game_factory_test PRIVATE modlock::sdk gtest_main)
  add_test(NAME world_text_game_factory_test COMMAND world_text_game_factory_test)
endif()
