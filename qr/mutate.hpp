#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "qr/compiled_query.hpp"
#include "qr/predicate.hpp"
#include "qr/table.hpp"

namespace qr {

/// A marker meaning "the database's time".
///
/// Used as the value passed to `set()`/`do_update()` so a timestamp column is
/// stamped at write time rather than from C++'s clock. Spelled as a named
/// call (`qr::now()`) rather than a string, so consumer code never has to
/// embed the SQL literal `"now()"` by hand. See Postgres's
/// [`now()`](https://www.postgresql.org/docs/current/functions-datetime.html)
/// docs.
struct NowTag {};

/// Returns a `NowTag`.
///
/// Pass it to `set()`/`do_update()` where you'd write `now()` in SQL
/// directly.
///
/// **Example:**
/// \code
///   qr::update<Orders>().set(Orders::updated_at, qr::now()).where(Orders::id == 7);
/// \endcode
constexpr NowTag now() { return {}; }

using AssignedValue = std::variant<Param, NowTag>;

struct ExcludedTag {};

struct ConflictUpdate {
  std::string_view column;
  std::variant<ExcludedTag, NowTag> value;
};

/// An *INSERT* query under construction.
///
/// Build one with `insert_into<T>()`, add one `set(col, value)` call per
/// column, then either run it directly (`Connection::insert`), ask for a
/// generated value back (`Connection::insert_returning`), or turn it into an
/// upsert with `on_conflict()`/`do_update()`.
///
/// **Example:**
/// \code
///   auto q = qr::insert_into<Users>()
///                .set(Users::name, "bob")
///                .set(Users::score, 4.5)
///                .returning(Users::id);
///   int64_t new_id = db.insert_returning<int64_t>(std::move(q));
/// \endcode
template <typename TableT>
class InsertQuery {
 public:
  /// Adds one column to the *INSERT*.
  ///
  /// `value` converts the same way a comparison operator's right-hand side
  /// does — for an `optional<T>` column, pass a bare `T` or `std::nullopt`
  /// directly with no need to wrap it yourself.
  ///
  /// **Example:**
  /// \code
  ///   qr::insert_into<Users>().set(Users::name, "bob").set(Users::score, std::nullopt);
  /// \endcode
  template <typename T, typename U>
    requires std::constructible_from<T, U&&>
  InsertQuery& set(const Column<TableT, T>& col, U&& value) {
    columns_.push_back(col.column_name);
    values_.push_back(to_param(static_cast<T>(std::forward<U>(value))));
    return *this;
  }

  /// Overload of `set()` for stamping a column with the database's current time.
  ///
  /// **Example:** `qr::insert_into<Orders>().set(Orders::created_at, qr::now());`
  template <typename T>
  InsertQuery& set(const Column<TableT, T>& col, NowTag) {
    columns_.push_back(col.column_name);
    values_.push_back(NowTag{});
    return *this;
  }

  /// Turns this *INSERT* into an upsert.
  ///
  /// On a unique/primary-key conflict on the given column, fall back to an
  /// update instead of erroring out. Follow with one or more `do_update()`
  /// calls to say what gets updated on conflict — with none, a conflict
  /// becomes a silent no-op (Postgres's
  /// [*ON CONFLICT ... DO NOTHING*](https://www.postgresql.org/docs/current/sql-insert.html#SQL-ON-CONFLICT)).
  ///
  /// **Example:**
  /// \code
  ///   qr::insert_into<Users>()
  ///       .set(Users::id, 7)
  ///       .set(Users::score, 4.5)
  ///       .on_conflict(Users::id)
  ///       .do_update(Users::score);
  /// \endcode
  template <typename... Ts>
  InsertQuery& on_conflict(const Column<TableT, Ts>&... cols) {
    (conflict_target_.push_back(cols.column_name), ...);
    return *this;
  }

