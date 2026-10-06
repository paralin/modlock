#include "modlock/gameinterop/native_trace.h"

#include "modlock/gameinterop/entity_abi.h"
#include "native_trace_layout.h"

#if defined(_WIN32)
#include <safetyhook.hpp>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <numbers>
#include <type_traits>
#include <utility>

namespace modlock::gameinterop {
namespace {

// Native VPhys2 query layouts stay private to the engine adapter.
// Public inputs and results own their values.
struct alignas(8) Ray {
  std::byte shape[40]{};
  std::uint8_t type = 0;
  std::byte padding[7]{};
};
static_assert(sizeof(Ray) == 48);

struct QueryAttributes {
  std::uint64_t with = 0;
  std::uint64_t exclude = 0;
  std::uint64_t as = 0;
  std::uint32_t entities[2]{UINT32_MAX, UINT32_MAX};
  std::uint32_t owners[2]{UINT32_MAX, UINT32_MAX};
  std::uint16_t hierarchies[2]{};
  std::uint16_t detail_layers = UINT16_MAX;
  std::uint8_t detail = 0;
  std::uint8_t objects = 0x0f;
  std::uint8_t collision = 0;
  std::uint8_t flags = 0x49;
};
static_assert(sizeof(QueryAttributes) == 56);

struct Filter {
  const void* vtable = nullptr;
  QueryAttributes attributes;
  std::uint8_t iterate = 1;
};
static_assert(sizeof(Filter) == 72);
static_assert(offsetof(Filter, iterate) == 0x40);

bool Finite(const TraceVector& value) {
  return std::ranges::all_of(value, [](float component) { return std::isfinite(component); });
}

std::expected<Ray, std::string> BuildRay(const TraceShape& shape) {
  Ray ray;
  ray.type = static_cast<std::uint8_t>(shape.index());
  bool valid = std::visit(
      [&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, TraceMesh>) {
          struct Mesh {
            TraceVector mins;
            TraceVector maxs;
            const TraceVector* vertices;
            std::int32_t count;
          } mesh{value.mins, value.maxs, value.vertices.data(), 0};
          static_assert(sizeof(Mesh) == 40);
          if (value.vertices.empty() || value.vertices.size() > INT32_MAX ||
              !std::ranges::all_of(value.vertices, Finite))
            return false;
          mesh.count = static_cast<std::int32_t>(value.vertices.size());
          std::memcpy(ray.shape, &mesh, sizeof(mesh));
          return Finite(value.mins) && Finite(value.maxs);
        } else {
          static_assert(sizeof(T) <= sizeof(ray.shape));
          std::memcpy(ray.shape, &value, sizeof(value));
          if constexpr (std::is_same_v<T, TraceLine>)
            return Finite(value.offset) && std::isfinite(value.radius) && value.radius >= 0;
          if constexpr (std::is_same_v<T, TraceSphere>)
            return Finite(value.center) && std::isfinite(value.radius) && value.radius >= 0;
          if constexpr (std::is_same_v<T, TraceHull>)
            return Finite(value.mins) && Finite(value.maxs) && value.mins[0] <= value.maxs[0] &&
                   value.mins[1] <= value.maxs[1] && value.mins[2] <= value.maxs[2];
          if constexpr (std::is_same_v<T, TraceCapsule>)
            return Finite(value.center_a) && Finite(value.center_b) &&
                   std::isfinite(value.radius) && value.radius >= 0;
        }
      },
      shape);
  if (!valid) return std::unexpected("trace shape has invalid bounds or coordinates");
  return ray;
}

#if defined(_WIN32)
safetyhook::InlineHook* g_hook = nullptr;
std::atomic<void*> g_physics = nullptr;

void TraceThunk(void* physics, Ray* ray, TraceVector* start, TraceVector* end, Filter* filter,
                GameTrace* result) {
  g_physics.store(physics, std::memory_order_release);
  g_hook->call<void>(physics, ray, start, end, filter, result);
}

void FilterDestructor(Filter*, std::uint8_t) {}

std::uint8_t ShouldHitEntity(Filter* filter, void* entity) {
  if (!entity) return 0;
  const auto handle = ReferenceHandleOf(entity);
  if (!handle) return 1;
  const auto index = *handle & 0x7fff;
  return index != filter->attributes.entities[0] && index != filter->attributes.entities[1];
}

struct FilterVtable {
  void (*destroy)(Filter*, std::uint8_t);
  std::uint8_t (*should_hit)(Filter*, void*);
};
const FilterVtable kFilterVtable{FilterDestructor, ShouldHitEntity};
#endif

}  // namespace

struct NativeTrace::Impl {
#if defined(_WIN32)
  safetyhook::InlineHook hook;
  ~Impl() {
    if (g_hook == &hook) {
      hook.reset();
      g_hook = nullptr;
      g_physics.store(nullptr, std::memory_order_release);
    }
  }
#endif
};

