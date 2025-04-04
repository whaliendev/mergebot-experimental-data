#include "catch.hpp"
#include "test_helpers.hpp"
using namespace duckdb;
using namespace std;
TEST_CASE("Test ALTER TABLE RENAME COLUMN", "[alter][.]") {
 unique_ptr<DuckDBResult> result;
 DuckDB db(nullptr);
 DuckDBConnection con(db);
 REQUIRE_NO_FAIL(
     con.Query("CREATE TABLE test(i INTEGER, j INTEGER)"));
 REQUIRE_NO_FAIL(
     con.Query("ALTER TABLE test RENAME COLUMN i TO k"));
 result = con.Query(
     "SELECT * FROM test");
 REQUIRE(result->column_count() == 2);
 REQUIRE(result->names[0] == "k");
 REQUIRE(result->names[1] == "j");
 REQUIRE_NO_FAIL(con.Query("DROP TABLE IF EXISTS test"));
}
TEST_CASE("Test ALTER TABLE RENAME COLUMN with transactions", "[alter]") {
 unique_ptr<DuckDBResult> result;
 DuckDB db(nullptr);
 DuckDBConnection con(db);
 DuckDBConnection con2(db);
 REQUIRE_NO_FAIL(
     con.Query("CREATE TABLE test(i INTEGER, j INTEGER)"));
 REQUIRE_NO_FAIL(con.Query("START TRANSACTION"));
 REQUIRE_NO_FAIL(con2.Query("START TRANSACTION"));
 REQUIRE_NO_FAIL(
     con.Query("ALTER TABLE test RENAME COLUMN i TO k"));
 REQUIRE_FAIL(con.Query("SELECT i FROM test"));
 REQUIRE_NO_FAIL(con.Query("SELECT k FROM test"));
 REQUIRE_NO_FAIL(con2.Query("SELECT i FROM test"));
 REQUIRE_FAIL(con2.Query("SELECT k FROM test"));
 REQUIRE_NO_FAIL(con.Query("COMMIT"));
 REQUIRE_NO_FAIL(con2.Query("SELECT i FROM test"));
 REQUIRE_FAIL(con2.Query("SELECT k FROM test"));
 REQUIRE_NO_FAIL(con2.Query("COMMIT"));
 REQUIRE_FAIL(con.Query("SELECT i FROM test"));
 REQUIRE_NO_FAIL(con.Query("SELECT k FROM test"));
}
TEST_CASE("Test ALTER TABLE RENAME COLUMN with rollback", "[alter]") {
 unique_ptr<DuckDBResult> result;
 DuckDB db(nullptr);
 DuckDBConnection con(db);
 REQUIRE_NO_FAIL(
     con.Query("CREATE TABLE test(i INTEGER, j INTEGER)"));
 REQUIRE_NO_FAIL(con.Query("START TRANSACTION"));
 REQUIRE_NO_FAIL(
     con.Query("ALTER TABLE test RENAME COLUMN i TO k"));
 REQUIRE_FAIL(con.Query("SELECT i FROM test"));
 REQUIRE_NO_FAIL(con.Query("SELECT k FROM test"));
 REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
 REQUIRE_NO_FAIL(con.Query("SELECT i FROM test"));
 REQUIRE_FAIL(con.Query("SELECT k FROM test"));
}
TEST_CASE("Test failure conditions of ALTER TABLE", "[alter]") {
 unique_ptr<DuckDBResult> result;
 DuckDB db(nullptr);
 DuckDBConnection con(db);
 REQUIRE_NO_FAIL(
     con.Query("CREATE TABLE test(i INTEGER, j INTEGER)"));
 REQUIRE_FAIL(
     con.Query("ALTER TABLE test RENAME COLUMN blablabla TO k"));
 REQUIRE_FAIL(
     con.Query("ALTER TABLE test RENAME COLUMN i TO j"));
 REQUIRE_NO_FAIL(
     con.Query("SELECT i, j FROM test"));
}
