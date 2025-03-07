#define LOG_TAG "nativebridge"
#include "nativebridge/native_bridge.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <log/log.h>
namespace android {
struct NativeBridgeRuntimeValues {
    const char* os_arch;
    const char* cpu_abi;
    const char* cpu_abi2;
    const char* *supported_abis;
    int32_t abi_count;
};
static constexpr const char* kNativeBridgeInterfaceSymbol = "NativeBridgeItf";
enum class NativeBridgeState {
  kNotSetup,
  kOpened,
  kPreInitialized,
  kInitialized,
  kClosed
};
static constexpr const char* kNotSetupString = "kNotSetup";
static constexpr const char* kOpenedString = "kOpened";
static constexpr const char* kPreInitializedString = "kPreInitialized";
static constexpr const char* kInitializedString = "kInitialized";
static constexpr const char* kClosedString = "kClosed";
static const char* GetNativeBridgeStateString(NativeBridgeState state) {
  switch (state) {
    case NativeBridgeState::kNotSetup:
      return kNotSetupString;
    case NativeBridgeState::kOpened:
      return kOpenedString;
    case NativeBridgeState::kPreInitialized:
      return kPreInitializedString;
    case NativeBridgeState::kInitialized:
      return kInitializedString;
    case NativeBridgeState::kClosed:
      return kClosedString;
  }
}
static NativeBridgeState state = NativeBridgeState::kNotSetup;
enum NativeBridgeImplementationVersion {
    DEFAULT_VERSION = 1,
    SIGNAL_VERSION = 2,
    NAMESPACE_VERSION = 3,
};
static bool had_error = false;
static void* native_bridge_handle = nullptr;
static const NativeBridgeCallbacks* callbacks = nullptr;
static const NativeBridgeRuntimeCallbacks* runtime_callbacks = nullptr;
static char* app_code_cache_dir = nullptr;
static constexpr const char* kCodeCacheDir = "code_cache";
static bool CharacterAllowed(char c, bool first) {
  if (first) {
    return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z');
  } else {
    return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || ('0' <= c && c <= '9') ||
           (c == '.') || (c == '_') || (c == '-');
  }
}
static void ReleaseAppCodeCacheDir() {
  if (app_code_cache_dir != nullptr) {
    delete[] app_code_cache_dir;
    app_code_cache_dir = nullptr;
  }
}
bool NativeBridgeNameAcceptable(const char* nb_library_filename) {
  const char* ptr = nb_library_filename;
  if (*ptr == 0) {
    return true;
  } else {
    if (!CharacterAllowed(*ptr, true)) {
      ALOGE("Native bridge library %s has been rejected for first character %c",
            nb_library_filename,
            *ptr);
      return false;
    } else {
      ptr++;
      while (*ptr != 0) {
        if (!CharacterAllowed(*ptr, false)) {
          ALOGE("Native bridge library %s has been rejected for %c", nb_library_filename, *ptr);
          return false;
        }
        ptr++;
      }
    }
    return true;
  }
}
static bool isCompatibleWith(const uint32_t version) {
  if (callbacks == nullptr || callbacks->version == 0 || version == 0) {
    return false;
  }
  if (callbacks->version >= SIGNAL_VERSION) {
    return callbacks->isCompatibleWith(version);
  }
  return true;
}
static void CloseNativeBridge(bool with_error) {
  state = NativeBridgeState::kClosed;
  had_error |= with_error;
  ReleaseAppCodeCacheDir();
}
bool LoadNativeBridge(const char* nb_library_filename,
                      const NativeBridgeRuntimeCallbacks* runtime_cbs) {
  if (state != NativeBridgeState::kNotSetup) {
    if (nb_library_filename != nullptr) {
      ALOGW("Called LoadNativeBridge for an already set up native bridge. State is %s.",
            GetNativeBridgeStateString(state));
    }
    had_error = true;
    return false;
  }
  if (nb_library_filename == nullptr || *nb_library_filename == 0) {
    CloseNativeBridge(false);
    return false;
  } else {
    if (!NativeBridgeNameAcceptable(nb_library_filename)) {
      CloseNativeBridge(true);
    } else {
      void* handle = dlopen(nb_library_filename, RTLD_LAZY);
      if (handle != nullptr) {
        callbacks = reinterpret_cast<NativeBridgeCallbacks*>(dlsym(handle,
                                                                   kNativeBridgeInterfaceSymbol));
        if (callbacks != nullptr) {
          if (isCompatibleWith(NAMESPACE_VERSION)) {
            native_bridge_handle = handle;
          } else {
            callbacks = nullptr;
            dlclose(handle);
            ALOGW("Unsupported native bridge interface.");
          }
        } else {
          dlclose(handle);
        }
      }
      if (callbacks == nullptr) {
        CloseNativeBridge(true);
      } else {
        runtime_callbacks = runtime_cbs;
        state = NativeBridgeState::kOpened;
      }
    }
    return state == NativeBridgeState::kOpened;
  }
}
#if defined(__arm__)
static const char* kRuntimeISA = "arm";
#elif defined(__aarch64__)
static const char* kRuntimeISA = "arm64";
#elif defined(__mips__) && !defined(__LP64__)
static const char* kRuntimeISA = "mips";
#elif defined(__mips__) && defined(__LP64__)
static const char* kRuntimeISA = "mips64";
#elif defined(__i386__)
static const char* kRuntimeISA = "x86";
#elif defined(__x86_64__)
static const char* kRuntimeISA = "x86_64";
#else
static const char* kRuntimeISA = "unknown";
#endif
bool NeedsNativeBridge(const char* instruction_set) {
  if (instruction_set == nullptr) {
    ALOGE("Null instruction set in NeedsNativeBridge.");
    return false;
  }
  return strncmp(instruction_set, kRuntimeISA, strlen(kRuntimeISA) + 1) != 0;
}
#ifdef __APPLE__
template<typename T> void UNUSED(const T&) {}
#endif
bool PreInitializeNativeBridge(const char* app_data_dir_in, const char* instruction_set) {
  if (state != NativeBridgeState::kOpened) {
    ALOGE("Invalid state: native bridge is expected to be opened.");
    CloseNativeBridge(true);
    return false;
  }
  if (app_data_dir_in == nullptr) {
    ALOGE("Application private directory cannot be null.");
    CloseNativeBridge(true);
    return false;
  }
  const size_t len = strlen(app_data_dir_in) + strlen(kCodeCacheDir) + 2;
  app_code_cache_dir = new char[len];
  snprintf(app_code_cache_dir, len, "%s/%s", app_data_dir_in, kCodeCacheDir);
  state = NativeBridgeState::kPreInitialized;
#ifndef __APPLE__
  if (instruction_set == nullptr) {
    return true;
  }
  size_t isa_len = strlen(instruction_set);
  if (isa_len > 10) {
    ALOGW("Instruction set %s is malformed, must be less than or equal to 10 characters.",
          instruction_set);
    return true;
  }
  char cpuinfo_path[1024];
#if defined(__ANDROID__)
  snprintf(cpuinfo_path, sizeof(cpuinfo_path), "/system/lib"
#ifdef __LP64__
      "64"
#endif
      "/%s/cpuinfo", instruction_set);
#else
  snprintf(cpuinfo_path, sizeof(cpuinfo_path), "./cpuinfo");
#endif
  if (TEMP_FAILURE_RETRY(mount(cpuinfo_path,
                               "/proc/cpuinfo",
                               nullptr,
                               MS_BIND,
                               nullptr)) == -1) {
    ALOGW("Failed to bind-mount %s as /proc/cpuinfo: %s", cpuinfo_path, strerror(errno));
  }
#else
  UNUSED(instruction_set);
  ALOGW("Mac OS does not support bind-mounting. Host simulation of native bridge impossible.");
#endif
  return true;
}
static void SetCpuAbi(JNIEnv* env, jclass build_class, const char* field, const char* value) {
  if (value != nullptr) {
    jfieldID field_id = env->GetStaticFieldID(build_class, field, "Ljava/lang/String;");
    if (field_id == nullptr) {
      env->ExceptionClear();
      ALOGW("Could not find %s field.", field);
      return;
    }
    jstring str = env->NewStringUTF(value);
    if (str == nullptr) {
      env->ExceptionClear();
      ALOGW("Could not create string %s.", value);
      return;
    }
    env->SetStaticObjectField(build_class, field_id, str);
  }
}
static void SetupEnvironment(const NativeBridgeCallbacks* callbacks, JNIEnv* env, const char* isa) {
  if (env == nullptr) {
    ALOGW("No JNIEnv* to set up app environment.");
    return;
  }
  const struct NativeBridgeRuntimeValues* env_values = callbacks->getAppEnv(isa);
  if (env_values == nullptr) {
    return;
  }
  jint success = env->PushLocalFrame(16);
  if (success < 0) {
    ALOGW("Out of memory while setting up app environment.");
    env->ExceptionClear();
    return;
  }
  if (env_values->cpu_abi != nullptr || env_values->cpu_abi2 != nullptr ||
      env_values->abi_count >= 0) {
    jclass bclass_id = env->FindClass("android/os/Build");
    if (bclass_id != nullptr) {
      SetCpuAbi(env, bclass_id, "CPU_ABI", env_values->cpu_abi);
      SetCpuAbi(env, bclass_id, "CPU_ABI2", env_values->cpu_abi2);
    } else {
      env->ExceptionClear();
      ALOGW("Could not find Build class.");
    }
  }
  if (env_values->os_arch != nullptr) {
    jclass sclass_id = env->FindClass("java/lang/System");
    if (sclass_id != nullptr) {
      jmethodID set_prop_id = env->GetStaticMethodID(sclass_id, "setUnchangeableSystemProperty",
          "(Ljava/lang/String;Ljava/lang/String;)V");
      if (set_prop_id != nullptr) {
        env->CallStaticVoidMethod(sclass_id, set_prop_id, env->NewStringUTF("os.arch"),
            env->NewStringUTF(env_values->os_arch));
      } else {
        env->ExceptionClear();
        ALOGW("Could not find System#setUnchangeableSystemProperty.");
      }
    } else {
      env->ExceptionClear();
      ALOGW("Could not find System class.");
    }
  }
  env->PopLocalFrame(nullptr);
}
bool InitializeNativeBridge(JNIEnv* env, const char* instruction_set) {
  if (state == NativeBridgeState::kPreInitialized) {
    struct stat st;
    if (stat(app_code_cache_dir, &st) == -1) {
      if (errno == ENOENT) {
        if (mkdir(app_code_cache_dir, S_IRWXU | S_IRWXG | S_IXOTH) == -1) {
          ALOGW("Cannot create code cache directory %s: %s.", app_code_cache_dir, strerror(errno));
          ReleaseAppCodeCacheDir();
        }
      } else {
        ALOGW("Cannot stat code cache directory %s: %s.", app_code_cache_dir, strerror(errno));
        ReleaseAppCodeCacheDir();
      }
    } else if (!S_ISDIR(st.st_mode)) {
      ALOGW("Code cache is not a directory %s.", app_code_cache_dir);
      ReleaseAppCodeCacheDir();
    }
    if (state == NativeBridgeState::kPreInitialized) {
      if (callbacks->initialize(runtime_callbacks, app_code_cache_dir, instruction_set)) {
        SetupEnvironment(callbacks, env, instruction_set);
        state = NativeBridgeState::kInitialized;
        ReleaseAppCodeCacheDir();
      } else {
        dlclose(native_bridge_handle);
        CloseNativeBridge(true);
      }
    }
  } else {
    CloseNativeBridge(true);
  }
  return state == NativeBridgeState::kInitialized;
}
void UnloadNativeBridge() {
  switch(state) {
    case NativeBridgeState::kOpened:
    case NativeBridgeState::kPreInitialized:
    case NativeBridgeState::kInitialized:
      dlclose(native_bridge_handle);
      CloseNativeBridge(false);
      break;
    case NativeBridgeState::kNotSetup:
      CloseNativeBridge(true);
      break;
    case NativeBridgeState::kClosed:
      break;
  }
}
bool NativeBridgeError() {
  return had_error;
}
bool NativeBridgeAvailable() {
  return state == NativeBridgeState::kOpened
      || state == NativeBridgeState::kPreInitialized
      || state == NativeBridgeState::kInitialized;
}
bool NativeBridgeInitialized() {
  return state == NativeBridgeState::kInitialized;
}
void* NativeBridgeLoadLibrary(const char* libpath, int flag) {
  if (NativeBridgeInitialized()) {
    return callbacks->loadLibrary(libpath, flag);
  }
  return nullptr;
}
void* NativeBridgeGetTrampoline(void* handle, const char* name, const char* shorty,
                                uint32_t len) {
  if (NativeBridgeInitialized()) {
    return callbacks->getTrampoline(handle, name, shorty, len);
  }
  return nullptr;
}
bool NativeBridgeIsSupported(const char* libpath) {
  if (NativeBridgeInitialized()) {
    return callbacks->isSupported(libpath);
  }
  return false;
}
uint32_t NativeBridgeGetVersion() {
  if (NativeBridgeAvailable()) {
    return callbacks->version;
  }
  return 0;
}
NativeBridgeSignalHandlerFn NativeBridgeGetSignalHandler(int signal) {
  if (NativeBridgeInitialized()) {
    if (isCompatibleWith(SIGNAL_VERSION)) {
      return callbacks->getSignalHandler(signal);
    } else {
      ALOGE("not compatible with version %d, cannot get signal handler", SIGNAL_VERSION);
    }
  }
  return nullptr;
}
int NativeBridgeUnloadLibrary(void* handle) {
  if (NativeBridgeInitialized()) {
    if (isCompatibleWith(NAMESPACE_VERSION)) {
      return callbacks->unloadLibrary(handle);
    } else {
      ALOGE("not compatible with version %d, cannot unload library", NAMESPACE_VERSION);
    }
  }
  return -1;
}
const char* NativeBridgeGetError() {
  if (NativeBridgeInitialized()) {
    if (isCompatibleWith(NAMESPACE_VERSION)) {
      return callbacks->getError();
    } else {
      return "native bridge implementation is not compatible with version 3, cannot get message";
    }
  }
  return "native bridge is not initialized";
}
bool NativeBridgeIsPathSupported(const char* path) {
  if (NativeBridgeInitialized()) {
    if (isCompatibleWith(NAMESPACE_VERSION)) {
      return callbacks->isPathSupported(path);
    } else {
      ALOGE("not compatible with version %d, cannot check via library path", NAMESPACE_VERSION);
    }
  }
  return false;
}
bool NativeBridgeInitAnonymousNamespace(const char* public_ns_sonames,
                                        const char* anon_ns_library_path) {
  if (NativeBridgeInitialized()) {
    if (isCompatibleWith(NAMESPACE_VERSION)) {
      return callbacks->initAnonymousNamespace(public_ns_sonames, anon_ns_library_path);
    } else {
      ALOGE("not compatible with version %d, cannot init namespace", NAMESPACE_VERSION);
    }
  }
  return false;
}
native_bridge_namespace_t* NativeBridgeCreateNamespace(const char* name,
                                                       const char* ld_library_path,
                                                       const char* default_library_path,
                                                       uint64_t type,
                                                       const char* permitted_when_isolated_path,
                                                       native_bridge_namespace_t* parent_ns) {
  if (NativeBridgeInitialized()) {
    if (isCompatibleWith(NAMESPACE_VERSION)) {
      return callbacks->createNamespace(name,
                                        ld_library_path,
                                        default_library_path,
                                        type,
                                        permitted_when_isolated_path,
                                        parent_ns);
    } else {
      ALOGE("not compatible with version %d, cannot create namespace %s", NAMESPACE_VERSION, name);
    }
  }
  return nullptr;
}
bool NativeBridgeLinkNamespaces(native_bridge_namespace_t* from, native_bridge_namespace_t* to,
                                const char* shared_libs_sonames) {
  if (NativeBridgeInitialized()) {
    if (isCompatibleWith(NAMESPACE_VERSION)) {
      return callbacks->linkNamespaces(from, to, shared_libs_sonames);
    } else {
      ALOGE("not compatible with version %d, cannot init namespace", NAMESPACE_VERSION);
    }
  }
  return false;
}
void* NativeBridgeLoadLibraryExt(const char* libpath, int flag, native_bridge_namespace_t* ns) {
  if (NativeBridgeInitialized()) {
    if (isCompatibleWith(NAMESPACE_VERSION)) {
      return callbacks->loadLibraryExt(libpath, flag, ns);
    } else {
      ALOGE("not compatible with version %d, cannot load library in namespace", NAMESPACE_VERSION);
    }
  }
  return nullptr;
}
};
