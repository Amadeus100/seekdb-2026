/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#include "file_scan_utils.h"
#include "parquet_file_reader.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <atomic>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif
#include <unordered_set>
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

namespace oceanbase { namespace sql { namespace file_sql {
namespace fs = std::filesystem;
namespace {
constexpr size_t MAX_RECORD = 16 * 1024 * 1024;
constexpr size_t MAX_COLUMNS = 1024;
bool valid_options(const Options &o)
{
  auto valid = [](char c) { return c != '\0' && c != '\n' && c != '\r'; };
  return valid(o.delimiter) && valid(o.quote) && valid(o.escape) && o.delimiter != o.quote
      && o.nullstr.find_first_of(std::string() + o.delimiter + o.quote + '\n' + '\r' + '\0') == std::string::npos;
}
// Match basename wildcards without a backtracking regular-expression engine.
// Advance '?' and '*' by complete UTF-8 characters.
bool wildcard(const std::string &pattern, const std::string &name)
{
  auto next = [](const std::string &s, size_t i) {
    ++i; while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xc0) == 0x80) { ++i; }
    return i;
  };
  size_t p = 0, n = 0, star = std::string::npos, retry = 0;
  while (n < name.size()) {
    if (p < pattern.size() && pattern[p] == '?') { ++p; n = next(name, n); }
    else if (p < pattern.size() && pattern[p] == '*') { star = p++; retry = n; }
    else if (p < pattern.size() && pattern[p] == name[n]) { ++p; ++n; }
    else if (star != std::string::npos && retry < name.size()) { p = star + 1; n = retry = next(name, retry); }
    else { return false; }
  }
  while (p < pattern.size() && pattern[p] == '*') { ++p; }
  return p == pattern.size();
}
std::string lower(std::string text)
{
  for (char &c : text) { if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; } }
  return text;
}

bool valid_utf8(const std::string &text)
{
  for (size_t i = 0; i < text.size();) {
    unsigned char c = text[i++];
    if (c < 0x80) { continue; }
    unsigned count = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
    if (!count || count > text.size() - i) { return false; }
    unsigned char first = text[i];
    if ((c == 0xe0 && first < 0xa0) || (c == 0xed && first > 0x9f)
        || (c == 0xf0 && first < 0x90) || (c == 0xf4 && first > 0x8f)) { return false; }
    while (count--) { if ((static_cast<unsigned char>(text[i++]) & 0xc0) != 0x80) { return false; } }
  }
  return true;
}
void merge_value(Column &column, const Cell &cell, bool json)
{
  column.nullable = column.nullable || cell.null;
  if (cell.null) { return; }
  Type type = FileScan::classify(cell.text);
  if (json && cell.quoted && type != Type::Date && type != Type::Datetime) { type = Type::Varchar; }
  if (type == Type::Bigint) {
    int64_t value = 0; std::from_chars(cell.text.data(), cell.text.data() + cell.text.size(), value);
    column.unsafe_integer = column.unsafe_integer || value > 9007199254740992LL || value < -9007199254740992LL;
  }
  column.type = FileScan::widen(column.type, type);
  if (column.type == Type::Double && column.unsafe_integer) { column.type = Type::Varchar; }
}
bool signature(const std::string &path, uint64_t &size, int64_t &modified)
{
  std::error_code ec;
  auto p = fs::u8path(path);
  if (!fs::is_regular_file(p, ec) || ec) { return false; }
  size = fs::file_size(p, ec);
  if (ec) { return false; }
  auto time = fs::last_write_time(p, ec);
  if (ec) { return false; }
  modified = time.time_since_epoch().count();
  return true;
}
std::string utf8(const fs::path &path)
{
  const auto s = path.generic_u8string();
  return std::string(reinterpret_cast<const char *>(s.data()), s.size());
}
}

