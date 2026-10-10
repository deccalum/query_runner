#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "qr/compiled_query.hpp"
#include "qr/param.hpp"
#include "qr/predicate.hpp"
#include "qr/table.hpp"

namespace qr {

template <typename P>
struct MemberPointer;

template <typename C, typename M>
struct MemberPointer<M C::*> {
  using owner = C;
  using type = M;
};

template <typename TableT, auto Member>
using member_column_t = Column<TableT, typename MemberPointer<decltype(Member)>::type>;

template <typename T>
constexpr bool is_column_value() {
  using U = unwrap_optional_t<T>;
  return std::is_arithmetic_v<U> || std::is_enum_v<U> || std::same_as<U, std::string> ||
         std::same_as<U, std::vector<std::byte>> || IsNumericArray<U>::value ||
         std::same_as<U, std::chrono::system_clock::time_point> ||
         std::is_convertible_v<const U&, double>;
}

template <typename T>
constexpr std::string_view sql_type() {
  using U = unwrap_optional_t<T>;
  if constexpr (std::same_as<U, bool>) {
    return "bool";
  } else if constexpr (std::same_as<U, int>) {
    return "int4";
  } else if constexpr (std::same_as<U, int64_t>) {
    return "int8";
  } else if constexpr (std::same_as<U, oid>) {
    return "oid";
  } else if constexpr (std::same_as<U, std::string>) {
    return "text";
  } else if constexpr (std::same_as<U, std::vector<std::byte>>) {
    return "bytea";
  } else if constexpr (std::same_as<U, std::chrono::system_clock::time_point>) {
    return "timestamptz";
  } else if constexpr (IsNumericArray<U>::value) {
    return pg_array_type<typename U::value_type>();
  } else if constexpr (HasAdlToString<U>) {
    return "text";
  } else if constexpr (std::is_convertible_v<const U&, double>) {
    return "float8";
  } else {
    static_assert(false, "qr::sql_type: no Postgres type known");
  }
}

template <typename M>
Param column_param(const M& value) {
  if constexpr (IsOptionalType<M>::value) {
    return value.has_value() ? column_param(*value) : Param{std::monostate{}};
  } else if constexpr (std::is_class_v<M> && std::is_convertible_v<const M&, double>) {
    return to_param(static_cast<double>(value));
  } else {
    return to_param(value);
  }
}

struct ColumnSpec {
  std::string name;
  std::string_view type;
  bool nullable;
  bool key;
  bool serial;
  std::string_view references;
  std::string_view unit;
};

#if QR_HAS_MEMBER_NAMES
namespace detail {

template <auto Member>
consteval std::string_view member_name() {
  using Owner = typename MemberPointer<decltype(Member)>::owner;
  using Type = typename MemberPointer<decltype(Member)>::type;
  std::string_view name;
  template for (constexpr std::meta::info member : nonstatic_members_of<Owner>()) {
    if constexpr (std::meta::is_same_type(std::meta::type_of(member), ^^Type)) {
      if (&[:member:] == Member) name = std::define_static_string(std::meta::identifier_of(member));
    }
  }
  return name;
}

template <typename M>
consteval std::string_view unit_of() {
  if constexpr (std::is_class_v<M> && std::is_convertible_v<const M&, double> &&
                std::meta::has_template_arguments(^^M)) {
    return std::define_static_string(std::meta::identifier_of(std::meta::template_of(^^M)));
  } else {
    return {};
  }
}

template <typename Tag>
consteval bool annotated(std::meta::info member) {
  for (std::meta::info note : std::meta::annotations_of(member)) {
    if (std::meta::is_same_type(std::meta::remove_const(std::meta::type_of(note)), ^^Tag)) return true;
  }
  return false;
}

consteval std::meta::info referenced(std::meta::info member) {
  for (std::meta::info note : std::meta::annotations_of(member)) {
    const std::meta::info type = std::meta::remove_const(std::meta::type_of(note));
    if (std::meta::has_template_arguments(type) && std::meta::template_of(type) == ^^references_t)
      return std::meta::template_arguments_of(type)[0];
  }
  return std::meta::info{};
}

template <typename T>
consteval std::size_t leaf_count();

template <typename M>
consteval std::size_t member_leaf_count() {
  if constexpr (is_column_value<M>()) {
    return 1;
  } else {
    return leaf_count<M>();
  }
}

template <typename T>
consteval std::size_t leaf_count() {
  std::size_t count = 0;
  template for (constexpr std::meta::info base : bases_of<T>()) {
    count += leaf_count<typename [:std::meta::type_of(base):]>();
  }
  template for (constexpr std::meta::info member : nonstatic_members_of<T>()) {
    count += member_leaf_count<typename [:std::meta::type_of(member):]>();
  }
  return count;
}

template <typename T>
void collect(std::vector<ColumnSpec>& columns, const std::string& prefix);

template <typename M, std::meta::info member>
void collect_member(std::vector<ColumnSpec>& columns, const std::string& prefix) {
  const std::string name = prefix + std::define_static_string(std::meta::identifier_of(member));
  if constexpr (is_column_value<M>()) {
    constexpr std::meta::info target = referenced(member);
    std::string_view references;
    if constexpr (target != std::meta::info{}) {
      references = table_name_of<typename [:target:]>();
    }
    columns.push_back(ColumnSpec{name, sql_type<M>(), IsOptionalType<M>::value,
                                 annotated<key_t>(member), annotated<serial_t>(member), references,
                                 unit_of<M>()});
  } else {
    collect<M>(columns, name + "_");
  }
}

template <typename T>
void collect(std::vector<ColumnSpec>& columns, const std::string& prefix) {
  template for (constexpr std::meta::info base : bases_of<T>()) {
    collect<typename [:std::meta::type_of(base):]>(columns, prefix);
  }
  template for (constexpr std::meta::info member : nonstatic_members_of<T>()) {
    collect_member<typename [:std::meta::type_of(member):], member>(columns, prefix);
  }
}

}  // namespace detail

template <typename TableT, auto Member>
inline constexpr member_column_t<TableT, Member> column{detail::member_name<Member>()};

template <typename T>
const std::vector<ColumnSpec>& columns_of() {
  static const std::vector<ColumnSpec> columns = [] {
    std::vector<ColumnSpec> collected;
    detail::collect<T>(collected, "");
    if (!std::ranges::any_of(collected, &ColumnSpec::key))
      for (ColumnSpec& column : collected) column.key = !column.references.empty();
    return collected;
  }();
  return columns;
}

template <typename T, typename F>
void each_value(T& row, F& visit);

template <typename B, typename T, typename F>
void each_base_value(T& row, F& visit) {
  using Ref = std::conditional_t<std::is_const_v<T>, const B&, B&>;
  each_value(static_cast<Ref>(row), visit);
}

template <typename T, typename F>
void each_value(T& row, F& visit) {
  using Plain = std::remove_const_t<T>;
  template for (constexpr std::meta::info base : detail::bases_of<Plain>()) {
    each_base_value<typename [:std::meta::type_of(base):]>(row, visit);
  }
  template for (constexpr std::meta::info member : detail::nonstatic_members_of<Plain>()) {
    if constexpr (is_column_value<typename [:std::meta::type_of(member):]>()) {
      visit.cell(row.[:member:]);
    } else {
      each_value(row.[:member:], visit);
    }
  }
}

template <typename T>
std::size_t insert_width() {
  std::size_t width = 0;
  for (const ColumnSpec& column : columns_of<T>()) width += column.serial ? 0 : 1;
  return width;
}

template <typename T>
std::size_t insert_chunk() {
  constexpr std::size_t kMaxParams = 65535;
  const std::size_t width = insert_width<T>();
  return width == 0 ? 1 : kMaxParams / width;
}

template <typename T>
CompiledQuery compile_create() {
  const std::vector<ColumnSpec>& columns = columns_of<T>();
  std::string sql = "CREATE TABLE IF NOT EXISTS ";
  append_identifier(sql, table_name_of<T>());
  sql += " (";
  std::string key;
  std::string references;
  for (std::size_t i = 0; i < columns.size(); ++i) {
    const ColumnSpec& column = columns[i];
    if (i > 0) sql += ", ";
    append_identifier(sql, column.name);
    sql += ' ';
    sql += column.type;
    if (column.serial) sql += " GENERATED ALWAYS AS IDENTITY";
    if (!column.nullable) sql += " NOT NULL";
    if (column.key) {
      if (!key.empty()) key += ", ";
      append_identifier(key, column.name);
    }
    if (!column.references.empty()) {
      references += ", FOREIGN KEY (";
      append_identifier(references, column.name);
      references += ") REFERENCES ";
      append_identifier(references, column.references);
    }
  }
  if (!key.empty()) {
    sql += ", PRIMARY KEY (";
    sql += key;
    sql += ')';
  }
  sql += references;
  sql += ')';
  return CompiledQuery{std::move(sql), {}};
}

struct insert_params {
  const std::vector<ColumnSpec>& columns;
  std::vector<Param>& params;
  std::size_t i = 0;

