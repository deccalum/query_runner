#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "qr/compiled_query.hpp"
#include "qr/predicate.hpp"
#include "qr/row.hpp"
#include "qr/table.hpp"

namespace qr {

struct StoredColumn {
  std::string name;
  int64_t type;
  bool not_null;
};

struct StoredPrimaryKey {
  std::string column;
  std::string constraint;
};

struct StoredForeignKey {
  std::string column;
  std::string references;
  std::string constraint;
  bool cascade;
};

inline CompiledQuery compile_select_stored_tables() {
  return CompiledQuery{"SELECT tablename FROM pg_tables WHERE schemaname = current_schema()", {}};
}

inline CompiledQuery compile_select_stored_columns(std::string_view table) {
  return CompiledQuery{
    "SELECT a.attname, a.atttypid::int8, a.attnotnull FROM pg_attribute a "
    "WHERE a.attrelid = quote_ident($1)::regclass AND a.attnum > 0 AND NOT a.attisdropped",
    {Param{std::string(table)}}};
}

inline CompiledQuery compile_select_stored_primary_key(std::string_view table) {
  return CompiledQuery{
    "SELECT a.attname, c.conname FROM pg_constraint c "
    "JOIN pg_attribute a ON a.attrelid = c.conrelid AND a.attnum = ANY(c.conkey) "
    "WHERE c.conrelid = quote_ident($1)::regclass AND c.contype = 'p'",
    {Param{std::string(table)}}};
}

inline CompiledQuery compile_select_stored_foreign_keys(std::string_view table) {
  return CompiledQuery{
    "SELECT a.attname, c.confrelid::regclass::text, c.conname, c.confdeltype = 'c' FROM pg_constraint c "
    "JOIN pg_attribute a ON a.attrelid = c.conrelid AND a.attnum = c.conkey[1] "
    "WHERE c.conrelid = quote_ident($1)::regclass AND c.contype = 'f'",
    {Param{std::string(table)}}};
}

inline CompiledQuery compile_unlink_unreferenced_blobs(const std::vector<ColumnRef>& referencing) {
  std::string sql = "SELECT lo_unlink(o) FROM (SELECT oid AS o FROM pg_largeobject_metadata";
  for (const ColumnRef& reference : referencing) {
    sql += " EXCEPT SELECT ";
    append_identifier(sql, reference.column_name);
    sql += " FROM ";
    append_identifier(sql, reference.table_name);
  }
  sql += ") AS unreferenced";
  return CompiledQuery{std::move(sql), {}};
}

#if QR_HAS_MEMBER_NAMES
template <typename T>
CompiledQuery compile_select_column_type_oids() {
  const std::vector<ColumnSpec>& columns = columns_of<T>();
  std::string sql = "SELECT wanted.type FROM unnest(ARRAY[";
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (i > 0) sql += ", ";
    sql += '\'';
    sql += columns[i].type;
    sql += "'::regtype::oid::int8";
  }
  sql += "]) WITH ORDINALITY AS wanted(type, position) ORDER BY wanted.position";
  return CompiledQuery{std::move(sql), {}};
}

template <typename T>
void append_alter_table(std::string& sql) {
  sql += "ALTER TABLE ";
  append_identifier(sql, table_name_of<T>());
}

template <typename T>
CompiledQuery compile_rename_table(std::string_view from) {
  std::string sql = "ALTER TABLE ";
  append_identifier(sql, from);
  sql += " RENAME TO ";
  append_identifier(sql, table_name_of<T>());
  return CompiledQuery{std::move(sql), {}};
}

template <typename T>
CompiledQuery compile_rename_column(std::string_view from, std::string_view to) {
  std::string sql;
  append_alter_table<T>(sql);
  sql += " RENAME COLUMN ";
  append_identifier(sql, from);
  sql += " TO ";
  append_identifier(sql, to);
  return CompiledQuery{std::move(sql), {}};
}

template <typename T>
CompiledQuery compile_add_column(const ColumnSpec& column) {
  std::string sql;
  append_alter_table<T>(sql);
  sql += " ADD COLUMN ";
  append_identifier(sql, column.name);
  sql += ' ';
  sql += column.type;
  if (column.serial) sql += " GENERATED ALWAYS AS IDENTITY";
  return CompiledQuery{std::move(sql), {}};
}

template <typename T>
CompiledQuery compile_retype_column(const ColumnSpec& column) {
  std::string sql;
  append_alter_table<T>(sql);
  sql += " ALTER COLUMN ";
  append_identifier(sql, column.name);
  sql += " TYPE ";
  sql += column.type;
  sql += " USING ";
  append_identifier(sql, column.name);
  sql += "::";
  sql += column.type;
  return CompiledQuery{std::move(sql), {}};
}

template <typename T>
CompiledQuery compile_set_not_null(std::string_view column) {
  std::string sql;
  append_alter_table<T>(sql);
  sql += " ALTER COLUMN ";
  append_identifier(sql, column);
  sql += " SET NOT NULL";
  return CompiledQuery{std::move(sql), {}};
}

template <typename T>
CompiledQuery compile_drop_not_null(std::string_view column) {
  std::string sql;
  append_alter_table<T>(sql);
  sql += " ALTER COLUMN ";
  append_identifier(sql, column);
  sql += " DROP NOT NULL";
  return CompiledQuery{std::move(sql), {}};
}

template <typename T>
CompiledQuery compile_delete_null_rows(std::string_view column) {
  std::string sql = "DELETE FROM ";
  append_identifier(sql, table_name_of<T>());
  sql += " WHERE ";
  append_identifier(sql, column);
  sql += " IS NULL";
  return CompiledQuery{std::move(sql), {}};
}

template <typename T>
CompiledQuery compile_drop_column(std::string_view column) {
  std::string sql;
  append_alter_table<T>(sql);
  sql += " DROP COLUMN ";
  append_identifier(sql, column);
  return CompiledQuery{std::move(sql), {}};
}

template <typename T>
CompiledQuery compile_drop_constraint(std::string_view constraint) {
  std::string sql;
  append_alter_table<T>(sql);
  sql += " DROP CONSTRAINT ";
  append_identifier(sql, constraint);
  sql += " CASCADE";
  return CompiledQuery{std::move(sql), {}};
}

template <typename T>
CompiledQuery compile_add_primary_key() {
  std::string sql;
  append_alter_table<T>(sql);
  sql += " ADD PRIMARY KEY (";
  bool first = true;
  for (const ColumnSpec& column : columns_of<T>()) {
    if (!column.key) continue;
    if (!first) sql += ", ";
    first = false;
    append_identifier(sql, column.name);
  }
  sql += ')';
  return CompiledQuery{std::move(sql), {}};
}

template <typename T>
CompiledQuery compile_add_foreign_key(const ColumnSpec& column) {
  std::string sql;
  append_alter_table<T>(sql);
  sql += " ADD FOREIGN KEY (";
  append_identifier(sql, column.name);
  sql += ") REFERENCES ";
  append_identifier(sql, column.references);
  if (column.cascade) sql += " ON DELETE CASCADE";
  return CompiledQuery{std::move(sql), {}};
}
#endif

}  // namespace qr
