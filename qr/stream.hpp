#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "qr/param.hpp"
#include "qr/row.hpp"

namespace qr {

namespace detail {

template <typename V>
void write_number(std::FILE* out, V value) {
  std::array<std::byte, sizeof(V)> bytes = std::bit_cast<std::array<std::byte, sizeof(V)>>(value);
  if constexpr (std::endian::native == std::endian::big) std::ranges::reverse(bytes);
  std::fwrite(bytes.data(), 1, bytes.size(), out);
}

inline void write_text(std::FILE* out, std::string_view value) {
  write_number(out, static_cast<std::uint32_t>(value.size()));
  std::fwrite(value.data(), 1, value.size(), out);
}

template <typename V>
void write_present(std::FILE* out, const V& value) {
  if constexpr (std::is_same_v<V, bool>) {
    write_number(out, static_cast<std::uint8_t>(value));
  } else if constexpr (std::is_same_v<V, std::string>) {
    write_text(out, value);
  } else if constexpr (std::is_same_v<V, std::chrono::system_clock::time_point>) {
    const std::chrono::microseconds since
      = std::chrono::duration_cast<std::chrono::microseconds>(value.time_since_epoch());
    write_number(out, static_cast<std::int64_t>(since.count()));
  } else if constexpr (std::is_same_v<V, std::vector<std::byte>>) {
    write_number(out, static_cast<std::uint32_t>(value.size()));
    std::fwrite(value.data(), 1, value.size(), out);
  } else if constexpr (IsNumericArray<V>::value) {
    using Element = typename V::value_type;
    write_number(out, static_cast<std::uint32_t>(value.size()));
    for (const Element& each : value) {
      if constexpr (std::is_integral_v<Element> && !std::is_same_v<Element, bool>) {
        write_number(out, static_cast<std::int64_t>(each));
      } else {
        write_number(out, static_cast<double>(each));
      }
    }
  } else if constexpr (HasAdlToColumn<V>) {
    write_present(out, to_column(value));
  } else if constexpr (HasAdlToString<V>) {
    write_text(out, to_string(value));
  } else if constexpr (std::is_integral_v<V> || std::is_enum_v<V>) {
    write_number(out, static_cast<std::int64_t>(value));
  } else {
    write_number(out, static_cast<double>(value));
  }
}

struct row_writer {
  std::FILE* out;

  template <typename V>
  void cell(const V& value) {
    if constexpr (IsOptionalType<V>::value) {
      write_number(out, static_cast<std::uint8_t>(value.has_value()));
      if (value) write_present(out, *value);
    } else {
      write_number(out, std::uint8_t{1});
      write_present(out, value);
    }
  }
};

}  // namespace detail

template <typename T>
void write_rows(const std::vector<T>& rows, std::FILE* out) {
  const std::vector<ColumnSpec>& columns = columns_of<T>();
  detail::write_number(out, static_cast<std::uint32_t>(columns.size()));
  for (const ColumnSpec& column : columns) {
    detail::write_text(out, column.name);
    detail::write_text(out, column.type);
    detail::write_number(out, static_cast<std::uint8_t>(column.key));
    detail::write_number(out, static_cast<std::uint8_t>(!column.references.empty()));
    detail::write_text(out, column.unit);
  }

  detail::write_number(out, static_cast<std::uint32_t>(rows.size()));
  detail::row_writer visit{out};
  for (const T& each : rows) each_value(each, visit);
}

}  // namespace qr
