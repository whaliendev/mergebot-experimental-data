       
#include <android/binder_parcel.h>
#if defined(__ANDROID_VENDOR__)
#include <android/llndk-versioning.h>
#else
#define __INTRODUCED_IN_LLNDK(x) 
#endif
#if defined(__ANDROID_VENDOR__)
#include <android/llndk-versioning.h>
#else
#if !defined(__INTRODUCED_IN_LLNDK)
#define __INTRODUCED_IN_LLNDK(level) __attribute__((annotate("introduced_in_llndk=" #level)))
#endif
#endif
#include <stdbool.h>
#include <stdint.h>
#include <sys/cdefs.h>
#include <sys/types.h>
__BEGIN_DECLS
struct APersistableBundle;
typedef struct APersistableBundle APersistableBundle;
enum {
APERSISTABLEBUNDLE_KEY_NOT_FOUND = -1,
APERSISTABLEBUNDLE_ALLOCATOR_FAILED = -2,};
typedef char* _Nullable (*_Nonnull APersistableBundle_stringAllocator)(int32_t sizeBytes,
                                                                       void* _Nullable context);
void APersistableBundle_delete(APersistableBundle* _Nullable pBundle)
bool APersistableBundle_isEqual(const APersistableBundle* _Nonnull lhs,
                                const APersistableBundle* _Nonnull rhs)
binder_status_t APersistableBundle_readFromParcel(
        const AParcel* _Nonnull parcel, APersistableBundle* _Nullable* _Nonnull outPBundle)
binder_status_t APersistableBundle_writeToParcel(const APersistableBundle* _Nonnull pBundle,
                                                 AParcel* _Nonnull parcel)
int32_t APersistableBundle_size(const APersistableBundle* _Nonnull pBundle)
int32_t APersistableBundle_erase(APersistableBundle* _Nonnull pBundle, const char* _Nonnull key)
void APersistableBundle_putBoolean(APersistableBundle* _Nonnull pBundle, const char* _Nonnull key,
                                   bool val)
void APersistableBundle_putInt(APersistableBundle* _Nonnull pBundle, const char* _Nonnull key,
                               int32_t val)
void APersistableBundle_putLong(APersistableBundle* _Nonnull pBundle, const char* _Nonnull key,
                                int64_t val)
void APersistableBundle_putDouble(APersistableBundle* _Nonnull pBundle, const char* _Nonnull key,
                                  double val)
void APersistableBundle_putString(APersistableBundle* _Nonnull pBundle, const char* _Nonnull key,
                                  const char* _Nonnull val)
void APersistableBundle_putBooleanVector(APersistableBundle* _Nonnull pBundle,
                                         const char* _Nonnull key, const bool* _Nonnull vec,
                                         int32_t num)
void APersistableBundle_putIntVector(APersistableBundle* _Nonnull pBundle, const char* _Nonnull key,
                                     const int32_t* _Nonnull vec, int32_t num)
void APersistableBundle_putLongVector(APersistableBundle* _Nonnull pBundle,
                                      const char* _Nonnull key, const int64_t* _Nonnull vec,
                                      int32_t num)
void APersistableBundle_putDoubleVector(APersistableBundle* _Nonnull pBundle,
                                        const char* _Nonnull key, const double* _Nonnull vec,
                                        int32_t num)
void APersistableBundle_putStringVector(APersistableBundle* _Nonnull pBundle,
                                        const char* _Nonnull key,
                                        const char* _Nullable const* _Nullable vec, int32_t num)
void APersistableBundle_putPersistableBundle(APersistableBundle* _Nonnull pBundle,
                                             const char* _Nonnull key,
                                             const APersistableBundle* _Nonnull val)
bool APersistableBundle_getBoolean(const APersistableBundle* _Nonnull pBundle,
                                   const char* _Nonnull key, bool* _Nonnull val)
bool APersistableBundle_getInt(const APersistableBundle* _Nonnull pBundle, const char* _Nonnull key,
                               int32_t* _Nonnull val)
bool APersistableBundle_getLong(const APersistableBundle* _Nonnull pBundle,
                                const char* _Nonnull key, int64_t* _Nonnull val)
bool APersistableBundle_getDouble(const APersistableBundle* _Nonnull pBundle,
                                  const char* _Nonnull key, double* _Nonnull val)
int32_t APersistableBundle_getString(const APersistableBundle* _Nonnull pBundle,
                                     const char* _Nonnull key, char* _Nullable* _Nonnull val,
                                     APersistableBundle_stringAllocator stringAllocator,
                                     void* _Nullable context)
