#pragma once

#include <cerrno>
#include <expected>
#include <string>
#include <system_error>
#include <utility>

namespace rail {

struct Error {
  std::error_code Code;
  std::string Context;

  std::string message() const { return Context.empty() ? Code.message() : Context + ": " + Code.message(); }
};

template <typename T> using Result = std::expected<T, Error>;

inline std::unexpected<Error> fail(std::error_code Code, std::string Context) { return std::unexpected(Error{Code, std::move(Context)}); }

inline std::unexpected<Error> failErrno(std::string Context) { return fail(std::error_code(errno, std::generic_category()), std::move(Context)); }

inline std::unexpected<Error> failMessage(std::string Context) { return fail(std::make_error_code(std::errc::protocol_error), std::move(Context)); }

} // namespace rail
