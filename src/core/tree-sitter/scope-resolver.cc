#include "scope-resolver.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include "parser.h"
#include "regex.h"
#include "strings.h"
#include "text-buffer.h"

using std::string;
using std::u16string;
using std::vector;

namespace tree_sitter {

namespace {

bool starts_with(const string &s, const char *prefix) {
  return s.compare(0, strlen(prefix), prefix) == 0;
}

bool contains(const string &s, const char *part) { return s.find(part) != string::npos; }

// JavaScript's `\s`.
bool is_js_space(uint32_t c) {
  return c == ' ' || (c >= 0x09 && c <= 0x0d) || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) ||
    c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000 || c == 0xfeff;
}

// `string.split(/\s+/)`, for ASCII-compatible UTF-8.
vector<string> split_on_spaces(const string &s) {
  vector<string> result;
  string current;
  size_t i = 0;
  while (i < s.size()) {
    if (is_js_space(static_cast<unsigned char>(s[i]))) {
      while (i < s.size() && is_js_space(static_cast<unsigned char>(s[i]))) i++;
      result.push_back(current);
      current.clear();
    } else {
      current.push_back(s[i++]);
    }
  }
  result.push_back(current);
  return result;
}

// `interpretPossibleKeyValuePair` without value coercion: splits on the first
// space.
void split_key_value(const string &raw, string *key, optional<string> *value) {
  size_t space = raw.find(' ');
  if (space == string::npos) {
    *key = raw;
    *value = optional<string>();
  } else {
    *key = raw.substr(0, space);
    *value = raw.substr(space + 1);
  }
}

// JavaScript's `Number(string)`.
double js_number(const optional<string> &value) {
  if (!value) return 0;  // Number(null)
  const string &raw = *value;
  size_t start = 0, end = raw.size();
  while (start < end && is_js_space(static_cast<unsigned char>(raw[start]))) start++;
  while (end > start && is_js_space(static_cast<unsigned char>(raw[end - 1]))) end--;
  string s = raw.substr(start, end - start);
  if (s.empty()) return 0;
  if (s == "Infinity" || s == "+Infinity") return INFINITY;
  if (s == "-Infinity") return -INFINITY;
  if (s.size() > 2 && s[0] == '0') {
    int base = s[1] == 'x' || s[1] == 'X' ? 16 : s[1] == 'o' || s[1] == 'O' ? 8 : s[1] == 'b' || s[1] == 'B' ? 2 : 0;
    if (base) {
      double result = 0;
      for (size_t i = 2; i < s.size(); i++) {
        char c = s[i];
        int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
        if (digit >= base) return NAN;
        result = result * base + digit;
      }
      return result;
    }
  }
  // A decimal literal: digits, an optional fraction and an optional exponent.
  size_t i = 0;
  if (s[i] == '+' || s[i] == '-') i++;
  size_t digits = 0;
  while (i < s.size() && isdigit(static_cast<unsigned char>(s[i]))) { i++; digits++; }
  if (i < s.size() && s[i] == '.') {
    i++;
    while (i < s.size() && isdigit(static_cast<unsigned char>(s[i]))) { i++; digits++; }
  }
  if (digits == 0) return NAN;
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    i++;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
    size_t exponent_digits = 0;
    while (i < s.size() && isdigit(static_cast<unsigned char>(s[i]))) { i++; exponent_digits++; }
    if (exponent_digits == 0) return NAN;
  }
  if (i != s.size()) return NAN;
  return strtod(s.c_str(), nullptr);
}

