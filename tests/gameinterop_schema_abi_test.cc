// Contract tests for the live schema ABI: the type scope's FindDeclaredClass
// consumes (type_scope, out handle, class name) because SchemaMetaInfoHandle_t
// carries user-provided constructors and cannot return whole in RAX - the
// host9 access violation wrote through a name pointer passed in its place.
// A second fixture pins the SchemaClassInfoData_t field-walk offsets with an
// independently encoded layout.
#include <array>
#include <cstring>
#include <string>

#include "gtest/gtest.h"
#include "modlock/gameinterop/entity_abi.h"

namespace {

using modlock::gameinterop::SchemaField;

TEST(NotifyEntityStateChanged, PublishesFullChangeThroughTheCurrentEntityAbi) {
  struct Entity {
    void** vtable;
    bool notified = false;
  };
  std::array<void*, 29> table{};
  // The installed server's old slot returns RequiredEdictIndex through RDX.
  table[27] = reinterpret_cast<void*>(+[](void*, void* result) {
    const int32_t invalid_index = -1;
    std::memcpy(result, &invalid_index, sizeof(invalid_index));
  });
  Entity entity{table.data()};
  EXPECT_FALSE(modlock::gameinterop::CanNotifyEntityStateChanged(&entity));
  EXPECT_FALSE(modlock::gameinterop::NotifyEntityStateChanged(&entity));
  table[28] = reinterpret_cast<void*>(+[](void* self, const void* changed) {
    uint32_t change_type = 1;
    std::memcpy(&change_type, changed, sizeof(change_type));
    EXPECT_EQ(change_type, 0u);
    static_cast<Entity*>(self)->notified = true;
  });
  EXPECT_TRUE(modlock::gameinterop::CanNotifyEntityStateChanged(&entity));
  EXPECT_TRUE(modlock::gameinterop::NotifyEntityStateChanged(&entity));
  EXPECT_TRUE(entity.notified);
}

struct FakeTypeScope {
  void* vtable;
};

// RecordedFindDeclaredClass is the live-ABI stand-in: it receives
// (this, out, name) and stores what it was handed before writing the result.
struct FindCall {
  void* this_ptr = nullptr;
  void* out = nullptr;
  const char* name = nullptr;
  void* result = nullptr;

  void Invoke(void* self, void* out_handle, const char* class_name) {
    this_ptr = self;
    out = out_handle;
    name = class_name;
    *static_cast<void**>(out_handle) = result;
  }
};

FindCall g_find_call;

void FindDeclaredClassEntry(void* self, void* out_handle, const char* class_name) {
  g_find_call.Invoke(self, out_handle, class_name);
}

TEST(FindDeclaredClassOf, UsesTheHiddenReturnAbiAndReturnsTheOutSlot) {
  alignas(16) void* table[3];
  std::memset(table, 0, sizeof(table));
  table[modlock::gameinterop::kTypeScopeFindDeclaredClassSlot] =
      reinterpret_cast<void*>(&FindDeclaredClassEntry);
  FakeTypeScope type_scope{table};

  void* class_info = reinterpret_cast<void*>(0x12345678);
  g_find_call.result = class_info;
  auto resolved = modlock::gameinterop::FindDeclaredClassOf(&type_scope, "CBasePlayerController");

  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved.value(), class_info);
  // The hidden-return ABI: out slot in argument two, class name in argument
  // three. The pre-fix call shape passed (this, name, null), so these
  // assertions fail against it.
  EXPECT_EQ(g_find_call.this_ptr, &type_scope);
  ASSERT_NE(g_find_call.out, nullptr);
  EXPECT_EQ(*static_cast<void**>(g_find_call.out), class_info)
      << "the out slot did not point at the caller's storage";
  EXPECT_STREQ(g_find_call.name, "CBasePlayerController");
}