  /// On conflict, sets `col` to the value that was about to be inserted.
  ///
  /// Only meaningful after `on_conflict()` — see its example above.
  template <typename T>
  InsertQuery& do_update(const Column<TableT, T>& col) {
    conflict_updates_.push_back(ConflictUpdate{col.column_name, ExcludedTag{}});
    return *this;
  }

  /// On conflict, stamps `col` with the database's current time.
  ///
  /// Instead of the incoming value — the usual choice for an `updated_at`
  /// column on an upsert.
  ///
  /// **Example:**
  /// \code
  ///   qr::insert_into<Orders>()
  ///       .set(Orders::id, 7)
  ///       .set(Orders::total, 12.0)
  ///       .on_conflict(Orders::id)
  ///       .do_update(Orders::total)
  ///       .do_update(Orders::updated_at, qr::now());
  /// \endcode
  template <typename T>
  InsertQuery& do_update(const Column<TableT, T>& col, NowTag) {
    conflict_updates_.push_back(ConflictUpdate{col.column_name, NowTag{}});
    return *this;
  }

  /// Adds `RETURNING col`.
  ///
  /// Pair with `Connection::insert_returning<R>()` to get a generated value
  /// (typically the new row's id) back from the *INSERT*.
  ///
  /// **Example:** `qr::insert_into<Users>().set(Users::name, "bob").returning(Users::id);`
  template <typename T>
  InsertQuery& returning(const Column<TableT, T>& col) {
    returning_ = col.column_name;
    return *this;
  }

  std::vector<std::string_view> columns_;
  std::vector<AssignedValue> values_;
  std::vector<std::string_view> conflict_target_;
  std::vector<ConflictUpdate> conflict_updates_;
  std::optional<std::string_view> returning_;
};

/// Starts an *INSERT* query into `TableT`.
///
/// See `InsertQuery` for the full chain
/// (`set`/`on_conflict`/`do_update`/`returning`).
template <typename TableT>
InsertQuery<TableT> insert_into() {
  return InsertQuery<TableT>{};
}

/// Turns an `InsertQuery` into parameterized SQL + bound params.
///
/// See `compile(const SelectQuery&)` for why this is useful outside of
/// `Connection`.
///
/// **Example:**
/// \code
///   qr::compile(qr::insert_into<Users>().set(Users::name, "bob")).sql;
///   // -> "INSERT INTO users (name) VALUES ($1)"
/// \endcode
template <typename TableT>
CompiledQuery compile(InsertQuery<TableT> q) {
  std::string sql = "INSERT INTO ";
  sql += TableT::table_name;
  sql += " (";
  for (size_t i = 0; i < q.columns_.size(); ++i) {
    if (i > 0) sql += ", ";
    sql += q.columns_[i];
  }
  sql += ") VALUES (";
  std::vector<Param> params;
  for (size_t i = 0; i < q.values_.size(); ++i) {
    if (i > 0) sql += ", ";
    if (std::holds_alternative<NowTag>(q.values_[i])) {
      sql += "now()";
    } else {
      params.push_back(std::move(std::get<Param>(q.values_[i])));
      sql += '$';
      sql += std::to_string(params.size());
    }
  }
  sql += ')';
  if (!q.conflict_target_.empty()) {
    sql += " ON CONFLICT (";
    for (size_t i = 0; i < q.conflict_target_.size(); ++i) {
      if (i > 0) sql += ", ";
      sql += q.conflict_target_[i];
    }
    sql += ')';
    if (q.conflict_updates_.empty()) {
      sql += " DO NOTHING";
    } else {
      sql += " DO UPDATE SET ";
      for (size_t i = 0; i < q.conflict_updates_.size(); ++i) {
        if (i > 0) sql += ", ";
        sql += q.conflict_updates_[i].column;
        sql += " = ";
        if (std::holds_alternative<NowTag>(q.conflict_updates_[i].value)) {
          sql += "now()";
        } else {
          sql += "EXCLUDED.";
          sql += q.conflict_updates_[i].column;
        }
      }
    }
  }
  if (q.returning_) {
    sql += " RETURNING ";
    sql += *q.returning_;
  }
  return CompiledQuery{std::move(sql), std::move(params)};
}

/// Many rows, one *INSERT* statement.
///
/// Fix the column list up front, then call `add()` once per row — each
/// `add()` appends one VALUES tuple, so a caller inserting N rows pays one
/// round-trip instead of N. Run it with `Connection::insert_bulk()`.
///
/// **Example:**
/// \code
///   auto q = qr::bulk_insert_into<Orders>(Orders::user_id, Orders::total);
///   for (const auto& [user_id, total] : pending) q.add(user_id, total);
///   db.insert_bulk(std::move(q));
/// \endcode
template <typename TableT, typename... Cols>
class BulkInsertQuery {
 public:
  explicit BulkInsertQuery(Cols... cols) : columns_{cols.column_name...} {}

