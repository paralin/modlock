#include <iostream>
#include <string_view>

#include "modlock/engine_host.h"
#include "modlock/gameinterop/engine_server.h"
#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/plugin_library.h"
#include "modlock/render/world_text_game_factory.h"

namespace {

// HelloPlugin creates a native entity and removes it before shutdown.
// Pass --reload to exercise the same operation across a world replacement.
// The host's explicit check mode exercises only library and plugin lifetime.
class HelloPlugin final : public modlock::Plugin {
 public:
  explicit HelloPlugin(const modlock::PluginContext& context)
      : check_only_(context.check_only), engine_(context.engine) {
    for (int i = 1; i < context.argc; ++i) {
      if (std::string_view(context.argv[i]) == "--reload") {
        reload_ = true;
      }
    }
  }
  ~HelloPlugin() override { std::cout << "hello: destroyed\n"; }
  uint32_t InterfaceVersion() const override { return modlock::PluginInterfaceVersion; }
  const char* Name() const override { return "hello"; }
  bool Load() override {
    std::cout << std::unitbuf;
    std::cout << "hello: loaded\n";
    return true;
  }
  bool Start() override {
    std::cout << "hello: started\n";
#if defined(SAMPLE_FAIL_START)
    return false;
#else
    if (!check_only_) {
      auto world = engine_->OnWorld({}, [this](std::string_view) {
        // Each world starts unfinished and asks to keep simulating.
        ++worlds_;
        finished_ = false;
        observed_ = false;
        auto server = modlock::gameinterop::EngineServer::Resolve();
        if (server && server->ServerCommand("sv_hibernate_when_empty 0\n")) {
          std::cout << "hello: native world started, requested simulation\n";
        }
      });
      if (!world) {
        std::cout << "hello: " << world.error() << '\n';
        return false;
      }
      world_ = std::move(*world);
      auto ending = engine_->OnWorldEnding([this] {
        if (text_) {
          text_->Remove();
          text_.reset();
          ++removed_;
          std::cout << "hello: native entity removed before shutdown\n";
        }
        auto server = modlock::gameinterop::EngineServer::Resolve();
        if (server && server->ReadClock()) {
          std::cout << "hello: world cleanup before shutdown\n";
        }
      });
      if (!ending) {
        return false;
      }
      world_.Add(std::move(*ending));
    }
    return true;
#endif
  }
  void Tick() override {
    // A finished or check-only run does nothing more.
    if (finished_) {
      return;
    }
    if (check_only_) {
      finished_ = true;
      std::cout << "hello: lifecycle tick\n";
      return;
    }

    // Wait for the server and its clock.
    auto server = modlock::gameinterop::EngineServer::Resolve();
    if (!server) {
      if (!observed_) {
        std::cout << "hello: " << server.error() << '\n';
      }
      observed_ = true;
      return;
    }
    auto clock = server->ReadClock();
    if (!clock) {
      if (!observed_) {
        std::cout << "hello: " << clock.error() << '\n';
      }
      observed_ = true;
      return;
    }

    // Report the first observed tick.
    if (!observed_) {
      std::cout << "hello: observed native tick " << clock->tick << '\n';
    }
    observed_ = true;

    // Create the sample's world text.
    auto image = modlock::gameinterop::MappedModuleImage::ForModule(L"server.dll");
    auto entities = modlock::gameinterop::ResolveLiveEntitySystem();
    if (image && entities) {
      auto factory = modlock::render::WorldTextGameFactory::TryCreate(*image);
      if (factory) {
        factory_ = std::move(*factory);
        factory_->SetEntitySystem(*entities);
        modlock::Vec3 origin;
        origin.set_z(200);
        auto text = factory_->Create("Modlock sample", origin, {}, {});
        if (text) {
          text_ = std::move(*text);
          std::cout << "hello: native entity created\n";
        } else {
          std::cout << "hello: " << text.error() << '\n';
        }
      } else {
        std::cout << "hello: " << factory.error() << '\n';
      }
    }

    // Without the text, fail and quit.
    if (!text_) {
      failed_ = true;
      finished_ = server->ServerCommand("quit\n");
      return;
    }

    // Echo a command, then reload once or quit.
    if (!server->ServerCommand("echo modlock_sample_native_operation\n")) {
      return;
    }
    std::cout << "hello: native frame " << clock->tick << ", command accepted\n";
    finished_ =
        server->ServerCommand(reload_ && worlds_ == 1 ? "changelevel dl_midtown\n" : "quit\n");
  }
  void Stop() override {
    if (text_) {
      text_->InvalidateAfterEngineReset();
      text_.reset();
    }
    world_.Reset();
    std::cout << "hello: stopped\n";
  }

  std::optional<int> ExitCode() const override {
    return !check_only_ && (failed_ || removed_ != (reload_ ? 2 : 1)) ? 1 : 0;
  }

 private:
  bool check_only_;
  modlock::EngineHost* engine_;
  modlock::Subscription world_;
  std::unique_ptr<modlock::render::WorldTextGameFactory> factory_;
  std::unique_ptr<modlock::render::WorldTextEntity> text_;
  bool failed_ = false;
  bool reload_ = false;
  int worlds_ = 0;
  int removed_ = 0;
  bool finished_ = false;
  bool observed_ = false;
};

}  // namespace

MODLOCK_PLUGIN_EXPORT modlock::PluginManifest ModlockPluginManifest_v1() {
#if defined(SAMPLE_ABI_MISMATCH)
  return {modlock::PluginInterfaceVersion, "incompatible-test-sdk"};
#else
  return {modlock::PluginInterfaceVersion, MODLOCK_SDK_ABI};
#endif
}

MODLOCK_PLUGIN_EXPORT modlock::Plugin* ModlockPluginCreate_v1(
    const modlock::PluginContext* context) {
  if (!context) {
    return nullptr;
  }
  return new HelloPlugin(*context);
}

MODLOCK_PLUGIN_EXPORT void ModlockPluginDestroy_v1(modlock::Plugin* plugin) {
  delete plugin;
}
