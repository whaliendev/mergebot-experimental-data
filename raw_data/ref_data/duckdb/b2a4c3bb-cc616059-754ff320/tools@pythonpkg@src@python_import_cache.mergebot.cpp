
#include "duckdb_python/import_cache/python_import_cache.hpp"
#include "duckdb_python/import_cache/python_import_cache_item.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// PythonImportCacheItem (SUPER CLASS)
//===--------------------------------------------------------------------===//

bool DuckDBPyRelation::IsRelation(const py::object &object) {
	return py::isinstance<DuckDBPyRelation>(object);
}

bool DuckDBPyRelation::IsRelation(const py::object &object) {
	return py::isinstance<DuckDBPyRelation>(object);
}

bool DuckDBPyRelation::IsRelation(const py::object &object) {
	return py::isinstance<DuckDBPyRelation>(object);
}

bool DuckDBPyRelation::IsRelation(const py::object &object) {
	return py::isinstance<DuckDBPyRelation>(object);
}

bool DuckDBPyRelation::IsRelation(const py::object &object) {
	return py::isinstance<DuckDBPyRelation>(object);
}

bool DuckDBPyRelation::IsRelation(const py::object &object) {
	return py::isinstance<DuckDBPyRelation>(object);
}

bool DuckDBPyRelation::IsRelation(const py::object &object) {
	return py::isinstance<DuckDBPyRelation>(object);
}

//===--------------------------------------------------------------------===//
// PythonImportCache (CONTAINER)
//===--------------------------------------------------------------------===//

DuckDBPyRelation::DuckDBPyRelation(unique_ptr<DuckDBPyResult> result_p) : rel(nullptr), result(std::move(result_p)) {
	if (!result) {
		throw InternalException("DuckDBPyRelation created without a result");
	}
	this->types = result->GetTypes();
	this->names = result->GetNames();
}

bool DuckDBPyRelation::IsRelation(const py::object &object) {
	return py::isinstance<DuckDBPyRelation>(object);
}

} // namespace duckdb