const char *type_name(Type type)
{
  switch (type) {
    case Type::Bigint: return "BIGINT";
    case Type::Double: return "DOUBLE";
    case Type::Boolean: return "BOOLEAN";
    case Type::Date: return "DATE";
    case Type::Datetime: return "DATETIME";
    case Type::Null: return "NULL";
    default: return "VARCHAR";
  }
}

std::string Schema::encode() const
{
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
  w.StartObject();
  auto field = [&](const char *key, const std::string &value) {
    w.Key(key); w.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
  };
  field("path", options.path); field("root", options.secure_root);
  field("format", options.format); field("nullstr", options.nullstr);
  w.Key("delimiter"); w.Int(static_cast<unsigned char>(options.delimiter));
  w.Key("quote"); w.Int(static_cast<unsigned char>(options.quote));
  w.Key("escape"); w.Int(static_cast<unsigned char>(options.escape));
  w.Key("header"); w.Bool(options.header);
  w.Key("size"); w.Uint64(size); w.Key("modified"); w.Int64(modified);
  w.Key("columns"); w.StartArray();
  for (const Column &column : columns) {
    w.StartArray(); w.String(column.name.data(), static_cast<rapidjson::SizeType>(column.name.size()));
    w.Int(static_cast<int>(column.type)); w.Bool(column.nullable); w.EndArray();
  }
  w.EndArray(); w.EndObject();
  return std::string(DESCRIPTOR_PREFIX) + std::string(buffer.GetString(), buffer.GetSize());
}

bool Schema::decode(const std::string &encoded, Schema &schema)
{
  const std::string prefix(DESCRIPTOR_PREFIX);
  if (encoded.compare(0, prefix.size(), prefix) != 0) { return false; }
  rapidjson::Document d;
  d.Parse(encoded.data() + prefix.size(), encoded.size() - prefix.size());
  if (d.HasParseError() || !d.IsObject()) { return false; }
  for (const char *key : {"path", "root", "format", "nullstr"}) {
    if (!d.HasMember(key) || !d[key].IsString()) { return false; }
  }
  for (const char *key : {"delimiter", "quote", "escape"}) {
    if (!d.HasMember(key) || !d[key].IsInt() || d[key].GetInt() < 0 || d[key].GetInt() > 255) { return false; }
  }
  if (!d.HasMember("header") || !d["header"].IsBool()
      || !d.HasMember("size") || !d["size"].IsUint64()
      || !d.HasMember("modified") || !d["modified"].IsInt64()
      || !d.HasMember("columns") || !d["columns"].IsArray()
      || d["columns"].Size() > MAX_COLUMNS) { return false; }
  schema = Schema();
  auto str = [&](const char *key) { return std::string(d[key].GetString(), d[key].GetStringLength()); };
  schema.options.path = str("path"); schema.options.secure_root = str("root");
  schema.options.format = str("format"); schema.options.nullstr = str("nullstr");
  schema.options.header = d["header"].GetBool();
  schema.options.delimiter = static_cast<char>(d["delimiter"].GetInt());
  schema.options.quote = static_cast<char>(d["quote"].GetInt());
  schema.options.escape = static_cast<char>(d["escape"].GetInt());
  schema.size = d["size"].GetUint64(); schema.modified = d["modified"].GetInt64();
  for (const auto &c : d["columns"].GetArray()) {
    if (!c.IsArray() || c.Size() != 3 || !c[0].IsString() || !c[1].IsInt()
        || c[1].GetInt() < 0 || c[1].GetInt() > static_cast<int>(Type::Datetime) || !c[2].IsBool()) { return false; }
    schema.columns.push_back({std::string(c[0].GetString(), c[0].GetStringLength()),
        static_cast<Type>(c[1].GetInt()), c[2].GetBool()});
  }
  return true;
}

