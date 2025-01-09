       
#include <android/binder_parcel.h>
<<<<<<< HEAD
#if defined(__ANDROID_VENDOR__)
#include <android/llndk-versioning.h>
#else
#if !defined(__INTRODUCED_IN_LLNDK)
#define __INTRODUCED_IN_LLNDK(level) __attribute__((annotate("introduced_in_llndk=" #level)))
#endif
#endif
||||||| 8bf4487ae5
=======
#if defined(__ANDROID_VENDOR__)
#include <android/llndk-versioning.h>
#else
#define __INTRODUCED_IN_LLNDK(x) 
#endif
>>>>>>> 8b1d65e8
#include <stdbool.h>
#include <stdint.h>
#include <sys/cdefs.h>
#include <sys/types.h>
__BEGIN_DECLS
struct APersistableBundle;
typedef struct APersistableBundle APersistableBundle;
enum {
    APERSISTABLEBUNDLE_KEY_NOT_FOUND = -1,
    APERSISTABLEBUNDLE_ALLOCATOR_FAILED = -2,
};
typedef char* _Nullable (*_Nonnull APersistableBundle_stringAllocator)(int32_t sizeBytes,
                                                                       void* _Nullable context);
APersistableBundle* _Nullable APersistableBundle_new() __INTRODUCED_IN(__ANDROID_API_V__)
        __INTRODUCED_IN_LLNDK(202404);
APersistableBundle* _Nullable APersistableBundle_dup(const APersistableBundle* _Nonnull pBundle)
        __INTRODUCED_IN(__ANDROID_API_V__) __INTRODUCED_IN_LLNDK(202404);
void APersistableBundle_delete(APersistableBundle* _Nullable pBundle)
        __INTRODUCED_IN(__ANDROID_API_V__) __INTRODUCED_IN_LLNDK(202404);
bool APersistableBundle_isEqual(const APersistableBundle* _Nonnull lhs,
                                const APersistableBundle* _Nonnull rhs)
        __INTRODUCED_IN(__ANDROID_API_V__) __INTRODUCED_IN_LLNDK(202404);
binder_status_t APersistableBundle_readFromParcel(
        const AParcel* _Nonnull parcel, APersistableBundle* _Nullable* _Nonnull outPBundle)
        __INTRODUCED_IN(__ANDROID_API_V__) __INTRODUCED_IN_LLNDK(202404);
binder_status_t APersistableBundle_writeToParcel(const APersistableBundle* _Nonnull pBundle,
                                                 AParcel* _Nonnull parcel)
        __INTRODUCED_IN(__ANDROID_API_V__) __INTRODUCED_IN_LLNDK(202404);
int32_t APersistableBundle_size(const APersistableBundle* _Nonnull pBundle)
        __INTRODUCED_IN(__ANDROID_API_V__) __INTRODUCED_IN_LLNDK(202404);
int32_t APersistableBundle_erase(APersistableBundle* _Nonnull pBundle, const char* _Nonnull key)
        __INTRODUCED_IN(__ANDROID_API_V__) __INTRODUCED_IN_LLNDK(202404);
void APersistableBundle_putBoolean(APersistableBundle* _Nonnull pBundle, const char* _Nonnull key,
                                   bool val) __INTRODUCED_IN(__ANDROID_API_V__)
        __INTRODUCED_IN_LLNDK(202404);
void APersistableBundle_putInt(APersistableBundle* _Nonnull pBundle, const char* _Nonnull key,
                               int32_t val) __INTRODUCED_IN(__ANDROID_API_V__)
        __INTRODUCED_IN_LLNDK(202404);
void APersistableBundle_putLong(APersistableBundle* _Nonnull pBundle, const char* _Nonnull key,
                                int64_t val) __INTRODUCED_IN(__ANDROID_API_V__)
        __INTRODUCED_IN_LLNDK(202404);
void APersistableBundle_putDouble(APersistableBundle* _Nonnull pBundle, const char* _Nonnull key,
                                  double val) __INTRODUCED_IN(__ANDROID_API_V__)
        __INTRODUCED_IN_LLNDK(202404);
void APersistableBundle_putString(APersistableBundle* _Nonnull pBundle, const char* _Nonnull key,
                                  const char* _Nonnull val) __INTRODUCED_IN(__ANDROID_API_V__)
        __INTRODUCED_IN_LLNDK(202404);
void APersistableBundle_putBooleanVector(APersistableBundle* _Nonnull pBundle,
                                         const char* _Nonnull key, const bool* _Nonnull vec,
                                         int32_t num) __INTRODUCED_IN(__ANDROID_API_V__)
        __INTRODUCED_IN_LLNDK(202404);
