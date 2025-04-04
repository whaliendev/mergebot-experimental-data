#ifndef ART_RUNTIME_HIDDEN_API_H_
#define ART_RUNTIME_HIDDEN_API_H_ 
#include "art_field-inl.h"
#include "art_method-inl.h"
<<<<<<< HEAD
#include "base/mutex.h"
||||||| f6590308d2
=======
#include "base/dumpable.h"
>>>>>>> be4aa299
#include "dex/hidden_api_access_flags.h"
#include "mirror/class-inl.h"
#include "reflection.h"
#include "runtime.h"
namespace art {
namespace hiddenapi {
enum class EnforcementPolicy {
  kNoChecks = 0,
  kAllLists = 1,
  kDarkGreyAndBlackList = 2,
  kBlacklistOnly = 3,
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
inline Action GetMemberAction(uint32_t access_flags) {
  EnforcementPolicy policy = Runtime::Current()->GetHiddenApiEnforcementPolicy();
  if (policy == EnforcementPolicy::kNoChecks) {
    return kAllow;
  }
  HiddenApiAccessFlags::ApiList api_list = HiddenApiAccessFlags::DecodeFromRuntime(access_flags);
  if (api_list == HiddenApiAccessFlags::kWhitelist) {
    return kAllow;
  }
  if (static_cast<int>(policy) > static_cast<int>(api_list)) {
    return api_list == HiddenApiAccessFlags::kDarkGreylist
        ? kAllowButWarnAndToast
        : kAllowButWarn;
  } else {
    return kDeny;
  }
}
<<<<<<< HEAD
namespace detail {
class MemberSignature {
 private:
  std::string member_type_;
  std::vector<std::string> signature_parts_;
  std::string tmp_;
 public:
  explicit MemberSignature(ArtField* field) REQUIRES_SHARED(Locks::mutator_lock_);
  explicit MemberSignature(ArtMethod* method) REQUIRES_SHARED(Locks::mutator_lock_);
  void Dump(std::ostream& os) const;
  bool DoesPrefixMatch(const std::string& prefix) const;
  bool IsExempted(const std::vector<std::string>& exemptions);
  void WarnAboutAccess(AccessMethod access_method, HiddenApiAccessFlags::ApiList list);
};
template<typename T>
bool ShouldBlockAccessToMemberImpl(T* member,
                                   Action action,
                                   AccessMethod access_method)
    REQUIRES_SHARED(Locks::mutator_lock_);
ALWAYS_INLINE
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
||||||| f6590308d2
inline void WarnAboutMemberAccess(ArtField* field, AccessMethod access_method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  std::string tmp;
  LOG(WARNING) << "Accessing hidden field "
               << field->GetDeclaringClass()->GetDescriptor(&tmp) << "->"
               << field->GetName() << ":" << field->GetTypeDescriptor()
               << " (" << HiddenApiAccessFlags::DecodeFromRuntime(field->GetAccessFlags())
               << ", " << access_method << ")";
}
=======
>>>>>>> be4aa299
<<<<<<< HEAD
}
||||||| f6590308d2
inline void WarnAboutMemberAccess(ArtMethod* method, AccessMethod access_method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  std::string tmp;
  LOG(WARNING) << "Accessing hidden method "
               << method->GetDeclaringClass()->GetDescriptor(&tmp) << "->"
               << method->GetName() << method->GetSignature().ToString()
               << " (" << HiddenApiAccessFlags::DecodeFromRuntime(method->GetAccessFlags())
               << ", " << access_method << ")";
}
=======
class MemberSignature {
 private:
  std::string member_type_;
  std::vector<std::string> signature_parts_;
  std::string tmp_;
 public:
  explicit MemberSignature(ArtField* field) REQUIRES_SHARED(Locks::mutator_lock_) {
    member_type_ = "field";
    signature_parts_ = {
      field->GetDeclaringClass()->GetDescriptor(&tmp_),
      "->",
      field->GetName(),
      ":",
      field->GetTypeDescriptor()
    };
  }
  explicit MemberSignature(ArtMethod* method) REQUIRES_SHARED(Locks::mutator_lock_) {
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
>>>>>>> be4aa299
template<typename T>
inline bool ShouldBlockAccessToMember(T* member,
                                      Thread* self,
                                      std::function<bool(Thread*)> fn_caller_in_platform,
                                      AccessMethod access_method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(member != nullptr);
  Action action = GetMemberAction(member->GetAccessFlags());
  if (action == kAllow) {
    return false;
  }
  if (fn_caller_in_platform(self)) {
    return false;
  }
<<<<<<< HEAD
  return detail::ShouldBlockAccessToMemberImpl(member, action, access_method);
||||||| f6590308d2
  WarnAboutMemberAccess(member, access_method);
  if (action == kDeny) {
    return true;
  }
  DCHECK(action == kAllowButWarn || action == kAllowButWarnAndToast);
  Runtime* runtime = Runtime::Current();
  if (runtime->ShouldDedupeHiddenApiWarnings()) {
    member->SetAccessFlags(HiddenApiAccessFlags::EncodeForRuntime(
        member->GetAccessFlags(), HiddenApiAccessFlags::kWhitelist));
  }
  if (action == kAllowButWarnAndToast || runtime->ShouldAlwaysSetHiddenApiWarningFlag()) {
    runtime->SetPendingHiddenApiWarning(true);
  }
  return false;
}
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
=======
  MemberSignature member_signature(member);
  Runtime* runtime = Runtime::Current();
  if (action == kDeny) {
    if (member_signature.IsExempted(runtime->GetHiddenApiExemptions())) {
      action = kAllowButWarn;
      member->SetAccessFlags(HiddenApiAccessFlags::EncodeForRuntime(
              member->GetAccessFlags(), HiddenApiAccessFlags::kLightGreylist));
    }
  }
  member_signature.WarnAboutAccess(access_method,
      HiddenApiAccessFlags::DecodeFromRuntime(member->GetAccessFlags()));
  if (action == kDeny) {
    return true;
  }
  DCHECK(action == kAllowButWarn || action == kAllowButWarnAndToast);
  if (runtime->ShouldDedupeHiddenApiWarnings()) {
    member->SetAccessFlags(HiddenApiAccessFlags::EncodeForRuntime(
        member->GetAccessFlags(), HiddenApiAccessFlags::kWhitelist));
  }
  if (action == kAllowButWarnAndToast || runtime->ShouldAlwaysSetHiddenApiWarningFlag()) {
    runtime->SetPendingHiddenApiWarning(true);
  }
  return false;
}
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
>>>>>>> be4aa299
}
inline bool IsCallerInPlatformDex(ObjPtr<mirror::Class> caller)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return !caller.IsNull() &&
      detail::IsCallerInPlatformDex(caller->GetClassLoader(), caller->GetDexCache());
}
template<typename T>
inline bool ShouldBlockAccessToMember(T* member,
                                      ObjPtr<mirror::ClassLoader> caller_class_loader,
                                      ObjPtr<mirror::DexCache> caller_dex_cache,
                                      AccessMethod access_method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  bool caller_in_platform = detail::IsCallerInPlatformDex(caller_class_loader, caller_dex_cache);
  return ShouldBlockAccessToMember(member,
                                                nullptr,
                                   [caller_in_platform] (Thread*) { return caller_in_platform; },
                                   access_method);
}
}
}
#endif
