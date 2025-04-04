/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_RUNTIME_HIDDEN_API_H_
#define ART_RUNTIME_HIDDEN_API_H_

#include "art_field-inl.h"
#include "art_method-inl.h"
#include "dex/hidden_api_access_flags.h"
#include "mirror/class-inl.h"
#include "reflection.h"
#include "runtime.h"

namespace art {
namespace hiddenapi {

// Hidden API enforcement policy
// This must be kept in sync with ApplicationInfo.ApiEnforcementPolicy in
// frameworks/base/core/java/android/content/pm/ApplicationInfo.java
enum class EnforcementPolicy {
  kNoChecks             = 0,
  kAllLists             = 1,  // ban anything but whitelist
  kDarkGreyAndBlackList = 2,  // ban dark grey & blacklist
  kBlacklistOnly        = 3,  // ban blacklist violations only
  kMax = kBlacklistOnly,
};

inline EnforcementPolicy EnforcementPolicyFromInt(int api_policy_int) {
  DCHECK_GE(api_policy_int, 0);
  DCHECK_LE(api_policy_int, static_cast<int>(EnforcementPolicy::kMax));
  return static_cast<EnforcementPolicy>(api_policy_int);
}

enum Action {
  kAllow,
  kAllowButWarn,
  kAllowButWarnAndToast,
  kDeny
};

enum AccessMethod {
  kReflection,
  kJNI,
  kLinking,
};

inline std::ostream& operator<<(std::ostream& os, AccessMethod value) {
  switch (value) {
    case kReflection:
      os << "reflection";
      break;
    case kJNI:
      os << "JNI";
      break;
    case kLinking:
      os << "linking";
      break;
  }
  return os;
}

static constexpr bool EnumsEqual(EnforcementPolicy policy, HiddenApiAccessFlags::ApiList apiList) {
  return static_cast<int>(policy) == static_cast<int>(apiList);
}

inline Action GetMemberAction(uint32_t access_flags) {
  EnforcementPolicy policy = Runtime::Current()->GetHiddenApiEnforcementPolicy();
  if (policy == EnforcementPolicy::kNoChecks) {
    // Exit early. Nothing to enforce.
    return kAllow;
  }

  HiddenApiAccessFlags::ApiList api_list = HiddenApiAccessFlags::DecodeFromRuntime(access_flags);
  if (api_list == HiddenApiAccessFlags::kWhitelist) {
    return kAllow;
  }
  // The logic below relies on equality of values in the enums EnforcementPolicy and
  // HiddenApiAccessFlags::ApiList, and their ordering. Assert that this is as expected.
  static_assert(
      EnumsEqual(EnforcementPolicy::kAllLists, HiddenApiAccessFlags::kLightGreylist) &&
      EnumsEqual(EnforcementPolicy::kDarkGreyAndBlackList, HiddenApiAccessFlags::kDarkGreylist) &&
      EnumsEqual(EnforcementPolicy::kBlacklistOnly, HiddenApiAccessFlags::kBlacklist),
      "Mismatch between EnforcementPolicy and ApiList enums");
  static_assert(
      EnforcementPolicy::kAllLists < EnforcementPolicy::kDarkGreyAndBlackList &&
      EnforcementPolicy::kDarkGreyAndBlackList < EnforcementPolicy::kBlacklistOnly,
      "EnforcementPolicy values ordering not correct");
  if (static_cast<int>(policy) > static_cast<int>(api_list)) {
    return api_list == HiddenApiAccessFlags::kDarkGreylist
        ? kAllowButWarnAndToast
        : kAllowButWarn;
  } else {
    return kDeny;
  }
}

// Issue a warning about field access.
inline void WarnAboutMemberAccess(ArtField* field, AccessMethod access_method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  std::string tmp;
  LOG(WARNING) << "Accessing hidden field "
               << field->GetDeclaringClass()->GetDescriptor(&tmp) << "->"
               << field->GetName() << ":" << field->GetTypeDescriptor()
               << " (" << HiddenApiAccessFlags::DecodeFromRuntime(field->GetAccessFlags())
               << ", " << access_method << ")";
}

// Issue a warning about method access.
inline void WarnAboutMemberAccess(ArtMethod* method, AccessMethod access_method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  std::string tmp;
  LOG(WARNING) << "Accessing hidden method "
               << method->GetDeclaringClass()->GetDescriptor(&tmp) << "->"
               << method->GetName() << method->GetSignature().ToString()
               << " (" << HiddenApiAccessFlags::DecodeFromRuntime(method->GetAccessFlags())
               << ", " << access_method << ")";
}

// Returns true if access to `member` should be denied to the caller of the
// reflective query. The decision is based on whether the caller is in the
// platform or not. Because different users of this function determine this
// in a different way, `fn_caller_in_platform(self)` is called and should
// return true if the caller is located in the platform.
// This function might print warnings into the log if the member is hidden.
template<typename T>
inline bool ShouldBlockAccessToMember(T* member,
                                      Thread* self,
                                      std::function<bool(Thread*)> fn_caller_in_platform,
                                      AccessMethod access_method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(member != nullptr);

  Action action = GetMemberAction(member->GetAccessFlags());
  if (action == kAllow) {
    // Nothing to do.
    return false;
  }

  // Member is hidden. Invoke `fn_caller_in_platform` and find the origin of the access.
  // This can be *very* expensive. Save it for last.
  if (fn_caller_in_platform(self)) {
    // Caller in the platform. Exit.
    return false;
  }

  // Member is hidden and caller is not in the platform.

  // Print a log message with information about this class member access.
  // We do this regardless of whether we block the access or not.
  WarnAboutMemberAccess(member, access_method);

  if (action == kDeny) {
    // Block access
    return true;
  }

  // Allow access to this member but print a warning.
  DCHECK(action == kAllowButWarn || action == kAllowButWarnAndToast);

  Runtime* runtime = Runtime::Current();

  // Depending on a runtime flag, we might move the member into whitelist and
  // skip the warning the next time the member is accessed.
  if (runtime->ShouldDedupeHiddenApiWarnings()) {
    member->SetAccessFlags(HiddenApiAccessFlags::EncodeForRuntime(
        member->GetAccessFlags(), HiddenApiAccessFlags::kWhitelist));
  }

  // If this action requires a UI warning, set the appropriate flag.
  if (action == kAllowButWarnAndToast || runtime->ShouldAlwaysSetHiddenApiWarningFlag()) {
    runtime->SetPendingHiddenApiWarning(true);
  }

  return false;
}

// Returns true if the caller is either loaded by the boot strap class loader or comes from
// a dex file located in ${ANDROID_ROOT}/framework/.
inline bool IsCallerInPlatformDex(ObjPtr<mirror::ClassLoader> caller_class_loader,
                                  ObjPtr<mirror::DexCache> caller_dex_cache)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (caller_class_loader.IsNull()) {
    return true;
  } else if (caller_dex_cache.IsNull()) {
    return false;
  } else {
    const DexFile* caller_dex_file = caller_dex_cache->GetDexFile();
    return caller_dex_file != nullptr && caller_dex_file->IsPlatformDexFile();
  }
}

inline bool IsCallerInPlatformDex(ObjPtr<mirror::Class> caller)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return !caller.IsNull() && IsCallerInPlatformDex(caller->GetClassLoader(), caller->GetDexCache());
}

// Returns true if access to `member` should be denied to a caller loaded with
// `caller_class_loader`.
// This function might print warnings into the log if the member is hidden.
template<typename T>
inline bool ShouldBlockAccessToMember(T* member,
                                      ObjPtr<mirror::ClassLoader> caller_class_loader,
                                      ObjPtr<mirror::DexCache> caller_dex_cache,
                                      AccessMethod access_method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  bool caller_in_platform = IsCallerInPlatformDex(caller_class_loader, caller_dex_cache);
  return ShouldBlockAccessToMember(member,
                                   /* thread */ nullptr,
                                   [caller_in_platform] (Thread*) { return caller_in_platform; },
                                   access_method);
}

}  // namespace hiddenapi
}  // namespace art

#endif  // ART_RUNTIME_HIDDEN_API_H_
