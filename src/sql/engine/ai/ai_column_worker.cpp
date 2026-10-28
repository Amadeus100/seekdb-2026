/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#define USING_LOG_PREFIX SQL_ENG
#include "ai_column.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_client.h"
#include "sql/engine/basic/file_scan_utils.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "share/ob_server_struct.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "lib/thread/threads.h"
#include "lib/worker.h"
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cfloat>
#include <cerrno>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace oceanbase { namespace sql {
using namespace common;
using namespace share;
using namespace common::sqlclient;
namespace {
constexpr const char *TASK_TABLE = "oceanbase.__all_ai_column_task";
struct AIColumnTask {
  uint64_t table = 0, column = 0;
  std::string request, predicate, input, config, result, table_name, column_name;
  AIColumnDefinition definition;
  int64_t state = 0, retries = 0;
  bool input_null = false, result_null = true, eligible = false, leased = false;
};
using Tasks = std::vector<AIColumnTask>;
std::atomic<uint64_t> owner_sequence{0};
std::mutex group_claim_mutex;
std::string task_filter(uint64_t table, const std::string &request)
{
  return "table_id = " + std::to_string(table) + " AND request_id = " + ai_sql_literal(request);
}
int write_sql(ObISQLClient &client, const std::string &sql, int64_t *affected = nullptr)
{
  int64_t count = 0;
  int ret = client.write(sql.c_str(), count);
  if (affected) { *affected = count; }
  return ret;
}
int string_field(ObMySQLResult &row, const char *name, std::string &text, bool *is_null = nullptr)
{
  ObString value;
  int ret = row.get_varchar(name, value);
  if (is_null) { *is_null = ret == OB_ERR_NULL_VALUE; }
  if (ret == OB_ERR_NULL_VALUE) { text.clear(); ret = OB_SUCCESS; }
  else if (ret == OB_SUCCESS) { text.assign(value.ptr(), value.length()); }
  return ret;
}
int id_field(ObMySQLResult &row, const char *name, uint64_t &value)
{
  int64_t stored = 0;
  const int ret = row.get_int(name, stored);
  if (ret != OB_SUCCESS) { return ret; }
  if (stored < 0) { return OB_INVALID_DATA; }
  value = static_cast<uint64_t>(stored);
  return OB_SUCCESS;
}
int read_tasks(ObISQLClient &client, uint64_t table, const std::string &request, Tasks &tasks,
               bool for_update = false)
{
  int ret = OB_SUCCESS;
  std::string sql = "SELECT q.*, a.definition, a.column_name, t.table_name, d.database_name, "
      "q.eligible_ts <= NOW(6) AS eligible, "
      "(q.state = 1 AND q.lease_until > NOW(6)) AS leased "
      "FROM oceanbase.__all_ai_column_task q JOIN oceanbase.__all_ai_column a "
      "ON q.table_id=a.table_id AND q.column_id=a.column_id "
      "AND q.definition_version=a.definition_version "
      "JOIN oceanbase.__all_table t ON t.table_id=q.table_id "
      "JOIN oceanbase.__all_database d ON d.database_id=t.database_id "
      "WHERE q." + task_filter(table, request) + " ORDER BY q.column_id";
  if (for_update) { sql += " FOR UPDATE"; }
  SMART_VAR(ObMySQLProxy::MySQLResult, result) {
    if (OB_FAIL(client.read(result, sql.c_str()))) {
    } else if (result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED;
    } else {
      ObMySQLResult &row = *result.get_result();
      int next = OB_SUCCESS;
      while (OB_SUCC(ret) && (next = row.next()) == OB_SUCCESS) {
        if (tasks.size() == 64) { ret = OB_SIZE_OVERFLOW; break; }
        AIColumnTask task;
        std::string definition, database, table_name;
        task.table = table; task.request = request;
        OZ(id_field(row, "column_id", task.column));
        OZ(row.get_int("state", task.state));
        OZ(row.get_int("retry_count", task.retries));
        OZ(row.get_bool("eligible", task.eligible));
        OZ(row.get_bool("leased", task.leased));
        OZ(string_field(row, "rowkey_predicate", task.predicate));
        OZ(string_field(row, "input_content", task.input, &task.input_null));
        OZ(string_field(row, "config_content", task.config));
        OZ(string_field(row, "result_content", task.result, &task.result_null));
        OZ(string_field(row, "definition", definition));
        OZ(string_field(row, "column_name", task.column_name));
        OZ(string_field(row, "database_name", database));
        OZ(string_field(row, "table_name", table_name));
        if (OB_SUCC(ret) && !AIColumnDefinition::decode(definition, task.definition)) { ret = OB_INVALID_DATA; }
        if (OB_SUCC(ret)) {
          task.table_name = ai_quote_identifier(database) + "." + ai_quote_identifier(table_name);
          tasks.push_back(std::move(task));
        }
      }
      if (OB_SUCC(ret) && next != OB_ITER_END) { ret = next; }
    }
  }
  return ret;
}
int select_group(uint64_t &table, std::string &request)
{
  int ret = OB_SUCCESS;
  constexpr const char *sql =
      "SELECT table_id,request_id FROM oceanbase.__all_ai_column_task "
      "GROUP BY table_id,request_id HAVING MIN(eligible_ts)<=NOW(6) "
      "AND SUM(state=1 AND lease_until>NOW(6))=0 "
      "AND (SUM(state NOT IN (2,4))=0 "
      "OR SUM((state IN (0,3) AND eligible_ts<=NOW(6)) OR state=1)>0) "
      "ORDER BY MIN(eligible_ts),table_id,request_id LIMIT 1";
  SMART_VAR(ObMySQLProxy::MySQLResult, result) {
    if (OB_FAIL(GCTX.sql_proxy_->read(result, sql))) {
    } else if (result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(result.get_result()->next())) {
    } else if (OB_FAIL(id_field(*result.get_result(), "table_id", table))) {
    } else { ret = string_field(*result.get_result(), "request_id", request); }
  }
  return ret;
}
int typed_result(const AIColumnDefinition &definition, const std::string &text, std::string &literal)
{
  int ret = OB_SUCCESS;
  const ObObjType type = static_cast<ObObjType>(definition.result_type);
  if (definition.embedding) {
    rapidjson::Document d; d.Parse(text.data(), text.size());
    if (d.HasParseError() || !d.IsArray() || d.Size() != definition.dimension) { return OB_INVALID_DATA; }
    for (auto &value : d.GetArray()) {
      if (!value.IsNumber() || !std::isfinite(value.GetDouble()) || std::fabs(value.GetDouble()) > FLT_MAX) { return OB_INVALID_DATA; }
    }
    // A validated numeric JSON array is also a native seekdb array literal.
    // Assignment supplies the destination VECTOR dimension and index type.
    literal = text;
  } else if (type == ObJsonType) {
    rapidjson::Document d; d.Parse(text.data(), text.size());
    if (d.HasParseError()) { return OB_INVALID_DATA; }
    literal = "CAST(" + ai_sql_literal(text) + " AS JSON)";
  } else if (ob_is_integer_type(type)) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { return OB_INVALID_DATA; }
    std::string number = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    if (number[0] == '+') { number.erase(0, 1); }
    if (ob_is_unsigned_type(type)) {
      uint64_t value = 0;
      auto parsed = std::from_chars(number.data(), number.data() + number.size(), value);
      const int bits = type == ObUTinyIntType ? 8 : type == ObUSmallIntType ? 16 : type == ObUMediumIntType ? 24 : type == ObUInt32Type ? 32 : 64;
      if (parsed.ec != std::errc() || parsed.ptr != number.data() + number.size()
          || (bits < 64 && value >= (UINT64_C(1) << bits))) { return OB_INVALID_DATA; }
      literal = number;
      return ret;
    }
    int64_t value = 0;
    auto parsed = std::from_chars(number.data(), number.data() + number.size(), value);
    if (parsed.ec != std::errc() || parsed.ptr != number.data() + number.size()) { return OB_INVALID_DATA; }
    const int bits = type == ObTinyIntType ? 8 : type == ObSmallIntType ? 16 : type == ObMediumIntType ? 24 : type == ObInt32Type ? 32 : 64;
    if (bits != 64 && (value < -(INT64_C(1) << (bits - 1)) || value >= (INT64_C(1) << (bits - 1)))) { return OB_INVALID_DATA; }
    literal = number;
  } else if (ob_is_numeric_type(type)) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { return OB_INVALID_DATA; }
    const std::string number = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    char *end = nullptr; errno = 0;
    const double value = std::strtod(number.c_str(), &end);
    if (errno == ERANGE || end != number.c_str() + number.size() || !std::isfinite(value)
        || ((type == ObFloatType || type == ObUFloatType) && std::fabs(value) > FLT_MAX)
        || ((type == ObUFloatType || type == ObUDoubleType || type == ObUNumberType) && value < 0)) { return OB_INVALID_DATA; }
    literal = ai_sql_literal(number);
  } else if (type == ObDateType || type == ObDateTimeType) {
    int64_t micros = 0; bool date = false;
    if (!file_sql::FileScan::parse_time(text, micros, date) || (type == ObDateType && !date)) { return OB_INVALID_DATA; }
    literal = ai_sql_literal(text);
  } else {
    if (definition.result_length > 0 && !ob_is_text_tc(type)
        && text.size() > static_cast<uint64_t>(definition.result_length)) { return OB_SIZE_OVERFLOW; }
    literal = ai_sql_literal(text);
  }
  return ret;
}
int invoke_model(AIColumnTask &task)
{
  if (task.input_null) { task.result_null = true; return OB_SUCCESS; }
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AIColumn");
  ObAIFuncExprInfo *info = nullptr;
  ObAiModelEndpointInfo endpoint;
  query::ObIAiEndpointResolver *resolver = share::server_service<query::ObIAiEndpointResolver>();
  ObString model_name(task.definition.model.size(), task.definition.model.data());
  ObString input(task.input.size(), task.input.data());
  ObJsonObject *config = nullptr;
  if (!task.config.empty() && task.config != "NULL") {
    ObString config_text(task.config.size(), task.config.data());
    ret = ObAIFuncJsonUtils::get_json_object_form_str(allocator, config_text, config);
  }
  if (OB_SUCC(ret) && config != nullptr) {
    for (const char *key : {"messages", "model", "prompt", "input", "system", "stream"}) {
      if (config->get_value(ObString(key)) != nullptr) { ret = OB_INVALID_ARGUMENT; break; }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (resolver == nullptr) { ret = OB_NOT_INIT;
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_name, info))) {
  } else if (OB_FAIL(resolver->resolve_by_model_name(model_name, allocator, endpoint, false))) {
  } else {
    if (task.definition.input_json) {
      ObJsonObject *prompt = nullptr;
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_object_form_str(allocator, input, prompt))) {
      } else { ret = ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt, input); }
    }
    ObString output;
    if (OB_FAIL(ret)) {
    } else if (task.definition.embedding) {
      if (config == nullptr) { ret = ObAIFuncJsonUtils::get_json_object(allocator, config); }
      ObJsonInt *dimension = nullptr;
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, task.definition.dimension, dimension))) {
      } else if (OB_FAIL(config->add("dimensions", dimension))) {
      } else {
        ObAIFuncModel model(allocator, *info, endpoint);
        ret = model.call_dense_embedding(input, config, output);
      }
    } else {
      ObAIFuncIComplete *provider = nullptr;
      ObArray<ObString> headers;
      ObJsonObject *body = nullptr, *response = nullptr;
      ObIJsonBase *value = nullptr;
      ObString key;
      ObString system(task.definition.system_prompt.size(), task.definition.system_prompt.data());
      ObString request_model = endpoint.get_request_model_name().empty() ? info->model_ : endpoint.get_request_model_name();
      ObAIFuncClient client;
      if (OB_FAIL(ObAIFuncUtils::get_complete_provider(allocator, endpoint.get_provider(), provider))) {
      } else if (OB_FAIL(endpoint.get_unencrypted_access_key(allocator, key))) {
      } else if (OB_FAIL(provider->get_header(allocator, key, headers))) {
      } else if (OB_FAIL(provider->get_body(allocator, request_model, system, input, config, body))) {
      } else if (OB_FAIL(client.send_post(allocator, endpoint.get_url(), headers, body, response))) {
      } else if (OB_FAIL(provider->parse_output(allocator, response, value))) {
      } else if (value == nullptr || value->json_type() != ObJsonNodeType::J_STRING || value->get_data_length() == 0) {
        ret = OB_INVALID_DATA;
      } else { output.assign_ptr(value->get_data(), value->get_data_length()); }
    }
    if (OB_SUCC(ret)) {
      task.result.assign(output.ptr(), output.length()); task.result_null = false;
      std::string literal;
      ret = typed_result(task.definition, task.result, literal);
    }
  }
  return ret;
}

