
#include "duckdb_python/pandas_scan.hpp"
#include "duckdb_python/array_wrapper.hpp"
#include "duckdb_python/pandas/pandas_scan.hpp"
#include "duckdb_python/pandas/pandas_bind.hpp"
#include "duckdb_python/numpy/array_wrapper.hpp"
#include "duckdb_python/vector_conversion.hpp"
#include "utf8proc_wrapper.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb_python/numpy/numpy_scan.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb_python/pandas/column/pandas_numpy_column.hpp"
#include "duckdb/common/atomic.hpp"

namespace duckdb {

struct PandasScanFunctionData : public PyTableFunctionData {
 PandasScanFunctionData(py::handle df, idx_t row_count, vector<PandasColumnBindData> pandas_bind_data,
	                       vector<LogicalType> sql_types)
	    : df(df), row_count(row_count), lines_read(0), pandas_bind_data(std::move(pandas_bind_data)),
	      sql_types(std::move(sql_types)) {
	}
 py::handle df;
 idx_t row_count;
 atomic<idx_t> lines_read;
 vector<PandasColumnBindData> pandas_bind_data;
 vector<LogicalType> sql_types;
 
 () = delete;{
		py::gil_scoped_acquire acquire;
		pandas_bind_data.clear();
	}
};

struct PandasScanLocalState : public LocalTableFunctionState {
 PandasScanLocalState(idx_t start, idx_t end) : start(start), end(end), batch_index(0) {
	}
 
 idx_t start;
 idx_t end;
 idx_t batch_index;
 vector<column_t> column_ids;
};

struct PandasScanGlobalState : public GlobalTableFunctionState {
 explicit PandasScanGlobalState(idx_t max_threads) : position(0), batch_index(0), max_threads(max_threads) {
	}
 
 std::mutex lock;
 idx_t position;
 idx_t batch_index;
 idx_t max_threads;
 