// `name.replace(pattern, replacement)` with a string pattern, including the
// replacement's `$` patterns.
string js_replace(const string &name, const string &pattern, const string &replacement) {
  size_t position = name.find(pattern);
  if (position == string::npos) return name;
  string substituted;
  for (size_t i = 0; i < replacement.size(); i++) {
    char c = replacement[i];
    if (c == '$' && i + 1 < replacement.size()) {
      char next = replacement[i + 1];
      if (next == '$') { substituted.push_back('$'); i++; continue; }
      if (next == '&') { substituted += pattern; i++; continue; }
      if (next == '`') { substituted += name.substr(0, position); i++; continue; }
      if (next == '\'') { substituted += name.substr(position + pattern.size()); i++; continue; }
    }
    substituted.push_back(c);
  }
  return name.substr(0, position) + substituted + name.substr(position + pattern.size());
}

// A node property path ending in a position, like
// `firstChild.nextSibling.endPosition` (see `resolveNodePosition`).
struct NodePositionDescriptor {
  enum class Step {
    Parent, FirstChild, LastChild, FirstNamedChild, LastNamedChild,
    NextSibling, PreviousSibling, NextNamedSibling, PreviousNamedSibling
  };
  vector<Step> steps;
  bool end = false;
  bool valid = false;

  static NodePositionDescriptor parse(const optional<string> &value) {
    NodePositionDescriptor result;
    if (!value) return result;
    vector<string> parts;
    size_t start = 0;
    while (true) {
      size_t dot = value->find('.', start);
      parts.push_back(value->substr(start, dot == string::npos ? string::npos : dot - start));
      if (dot == string::npos) break;
      start = dot + 1;
    }
    const string &last = parts.back();
    if (last == "startPosition") result.end = false;
    else if (last == "endPosition") result.end = true;
    else return result;
    for (size_t i = 0; i + 1 < parts.size(); i++) {
      const string &part = parts[i];
      if (part == "parent") result.steps.push_back(Step::Parent);
      else if (part == "firstChild") result.steps.push_back(Step::FirstChild);
      else if (part == "lastChild") result.steps.push_back(Step::LastChild);
      else if (part == "firstNamedChild") result.steps.push_back(Step::FirstNamedChild);
      else if (part == "lastNamedChild") result.steps.push_back(Step::LastNamedChild);
      else if (part == "nextSibling") result.steps.push_back(Step::NextSibling);
      else if (part == "previousSibling") result.steps.push_back(Step::PreviousSibling);
      else if (part == "nextNamedSibling") result.steps.push_back(Step::NextNamedSibling);
      else if (part == "previousNamedSibling") result.steps.push_back(Step::PreviousNamedSibling);
      else return result;
    }
    result.valid = true;
    return result;
  }

  optional<Point> resolve(TSNode node) const {
    if (!valid) return optional<Point>();
    for (Step step : steps) {
      uint32_t count;
      switch (step) {
        case Step::Parent: node = ts_node_parent(node); break;
        case Step::FirstChild: node = ts_node_child(node, 0); break;
        case Step::LastChild:
          count = ts_node_child_count(node);
          node = count ? ts_node_child(node, count - 1) : TSNode{};
          break;
        case Step::FirstNamedChild: node = ts_node_named_child(node, 0); break;
        case Step::LastNamedChild:
          count = ts_node_named_child_count(node);
          node = count ? ts_node_named_child(node, count - 1) : TSNode{};
          break;
        case Step::NextSibling: node = ts_node_next_sibling(node); break;
        case Step::PreviousSibling: node = ts_node_prev_sibling(node); break;
        case Step::NextNamedSibling: node = ts_node_next_named_sibling(node); break;
        case Step::PreviousNamedSibling: node = ts_node_prev_named_sibling(node); break;
      }
      if (ts_node_is_null(node)) return optional<Point>();
    }
    return point_from_ts(end ? ts_node_end_point(node) : ts_node_start_point(node));
  }
};

enum class TestKind {
  Type, HasError, Injection, Root, First, Last, FirstOfType, LastOfType, LastTextOnRow, FirstTextOnRow,
  DescendantOfType, ChildOfType, AncestorTypeNearerThan, AncestorOfType, ParentOfType, RangeWithData,
  DescendantOfNodeWithData, StartsOnSameRowAs, EndsOnSameRowAs, Config
};

