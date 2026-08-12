#pragma once

#include <charconv>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace qr {

/// A bound query parameter, narrowed to the handful of concrete types
/// `pqxx::params::append` understands.
///
/// This is the "wire type" every column value gets converted to via
/// `to_param()` before it's sent to Postgres — it's the element type of
/// `CompiledQuery::params`, which is what you inspect in tests that assert
/// on generated SQL without touching a database.
///
/// `std::monostate` means a genuine SQL *NULL* (e.g. from an empty
/// `std::optional`), not "no value was given" — a `qr::Param` is always
/// populated with something.
///
/// **Example:**
/// \code
///   qr::Param p = qr::to_param(std::optional<int>{});  // holds std::monostate (NULL)
///   qr::Param q = qr::to_param(42);                    // holds an int
///   std::get<int>(q) == 42;
/// \endcode
using Param =
    std::variant<std::monostate, bool, int, int64_t, double, std::string, std::vector<std::byte>>;

template <typename T>
struct IsOptionalType : std::false_type {};

template <typename T>
struct IsOptionalType<std::optional<T>> : std::true_type {};

template <typename T>
struct UnwrapOptional {
  using type = T;
};

template <typename T>
struct UnwrapOptional<std::optional<T>> {
  using type = T;
};

template <typename T>
using unwrap_optional_t = typename UnwrapOptional<T>::type;

/// The ADL hook qr looks for on an enum used as a column's value type.
///
/// Works for any closed-set type, not just enums — see `to_param()` below
/// for how it's used, and `HasAdlFromString` for its decode-side
/// counterpart.
///
/// **Example:**
/// \code
///   enum class OrderStatus { Pending, Shipped };
///   // declared in OrderStatus's own namespace, so ADL finds it:
///   constexpr std::string_view to_string(OrderStatus s) {
///     return s == OrderStatus::Pending ? "pending" : "shipped";
///   }
/// \endcode
template <typename T>
concept HasAdlToString = requires(T v) {
  { to_string(v) } -> std::convertible_to<std::string_view>;
};

/// Decode-side counterpart to `HasAdlToString`.
///
/// Used when reading a column back out of a query result. The
/// `std::type_identity<T>` tag exists because return type alone can't
/// distinguish overloads for different enums — write it as
/// `from_string(s, std::type_identity<OrderStatus>{})` next to
/// `OrderStatus`'s `to_string()`.
template <typename T>
concept HasAdlFromString = requires(std::string_view s) {
  { from_string(s, std::type_identity<T>{}) } -> std::same_as<T>;
};

// Civil calendar <-> days-since-epoch, per Howard Hinnant's public-domain
// algorithm (http://howardhinnant.github.io/date_algorithms.html). Used
// instead of gmtime/strftime/strptime because those are POSIX extensions
// that glibc hides under -std=c++23 (strict ANSI) without _GNU_SOURCE —
// this is pure integer arithmetic, portable regardless of libc visibility
// macros or <chrono> calendar-support maturity in the standard library.
constexpr int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

constexpr void civil_from_days(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y += (m <= 2);
}

// pqxx has no chrono support at all (no nullness/string_traits specialization
// for any time_point), so timestamps round-trip as Postgres's own text format
// ("YYYY-MM-DD HH:MM:SS.ffffff", UTC) — the same text form to_char()/psql use,
// which Postgres coerces implicitly when the bound param lands in a timestamp
// column.
inline std::string to_timestamp_text(std::chrono::system_clock::time_point tp) {
  using namespace std::chrono;
  const time_point<system_clock, days> day_point = floor<days>(tp);
  const auto time_of_day = tp - day_point;
  const auto hours_part = duration_cast<hours>(time_of_day);
  const auto minutes_part = duration_cast<minutes>(time_of_day - hours_part);
  const auto seconds_part = duration_cast<seconds>(time_of_day - hours_part - minutes_part);
  const auto micros_part =
      duration_cast<microseconds>(time_of_day - hours_part - minutes_part - seconds_part);

  int64_t y;
  unsigned m, d;
  civil_from_days(day_point.time_since_epoch().count(), y, m, d);

  // The trailing +00 is not decoration: a timestamptz input without an offset is
  // interpreted in the session's TimeZone, so omitting it would silently shift
  // the stored instant on any connection that is not UTC.
  char out[48];
  std::snprintf(out, sizeof(out), "%04lld-%02u-%02u %02lld:%02lld:%02lld.%06lld+00",
                static_cast<long long>(y), m, d, static_cast<long long>(hours_part.count()),
                static_cast<long long>(minutes_part.count()),
                static_cast<long long>(seconds_part.count()),
                static_cast<long long>(micros_part.count()));
  return std::string(out);
}

