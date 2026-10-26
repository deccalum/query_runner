#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <pqxx/blob>
#include <pqxx/pqxx>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "qr/mutate.hpp"
#include "qr/param.hpp"
#include "qr/predicate.hpp"
#include "qr/row.hpp"
#include "qr/select.hpp"

namespace  qr {

// Parse a Postgres array text literal like "{19.5,51,65}" (or "{}") into its
// elements. Postgres emits numeric arrays without quoting, comma-separated.
template <typename U>
std::vector<U> parse_pg_array(std::string_view text) {
  std::vector<U> out;
  const std::size_t open = text.find('{');
  const std::size_t close = text.rfind('}');
  const std::string_view body = text.substr(open + 1, close - open - 1);
  for (std::size_t pos = 0; pos < body.size();) {
    std::size_t comma = body.find(',', pos);
    if (comma == std::string_view::npos) comma = body.size();
    U value{};
    std::from_chars(body.data() + pos, body.data() + comma, value);
    out.push_back(value);
    pos = comma + 1;
  }
  return out;
}

template <typename T, typename Field>
T extract_field(const Field& f) {
  if constexpr (IsOptionalType<T>::value) {
    using Inner = typename T::value_type;
    return f.is_null() ? T{} : T{extract_field<Inner>(f)};
  } else if constexpr (HasAdlFromColumn<T>) {
    return from_column(extract_field<adl_column_t<T>>(f), std::type_identity<T>{});
  } else if constexpr (HasAdlFromString<T>) {
    return from_string(f.template as<std::string>(), std::type_identity<T>{});
  } else if constexpr (std::same_as<T, std::chrono::system_clock::time_point>) {
    return parse_time_point(f.template as<std::string>());
  } else if constexpr (IsNumericArray<T>::value) {
    return parse_pg_array<typename T::value_type>(f.template as<std::string>());
  } else if constexpr (std::same_as<T, oid>) {
    return static_cast<oid>(f.template as<int64_t>());
  } else {
    return f.template as<T>();
  }
}

template <typename M, typename Field>
void assign_field(M& target, const Field& field) {
  if constexpr (IsOptionalType<M>::value) {
    if (field.is_null()) {
      target.reset();
    } else {
      target.emplace();
      assign_field(*target, field);
    }
  } else if constexpr (std::is_class_v<M> && std::is_convertible_v<const M&, double>) {
    target = extract_field<double>(field);
  } else {
    target = extract_field<M>(field);
  }
}

#if QR_HAS_MEMBER_NAMES
template <typename Row>
struct field_assigner {
  const Row& result_row;
  int i = 0;

  template <typename V>
  void cell(V& value) {
    assign_field(value, result_row[i]);
    ++i;
  }

  template <typename S>
  void group(std::optional<S>& target) {
    constexpr std::size_t width = detail::leaf_count<S>();
    bool present = false;
    for (std::size_t j = 0; j < width; ++j) present = present || !result_row[i + static_cast<int>(j)].is_null();
    if (present) {
      target.emplace();
      each_value(*target, *this);
    } else {
      target.reset();
      i += static_cast<int>(width);
    }
  }

  template <typename... S>
  void group(std::variant<S...>& target) {
    template for (constexpr std::size_t k : std::views::iota(0uz, sizeof...(S))) {
      constexpr std::size_t width = detail::leaf_count<std::variant_alternative_t<k, std::variant<S...>>>();
      bool present = false;
      for (std::size_t j = 0; j < width; ++j) present = present || !result_row[i + static_cast<int>(j)].is_null();
      if (present) {
        each_value(target.template emplace<k>(), *this);
      } else {
        i += static_cast<int>(width);
      }
    }
  }
};

template <typename T, typename Row>
T decode_row(const Row& result_row) {
  T row{};
  field_assigner<Row> visit{result_row};
  each_value(row, visit);
  return row;
}
#endif

inline pqxx::params to_pqxx_params(const std::vector<Param>& params) {
  pqxx::params out;
  out.reserve(params.size());
  for (const auto& p : params) {
    std::visit([&](const auto& v) {
      if constexpr (std::same_as<std::decay_t<decltype(v)>, std::monostate>) {
        out.append();
      } else {
        out.append(v);
      }
    }, p);
  }
  return out;
}

/// Wraps one `pqxx::connection` and runs qr query builders against it.
///
/// Construct one per process with a Postgres connection string, then pass
/// query builders from select.hpp/mutate.hpp to its methods. Every call opens
/// its own transaction and commits before returning, unless it runs inside
/// `transaction()`, which commits every call in its body together.
///
/// **Example:**
/// \code
///   qr::Connection db(connection_string);
///   auto rows = db.select_rows(qr::select(Users::id, Users::name).from<Users>(),
///                               [](int64_t id, std::string name) {
///                                 return UserRow{id, std::move(name)};
///                               });
/// \endcode
class Connection {
 public:
  /// Opens a connection using a libpq connection string (e.g.
  /// `"postgresql://user:pass@host/dbname"`).
  explicit Connection(const std::string& connection_string) : conn_(connection_string) {}