void APersistableBundle_putIntVector(APersistableBundle* _Nonnull pBundle, const char* _Nonnull key,
                                     const int32_t* _Nonnull vec, int32_t num)
        __INTRODUCED_IN(__ANDROID_API_V__) __INTRODUCED_IN_LLNDK(202404);
void APersistableBundle_putLongVector(APersistableBundle* _Nonnull pBundle,
                                      const char* _Nonnull key, const int64_t* _Nonnull vec,
                                      int32_t num) __INTRODUCED_IN(__ANDROID_API_V__)
        __INTRODUCED_IN_LLNDK(202404);
void APersistableBundle_putDoubleVector(APersistableBundle* _Nonnull pBundle,
                                        const char* _Nonnull key, const double* _Nonnull vec,
                                        int32_t num) __INTRODUCED_IN(__ANDROID_API_V__)
        __INTRODUCED_IN_LLNDK(202404);
void APersistableBundle_putStringVector(APersistableBundle* _Nonnull pBundle,
                                        const char* _Nonnull key,
                                        const char* _Nullable const* _Nullable vec, int32_t num)
        __INTRODUCED_IN(__ANDROID_API_V__) __INTRODUCED_IN_LLNDK(202404);
void APersistableBundle_putPersistableBundle(APersistableBundle* _Nonnull pBundle,
                                             const char* _Nonnull key,
                                             const APersistableBundle* _Nonnull val)
        __INTRODUCED_IN(__ANDROID_API_V__) __INTRODUCED_IN_LLNDK(202404);
bool APersistableBundle_getBoolean(const APersistableBundle* _Nonnull pBundle,
                                   const char* _Nonnull key, bool* _Nonnull val)
        __INTRODUCED_IN(__ANDROID_API_V__) __INTRODUCED_IN_LLNDK(202404);
bool APersistableBundle_getInt(const APersistableBundle* _Nonnull pBundle, const char* _Nonnull key,
                               int32_t* _Nonnull val) __INTRODUCED_IN(__ANDROID_API_V__)
        __INTRODUCED_IN_LLNDK(202404);
bool APersistableBundle_getLong(const APersistableBundle* _Nonnull pBundle,
                                const char* _Nonnull key, int64_t* _Nonnull val)
        __INTRODUCED_IN(__ANDROID_API_V__) __INTRODUCED_IN_LLNDK(202404);
bool APersistableBundle_getDouble(const APersistableBundle* _Nonnull pBundle,
                                  const char* _Nonnull key, double* _Nonnull val)
        __INTRODUCED_IN(__ANDROID_API_V__) __INTRODUCED_IN_LLNDK(202404);
int32_t APersistableBundle_getString(const APersistableBundle* _Nonnull pBundle,
                                     const char* _Nonnull key, char* _Nullable* _Nonnull val,
                                     APersistableBundle_stringAllocator stringAllocator,
                                     void* _Nullable context) __INTRODUCED_IN(__ANDROID_API_V__)
        __INTRODUCED_IN_LLNDK(202404);
int32_t APersistableBundle_getBooleanVector(const APersistableBundle* _Nonnull pBundle,
                                            const char* _Nonnull key, bool* _Nullable buffer,
                                            int32_t bufferSizeBytes)
        __INTRODUCED_IN(__ANDROID_API_V__) __INTRODUCED_IN_LLNDK(202404);
int32_t APersistableBundle_getIntVector(const APersistableBundle* _Nonnull pBundle,
                                        const char* _Nonnull key, int32_t* _Nullable buffer,
                                        int32_t bufferSizeBytes) __INTRODUCED_IN(__ANDROID_API_V__)
        __INTRODUCED_IN_LLNDK(202404);
int32_t APersistableBundle_getLongVector(const APersistableBundle* _Nonnull pBundle,
                                         const char* _Nonnull key, int64_t* _Nullable buffer,
                                         int32_t bufferSizeBytes) __INTRODUCED_IN(__ANDROID_API_V__)
        __INTRODUCED_IN_LLNDK(202404);
int32_t APersistableBundle_getDoubleVector(const APersistableBundle* _Nonnull pBundle,
                                           const char* _Nonnull key, double* _Nullable buffer,
                                           int32_t bufferSizeBytes)
        __INTRODUCED_IN(__ANDROID_API_V__) __INTRODUCED_IN_LLNDK(202404);
int32_t APersistableBundle_getStringVector(const APersistableBundle* _Nonnull pBundle,
                                           const char* _Nonnull key,
                                           char* _Nullable* _Nullable buffer,
                                           int32_t bufferSizeBytes,
                                           APersistableBundle_stringAllocator stringAllocator,
                                           void* _Nullable context)
        __INTRODUCED_IN(__ANDROID_API_V__) __INTRODUCED_IN_LLNDK(202404);
