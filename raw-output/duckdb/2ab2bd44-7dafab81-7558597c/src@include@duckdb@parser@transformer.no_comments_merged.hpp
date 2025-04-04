       
#include "duckdb/common/constants.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/parser/tokens.hpp"
#include "duckdb/parser/parsed_data/create_info.hpp"
#include "duckdb/parser/group_by_node.hpp"
#include "duckdb/parser/query_node.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/parser/query_node/cte_node.hpp"
#include "pg_definitions.hpp"
#include "nodes/parsenodes.hpp"
#include "nodes/primnodes.hpp"
namespace duckdb {
class ColumnDefinition;
class StackChecker;
struct OrderByNode;
struct CopyInfo;
struct CommonTableExpressionInfo;
struct GroupingExpressionMap;
class OnConflictInfo;
class UpdateSetInfo;
struct ParserOptions;
struct PivotColumn;
class Transformer {
 friend class StackChecker;
 struct CreatePivotEntry {
  string enum_name;
  unique_ptr<SelectNode> base;
  unique_ptr<ParsedExpression> column;
  unique_ptr<QueryNode> subquery;
 };
public:
 explicit Transformer(ParserOptions &options);
 explicit Transformer(Transformer &parent);
 ~Transformer();
 bool TransformParseTree(duckdb_libpgquery::PGList *tree, vector<unique_ptr<SQLStatement>> &statements);
 string NodetypeToString(duckdb_libpgquery::PGNodeTag type);
 idx_t ParamCount() const;
private:
 optional_ptr<Transformer> parent;
 ParserOptions &options;
 idx_t prepared_statement_parameter_index = 0;
 case_insensitive_map_t<idx_t> named_param_map;
 unordered_map<string, duckdb_libpgquery::PGWindowDef *> window_clauses;
 vector<unique_ptr<CreatePivotEntry>> pivot_entries;
 vector<CommonTableExpressionMap *> stored_cte_map;
 bool in_window_definition = false;
 void Clear();
 bool InWindowDefinition();
 Transformer &RootTransformer();
 const Transformer &RootTransformer() const;
 void SetParamCount(idx_t new_count);
 void SetNamedParam(const string &name, int32_t index);
 bool GetNamedParam(const string &name, int32_t &index);
 bool HasNamedParameters() const;
 void AddPivotEntry(string enum_name, unique_ptr<SelectNode> source, unique_ptr<ParsedExpression> column,
                    unique_ptr<QueryNode> subquery);
 unique_ptr<SQLStatement> GenerateCreateEnumStmt(unique_ptr<CreatePivotEntry> entry);
 bool HasPivotEntries();
 idx_t PivotEntryCount();
 vector<unique_ptr<CreatePivotEntry>> &GetPivotEntries();
 void PivotEntryCheck(const string &type);
 void ExtractCTEsRecursive(CommonTableExpressionMap &cte_map);
private:
 unique_ptr<SQLStatement> TransformStatement(duckdb_libpgquery::PGNode &stmt);
 unique_ptr<SQLStatement> TransformStatementInternal(duckdb_libpgquery::PGNode &stmt);
 unique_ptr<SelectStatement> TransformSelect(optional_ptr<duckdb_libpgquery::PGNode> node, bool is_select = true);
 unique_ptr<SelectStatement> TransformSelect(duckdb_libpgquery::PGSelectStmt &select, bool is_select = true);
 unique_ptr<AlterStatement> TransformAlter(duckdb_libpgquery::PGAlterTableStmt &stmt);
 unique_ptr<AlterStatement> TransformRename(duckdb_libpgquery::PGRenameStmt &stmt);
 unique_ptr<CreateStatement> TransformCreateTable(duckdb_libpgquery::PGCreateStmt &node);
 unique_ptr<CreateStatement> TransformCreateTableAs(duckdb_libpgquery::PGCreateTableAsStmt &stmt);
 unique_ptr<CreateStatement> TransformCreateSchema(duckdb_libpgquery::PGCreateSchemaStmt &stmt);
 unique_ptr<CreateStatement> TransformCreateSequence(duckdb_libpgquery::PGCreateSeqStmt &node);
 unique_ptr<CreateStatement> TransformCreateView(duckdb_libpgquery::PGViewStmt &node);
 unique_ptr<CreateStatement> TransformCreateIndex(duckdb_libpgquery::PGIndexStmt &stmt);
 unique_ptr<CreateStatement> TransformCreateFunction(duckdb_libpgquery::PGCreateFunctionStmt &stmt);
 unique_ptr<CreateStatement> TransformCreateType(duckdb_libpgquery::PGCreateTypeStmt &stmt);
 unique_ptr<CreateStatement> TransformCreateDatabase(duckdb_libpgquery::PGCreateDatabaseStmt &stmt);
 unique_ptr<AlterStatement> TransformAlterSequence(duckdb_libpgquery::PGAlterSeqStmt &stmt);
 unique_ptr<SQLStatement> TransformDrop(duckdb_libpgquery::PGDropStmt &stmt);
 unique_ptr<InsertStatement> TransformInsert(duckdb_libpgquery::PGInsertStmt &stmt);
 unique_ptr<OnConflictInfo> TransformOnConflictClause(duckdb_libpgquery::PGOnConflictClause *node,
                                                      const string &relname);
 unique_ptr<OnConflictInfo> DummyOnConflictClause(duckdb_libpgquery::PGOnConflictActionAlias type,
                                                  const string &relname);
 unique_ptr<CopyStatement> TransformCopy(duckdb_libpgquery::PGCopyStmt &stmt);
 void TransformCopyOptions(CopyInfo &info, optional_ptr<duckdb_libpgquery::PGList> options);
 unique_ptr<TransactionStatement> TransformTransaction(duckdb_libpgquery::PGTransactionStmt &stmt);
 unique_ptr<DeleteStatement> TransformDelete(duckdb_libpgquery::PGDeleteStmt &stmt);
 unique_ptr<UpdateStatement> TransformUpdate(duckdb_libpgquery::PGUpdateStmt &stmt);
 unique_ptr<SQLStatement> TransformPragma(duckdb_libpgquery::PGPragmaStmt &stmt);
 unique_ptr<ExportStatement> TransformExport(duckdb_libpgquery::PGExportStmt &stmt);
 unique_ptr<PragmaStatement> TransformImport(duckdb_libpgquery::PGImportStmt &stmt);
 unique_ptr<ExplainStatement> TransformExplain(duckdb_libpgquery::PGExplainStmt &stmt);
 unique_ptr<SQLStatement> TransformVacuum(duckdb_libpgquery::PGVacuumStmt &stmt);
 unique_ptr<SQLStatement> TransformShow(duckdb_libpgquery::PGVariableShowStmt &stmt);
 unique_ptr<ShowStatement> TransformShowSelect(duckdb_libpgquery::PGVariableShowSelectStmt &stmt);
 unique_ptr<AttachStatement> TransformAttach(duckdb_libpgquery::PGAttachStmt &stmt);
 unique_ptr<DetachStatement> TransformDetach(duckdb_libpgquery::PGDetachStmt &stmt);
 unique_ptr<SetStatement> TransformUse(duckdb_libpgquery::PGUseStmt &stmt);
 unique_ptr<PrepareStatement> TransformPrepare(duckdb_libpgquery::PGPrepareStmt &stmt);
 unique_ptr<ExecuteStatement> TransformExecute(duckdb_libpgquery::PGExecuteStmt &stmt);
 unique_ptr<CallStatement> TransformCall(duckdb_libpgquery::PGCallStmt &stmt);
 unique_ptr<DropStatement> TransformDeallocate(duckdb_libpgquery::PGDeallocateStmt &stmt);
 unique_ptr<QueryNode> TransformPivotStatement(duckdb_libpgquery::PGSelectStmt &select);
 unique_ptr<SQLStatement> CreatePivotStatement(unique_ptr<SQLStatement> statement);
 PivotColumn TransformPivotColumn(duckdb_libpgquery::PGPivot &pivot);
 vector<PivotColumn> TransformPivotList(duckdb_libpgquery::PGList &list);
 unique_ptr<SetStatement> TransformSet(duckdb_libpgquery::PGVariableSetStmt &set);
 unique_ptr<SetStatement> TransformSetVariable(duckdb_libpgquery::PGVariableSetStmt &stmt);
 unique_ptr<SetStatement> TransformResetVariable(duckdb_libpgquery::PGVariableSetStmt &stmt);
 unique_ptr<SQLStatement> TransformCheckpoint(duckdb_libpgquery::PGCheckPointStmt &stmt);
 unique_ptr<LoadStatement> TransformLoad(duckdb_libpgquery::PGLoadStmt &stmt);
 unique_ptr<QueryNode> TransformSelectNode(duckdb_libpgquery::PGSelectStmt &select);
 unique_ptr<QueryNode> TransformSelectInternal(duckdb_libpgquery::PGSelectStmt &select);
 void TransformModifiers(duckdb_libpgquery::PGSelectStmt &stmt, QueryNode &node);
 unique_ptr<ParsedExpression> TransformBoolExpr(duckdb_libpgquery::PGBoolExpr &root);
 unique_ptr<ParsedExpression> TransformCase(duckdb_libpgquery::PGCaseExpr &root);
 unique_ptr<ParsedExpression> TransformTypeCast(duckdb_libpgquery::PGTypeCast &root);
 unique_ptr<ParsedExpression> TransformCoalesce(duckdb_libpgquery::PGAExpr &root);
 unique_ptr<ParsedExpression> TransformColumnRef(duckdb_libpgquery::PGColumnRef &root);
 unique_ptr<ConstantExpression> TransformValue(duckdb_libpgquery::PGValue val);
 unique_ptr<ParsedExpression> TransformAExpr(duckdb_libpgquery::PGAExpr &root);
 unique_ptr<ParsedExpression> TransformAExprInternal(duckdb_libpgquery::PGAExpr &root);
 unique_ptr<ParsedExpression> TransformExpression(optional_ptr<duckdb_libpgquery::PGNode> node);
 unique_ptr<ParsedExpression> TransformExpression(duckdb_libpgquery::PGNode &node);
 unique_ptr<ParsedExpression> TransformFuncCall(duckdb_libpgquery::PGFuncCall &root);
 unique_ptr<ParsedExpression> TransformInterval(duckdb_libpgquery::PGIntervalConstant &root);
 unique_ptr<ParsedExpression> TransformLambda(duckdb_libpgquery::PGLambdaFunction &node);
 unique_ptr<ParsedExpression> TransformArrayAccess(duckdb_libpgquery::PGAIndirection &node);
 unique_ptr<ParsedExpression> TransformPositionalReference(duckdb_libpgquery::PGPositionalReference &node);
 unique_ptr<ParsedExpression> TransformStarExpression(duckdb_libpgquery::PGAStar &node);
 unique_ptr<ParsedExpression> TransformBooleanTest(duckdb_libpgquery::PGBooleanTest &node);
 unique_ptr<ParsedExpression> TransformConstant(duckdb_libpgquery::PGAConst &c);
 unique_ptr<ParsedExpression> TransformGroupingFunction(duckdb_libpgquery::PGGroupingFunc &n);
 unique_ptr<ParsedExpression> TransformResTarget(duckdb_libpgquery::PGResTarget &root);
 unique_ptr<ParsedExpression> TransformNullTest(duckdb_libpgquery::PGNullTest &root);
 unique_ptr<ParsedExpression> TransformParamRef(duckdb_libpgquery::PGParamRef &node);
 unique_ptr<ParsedExpression> TransformNamedArg(duckdb_libpgquery::PGNamedArgExpr &root);
 unique_ptr<ParsedExpression> TransformSQLValueFunction(duckdb_libpgquery::PGSQLValueFunction &node);
 unique_ptr<ParsedExpression> TransformSubquery(duckdb_libpgquery::PGSubLink &root);
 unique_ptr<Constraint> TransformConstraint(duckdb_libpgquery::PGListCell *cell);
 unique_ptr<Constraint> TransformConstraint(duckdb_libpgquery::PGListCell *cell, ColumnDefinition &column,
                                            idx_t index);
 unique_ptr<UpdateSetInfo> TransformUpdateSetInfo(duckdb_libpgquery::PGList *target_list,
                                                  duckdb_libpgquery::PGNode *where_clause);
 vector<unique_ptr<ParsedExpression>> TransformIndexParameters(duckdb_libpgquery::PGList &list,
                                                               const string &relation_name);
 unique_ptr<ParsedExpression> TransformCollateExpr(duckdb_libpgquery::PGCollateClause &collate);
 string TransformCollation(optional_ptr<duckdb_libpgquery::PGCollateClause> collate);
 ColumnDefinition TransformColumnDefinition(duckdb_libpgquery::PGColumnDef &cdef);
 OnCreateConflict TransformOnConflict(duckdb_libpgquery::PGOnCreateConflict conflict);
 string TransformAlias(duckdb_libpgquery::PGAlias *root, vector<string> &column_name_alias);
 vector<string> TransformStringList(duckdb_libpgquery::PGList *list);
<<<<<<< HEAD
 void TransformCTE(duckdb_libpgquery::PGWithClause *de_with_clause, CommonTableExpressionMap &cte_map,
                   vector<unique_ptr<CTENode>> *materialized_ctes);
 static unique_ptr<QueryNode> TransformMaterializedCTE(unique_ptr<QueryNode> root,
                                                       vector<unique_ptr<CTENode>> &materialized_ctes);
 unique_ptr<SelectStatement> TransformRecursiveCTE(duckdb_libpgquery::PGCommonTableExpr *node,
||||||| 7558597cf2
 void TransformCTE(duckdb_libpgquery::PGWithClause *de_with_clause, CommonTableExpressionMap &cte_map);
 unique_ptr<SelectStatement> TransformRecursiveCTE(duckdb_libpgquery::PGCommonTableExpr *node,
=======
 void TransformCTE(duckdb_libpgquery::PGWithClause &de_with_clause, CommonTableExpressionMap &cte_map);
 unique_ptr<SelectStatement> TransformRecursiveCTE(duckdb_libpgquery::PGCommonTableExpr &cte,
>>>>>>> 7dafab81
                                                   CommonTableExpressionInfo &info);
 unique_ptr<ParsedExpression> TransformUnaryOperator(const string &op, unique_ptr<ParsedExpression> child);
 unique_ptr<ParsedExpression> TransformBinaryOperator(string op, unique_ptr<ParsedExpression> left,
                                                      unique_ptr<ParsedExpression> right);
 unique_ptr<TableRef> TransformTableRefNode(duckdb_libpgquery::PGNode &n);
 unique_ptr<TableRef> TransformFrom(optional_ptr<duckdb_libpgquery::PGList> root);
 unique_ptr<TableRef> TransformRangeVar(duckdb_libpgquery::PGRangeVar &root);
 unique_ptr<TableRef> TransformRangeFunction(duckdb_libpgquery::PGRangeFunction &root);
 unique_ptr<TableRef> TransformJoin(duckdb_libpgquery::PGJoinExpr &root);
 unique_ptr<TableRef> TransformPivot(duckdb_libpgquery::PGPivotExpr &root);
 unique_ptr<TableRef> TransformRangeSubselect(duckdb_libpgquery::PGRangeSubselect &root);
 unique_ptr<TableRef> TransformValuesList(duckdb_libpgquery::PGList *list);
 QualifiedName TransformQualifiedName(duckdb_libpgquery::PGRangeVar &root);
 LogicalType TransformTypeName(duckdb_libpgquery::PGTypeName &name);
 bool TransformGroupBy(optional_ptr<duckdb_libpgquery::PGList> group, SelectNode &result);
 void TransformGroupByNode(duckdb_libpgquery::PGNode &n, GroupingExpressionMap &map, SelectNode &result,
                           vector<GroupingSet> &result_sets);
 void AddGroupByExpression(unique_ptr<ParsedExpression> expression, GroupingExpressionMap &map, GroupByNode &result,
                           vector<idx_t> &result_set);
 void TransformGroupByExpression(duckdb_libpgquery::PGNode &n, GroupingExpressionMap &map, GroupByNode &result,
                                 vector<idx_t> &result_set);
 bool TransformOrderBy(duckdb_libpgquery::PGList *order, vector<OrderByNode> &result);
 void TransformExpressionList(duckdb_libpgquery::PGList &list, vector<unique_ptr<ParsedExpression>> &result);
 void TransformWindowDef(duckdb_libpgquery::PGWindowDef &window_spec, WindowExpression &expr,
                         const char *window_name = nullptr);
 void TransformWindowFrame(duckdb_libpgquery::PGWindowDef &window_spec, WindowExpression &expr);
 unique_ptr<SampleOptions> TransformSampleOptions(optional_ptr<duckdb_libpgquery::PGNode> options);
 bool ExpressionIsEmptyStar(ParsedExpression &expr);
 OnEntryNotFound TransformOnEntryNotFound(bool missing_ok);
 Vector PGListToVector(optional_ptr<duckdb_libpgquery::PGList> column_list, idx_t &size);
 vector<string> TransformConflictTarget(duckdb_libpgquery::PGList &list);
private:
 idx_t stack_depth;
 void InitializeStackCheck();
 StackChecker StackCheck(idx_t extra_stack = 1);
public:
 template <class T>
 static T &PGCast(duckdb_libpgquery::PGNode &node) {
  return reinterpret_cast<T &>(node);
 }
 template <class T>
 static optional_ptr<T> PGPointerCast(void *ptr) {
  return optional_ptr<T>(reinterpret_cast<T *>(ptr));
 }
};
class StackChecker {
public:
 StackChecker(Transformer &transformer, idx_t stack_usage);
 ~StackChecker();
 StackChecker(StackChecker &&) noexcept;
 StackChecker(const StackChecker &) = delete;
private:
 Transformer &transformer;
 idx_t stack_usage;
};
vector<string> ReadPgListToString(duckdb_libpgquery::PGList *column_list);
}