  /// Appends one row.
  ///
  /// One value per column, in the order passed to `bulk_insert_into()`. See
  /// the class example above.
  BulkInsertQuery& add(const typename Cols::value_type&... values) {
    (params_.push_back(to_param(values)), ...);
    ++rows_;
    return *this;
  }

  /// True until `add()` has been called at least once.
  ///
  /// `Connection::insert_bulk()` checks this to avoid sending an empty
  /// *INSERT*.
  ///
  /// \return whether any rows have been added yet.
  bool empty() const { return rows_ == 0; }

  static constexpr size_t columns_per_row = sizeof...(Cols);

  std::array<std::string_view, sizeof...(Cols)> columns_;
  std::vector<Param> params_;
  size_t rows_ = 0;
};

/// Starts a `BulkInsertQuery` into `TableT` over the given columns.
///
/// See `BulkInsertQuery` for the full example.
template <typename TableT, typename... Cols>
BulkInsertQuery<TableT, Cols...> bulk_insert_into(Cols... cols) {
  return BulkInsertQuery<TableT, Cols...>(cols...);
}

/// Turns a `BulkInsertQuery` into parameterized SQL + bound params.
///
/// One `$n` per value across every row, and one VALUES tuple per row.
///
/// **Example:**
/// \code
///   auto q = qr::bulk_insert_into<Orders>(Orders::user_id, Orders::total);
///   q.add(1, 2.5);
///   q.add(2, 3.5);
///   qr::compile(q).sql;
///   // -> "INSERT INTO orders (user_id, total) VALUES ($1, $2), ($3, $4)"
/// \endcode
template <typename TableT, typename... Cols>
CompiledQuery compile(BulkInsertQuery<TableT, Cols...> q) {
  std::string sql = "INSERT INTO ";
  sql += TableT::table_name;
  sql += " (";
  for (size_t i = 0; i < q.columns_.size(); ++i) {
    if (i > 0) sql += ", ";
    sql += q.columns_[i];
  }
  sql += ") VALUES ";

  size_t placeholder = 1;
  for (size_t r = 0; r < q.rows_; ++r) {
    if (r > 0) sql += ", ";
    sql += '(';
    for (size_t c = 0; c < q.columns_per_row; ++c) {
      if (c > 0) sql += ", ";
      sql += '$';
      sql += std::to_string(placeholder++);
    }
    sql += ')';
  }

  return CompiledQuery{std::move(sql), std::move(q.params_)};
}

/// An *UPDATE* query under construction.
///
/// Build one with `update<T>()`, add one `set(col, value)` call per column,
/// and usually a `.where(...)` — an *UPDATE* with no *WHERE* touches every
/// row in the table.
///
/// **Example:**
/// \code
///   auto q = qr::update<Users>().set(Users::score, 9.0).where(Users::id == 7);
///   db.update(q);
/// \endcode
template <typename TableT>
class UpdateQuery {
 public:
  struct Assignment {
    std::string_view column;
    AssignedValue value;
  };

