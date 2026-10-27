/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#include "sql/engine/basic/file_scan_utils.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>

using namespace oceanbase::sql::file_sql;
namespace fs = std::filesystem;
static int checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) { \
  std::cerr << "check failed at line " << __LINE__ << ": " << #value << '\n'; std::exit(1); } } while (false)
static void write(const fs::path &path, const std::string &data)
{
  std::ofstream out(path, std::ios::binary); out.write(data.data(), data.size()); CHECK(out.good());
}
int main(int argc, char **argv)
{
  CHECK(argc == 3);
  fs::path dir = fs::path(argv[1]) / "file-scan-unit-fixtures";
  fs::create_directories(dir);
  Options options; options.secure_root = dir.generic_string();
  options.path = (dir / "quoted.csv").generic_string(); options.format = "csv";
  write(options.path, "id,name,active,day,ts,id\r\n1,\"a,b\nworld\",true,2024-02-29,2024-02-29T12:13:14,9\r\n2,\"say \"\"hi\"\"\",false,,,10\r\n");
  FileScan scan; Schema schema;
  CHECK(scan.infer(options, schema)); CHECK(schema.columns.size() == 6);
  CHECK(schema.columns[0].type == Type::Bigint); CHECK(schema.columns[2].type == Type::Boolean);
  CHECK(schema.columns[3].type == Type::Date); CHECK(schema.columns[4].type == Type::Datetime);
  CHECK(schema.columns[3].nullable); CHECK(schema.columns[5].name == "id__2");
  Schema decoded; CHECK(Schema::decode(schema.encode(), decoded));
  CHECK(decoded.columns[5].name == "id__2"); CHECK(scan.open(decoded));
  std::vector<Cell> row; bool end;
  CHECK(scan.next(row, end) && !end); CHECK(row[1].text == "a,b\nworld");
  CHECK(scan.next(row, end) && !end); CHECK(row[1].text == "say \"hi\""); CHECK(row[3].null);
  CHECK(scan.next(row, end) && end); CHECK(scan.rescan()); CHECK(scan.next(row, end) && !end);
  scan.close();
  CHECK(FileScan::classify("9223372036854775807") == Type::Bigint);
  CHECK(FileScan::classify("9223372036854775808") == Type::Varchar);
  CHECK(FileScan::classify("-9223372036854775808") == Type::Bigint);
  CHECK(FileScan::classify("-9223372036854775809") == Type::Varchar);
  CHECK(FileScan::classify("1e1000") == Type::Varchar); CHECK(FileScan::classify("NaN") == Type::Varchar);
  CHECK(FileScan::classify("2023-02-29") == Type::Varchar); CHECK(FileScan::classify("2024-02-29") == Type::Date);
  CHECK(FileScan::classify("2024-01-01 24:00:00") == Type::Varchar);
  CHECK(FileScan::widen(Type::Bigint, Type::Double) == Type::Double);
  CHECK(FileScan::widen(Type::Boolean, Type::Bigint) == Type::Varchar);
  CHECK(FileScan::widen(Type::Date, Type::Datetime) == Type::Datetime);
  options.format = "json"; options.path = (dir / "union.jsonl").generic_string();
  write(options.path, "{\"id\":1,\"name\":\"你好\",\"score\":3.5}\n{\"id\":2,\"other\":true}\n");
  CHECK(scan.infer(options, schema)); CHECK(schema.columns.size() == 4);
  CHECK(schema.columns[1].nullable && schema.columns[2].nullable && schema.columns[3].nullable);
  CHECK(scan.open(schema)); CHECK(scan.next(row, end) && !end); CHECK(row[3].null);
  CHECK(scan.next(row, end) && !end); CHECK(row[1].null && row[2].null && row[3].text == "true");
  scan.close();
  write(options.path, "{\"nested\":{\"a\":1}}\n"); CHECK(!scan.infer(options, schema));
  write(options.path, "{\"a\":1,\"a\":2}\n"); CHECK(!scan.infer(options, schema));
  options.format = "csv"; options.path = (dir / "bad.csv").generic_string();
  write(options.path, "a,b\n1\n"); CHECK(!scan.infer(options, schema));
  write(options.path, "a,b\n1,\"missing\n"); CHECK(!scan.infer(options, schema));
  write(options.path, "a,b\n1,2\n"); CHECK(scan.infer(options, schema)); CHECK(scan.open(schema));
  scan.close(); write(options.path, "a,b\n1,200\n"); CHECK(!scan.open(schema));
  std::string canonical, error;
  CHECK(!FileScan::secure_path(options.path, "", canonical, error));
  CHECK(!FileScan::secure_path((dir.parent_path() / "outside.csv").generic_string(), options.secure_root, canonical, error));
  options.format = "glob"; options.path = (dir / "*.csv").generic_string();
  CHECK(scan.infer(options, schema)); CHECK(scan.open(schema));
  int files = 0; while (scan.next(row, end) && !end) { ++files; CHECK(row.size() == 1); }
  CHECK(files == 2); CHECK(scan.rescan());
  scan.close(); options.format = "parquet"; options.path = (dir / "simple.parquet").generic_string();
  fs::copy_file(fs::path(argv[2]), options.path, fs::copy_options::overwrite_existing);
  CHECK(scan.infer(options, schema)); CHECK(schema.columns.size() == 1);
  CHECK(schema.columns[0].name == "c1" && schema.columns[0].type == Type::Bigint);
  CHECK(scan.open(schema));
  int parquet_rows = 0;
  while (scan.next(row, end) && !end) { CHECK(!row[0].null && row[0].text == std::to_string(parquet_rows)); ++parquet_rows; }
  if (!scan.error().empty()) { std::cerr << scan.error() << '\n'; }
  CHECK(end && parquet_rows == 7); CHECK(scan.rescan()); CHECK(scan.next(row, end) && !end && row[0].text == "0");
  std::cout << checks << " file scanner checks passed\n";
  return 0;
}
