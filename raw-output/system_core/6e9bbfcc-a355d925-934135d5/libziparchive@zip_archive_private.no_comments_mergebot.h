       
#include <ziparchive/zip_archive.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <memory>
#include <utility>
#include <vector>
#include "android-base/macros.h"
#include "android-base/mapped_file.h"
#include "zip_cd_entry_map.h"
#include "zip_error.h"
class MappedZipFile {
public:
  explicitMappedZipFile(const int fd): has_fd_(true), fd_(fd), fd_offset_(0), base_ptr_(nullptr), data_length_(-1), data_length_(0) {}
  explicitMappedZipFile(const int fd, off64_t length, off64_t offset): has_fd_(true), fd_(fd), fd_offset_(offset), base_ptr_(nullptr), data_length_(length) {}
  explicitMappedZipFile(const void* address, size_t length): has_fd_(false), fd_(-1), fd_offset_(0), base_ptr_(address), data_length_(static_cast<off64_t>(length)) {}
  bool HasFd() const { return has_fd_; }
  int GetFileDescriptor() const;
  const void* GetBasePtr() const;
  off64_t GetFileOffset() const;
  off64_t GetFileLength() const;
  bool ReadAtOffset(uint8_t* buf, size_t len, off64_t off) const;
private:
  const bool has_fd_;
  const int fd_;
  const off64_t fd_offset_;
  const void* const base_ptr_;
  mutable off64_t data_length_;
};
class CentralDirectory {
public:
  CentralDirectory(void) : base_ptr_(nullptr), length_(0) {}
  const uint8_t* GetBasePtr() const { return base_ptr_; }
  size_t GetMapLength() const { return length_; }
  void Initialize(const void* map_base_ptr, off64_t cd_start_offset, size_t cd_size);
private:
  const uint8_t* base_ptr_;
  size_t length_;
};
struct ZipArchive {
  mutable MappedZipFile mapped_zip;
  const bool close_file;
  off64_t directory_offset;
  CentralDirectory central_directory;
  std::unique_ptr<android::base::MappedFile> directory_map;
  uint16_t num_entries;
  std::unique_ptr<CdEntryMapInterface> cd_entry_map;
  ZipArchive(MappedZipFile&& map, bool assume_ownership);
  ZipArchive(const void* address, size_t length);
  ~ZipArchive();
  bool InitializeCentralDirectory(off64_t cd_start_offset, size_t cd_size);
};
