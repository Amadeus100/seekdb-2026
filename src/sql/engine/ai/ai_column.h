/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include "share/ai_service/ai_column_definition.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_table_schema.h"
#include "sql/parser/parse_node.h"

namespace oceanbase { namespace sql {
class ObExecContext;
class ObSQLSessionInfo;
class ObStmt;
struct ObResolverParams;
int resolve_ai_column_command(ObResolverParams &params, const ParseNode &node, ObStmt *&stmt);
bool get_ai_column_definition(const share::schema::ObColumnSchemaV2 &column,
                              share::AIColumnDefinition &definition);
int resolve_ai_column_attribute(const ParseNode &node,
    share::schema::ObColumnSchemaV2 &column, common::ObIAllocator &allocator);
int finalize_ai_column_schema(share::schema::ObTableSchema &table,
    common::ObIAllocator &allocator, const share::schema::ObTableSchema *base = nullptr);
int install_ai_column_triggers(ObExecContext &ctx, uint64_t table_id, bool backfill = false);
int check_ai_column_strict_read(ObSQLSessionInfo &session,
    const share::schema::ObTableSchema &table, const share::schema::ObColumnSchemaV2 &column);
int check_ai_column_assignment(ObSQLSessionInfo &session,
    const share::schema::ObTableSchema &table, uint64_t column_id);
void start_ai_column_worker();
void stop_ai_column_worker();
bool is_ai_column_internal_access();
uint64_t ai_column_trigger_compile_table();
class AIColumnTriggerCompileGuard {
public:
  explicit AIColumnTriggerCompileGuard(uint64_t table_id);
  ~AIColumnTriggerCompileGuard();
private:
  uint64_t previous_;
};
class AIColumnInternalAccessGuard {
public:
  AIColumnInternalAccessGuard();
  ~AIColumnInternalAccessGuard();
private:
  bool previous_;
};
std::string ai_quote_identifier(const std::string &text);
std::string ai_sql_literal(const std::string &text);
std::string ai_qualify_expression(const std::string &expression,
    const std::vector<std::string> &dependencies, const std::string &qualifier);
}} // namespace oceanbase::sql