class AIColumnWorker : public lib::Threads {
public:
  AIColumnWorker() : lib::Threads(4) {}
private:
  int process_group(uint64_t table, const std::string &request) {
    int ret = OB_SUCCESS;
    const std::string filter = task_filter(table, request);
    const std::string owner = std::to_string(ObTimeUtility::current_time()) + ":" + std::to_string(owner_sequence.fetch_add(1));
    Tasks tasks;
    // Serialize short local claims, while the model calls remain concurrent.
    // Durable row locks and leases still protect claims across recovery.
    std::unique_lock<std::mutex> local_claim(group_claim_mutex);
    ObMySQLTransaction claim;
    // A short transaction serializes group claims. No row lock is held across
    // network calls, and only committed business tasks are visible here.
    OZ(claim.start(GCTX.sql_proxy_));
    if (OB_SUCC(ret)) {
      SMART_VAR(ObMySQLProxy::MySQLResult, result) {
        std::string lock = "SELECT column_id FROM " + std::string(TASK_TABLE) + " WHERE " + filter + " FOR UPDATE";
        if (OB_FAIL(claim.read(result, lock.c_str()))) {
        } else if (result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED;
        } else {
          int next = OB_SUCCESS;
          while ((next = result.get_result()->next()) == OB_SUCCESS) {}
          if (next != OB_ITER_END) { ret = next; }
        }
      }
    }
    OZ(read_tasks(claim, table, request, tasks));
    for (const auto &task : tasks) { if (task.leased) { ret = OB_EAGAIN; break; } }
    if (OB_SUCC(ret)) {
      std::string sql = "UPDATE " + std::string(TASK_TABLE) + " SET state=1,lease_owner=" + ai_sql_literal(owner)
          + ",lease_until=DATE_ADD(NOW(6),INTERVAL 120 SECOND) WHERE " + filter
          + " AND (state=1 OR (state IN (0,3) AND eligible_ts<=NOW(6)))";
      ret = write_sql(claim, sql);
    }
    if (claim.is_started()) { const int ended = claim.end(ret == OB_SUCCESS); if (ret == OB_SUCCESS) { ret = ended; } }
    local_claim.unlock();
    if (OB_FAIL(ret)) { return ret; }
    if (tasks.empty()) {
      // Definitions removed or replaced by DDL invalidate old task ownership.
      return write_sql(*GCTX.sql_proxy_, "DELETE FROM " + std::string(TASK_TABLE) + " WHERE " + filter);
    }
    for (auto &task : tasks) {
      if (has_set_stop()) { return OB_CANCELED; }
      if (task.state == 2 || task.state == 4 || (task.state == 3 && !task.eligible)) { continue; }
      const std::string owned = filter + " AND column_id=" + std::to_string(task.column)
          + " AND state=1 AND lease_owner=" + ai_sql_literal(owner);
      int64_t affected = 0;
      OZ(write_sql(*GCTX.sql_proxy_, "UPDATE " + std::string(TASK_TABLE)
          + " SET lease_until=GREATEST(DATE_ADD(NOW(6),INTERVAL 120 SECOND),"
            "DATE_ADD(lease_until,INTERVAL 1 MICROSECOND)) WHERE " + owned, &affected));
      if (OB_FAIL(ret)) { return ret; }
      if (affected == 0) { continue; }
      const int model_ret = invoke_model(task);
      const bool retry = model_ret != OB_SUCCESS && task.retries < task.definition.max_retries;
      std::string sql = "UPDATE " + std::string(TASK_TABLE) + " SET state="
          + std::to_string(model_ret == OB_SUCCESS ? 2 : retry ? 3 : 4)
          + ",retry_count=retry_count+" + std::string(retry ? "1" : "0")
          + ",last_error=" + std::to_string(model_ret) + ",lease_owner=NULL,lease_until=NULL"
          + ",eligible_ts=DATE_ADD(NOW(6),INTERVAL " + std::string(retry ? "1" : "0") + " SECOND)"
          + ",result_content=" + (model_ret != OB_SUCCESS || task.result_null ? "NULL" : ai_sql_literal(task.result))
          + " WHERE " + owned;
      OZ(write_sql(*GCTX.sql_proxy_, sql));
      OZ(write_sql(*GCTX.sql_proxy_, "UPDATE " + std::string(TASK_TABLE)
          + " SET lease_until=DATE_ADD(NOW(6),INTERVAL 120 SECOND) WHERE " + filter
          + " AND state=1 AND lease_owner=" + ai_sql_literal(owner)));
      if (OB_FAIL(ret)) { return ret; }
    }
    return publish(table, request);
  }
  int publish(uint64_t table, const std::string &request) {
    int ret = OB_SUCCESS;
    std::lock_guard<std::mutex> local_publish(group_claim_mutex);
    Tasks tasks;
    OZ(read_tasks(*GCTX.sql_proxy_, table, request, tasks));
    if (OB_FAIL(ret) || tasks.empty()) { return ret; }
    for (const auto &task : tasks) { if (task.state != 2 && task.state != 4) { return OB_SUCCESS; } }
    std::string sql = "UPDATE " + tasks[0].table_name + " SET ";
    std::string pending;
    bool first = true;
    for (const auto &task : tasks) {
      if (task.table_name != tasks[0].table_name || task.predicate != tasks[0].predicate) { return OB_INVALID_DATA; }
      std::string value = "NULL";
      if (task.state == 2 && !task.result_null) { OZ(typed_result(task.definition, task.result, value)); }
      const std::string column = ai_quote_identifier(task.column_name);
      const std::string state = ai_quote_identifier("__ai_state_" + std::to_string(task.column));
      const std::string guard = ai_quote_identifier("__ai_request_" + std::to_string(task.column)) + "=" + ai_sql_literal(request)
          + " AND " + state + "='PENDING'"
          + " AND " + ai_quote_identifier("__ai_spec_" + std::to_string(task.column)) + "=" + ai_sql_literal(std::to_string(task.definition.version()))
          + " AND EXISTS(SELECT 1 FROM oceanbase.__all_ai_column a WHERE a.table_id=" + std::to_string(table)
          + " AND a.column_id=" + std::to_string(task.column) + " AND a.definition_version=" + std::to_string(task.definition.version()) + ")";
      if (!first) { sql += ','; } first = false;
      if (!pending.empty()) { pending += " OR "; }
      pending += '(' + guard + ')';
      sql += column + "=IF(" + guard + "," + value + "," + column + "),"
          + state + "=IF(" + guard + "," + ai_sql_literal(task.state == 2 ? "READY" : "FAILED") + "," + state + ")";
    }
    sql += " WHERE (" + tasks[0].predicate + ") AND (" + pending + ")";
    if (OB_FAIL(ret)) { return ret; }
    ObMySQLTransaction transaction;
    OZ(transaction.start(GCTX.sql_proxy_));
    // Native DML maintains ordinary and vector indexes; the whole generation
    // group becomes visible at the single transaction commit below.
    OZ(write_sql(transaction, sql));
    OZ(write_sql(transaction, "DELETE FROM " + std::string(TASK_TABLE) + " WHERE " + task_filter(table, request)));
    if (transaction.is_started()) { const int ended = transaction.end(ret == OB_SUCCESS); if (ret == OB_SUCCESS) { ret = ended; } }
    return ret;
  }
  void run1() override {
    lib::set_thread_name("AIColumn");
    while (!has_set_stop()) {
      try {
        THIS_WORKER.set_timeout_ts(ObTimeUtility::current_time() + 120000000);
        uint64_t table = 0; std::string request;
        if (GCTX.sql_proxy_ != nullptr && select_group(table, request) == OB_SUCCESS) {
          const int ret = process_group(table, request);
          if (ret == OB_SUCCESS) { continue; }
          if (ret != OB_EAGAIN && REACH_TIME_INTERVAL(1000000)) {
            LOG_WARN("AI COLUMN task group execution failed", K(ret), K(table));
          }
        }
      } catch (const std::exception &) {
        // An abandoned lease is reclaimable after restart or a local failure.
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
};
AIColumnWorker worker;
std::mutex worker_mutex;
bool worker_started = false;
} // namespace
void start_ai_column_worker()
{
  std::lock_guard<std::mutex> lock(worker_mutex);
  if (!worker_started && worker.init() == OB_SUCCESS && worker.start() == OB_SUCCESS) { worker_started = true; }
}
void stop_ai_column_worker()
{
  std::lock_guard<std::mutex> lock(worker_mutex);
  if (worker_started) { worker.stop(); worker.wait(); worker.destroy(); worker_started = false; }
}
}} // namespace oceanbase::sql
