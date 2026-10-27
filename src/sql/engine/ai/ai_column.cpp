/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#define USING_LOG_PREFIX SQL_ENG
#include "ai_column.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/ob_server_struct.h"
#include "lib/mysqlclient/ob_mysql_proxy.h"
#include "lib/mysqlclient/ob_mysql_transaction.h"
#include "lib/mysqlclient/ob_mysql_result.h"
#include "lib/thread/thread_pool.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <set>
#include <sstream>

namespace oceanbase { namespace sql {
using namespace common;
using namespace share;
using namespace share::schema;
namespace {
std::string ai_lower(std::string text) {
  for (char &c : text) { if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; } }
  return text;
}
std::string ai_string(const ObString &text) { return std::string(text.ptr(), text.length()); }
std::string ai_trim(const std::string &text) {
  size_t first = text.find_first_not_of(" \t\r\n");
  return first == std::string::npos ? std::string() : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
bool ai_split_arguments(const std::string &text, std::vector<std::string> &args) {
  size_t begin = text.find('(');
  if (begin == std::string::npos) { return false; }
  size_t start = begin + 1; int depth = 1; char quote = 0;
  for (size_t i = start; i < text.size(); ++i) {
    char c = text[i];
    if (quote) {
      if (c == '\\' && quote != '`' && i + 1 < text.size()) { ++i; }
      else if (c == quote) { if (i + 1 < text.size() && text[i + 1] == quote) { ++i; } else { quote = 0; } }
    } else if (c == '\'' || c == '"' || c == '`') { quote = c;
    } else if (c == '(') { ++depth;
    } else if (c == ')') {
      if (--depth == 0) { args.push_back(ai_trim(text.substr(start, i - start))); return ai_trim(text.substr(i + 1)).empty(); }
    } else if (c == ',' && depth == 1) { args.push_back(ai_trim(text.substr(start, i - start))); start = i + 1; }
  }
  return false;
}
int ai_store_definition(ObColumnSchemaV2 &column, const AIColumnDefinition &definition)
{
  int ret = OB_SUCCESS;
  std::string encoded = definition.encode();
  ObSEArray<ObString, 2> info;
  if (encoded.size() > 48 * 1024) { ret = OB_SIZE_OVERFLOW; }
  for (int64_t i = 0; OB_SUCC(ret) && i < column.get_extended_type_info().count(); ++i) {
    const ObString &entry = column.get_extended_type_info().at(i);
    if (!entry.prefix_match(AI_COLUMN_DEFINITION_PREFIX)) { ret = info.push_back(entry); }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(info.push_back(ObString(encoded.size(), encoded.data())))) {
  } else if (OB_FAIL(column.set_extended_type_info(info))) {
  } else { column.add_column_flag(AI_COLUMN_SCHEMA_FLAG); }
  return ret;
}
int ai_collect_dependencies(const ParseNode *node, std::vector<std::string> &dependencies, int depth)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node)) { return ret; }
  if (depth > 128 || node->type_ == T_SELECT || node->type_ == T_REF_QUERY
      || node->type_ == T_QUESTIONMARK || node->type_ == T_USER_VARIABLE_IDENTIFIER) { return OB_NOT_SUPPORTED; }
  if (node->type_ == T_COLUMN_REF) {
    if (node->num_child_ != 3 || OB_NOT_NULL(node->children_[0]) || OB_NOT_NULL(node->children_[1])
        || OB_ISNULL(node->children_[2])) { return OB_NOT_SUPPORTED; }
    std::string name(node->children_[2]->str_value_, node->children_[2]->str_len_);
    if (std::find(dependencies.begin(), dependencies.end(), name) == dependencies.end()) { dependencies.push_back(name); }
  } else {
    if (node->type_ == T_FUN_SYS && node->num_child_ == 2 && OB_NOT_NULL(node->children_[0])) {
      std::string name = ai_lower(std::string(node->children_[0]->str_value_, node->children_[0]->str_len_));
      if (name == "ai_complete" || name == "ai_embed" || name == "ai_rerank"
          || name == "uuid" || name == "rand" || name == "sleep") { return OB_NOT_SUPPORTED; }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < node->num_child_; ++i) {
      ret = ai_collect_dependencies(node->children_[i], dependencies, depth + 1);
    }
  }
  return ret;
}
}

