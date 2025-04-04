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
#include "base/dumpable.h"
#include "base/mutex.h"
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
kNoChecks             = 0,kAllLists             = 1,                              // ban anything but whitelist
kDarkGreyAndBlackList = 2,                              // ban dark grey & blacklist
kBlacklistOnly        = 3,                              // ban blacklist violations only
kMax = kBlacklistOnly,};

inline EnforcementPolicy EnforcementPolicyFromInt(int api_policy_int) {
  DCHECK_GE(api_policy_int, 0);
  DCHECK_LE(api_policy_int, static_cast<int>(EnforcementPolicy::kMax));
  return static_cast<EnforcementPolicy>(api_policy_int);
}

enum Action {
kAllow,kAllowButWarn,kAllowButWarnAndToast,  kDeny
};

enum AccessMethod {
kReflection,kJNI,kLinking,};

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
  // HiddenApiAccessFlags::ApiList, and their ordering. Assertions are in hidden_api.cc.
  if (static_cast<int>(policy) > static_cast<int>(api_list)) {
    return api_list == HiddenApiAccessFlags::kDarkGreylist
        ? kAllowButWarnAndToast
        : kAllowButWarn;
  } else {
    return kDeny;
  }
}


// Implementation details. DO NOT ACCESS DIRECTLY.
namespace detail {

// Class to encapsulate the signature of a member (ArtField or ArtMethod). This
// is used as a helper when matching prefixes, and when logging the signature.
class MemberSignature {
private:
  std::string member_type_;
  std::vector<std::string> signature_parts_;
  std::string tmp_;
  
public:
explicit MemberSignature(ArtField* field)                                            REQUIRES_SHARED(Locks::mutator_lock_);
explicit MemberSignature(ArtMethod* method)                                              REQUIRES_SHARED(Locks::mutator_lock_);
                                              
  void Dump(std::ostream& os) const;
  
  // Performs prefix match on this member. Since the full member signature is
  // composed of several parts, we match each part in turn (rather than
  // building the entire thing in memory and performing a simple prefix match)
  bool DoesPrefixMatch(const std::string& prefix) const;
  
  bool IsExempted(const std::vector<std::string>& exemptions);
  
  void WarnAboutAccess(AccessMethod access_method, HiddenApiAccessFlags::ApiList list);
};

// Returns true if the caller is either loaded by the boot strap class loader or comes from
// a dex file located in ${ANDROID_ROOT}/framework/.
ALWAYS_INLINE
inline bool IsCallerInPlatformDex(ObjPtr<mirror::ClassLoader> caller_class_loader,
                                  ObjPtr<mirror::DexCache> caller_dex_cache)
    REQUIRES_SHARED(Locks::mutator_lock_){
    if (caller_class_loader.IsNull()) {
    return true;
    } else if (caller_dex_cache.IsNull()) {
    return false;
    } else {
    const DexFile* caller_dex_file = caller_dex_cache->GetDexFile();
    return caller_dex_file != nullptr && caller_dex_file->IsPlatformDexFile();
    }
    }
    
} // namespace detail

// Class to encapsulate the signature of a member (ArtField or ArtMethod). This
// is used as a helper when matching prefixes, and when logging the signature.
class MemberSignature {
private:
  std::string member_type_;
  std::vector<std::string> signature_parts_;
  std::string tmp_;
  
public:
explicit MemberSignature(ArtField* field)                                            REQUIRES_SHARED(Locks::mutator_lock_){
                                            member_type_ = "field";
                                            signature_parts_ = {
                                            field->GetDeclaringClass()->GetDescriptor(&tmp_),
                                            "->",
                                            field->GetName(),
                                            ":",
                                            field->GetTypeDescriptor()
                                            };
                                            }
                                            
explicit MemberSignature(ArtMethod* method)                                              REQUIRES_SHARED(Locks::mutator_lock_){
                                              member_type_ = "method";
                                              signature_parts_ = {
                                              method->GetDeclaringClass()->GetDescriptor(&tmp_),
                                              "->",
                                              method->GetName(),
                                              method->GetSignature().ToString()
                                              };
                                              }
                                              
  const std::vector<std::string>& Parts() const {
    return signature_parts_;
  }
  
  void Dump(std::ostream& os) const {
    for (std::string part : signature_parts_) {
      os << part;
    }
  }
  // Performs prefix match on this member. Since the full member signature is
  // composed of several parts, we match each part in turn (rather than
  // building the entire thing in memory and performing a simple prefix match)
  bool DoesPrefixMatch(const std::string& prefix) const {
    size_t pos = 0;
    for (const std::string& part : signature_parts_) {
      size_t count = std::min(prefix.length() - pos, part.length());
      if (prefix.compare(pos, count, part, 0, count) == 0) {
        pos += count;
      } else {
        return false;
      }
    }
    // We have a complete match if all parts match (we exit the loop without
    // returning) AND we've matched the whole prefix.
    return pos == prefix.length();
  }
  
  bool IsExempted(const std::vector<std::string>& exemptions) {
    for (const std::string& exemption : exemptions) {
      if (DoesPrefixMatch(exemption)) {
        return true;
      }
    }
    return false;
  }
  
  void WarnAboutAccess(AccessMethod access_method, HiddenApiAccessFlags::ApiList list) {
    LOG(WARNING) << "Accessing hidden " << member_type_ << " " << Dumpable<MemberSignature>(*this)
                 << " (" << list << ", " << access_method << ")";
  }
};

    REQUIRES_SHARED(Locks::mutator_lock_) {
    bool caller_in_platform = IsCallerInPlatformDex(caller_class_loader, caller_dex_cache);
    return ShouldBlockAccessToMember(member,
                                   /* thread */ nullptr,
                                   [caller_in_platform] (Thread*) { return caller_in_platform; },
                                   access_method);
    }
    
inline bool IsCallerInPlatformDex(ObjPtr<mirror::Class> caller)
    REQUIRES_SHARED(Locks::mutator_lock_) {
    bool caller_in_platform = IsCallerInPlatformDex(caller_class_loader, caller_dex_cache);
    return ShouldBlockAccessToMember(member,
                                   /* thread */ nullptr,
                                   [caller_in_platform] (Thread*) { return caller_in_platform; },
                                   access_method);
    }
    
inline bool IsCallerInPlatformDex(ObjPtr<mirror::Class> caller)
    REQUIRES_SHARED(Locks::mutator_lock_) {
    bool caller_in_platform = IsCallerInPlatformDex(caller_class_loader, caller_dex_cache);
    return ShouldBlockAccessToMember(member,
                                   /* thread */ nullptr,
                                   [caller_in_platform] (Thread*) { return caller_in_platform; },
                                   access_method);
    }
    
    REQUIRES_SHARED(Locks::mutator_lock_) {
    bool caller_in_platform = IsCallerInPlatformDex(caller_class_loader, caller_dex_cache);
    return ShouldBlockAccessToMember(member,
                                   /* thread */ nullptr,
                                   [caller_in_platform] (Thread*) { return caller_in_platform; },
                                   access_method);
    }
    
} // namespace hiddenapi

} // namespace art

#endif// ART_RUNTIME_HIDDEN_API_H_