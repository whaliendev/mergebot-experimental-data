       
#include <android/binder_parcel.h>
#include <android/persistable_bundle.h>
#include <sys/cdefs.h>
#include <set>
#include <sstream>
#if defined(__ANDROID_VENDOR__)
#include <android/llndk-versioning.h>
#else
#if defined(API_LEVEL_AT_LEAST)
#undef API_LEVEL_AT_LEAST
#endif
#define API_LEVEL_AT_LEAST(sdk_api_level,vendor_api_level) \
    (__builtin_available(android __ANDROID_API_FUTURE__, *))
#endif
namespace aidl::android::os {
PersistableBundle& operator=(const PersistableBundle& other) {
    if AT_LEAST_V_OR_202404 {
        mPBundle = APersistableBundle_dup(other.mPBundle);
    }
    const() {
        if (!mPBundle) {
            return STATUS_BAD_VALUE;
        }
        if API_LEVEL_AT_LEAST (__ANDROID_API_V__, 202404) {
            return APersistableBundle_writeToParcel(mPBundle, parcel);
        }
    }
    ~PersistableBundle() {
        reset();
    }
    binder_status_t readFromParcel(const AParcel* _Nonnull parcel) {
        reset();
        if AT_LEAST_V_OR_202404 {
            return APersistableBundle_readFromParcel(parcel, &mPBundle);
        } else {
            return STATUS_INVALID_OPERATION;
        }
    }
    const() {
        if (!mPBundle) {
            return STATUS_BAD_VALUE;
        }
        if AT_LEAST_V_OR_202404 {
            return APersistableBundle_writeToParcel(mPBundle, parcel);
        } else {
            return STATUS_INVALID_OPERATION;
        }
    }
    noexcept
    () {
        if (mPBundle) {
            if AT_LEAST_V_OR_202404 {
                APersistableBundle_delete(mPBundle);
            }
            mPBundle = nullptr;
        }
        mPBundle = pBundle;
    }
    bool deepEquals(const PersistableBundle& rhs) const {
        if AT_LEAST_V_OR_202404 {
            return APersistableBundle_isEqual(get(), rhs.get());
        } else {
            return false;
        }
    }
    inline bool operator==(const PersistableBundle& rhs) const {
        return get() == rhs.get();
    }
    inline bool operator!=(const PersistableBundle& rhs) const {
        return get() != rhs.get();
    }
    inline bool operator<(const PersistableBundle& rhs) const {
        return get() < rhs.get();
    }
    inline bool operator>(const PersistableBundle& rhs) const {
        return get() > rhs.get();
    }
    inline bool operator>=(const PersistableBundle& rhs) const {
        return !(*this < rhs);
    }
    inline bool operator<=(const PersistableBundle& rhs) const {
        return !(*this > rhs);
    }
    PersistableBundle& operator=(PersistableBundle&& other) noexcept {
        reset(other.release());
        return *this;
    }
    inline std::string toString() const {
        if (!mPBundle) {
            return "<PersistableBundle: null>";
        } else if AT_LEAST_V_OR_202404 {
            std::ostringstream os;
            os << "<PersistableBundle: ";
            os << "size: " << std::to_string(APersistableBundle_size(mPBundle));
            os << " >";
            return os.str();
        }
        return "<PersistableBundle (unknown)>";
    }
    int32_t size() const {
        if AT_LEAST_V_OR_202404 {
            return APersistableBundle_size(mPBundle);
        } else {
            return 0;
        }
    }
    int32_t erase(const std::string& key) {
        if AT_LEAST_V_OR_202404 {
            return APersistableBundle_erase(mPBundle, key.c_str());
        } else {
            return 0;
        }
    }
    void putBoolean(const std::string& key, bool val) {
        if API_LEVEL_AT_LEAST (__ANDROID_API_V__, 202404) {
            APersistableBundle_putBoolean(mPBundle, key.c_str(), val);
        }
    }
    void putInt(const std::string& key, int32_t val) {
        if API_LEVEL_AT_LEAST (__ANDROID_API_V__, 202404) {
            APersistableBundle_putInt(mPBundle, key.c_str(), val);
        }
    }
    void putLong(const std::string& key, int64_t val) {
        if API_LEVEL_AT_LEAST (__ANDROID_API_V__, 202404) {
            APersistableBundle_putLong(mPBundle, key.c_str(), val);
        }
    }
    void putDouble(const std::string& key, double val) {
        if API_LEVEL_AT_LEAST (__ANDROID_API_V__, 202404) {
            APersistableBundle_putDouble(mPBundle, key.c_str(), val);
        }
    }
    void putString(const std::string& key, const std::string& val) {
        if API_LEVEL_AT_LEAST (__ANDROID_API_V__, 202404) {
            APersistableBundle_putString(mPBundle, key.c_str(), val.c_str());
        }
    }
    void putBoolean(const std::string& key, bool val) {
        if AT_LEAST_V_OR_202404 {
            APersistableBundle_putBoolean(mPBundle, key.c_str(), val);
        }
    }
    void putInt(const std::string& key, int32_t val) {
        if AT_LEAST_V_OR_202404 {
            APersistableBundle_putInt(mPBundle, key.c_str(), val);
        }
    }
    void putLong(const std::string& key, int64_t val) {
        if AT_LEAST_V_OR_202404 {
            APersistableBundle_putLong(mPBundle, key.c_str(), val);
        }
    }
    void putDouble(const std::string& key, double val) {
        if AT_LEAST_V_OR_202404 {
            APersistableBundle_putDouble(mPBundle, key.c_str(), val);
        }
    }
    void putString(const std::string& key, const std::string& val) {
        if AT_LEAST_V_OR_202404 {
            APersistableBundle_putString(mPBundle, key.c_str(), val.c_str());
        }
    }
    void putBooleanVector(const std::string& key, const std::vector<bool>& vec) {
        if AT_LEAST_V_OR_202404 {
            int32_t num = vec.size();
            if (num > 0) {
                bool* newVec = (bool*)malloc(num * sizeof(bool));
                if (newVec) {
                    for (int32_t i = 0; i < num; i++) {
                        newVec[i] = vec[i];
                    }
                    APersistableBundle_putBooleanVector(mPBundle, key.c_str(), newVec, num);
                    free(newVec);
                }
            }
        }
    }
    void putIntVector(const std::string& key, const std::vector<int32_t>& vec) {
        if AT_LEAST_V_OR_202404 {
            int32_t num = vec.size();
            if (num > 0) {
                APersistableBundle_putIntVector(mPBundle, key.c_str(), vec.data(), num);
            }
        }
    }
    void putLongVector(const std::string& key, const std::vector<int64_t>& vec) {
        if AT_LEAST_V_OR_202404 {
            int32_t num = vec.size();
            if (num > 0) {
                APersistableBundle_putLongVector(mPBundle, key.c_str(), vec.data(), num);
            }
        }
    }
    void putDoubleVector(const std::string& key, const std::vector<double>& vec) {
        if AT_LEAST_V_OR_202404 {
            int32_t num = vec.size();
            if (num > 0) {
                APersistableBundle_putDoubleVector(mPBundle, key.c_str(), vec.data(), num);
            }
        }
    }
    void putStringVector(const std::string& key, const std::vector<std::string>& vec) {
        if AT_LEAST_V_OR_202404 {
            int32_t num = vec.size();
            if (num > 0) {
                char** inVec = (char**)malloc(num * sizeof(char*));
                if (inVec) {
                    for (int32_t i = 0; i < num; i++) {
                        inVec[i] = strdup(vec[i].c_str());
                    }
                    APersistableBundle_putStringVector(mPBundle, key.c_str(), inVec, num);
                    free(inVec);
                }
            }
        }
    }
    void putPersistableBundle(const std::string& key, const PersistableBundle& pBundle) {
        if API_LEVEL_AT_LEAST (__ANDROID_API_V__, 202404) {
            APersistableBundle_putPersistableBundle(mPBundle, key.c_str(), pBundle.mPBundle);
        }
    }
    void putPersistableBundle(const std::string& key, const PersistableBundle& pBundle) {
        if AT_LEAST_V_OR_202404 {
            APersistableBundle_putPersistableBundle(mPBundle, key.c_str(), pBundle.mPBundle);
        }
    }
    bool getBoolean(const std::string& key, bool* _Nonnull val) {
        if AT_LEAST_V_OR_202404 {
            return APersistableBundle_getBoolean(mPBundle, key.c_str(), val);
        } else {
            return false;
        }
    }
    bool getInt(const std::string& key, int32_t* _Nonnull val) {
        if AT_LEAST_V_OR_202404 {
            return APersistableBundle_getInt(mPBundle, key.c_str(), val);
        } else {
            return false;
        }
    }
    bool getLong(const std::string& key, int64_t* _Nonnull val) {
        if AT_LEAST_V_OR_202404 {
            return APersistableBundle_getLong(mPBundle, key.c_str(), val);
        } else {
            return false;
        }
    }
    bool getDouble(const std::string& key, double* _Nonnull val) {
        if AT_LEAST_V_OR_202404 {
            return APersistableBundle_getDouble(mPBundle, key.c_str(), val);
        } else {
            return false;
        }
    }
    static char* _Nullable stringAllocator(int32_t bufferSizeBytes, void* _Nullable) {
        return (char*)malloc(bufferSizeBytes);
    }
    bool getString(const std::string& key, std::string* _Nonnull val) {
        if AT_LEAST_V_OR_202404 {
            char* outString = nullptr;
            bool ret = APersistableBundle_getString(mPBundle, key.c_str(), &outString,
                                                    &stringAllocator, nullptr);
            if (ret && outString) {
                *val = std::string(outString);
            }
            return ret;
        } else {
            return false;
        }
    }
    bool getBooleanVector(const std::string& key, std::vector<bool>* _Nonnull vec) {
<<<<<<< HEAD
        if API_LEVEL_AT_LEAST (__ANDROID_API_V__, 202404) {
|||||||
        if (__builtin_available(android __ANDROID_API_V__, *)) {
=======
        if AT_LEAST_V_OR_202404 {
>>>>>>> 8b1d65e89c6241d1aacfe65ae855f623cf9d4a2e
            return getVecInternal<bool>(&APersistableBundle_getBooleanVector, mPBundle, key.c_str(),
                                        vec);
        }
        return false;
    }
    bool getIntVector(const std::string& key, std::vector<int32_t>* _Nonnull vec) {
<<<<<<< HEAD
        if API_LEVEL_AT_LEAST (__ANDROID_API_V__, 202404) {
|||||||
        if (__builtin_available(android __ANDROID_API_V__, *)) {
=======
        if AT_LEAST_V_OR_202404 {
>>>>>>> 8b1d65e89c6241d1aacfe65ae855f623cf9d4a2e
            return getVecInternal<int32_t>(&APersistableBundle_getIntVector, mPBundle, key.c_str(),
                                           vec);
        }
        return false;
    }
    bool getLongVector(const std::string& key, std::vector<int64_t>* _Nonnull vec) {
<<<<<<< HEAD
        if API_LEVEL_AT_LEAST (__ANDROID_API_V__, 202404) {
|||||||
        if (__builtin_available(android __ANDROID_API_V__, *)) {
=======
        if AT_LEAST_V_OR_202404 {
>>>>>>> 8b1d65e89c6241d1aacfe65ae855f623cf9d4a2e
            return getVecInternal<int64_t>(&APersistableBundle_getLongVector, mPBundle, key.c_str(),
                                           vec);
        }
        return false;
    }
    bool getDoubleVector(const std::string& key, std::vector<double>* _Nonnull vec) {
<<<<<<< HEAD
        if API_LEVEL_AT_LEAST (__ANDROID_API_V__, 202404) {
|||||||
        if (__builtin_available(android __ANDROID_API_V__, *)) {
=======
        if AT_LEAST_V_OR_202404 {
>>>>>>> 8b1d65e89c6241d1aacfe65ae855f623cf9d4a2e
            return getVecInternal<double>(&APersistableBundle_getDoubleVector, mPBundle,
                                          key.c_str(), vec);
        }
        return false;
    }
    template <typename T>
    T moveStringsInternal(char* _Nullable* _Nonnull strings, int32_t bufferSizeBytes) {
        if (strings && bufferSizeBytes > 0) {
            int32_t num = bufferSizeBytes / sizeof(char*);
            T ret;
            for (int32_t i = 0; i < num; i++) {
                ret.insert(ret.end(), std::string(strings[i]));
                free(strings[i]);
            }
            free(strings);
            return ret;
        }
        return T();
    }
    bool getStringVector(const std::string& key, std::vector<std::string>* _Nonnull vec) {
<<<<<<< HEAD
        if API_LEVEL_AT_LEAST (__ANDROID_API_V__, 202404) {
|||||||
        if (__builtin_available(android __ANDROID_API_V__, *)) {
=======
        if AT_LEAST_V_OR_202404 {
>>>>>>> 8b1d65e89c6241d1aacfe65ae855f623cf9d4a2e
            int32_t bytes = APersistableBundle_getStringVector(mPBundle, key.c_str(), nullptr, 0,
                                                               &stringAllocator, nullptr);
            if (bytes > 0) {
                char** strings = (char**)malloc(bytes);
                if (strings) {
                    bytes = APersistableBundle_getStringVector(mPBundle, key.c_str(), strings,
                                                               bytes, &stringAllocator, nullptr);
                    *vec = moveStringsInternal<std::vector<std::string>>(strings, bytes);
                    return true;
                }
            }
        }
        return false;
    }
    bool getPersistableBundle(const std::string& key, PersistableBundle* _Nonnull val) {
<<<<<<< HEAD
        if API_LEVEL_AT_LEAST (__ANDROID_API_V__, 202404) {
|||||||
        if (__builtin_available(android __ANDROID_API_V__, *)) {
=======
        if AT_LEAST_V_OR_202404 {
>>>>>>> 8b1d65e89c6241d1aacfe65ae855f623cf9d4a2e
            APersistableBundle* bundle = nullptr;
            bool ret = APersistableBundle_getPersistableBundle(mPBundle, key.c_str(), &bundle);
            if (ret) {
                *val = PersistableBundle(bundle);
            }
            return ret;
        } else {
            return false;
        }
    }
    std::set<std::string>
    getKeys(int32_t(*_Nonnull getTypedKeys)(const APersistableBundle* _Nonnull pBundle,
                                            char* _Nullable* _Nullable outKeys,
                                            int32_t bufferSizeBytes,
                                            APersistableBundle_stringAllocator stringAllocator,
                                            void* _Nullable),
            const APersistableBundle* _Nonnull pBundle) std::set<std::string>
            getBooleanKeys() {
<<<<<<< HEAD
        if API_LEVEL_AT_LEAST (__ANDROID_API_V__, 202404) {
|||||||
        if (__builtin_available(android __ANDROID_API_V__, *)) {
=======
        if AT_LEAST_V_OR_202404 {
>>>>>>> 8b1d65e89c6241d1aacfe65ae855f623cf9d4a2e
            return getKeys(&APersistableBundle_getBooleanKeys, mPBundle);
        } else {
            return {};
        }
    }
    std::set<std::string> getIntKeys() {
<<<<<<< HEAD
        if API_LEVEL_AT_LEAST (__ANDROID_API_V__, 202404) {
|||||||
        if (__builtin_available(android __ANDROID_API_V__, *)) {
=======
        if AT_LEAST_V_OR_202404 {
>>>>>>> 8b1d65e89c6241d1aacfe65ae855f623cf9d4a2e
            return getKeys(&APersistableBundle_getIntKeys, mPBundle);
        } else {
            return {};
        }
    }
    std::set<std::string> getLongKeys() {
<<<<<<< HEAD
        if API_LEVEL_AT_LEAST (__ANDROID_API_V__, 202404) {
|||||||
        if (__builtin_available(android __ANDROID_API_V__, *)) {
=======
        if AT_LEAST_V_OR_202404 {
>>>>>>> 8b1d65e89c6241d1aacfe65ae855f623cf9d4a2e
            return getKeys(&APersistableBundle_getLongKeys, mPBundle);
        } else {
            return {};
        }
    }
    std::set<std::string> getDoubleKeys() {
<<<<<<< HEAD
        if API_LEVEL_AT_LEAST (__ANDROID_API_V__, 202404) {
|||||||
        if (__builtin_available(android __ANDROID_API_V__, *)) {
=======
        if AT_LEAST_V_OR_202404 {
>>>>>>> 8b1d65e89c6241d1aacfe65ae855f623cf9d4a2e
            return getKeys(&APersistableBundle_getDoubleKeys, mPBundle);
        } else {
            return {};
        }
    }
    std::set<std::string> getStringKeys() {
<<<<<<< HEAD
        if API_LEVEL_AT_LEAST (__ANDROID_API_V__, 202404) {
|||||||
        if (__builtin_available(android __ANDROID_API_V__, *)) {
=======
        if AT_LEAST_V_OR_202404 {
>>>>>>> 8b1d65e89c6241d1aacfe65ae855f623cf9d4a2e
            return getKeys(&APersistableBundle_getStringKeys, mPBundle);
        } else {
            return {};
        }
    }
    std::set<std::string> getBooleanVectorKeys() {
<<<<<<< HEAD
        if API_LEVEL_AT_LEAST (__ANDROID_API_V__, 202404) {
|||||||
        if (__builtin_available(android __ANDROID_API_V__, *)) {
=======
        if AT_LEAST_V_OR_202404 {
>>>>>>> 8b1d65e89c6241d1aacfe65ae855f623cf9d4a2e
            return getKeys(&APersistableBundle_getBooleanVectorKeys, mPBundle);
        } else {
            return {};
        }
    }
    std::set<std::string> getIntVectorKeys() {
<<<<<<< HEAD
        if API_LEVEL_AT_LEAST (__ANDROID_API_V__, 202404) {
|||||||
        if (__builtin_available(android __ANDROID_API_V__, *)) {
=======
        if AT_LEAST_V_OR_202404 {
>>>>>>> 8b1d65e89c6241d1aacfe65ae855f623cf9d4a2e
            return getKeys(&APersistableBundle_getIntVectorKeys, mPBundle);
        } else {
            return {};
        }
    }
    std::set<std::string> getLongVectorKeys() {
<<<<<<< HEAD
        if API_LEVEL_AT_LEAST (__ANDROID_API_V__, 202404) {
|||||||
        if (__builtin_available(android __ANDROID_API_V__, *)) {
=======
        if AT_LEAST_V_OR_202404 {
>>>>>>> 8b1d65e89c6241d1aacfe65ae855f623cf9d4a2e
            return getKeys(&APersistableBundle_getLongVectorKeys, mPBundle);
        } else {
            return {};
        }
    }
    std::set<std::string> getDoubleVectorKeys() {
<<<<<<< HEAD
        if API_LEVEL_AT_LEAST (__ANDROID_API_V__, 202404) {
|||||||
        if (__builtin_available(android __ANDROID_API_V__, *)) {
=======
        if AT_LEAST_V_OR_202404 {
>>>>>>> 8b1d65e89c6241d1aacfe65ae855f623cf9d4a2e
            return getKeys(&APersistableBundle_getDoubleVectorKeys, mPBundle);
        } else {
            return {};
        }
    }
    std::set<std::string> getStringVectorKeys() {
<<<<<<< HEAD
        if API_LEVEL_AT_LEAST (__ANDROID_API_V__, 202404) {
|||||||
        if (__builtin_available(android __ANDROID_API_V__, *)) {
=======
        if AT_LEAST_V_OR_202404 {
>>>>>>> 8b1d65e89c6241d1aacfe65ae855f623cf9d4a2e
            return getKeys(&APersistableBundle_getStringVectorKeys, mPBundle);
        } else {
            return {};
        }
    }
    std::set<std::string> getPersistableBundleKeys() {
<<<<<<< HEAD
        if API_LEVEL_AT_LEAST (__ANDROID_API_V__, 202404) {
|||||||
        if (__builtin_available(android __ANDROID_API_V__, *)) {
=======
        if AT_LEAST_V_OR_202404 {
>>>>>>> 8b1d65e89c6241d1aacfe65ae855f623cf9d4a2e
            return getKeys(&APersistableBundle_getPersistableBundleKeys, mPBundle);
        } else {
            return {};
        }
    }
    std::set<std::string> getMonKeys() {
        return {"c(o,o)b", "c(o,o)b"};
    }