const std::map<string, TestKind> TESTS = {
  {"type", TestKind::Type},
  {"hasError", TestKind::HasError},
  {"injection", TestKind::Injection},
  {"root", TestKind::Root},
  {"first", TestKind::First},
  {"last", TestKind::Last},
  {"firstOfType", TestKind::FirstOfType},
  {"lastOfType", TestKind::LastOfType},
  {"lastTextOnRow", TestKind::LastTextOnRow},
  {"firstTextOnRow", TestKind::FirstTextOnRow},
  {"descendantOfType", TestKind::DescendantOfType},
  {"childOfType", TestKind::ChildOfType},
  {"ancestorTypeNearerThan", TestKind::AncestorTypeNearerThan},
  {"ancestorOfType", TestKind::AncestorOfType},
  {"parentOfType", TestKind::ParentOfType},
  {"rangeWithData", TestKind::RangeWithData},
  {"descendantOfNodeWithData", TestKind::DescendantOfNodeWithData},
  {"startsOnSameRowAs", TestKind::StartsOnSameRowAs},
  {"endsOnSameRowAs", TestKind::EndsOnSameRowAs},
  {"config", TestKind::Config},
};

enum class AdjustmentKind {
  StartAt, EndAt, OffsetStart, OffsetEnd, StartAndEndAroundFirstMatchOf, StartBeforeFirstMatchOf,
  StartAfterFirstMatchOf, EndBeforeFirstMatchOf, EndAfterFirstMatchOf
};

const std::map<string, AdjustmentKind> ADJUSTMENTS = {
  {"startAt", AdjustmentKind::StartAt},
  {"endAt", AdjustmentKind::EndAt},
  {"offsetStart", AdjustmentKind::OffsetStart},
  {"offsetEnd", AdjustmentKind::OffsetEnd},
  {"startAndEndAroundFirstMatchOf", AdjustmentKind::StartAndEndAroundFirstMatchOf},
  {"startBeforeFirstMatchOf", AdjustmentKind::StartBeforeFirstMatchOf},
  {"startAfterFirstMatchOf", AdjustmentKind::StartAfterFirstMatchOf},
  {"endBeforeFirstMatchOf", AdjustmentKind::EndBeforeFirstMatchOf},
  {"endAfterFirstMatchOf", AdjustmentKind::EndAfterFirstMatchOf},
};

// `normalizeTestProperty`.
string normalize_test_property(string prop) {
  if (starts_with(prop, "test.")) prop = prop.substr(5);
  if (starts_with(prop, "onlyIfNot")) {
    prop = (prop.size() > 9 ? string(1, tolower(prop[9])) : string()) + (prop.size() > 10 ? prop.substr(10) : string());
  }
  if (starts_with(prop, "onlyIf")) {
    prop = (prop.size() > 6 ? string(1, tolower(prop[6])) : string()) + (prop.size() > 7 ? prop.substr(7) : string());
  }
  return prop;
}

// `capturePropertyIsCaptureSetting` and `normalizeCaptureSettingProperty`:
// returns "final", "shy" or "".
string capture_setting(const string &prop) {
  if (prop == "test.final") return "final";
  if (prop == "test.shy") return "shy";
  if (contains(prop, ".") && !starts_with(prop, "capture.")) return "";
  string normalized = starts_with(prop, "capture.") ? prop.substr(8) : prop;
  return normalized == "final" || normalized == "shy" ? normalized : "";
}

using Data = vector<QueryProperty>;

const QueryProperty *find_property(const Data &data, const string &key) {
  for (auto &property : data) {
    if (property.key == key) return &property;
  }
  return nullptr;
}

// `existingData[key] === value` or `key in existingData`.
bool data_matches(const Data &data, const string &key, const optional<string> &value) {
  const QueryProperty *property = find_property(data, key);
  if (!property) return false;
  if (!value) return true;
  return property->value && *property->value == *value;
}