bool APersistableBundle_getPersistableBundle(const APersistableBundle* _Nonnull pBundle,
                                             const char* _Nonnull key,
                                             APersistableBundle* _Nullable* _Nonnull outBundle)
        __INTRODUCED_IN(__ANDROID_API_V__) __INTRODUCED_IN_LLNDK(202404);
int32_t APersistableBundle_getBooleanKeys(const APersistableBundle* _Nonnull pBundle,
                                          char* _Nullable* _Nullable outKeys,
                                          int32_t bufferSizeBytes,
                                          APersistableBundle_stringAllocator stringAllocator,
                                          void* _Nullable context)
        __INTRODUCED_IN(__ANDROID_API_V__) __INTRODUCED_IN_LLNDK(202404);
int32_t APersistableBundle_getIntKeys(const APersistableBundle* _Nonnull pBundle,
                                      char* _Nullable* _Nullable outKeys, int32_t bufferSizeBytes,
                                      APersistableBundle_stringAllocator stringAllocator,
                                      void* _Nullable context) __INTRODUCED_IN(__ANDROID_API_V__)
        __INTRODUCED_IN_LLNDK(202404);
int32_t APersistableBundle_getLongKeys(const APersistableBundle* _Nonnull pBundle,
                                       char* _Nullable* _Nullable outKeys, int32_t bufferSizeBytes,
                                       APersistableBundle_stringAllocator stringAllocator,
                                       void* _Nullable context) __INTRODUCED_IN(__ANDROID_API_V__)
        __INTRODUCED_IN_LLNDK(202404);
int32_t APersistableBundle_getDoubleKeys(const APersistableBundle* _Nonnull pBundle,
                                         char* _Nullable* _Nullable outKeys,
                                         int32_t bufferSizeBytes,
                                         APersistableBundle_stringAllocator stringAllocator,
                                         void* _Nullable context) __INTRODUCED_IN(__ANDROID_API_V__)
        __INTRODUCED_IN_LLNDK(202404);
int32_t APersistableBundle_getStringKeys(const APersistableBundle* _Nonnull pBundle,
                                         char* _Nullable* _Nullable outKeys,
                                         int32_t bufferSizeBytes,
                                         APersistableBundle_stringAllocator stringAllocator,
                                         void* _Nullable context) __INTRODUCED_IN(__ANDROID_API_V__)
        __INTRODUCED_IN_LLNDK(202404);
int32_t APersistableBundle_getBooleanVectorKeys(const APersistableBundle* _Nonnull pBundle,
                                                char* _Nullable* _Nullable outKeys,
                                                int32_t bufferSizeBytes,
                                                APersistableBundle_stringAllocator stringAllocator,
                                                void* _Nullable context)
        __INTRODUCED_IN(__ANDROID_API_V__) __INTRODUCED_IN_LLNDK(202404);
int32_t APersistableBundle_getIntVectorKeys(const APersistableBundle* _Nonnull pBundle,
                                            char* _Nullable* _Nullable outKeys,
                                            int32_t bufferSizeBytes,
                                            APersistableBundle_stringAllocator stringAllocator,
                                            void* _Nullable context)
        __INTRODUCED_IN(__ANDROID_API_V__) __INTRODUCED_IN_LLNDK(202404);
int32_t APersistableBundle_getLongVectorKeys(const APersistableBundle* _Nonnull pBundle,
                                             char* _Nullable* _Nullable outKeys,
                                             int32_t bufferSizeBytes,
                                             APersistableBundle_stringAllocator stringAllocator,
                                             void* _Nullable context)
        __INTRODUCED_IN(__ANDROID_API_V__) __INTRODUCED_IN_LLNDK(202404);
int32_t APersistableBundle_getDoubleVectorKeys(const APersistableBundle* _Nonnull pBundle,
                                               char* _Nullable* _Nullable outKeys,
                                               int32_t bufferSizeBytes,
                                               APersistableBundle_stringAllocator stringAllocator,
                                               void* _Nullable context)
        __INTRODUCED_IN(__ANDROID_API_V__) __INTRODUCED_IN_LLNDK(202404);
int32_t APersistableBundle_getStringVectorKeys(const APersistableBundle* _Nonnull pBundle,
                                               char* _Nullable* _Nullable outKeys,
                                               int32_t bufferSizeBytes,
                                               APersistableBundle_stringAllocator stringAllocator,
                                               void* _Nullable context)
        __INTRODUCED_IN(__ANDROID_API_V__) __INTRODUCED_IN_LLNDK(202404);
int32_t APersistableBundle_getPersistableBundleKeys(
        const APersistableBundle* _Nonnull pBundle, char* _Nullable* _Nullable outKeys,
        int32_t bufferSizeBytes, APersistableBundle_stringAllocator stringAllocator,
        void* _Nullable context) __INTRODUCED_IN(__ANDROID_API_V__) __INTRODUCED_IN_LLNDK(202404);
__END_DECLS
