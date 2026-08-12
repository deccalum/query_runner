#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <qr/qr.hpp>

namespace {

enum class Color { Red, Green };

[[maybe_unused]] constexpr std::string_view to_string(Color c) {
  return c == Color::Red ? "red" : "green";
}

struct Users {
  [[maybe_unused]] static constexpr std::string_view table_name = "users";

  static constexpr qr::Column<Users, int64_t> id{"id"};
  static constexpr qr::Column<Users, std::string> name{"name"};
  static constexpr qr::Column<Users, std::optional<double>> score{"score"};
  static constexpr qr::Column<Users, Color> color{"color"};
  static constexpr qr::Column<Users, std::optional<std::chrono::system_clock::time_point>> seen_at{
      "seen_at"};
};

struct Orders {
  [[maybe_unused]] static constexpr std::string_view table_name = "orders";

  [[maybe_unused]] static constexpr qr::Column<Orders, int64_t> id{"id"};
  static constexpr qr::Column<Orders, int64_t> user_id{"user_id"};
  static constexpr qr::Column<Orders, double> total{"total"};
};

TEST(QrSelect, JoinWhereOrderLimit) {
  auto q = qr::select(Users::id, Users::name)
               .from<Users>()
               .join<Orders>(qr::on(Orders::user_id, Users::id))
               .where(Users::name == "bob" && Users::score > 1.5)
               .order_by(Orders::total, qr::SortDir::Desc)
               .order_by(Users::id)
               .limit(10);
  const qr::CompiledQuery c = qr::compile(q);
  EXPECT_EQ(c.sql,
            "SELECT users.id, users.name FROM users"
            " JOIN orders ON orders.user_id = users.id"
            " WHERE (users.name = $1 AND users.score > $2)"
            " ORDER BY orders.total DESC, users.id LIMIT 10");
  ASSERT_EQ(c.params.size(), 2u);
  EXPECT_EQ(std::get<std::string>(c.params[0]), "bob");
  EXPECT_EQ(std::get<double>(c.params[1]), 1.5);
}

TEST(QrSelect, CountPlain) {
  auto plain = qr::select(Users::id).from<Users>();
  EXPECT_EQ(qr::compile_count(plain).sql, "SELECT COUNT(*) FROM users");
}

TEST(QrSelect, CountDistinct) {
  auto distinct = qr::select(Users::id).distinct().from<Users>().where(Users::color == Color::Red);
  const qr::CompiledQuery c = qr::compile_count(distinct);
  EXPECT_EQ(c.sql, "SELECT COUNT(DISTINCT users.id) FROM users WHERE users.color = $1");
  ASSERT_EQ(c.params.size(), 1u);
  EXPECT_EQ(std::get<std::string>(c.params[0]), "red");
}

TEST(QrIn, NumericArrayLiteral) {
  const std::vector<int64_t> ids{1, 2, 3};
  auto q = qr::delete_from<Users>().where(qr::in(Users::id, ids) && qr::is_null(Users::score));
  const qr::CompiledQuery c = qr::compile(q);
  EXPECT_EQ(c.sql, "DELETE FROM users WHERE (users.id = ANY($1::int8[]) AND users.score IS NULL)");
  ASSERT_EQ(c.params.size(), 1u);
  EXPECT_EQ(std::get<std::string>(c.params[0]), "{1,2,3}");
}

TEST(QrIn, TextArrayLiteralEscapesQuotesAndBackslashes) {
  const std::vector<std::string> names{"a\"b", "c\\d"};
  auto q = qr::select(Users::id).from<Users>().where(qr::in(Users::name, names));
  const qr::CompiledQuery c = qr::compile(q);
  EXPECT_EQ(c.sql, "SELECT users.id FROM users WHERE users.name = ANY($1::text[])");
  ASSERT_EQ(c.params.size(), 1u);
  EXPECT_EQ(std::get<std::string>(c.params[0]), "{\"a\\\"b\",\"c\\\\d\"}");
}

TEST(QrIn, EmptyRangeCompilesToEmptyArray) {
  const std::vector<int64_t> none;
  auto q = qr::delete_from<Users>().where(qr::in(Users::id, none));
  ASSERT_EQ(qr::compile(q).params.size(), 1u);
  EXPECT_EQ(std::get<std::string>(qr::compile(q).params[0]), "{}");
}

TEST(QrInsert, ReturningAndImplicitConversion) {
  auto q = qr::insert_into<Users>()
               .set(Users::name, "bob")
               .set(Users::score, 2.0)
               .set(Users::seen_at, qr::now())
               .returning(Users::id);
  const qr::CompiledQuery c = qr::compile(q);
  EXPECT_EQ(c.sql, "INSERT INTO users (name, score, seen_at) VALUES ($1, $2, now()) RETURNING id");
  ASSERT_EQ(c.params.size(), 2u);
  EXPECT_EQ(std::get<std::string>(c.params[0]), "bob");
  EXPECT_EQ(std::get<double>(c.params[1]), 2.0);
}

TEST(QrInsert, NulloptBindsAsSqlNull) {
  auto q = qr::insert_into<Users>().set(Users::score, std::nullopt);
  const qr::CompiledQuery c = qr::compile(q);
  ASSERT_EQ(c.params.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(c.params[0]));
}

TEST(QrUpsert, DoUpdateSetsExcludedAndNow) {
  auto q = qr::insert_into<Users>()
               .set(Users::name, std::string("bob"))
               .set(Users::score, 2.0)
               .on_conflict(Users::id)
               .do_update(Users::score)
               .do_update(Users::seen_at, qr::now());
  EXPECT_EQ(qr::compile(q).sql,
            "INSERT INTO users (name, score) VALUES ($1, $2)"
            " ON CONFLICT (id) DO UPDATE SET score = EXCLUDED.score, seen_at = now()");
}

TEST(QrUpsert, NoDoUpdateMeansDoNothing) {
  auto q = qr::insert_into<Users>().set(Users::name, "x").on_conflict(Users::id);
  EXPECT_EQ(qr::compile(q).sql, "INSERT INTO users (name) VALUES ($1) ON CONFLICT (id) DO NOTHING");
}

TEST(QrBulkInsert, MultipleRowsOneStatement) {
  auto q = qr::bulk_insert_into<Orders>(Orders::user_id, Orders::total);
  q.add(1, 2.5);
  q.add(2, 3.5);
  const qr::CompiledQuery c = qr::compile(q);
  EXPECT_EQ(c.sql, "INSERT INTO orders (user_id, total) VALUES ($1, $2), ($3, $4)");
  ASSERT_EQ(c.params.size(), 4u);
  EXPECT_EQ(std::get<int64_t>(c.params[2]), 2);
}

TEST(QrUpdate, SetAndWhere) {
  auto q = qr::update<Users>()
               .set(Users::score, 5)
               .set(Users::seen_at, qr::now())
               .where(Users::id == 7);
  const qr::CompiledQuery c = qr::compile(q);
  EXPECT_EQ(c.sql, "UPDATE users SET score = $1, seen_at = now() WHERE users.id = $2");
  ASSERT_EQ(c.params.size(), 2u);
  EXPECT_EQ(std::get<double>(c.params[0]), 5.0);
  EXPECT_EQ(std::get<int64_t>(c.params[1]), 7);
}

TEST(QrToParam, EnumUsesAdlToString) {
  EXPECT_EQ(std::get<std::string>(qr::to_param(Color::Green)), "green");
}

TEST(QrToParam, OptionalRoundTrips) {
  EXPECT_TRUE(std::holds_alternative<std::monostate>(qr::to_param(std::optional<int>{})));
  EXPECT_EQ(std::get<int>(qr::to_param(std::optional<int>{4})), 4);
}

TEST(QrTimestamps, EncodeIncludesUtcOffset) {
  using namespace std::chrono;
  const system_clock::time_point tp =
      sys_days{2024y / 3 / 7} + 1h + 2min + 3s + microseconds{456789};
  EXPECT_EQ(qr::to_timestamp_text(tp), "2024-03-07 01:02:03.456789+00");
}

TEST(QrTimestamps, DecodeRoundTripsThroughUtc) {
  using namespace std::chrono;
  const system_clock::time_point tp =
      sys_days{2024y / 3 / 7} + 1h + 2min + 3s + microseconds{456789};
  EXPECT_EQ(qr::parse_timestamp("2024-03-07 01:02:03.456789+00"), tp);
}

TEST(QrTimestamps, DecodeAppliesNonUtcOffset) {
  using namespace std::chrono;
  const system_clock::time_point tp =
      sys_days{2024y / 3 / 7} + 1h + 2min + 3s + microseconds{456789};
  EXPECT_EQ(qr::parse_timestamp("2024-03-07 03:02:03.456789+02"), tp);
}

TEST(QrTimestamps, DecodePadsShortFraction) {
  using namespace std::chrono;
  EXPECT_EQ(qr::parse_timestamp("2024-03-07 01:02:03.5"),
            sys_days{2024y / 3 / 7} + 1h + 2min + 3s + milliseconds{500});
}

TEST(QrPgArray, ParsesNumericElements) {
  EXPECT_EQ(qr::parse_pg_array<double>("{19.5,51}"), (std::vector<double>{19.5, 51}));
}

TEST(QrPgArray, EmptyLiteralParsesToEmptyVector) {
  EXPECT_TRUE(qr::parse_pg_array<int>("{}").empty());
}

}  // namespace
