#include "column_writer.hpp"
#include "duckdb.hpp"
#include "geo_parquet.hpp" #include "parquet_dbp_encoder.hpp"
#include "parquet_rle_bp_decoder.hpp"
#include "parquet_rle_bp_encoder.hpp"
#include "parquet_writer.hpp"
#ifndef DUCKDB_AMALGAMATION
#include "duckdb/common/exception.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/serializer/buffered_file_writer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/serializer/write_stream.hpp"
#include "duckdb/common/string_map_set.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/uhugeint.hpp"
#include "duckdb/execution/expression_executor.hpp"
#endif
#include "brotli/encode.h"
#include "lz4.hpp"
#include "miniz_wrapper.hpp"
#include "snappy.h"
#include "zstd.h"
namespace duckdb {
using namespace duckdb_parquet;
using namespace duckdb_miniz;
using duckdb_parquet::CompressionCodec;
using duckdb_parquet::ConvertedType;
using duckdb_parquet::Encoding;
using duckdb_parquet::FieldRepetitionType;
using duckdb_parquet::FileMetaData;
using duckdb_parquet::PageHeader;
using duckdb_parquet::PageType;
using ParquetRowGroup = duckdb_parquet::RowGroup;
using duckdb_parquet::Type;
#define PARQUET_DEFINE_VALID 65535
ColumnWriterStatistics::~ColumnWriterStatistics() {
}
bool ColumnWriterStatistics::HasStats() {
 return false;
}
string ColumnWriterStatistics::GetMin() {
 return string();
}
string ColumnWriterStatistics::GetMax() {
 return string();
}
string ColumnWriterStatistics::GetMinValue() {
 return string();
}
string ColumnWriterStatistics::GetMaxValue() {
 return string();
}
RleBpEncoder::RleBpEncoder(uint32_t bit_width)
    : byte_width((bit_width + 7) / 8), byte_count(idx_t(-1)), run_count(idx_t(-1)) {
}
void RleBpEncoder::BeginPrepare(uint32_t first_value) {
 byte_count = 0;
 run_count = 1;
 current_run_count = 1;
 last_value = first_value;
}
void RleBpEncoder::FinishRun() {
 byte_count += ParquetDecodeUtils::GetVarintSize(current_run_count << 1) + byte_width;
 current_run_count = 1;
 run_count++;
}
void RleBpEncoder::PrepareValue(uint32_t value) {
 if (value != last_value) {
  FinishRun();
  last_value = value;
 } else {
  current_run_count++;
 }
}
void RleBpEncoder::FinishPrepare() {
 FinishRun();
}
idx_t RleBpEncoder::GetByteCount() {
 D_ASSERT(byte_count != idx_t(-1));
 return byte_count;
}
void RleBpEncoder::BeginWrite(WriteStream &writer, uint32_t first_value) {
 last_value = first_value;
 current_run_count = 1;
}
void RleBpEncoder::WriteRun(WriteStream &writer) {
 ParquetDecodeUtils::VarintEncode(current_run_count << 1, writer);
 D_ASSERT(last_value >> (byte_width * 8) == 0);
 switch (byte_width) {
 case 1:
  writer.Write<uint8_t>(last_value);
  break;
 case 2:
  writer.Write<uint16_t>(last_value);
  break;
 case 3:
  writer.Write<uint8_t>(last_value & 0xFF);
  writer.Write<uint8_t>((last_value >> 8) & 0xFF);
  writer.Write<uint8_t>((last_value >> 16) & 0xFF);
  break;
 case 4:
  writer.Write<uint32_t>(last_value);
  break;
 default:
  throw InternalException("unsupported byte width for RLE encoding");
 }
 current_run_count = 1;
}
void RleBpEncoder::WriteValue(WriteStream &writer, uint32_t value) {
 if (value != last_value) {
  WriteRun(writer);
  last_value = value;
 } else {
  current_run_count++;
 }
}
void RleBpEncoder::FinishWrite(WriteStream &writer) {
 WriteRun(writer);
}
ColumnWriter::ColumnWriter(ParquetWriter &writer, idx_t schema_idx, vector<string> schema_path_p, idx_t max_repeat,
                           idx_t max_define, bool can_have_nulls)
    : writer(writer), schema_idx(schema_idx), schema_path(std::move(schema_path_p)), max_repeat(max_repeat),
      max_define(max_define), can_have_nulls(can_have_nulls) {
}
ColumnWriter::~ColumnWriter() {
}
ColumnWriterState::~ColumnWriterState() {
}
void ColumnWriter::CompressPage(MemoryStream &temp_writer, size_t &compressed_size, data_ptr_t &compressed_data,
                                unique_ptr<data_t[]> &compressed_buf) {
 switch (writer.GetCodec()) {
 case CompressionCodec::UNCOMPRESSED:
  compressed_size = temp_writer.GetPosition();
  compressed_data = temp_writer.GetData();
  break;
 case CompressionCodec::SNAPPY: {
  compressed_size = duckdb_snappy::MaxCompressedLength(temp_writer.GetPosition());
  compressed_buf = unique_ptr<data_t[]>(new data_t[compressed_size]);
  duckdb_snappy::RawCompress(const_char_ptr_cast(temp_writer.GetData()), temp_writer.GetPosition(),
                             char_ptr_cast(compressed_buf.get()), &compressed_size);
  compressed_data = compressed_buf.get();
  D_ASSERT(compressed_size <= duckdb_snappy::MaxCompressedLength(temp_writer.GetPosition()));
  break;
 }
 case CompressionCodec::LZ4_RAW: {
  compressed_size = duckdb_lz4::LZ4_compressBound(UnsafeNumericCast<int32_t>(temp_writer.GetPosition()));
  compressed_buf = unique_ptr<data_t[]>(new data_t[compressed_size]);
  compressed_size = duckdb_lz4::LZ4_compress_default(
      const_char_ptr_cast(temp_writer.GetData()), char_ptr_cast(compressed_buf.get()),
      UnsafeNumericCast<int32_t>(temp_writer.GetPosition()), UnsafeNumericCast<int32_t>(compressed_size));
  compressed_data = compressed_buf.get();
  break;
 }
 case CompressionCodec::GZIP: {
  MiniZStream s;
  compressed_size = s.MaxCompressedLength(temp_writer.GetPosition());
  compressed_buf = unique_ptr<data_t[]>(new data_t[compressed_size]);
  s.Compress(const_char_ptr_cast(temp_writer.GetData()), temp_writer.GetPosition(),
             char_ptr_cast(compressed_buf.get()), &compressed_size);
  compressed_data = compressed_buf.get();
  break;
 }
 case CompressionCodec::ZSTD: {
  compressed_size = duckdb_zstd::ZSTD_compressBound(temp_writer.GetPosition());
  compressed_buf = unique_ptr<data_t[]>(new data_t[compressed_size]);
  compressed_size = duckdb_zstd::ZSTD_compress((void *)compressed_buf.get(), compressed_size,
                                               (const void *)temp_writer.GetData(), temp_writer.GetPosition(),
                                               writer.CompressionLevel());
  compressed_data = compressed_buf.get();
  break;
 }
 case CompressionCodec::BROTLI: {
  compressed_size = duckdb_brotli::BrotliEncoderMaxCompressedSize(temp_writer.GetPosition());
  compressed_buf = unique_ptr<data_t[]>(new data_t[compressed_size]);
  duckdb_brotli::BrotliEncoderCompress(BROTLI_DEFAULT_QUALITY, BROTLI_DEFAULT_WINDOW, BROTLI_DEFAULT_MODE,
                                       temp_writer.GetPosition(), temp_writer.GetData(), &compressed_size,
                                       compressed_buf.get());
  compressed_data = compressed_buf.get();
  break;
 }
 default:
  throw InternalException("Unsupported codec for Parquet Writer");
 }
 if (compressed_size > idx_t(NumericLimits<int32_t>::Maximum())) {
  throw InternalException("Parquet writer: %d compressed page size out of range for type integer",
                          temp_writer.GetPosition());
 }
}
void ColumnWriter::HandleRepeatLevels(ColumnWriterState &state, ColumnWriterState *parent, idx_t count,
                                      idx_t max_repeat) const {
 if (!parent) {
  return;
 }
 while (state.repetition_levels.size() < parent->repetition_levels.size()) {
  state.repetition_levels.push_back(parent->repetition_levels[state.repetition_levels.size()]);
 }
}
void ColumnWriter::HandleDefineLevels(ColumnWriterState &state, ColumnWriterState *parent, const ValidityMask &validity,
                                      const idx_t count, const uint16_t define_value, const uint16_t null_value) const {
 if (parent) {
  idx_t vector_index = 0;
  while (state.definition_levels.size() < parent->definition_levels.size()) {
   idx_t current_index = state.definition_levels.size();
   if (parent->definition_levels[current_index] != PARQUET_DEFINE_VALID) {
    state.definition_levels.push_back(parent->definition_levels[current_index]);
   } else if (validity.RowIsValid(vector_index)) {
    state.definition_levels.push_back(define_value);
   } else {
    if (!can_have_nulls) {
     throw IOException("Parquet writer: map key column is not allowed to contain NULL values");
    }
    state.null_count++;
    state.definition_levels.push_back(null_value);
   }
   if (parent->is_empty.empty() || !parent->is_empty[current_index]) {
    vector_index++;
   }
  }
 } else {
  for (idx_t i = 0; i < count; i++) {
   const auto is_null = !validity.RowIsValid(i);
   state.definition_levels.emplace_back(is_null ? null_value : define_value);
   state.null_count += is_null;
  }
  if (!can_have_nulls && state.null_count != 0) {
   throw IOException("Parquet writer: map key column is not allowed to contain NULL values");
  }
 }
}
class ColumnWriterPageState {
public:
 virtual ~ColumnWriterPageState() {
 }
public:
 template <class TARGET>
 TARGET &Cast() {
  DynamicCastCheck<TARGET>(this);
  return reinterpret_cast<TARGET &>(*this);
 }
 template <class TARGET>
 const TARGET &Cast() const {
  D_ASSERT(dynamic_cast<const TARGET *>(this));
  return reinterpret_cast<const TARGET &>(*this);
 }
};
struct PageInformation {
 idx_t offset = 0;
 idx_t row_count = 0;
 idx_t empty_count = 0;
 idx_t estimated_page_size = 0;
};
struct PageWriteInformation {
 PageHeader page_header;
 unique_ptr<MemoryStream> temp_writer;
 unique_ptr<ColumnWriterPageState> page_state;
 idx_t write_page_idx = 0;
 idx_t write_count = 0;
 idx_t max_write_count = 0;
 size_t compressed_size;
 data_ptr_t compressed_data;
 unique_ptr<data_t[]> compressed_buf;
};
class BasicColumnWriterState : public ColumnWriterState {
public:
 BasicColumnWriterState(duckdb_parquet::RowGroup &row_group, idx_t col_idx)
     : row_group(row_group), col_idx(col_idx) {
  page_info.emplace_back();
 }
 ~BasicColumnWriterState() override = default;
 duckdb_parquet::RowGroup &row_group;
 idx_t col_idx;
 vector<PageInformation> page_info;
 vector<PageWriteInformation> write_info;
 unique_ptr<ColumnWriterStatistics> stats_state;
 idx_t current_page = 0;
};
class BasicColumnWriter : public ColumnWriter {
public:
 BasicColumnWriter(ParquetWriter &writer, idx_t schema_idx, vector<string> schema_path, idx_t max_repeat,
                   idx_t max_define, bool can_have_nulls)
     : ColumnWriter(writer, schema_idx, std::move(schema_path), max_repeat, max_define, can_have_nulls) {
 }
 ~BasicColumnWriter() override = default;
 static constexpr const idx_t MAX_UNCOMPRESSED_PAGE_SIZE = 100000000;
 static constexpr const idx_t MAX_UNCOMPRESSED_DICT_PAGE_SIZE = 1e9;
 static constexpr const idx_t DICTIONARY_ANALYZE_THRESHOLD = 1e4;
 static constexpr const idx_t MAX_DICTIONARY_KEY_SIZE = sizeof(uint32_t);
 static constexpr const idx_t STRING_LENGTH_SIZE = sizeof(uint32_t);
public:
 unique_ptr<ColumnWriterState> InitializeWriteState(duckdb_parquet::RowGroup &row_group) override;
 void Prepare(ColumnWriterState &state, ColumnWriterState *parent, Vector &vector, idx_t count) override;
 void BeginWrite(ColumnWriterState &state) override;
 void Write(ColumnWriterState &state, Vector &vector, idx_t count) override;
 void FinalizeWrite(ColumnWriterState &state) override;
protected:
 static void WriteLevels(WriteStream &temp_writer, const unsafe_vector<uint16_t> &levels, idx_t max_value,
                         idx_t start_offset, idx_t count);
 virtual duckdb_parquet::Encoding::type GetEncoding(BasicColumnWriterState &state);
 void NextPage(BasicColumnWriterState &state);
 void FlushPage(BasicColumnWriterState &state);
 virtual unique_ptr<ColumnWriterStatistics> InitializeStatsState();
 virtual unique_ptr<ColumnWriterPageState> InitializePageState(BasicColumnWriterState &state);
 virtual void FlushPageState(WriteStream &temp_writer, ColumnWriterPageState *state);
 virtual idx_t GetRowSize(const Vector &vector, const idx_t index, const BasicColumnWriterState &state) const;
 virtual void WriteVector(WriteStream &temp_writer, ColumnWriterStatistics *stats, ColumnWriterPageState *page_state,
                          Vector &vector, idx_t chunk_start, idx_t chunk_end) = 0;
 virtual bool HasDictionary(BasicColumnWriterState &state_p) {
  return false;
 }
 virtual idx_t DictionarySize(BasicColumnWriterState &state_p);
 void WriteDictionary(BasicColumnWriterState &state, unique_ptr<MemoryStream> temp_writer, idx_t row_count);
 virtual void FlushDictionary(BasicColumnWriterState &state, ColumnWriterStatistics *stats);
 void SetParquetStatistics(BasicColumnWriterState &state, duckdb_parquet::ColumnChunk &column);
 void RegisterToRowGroup(duckdb_parquet::RowGroup &row_group);
};
unique_ptr<ColumnWriterState> BasicColumnWriter::InitializeWriteState(duckdb_parquet::RowGroup &row_group) {
 auto result = make_uniq<BasicColumnWriterState>(row_group, row_group.columns.size());
 RegisterToRowGroup(row_group);
 return std::move(result);
}
void BasicColumnWriter::RegisterToRowGroup(duckdb_parquet::RowGroup &row_group) {
 duckdb_parquet::ColumnChunk column_chunk;
 column_chunk.__isset.meta_data = true;
 column_chunk.meta_data.codec = writer.GetCodec();
 column_chunk.meta_data.path_in_schema = schema_path;
 column_chunk.meta_data.num_values = 0;
 column_chunk.meta_data.type = writer.GetType(schema_idx);
 row_group.columns.push_back(std::move(column_chunk));
}
unique_ptr<ColumnWriterPageState> BasicColumnWriter::InitializePageState(BasicColumnWriterState &state) {
 return nullptr;
}
void BasicColumnWriter::FlushPageState(WriteStream &temp_writer, ColumnWriterPageState *state) {
}
void BasicColumnWriter::Prepare(ColumnWriterState &state_p, ColumnWriterState *parent, Vector &vector, idx_t count) {
 auto &state = state_p.Cast<BasicColumnWriterState>();
 auto &col_chunk = state.row_group.columns[state.col_idx];
 idx_t start = 0;
 idx_t vcount = parent ? parent->definition_levels.size() - state.definition_levels.size() : count;
 idx_t parent_index = state.definition_levels.size();
 auto &validity = FlatVector::Validity(vector);
 HandleRepeatLevels(state, parent, count, max_repeat);
 HandleDefineLevels(state, parent, validity, count, max_define, max_define - 1);
 idx_t vector_index = 0;
 reference<PageInformation> page_info_ref = state.page_info.back();
 for (idx_t i = start; i < vcount; i++) {
  auto &page_info = page_info_ref.get();
  page_info.row_count++;
  col_chunk.meta_data.num_values++;
  if (parent && !parent->is_empty.empty() && parent->is_empty[parent_index + i]) {
   page_info.empty_count++;
   continue;
  }
  if (validity.RowIsValid(vector_index)) {
   page_info.estimated_page_size += GetRowSize(vector, vector_index, state);
   if (page_info.estimated_page_size >= MAX_UNCOMPRESSED_PAGE_SIZE) {
    PageInformation new_info;
    new_info.offset = page_info.offset + page_info.row_count;
    state.page_info.push_back(new_info);
    page_info_ref = state.page_info.back();
   }
  }
  vector_index++;
 }
}
duckdb_parquet::Encoding::type BasicColumnWriter::GetEncoding(BasicColumnWriterState &state) {
 return Encoding::PLAIN;
}
void BasicColumnWriter::BeginWrite(ColumnWriterState &state_p) {
 auto &state = state_p.Cast<BasicColumnWriterState>();
 state.stats_state = InitializeStatsState();
 for (idx_t page_idx = 0; page_idx < state.page_info.size(); page_idx++) {
  auto &page_info = state.page_info[page_idx];
  if (page_info.row_count == 0) {
   D_ASSERT(page_idx + 1 == state.page_info.size());
   state.page_info.erase_at(page_idx);
   break;
  }
  PageWriteInformation write_info;
  auto &hdr = write_info.page_header;
  hdr.compressed_page_size = 0;
  hdr.uncompressed_page_size = 0;
  hdr.type = PageType::DATA_PAGE;
  hdr.__isset.data_page_header = true;
  hdr.data_page_header.num_values = UnsafeNumericCast<int32_t>(page_info.row_count);
  hdr.data_page_header.encoding = GetEncoding(state);
  hdr.data_page_header.definition_level_encoding = Encoding::RLE;
  hdr.data_page_header.repetition_level_encoding = Encoding::RLE;
  write_info.temp_writer = make_uniq<MemoryStream>(
      MaxValue<idx_t>(NextPowerOfTwo(page_info.estimated_page_size), MemoryStream::DEFAULT_INITIAL_CAPACITY));
  write_info.write_count = page_info.empty_count;
  write_info.max_write_count = page_info.row_count;
  write_info.page_state = InitializePageState(state);
  write_info.compressed_size = 0;
  write_info.compressed_data = nullptr;
  state.write_info.push_back(std::move(write_info));
 }
 NextPage(state);
}
void BasicColumnWriter::WriteLevels(WriteStream &temp_writer, const unsafe_vector<uint16_t> &levels, idx_t max_value,
                                    idx_t offset, idx_t count) {
 if (levels.empty() || count == 0) {
  return;
 }
 auto bit_width = RleBpDecoder::ComputeBitWidth((max_value));
 RleBpEncoder rle_encoder(bit_width);
 rle_encoder.BeginPrepare(levels[offset]);
 for (idx_t i = offset + 1; i < offset + count; i++) {
  rle_encoder.PrepareValue(levels[i]);
 }
 rle_encoder.FinishPrepare();
 temp_writer.Write<uint32_t>(rle_encoder.GetByteCount());
 rle_encoder.BeginWrite(temp_writer, levels[offset]);
 for (idx_t i = offset + 1; i < offset + count; i++) {
  rle_encoder.WriteValue(temp_writer, levels[i]);
 }
 rle_encoder.FinishWrite(temp_writer);
}
void BasicColumnWriter::NextPage(BasicColumnWriterState &state) {
 if (state.current_page > 0) {
  FlushPage(state);
 }
 if (state.current_page >= state.write_info.size()) {
  state.current_page = state.write_info.size() + 1;
  return;
 }
 auto &page_info = state.page_info[state.current_page];
 auto &write_info = state.write_info[state.current_page];
 state.current_page++;
 auto &temp_writer = *write_info.temp_writer;
 WriteLevels(temp_writer, state.repetition_levels, max_repeat, page_info.offset, page_info.row_count);
 WriteLevels(temp_writer, state.definition_levels, max_define, page_info.offset, page_info.row_count);
}
void BasicColumnWriter::FlushPage(BasicColumnWriterState &state) {
 D_ASSERT(state.current_page > 0);
 if (state.current_page > state.write_info.size()) {
  return;
 }
 auto &write_info = state.write_info[state.current_page - 1];
 auto &temp_writer = *write_info.temp_writer;
 auto &hdr = write_info.page_header;
 FlushPageState(temp_writer, write_info.page_state.get());
 if (temp_writer.GetPosition() > idx_t(NumericLimits<int32_t>::Maximum())) {
  throw InternalException("Parquet writer: %d uncompressed page size out of range for type integer",
                          temp_writer.GetPosition());
 }
 hdr.uncompressed_page_size = UnsafeNumericCast<int32_t>(temp_writer.GetPosition());
 CompressPage(temp_writer, write_info.compressed_size, write_info.compressed_data, write_info.compressed_buf);
 hdr.compressed_page_size = UnsafeNumericCast<int32_t>(write_info.compressed_size);
 D_ASSERT(hdr.uncompressed_page_size > 0);
 D_ASSERT(hdr.compressed_page_size > 0);
 if (write_info.compressed_buf) {
  D_ASSERT(write_info.compressed_buf.get() == write_info.compressed_data);
  write_info.temp_writer.reset();
 }
}
unique_ptr<ColumnWriterStatistics> BasicColumnWriter::InitializeStatsState() {
 return make_uniq<ColumnWriterStatistics>();
}
idx_t BasicColumnWriter::GetRowSize(const Vector &vector, const idx_t index,
                                    const BasicColumnWriterState &state) const {
 throw InternalException("GetRowSize unsupported for struct/list column writers");
}
void BasicColumnWriter::Write(ColumnWriterState &state_p, Vector &vector, idx_t count) {
 auto &state = state_p.Cast<BasicColumnWriterState>();
 idx_t remaining = count;
 idx_t offset = 0;
 while (remaining > 0) {
  auto &write_info = state.write_info[state.current_page - 1];
  if (!write_info.temp_writer) {
   throw InternalException("Writes are not correctly aligned!?");
  }
  auto &temp_writer = *write_info.temp_writer;
  idx_t write_count = MinValue<idx_t>(remaining, write_info.max_write_count - write_info.write_count);
  D_ASSERT(write_count > 0);
  WriteVector(temp_writer, state.stats_state.get(), write_info.page_state.get(), vector, offset,
              offset + write_count);
  write_info.write_count += write_count;
  if (write_info.write_count == write_info.max_write_count) {
   NextPage(state);
  }
  offset += write_count;
  remaining -= write_count;
 }
}
void BasicColumnWriter::SetParquetStatistics(BasicColumnWriterState &state, duckdb_parquet::ColumnChunk &column_chunk) {
 if (max_repeat == 0) {
  column_chunk.meta_data.statistics.null_count = NumericCast<int64_t>(state.null_count);
  column_chunk.meta_data.statistics.__isset.null_count = true;
  column_chunk.meta_data.__isset.statistics = true;
 }
 auto min = state.stats_state->GetMin();
 if (!min.empty()) {
  column_chunk.meta_data.statistics.min = std::move(min);
  column_chunk.meta_data.statistics.__isset.min = true;
  column_chunk.meta_data.__isset.statistics = true;
 }
 auto max = state.stats_state->GetMax();
 if (!max.empty()) {
  column_chunk.meta_data.statistics.max = std::move(max);
  column_chunk.meta_data.statistics.__isset.max = true;
  column_chunk.meta_data.__isset.statistics = true;
 }
 if (state.stats_state->HasStats()) {
  column_chunk.meta_data.statistics.min_value = state.stats_state->GetMinValue();
  column_chunk.meta_data.statistics.__isset.min_value = true;
  column_chunk.meta_data.__isset.statistics = true;
  column_chunk.meta_data.statistics.max_value = state.stats_state->GetMaxValue();
  column_chunk.meta_data.statistics.__isset.max_value = true;
  column_chunk.meta_data.__isset.statistics = true;
 }
 if (HasDictionary(state)) {
  column_chunk.meta_data.statistics.distinct_count = UnsafeNumericCast<int64_t>(DictionarySize(state));
  column_chunk.meta_data.statistics.__isset.distinct_count = true;
  column_chunk.meta_data.__isset.statistics = true;
 }
 for (const auto &write_info : state.write_info) {
  column_chunk.meta_data.encodings.push_back(write_info.page_header.data_page_header.encoding);
 }
}
void BasicColumnWriter::FinalizeWrite(ColumnWriterState &state_p) {
 auto &state = state_p.Cast<BasicColumnWriterState>();
 auto &column_chunk = state.row_group.columns[state.col_idx];
 FlushPage(state);
 auto &column_writer = writer.GetWriter();
 auto start_offset = column_writer.GetTotalWritten();
 if (HasDictionary(state)) {
  column_chunk.meta_data.statistics.distinct_count = UnsafeNumericCast<int64_t>(DictionarySize(state));
  column_chunk.meta_data.statistics.__isset.distinct_count = true;
  column_chunk.meta_data.dictionary_page_offset = UnsafeNumericCast<int64_t>(column_writer.GetTotalWritten());
  column_chunk.meta_data.__isset.dictionary_page_offset = true;
  FlushDictionary(state, state.stats_state.get());
 }
 column_chunk.meta_data.data_page_offset = 0;
 SetParquetStatistics(state, column_chunk);
 idx_t total_uncompressed_size = 0;
 for (auto &write_info : state.write_info) {
  if (column_chunk.meta_data.data_page_offset == 0 && (write_info.page_header.type == PageType::DATA_PAGE ||
                                                       write_info.page_header.type == PageType::DATA_PAGE_V2)) {
   column_chunk.meta_data.data_page_offset = UnsafeNumericCast<int64_t>(column_writer.GetTotalWritten());
   ;
  }
  D_ASSERT(write_info.page_header.uncompressed_page_size > 0);
  auto header_start_offset = column_writer.GetTotalWritten();
  writer.Write(write_info.page_header);
  total_uncompressed_size += column_writer.GetTotalWritten() - header_start_offset;
  total_uncompressed_size += write_info.page_header.uncompressed_page_size;
  writer.WriteData(write_info.compressed_data, write_info.compressed_size);
 }
 column_chunk.meta_data.total_compressed_size =
     UnsafeNumericCast<int64_t>(column_writer.GetTotalWritten() - start_offset);
 column_chunk.meta_data.total_uncompressed_size = UnsafeNumericCast<int64_t>(total_uncompressed_size);
}
void BasicColumnWriter::FlushDictionary(BasicColumnWriterState &state, ColumnWriterStatistics *stats) {
 throw InternalException("This page does not have a dictionary");
}
idx_t BasicColumnWriter::DictionarySize(BasicColumnWriterState &state) {
 throw InternalException("This page does not have a dictionary");
}
void BasicColumnWriter::WriteDictionary(BasicColumnWriterState &state, unique_ptr<MemoryStream> temp_writer,
                                        idx_t row_count) {
 D_ASSERT(temp_writer);
 D_ASSERT(temp_writer->GetPosition() > 0);
 PageWriteInformation write_info;
 auto &hdr = write_info.page_header;
 hdr.uncompressed_page_size = UnsafeNumericCast<int32_t>(temp_writer->GetPosition());
 hdr.type = PageType::DICTIONARY_PAGE;
 hdr.__isset.dictionary_page_header = true;
 hdr.dictionary_page_header.encoding = Encoding::PLAIN;
 hdr.dictionary_page_header.is_sorted = false;
 hdr.dictionary_page_header.num_values = UnsafeNumericCast<int32_t>(row_count);
 write_info.temp_writer = std::move(temp_writer);
 write_info.write_count = 0;
 write_info.max_write_count = 0;
 CompressPage(*write_info.temp_writer, write_info.compressed_size, write_info.compressed_data,
              write_info.compressed_buf);
 hdr.compressed_page_size = UnsafeNumericCast<int32_t>(write_info.compressed_size);
 state.write_info.insert(state.write_info.begin(), std::move(write_info));
}
template <class SRC, class T, class OP>
class NumericStatisticsState : public ColumnWriterStatistics {
public:
 NumericStatisticsState() : min(NumericLimits<T>::Maximum()), max(NumericLimits<T>::Minimum()) {
 }
 T min;
 T max;
public:
 bool HasStats() override {
  return min <= max;
 }
 string GetMin() override {
  return NumericLimits<SRC>::IsSigned() ? GetMinValue() : string();
 }
 string GetMax() override {
  return NumericLimits<SRC>::IsSigned() ? GetMaxValue() : string();
 }
 string GetMinValue() override {
  return HasStats() ? string((char *)&min, sizeof(T)) : string();
 }
 string GetMaxValue() override {
  return HasStats() ? string((char *)&max, sizeof(T)) : string();
 }
};
struct BaseParquetOperator {
 template <class SRC, class TGT>
 static unique_ptr<ColumnWriterStatistics> InitializeStats() {
  return make_uniq<NumericStatisticsState<SRC, TGT, BaseParquetOperator>>();
 }
 template <class SRC, class TGT>
 static void HandleStats(ColumnWriterStatistics *stats, SRC source_value, TGT target_value) {
  auto &numeric_stats = (NumericStatisticsState<SRC, TGT, BaseParquetOperator> &)*stats;
  if (LessThan::Operation(target_value, numeric_stats.min)) {
   numeric_stats.min = target_value;
  }
  if (GreaterThan::Operation(target_value, numeric_stats.max)) {
   numeric_stats.max = target_value;
  }
 }
};
struct ParquetCastOperator : public BaseParquetOperator {
 template <class SRC, class TGT>
 static TGT Operation(SRC input) {
  return TGT(input);
 }
};
struct ParquetTimestampNSOperator : public BaseParquetOperator {
 template <class SRC, class TGT>
 static TGT Operation(SRC input) {
  return TGT(input);
 }
};
struct ParquetTimestampSOperator : public BaseParquetOperator {
 template <class SRC, class TGT>
 static TGT Operation(SRC input) {
  return Timestamp::FromEpochSecondsPossiblyInfinite(input).value;
 }
};
struct ParquetTimeTZOperator : public BaseParquetOperator {
 template <class SRC, class TGT>
 static TGT Operation(SRC input) {
  return input.time().micros;
 }
};
struct ParquetHugeintOperator {
 template <class SRC, class TGT>
 static TGT Operation(SRC input) {
  return Hugeint::Cast<double>(input);
 }
 template <class SRC, class TGT>
 static unique_ptr<ColumnWriterStatistics> InitializeStats() {
  return make_uniq<ColumnWriterStatistics>();
 }
 template <class SRC, class TGT>
 static void HandleStats(ColumnWriterStatistics *stats, SRC source_value, TGT target_value) {
 }
};
struct ParquetUhugeintOperator {
 template <class SRC, class TGT>
 static TGT Operation(SRC input) {
  return Uhugeint::Cast<double>(input);
 }
 template <class SRC, class TGT>
 static unique_ptr<ColumnWriterStatistics> InitializeStats() {
  return make_uniq<ColumnWriterStatistics>();
 }
 template <class SRC, class TGT>
 static void HandleStats(ColumnWriterStatistics *stats, SRC source_value, TGT target_value) {
 }
};
template <class SRC, class TGT, class OP = ParquetCastOperator>
static void TemplatedWritePlain(Vector &col, ColumnWriterStatistics *stats, const idx_t chunk_start,
                                const idx_t chunk_end, const ValidityMask &mask, WriteStream &ser) {
 static constexpr idx_t WRITE_COMBINER_CAPACITY = 8;
 TGT write_combiner[WRITE_COMBINER_CAPACITY];
 idx_t write_combiner_count = 0;
 const auto *ptr = FlatVector::GetData<SRC>(col);
 for (idx_t r = chunk_start; r < chunk_end; r++) {
  if (!mask.RowIsValid(r)) {
   continue;
  }
  TGT target_value = OP::template Operation<SRC, TGT>(ptr[r]);
  OP::template HandleStats<SRC, TGT>(stats, ptr[r], target_value);
  write_combiner[write_combiner_count++] = target_value;
  if (write_combiner_count == WRITE_COMBINER_CAPACITY) {
   ser.WriteData(const_data_ptr_cast(write_combiner), WRITE_COMBINER_CAPACITY * sizeof(TGT));
   write_combiner_count = 0;
  }
 }
 ser.WriteData(const_data_ptr_cast(write_combiner), write_combiner_count * sizeof(TGT));
}
class StandardColumnWriterState : public BasicColumnWriterState {
public:
 StandardColumnWriterState(duckdb_parquet::format::RowGroup &row_group, idx_t col_idx)
     : BasicColumnWriterState(row_group, col_idx) {
 }
 ~StandardColumnWriterState() override = default;
 idx_t total_value_count = 0;
};
class StandardWriterPageState : public ColumnWriterPageState {
public:
 explicit StandardWriterPageState(const idx_t total_value_count) : encoder(total_value_count), initialized(false) {
 }
 DbpEncoder encoder;
 bool initialized;
};
template <class SRC, class TGT, class OP = ParquetCastOperator>
class StandardColumnWriter : public BasicColumnWriter {
public:
 StandardColumnWriter(ParquetWriter &writer, idx_t schema_idx, vector<string> schema_path_p,
                      idx_t max_repeat, idx_t max_define, bool can_have_nulls)
     : BasicColumnWriter(writer, schema_idx, std::move(schema_path_p), max_repeat, max_define, can_have_nulls) {
 }
 ~StandardColumnWriter() override = default;
public:
 unique_ptr<ColumnWriterState> InitializeWriteState(duckdb_parquet::format::RowGroup &row_group) override {
  auto result = make_uniq<StandardColumnWriterState>(row_group, row_group.columns.size());
  RegisterToRowGroup(row_group);
  return std::move(result);
 }
 unique_ptr<ColumnWriterPageState> InitializePageState(BasicColumnWriterState &state_p) override {
  auto &state = state_p.Cast<StandardColumnWriterState>();
  auto result = make_uniq<StandardWriterPageState>(state.total_value_count);
  return std::move(result);
 }
 void FlushPageState(WriteStream &temp_writer, ColumnWriterPageState *state_p) override {
  auto &page_state = state_p->Cast<StandardWriterPageState>();
  if (!page_state.initialized) {
   page_state.encoder.BeginWrite(temp_writer, 0);
  }
  page_state.encoder.FinishWrite(temp_writer);
 }
 Encoding::type GetEncoding(BasicColumnWriterState &state) override {
  return HasAnalyze() ? Encoding::DELTA_BINARY_PACKED : Encoding::PLAIN;
 }
 bool HasAnalyze() override {
  const auto type = writer.GetType(schema_idx);
  return type == Type::type::INT32 || type == Type::type::INT64;
 }
 void Analyze(ColumnWriterState &state_p, ColumnWriterState *parent, Vector &vector, idx_t count) override {
  D_ASSERT(HasAnalyze());
  auto &state = state_p.Cast<StandardColumnWriterState>();
  const bool check_parent_empty = parent && !parent->is_empty.empty();
  const idx_t parent_index = state.definition_levels.size();
  const idx_t vcount =
      check_parent_empty ? parent->definition_levels.size() - state.definition_levels.size() : count;
  const auto &validity = FlatVector::Validity(vector);
  idx_t vector_index = 0;
  for (idx_t i = 0; i < vcount; i++) {
   if (check_parent_empty && parent->is_empty[parent_index + i]) {
    continue;
   }
   if (validity.RowIsValid(vector_index)) {
    state.total_value_count++;
   }
   vector_index++;
  }
 }
 void FinalizeAnalyze(ColumnWriterState &state) override {
 }
 unique_ptr<ColumnWriterStatistics> InitializeStatsState() override {
  return OP::template InitializeStats<SRC, TGT>();
 }
 void WriteVector(WriteStream &temp_writer, ColumnWriterStatistics *stats, ColumnWriterPageState *page_state_p,
                  Vector &input_column, idx_t chunk_start, idx_t chunk_end) override {
  const auto &mask = FlatVector::Validity(input_column);
  if (HasAnalyze()) {
   auto &page_state = page_state_p->Cast<StandardWriterPageState>();
   auto &encoder = page_state.encoder;
   const auto *ptr = FlatVector::GetData<SRC>(input_column);
   idx_t r = chunk_start;
   if (!page_state.initialized) {
    for (; r < chunk_end; r++) {
     if (!mask.RowIsValid(r)) {
      continue;
     }
     const TGT target_value = OP::template Operation<SRC, TGT>(ptr[r]);
     OP::template HandleStats<SRC, TGT>(stats, ptr[r], target_value);
     encoder.BeginWrite(temp_writer, target_value);
     page_state.initialized = true;
     r++;
     break;
    }
   }
   for (; r < chunk_end; r++) {
    if (!mask.RowIsValid(r)) {
     continue;
    }
    const TGT target_value = OP::template Operation<SRC, TGT>(ptr[r]);
    OP::template HandleStats<SRC, TGT>(stats, ptr[r], target_value);
    encoder.WriteValue(temp_writer, target_value);
   }
  } else {
   TemplatedWritePlain<SRC, TGT, OP>(input_column, stats, chunk_start, chunk_end, mask, temp_writer);
  }
 }
 idx_t GetRowSize(const Vector &vector, const idx_t index, const BasicColumnWriterState &state) const override {
  return sizeof(TGT);
 }
};
class BooleanStatisticsState : public ColumnWriterStatistics {
public:
 BooleanStatisticsState() : min(true), max(false) {
 }
 bool min;
 bool max;
public:
 bool HasStats() override {
  return !(min && !max);
 }
 string GetMin() override {
  return GetMinValue();
 }
 string GetMax() override {
  return GetMaxValue();
 }
 string GetMinValue() override {
  return HasStats() ? string(const_char_ptr_cast(&min), sizeof(bool)) : string();
 }
 string GetMaxValue() override {
  return HasStats() ? string(const_char_ptr_cast(&max), sizeof(bool)) : string();
 }
};
class BooleanWriterPageState : public ColumnWriterPageState {
public:
 uint8_t byte = 0;
 uint8_t byte_pos = 0;
};
class BooleanColumnWriter : public BasicColumnWriter {
public:
 BooleanColumnWriter(ParquetWriter &writer, idx_t schema_idx, vector<string> schema_path_p, idx_t max_repeat,
                     idx_t max_define, bool can_have_nulls)
     : BasicColumnWriter(writer, schema_idx, std::move(schema_path_p), max_repeat, max_define, can_have_nulls) {
 }
 ~BooleanColumnWriter() override = default;
public:
 unique_ptr<ColumnWriterStatistics> InitializeStatsState() override {
  return make_uniq<BooleanStatisticsState>();
 }
 void WriteVector(WriteStream &temp_writer, ColumnWriterStatistics *stats_p, ColumnWriterPageState *state_p,
                  Vector &input_column, idx_t chunk_start, idx_t chunk_end) override {
  auto &stats = stats_p->Cast<BooleanStatisticsState>();
  auto &state = state_p->Cast<BooleanWriterPageState>();
  auto &mask = FlatVector::Validity(input_column);
  auto *ptr = FlatVector::GetData<bool>(input_column);
  for (idx_t r = chunk_start; r < chunk_end; r++) {
   if (mask.RowIsValid(r)) {
    if (ptr[r]) {
     stats.max = true;
     state.byte |= 1 << state.byte_pos;
    } else {
     stats.min = false;
    }
    state.byte_pos++;
    if (state.byte_pos == 8) {
     temp_writer.Write<uint8_t>(state.byte);
     state.byte = 0;
     state.byte_pos = 0;
    }
   }
  }
 }
 unique_ptr<ColumnWriterPageState> InitializePageState(BasicColumnWriterState &state) override {
  return make_uniq<BooleanWriterPageState>();
 }
 void FlushPageState(WriteStream &temp_writer, ColumnWriterPageState *state_p) override {
  auto &state = state_p->Cast<BooleanWriterPageState>();
  if (state.byte_pos > 0) {
   temp_writer.Write<uint8_t>(state.byte);
   state.byte = 0;
   state.byte_pos = 0;
  }
 }
 idx_t GetRowSize(const Vector &vector, const idx_t index, const BasicColumnWriterState &state) const override {
  return sizeof(bool);
 }
};
static void WriteParquetDecimal(hugeint_t input, data_ptr_t result) {
 bool positive = input >= 0;
 if (!positive) {
  input = NumericLimits<hugeint_t>::Maximum() + input + 1;
 }
 uint64_t high_bytes = uint64_t(input.upper);
 uint64_t low_bytes = input.lower;
 for (idx_t i = 0; i < sizeof(uint64_t); i++) {
  auto shift_count = (sizeof(uint64_t) - i - 1) * 8;
  result[i] = (high_bytes >> shift_count) & 0xFF;
 }
 for (idx_t i = 0; i < sizeof(uint64_t); i++) {
  auto shift_count = (sizeof(uint64_t) - i - 1) * 8;
  result[sizeof(uint64_t) + i] = (low_bytes >> shift_count) & 0xFF;
 }
 if (!positive) {
  result[0] |= 0x80;
 }
}
class FixedDecimalStatistics : public ColumnWriterStatistics {
public:
 FixedDecimalStatistics() : min(NumericLimits<hugeint_t>::Maximum()), max(NumericLimits<hugeint_t>::Minimum()) {
 }
 hugeint_t min;
 hugeint_t max;
public:
 string GetStats(hugeint_t &input) {
  data_t buffer[16];
  WriteParquetDecimal(input, buffer);
  return string(const_char_ptr_cast(buffer), 16);
 }
 bool HasStats() override {
  return min <= max;
 }
 void Update(hugeint_t &val) {
  if (LessThan::Operation(val, min)) {
   min = val;
  }
  if (GreaterThan::Operation(val, max)) {
   max = val;
  }
 }
 string GetMin() override {
  return GetMinValue();
 }
 string GetMax() override {
  return GetMaxValue();
 }
 string GetMinValue() override {
  return HasStats() ? GetStats(min) : string();
 }
 string GetMaxValue() override {
  return HasStats() ? GetStats(max) : string();
 }
};
class FixedDecimalColumnWriter : public BasicColumnWriter {
public:
 FixedDecimalColumnWriter(ParquetWriter &writer, idx_t schema_idx, vector<string> schema_path_p, idx_t max_repeat,
                          idx_t max_define, bool can_have_nulls)
     : BasicColumnWriter(writer, schema_idx, std::move(schema_path_p), max_repeat, max_define, can_have_nulls) {
 }
 ~FixedDecimalColumnWriter() override = default;
public:
 unique_ptr<ColumnWriterStatistics> InitializeStatsState() override {
  return make_uniq<FixedDecimalStatistics>();
 }
 void WriteVector(WriteStream &temp_writer, ColumnWriterStatistics *stats_p, ColumnWriterPageState *page_state,
                  Vector &input_column, idx_t chunk_start, idx_t chunk_end) override {
  auto &mask = FlatVector::Validity(input_column);
  auto *ptr = FlatVector::GetData<hugeint_t>(input_column);
  auto &stats = stats_p->Cast<FixedDecimalStatistics>();
  data_t temp_buffer[16];
  for (idx_t r = chunk_start; r < chunk_end; r++) {
   if (mask.RowIsValid(r)) {
    stats.Update(ptr[r]);
    WriteParquetDecimal(ptr[r], temp_buffer);
    temp_writer.WriteData(temp_buffer, 16);
   }
  }
 }
 idx_t GetRowSize(const Vector &vector, const idx_t index, const BasicColumnWriterState &state) const override {
  return sizeof(hugeint_t);
 }
};
class UUIDColumnWriter : public BasicColumnWriter {
 static constexpr const idx_t PARQUET_UUID_SIZE = 16;
public:
 UUIDColumnWriter(ParquetWriter &writer, idx_t schema_idx, vector<string> schema_path_p, idx_t max_repeat,
                  idx_t max_define, bool can_have_nulls)
     : BasicColumnWriter(writer, schema_idx, std::move(schema_path_p), max_repeat, max_define, can_have_nulls) {
 }
 ~UUIDColumnWriter() override = default;
public:
 static void WriteParquetUUID(hugeint_t input, data_ptr_t result) {
  uint64_t high_bytes = input.upper ^ (int64_t(1) << 63);
  uint64_t low_bytes = input.lower;
  for (idx_t i = 0; i < sizeof(uint64_t); i++) {
   auto shift_count = (sizeof(uint64_t) - i - 1) * 8;
   result[i] = (high_bytes >> shift_count) & 0xFF;
  }
  for (idx_t i = 0; i < sizeof(uint64_t); i++) {
   auto shift_count = (sizeof(uint64_t) - i - 1) * 8;
   result[sizeof(uint64_t) + i] = (low_bytes >> shift_count) & 0xFF;
  }
 }
 void WriteVector(WriteStream &temp_writer, ColumnWriterStatistics *stats_p, ColumnWriterPageState *page_state,
                  Vector &input_column, idx_t chunk_start, idx_t chunk_end) override {
  auto &mask = FlatVector::Validity(input_column);
  auto *ptr = FlatVector::GetData<hugeint_t>(input_column);
  data_t temp_buffer[PARQUET_UUID_SIZE];
  for (idx_t r = chunk_start; r < chunk_end; r++) {
   if (mask.RowIsValid(r)) {
    WriteParquetUUID(ptr[r], temp_buffer);
    temp_writer.WriteData(temp_buffer, PARQUET_UUID_SIZE);
   }
  }
 }
 idx_t GetRowSize(const Vector &vector, const idx_t index, const BasicColumnWriterState &state) const override {
  return PARQUET_UUID_SIZE;
 }
};
class IntervalColumnWriter : public BasicColumnWriter {
 static constexpr const idx_t PARQUET_INTERVAL_SIZE = 12;
public:
 IntervalColumnWriter(ParquetWriter &writer, idx_t schema_idx, vector<string> schema_path_p, idx_t max_repeat,
                      idx_t max_define, bool can_have_nulls)
     : BasicColumnWriter(writer, schema_idx, std::move(schema_path_p), max_repeat, max_define, can_have_nulls) {
 }
 ~IntervalColumnWriter() override = default;
public:
 static void WriteParquetInterval(interval_t input, data_ptr_t result) {
  if (input.days < 0 || input.months < 0 || input.micros < 0) {
   throw IOException("Parquet files do not support negative intervals");
  }
  Store<uint32_t>(input.months, result);
  Store<uint32_t>(input.days, result + sizeof(uint32_t));
  Store<uint32_t>(input.micros / 1000, result + sizeof(uint32_t) * 2);
 }
 void WriteVector(WriteStream &temp_writer, ColumnWriterStatistics *stats_p, ColumnWriterPageState *page_state,
                  Vector &input_column, idx_t chunk_start, idx_t chunk_end) override {
  auto &mask = FlatVector::Validity(input_column);
  auto *ptr = FlatVector::GetData<interval_t>(input_column);
  data_t temp_buffer[PARQUET_INTERVAL_SIZE];
  for (idx_t r = chunk_start; r < chunk_end; r++) {
   if (mask.RowIsValid(r)) {
    WriteParquetInterval(ptr[r], temp_buffer);
    temp_writer.WriteData(temp_buffer, PARQUET_INTERVAL_SIZE);
   }
  }
 }
 idx_t GetRowSize(const Vector &vector, const idx_t index, const BasicColumnWriterState &state) const override {
  return PARQUET_INTERVAL_SIZE;
 }
};
class StringStatisticsState : public ColumnWriterStatistics {
 static constexpr const idx_t MAX_STRING_STATISTICS_SIZE = 10000;
public:
 StringStatisticsState() : has_stats(false), values_too_big(false), min(), max() {
 }
 bool has_stats;
 bool values_too_big;
 string min;
 string max;
public:
 bool HasStats() override {
  return has_stats;
 }
 void Update(const string_t &val) {
  if (values_too_big) {
   return;
  }
  auto str_len = val.GetSize();
  if (str_len > MAX_STRING_STATISTICS_SIZE) {
   values_too_big = true;
   has_stats = false;
   min = string();
   max = string();
   return;
  }
  if (!has_stats || LessThan::Operation(val, string_t(min))) {
   min = val.GetString();
  }
  if (!has_stats || GreaterThan::Operation(val, string_t(max))) {
   max = val.GetString();
  }
  has_stats = true;
 }
 string GetMin() override {
  return GetMinValue();
 }
 string GetMax() override {
  return GetMaxValue();
 }
 string GetMinValue() override {
  return HasStats() ? min : string();
 }
 string GetMaxValue() override {
  return HasStats() ? max : string();
 }
};
class StringColumnWriterState : public BasicColumnWriterState {
public:
 StringColumnWriterState(duckdb_parquet::RowGroup &row_group, idx_t col_idx)
     : BasicColumnWriterState(row_group, col_idx) {
 }
 ~StringColumnWriterState() override = default;
 idx_t estimated_dict_page_size = 0;
 idx_t estimated_rle_pages_size = 0;
 idx_t estimated_plain_size = 0;
 string_map_t<uint32_t> dictionary;
 uint32_t key_bit_width;
 bool IsDictionaryEncoded() const {
  return key_bit_width != 0;
 }
};
class StringWriterPageState : public ColumnWriterPageState {
public:
 explicit StringWriterPageState(uint32_t bit_width, const string_map_t<uint32_t> &values)
     : bit_width(bit_width), dictionary(values), encoder(bit_width), written_value(false) {
  D_ASSERT(IsDictionaryEncoded() || (bit_width == 0 && dictionary.empty()));
 }
 bool IsDictionaryEncoded() {
  return bit_width != 0;
 }
 uint32_t bit_width;
 const string_map_t<uint32_t> &dictionary;
 RleBpEncoder encoder;
 bool written_value;
};
class StringColumnWriter : public BasicColumnWriter {
public:
 StringColumnWriter(ParquetWriter &writer, idx_t schema_idx, vector<string> schema_path_p, idx_t max_repeat,
                    idx_t max_define, bool can_have_nulls)
     : BasicColumnWriter(writer, schema_idx, std::move(schema_path_p), max_repeat, max_define, can_have_nulls) {
 }
 ~StringColumnWriter() override = default;
public:
 unique_ptr<ColumnWriterStatistics> InitializeStatsState() override {
  return make_uniq<StringStatisticsState>();
 }
 unique_ptr<ColumnWriterState> InitializeWriteState(duckdb_parquet::RowGroup &row_group) override {
  auto result = make_uniq<StringColumnWriterState>(row_group, row_group.columns.size());
  RegisterToRowGroup(row_group);
  return std::move(result);
 }
 bool HasAnalyze() override {
  return true;
 }
 void Analyze(ColumnWriterState &state_p, ColumnWriterState *parent, Vector &vector, idx_t count) override {
  auto &state = state_p.Cast<StringColumnWriterState>();
  if (writer.DictionaryCompressionRatioThreshold() == NumericLimits<double>::Maximum() ||
      (state.dictionary.size() > DICTIONARY_ANALYZE_THRESHOLD && WontUseDictionary(state))) {
   return;
  }
  idx_t vcount = parent ? parent->definition_levels.size() - state.definition_levels.size() : count;
  idx_t parent_index = state.definition_levels.size();
  auto &validity = FlatVector::Validity(vector);
  idx_t vector_index = 0;
  uint32_t new_value_index = state.dictionary.size();
  uint32_t last_value_index = -1;
  idx_t run_length = 0;
  idx_t run_count = 0;
  auto strings = FlatVector::GetData<string_t>(vector);
  for (idx_t i = 0; i < vcount; i++) {
   if (parent && !parent->is_empty.empty() && parent->is_empty[parent_index + i]) {
    continue;
   }
   if (validity.RowIsValid(vector_index)) {
    run_length++;
    const auto &value = strings[vector_index];
    auto found = state.dictionary.insert(string_map_t<uint32_t>::value_type(value, new_value_index));
    state.estimated_plain_size += value.GetSize() + STRING_LENGTH_SIZE;
    if (found.second) {
     new_value_index++;
     state.estimated_dict_page_size += value.GetSize() + MAX_DICTIONARY_KEY_SIZE;
    }
    if (last_value_index != found.first->second) {
     state.estimated_rle_pages_size += ParquetDecodeUtils::GetVarintSize(run_length);
     run_length = 0;
     run_count++;
     last_value_index = found.first->second;
    }
   }
   vector_index++;
  }
  state.estimated_rle_pages_size += MAX_DICTIONARY_KEY_SIZE * run_count;
 }
 void FinalizeAnalyze(ColumnWriterState &state_p) override {
  auto &state = state_p.Cast<StringColumnWriterState>();
  if (WontUseDictionary(state)) {
   state.dictionary.clear();
   state.key_bit_width = 0;
  } else {
   state.key_bit_width = RleBpDecoder::ComputeBitWidth(state.dictionary.size());
  }
 }
 void WriteVector(WriteStream &temp_writer, ColumnWriterStatistics *stats_p, ColumnWriterPageState *page_state_p,
                  Vector &input_column, idx_t chunk_start, idx_t chunk_end) override {
  auto &page_state = page_state_p->Cast<StringWriterPageState>();
  auto &mask = FlatVector::Validity(input_column);
  auto &stats = stats_p->Cast<StringStatisticsState>();
  auto *ptr = FlatVector::GetData<string_t>(input_column);
  if (page_state.IsDictionaryEncoded()) {
   for (idx_t r = chunk_start; r < chunk_end; r++) {
    if (!mask.RowIsValid(r)) {
     continue;
    }
    auto value_index = page_state.dictionary.at(ptr[r]);
    if (!page_state.written_value) {
     temp_writer.Write<uint8_t>(page_state.bit_width);
     page_state.encoder.BeginWrite(temp_writer, value_index);
     page_state.written_value = true;
    } else {
     page_state.encoder.WriteValue(temp_writer, value_index);
    }
   }
  } else {
   for (idx_t r = chunk_start; r < chunk_end; r++) {
    if (!mask.RowIsValid(r)) {
     continue;
    }
    stats.Update(ptr[r]);
    temp_writer.Write<uint32_t>(ptr[r].GetSize());
    temp_writer.WriteData(const_data_ptr_cast(ptr[r].GetData()), ptr[r].GetSize());
   }
  }
 }
 unique_ptr<ColumnWriterPageState> InitializePageState(BasicColumnWriterState &state_p) override {
  auto &state = state_p.Cast<StringColumnWriterState>();
  return make_uniq<StringWriterPageState>(state.key_bit_width, state.dictionary);
 }
 void FlushPageState(WriteStream &temp_writer, ColumnWriterPageState *state_p) override {
  auto &page_state = state_p->Cast<StringWriterPageState>();
  if (page_state.bit_width != 0) {
   if (!page_state.written_value) {
    temp_writer.Write<uint8_t>(page_state.bit_width);
    return;
   }
   page_state.encoder.FinishWrite(temp_writer);
  }
 }
 duckdb_parquet::Encoding::type GetEncoding(BasicColumnWriterState &state_p) override {
  auto &state = state_p.Cast<StringColumnWriterState>();
  return state.IsDictionaryEncoded() ? Encoding::RLE_DICTIONARY : Encoding::PLAIN;
 }
 bool HasDictionary(BasicColumnWriterState &state_p) override {
  auto &state = state_p.Cast<StringColumnWriterState>();
  return state.IsDictionaryEncoded();
 }
 idx_t DictionarySize(BasicColumnWriterState &state_p) override {
  auto &state = state_p.Cast<StringColumnWriterState>();
  D_ASSERT(state.IsDictionaryEncoded());
  return state.dictionary.size();
 }
 void FlushDictionary(BasicColumnWriterState &state_p, ColumnWriterStatistics *stats_p) override {
  auto &stats = stats_p->Cast<StringStatisticsState>();
  auto &state = state_p.Cast<StringColumnWriterState>();
  if (!state.IsDictionaryEncoded()) {
   return;
  }
  auto values = vector<string_t>(state.dictionary.size());
  for (const auto &entry : state.dictionary) {
   D_ASSERT(values[entry.second].GetSize() == 0);
   values[entry.second] = entry.first;
  }
  auto temp_writer = make_uniq<MemoryStream>(
      MaxValue<idx_t>(NextPowerOfTwo(state.estimated_dict_page_size), MemoryStream::DEFAULT_INITIAL_CAPACITY));
  for (idx_t r = 0; r < values.size(); r++) {
   auto &value = values[r];
   stats.Update(value);
   temp_writer->Write<uint32_t>(value.GetSize());
   temp_writer->WriteData(const_data_ptr_cast((value.GetData())), value.GetSize());
  }
  WriteDictionary(state, std::move(temp_writer), values.size());
 }
 idx_t GetRowSize(const Vector &vector, const idx_t index, const BasicColumnWriterState &state_p) const override {
  auto &state = state_p.Cast<StringColumnWriterState>();
  if (state.IsDictionaryEncoded()) {
   return (state.key_bit_width + 7) / 8;
  } else {
   auto strings = FlatVector::GetData<string_t>(vector);
   return strings[index].GetSize();
  }
 }
private:
 bool WontUseDictionary(StringColumnWriterState &state) const {
  return state.estimated_dict_page_size > MAX_UNCOMPRESSED_DICT_PAGE_SIZE ||
         DictionaryCompressionRatio(state) < writer.DictionaryCompressionRatioThreshold();
 }
 static double DictionaryCompressionRatio(StringColumnWriterState &state) {
  if (state.estimated_plain_size == 0 || state.estimated_rle_pages_size == 0 ||
      state.estimated_dict_page_size == 0) {
   return 1;
  }
  return double(state.estimated_plain_size) /
         double(state.estimated_rle_pages_size + state.estimated_dict_page_size);
 }
};
class WKBColumnWriterState final : public StringColumnWriterState {
public:
 WKBColumnWriterState(ClientContext &context, duckdb_parquet::RowGroup &row_group, idx_t col_idx)
     : StringColumnWriterState(row_group, col_idx), geo_data(), geo_data_writer(context) {
 }
 GeoParquetColumnMetadata geo_data;
 GeoParquetColumnMetadataWriter geo_data_writer;
};
class WKBColumnWriter final : public StringColumnWriter {
public:
 WKBColumnWriter(ClientContext &context_p, ParquetWriter &writer, idx_t schema_idx, vector<string> schema_path_p,
                 idx_t max_repeat, idx_t max_define, bool can_have_nulls, string name)
     : StringColumnWriter(writer, schema_idx, std::move(schema_path_p), max_repeat, max_define, can_have_nulls),
       column_name(std::move(name)), context(context_p) {
  this->writer.GetGeoParquetData().RegisterGeometryColumn(column_name);
 }
 unique_ptr<ColumnWriterState> InitializeWriteState(duckdb_parquet::RowGroup &row_group) override {
  auto result = make_uniq<WKBColumnWriterState>(context, row_group, row_group.columns.size());
  RegisterToRowGroup(row_group);
  return std::move(result);
 }
 void Write(ColumnWriterState &state, Vector &vector, idx_t count) override {
  StringColumnWriter::Write(state, vector, count);
  auto &geo_state = state.Cast<WKBColumnWriterState>();
  geo_state.geo_data_writer.Update(geo_state.geo_data, vector, count);
 }
 void FinalizeWrite(ColumnWriterState &state) override {
  StringColumnWriter::FinalizeWrite(state);
  const auto &geo_state = state.Cast<WKBColumnWriterState>();
  writer.GetGeoParquetData().FlushColumnMeta(column_name, geo_state.geo_data);
 }
private:
 string column_name;
 ClientContext &context;
};
class EnumWriterPageState : public ColumnWriterPageState {
public:
 explicit EnumWriterPageState(uint32_t bit_width) : encoder(bit_width), written_value(false) {
 }
 RleBpEncoder encoder;
 bool written_value;
};
class EnumColumnWriter : public BasicColumnWriter {
public:
 EnumColumnWriter(ParquetWriter &writer, LogicalType enum_type_p, idx_t schema_idx, vector<string> schema_path_p,
                  idx_t max_repeat, idx_t max_define, bool can_have_nulls)
     : BasicColumnWriter(writer, schema_idx, std::move(schema_path_p), max_repeat, max_define, can_have_nulls),
       enum_type(std::move(enum_type_p)) {
  bit_width = RleBpDecoder::ComputeBitWidth(EnumType::GetSize(enum_type));
 }
 ~EnumColumnWriter() override = default;
 LogicalType enum_type;
 uint32_t bit_width;
public:
 unique_ptr<ColumnWriterStatistics> InitializeStatsState() override {
  return make_uniq<StringStatisticsState>();
 }
 template <class T>
 void WriteEnumInternal(WriteStream &temp_writer, Vector &input_column, idx_t chunk_start, idx_t chunk_end,
                        EnumWriterPageState &page_state) {
  auto &mask = FlatVector::Validity(input_column);
  auto *ptr = FlatVector::GetData<T>(input_column);
  for (idx_t r = chunk_start; r < chunk_end; r++) {
   if (mask.RowIsValid(r)) {
    if (!page_state.written_value) {
     temp_writer.Write<uint8_t>(bit_width);
     page_state.encoder.BeginWrite(temp_writer, ptr[r]);
     page_state.written_value = true;
    } else {
     page_state.encoder.WriteValue(temp_writer, ptr[r]);
    }
   }
  }
 }
 void WriteVector(WriteStream &temp_writer, ColumnWriterStatistics *stats_p, ColumnWriterPageState *page_state_p,
                  Vector &input_column, idx_t chunk_start, idx_t chunk_end) override {
  auto &page_state = page_state_p->Cast<EnumWriterPageState>();
  switch (enum_type.InternalType()) {
  case PhysicalType::UINT8:
   WriteEnumInternal<uint8_t>(temp_writer, input_column, chunk_start, chunk_end, page_state);
   break;
  case PhysicalType::UINT16:
   WriteEnumInternal<uint16_t>(temp_writer, input_column, chunk_start, chunk_end, page_state);
   break;
  case PhysicalType::UINT32:
   WriteEnumInternal<uint32_t>(temp_writer, input_column, chunk_start, chunk_end, page_state);
   break;
  default:
   throw InternalException("Unsupported internal enum type");
  }
 }
 unique_ptr<ColumnWriterPageState> InitializePageState(BasicColumnWriterState &state) override {
  return make_uniq<EnumWriterPageState>(bit_width);
 }
 void FlushPageState(WriteStream &temp_writer, ColumnWriterPageState *state_p) override {
  auto &page_state = state_p->Cast<EnumWriterPageState>();
  if (!page_state.written_value) {
   temp_writer.Write<uint8_t>(bit_width);
   return;
  }
  page_state.encoder.FinishWrite(temp_writer);
 }
 duckdb_parquet::Encoding::type GetEncoding(BasicColumnWriterState &state) override {
  return Encoding::RLE_DICTIONARY;
 }
 bool HasDictionary(BasicColumnWriterState &state) override {
  return true;
 }
 idx_t DictionarySize(BasicColumnWriterState &state_p) override {
  return EnumType::GetSize(enum_type);
 }
 void FlushDictionary(BasicColumnWriterState &state, ColumnWriterStatistics *stats_p) override {
  auto &stats = stats_p->Cast<StringStatisticsState>();
  auto &enum_values = EnumType::GetValuesInsertOrder(enum_type);
  auto enum_count = EnumType::GetSize(enum_type);
  auto string_values = FlatVector::GetData<string_t>(enum_values);
  auto temp_writer = make_uniq<MemoryStream>();
  for (idx_t r = 0; r < enum_count; r++) {
   D_ASSERT(!FlatVector::IsNull(enum_values, r));
   stats.Update(string_values[r]);
   temp_writer->Write<uint32_t>(string_values[r].GetSize());
   temp_writer->WriteData(const_data_ptr_cast(string_values[r].GetData()), string_values[r].GetSize());
  }
  WriteDictionary(state, std::move(temp_writer), enum_count);
 }
 idx_t GetRowSize(const Vector &vector, const idx_t index, const BasicColumnWriterState &state) const override {
  return (bit_width + 7) / 8;
 }
};
class StructColumnWriter : public ColumnWriter {
public:
 StructColumnWriter(ParquetWriter &writer, idx_t schema_idx, vector<string> schema_path_p, idx_t max_repeat,
                    idx_t max_define, vector<unique_ptr<ColumnWriter>> child_writers_p, bool can_have_nulls)
     : ColumnWriter(writer, schema_idx, std::move(schema_path_p), max_repeat, max_define, can_have_nulls),
       child_writers(std::move(child_writers_p)) {
 }
 ~StructColumnWriter() override = default;
 vector<unique_ptr<ColumnWriter>> child_writers;
public:
 unique_ptr<ColumnWriterState> InitializeWriteState(duckdb_parquet::RowGroup &row_group) override;
 bool HasAnalyze() override;
 void Analyze(ColumnWriterState &state, ColumnWriterState *parent, Vector &vector, idx_t count) override;
 void FinalizeAnalyze(ColumnWriterState &state) override;
 void Prepare(ColumnWriterState &state, ColumnWriterState *parent, Vector &vector, idx_t count) override;
 void BeginWrite(ColumnWriterState &state) override;
 void Write(ColumnWriterState &state, Vector &vector, idx_t count) override;
 void FinalizeWrite(ColumnWriterState &state) override;
};
class StructColumnWriterState : public ColumnWriterState {
public:
 StructColumnWriterState(duckdb_parquet::RowGroup &row_group, idx_t col_idx)
     : row_group(row_group), col_idx(col_idx) {
 }
 ~StructColumnWriterState() override = default;
 duckdb_parquet::RowGroup &row_group;
 idx_t col_idx;
 vector<unique_ptr<ColumnWriterState>> child_states;
};
unique_ptr<ColumnWriterState> StructColumnWriter::InitializeWriteState(duckdb_parquet::RowGroup &row_group) {
 auto result = make_uniq<StructColumnWriterState>(row_group, row_group.columns.size());
 result->child_states.reserve(child_writers.size());
 for (auto &child_writer : child_writers) {
  result->child_states.push_back(child_writer->InitializeWriteState(row_group));
 }
 return std::move(result);
}
bool StructColumnWriter::HasAnalyze() {
 for (auto &child_writer : child_writers) {
  if (child_writer->HasAnalyze()) {
   return true;
  }
 }
 return false;
}
void StructColumnWriter::Analyze(ColumnWriterState &state_p, ColumnWriterState *parent, Vector &vector, idx_t count) {
 auto &state = state_p.Cast<StructColumnWriterState>();
 auto &child_vectors = StructVector::GetEntries(vector);
 for (idx_t child_idx = 0; child_idx < child_writers.size(); child_idx++) {
  if (child_writers[child_idx]->HasAnalyze()) {
   child_writers[child_idx]->Analyze(*state.child_states[child_idx], &state_p, *child_vectors[child_idx],
                                     count);
  }
 }
}
void StructColumnWriter::FinalizeAnalyze(ColumnWriterState &state_p) {
 auto &state = state_p.Cast<StructColumnWriterState>();
 for (idx_t child_idx = 0; child_idx < child_writers.size(); child_idx++) {
  if (child_writers[child_idx]->HasAnalyze()) {
   child_writers[child_idx]->FinalizeAnalyze(*state.child_states[child_idx]);
  }
 }
}
void StructColumnWriter::Prepare(ColumnWriterState &state_p, ColumnWriterState *parent, Vector &vector, idx_t count) {
 auto &state = state_p.Cast<StructColumnWriterState>();
 auto &validity = FlatVector::Validity(vector);
 if (parent) {
  while (state.is_empty.size() < parent->is_empty.size()) {
   state.is_empty.push_back(parent->is_empty[state.is_empty.size()]);
  }
 }
 HandleRepeatLevels(state_p, parent, count, max_repeat);
 HandleDefineLevels(state_p, parent, validity, count, PARQUET_DEFINE_VALID, max_define - 1);
 auto &child_vectors = StructVector::GetEntries(vector);
 for (idx_t child_idx = 0; child_idx < child_writers.size(); child_idx++) {
  child_writers[child_idx]->Prepare(*state.child_states[child_idx], &state_p, *child_vectors[child_idx], count);
 }
}
void StructColumnWriter::BeginWrite(ColumnWriterState &state_p) {
 auto &state = state_p.Cast<StructColumnWriterState>();
 for (idx_t child_idx = 0; child_idx < child_writers.size(); child_idx++) {
  child_writers[child_idx]->BeginWrite(*state.child_states[child_idx]);
 }
}
void StructColumnWriter::Write(ColumnWriterState &state_p, Vector &vector, idx_t count) {
 auto &state = state_p.Cast<StructColumnWriterState>();
 auto &child_vectors = StructVector::GetEntries(vector);
 for (idx_t child_idx = 0; child_idx < child_writers.size(); child_idx++) {
  child_writers[child_idx]->Write(*state.child_states[child_idx], *child_vectors[child_idx], count);
 }
}
void StructColumnWriter::FinalizeWrite(ColumnWriterState &state_p) {
 auto &state = state_p.Cast<StructColumnWriterState>();
 for (idx_t child_idx = 0; child_idx < child_writers.size(); child_idx++) {
  state.child_states[child_idx]->null_count += state_p.null_count;
  child_writers[child_idx]->FinalizeWrite(*state.child_states[child_idx]);
 }
}
class ListColumnWriter : public ColumnWriter {
public:
 ListColumnWriter(ParquetWriter &writer, idx_t schema_idx, vector<string> schema_path_p, idx_t max_repeat,
                  idx_t max_define, unique_ptr<ColumnWriter> child_writer_p, bool can_have_nulls)
     : ColumnWriter(writer, schema_idx, std::move(schema_path_p), max_repeat, max_define, can_have_nulls),
       child_writer(std::move(child_writer_p)) {
 }
 ~ListColumnWriter() override = default;
 unique_ptr<ColumnWriter> child_writer;
public:
 unique_ptr<ColumnWriterState> InitializeWriteState(duckdb_parquet::RowGroup &row_group) override;
 bool HasAnalyze() override;
 void Analyze(ColumnWriterState &state, ColumnWriterState *parent, Vector &vector, idx_t count) override;
 void FinalizeAnalyze(ColumnWriterState &state) override;
 void Prepare(ColumnWriterState &state, ColumnWriterState *parent, Vector &vector, idx_t count) override;
 void BeginWrite(ColumnWriterState &state) override;
 void Write(ColumnWriterState &state, Vector &vector, idx_t count) override;
 void FinalizeWrite(ColumnWriterState &state) override;
};
class ListColumnWriterState : public ColumnWriterState {
public:
 ListColumnWriterState(duckdb_parquet::RowGroup &row_group, idx_t col_idx) : row_group(row_group), col_idx(col_idx) {
 }
 ~ListColumnWriterState() override = default;
 duckdb_parquet::RowGroup &row_group;
 idx_t col_idx;
 unique_ptr<ColumnWriterState> child_state;
 idx_t parent_index = 0;
};
unique_ptr<ColumnWriterState> ListColumnWriter::InitializeWriteState(duckdb_parquet::RowGroup &row_group) {
 auto result = make_uniq<ListColumnWriterState>(row_group, row_group.columns.size());
 result->child_state = child_writer->InitializeWriteState(row_group);
 return std::move(result);
}
bool ListColumnWriter::HasAnalyze() {
 return child_writer->HasAnalyze();
}
void ListColumnWriter::Analyze(ColumnWriterState &state_p, ColumnWriterState *parent, Vector &vector, idx_t count) {
 auto &state = state_p.Cast<ListColumnWriterState>();
 auto &list_child = ListVector::GetEntry(vector);
 auto list_count = ListVector::GetListSize(vector);
 child_writer->Analyze(*state.child_state, &state_p, list_child, list_count);
}
void ListColumnWriter::FinalizeAnalyze(ColumnWriterState &state_p) {
 auto &state = state_p.Cast<ListColumnWriterState>();
 child_writer->FinalizeAnalyze(*state.child_state);
}
idx_t GetConsecutiveChildList(Vector &list, Vector &result, idx_t offset, idx_t count) {
 auto &validity = FlatVector::Validity(list);
 auto list_entries = FlatVector::GetData<list_entry_t>(list);
 bool is_consecutive = true;
 idx_t total_length = 0;
 for (idx_t c = offset; c < offset + count; c++) {
  if (!validity.RowIsValid(c)) {
   continue;
  }
  if (list_entries[c].offset != total_length) {
   is_consecutive = false;
  }
  total_length += list_entries[c].length;
 }
 if (is_consecutive) {
  return total_length;
 }
 SelectionVector sel(total_length);
 idx_t index = 0;
 for (idx_t c = offset; c < offset + count; c++) {
  if (!validity.RowIsValid(c)) {
   continue;
  }
  for (idx_t k = 0; k < list_entries[c].length; k++) {
   sel.set_index(index++, list_entries[c].offset + k);
  }
 }
 result.Slice(sel, total_length);
 result.Flatten(total_length);
 return total_length;
}
void ListColumnWriter::Prepare(ColumnWriterState &state_p, ColumnWriterState *parent, Vector &vector, idx_t count) {
 auto &state = state_p.Cast<ListColumnWriterState>();
 auto list_data = FlatVector::GetData<list_entry_t>(vector);
 auto &validity = FlatVector::Validity(vector);
 idx_t start = 0;
 idx_t vcount = parent ? parent->definition_levels.size() - state.parent_index : count;
 idx_t vector_index = 0;
 for (idx_t i = start; i < vcount; i++) {
  idx_t parent_index = state.parent_index + i;
  if (parent && !parent->is_empty.empty() && parent->is_empty[parent_index]) {
   state.definition_levels.push_back(parent->definition_levels[parent_index]);
   state.repetition_levels.push_back(parent->repetition_levels[parent_index]);
   state.is_empty.push_back(true);
   continue;
  }
  auto first_repeat_level =
      parent && !parent->repetition_levels.empty() ? parent->repetition_levels[parent_index] : max_repeat;
  if (parent && parent->definition_levels[parent_index] != PARQUET_DEFINE_VALID) {
   state.definition_levels.push_back(parent->definition_levels[parent_index]);
   state.repetition_levels.push_back(first_repeat_level);
   state.is_empty.push_back(true);
  } else if (validity.RowIsValid(vector_index)) {
   if (list_data[vector_index].length == 0) {
    state.definition_levels.push_back(max_define);
    state.is_empty.push_back(true);
   } else {
    state.definition_levels.push_back(PARQUET_DEFINE_VALID);
    state.is_empty.push_back(false);
   }
   state.repetition_levels.push_back(first_repeat_level);
   for (idx_t k = 1; k < list_data[vector_index].length; k++) {
    state.repetition_levels.push_back(max_repeat + 1);
    state.definition_levels.push_back(PARQUET_DEFINE_VALID);
    state.is_empty.push_back(false);
   }
  } else {
   if (!can_have_nulls) {
    throw IOException("Parquet writer: map key column is not allowed to contain NULL values");
   }
   state.definition_levels.push_back(max_define - 1);
   state.repetition_levels.push_back(first_repeat_level);
   state.is_empty.push_back(true);
  }
  vector_index++;
 }
 state.parent_index += vcount;
 auto &list_child = ListVector::GetEntry(vector);
 Vector child_list(list_child);
 auto child_length = GetConsecutiveChildList(vector, child_list, 0, count);
 child_writer->Prepare(*state.child_state, &state_p, child_list, child_length);
}
void ListColumnWriter::BeginWrite(ColumnWriterState &state_p) {
 auto &state = state_p.Cast<ListColumnWriterState>();
 child_writer->BeginWrite(*state.child_state);
}
void ListColumnWriter::Write(ColumnWriterState &state_p, Vector &vector, idx_t count) {
 auto &state = state_p.Cast<ListColumnWriterState>();
 auto &list_child = ListVector::GetEntry(vector);
 Vector child_list(list_child);
 auto child_length = GetConsecutiveChildList(vector, child_list, 0, count);
 child_writer->Write(*state.child_state, child_list, child_length);
}
void ListColumnWriter::FinalizeWrite(ColumnWriterState &state_p) {
 auto &state = state_p.Cast<ListColumnWriterState>();
 child_writer->FinalizeWrite(*state.child_state);
}
class ArrayColumnWriter : public ListColumnWriter {
public:
 ArrayColumnWriter(ParquetWriter &writer, idx_t schema_idx, vector<string> schema_path_p, idx_t max_repeat,
                   idx_t max_define, unique_ptr<ColumnWriter> child_writer_p, bool can_have_nulls)
     : ListColumnWriter(writer, schema_idx, std::move(schema_path_p), max_repeat, max_define,
                        std::move(child_writer_p), can_have_nulls) {
 }
 ~ArrayColumnWriter() override = default;
public:
 void Analyze(ColumnWriterState &state, ColumnWriterState *parent, Vector &vector, idx_t count) override;
 void Prepare(ColumnWriterState &state, ColumnWriterState *parent, Vector &vector, idx_t count) override;
 void Write(ColumnWriterState &state, Vector &vector, idx_t count) override;
};
void ArrayColumnWriter::Analyze(ColumnWriterState &state_p, ColumnWriterState *parent, Vector &vector, idx_t count) {
 auto &state = state_p.Cast<ListColumnWriterState>();
 auto &array_child = ArrayVector::GetEntry(vector);
 auto array_size = ArrayType::GetSize(vector.GetType());
 child_writer->Analyze(*state.child_state, &state_p, array_child, array_size * count);
}
void ArrayColumnWriter::Prepare(ColumnWriterState &state_p, ColumnWriterState *parent, Vector &vector, idx_t count) {
 auto &state = state_p.Cast<ListColumnWriterState>();
 auto array_size = ArrayType::GetSize(vector.GetType());
 auto &validity = FlatVector::Validity(vector);
 idx_t start = 0;
 idx_t vcount = parent ? parent->definition_levels.size() - state.parent_index : count;
 idx_t vector_index = 0;
 for (idx_t i = start; i < vcount; i++) {
  idx_t parent_index = state.parent_index + i;
  if (parent && !parent->is_empty.empty() && parent->is_empty[parent_index]) {
   state.definition_levels.push_back(parent->definition_levels[parent_index]);
   state.repetition_levels.push_back(parent->repetition_levels[parent_index]);
   state.is_empty.push_back(true);
   continue;
  }
  auto first_repeat_level =
      parent && !parent->repetition_levels.empty() ? parent->repetition_levels[parent_index] : max_repeat;
  if (parent && parent->definition_levels[parent_index] != PARQUET_DEFINE_VALID) {
   state.definition_levels.push_back(parent->definition_levels[parent_index]);
   state.repetition_levels.push_back(first_repeat_level);
   state.is_empty.push_back(false);
   for (idx_t k = 1; k < array_size; k++) {
    state.repetition_levels.push_back(max_repeat + 1);
    state.definition_levels.push_back(parent->definition_levels[parent_index]);
    state.is_empty.push_back(false);
   }
  } else if (validity.RowIsValid(vector_index)) {
   state.definition_levels.push_back(PARQUET_DEFINE_VALID);
   state.is_empty.push_back(false);
   state.repetition_levels.push_back(first_repeat_level);
   for (idx_t k = 1; k < array_size; k++) {
    state.repetition_levels.push_back(max_repeat + 1);
    state.definition_levels.push_back(PARQUET_DEFINE_VALID);
    state.is_empty.push_back(false);
   }
  } else {
   state.definition_levels.push_back(max_define - 1);
   state.repetition_levels.push_back(first_repeat_level);
   state.is_empty.push_back(false);
   for (idx_t k = 1; k < array_size; k++) {
    state.repetition_levels.push_back(max_repeat + 1);
    state.definition_levels.push_back(max_define - 1);
    state.is_empty.push_back(false);
   }
  }
  vector_index++;
 }
 state.parent_index += vcount;
 auto &array_child = ArrayVector::GetEntry(vector);
 child_writer->Prepare(*state.child_state, &state_p, array_child, count * array_size);
}
void ArrayColumnWriter::Write(ColumnWriterState &state_p, Vector &vector, idx_t count) {
 auto &state = state_p.Cast<ListColumnWriterState>();
 auto array_size = ArrayType::GetSize(vector.GetType());
 auto &array_child = ArrayVector::GetEntry(vector);
 child_writer->Write(*state.child_state, array_child, count * array_size);
}
unique_ptr<ColumnWriter> ColumnWriter::CreateWriterRecursive(ClientContext &context,
                                                             vector<duckdb_parquet::SchemaElement> &schemas,
                                                             ParquetWriter &writer, const LogicalType &type,
                                                             const string &name, vector<string> schema_path,
                                                             optional_ptr<const ChildFieldIDs> field_ids,
                                                             idx_t max_repeat, idx_t max_define, bool can_have_nulls) {
 auto null_type = can_have_nulls ? FieldRepetitionType::OPTIONAL : FieldRepetitionType::REQUIRED;
 if (!can_have_nulls) {
  max_define--;
 }
 idx_t schema_idx = schemas.size();
 optional_ptr<const FieldID> field_id;
 optional_ptr<const ChildFieldIDs> child_field_ids;
 if (field_ids) {
  auto field_id_it = field_ids->ids->find(name);
  if (field_id_it != field_ids->ids->end()) {
   field_id = &field_id_it->second;
   child_field_ids = &field_id->child_field_ids;
  }
 }
 if (type.id() == LogicalTypeId::STRUCT || type.id() == LogicalTypeId::UNION) {
  auto &child_types = StructType::GetChildTypes(type);
  duckdb_parquet::SchemaElement schema_element;
  schema_element.repetition_type = null_type;
  schema_element.num_children = UnsafeNumericCast<int32_t>(child_types.size());
  schema_element.__isset.num_children = true;
  schema_element.__isset.type = false;
  schema_element.__isset.repetition_type = true;
  schema_element.name = name;
  if (field_id && field_id->set) {
   schema_element.__isset.field_id = true;
   schema_element.field_id = field_id->field_id;
  }
  schemas.push_back(std::move(schema_element));
  schema_path.push_back(name);
  vector<unique_ptr<ColumnWriter>> child_writers;
  child_writers.reserve(child_types.size());
  for (auto &child_type : child_types) {
   child_writers.push_back(CreateWriterRecursive(context, schemas, writer, child_type.second, child_type.first,
                                                 schema_path, child_field_ids, max_repeat, max_define + 1));
  }
  return make_uniq<StructColumnWriter>(writer, schema_idx, std::move(schema_path), max_repeat, max_define,
                                       std::move(child_writers), can_have_nulls);
 }
 if (type.id() == LogicalTypeId::LIST || type.id() == LogicalTypeId::ARRAY) {
  auto is_list = type.id() == LogicalTypeId::LIST;
  auto &child_type = is_list ? ListType::GetChildType(type) : ArrayType::GetChildType(type);
  duckdb_parquet::SchemaElement optional_element;
  optional_element.repetition_type = null_type;
  optional_element.num_children = 1;
  optional_element.converted_type = ConvertedType::LIST;
  optional_element.__isset.num_children = true;
  optional_element.__isset.type = false;
  optional_element.__isset.repetition_type = true;
  optional_element.__isset.converted_type = true;
  optional_element.name = name;
  if (field_id && field_id->set) {
   optional_element.__isset.field_id = true;
   optional_element.field_id = field_id->field_id;
  }
  schemas.push_back(std::move(optional_element));
  schema_path.push_back(name);
  duckdb_parquet::SchemaElement repeated_element;
  repeated_element.repetition_type = FieldRepetitionType::REPEATED;
  repeated_element.num_children = 1;
  repeated_element.__isset.num_children = true;
  repeated_element.__isset.type = false;
  repeated_element.__isset.repetition_type = true;
  repeated_element.name = is_list ? "list" : "array";
  schemas.push_back(std::move(repeated_element));
  schema_path.emplace_back(is_list ? "list" : "array");
  auto child_writer = CreateWriterRecursive(context, schemas, writer, child_type, "element", schema_path,
                                            child_field_ids, max_repeat + 1, max_define + 2);
  if (is_list) {
   return make_uniq<ListColumnWriter>(writer, schema_idx, std::move(schema_path), max_repeat, max_define,
                                      std::move(child_writer), can_have_nulls);
  } else {
   return make_uniq<ArrayColumnWriter>(writer, schema_idx, std::move(schema_path), max_repeat, max_define,
                                       std::move(child_writer), can_have_nulls);
  }
 }
 if (type.id() == LogicalTypeId::MAP) {
  duckdb_parquet::SchemaElement top_element;
  top_element.repetition_type = null_type;
  top_element.num_children = 1;
  top_element.converted_type = ConvertedType::MAP;
  top_element.__isset.repetition_type = true;
  top_element.__isset.num_children = true;
  top_element.__isset.converted_type = true;
  top_element.__isset.type = false;
  top_element.name = name;
  if (field_id && field_id->set) {
   top_element.__isset.field_id = true;
   top_element.field_id = field_id->field_id;
  }
  schemas.push_back(std::move(top_element));
  schema_path.push_back(name);
  duckdb_parquet::SchemaElement kv_element;
  kv_element.repetition_type = FieldRepetitionType::REPEATED;
  kv_element.num_children = 2;
  kv_element.__isset.repetition_type = true;
  kv_element.__isset.num_children = true;
  kv_element.__isset.type = false;
  kv_element.name = "key_value";
  schemas.push_back(std::move(kv_element));
  schema_path.emplace_back("key_value");
  vector<LogicalType> kv_types {MapType::KeyType(type), MapType::ValueType(type)};
  vector<string> kv_names {"key", "value"};
  vector<unique_ptr<ColumnWriter>> child_writers;
  child_writers.reserve(2);
  for (idx_t i = 0; i < 2; i++) {
   bool is_key = i == 0;
   auto child_writer = CreateWriterRecursive(context, schemas, writer, kv_types[i], kv_names[i], schema_path,
                                             child_field_ids, max_repeat + 1, max_define + 2, !is_key);
   child_writers.push_back(std::move(child_writer));
  }
  auto struct_writer = make_uniq<StructColumnWriter>(writer, schema_idx, schema_path, max_repeat, max_define,
                                                     std::move(child_writers), can_have_nulls);
  return make_uniq<ListColumnWriter>(writer, schema_idx, schema_path, max_repeat, max_define,
                                     std::move(struct_writer), can_have_nulls);
 }
 duckdb_parquet::SchemaElement schema_element;
 schema_element.type = ParquetWriter::DuckDBTypeToParquetType(type);
 schema_element.repetition_type = null_type;
 schema_element.__isset.num_children = false;
 schema_element.__isset.type = true;
 schema_element.__isset.repetition_type = true;
 schema_element.name = name;
 if (field_id && field_id->set) {
  schema_element.__isset.field_id = true;
  schema_element.field_id = field_id->field_id;
 }
 ParquetWriter::SetSchemaProperties(type, schema_element);
 schemas.push_back(std::move(schema_element));
 schema_path.push_back(name);
 if (type.id() == LogicalTypeId::BLOB && type.GetAlias() == "WKB_BLOB" &&
     GeoParquetFileMetadata::IsGeoParquetConversionEnabled(context)) {
  return make_uniq<WKBColumnWriter>(context, writer, schema_idx, std::move(schema_path), max_repeat, max_define,
                                    can_have_nulls, name);
 }
 switch (type.id()) {
 case LogicalTypeId::BOOLEAN:
  return make_uniq<BooleanColumnWriter>(writer, schema_idx, std::move(schema_path), max_repeat, max_define,
                                        can_have_nulls);
 case LogicalTypeId::TINYINT:
  return make_uniq<StandardColumnWriter<int8_t, int32_t>>(writer, schema_idx, std::move(schema_path), max_repeat,
                                                          max_define, can_have_nulls);
 case LogicalTypeId::SMALLINT:
  return make_uniq<StandardColumnWriter<int16_t, int32_t>>(writer, schema_idx, std::move(schema_path), max_repeat,
                                                           max_define, can_have_nulls);
 case LogicalTypeId::INTEGER:
 case LogicalTypeId::DATE:
  return make_uniq<StandardColumnWriter<int32_t, int32_t>>(writer, schema_idx, std::move(schema_path), max_repeat,
                                                           max_define, can_have_nulls);
 case LogicalTypeId::BIGINT:
 case LogicalTypeId::TIME:
 case LogicalTypeId::TIMESTAMP:
 case LogicalTypeId::TIMESTAMP_TZ:
 case LogicalTypeId::TIMESTAMP_MS:
  return make_uniq<StandardColumnWriter<int64_t, int64_t>>(writer, schema_idx, std::move(schema_path), max_repeat,
                                                           max_define, can_have_nulls);
 case LogicalTypeId::TIME_TZ:
  return make_uniq<StandardColumnWriter<dtime_tz_t, int64_t, ParquetTimeTZOperator>>(
      writer, schema_idx, std::move(schema_path), max_repeat, max_define, can_have_nulls);
 case LogicalTypeId::HUGEINT:
  return make_uniq<StandardColumnWriter<hugeint_t, double, ParquetHugeintOperator>>(
      writer, schema_idx, std::move(schema_path), max_repeat, max_define, can_have_nulls);
 case LogicalTypeId::UHUGEINT:
  return make_uniq<StandardColumnWriter<uhugeint_t, double, ParquetUhugeintOperator>>(
      writer, schema_idx, std::move(schema_path), max_repeat, max_define, can_have_nulls);
 case LogicalTypeId::TIMESTAMP_NS:
  return make_uniq<StandardColumnWriter<int64_t, int64_t, ParquetTimestampNSOperator>>(
      writer, schema_idx, std::move(schema_path), max_repeat, max_define, can_have_nulls);
 case LogicalTypeId::TIMESTAMP_SEC:
  return make_uniq<StandardColumnWriter<int64_t, int64_t, ParquetTimestampSOperator>>(
      writer, schema_idx, std::move(schema_path), max_repeat, max_define, can_have_nulls);
 case LogicalTypeId::UTINYINT:
  return make_uniq<StandardColumnWriter<uint8_t, int32_t>>(writer, schema_idx, std::move(schema_path), max_repeat,
                                                           max_define, can_have_nulls);
 case LogicalTypeId::USMALLINT:
  return make_uniq<StandardColumnWriter<uint16_t, int32_t>>(writer, schema_idx, std::move(schema_path),
                                                            max_repeat, max_define, can_have_nulls);
 case LogicalTypeId::UINTEGER:
  return make_uniq<StandardColumnWriter<uint32_t, uint32_t>>(writer, schema_idx, std::move(schema_path),
                                                             max_repeat, max_define, can_have_nulls);
 case LogicalTypeId::UBIGINT:
  return make_uniq<StandardColumnWriter<uint64_t, uint64_t>>(writer, schema_idx, std::move(schema_path),
                                                             max_repeat, max_define, can_have_nulls);
 case LogicalTypeId::FLOAT:
  return make_uniq<StandardColumnWriter<float, float>>(writer, schema_idx, std::move(schema_path), max_repeat,
                                                       max_define, can_have_nulls);
 case LogicalTypeId::DOUBLE:
  return make_uniq<StandardColumnWriter<double, double>>(writer, schema_idx, std::move(schema_path), max_repeat,
                                                         max_define, can_have_nulls);
 case LogicalTypeId::DECIMAL:
  switch (type.InternalType()) {
  case PhysicalType::INT16:
   return make_uniq<StandardColumnWriter<int16_t, int32_t>>(writer, schema_idx, std::move(schema_path),
                                                            max_repeat, max_define, can_have_nulls);
  case PhysicalType::INT32:
   return make_uniq<StandardColumnWriter<int32_t, int32_t>>(writer, schema_idx, std::move(schema_path),
                                                            max_repeat, max_define, can_have_nulls);
  case PhysicalType::INT64:
   return make_uniq<StandardColumnWriter<int64_t, int64_t>>(writer, schema_idx, std::move(schema_path),
                                                            max_repeat, max_define, can_have_nulls);
  default:
   return make_uniq<FixedDecimalColumnWriter>(writer, schema_idx, std::move(schema_path), max_repeat,
                                              max_define, can_have_nulls);
  }
 case LogicalTypeId::BLOB:
 case LogicalTypeId::VARCHAR:
  return make_uniq<StringColumnWriter>(writer, schema_idx, std::move(schema_path), max_repeat, max_define,
                                       can_have_nulls);
 case LogicalTypeId::UUID:
  return make_uniq<UUIDColumnWriter>(writer, schema_idx, std::move(schema_path), max_repeat, max_define,
                                     can_have_nulls);
 case LogicalTypeId::INTERVAL:
  return make_uniq<IntervalColumnWriter>(writer, schema_idx, std::move(schema_path), max_repeat, max_define,
                                         can_have_nulls);
 case LogicalTypeId::ENUM:
  return make_uniq<EnumColumnWriter>(writer, type, schema_idx, std::move(schema_path), max_repeat, max_define,
                                     can_have_nulls);
 default:
  throw InternalException("Unsupported type \"%s\" in Parquet writer", type.ToString());
 }
}
}