bool FileScan::parse_time(const std::string &s, int64_t &micros, bool &date_only)
{
  using namespace std::chrono;
  date_only = s.size() == 10;
  if (!date_only && (s.size() < 19 || s.size() > 26)) { return false; }
  if (s.size() < 10 || s[4] != '-' || s[7] != '-') { return false; }
  auto digits = [&](size_t pos, size_t n, int &value) {
    value = 0;
    for (size_t i = pos; i < pos + n; ++i) {
      if (s[i] < '0' || s[i] > '9') { return false; }
      value = value * 10 + s[i] - '0';
    }
    return true;
  };
  int y, m, d, h = 0, minute = 0, second = 0, fraction = 0;
  if (!digits(0, 4, y) || !digits(5, 2, m) || !digits(8, 2, d) || y < 1) { return false; }
  year_month_day day{year{y}, month{static_cast<unsigned>(m)}, std::chrono::day{static_cast<unsigned>(d)}};
  if (!day.ok()) { return false; }
  if (!date_only) {
    if ((s[10] != ' ' && s[10] != 'T') || s[13] != ':' || s[16] != ':'
        || !digits(11, 2, h) || !digits(14, 2, minute) || !digits(17, 2, second)
        || h > 23 || minute > 59 || second > 59) { return false; }
    if (s.size() > 19) {
      if (s[19] != '.' || s.size() == 20 || !digits(20, s.size() - 20, fraction)) { return false; }
      for (size_t n = s.size() - 20; n < 6; ++n) { fraction *= 10; }
    }
  }
  micros = duration_cast<microseconds>(sys_days{day}.time_since_epoch()).count()
      + (static_cast<int64_t>(h) * 3600 + minute * 60 + second) * 1000000 + fraction;
  return true;
}

Type FileScan::classify(const std::string &s)
{
  if (s == "true" || s == "false" || s == "TRUE" || s == "FALSE") { return Type::Boolean; }
  int64_t integer;
  auto i = std::from_chars(s.data(), s.data() + s.size(), integer);
  if (i.ec == std::errc() && i.ptr == s.data() + s.size()) { return Type::Bigint; }
  // An integral token outside BIGINT must retain every digit as text.
  size_t start = (!s.empty() && (s[0] == '-' || s[0] == '+')) ? 1 : 0;
  bool integral = start < s.size();
  for (size_t p = start; integral && p < s.size(); ++p) { integral = s[p] >= '0' && s[p] <= '9'; }
  if (integral) { return Type::Varchar; }
  double real;
  auto r = std::from_chars(s.data(), s.data() + s.size(), real);
  if (r.ec == std::errc() && r.ptr == s.data() + s.size() && std::isfinite(real)) { return Type::Double; }
  int64_t time;
  bool date;
  if (parse_time(s, time, date)) { return date ? Type::Date : Type::Datetime; }
  return Type::Varchar;
}

Type FileScan::widen(Type left, Type right)
{
  if (left == right || right == Type::Null) { return left; }
  if (left == Type::Null) { return right; }
  if ((left == Type::Bigint && right == Type::Double)
      || (left == Type::Double && right == Type::Bigint)) { return Type::Double; }
  if ((left == Type::Date && right == Type::Datetime)
      || (left == Type::Datetime && right == Type::Date)) { return Type::Datetime; }
  return Type::Varchar;
}

bool FileScan::secure_path(const std::string &path, const std::string &root,
                          std::string &canonical, std::string &error)
{
  if (path.empty() || root.empty() || lower(root) == "null" || path.find('\0') != std::string::npos
      || root.find('\0') != std::string::npos || path.find("://") != std::string::npos) {
    error = "file SQL requires a local path and nonempty secure_file_priv"; return false;
  }
  std::error_code ec;
  fs::path base = fs::canonical(fs::u8path(root), ec);
  if (ec || !fs::is_directory(base, ec)) { error = "invalid secure_file_priv directory"; return false; }
  fs::path target = fs::canonical(fs::u8path(path), ec);
  if (ec) { error = "file does not exist"; return false; }
  auto b = base.begin(); auto t = target.begin();
  for (; b != base.end() && t != target.end(); ++b, ++t) {
#ifdef _WIN32
    if (lower(utf8(*b)) != lower(utf8(*t))) { break; }
#else
    if (*b != *t) { break; }
#endif
  }
  if (b != base.end()) { error = "path escapes secure_file_priv"; return false; }
  canonical = utf8(target);
  return true;
}