bool is_final(const Data *data) {
  return data && (find_property(*data, "capture.final") || find_property(*data, "final"));
}

bool type_in(const char *type, const vector<string> &types) {
  for (auto &t : types) if (t == type) return true;
  return false;
}

bool is_blank(const u16string &text, size_t start, size_t end) {
  for (size_t i = start; i < end && i < text.size(); i++) {
    if (!is_js_space(text[i])) return false;
  }
  return true;
}

struct Check {
  enum class Kind { Final, Shy, Test } kind;
  TestKind test;
  bool negate = false;
  optional<string> value;
  // Precomputed from `value`.
  bool multiple = false;
  vector<string> types;
  string key;
  optional<string> key_value;
  NodePositionDescriptor descriptor;
  vector<TSSymbol> symbols;
};

Check make_test(TestKind test, const string &prop, const optional<string> &value, const TSLanguage *language) {
  Check check;
  check.kind = Check::Kind::Test;
  check.test = test;
  check.negate = contains(prop, "onlyIfNot");
  check.value = value;
  if (!value) return check;
  switch (test) {
    case TestKind::Type:
    case TestKind::DescendantOfType:
    case TestKind::ChildOfType:
    case TestKind::ParentOfType:
      check.multiple = contains(*value, " ");
      check.types = check.multiple ? split_on_spaces(*value) : vector<string>{*value};
      break;
    case TestKind::AncestorTypeNearerThan:
      check.types = split_on_spaces(*value);
      break;
    case TestKind::AncestorOfType:
      check.types = contains(*value, " ") ? split_on_spaces(*value) : vector<string>{*value};
      check.symbols = symbols_for_types(language, check.types);
      break;
    case TestKind::RangeWithData:
    case TestKind::DescendantOfNodeWithData:
    case TestKind::Config:
      split_key_value(*value, &check.key, &check.key_value);
      break;
    case TestKind::StartsOnSameRowAs:
    case TestKind::EndsOnSameRowAs:
      check.descriptor = NodePositionDescriptor::parse(value);
      break;
    default:
      break;
  }
  return check;
}

struct Adjustment {
  AdjustmentKind kind;
  optional<string> value;
  NodePositionDescriptor descriptor;
  double offset = 0;
  std::shared_ptr<Regex> regex;
};

struct Range {
  Point start;
  Point end;
  uint32_t start_index;
  uint32_t end_index;
};

uint64_t range_key(uint32_t start_index, uint32_t end_index) {
  return (static_cast<uint64_t>(start_index) << 32) | end_index;
}

}  // namespace

struct ScopeResolver::Pattern {
  bool invalidate_on_change = false;
  vector<Adjustment> adjustments;
  vector<Check> set_checks;
  vector<Check> asserted;
  vector<Check> refuted;
  // What `setDataForRange` stores for this pattern's captures.
  Data data;
};

struct ScopeResolver::Resolution {
  const HighlightContext &context;
  TextBuffer *buffer;
  std::unordered_map<uint64_t, const Data *> range_data;
  struct Bundle {
    vector<uint32_t> open;
    // In the reverse of their final order (captures that match later close
    // first).
    vector<uint32_t> close;
  };
  std::map<Point, Bundle> boundaries;

  uint32_t position_to_index(Point position) { return buffer->clip_position(position).offset; }

  Point index_to_position(double index) {
    // As N-API's Int64Value converts it.
    int64_t offset = std::isfinite(index) ? static_cast<int64_t>(index) : 0;
    offset = std::max<int64_t>(0, std::min<int64_t>(offset, buffer->size()));
    return buffer->position_for_offset(offset);
  }

  Point adjust_position_by_offset(Point position, double offset) {
    double index = static_cast<double>(position_to_index(position)) + offset;
    return buffer->clip_position(index_to_position(index)).position;
  }

