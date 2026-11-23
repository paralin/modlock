//go:build deps_only

#include "modlock/wasm_host.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

#include "wasm/wasm_plugin.h"

namespace modlock {

using wasm::Mods;
using wasm::ReadBuild;
using wasm::WasmPlugin;

namespace {

// Preparer compiles builds' modules on its own thread, one at a time, and
// keeps the newest prepared module of each mod, so a later load of that build
// finds it compiled. Every method but the worker's runs on the engine thread.
class Preparer {
 public:
  Preparer(wasm::Runtime& runtime, std::filesystem::path interpreters)
      : runtime_(runtime),
        interpreters_(std::move(interpreters)),
        worker_([this](std::stop_token stop) { Run(stop); }) {}

  // Prepare reports whether the build at path is compiled, queuing its
  // compile the first time it is asked. A failure is reported once.
  std::expected<bool, std::string> Prepare(const std::filesystem::path& path) {
    std::lock_guard lock(mu_);
    auto [found, added] = builds_.try_emplace(path);
    if (added) {
      queue_.push_back(path);
      wake_.notify_one();
      return false;
    }
    if (found->second.error) {
      auto error = std::move(*found->second.error);
      builds_.erase(found);
      return std::unexpected(std::move(error));
    }
    return found->second.module != nullptr;
  }

 private:
  // Build is one prepared build: compiling until module or error is set.
  struct Build {
    std::string name;
    std::shared_ptr<const wasmtime::Module> module;
    std::optional<std::string> error;
  };

  // Run compiles each queued build until the host stops. A compile in
  // progress finishes first, since Wasmtime cannot abandon one.
  void Run(std::stop_token stop) {
    while (true) {
      std::filesystem::path path;
      {
        std::unique_lock lock(mu_);
        if (!wake_.wait(lock, stop, [this] { return !queue_.empty(); })) {
          return;
        }
        path = std::move(queue_.front());
        queue_.pop_front();
      }

      // Read and compile the build outside the lock.
      Build build;
      if (auto read = wasm::ReadBuild(path, interpreters_); !read) {
        build.error = read.error();
      } else if (auto compiled = runtime_.Compile(read->module); !compiled) {
        build.name = read->name;
        build.error = read->name + ": " + compiled.error();
      } else {
        build.name = read->name;
        build.module = std::move(*compiled);
      }

      // Keep only the newest prepared build of each mod.
      std::lock_guard lock(mu_);
      if (build.module) {
        std::erase_if(builds_, [&](const auto& entry) {
          return entry.first != path && entry.second.name == build.name && entry.second.module;
        });
      }
      builds_.insert_or_assign(path, std::move(build));
    }
  }