FileScan::~FileScan() { close(); }
void FileScan::close() { delete parquet_; parquet_ = nullptr; if (stream_.is_open()) { stream_.close(); } stream_.clear(); }
bool FileScan::fail(const std::string &message)
{
  error_ = schema_.options.path + ":" + std::to_string(line_) + ": " + message;
  return false;
}
bool FileScan::unchanged()
{
  uint64_t size; int64_t modified;
  return (signature(schema_.options.path, size, modified) && size == schema_.size
      && modified == schema_.modified) || fail("file changed during query; retry with a new plan");
}
bool FileScan::open_input()
{
  close(); line_ = 0;
  std::string canonical, error;
  if (!secure_path(schema_.options.path, schema_.options.secure_root, canonical, error)) { return fail(error); }
  if (canonical != schema_.options.path) { return fail("file path changed"); }
  if (!unchanged()) { return false; }
#ifndef _WIN32
  // Open the inode first, then validate the kernel-resolved target. An attacker
  // cannot redirect an intermediate directory symlink between check and read.
  int descriptor = ::open(canonical.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) { return fail("cannot open input file"); }
  std::string fd_path = "/proc/self/fd/" + std::to_string(descriptor);
  char actual[8192];
  ssize_t length = ::readlink(fd_path.c_str(), actual, sizeof(actual));
  if (length < 0 || static_cast<size_t>(length) == sizeof(actual)
      || std::string(actual, length) != canonical) {
    ::close(descriptor); return fail("file target changed during open");
  }
  stream_.open(fd_path, std::ios::binary);
  ::close(descriptor);
#else
  stream_.open(fs::u8path(canonical), std::ios::binary);
#endif
  if (!stream_.is_open()) { return fail("cannot open input file"); }
  // Strip only a leading UTF-8 BOM.
  char bom[3] = {};
  stream_.read(bom, 3);
  if (stream_.gcount() != 3 || static_cast<unsigned char>(bom[0]) != 0xef
      || static_cast<unsigned char>(bom[1]) != 0xbb || static_cast<unsigned char>(bom[2]) != 0xbf) {
    stream_.clear(); stream_.seekg(0);
  }
  return true;
}
bool FileScan::csv_record(std::vector<Cell> &row, bool &end)
{
  row.clear(); end = false;
  Cell cell; cell.null = false;
  bool in_quote = false, after_quote = false, any = false;
  size_t bytes = 0;
  const Options &o = schema_.options;
  auto push = [&]() {
    cell.null = !cell.quoted && cell.text == o.nullstr;
    row.push_back(std::move(cell)); cell = Cell(); cell.null = false;
    after_quote = false;
  };
  for (;;) {
    int value = stream_.get();
    if (value == std::char_traits<char>::eof()) {
      if (stream_.bad()) { return fail("input read failed"); }
      if (in_quote) { return fail("unterminated quoted CSV field"); }
      if (!any && row.empty() && cell.text.empty()) { end = true; return true; }
      push(); ++line_;
      for (const Cell &value : row) { if (!valid_utf8(value.text)) { return fail("invalid UTF-8 CSV field"); } }
      return true;
    }
    any = true;
    if (++bytes > MAX_RECORD) { return fail("record exceeds 16 MiB limit"); }
    char c = static_cast<char>(value);
    if (in_quote) {
      if (c == o.escape && o.escape != o.quote) {
        int escaped = stream_.get();
        if (escaped == std::char_traits<char>::eof()) { return fail("unterminated CSV escape"); }
        cell.text.push_back(static_cast<char>(escaped)); ++bytes;
      } else if (c == o.quote) {
        if (o.escape == o.quote && stream_.peek() == o.quote) { stream_.get(); cell.text.push_back(c); ++bytes; }
        else { in_quote = false; after_quote = true; }
      } else if (c == '\r' && stream_.peek() == '\n') { stream_.get(); cell.text.push_back('\n'); ++bytes; }
      else { cell.text.push_back(c); }
    } else if (c == o.delimiter) {
      push();
      if (row.size() >= MAX_COLUMNS) { return fail("too many columns"); }
    } else if (c == '\r' || c == '\n') {
      if (c == '\r' && stream_.peek() == '\n') { stream_.get(); }
      push(); ++line_;
      for (const Cell &value : row) { if (!valid_utf8(value.text)) { return fail("invalid UTF-8 CSV field"); } }
      return true;
    } else if (!after_quote && cell.text.empty() && c == o.quote && !cell.quoted) {
      in_quote = true; cell.quoted = true;
    } else if (after_quote || c == o.quote) { return fail("unexpected character in CSV field"); }
    else { cell.text.push_back(c); }
  }
}