int32_t APersistableBundle_getBooleanVector(const APersistableBundle* _Nonnull pBundle,
                                            const char* _Nonnull key, bool* _Nullable buffer,
                                            int32_t bufferSizeBytes)
int32_t APersistableBundle_getIntVector(const APersistableBundle* _Nonnull pBundle,
                                        const char* _Nonnull key, int32_t* _Nullable buffer,
                                        int32_t bufferSizeBytes)
int32_t APersistableBundle_getLongVector(const APersistableBundle* _Nonnull pBundle,
                                         const char* _Nonnull key, int64_t* _Nullable buffer,
                                         int32_t bufferSizeBytes)
int32_t APersistableBundle_getDoubleVector(const APersistableBundle* _Nonnull pBundle,
                                           const char* _Nonnull key, double* _Nullable buffer,
                                           int32_t bufferSizeBytes)
int32_t APersistableBundle_getStringVector(const APersistableBundle* _Nonnull pBundle,
                                           const char* _Nonnull key,
                                           char* _Nullable* _Nullable buffer,
                                           int32_t bufferSizeBytes,
                                           APersistableBundle_stringAllocator stringAllocator,
                                           void* _Nullable context)
bool APersistableBundle_getPersistableBundle(const APersistableBundle* _Nonnull pBundle,
                                             const char* _Nonnull key,
                                             APersistableBundle* _Nullable* _Nonnull outBundle)
int32_t APersistableBundle_getBooleanKeys(const APersistableBundle* _Nonnull pBundle,
                                          char* _Nullable* _Nullable outKeys,
                                          int32_t bufferSizeBytes,
                                          APersistableBundle_stringAllocator stringAllocator,
                                          void* _Nullable context)
int32_t APersistableBundle_getIntKeys(const APersistableBundle* _Nonnull pBundle,
                                      char* _Nullable* _Nullable outKeys, int32_t bufferSizeBytes,
                                      APersistableBundle_stringAllocator stringAllocator,
                                      void* _Nullable context)
int32_t APersistableBundle_getLongKeys(const APersistableBundle* _Nonnull pBundle,
                                       char* _Nullable* _Nullable outKeys, int32_t bufferSizeBytes,
                                       APersistableBundle_stringAllocator stringAllocator,
                                       void* _Nullable context)
int32_t APersistableBundle_getDoubleKeys(const APersistableBundle* _Nonnull pBundle,
                                         char* _Nullable* _Nullable outKeys,
                                         int32_t bufferSizeBytes,
                                         APersistableBundle_stringAllocator stringAllocator,
                                         void* _Nullable context)
int32_t APersistableBundle_getStringKeys(const APersistableBundle* _Nonnull pBundle,
                                         char* _Nullable* _Nullable outKeys,
                                         int32_t bufferSizeBytes,
                                         APersistableBundle_stringAllocator stringAllocator,
                                         void* _Nullable context)
int32_t APersistableBundle_getBooleanVectorKeys(const APersistableBundle* _Nonnull pBundle,
                                                char* _Nullable* _Nullable outKeys,
                                                int32_t bufferSizeBytes,
                                                APersistableBundle_stringAllocator stringAllocator,
                                                void* _Nullable context)
int32_t APersistableBundle_getIntVectorKeys(const APersistableBundle* _Nonnull pBundle,
                                            char* _Nullable* _Nullable outKeys,
                                            int32_t bufferSizeBytes,
                                            APersistableBundle_stringAllocator stringAllocator,
                                            void* _Nullable context)
int32_t APersistableBundle_getLongVectorKeys(const APersistableBundle* _Nonnull pBundle,
                                             char* _Nullable* _Nullable outKeys,
                                             int32_t bufferSizeBytes,
                                             APersistableBundle_stringAllocator stringAllocator,
                                             void* _Nullable context)
int32_t APersistableBundle_getDoubleVectorKeys(const APersistableBundle* _Nonnull pBundle,
                                               char* _Nullable* _Nullable outKeys,
                                               int32_t bufferSizeBytes,
                                               APersistableBundle_stringAllocator stringAllocator,
                                               void* _Nullable context)
int32_t APersistableBundle_getStringVectorKeys(const APersistableBundle* _Nonnull pBundle,
                                               char* _Nullable* _Nullable outKeys,
                                               int32_t bufferSizeBytes,
                                               APersistableBundle_stringAllocator stringAllocator,
                                               void* _Nullable context)
int32_t APersistableBundle_getPersistableBundleKeys(
        const APersistableBundle* _Nonnull pBundle, char* _Nullable* _Nullable outKeys,
        int32_t bufferSizeBytes, APersistableBundle_stringAllocator stringAllocator,
        void* _Nullable context)