 idx_t MaxThreads() const override {
		return max_threads;
	}
};

PandasScanFunction::PandasScanFunction()
    : TableFunction("pandas_scan", {
	get_batch_index = PandasScanGetBatchIndex;
	cardinality = PandasScanCardinality;
	table_scan_progress = PandasProgress;
	projection_pushdown = true;
}

py::object PandasScanFunction::PandasReplaceCopiedNames(const py::object &original_df) {
	auto copy_df = original_df.attr("copy")(false);
	unordered_map<string, idx_t> name_map;
	unordered_set<string> columns_seen;
	py::list column_name_list;
	auto df_columns = py::list(original_df.attr("columns"));

	for (auto &column_name_py : df_columns) {
		string column_name = py::str(column_name_py);
		// put it all lower_case
		auto column_name_low = StringUtil::Lower(column_name);
		name_map[column_name_low] = 1;
	}
	for (auto &column_name_py : df_columns) {
		const string column_name = py::str(column_name_py);
		auto column_name_low = StringUtil::Lower(column_name);
		if (columns_seen.find(column_name_low) == columns_seen.end()) {
			// `column_name` has not been seen before -> It isn't a duplicate
			column_name_list.append(column_name);
			columns_seen.insert(column_name_low);
		} else {
			// `column_name` already seen. Deduplicate by with suffix _{x} where x starts at the repetition number of
			// `column_name` If `column_name_{x}` already exists in `name_map`, increment x and try again.
			string new_column_name = column_name + "_" + std::to_string(name_map[column_name_low]);
			auto new_column_name_low = StringUtil::Lower(new_column_name);
			while (name_map.find(new_column_name_low) != name_map.end()) {
				// This name is already here due to a previous definition
				name_map[column_name_low]++;
				new_column_name = column_name + "_" + std::to_string(name_map[column_name_low]);
				new_column_name_low = StringUtil::Lower(new_column_name);
			}
			column_name_list.append(new_column_name);
			columns_seen.insert(new_column_name_low);
			name_map[column_name_low]++;
		}
	}

	copy_df.attr("columns") = column_name_list;
	return copy_df;
}

py::object PandasScanFunction::PandasReplaceCopiedNames(const py::object &original_df) {
	auto copy_df = original_df.attr("copy")(false);
	unordered_map<string, idx_t> name_map;
	unordered_set<string> columns_seen;
	py::list column_name_list;
	auto df_columns = py::list(original_df.attr("columns"));

	for (auto &column_name_py : df_columns) {
		string column_name = py::str(column_name_py);
		// put it all lower_case
		auto column_name_low = StringUtil::Lower(column_name);
		name_map[column_name_low] = 1;
	}
	for (auto &column_name_py : df_columns) {
		const string column_name = py::str(column_name_py);
		auto column_name_low = StringUtil::Lower(column_name);
		if (columns_seen.find(column_name_low) == columns_seen.end()) {
			// `column_name` has not been seen before -> It isn't a duplicate
			column_name_list.append(column_name);
			columns_seen.insert(column_name_low);
		} else {
			// `column_name` already seen. Deduplicate by with suffix _{x} where x starts at the repetition number of
			// `column_name` If `column_name_{x}` already exists in `name_map`, increment x and try again.
			string new_column_name = column_name + "_" + std::to_string(name_map[column_name_low]);
			auto new_column_name_low = StringUtil::Lower(new_column_name);
			while (name_map.find(new_column_name_low) != name_map.end()) {
				// This name is already here due to a previous definition
				name_map[column_name_low]++;
				new_column_name = column_name + "_" + std::to_string(name_map[column_name_low]);
				new_column_name_low = StringUtil::Lower(new_column_name);
			}
			column_name_list.append(new_column_name);
			columns_seen.insert(new_column_name_low);
			name_map[column_name_low]++;
		}
	}

	copy_df.attr("columns") = column_name_list;
	return copy_df;
}

py::object PandasScanFunction::PandasReplaceCopiedNames(const py::object &original_df) {
	auto copy_df = original_df.attr("copy")(false);
	unordered_map<string, idx_t> name_map;
	unordered_set<string> columns_seen;
	py::list column_name_list;
	auto df_columns = py::list(original_df.attr("columns"));

	for (auto &column_name_py : df_columns) {
		string column_name = py::str(column_name_py);
		// put it all lower_case
		auto column_name_low = StringUtil::Lower(column_name);
		name_map[column_name_low] = 1;
	}
	for (auto &column_name_py : df_columns) {
		const string column_name = py::str(column_name_py);
		auto column_name_low = StringUtil::Lower(column_name);
		if (columns_seen.find(column_name_low) == columns_seen.end()) {
			// `column_name` has not been seen before -> It isn't a duplicate
			column_name_list.append(column_name);
			columns_seen.insert(column_name_low);
		} else {
			// `column_name` already seen. Deduplicate by with suffix _{x} where x starts at the repetition number of
			// `column_name` If `column_name_{x}` already exists in `name_map`, increment x and try again.
			string new_column_name = column_name + "_" + std::to_string(name_map[column_name_low]);
			auto new_column_name_low = StringUtil::Lower(new_column_name);
			while (name_map.find(new_column_name_low) != name_map.end()) {
				// This name is already here due to a previous definition
				name_map[column_name_low]++;
				new_column_name = column_name + "_" + std::to_string(name_map[column_name_low]);
				new_column_name_low = StringUtil::Lower(new_column_name);
			}
			column_name_list.append(new_column_name);
			columns_seen.insert(new_column_name_low);
			name_map[column_name_low]++;
		}
	}

	copy_df.attr("columns") = column_name_list;
	return copy_df;
}

py::object PandasScanFunction::PandasReplaceCopiedNames(const py::object &original_df) {
	auto copy_df = original_df.attr("copy")(false);
	unordered_map<string, idx_t> name_map;
	unordered_set<string> columns_seen;
	py::list column_name_list;
	auto df_columns = py::list(original_df.attr("columns"));

	for (auto &column_name_py : df_columns) {
		string column_name = py::str(column_name_py);
		// put it all lower_case
		auto column_name_low = StringUtil::Lower(column_name);
		name_map[column_name_low] = 1;
	}
	for (auto &column_name_py : df_columns) {
		const string column_name = py::str(column_name_py);
		auto column_name_low = StringUtil::Lower(column_name);
		if (columns_seen.find(column_name_low) == columns_seen.end()) {
			// `column_name` has not been seen before -> It isn't a duplicate
			column_name_list.append(column_name);
			columns_seen.insert(column_name_low);
		} else {
			// `column_name` already seen. Deduplicate by with suffix _{x} where x starts at the repetition number of
			// `column_name` If `column_name_{x}` already exists in `name_map`, increment x and try again.
			string new_column_name = column_name + "_" + std::to_string(name_map[column_name_low]);
			auto new_column_name_low = StringUtil::Lower(new_column_name);
			while (name_map.find(new_column_name_low) != name_map.end()) {
				// This name is already here due to a previous definition
				name_map[column_name_low]++;
				new_column_name = column_name + "_" + std::to_string(name_map[column_name_low]);
				new_column_name_low = StringUtil::Lower(new_column_name);
			}
			column_name_list.append(new_column_name);
			columns_seen.insert(new_column_name_low);
			name_map[column_name_low]++;
		}
	}

	copy_df.attr("columns") = column_name_list;
	return copy_df;
}

py::object PandasScanFunction::PandasReplaceCopiedNames(const py::object &original_df) {
	auto copy_df = original_df.attr("copy")(false);
	unordered_map<string, idx_t> name_map;
	unordered_set<string> columns_seen;
	py::list column_name_list;
	auto df_columns = py::list(original_df.attr("columns"));

	for (auto &column_name_py : df_columns) {
		string column_name = py::str(column_name_py);
		// put it all lower_case
		auto column_name_low = StringUtil::Lower(column_name);
		name_map[column_name_low] = 1;
	}
	for (auto &column_name_py : df_columns) {
		const string column_name = py::str(column_name_py);
		auto column_name_low = StringUtil::Lower(column_name);
		if (columns_seen.find(column_name_low) == columns_seen.end()) {
			// `column_name` has not been seen before -> It isn't a duplicate
			column_name_list.append(column_name);
			columns_seen.insert(column_name_low);
		} else {
			// `column_name` already seen. Deduplicate by with suffix _{x} where x starts at the repetition number of
			// `column_name` If `column_name_{x}` already exists in `name_map`, increment x and try again.
			string new_column_name = column_name + "_" + std::to_string(name_map[column_name_low]);
			auto new_column_name_low = StringUtil::Lower(new_column_name);
			while (name_map.find(new_column_name_low) != name_map.end()) {
				// This name is already here due to a previous definition
				name_map[column_name_low]++;
				new_column_name = column_name + "_" + std::to_string(name_map[column_name_low]);
				new_column_name_low = StringUtil::Lower(new_column_name);
			}
			column_name_list.append(new_column_name);
			columns_seen.insert(new_column_name_low);
			name_map[column_name_low]++;
		}
	}

	copy_df.attr("columns") = column_name_list;
	return copy_df;
}

py::object PandasScanFunction::PandasReplaceCopiedNames(const py::object &original_df) {
	auto copy_df = original_df.attr("copy")(false);
	unordered_map<string, idx_t> name_map;
	unordered_set<string> columns_seen;
	py::list column_name_list;
	auto df_columns = py::list(original_df.attr("columns"));

	for (auto &column_name_py : df_columns) {
		string column_name = py::str(column_name_py);
		// put it all lower_case
		auto column_name_low = StringUtil::Lower(column_name);
		name_map[column_name_low] = 1;
	}
	for (auto &column_name_py : df_columns) {
		const string column_name = py::str(column_name_py);
		auto column_name_low = StringUtil::Lower(column_name);
		if (columns_seen.find(column_name_low) == columns_seen.end()) {
			// `column_name` has not been seen before -> It isn't a duplicate
			column_name_list.append(column_name);
			columns_seen.insert(column_name_low);
		} else {
			// `column_name` already seen. Deduplicate by with suffix _{x} where x starts at the repetition number of
			// `column_name` If `column_name_{x}` already exists in `name_map`, increment x and try again.
			string new_column_name = column_name + "_" + std::to_string(name_map[column_name_low]);
			auto new_column_name_low = StringUtil::Lower(new_column_name);
			while (name_map.find(new_column_name_low) != name_map.end()) {
				// This name is already here due to a previous definition
				name_map[column_name_low]++;
				new_column_name = column_name + "_" + std::to_string(name_map[column_name_low]);
				new_column_name_low = StringUtil::Lower(new_column_name);
			}
			column_name_list.append(new_column_name);
			columns_seen.insert(new_column_name_low);
			name_map[column_name_low]++;
		}
	}

	copy_df.attr("columns") = column_name_list;
	return copy_df;
}

py::object PandasScanFunction::PandasReplaceCopiedNames(const py::object &original_df) {
	auto copy_df = original_df.attr("copy")(false);
	unordered_map<string, idx_t> name_map;
	unordered_set<string> columns_seen;
	py::list column_name_list;
	auto df_columns = py::list(original_df.attr("columns"));

	for (auto &column_name_py : df_columns) {
		string column_name = py::str(column_name_py);
		// put it all lower_case
		auto column_name_low = StringUtil::Lower(column_name);
		name_map[column_name_low] = 1;
	}
	for (auto &column_name_py : df_columns) {
		const string column_name = py::str(column_name_py);
		auto column_name_low = StringUtil::Lower(column_name);
		if (columns_seen.find(column_name_low) == columns_seen.end()) {
			// `column_name` has not been seen before -> It isn't a duplicate
			column_name_list.append(column_name);
			columns_seen.insert(column_name_low);
		} else {
			// `column_name` already seen. Deduplicate by with suffix _{x} where x starts at the repetition number of
			// `column_name` If `column_name_{x}` already exists in `name_map`, increment x and try again.
			string new_column_name = column_name + "_" + std::to_string(name_map[column_name_low]);
			auto new_column_name_low = StringUtil::Lower(new_column_name);
			while (name_map.find(new_column_name_low) != name_map.end()) {
				// This name is already here due to a previous definition
				name_map[column_name_low]++;
				new_column_name = column_name + "_" + std::to_string(name_map[column_name_low]);
				new_column_name_low = StringUtil::Lower(new_column_name);
			}
			column_name_list.append(new_column_name);
			columns_seen.insert(new_column_name_low);
			name_map[column_name_low]++;
		}
	}

	copy_df.attr("columns") = column_name_list;
	return copy_df;
}

py::object PandasScanFunction::PandasReplaceCopiedNames(const py::object &original_df) {
	auto copy_df = original_df.attr("copy")(false);
	unordered_map<string, idx_t> name_map;
	unordered_set<string> columns_seen;
	py::list column_name_list;
	auto df_columns = py::list(original_df.attr("columns"));

	for (auto &column_name_py : df_columns) {
		string column_name = py::str(column_name_py);
		// put it all lower_case
		auto column_name_low = StringUtil::Lower(column_name);
		name_map[column_name_low] = 1;
	}
	for (auto &column_name_py : df_columns) {
		const string column_name = py::str(column_name_py);
		auto column_name_low = StringUtil::Lower(column_name);
		if (columns_seen.find(column_name_low) == columns_seen.end()) {
			// `column_name` has not been seen before -> It isn't a duplicate
			column_name_list.append(column_name);
			columns_seen.insert(column_name_low);
		} else {
			// `column_name` already seen. Deduplicate by with suffix _{x} where x starts at the repetition number of
			// `column_name` If `column_name_{x}` already exists in `name_map`, increment x and try again.
			string new_column_name = column_name + "_" + std::to_string(name_map[column_name_low]);
			auto new_column_name_low = StringUtil::Lower(new_column_name);
			while (name_map.find(new_column_name_low) != name_map.end()) {
				// This name is already here due to a previous definition
				name_map[column_name_low]++;
				new_column_name = column_name + "_" + std::to_string(name_map[column_name_low]);
				new_column_name_low = StringUtil::Lower(new_column_name);
			}
			column_name_list.append(new_column_name);
			columns_seen.insert(new_column_name_low);
			name_map[column_name_low]++;
		}
	}

	copy_df.attr("columns") = column_name_list;
	return copy_df;
}

py::object PandasScanFunction::PandasReplaceCopiedNames(const py::object &original_df) {
	auto copy_df = original_df.attr("copy")(false);
	unordered_map<string, idx_t> name_map;
	unordered_set<string> columns_seen;
	py::list column_name_list;
	auto df_columns = py::list(original_df.attr("columns"));

	for (auto &column_name_py : df_columns) {
		string column_name = py::str(column_name_py);
		// put it all lower_case
		auto column_name_low = StringUtil::Lower(column_name);
		name_map[column_name_low] = 1;
	}
	for (auto &column_name_py : df_columns) {
		const string column_name = py::str(column_name_py);
		auto column_name_low = StringUtil::Lower(column_name);
		if (columns_seen.find(column_name_low) == columns_seen.end()) {
			// `column_name` has not been seen before -> It isn't a duplicate
			column_name_list.append(column_name);
			columns_seen.insert(column_name_low);
		} else {
			// `column_name` already seen. Deduplicate by with suffix _{x} where x starts at the repetition number of
			// `column_name` If `column_name_{x}` already exists in `name_map`, increment x and try again.
			string new_column_name = column_name + "_" + std::to_string(name_map[column_name_low]);
			auto new_column_name_low = StringUtil::Lower(new_column_name);
			while (name_map.find(new_column_name_low) != name_map.end()) {
				// This name is already here due to a previous definition
				name_map[column_name_low]++;
				new_column_name = column_name + "_" + std::to_string(name_map[column_name_low]);
				new_column_name_low = StringUtil::Lower(new_column_name);
			}
			column_name_list.append(new_column_name);
			columns_seen.insert(new_column_name_low);
			name_map[column_name_low]++;
		}
	}

	copy_df.attr("columns") = column_name_list;
	return copy_df;
}

py::object PandasScanFunction::PandasReplaceCopiedNames(const py::object &original_df) {
	auto copy_df = original_df.attr("copy")(false);
	unordered_map<string, idx_t> name_map;
	unordered_set<string> columns_seen;
	py::list column_name_list;
	auto df_columns = py::list(original_df.attr("columns"));

	for (auto &column_name_py : df_columns) {
		string column_name = py::str(column_name_py);
		// put it all lower_case
		auto column_name_low = StringUtil::Lower(column_name);
		name_map[column_name_low] = 1;
	}
	for (auto &column_name_py : df_columns) {
		const string column_name = py::str(column_name_py);
		auto column_name_low = StringUtil::Lower(column_name);
		if (columns_seen.find(column_name_low) == columns_seen.end()) {
			// `column_name` has not been seen before -> It isn't a duplicate
			column_name_list.append(column_name);
			columns_seen.insert(column_name_low);
		} else {
			// `column_name` already seen. Deduplicate by with suffix _{x} where x starts at the repetition number of
			// `column_name` If `column_name_{x}` already exists in `name_map`, increment x and try again.
			string new_column_name = column_name + "_" + std::to_string(name_map[column_name_low]);
			auto new_column_name_low = StringUtil::Lower(new_column_name);
			while (name_map.find(new_column_name_low) != name_map.end()) {
				// This name is already here due to a previous definition
				name_map[column_name_low]++;
				new_column_name = column_name + "_" + std::to_string(name_map[column_name_low]);
				new_column_name_low = StringUtil::Lower(new_column_name);
			}
			column_name_list.append(new_column_name);
			columns_seen.insert(new_column_name_low);
			name_map[column_name_low]++;
		}
	}

	copy_df.attr("columns") = column_name_list;
	return copy_df;
}

} // namespace duckdb
