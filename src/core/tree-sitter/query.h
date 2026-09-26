#ifndef SUPERSTRING_TREE_SITTER_QUERY_H_
#define SUPERSTRING_TREE_SITTER_QUERY_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <tree_sitter/api.h>
#include "optional.h"
#include "regex.h"

namespace tree_sitter {

class Grammar;

// A `#set!`, `#is?` or `#is-not?` property: `(key value?)`.
struct QueryProperty {
  std::string key;
  optional<std::string> value;
};

struct QueryCapture {
  uint32_t pattern_index;
  uint32_t capture_id;
  TSNode node;
};

// A compiled Tree-sitter query whose predicates are evaluated like
// web-tree-sitter 0.25's (`#eq?`, `#match?` with JavaScript RegExp
// semantics, `#any-of?` and their variants filter matches; `#set!`, `#is?`
// and `#is-not?` become per-pattern properties; anything else is ignored).
class Query {
public:
  // Returns null and fills `error` (web-tree-sitter's message) on failure.
  static std::shared_ptr<Query> create(std::shared_ptr<Grammar> grammar, const std::string &source,
                                       std::string *error);
  ~Query();

  // Returns the text of a node, for text predicates.
  using TextForNode = std::function<std::u16string(TSNode)>;

  // Captures of matches within the node, in document order, that satisfy
  // their pattern's text predicates. Points are in bytes (UTF-16 columns
  // times two); a zero end point means "no limit".
  void captures(TSNode node, TSPoint start, TSPoint end, const TextForNode &text_for_node,
                std::vector<QueryCapture> *result);

  uint32_t pattern_count() const;
  uint32_t capture_count() const;
  const std::string &capture_name(uint32_t id) const { return capture_names_[id]; }
  const std::vector<QueryProperty> &set_properties(uint32_t pattern) const { return patterns_[pattern].set_properties; }
  const std::vector<QueryProperty> &asserted_properties(uint32_t pattern) const { return patterns_[pattern].asserted_properties; }
  const std::vector<QueryProperty> &refuted_properties(uint32_t pattern) const { return patterns_[pattern].refuted_properties; }
  bool has_set_properties(uint32_t pattern) const { return patterns_[pattern].has_set_properties; }
  bool has_asserted_properties(uint32_t pattern) const { return patterns_[pattern].has_asserted_properties; }
  bool has_refuted_properties(uint32_t pattern) const { return patterns_[pattern].has_refuted_properties; }
  const TSQuery *ts_query() const { return query_; }
  const TSLanguage *language() const;

private:
  struct TextPredicate {
    enum class Kind { Eq, Match, AnyOf } kind;
    bool positive;
    bool match_all;
    std::string capture_name;
    // Eq: compare with another capture, or with `value`.
    optional<std::string> other_capture_name;
    std::u16string value;
    std::vector<std::u16string> values;
    std::shared_ptr<Regex> regex;
  };

  struct Pattern {
    std::vector<TextPredicate> text_predicates;
    std::vector<QueryProperty> set_properties;
    std::vector<QueryProperty> asserted_properties;
    std::vector<QueryProperty> refuted_properties;
    bool has_set_properties = false;
    bool has_asserted_properties = false;
    bool has_refuted_properties = false;
  };

  Query(std::shared_ptr<Grammar> grammar, TSQuery *query);
  bool parse_predicates(std::string *error);
  bool satisfies_text_predicates(const Pattern &pattern, const TSQueryMatch &match,
                                 const TextForNode &text_for_node) const;

  std::shared_ptr<Grammar> grammar_;
  TSQuery *query_;
  TSQueryCursor *cursor_;
  std::vector<std::string> capture_names_;
  std::vector<Pattern> patterns_;
};

}  // namespace tree_sitter

#endif  // SUPERSTRING_TREE_SITTER_QUERY_H_
