/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#include "parquet_file_reader.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>

namespace oceanbase { namespace sql { namespace file_sql {
namespace {
constexpr size_t MAX_PAGE = 64 * 1024 * 1024;
constexpr size_t MAX_RETAINED = 128 * 1024 * 1024;
size_t cell_memory(const std::vector<Cell> &cells)
{
  size_t bytes = cells.capacity() * sizeof(Cell);
  for (const Cell &cell : cells) {
    if (cell.text.capacity() > MAX_RETAINED - std::min(bytes, MAX_RETAINED)) { return MAX_RETAINED + 1; }
    bytes += cell.text.capacity();
  }
  return bytes;
}
struct CompactValue {
  int64_t number = 0;
  std::string text;
  std::vector<CompactValue> list;
  std::map<int, CompactValue> fields;
  const CompactValue &at(int id) const {
    static const CompactValue missing;
    auto i = fields.find(id); return i == fields.end() ? missing : i->second;
  }
};
struct CompactReader {
  const unsigned char *data;
  size_t length;
  size_t pos = 0;
  size_t nodes = 0;
  bool varint(uint64_t &v) {
    v = 0;
    for (unsigned shift = 0; shift < 70 && pos < length; shift += 7) {
      unsigned char c = data[pos++];
      if (shift == 63 && (c & 0xfe)) { return false; }
      v |= static_cast<uint64_t>(c & 127) << shift;
      if (!(c & 128)) { return true; }
    }
    return false;
  }
  bool read(int type, CompactValue &v, unsigned depth = 0) {
    if (depth > 32 || ++nodes > 100000) { return false; }
    uint64_t n = 0;
    switch (type) {
      case 1: v.number = 1; return true;
      case 2: v.number = 0; return true;
      case 3: if (pos == length) { return false; } v.number = static_cast<int8_t>(data[pos++]); return true;
      case 4: case 5: case 6:
        if (!varint(n)) { return false; } v.number = static_cast<int64_t>((n >> 1) ^ (0ULL - (n & 1))); return true;
      case 7: if (length - pos < 8) { return false; } pos += 8; return true;
      case 8:
        if (!varint(n) || n > length - pos) { return false; }
        v.text.assign(reinterpret_cast<const char *>(data + pos), n); pos += n; return true;
      case 9: case 10: {
        if (pos == length) { return false; }
        unsigned char header = data[pos++]; n = header >> 4;
        if (n == 15 && !varint(n)) { return false; }
        if (n > 100000 - nodes) { return false; }
        nodes += n; // Account for list storage before allocating its elements.
        v.list.resize(n);
        for (auto &item : v.list) { if (!read(header & 15, item, depth + 1)) { return false; } }
        return true;
      }
      case 11: {
        if (!varint(n) || n > 1000000) { return false; }
        if (!n) { return true; }
        if (pos == length) { return false; }
        unsigned char types = data[pos++];
        for (uint64_t i = 0; i < n; ++i) {
          CompactValue key, value;
          if (!read(types >> 4, key, depth + 1) || !read(types & 15, value, depth + 1)) { return false; }
        }
        return true;
      }
      case 12: {
        int previous = 0;
        while (pos < length) {
          unsigned char header = data[pos++];
          if (!header) { return true; }
          int id = previous + (header >> 4);
          if (!(header >> 4)) {
            if (!varint(n)) { return false; }
            int64_t decoded = static_cast<int64_t>((n >> 1) ^ (0ULL - (n & 1)));
            if (decoded < 0 || decoded > 32767) { return false; }
            id = static_cast<int>(decoded);
          }
          if (v.fields.count(id) || !read(header & 15, v.fields[id], depth + 1)) { return false; }
          previous = id;
        }
        return false;
      }
      default: return false;
    }
  }
};
uint32_t little32(const unsigned char *p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8
      | static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}
bool snappy(const std::vector<unsigned char> &input, size_t expected, std::vector<unsigned char> &output)
{
  CompactReader r{input.data(), input.size()}; uint64_t length;
  if (!r.varint(length) || length != expected || expected > MAX_PAGE) { return false; }
  output.clear(); output.reserve(expected);
  while (r.pos < r.length && output.size() < expected) {
    unsigned char tag = r.data[r.pos++]; size_t count = 0, offset = 0;
    if ((tag & 3) == 0) {
      count = tag >> 2;
      if (count < 60) { ++count; }
      else {
        unsigned bytes = static_cast<unsigned>(count - 59); count = 0;
        if (bytes > 4 || bytes > r.length - r.pos) { return false; }
        for (unsigned i = 0; i < bytes; ++i) { count |= static_cast<size_t>(r.data[r.pos++]) << (8 * i); }
        ++count;
      }
      if (count > r.length - r.pos || count > expected - output.size()) { return false; }
      output.insert(output.end(), r.data + r.pos, r.data + r.pos + count); r.pos += count;
    } else {
      unsigned bytes = (tag & 3) == 1 ? 1 : (tag & 3) == 2 ? 2 : 4;
      if (bytes > r.length - r.pos) { return false; }
      if ((tag & 3) == 1) {
        count = 4 + ((tag >> 2) & 7); offset = (static_cast<size_t>(tag & 0xe0) << 3) | r.data[r.pos++];
      } else {
        count = 1 + (tag >> 2);
        for (unsigned i = 0; i < bytes; ++i) { offset |= static_cast<size_t>(r.data[r.pos++]) << (8 * i); }
      }
      if (!offset || offset > output.size() || count > expected - output.size()) { return false; }
      for (size_t i = 0; i < count; ++i) { output.push_back(output[output.size() - offset]); }
    }
  }
  return r.pos == r.length && output.size() == expected;
}
bool hybrid(const unsigned char *data, size_t length, unsigned width, size_t count, std::vector<uint32_t> &values)
{
  if (width > 32 || count > 1000000) { return false; }
  CompactReader r{data, length}; values.clear(); values.reserve(count);
  if (!width) { values.assign(count, 0); return true; }
  while (values.size() < count) {
    uint64_t header;
    if (!r.varint(header) || !header) { return false; }
    if (!(header & 1)) {
      uint64_t run = header >> 1; unsigned bytes = (width + 7) / 8; uint32_t value = 0;
      if (bytes > r.length - r.pos || run > count - values.size()) { return false; }
      for (unsigned i = 0; i < bytes; ++i) { value |= static_cast<uint32_t>(r.data[r.pos++]) << (8 * i); }
      if (width < 32 && value >= (1u << width)) { return false; }
      values.insert(values.end(), run, value);
    } else {
      uint64_t groups = header >> 1;
      if (groups > (r.length - r.pos) / width) { return false; }
      uint64_t run = groups * 8;
      for (uint64_t i = 0; i < run && values.size() < count; ++i) {
        uint32_t value = 0;
        for (unsigned bit = 0; bit < width; ++bit) {
          uint64_t index = i * width + bit;
          value |= static_cast<uint32_t>((r.data[r.pos + index / 8] >> (index % 8)) & 1) << bit;
        }
        values.push_back(value);
      }
      r.pos += groups * width;
    }
  }
  return true;
}
std::string day_text(int64_t days)
{
  using namespace std::chrono;
  year_month_day day{sys_days{std::chrono::days{days}}};
  char text[16]; std::snprintf(text, sizeof(text), "%04d-%02u-%02u", int(day.year()), unsigned(day.month()), unsigned(day.day()));
  return text;
}
}

struct ParquetReader::Impl {
  struct Field { int type = -1; int converted = -1; bool optional = false; int width = 0; };
  struct Chunk { int type = -1; int codec = 0; uint64_t offset = 0, bytes = 0, count = 0; };
  struct Group { uint64_t rows = 0; std::vector<Chunk> chunks; };
  struct Cursor { uint64_t offset = 0, end = 0; std::vector<Cell> dictionary, page; size_t index = 0; };
  explicit Impl(std::ifstream &s) : stream(s) {}
  std::ifstream &stream;
  std::string message;
  uint64_t size = 0, rows = 0, group_row = 0;
  size_t group = 0;
  std::vector<Field> fields;
  std::vector<Group> groups;
  std::vector<Cursor> cursors;
  std::vector<bool> projection;
  size_t retained_memory() const {
    size_t bytes = 0;
    for (const Cursor &cursor : cursors) {
      bytes += cell_memory(cursor.dictionary) + cell_memory(cursor.page);
      if (bytes > MAX_RETAINED) { break; }
    }
    return bytes;
  }
  bool fail(const char *text) { message = text; return false; }
  bool read(uint64_t offset, size_t length, std::vector<unsigned char> &buffer) {
    if (offset > size || length > size - offset || length > MAX_PAGE) { return fail("invalid Parquet byte range"); }
    buffer.resize(length); stream.clear(); stream.seekg(offset);
    stream.read(reinterpret_cast<char *>(buffer.data()), length);
    return stream.gcount() == static_cast<std::streamsize>(length) || fail("truncated Parquet file");
  }
  bool plain(const unsigned char *data, size_t length, size_t count, const Field &field, std::vector<Cell> &out) {
    std::vector<Cell>().swap(out);
    size_t used = retained_memory() + count * sizeof(Cell);
    if (used > MAX_RETAINED) { return fail("Parquet retained page memory exceeds 128 MiB"); }
    out.reserve(count); size_t pos = 0;
    for (size_t i = 0; i < count; ++i) {
      Cell cell; cell.null = false;
      size_t bytes = field.type == 1 || field.type == 4 ? 4 : field.type == 2 || field.type == 5 ? 8 : field.type == 7 ? field.width : 0;
      if (bytes > length - pos) { return fail("truncated Parquet PLAIN value"); }
      if (field.type == 0) {
        if (i / 8 >= length) { return fail("truncated Parquet boolean page"); }
        cell.text = (data[i / 8] >> (i % 8)) & 1 ? "true" : "false";
      } else if (field.type == 1 || field.type == 2) {
        uint64_t bits = 0;
        for (size_t b = 0; b < bytes; ++b) { bits |= static_cast<uint64_t>(data[pos + b]) << (8 * b); }
        int64_t value = bytes == 4 ? static_cast<int32_t>(bits) : static_cast<int64_t>(bits);
        if (field.converted == 6) { cell.text = day_text(value); }
        else if (field.converted == 9 || field.converted == 10 || field.converted == 20) {
          int64_t us = field.converted == 20 ? value / 1000 : value;
          if (field.converted == 9) {
            if (value > INT64_MAX / 1000 || value < INT64_MIN / 1000) { return fail("Parquet timestamp overflow"); }
            us *= 1000;
          }
          int64_t days = us / 86400000000LL, time = us % 86400000000LL;
          if (time < 0) { --days; time += 86400000000LL; }
          char text[40]; std::snprintf(text, sizeof(text), " %02lld:%02lld:%02lld.%06lld",
              static_cast<long long>(time / 3600000000LL), static_cast<long long>(time / 60000000LL % 60),
              static_cast<long long>(time / 1000000 % 60), static_cast<long long>(time % 1000000));
          cell.text = day_text(days) + text;
        } else if (field.converted >= 11 && field.converted <= 14) { cell.text = std::to_string(bits); }
        else { cell.text = std::to_string(value); }
      } else if (field.type == 4 || field.type == 5) {
        double value;
        if (field.type == 4) { float f; std::memcpy(&f, data + pos, 4); value = f; }
        else { std::memcpy(&value, data + pos, 8); }
        if (!std::isfinite(value)) { return fail("non-finite Parquet number"); }
        char text[64]; auto end = std::to_chars(text, text + sizeof(text), value);
        if (end.ec != std::errc()) { return fail("invalid Parquet number"); }
        cell.text.assign(text, end.ptr);
      } else if (field.type == 6 || field.type == 7) {
        if (field.type == 6) {
          if (length - pos < 4) { return fail("truncated Parquet byte array length"); }
          bytes = little32(data + pos); pos += 4;
        }
        if (bytes > length - pos || bytes > 16 * 1024 * 1024) { return fail("oversized Parquet string"); }
        cell.text.assign(reinterpret_cast<const char *>(data + pos), bytes);
      } else { return fail("unsupported Parquet physical type"); }
      if (field.type != 0) { pos += bytes; }
      if (cell.text.capacity() > MAX_RETAINED - used) { return fail("Parquet decoded values exceed 128 MiB"); }
      used += cell.text.capacity();
      out.push_back(std::move(cell));
    }
    return true;
  }
  bool page(size_t column) {
    Cursor &cursor = cursors[column]; const Chunk &chunk = groups[group].chunks[column]; const Field &field = fields[column];
    std::vector<Cell>().swap(cursor.page);
    while (cursor.offset < cursor.end) {
      std::vector<unsigned char> header_bytes;
      if (!read(cursor.offset, std::min<uint64_t>(65536, cursor.end - cursor.offset), header_bytes)) { return false; }
      CompactReader r{header_bytes.data(), header_bytes.size()}; CompactValue header;
      if (!r.read(12, header)) { return fail("invalid Parquet page header"); }
      int64_t compressed = header.at(3).number, uncompressed = header.at(2).number;
      if (compressed < 0 || uncompressed < 0 || compressed > MAX_PAGE || uncompressed > MAX_PAGE
          || r.pos > cursor.end - cursor.offset || static_cast<uint64_t>(compressed) > cursor.end - cursor.offset - r.pos) {
        return fail("invalid Parquet page size");
      }
      std::vector<unsigned char> packed, decoded;
      if (!read(cursor.offset + r.pos, compressed, packed)) { return false; }
      cursor.offset += r.pos + compressed;
      const int type = header.at(1).number;
      const bool v2 = type == 3;
      size_t level_bytes = 0;
      if (v2) {
        const auto &h = header.at(8);
        int64_t definition = h.at(5).number, repetition = h.at(6).number;
        if (definition < 0 || repetition != 0 || static_cast<uint64_t>(definition) > packed.size()) { return fail("invalid Parquet V2 levels"); }
        level_bytes = definition;
      }
      bool is_compressed = chunk.codec != 0 && (!v2 || !header.at(8).fields.count(7) || header.at(8).at(7).number != 0);
      if (is_compressed) {
        if (chunk.codec != 1 || level_bytes > static_cast<uint64_t>(uncompressed)) { return fail("unsupported Parquet compression codec"); }
        std::vector<unsigned char> body(packed.begin() + level_bytes, packed.end());
        std::vector<unsigned char> raw;
        if (!snappy(body, uncompressed - level_bytes, raw)) { return fail("invalid Snappy page"); }
        decoded.assign(packed.begin(), packed.begin() + level_bytes); decoded.insert(decoded.end(), raw.begin(), raw.end());
      } else { decoded.swap(packed); }
      if (decoded.size() != static_cast<size_t>(uncompressed)) { return fail("Parquet page size mismatch"); }
      if (type == 2) {
        int64_t count = header.at(7).at(1).number;
        if (header.at(7).at(2).number != 0 || count < 0 || count > 1000000) { return fail("unsupported Parquet dictionary"); }
        if (!plain(decoded.data(), decoded.size(), count, field, cursor.dictionary)) { return false; }
        continue;
      }
      if (type != 0 && type != 3) { continue; }
      const auto &h = header.at(v2 ? 8 : 5);
      int64_t count = h.at(1).number, encoding = h.at(v2 ? 4 : 2).number;
      if (count <= 0 || count > 1000000) { return fail("invalid Parquet page value count"); }
      size_t pos = 0; std::vector<uint32_t> levels;
      if (field.optional) {
        if (!v2) {
          if (decoded.size() < 4 || h.at(3).number != 3) { return fail("unsupported Parquet definition encoding"); }
          level_bytes = little32(decoded.data()); pos = 4;
        }
        if (level_bytes > decoded.size() - pos || !hybrid(decoded.data() + pos, level_bytes, 1, count, levels)) {
          return fail("invalid Parquet definition levels");
        }
        pos += level_bytes;
      } else { levels.assign(count, 1); pos = level_bytes; }
      size_t nonnull = 0; for (auto level : levels) { if (level > 1) { return fail("invalid Parquet null level"); } nonnull += level; }
      std::vector<Cell> values;
      if (encoding == 0) {
        if (!plain(decoded.data() + pos, decoded.size() - pos, nonnull, field, values)) { return false; }
      } else if (encoding == 3 && field.type == 0) {
        // RLE boolean values carry a four-byte length in both page versions.
        // https://parquet.apache.org/docs/file-format/data-pages/encodings/
        if (decoded.size() - pos < 4) { return fail("missing Parquet boolean RLE length"); }
        size_t length = little32(decoded.data() + pos); pos += 4;
        std::vector<uint32_t> booleans;
        if (length > decoded.size() - pos
            || !hybrid(decoded.data() + pos, length, 1, nonnull, booleans)) {
          return fail("invalid Parquet boolean RLE values");
        }
        for (uint32_t value : booleans) { values.push_back({value ? "true" : "false", false, false}); }
      } else if (encoding == 2 || encoding == 8) {
        if (pos >= decoded.size()) { if (nonnull) { return fail("missing Parquet dictionary indices"); } }
        else {
          std::vector<uint32_t> indices;
          unsigned width = decoded[pos++];
          if (!hybrid(decoded.data() + pos, decoded.size() - pos, width, nonnull, indices)) { return fail("invalid dictionary indices"); }
          size_t used = retained_memory() + nonnull * sizeof(Cell);
          if (used > MAX_RETAINED) { return fail("Parquet dictionary expansion exceeds 128 MiB"); }
          values.reserve(nonnull);
          for (auto index : indices) {
            if (index >= cursor.dictionary.size()) { return fail("Parquet dictionary index out of range"); }
            const size_t extra = cursor.dictionary[index].text.size() * 2 + 32;
            if (extra > MAX_RETAINED - used) { return fail("Parquet dictionary expansion exceeds 128 MiB"); }
            used += extra;
            values.push_back(cursor.dictionary[index]);
          }
        }
      } else { return fail("unsupported Parquet data encoding"); }
      if (retained_memory() + cell_memory(values) + count * sizeof(Cell) > MAX_RETAINED) {
        return fail("Parquet retained page memory exceeds 128 MiB");
      }
      cursor.page.reserve(count); cursor.index = 0;
      size_t value = 0;
      for (auto level : levels) { cursor.page.push_back(level ? std::move(values[value++]) : Cell()); }
      return true;
    }
    return fail("Parquet column ended before row group");
  }
};

ParquetReader::ParquetReader(std::ifstream &stream) : impl_(new Impl(stream)) {}
ParquetReader::~ParquetReader() = default;
const std::string &ParquetReader::error() const { return impl_->message; }
void ParquetReader::project(const std::vector<size_t> &columns)
{
  impl_->projection.assign(impl_->fields.size(), false);
  for (size_t column : columns) { if (column < impl_->projection.size()) { impl_->projection[column] = true; } }
}
bool ParquetReader::read_schema(Schema &schema)
{
  Impl &p = *impl_; p.size = schema.size;
  if (p.size < 12) { return p.fail("invalid Parquet file size"); }
  std::vector<unsigned char> footer, magic;
  if (!p.read(p.size - 8, 8, footer) || !p.read(0, 4, magic)) { return false; }
  if (std::memcmp(magic.data(), "PAR1", 4) || std::memcmp(footer.data() + 4, "PAR1", 4)) { return p.fail("invalid Parquet magic"); }
  uint32_t length = little32(footer.data());
  if (length > 16 * 1024 * 1024 || length > p.size - 12 || !p.read(p.size - 8 - length, length, footer)) { return p.fail("invalid Parquet footer size"); }
  CompactReader r{footer.data(), footer.size()}; CompactValue metadata;
  if (!r.read(12, metadata) || r.pos != r.length || metadata.at(3).number < 0) { return p.fail("invalid Parquet footer"); }
  const auto &fields = metadata.at(2).list;
  if (fields.size() < 2 || fields.size() > 1025 || fields[0].at(5).number != static_cast<int64_t>(fields.size() - 1)) { return p.fail("only flat Parquet schemas are supported"); }
  schema.columns.clear();
  for (size_t i = 1; i < fields.size(); ++i) {
    const auto &value = fields[i]; Impl::Field field;
    field.type = value.at(1).number; field.width = value.at(2).number;
    field.optional = value.at(3).number == 1;
    field.converted = value.fields.count(6) ? static_cast<int>(value.at(6).number) : -1;
    const auto &logical = value.at(10);
    if (logical.fields.count(6)) { field.converted = 6; }
    if (logical.fields.count(8)) {
      const auto &unit = logical.at(8).at(2);
      if (unit.fields.count(1)) { field.converted = 9; }
      else if (unit.fields.count(2)) { field.converted = 10; }
      else if (unit.fields.count(3)) { field.converted = 20; }
      else { return p.fail("unsupported Parquet timestamp unit"); }
    }
    if (logical.fields.count(9) && logical.at(9).at(2).number == 0) {
      field.converted = logical.at(9).at(1).number == 64 ? 14 : 13;
    }
    if (logical.fields.count(5) || field.converted == 5) { return p.fail("Parquet DECIMAL is not a basic file SQL type"); }
    if (value.at(3).number > 1 || value.at(5).number || !value.fields.count(1)
        || field.type == 3 || field.type < 0 || field.type > 7 || value.at(4).text.empty()) { return p.fail("unsupported Parquet field"); }
    Type type = field.type == 0 ? Type::Boolean : field.type == 1 || field.type == 2 ? Type::Bigint
        : field.type == 4 || field.type == 5 ? Type::Double : Type::Varchar;
    if (field.converted == 6) { type = Type::Date; }
    if (field.converted == 9 || field.converted == 10 || field.converted == 20) { type = Type::Datetime; }
    if (field.converted == 14) { type = Type::Varchar; }
    for (const auto &column : schema.columns) { if (column.name == value.at(4).text) { return p.fail("duplicate Parquet column"); } }
    schema.columns.push_back({value.at(4).text, type, field.optional}); p.fields.push_back(field);
  }
  uint64_t rows = 0;
  for (const auto &value : metadata.at(4).list) {
    Impl::Group group;
    if (value.at(3).number < 0 || value.at(1).list.size() != p.fields.size()) { return p.fail("invalid Parquet row group"); }
    group.rows = value.at(3).number;
    for (size_t i = 0; i < value.at(1).list.size(); ++i) {
      const auto &column = value.at(1).list[i]; const auto &m = column.at(3); Impl::Chunk chunk;
      if (!column.at(1).text.empty() || m.at(1).number != p.fields[i].type
          || m.at(5).number != static_cast<int64_t>(group.rows) || m.at(7).number < 0 || m.at(9).number < 4) { return p.fail("invalid Parquet column metadata"); }
      chunk.type = m.at(1).number; chunk.codec = m.at(4).number; chunk.count = m.at(5).number;
      chunk.offset = m.at(9).number;
      if (m.fields.count(11)) { if (m.at(11).number < 4) { return p.fail("invalid dictionary offset"); } chunk.offset = std::min<uint64_t>(chunk.offset, m.at(11).number); }
      chunk.bytes = m.at(7).number;
      if (chunk.offset > p.size - 8 - length || chunk.bytes > p.size - 8 - length - chunk.offset) { return p.fail("Parquet column exceeds file boundary"); }
      group.chunks.push_back(chunk);
    }
    if (group.rows > UINT64_MAX - rows) { return p.fail("Parquet row count overflow"); }
    rows += group.rows; p.groups.push_back(std::move(group));
  }
  if (rows != static_cast<uint64_t>(metadata.at(3).number)) { return p.fail("Parquet row count mismatch"); }
  p.rows = rows; p.projection.assign(p.fields.size(), true);
  return true;
}
bool ParquetReader::next(std::vector<Cell> &row, bool &end)
{
  Impl &p = *impl_; end = false;
  while (p.group < p.groups.size() && p.group_row == p.groups[p.group].rows) {
    ++p.group; p.group_row = 0; p.cursors.clear();
  }
  if (p.group == p.groups.size()) { end = true; return true; }
  if (p.cursors.empty()) {
    p.cursors.resize(p.fields.size());
    for (size_t i = 0; i < p.fields.size(); ++i) {
      p.cursors[i].offset = p.groups[p.group].chunks[i].offset;
      p.cursors[i].end = p.cursors[i].offset + p.groups[p.group].chunks[i].bytes;
    }
  }
  row.assign(p.fields.size(), Cell());
  for (size_t i = 0; i < p.fields.size(); ++i) {
    if (!p.projection[i]) { continue; }
    auto &cursor = p.cursors[i];
    if (cursor.index == cursor.page.size() && !p.page(i)) { return false; }
    row[i] = cursor.page[cursor.index++];
  }
  ++p.group_row;
  return true;
}
}}}
