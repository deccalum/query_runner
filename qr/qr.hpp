#pragma once

/// qr — a small typed SQL query builder over libpqxx.
///
/// to replace hand-written SQL strings in application code.
///
/// **Quickstart:**
/// \code
///   struct Users {
///     static constexpr std::string_view table_name = "users";
///     static constexpr qr::Column<Users, int64_t> id{};
///     static constexpr qr::Column<Users, std::string> name{};
///     static constexpr qr::Column<Users, std::optional<double>> score{"score"};
///   };
///
///   qr::Connection db(connection_string);
///
///   int64_t id = db.insert_returning<int64_t>(
///       qr::insert_into<Users>().set(Users::name, "bob").returning(Users::id));
///
///   struct UserRow { int64_t id; std::string name; };
///   std::vector<UserRow> users = db.select_as<UserRow>(
///       qr::select(Users::id, Users::name).from<Users>().where(Users::score > 1.5));
///
///   db.update(qr::update<Users>().set(Users::score, 9.0).where(Users::id == id));
///   db.remove(qr::delete_from<Users>().where(Users::id == id));
/// \endcode
///
/// See each header for the full API:
///   - table.hpp
///   - predicate.hpp (*WHERE* clauses)
///   - select.hpp/mutate.hpp (the query builders)
///   - connection.hpp (running them)
///   - stream.hpp (binary row output)
///   - compiled_query.hpp (the generated SQL, handy in tests).

// IWYU pragma: begin_exports
#include "qr/compiled_query.hpp"  // IWYU pragma: keep
#include "qr/connection.hpp"      // IWYU pragma: keep
#include "qr/mutate.hpp"          // IWYU pragma: keep
#include "qr/param.hpp"           // IWYU pragma: keep
#include "qr/predicate.hpp"       // IWYU pragma: keep
#include "qr/row.hpp"             // IWYU pragma: keep
#include "qr/schema.hpp"          // IWYU pragma: keep
#include "qr/select.hpp"          // IWYU pragma: keep
#include "qr/stream.hpp"          // IWYU pragma: keep
#include "qr/table.hpp"           // IWYU pragma: keep
// IWYU pragma: end_exports