  /// The name of the connected database.
  ///
  /// for logging which DB a `Connection` is bound to.
  std::string_view dbname() const { return conn_.dbname(); }

  /// Runs a *SELECT* and maps each row through `mapper`.
  ///
  /// The parameters must match the selected columns' value types in order.
  /// Use this when the row shape needs computation beyond a plain
  /// field-by-field copy (renamed fields, derived values, ...) — for a
  /// straight aggregate-init, `select_as()` is shorter.
  ///
  /// **Example:**
  /// \code
  ///   auto rows = db.select_rows(
  ///       qr::select(Users::id, Users::score).from<Users>(),
  ///       [](int64_t id, std::optional<double> score) {
  ///         return UserSummary{id, score.value_or(0.0)};
  ///       });
  /// \endcode
  ///
  /// \return the mapped rows, one per result row.
  template <typename... Cols, typename Mapper>
  auto select_rows(const SelectQuery<Cols...>& query, Mapper&& mapper)
      -> std::vector<std::invoke_result_t<Mapper, typename Cols::value_type...>> {
    const pqxx::result result = run(compile(query));

    std::vector<std::invoke_result_t<Mapper, typename Cols::value_type...>> out;
    out.reserve(result.size());
    for (const auto& row : result) {
      out.push_back(extract_row<typename Cols::value_type...>(row, mapper,
                                                              std::index_sequence_for<Cols...>{}));
    }
    return out;
  }

  /// Overload for a single-column *SELECT* with no reshaping.
  ///
  /// Returns the column's values directly, with no mapper to write.
  ///
  /// **Example:**
  /// \code
  ///   std::vector<int64_t> ids = db.select_rows(qr::select(Users::id).from<Users>());
  /// \endcode
  ///
  /// \return the column's values, one per result row.
  template <typename Col>
  std::vector<typename Col::value_type> select_rows(const SelectQuery<Col>& query) {
    return select_rows(query, [](typename Col::value_type value) {
      return value;
    });
  }

  /// Runs a *SELECT* and aggregate-initializes one `RowT` per row.
  ///
  /// Builds `RowT{col0, col1, ...}` in selected-column order. Use this when
  /// `RowT`'s members already line up with the query's columns — it's
  /// `select_rows()` without writing the mapper lambda by hand.
  ///
  /// **Example:**
  /// \code
  ///   struct UserRow { int64_t id; std::string name; };
  ///   std::vector<UserRow> rows =
  ///       db.select_as<UserRow>(qr::select(Users::id, Users::name).from<Users>());
  /// \endcode
  ///
  /// \return one `RowT` per result row.
  template <typename RowT, typename... Cols>
  std::vector<RowT> select_as(const SelectQuery<Cols...>& query) {
    return select_rows(query, [](typename Cols::value_type... values) {
      return RowT{std::move(values)...};
    });
  }

  /// Runs a *SELECT* `COUNT(*)` instead of pulling rows back.
  ///
  /// Uses `COUNT(DISTINCT ...)` if the query used `.distinct()`. The query's
  /// own joins/where still apply. Cheaper than `select_rows(...).size()`
  /// whenever you only need the number of matching rows.
  ///
  /// **Example:**
  /// \code
  ///   int64_t n = db.count(qr::select(Users::id).from<Users>().where(Users::score > 1.5));
  /// \endcode
  ///
  /// \return the number of matching rows.
  template <typename... Cols>
  int64_t count(const SelectQuery<Cols...>& query) {
    return run(compile_count(query)).one_field().template as<int64_t>();
  }

