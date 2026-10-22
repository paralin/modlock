#include "host_app/windows/windows_module_loader.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace modlock::host_app {
namespace {

// Owning handle wrapper for the loader's load reference. The engine and game
// server modules stay mapped until the process terminates: Load pins each
// module with GET_MODULE_HANDLE_EX_FLAG_PIN, so releasing this reference
// cannot unload a module whose code the engine still calls during process
// teardown.
class WindowsLoadedModule final : public LoadedModule {
 public:
  explicit WindowsLoadedModule(HMODULE module) : module_(module) {}
  ~WindowsLoadedModule() override { FreeLibrary(module_); }

 private:
  HMODULE module_;
};

}  // namespace

std::expected<std::unique_ptr<LoadedModule>, std::string> WindowsModuleLoader::Load(
    const std::filesystem::path& path) {
  // LOAD_WITH_ALTERED_SEARCH_PATH makes the loader resolve the module's own
  // dependencies (tier0.dll and friends) from its directory first; plain
  // LoadLibraryW with an absolute path does not search that directory.
  HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (module == nullptr) {
    const DWORD code = GetLastError();
    return std::unexpected("LoadLibraryExW failed with error code " + std::to_string(code));
  }

  // Pin the module to process lifetime (GET_MODULE_HANDLE_EX_FLAG_PIN keeps
  // it loaded until process termination regardless of any FreeLibrary, and
  // FROM_ADDRESS identifies the module by an address inside it). The wrapper
  // below still owns the load reference and releases it at destruction.
  HMODULE pinned = nullptr;
  if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(module), &pinned)) {
    const DWORD code = GetLastError();
    ::FreeLibrary(module);
    return std::unexpected("GetModuleHandleExW pin failed for " + path.string() +
                           " with error code " + std::to_string(code));
  }

  // Release ownership to the caller; the module stays mapped for process
  // lifetime even after the wrapper's reference is released.
  std::unique_ptr<LoadedModule> owned = std::make_unique<WindowsLoadedModule>(module);
  return owned;
}

}  // namespace modlock::host_app