std::string ai_quote_identifier(const std::string &text)
{
  std::string result = "`";
  for (char c : text) { if (c == '`') { result += '`'; } result += c; }
  return result + '`';
}
std::string ai_sql_literal(const std::string &text)
{
  static const char hex[] = "0123456789abcdef";
  if (text.empty()) { return "''"; }
  std::string result = "CONVERT(0x"; result.reserve(text.size() * 2 + 32);
  for (unsigned char c : text) { result += hex[c >> 4]; result += hex[c & 15]; }
  return result + " USING utf8mb4)";
}
std::string ai_qualify_expression(const std::string &expression,
    const std::vector<std::string> &dependencies, const std::string &qualifier)
{
  std::string out;
  for (size_t i = 0; i < expression.size();) {
    size_t start = i; char c = expression[i];
    if (c == '\'' || c == '"') {
      char quote = c; ++i;
      while (i < expression.size()) {
        if (expression[i] == '\\' && i + 1 < expression.size()) { i += 2; }
        else if (expression[i++] == quote) { if (i < expression.size() && expression[i] == quote) { ++i; } else { break; } }
      }
      out.append(expression, start, i - start);
    } else if (c == '`' || std::isalnum(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 128) {
      std::string name;
      bool quoted = c == '`';
      if (quoted) {
        ++i;
        while (i < expression.size()) {
          char value = expression[i++];
          if (value == '`') { if (i < expression.size() && expression[i] == '`') { ++i; } else { break; } }
          name += value;
        }
      } else {
        while (i < expression.size() && (std::isalnum(static_cast<unsigned char>(expression[i]))
            || expression[i] == '_' || static_cast<unsigned char>(expression[i]) >= 128)) { name += expression[i++]; }
      }
      size_t next = expression.find_first_not_of(" \t\r\n", i);
      bool function = next != std::string::npos && expression[next] == '(' && !quoted;
      auto dependency = std::find_if(dependencies.begin(), dependencies.end(), [&](const std::string &d) { return ai_lower(d) == ai_lower(name); });
      if (!function && dependency != dependencies.end()) { out += qualifier + "." + ai_quote_identifier(*dependency); }
      else { out.append(expression, start, i - start); }
    } else { out += expression[i++]; }
  }
  return out;
}
bool get_ai_column_definition(const ObColumnSchemaV2 &column, AIColumnDefinition &definition)
{
  const auto &info = column.get_extended_type_info();
  return column.has_column_flag(AI_COLUMN_SCHEMA_FLAG) && info.count() > 0
      && AIColumnDefinition::decode(ai_string(info.at(info.count() - 1)), definition);
}

int resolve_ai_column_attribute(const ParseNode &node, ObColumnSchemaV2 &column, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  AIColumnDefinition definition;
  const ParseNode *expression = node.num_child_ == 2 ? node.children_[0] : nullptr;
  const ParseNode *parameters = nullptr;
  std::vector<std::string> args;
  if (OB_ISNULL(expression) || expression->type_ != T_FUN_SYS || expression->num_child_ != 2
      || OB_ISNULL(expression->children_[0]) || OB_ISNULL(parameters = expression->children_[1])) {
    ret = OB_NOT_SUPPORTED;
  } else {
    std::string function = ai_lower(std::string(expression->children_[0]->str_value_, expression->children_[0]->str_len_));
    definition.embedding = function == "ai_embed";
    if ((function != "ai_complete" && function != "ai_embed") || parameters->num_child_ < 2
        || parameters->num_child_ > 3 || parameters->children_[0]->type_ != T_VARCHAR) { ret = OB_INVALID_ARGUMENT;
    } else {
      definition.model.assign(parameters->children_[0]->str_value_, parameters->children_[0]->str_len_);
      definition.expression.assign(expression->str_value_, expression->str_len_);
      if (!ai_split_arguments(definition.expression, args) || args.size() != static_cast<size_t>(parameters->num_child_)) { ret = OB_INVALID_ARGUMENT;
      } else {
        definition.input = args[1];
        definition.input_json = parameters->children_[1]->type_ == T_FUN_SYS
            && parameters->children_[1]->num_child_ == 2 && OB_NOT_NULL(parameters->children_[1]->children_[0])
            && ObString(parameters->children_[1]->children_[0]->str_len_, parameters->children_[1]->children_[0]->str_value_).case_compare("AI_PROMPT") == 0;
        if (args.size() == 3 && !definition.embedding) { definition.config = args[2]; }
        ret = ai_collect_dependencies(parameters->children_[1], definition.dependencies, 0);
      }
      definition.result_type = column.get_data_type(); definition.result_length = column.get_data_length();
      if (OB_SUCC(ret) && definition.embedding) {
        if (!column.is_collection() || column.get_extended_type_info().count() != 1) { ret = OB_INVALID_ARGUMENT;
        } else {
          std::string vector_type = ai_lower(ai_string(column.get_extended_type_info().at(0)));
          size_t end = vector_type.rfind(')'); size_t start = vector_type.find_last_of(",(", end);
          if (vector_type.find("vector") != 0 || start == std::string::npos || end == std::string::npos) { ret = OB_INVALID_ARGUMENT;
          } else {
            std::string dimension = ai_trim(vector_type.substr(start + 1, end - start - 1));
            auto parsed = std::from_chars(dimension.data(), dimension.data() + dimension.size(), definition.dimension);
            if (parsed.ec != std::errc() || definition.dimension <= 0 || definition.dimension > 65536) { ret = OB_INVALID_ARGUMENT; }
          }
          if (OB_SUCC(ret) && args.size() == 3) {
            const ParseNode *dimension = parameters->children_[2];
            if (dimension->type_ != T_INT || dimension->value_ != definition.dimension) { ret = OB_ERR_INVALID_TYPE_FOR_OP; }
          }
        }
      } else if (OB_SUCC(ret)) {
        if (column.is_collection() || column.is_enum_or_set() || column.is_generated_column()) { ret = OB_NOT_SUPPORTED;
        } else if (column.is_json()) { definition.system_prompt = "Return exactly one valid JSON value. Do not include markdown or explanatory text.";
        } else if (ob_is_integer_type(column.get_data_type())) { definition.system_prompt = "Return exactly one base-10 integer, with no explanation or markdown.";
        } else if (column.get_data_type() == ObDateType) { definition.system_prompt = "Return exactly one valid date in YYYY-MM-DD format.";
        } else if (column.get_data_type() == ObDateTimeType) { definition.system_prompt = "Return exactly one valid datetime in YYYY-MM-DD HH:MM:SS format.";
        } else { definition.system_prompt = "Return only the generated value as plain text, without markdown or explanatory text."; }
      }
      const ParseNode *options = node.children_[1];
      std::set<std::string> used;
      for (int64_t i = 0; OB_SUCC(ret) && OB_NOT_NULL(options) && i < options->num_child_; ++i) {
        const ParseNode *option = options->children_[i];
        std::string name = ai_lower(std::string(option->children_[0]->str_value_, option->children_[0]->str_len_));
        const ParseNode *value = option->children_[1];
        if (!used.insert(name).second) { ret = OB_INVALID_ARGUMENT;
        } else if (name == "sync_mode") {
          if (value->type_ != T_VARCHAR || ObString(value->str_len_, value->str_value_).case_compare("async") != 0) { ret = OB_NOT_SUPPORTED; }
        } else if (name == "max_retries") {
          if (value->type_ != T_INT || value->value_ < 0 || value->value_ > 10) { ret = OB_INVALID_ARGUMENT; }
          else { definition.max_retries = value->value_; }
        } else { ret = OB_INVALID_ARGUMENT; }
      }
      ObAIFuncExprInfo *model = nullptr;
      ObString model_name(definition.model.size(), definition.model.data());
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_name, model))) {
      } else if ((definition.embedding && !ObAIFuncUtils::is_dense_embedding_type(model))
          || (!definition.embedding && !ObAIFuncUtils::is_completion_type(model))) { ret = OB_INVALID_ARGUMENT;
      } else { ret = ai_store_definition(column, definition); }
    }
  }
  if (ret == OB_NOT_SUPPORTED) { LOG_USER_ERROR(OB_NOT_SUPPORTED, "invalid AI COLUMN function, type, dependency or option"); }
  return ret;
}