bool FileScan::json_record(std::vector<std::string> &names, std::vector<Cell> &row, bool &end)
{
  names.clear(); row.clear(); end = false;
  std::string line;
  for (;;) {
    line.clear();
    int c;
    while ((c = stream_.get()) != std::char_traits<char>::eof() && c != '\n') {
      if (line.size() == MAX_RECORD) { return fail("JSONL record exceeds 16 MiB limit"); }
      line.push_back(static_cast<char>(c));
    }
    if (stream_.bad()) { return fail("input read failed"); }
    if (c == std::char_traits<char>::eof() && line.empty()) { end = true; return true; }
    ++line_;
    if (line.find_first_not_of(" \t\r") != std::string::npos) { break; }
  }
  rapidjson::Document d;
  d.Parse<rapidjson::kParseValidateEncodingFlag>(line.data(), line.size());
  if (d.HasParseError() || !d.IsObject()) { return fail("JSONL requires one flat object per line"); }
  if (d.MemberCount() > MAX_COLUMNS) { return fail("too many JSON fields"); }
  std::unordered_set<std::string> keys;
  for (auto i = d.MemberBegin(); i != d.MemberEnd(); ++i) {
    std::string name(i->name.GetString(), i->name.GetStringLength());
    if (name.empty() || !keys.insert(lower(name)).second) { return fail("empty or duplicate JSON field"); }
    if (i->value.IsArray() || i->value.IsObject()) { return fail("nested JSON fields are not supported"); }
    Cell cell;
    cell.null = i->value.IsNull();
    if (i->value.IsString()) { cell.text.assign(i->value.GetString(), i->value.GetStringLength()); cell.quoted = true; }
    else if (!cell.null) {
      rapidjson::StringBuffer b; rapidjson::Writer<rapidjson::StringBuffer> w(b);
      i->value.Accept(w); cell.text.assign(b.GetString(), b.GetSize());
    }
    names.push_back(std::move(name)); row.push_back(std::move(cell));
  }
  return true;
}