  template <typename V>
  void cell(const V& value) {
    if (!columns[i].serial) params.push_back(column_param(value));
    ++i;
  }
};

template <typename T>
CompiledQuery compile_insert(std::span<const T> rows) {
  const std::vector<ColumnSpec>& columns = columns_of<T>();
  const std::size_t width = insert_width<T>();
  std::string sql = "INSERT INTO ";
  append_identifier(sql, table_name_of<T>());
  if (width == 0) {
    sql += " DEFAULT VALUES";
    return CompiledQuery{std::move(sql), {}};
  }
  sql += " (";
  bool first = true;
  for (const ColumnSpec& column : columns) {
    if (column.serial) continue;
    if (!first) sql += ", ";
    first = false;
    append_identifier(sql, column.name);
  }
  sql += ") VALUES ";

  std::vector<Param> params;
  for (std::size_t r = 0; r < rows.size(); ++r) {
    if (r > 0) sql += ", ";
    sql += '(';
    for (std::size_t c = 0; c < width; ++c) {
      if (c > 0) sql += ", ";
      sql += '$';
      sql += std::to_string(params.size() + c + 1);
    }
    sql += ')';
    insert_params visit{columns, params};
    each_value(rows[r], visit);
  }
  return CompiledQuery{std::move(sql), std::move(params)};
}

template <typename T>
CompiledQuery compile_insert_returning(const T& row) {
  CompiledQuery query = compile_insert(std::span<const T>(&row, 1));
  std::string returning;
  for (const ColumnSpec& column : columns_of<T>()) {
    if (!column.serial) continue;
    if (!returning.empty()) returning += ", ";
    append_identifier(returning, column.name);
  }
  query.sql += " RETURNING ";
  query.sql += returning;
  return query;
}

template <typename T>
CompiledQuery compile_upsert(const T& row) {
  CompiledQuery query = compile_insert(std::span<const T>(&row, 1));
  std::string target;
  std::string update;
  for (const ColumnSpec& column : columns_of<T>()) {
    if (column.key) {
      if (!target.empty()) target += ", ";
      append_identifier(target, column.name);
    } else if (!column.serial) {
      if (!update.empty()) update += ", ";
      append_identifier(update, column.name);
      update += " = EXCLUDED.";
      append_identifier(update, column.name);
    }
  }
  query.sql += " ON CONFLICT (";
  query.sql += target;
  query.sql += ')';
  query.sql += update.empty() ? " DO NOTHING" : " DO UPDATE SET " + update;
  return query;
}

template <typename T>
CompiledQuery compile_insert_missing(std::span<const T> rows) {
  CompiledQuery query = compile_insert(rows);
  std::string target;
  for (const ColumnSpec& column : columns_of<T>()) {
    if (!column.key) continue;
    if (!target.empty()) target += ", ";
    append_identifier(target, column.name);
  }
  query.sql += " ON CONFLICT (";
  query.sql += target;
  query.sql += ") DO NOTHING";
  return query;
}

template <typename T>
CompiledQuery compile_select(const std::optional<Predicate>& where) {
  const std::vector<ColumnSpec>& columns = columns_of<T>();
  const std::string_view table = table_name_of<T>();
  std::string sql = "SELECT ";
  std::string order;
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (i > 0) sql += ", ";
    append_column_ref(sql, ColumnRef{table, columns[i].name});
    if (columns[i].key) {
      if (!order.empty()) order += ", ";
      append_column_ref(order, ColumnRef{table, columns[i].name});
    }
  }
  sql += " FROM ";
  append_identifier(sql, table);

  std::vector<Param> params;
  if (where) {
    sql += " WHERE ";
    compile_predicate(*where, sql, params);
  }
  if (!order.empty()) {
    sql += " ORDER BY ";
    sql += order;
  }
  return CompiledQuery{std::move(sql), std::move(params)};
}
#else
template <typename TableT, auto Member>
extern const member_column_t<TableT, Member> column;

namespace detail {
template <typename T>
consteval std::size_t leaf_count();
}

template <typename T>
const std::vector<ColumnSpec>& columns_of();
#endif

}  // namespace qr
