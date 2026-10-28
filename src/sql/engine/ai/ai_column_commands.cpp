/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#define USING_LOG_PREFIX SQL_RESV
#include "ai_column.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/resolver/ob_schema_checker.h"
#include "sql/parser/ob_parser.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/ob_sql_context.h"
#include <cctype>

namespace oceanbase { namespace sql {
using namespace common;
using namespace share;
using namespace share::schema;
namespace {
std::string text(const ObString &s) { return std::string(s.ptr(),s.length()); }
std::string lower(std::string s) {
  for (char &c : s) { if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; } }
  return s;
}
// Unquoted status denotes the pseudo-column. Quoted identifiers and string
// literals retain their original SQL spelling and business meaning.
std::string retry_predicate(const std::string &source, const std::string &state)
{
  std::string result;
  for (size_t i = 0; i < source.size();) {
    size_t start = i;
    char c = source[i];
    if (c == '\'' || c == '"' || c == '`') {
      const char quote = c; ++i;
      while (i < source.size()) {
        if (source[i] == '\\' && quote != '`' && i + 1 < source.size()) { i += 2; }
        else if (source[i++] == quote) {
          if (i < source.size() && source[i] == quote) { ++i; } else { break; }
        }
      }
      result.append(source,start,i-start);
    } else if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 128) {
      while (i < source.size() && (std::isalnum(static_cast<unsigned char>(source[i]))
          || source[i] == '_' || static_cast<unsigned char>(source[i]) >= 128)) { ++i; }
      const std::string name = source.substr(start,i-start);
      result += lower(name) == "status" ? ai_quote_identifier(state) : name;
    } else { result += source[i++]; }
  }
  return result;
}
}
int resolve_ai_column_command(ObResolverParams &params, const ParseNode &node, ObStmt *&stmt)
{
  int ret = OB_SUCCESS;
  const bool show = node.type_ == T_SHOW_AI_COLUMNS;
  if ((!show && node.type_ != T_AI_COLUMN_RETRY) || node.num_child_ != (show ? 1 : 4)
      || node.children_[0] == nullptr || params.session_info_ == nullptr || params.schema_checker_ == nullptr) {
    return OB_INVALID_ARGUMENT;
  }
  if (!show && node.reserved_) {
    LOG_USER_ERROR(OB_NOT_SUPPORTED,"combining AI COLUMN RETRY with another ALTER action");
    return OB_NOT_SUPPORTED;
  }
  const ParseNode *relation = node.children_[0];
  if (relation->type_ != T_RELATION_FACTOR || relation->num_child_ < 2 || relation->children_[1] == nullptr) { return OB_INVALID_ARGUMENT; }
  const ParseNode *db = relation->children_[0], *name = relation->children_[1];
  const ObString database = db == nullptr ? params.session_info_->get_database_name() : ObString(db->str_len_,db->str_value_);
  const ObString table_name(name->str_len_,name->str_value_);
  const ObTableSchema *table = nullptr;
  OZ(params.schema_checker_->get_table_schema(database,table_name,false,table));
  if (OB_SUCC(ret) && table == nullptr) { return OB_TABLE_NOT_EXIST; }
  if (OB_FAIL(ret)) { return ret; }
  const std::string qualified = ai_quote_identifier(text(database)) + '.' + ai_quote_identifier(text(table_name));
  std::string sql;
  if (show) {
    sql = "SELECT column_name,ai_model_name,normalized_expr,sync_mode,max_retries,definition_version "
        "FROM oceanbase.__all_ai_column WHERE table_id=" + std::to_string(table->get_table_id()) + " ORDER BY column_id";
  } else {
    const ParseNode *column_name = node.children_[1];
    if (column_name == nullptr) { return OB_INVALID_ARGUMENT; }
    const ObColumnSchemaV2 *column = table->get_column_schema(ObString(column_name->str_len_,column_name->str_value_));
    AIColumnDefinition definition;
    if (column == nullptr || !get_ai_column_definition(*column,definition)) {
      LOG_USER_ERROR(OB_NOT_SUPPORTED,"retrying a column that is not an AI column");
      return OB_NOT_SUPPORTED;
    }
    const ObColumnSchemaV2 *state = table->get_column_schema(definition.state_column);
    const ObColumnSchemaV2 *request = table->get_column_schema(definition.request_column);
    if (state == nullptr || request == nullptr) { return OB_INVALID_DATA; }
    const std::string state_name = text(state->get_column_name_str());
    sql = "UPDATE " + qualified + " SET " + ai_quote_identifier(state_name) + "='PENDING',"
        + ai_quote_identifier(text(request->get_column_name_str())) + "=UUID()";
    if (node.children_[2] != nullptr) {
      const ParseNode *predicate = node.children_[2];
      if (predicate->str_value_ == nullptr || predicate->str_len_ <= 0) { return OB_INVALID_ARGUMENT; }
      sql += " WHERE " + retry_predicate(std::string(predicate->str_value_,predicate->str_len_), state_name);
    }
  }
  ParseResult parsed;
  ObParser parser(*params.allocator_,params.session_info_->get_sql_mode());
  ObString stored;
  AIColumnInternalAccessGuard internal;
  if (OB_FAIL(ob_write_string(*params.allocator_,ObString(sql.size(),sql.data()),stored))) {
  } else if (OB_FAIL(parser.parse(stored,parsed))) {
  } else if (parsed.result_tree_ == nullptr || parsed.result_tree_->num_child_ != 1) { ret = OB_ERR_UNEXPECTED;
  } else {
    ObResolver resolver(params);
    ret = resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,*parsed.result_tree_->children_[0],stmt);
  }
  return ret;
}
}} // namespace oceanbase::sql