NativeTrace::NativeTrace(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
NativeTrace::NativeTrace(NativeTrace&&) noexcept = default;
NativeTrace& NativeTrace::operator=(NativeTrace&&) noexcept = default;
NativeTrace::~NativeTrace() = default;

std::expected<NativeTrace, std::string> NativeTrace::Install(const ModuleImage& server) {
#if defined(_WIN32)
  if (g_hook) return std::unexpected("native trace is already installed");
  auto target = ResolveSignature(server, "physics.trace-shape");
  if (!target) return std::unexpected(target.error());
  auto hook = safetyhook::create_inline(*target, reinterpret_cast<void*>(&TraceThunk),
                                        safetyhook::InlineHook::StartDisabled);
  if (!hook) return std::unexpected("native trace hook creation failed");
  auto impl = std::make_unique<Impl>();
  impl->hook = std::move(hook);
  g_hook = &impl->hook;
  if (auto enabled = impl->hook.enable(); !enabled)
    return std::unexpected("native trace hook activation failed");
  return NativeTrace(std::move(impl));
#else
  (void)server;
  return std::unexpected("native tracing requires the Windows host");
#endif
}

bool NativeTrace::IsReady() const {
#if defined(_WIN32)
  return impl_ && g_physics.load(std::memory_order_acquire) != nullptr;
#else
  return false;
#endif
}

std::expected<TraceResult, std::string> NativeTrace::Query(const TraceVector& start,
                                                           const TraceVector& end,
                                                           const TraceShape& shape,
                                                           const TraceOptions& options) const {
  if (!Finite(start) || !Finite(end)) return std::unexpected("trace endpoints are not finite");
  auto ray = BuildRay(shape);
  if (!ray) return std::unexpected(ray.error());
#if defined(_WIN32)
  void* physics = g_physics.load(std::memory_order_acquire);
  if (!impl_ || !physics) return std::unexpected("native physics query is not ready");
  Filter filter;
  filter.vtable = &kFilterVtable;
  auto& attributes = filter.attributes;
  attributes.with = options.interacts_with;
  attributes.exclude = options.interacts_exclude;
  attributes.as = options.interacts_as;
  for (size_t i = 0; i < 2; ++i) {
    attributes.entities[i] = options.ignored_entities[i] == UINT32_MAX
                                 ? UINT32_MAX
                                 : options.ignored_entities[i] & 0x7fff;
    attributes.owners[i] =
        options.ignored_owners[i] == UINT32_MAX ? UINT32_MAX : options.ignored_owners[i] & 0x7fff;
    attributes.hierarchies[i] = options.ignored_hierarchies[i];
  }
  attributes.detail_layers = options.included_detail_layers;
  attributes.detail = options.target_detail_layer;
  attributes.objects = options.object_set;
  attributes.collision = options.collision_group;
  attributes.flags = 0x40 | static_cast<unsigned>(options.hit_solid) |
                     (options.require_contacts << 1) | (options.hit_trigger << 2) |
                     (options.ignore_disabled_pairs << 3) | (options.ignore_shared_hitboxes << 4) |
                     (options.force_hit_everything << 5);
  filter.iterate = options.iterate_entities;
  GameTrace native;
  const float one = 1;
  std::memcpy(native.transform + 12, &one, sizeof(one));
  auto from = start;
  auto to = end;
  impl_->hook.call<void>(physics, &*ray, &from, &to, &filter, &native);
  if (!std::isfinite(native.fraction) || native.fraction < 0 || native.fraction > 1 ||
      !Finite(native.normal))
    return std::unexpected("native trace returned invalid hit data");
  auto result = ReadGameTrace(native);
  for (size_t i = 0; i < 3; ++i)
    result.position[i] = start[i] + (end[i] - start[i]) * native.fraction;
  return result;
#else
  (void)options;
  return std::unexpected("native tracing requires the Windows host");
#endif
}

std::expected<TraceResult, std::string> NativeTrace::RayAngles(const TraceVector& start,
                                                               const TraceVector& angles,
                                                               float distance,
                                                               const TraceOptions& options) const {
  if (!Finite(angles) || !std::isfinite(distance) || distance <= 0)
    return std::unexpected("trace angles or distance are invalid");
  const float pitch = angles[0] * std::numbers::pi_v<float> / 180;
  const float yaw = angles[1] * std::numbers::pi_v<float> / 180;
  const TraceVector end{start[0] + std::cos(pitch) * std::cos(yaw) * distance,
                        start[1] + std::cos(pitch) * std::sin(yaw) * distance,
                        start[2] - std::sin(pitch) * distance};
  return Query(start, end, TraceLine{}, options);
}

void NativeTrace::InvalidateAfterEngineReset() {
#if defined(_WIN32)
  if (impl_) g_physics.store(nullptr, std::memory_order_release);
#endif
}

}  // namespace modlock::gameinterop
