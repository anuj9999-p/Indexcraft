#pragma once
// Error handling for IndexCraft: a Status (code + message) and Result<T>.
// The engine does not throw for expected failures (I/O errors, corrupt files,
// duplicate keys, syntax errors); it returns a Status instead.

#include <optional>
#include <string>
#include <utility>

namespace indexcraft {

enum class Code {
  Ok = 0,
  InvalidArgument,
  DuplicateKey,
  NotFound,
  Corrupt,       // on-disk data failed validation
  IoError,
  SyntaxError,   // SQL-like query could not be parsed
  Unsupported,   // valid request, but outside the supported subset
  Internal       // invariant violation inside the engine
};

inline const char* code_name(Code c) {
  switch (c) {
    case Code::Ok: return "OK";
    case Code::InvalidArgument: return "INVALID_ARGUMENT";
    case Code::DuplicateKey: return "DUPLICATE_KEY";
    case Code::NotFound: return "NOT_FOUND";
    case Code::Corrupt: return "CORRUPT";
    case Code::IoError: return "IO_ERROR";
    case Code::SyntaxError: return "SYNTAX_ERROR";
    case Code::Unsupported: return "UNSUPPORTED";
    case Code::Internal: return "INTERNAL";
  }
  return "UNKNOWN";
}

class Status {
 public:
  Status() = default;
  Status(Code code, std::string message) : code_(code), message_(std::move(message)) {}

  static Status Ok() { return Status(); }
  static Status Error(Code code, std::string message) { return Status(code, std::move(message)); }

  bool ok() const { return code_ == Code::Ok; }
  Code code() const { return code_; }
  const std::string& message() const { return message_; }
  std::string to_string() const {
    if (ok()) return "OK";
    return std::string(code_name(code_)) + ": " + message_;
  }

 private:
  Code code_ = Code::Ok;
  std::string message_;
};

template <typename T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}                      // NOLINT: implicit by design
  Result(Status status) : status_(std::move(status)) {}              // NOLINT: implicit by design

  bool ok() const { return status_.ok(); }
  const Status& status() const { return status_; }
  T& value() { return *value_; }
  const T& value() const { return *value_; }
  T take() { return std::move(*value_); }

 private:
  Status status_;
  std::optional<T> value_;
};

}  // namespace indexcraft
