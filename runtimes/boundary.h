// The boundary every interpreter module shares with the host: the two host
// imports, the protobuf framing an interpreter reads from the start event,
// and the Log call it reports its own failures through.

#ifndef MODLOCK_RUNTIMES_BOUNDARY_H_
#define MODLOCK_RUNTIMES_BOUNDARY_H_

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// host_call hands the host one encoded Call and returns the length of the
// encoded Reply, which host_read then copies out.
__attribute__((import_module("modlock"), import_name("host_call"))) uint32_t
host_call(const void* data, uint32_t size);

// host_read copies the host's pending message into data, which holds exactly
// the length the host announced.
__attribute__((import_module("modlock"), import_name("host_read"))) void host_read(void* data,
                                                                                   uint32_t size);

#ifdef __cplusplus
}
#endif

// put_varint writes value as a protobuf varint at out and returns its length.
static inline size_t put_varint(uint8_t* out, uint64_t value) {
  size_t size = 0;
  while (value >= 0x80) {
    out[size++] = (uint8_t)(value | 0x80);
    value >>= 7;
  }
  out[size++] = (uint8_t)value;
  return size;
}

// get_varint reads a protobuf varint at *at, before end, and advances *at.
static inline int get_varint(const uint8_t** at, const uint8_t* end, uint64_t* value) {
  uint64_t read = 0;
  for (int shift = 0; shift < 64 && *at < end; shift += 7) {
    uint8_t byte = *(*at)++;
    read |= (uint64_t)(byte & 0x7f) << shift;
    if (!(byte & 0x80)) {
      *value = read;
      return 1;
    }
  }
  return 0;
}

// find_field finds the length-delimited field number in an encoded message
// and returns its bytes through out and out_size.
static inline int find_field(const uint8_t* data, size_t size, uint32_t number, const uint8_t** out,
                             size_t* out_size) {
  const uint8_t* at = data;
  const uint8_t* end = data + size;
  while (at < end) {
    uint64_t key;
    uint64_t value;
    if (!get_varint(&at, end, &key)) return 0;
    switch (key & 7) {
      case 0:
        if (!get_varint(&at, end, &value)) return 0;
        break;
      case 1:
        if (end - at < 8) return 0;
        at += 8;
        break;
      case 2:
        if (!get_varint(&at, end, &value) || value > (uint64_t)(end - at)) return 0;
        if (key >> 3 == number) {
          *out = at;
          *out_size = (size_t)value;
          return 1;
        }
        at += value;
        break;
      case 5:
        if (end - at < 4) return 0;
        at += 4;
        break;
      default:
        return 0;
    }
  }
  return 0;
}

// start_source finds StartEvent.source in an encoded start Call: Call.request
// is field 2 and StartEvent.source field 3.
static inline int start_source(const uint8_t* call, size_t size, const uint8_t** source,
                               size_t* source_size) {
  const uint8_t* event;
  size_t event_size;
  return find_field(call, size, 2, &event, &event_size) &&
         find_field(event, event_size, 3, source, source_size);
}

// little reads a little-endian integer of size bytes at data.
static inline uint32_t little(const uint8_t* data, int size) {
  uint32_t value = 0;
  for (int i = size - 1; i >= 0; --i) value = value << 8 | data[i];
  return value;
}

// zip_file receives one file of a zip: its name and its contents.
typedef int (*zip_file)(void* context, const char* name, size_t name_size, const uint8_t* data,
                        size_t size);

// read_zip hands each file of an uncompressed zip to each, walking the zip's
// central directory. It returns 0 when the zip is unreadable, compresses a
// file, or each returns 0.
static inline int read_zip(const uint8_t* zip, size_t size, zip_file each, void* context) {
  // Find the end of central directory record, which ends the archive before
  // a comment of at most 64 KiB.
  const size_t end_size = 22;
  if (size < end_size) return 0;
  size_t end = size - end_size;
  while (little(zip + end, 4) != 0x06054b50) {
    if (end == 0 || size - end > end_size + 0xffff) return 0;
    --end;
  }
  size_t entries = little(zip + end + 10, 2);
  size_t at = little(zip + end + 16, 4);

  // Read each file through its local header, skipping directories.
  for (size_t i = 0; i < entries; ++i) {
    if (at + 46 > size || little(zip + at, 4) != 0x02014b50) return 0;
    uint32_t method = little(zip + at + 10, 2);
    size_t stored = little(zip + at + 20, 4);
    size_t name_size = little(zip + at + 28, 2);
    size_t local = little(zip + at + 42, 4);
    const char* name = (const char*)(zip + at + 46);
    if (at + 46 + name_size > size) return 0;
    at += 46 + name_size + little(zip + at + 30, 2) + little(zip + at + 32, 2);
    if (name_size && name[name_size - 1] == '/') continue;
    if (method != 0) return 0;
    if (local + 30 > size || little(zip + local, 4) != 0x04034b50) return 0;
    size_t data = local + 30 + little(zip + local + 26, 2) + little(zip + local + 28, 2);
    if (data + stored > size || !each(context, name, name_size, zip + data, stored)) return 0;
  }
  return 1;
}

// exchange sends an encoded Call and returns the encoded Reply, which the
// caller frees. An empty Reply still has a buffer, because the interpreters
// copy it with memcpy, which needs one.
static inline uint8_t* exchange(const uint8_t* request, size_t size, uint32_t* response_size) {
  *response_size = host_call(request, (uint32_t)size);
  uint8_t* response = (uint8_t*)malloc(*response_size ? *response_size : 1);
  if (!response) __builtin_trap();
  if (*response_size) host_read(response, *response_size);
  return response;
}

// host_log writes one line to the server log through the host's Log call.
static inline void host_log(const char* text, size_t size) {
  // Call{method: "Log", request: LogRequest{message: text}}: the method is
  // field 1, the request field 2, and the message field 1 inside it.
  static const uint8_t method[] = {0x0a, 3, 'L', 'o', 'g'};
  uint8_t prefix[10];
  size_t prefix_size = put_varint(prefix, size);
  size_t inner = 1 + prefix_size + size;
  uint8_t* request = (uint8_t*)malloc(sizeof(method) + inner + 11);
  if (!request) __builtin_trap();
  memcpy(request, method, sizeof(method));
  size_t at = sizeof(method);
  request[at++] = 0x12;
  at += put_varint(request + at, inner);
  request[at++] = 0x0a;
  memcpy(request + at, prefix, prefix_size);
  at += prefix_size;
  memcpy(request + at, text, size);
  at += size;

  uint32_t response_size;
  free(exchange(request, at, &response_size));
  free(request);
}

#endif  // MODLOCK_RUNTIMES_BOUNDARY_H_