  wasm::Runtime& runtime_;
  const std::filesystem::path interpreters_;
  std::mutex mu_;
  std::condition_variable_any wake_;
  std::map<std::filesystem::path, Build> builds_;
  std::deque<std::filesystem::path> queue_;
  // worker_ is last, so it stops before the state it uses is destroyed.
  std::jthread worker_;
};

}  // namespace

// Impl holds the host's mods and the builds compiling for them. The preparer
// is destroyed before the runtime it compiles with.
struct WasmHost::Impl : Mods {
  std::optional<Preparer> preparer;
};

WasmHost::WasmHost(std::filesystem::path interpreters) : impl_(std::make_unique<Impl>()) {
  impl_->interpreters = std::move(interpreters);

  // Report each change of the wanted controllers to the observers.
  impl_->game.Controllers().Listen([impl = impl_.get()](const auto& wants) {
    impl->Notify([&](WasmHostObserver& observer) { observer.ControllersWanted(wants); });
  });
}

WasmHost::~WasmHost() = default;

void WasmHost::Observe(WasmHostObserver* observer) {
  if (std::ranges::find(impl_->observers, observer) == impl_->observers.end()) {
    impl_->observers.push_back(observer);
  }
}

void WasmHost::Unobserve(WasmHostObserver* observer) {
  std::erase(impl_->observers, observer);
}

std::expected<std::unique_ptr<Plugin>, std::string> WasmHost::Load(
    const std::filesystem::path& path, const PluginContext& context, std::string_view name) {
  // Read the build.
  auto build = ReadBuild(path, impl_->interpreters);
  if (!build) {
    return std::unexpected(build.error());
  }
  std::string instance_name = name.empty() ? build->name : std::string(name);
  if (impl_->plugins.contains(instance_name)) {
    return std::unexpected(path.string() + ": a mod named " + instance_name + " is already loaded");
  }

  // Load the mod into a sandbox that answers through the plugin.
  auto plugin = std::make_unique<WasmPlugin>(*impl_, instance_name, build->name, context);
  auto instance = plugin->Instantiate(build->module);
  if (!instance) {
    return std::unexpected(instance_name + ": " + instance.error());
  }
  impl_->plugins.emplace(std::move(instance_name), plugin.get());
  plugin->Attach(std::move(*instance), std::move(*build));
  return plugin;
}

std::expected<void, std::string> WasmHost::Reload(const std::filesystem::path& path) {
  // Read the build and reload the mod of that name.
  auto build = ReadBuild(path, impl_->interpreters);
  if (!build) {
    return std::unexpected(build.error());
  }
  auto found = impl_->plugins.find(build->name);
  if (found == impl_->plugins.end()) {
    return std::unexpected("no running mod is named " + build->name);
  }
  return found->second->Reload(std::move(*build));
}

std::expected<bool, std::string> WasmHost::Prepare(const std::filesystem::path& path) {
  // The worker starts with the first build a host prepares.
  if (!impl_->preparer) {
    impl_->preparer.emplace(impl_->runtime, impl_->interpreters);
  }
  return impl_->preparer->Prepare(path);
}

bool WasmHost::Held(std::string_view mod) const {
  const auto found = impl_->plugins.find(mod);
  return found != impl_->plugins.end() && found->second->Held();
}

std::expected<void, std::string> WasmHost::Restart(std::string_view mod, bool force) {
  auto found = impl_->plugins.find(mod);
  if (found == impl_->plugins.end()) {
    return std::unexpected("no running mod is named " + std::string(mod));
  }
  found->second->RequestRestart(force);
  return {};
}

bool WasmHost::Restarting(std::string_view mod) const {
  const auto found = impl_->plugins.find(mod);
  return found != impl_->plugins.end() && found->second->Restarting();
}

std::optional<uint32_t> WasmHost::BoardVersion(std::string_view mod, std::string_view board) const {
  const auto found = impl_->plugins.find(mod);
  if (found == impl_->plugins.end()) {
    return std::nullopt;
  }
  return found->second->BoardVersion(board);
}

std::expected<void, std::string> WasmHost::Scope(std::string_view mod, WasmScope scope) {
  auto found = impl_->plugins.find(mod);
  if (found == impl_->plugins.end()) {
    return std::unexpected("no running mod is named " + std::string(mod));
  }

  // One limited mod at a time changes the world.
  if (scope.players && scope.world) {
    for (const auto& [name, plugin] : impl_->plugins) {
      if (name != mod && plugin->Scoped().Limited() && plugin->Scoped().World()) {
        return std::unexpected("the mod " + name + " already changes the world");
      }
    }
  }
  found->second->Scope(std::move(scope));
  return {};
}

void WasmHost::Provide(std::string service, WasmExtension extension) {
  if (extension) {
    impl_->extensions.insert_or_assign(std::move(service), std::move(extension));
  } else {
    impl_->extensions.erase(service);
  }
}

void WasmHost::ProvideLater(std::string service, WasmLateExtension extension) {
  if (extension) {
    impl_->late_extensions.insert_or_assign(std::move(service), std::move(extension));
  } else {
    impl_->late_extensions.erase(service);
  }
}

void WasmHost::KeepSettings(WasmSettings* settings) {
  impl_->settings = settings ? settings : &impl_->kept;
}

void WasmHost::KeepMetrics(WasmMetrics* metrics) {
  impl_->metrics = metrics;
}

void WasmHost::KeepFiles(std::filesystem::path dir) {
  impl_->files = std::move(dir);
}

void WasmHost::SettingChanged(std::string_view mod, uint64_t steam_id, std::string_view key,
                              std::string_view value) {
  if (auto found = impl_->plugins.find(mod); found != impl_->plugins.end()) {
    found->second->SettingChanged(steam_id, key, value);
  }
}

std::expected<void, std::string> WasmHost::AssignTeams(std::string_view mod,
                                                       const wasm::TeamAssignments& request) {
  // The instance owns its seating for as long as it runs.
  const auto found = impl_->plugins.find(mod);
  if (found == impl_->plugins.end()) {
    return std::unexpected("no running mod is named " + std::string(mod));
  }
  return found->second->AssignTeams(request);
}

void WasmHost::SetRoles(wasm::players::RolesTable roles) {
  auto& current = impl_->roles[roles.mod()];
  if (roles.SerializeAsString() == current.SerializeAsString()) {
    return;
  }
  current = std::move(roles);
  ++impl_->roles_revision;
}

void WasmHost::ApplyControllers(
    int32_t slot,
    const google::protobuf::RepeatedPtrField<wasm::controllers::ControllerFrame>& frames,
    bool ended) {
  impl_->game.Controllers().Apply(slot, frames, ended, wasm::ControllerTable::Clock::now());
}

std::expected<std::string, std::string> WasmHost::Call(std::string_view mod,
                                                       std::string_view service,
                                                       std::string_view method,
                                                       std::string_view payload) {
  // Find the mod and call its service.
  auto found = impl_->plugins.find(mod);
  if (found == impl_->plugins.end()) {
    return std::unexpected("no running mod is named " + std::string(mod));
  }
  wasm::ServiceCall call;
  call.set_service(std::string(service));
  call.set_method(std::string(method));
  call.set_payload(std::string(payload));
  return found->second->Serve(call);
}

}  // namespace modlock
