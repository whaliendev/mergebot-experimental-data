#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <log/log.h>
#include <stdlib.h>
#include <utils/Compat.h>
#include <utils/FileMap.h>
#include <zlib.h>
#include <JNIHelp.h>
#include <string.h>
#include <unistd.h>
#include <memory>
#include <vector>
#include "base/file.h"
#include "base/macros.h"
#define DISALLOW_IMPLICIT_CONSTRUCTORS(TypeName) \
    TypeName(); \
    TypeName(const TypeName&); \
    void operator=(const TypeName&)
#include "base/memory.h"
#include "log/log.h"
#include "utils/Compat.h"
#include "utils/FileMap.h"
#include "zlib.h"
#include "entry_name_utils-inl.h"
#include "ziparchive/zip_archive.h"
using android::base::get_unaligned;
#ifndef O_BINARY
#define O_BINARY 0
#endif
static const uint32_t kGPBDDFlagMask = 0x0008;
static const uint32_t kMaxCommentLen = 65535;
static const uint32_t kMaxEOCDSearch = kMaxCommentLen + sizeof(EocdRecord);
static const char* kErrorMessages[] = {
  "Unknown return code.",
  "Iteration ended",
  "Zlib error",
  "Invalid file",
  "Invalid handle",
  "Duplicate entries in archive",
  "Empty archive",
  "Entry not found",
  "Invalid offset",
  "Inconsistent information",
  "Invalid entry name",
  "I/O Error",
  "File mapping failed"
};
static const int32_t kErrorMessageUpperBound = 0;
static const int32_t kIterationEnd = -1;
static const int32_t kZlibError = -2;
static const int32_t kInvalidFile = -3;
static const int32_t kInvalidHandle = -4;
static const int32_t kDuplicateEntry = -5;
static const int32_t kEmptyArchive = -6;
static const int32_t kEntryNotFound = -7;
static const int32_t kInvalidOffset = -8;
static const int32_t kInconsistentInformation = -9;
static const int32_t kInvalidEntryName = -10;
static const int32_t kIoError = -11;
static const int32_t kMmapFailed = -12;
static const int32_t kErrorMessageLowerBound = -13;
struct ZipArchive {
  const int fd;
  const bool close_file;
  off64_t directory_offset;
  android::FileMap directory_map;
  uint16_t num_entries;
  uint32_t hash_table_size;
  ZipEntryName* hash_table;
  ZipArchive(const int fd, bool assume_ownership): fd(fd), close_file(assume_ownership), directory_offset(0), num_entries(0), hash_table_size(0), hash_table(NULL) {}
  ~ZipArchive() {
    if (close_file && fd >= 0) {
      close(fd);
    }
    free(hash_table);
  }
};
static uint32_t RoundUpPower2(uint32_t val) {
  val--;
  val |= val >> 1;
  val |= val >> 2;
  val |= val >> 4;
  val |= val >> 8;
  val |= val >> 16;
  val++;
  return val;
}
static uint32_t ComputeHash(const ZipEntryName& name) {
  uint32_t hash = 0;
  uint16_t len = name.name_length;
  const uint8_t* str = name.name;
  while (len--) {
    hash = hash * 31 + *str++;
  }
  return hash;
}
static int64_t EntryToIndex(const ZipEntryName* hash_table, const uint32_t hash_table_size, const ZipEntryName& name) {
  const uint32_t hash = ComputeHash(name);
  uint32_t ent = hash & (hash_table_size - 1);
  while (hash_table[ent].name != NULL) {
    if (hash_table[ent].name_length == name.name_length &&
        memcmp(hash_table[ent].name, name.name, name.name_length) == 0) {
      return ent;
    }
    ent = (ent + 1) & (hash_table_size - 1);
  }
  ALOGV("Zip: Unable to find entry %.*s", name.name_length, name.name);
  return kEntryNotFound;
}
static int32_t AddToHash(ZipEntryName *hash_table, const uint64_t hash_table_size,
                         const ZipEntryName& name) {
  const uint64_t hash = ComputeHash(name);
  uint32_t ent = hash & (hash_table_size - 1);
  while (hash_table[ent].name != NULL) {
    if (hash_table[ent].name_length == name.name_length &&
        memcmp(hash_table[ent].name, name.name, name.name_length) == 0) {
      ALOGW("Zip: Found duplicate entry %.*s", name.name_length, name.name);
      return kDuplicateEntry;
    }
    ent = (ent + 1) & (hash_table_size - 1);
  }
  hash_table[ent].name = name.name;
  hash_table[ent].name_length = name.name_length;
  return 0;
}
static int32_t MapCentralDirectory0(int fd, const char* debug_file_name,
                                    ZipArchive* archive, off64_t file_length,
                                    off64_t read_amount, uint8_t* scan_buffer) {
  const off64_t search_start = file_length - read_amount;
  if (lseek64(fd, search_start, SEEK_SET) != search_start) {
    ALOGW("Zip: seek %" PRId64 " failed: %s", static_cast<int64_t>(search_start),
          strerror(errno));
    return kIoError;
  }
  ssize_t actual = TEMP_FAILURE_RETRY(
      read(fd, scan_buffer, static_cast<size_t>(read_amount)));
  if (actual != static_cast<ssize_t>(read_amount)) {
    ALOGW("Zip: read %" PRId64 " failed: %s", static_cast<int64_t>(read_amount),
          strerror(errno));
    return kIoError;
  }
  int i = read_amount - sizeof(EocdRecord);
  for (; i >= 0; i--) {
    if (scan_buffer[i] == 0x50) {
      uint32_t* sig_addr = reinterpret_cast<uint32_t*>(&scan_buffer[i]);
      if (get_unaligned<uint32_t>(sig_addr) == EocdRecord::kSignature) {
        ALOGV("+++ Found EOCD at buf+%d", i);
        break;
      }
    }
  }
  if (i < 0) {
    ALOGD("Zip: EOCD not found, %s is not zip", debug_file_name);
    return kInvalidFile;
  }
  const off64_t eocd_offset = search_start + i;
  const EocdRecord* eocd = reinterpret_cast<const EocdRecord*>(scan_buffer + i);
  const off64_t calculated_length = eocd_offset + sizeof(EocdRecord)
      + eocd->comment_length;
  if (calculated_length != file_length) {
    ALOGW("Zip: %" PRId64 " extraneous bytes at the end of the central directory",
          static_cast<int64_t>(file_length - calculated_length));
    return kInvalidFile;
  }
  if (static_cast<off64_t>(eocd->cd_start_offset) + eocd->cd_size > eocd_offset) {
    ALOGW("Zip: bad offsets (dir %" PRIu32 ", size %" PRIu32 ", eocd %" PRId64 ")",
        eocd->cd_start_offset, eocd->cd_size, static_cast<int64_t>(eocd_offset));
#if defined(__ANDROID__)
    if (eocd->cd_start_offset + eocd->cd_size <= eocd_offset) {
      android_errorWriteLog(0x534e4554, "31251826");
    }
#endif
    return kInvalidOffset;
  }
  if (eocd->num_records == 0) {
    ALOGW("Zip: empty archive?");
    return kEmptyArchive;
  }
  ALOGV("+++ num_entries=%" PRIu32 "dir_size=%" PRIu32 " dir_offset=%" PRIu32,
        eocd->num_records, eocd->cd_size, eocd->cd_start_offset);
  if (!archive->directory_map.create(debug_file_name, fd,
          static_cast<off64_t>(eocd->cd_start_offset),
          static_cast<size_t>(eocd->cd_size), true ) ) {
    return kMmapFailed;
  }
  archive->num_entries = eocd->num_records;
  archive->directory_offset = eocd->cd_start_offset;
  return 0;
}
static int32_t MapCentralDirectory(int fd, const char* debug_file_name,
                                   ZipArchive* archive) {
  off64_t file_length = lseek64(fd, 0, SEEK_END);
  if (file_length == -1) {
    ALOGV("Zip: lseek on fd %d failed", fd);
    return kInvalidFile;
  }
  if (file_length > static_cast<off64_t>(0xffffffff)) {
    ALOGV("Zip: zip file too long %" PRId64, static_cast<int64_t>(file_length));
    return kInvalidFile;
  }
  if (file_length < static_cast<off64_t>(sizeof(EocdRecord))) {
    ALOGV("Zip: length %" PRId64 " is too small to be zip", static_cast<int64_t>(file_length));
    return kInvalidFile;
  }
  off64_t read_amount = kMaxEOCDSearch;
  if (file_length < read_amount) {
    read_amount = file_length;
  }
  uint8_t* scan_buffer = reinterpret_cast<uint8_t*>(malloc(read_amount));
  int32_t result = MapCentralDirectory0(fd, debug_file_name, archive,
                                        file_length, read_amount, scan_buffer);
  free(scan_buffer);
  return result;
}
static inline ssize_t ReadAtOffset(int fd, uint8_t* buf, size_t len, off64_t off);
static int32_t ParseZipArchive(ZipArchive* archive) {
  const uint8_t* const cd_ptr =
      reinterpret_cast<const uint8_t*>(archive->directory_map.getDataPtr());
  const size_t cd_length = archive->directory_map.getDataLength();
  const uint16_t num_entries = archive->num_entries;
  archive->hash_table_size = RoundUpPower2(1 + (num_entries * 4) / 3);
  archive->hash_table = reinterpret_cast<ZipEntryName*>(calloc(archive->hash_table_size,
      sizeof(ZipEntryName)));
  const uint8_t* const cd_end = cd_ptr + cd_length;
  const uint8_t* ptr = cd_ptr;
  for (uint16_t i = 0; i < num_entries; i++) {
    if (ptr > cd_end - sizeof(CentralDirectoryRecord)) {
      ALOGW("Zip: ran off the end (at %" PRIu16 ")", i);
#if defined(__ANDROID__)
      android_errorWriteLog(0x534e4554, "36392138");
#endif
      return -1;
    }
    const CentralDirectoryRecord* cdr =
        reinterpret_cast<const CentralDirectoryRecord*>(ptr);
    if (cdr->record_signature != CentralDirectoryRecord::kSignature) {
      ALOGW("Zip: missed a central dir sig (at %" PRIu16 ")", i);
      return -1;
    }
    const off64_t local_header_offset = cdr->local_file_header_offset;
    if (local_header_offset >= archive->directory_offset) {
      ALOGW("Zip: bad LFH offset %" PRId64 " at entry %" PRIu16,
          static_cast<int64_t>(local_header_offset), i);
      return -1;
    }
    const uint16_t file_name_length = cdr->file_name_length;
    const uint16_t extra_length = cdr->extra_field_length;
    const uint16_t comment_length = cdr->comment_length;
    const uint8_t* file_name = ptr + sizeof(CentralDirectoryRecord);
    if (!IsValidEntryName(file_name, file_name_length)) {
      return -1;
    }
    ZipEntryName entry_name;
    entry_name.name = file_name;
    entry_name.name_length = file_name_length;
    const int add_result = AddToHash(archive->hash_table,
        archive->hash_table_size, entry_name);
    if (add_result != 0) {
      ALOGW("Zip: Error adding entry to hash table %d", add_result);
      return add_result;
    }
    ptr += sizeof(CentralDirectoryRecord) + file_name_length + extra_length + comment_length;
    if ((ptr - cd_ptr) > static_cast<int64_t>(cd_length)) {
      ALOGW("Zip: bad CD advance (%tu vs %zu) at entry %" PRIu16,
          ptr - cd_ptr, cd_length, i);
      return -1;
    }
  }
<<<<<<< HEAD
  uint32_t lfh_start_bytes;
  if (ReadAtOffset(archive->fd, reinterpret_cast<uint8_t*>(&lfh_start_bytes),
                   sizeof(uint32_t), 0) != sizeof(uint32_t)) {
    ALOGW("Zip: Unable to read header for entry at offset == 0.");
    return -1;
  }
  if (lfh_start_bytes != LocalFileHeader::kSignature) {
    ALOGW("Zip: Entry at offset zero has invalid LFH signature %" PRIx32, lfh_start_bytes);
#if defined(__ANDROID__)
    android_errorWriteLog(0x534e4554, "64211847");
#endif
    return -1;
  }
||||||| 3065de2c8
=======
  uint32_t lfh_start_bytes;
  if (!archive->mapped_zip.ReadAtOffset(reinterpret_cast<uint8_t*>(&lfh_start_bytes),
                                        sizeof(uint32_t), 0)) {
    ALOGW("Zip: Unable to read header for entry at offset == 0.");
    return -1;
  }
  if (lfh_start_bytes != LocalFileHeader::kSignature) {
    ALOGW("Zip: Entry at offset zero has invalid LFH signature %" PRIx32, lfh_start_bytes);
#if defined(__ANDROID__)
    android_errorWriteLog(0x534e4554, "64211847");
#endif
    return -1;
  }
>>>>>>> fcf9c96f
  ALOGV("+++ zip good scan %" PRIu16 " entries", num_entries);
  return 0;
}
static int32_t OpenArchiveInternal(ZipArchive* archive,
                                   const char* debug_file_name) {
  int32_t result = -1;
  if ((result = MapCentralDirectory(archive->fd, debug_file_name, archive))) {
    return result;
  }
  if ((result = ParseZipArchive(archive))) {
    return result;
  }
  return 0;
}
int32_t OpenArchiveFd(int fd, const char* debug_file_name,
                      ZipArchiveHandle* handle, bool assume_ownership) {
  ZipArchive* archive = new ZipArchive(fd, assume_ownership);
  *handle = archive;
  return OpenArchiveInternal(archive, debug_file_name);
}
int32_t OpenArchive(const char* fileName, ZipArchiveHandle* handle) {
  const int fd = open(fileName, O_RDONLY | O_BINARY, 0);
  ZipArchive* archive = new ZipArchive(fd, true);
  *handle = archive;
  if (fd < 0) {
    ALOGW("Unable to open '%s': %s", fileName, strerror(errno));
    return kIoError;
  }
  return OpenArchiveInternal(archive, fileName);
}
void CloseArchive(ZipArchiveHandle handle) {
  ZipArchive* archive = reinterpret_cast<ZipArchive*>(handle);
  ALOGV("Closing archive %p", archive);
  delete archive;
}
static int32_t UpdateEntryFromDataDescriptor(int fd,
                                             ZipEntry *entry) {
  uint8_t ddBuf[sizeof(DataDescriptor) + sizeof(DataDescriptor::kOptSignature)];
  ssize_t actual = TEMP_FAILURE_RETRY(read(fd, ddBuf, sizeof(ddBuf)));
  if (actual != sizeof(ddBuf)) {
    return kIoError;
  }
  const uint32_t ddSignature = *(reinterpret_cast<const uint32_t*>(ddBuf));
  const uint16_t offset = (ddSignature == DataDescriptor::kOptSignature) ? 4 : 0;
  const DataDescriptor* descriptor = reinterpret_cast<const DataDescriptor*>(ddBuf + offset);
  entry->crc32 = descriptor->crc32;
  entry->compressed_length = descriptor->compressed_size;
  entry->uncompressed_length = descriptor->uncompressed_size;
  return 0;
}
static inline ssize_t ReadAtOffset(int fd, uint8_t* buf, size_t len,
                                   off64_t off) {
#if !defined(_WIN32)
  return TEMP_FAILURE_RETRY(pread64(fd, buf, len, off));
#else
  if (lseek64(fd, off, SEEK_SET) != off) {
    ALOGW("Zip: failed seek to offset %" PRId64, off);
    return kIoError;
  }
  return TEMP_FAILURE_RETRY(read(fd, buf, len));
#endif
}
static int32_t FindEntry(const ZipArchive* archive, const int ent,
                         ZipEntry* data) {
  const uint16_t nameLen = archive->hash_table[ent].name_length;
  const uint8_t* ptr = archive->hash_table[ent].name;
  ptr -= sizeof(CentralDirectoryRecord);
  const uint8_t* base_ptr = reinterpret_cast<const uint8_t*>(
    archive->directory_map.getDataPtr());
  if (ptr < base_ptr || ptr > base_ptr + archive->directory_map.getDataLength()) {
    ALOGW("Zip: Invalid entry pointer");
    return kInvalidOffset;
  }
  const CentralDirectoryRecord *cdr =
      reinterpret_cast<const CentralDirectoryRecord*>(ptr);
  const off64_t cd_offset = archive->directory_offset;
  data->method = cdr->compression_method;
  data->mod_time = cdr->last_mod_time;
  data->crc32 = cdr->crc32;
  data->compressed_length = cdr->compressed_size;
  data->uncompressed_length = cdr->uncompressed_size;
  const off64_t local_header_offset = cdr->local_file_header_offset;
  if (local_header_offset + static_cast<off64_t>(sizeof(LocalFileHeader)) >= cd_offset) {
    ALOGW("Zip: bad local hdr offset in zip");
    return kInvalidOffset;
  }
  uint8_t lfh_buf[sizeof(LocalFileHeader)];
  ssize_t actual = ReadAtOffset(archive->fd, lfh_buf, sizeof(lfh_buf),
                                 local_header_offset);
  if (actual != sizeof(lfh_buf)) {
    ALOGW("Zip: failed reading lfh name from offset %" PRId64,
        static_cast<int64_t>(local_header_offset));
    return kIoError;
  }
  const LocalFileHeader *lfh = reinterpret_cast<const LocalFileHeader*>(lfh_buf);
  if (lfh->lfh_signature != LocalFileHeader::kSignature) {
    ALOGW("Zip: didn't find signature at start of lfh, offset=%" PRId64,
        static_cast<int64_t>(local_header_offset));
    return kInvalidOffset;
  }
  if ((lfh->gpb_flags & kGPBDDFlagMask) == 0) {
    data->has_data_descriptor = 0;
    if (data->compressed_length != lfh->compressed_size
        || data->uncompressed_length != lfh->uncompressed_size
        || data->crc32 != lfh->crc32) {
      ALOGW("Zip: size/crc32 mismatch. expected {%" PRIu32 ", %" PRIu32
        ", %" PRIx32 "}, was {%" PRIu32 ", %" PRIu32 ", %" PRIx32 "}",
        data->compressed_length, data->uncompressed_length, data->crc32,
        lfh->compressed_size, lfh->uncompressed_size, lfh->crc32);
      return kInconsistentInformation;
    }
  } else {
    data->has_data_descriptor = 1;
  }
  if (lfh->file_name_length == nameLen) {
    const off64_t name_offset = local_header_offset + sizeof(LocalFileHeader);
    if (name_offset + lfh->file_name_length > cd_offset) {
      ALOGW("Zip: Invalid declared length");
      return kInvalidOffset;
    }
    uint8_t* name_buf = reinterpret_cast<uint8_t*>(malloc(nameLen));
    ssize_t actual = ReadAtOffset(archive->fd, name_buf, nameLen,
                                  name_offset);
    if (actual != nameLen) {
      ALOGW("Zip: failed reading lfh name from offset %" PRId64, static_cast<int64_t>(name_offset));
      free(name_buf);
      return kIoError;
    }
    if (memcmp(archive->hash_table[ent].name, name_buf, nameLen)) {
      free(name_buf);
      return kInconsistentInformation;
    }
    free(name_buf);
  } else {
    ALOGW("Zip: lfh name did not match central directory.");
    return kInconsistentInformation;
  }
  const off64_t data_offset = local_header_offset + sizeof(LocalFileHeader)
      + lfh->file_name_length + lfh->extra_field_length;
  if (data_offset > cd_offset) {
    ALOGW("Zip: bad data offset %" PRId64 " in zip", static_cast<int64_t>(data_offset));
    return kInvalidOffset;
  }
  if (static_cast<off64_t>(data_offset + data->compressed_length) > cd_offset) {
    ALOGW("Zip: bad compressed length in zip (%" PRId64 " + %" PRIu32 " > %" PRId64 ")",
      static_cast<int64_t>(data_offset), data->compressed_length, static_cast<int64_t>(cd_offset));
    return kInvalidOffset;
  }
  if (data->method == kCompressStored &&
    static_cast<off64_t>(data_offset + data->uncompressed_length) > cd_offset) {
     ALOGW("Zip: bad uncompressed length in zip (%" PRId64 " + %" PRIu32 " > %" PRId64 ")",
       static_cast<int64_t>(data_offset), data->uncompressed_length,
       static_cast<int64_t>(cd_offset));
     return kInvalidOffset;
  }
  data->offset = data_offset;
  return 0;
}
struct IterationHandle {
  uint32_t position;
  const uint8_t* prefix;
  const uint16_t prefix_len;
  const uint8_t* suffix;
  const uint16_t suffix_len;
  ZipArchive* archive;
  IterationHandle(const ZipEntryName* prefix_name, const ZipEntryName* suffix_name): prefix(NULL), prefix_len(prefix_name ? prefix_name->name_length : 0), suffix(NULL), suffix_len(suffix_name ? suffix_name->name_length : 0) {
    if (prefix_name) {
      uint8_t* prefix_copy = new uint8_t[prefix_len];
      memcpy(prefix_copy, prefix_name->name, prefix_len);
      prefix = prefix_copy;
    }
    if (suffix_name) {
      uint8_t* suffix_copy = new uint8_t[suffix_len];
      memcpy(suffix_copy, suffix_name->name, suffix_len);
      suffix = suffix_copy;
    }
  }
  ~IterationHandle() = delete;{
    delete[] prefix;
    delete[] suffix;
  }
};
int32_t StartIteration(ZipArchiveHandle handle, void** cookie_ptr, const ZipEntryName* optional_prefix, const ZipEntryName* optional_suffix) {
  ZipArchive* archive = reinterpret_cast<ZipArchive*>(handle);
  if (archive == NULL || archive->hash_table == NULL) {
    ALOGW("Zip: Invalid ZipArchiveHandle");
    return kInvalidHandle;
  }
  IterationHandle* cookie = new IterationHandle(optional_prefix, optional_suffix);
  cookie->position = 0;
  cookie->archive = archive;
  *cookie_ptr = cookie ;
  return 0;
}
void EndIteration(void* cookie) {
  delete reinterpret_cast<IterationHandle*>(cookie);
}
int32_t FindEntry(const ZipArchiveHandle handle, const ZipEntryName& entryName,
                  ZipEntry* data) {
  const ZipArchive* archive = reinterpret_cast<ZipArchive*>(handle);
  if (entryName.name_length == 0) {
    ALOGW("Zip: Invalid filename %.*s", entryName.name_length, entryName.name);
    return kInvalidEntryName;
  }
  const int64_t ent = EntryToIndex(archive->hash_table,
    archive->hash_table_size, entryName);
  if (ent < 0) {
    ALOGV("Zip: Could not find entry %.*s", entryName.name_length, entryName.name);
    return ent;
  }
  return FindEntry(archive, ent, data);
}
int32_t Next(void* cookie, ZipEntry* data, ZipEntryName* name) {
  IterationHandle* handle = reinterpret_cast<IterationHandle*>(cookie);
  if (handle == NULL) {
    return kInvalidHandle;
  }
  ZipArchive* archive = handle->archive;
  if (archive == NULL || archive->hash_table == NULL) {
    ALOGW("Zip: Invalid ZipArchiveHandle");
    return kInvalidHandle;
  }
  const uint32_t currentOffset = handle->position;
  const uint32_t hash_table_length = archive->hash_table_size;
  const ZipEntryName *hash_table = archive->hash_table;
  for (uint32_t i = currentOffset; i < hash_table_length; ++i) {
    if (hash_table[i].name != NULL &&
        (handle->prefix_len == 0 ||
         (hash_table[i].name_length >= handle->prefix_len &&
          memcmp(handle->prefix, hash_table[i].name, handle->prefix_len) == 0)) &&
        (handle->suffix_len == 0 ||
         (hash_table[i].name_length >= handle->suffix_len &&
          memcmp(handle->suffix,
                 hash_table[i].name + hash_table[i].name_length - handle->suffix_len,
                 handle->suffix_len) == 0))) {
      handle->position = (i + 1);
      const int error = FindEntry(archive, i, data);
      if (!error) {
        name->name = hash_table[i].name;
        name->name_length = hash_table[i].name_length;
      }
      return error;
    }
  }
  handle->position = 0;
  return kIterationEnd;
}
class Writer {
public:
  virtual bool Append(uint8_t* buf, size_t buf_size) = 0;
  ~Writer(){}
protected:
  Writer() = default;
private:
  DISALLOW_COPY_AND_ASSIGN(Writer);
};
class MemoryWriter : public Writer {
public:
  MemoryWriter(uint8_t* buf, size_t size): Writer(), buf_(buf), size_(size), bytes_written_(0) {
  }
  virtual bool Append(uint8_t* buf, size_t buf_size) override {
    if (bytes_written_ + buf_size > size_) {
      ALOGW("Zip: Unexpected size " ZD " (declared) vs " ZD " (actual)",
            size_, bytes_written_ + buf_size);
      return false;
    }
    memcpy(buf_ + bytes_written_, buf, buf_size);
    bytes_written_ += buf_size;
    return true;
  }
private:
  uint8_t* const buf_;
  const size_t size_;
  size_t bytes_written_;
};
class FileWriter : public Writer {
public:
  static std::unique_ptr<FileWriter> Create(int fd, const ZipEntry* entry) {
    const uint32_t declared_length = entry->uncompressed_length;
    const off64_t current_offset = lseek64(fd, 0, SEEK_CUR);
    if (current_offset == -1) {
      ALOGW("Zip: unable to seek to current location on fd %d: %s", fd, strerror(errno));
      return nullptr;
    }
    int result = 0;
  #if defined(__linux__)
    if (declared_length > 0) {
      result = TEMP_FAILURE_RETRY(fallocate(fd, 0, current_offset, declared_length));
      if (result == -1 && errno == ENOSPC) {
        ALOGW("Zip: unable to allocate space for file to %" PRId64 ": %s",
              static_cast<int64_t>(declared_length + current_offset), strerror(errno));
        return std::unique_ptr<FileWriter>(nullptr);
      }
    }
  #endif
    result = TEMP_FAILURE_RETRY(ftruncate(fd, declared_length + current_offset));
    if (result == -1) {
      ALOGW("Zip: unable to truncate file to %" PRId64 ": %s",
            static_cast<int64_t>(declared_length + current_offset), strerror(errno));
      return std::unique_ptr<FileWriter>(nullptr);
    }
    return std::unique_ptr<FileWriter>(new FileWriter(fd, declared_length));
  }
  virtual bool Append(uint8_t* buf, size_t buf_size) override {
    if (total_bytes_written_ + buf_size > declared_length_) {
      ALOGW("Zip: Unexpected size " ZD " (declared) vs " ZD " (actual)",
            declared_length_, total_bytes_written_ + buf_size);
      return false;
    }
    const bool result = android::base::WriteFully(fd_, buf, buf_size);
    if (result) {
      total_bytes_written_ += buf_size;
    } else {
      ALOGW("Zip: unable to write " ZD " bytes to file; %s", buf_size, strerror(errno));
    }
    return result;
  }
private:
  FileWriter(const int fd, const size_t declared_length): Writer(), fd_(fd), declared_length_(declared_length), total_bytes_written_(0) {
  }
  const int fd_;
  const size_t declared_length_;
  size_t total_bytes_written_;
};
static inline int zlib_inflateInit2(z_stream* stream, int window_bits) {
  return inflateInit2(stream, window_bits);
}
static int32_t InflateEntryToWriter(int fd, const ZipEntry* entry, Writer* writer, uint64_t* crc_out) {
  const size_t kBufSize = 32768;
  std::vector<uint8_t> read_buf(kBufSize);
  std::vector<uint8_t> write_buf(kBufSize);
  z_stream zstream;
  int zerr;
  memset(&zstream, 0, sizeof(zstream));
  zstream.zalloc = Z_NULL;
  zstream.zfree = Z_NULL;
  zstream.opaque = Z_NULL;
  zstream.next_in = NULL;
  zstream.avail_in = 0;
  zstream.next_out = &write_buf[0];
  zstream.avail_out = kBufSize;
  zstream.data_type = Z_UNKNOWN;
  zerr = zlib_inflateInit2(&zstream, -MAX_WBITS);
  if (zerr != Z_OK) {
    if (zerr == Z_VERSION_ERROR) {
      ALOGE("Installed zlib is not compatible with linked version (%s)",
        ZLIB_VERSION);
    } else {
      ALOGW("Call to inflateInit2 failed (zerr=%d)", zerr);
    }
    return kZlibError;
  }
  auto zstream_deleter = [](z_stream* stream) {
    inflateEnd(stream);
  };
  std::unique_ptr<z_stream, decltype(zstream_deleter)> zstream_guard(&zstream, zstream_deleter);
  const uint32_t uncompressed_length = entry->uncompressed_length;
  uint32_t compressed_length = entry->compressed_length;
  do {
    if (zstream.avail_in == 0) {
      const ZD_TYPE getSize = (compressed_length > kBufSize) ? kBufSize : compressed_length;
      const ZD_TYPE actual = TEMP_FAILURE_RETRY(read(fd, &read_buf[0], getSize));
      if (actual != getSize) {
        ALOGW("Zip: inflate read failed (" ZD " vs " ZD ")", actual, getSize);
        return kIoError;
      }
      compressed_length -= getSize;
      zstream.next_in = &read_buf[0];
      zstream.avail_in = getSize;
    }
    zerr = inflate(&zstream, Z_NO_FLUSH);
    if (zerr != Z_OK && zerr != Z_STREAM_END) {
      ALOGW("Zip: inflate zerr=%d (nIn=%p aIn=%u nOut=%p aOut=%u)",
          zerr, zstream.next_in, zstream.avail_in,
          zstream.next_out, zstream.avail_out);
      return kZlibError;
    }
    if (zstream.avail_out == 0 ||
      (zerr == Z_STREAM_END && zstream.avail_out != kBufSize)) {
      const size_t write_size = zstream.next_out - &write_buf[0];
      if (!writer->Append(&write_buf[0], write_size)) {
        return kInconsistentInformation;
      }
      zstream.next_out = &write_buf[0];
      zstream.avail_out = kBufSize;
    }
  } while (zerr == Z_OK);
  assert(zerr == Z_STREAM_END);
  *crc_out = zstream.adler;
  if (zstream.total_out != uncompressed_length || compressed_length != 0) {
    ALOGW("Zip: size mismatch on inflated file (%lu vs %" PRIu32 ")",
        zstream.total_out, uncompressed_length);
    return kInconsistentInformation;
  }
  return 0;
}
static int32_t CopyEntryToWriter(int fd, const ZipEntry* entry, Writer* writer, uint64_t *crc_out) {
  static const uint32_t kBufSize = 32768;
  std::vector<uint8_t> buf(kBufSize);
  const uint32_t length = entry->uncompressed_length;
  uint32_t count = 0;
  uint64_t crc = 0;
  while (count < length) {
    uint32_t remaining = length - count;
    const ssize_t block_size = (remaining > kBufSize) ? kBufSize : remaining;
    const ssize_t actual = TEMP_FAILURE_RETRY(read(fd, &buf[0], block_size));
    if (actual != block_size) {
      ALOGW("CopyFileToFile: copy read failed (" ZD " vs " ZD ")", actual, block_size);
      return kIoError;
    }
    if (!writer->Append(&buf[0], block_size)) {
      return kIoError;
    }
    crc = crc32(crc, &buf[0], block_size);
    count += block_size;
  }
  *crc_out = crc;
  return 0;
}
int32_t ExtractToWriter(ZipArchiveHandle handle, ZipEntry* entry, Writer* writer) {
  ZipArchive* archive = reinterpret_cast<ZipArchive*>(handle);
  const uint16_t method = entry->method;
  off64_t data_offset = entry->offset;
  if (lseek64(archive->fd, data_offset, SEEK_SET) != data_offset) {
    ALOGW("Zip: lseek to data at %" PRId64 " failed", static_cast<int64_t>(data_offset));
    return kIoError;
  }
  int32_t return_value = -1;
  uint64_t crc = 0;
  if (method == kCompressStored) {
    return_value = CopyEntryToWriter(archive->fd, entry, writer, &crc);
  } else if (method == kCompressDeflated) {
    return_value = InflateEntryToWriter(archive->fd, entry, writer, &crc);
  }
  if (!return_value && entry->has_data_descriptor) {
    return_value = UpdateEntryFromDataDescriptor(archive->fd, entry);
    if (return_value) {
      return return_value;
    }
  }
  if (entry->crc32 != crc && false) {
    ALOGW("Zip: crc mismatch: expected %" PRIu32 ", was %" PRIu64, entry->crc32, crc);
    return kInconsistentInformation;
  }
  return return_value;
}
int32_t ExtractToMemory(ZipArchiveHandle handle, ZipEntry* entry,
                        uint8_t* begin, uint32_t size) {
  std::unique_ptr<Writer> writer(new MemoryWriter(begin, size));
  return ExtractToWriter(handle, entry, writer.get());
}
int32_t ExtractEntryToFile(ZipArchiveHandle handle,
                           ZipEntry* entry, int fd) {
  std::unique_ptr<Writer> writer(FileWriter::Create(fd, entry));
  if (writer.get() == nullptr) {
    return kIoError;
  }
  return ExtractToWriter(handle, entry, writer.get());
}
const char* ErrorCodeString(int32_t error_code) {
  if (error_code > kErrorMessageLowerBound && error_code < kErrorMessageUpperBound) {
    return kErrorMessages[error_code * -1];
  }
  return kErrorMessages[0];
}
int GetFileDescriptor(const ZipArchiveHandle handle) {
  return reinterpret_cast<ZipArchive*>(handle)->fd;
}
struct EocdRecord {
  static const uint32_t kSignature = 0x06054b50;
  uint32_t eocd_signature;
  uint16_t disk_num;
  uint16_t cd_start_disk;
  uint16_t num_records_on_disk;
  uint16_t num_records;
  uint32_t cd_size;
  uint32_t cd_start_offset;
  uint16_t comment_length;
 private:
  EocdRecord() = default;
  DISALLOW_COPY_AND_ASSIGN(EocdRecord);
} __attribute__((packed));
struct CentralDirectoryRecord {
  static const uint32_t kSignature = 0x02014b50;
  uint32_t record_signature;
  uint16_t version_made_by;
  uint16_t version_needed;
  uint16_t gpb_flags;
  uint16_t compression_method;
  uint16_t last_mod_time;
  uint16_t last_mod_date;
  uint32_t crc32;
  uint32_t compressed_size;
  uint32_t uncompressed_size;
  uint16_t file_name_length;
  uint16_t extra_field_length;
  uint16_t comment_length;
  uint16_t file_start_disk;
  uint16_t internal_file_attributes;
  uint32_t external_file_attributes;
  uint32_t local_file_header_offset;
 private:
  CentralDirectoryRecord() = default;
  DISALLOW_COPY_AND_ASSIGN(CentralDirectoryRecord);
} __attribute__((packed));
struct LocalFileHeader {
  static const uint32_t kSignature = 0x04034b50;
  uint32_t lfh_signature;
  uint16_t version_needed;
  uint16_t gpb_flags;
  uint16_t compression_method;
  uint16_t last_mod_time;
  uint16_t last_mod_date;
  uint32_t crc32;
  uint32_t compressed_size;
  uint32_t uncompressed_size;
  uint16_t file_name_length;
  uint16_t extra_field_length;
 private:
  LocalFileHeader() = default;
  DISALLOW_COPY_AND_ASSIGN(LocalFileHeader);
} __attribute__((packed));
struct DataDescriptor {
  static const uint32_t kOptSignature = 0x08074b50;
  uint32_t crc32;
  uint32_t compressed_size;
  uint32_t uncompressed_size;
 private:
  DataDescriptor() = default;
  DISALLOW_COPY_AND_ASSIGN(DataDescriptor);
} __attribute__((packed));
