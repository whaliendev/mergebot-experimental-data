#ifndef ART_COMPILER_DRIVER_COMPILER_OPTIONS_H_
#define ART_COMPILER_DRIVER_COMPILER_OPTIONS_H_ 
#include <ostream>
#include <string>
#include <vector>
#include "base/macros.h"
#include "globals.h"
namespace art {
class PassManagerOptions;
class CompilerOptions FINAL {
 public:
  enum CompilerFilter {
    kVerifyNone,
    kInterpretOnly,
    kVerifyAtRuntime,
    kSpace,
    kBalanced,
    kSpeed,
    kEverything,
    kTime,
  };
  static const CompilerFilter kDefaultCompilerFilter = kSpeed;
  static const size_t kDefaultHugeMethodThreshold = 10000;
  static const size_t kDefaultLargeMethodThreshold = 600;
  static const size_t kDefaultSmallMethodThreshold = 60;
  static const size_t kDefaultTinyMethodThreshold = 20;
  static const size_t kDefaultNumDexMethodsThreshold = 900;
  static constexpr double kDefaultTopKProfileThreshold = 90.0;
  static const bool kDefaultGenerateDebugInfo = kIsDebugBuild;
  static const bool kDefaultIncludePatchInformation = false;
  static const size_t kDefaultInlineDepthLimit = 3;
  static const size_t kDefaultInlineMaxCodeUnits = 18;
  CompilerOptions();
  ~CompilerOptions();
  CompilerOptions(CompilerFilter compiler_filter,
                  size_t huge_method_threshold,
                  size_t large_method_threshold,
                  size_t small_method_threshold,
                  size_t tiny_method_threshold,
                  size_t num_dex_methods_threshold,
                  size_t inline_depth_limit,
                  size_t inline_max_code_units,
                  bool include_patch_information,
                  double top_k_profile_threshold,
                  bool debuggable,
                  bool generate_debug_info,
                  bool implicit_null_checks,
                  bool implicit_so_checks,
                  bool implicit_suspend_checks,
                  bool compile_pic,
                  const std::vector<std::string>* verbose_methods,
                  PassManagerOptions* pass_manager_options,
                  std::ostream* init_failure_output,
                  bool abort_on_hard_verifier_failure);
  CompilerFilter GetCompilerFilter() const {
    return compiler_filter_;
  }
  void SetCompilerFilter(CompilerFilter compiler_filter) {
    compiler_filter_ = compiler_filter;
  }
  bool VerifyAtRuntime() const {
    return compiler_filter_ == CompilerOptions::kVerifyAtRuntime;
  }
  bool IsCompilationEnabled() const {
    return compiler_filter_ != CompilerOptions::kVerifyNone &&
        compiler_filter_ != CompilerOptions::kInterpretOnly &&
        compiler_filter_ != CompilerOptions::kVerifyAtRuntime;
  }
  bool IsVerificationEnabled() const {
    return compiler_filter_ != CompilerOptions::kVerifyNone &&
        compiler_filter_ != CompilerOptions::kVerifyAtRuntime;
  }
  bool NeverVerify() const {
    return compiler_filter_ == CompilerOptions::kVerifyNone;
  }
  size_t GetHugeMethodThreshold() const {
    return huge_method_threshold_;
  }
  size_t GetLargeMethodThreshold() const {
    return large_method_threshold_;
  }
  size_t GetSmallMethodThreshold() const {
    return small_method_threshold_;
  }
  size_t GetTinyMethodThreshold() const {
    return tiny_method_threshold_;
  }
  bool IsHugeMethod(size_t num_dalvik_instructions) const {
    return num_dalvik_instructions > huge_method_threshold_;
  }
  bool IsLargeMethod(size_t num_dalvik_instructions) const {
    return num_dalvik_instructions > large_method_threshold_;
  }
  bool IsSmallMethod(size_t num_dalvik_instructions) const {
    return num_dalvik_instructions > small_method_threshold_;
  }
  bool IsTinyMethod(size_t num_dalvik_instructions) const {
    return num_dalvik_instructions > tiny_method_threshold_;
  }
  size_t GetNumDexMethodsThreshold() const {
    return num_dex_methods_threshold_;
  }
  size_t GetInlineDepthLimit() const {
    return inline_depth_limit_;
  }
  size_t GetInlineMaxCodeUnits() const {
    return inline_max_code_units_;
  }
  double GetTopKProfileThreshold() const {
    return top_k_profile_threshold_;
  }
  bool GetDebuggable() const {
    return debuggable_;
  }
  bool GetGenerateDebugInfo() const {
    return generate_debug_info_;
  }
  bool GetImplicitNullChecks() const {
    return implicit_null_checks_;
  }
  bool GetImplicitStackOverflowChecks() const {
    return implicit_so_checks_;
  }
  bool GetImplicitSuspendChecks() const {
    return implicit_suspend_checks_;
  }
  bool GetIncludePatchInformation() const {
    return include_patch_information_;
  }
  bool GetCompilePic() const {
    return compile_pic_;
  }
  bool HasVerboseMethods() const {
    return verbose_methods_ != nullptr && !verbose_methods_->empty();
  }
  bool IsVerboseMethod(const std::string& pretty_method) const {
    for (const std::string& cur_method : *verbose_methods_) {
      if (pretty_method.find(cur_method) != std::string::npos) {
        return true;
      }
    }
    return false;
  }
  std::ostream* GetInitFailureOutput() const {
    return init_failure_output_;
  }
  const PassManagerOptions* GetPassManagerOptions() const {
    return pass_manager_options_.get();
  }
  bool AbortOnHardVerifierFailure() const {
    return abort_on_hard_verifier_failure_;
  }
 private:
  CompilerFilter compiler_filter_;
  const size_t huge_method_threshold_;
  const size_t large_method_threshold_;
  const size_t small_method_threshold_;
  const size_t tiny_method_threshold_;
  const size_t num_dex_methods_threshold_;
  const size_t inline_depth_limit_;
  const size_t inline_max_code_units_;
  const bool include_patch_information_;
  const double top_k_profile_threshold_;
  const bool debuggable_;
  const bool generate_debug_info_;
  const bool implicit_null_checks_;
  const bool implicit_so_checks_;
  const bool implicit_suspend_checks_;
  const bool compile_pic_;
  const std::vector<std::string>* const verbose_methods_;
  std::unique_ptr<PassManagerOptions> pass_manager_options_;
  const bool abort_on_hard_verifier_failure_;
  std::ostream* const init_failure_output_;
  DISALLOW_COPY_AND_ASSIGN(CompilerOptions);
};
std::ostream& operator<<(std::ostream& os, const CompilerOptions::CompilerFilter& rhs);
}
#endif
