
#include "catch.hpp"
#include "test_helpers.hpp"

using namespace duckdb;
using namespace std;

<<<<<<< ours
TEST_CASE("Test ALTER TABLE RENAME COLUMN", "[alter]") {
||||||| base
TEST_CASE("Test ALTER TABLE", "[alter]") {
=======
TEST_CASE("Test ALTER TABLE", "[alter][.]") {
>>>>>>> theirs
	unique_ptr<DuckDBResult> result;
	DuckDB db(nullptr);
	DuckDBConnection con(db);

<<<<<<< ours
	// CREATE TABLE AND ALTER IT TO RENAME A COLUMN
||||||| base
	// CREATE TABLE AND ALTER IT TO ADD ONE COLUMN
=======
	return;

	// CREATE TABLE AND ALTER IT TO ADD ONE COLUMN
>>>>>>> theirs
	REQUIRE_NO_FAIL(
<<<<<<< ours
	    con.Query("CREATE TABLE test(i INTEGER, j INTEGER)"));
	REQUIRE_NO_FAIL(
	    con.Query("ALTER TABLE test RENAME COLUMN i TO k"));
	
	result = con.Query(
	    "SELECT * FROM test");
	REQUIRE(result->column_count() == 2);
||||||| base
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
>>>>>>> theirs
	REQUIRE(result->names[0] == "k");
	REQUIRE(result->names[1] == "j");

<<<<<<< ours
	// DROP TABLE IF EXISTS
	REQUIRE_NO_FAIL(con.Query("DROP TABLE IF EXISTS test"));
}

TEST_CASE("Test ALTER TABLE RENAME COLUMN with transactions", "[alter]") {
	unique_ptr<DuckDBResult> result;
	DuckDB db(nullptr);
	DuckDBConnection con(db);
	DuckDBConnection con2(db);

	// CREATE TABLE
	REQUIRE_NO_FAIL(
	    con.Query("CREATE TABLE test(i INTEGER, j INTEGER)"));
||||||| base
	//ALTER TABLE TO ADD ONE COLUMN
	REQUIRE_NO_FAIL(
	    con.Query("ALTER TABLE integers ADD COLUMN l INTEGER"));
	result = con.Query(
	    "SELECT i, j, k, l FROM test");
	REQUIRE(result->names.size() == 4);
	REQUIRE(result->names[0] == "i");
	REQUIRE(result->names[0] == "j");
	REQUIRE(result->names[0] == "k");
	REQUIRE(result->names[0] == "l");
=======
	// ALTER TABLE TO ADD ONE COLUMN
	REQUIRE_NO_FAIL(con.Query("ALTER TABLE integers ADD COLUMN l INTEGER"));
	result = con.Query("SELECT i, j, k, l FROM test");
	REQUIRE(result->names.size() == 4);
	REQUIRE(result->names[0] == "i");
	REQUIRE(result->names[0] == "j");
	REQUIRE(result->names[0] == "k");
	REQUIRE(result->names[0] == "l");
>>>>>>> theirs

<<<<<<< ours
||||||| base
	//ALTER TABLE TO DROP ONE COLUMN
	/*REQUIRE_NO_FAIL(
	    con.Query("ALTER TABLE integers DROP COLUMN l"));
	result = con.Query(
	    "SELECT i, j, k, l FROM test");
	REQUIRE(result->names.size() == 3);
	REQUIRE(result->names[0] == "i");
	REQUIRE(result->names[0] == "j");
	REQUIRE(result->names[0] == "k");
	REQUIRE(result->names[0] == "l");
=======
	// ALTER TABLE TO DROP ONE COLUMN
	/*REQUIRE_NO_FAIL(
	    con.Query("ALTER TABLE integers DROP COLUMN l"));
	result = con.Query(
	    "SELECT i, j, k, l FROM test");
	REQUIRE(result->names.size() == 3);
	REQUIRE(result->names[0] == "i");
	REQUIRE(result->names[0] == "j");
	REQUIRE(result->names[0] == "k");
	REQUIRE(result->names[0] == "l");
>>>>>>> theirs

	// start two transactions
	REQUIRE_NO_FAIL(con.Query("START TRANSACTION"));
	REQUIRE_NO_FAIL(con2.Query("START TRANSACTION"));

	// rename column in first transaction
	REQUIRE_NO_FAIL(
	    con.Query("ALTER TABLE test RENAME COLUMN i TO k"));

	// first transaction should see the new name
	REQUIRE_FAIL(con.Query("SELECT i FROM test"));
	REQUIRE_NO_FAIL(con.Query("SELECT k FROM test"));

	// second transaction should still consider old name
	REQUIRE_NO_FAIL(con2.Query("SELECT i FROM test"));
	REQUIRE_FAIL(con2.Query("SELECT k FROM test"));

	// now commit
	REQUIRE_NO_FAIL(con.Query("COMMIT"));

	// second transaction should still see old name
	REQUIRE_NO_FAIL(con2.Query("SELECT i FROM test"));
	REQUIRE_FAIL(con2.Query("SELECT k FROM test"));

	// now rollback the second transasction
	// it should now see the new name
	REQUIRE_NO_FAIL(con2.Query("COMMIT"));

	REQUIRE_FAIL(con.Query("SELECT i FROM test"));
	REQUIRE_NO_FAIL(con.Query("SELECT k FROM test"));
}

TEST_CASE("Test ALTER TABLE RENAME COLUMN with rollback", "[alter]") {
	unique_ptr<DuckDBResult> result;
	DuckDB db(nullptr);
	DuckDBConnection con(db);

	// CREATE TABLE
	REQUIRE_NO_FAIL(
	    con.Query("CREATE TABLE test(i INTEGER, j INTEGER)"));


	// rename the column
	REQUIRE_NO_FAIL(con.Query("START TRANSACTION"));

	// rename column in first transaction
	REQUIRE_NO_FAIL(
	    con.Query("ALTER TABLE test RENAME COLUMN i TO k"));

	// now we should see the new name
	REQUIRE_FAIL(con.Query("SELECT i FROM test"));
	REQUIRE_NO_FAIL(con.Query("SELECT k FROM test"));

	// rollback
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));

	// now we should see the old name again
	REQUIRE_NO_FAIL(con.Query("SELECT i FROM test"));
	REQUIRE_FAIL(con.Query("SELECT k FROM test"));
}


TEST_CASE("Test failure conditions of ALTER TABLE", "[alter]") {
	unique_ptr<DuckDBResult> result;
	DuckDB db(nullptr);
	DuckDBConnection con(db);

	// CREATE TABLE AND ALTER IT TO RENAME A COLUMN
	REQUIRE_NO_FAIL(
	    con.Query("CREATE TABLE test(i INTEGER, j INTEGER)"));

	// rename a column that does not exist
	REQUIRE_FAIL(
	    con.Query("ALTER TABLE test RENAME COLUMN blablabla TO k"));

	// rename a column to an already existing column
	REQUIRE_FAIL(
	    con.Query("ALTER TABLE test RENAME COLUMN i TO j"));

	// after failure original columns should still be there
	REQUIRE_NO_FAIL(
	    con.Query("SELECT i, j FROM test"));
}