  /// Adds one column assignment.
  ///
  /// Converts `value` the same way `InsertQuery::set()` does — see its doc
  /// for the `optional<T>` shorthand.
  ///
  /// **Example:** `qr::update<Users>().set(Users::score, 9.0);`
  template <typename T, typename U>
    requires std::constructible_from<T, U&&>
  UpdateQuery& set(const Column<TableT, T>& col, U&& value) {
    sets_.push_back(Assignment{col.column_name, to_param(static_cast<T>(std::forward<U>(value)))});
    return *this;
  }

  /// Overload of `set()` for stamping a column with the database's current time.
  ///
  /// **Example:** `qr::update<Orders>().set(Orders::updated_at, qr::now());`
  template <typename T>
  UpdateQuery& set(const Column<TableT, T>& col, NowTag) {
    sets_.push_back(Assignment{col.column_name, NowTag{}});
    return *this;
  }

  /// Sets the *WHERE* clause.
  ///
  /// See `Predicate`, and the class example above for why setting one
  /// usually matters.
  UpdateQuery& where(Predicate p) {
    where_ = std::move(p);
    return *this;
  }

  std::vector<Assignment> sets_;
  std::optional<Predicate> where_;
};

/// Starts an *UPDATE* query against `TableT`.
///
/// See `UpdateQuery` for the full chain and why `.where()` matters.
template <typename TableT>
UpdateQuery<TableT> update() {
  return UpdateQuery<TableT>{};
}

/// Turns an `UpdateQuery` into parameterized SQL + bound params.
///
/// **Example:**
/// \code
///   qr::compile(qr::update<Users>().set(Users::score, 9.0).where(Users::id == 7)).sql;
///   // -> "UPDATE users SET score = $1 WHERE users.id = $2"
/// \endcode
template <typename TableT>
CompiledQuery compile(const UpdateQuery<TableT>& q) {
  std::string sql = "UPDATE ";
  sql += TableT::table_name;
  sql += " SET ";

  std::vector<Param> params;
  for (size_t i = 0; i < q.sets_.size(); ++i) {
    if (i > 0) sql += ", ";
    sql += q.sets_[i].column;
    sql += " = ";
    if (std::holds_alternative<NowTag>(q.sets_[i].value)) {
      sql += "now()";
    } else {
      params.push_back(std::get<Param>(q.sets_[i].value));
      sql += '$';
      sql += std::to_string(params.size());
    }
  }

  if (q.where_) {
    sql += " WHERE ";
    compile_predicate(*q.where_, sql, params);
  }

  return CompiledQuery{std::move(sql), std::move(params)};
}

/// A *DELETE* query under construction.
///
/// Build one with `delete_from<T>()` and usually a `.where(...)` — a
/// *DELETE* with no *WHERE* removes every row in the table.
///
/// **Example:**
/// \code
///   auto q = qr::delete_from<Users>().where(Users::id == 7);
///   std::size_t removed = db.remove(q);
/// \endcode
template <typename TableT>
class DeleteQuery {
 public:
  /// Sets the *WHERE* clause.
  ///
  /// See `Predicate`, and the class example above.
  DeleteQuery& where(Predicate p) {
    where_ = std::move(p);
    return *this;
  }

  std::optional<Predicate> where_;
};

/// Starts a *DELETE* query against `TableT`.
///
/// See `DeleteQuery` for the full example.
template <typename TableT>
DeleteQuery<TableT> delete_from() {
  return DeleteQuery<TableT>{};
}

/// Turns a `DeleteQuery` into parameterized SQL + bound params.
///
/// **Example:**
/// \code
///   qr::compile(qr::delete_from<Users>().where(Users::id == 7)).sql;
///   // -> "DELETE FROM users WHERE users.id = $1"
/// \endcode
template <typename TableT>
CompiledQuery compile(const DeleteQuery<TableT>& q) {
  std::string sql = "DELETE FROM ";
  sql += TableT::table_name;

  std::vector<Param> params;
  if (q.where_) {
    sql += " WHERE ";
    compile_predicate(*q.where_, sql, params);
  }

  return CompiledQuery{std::move(sql), std::move(params)};
}

}  // namespace qr
