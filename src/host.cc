#include "modlock/host.h"

namespace modlock::host {

PluginHost::~PluginHost() {
  StopAll();
}

std::unique_ptr<std::string> PluginHost::Register(std::unique_ptr<Plugin> plugin) {
  // Accept a named plugin of this version while registration is open.
  if (!accepting_) {
    return std::make_unique<std::string>("plugin registration is closed");
  }
  if (!plugin) {
    return std::make_unique<std::string>("null plugin");
  }
  if (plugin->InterfaceVersion() != PluginInterfaceVersion) {
    auto error =
        std::make_unique<std::string>(plugin->Name() != nullptr ? plugin->Name() : "<unnamed>");
    *error += ": interface version " + std::to_string(plugin->InterfaceVersion()) +
              " does not match "
              "host version " +
              std::to_string(PluginInterfaceVersion);
    return error;
  }
  const char* name = plugin->Name();
  if (!name || !*name) {
    return std::make_unique<std::string>("plugin name is empty");
  }

  // Names are unique.
  for (const auto& entry : entries_) {
    if (entry.plugin->Name() != nullptr && name != nullptr &&
        std::string(entry.plugin->Name()) == name) {
      auto error = std::make_unique<std::string>(name);
      *error += ": duplicate plugin name";
      return error;
    }
  }
  entries_.push_back(Entry{.plugin = std::move(plugin)});
  return nullptr;
}

bool PluginHost::StartAll(std::string& error) {
  if (stopped_) {
    error = "plugin host has stopped";
    return false;
  }
  accepting_ = false;
  for (auto& entry : entries_) {
    if (entry.state != State::kRegistered) {
      continue;
    }
    entry.state = State::kLoading;
    if (!entry.plugin->Load()) {
      error = std::string(entry.plugin->Name() != nullptr ? entry.plugin->Name() : "<unnamed>") +
              ": Load returned false";
      StopAll();
      return false;
    }
    if (!entry.plugin->Start()) {
      error = std::string(entry.plugin->Name()) + ": Start returned false";
      StopAll();
      return false;
    }
    entry.state = State::kStarted;
  }
  return true;
}

void PluginHost::TickAll() {
  TickContext context;
  TickAll(context);
}

void PluginHost::TickAll(const TickContext& context) {
  for (auto& entry : entries_) {
    if (entry.state != State::kStarted) {
      continue;
    }
    if (auto* contextual = dynamic_cast<TickContextPlugin*>(entry.plugin.get());
        contextual != nullptr) {
      contextual->Tick(context);
    } else {
      entry.plugin->Tick();
    }
    entry.ticks++;
  }
}

void PluginHost::StopAll() {
  accepting_ = false;
  stopped_ = true;
  for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
    if (it->state == State::kStarted || it->state == State::kLoading) {
      it->plugin->Stop();
      it->state = State::kStopped;
    }
  }
}

std::vector<LoadedPlugin> PluginHost::Plugins() const {
  std::vector<LoadedPlugin> out;
  out.reserve(entries_.size());
  for (const auto& entry : entries_) {
    out.push_back(LoadedPlugin{
        .name = entry.plugin->Name() != nullptr ? entry.plugin->Name() : "",
        .interface_version = entry.plugin->InterfaceVersion(),
        .ticks = entry.ticks,
    });
  }
  return out;
}

int PluginHost::ExitCode(int engine_exit) const {
  if (engine_exit != 0) {
    return engine_exit;
  }
  for (const auto& entry : entries_) {
    if (auto result = entry.plugin->ExitCode(); result && *result != 0) {
      return *result;
    }
  }
  return 0;
}

}  // namespace modlock::host
