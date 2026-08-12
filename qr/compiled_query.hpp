#pragma once

#include <string>
#include <vector>

#include "qr/param.hpp"

namespace qr {

/// The result of compiling any query builder into runnable SQL.
///
/// Produced by `compile()`/`compile_count()`: parameterized SQL text with
/// `$1`, `$2`, ... in place of literal values, plus the values themselves in
/// bind order. `Connection` sends `sql` and `params` straight to libpqxx.
///
/// **Example:**
/// \code
///   qr::CompiledQuery c =
///       qr::compile(qr::select(Users::id).from<Users>().where(Users::id == 7));
///   c.sql;      // "SELECT users.id FROM users WHERE users.id = $1"
///   c.params;   // { qr::Param holding int64_t{7} }
/// \endcode
struct CompiledQuery {
  std::string sql;
  std::vector<Param> params;
};

}  // namespace qr
