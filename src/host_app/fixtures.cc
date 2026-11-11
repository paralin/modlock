#include "host_app/fixtures.h"

#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include "src/host_app/control/control.pb.h"

namespace modlock::host_app {

std::expected<std::string, std::string> ProvideFixtures(WasmHost& host, std::string_view data) {
  // Read the replies.
  control::Fixtures fixtures;
  if (!fixtures.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
    return std::unexpected("the fixtures file is malformed");
  }

  // Keep each service's replies by mod and method, and its files by mod.
  struct Service {
    std::map<std::pair<std::string, std::string>, std::string, std::less<>> replies;
    std::map<std::string, std::string, std::less<>> sources;
  };
  std::map<std::string, std::shared_ptr<Service>> services;
  for (const auto& fixture : fixtures.fixtures()) {
    auto& service = services[fixture.service()];
    if (!service) {
      service = std::make_shared<Service>();
    }
    service->replies.insert_or_assign({fixture.mod(), fixture.method()}, fixture.payload());
    service->sources.insert_or_assign(fixture.mod(), fixture.source());
  }

  // Provide each service, answering a mod only from its own fixture.
  std::string lines;
  for (auto& [name, service] : services) {
    for (const auto& [mod, source] : service->sources) {
      lines += mod + ": service " + name + ": fixture " + source + "\n";
    }
    host.Provide(name, [service](std::string_view mod, std::string_view method,
                                 std::string_view) -> std::expected<std::string, std::string> {
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
    });
  }
  return lines;
}

}  // namespace modlock::host_app
