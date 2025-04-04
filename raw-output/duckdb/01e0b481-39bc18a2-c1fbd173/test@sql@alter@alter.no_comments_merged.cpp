#include "catch.hpp"
#include "test_helpers.hpp"
using namespace duckdb;
using namespace std;
<<<<<<< HEAD
TEST_CASE("Test ALTER TABLE RENAME COLUMN", "[alter]") {
||||||| c1fbd173f6
TEST_CASE("Test ALTER TABLE", "[alter]") {
=======
TEST_CASE("Test ALTER TABLE", "[alter][.]") {
>>>>>>> 39bc18a2
 unique_ptr<DuckDBResult> result;
 DuckDB db(nullptr);
 DuckDBConnection con(db);
<<<<<<< HEAD
||||||| c1fbd173f6
=======
 return;
>>>>>>> 39bc18a2
 REQUIRE_NO_FAIL(
<<<<<<< HEAD
     con.Query("CREATE TABLE test(i INTEGER, j INTEGER)"));
 REQUIRE_NO_FAIL(
     con.Query("ALTER TABLE test RENAME COLUMN i TO k"));
 result = con.Query(
     "SELECT * FROM test");
 REQUIRE(result->column_count() == 2);
||||||| c1fbd173f6
     con.Query("CREATE TABLE IF NOT EXISTS test(i INTEGER, j INTEGER)"));
 REQUIRE_NO_FAIL(
     con.Query("ALTER TABLE test ADD COLUMN k INTEGER"));
 result = con.Query(
     "SELECT i, j, k FROM test");
 REQUIRE(result->names.size() == 3);
 REQUIRE(result->names[0] == "i");
 REQUIRE(result->names[0] == "j");
=======
     con.Query("CREATE TABLE IF NOT EXISTS test(i INTEGER, j INTEGER)"));
 REQUIRE_NO_FAIL(con.Query("ALTER TABLE test ADD COLUMN k INTEGER"));
 result = con.Query("SELECT i, j, k FROM test");
 REQUIRE(result->names.size() == 3);
 REQUIRE(result->names[0] == "i");
 REQUIRE(result->names[0] == "j");
>>>>>>> 39bc18a2
 REQUIRE(result->names[0] == "k");
<<<<<<< HEAD
 REQUIRE(result->names[1] == "j");
||||||| c1fbd173f6
 REQUIRE_NO_FAIL(
     con.Query("ALTER TABLE integers ADD COLUMN l INTEGER"));
 result = con.Query(
     "SELECT i, j, k, l FROM test");
 REQUIRE(result->names.size() == 4);
 REQUIRE(result->names[0] == "i");
 REQUIRE(result->names[0] == "j");
 REQUIRE(result->names[0] == "k");
 REQUIRE(result->names[0] == "l");
