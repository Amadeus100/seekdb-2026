/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#define USING_LOG_PREFIX SQL_ENG
#include "ai_column.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "query/session/ob_inner_sql_connection_access.h"

namespace oceanbase { namespace sql {
using namespace common;
using namespace common::sqlclient;
using namespace share;
using namespace share::schema;
namespace {
thread_local bool ai_internal_access = false;
thread_local uint64_t ai_trigger_compile_table = OB_INVALID_ID;
struct AIColumnTarget {
  const ObColumnSchemaV2 *column = nullptr;
  AIColumnDefinition definition;
  std::string state, request, spec;
};
std::string str(const ObString &s) { return std::string(s.ptr(), s.length()); }
std::string ref(const std::string &qualifier, const std::string &column) { return qualifier + "." + ai_quote_identifier(column); }
std::string rowkey_expression(const ObTableSchema &table, const std::string &qualifier)
{
  std::string sql = "CONCAT(";
  const auto &key = table.get_rowkey_info();
  for (int64_t i = 0; i < key.get_size(); ++i) {
    const ObRowkeyColumn *part = key.get_column(i);
    const ObColumnSchemaV2 *column = part == nullptr ? nullptr : table.get_column_schema(part->column_id_);
    if (column == nullptr) { return {}; }
    if (i) { sql += ',' + ai_sql_literal(" AND ") + ','; }
    std::string name = str(column->get_column_name_str());
    sql += ai_sql_literal(ai_quote_identifier(name) + " = ") + ',';
    if (ob_is_integer_type(column->get_data_type())) {
      sql += "CAST(" + ref(qualifier, name) + " AS CHAR)";
    } else {
      sql += "CONCAT('CONVERT(0x',HEX(" + ref(qualifier, name) + "),' USING utf8mb4)')";
    }
  }
  return key.get_size() > 0 ? sql + ')' : std::string();
}
std::string different(const std::string &name)
{
  return "NOT(CAST(" + ref("NEW",name) + " AS BINARY)<=>CAST(" + ref("OLD",name) + " AS BINARY))";
}
int execute_sql(ObSQLSessionInfo &session, const std::string &sql)
{
  int ret = OB_SUCCESS;
  ObISQLConnectionGuard connection;
  AIColumnInternalAccessGuard internal;
  if (OB_FAIL(query::ObInnerSQLConnectionAccess::create_connection_with_external_session(&session, connection))) {
  } else if (connection.get_ptr() == nullptr) { ret = OB_ERR_UNEXPECTED;
  } else {
    int64_t affected = 0;
    ret = connection->execute_write(ObString(sql.size(), sql.data()), affected, true);
  }
  return ret;
}
std::string enqueue(const ObTableSchema &table, const AIColumnTarget &target,
                    const std::string &key, const std::string &qualifier)
{
  const auto &d = target.definition;
  const std::string input = ai_qualify_expression(d.input, d.dependencies, qualifier);
  const std::string config = ai_qualify_expression(d.config, d.dependencies, qualifier);
  return "INSERT INTO oceanbase.__all_ai_column_task "
      "(table_id,request_id,column_id,definition_version,rowkey_predicate,input_content,config_content,state,retry_count,eligible_ts) VALUES("
      + std::to_string(table.get_table_id()) + ',' + ref(qualifier, target.request) + ','
      + std::to_string(target.column->get_column_id()) + ',' + std::to_string(d.version()) + ',' + key
      + ",CAST((" + input + ") AS CHAR),CAST((" + config + ") AS CHAR),0,0,NOW(6));";
}
} // namespace
bool is_ai_column_internal_access() { return ai_internal_access; }
uint64_t ai_column_trigger_compile_table() { return ai_trigger_compile_table; }
AIColumnTriggerCompileGuard::AIColumnTriggerCompileGuard(uint64_t table_id)
    : previous_(ai_trigger_compile_table) { ai_trigger_compile_table = table_id; }
AIColumnTriggerCompileGuard::~AIColumnTriggerCompileGuard() { ai_trigger_compile_table = previous_; }
AIColumnInternalAccessGuard::AIColumnInternalAccessGuard() : previous_(ai_internal_access) { ai_internal_access = true; }
AIColumnInternalAccessGuard::~AIColumnInternalAccessGuard() { ai_internal_access = previous_; }

int install_ai_column_triggers(ObExecContext &ctx, uint64_t table_id, bool backfill)
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  const ObDatabaseSchema *database = nullptr;
  std::vector<AIColumnTarget> columns;
  if (GCTX.schema_service_ == nullptr || ctx.get_my_session() == nullptr) { return OB_NOT_INIT; }
  OZ(GCTX.schema_service_->get_runtime_schema_guard(guard));
  OZ(guard.get_table_schema(table_id, table));
  if (OB_SUCC(ret) && table == nullptr) { return OB_ERR_UNEXPECTED; }
  if (OB_FAIL(ret)) { return ret; }
  for (auto it = table->column_begin(); it != table->column_end(); ++it) {
    AIColumnTarget target;
    if (get_ai_column_definition(**it, target.definition)) {
      target.column = *it;
      for (int i = 0; i < 3; ++i) {
        uint64_t id = i == 0 ? target.definition.state_column : i == 1 ? target.definition.request_column : target.definition.spec_column;
        const ObColumnSchemaV2 *hidden = table->get_column_schema(id);
        if (hidden == nullptr || !hidden->is_hidden()) { return OB_INVALID_DATA; }
        (i == 0 ? target.state : i == 1 ? target.request : target.spec) = str(hidden->get_column_name_str());
      }
      columns.push_back(std::move(target));
    }
  }
  if (columns.empty()) { return OB_SUCCESS; }
  if (columns.size() > 64) { return OB_SIZE_OVERFLOW; }
  OZ(guard.get_database_schema(table->get_database_id(), database));
  if (OB_SUCC(ret) && database == nullptr) { return OB_ERR_UNEXPECTED; }
  if (OB_FAIL(ret)) { return ret; }
  const std::string db = ai_quote_identifier(str(database->get_database_name_str()));
  const std::string name = db + "." + ai_quote_identifier(str(table->get_table_name_str()));
  const std::string old_key = rowkey_expression(*table, "OLD"), new_key = rowkey_expression(*table, "NEW");
  if (old_key.empty() || new_key.empty()) {
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN requires a stored row identity");
    return OB_NOT_SUPPORTED;
  }
  std::string key_changed = "FALSE";
  for (int64_t i = 0; i < table->get_rowkey_info().get_size(); ++i) {
    const auto *part = table->get_rowkey_info().get_column(i);
    const auto *column = table->get_column_schema(part->column_id_);
    key_changed += " OR " + different(str(column->get_column_name_str()));
  }
  std::string before_insert = "BEGIN DECLARE ai_generation VARCHAR(36); SET ai_generation=UUID();";
  std::string after_insert = "BEGIN ";
  std::string before_update = before_insert;
  std::string after_update = "BEGIN ";
  for (const auto &column : columns) {
    const std::string result = str(column.column->get_column_name_str());
    const std::string invalidate = "SET " + ref("NEW",result) + "=NULL,"
        + ref("NEW",column.state) + "='PENDING'," + ref("NEW",column.request) + "=ai_generation,"
        + ref("NEW",column.spec) + '=' + ai_sql_literal(std::to_string(column.definition.version())) + ';';
    before_insert += invalidate;
    after_insert += enqueue(*table, column, new_key, "NEW");
    std::string changed = key_changed;
    for (const auto &dependency : column.definition.dependencies) { changed += " OR " + different(dependency); }
    changed += " OR (" + different(column.request) + " AND " + ref("NEW",column.state) + "='PENDING')";
    before_update += "IF " + changed + " THEN " + invalidate + " END IF;";
    after_update += "IF " + different(column.request) + " THEN DELETE FROM oceanbase.__all_ai_column_task WHERE table_id="
        + std::to_string(table_id) + " AND column_id=" + std::to_string(column.column->get_column_id())
        + " AND rowkey_predicate=" + old_key + ';' + enqueue(*table,column,new_key,"NEW") + " END IF;";
  }
  before_insert += "END"; after_insert += "END"; before_update += "END"; after_update += "END";
  const std::string after_delete = "BEGIN DELETE FROM oceanbase.__all_ai_column_task WHERE table_id="
      + std::to_string(table_id) + " AND rowkey_predicate=" + old_key + "; END";
  const std::vector<std::pair<std::string,std::string>> triggers = {
    {"BEFORE INSERT",before_insert},{"AFTER INSERT",after_insert},
    {"BEFORE UPDATE",before_update},{"AFTER UPDATE",after_update},{"AFTER DELETE",after_delete}};
  for (size_t i = 0; OB_SUCC(ret) && i < triggers.size(); ++i) {
    const std::string trigger = db + "." + ai_quote_identifier("__seekdb_ai_" + std::to_string(table_id) + '_' + std::to_string(i));
    OZ(execute_sql(*ctx.get_my_session(), "DROP TRIGGER IF EXISTS " + trigger));
    OZ(execute_sql(*ctx.get_my_session(), "CREATE TRIGGER " + trigger + ' ' + triggers[i].first
        + " ON " + name + " FOR EACH ROW " + triggers[i].second));
  }
  if (OB_SUCC(ret) && backfill) {
    std::string sql = "UPDATE " + name + " SET ";
    std::string pending;
    bool first = true;
    for (const auto &column : columns) {
      if (!first) { sql += ','; } first = false;
      const std::string changed = "NOT(" + ai_quote_identifier(column.spec) + "<=>"
          + ai_sql_literal(std::to_string(column.definition.version())) + ")";
      sql += ai_quote_identifier(column.state) + "=IF(" + changed + ",'PENDING',"
          + ai_quote_identifier(column.state) + ")," + ai_quote_identifier(column.request)
          + "=IF(" + changed + ",UUID()," + ai_quote_identifier(column.request) + ")";
      if (!pending.empty()) { pending += " OR "; }
      pending += changed;
    }
    sql += " WHERE " + pending;
    ret = execute_sql(*ctx.get_my_session(), sql);
  }
  return ret;
}

int check_ai_column_strict_read(ObSQLSessionInfo &session, const ObTableSchema &table,
                                const ObColumnSchemaV2 &column)
{
  if (!session.is_user_session() || is_ai_column_internal_access()) { return OB_SUCCESS; }
  AIColumnDefinition definition;
  if (!get_ai_column_definition(column, definition)) { return OB_SUCCESS; }
  int ret = OB_SUCCESS;
  bool enabled = false;
  OZ(session.get_sys_variable(share::SYS_VAR_AI_COLUMN_STRICT_READ, enabled));
  if (OB_FAIL(ret) || !enabled) { return ret; }
  ObSchemaGetterGuard schema_guard;
  const ObDatabaseSchema *database = nullptr;
  const ObColumnSchemaV2 *state = table.get_column_schema(definition.state_column);
  if (state == nullptr || GCTX.schema_service_ == nullptr) { return OB_ERR_UNEXPECTED; }
  OZ(GCTX.schema_service_->get_runtime_schema_guard(schema_guard));
  OZ(schema_guard.get_database_schema(table.get_database_id(), database));
  if (OB_SUCC(ret) && database == nullptr) { return OB_ERR_UNEXPECTED; }
  if (OB_FAIL(ret)) { return ret; }
  const std::string sql = "SELECT 1 AS pending FROM " + ai_quote_identifier(str(database->get_database_name_str()))
      + '.' + ai_quote_identifier(str(table.get_table_name_str())) + " WHERE NOT("
      + ai_quote_identifier(str(state->get_column_name_str())) + "<=>'READY') LIMIT 1";
  ObISQLConnectionGuard connection;
  AIColumnInternalAccessGuard internal;
  OZ(query::ObInnerSQLConnectionAccess::create_connection_with_external_session(&session, connection));
  if (OB_FAIL(ret)) { return ret; }
  SMART_VAR(ObMySQLProxy::MySQLResult, result) {
    if (OB_FAIL(connection->execute_read(ObString(sql.size(),sql.data()), result))) {
    } else if (result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED;
    } else {
      const int next = result.get_result()->next();
      if (next == OB_SUCCESS) {
        ret = OB_AI_COLUMN_NOT_READY;
        LOG_USER_ERROR(OB_AI_COLUMN_NOT_READY, table.get_table_id(), column.get_column_id());
      } else if (next != OB_ITER_END) { ret = next; }
    }
  }
  return ret;
}
int check_ai_column_assignment(ObSQLSessionInfo &session, const ObTableSchema &table, uint64_t column_id)
{
  if (!session.is_user_session() || is_ai_column_internal_access() || !table.is_user_table()) { return OB_SUCCESS; }
  const ObColumnSchemaV2 *column = table.get_column_schema(column_id);
  if (column == nullptr) { return OB_ERR_UNEXPECTED; }
  if (session.is_user_session() && !is_ai_column_internal_access()
      && (column->has_column_flag(AI_COLUMN_SCHEMA_FLAG)
          || (column->is_hidden() && column->get_column_name_str().prefix_match("__ai_")))) {
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "direct assignment to an AI COLUMN or its internal state");
    return OB_NOT_SUPPORTED;
  }
  return OB_SUCCESS;
}
}} // namespace oceanbase::sql
