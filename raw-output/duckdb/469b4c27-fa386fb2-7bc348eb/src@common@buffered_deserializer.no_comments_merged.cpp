#include "common/buffered_deserializer.hpp"
using namespace duckdb;
using namespace std;
<<<<<<< HEAD
BufferedDeserializer::BufferedDeserializer(uint8_t *ptr, uint64_t data_size) : ptr(ptr), endptr(ptr + data_size) {
||||||| 7bc348eb96
BufferedDeserializer::BufferedDeserializer(uint8_t *ptr, uint64_t data_size) :
 ptr(ptr), endptr(ptr + data_size) {
=======
BufferedDeserializer::BufferedDeserializer(data_ptr_t ptr, index_t data_size) : ptr(ptr), endptr(ptr + data_size) {
>>>>>>> fa386fb2
}
BufferedDeserializer::BufferedDeserializer(BufferedSerializer &serializer)
    : BufferedDeserializer(serializer.data, serializer.maximum_size) {
}
void BufferedDeserializer::Read(data_ptr_t buffer, index_t read_size) {
 if (ptr + read_size > endptr) {
  throw SerializationException("Failed to deserialize: not enough data in buffer to fulfill read request");
 }
 memcpy(buffer, ptr, read_size);
 ptr += read_size;
}