  const Data *data_for_range(uint32_t start_index, uint32_t end_index) {
    auto entry = range_data.find(range_key(start_index, end_index));
    return entry == range_data.end() ? nullptr : entry->second;
  }

  const Data *data_for_node(TSNode node) {
    return data_for_range(ts_node_start_byte(node) / 2, ts_node_end_byte(node) / 2);
  }
};

namespace {

bool apply_test(const Check &check, TSNode node, const Data *existing, ScopeResolver::Resolution &resolution) {
  // Tests that need a value fail without one (in JavaScript they'd throw).
  auto needs_value = [&]() { return !check.value; };
  switch (check.test) {
    case TestKind::Type:
      if (needs_value()) return false;
      return type_in(ts_node_type(node), check.types);

    case TestKind::HasError:
      return ts_node_has_error(node);

    case TestKind::Injection:
      return resolution.context.injection;

    case TestKind::Root:
      return ts_node_is_null(ts_node_parent(node));

    case TestKind::First: {
      TSNode parent = ts_node_parent(node);
      if (ts_node_is_null(parent)) return true;
      TSNode first = ts_node_child(parent, 0);
      return !ts_node_is_null(first) && first.id == node.id;
    }

    case TestKind::Last: {
      TSNode parent = ts_node_parent(node);
      if (ts_node_is_null(parent)) return true;
      uint32_t count = ts_node_child_count(parent);
      if (count == 0) return false;
      return ts_node_child(parent, count - 1).id == node.id;
    }

    case TestKind::FirstOfType:
    case TestKind::LastOfType: {
      TSNode parent = ts_node_parent(node);
      if (ts_node_is_null(parent)) return true;
      uint32_t count = ts_node_child_count(parent);
      const char *type_name = ts_node_type(node);
      for (uint32_t j = 0; j < count; j++) {
        uint32_t i = check.test == TestKind::FirstOfType ? j : count - 1 - j;
        TSNode child = ts_node_child(parent, i);
        if (ts_node_is_null(child)) continue;
        if (child.id == node.id) return true;
        if (strcmp(ts_node_type(child), type_name) == 0) return false;
      }
      return false;
    }

    case TestKind::LastTextOnRow: {
      Point end = point_from_ts(ts_node_end_point(node));
      auto line = resolution.buffer->line_for_row(end.row);
      if (!line) return true;
      return is_blank(*line, end.column, line->size());
    }

    case TestKind::FirstTextOnRow: {
      Point start = point_from_ts(ts_node_start_point(node));
      auto line = resolution.buffer->line_for_row(start.row);
      if (!line) return true;
      return is_blank(*line, 0, std::min<size_t>(start.column, line->size()));
    }

    case TestKind::DescendantOfType: {
      if (needs_value()) return false;
      for (TSNode current = ts_node_parent(node); !ts_node_is_null(current); current = ts_node_parent(current)) {
        if (type_in(ts_node_type(current), check.types)) return true;
      }
      return false;
    }

    case TestKind::ChildOfType: {
      if (needs_value()) return false;
      TSNode parent = ts_node_parent(node);
      if (ts_node_is_null(parent)) return false;
      return type_in(ts_node_type(parent), check.types);
    }

    case TestKind::AncestorTypeNearerThan: {
      if (needs_value()) return false;
      const string &target = check.types[0];
      vector<string> rejected(check.types.begin() + 1, check.types.end());
      for (TSNode current = ts_node_parent(node); !ts_node_is_null(current); current = ts_node_parent(current)) {
        const char *type = ts_node_type(current);
        if (type_in(type, rejected)) return false;
        if (target == type) return true;
      }
      return false;
    }

    case TestKind::AncestorOfType: {
      if (needs_value()) return false;
      vector<TSNode> descendants;
      descendants_of_type(node, check.symbols, {0, 0}, {0, 0}, &descendants, true);
      return !descendants.empty();
    }

    case TestKind::ParentOfType: {
      if (needs_value()) return false;
      uint32_t count = ts_node_child_count(node);
      for (uint32_t i = 0; i < count; i++) {
        if (type_in(ts_node_type(ts_node_child(node, i)), check.types)) return true;
      }
      return false;
    }

    case TestKind::RangeWithData:
      if (!existing) return false;
      if (needs_value()) return false;
      if (check.key.empty()) return true;
      return data_matches(*existing, check.key, check.key_value);

    case TestKind::DescendantOfNodeWithData: {
      if (needs_value()) return false;
      if (check.key.empty()) return true;
      for (TSNode current = ts_node_parent(node); !ts_node_is_null(current); current = ts_node_parent(current)) {
        const Data *data = resolution.data_for_node(current);
        if (data && data_matches(*data, check.key, check.key_value)) return true;
      }
      return false;
    }

    case TestKind::StartsOnSameRowAs:
    case TestKind::EndsOnSameRowAs: {
      if (needs_value()) return false;
      auto other = check.descriptor.resolve(node);
      if (!other) return false;
      TSPoint own = check.test == TestKind::StartsOnSameRowAs ? ts_node_start_point(node) : ts_node_end_point(node);
      return other->row == own.row;
    }

    case TestKind::Config:
      if (needs_value()) return false;
      if (check.key.empty()) return true;
      return resolution.context.config && resolution.context.config(check.key, check.key_value);
  }
  return false;
}

// The first match of a JavaScript RegExp (without flags) in `text`.
bool first_match(const Regex &regex, const u16string &text, size_t *start, size_t *end) {
  Regex::MatchData match_data(regex);
  auto result = regex.match(
    text.data(), text.size(), match_data,
    Regex::MatchOptions::IsEndSearch | Regex::MatchOptions::IsBeginningOfLine | Regex::MatchOptions::IsEndOfLine
  );
  if (result.type != Regex::MatchResult::Full) return false;
  *start = result.start_offset;
  *end = result.end_offset;
  return true;
}

}  // namespace

