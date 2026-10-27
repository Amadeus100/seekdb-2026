/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

namespace oceanbase { namespace share {
constexpr int64_t AI_COLUMN_SCHEMA_FLAG = INT64_C(1) << 4;
constexpr const char *AI_COLUMN_DEFINITION_PREFIX = "seekdb-ai-column-v1:";

// Stored as the final extended-type-info entry. Collection type information
// remains in entry zero; the existing schema copy and wire formats own strings.
struct AIColumnDefinition {
  std::string model;
  std::string input;
  std::string config = "NULL";
  std::string expression;
  std::string system_prompt;
  std::vector<std::string> dependencies;
  bool embedding = false;
  bool input_json = false;
  int64_t dimension = 0;
  int64_t max_retries = 2;
  int64_t result_type = 0;
  int64_t result_length = 0;
  uint64_t state_column = 0;
  uint64_t request_column = 0;
  uint64_t spec_column = 0;
  uint64_t version() const {
    const std::string data = expression + system_prompt + std::to_string(dimension) + std::to_string(max_retries);
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : data) { hash = (hash ^ c) * 1099511628211ULL; }
    return hash & INT64_MAX;
  }
  std::string encode() const {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
    w.StartObject();
    auto str = [&](const char *key, const std::string &value) {
      w.Key(key); w.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
    };
    str("model", model); str("input", input); str("config", config);
    str("expression", expression); str("system_prompt", system_prompt);
    w.Key("embedding"); w.Bool(embedding); w.Key("input_json"); w.Bool(input_json);
    w.Key("dimension"); w.Int64(dimension); w.Key("max_retries"); w.Int64(max_retries);
    w.Key("result_type"); w.Int64(result_type); w.Key("result_length"); w.Int64(result_length);
    w.Key("state_column"); w.Uint64(state_column); w.Key("request_column"); w.Uint64(request_column);
    w.Key("spec_column"); w.Uint64(spec_column);
    w.Key("dependencies"); w.StartArray();
    for (const auto &name : dependencies) { w.String(name.data(), static_cast<rapidjson::SizeType>(name.size())); }
    w.EndArray(); w.EndObject();
    return std::string(AI_COLUMN_DEFINITION_PREFIX) + std::string(buffer.GetString(), buffer.GetSize());
  }
  static bool decode(const std::string &encoded, AIColumnDefinition &definition) {
    const std::string prefix(AI_COLUMN_DEFINITION_PREFIX);
    if (encoded.compare(0, prefix.size(), prefix)) { return false; }
    rapidjson::Document d;
    d.Parse(encoded.data() + prefix.size(), encoded.size() - prefix.size());
    if (d.HasParseError() || !d.IsObject()) { return false; }
    for (const char *key : {"model", "input", "config", "expression", "system_prompt"}) {
      if (!d.HasMember(key) || !d[key].IsString()) { return false; }
    }
    for (const char *key : {"dimension", "max_retries", "result_type", "result_length"}) {
      if (!d.HasMember(key) || !d[key].IsInt64()) { return false; }
    }
    for (const char *key : {"state_column", "request_column", "spec_column"}) {
      if (!d.HasMember(key) || !d[key].IsUint64()) { return false; }
    }
    if (!d.HasMember("dependencies") || !d["dependencies"].IsArray()
        || !d.HasMember("embedding") || !d["embedding"].IsBool()
        || !d.HasMember("input_json") || !d["input_json"].IsBool()) { return false; }
    auto str = [&](const char *key) { return std::string(d[key].GetString(), d[key].GetStringLength()); };
    definition = AIColumnDefinition();
    definition.model = str("model"); definition.input = str("input"); definition.config = str("config");
    definition.expression = str("expression"); definition.system_prompt = str("system_prompt");
    definition.dimension = d["dimension"].GetInt64(); definition.max_retries = d["max_retries"].GetInt64();
    definition.result_type = d["result_type"].GetInt64(); definition.result_length = d["result_length"].GetInt64();
    definition.state_column = d["state_column"].GetUint64(); definition.request_column = d["request_column"].GetUint64();
    definition.spec_column = d["spec_column"].GetUint64();
    definition.embedding = d["embedding"].GetBool(); definition.input_json = d["input_json"].GetBool();
    for (const auto &name : d["dependencies"].GetArray()) {
      if (!name.IsString()) { return false; }
      definition.dependencies.emplace_back(name.GetString(), name.GetStringLength());
    }
    return definition.max_retries >= 0 && definition.max_retries <= 10
        && !definition.model.empty() && !definition.input.empty();
  }
};
}} // namespace oceanbase::share