  /// Runs an *INSERT* and discards any result.
  ///
  /// For statements without a `.returning(...)`.
  ///
  /// **Example:**
  /// \code
  ///   db.insert(qr::insert_into<Users>().set(Users::name, "bob").set(Users::score, 4.5));
  /// \endcode
  ///
  /// \sa insert_returning()
  template <typename TableT>
  void insert(InsertQuery<TableT> query) {
    run(compile(std::move(query)));
  }

  /// Runs a `BulkInsertQuery` as one round-trip.
  ///
  /// A query with no rows added is skipped entirely.
  ///
  /// **Example:**
  /// \code
  ///   auto q = qr::bulk_insert_into<Orders>(Orders::user_id, Orders::total);
  ///   q.add(1, 2.5);
  ///   db.insert_bulk(std::move(q));
  /// \endcode
  ///
  /// \sa BulkInsertQuery::empty()
  template <typename TableT, typename... Cols>
  void insert_bulk(BulkInsertQuery<TableT, Cols...> query) {
    if (query.empty()) return;
    run(compile(std::move(query)));
  }

  /// Runs an *INSERT* built with `.returning(col)`.
  ///
  /// The usual way to get a generated id back after inserting a row.
  ///
  /// **Example:**
  /// \code
  ///   int64_t id = db.insert_returning<int64_t>(
  ///       qr::insert_into<Users>().set(Users::name, "bob").returning(Users::id));
  /// \endcode
  ///
  /// \return the value of the `.returning(col)` column, as `R`.
  template <typename R, typename TableT>
  R insert_returning(InsertQuery<TableT> query) {
    return run(compile(std::move(query))).one_field().template as<R>();
  }

  /// Runs an *UPDATE*.
  ///
  /// **Example:**
  /// \code
  ///   db.update(qr::update<Users>().set(Users::score, 9.0).where(Users::id == 7));
  /// \endcode
  template <typename TableT>
  void update(const UpdateQuery<TableT>& query) {
    run(compile(query));
  }

  /// Runs a *DELETE* and returns the number of rows removed.
  ///
  /// Named `remove` (not `delete`) because `delete` is a C++ keyword — pairs
  /// with `delete_from<T>()` in mutate.hpp.
  ///
  /// **Example:**
  /// \code
  ///   std::size_t n = db.remove(qr::delete_from<Users>().where(Users::id == 7));
  /// \endcode
  ///
  /// \return the number of rows removed.
  template <typename TableT>
  std::size_t remove(const DeleteQuery<TableT>& query) {
    return static_cast<std::size_t>(run(compile(query)).affected_rows());
  }

  /// Reads a Large Object (as written by `lo_from_bytea`) into memory.
  ///
  /// Large Objects are read in chunks through libpq's binary fast-path, so
  /// unlike a plain `bytea` column this has no text-protocol size limit —
  /// use it for columns storing an `oid` too big to `SELECT` as `bytea`.
  ///
  /// **Example:**
  /// \code
  ///   std::vector<std::byte> audio = db.read_blob(row_oid);
  /// \endcode
  std::vector<std::byte> read_blob(oid id) {
    std::vector<std::byte> out;
    within([&](pqxx::work& tx) {
      pqxx::blob b = pqxx::blob::open_r(tx, static_cast<pqxx::oid>(id));
      pqxx::bytes chunk;
      constexpr std::size_t kChunkSize = 8 * 1024 * 1024;
      while (b.read(chunk, kChunkSize) > 0) out.insert(out.end(), chunk.begin(), chunk.end());
    });
    return out;
  }

  std::vector<std::byte> read_blob_head(oid id, std::size_t length) {
    std::vector<std::byte> out;
    within([&](pqxx::work& tx) {
      pqxx::blob b = pqxx::blob::open_r(tx, static_cast<pqxx::oid>(id));
      pqxx::bytes head;
      b.read(head, length);
      out.assign(head.begin(), head.end());
    });
    return out;
  }

