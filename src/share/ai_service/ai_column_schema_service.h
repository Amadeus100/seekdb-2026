/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include "common/mysqlclient/ob_isql_client.h"
#include "share/schema/ob_table_schema.h"

namespace oceanbase { namespace share {
// The caller owns the schema DDL transaction. Never open a separate transaction
// here: metadata must be committed or rolled back with its column schema.
int persist_ai_column_metadata(common::ObISQLClient &client,
    const schema::ObTableSchema &table, const schema::ObColumnSchemaV2 &column);
int persist_ai_column_metadata(common::ObISQLClient &client, const schema::ObTableSchema &table);
int delete_ai_column_metadata(common::ObISQLClient &client, uint64_t table_id,
                              uint64_t column_id = UINT64_MAX);
}} // namespace oceanbase::share
