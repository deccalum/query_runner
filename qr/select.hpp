#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "qr/compiled_query.hpp"
#include "qr/predicate.hpp"
#include "qr/table.hpp"

namespace qr {

/// Sort direction for `order_by()`.
///
/// `Asc` is the default if you don't pass one.
///
/// **Example:** `qr::select(Users::id).from<Users>().order_by(Users::id, qr::SortDir::Desc);`
enum class SortDir { Asc, Desc };

struct OrderTerm {
  ColumnRef column;
  SortDir dir;
};

/// A *SELECT* query under construction.
///
/// Build one with `select(cols...)`, then chain `.from<T>()`, and
/// optionally `.join<T>(on)` / `.where(pred)` / `.order_by(col)` /
/// `.limit(n)` — each returns `*this`, so calls read top-to-bottom in
/// clause order. Nothing touches the database until you pass the finished
/// query to a `Connection` method (`select_rows`/`select_as`/`count`) or
/// compile it yourself with `compile()`/`compile_count()`.
///
/// **Example:**
/// \code
///   auto q = qr::select(Users::id, Users::name)
///                .from<Users>()
///                .join<Orders>(qr::on(Orders::user_id, Users::id))
///                .where(Users::score > 1.5)
///                .order_by(Users::id)
///                .limit(20);
///
///   struct UserRow { int64_t id; std::string name; };
///   std::vector<UserRow> rows = db.select_as<UserRow>(q);
/// \endcode
template <typename... Cols>
class SelectQuery {
 public:
  struct Join {
    std::string_view table;
    ColumnEq on;
  };

  explicit SelectQuery(Cols... cols) : columns_(cols...) {}

  /// *SELECT DISTINCT* instead of *SELECT*.
  ///
  /// Also changes what `Connection::count()` computes for this query — see
  /// its doc.
  ///
  /// **Example:** `qr::select(Users::name).distinct().from<Users>();`
  SelectQuery& distinct() {
    distinct_ = true;
    return *this;
  }

  /// Sets the *FROM* table.
  ///
  /// Every `SelectQuery` needs exactly one — call this once, right after
  /// `select(cols...)`.
  ///
  /// **Example:** `qr::select(Users::id).from<Users>();`
  template <typename TableT>
  SelectQuery& from() {
    from_table_ = TableT::table_name;
    return *this;
  }

  /// Adds an *INNER JOIN* against `TableT`.
  ///
  /// Joined on the given column equality (build that with `qr::on()`).
  /// Call once per table you're joining; call order matches the SQL *JOIN*
  /// order.
  ///
  /// **Example:**
  /// \code
  ///   qr::select(Users::id).from<Users>().join<Orders>(qr::on(Orders::user_id, Users::id));
  /// \endcode
  template <typename TableT>
  SelectQuery& join(ColumnEq on_clause) {
    joins_.push_back(Join{TableT::table_name, on_clause});
    return *this;
  }

  /// Sets the *WHERE* clause from a `Predicate`.
  ///
  /// See `Predicate`, and the comparison operators / `is_null()` / `in()`
  /// that build one.
  ///
  /// **Example:** `qr::select(Users::id).from<Users>().where(Users::name == "bob");`
  SelectQuery& where(Predicate p) {
    where_ = std::move(p);
    return *this;
  }

  /// Appends one *ORDER BY* term.
  ///
  /// Call multiple times for a multi-column sort — later calls append,
  /// they don't replace earlier ones.
  ///
  /// **Example:**
  /// \code
  ///   qr::select(Orders::id)
  ///       .from<Orders>()
  ///       .order_by(Orders::total, qr::SortDir::Desc)
  ///       .order_by(Orders::id);
  ///   // -> "... ORDER BY orders.total DESC, orders.id"
  /// \endcode
  template <typename TableT, typename T>
  SelectQuery& order_by(const Column<TableT, T>& col, SortDir dir = SortDir::Asc) {
    order_by_.push_back(OrderTerm{ColumnRef{TableT::table_name, col.column_name}, dir});
    return *this;
  }

  /// Sets `LIMIT n`.
  ///
  /// **Example:** `qr::select(Users::id).from<Users>().limit(1);`
  SelectQuery& limit(size_t n) {
    limit_ = n;
    return *this;
  }

