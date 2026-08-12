#pragma once

#include <string_view>

namespace qr {

/// A typed, named reference to one column of `TableT`.
///
/// Every table in your schema is a plain struct with one
/// `static constexpr Column<Self, T>` member per column, plus a
/// `static constexpr std::string_view table_name`. `TableT` only needs to be
/// complete inside `Column`'s member functions (not at the point a member is
/// declared), which is what makes it legal for a table to declare columns
/// that name itself, as below.
///
/// **Example:**
/// \code
///   struct Users {
///     static constexpr std::string_view table_name = "users";
///
///     static constexpr qr::Column<Users, int64_t> id{"id"};
///     static constexpr qr::Column<Users, std::string> name{"name"};
///     static constexpr qr::Column<Users, std::optional<double>> score{"score"};
///   };
/// \endcode
///
/// Once declared, `Users::id`, `Users::name`, etc. are what you pass to
/// `select()`, `where()`, `set()`, and every other qr:: entry point. The
/// column's C++ type (`int64_t`, `string`, `optional<double>`, ...) is what
/// tells qr how to bind and decode it.
template <typename TableT, typename T>
struct Column {
  std::string_view column_name;
  constexpr explicit Column(std::string_view name) : column_name(name) {}
  using value_type = T;
  using table_type = TableT;
  static constexpr std::string_view table_name() { return TableT::table_name; }
};

}  // namespace qr
