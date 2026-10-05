// The Luau runtime runs Luau mods. It is a WASI reactor with the same boundary
// as a compiled mod: the host hands the start event, which carries the mod's
// sources as an uncompressed zip in StartEvent.source, and the runtime runs
// the prelude, which requires the mod's main module. Every event, the start
// event included, then goes to the function the library registers with
// __modlock.handle, which returns the encoded Reply.
//
// Errors unwind as C++ exceptions, so the module needs the WebAssembly
// exception handling proposal.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <string_view>

#include "../boundary.h"
#include "lua.h"
#include "luacode.h"
#include "lualib.h"

namespace {

// prelude defines require over the mod's sources and runs the main module.
constexpr char prelude[] = {
#embed "prelude.luau"
};

// sources maps each path in the mod's zip to its contents.
std::map<std::string, std::string, std::less<>> sources;

// state is the interpreter for the instance's lifetime, and thread the
// sandboxed thread the mod runs on; a reload starts a new instance.
lua_State* state;
lua_State* thread;

// handler references the event function the library registered.
int handler = LUA_NOREF;

// result holds the last encoded Reply until the next event.
std::string result;

// Log writes text to the server log.
void Log(std::string_view text) { host_log(text.data(), text.size()); }

// Keep adds one file of the mod's zip to sources.
int Keep(void*, const char* name, size_t name_size, const uint8_t* data, size_t size) {
  sources.emplace(std::string(name, name_size),
                  std::string(reinterpret_cast<const char*>(data), size));
  return 1;
}

// Traceback is the error handler of every protected call: it appends the
// stack to the error message.
int Traceback(lua_State* L) {
  const char* message = lua_tostring(L, 1);
  std::string text = message ? message : "an error without a message";
  text += '\n';
  text += lua_debugtrace(L);
  lua_pushlstring(L, text.data(), text.size());
  return 1;
}

// Call calls the function below nargs arguments on L with Traceback, and
// logs the error under what when it fails.
bool Call(lua_State* L, int nargs, int nresults, std::string_view what) {
  const int function = lua_gettop(L) - nargs;
  lua_pushcfunction(L, Traceback, "traceback");
  lua_insert(L, function);
  const int status = lua_pcall(L, nargs, nresults, function);
  if (status != LUA_OK) {
    size_t size;
    const char* error = lua_tolstring(L, -1, &size);
    Log(std::string(what) + ": " + std::string(error ? std::string_view(error, size) : "?"));
    lua_pop(L, 1);
  }
  lua_remove(L, function);
  return status == LUA_OK;
}

// HostCall is __modlock.hostCall(call: string): string, which exchanges an
// encoded Call for the host's encoded Reply.
int HostCall(lua_State* L) {
  size_t size;
  const char* request = luaL_checklstring(L, 1, &size);
  uint32_t response_size;
  uint8_t* response = exchange(reinterpret_cast<const uint8_t*>(request), size, &response_size);
  lua_pushlstring(L, reinterpret_cast<const char*>(response), response_size);
  free(response);
  return 1;
}

// HostLog is __modlock.log(text: string).
int HostLog(lua_State* L) {
  size_t size;
  const char* text = luaL_checklstring(L, 1, &size);
  host_log(text, size);
  return 0;
}

// Source is __modlock.source(path: string): string?, the source at path in
// the mod's zip.
int Source(lua_State* L) {
  const auto found = sources.find(std::string_view(luaL_checkstring(L, 1)));
  if (found == sources.end()) return 0;
  lua_pushlstring(L, found->second.data(), found->second.size());
  return 1;
}

// Load is __modlock.load(path: string): (function?, string?), the compiled
// module at path in the mod's zip, or nil and why it does not compile.
int Load(lua_State* L) {
  const std::string path = luaL_checkstring(L, 1);
  const auto found = sources.find(path);
  if (found == sources.end()) {
    lua_pushnil(L);
    lua_pushstring(L, ("the mod has no " + path).c_str());
    return 2;
  }
  lua_CompileOptions options{};
  options.optimizationLevel = 1;
  options.debugLevel = 1;
  size_t size;
  char* bytecode = luau_compile(found->second.data(), found->second.size(), &options, &size);
  const int status = luau_load(L, ("@" + path).c_str(), bytecode, size, 0);
  free(bytecode);
  if (status != 0) {
    lua_pushnil(L);
    lua_insert(L, -2);
    return 2;
  }
  return 1;
}

// Handle is __modlock.handle(event: (call: string) -> string), which
// registers the function every event goes to.
int Handle(lua_State* L) {
  luaL_checktype(L, 1, LUA_TFUNCTION);
  if (handler != LUA_NOREF) lua_unref(L, handler);
  handler = lua_ref(L, 1);
  return 0;
}

// Print is print(...), which logs its arguments separated by tabs.
int Print(lua_State* L) {
  std::string line;
  for (int i = 1; i <= lua_gettop(L); ++i) {
    size_t size;
    const char* text = luaL_tolstring(L, i, &size);
    if (i > 1) line += '\t';
    line.append(text, size);
    lua_pop(L, 1);
  }
  Log(line);
  return 0;
}

// Start reads the mod's sources from the start event, builds the sandbox and
// runs the prelude.
bool Start(const uint8_t* event, size_t size) {
  const uint8_t* zip;
  size_t zip_size;
  if (!start_source(event, size, &zip, &zip_size)) {
    Log("the start event carries no Luau sources");
    return false;
  }
  if (!read_zip(zip, zip_size, Keep, nullptr)) {
    Log("the mod's sources are not a readable uncompressed zip");
    return false;
  }

  // Open the libraries, replace print, then freeze the globals and run the
  // mod on a thread with its own writable globals.
  state = luaL_newstate();
  luaL_openlibs(state);
  lua_pushcfunction(state, Print, "print");
  lua_setglobal(state, "print");
  luaL_sandbox(state);
  thread = lua_newthread(state);
  lua_ref(state, -1);
  lua_pop(state, 1);
  luaL_sandboxthread(thread);

  // Run the prelude with the host bridge as its argument.
  lua_CompileOptions options{};
  options.optimizationLevel = 1;
  options.debugLevel = 1;
  size_t bytecode_size;
  char* bytecode = luau_compile(prelude, sizeof(prelude), &options, &bytecode_size);
  const int status = luau_load(thread, "=prelude", bytecode, bytecode_size, 0);
  free(bytecode);
  if (status != 0) {
    Log(lua_tostring(thread, -1));
    return false;
  }
  static const luaL_Reg bridge[] = {
      {"hostCall", HostCall}, {"log", HostLog},   {"source", Source},
      {"load", Load},         {"handle", Handle}, {nullptr, nullptr},
  };
  lua_newtable(thread);
  luaL_register(thread, nullptr, bridge);
  return Call(thread, 1, 0, "start");
}

}  // namespace

