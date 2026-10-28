/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
#include <cstdio>

namespace oceanbase { namespace sql { namespace file_sql {

enum class Type { Null, Bigint, Double, Boolean, Varchar, Date, Datetime };
struct Cell {
  std::string text;
  bool null = true;
  bool quoted = false;
};
struct Column {
  std::string name;
  Type type = Type::Null;
  bool nullable = false;
  bool unsafe_integer = false;
  uint8_t datetime_scale = 0;
};
struct Options {
  std::string format;
  std::string path;
  std::string secure_root;
  std::string nullstr;
  char delimiter = ',';
  char quote = '"';
  char escape = '"';
  bool header = true;
};
struct Schema {
  Options options;
  std::vector<Column> columns;
  uint64_t size = 0;
  int64_t modified = 0;
  std::string encode() const;
  static bool decode(const std::string &encoded, Schema &schema);
};

// This reader owns only one logical record. Schema inference is a separate,
// complete streaming pass; execution never materializes the entire input.
class ParquetReader;
class FileScan {
public:
  FileScan() = default;
  ~FileScan();
  FileScan(const FileScan &) = delete;
  FileScan &operator=(const FileScan &) = delete;
  bool infer(const Options &options, Schema &schema);
  bool open(const Schema &schema);
  bool next(std::vector<Cell> &row, bool &end);
  bool rescan();
  void project(const std::vector<size_t> &columns);
  void close();
  const std::string &error() const { return error_; }
  static Type classify(const std::string &text);
  static Type widen(Type left, Type right);
  static bool parse_time(const std::string &text, int64_t &micros, bool &date_only);
  static bool secure_path(const std::string &path, const std::string &root,
                          std::string &canonical, std::string &error);
private:
  bool open_input();
  bool csv_record(std::vector<Cell> &row, bool &end);
  bool json_record(std::vector<std::string> &names, std::vector<Cell> &row, bool &end);
  bool unchanged();
  bool fail(const std::string &message);
  Schema schema_;
  std::ifstream stream_;
  std::string error_;
  uint64_t line_ = 0;
  bool inference_ = false;
  ParquetReader *parquet_ = nullptr;
  bool has_projection_ = false;
  std::vector<size_t> projection_;
  std::vector<std::string> files_;
  size_t file_index_ = 0;
};

const char *type_name(Type type);
constexpr const char *DESCRIPTOR_PREFIX = "seekdb-file-scan-v1:";

class CsvCopyWriter {
public:
  ~CsvCopyWriter();
  bool open(const Schema &schema);
  bool append(const std::vector<Cell> &row);
  bool commit();
  void abort();
  const std::string &error() const { return error_; }
private:
  bool fail(const std::string &message);
  Schema schema_;
  std::FILE *file_ = nullptr;
  int directory_ = -1;
  std::string target_, temporary_, parent_, error_;
};

}}} // namespace oceanbase::sql::file_sql
