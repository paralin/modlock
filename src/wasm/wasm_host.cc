//go:build deps_only

#include "modlock/wasm_host.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "wasm/wasm_plugin.h"

namespace modlock {
namespace {

using wasm::Build;
using wasm::Mods;
using wasm::ReadBuild;
using wasm::Tune;
using wasm::WasmPlugin;

// ClientMod runs the part of a mod that lives in a player's game: the
// manifest's ability tuning and its movement model's prediction. The server
// runs the mod itself, so the player's game runs no sandbox.
class ClientMod final : public Plugin {
 public:
  explicit ClientMod(Build build) : build_(std::move(build)) {}

  uint32_t InterfaceVersion() const override { return PluginInterfaceVersion; }

  const char* Name() const override { return build_.name.c_str(); }

  bool Start() override {
    // Only a QuakeWorld mod predicts movement.
    const auto& movement = build_.manifest.movement();
    if (movement.model() != wasm::manifest::Movement::MODEL_QUAKEWORLD) {
      return true;
    }

    // Build the client's movement model.
    auto created = quake::PlayerMovement::Create(quake::PlayerMovement::Side::kClient,
                                                 build_.collision, movement.scale());
    if (!created) {
      Log("movement prediction is unavailable: " + created.error());
      return true;
    }
    movement_ = std::move(*created);

    // Step each predicted command through it.
    auto hook = gameinterop::MovementHook::Install(
        gameinterop::MovementHook::Module::kClient, [this](gameinterop::MovementCall& call) {
          // The client's ability data loads before its first predicted command.
          Tick();
          for (auto& state : call.buttons) {
            state &= ~build_.manifest.movement().unpredicted_buttons();
          }
          return movement_->Step(call);
        });
    if (!hook) {
      Log("movement prediction is unavailable: " + hook.error());
      movement_.reset();
      return true;
    }
    hook_ = std::move(*hook);
    return true;
  }

  void Tick() override {
    if (!tuned_) {
      tuned_ = Tune(L"client.dll", "client.dll", build_.manifest, Log);
    }
  }

  void Stop() override {
    hook_.reset();
    movement_.reset();
  }

 private:
  static void Log(std::string_view text) { std::cerr << "client mod: " << text << '\n'; }

  Build build_;
  bool tuned_ = false;
  // hook_ runs movement_ in the client's prediction; it goes first.
  std::optional<quake::PlayerMovement> movement_;
  std::optional<gameinterop::MovementHook> hook_;
};

}  // namespace

// Impl holds the host's mods.
struct WasmHost::Impl : Mods {};

WasmHost::WasmHost(std::filesystem::path interpreters) : impl_(std::make_unique<Impl>()) {
  impl_->interpreters = std::move(interpreters);
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
  // Read the build; a client launch runs it as a client mod.
  auto build = ReadBuild(path, impl_->interpreters);
  if (!build) {
    return std::unexpected(build.error());
  }
  if (context.launch != nullptr && !context.launch->connect.empty()) {
    return std::make_unique<ClientMod>(std::move(*build));
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

bool WasmHost::Held(std::string_view mod) const {
  const auto found = impl_->plugins.find(mod);
  return found != impl_->plugins.end() && found->second->Held();
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

void WasmHost::SetRoles(wasm::players::RolesTable roles) {
  auto& current = impl_->roles[roles.mod()];
  if (roles.SerializeAsString() == current.SerializeAsString()) {
    return;
  }
  current = std::move(roles);
  ++impl_->roles_revision;
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
