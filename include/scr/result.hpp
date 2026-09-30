#pragma once

#include <optional>
#include <utility>

#include "scr/status.hpp"

namespace scr {

// Result<T> carries either a value or a failure Status. value()/take() have the
// precondition ok() == true; every call site checks ok() first.
template <typename T>
class Result {
 public:
  static Result success(T value) {
    Result result;
    result.value_.emplace(std::move(value));
    return result;
  }

  static Result failure(Status status) {
    Result result;
    result.status_ = std::move(status);
    return result;
  }

  static Result failure(ErrorCode code, std::string detail, std::string subject = {}) {
    return failure(Status::failure(code, std::move(detail), std::move(subject)));
  }

  bool ok() const noexcept { return value_.has_value(); }
  explicit operator bool() const noexcept { return ok(); }

  const T& value() const noexcept { return *value_; }
  T& value() noexcept { return *value_; }
  T take() noexcept { return std::move(*value_); }

  const Status& status() const noexcept { return status_; }

 private:
  Result() = default;

  std::optional<T> value_;
  Status status_;
};

// Result<void>: success carries no value. This is the type used by every
// operation that either succeeds or reports a refusal.
template <>
class Result<void> {
 public:
  static Result success() { return Result(); }

  static Result failure(Status status) {
    Result result;
    result.ok_ = false;
    result.status_ = std::move(status);
    return result;
  }

  static Result failure(ErrorCode code, std::string detail, std::string subject = {}) {
    return failure(Status::failure(code, std::move(detail), std::move(subject)));
  }

  bool ok() const noexcept { return ok_; }
  explicit operator bool() const noexcept { return ok_; }

  const Status& status() const noexcept { return status_; }

 private:
  Result() = default;

  bool ok_ = true;
  Status status_;
};

using VoidResult = Result<void>;

inline VoidResult ok_result() { return VoidResult::success(); }

}  // namespace scr
