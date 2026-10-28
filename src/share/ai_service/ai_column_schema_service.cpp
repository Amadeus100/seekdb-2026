/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#define USING_LOG_PREFIX SHARE_SCHEMA
#include "ai_column_schema_service.h"
#include "ai_column_definition.h"
#include "share/ob_dml_sql_splicer.h"
#include "lib/string/ob_sql_string.h"

namespace oceanbase { namespace share {
using namespace common;
using namespace schema;

int persist_ai_column_metadata(ObISQLClient &client, const ObTableSchema &table,
                                const ObColumnSchemaV2 &column)
{
  int ret = OB_SUCCESS;
  if (!table.is_user_table() || !column.has_column_flag(AI_COLUMN_SCHEMA_FLAG)) { return ret; }
  const auto &info = column.get_extended_type_info();
  AIColumnDefinition definition;
  if (info.empty() || !AIColumnDefinition::decode(
      std::string(info.at(info.count() - 1).ptr(), info.at(info.count() - 1).length()), definition)) {
    return OB_INVALID_DATA;
  }
  std::string dependencies;
  for (const std::string &name : definition.dependencies) {
    const ObColumnSchemaV2 *source = table.get_column_schema(ObString(name.size(), name.data()));
    if (source == nullptr) { return OB_ERR_BAD_FIELD_ERROR; }
    if (!dependencies.empty()) { dependencies += ','; }
    dependencies += std::to_string(source->get_column_id());
  }
  std::string encoded = definition.encode();
  std::string normalized = definition.expression;
  if (!definition.system_prompt.empty()) { normalized += " /* " + definition.system_prompt + " */"; }
  auto text = [](const std::string &value) { return ObString(value.size(), value.c_str()); };
  ObDMLSqlSplicer dml;
  ObSqlString sql;
  int64_t affected = 0;
  OZ(dml.add_pk_column("table_id", table.get_table_id()));
  OZ(dml.add_pk_column("column_id", column.get_column_id()));
  OZ(dml.add_column("database_id", table.get_database_id()));
  OZ(dml.add_column("column_name", ObHexEscapeSqlStr(column.get_column_name_str())));
  OZ(dml.add_column("ai_model_name", ObHexEscapeSqlStr(text(definition.model))));
  OZ(dml.add_column("normalized_expr", ObHexEscapeSqlStr(text(normalized))));
  OZ(dml.add_column("dependency_column_ids", ObHexEscapeSqlStr(text(dependencies))));
  OZ(dml.add_column("sync_mode", "async"));
  OZ(dml.add_column("max_retries", definition.max_retries));
  OZ(dml.add_column("definition_version", definition.version()));
  OZ(dml.add_column("state_column_id", definition.state_column));
  OZ(dml.add_column("request_column_id", definition.request_column));
  OZ(dml.add_column("result_spec_column_id", definition.spec_column));
  OZ(dml.add_column("definition", ObHexEscapeSqlStr(text(encoded))));
  OZ(dml.splice_replace_sql("oceanbase.__all_ai_column", sql));
  OZ(client.write(sql.ptr(), affected));
  return ret;
}

int persist_ai_column_metadata(ObISQLClient &client, const ObTableSchema &table)
{
  int ret = OB_SUCCESS;
  for (auto it = table.column_begin(); OB_SUCC(ret) && it != table.column_end(); ++it) {
    ret = persist_ai_column_metadata(client, table, **it);
  }
  return ret;
}

int delete_ai_column_metadata(ObISQLClient &client, uint64_t table_id, uint64_t column_id)
{
  int ret = OB_SUCCESS;
  int64_t affected = 0;
  for (const char *name : {"__all_ai_column_task", "__all_ai_column"}) {
    ObSqlString sql;
    OZ(sql.append_fmt("DELETE FROM oceanbase.%s WHERE table_id = %llu", name, static_cast<unsigned long long>(table_id)));
    if (column_id != UINT64_MAX) { OZ(sql.append_fmt(" AND column_id = %llu", static_cast<unsigned long long>(column_id))); }
    OZ(client.write(sql.ptr(), affected));
  }
  return ret;
}
}} // namespace oceanbase::share
