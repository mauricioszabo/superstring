#include "query.h"
#include <algorithm>
#include "grammar.h"
#include "strings.h"

using std::string;
using std::u16string;
using std::vector;

namespace tree_sitter {

namespace {

// The number of UTF-16 code units in the first `byte_count` bytes of a UTF-8
// string (which is how web-tree-sitter reports error offsets).
size_t utf16_length_of_utf8_prefix(const string &input, size_t byte_count) {
  return utf8_to_utf16(input.substr(0, std::min(byte_count, input.size()))).size();
}

u16string utf16_substring_as_utf8_source(const string &source, size_t start, size_t length) {
  u16string text = utf8_to_utf16(source);
  if (start >= text.size()) return u16string();
  return text.substr(start, length);
}

bool is_word_character(char16_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

// Mirrors web-tree-sitter's QueryError messages.
string format_query_error(const string &source, uint32_t error_offset, TSQueryError error_type) {
  size_t error_index = utf16_length_of_utf8_prefix(source, error_offset);
  u16string suffix = utf16_substring_as_utf8_source(source, error_index, 100);
  size_t newline = suffix.find(u'\n');
  if (newline != u16string::npos) suffix.resize(newline);
  u16string word;
  for (size_t i = 0; i < suffix.size(); i++) {
    if (is_word_character(suffix[i])) {
      size_t j = i;
      while (j < suffix.size() && is_word_character(suffix[j])) j++;
      word = suffix.substr(i, j - i);
      break;
    }
  }
  string suffix_description = std::to_string(error_index) + ": '" + utf16_to_utf8(suffix) + "'...";
  switch (error_type) {
    case TSQueryErrorNodeType: return "Bad node name '" + utf16_to_utf8(word) + "'";
    case TSQueryErrorField: return "Bad field name '" + utf16_to_utf8(word) + "'";
    case TSQueryErrorCapture: return "Bad capture name @" + utf16_to_utf8(word);
    case TSQueryErrorStructure: return "Bad pattern structure at offset " + suffix_description;
    default: return "Bad syntax at offset " + suffix_description;
  }
}

struct Step {
  bool is_capture;
  string value;  // Capture name or string value.
};

}  // namespace

std::shared_ptr<Query> Query::create(std::shared_ptr<Grammar> grammar, const string &source, string *error) {
  uint32_t error_offset = 0;
  TSQueryError error_type = TSQueryErrorNone;
  TSQuery *ts_query = ts_query_new(grammar->language(), source.data(), source.size(), &error_offset, &error_type);
  if (!ts_query) {
    *error = format_query_error(source, error_offset, error_type);
    return nullptr;
  }
  std::shared_ptr<Query> query(new Query(std::move(grammar), ts_query));
  if (!query->parse_predicates(error)) return nullptr;
  return query;
}

Query::Query(std::shared_ptr<Grammar> grammar, TSQuery *query) :
  grammar_{std::move(grammar)}, query_{query}, cursor_{ts_query_cursor_new()} {
  uint32_t capture_count = ts_query_capture_count(query_);
  for (uint32_t i = 0; i < capture_count; i++) {
    uint32_t length;
    const char *name = ts_query_capture_name_for_id(query_, i, &length);
    capture_names_.emplace_back(name, length);
  }
  ts_query_cursor_set_match_limit(cursor_, UINT32_MAX);
}

Query::~Query() {
  ts_query_cursor_delete(cursor_);
  ts_query_delete(query_);
}

const TSLanguage *Query::language() const { return grammar_->language(); }

uint32_t Query::pattern_count() const { return ts_query_pattern_count(query_); }
uint32_t Query::capture_count() const { return capture_names_.size(); }

// Mirrors web-tree-sitter's parsePattern and friends, including their
// argument validation and error messages.
bool Query::parse_predicates(string *error) {
  uint32_t pattern_count = ts_query_pattern_count(query_);
  patterns_.resize(pattern_count);
  for (uint32_t pattern_index = 0; pattern_index < pattern_count; pattern_index++) {
    Pattern &pattern = patterns_[pattern_index];
    uint32_t step_count;
    const TSQueryPredicateStep *predicate_steps = ts_query_predicates_for_pattern(query_, pattern_index, &step_count);
    vector<Step> steps;
    for (uint32_t i = 0; i < step_count; i++) {
      const TSQueryPredicateStep &predicate_step = predicate_steps[i];
      if (predicate_step.type == TSQueryPredicateStepTypeCapture) {
        steps.push_back({true, capture_names_[predicate_step.value_id]});
        continue;
      }
      if (predicate_step.type == TSQueryPredicateStepTypeString) {
        uint32_t length;
        const char *value = ts_query_string_value_for_id(query_, predicate_step.value_id, &length);
        steps.push_back({false, string(value, length)});
        continue;
      }
      if (steps.empty()) continue;

      if (steps[0].is_capture) {
        *error = "Predicates must begin with a literal value";
        return false;
      }
      const string &op = steps[0].value;
      size_t argument_count = steps.size() - 1;

      if (op == "eq?" || op == "not-eq?" || op == "any-eq?" || op == "any-not-eq?") {
        if (steps.size() != 3) {
          *error = "Wrong number of arguments to `#" + op + "` predicate. Expected 2, got " + std::to_string(argument_count);
          return false;
        }
        if (!steps[1].is_capture) {
          *error = "First argument of `#" + op + "` predicate must be a capture. Got \"" + steps[1].value + "\"";
          return false;
        }
        TextPredicate predicate;
        predicate.kind = TextPredicate::Kind::Eq;
        predicate.positive = op == "eq?" || op == "any-eq?";
        predicate.match_all = op.compare(0, 4, "any-") != 0;
        predicate.capture_name = steps[1].value;
        if (steps[2].is_capture) {
          predicate.other_capture_name = steps[2].value;
        } else {
          predicate.value = utf8_to_utf16(steps[2].value);
        }
        pattern.text_predicates.push_back(std::move(predicate));
      } else if (op == "match?" || op == "not-match?" || op == "any-match?" || op == "any-not-match?") {
        if (steps.size() != 3) {
          *error = "Wrong number of arguments to `#" + op + "` predicate. Expected 2, got " + std::to_string(argument_count) + ".";
          return false;
        }
        if (!steps[1].is_capture) {
          *error = "First argument of `#" + op + "` predicate must be a capture. Got \"" + steps[1].value + "\".";
          return false;
        }
        if (steps[2].is_capture) {
          *error = "Second argument of `#" + op + "` predicate must be a string. Got @" + steps[2].value + ".";
          return false;
        }
        TextPredicate predicate;
        predicate.kind = TextPredicate::Kind::Match;
        predicate.positive = op == "match?" || op == "any-match?";
        predicate.match_all = op.compare(0, 4, "any-") != 0;
        predicate.capture_name = steps[1].value;
        u16string regex_error;
        predicate.regex = std::make_shared<Regex>(utf8_to_utf16(steps[2].value), &regex_error, false, false, false);
        if (!regex_error.empty()) {
          *error = "Invalid regular expression: /" + steps[2].value + "/: " + utf16_to_utf8(regex_error);
          return false;
        }
        pattern.text_predicates.push_back(std::move(predicate));
      } else if (op == "any-of?" || op == "not-any-of?") {
        if (steps.size() < 2) {
          *error = "Wrong number of arguments to `#" + op + "` predicate. Expected at least 1. Got " + std::to_string(argument_count) + ".";
          return false;
        }
        if (!steps[1].is_capture) {
          *error = "First argument of `#" + op + "` predicate must be a capture. Got \"" + steps[1].value + "\".";
          return false;
        }
        TextPredicate predicate;
        predicate.kind = TextPredicate::Kind::AnyOf;
        predicate.positive = op == "any-of?";
        predicate.match_all = true;
        predicate.capture_name = steps[1].value;
        for (size_t j = 2; j < steps.size(); j++) {
          if (steps[j].is_capture) {
            *error = "Arguments to `#" + op + "` predicate must be strings.\".";
            return false;
          }
          predicate.values.push_back(utf8_to_utf16(steps[j].value));
        }
        pattern.text_predicates.push_back(std::move(predicate));
      } else if (op == "is?" || op == "is-not?" || op == "set!") {
        if (steps.size() < 2 || steps.size() > 3) {
          if (op == "set!") {
            *error = "Wrong number of arguments to `#set!` predicate. Expected 1 or 2. Got " + std::to_string(argument_count) + ".";
          } else {
            *error = "Wrong number of arguments to `#" + op + "` predicate. Expected 1 or 2. Got " + std::to_string(argument_count) + ".";
          }
          return false;
        }
        for (auto &step : steps) {
          if (step.is_capture) {
            *error = op == "set!"
              ? "Arguments to `#set!` predicate must be strings.\"."
              : "Arguments to `#" + op + "` predicate must be strings.\".";
            return false;
          }
        }
        QueryProperty property{steps[1].value, steps.size() == 3 ? optional<string>(steps[2].value) : optional<string>()};
        vector<QueryProperty> *properties;
        if (op == "set!") {
          properties = &pattern.set_properties;
          pattern.has_set_properties = true;
        } else if (op == "is?") {
          properties = &pattern.asserted_properties;
          pattern.has_asserted_properties = true;
        } else {
          properties = &pattern.refuted_properties;
          pattern.has_refuted_properties = true;
        }
        // Like assigning to an object key: a later value replaces an earlier one.
        auto existing = std::find_if(properties->begin(), properties->end(), [&](const QueryProperty &p) {
          return p.key == property.key;
        });
        if (existing != properties->end()) {
          existing->value = property.value;
        } else {
          properties->push_back(std::move(property));
        }
      }
      // Other predicates are ignored, as Pulsar ignores web-tree-sitter's
      // `predicates` for them.
      steps.clear();
    }
  }
  return true;
}

bool Query::satisfies_text_predicates(const Pattern &pattern, const TSQueryMatch &match,
                                      const TextForNode &text_for_node) const {
  for (const TextPredicate &predicate : pattern.text_predicates) {
    vector<u16string> texts;
    vector<u16string> other_texts;
    for (uint16_t i = 0; i < match.capture_count; i++) {
      const string &name = capture_names_[match.captures[i].index];
      if (name == predicate.capture_name) texts.push_back(text_for_node(match.captures[i].node));
      if (predicate.other_capture_name && name == *predicate.other_capture_name) {
        other_texts.push_back(text_for_node(match.captures[i].node));
      }
    }

    bool satisfied;
    switch (predicate.kind) {
      case TextPredicate::Kind::Eq: {
        auto compare = [&](const u16string &text, const u16string &other) {
          return predicate.positive ? text == other : text != other;
        };
        auto test = [&](const u16string &text) {
          if (predicate.other_capture_name) {
            return std::any_of(other_texts.begin(), other_texts.end(), [&](const u16string &other) {
              return compare(text, other);
            });
          }
          return compare(text, predicate.value);
        };
        satisfied = predicate.match_all
          ? std::all_of(texts.begin(), texts.end(), test)
          : std::any_of(texts.begin(), texts.end(), test);
        break;
      }
      case TextPredicate::Kind::Match: {
        if (texts.empty()) {
          satisfied = !predicate.positive;
          break;
        }
        Regex::MatchData match_data(*predicate.regex);
        auto test = [&](const u16string &text) {
          // The whole text is the subject, as with RegExp#test.
          auto result = predicate.regex->match(
            text.data(), text.size(), match_data,
            Regex::MatchOptions::IsEndSearch | Regex::MatchOptions::IsBeginningOfLine | Regex::MatchOptions::IsEndOfLine
          );
          bool matched = result.type == Regex::MatchResult::Full;
          return predicate.positive ? matched : !matched;
        };
        satisfied = predicate.match_all
          ? std::all_of(texts.begin(), texts.end(), test)
          : std::any_of(texts.begin(), texts.end(), test);
        break;
      }
      case TextPredicate::Kind::AnyOf: {
        if (texts.empty()) {
          satisfied = !predicate.positive;
          break;
        }
        bool all_included = std::all_of(texts.begin(), texts.end(), [&](const u16string &text) {
          return std::find(predicate.values.begin(), predicate.values.end(), text) != predicate.values.end();
        });
        satisfied = all_included == predicate.positive;
        break;
      }
    }
    if (!satisfied) return false;
  }
  return true;
}

void Query::captures(TSNode node, TSPoint start, TSPoint end, const TextForNode &text_for_node,
                     vector<QueryCapture> *result) {
  ts_query_cursor_set_point_range(cursor_, start, end);
  ts_query_cursor_exec(cursor_, query_, node);
  TSQueryMatch match;
  uint32_t capture_index;
  while (ts_query_cursor_next_capture(cursor_, &match, &capture_index)) {
    const Pattern &pattern = patterns_[match.pattern_index];
    if (!pattern.text_predicates.empty() && !satisfies_text_predicates(pattern, match, text_for_node)) {
      continue;
    }
    const TSQueryCapture &capture = match.captures[capture_index];
    result->push_back({match.pattern_index, capture.index, capture.node});
  }
}

}  // namespace tree_sitter
