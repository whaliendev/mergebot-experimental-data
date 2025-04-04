#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/tableref/bound_subqueryref.hpp"

namespace duckdb {

unique_ptr<BoundTableRef> Binder::Bind(SubqueryRef &ref, CommonTableExpressionInfo *cte) {
	auto binder = Binder::CreateBinder(context, this);
	binder->can_contain_nulls = true;
	if (cte) {
		binder->bound_ctes.insert(cte);
	}
	binder->alias = ref.alias.empty() ? "unnamed_subquery" : ref.alias;
	auto subquery = binder->BindNode(*ref.subquery->node);
	idx_t bind_index = subquery->GetRootIndex();
	string subquery_alias;
	if (ref.alias.empty()) {
		subquery_alias = "unnamed_subquery" + to_string(bind_index);
	} else {
		subquery_alias = ref.alias;
	}
<<<<<<< HEAD
	auto result = make_unique<BoundSubqueryRef>(std::move(binder), std::move(subquery));
	bind_context.AddSubquery(bind_index, subquery_alias, ref, *result->subquery);
||||||| 4161f39ca1
	auto result = make_unique<BoundSubqueryRef>(std::move(binder), std::move(subquery));
	bind_context.AddSubquery(bind_index, alias, ref, *result->subquery);
=======
	auto result = make_uniq<BoundSubqueryRef>(std::move(binder), std::move(subquery));
	bind_context.AddSubquery(bind_index, alias, ref, *result->subquery);
>>>>>>> d84e329b
	MoveCorrelatedExpressions(*result->binder);
	return std::move(result);
}

} // namespace duckdb