// Decode side of to_timestamp_text — parses Postgres's
// "YYYY-MM-DD HH:MM:SS[.ffffff]" text (the fraction is omitted when zero).
inline std::chrono::system_clock::time_point parse_timestamp(std::string_view text) {
  auto read_int = [&](size_t start, size_t len) {
    int value = 0;
    std::from_chars(text.data() + start, text.data() + start + len, value);
    return value;
  };

  const int64_t year = read_int(0, 4);
  const unsigned month = static_cast<unsigned>(read_int(5, 2));
  const unsigned day = static_cast<unsigned>(read_int(8, 2));
  const int hour = read_int(11, 2);
  const int minute = read_int(14, 2);
  const int second = read_int(17, 2);

  // Postgres trims trailing zeros from the fraction and appends the session
  // offset, so the text after '.' is digits followed by something like "+02" —
  // take only the digits, then right-pad, or ".5" would decode as 5µs not 0.5s.
  int micros = 0;
  if (const auto dot = text.find('.'); dot != std::string_view::npos) {
    std::string digits;
    for (size_t i = dot + 1; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i)
      digits.push_back(text[i]);
    digits.resize(6, '0');
    std::from_chars(digits.data(), digits.data() + digits.size(), micros);
  }

  // Trailing [+-]HH[:MM]. Searched past index 10 so the date's own '-'
  // separators are not mistaken for the offset sign.
  int offset_minutes = 0;
  if (const auto sign = text.find_last_of("+-"); sign != std::string_view::npos && sign > 10) {
    const int offset_hours = read_int(sign + 1, 2);
    const bool has_minutes = sign + 5 <= text.size() && text[sign + 3] == ':';
    const int extra_minutes = has_minutes ? read_int(sign + 4, 2) : 0;
    offset_minutes = (offset_hours * 60 + extra_minutes) * (text[sign] == '-' ? -1 : 1);
  }

  using namespace std::chrono;
  const time_point<system_clock, days> day_point{days{days_from_civil(year, month, day)}};
  system_clock::time_point tp = day_point;
  tp += hours(hour) + minutes(minute) + seconds(second) + microseconds(micros);
  return tp - minutes(offset_minutes);
}

/// Converts one column value into its bound-parameter form (`qr::Param`).
///
/// This runs implicitly wherever you hand qr a value — `InsertQuery::set()`,
/// `UpdateQuery::set()`, and every comparison operator (`==`, `!=`, `<`,
/// ...) all call it — so you rarely need to call it directly outside of
/// tests.
///
/// `std::optional<T>` unwraps to *NULL* when empty, or `to_param(*value)`
/// otherwise. Enum types (and anything else with a closed set of values)
/// are handled generically: if your type has an ADL `to_string(T)` visible
/// from its own namespace (`HasAdlToString` above), qr finds it
/// automatically and stores the string form — no specialization needed
/// inside this header.
///
/// **Example:**
/// \code
///   qr::to_param(OrderStatus::Pending);    // -> Param holding "pending" (via to_string)
///   qr::to_param(std::optional<int>{});    // -> Param holding std::monostate (NULL)
///   qr::to_param(std::optional<int>{7});   // -> Param holding 7
/// \endcode
///
/// \return the bound-parameter form of `value`.
template <typename T>
Param to_param(T value) {
  if constexpr (IsOptionalType<T>::value) {
    return value.has_value() ? to_param(std::move(*value)) : Param{std::monostate{}};
  } else if constexpr (std::same_as<T, bool>) {
    return Param{value};
  } else if constexpr (std::same_as<T, int>) {
    return Param{value};
  } else if constexpr (std::same_as<T, int64_t>) {
    return Param{value};
  } else if constexpr (std::same_as<T, double>) {
    return Param{value};
  } else if constexpr (std::same_as<T, std::vector<std::byte>>) {
    return Param{std::move(value)};
  } else if constexpr (std::same_as<T, std::chrono::system_clock::time_point>) {
    return Param{to_timestamp_text(value)};
  } else if constexpr (std::convertible_to<T, std::string>) {
    return Param{std::string(std::move(value))};
  } else if constexpr (HasAdlToString<T>) {
    return Param{std::string(to_string(value))};
  } else {
    // Landed here? T is a column's value_type (or the inner type of an optional<T>)
    // that isn't a builtin, isn't string-convertible, and has no ADL to_string(T).
    // Fix: define `std::string_view to_string(T)` next to T's own definition, in T's
    // own namespace — see HasAdlToString above for a worked example.
    static_assert(false,
                  "qr::to_param: no conversion known for this type — add an ADL "
                  "to_string(T) next to T's definition (see qr::HasAdlToString)");
  }
}

}  // namespace qr
