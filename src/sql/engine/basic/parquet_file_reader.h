/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include "file_scan_utils.h"
#include <memory>

namespace oceanbase { namespace sql { namespace file_sql {
// Flat, page-streaming Parquet reader. No external query engine is involved.
// A projected column retains at most one decoded data page and its dictionary.
class ParquetReader {
public:
  explicit ParquetReader(std::ifstream &stream);
  ~ParquetReader();
  bool read_schema(Schema &schema);
  bool next(std::vector<Cell> &row, bool &end);
  void project(const std::vector<size_t> &columns);
  const std::string &error() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}}}