// modlock_event receives one encoded Call of size bytes, hands it to the
// library's handler, and returns the encoded Reply's address in the high 32
// bits and its length in the low 32 bits. A mod that fails to start traps,
// which stops it.
extern "C" __attribute__((export_name("modlock_event"))) uint64_t modlock_event(uint32_t size) {
  // Copy the event out of the host.
  std::string event(size, '\0');
  host_read(event.data(), size);
  const bool starting = !state;
  if (starting && !Start(reinterpret_cast<const uint8_t*>(event.data()), event.size())) {
    __builtin_trap();
  }
  if (handler == LUA_NOREF) {
    if (starting) {
      Log("the mod's library registered no event handler");
      __builtin_trap();
    }
    return 0;
  }

  // Hand it to the handler and keep its answer until the next event.
  lua_getref(thread, handler);
  lua_pushlstring(thread, event.data(), event.size());
  result.clear();
  if (!Call(thread, 1, 1, "event")) {
    if (starting) __builtin_trap();
    return 0;
  }
  size_t answer_size;
  if (const char* answer = lua_tolstring(thread, -1, &answer_size)) {
    result.assign(answer, answer_size);
  }
  lua_pop(thread, 1);
  if (result.empty()) return 0;
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(result.data())) << 32 | result.size();
}
