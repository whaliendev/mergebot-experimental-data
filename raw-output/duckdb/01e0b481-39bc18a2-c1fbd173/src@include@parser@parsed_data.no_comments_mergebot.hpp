       
#include "common/internal_types.hpp"
#include "function/function.hpp"
#include "parser/column_definition.hpp"
#include "parser/constraint.hpp"
namespace duckdb {
struct CreateTableInformation {
 std::string schema;
 std::string table;
 std::vector<ColumnDefinition> columns;
 std::vector<std::unique_ptr<Constraint>> constraints;
 bool if_not_exists = false;
 CreateTableInformation() : schema(DEFAULT_SCHEMA), if_not_exists(false) {
 }
 CreateTableInformation(std::string schema, std::string table,
                        std::vector<ColumnDefinition> columns)
     : schema(schema), table(table), columns(columns), if_not_exists(false) {
 }
};
struct DropTableInformation {
 std::string schema;
 std::string table;
 bool if_exists = false;
 bool cascade = false;
 DropTableInformation()
     : schema(DEFAULT_SCHEMA), if_exists(false), cascade(false) {
 }
};
enum class AlterType : uint8_t {
INVALID = 0, ALTER_TABLE = 1
};
struct AlterInformation {
 AlterType type;
 AlterInformation(AlterType type): type(type) { }
};
enum class AlterTableType : uint8_t {
INVALID = 0, RENAME_COLUMN = 1
};
struct AlterTableInformation : public AlterInformation {
 AlterTableType alter_table_type;
 std::string schema;
 std::string table;
 AlterTableInformation(AlterTableType type, std::string schema, std::string table): AlterInformation(AlterType::ALTER_TABLE), alter_table_type(type), schema(schema), table(table) { }
};
struct RenameColumnInformation : public AlterTableInformation {
 std::string name;
 std::string new_name;
 RenameColumnInformation(std::string schema, std::string table, std::string name, std::string new_name): AlterTableInformation(AlterTableType::RENAME_COLUMN, schema, table), name(name), new_name(new_name) {
 }
};
struct CreateSchemaInformation {
 std::string schema;
 bool if_not_exists = false;
 CreateSchemaInformation() : if_not_exists(false) {
 }
};
struct DropSchemaInformation {
 std::string schema;
 bool if_exists = false;
 bool cascade = false;
 DropSchemaInformation() : if_exists(false), cascade(false) {
 }
};
struct CreateTableFunctionInformation {
 std::string schema;
 std::string name;
 bool or_replace = false;
 std::vector<ColumnDefinition> return_values;
 std::vector<TypeId> arguments;
 table_function_init_t init;
 table_function_t function;
 table_function_final_t final;
 CreateTableFunctionInformation()
     : schema(DEFAULT_SCHEMA), or_replace(false) {
 }
};
struct DropTableFunctionInformation {
 std::string schema;
 std::string name;
 bool if_exists = false;
 bool cascade = false;
 DropTableFunctionInformation()
     : schema(DEFAULT_SCHEMA), if_exists(false), cascade(false) {
 }
};
struct CreateScalarFunctionInformation {
 std::string schema;
 std::string name;
 bool or_replace = false;
 scalar_function_t function;
 matches_argument_function_t matches;
 get_return_type_function_t return_type;
 CreateScalarFunctionInformation()
     : schema(DEFAULT_SCHEMA), or_replace(false) {
 }
};
struct CreateIndexInformation {
 std::string schema;
 std::string table;
    std::vector<std::string> indexed_columns;
    IndexType index_type;
    std::string index_name;
    bool unique = false;
 bool if_not_exists = false;
 CreateIndexInformation() : schema(DEFAULT_SCHEMA), if_not_exists(false) {
 }
 CreateIndexInformation(std::string schema, std::string table, std::vector<std::string> indexed_columns, IndexType index_type, std::string index_name, bool unique): schema(schema), table(table), indexed_columns(indexed_columns), index_type(index_type), index_name(index_name), if_not_exists(false) {
 }
};
}