  std::tuple<Cols...> columns_;
  std::string_view from_table_;
  std::vector<Join> joins_;
  std::optional<Predicate> where_;
  std::vector<OrderTerm> order_by_;
  std::optional<size_t> limit_;
  bool distinct_ = false;
};

/// Starts a *SELECT* query over the given columns.
///
/// The entry point for every read. The columns can come from more than one
/// table (for a join); pass the result through `.from<T>()` and the rest of
/// `SelectQuery`'s chain before running it. See `SelectQuery` for the full
/// example.
///
/// **Example:** `auto q = qr::select(Users::id, Users::name).from<Users>();`
template <typename... Cols>
SelectQuery<Cols...> select(Cols... cols) {
  return SelectQuery<Cols...>(cols...);
}

template <typename... Cols>
void append_column_list(std::string& sql, const std::tuple<Cols...>& columns) {
  bool first = true;
  std::apply(
      [&](const auto&... cols) {
        ((sql += (first ? "" : ", "), first = false,
          append_column_ref(sql, ColumnRef{cols.table_name(), cols.column_name})),
         ...);
      },
      columns);
}

template <typename... Cols>
void append_source(std::string& sql, std::vector<Param>& params, const SelectQuery<Cols...>& q) {
  sql += " FROM ";
  sql += q.from_table_;

  for (const auto& j : q.joins_) {
    sql += " JOIN ";
    sql += j.table;
    sql += " ON ";
    append_column_ref(sql, j.on.left);
    sql += " = ";
    append_column_ref(sql, j.on.right);
  }

  if (q.where_) {
    sql += " WHERE ";
    compile_predicate(*q.where_, sql, params);
  }
}

/// Turns a `SelectQuery` into parameterized SQL + bound params.
///
/// Doesn't touch a database — `Connection` uses this internally, but it's
/// also a convenient seam for testing query construction in isolation.
///
/// **Example:**
/// \code
///   qr::CompiledQuery c =
///       qr::compile(qr::select(Users::id).from<Users>().where(Users::id == 7));
///   c.sql;      // "SELECT users.id FROM users WHERE users.id = $1"
///   c.params;   // { qr::Param holding int64_t{7} }
/// \endcode
template <typename... Cols>
CompiledQuery compile(const SelectQuery<Cols...>& q) {
  std::string sql = "SELECT ";
  if (q.distinct_) sql += "DISTINCT ";
  append_column_list(sql, q.columns_);

  std::vector<Param> params;
  append_source(sql, params, q);

  if (!q.order_by_.empty()) {
    sql += " ORDER BY ";
    bool first = true;
    for (const OrderTerm& term : q.order_by_) {
      if (!first) sql += ", ";
      first = false;
      append_column_ref(sql, term.column);
      if (term.dir == SortDir::Desc) sql += " DESC";
    }
  }

  if (q.limit_) {
    sql += " LIMIT ";
    sql += std::to_string(*q.limit_);
  }

  return CompiledQuery{std::move(sql), std::move(params)};
}

/// Like `compile()`, but produces `"SELECT COUNT(*) ..."`.
///
/// Or `"SELECT COUNT(DISTINCT ...)"` when `.distinct()` was set — instead
/// of selecting the columns. Used by `Connection::count()` to get a row
/// count without pulling any rows over the wire.
///
/// **Example:**
/// \code
///   qr::compile_count(qr::select(Users::id).from<Users>()).sql;
///   // -> "SELECT COUNT(*) FROM users"
/// \endcode
template <typename... Cols>
CompiledQuery compile_count(const SelectQuery<Cols...>& q) {
  std::string sql = "SELECT COUNT(";
  if (q.distinct_) {
    sql += "DISTINCT ";
    if constexpr (sizeof...(Cols) > 1) {
      sql += '(';
      append_column_list(sql, q.columns_);
      sql += ')';
    } else {
      append_column_list(sql, q.columns_);
    }
  } else {
    sql += '*';
  }
  sql += ')';

  std::vector<Param> params;
  append_source(sql, params, q);

  return CompiledQuery{std::move(sql), std::move(params)};
}

}  // namespace qr
