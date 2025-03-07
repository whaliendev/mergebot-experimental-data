#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/execution/column_binding_resolver.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/optimizer/build_probe_side_optimizer.hpp"
#include "duckdb/optimizer/column_lifetime_analyzer.hpp"
#include "duckdb/optimizer/common_aggregate_optimizer.hpp"
#include "duckdb/optimizer/cse_optimizer.hpp"
#include "duckdb/optimizer/cte_filter_pusher.hpp"
#include "duckdb/optimizer/deliminator.hpp"
#include "duckdb/optimizer/expression_heuristics.hpp"
#include "duckdb/optimizer/filter_pullup.hpp"
#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/optimizer/empty_result_pullup.hpp"
#include "duckdb/optimizer/in_clause_rewriter.hpp"
#include "duckdb/optimizer/join_filter_pushdown_optimizer.hpp"
#include "duckdb/optimizer/join_order/join_order_optimizer.hpp"
#include "duckdb/optimizer/limit_pushdown.hpp"
#include "duckdb/optimizer/regex_range_filter.hpp"
#include "duckdb/optimizer/remove_duplicate_groups.hpp"
#include "duckdb/optimizer/remove_unused_columns.hpp"
#include "duckdb/optimizer/rule/equal_or_null_simplification.hpp"
#include "duckdb/optimizer/rule/in_clause_simplification.hpp"
#include "duckdb/optimizer/rule/join_dependent_filter.hpp"
#include "duckdb/optimizer/rule/list.hpp"
#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/optimizer/topn_optimizer.hpp"
#include "duckdb/optimizer/sampling_pushdown.hpp"
#include "duckdb/optimizer/unnest_rewriter.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/planner.hpp"
namespace duckdb {
Optimizer::Optimizer(Binder &binder, ClientContext &context) : context(context), binder(binder), rewriter(context) {
 rewriter.rules.push_back(make_uniq<ConstantFoldingRule>(rewriter));
 rewriter.rules.push_back(make_uniq<DistributivityRule>(rewriter));
 rewriter.rules.push_back(make_uniq<ArithmeticSimplificationRule>(rewriter));
 rewriter.rules.push_back(make_uniq<CaseSimplificationRule>(rewriter));
 rewriter.rules.push_back(make_uniq<ConjunctionSimplificationRule>(rewriter));
 rewriter.rules.push_back(make_uniq<DatePartSimplificationRule>(rewriter));
 rewriter.rules.push_back(make_uniq<ComparisonSimplificationRule>(rewriter));
 rewriter.rules.push_back(make_uniq<InClauseSimplificationRule>(rewriter));
 rewriter.rules.push_back(make_uniq<EqualOrNullSimplification>(rewriter));
 rewriter.rules.push_back(make_uniq<MoveConstantsRule>(rewriter));
 rewriter.rules.push_back(make_uniq<LikeOptimizationRule>(rewriter));
 rewriter.rules.push_back(make_uniq<OrderedAggregateOptimizer>(rewriter));
 rewriter.rules.push_back(make_uniq<RegexOptimizationRule>(rewriter));
 rewriter.rules.push_back(make_uniq<EmptyNeedleRemovalRule>(rewriter));
 rewriter.rules.push_back(make_uniq<EnumComparisonRule>(rewriter));
 rewriter.rules.push_back(make_uniq<JoinDependentFilterRule>(rewriter));
 rewriter.rules.push_back(make_uniq<TimeStampComparison>(context, rewriter));
#ifdef DEBUG
 for (auto &rule : rewriter.rules) {
  D_ASSERT(rule->root);
 }
#endif
}
ClientContext &Optimizer::GetContext() {
 return context;
}
bool Optimizer::OptimizerDisabled(OptimizerType type) {
 auto &config = DBConfig::GetConfig(context);
 return config.options.disabled_optimizers.find(type) != config.options.disabled_optimizers.end();
}
void Optimizer::RunOptimizer(OptimizerType type, const std::function<void()> &callback) {
 if (OptimizerDisabled(type)) {
  return;
 }
 auto &profiler = QueryProfiler::Get(context);
 profiler.StartPhase(MetricsUtils::GetOptimizerMetricByType(type));
 callback();
 profiler.EndPhase();
 if (plan) {
  Verify(*plan);
 }
}
void Optimizer::Verify(LogicalOperator &op) {
 ColumnBindingResolver::Verify(op);
}
void Optimizer::RunBuiltInOptimizers() {
 switch (plan->type) {
 case LogicalOperatorType::LOGICAL_TRANSACTION:
 case LogicalOperatorType::LOGICAL_PRAGMA:
 case LogicalOperatorType::LOGICAL_SET:
 case LogicalOperatorType::LOGICAL_UPDATE_EXTENSIONS:
 case LogicalOperatorType::LOGICAL_CREATE_SECRET:
 case LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR:
  if (plan->children.empty()) {
   return;
  }
  break;
 default:
  break;
 }
 RunOptimizer(OptimizerType::EXPRESSION_REWRITER, [&]() { rewriter.VisitOperator(*plan); });
 RunOptimizer(OptimizerType::FILTER_PULLUP, [&]() {
  FilterPullup filter_pullup;
  plan = filter_pullup.Rewrite(std::move(plan));
 });
 RunOptimizer(OptimizerType::FILTER_PUSHDOWN, [&]() {
  FilterPushdown filter_pushdown(*this);
  unordered_set<idx_t> top_bindings;
  filter_pushdown.CheckMarkToSemi(*plan, top_bindings);
  plan = filter_pushdown.Rewrite(std::move(plan));
 });
 RunOptimizer(OptimizerType::CTE_FILTER_PUSHER, [&]() {
  CTEFilterPusher cte_filter_pusher(*this);
  plan = cte_filter_pusher.Optimize(std::move(plan));
 });
 RunOptimizer(OptimizerType::REGEX_RANGE, [&]() {
  RegexRangeFilter regex_opt;
  plan = regex_opt.Rewrite(std::move(plan));
 });
 RunOptimizer(OptimizerType::IN_CLAUSE, [&]() {
  InClauseRewriter ic_rewriter(context, *this);
  plan = ic_rewriter.Rewrite(std::move(plan));
 });
 RunOptimizer(OptimizerType::DELIMINATOR, [&]() {
  Deliminator deliminator;
  plan = deliminator.Optimize(std::move(plan));
 });
 RunOptimizer(OptimizerType::EMPTY_RESULT_PULLUP, [&]() {
  EmptyResultPullup empty_result_pullup;
  plan = empty_result_pullup.Optimize(std::move(plan));
 });
 RunOptimizer(OptimizerType::JOIN_ORDER, [&]() {
  JoinOrderOptimizer optimizer(context);
  plan = optimizer.Optimize(std::move(plan));
 });
 RunOptimizer(OptimizerType::UNNEST_REWRITER, [&]() {
  UnnestRewriter unnest_rewriter;
  plan = unnest_rewriter.Optimize(std::move(plan));
 });
 RunOptimizer(OptimizerType::UNUSED_COLUMNS, [&]() {
  RemoveUnusedColumns unused(binder, context, true);
  unused.VisitOperator(*plan);
 });
 RunOptimizer(OptimizerType::DUPLICATE_GROUPS, [&]() {
  RemoveDuplicateGroups remove;
  remove.VisitOperator(*plan);
 });
 RunOptimizer(OptimizerType::COMMON_SUBEXPRESSIONS, [&]() {
  CommonSubExpressionOptimizer cse_optimizer(binder);
  cse_optimizer.VisitOperator(*plan);
 });
 RunOptimizer(OptimizerType::COLUMN_LIFETIME, [&]() {
  ColumnLifetimeAnalyzer column_lifetime(*this, true);
  column_lifetime.VisitOperator(*plan);
 });
 RunOptimizer(OptimizerType::BUILD_SIDE_PROBE_SIDE, [&]() {
  BuildProbeSideOptimizer build_probe_side_optimizer(context, *plan);
  build_probe_side_optimizer.VisitOperator(*plan);
 });
 RunOptimizer(OptimizerType::LIMIT_PUSHDOWN, [&]() {
  LimitPushdown limit_pushdown;
  plan = limit_pushdown.Optimize(std::move(plan));
 });
 RunOptimizer(OptimizerType::SAMPLING_PUSHDOWN, [&]() {
  SamplingPushdown sampling_pushdown;
  plan = sampling_pushdown.Optimize(std::move(plan));
 });
 RunOptimizer(OptimizerType::TOP_N, [&]() {
  TopN topn;
  plan = topn.Optimize(std::move(plan));
 });
 column_binding_map_t<unique_ptr<BaseStatistics>> statistics_map;
 RunOptimizer(OptimizerType::STATISTICS_PROPAGATION, [&]() {
  StatisticsPropagator propagator(*this, *plan);
  propagator.PropagateStatistics(plan);
  statistics_map = propagator.GetStatisticsMap();
 });
 RunOptimizer(OptimizerType::COMMON_AGGREGATE, [&]() {
  CommonAggregateOptimizer common_aggregate(true);
  common_aggregate.VisitOperator(*plan);
 });
 RunOptimizer(OptimizerType::COLUMN_LIFETIME, [&]() {
  ColumnLifetimeAnalyzer column_lifetime(*this, true);
  column_lifetime.VisitOperator(*plan);
 });
 RunOptimizer(OptimizerType::REORDER_FILTER, [&]() {
  ExpressionHeuristics expression_heuristics(*this);
  plan = expression_heuristics.Rewrite(std::move(plan));
 });
 RunOptimizer(OptimizerType::JOIN_FILTER_PUSHDOWN, [&]() {
  JoinFilterPushdownOptimizer join_filter_pushdown(*this);
  join_filter_pushdown.VisitOperator(*plan);
 });
}
unique_ptr<LogicalOperator> Optimizer::Optimize(unique_ptr<LogicalOperator> plan_p) {
 Verify(*plan_p);
 this->plan = std::move(plan_p);
 RunBuiltInOptimizers();
 for (auto &optimizer_extension : DBConfig::GetConfig(context).optimizer_extensions) {
  RunOptimizer(OptimizerType::EXTENSION, [&]() {
   OptimizerExtensionInput input {GetContext(), *this, optimizer_extension.optimizer_info.get()};
   optimizer_extension.optimize_function(input, plan);
  });
 }
 Planner::VerifyPlan(context, plan);
 return std::move(plan);
}
}
