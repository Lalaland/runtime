/*
 * Copyright 2020 The TensorFlow Runtime Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//===- error_util.h ---------------------------------------------*- C++ -*-===//
//
// This file defines utilities related to error handling.
//
//===----------------------------------------------------------------------===//

#ifndef TFRT_SUPPORT_ERROR_UTIL_H_
#define TFRT_SUPPORT_ERROR_UTIL_H_

#include <memory>
#include <tuple>
#include <type_traits>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"
#include "tfrt/support/string_util.h"

// Concatenate 'left' and 'right'.
#define TFRT_CONCAT(left, right) TFRT_CONCAT_IMPL(left, right)
#define TFRT_CONCAT_IMPL(left, right) left##right

// Helper macro to get value from llvm::Expected.
//
// The result of 'expr' should be a llvm::Expected<T>. If it has a value, it
// is assigned to 'lhs'. Otherwise the error is returned.
//
// Usage: TFRT_ASSIGN_OR_RETURN(auto value, GetExpectedValue());
#define TFRT_ASSIGN_OR_RETURN(lhs, expr) \
  TFRT_ASSIGN_OR_RETURN_IMPL(TFRT_CONCAT(_expected_, __COUNTER__), lhs, expr)
#define TFRT_ASSIGN_OR_RETURN_IMPL(expected, lhs, expr) \
  auto expected = expr;                                 \
  if (!expected) return expected.takeError();           \
  lhs = std::move(*expected)

namespace tfrt {

namespace internal {
// Pimpl class holding a stack trace, see CreateStackTrace() below.
struct StackTraceImpl;
struct StackTraceDeleter {
  void operator()(StackTraceImpl* ptr) const;
};
// Print a previously captured stack trace to 'os'. Does not print anything
// if 'stack_trace' is a nullptr. Found through template ADL.
llvm::raw_ostream& operator<<(
    llvm::raw_ostream& os,
    const std::unique_ptr<StackTraceImpl, StackTraceDeleter>& stack_trace);
}  // namespace internal

// Holds a stack trace that can be written to a llvm::raw_ostream.
using StackTrace =
    std::unique_ptr<internal::StackTraceImpl, internal::StackTraceDeleter>;

// Capture the current stack trace, without the first 'skip_count' frames. The
// result may be empty (i.e. does not print anything) if capturing traces is
// not supported.
StackTrace CreateStackTrace(int skip_count = 0);

namespace internal {
// TMP to prevent elements of temporary reference types (i.e. llvm::ArrayRef,
// llvm::StringRef) because the underlying data is likely to go away before
// the Error is printed.
template <typename T>
struct IsTempRef : public std::integral_constant<bool, false> {};
template <typename T>
struct IsTempRef<llvm::ArrayRef<T>>
    : public std::integral_constant<bool, true> {};
template <>
struct IsTempRef<llvm::StringRef> : public std::integral_constant<bool, true> {
};
}  // namespace internal

// ErrorInfo with a pack of elements that are logged to llvm::raw_ostream.
template <typename... Args>
class TupleErrorInfo : public llvm::ErrorInfo<TupleErrorInfo<Args...>> {
  using Tuple = decltype(std::make_tuple(std::declval<Args>()...));

  template <bool...>
  struct BoolPack;
  template <bool... Bs>
  using AllFalse = std::is_same<BoolPack<Bs..., false>, BoolPack<false, Bs...>>;
  static_assert(
      AllFalse<internal::IsTempRef<std::decay_t<Args>>::value...>::value,
      "Argument types should not be temporary references.");

 public:
  // Required field for all ErrorInfo derivatives.
  static char ID;

  explicit TupleErrorInfo(Args... args) : tuple_(std::forward<Args>(args)...) {}

  template <typename T>
  constexpr const T& get() const {
    return std::get<T>(tuple_);
  }
  template <std::size_t I>
  constexpr const auto& get() const {
    return std::get<I>(tuple_);
  }

  void log(llvm::raw_ostream& os) const override {
    log(os, std::make_index_sequence<sizeof...(Args)>());
  }

  std::string message() const override {
    std::string message;
    llvm::raw_string_ostream os(message);
    log(os);
    return os.str();
  }

  std::error_code convertToErrorCode() const override {
    return llvm::inconvertibleErrorCode();
  }

 private:
  template <std::size_t... Is>
  void log(llvm::raw_ostream& os, std::index_sequence<Is...>) const {
    internal::ToStreamHelper(os, get<Is>()...);
  }

  Tuple tuple_;
};
template <typename... Args>
char TupleErrorInfo<Args...>::ID;

// Create error from args, which are written to an llvm::raw_ostream when the
// error is logged.
template <typename... Args>
llvm::Error MakeTupleError(Args&&... args) {
  return llvm::make_error<TupleErrorInfo<Args...>>(std::forward<Args>(args)...);
}

// Create error from args by writing them to an llvm::raw_ostream immediately.
template <typename... Args>
llvm::Error MakeStringError(Args&&... args) {
  return MakeTupleError(StrCat(std::forward<Args>(args)...));
}

}  // namespace tfrt

#endif  // TFRT_SUPPORT_ERROR_UTIL_H_