bool FileScan::infer(const Options &options, Schema &schema)
{
  schema_ = Schema(); schema_.options = options; error_.clear(); inference_ = true;
  if (!valid_options(options)) { return fail("invalid delimiter, quote, escape or NULL text"); }
  std::string error, canonical;
  if (options.format == "glob") {
    if (options.path.size() > 4096 || options.path.find('\0') != std::string::npos) { return fail("invalid glob pattern"); }
    fs::path pattern = fs::u8path(options.path);
    if (!secure_path(utf8(pattern.parent_path()), options.secure_root, canonical, error)) { return fail(error); }
    schema_.options.path = canonical + "/" + utf8(pattern.filename());
    schema_.columns.push_back({"file", Type::Varchar, false});
    schema = schema_; return true;
  }
  if (!secure_path(options.path, options.secure_root, canonical, error)) { return fail(error); }
  schema_.options.path = canonical;
  if (!signature(canonical, schema_.size, schema_.modified)) { return fail("input must be a regular file"); }
  if (options.format == "parquet") {
    if (!open_input()) { return false; }
    parquet_ = new ParquetReader(stream_);
    if (!parquet_->read_schema(schema_)) { return fail(parquet_->error()); }
    if (!unchanged()) { return false; }
    schema = schema_; close(); inference_ = false; return true;
  }
  if (options.format != "csv" && options.format != "json") { return fail("unsupported file format"); }
  if (!open_input()) { return false; }
  bool end = false;
  std::vector<Cell> row;
  std::vector<std::string> names;
  uint64_t rows = 0;
  if (options.format == "csv") {
    if (!csv_record(row, end)) { return false; }
    if (end) { return fail("cannot infer schema from an empty CSV"); }
    std::unordered_set<std::string> used;
    for (size_t i = 0; i < row.size(); ++i) {
      std::string base = options.header && !row[i].text.empty() ? row[i].text : "column" + std::to_string(i);
      if (base.size() > 256 || base.find('\0') != std::string::npos) { return fail("invalid column name"); }
      std::string name = base;
      for (unsigned suffix = 2; !used.insert(lower(name)).second; ++suffix) { name = base + "__" + std::to_string(suffix); }
      schema_.columns.push_back({name, Type::Null, false});
    }
    if (!options.header) {
      for (size_t i = 0; i < row.size(); ++i) {
        merge_value(schema_.columns[i], row[i], false);
      }
      ++rows;
    }
  }
  for (;;) {
    if (options.format == "csv") {
      if (!csv_record(row, end)) { return false; }
      if (end) { break; }
      if (row.size() != schema_.columns.size()) { return fail("CSV field count does not match schema"); }
      for (size_t i = 0; i < row.size(); ++i) {
        Column &column = schema_.columns[i];
        merge_value(column, row[i], false);
      }
    } else {
      if (!json_record(names, row, end)) { return false; }
      if (end) { break; }
      std::vector<bool> seen(schema_.columns.size(), false);
      for (size_t i = 0; i < names.size(); ++i) {
        size_t index = 0;
        while (index < schema_.columns.size() && schema_.columns[index].name != names[i]) { ++index; }
        if (index == schema_.columns.size()) {
          if (index == MAX_COLUMNS) { return fail("schema exceeds 1024 columns"); }
          schema_.columns.push_back({names[i], Type::Null, rows > 0}); seen.push_back(false);
        }
        seen[index] = true;
        Column &column = schema_.columns[index];
        merge_value(column, row[i], true);
      }
      for (size_t i = 0; i < seen.size(); ++i) { if (!seen[i]) { schema_.columns[i].nullable = true; } }
    }
    ++rows;
  }
  if (!unchanged()) { return false; }
  if (schema_.columns.empty()) { return fail("no columns found"); }
  for (Column &column : schema_.columns) { if (column.type == Type::Null) { column.nullable = true; } }
  schema = schema_; close(); inference_ = false;
  return true;
}

void FileScan::project(const std::vector<size_t> &columns)
{
  has_projection_ = true; projection_ = columns;
  if (parquet_) { parquet_->project(columns); }
}