ScopeResolver::ScopeResolver(std::shared_ptr<Query> query) : query_{std::move(query)} {
  patterns_.resize(query_->pattern_count());
}

ScopeResolver::~ScopeResolver() {}

const ScopeResolver::Pattern &ScopeResolver::pattern(uint32_t index) {
  auto &slot = patterns_[index];
  if (slot) return *slot;
  slot.reset(new Pattern());
  Pattern &pattern = *slot;
  const TSLanguage *language = query_->language();

  for (auto &property : query_->set_properties(index)) {
    const string &key = property.key;
    if (key == "highlight.invalidateOnChange") pattern.invalidate_on_change = true;

    string adjustment = starts_with(key, "adjust.") ? key.substr(7) : key;
    auto adjustment_kind = ADJUSTMENTS.find(adjustment);
    if (adjustment_kind != ADJUSTMENTS.end()) {
      Adjustment a;
      a.kind = adjustment_kind->second;
      a.value = property.value;
      switch (a.kind) {
        case AdjustmentKind::StartAt:
        case AdjustmentKind::EndAt:
          a.descriptor = NodePositionDescriptor::parse(property.value);
          break;
        case AdjustmentKind::OffsetStart:
        case AdjustmentKind::OffsetEnd:
          a.offset = js_number(property.value);
          break;
        default: {
          // `new RegExp(null)` matches "null".
          u16string error;
          auto regex = std::make_shared<Regex>(utf8_to_utf16(property.value ? *property.value : "null"), &error,
                                               false, false, false);
          if (error.empty()) a.regex = regex;
          break;
        }
      }
      pattern.adjustments.push_back(std::move(a));
    }

    string setting = capture_setting(key);
    if (!setting.empty()) {
      Check check;
      check.kind = setting == "final" ? Check::Kind::Final : Check::Kind::Shy;
      pattern.set_checks.push_back(std::move(check));
      continue;
    }
    auto test = TESTS.find(normalize_test_property(key));
    if (test != TESTS.end()) pattern.set_checks.push_back(make_test(test->second, key, property.value, language));
  }

  for (auto &property : query_->asserted_properties(index)) {
    auto test = TESTS.find(normalize_test_property(property.key));
    if (test != TESTS.end()) pattern.asserted.push_back(make_test(test->second, property.key, property.value, language));
  }
  for (auto &property : query_->refuted_properties(index)) {
    auto test = TESTS.find(normalize_test_property(property.key));
    if (test != TESTS.end()) pattern.refuted.push_back(make_test(test->second, property.key, property.value, language));
  }

  // `setDataForRange` copies `test.final` and `test.shy` to `capture.*`.
  pattern.data = query_->set_properties(index);
  for (const char *prop : {"final", "shy"}) {
    const QueryProperty *legacy = find_property(pattern.data, string("test.") + prop);
    if (!legacy) continue;
    optional<string> value = legacy->value;
    string key = string("capture.") + prop;
    auto existing = std::find_if(pattern.data.begin(), pattern.data.end(), [&](const QueryProperty &p) {
      return p.key == key;
    });
    if (existing != pattern.data.end()) existing->value = value;
    else pattern.data.push_back({key, value});
  }
  return pattern;
}