TEST(FindDeclaredClassOf, NullHandleReportsANamedErrorWithoutDereference) {
  alignas(16) void* table[3];
  std::memset(table, 0, sizeof(table));
  table[modlock::gameinterop::kTypeScopeFindDeclaredClassSlot] =
      reinterpret_cast<void*>(&FindDeclaredClassEntry);
  FakeTypeScope type_scope{table};

  g_find_call.result = nullptr;
  auto resolved = modlock::gameinterop::FindDeclaredClassOf(&type_scope, "CBasePlayerWeapon");
  ASSERT_FALSE(resolved.has_value());
  EXPECT_NE(resolved.error().find("CBasePlayerWeapon"), std::string::npos);
}

TEST(FindDeclaredClassOf, RejectsAnUnresolvedTypeScope) {
  auto resolved = modlock::gameinterop::FindDeclaredClassOf(nullptr, "any");
  ASSERT_FALSE(resolved.has_value());
  EXPECT_FALSE(resolved.error().empty());
}

// Independently encoded SchemaClassInfoData_t / SchemaClassFieldData_t layout
// (sourcesdk public/schemasystem/schematypes.h:330-397). Drift between these
// constants and the production mirror fails resolution here.
constexpr size_t kInfoFields = 0x30;
constexpr size_t kFieldStride = 32;
constexpr size_t kFieldNameOffset = 0x00;
constexpr size_t kFieldOffsetField = 0x10;
constexpr size_t kFieldMetadataCount = 0x14;
constexpr size_t kFieldMetadata = 0x18;
constexpr size_t kMetadataStride = 16;

struct LayoutBuffers {
  std::array<unsigned char, 0x40> info{};
  std::array<unsigned char, 2 * kFieldStride> fields{};
  std::array<unsigned char, kMetadataStride> metadata{};
};

FakeTypeScope* g_persistent_scope = nullptr;
void* g_class_info = nullptr;

void ScopeFindDeclaredClassEntry(void* self, void* out_handle, const char* class_name) {
  (void)self;
  (void)class_name;
  *static_cast<void**>(out_handle) = g_class_info;
}

void* SystemFindTypeScopeEntry(void* /*self*/, const char* /*module*/, const char** /*binding*/) {
  return g_persistent_scope;
}

