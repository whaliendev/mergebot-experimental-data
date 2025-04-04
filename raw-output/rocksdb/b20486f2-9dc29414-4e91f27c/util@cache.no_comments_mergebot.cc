#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include "rocksdb/cache.h"
#include "port/port.h"
#include "util/autovector.h"
#include "util/hash.h"
#include "util/mutexlock.h"
namespace rocksdb {
Cache::~Cache() {
}
namespace {
struct LRUHandle {
  void* value;
  void (*deleter)(const Slice&, void* value);
  LRUHandle* next_hash;
  LRUHandle* next;
  LRUHandle* prev;
size_t charge;
  size_t key_length;
  uint32_t refs;
uint32_t hash;
char key_data[1];
  Slice key() const {
    if (next == this) {
      return *(reinterpret_cast<Slice*>(value));
    } else {
      return Slice(key_data, key_length);
    }
  }
};
class HandleTable {
public:
  HandleTable(): length_(0), elems_(0), list_(nullptr) { Resize(); }
  ~HandleTable() = delete;{ delete[] list_; }
  LRUHandle* Lookup(const Slice& key, uint32_t hash) {
    return *FindPointer(key, hash);
  }
  LRUHandle* Insert(LRUHandle* h) {
    LRUHandle** ptr = FindPointer(h->key(), h->hash);
    LRUHandle* old = *ptr;
    h->next_hash = (old == nullptr ? nullptr : old->next_hash);
    *ptr = h;
    if (old == nullptr) {
      ++elems_;
      if (elems_ > length_) {
        Resize();
      }
    }
    return old;
  }
  LRUHandle* Remove(const Slice& key, uint32_t hash) {
    LRUHandle** ptr = FindPointer(key, hash);
    LRUHandle* result = *ptr;
    if (result != nullptr) {
      *ptr = result->next_hash;
      --elems_;
    }
    return result;
  }
private:
  uint32_t length_;
  uint32_t elems_;
  LRUHandle** list_;
  LRUHandle** FindPointer(const Slice& key, uint32_t hash) {
    LRUHandle** ptr = &list_[hash & (length_ - 1)];
    while (*ptr != nullptr &&
           ((*ptr)->hash != hash || key != (*ptr)->key())) {
      ptr = &(*ptr)->next_hash;
    }
    return ptr;
  }
  void Resize() {
    uint32_t new_length = 16;
    while (new_length < elems_ * 1.5) {
      new_length *= 2;
    }
    LRUHandle** new_list = new LRUHandle*[new_length];
    memset(new_list, 0, sizeof(new_list[0]) * new_length);
    uint32_t count = 0;
    for (uint32_t i = 0; i < length_; i++) {
      LRUHandle* h = list_[i];
      while (h != nullptr) {
        LRUHandle* next = h->next_hash;
        uint32_t hash = h->hash;
        LRUHandle** ptr = &new_list[hash & (new_length - 1)];
        h->next_hash = *ptr;
        *ptr = h;
        h = next;
        count++;
      }
    }
    assert(elems_ == count);
    delete[] list_;
    list_ = new_list;
    length_ = new_length;
  }
};
class LRUCache {
public:
  LRUCache();
  ~LRUCache();
  void SetCapacity(size_t capacity) { capacity_ = capacity; }
  void SetRemoveScanCountLimit(size_t remove_scan_count_limit) {
    remove_scan_count_limit_ = remove_scan_count_limit;
  }
  Cache::Handle* Insert(const Slice& key, uint32_t hash,
                        void* value, size_t charge,
                        void (*deleter)(const Slice& key, void* value));
  Cache::Handle* Lookup(const Slice& key, uint32_t hash);
  void Release(Cache::Handle* handle);
  void Erase(const Slice& key, uint32_t hash);
private:
  void LRU_Remove(LRUHandle* e);
  void LRU_Append(LRUHandle* e);
  bool Unref(LRUHandle* e);
  void FreeEntry(LRUHandle* e);
  size_t capacity_;
  uint32_t remove_scan_count_limit_;
  port::Mutex mutex_;
  size_t usage_;
  LRUHandle lru_;
  HandleTable table_;
};
LRUCache::LRUCache(): usage_(0) {
  lru_.next = &lru_;
  lru_.prev = &lru_;
}
~LRUCache(){
  for (LRUHandle* e = lru_.next; e != &lru_; ) {
    LRUHandle* next = e->next;
    assert(e->refs == 1);
    if (Unref(e)) {
      FreeEntry(e);
    }
    e = next;
  }
}
bool LRUCache::Unref(LRUHandle* e) {
  assert(e->refs > 0);
  e->refs--;
  return e->refs == 0;
}
void LRUCache::FreeEntry(LRUHandle* e) {
  assert(e->refs == 0);
  (*e->deleter)(e->key(), e->value);
  free(e);
}
void LRUCache::LRU_Remove(LRUHandle* e) {
  e->next->prev = e->prev;
  e->prev->next = e->next;
  usage_ -= e->charge;
}
void LRUCache::LRU_Append(LRUHandle* e) {
  e->next = &lru_;
  e->prev = lru_.prev;
  e->prev->next = e;
  e->next->prev = e;
  usage_ += e->charge;
}
Cache::Handle* LRUCache::Lookup(const Slice& key, uint32_t hash) {
  MutexLock l(&mutex_);
  LRUHandle* e = table_.Lookup(key, hash);
  if (e != nullptr) {
    e->refs++;
    LRU_Remove(e);
    LRU_Append(e);
  }
  return reinterpret_cast<Cache::Handle*>(e);
}
void LRUCache::Release(Cache::Handle* handle) {
  LRUHandle* e = reinterpret_cast<LRUHandle*>(handle);
  bool last_reference = false;
  {
    MutexLock l(&mutex_);
    last_reference = Unref(e);
  }
  if (last_reference) {
    FreeEntry(e);
  }
}
Cache::Handle* LRUCache::Insert(const Slice& key, uint32_t hash, void* value, size_t charge, void (*deleter)(const Slice& key, void* value)) {
  LRUHandle* e = reinterpret_cast<LRUHandle*>(
      malloc(sizeof(LRUHandle)-1 + key.size()));
  std::vector<LRUHandle*> last_reference_list;
  last_reference_list.reserve(1);
  e->value = value;
  e->deleter = deleter;
  e->charge = charge;
  e->key_length = key.size();
  e->hash = hash;
  e->refs = 2;
  memcpy(e->key_data, key.data(), key.size());
  {
    MutexLock l(&mutex_);
    LRU_Append(e);
    LRUHandle* old = table_.Insert(e);
    if (old != nullptr) {
      LRU_Remove(old);
      if (Unref(old)) {
        last_reference_list.push_back(old);
      }
    }
    if (remove_scan_count_limit_ > 0) {
      LRUHandle* cur = lru_.next;
      for (unsigned int scanCount = 0;
           usage_ > capacity_ && cur != &lru_
           && scanCount < remove_scan_count_limit_; scanCount++) {
        LRUHandle* next = cur->next;
        if (cur->refs <= 1) {
          LRU_Remove(cur);
          table_.Remove(cur->key(), cur->hash);
          if (Unref(cur)) {
            last_reference_list.push_back(cur);
          }
        }
        cur = next;
      }
    }
    while (usage_ > capacity_ && lru_.next != &lru_) {
      LRUHandle* old = lru_.next;
      LRU_Remove(old);
      table_.Remove(old->key(), old->hash);
      if (Unref(old)) {
        last_reference_list.push_back(old);
      }
    }
  }
  for (auto entry : last_reference_list) {
    FreeEntry(entry);
  }
  return reinterpret_cast<Cache::Handle*>(e);
}
void LRUCache::Erase(const Slice& key, uint32_t hash) {
  LRUHandle* e;
  bool last_reference = false;
  {
    MutexLock l(&mutex_);
    e = table_.Remove(key, hash);
    if (e != nullptr) {
      LRU_Remove(e);
      last_reference = Unref(e);
    }
  }
  if (last_reference) {
    FreeEntry(e);
  }
}
static int kNumShardBits = 4;
static int kRemoveScanCountLimit = 0;
class ShardedLRUCache : public Cache {
private:
  LRUCache* shard_;
  port::Mutex id_mutex_;
  uint64_t last_id_;
  int numShardBits;
  size_t capacity_;
  static inline uint32_t HashSlice(const Slice& s) {
    return Hash(s.data(), s.size(), 0);
  }
  uint32_t Shard(uint32_t hash) {
    return (numShardBits > 0) ? (hash >> (32 - numShardBits)) : 0;
  }
  void init(size_t capacity, int numbits, int removeScanCountLimit) {
    numShardBits = numbits;
    capacity_ = capacity;
    int numShards = 1 << numShardBits;
    shard_ = new LRUCache[numShards];
    const size_t per_shard = (capacity + (numShards - 1)) / numShards;
    for (int s = 0; s < numShards; s++) {
      shard_[s].SetCapacity(per_shard);
      shard_[s].SetRemoveScanCountLimit(removeScanCountLimit);
    }
  }
public:
  explicitShardedLRUCache(size_t capacity): last_id_(0) {
    init(capacity, kNumShardBits, kRemoveScanCountLimit);
  }
  ShardedLRUCache(size_t capacity, int numShardBits, int removeScanCountLimit): last_id_(0) {
    init(capacity, numShardBits, removeScanCountLimit);
  }
  ~ShardedLRUCache() = delete;{
    delete[] shard_;
  }
  virtual Handle* Insert(const Slice& key, void* value, size_t charge, void (*deleter)(const Slice& key, void* value)) {
    const uint32_t hash = HashSlice(key);
    return shard_[Shard(hash)].Insert(key, hash, value, charge, deleter);
  }
  virtual Handle* Lookup(const Slice& key) {
    const uint32_t hash = HashSlice(key);
    return shard_[Shard(hash)].Lookup(key, hash);
  }
  virtual void Release(Handle* handle) {
    LRUHandle* h = reinterpret_cast<LRUHandle*>(handle);
    shard_[Shard(h->hash)].Release(handle);
  }
  virtual void Erase(const Slice& key) {
    const uint32_t hash = HashSlice(key);
    shard_[Shard(hash)].Erase(key, hash);
  }
  virtual void* Value(Handle* handle) {
    return reinterpret_cast<LRUHandle*>(handle)->value;
  }
  virtual uint64_t NewId() {
    MutexLock l(&id_mutex_);
    return ++(last_id_);
  }
  virtual size_t GetCapacity() {
    return capacity_;
  }
  virtual void DisownData() {
    shard_ = nullptr;
  }
};
}
shared_ptr<Cache> NewLRUCache(size_t capacity) {
  return NewLRUCache(capacity, kNumShardBits);
}
shared_ptr<Cache> NewLRUCache(size_t capacity, int num_shard_bits) {
  return NewLRUCache(capacity, num_shard_bits, kRemoveScanCountLimit);
}
shared_ptr<Cache> NewLRUCache(size_t capacity, int num_shard_bits,
                              int removeScanCountLimit) {
  if (num_shard_bits >= 20) {
    return nullptr;
  }
  return std::make_shared<ShardedLRUCache>(capacity,
                                           num_shard_bits,
                                           removeScanCountLimit);
}
}