int finalize_ai_column_schema(ObTableSchema &table, ObIAllocator &allocator, const ObTableSchema *base)
{
  int ret = OB_SUCCESS;
  std::vector<uint64_t> ids;
  for (auto it = table.column_begin(); it != table.column_end(); ++it) {
    if ((*it)->has_column_flag(AI_COLUMN_SCHEMA_FLAG)) { ids.push_back((*it)->get_column_id()); }
  }
  uint64_t next = std::max(table.get_max_used_column_id(), base ? base->get_max_used_column_id() : 0);
  for (uint64_t id : ids) {
    if (OB_FAIL(ret)) { break; }
    ObColumnSchemaV2 *column = table.get_column_schema(id);
    AIColumnDefinition definition;
    if (!get_ai_column_definition(*column, definition)) { ret = OB_INVALID_DATA; break; }
    for (const std::string &name : definition.dependencies) {
      ObString dependency(name.size(), name.data());
      const ObColumnSchemaV2 *source = table.get_column_schema(dependency);
      if (OB_ISNULL(source) && base) { source = base->get_column_schema(dependency); }
      if (OB_ISNULL(source)) { ret = OB_ERR_BAD_FIELD_ERROR; break;
      } else if (source->has_column_flag(AI_COLUMN_SCHEMA_FLAG)) {
        ret = OB_NOT_SUPPORTED; LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI columns cannot depend on AI columns"); break;
      } else if (source->is_generated_column()) { ret = OB_NOT_SUPPORTED; break; }
    }
    if (OB_FAIL(ret) || definition.state_column) { continue; }
    for (int i = 0; OB_SUCC(ret) && i < 3; ++i) {
      ObColumnSchemaV2 hidden(&allocator);
      hidden.set_table_id(table.get_table_id()); hidden.set_column_id(++next);
      hidden.set_schema_version(table.get_schema_version()); hidden.set_data_type(ObVarcharType);
      hidden.set_data_length(i == 0 ? 16 : i == 1 ? 36 : 64);
      hidden.set_charset_type(CHARSET_UTF8MB4); hidden.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      hidden.set_is_hidden(true); hidden.set_nullable(true);
      std::string name = std::string(i == 0 ? "__ai_state_" : i == 1 ? "__ai_request_" : "__ai_spec_") + std::to_string(id);
      if (OB_FAIL(hidden.set_column_name(ObString(name.size(), name.data())))) {
      } else if (OB_FAIL(table.add_column(hidden))) {
      } else if (i == 0) { definition.state_column = next;
      } else if (i == 1) { definition.request_column = next;
      } else { definition.spec_column = next; }
    }
    if (OB_SUCC(ret)) { column = table.get_column_schema(id); ret = ai_store_definition(*column, definition); }
  }
  if (OB_SUCC(ret) && !ids.empty()) { table.set_max_used_column_id(next); }
  return ret;
}

}} // namespace oceanbase::sql
