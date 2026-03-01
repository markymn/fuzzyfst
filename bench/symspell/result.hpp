#pragma once

#include <stdexcept>
#include <string>
#include <variant>

namespace yams::symspell {

// Minimal error codes used by symspell
enum class ErrorCode {
    Success = 0,
    DatabaseError,
    InternalError,
    Unknown
};

// Convert error code to string
constexpr const char* errorToString(ErrorCode error) {
    switch (error) {
        case ErrorCode::Success:
            return "Success";
        case ErrorCode::DatabaseError:
            return "Database error";
        case ErrorCode::InternalError:
            return "Internal error";
        case ErrorCode::Unknown:
        default:
            return "Unknown error";
    }
}

struct Error {
    ErrorCode code;
    std::string message;

    Error() : code(ErrorCode::Success), message("") {}
    Error(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}
    Error(ErrorCode c) : code(c), message(errorToString(c)) {}
    Error(std::string msg) : code(ErrorCode::Unknown), message(std::move(msg)) {}

    bool operator==(ErrorCode c) const { return code == c; }
    bool operator!=(ErrorCode c) const { return code != c; }
};

// Simple Result type for operations that can fail
template <typename T> class Result {
public:
    Result() : data_(Error{ErrorCode::InternalError, "Uninitialized Result"}) {}
    Result(T&& value) : data_(std::move(value)) {}
    Result(const T& value) : data_(value) {}
    Result(ErrorCode error) : data_(Error{error}) {}
    Result(Error error) : data_(std::move(error)) {}

    bool has_value() const noexcept { return std::holds_alternative<T>(data_); }
    explicit operator bool() const noexcept { return has_value(); }

    const T& value() const& {
        if (!has_value()) {
            throw std::runtime_error("Result contains error");
        }
        return std::get<T>(data_);
    }

    T& value() & {
        if (!has_value()) {
            throw std::runtime_error("Result contains error");
        }
        return std::get<T>(data_);
    }

    const Error& error() const {
        if (has_value()) {
            throw std::runtime_error("Result contains value");
        }
        return std::get<Error>(data_);
    }

private:
    std::variant<T, Error> data_;
};

// Specialization for void
template <> class Result<void> {
public:
    Result() : error_() {}
    Result(ErrorCode error) : error_(Error{error}) {}
    Result(Error error) : error_(std::move(error)) {}

    bool has_value() const noexcept { return error_.code == ErrorCode::Success; }
    explicit operator bool() const noexcept { return has_value(); }

    void value() const {
        if (!has_value()) {
            throw std::runtime_error("Result contains error");
        }
    }

    const Error& error() const {
        if (has_value()) {
            throw std::runtime_error("Result contains value");
        }
        return error_;
    }

private:
    Error error_{ErrorCode::Success, ""};
};

} // namespace yams::symspell
