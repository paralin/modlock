#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "src/wasm/trace/trace.pb.h"
#include "src/wasm/wasm.pb.h"

namespace modlock::wasm {

// TraceWriter records the inputs one running build of a mod gets, each event
// the host delivers with the time it delivered it and each host call with
// its reply, to a file of length-delimited trace.Record messages, so the same
// session can be replayed to another build of the mod. Records go out in the
// order they happen. Once the file reaches its size limit the writer stops,
// leaving every record before it whole, and writes nothing more.
//
// A TraceWriter runs on the thread of the instance it records.
class TraceWriter {
 public:
  // Open creates the file at path, replacing any file there, and writes at
  // most max_bytes to it.
  [[nodiscard]] static std::expected<std::unique_ptr<TraceWriter>, std::string> Open(
      const std::filesystem::path& path, uint64_t max_bytes);

  // Event records an event the host delivers at at.
  void Event(const Call& call, std::chrono::system_clock::time_point at);

  // HostCall records a host call and the host's reply.
  void HostCall(const Call& call, const Reply& reply);

 private:
  TraceWriter(std::ofstream file, uint64_t max_bytes);

  // Write appends record after its length, or stops the writer once record
  // would pass the limit.
  void Write(const trace::Record& record);

  std::ofstream file_;
  // left_ is how many more bytes the file may take, and full_ is true once
  // the writer stopped.
  uint64_t left_;
  bool full_ = false;
  // encoded_ holds the record being written, kept to reuse its buffer.
  std::string encoded_;
};

}  // namespace modlock::wasm