bool FileScan::open(const Schema &schema)
{
  schema_ = schema; error_.clear(); inference_ = false;
  has_projection_ = false; projection_.clear();
  return rescan();
}
bool FileScan::rescan()
{
  if (schema_.options.format == "glob") {
    files_.clear(); file_index_ = 0;
    fs::path pattern = fs::u8path(schema_.options.path);
    const std::string filename_pattern = utf8(pattern.filename());
    std::error_code ec;
    fs::directory_iterator it(pattern.parent_path(), ec), end;
    if (ec) { return fail("cannot discover directory"); }
    while (it != end) {
      if (wildcard(filename_pattern, utf8(it->path().filename())) && it->is_regular_file(ec)) {
        std::string canonical, error;
        if (!secure_path(utf8(it->path()), schema_.options.secure_root, canonical, error)) { return fail(error); }
        files_.push_back(canonical);
        if (files_.size() > 100000) { return fail("glob exceeds 100000 files"); }
      }
      it.increment(ec);
      if (ec) { return fail("directory changed during discovery"); }
    }
    std::sort(files_.begin(), files_.end()); return true;
  }
  if (!open_input()) { return false; }
  if (schema_.options.format == "parquet") {
    parquet_ = new ParquetReader(stream_);
    Schema current = schema_;
    if (!parquet_->read_schema(current)) { return fail(parquet_->error()); }
    if (current.encode() != schema_.encode()) { return fail("Parquet schema changed"); }
    if (has_projection_) { parquet_->project(projection_); }
  }
  if (schema_.options.format == "csv" && schema_.options.header) {
    std::vector<Cell> ignored; bool end;
    if (!csv_record(ignored, end) || end) { return fail("missing CSV header"); }
  }
  return true;
}
bool FileScan::next(std::vector<Cell> &row, bool &end)
{
  if (schema_.options.format == "glob") {
    end = file_index_ == files_.size(); row.clear();
    if (!end) { row.push_back({files_[file_index_++], false, false}); }
    return true;
  }
  if (!unchanged()) { return false; }
  if (schema_.options.format == "parquet") {
    if (!parquet_->next(row, end)) { return fail(parquet_->error()); }
    return !end || unchanged();
  }
  if (schema_.options.format == "csv") {
    if (!csv_record(row, end)) { return false; }
    if (!end && row.size() != schema_.columns.size()) { return fail("CSV field count changed"); }
  } else {
    std::vector<std::string> names; std::vector<Cell> values;
    if (!json_record(names, values, end)) { return false; }
    row.assign(schema_.columns.size(), Cell());
    for (size_t i = 0; !end && i < names.size(); ++i) {
      size_t index = 0;
      while (index < schema_.columns.size() && schema_.columns[index].name != names[i]) { ++index; }
      if (index == schema_.columns.size()) { return fail("JSON schema changed"); }
      row[index] = std::move(values[i]);
    }
  }
  return !end || unchanged();
}

