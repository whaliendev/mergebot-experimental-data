#ifndef ART_RUNTIME_INTERPRETER_INTERPRETER_CACHE_INL_H_
#define ART_RUNTIME_INTERPRETER_INTERPRETER_CACHE_INL_H_ 
#include "interpreter_cache.h"
#include "thread.h"
namespace art {
inline void InterpreterCache::Set(Thread* self, const void* key, size_t value) {
  DCHECK(self->GetInterpreterCache() == this) << "Must be called from owning thread";
<<<<<<< HEAD
||||||| a991d42711
<<<<<<< HEAD
||||||| a991d42711
  if (gUseReadBarrier && self->GetWeakRefAccessEnabled()) {
=======
  if (!gUseReadBarrier || self->GetWeakRefAccessEnabled()) {
>>>>>>> 7758eaad
=======
  if (!gUseReadBarrier || self->GetWeakRefAccessEnabled()) {
>>>>>>> 7758eaad
  data_[IndexOf(key)] = Entry{key, value};
}}
inline void InterpreterCache::Set(Thread* self, const void* key, size_t value) {
  DCHECK(self->GetInterpreterCache() == this) << "Must be called from owning thread";
<<<<<<< HEAD
||||||| a991d42711
<<<<<<< HEAD
||||||| a991d42711
  if (gUseReadBarrier && self->GetWeakRefAccessEnabled()) {
=======
  if (!gUseReadBarrier || self->GetWeakRefAccessEnabled()) {
>>>>>>> 7758eaad
=======
  if (!gUseReadBarrier || self->GetWeakRefAccessEnabled()) {
>>>>>>> 7758eaad
  data_[IndexOf(key)] = Entry{key, value};
}}
}
#endif