TEST(SchemaLayout, FieldWalkResolvesOffsetAndNetworkedFlag) {
  LayoutBuffers buffers;
  const char* class_name = "CBasePlayerController";
  const char* field_name = "m_hPawn";

  // Two fields: an unnetworked decoy first, then the networked target.
  const char* decoy_name = "m_decoy";
  std::memcpy(buffers.fields.data() + 0 * kFieldStride + kFieldNameOffset, &decoy_name,
              sizeof(decoy_name));
  const int decoy_offset = 0x99;
  std::memcpy(buffers.fields.data() + 0 * kFieldStride + kFieldOffsetField, &decoy_offset,
              sizeof(decoy_offset));

  std::memcpy(buffers.fields.data() + 1 * kFieldStride + kFieldNameOffset, &field_name,
              sizeof(field_name));
  const int pawn_offset = 0x5d8;
  std::memcpy(buffers.fields.data() + 1 * kFieldStride + kFieldOffsetField, &pawn_offset,
              sizeof(pawn_offset));
  const int networked_count = 1;
  std::memcpy(buffers.fields.data() + 1 * kFieldStride + kFieldMetadataCount, &networked_count,
              sizeof(networked_count));
  const char* metadata_name = "MNetworkEnable";
  void* metadata_name_ptr = const_cast<char*>(metadata_name);
  std::memcpy(buffers.metadata.data(), &metadata_name_ptr, sizeof(metadata_name_ptr));
  unsigned char* metadata_base = buffers.metadata.data();
  std::memcpy(buffers.fields.data() + 1 * kFieldStride + kFieldMetadata, &metadata_base,
              sizeof(metadata_base));

  const int class_size = 0x600;
  constexpr size_t kInfoSize = 0x20;  // SchemaClassInfoData_t::m_nSize
  std::memcpy(buffers.info.data() + kInfoSize, &class_size, sizeof(class_size));
  uint16_t field_count = 2;
  constexpr size_t kInfoFieldCount = 0x24;  // SchemaClassInfoData_t::m_nFieldCount
  std::memcpy(buffers.info.data() + kInfoFieldCount, &field_count, sizeof(field_count));
  unsigned char* fields_base = buffers.fields.data();
  std::memcpy(buffers.info.data() + kInfoFields, &fields_base, sizeof(fields_base));

  alignas(16) void* scope_table[3];
  std::memset(scope_table, 0, sizeof(scope_table));
  scope_table[modlock::gameinterop::kTypeScopeFindDeclaredClassSlot] =
      reinterpret_cast<void*>(&ScopeFindDeclaredClassEntry);
  FakeTypeScope type_scope{scope_table};
  alignas(16) void* system_table[16];
  std::memset(system_table, 0, sizeof(system_table));
  system_table[modlock::gameinterop::kSchemaSystemFindTypeScopeSlot] =
      reinterpret_cast<void*>(&SystemFindTypeScopeEntry);
  FakeTypeScope system{system_table};

  g_persistent_scope = &type_scope;
  g_class_info = buffers.info.data();
  auto field = modlock::gameinterop::SchemaFieldOf(&system, "server.dll", class_name, field_name);
  ASSERT_TRUE(field.has_value()) << field.error();
  EXPECT_EQ(field->offset, static_cast<size_t>(pawn_offset));
  EXPECT_EQ(field->size, static_cast<size_t>(class_size - pawn_offset));
  EXPECT_TRUE(field->networked);
  auto size = modlock::gameinterop::SchemaClassSizeOf(&system, "server.dll", class_name);
  ASSERT_TRUE(size) << size.error();
  EXPECT_EQ(*size, static_cast<size_t>(class_size));
  const int invalid_size = -1;
  std::memcpy(buffers.info.data() + kInfoSize, &invalid_size, sizeof(invalid_size));
  EXPECT_FALSE(modlock::gameinterop::SchemaClassSizeOf(&system, "server.dll", class_name));
  g_persistent_scope = nullptr;
  g_class_info = nullptr;
}

// ---- Fake-schema paths through SchemaFieldOf: exact class, exact field,
// base-class walk, storage span, and named errors. ----

// BuildClassInfo encodes one SchemaClassInfoData_t mirror: size at 0x20,
// field count at 0x24, base-class count at 0x29, fields pointer at 0x30,
// base-classes pointer at 0x38.
struct ClassInfoBuffers {
  std::array<unsigned char, 0x40> info{};
  std::array<unsigned char, 2 * kFieldStride> fields{};
  std::array<unsigned char, 16> bases{};

  void set_class_size(int size) { std::memcpy(info.data() + 0x20, &size, sizeof(size)); }
  void set_field_count(uint16_t count) { std::memcpy(info.data() + 0x24, &count, sizeof(count)); }
  void set_base_count(uint8_t count) { std::memcpy(info.data() + 0x29, &count, sizeof(count)); }
  void set_fields_pointer(unsigned char* fields_base) {
    std::memcpy(info.data() + kInfoFields, &fields_base, sizeof(fields_base));
  }
  void set_bases_pointer(std::array<unsigned char, 16>* bases_base) {
    auto* pointer = bases_base->data();
    std::memcpy(info.data() + 0x38, &pointer, sizeof(pointer));
  }
  void set_field(size_t index, const char* name, int offset) {
    std::memcpy(fields.data() + index * kFieldStride + kFieldNameOffset, &name, sizeof(name));
    std::memcpy(fields.data() + index * kFieldStride + kFieldOffsetField, &offset, sizeof(offset));
  }
};

