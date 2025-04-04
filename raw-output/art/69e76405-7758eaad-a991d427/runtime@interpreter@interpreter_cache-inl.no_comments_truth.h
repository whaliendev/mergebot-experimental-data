#ifndef ART_RUNTIME_INTERPRETER_INTERPRETER_CACHE_INL_H_
#define ART_RUNTIME_INTERPRETER_INTERPRETER_CACHE_INL_H_ 
#include "interpreter_cache.h"
#include "thread.h"
namespace art {
inline bool InterpreterCache::Get(Thread* self, const void* key, size_t* value) {
  DCHECK(self->GetInterpreterCache() == this) << "Must be called from owning thread";
  Entry& entry = data_[IndexOf(key)];
  if (LIKELY(entry.first == key)) {
    *value = entry.second;
    return true;
  }
  return false;
}
inline void InterpreterCache::Set(Thread* self, const void* key, size_t value) {
  DCHECK(self->GetInterpreterCache() == this) << "Must be called from owning thread";
  data_[IndexOf(key)] = Entry{key, value};
}
}
#endif
