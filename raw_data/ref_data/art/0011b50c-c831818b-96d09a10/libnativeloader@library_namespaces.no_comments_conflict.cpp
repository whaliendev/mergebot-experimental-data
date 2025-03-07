#if defined(ART_TARGET_ANDROID)
#define LOG_TAG "nativeloader"
#include "library_namespaces.h"
#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <algorithm>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>
#include "android-base/file.h"
#include "android-base/logging.h"
#include "android-base/macros.h"
#include "android-base/result.h"
#include "android-base/stringprintf.h"
#include "android-base/strings.h"
#include "nativehelper/scoped_utf_chars.h"
#include "nativeloader/dlext_namespaces.h"
#include "public_libraries.h"
#include "utils.h"
namespace android::nativeloader {
namespace {
using ::android::base::Error;
constexpr const char* kApexPath = "/apex/";
constexpr const char* kClassloaderNamespaceName = "clns";
constexpr const char* kVendorClassloaderNamespaceName = "vendor-clns";
constexpr const char* kProductClassloaderNamespaceName = "product-clns";
constexpr const char* kSharedNamespaceSuffix = "-shared";
constexpr const char* kAlwaysPermittedDirectories = "/data:/mnt/expand";
constexpr const char* kVendorLibPath = "/vendor/" LIB;
constexpr const char* kProductLibPath = "/product/" LIB ":/system/product/" LIB;
const std::regex kVendorPathRegex("(/system)?/vendor/.*");
const std::regex kProductPathRegex("(/system)?/product/.*");
jobject GetParentClassLoader(JNIEnv* env, jobject class_loader) {
  jclass class_loader_class = env->FindClass("java/lang/ClassLoader");
  jmethodID get_parent =
      env->GetMethodID(class_loader_class, "getParent", "()Ljava/lang/ClassLoader;");
  return env->CallObjectMethod(class_loader, get_parent);
}
}
ApiDomain GetApiDomainFromPath(const std::string_view path) {
  if (std::regex_match(path.begin(), path.end(), kVendorPathRegex)) {
    return API_DOMAIN_VENDOR;
  }
  if (is_product_treblelized() && std::regex_match(path.begin(), path.end(), kProductPathRegex)) {
    return API_DOMAIN_PRODUCT;
  }
  return API_DOMAIN_DEFAULT;
}
Result<ApiDomain> GetApiDomainFromPathList(const std::string& path_list) {
  ApiDomain result = API_DOMAIN_DEFAULT;
  size_t start_pos = 0;
  while (true) {
    size_t end_pos = path_list.find(':', start_pos);
    ApiDomain api_domain =
        GetApiDomainFromPath(std::string_view(path_list).substr(start_pos, end_pos));
    if (api_domain != API_DOMAIN_DEFAULT) {
      if (result != API_DOMAIN_DEFAULT && result != api_domain) {
        return Error() << "Path list crosses partition boundaries: " << path_list;
      }
      result = api_domain;
    }
    if (end_pos == std::string::npos) {
      break;
    }
    start_pos = end_pos + 1;
  }
  return result;
}
void LibraryNamespaces::Initialize() {
  if (initialized_) {
    return;
  }
  for (const std::string& soname : android::base::Split(preloadable_public_libraries(), ":")) {
    void* handle = OpenSystemLibrary(soname.c_str(), RTLD_NOW | RTLD_NODELETE);
    LOG_ALWAYS_FATAL_IF(handle == nullptr,
                        "Error preloading public library %s: %s", soname.c_str(), dlerror());
  }
}
static constexpr const char LIBRARY_ALL[] = "ALL";
static const std::string filter_public_libraries(
    uint32_t target_sdk_version, const std::vector<std::string>& uses_libraries,
    const std::string& public_libraries) {
  if (target_sdk_version <= 30) {
    return public_libraries;
  }
  if (std::find(uses_libraries.begin(), uses_libraries.end(), LIBRARY_ALL) !=
      uses_libraries.end()) {
    return public_libraries;
  }
  std::vector<std::string> filtered;
  std::vector<std::string> orig = android::base::Split(public_libraries, ":");
  for (const std::string& lib : uses_libraries) {
    if (std::find(orig.begin(), orig.end(), lib) != orig.end()) {
      filtered.emplace_back(lib);
    }
  }
  return android::base::Join(filtered, ":");
}
Result<NativeLoaderNamespace*> LibraryNamespaces::Create(JNIEnv* env,
                                                         uint32_t target_sdk_version,
                                                         jobject class_loader,
                                                         ApiDomain api_domain,
                                                         bool is_shared,
                                                         const std::string& dex_path,
                                                         jstring library_path_j,
                                                         jstring permitted_path_j,
                                                         jstring uses_library_list_j) {
  std::string library_path;
  if (library_path_j != nullptr) {
    ScopedUtfChars library_path_utf_chars(env, library_path_j);
    library_path = library_path_utf_chars.c_str();
  }
  std::vector<std::string> uses_libraries;
  if (uses_library_list_j != nullptr) {
    ScopedUtfChars names(env, uses_library_list_j);
    uses_libraries = android::base::Split(names.c_str(), ":");
  } else {
    uses_libraries.emplace_back(LIBRARY_ALL);
  }
  std::string permitted_path = kAlwaysPermittedDirectories;
  if (permitted_path_j != nullptr) {
    ScopedUtfChars path(env, permitted_path_j);
    if (path.c_str() != nullptr && path.size() > 0) {
      permitted_path = permitted_path + ":" + path.c_str();
    }
  }
  LOG_ALWAYS_FATAL_IF(FindNamespaceByClassLoader(env, class_loader) != nullptr,
                      "There is already a namespace associated with this classloader");
  std::string system_exposed_libraries = default_public_libraries();
  std::string namespace_name = kClassloaderNamespaceName;
  ApiDomain unbundled_app_domain = API_DOMAIN_DEFAULT;
  const char* api_domain_msg = "other apk";
  if (!is_shared) {
    if (api_domain == API_DOMAIN_VENDOR) {
      unbundled_app_domain = API_DOMAIN_VENDOR;
      api_domain_msg = "unbundled vendor apk";
      library_path = library_path + ':' + kVendorLibPath;
      permitted_path = permitted_path + ':' + kVendorLibPath;
      system_exposed_libraries = system_exposed_libraries + ':' + llndk_libraries_vendor();
      namespace_name = kVendorClassloaderNamespaceName;
<<<<<<< HEAD
    } else if (api_domain == API_DOMAIN_PRODUCT) {
      unbundled_app_domain = API_DOMAIN_PRODUCT;
      api_domain_msg = "unbundled product apk";
||||||| 96d09a108d
    } else if (apk_origin == APK_ORIGIN_PRODUCT) {
      unbundled_app_origin = APK_ORIGIN_PRODUCT;
      apk_origin_msg = "unbundled product apk";
=======
    } else if (apk_origin == APK_ORIGIN_PRODUCT && is_product_treblelized()) {
      unbundled_app_origin = APK_ORIGIN_PRODUCT;
      apk_origin_msg = "unbundled product apk";
>>>>>>> c831818b
      library_path = library_path + ':' + kProductLibPath;
      permitted_path = permitted_path + ':' + kProductLibPath;
      system_exposed_libraries = system_exposed_libraries + ':' + llndk_libraries_product();
      namespace_name = kProductClassloaderNamespaceName;
    }
  }
  if (is_shared) {
    namespace_name = namespace_name + kSharedNamespaceSuffix;
  }
  static int clns_count = 0;
  namespace_name = android::base::StringPrintf("%s-%d", namespace_name.c_str(), ++clns_count);
  ALOGD(
      "Configuring %s for %s %s. target_sdk_version=%u, uses_libraries=%s, library_path=%s, "
      "permitted_path=%s",
      namespace_name.c_str(),
      api_domain_msg,
      dex_path.c_str(),
      static_cast<unsigned>(target_sdk_version),
      android::base::Join(uses_libraries, ':').c_str(),
      library_path.c_str(),
      permitted_path.c_str());
  if (unbundled_app_domain != API_DOMAIN_VENDOR) {
    const std::string libs =
        filter_public_libraries(target_sdk_version, uses_libraries, extended_public_libraries());
    if (!libs.empty()) {
      ALOGD("Extending system_exposed_libraries: %s", libs.c_str());
      system_exposed_libraries = system_exposed_libraries + ':' + libs;
    }
  }
  NativeLoaderNamespace* parent_ns = FindParentNamespaceByClassLoader(env, class_loader);
  bool is_main_classloader = app_main_namespace_ == nullptr && !library_path.empty();
  bool also_used_as_anonymous = is_main_classloader;
  Result<NativeLoaderNamespace> app_ns =
      NativeLoaderNamespace::Create(namespace_name,
                                    library_path,
                                    permitted_path,
                                    parent_ns,
                                    is_shared,
                                    target_sdk_version < 24 ,
                                    also_used_as_anonymous);
  if (!app_ns.ok()) {
    return app_ns.error();
  }
  bool is_bridged = app_ns->IsBridged();
  Result<NativeLoaderNamespace> system_ns = NativeLoaderNamespace::GetSystemNamespace(is_bridged);
  if (!system_ns.ok()) {
    return system_ns.error();
  }
  Result<void> linked = app_ns->Link(&system_ns.value(), system_exposed_libraries);
  if (!linked.ok()) {
    return linked.error();
  }
  for (const auto&[apex_ns_name, public_libs] : apex_public_libraries()) {
    Result<NativeLoaderNamespace> ns =
        NativeLoaderNamespace::GetExportedNamespace(apex_ns_name, is_bridged);
    if (ns.ok()) {
      linked = app_ns->Link(&ns.value(), public_libs);
      if (!linked.ok()) {
        return linked.error();
      }
    }
  }
  if (unbundled_app_domain == API_DOMAIN_VENDOR && !vndksp_libraries_vendor().empty()) {
    Result<NativeLoaderNamespace> vndk_ns =
        NativeLoaderNamespace::GetExportedNamespace(kVndkNamespaceName, is_bridged);
    if (vndk_ns.ok()) {
      linked = app_ns->Link(&vndk_ns.value(), vndksp_libraries_vendor());
      if (!linked.ok()) {
        return linked.error();
      }
    }
  }
  if (unbundled_app_domain == API_DOMAIN_PRODUCT && !vndksp_libraries_product().empty()) {
    Result<NativeLoaderNamespace> vndk_ns =
        NativeLoaderNamespace::GetExportedNamespace(kVndkProductNamespaceName, is_bridged);
    if (vndk_ns.ok()) {
      linked = app_ns->Link(&vndk_ns.value(), vndksp_libraries_product());
      if (!linked.ok()) {
        return linked.error();
      }
    }
  }
  for (const std::string& each_jar_path : android::base::Split(dex_path, ":")) {
    std::optional<std::string> apex_ns_name = FindApexNamespaceName(each_jar_path);
    if (apex_ns_name.has_value()) {
      const std::string& jni_libs = apex_jni_libraries(apex_ns_name.value());
      if (jni_libs != "") {
        Result<NativeLoaderNamespace> apex_ns =
            NativeLoaderNamespace::GetExportedNamespace(apex_ns_name.value(), is_bridged);
        if (apex_ns.ok()) {
          linked = app_ns->Link(&apex_ns.value(), jni_libs);
          if (!linked.ok()) {
            return linked.error();
          }
        }
      }
    }
  }
  const std::string vendor_libs =
      filter_public_libraries(target_sdk_version, uses_libraries, vendor_public_libraries());
  if (!vendor_libs.empty()) {
    Result<NativeLoaderNamespace> vendor_ns =
        NativeLoaderNamespace::GetExportedNamespace(kVendorNamespaceName, is_bridged);
    Result<NativeLoaderNamespace> target_ns = vendor_ns.ok() ? vendor_ns : system_ns;
    if (target_ns.ok()) {
      linked = app_ns->Link(&target_ns.value(), vendor_libs);
      if (!linked.ok()) {
        return linked.error();
      }
    }
  }
  const std::string product_libs =
      filter_public_libraries(target_sdk_version, uses_libraries, product_public_libraries());
  if (!product_libs.empty()) {
<<<<<<< HEAD
    Result<NativeLoaderNamespace> target_ns = system_ns;
    if (is_product_treblelized()) {
      target_ns = NativeLoaderNamespace::GetExportedNamespace(kProductNamespaceName, is_bridged);
    }
||||||| 96d09a108d
    auto target_ns = NativeLoaderNamespace::GetExportedNamespace(kProductNamespaceName, is_bridged);
=======
    auto target_ns = system_ns;
    if (is_product_treblelized()) {
      target_ns = NativeLoaderNamespace::GetExportedNamespace(kProductNamespaceName, is_bridged);
    }
>>>>>>> c831818b
    if (target_ns.ok()) {
      linked = app_ns->Link(&target_ns.value(), product_libs);
      if (!linked.ok()) {
        return linked.error();
      }
    } else {
      ALOGW("Namespace for product libs not found: %s", target_ns.error().message().c_str());
    }
  }
  std::pair<jweak, NativeLoaderNamespace>& emplaced =
      namespaces_.emplace_back(std::make_pair(env->NewWeakGlobalRef(class_loader), *app_ns));
  if (is_main_classloader) {
    app_main_namespace_ = &emplaced.second;
  }
  return &emplaced.second;
}
NativeLoaderNamespace* LibraryNamespaces::FindNamespaceByClassLoader(JNIEnv* env,
                                                                     jobject class_loader) {
  auto it = std::find_if(namespaces_.begin(), namespaces_.end(),
                         [&](const std::pair<jweak, NativeLoaderNamespace>& value) {
                           return env->IsSameObject(value.first, class_loader);
                         });
  if (it != namespaces_.end()) {
    return &it->second;
  }
  return nullptr;
}
NativeLoaderNamespace* LibraryNamespaces::FindParentNamespaceByClassLoader(JNIEnv* env,
                                                                           jobject class_loader) {
  jobject parent_class_loader = GetParentClassLoader(env, class_loader);
  while (parent_class_loader != nullptr) {
    NativeLoaderNamespace* ns;
    if ((ns = FindNamespaceByClassLoader(env, parent_class_loader)) != nullptr) {
      return ns;
    }
    parent_class_loader = GetParentClassLoader(env, parent_class_loader);
  }
  return nullptr;
}
std::optional<std::string> FindApexNamespaceName(const std::string& location) {
  if (android::base::StartsWith(location, kApexPath)) {
    size_t start_index = strlen(kApexPath);
    size_t slash_index = location.find_first_of('/', start_index);
    LOG_ALWAYS_FATAL_IF((slash_index == std::string::npos),
                        "Error finding namespace of apex: no slash in path %s", location.c_str());
    std::string name = location.substr(start_index, slash_index - start_index);
    std::replace(name.begin(), name.end(), '.', '_');
    return name;
  }
  return std::nullopt;
}
}
#endif