// ScopeEntry returns g_class_info for every FindDeclaredClass call.
void ScopeEntry(void* /*self*/, void* out_handle, const char* /*class_name*/) {
  *static_cast<void**>(out_handle) = g_class_info;
}

// MissingScopeEntry returns no type scope, the live shape when server.dll's
// schemas are not open yet.
void* MissingScopeEntry(void* /*self*/, const char* /*module*/, const char** /*binding*/) {
  return nullptr;
}

// MakeMissingScopeSystem builds a schema system whose FindTypeScopeForModule
// always refuses; table must hold 16 slots.
void MakeMissingScopeSystem(void** table) {
  std::memset(table, 0, 16 * sizeof(void*));
  table[modlock::gameinterop::kSchemaSystemFindTypeScopeSlot] =
      reinterpret_cast<void*>(&MissingScopeEntry);
}

TEST(SchemaFieldOfPaths, ClosedTypeScopeNamesTheModule) {
  alignas(16) void* system_table[16];
  MakeMissingScopeSystem(system_table);
  FakeTypeScope system{system_table};

  auto field = modlock::gameinterop::SchemaFieldOf(&system, "server.dll", "CCitadelPlayerPawn",
                                                   "m_nSolidType");
  ASSERT_FALSE(field.has_value());
  EXPECT_NE(field.error().find("server.dll"), std::string::npos) << field.error();
}

TEST(SchemaFieldOfPaths, MissingClassNamesTheClass) {
  alignas(16) void* system_table[16];
  std::memset(system_table, 0, sizeof(system_table));
  system_table[modlock::gameinterop::kSchemaSystemFindTypeScopeSlot] =
      reinterpret_cast<void*>(&SystemFindTypeScopeEntry);
  FakeTypeScope system{system_table};
  alignas(16) void* scope_table[3];
  std::memset(scope_table, 0, sizeof(scope_table));
  scope_table[modlock::gameinterop::kTypeScopeFindDeclaredClassSlot] =
      reinterpret_cast<void*>(&ScopeEntry);
  FakeTypeScope type_scope{scope_table};

  g_persistent_scope = &type_scope;
  g_class_info = nullptr;  // FindDeclaredClass finds nothing.
  auto field = modlock::gameinterop::SchemaFieldOf(&system, "server.dll", "CCitadelPlayerPawn",
                                                   "m_nSolidType");
  g_persistent_scope = nullptr;
  ASSERT_FALSE(field.has_value());
  EXPECT_NE(field.error().find("CCitadelPlayerPawn"), std::string::npos) << field.error();
}

TEST(SchemaFieldOfPaths, MissingFieldNamesTheField) {
  ClassInfoBuffers buffers;
  buffers.set_class_size(0x7F0);
  buffers.set_field_count(1);
  const char* decoy = "m_decoy";
  buffers.set_field(0, decoy, 0x10);
  unsigned char* fields_base = buffers.fields.data();
  buffers.set_fields_pointer(fields_base);

  alignas(16) void* system_table[16];
  std::memset(system_table, 0, sizeof(system_table));
  system_table[modlock::gameinterop::kSchemaSystemFindTypeScopeSlot] =
      reinterpret_cast<void*>(&SystemFindTypeScopeEntry);
  FakeTypeScope system{system_table};
  alignas(16) void* scope_table[3];
  std::memset(scope_table, 0, sizeof(scope_table));
  scope_table[modlock::gameinterop::kTypeScopeFindDeclaredClassSlot] =
      reinterpret_cast<void*>(&ScopeEntry);
  FakeTypeScope type_scope{scope_table};

  g_persistent_scope = &type_scope;
  g_class_info = buffers.info.data();
  auto field = modlock::gameinterop::SchemaFieldOf(&system, "server.dll", "CCitadelPlayerPawn",
                                                   "m_takedamage");
  g_persistent_scope = nullptr;
  g_class_info = nullptr;
  ASSERT_FALSE(field.has_value());
  EXPECT_NE(field.error().find("m_takedamage"), std::string::npos) << field.error();
}

