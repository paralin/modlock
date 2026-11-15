#include "host_app/fixtures.h"

#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include "src/host_app/control/control.pb.h"

namespace modlock::host_app {

std::expected<std::string, std::string> ProvideFixtures(WasmHost& host, std::string_view data,
                                                        Forward forward) {
  // Read the replies.
  control::Fixtures fixtures;
  if (!fixtures.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
    return std::unexpected("the fixtures file is malformed");
  }
  if (fixtures.forwards_size() > 0 && !forward) {
    return std::unexpected("the fixtures forward a service, which needs --control");
  }

  // Keep each service's replies by mod and method, its files by mod, and the
  // mods whose calls it forwards with where to.
  struct Service {
    std::map<std::pair<std::string, std::string>, std::string, std::less<>> replies;
    std::map<std::string, std::string, std::less<>> sources;
    std::map<std::string, std::string, std::less<>> forwards;
  };
  std::map<std::string, std::shared_ptr<Service>> services;
  auto named = [&services](const std::string& name) -> Service& {
    auto& service = services[name];
    if (!service) {
      service = std::make_shared<Service>();
    }
    return *service;
  };
  for (const auto& fixture : fixtures.fixtures()) {
    auto& service = named(fixture.service());
    service.replies.insert_or_assign({fixture.mod(), fixture.method()}, fixture.payload());
    service.sources.insert_or_assign(fixture.mod(), fixture.source());
  }
  for (const auto& forwarded : fixtures.forwards()) {
    named(forwarded.service()).forwards.insert_or_assign(forwarded.mod(), forwarded.target());
  }

  // Provide each service, answering a mod only from its own fixture.
  std::string lines;
  for (auto& [name, service] : services) {
    for (const auto& [mod, source] : service->sources) {
      if (!service->forwards.contains(mod)) {
        lines += mod + ": service " + name + ": fixture " + source + "\n";
      }
    }
    for (const auto& [mod, target] : service->forwards) {
      lines += mod + ": service " + name + ": forwarded to " + target + "\n";
    }
    auto answer = [service](std::string_view mod,
                            std::string_view method) -> std::expected<std::string, std::string> {
      const auto source = service->sources.find(mod);
      if (source == service->sources.end()) {
        return std::unexpected("this host has no fixture of this service for " + std::string(mod));
      }
      const auto found = service->replies.find(std::pair(std::string(mod), std::string(method)));
      if (found == service->replies.end()) {
        return std::unexpected("the fixture " + source->second + " has no method " +
                               std::string(method));
      }
      return found->second;
    };

    // A service with no forward answers every call at once.
    if (service->forwards.empty()) {
      host.Provide(name, [answer](std::string_view mod, std::string_view method,
                                  std::string_view) { return answer(mod, method); });
      continue;
    }

    // A forwarded service answers started calls: a forwarded mod's in a later
    // frame, and any other mod's from its fixture at once.
    host.ProvideLater(name, [service, answer, forward, name](std::string_view mod,
                                                             std::string_view method,
                                                             std::string_view payload,
                                                             WasmReply reply) {
      if (service->forwards.contains(mod)) {
        forward(mod, name, method, payload, std::move(reply));
        return;
      }
      reply(answer(mod, method));
    });
  }
  return lines;
}

}  // namespace modlock::host_app
