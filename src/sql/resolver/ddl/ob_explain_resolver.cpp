/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/ddl/ob_explain_resolver.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/parser/ob_parser.h"
#include "lib/string/ob_sql_string.h"

using namespace oceanbase;
using namespace sql;
using namespace common;
const static int64_t EXPLAIN_FORMAT = 0;
const static int64_t EXPLAIN_DISPLAY_OPTION = 1;
const static int64_t EXPLAIN_INTO_TABLE = 2;
const static int64_t EXPLAIN_STMT_ID = 3;
const static int64_t EXPLAIN_CHILD_STMT = 4;
int ObExplainResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;

  if (parse_tree.type_ == T_EXPLAIN && parse_tree.reserved_ == 1
      && parse_tree.num_child_ == 5 && OB_NOT_NULL(parse_tree.children_[4])) {
    ObStmt *child = nullptr;
    ObResolver resolver(params_);
    if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT, *parse_tree.children_[4], child))) {
    } else if (OB_ISNULL(child) || !child->is_select_stmt()) { ret = OB_INVALID_ARGUMENT;
    } else {
      ObSelectStmt *select = static_cast<ObSelectStmt *>(child);
      ObSqlString query;
      auto literal = [&](const ObString &text) -> int {
        int ret = query.append("CONVERT(0x");
        for (int64_t i = 0; OB_SUCC(ret) && i < text.length(); ++i) {
          ret = query.append_fmt("%02x", static_cast<unsigned char>(text.ptr()[i]));
        }
        if (OB_SUCC(ret)) { ret = query.append(" USING utf8mb4)"); }
        return ret;
      };
      for (int64_t i = 0; OB_SUCC(ret) && i < select->get_select_item_size(); ++i) {
        const SelectItem &item = select->get_select_item(i);
        const char *type = "VARCHAR";
        switch (item.expr_->get_data_type()) {
          case ObIntType: type = "BIGINT"; break;
          case ObDoubleType: type = "DOUBLE"; break;
          case ObTinyIntType: type = "BOOLEAN"; break;
          case ObDateType: type = "DATE"; break;
          case ObDateTimeType: type = "DATETIME"; break;
          case ObNullType: type = "NULL"; break;
          default: break;
        }
        if (OB_FAIL(query.append(i == 0 ? "SELECT " : " UNION ALL SELECT "))) {
        } else if (OB_FAIL(literal(item.alias_name_.empty() ? item.expr_name_ : item.alias_name_))) {
        } else if (OB_FAIL(query.append(" AS column_name, "))) {
        } else if (OB_FAIL(literal(ObString(type)))) {
        } else {
          ret = query.append_fmt(" AS column_type, '%s' AS `null`, NULL AS `key`, NULL AS `default`, NULL AS extra",
              item.expr_->get_result_flag() & NOT_NULL_FLAG ? "NO" : "YES");
        }
      }
      ObString sql;
      ParseResult parsed;
      ObParser parser(*allocator_, session_info_->get_sql_mode());
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(ob_write_string(*allocator_, query.string(), sql))) {
      } else if (OB_FAIL(parser.parse(sql, parsed))) {
      } else if (OB_ISNULL(parsed.result_tree_) || parsed.result_tree_->num_child_ != 1) { ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT, *parsed.result_tree_->children_[0], stmt_))) {
      } else { session_info_->set_stmt_type(stmt::T_SELECT); }
    }
    return ret;
  }
  ObExplainStmt *explain_stmt = NULL;
  if (!(T_EXPLAIN == parse_tree.type_  && 5 == parse_tree.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid EXPLAIN syntax",
             "type", parse_tree.type_,
             "# of child", parse_tree.num_child_);
  } else if (NULL == (explain_stmt = create_stmt<ObExplainStmt>())) {
    ret = OB_SQL_RESOLVER_NO_MEMORY;
    LOG_WARN("failed to create explain stmt");
  } else {
    ParseNode *child_node = parse_tree.children_[EXPLAIN_CHILD_STMT];
    ObStmt *child_stmt = NULL;
    ObExplainDisplayOpt opt;
    opt.with_tree_line_ = true;
    if (OB_ISNULL(child_node)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("empty parse tree for stmt");
    } else if (!(T_SELECT == child_node->type_
                 || T_DELETE == child_node->type_
                 || T_INSERT == child_node->type_
                 || T_UPDATE == child_node->type_
                 || T_REPLACE == child_node->type_)) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("invalid stmt type for EXPLAIN", "type", child_node->type_);
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "stmt type for EXPLAIN");
    } else {
      ObResolver resolver(params_);
      if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                   *child_node,
                                   child_stmt))) {
      } else if (OB_ISNULL(child_stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to generate stmt for child stmt");
      } else {
        explain_stmt->set_explain_query_stmt(static_cast<ObDMLStmt *>(child_stmt));
        ParseNode *format = parse_tree.children_[EXPLAIN_FORMAT];
        if (NULL != format) {
          switch (format->type_) {
          case T_OUTLINE:
          {
            explain_stmt->set_explain_format(EXPLAIN_OUTLINE);
            opt.with_tree_line_ = true;
          } break;
          case T_EXTENDED:
          {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED);
            opt.with_tree_line_ = true;
          } break;
          case T_PARTITIONS:
          {
            explain_stmt->set_explain_format(EXPLAIN_PARTITIONS);
            opt.with_tree_line_ = true;
          } break;
          case T_TRADITIONAL:
          {
            explain_stmt->set_explain_format(EXPLAIN_TRADITIONAL);
            opt.with_tree_line_ = true;
          } break;
          case T_FORMAT_JSON:
          {
            explain_stmt->set_explain_format(EXPLAIN_FORMAT_JSON);
            opt.with_tree_line_ = false;
          } break;
          case T_BASIC: {
            explain_stmt->set_explain_format(EXPLAIN_BASIC);
            opt.with_tree_line_ = true;
            break;
          }
          case T_EXTENDED_NOADDR: {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED_NOADDR);
            opt.with_tree_line_ = true;
            break;
          }
          case T_PLANREGRESS: {
            explain_stmt->set_explain_format(EXPLAIN_PLANREGRESS);
            opt.with_tree_line_ = true;
            break;
          }
          default:
            ret = OB_ERROR;
            LOG_WARN("unknown explain format", "format", format->type_);
            break;
          }
        }
      }
    }
    ParseNode *opt_node = parse_tree.children_[EXPLAIN_DISPLAY_OPTION];
    if (OB_SUCC(ret) && NULL != opt_node) {
      switch (opt_node->type_) {
      case T_PRETTY_COLOR:
        opt.with_color_ = true;
        // pass
      case T_PRETTY:
        opt.with_tree_line_ = true;
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (OB_SUCC(ret)) {
      explain_stmt->set_display_opt(opt);
    }
    ParseNode *into_table_node = parse_tree.children_[EXPLAIN_INTO_TABLE];
    if (OB_SUCC(ret) && NULL != into_table_node) {
      ObString into_table(into_table_node->str_len_, into_table_node->str_value_);
      explain_stmt->set_into_table(into_table);
    }
    ParseNode *statement_id_node = parse_tree.children_[EXPLAIN_STMT_ID];
    if (OB_SUCC(ret) && NULL != statement_id_node) {
      ObString statement_id(statement_id_node->str_len_, statement_id_node->str_value_);
      explain_stmt->set_statement_id(statement_id);
    }
  }

  return ret;
}