TEST(SchemaFieldOfPaths, FieldOnTheBaseClassResolvesThroughTheWalk) {
  ClassInfoBuffers derived;
  ClassInfoBuffers base;
  std::array<unsigned char, 16> bases{};

  // Derived: one own decoy field, one base class.
  derived.set_class_size(0x800);
  derived.set_field_count(1);
  const char* decoy = "m_decoy";
  derived.set_field(0, decoy, 0x10);
  derived.set_base_count(1);
  unsigned char* derived_fields = derived.fields.data();
  derived.set_fields_pointer(derived_fields);
  // SchemaBaseClasses[i] = { unsigned offset; CSchemaClassInfo* class; }.
  auto* base_info = base.info.data();
  std::memcpy(bases.data() + sizeof(void*), &base_info, sizeof(base_info));
  derived.set_bases_pointer(&bases);

  // Base: the target field lives here, not on the derived class.
  base.set_class_size(0x7F0);
  base.set_field_count(1);
  const char* takedamage = "m_takedamage";
  base.set_field(0, takedamage, 0x7EC);
  unsigned char* base_fields = base.fields.data();
  base.set_fields_pointer(base_fields);

  alignas(16) void* system_table[16];
  std::memset(system_table, 0, sizeof(system_table));
  system_table[modlock::gameinterop::kSchemaSystemFindTypeScopeSlot] =
      reinterpret_cast<void*>(&SystemFindTypeScopeEntry);
  FakeTypeScope system{system_table};
  alignas(16) void* scope_table[3];
  std::memset(scope_table, 0, sizeof(scope_table));
  scope_table[modlock::gameinterop::kTypeScopeFindDeclaredClassSlot] =
      reinterpret_cast<void*>(&ScopeEntry);
  FakeTypeScope type_scope{scope_table};

  g_persistent_scope = &type_scope;
  g_class_info = derived.info.data();
  auto field = modlock::gameinterop::SchemaFieldOf(&system, "server.dll", "CCitadelPlayerPawn",
                                                   "m_takedamage");
  g_persistent_scope = nullptr;
  g_class_info = nullptr;
  ASSERT_TRUE(field.has_value()) << field.error();
  EXPECT_EQ(field->offset, static_cast<size_t>(0x7EC));
  EXPECT_EQ(field->size, static_cast<size_t>(0x7F0 - 0x7EC));
  // Without MNetworkDisable a field may be networked.
  EXPECT_TRUE(field->networked);
}

TEST(SchemaFieldOfPaths, ZeroSizedStorageSpanIsAnError) {
  ClassInfoBuffers buffers;
  // The field offset sits at the class size, so the computed storage span is
  // empty and resolution must refuse rather than return a zero-width write.
  buffers.set_class_size(0x200);
  buffers.set_field_count(1);
  const char* solid = "m_nSolidType";
  buffers.set_field(0, solid, 0x200);
  unsigned char* fields_base = buffers.fields.data();
  buffers.set_fields_pointer(fields_base);

  alignas(16) void* system_table[16];
  std::memset(system_table, 0, sizeof(system_table));
  system_table[modlock::gameinterop::kSchemaSystemFindTypeScopeSlot] =
      reinterpret_cast<void*>(&SystemFindTypeScopeEntry);
  FakeTypeScope system{system_table};
  alignas(16) void* scope_table[3];
  std::memset(scope_table, 0, sizeof(scope_table));
  scope_table[modlock::gameinterop::kTypeScopeFindDeclaredClassSlot] =
      reinterpret_cast<void*>(&ScopeEntry);
  FakeTypeScope type_scope{scope_table};

  g_persistent_scope = &type_scope;
  g_class_info = buffers.info.data();
  auto field = modlock::gameinterop::SchemaFieldOf(&system, "server.dll", "CCitadelPlayerPawn",
                                                   "m_nSolidType");
  g_persistent_scope = nullptr;
  g_class_info = nullptr;
  ASSERT_FALSE(field.has_value());
  EXPECT_NE(field.error().find("storage span"), std::string::npos) << field.error();
}

