#pragma once

#include <string_view>
#include <utility>

#if defined(__GNUC__) && !defined(__clang__)
#include <meta>
#include <span>
#include <vector>
#define QR_HAS_MEMBER_NAMES 1
#else
#define QR_HAS_MEMBER_NAMES 0
#endif

namespace qr {

struct key_t {};
inline constexpr key_t key{};

struct serial_t {};
inline constexpr serial_t serial{};

template <typename TableT>
struct references_t {};
template <typename TableT>
inline constexpr references_t<TableT> references{};

template <typename TableT, typename T>
class Column;

namespace detail {

#if QR_HAS_MEMBER_NAMES
template <typename TableT, typename T>
struct ColumnEntry {
  const Column<TableT, T>* ptr;
  const char* name;
};

template <typename TableT, typename T>
std::span<const ColumnEntry<TableT, T>> column_entries();

template <typename TableT>
consteval std::string_view reflected_table_name() {
  return std::define_static_string(std::meta::identifier_of(^^TableT));
}
#else
template <typename TableT>
std::string_view reflected_table_name();
#endif

}  // namespace detail

template <typename TableT>
constexpr std::string_view table_name_of() {
  if constexpr (requires { TableT::table_name; }) {
    return TableT::table_name;
  } else {
    return detail::reflected_table_name<TableT>();
  }
}

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
///     static constexpr qr::Column<Users, int64_t> id{};
///     static constexpr qr::Column<Users, std::string> name{"override_name"};
///     static constexpr qr::Column<Users, std::optional<double>> score{};
///   };
/// \endcode
///
/// Once declared, `Users::id`, `Users::name`, etc. are what you pass to
/// `select()`, `where()`, `set()`, and every other qr:: entry point. The
/// column's C++ type (`int64_t`, `string`, `optional<double>`, ...) is what
/// tells qr how to bind and decode it.
///
/// The SQL name is the C++ member name.
template <typename TableT, typename T>
class Column {
  std::string_view column_name{};

 public:
  constexpr Column() = default;
  constexpr explicit Column(std::string_view name) : column_name(name) {}

  Column(const Column& other) : column_name(other.name()) {}

  using value_type = T;
  using table_type = TableT;

  static constexpr std::string_view table_name() { return table_name_of<TableT>(); }

  std::string_view name() const {
    if (!column_name.empty()) return column_name;
#if QR_HAS_MEMBER_NAMES
    for (const detail::ColumnEntry<TableT, T>& entry : detail::column_entries<TableT, T>())
      if (entry.ptr == this) return entry.name;
#endif
  }
};

#if QR_HAS_MEMBER_NAMES
namespace detail {

template <std::meta::info owner>
consteval std::span<const std::meta::info> members_of() {
  constexpr std::meta::access_context context = std::meta::access_context::unchecked();
  const std::vector<std::meta::info> found = std::meta::members_of(owner, context);
  return std::define_static_array(found);
}

template <typename T>
consteval std::span<const std::meta::info> bases_of() {
  constexpr std::meta::access_context context = std::meta::access_context::unchecked();
  const std::vector<std::meta::info> found = std::meta::bases_of(^^T, context);
  return std::define_static_array(found);
}

template <typename T>
consteval std::span<const std::meta::info> nonstatic_members_of() {
  constexpr std::meta::access_context context = std::meta::access_context::unchecked();
  const std::vector<std::meta::info> found = std::meta::nonstatic_data_members_of(^^T, context);
  return std::define_static_array(found);
}

template <typename T>
consteval std::span<const std::meta::info> static_members_of() {
  constexpr std::meta::access_context context = std::meta::access_context::unchecked();
  const std::vector<std::meta::info> found = std::meta::static_data_members_of(^^T, context);
  return std::define_static_array(found);
}

template <typename TableT, typename T>
consteval auto make_column_entries() {
  std::vector<ColumnEntry<TableT, T>> found;
  template for (constexpr auto member : static_members_of<TableT>()) {
    if constexpr (std::meta::has_identifier(member) &&
                  (std::meta::type_of(member) == ^^const Column<TableT, T> ||
                   std::meta::type_of(member) == ^^Column<TableT, T>)) {
      constexpr const Column<TableT, T>& object =
          std::meta::extract<const Column<TableT, T>&>(member);
      found.push_back(ColumnEntry<TableT, T>{
          &object, std::define_static_string(std::meta::identifier_of(member))});
    }
  }
  return std::define_static_array(found);
}

template <typename TableT, typename T>
std::span<const ColumnEntry<TableT, T>> column_entries() {
  static constexpr auto entries = make_column_entries<TableT, T>();
  return entries;
}

}  // namespace detail
#else
namespace detail {

template <auto owner>
consteval auto members_of();

template <typename T>
consteval auto bases_of();

template <typename T>
consteval auto nonstatic_members_of();

template <typename T>
consteval auto static_members_of();

}  // namespace detail
#endif

}  // namespace qr

#define QR_COLUMN(Table, Type, member) \
  static constexpr ::qr::Column<Table, Type> member{#member}

#if QR_HAS_MEMBER_NAMES
#define QR_KEY [[=::qr::key]]
#define QR_SERIAL [[=::qr::serial]]
#define QR_REFERENCES(Table) [[=::qr::references<Table>]]
#else
#define QR_KEY
#define QR_SERIAL
#define QR_REFERENCES(Table)
#endif
