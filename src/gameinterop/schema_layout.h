#pragma once

#include <cstddef>
#include <cstdint>

namespace modlock::gameinterop::schema {

// The schema system's x64 records, after sourcesdk
// public/schemasystem/schematypes.h. Game build 6711 added cpp_name after
// project. Every record is static data of its declaring module and lives as
// long as that module stays loaded.

struct MetadataEntry {
  const char* name;
  const void* data;
};

// Type is CSchemaType: a vtable, then the type's name as the schema spells it.
struct Type {
  const void* vtable;
  const char* name;
};

struct Field {
  const char* name;
  const Type* type;
  int32_t offset;
  int32_t metadata_count;
  const MetadataEntry* metadata;
};

struct ClassInfo;

struct BaseClass {
  uint32_t offset;
  const ClassInfo* info;
};

struct ClassInfo {
  // self points back at this record; a scan recognizes a record by it.
  const ClassInfo* self;
  const char* name;
  // project is the declaring module's name without its extension, such as
  // server.
  const char* project;
  const char* cpp_name;
  int32_t size;
  uint16_t field_count;
  uint16_t metadata_count;
  uint8_t alignment;
  uint8_t base_count;
  const Field* fields;
  const BaseClass* bases;
  const void* data_map;
  const MetadataEntry* metadata;
  const void* type_scope;
  const void* declared_class;
  uint32_t flags;
  uint32_t flags2;
};

struct Enumerator {
  const char* name;
  int64_t value;
  int32_t metadata_count;
  const MetadataEntry* metadata;
};

struct EnumInfo {
  const EnumInfo* self;
  const char* name;
  const char* project;
  uint8_t size;
  uint8_t alignment;
  uint16_t flags;
  uint16_t enumerator_count;
  uint16_t metadata_count;
  const Enumerator* enumerators;
  const MetadataEntry* metadata;
  const void* type_scope;
};

static_assert(sizeof(Field) == 32);
static_assert(sizeof(BaseClass) == 16);
static_assert(offsetof(ClassInfo, size) == 0x20);
static_assert(offsetof(ClassInfo, base_count) == 0x29);
static_assert(offsetof(ClassInfo, fields) == 0x30);
static_assert(offsetof(ClassInfo, flags) == 0x60);
static_assert(sizeof(Enumerator) == 32);
static_assert(offsetof(EnumInfo, enumerator_count) == 0x1c);
static_assert(offsetof(EnumInfo, enumerators) == 0x20);
static_assert(offsetof(EnumInfo, type_scope) == 0x30);

}  // namespace modlock::gameinterop::schema