  oid write_blob(std::span<const std::byte> bytes) {
    pqxx::oid id;
    within([&](pqxx::work& tx) {
      id = pqxx::blob::create(tx);
      pqxx::blob b = pqxx::blob::open_w(tx, id);
      constexpr std::size_t kChunkSize = 8 * 1024 * 1024;
      for (std::size_t offset = 0; offset < bytes.size(); offset += kChunkSize) {
        const std::size_t length = std::min(kChunkSize, bytes.size() - offset);
        b.write(bytes.subspan(offset, length));
      }
    });
    return static_cast<oid>(id);
  }

  void remove_blob(oid id) {
    within([&](pqxx::work& tx) { pqxx::blob::remove(tx, static_cast<pqxx::oid>(id)); });
  }

  template <typename F>
  void transaction(F&& body) {
    open_.emplace(conn_);
    body();
    open_->commit();
    open_.reset();
  }

  template <typename T>
  void create();

  template <typename T>
  void insert(const T& row);

  template <typename T>
  void insert(const std::vector<T>& rows);

  template <typename T>
  void insert_missing(const std::vector<T>& rows);

  template <typename T>
  int64_t insert_returning(const T& row);

  template <typename T>
  void upsert(const T& row);

  template <typename T>
  std::vector<T> select();

  template <typename T>
  std::vector<T> select(Predicate where);

  template <typename T>
  std::vector<T> run_rows(const CompiledQuery& compiled);

  pqxx::result run(const CompiledQuery& compiled) {
    pqxx::result result;
    within([&](pqxx::work& tx) { result = exec(tx, compiled); });
    return result;
  }

 private:
  template <typename... FieldTypes, typename Mapper, typename Row, size_t... I>
  static auto extract_row(const Row& row, Mapper&& mapper, std::index_sequence<I...>) {
    return mapper(extract_field<FieldTypes>(row[I])...);
  }

  static pqxx::result exec(pqxx::work& tx, const CompiledQuery& compiled) {
    return tx.exec(compiled.sql, to_pqxx_params(compiled.params));
  }

  template <typename F>
  void within(F&& step) {
    if (open_) {
      step(*open_);
      return;
    }
    pqxx::work tx(conn_);
    step(tx);
    tx.commit();
  }

  pqxx::connection conn_;
  std::optional<pqxx::work> open_;
};

#if QR_HAS_MEMBER_NAMES
template <typename T>
void Connection::create() {
  run(compile_create<T>());
}

template <typename T>
void Connection::insert(const T& row) {
  run(compile_insert(std::span<const T>(&row, 1), false));
}

template <typename T>
void Connection::insert(const std::vector<T>& rows) {
  const std::size_t chunk = insert_chunk<T>();
  within([&](pqxx::work& tx) {
    for (std::size_t first = 0; first < rows.size(); first += chunk) {
      const std::size_t count = std::min(chunk, rows.size() - first);
      exec(tx, compile_insert(std::span<const T>(rows).subspan(first, count), false));
    }
  });
}

template <typename T>
void Connection::insert_missing(const std::vector<T>& rows) {
  const std::size_t chunk = insert_chunk<T>();
  within([&](pqxx::work& tx) {
    for (std::size_t first = 0; first < rows.size(); first += chunk) {
      const std::size_t count = std::min(chunk, rows.size() - first);
      exec(tx, compile_insert_missing(std::span<const T>(rows).subspan(first, count)));
    }
  });
}

template <typename T>
int64_t Connection::insert_returning(const T& row) {
  return run(compile_insert_returning(row)).one_field().template as<int64_t>();
}

template <typename T>
void Connection::upsert(const T& row) {
  run(compile_upsert(row));
}

template <typename T>
std::vector<T> Connection::run_rows(const CompiledQuery& compiled) {
  const pqxx::result result = run(compiled);
  std::vector<T> out;
  out.reserve(result.size());
  for (const auto& result_row : result) {
    if constexpr (is_column_value<T>()) out.push_back(extract_field<T>(result_row[0]));
    else out.push_back(decode_row<T>(result_row));
  }
  return out;
}

template <typename T>
std::vector<T> Connection::select() {
  return run_rows<T>(compile_select<T>(std::nullopt));
}

template <typename T>
std::vector<T> Connection::select(Predicate where) {
  return run_rows<T>(compile_select<T>(std::move(where)));
}
#endif

}  // namespace qr
