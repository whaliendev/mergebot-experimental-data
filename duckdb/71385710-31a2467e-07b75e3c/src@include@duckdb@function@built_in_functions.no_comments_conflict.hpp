       
#include "duckdb/function/function.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
namespace duckdb {
class BuiltinFunctions {
public:
 BuiltinFunctions(CatalogTransaction transaction, Catalog &catalog);
 ~BuiltinFunctions();
 void Initialize();
public:
 void AddFunction(AggregateFunctionSet set);
 void AddFunction(AggregateFunction function);
 void AddFunction(ScalarFunctionSet set);
 void AddFunction(PragmaFunction function);
 void AddFunction(const string &name, PragmaFunctionSet functions);
 void AddFunction(ScalarFunction function);
 void AddFunction(const vector<string> &names, ScalarFunction function);
 void AddFunction(TableFunctionSet set);
 void AddFunction(TableFunction function);
 void AddFunction(CopyFunction function);
 void AddCollation(string name, ScalarFunction function, bool combinable = false,
                   bool not_required_for_equality = false);
private:
 CatalogTransaction transaction;
 Catalog &catalog;
private:
 template <class T>
 void Register() {
  T::RegisterFunction(*this);
 }
 void RegisterTableScanFunctions();
 void RegisterSQLiteFunctions();
 void RegisterReadFunctions();
 void RegisterTableFunctions();
 void RegisterArrowFunctions();
 void RegisterSnifferFunction();
 void RegisterDistributiveAggregates();
<<<<<<< HEAD
||||||| 07b75e3c3a
 void RegisterCompressedMaterializationFunctions();
 void RegisterOperators();
=======
 void RegisterOperators();
>>>>>>> 31a2467e
 void RegisterNestedFunctions();
 void RegisterSequenceFunctions();
 void RegisterExtensionOverloads();
 void RegisterPragmaFunctions();
 void AddExtensionFunction(ScalarFunctionSet set);
};
}
