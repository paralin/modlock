//go:build deps_only

#include "wasm/trace_writer.h"

#include <array>
#include <utility>

namespace modlock::wasm {

std::expected<std::unique_ptr<TraceWriter>, std::string> TraceWriter::Open(
    const std::filesystem::path& path, uint64_t max_bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    return std::unexpected("cannot create " + path.string());
  }
  return std::unique_ptr<TraceWriter>(new TraceWriter(std::move(file), max_bytes));
}

TraceWriter::TraceWriter(std::ofstream file, uint64_t max_bytes)
    : file_(std::move(file)), left_(max_bytes) {}

void TraceWriter::Event(const Call& call, std::chrono::system_clock::time_point at) {
  trace::Record record;
  auto* event = record.mutable_event();
  *event->mutable_call() = call;
  event->set_unix_nanos(
      std::chrono::duration_cast<std::chrono::nanoseconds>(at.time_since_epoch()).count());
  Write(record);
}

void TraceWriter::HostCall(const Call& call, const Reply& reply) {
  trace::Record record;
  auto* host_call = record.mutable_call();
  *host_call->mutable_call() = call;
  *host_call->mutable_reply() = reply;
  Write(record);
}

void TraceWriter::Write(const trace::Record& record) {
  if (full_) {
    return;
  }

  // Encode the length as a varint, then the record.
  record.SerializeToString(&encoded_);
  std::array<char, 10> length;
  size_t used = 0;
  for (uint64_t size = encoded_.size();; size >>= 7) {
    length[used++] = static_cast<char>((size & 0x7f) | (size >= 0x80 ? 0x80 : 0));
    if (size < 0x80) {
      break;
    }
  }

  // Stop before a record that would pass the limit, or after a failed write.
  const uint64_t bytes = used + encoded_.size();
  if (bytes > left_) {
    full_ = true;
    file_.flush();
    return;
  }
  file_.write(length.data(), static_cast<std::streamsize>(used));
  file_.write(encoded_.data(), static_cast<std::streamsize>(encoded_.size()));
  left_ -= bytes;
  if (!file_) {
    full_ = true;
  }
}

}  // namespace modlock::wasm