// SchemaClass is an independently encoded SchemaClassInfoData_t holding its
// self binding, name and first base, the parts the class check reads.
struct SchemaClass {
  std::array<unsigned char, 0x40> info{};
  std::array<void*, 2> bases{};

  SchemaClass(const char* name, SchemaClass* base) {
    void* self = info.data();
    std::memcpy(info.data(), &self, sizeof(self));
    std::memcpy(info.data() + 0x08, &name, sizeof(name));
    if (base == nullptr) return;
    info[0x29] = 1;
    bases[1] = base->info.data();
    void* table = bases.data();
    std::memcpy(info.data() + 0x38, &table, sizeof(table));
  }
};

// ClassedEntity lays out an entity whose identity reaches a schema class
// through CEntityIdentity::m_pClass, CEntityClass::m_pClassInfo and
// CEntityClassInfo::m_pSchemaBinding.
struct ClassedEntity {
  std::array<void*, 4> entity{};
  std::array<void*, 2> identity{};
  std::array<void*, 12> entity_class{};
  std::array<void*, 6> class_info{};

  explicit ClassedEntity(SchemaClass& schema) {
    entity[2] = identity.data();
    identity[0] = entity.data();
    identity[1] = entity_class.data();
    entity_class[11] = class_info.data();
    class_info[5] = schema.info.data();
  }
};

// CopyMemory reads test memory in place of the operating system's copy.
bool CopyMemory(const void* source, void* out, size_t size) {
  std::memcpy(out, source, size);
  return true;
}

TEST(SchemaClassOfEntity, ReadsTheBoundClassAndItsFirstBaseChain) {
  SchemaClass base("CBaseEntity", nullptr);
  SchemaClass pawn("CCitadelPlayerPawn", &base);
  SchemaClass trooper("CNPC_Trooper", &base);
  ClassedEntity entity(pawn);

  auto schema = modlock::gameinterop::SchemaClassOfEntity(entity.entity.data(), CopyMemory);
  ASSERT_TRUE(schema) << schema.error();
  EXPECT_EQ(modlock::gameinterop::SchemaClassNameOf(*schema), "CCitadelPlayerPawn");
  EXPECT_TRUE(modlock::gameinterop::SchemaClassDerivesFrom(*schema, "CCitadelPlayerPawn"));
  EXPECT_TRUE(modlock::gameinterop::SchemaClassDerivesFrom(*schema, "CBaseEntity"));
  EXPECT_FALSE(modlock::gameinterop::SchemaClassDerivesFrom(*schema, "CNPC_Trooper"));
  EXPECT_FALSE(
      modlock::gameinterop::SchemaClassDerivesFrom(trooper.info.data(), "CCitadelPlayerPawn"));
}

TEST(SchemaClassOfEntity, RefusesALinkThatIsNotAClassBinding) {
  SchemaClass pawn("CCitadelPlayerPawn", nullptr);
  ClassedEntity entity(pawn);

  // A moved CEntityClass layout lands on memory that does not bind itself.
  void* elsewhere = nullptr;
  std::memcpy(pawn.info.data(), &elsewhere, sizeof(elsewhere));
  auto schema = modlock::gameinterop::SchemaClassOfEntity(entity.entity.data(), CopyMemory);
  ASSERT_FALSE(schema);
  EXPECT_NE(schema.error().find("unreadable"), std::string::npos) << schema.error();

  // A failed copy reports the same instead of faulting.
  auto refused = modlock::gameinterop::SchemaClassOfEntity(
      entity.entity.data(), [](const void*, void*, size_t) { return false; });
  EXPECT_FALSE(refused);
}

}  // namespace
