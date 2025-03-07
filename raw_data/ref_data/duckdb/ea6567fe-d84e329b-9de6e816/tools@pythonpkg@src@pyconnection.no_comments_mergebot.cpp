#include "duckdb_python/pyconnection.hpp"
#include "duckdb_python/pyconnection/pyconnection.hpp"
#include "duckdb/catalog/default/default_types.hpp"
#include "duckdb/common/arrow/arrow.hpp"
#include "duckdb/common/enums/file_compression_type.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/table/read_csv.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/db_instance_cache.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/prepared_statement.hpp"
#include "duckdb/main/relation/read_csv_relation.hpp"
#include "duckdb/main/relation/read_json_relation.hpp"
#include "duckdb/main/relation/value_relation.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb_python/arrow_array_stream.hpp"
#include "duckdb_python/pandas_scan.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb_python/arrow/arrow_array_stream.hpp"
#include "duckdb_python/map.hpp"
#include "duckdb_python/pandas/pandas_scan.hpp"
#include "duckdb_python/pyrelation.hpp"
#include "duckdb_python/pyresult.hpp"
#include "duckdb_python/python_conversion.hpp"
#include "duckdb_python/jupyter_progress_bar_display.hpp"
#include "duckdb_python/pyfilesystem.hpp"
#include "duckdb_python/filesystem_object.hpp"
#include <random>
namespace duckdb {
PythonEnvironmentType DuckDBPyConnection::environment = PythonEnvironmentType::NORMAL;
DBInstanceCache instance_cache;
PythonEnvironmentType DuckDBPyConnection::environment = PythonEnvironmentType::NORMAL;
PythonEnvironmentType DuckDBPyConnection::environment = PythonEnvironmentType::NORMAL;
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
static bool IsArrowBackedDataFrame(const py::object &df) {
 auto &import_cache = *DuckDBPyConnection::ImportCache();
 D_ASSERT(py::isinstance(df, import_cache.pandas().DataFrame()));
 py::list dtypes = df.attr("dtypes");
 if (dtypes.empty()) {
  return false;
 }
 auto arrow_dtype =
     py::module_::import("pandas").attr("core").attr("arrays").attr("arrow").attr("dtype").attr("ArrowDtype");
 for (auto &dtype : dtypes) {
  if (!py::isinstance(dtype, arrow_dtype)) {
   return false;
  }
 }
 return true;
}
py::object ArrowTableFromDataframe(const py::object &df) {
 py::list names = df.attr("columns");
 auto getter = df.attr("__getitem__");
 py::list array_list;
 for (auto &name : names) {
  py::object column = getter(name);
  auto array = column.attr("array").attr("__arrow_array__")();
  array_list.append(array);
 }
 return py::module_::import("pyarrow").attr("lib").attr("Table").attr("from_arrays")(array_list,
                                                                                     py::arg("names") = names);
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
static void InitializeConnectionMethods(py::class_<DuckDBPyConnection, shared_ptr<DuckDBPyConnection>> &m) {
 m.def("cursor", &DuckDBPyConnection::Cursor, "Create a duplicate of the current connection")
     .def("register_filesystem", &DuckDBPyConnection::RegisterFilesystem, "Register a fsspec compliant filesystem",
          py::arg("filesystem"))
     .def("unregister_filesystem", &DuckDBPyConnection::UnregisterFilesystem, "Unregister a filesystem",
          py::arg("name"))
     .def("list_filesystems", &DuckDBPyConnection::ListFilesystems,
          "List registered filesystems, including builtin ones")
     .def("filesystem_is_registered", &DuckDBPyConnection::FileSystemIsRegistered,
          "Check if a filesystem with the provided name is currently registered", py::arg("name"))
     .def("duplicate", &DuckDBPyConnection::Cursor, "Create a duplicate of the current connection")
     .def("execute", &DuckDBPyConnection::Execute,
          "Execute the given SQL query, optionally using prepared statements with parameters set", py::arg("query"),
          py::arg("parameters") = py::none(), py::arg("multiple_parameter_sets") = false)
     .def("executemany", &DuckDBPyConnection::ExecuteMany,
          "Execute the given prepared statement multiple times using the list of parameter sets in parameters",
          py::arg("query"), py::arg("parameters") = py::none())
     .def("close", &DuckDBPyConnection::Close, "Close the connection")
     .def("fetchone", &DuckDBPyConnection::FetchOne, "Fetch a single row from a result following execute")
     .def("fetchmany", &DuckDBPyConnection::FetchMany, "Fetch the next set of rows from a result following execute",
          py::arg("size") = 1)
     .def("fetchall", &DuckDBPyConnection::FetchAll, "Fetch all rows from a result following execute")
     .def("fetchnumpy", &DuckDBPyConnection::FetchNumpy, "Fetch a result as list of NumPy arrays following execute")
     .def("fetchdf", &DuckDBPyConnection::FetchDF, "Fetch a result as DataFrame following execute()", py::kw_only(),
          py::arg("date_as_object") = false)
     .def("fetch_df", &DuckDBPyConnection::FetchDF, "Fetch a result as DataFrame following execute()", py::kw_only(),
          py::arg("date_as_object") = false)
     .def("fetch_df_chunk", &DuckDBPyConnection::FetchDFChunk,
          "Fetch a chunk of the result as Data.Frame following execute()", py::arg("vectors_per_chunk") = 1,
          py::kw_only(), py::arg("date_as_object") = false)
     .def("df", &DuckDBPyConnection::FetchDF, "Fetch a result as DataFrame following execute()", py::kw_only(),
          py::arg("date_as_object") = false)
     .def("pl", &DuckDBPyConnection::FetchPolars, "Fetch a result as Polars DataFrame following execute()",
          py::arg("chunk_size") = 1000000)
     .def("fetch_arrow_table", &DuckDBPyConnection::FetchArrow, "Fetch a result as Arrow table following execute()",
          py::arg("chunk_size") = 1000000)
     .def("fetch_record_batch", &DuckDBPyConnection::FetchRecordBatchReader,
          "Fetch an Arrow RecordBatchReader following execute()", py::arg("chunk_size") = 1000000)
     .def("arrow", &DuckDBPyConnection::FetchArrow, "Fetch a result as Arrow table following execute()",
          py::arg("chunk_size") = 1000000)
     .def("torch", &DuckDBPyConnection::FetchPyTorch,
          "Fetch a result as dict of PyTorch Tensors following execute()")
     .def("tf", &DuckDBPyConnection::FetchTF, "Fetch a result as dict of TensorFlow Tensors following execute()")
     .def("begin", &DuckDBPyConnection::Begin, "Start a new transaction")
     .def("commit", &DuckDBPyConnection::Commit, "Commit changes performed within a transaction")
     .def("rollback", &DuckDBPyConnection::Rollback, "Roll back changes performed within a transaction")
     .def("append", &DuckDBPyConnection::Append, "Append the passed Data.Frame to the named table",
          py::arg("table_name"), py::arg("df"))
     .def("register", &DuckDBPyConnection::RegisterPythonObject,
          "Register the passed Python Object value for querying with a view", py::arg("view_name"),
          py::arg("python_object"))
     .def("unregister", &DuckDBPyConnection::UnregisterPythonObject, "Unregister the view name",
          py::arg("view_name"))
     .def("table", &DuckDBPyConnection::Table, "Create a relation object for the name'd table",
          py::arg("table_name"))
     .def("view", &DuckDBPyConnection::View, "Create a relation object for the name'd view", py::arg("view_name"))
     .def("values", &DuckDBPyConnection::Values, "Create a relation object from the passed values",
          py::arg("values"))
     .def("table_function", &DuckDBPyConnection::TableFunction,
          "Create a relation object from the name'd table function with given parameters", py::arg("name"),
          py::arg("parameters") = py::none())
     .def("read_json", &DuckDBPyConnection::ReadJSON, "Create a relation object from the JSON file in 'name'",
          py::arg("name"), py::kw_only(), py::arg("columns") = py::none(), py::arg("sample_size") = py::none(),
          py::arg("maximum_depth") = py::none());
 DefineMethod({"sql", "query", "from_query"}, m, &DuckDBPyConnection::RunQuery,
              "Run a SQL query. If it is a SELECT statement, create a relation object from the given SQL query, "
              "otherwise run the query as-is.",
              py::arg("query"), py::arg("alias") = "query_relation");
 DefineMethod({"read_csv", "from_csv_auto"}, m, &DuckDBPyConnection::ReadCSV,
              "Create a relation object from the CSV file in 'name'", py::arg("name"), py::kw_only(),
              py::arg("header") = py::none(), py::arg("compression") = py::none(), py::arg("sep") = py::none(),
              py::arg("delimiter") = py::none(), py::arg("dtype") = py::none(), py::arg("na_values") = py::none(),
              py::arg("skiprows") = py::none(), py::arg("quotechar") = py::none(),
              py::arg("escapechar") = py::none(), py::arg("encoding") = py::none(), py::arg("parallel") = py::none(),
              py::arg("date_format") = py::none(), py::arg("timestamp_format") = py::none(),
              py::arg("sample_size") = py::none(), py::arg("all_varchar") = py::none(),
              py::arg("normalize_names") = py::none(), py::arg("filename") = py::none());
 m.def("from_df", &DuckDBPyConnection::FromDF, "Create a relation object from the Data.Frame in df",
       py::arg("df") = py::none())
     .def("from_arrow", &DuckDBPyConnection::FromArrow, "Create a relation object from an Arrow object",
          py::arg("arrow_object"));
 DefineMethod({"from_parquet", "read_parquet"}, m, &DuckDBPyConnection::FromParquet,
              "Create a relation object from the Parquet files in file_glob", py::arg("file_glob"),
              py::arg("binary_as_string") = false, py::kw_only(), py::arg("file_row_number") = false,
              py::arg("filename") = false, py::arg("hive_partitioning") = false, py::arg("union_by_name") = false,
              py::arg("compression") = py::none());
 DefineMethod({"from_parquet", "read_parquet"}, m, &DuckDBPyConnection::FromParquets,
              "Create a relation object from the Parquet files in file_globs", py::arg("file_globs"),
              py::arg("binary_as_string") = false, py::kw_only(), py::arg("file_row_number") = false,
              py::arg("filename") = false, py::arg("hive_partitioning") = false, py::arg("union_by_name") = false,
              py::arg("compression") = py::none());
 m.def("from_substrait", &DuckDBPyConnection::FromSubstrait, "Create a query object from protobuf plan",
       py::arg("proto"))
     .def("get_substrait", &DuckDBPyConnection::GetSubstrait, "Serialize a query to protobuf", py::arg("query"),
          py::kw_only(), py::arg("enable_optimizer") = true)
     .def("get_substrait_json", &DuckDBPyConnection::GetSubstraitJSON,
          "Serialize a query to protobuf on the JSON format", py::arg("query"), py::kw_only(),
          py::arg("enable_optimizer") = true)
     .def("from_substrait_json", &DuckDBPyConnection::FromSubstraitJSON,
          "Create a query object from a JSON protobuf plan", py::arg("json"))
     .def("get_table_names", &DuckDBPyConnection::GetTableNames, "Extract the required table names from a query",
          py::arg("query"))
     .def_property_readonly("description", &DuckDBPyConnection::GetDescription,
                            "Get result set attributes, mainly column names")
     .def("install_extension", &DuckDBPyConnection::InstallExtension, "Install an extension by name",
          py::arg("extension"), py::kw_only(), py::arg("force_install") = false)
     .def("load_extension", &DuckDBPyConnection::LoadExtension, "Load an installed extension", py::arg("extension"));
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
void Numpy::Scan(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
 D_ASSERT(bind_data.numpy_col->Backend() == PandasColumnBackend::NUMPY);
 auto &numpy_col = (PandasNumpyColumn &)*bind_data.numpy_col;
 auto &array = numpy_col.array;
 switch (bind_data.numpy_type) {
 case NumpyNullableType::BOOL:
  ScanPandasMasked<bool>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_8:
  ScanPandasMasked<uint8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_16:
  ScanPandasMasked<uint16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_32:
  ScanPandasMasked<uint32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::UINT_64:
  ScanPandasMasked<uint64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_8:
  ScanPandasMasked<int8_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_16:
  ScanPandasMasked<int16_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_32:
  ScanPandasMasked<int32_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::INT_64:
  ScanPandasMasked<int64_t>(bind_data, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_32:
  ScanPandasFpColumn<float>((float *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::FLOAT_64:
  ScanPandasFpColumn<double>((double *)array.data(), numpy_col.stride, count, offset, out);
  break;
 case NumpyNullableType::DATETIME:
 case NumpyNullableType::DATETIME_TZ: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
  }
  break;
 }
 case NumpyNullableType::TIMEDELTA: {
  auto src_ptr = (int64_t *)array.data();
  auto tgt_ptr = FlatVector::GetData<interval_t>(out);
  auto &mask = FlatVector::Validity(out);
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = offset + row;
   if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
    mask.SetInvalid(row);
    continue;
   }
   int64_t micro = src_ptr[source_idx] / 1000;
   int64_t days = micro / Interval::MICROS_PER_DAY;
   micro = micro % Interval::MICROS_PER_DAY;
   int64_t months = days / Interval::DAYS_PER_MONTH;
   days = days % Interval::DAYS_PER_MONTH;
   interval_t interval;
   interval.months = months;
   interval.days = days;
   interval.micros = micro;
   tgt_ptr[row] = interval;
  }
  break;
 }
 case NumpyNullableType::OBJECT: {
  auto src_ptr = (PyObject **)array.data();
  if (out.GetType().id() != LogicalTypeId::VARCHAR) {
   return ScanPandasObjectColumn(bind_data, src_ptr, count, offset, out);
  }
  auto tgt_ptr = FlatVector::GetData<string_t>(out);
  auto &out_mask = FlatVector::Validity(out);
  unique_ptr<PythonGILWrapper> gil;
  auto &import_cache = *DuckDBPyConnection::ImportCache();
  auto stride = numpy_col.stride;
  for (idx_t row = 0; row < count; row++) {
   auto source_idx = stride / sizeof(PyObject *) * (row + offset);
   PyObject *val = src_ptr[source_idx];
   if (bind_data.numpy_type == NumpyNullableType::OBJECT && !PyUnicode_CheckExact(val)) {
    if (val == Py_None) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (import_cache.pandas().libs.NAType.IsLoaded()) {
     auto val_type = Py_TYPE(val);
     auto na_type = (PyTypeObject *)import_cache.pandas().libs.NAType().ptr();
     if (val_type == na_type) {
      out_mask.SetInvalid(row);
      continue;
     }
    }
    if (py::isinstance<py::float_>(val) && std::isnan(PyFloat_AsDouble(val))) {
     out_mask.SetInvalid(row);
     continue;
    }
    if (!py::isinstance<py::str>(val)) {
     if (!gil) {
      gil = bind_data.object_str_val.GetLock();
     }
     bind_data.object_str_val.AssignInternal<PyObject>(
         [](py::str &obj, PyObject &new_val) {
          py::handle object_handle = &new_val;
          obj = py::str(object_handle);
         },
         *val, *gil);
     val = (PyObject *)bind_data.object_str_val.GetPointerTop()->ptr();
    }
   }
   if (!PyUnicode_CheckExact(val)) {
    out_mask.SetInvalid(row);
    continue;
   }
   if (PyUnicode_IS_COMPACT_ASCII(val)) {
    tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
   } else {
    auto ascii_obj = (PyASCIIObject *)val;
    auto unicode_obj = (PyCompactUnicodeObject *)val;
    if (unicode_obj->utf8) {
     tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
    } else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
     auto kind = PyUnicode_KIND(val);
     switch (kind) {
     case PyUnicode_1BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_2BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     case PyUnicode_4BYTE_KIND:
      tgt_ptr[row] =
          DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
      break;
     default:
      throw NotImplementedException(
          "Unsupported typekind constant %d for Python Unicode Compact decode", kind);
     }
    } else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode not ready legacy string");
    } else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
     throw InvalidInputException("Unsupported: decode ready legacy string");
    } else {
     throw InvalidInputException("Unsupported string type: no clue what this string is");
    }
   }
  }
  break;
 }
 case NumpyNullableType::CATEGORY: {
  switch (out.GetType().InternalType()) {
  case PhysicalType::UINT8:
   ScanPandasCategory<uint8_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT16:
   ScanPandasCategory<uint16_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  case PhysicalType::UINT32:
   ScanPandasCategory<uint32_t>(array, count, offset, out, bind_data.internal_categorical_type);
   break;
  default:
   throw InternalException("Invalid Physical Type for ENUMs");
  }
  break;
 }
 default:
  throw NotImplementedException("Unsupported pandas type");
 }
}
}