uint32_t ScopeResolver::scope_index(const string &name) {
  auto entry = scope_indexes_.find(name);
  if (entry != scope_indexes_.end()) return entry->second;
  uint32_t index = scope_names_.size();
  scope_names_.push_back(name);
  scope_indexes_.emplace(name, index);
  return index;
}

void ScopeResolver::resolve(TSNode root, Point from, Point to, const HighlightContext &context,
                            vector<HighlightBoundary> *boundaries, vector<const void *> *invalidated_node_ids) {
  TextBuffer *buffer = context.buffer;
  Resolution resolution{context, buffer, {}, {}};

  vector<QueryCapture> captures;
  query_->captures(root, point_to_ts(from), point_to_ts(to), [buffer](TSNode node) {
    return text_for_node(buffer, node);
  }, &captures);

  for (const QueryCapture &capture : captures) {
    TSNode node = capture.node;
    const Pattern &pattern = this->pattern(capture.pattern_index);
    if (pattern.invalidate_on_change) invalidated_node_ids->push_back(node.id);

    // Phantom nodes invented by the parse tree.
    if (ts_node_child_count(node) == 0 && ts_node_start_byte(node) == ts_node_end_byte(node)) continue;

    // `ScopeResolver.interpolateName`.
    string name = query_->capture_name(capture.capture_id);
    if (contains(name, "_TEXT_")) {
      u16string text = text_for_node(buffer, node);
      if (text.find(u' ') == u16string::npos) name = js_replace(name, "_TEXT_", utf16_to_utf8(text));
    }
    if (contains(name, "_TYPE_")) name = js_replace(name, "_TYPE_", ts_node_type(node));

    // `determineCaptureRange`.
    Range range{
      point_from_ts(ts_node_start_point(node)), point_from_ts(ts_node_end_point(node)),
      ts_node_start_byte(node) / 2, ts_node_end_byte(node) / 2
    };
    if (!pattern.adjustments.empty()) {
      bool valid = true;
      optional<u16string> node_text;
      Point node_start = range.start;
      for (const Adjustment &adjustment : pattern.adjustments) {
        switch (adjustment.kind) {
          case AdjustmentKind::StartAt:
          case AdjustmentKind::EndAt: {
            auto position = adjustment.descriptor.resolve(node);
            if (!position) { valid = false; break; }
            if (adjustment.kind == AdjustmentKind::StartAt) {
              range.start = *position;
              range.start_index = resolution.position_to_index(range.start);
            } else {
              range.end = *position;
              range.end_index = resolution.position_to_index(range.end);
            }
            break;
          }
          case AdjustmentKind::OffsetStart:
          case AdjustmentKind::OffsetEnd: {
            if (std::isnan(adjustment.offset)) { valid = false; break; }
            if (adjustment.kind == AdjustmentKind::OffsetStart) {
              range.start = resolution.adjust_position_by_offset(range.start, adjustment.offset);
              range.start_index = resolution.position_to_index(range.start);
            } else {
              range.end = resolution.adjust_position_by_offset(range.end, adjustment.offset);
              range.end_index = resolution.position_to_index(range.end);
            }
            break;
          }
          default: {
            if (!adjustment.regex) { valid = false; break; }
            if (!node_text) node_text = text_for_node(buffer, node);
            size_t match_start, match_end;
            if (!first_match(*adjustment.regex, *node_text, &match_start, &match_end)) { valid = false; break; }
            bool sets_start = adjustment.kind == AdjustmentKind::StartAndEndAroundFirstMatchOf ||
              adjustment.kind == AdjustmentKind::StartBeforeFirstMatchOf ||
              adjustment.kind == AdjustmentKind::StartAfterFirstMatchOf;
            bool sets_end = adjustment.kind == AdjustmentKind::StartAndEndAroundFirstMatchOf ||
              adjustment.kind == AdjustmentKind::EndBeforeFirstMatchOf ||
              adjustment.kind == AdjustmentKind::EndAfterFirstMatchOf;
            if (sets_start) {
              size_t offset = adjustment.kind == AdjustmentKind::StartAfterFirstMatchOf ? match_end : match_start;
              range.start = resolution.adjust_position_by_offset(node_start, offset);
              range.start_index = resolution.position_to_index(range.start);
            }
            if (sets_end) {
              size_t offset = adjustment.kind == AdjustmentKind::EndBeforeFirstMatchOf ? match_start : match_end;
              range.end = resolution.adjust_position_by_offset(node_start, offset);
              range.end_index = resolution.position_to_index(range.end);
            }
            break;
          }
        }
        if (!valid) break;
      }
      // `isValidRange`.
      if (!valid || range.start_index > range.end_index || !(range.start < range.end)) continue;
    }

    // `test`.
    const Data *existing = resolution.data_for_range(range.start_index, range.end_index);
    if (is_final(existing)) continue;
    bool passes = true;
    for (const Check &check : pattern.set_checks) {
      if (check.kind == Check::Kind::Final) {
        passes = !is_final(existing);
      } else if (check.kind == Check::Kind::Shy) {
        passes = existing == nullptr;
      } else {
        bool result = apply_test(check, node, existing, resolution);
        passes = check.negate ? !result : result;
      }
      if (!passes) break;
    }
    if (!passes) continue;
    for (const Check &check : pattern.asserted) {
      bool result = apply_test(check, node, existing, resolution);
      if (!(check.negate ? !result : result)) { passes = false; break; }
    }
    if (!passes) continue;
    for (const Check &check : pattern.refuted) {
      bool result = apply_test(check, node, existing, resolution);
      if (check.negate ? !result : result) { passes = false; break; }
    }
    if (!passes) continue;

    resolution.range_data[range_key(range.start_index, range.end_index)] = &pattern.data;

    if (name == "_IGNORE_" || starts_with(name, "_IGNORE_.")) continue;
    if (range.start == range.end) continue;

    uint32_t id = scope_index(name);
    resolution.boundaries[range.start].open.push_back(id);
    resolution.boundaries[range.end].close.push_back(id);
  }

  boundaries->reserve(resolution.boundaries.size());
  for (auto &entry : resolution.boundaries) {
    HighlightBoundary boundary;
    boundary.position = entry.first;
    boundary.open = std::move(entry.second.open);
    boundary.close.assign(entry.second.close.rbegin(), entry.second.close.rend());
    boundaries->push_back(std::move(boundary));
  }
}

}  // namespace tree_sitter
