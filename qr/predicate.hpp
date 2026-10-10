#pragma once

#include <charconv>
#include <concepts>
#include <cstdint>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "qr/param.hpp"
#include "qr/table.hpp"

namespace qr {

enum class CompareOp { Eq, Ne, Lt, Le, Gt, Ge };
enum class LogicOp { And, Or };

struct ColumnRef {
  std::string_view table_name;
  std::string_view column_name;
};

struct Comparison {
  ColumnRef column;
  CompareOp op;
  Param value;
};

struct IsNull {
  ColumnRef column;
  bool negate = false;
};

struct AnyOf {
  ColumnRef column;
  std::string_view element_type;
  std::string array_literal;
};

struct Logical;

/// A compiled, composable *WHERE*-clause fragment.
///
/// Build one with a comparison operator (`==`, `!=`, `<`, `<=`, `>`, `>=`)
/// against a `Column`, or with `is_null()`/`is_not_null()`/`in()`, then
/// combine fragments with `&&` and `||` — the result is still a `Predicate`,
/// so it nests arbitrarily deep. Pass the finished `Predicate` to
/// `.where()` on a select/update/delete query, or store it and hand it to a
/// helper function that builds several related queries.
///
/// **Example:**
/// \code
///   qr::Predicate p =
///       Users::name == "bob" && (Users::score > 1.5 || qr::is_null(Users::score));
///   auto q = qr::select(Users::id).from<Users>().where(p);
/// \endcode
struct Predicate {
  std::variant<Comparison, IsNull, AnyOf, std::shared_ptr<Logical>> node;
};

// Held behind a shared_ptr so Predicate (which Logical itself contains, via
// its two children) stays a complete, storable type.
struct Logical {
  LogicOp op;
  Predicate left;
  Predicate right;
};

/// Combines two predicates with SQL *AND*.
///
/// Compiles to `"(lhs AND rhs)"`. Chains naturally — `a && b && c` parses
/// as `(a && b) && c`, which compiles to `"((a AND b) AND c)"`.
inline Predicate operator&&(Predicate lhs, Predicate rhs) {
  return Predicate{
      std::make_shared<Logical>(Logical{LogicOp::And, std::move(lhs), std::move(rhs)})};
}

/// Same as `operator&&`, but with SQL *OR*: `"(lhs OR rhs)"`.
inline Predicate operator||(Predicate lhs, Predicate rhs) {
  return Predicate{std::make_shared<Logical>(Logical{LogicOp::Or, std::move(lhs), std::move(rhs)})};
}

template <typename TableT, typename T, typename U>
  requires std::constructible_from<unwrap_optional_t<T>, const U&>
Predicate compare(const Column<TableT, T>& col, CompareOp op, const U& value) {
  return Predicate{Comparison{ColumnRef{table_name_of<TableT>(), col.name()}, op,
                              to_param(static_cast<unwrap_optional_t<T>>(value))}};
}

/// Builds a `Predicate` comparing a column to a value.
///
/// Compiles to `"table.column = $n"` bound to `value`. Works for both plain
/// columns and `optional<T>` columns — for an optional column, `value` is
/// compared against `T` itself (there's no way to compare a column to SQL
/// *NULL* with `==`; use `is_null()`/`is_not_null()` for that, since
/// `col == std::nullopt` deliberately does not compile).
///
/// **Example:**
/// \code
///   auto q = qr::select(Users::id).from<Users>().where(Users::name == "bob");
/// \endcode
template <typename TableT, typename T, typename U>
  requires std::constructible_from<unwrap_optional_t<T>, const U&>
Predicate operator==(const Column<TableT, T>& col, const U& value) {
  return compare(col, CompareOp::Eq, value);
}

/// Same family as `operator==`, compiles to `"<>"`.
///
/// **Example:** `Users::name != "bob"`
template <typename TableT, typename T, typename U>
  requires std::constructible_from<unwrap_optional_t<T>, const U&>
Predicate operator!=(const Column<TableT, T>& col, const U& value) {
  return compare(col, CompareOp::Ne, value);
}

/// Same family as `operator==`, compiles to `"<"`.
///
/// **Example:** `Users::score < 1.5`
template <typename TableT, typename T, typename U>
  requires std::constructible_from<unwrap_optional_t<T>, const U&>
Predicate operator<(const Column<TableT, T>& col, const U& value) {
  return compare(col, CompareOp::Lt, value);
}

/// Same family as `operator==`, compiles to `"<="`.
///
/// **Example:** `Users::score <= 1.5`
template <typename TableT, typename T, typename U>
  requires std::constructible_from<unwrap_optional_t<T>, const U&>
Predicate operator<=(const Column<TableT, T>& col, const U& value) {
  return compare(col, CompareOp::Le, value);
}

/// Same family as `operator==`, compiles to `">"`.
///
/// **Example:** `Orders::total > 100.0`
template <typename TableT, typename T, typename U>
  requires std::constructible_from<unwrap_optional_t<T>, const U&>
Predicate operator>(const Column<TableT, T>& col, const U& value) {
  return compare(col, CompareOp::Gt, value);
}

/// Same family as `operator==`, compiles to `">="`.
///
/// **Example:** `Orders::total >= 100.0`
template <typename TableT, typename T, typename U>
  requires std::constructible_from<unwrap_optional_t<T>, const U&>
Predicate operator>=(const Column<TableT, T>& col, const U& value) {
  return compare(col, CompareOp::Ge, value);
}

/// Builds a `Predicate` testing whether an optional column is SQL *NULL*.
///
/// The only way to express nullness (see `operator==` above for why
/// `col == std::nullopt` isn't it).
///
/// **Example:**
/// \code
///   auto q = qr::select(Users::id).from<Users>().where(qr::is_null(Users::score));
/// \endcode
template <typename TableT, typename T>
Predicate is_null(const Column<TableT, T>& col) {
  return Predicate{IsNull{ColumnRef{table_name_of<TableT>(), col.name()}, false}};
}

/// Same as `is_null`, negated — compiles to *IS NOT NULL*.
///
/// **Example:** `qr::is_not_null(Users::score)`
template <typename TableT, typename T>
Predicate is_not_null(const Column<TableT, T>& col) {
  return Predicate{IsNull{ColumnRef{table_name_of<TableT>(), col.name()}, true}};
}

template <typename T>
consteval std::string_view pg_array_type() {
  if constexpr (std::same_as<T, bool>) {
    return "bool[]";
  } else if constexpr (std::same_as<T, int>) {
    return "int4[]";
  } else if constexpr (std::same_as<T, int64_t>) {
    return "int8[]";
  } else if constexpr (std::same_as<T, double>) {
    return "float8[]";
  } else {
    return "text[]";
  }
}

template <typename T>
void append_array_element(std::string& out, const T& value) {
  if constexpr (std::same_as<T, bool>) {
    out += value ? 't' : 'f';
  } else if constexpr (std::is_arithmetic_v<T>) {
    char buf[32];
    out.append(buf, std::to_chars(buf, buf + sizeof(buf), value).ptr);
  } else {
    const std::string text = [&] {
      if constexpr (HasAdlToString<T>) {
        return std::string(to_string(value));
      } else {
        return std::string(value);
      }
    }();
    out += '"';
    for (const char c : text) {
      if (c == '"' || c == '\\') out += '\\';
      out += c;
    }
    out += '"';
  }
}

/// Builds a `Predicate` testing column membership in a set.
///
/// Compiles to `"table.column = ANY($n::T[])"` — see Postgres's
/// [array comparison](https://www.postgresql.org/docs/current/functions-comparisons.html#id-1.5.8.30.16)
/// docs — with the whole set bound as a single array parameter, so
/// filtering by N ids costs one placeholder and one round-trip instead of N
/// separate comparisons. Accepts any range (`std::vector`, `std::span`,
/// ...) whose elements convert to the column's (unwrapped) value type. An
/// empty range compiles to an empty array, which matches nothing — the same
/// semantics as SQL's *IN ()*.
///
/// **Example:**
/// \code
///   std::vector<int64_t> ids = {1, 2, 3};
///   auto q = qr::select(Users::id).from<Users>().where(qr::in(Users::id, ids));
///   // -> "... WHERE users.id = ANY($1::int8[])", with $1 bound to "{1,2,3}"
/// \endcode
template <typename TableT, typename T, std::ranges::input_range R>
  requires std::constructible_from<unwrap_optional_t<T>, std::ranges::range_reference_t<R>>
Predicate in(const Column<TableT, T>& col, const R& values) {
  using Element = unwrap_optional_t<T>;
  std::string literal = "{";
  bool first = true;
  for (const auto& v : values) {
    if (!first) literal += ',';
    first = false;
    append_array_element(literal, static_cast<Element>(v));
  }
  literal += '}';
  return Predicate{AnyOf{ColumnRef{table_name_of<TableT>(), col.name()}, pg_array_type<Element>(),
                         std::move(literal)}};
}

/// The result of `on()` — a column-to-column equality for a *JOIN* clause.
///
/// Deliberately a distinct type from `Predicate` (which is
/// column-to-value): joining is a structural fact about the query, not a
/// row filter, and this keeps `operator==` unambiguous.
struct ColumnEq {
  ColumnRef left;
  ColumnRef right;
};

/// Builds the *ON* clause for a join.
///
/// Compiles to `"left = right"`, where both sides are columns (not bound
/// values) of the same underlying type. Pass the result to `.join<T>()` on
/// a `SelectQuery`.
///
/// **Example:**
/// \code
///   auto q = qr::select(Users::id, Orders::total)
///                .from<Users>()
///                .join<Orders>(qr::on(Orders::user_id, Users::id));
///   // -> "... FROM users JOIN orders ON orders.user_id = users.id"
/// \endcode
template <typename TableA, typename TableB, typename T>
ColumnEq on(const Column<TableA, T>& a, const Column<TableB, T>& b) {
  return ColumnEq{ColumnRef{table_name_of<TableA>(), a.name()},
                  ColumnRef{table_name_of<TableB>(), b.name()}};
}

constexpr std::string_view op_symbol(CompareOp op) {
  switch (op) {
    case CompareOp::Eq:
      return "=";
    case CompareOp::Ne:
      return "<>";
    case CompareOp::Lt:
      return "<";
    case CompareOp::Le:
      return "<=";
    case CompareOp::Gt:
      return ">";
    case CompareOp::Ge:
      return ">=";
  }
  std::unreachable();
}

inline void append_identifier(std::string& sql, std::string_view name) {
  sql += '"';
  sql += name;
  sql += '"';
}

inline void append_column_ref(std::string& sql, const ColumnRef& ref) {
  append_identifier(sql, ref.table_name);
  sql += '.';
  append_identifier(sql, ref.column_name);
}

inline void compile_predicate(const Predicate& pred, std::string& sql, std::vector<Param>& params) {
  std::visit(
      [&](const auto& node) {
        using N = std::decay_t<decltype(node)>;
        if constexpr (std::same_as<N, Comparison>) {
          append_column_ref(sql, node.column);
          sql += ' ';
          sql += op_symbol(node.op);
          sql += " $";
          params.push_back(node.value);
          sql += std::to_string(params.size());
        } else if constexpr (std::same_as<N, IsNull>) {
          append_column_ref(sql, node.column);
          sql += node.negate ? " IS NOT NULL" : " IS NULL";
        } else if constexpr (std::same_as<N, AnyOf>) {
          append_column_ref(sql, node.column);
          sql += " = ANY($";
          params.push_back(Param{node.array_literal});
          sql += std::to_string(params.size());
          sql += "::";
          sql += node.element_type;
          sql += ')';
        } else {
          sql += '(';
          compile_predicate(node->left, sql, params);
          sql += node->op == LogicOp::And ? " AND " : " OR ";
          compile_predicate(node->right, sql, params);
          sql += ')';
        }
      },
      pred.node);
}

}  // namespace qr