CsvCopyWriter::~CsvCopyWriter() { abort(); }
bool CsvCopyWriter::fail(const std::string &message) { error_ = message; return false; }
bool CsvCopyWriter::open(const Schema &schema)
{
  abort(); error_.clear(); schema_ = schema;
  if (!valid_options(schema.options)) { return fail("invalid COPY delimiter, quote, escape or NULL text"); }
  fs::path path = fs::u8path(schema.options.path);
  target_ = utf8(path.filename());
  if (target_.empty() || target_ == "." || target_ == ".." || target_.find('\0') != std::string::npos) {
    return fail("invalid COPY output filename");
  }
  std::string error;
  if (!FileScan::secure_path(utf8(path.parent_path().empty() ? fs::path(".") : path.parent_path()),
      schema.options.secure_root, parent_, error)) { return fail(error); }
  std::error_code ec;
  if (fs::exists(fs::u8path(parent_) / fs::u8path(target_), ec) || ec) { return fail("COPY target already exists"); }
#ifndef _WIN32
  directory_ = ::open(parent_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory_ < 0) { return fail("cannot open COPY output directory"); }
  char resolved[8192];
  std::string descriptor = "/proc/self/fd/" + std::to_string(directory_);
  ssize_t length = ::readlink(descriptor.c_str(), resolved, sizeof(resolved));
  if (length < 0 || static_cast<size_t>(length) == sizeof(resolved) || std::string(resolved, length) != parent_) {
    abort(); return fail("COPY output directory changed");
  }
#endif
  static std::atomic<uint64_t> sequence{0};
  for (unsigned attempt = 0; attempt < 32 && !file_; ++attempt) {
    temporary_ = ".seekdb-copy-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
        + "-" + std::to_string(sequence.fetch_add(1)) + ".tmp";
#ifndef _WIN32
    int descriptor = ::openat(directory_, temporary_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor >= 0) { file_ = ::fdopen(descriptor, "wb"); if (!file_) { ::close(descriptor); ::unlinkat(directory_, temporary_.c_str(), 0); } }
#else
    fs::path output = fs::u8path(parent_) / fs::u8path(temporary_);
    file_ = ::_wfopen(output.c_str(), L"wbx");
#endif
  }
  if (!file_) { temporary_.clear(); abort(); return fail("cannot create exclusive COPY temporary file"); }
  if (schema.options.header) {
    std::vector<Cell> header;
    for (const Column &column : schema.columns) { header.push_back({column.name, false, false}); }
    if (!append(header)) { abort(); return false; }
  }
  return true;
}
bool CsvCopyWriter::append(const std::vector<Cell> &row)
{
  if (!file_ || row.size() != schema_.columns.size()) { return fail("invalid COPY row"); }
  const Options &o = schema_.options;
  std::string record;
  for (size_t i = 0; i < row.size(); ++i) {
    if (i) { record.push_back(o.delimiter); }
    const Cell &cell = row[i];
    if (cell.null) { record += o.nullstr; }
    else {
      const bool quote = cell.text == o.nullstr || cell.text.find(o.delimiter) != std::string::npos
          || cell.text.find(o.quote) != std::string::npos || cell.text.find('\n') != std::string::npos
          || cell.text.find('\r') != std::string::npos;
      if (quote) { record.push_back(o.quote); }
      for (char c : cell.text) {
        if (quote && (c == o.quote || (o.escape != o.quote && c == o.escape))) { record.push_back(o.escape); }
        record.push_back(c);
      }
      if (quote) { record.push_back(o.quote); }
    }
    if (record.size() > MAX_RECORD) { return fail("COPY row exceeds 16 MiB limit"); }
  }
  record.push_back('\n');
  return std::fwrite(record.data(), 1, record.size(), file_) == record.size() || fail("COPY write failed");
}
bool CsvCopyWriter::commit()
{
  if (!file_) { return fail("COPY is not open"); }
  bool ok = std::fflush(file_) == 0;
#ifndef _WIN32
  if (ok) { ok = ::fsync(::fileno(file_)) == 0; }
#endif
  if (std::fclose(file_) != 0) { ok = false; } file_ = nullptr;
  if (!ok) { abort(); return fail("COPY flush failed"); }
#ifndef _WIN32
  if (::linkat(directory_, temporary_.c_str(), directory_, target_.c_str(), 0) != 0) {
    abort(); return fail("COPY target exists or atomic publication failed");
  }
  ::unlinkat(directory_, temporary_.c_str(), 0); temporary_.clear();
  ::fsync(directory_); ::close(directory_); directory_ = -1;
#else
  std::error_code ec;
  fs::path temporary = fs::u8path(parent_) / fs::u8path(temporary_);
  fs::create_hard_link(temporary, fs::u8path(parent_) / fs::u8path(target_), ec);
  if (ec) { abort(); return fail("COPY target exists or atomic publication failed"); }
  fs::remove(temporary, ec); temporary_.clear();
#endif
  return true;
}
void CsvCopyWriter::abort()
{
  if (file_) { std::fclose(file_); file_ = nullptr; }
  if (!temporary_.empty()) {
#ifndef _WIN32
    if (directory_ >= 0) { ::unlinkat(directory_, temporary_.c_str(), 0); }
#else
    std::error_code ec; fs::remove(fs::u8path(parent_) / fs::u8path(temporary_), ec);
#endif
    temporary_.clear();
  }
#ifndef _WIN32
  if (directory_ >= 0) { ::close(directory_); directory_ = -1; }
#endif
}

}}} // namespace oceanbase::sql::file_sql
