#include "modlock/plugin_library.h"

#include <cstring>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace modlock {
namespace {

// LibraryHandle releases a plugin image after all objects using it are gone.
class LibraryHandle {
 public:
#if defined(_WIN32)
  using Handle = HMODULE;
#else
  using Handle = void*;
#endif
  explicit LibraryHandle(Handle handle) : handle_(handle) {}
  ~LibraryHandle() {
#if defined(_WIN32)
    FreeLibrary(handle_);
#else
    dlclose(handle_);
#endif
  }

  LibraryHandle(const LibraryHandle&) = delete;
  LibraryHandle& operator=(const LibraryHandle&) = delete;

  template <typename Function>
  Function Symbol(const char* name) const {
#if defined(_WIN32)
    return reinterpret_cast<Function>(GetProcAddress(handle_, name));
#else
    return reinterpret_cast<Function>(dlsym(handle_, name));
#endif
  }

 private:
  Handle handle_;
};

// LibraryPlugin couples instance destruction to its creating image's lifetime.
class LibraryPlugin final : public Plugin, public TickContextPlugin {
 public:
  using Destroy = void (*)(Plugin*);
  LibraryPlugin(std::unique_ptr<LibraryHandle> library, Plugin* plugin, Destroy destroy)
      : library_(std::move(library)), plugin_(plugin), destroy_(destroy) {}
  ~LibraryPlugin() override { destroy_(plugin_); }

  uint32_t InterfaceVersion() const override { return plugin_->InterfaceVersion(); }
  const char* Name() const override { return plugin_->Name(); }
  bool Load() override { return plugin_->Load(); }
  bool Start() override { return plugin_->Start(); }
  void Tick() override { plugin_->Tick(); }
  void Tick(const TickContext& context) override {
    if (auto* contextual = dynamic_cast<TickContextPlugin*>(plugin_)) {
      contextual->Tick(context);
      return;
    }
    plugin_->Tick();
  }
  void Stop() override { plugin_->Stop(); }
  std::optional<int> ExitCode() const override { return plugin_->ExitCode(); }

 private:
  std::unique_ptr<LibraryHandle> library_;
  // plugin_ is released exclusively through its originating library.
  Plugin* plugin_;
  Destroy destroy_;
};

}  // namespace

std::expected<std::unique_ptr<Plugin>, std::string> LoadPluginLibrary(
    const std::filesystem::path& path, const PluginContext& context) {
  // Only an absolute path loads.
  if (!path.is_absolute()) {
    return std::unexpected("plugin path must be absolute: " + path.string());
  }

#if defined(_WIN32)
  // Load the library beside its own dependencies.
  auto handle = LoadLibraryExW(path.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!handle) {
    return std::unexpected(path.string() + ": LoadLibraryExW error " +
                           std::to_string(GetLastError()));
  }

#else
  // Other platforms load it with dlopen.
  auto handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    return std::unexpected(path.string() + ": " + dlerror());
  }
#endif

  // Find the versioned entry points.
  auto library = std::make_unique<LibraryHandle>(handle);
  auto describe = library->Symbol<PluginManifest (*)()>("ModlockPluginManifest_v1");
  auto create = library->Symbol<Plugin* (*)(const PluginContext*)>("ModlockPluginCreate_v1");
  auto destroy = library->Symbol<LibraryPlugin::Destroy>("ModlockPluginDestroy_v1");
  if (!describe || !create || !destroy) {
    return std::unexpected(path.string() + ": missing versioned plugin entry point");
  }

  // The manifest must match this SDK before the plugin is created.
  const auto manifest = describe();
  if (manifest.interface_version != PluginInterfaceVersion || !manifest.sdk_abi ||
      std::strcmp(manifest.sdk_abi, MODLOCK_SDK_ABI) != 0) {
    return std::unexpected(path.string() + ": incompatible Modlock SDK or C++ runtime");
  }
  auto* plugin = create(&context);
  if (!plugin) {
    return std::unexpected(path.string() + ": plugin creation failed");
  }
  return std::make_unique<LibraryPlugin>(std::move(library), plugin, destroy);
}

}  // namespace modlock
