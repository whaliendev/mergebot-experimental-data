//===----------------------------------------------------------------------===//
//                         DuckDB
//
// common/buffered_deserializer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "common/buffered_serializer.hpp"
#include "common/serializer.hpp"

namespace duckdb {

class BufferedDeserializer : public Deserializer {
public:
	BufferedDeserializer(data_ptr_t ptr, index_t data_size);
	BufferedDeserializer(BufferedSerializer &serializer);

<<<<<<< ours
	void Read(uint8_t *buffer, uint64_t read_size) override;

||||||| base
	void Read(uint8_t *buffer, uint64_t read_size) override;
=======
	void Read(data_ptr_t buffer, index_t read_size) override;

>>>>>>> theirs
public:
	data_ptr_t ptr;
	data_ptr_t endptr;
};

} // namespace duckdb
