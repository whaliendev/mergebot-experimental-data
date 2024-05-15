       
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/output_type.hpp"
#include "duckdb/common/enums/profiler_format.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/progress_bar/progress_bar.hpp"
namespace duckdb {
class ClientContext;
class PhysicalResultCollector;
class PreparedStatementData;
typedef std::function<unique_ptr<PhysicalResultCollector>(ClientContext &context, PreparedStatementData &data)>
    get_result_collector_t;
struct ClientConfig {
 string home_directory;
 bool enable_profiler = false;
 bool enable_detailed_profiling = false;
 ProfilerPrintFormat profiler_print_format = ProfilerPrintFormat::QUERY_TREE;
 string profiler_save_location;
 bool emit_profiler_output = true;
 const char *system_progress_bar_disable_reason = nullptr;
 bool enable_progress_bar = false;
 bool print_progress_bar = true;
 int wait_time = 2000;
 bool preserve_identifier_case = true;
 idx_t max_expression_depth = 1000;
 bool query_verification_enabled = false;
 bool verify_external = false;
 bool verify_serializer = false;
 bool enable_optimizer = true;
 bool enable_caching_operators = true;
 bool verify_parallelism = false;
 bool force_index_join = false;
 bool force_external = false;
 bool force_no_cross_product = false;
 bool force_asof_iejoin = false;
 bool force_async_pipelines = true;
 bool use_replacement_scans = true;
 idx_t perfect_ht_threshold = 12;
 idx_t ordered_aggregate_threshold = (idx_t(1) << 18);
 progress_bar_display_create_func_t display_create_func = nullptr;
 string custom_extension_repo = "";
 ExplainOutputType explain_output_type = ExplainOutputType::PHYSICAL_ONLY;
 idx_t pivot_limit = 100000;
 bool integer_division = false;
 case_insensitive_map_t<Value> set_variables;
 get_result_collector_t result_collector = nullptr;
public:
 static ClientConfig &GetConfig(ClientContext &context);
 static const ClientConfig &GetConfig(const ClientContext &context);
 string ExtractTimezone() const;
 bool AnyVerification() {
  return query_verification_enabled || verify_external || verify_serializer;
 }
};
}
