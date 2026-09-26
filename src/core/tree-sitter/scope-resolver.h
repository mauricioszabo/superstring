#ifndef SUPERSTRING_TREE_SITTER_SCOPE_RESOLVER_H_
#define SUPERSTRING_TREE_SITTER_SCOPE_RESOLVER_H_

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <tree_sitter/api.h>
#include "optional.h"
#include "point.h"
#include "query.h"

class TextBuffer;

namespace tree_sitter {

// What a highlights pass needs to know about the layer it runs for.
struct HighlightContext {
  TextBuffer *buffer;
  // Whether the layer is an injection (for `test.injection`).
  bool injection;
  // Evaluates `test.config` for a key and an optional raw value.
  std::function<bool(const std::string &key, const optional<std::string> &value)> config;
};

// The scopes to close, then open, at a position. Scopes are indexes into
// `ScopeResolver::scope_names()`.
struct HighlightBoundary {
  Point position;
  std::vector<uint32_t> close;
  std::vector<uint32_t> open;
};

// The highlighting half of Pulsar's `ScopeResolver` (src/scope-resolver.js)
// for one highlights query: capture name interpolation, `capture.final` and
// `capture.shy`, the `test.*` checks, the `adjust.*` range adjustments,
// `_IGNORE_` and boundary ordering, with the same results.
class ScopeResolver {
public:
  explicit ScopeResolver(std::shared_ptr<Query> query);
  ~ScopeResolver();

  // Resolves the query's captures within [from, to] (buffer points) into
  // boundaries in position order, like `LanguageLayer#getSyntaxBoundaries`
  // before it adds the language's root scopes. Adds the ids of nodes whose
  // captures set `highlight.invalidateOnChange` to `invalidated_node_ids`.
  void resolve(TSNode root, Point from, Point to, const HighlightContext &context,
               std::vector<HighlightBoundary> *boundaries, std::vector<const void *> *invalidated_node_ids);

  // Every scope name resolved so far, by index (interpolated names are added
  // as they are seen).
  const std::vector<std::string> &scope_names() const { return scope_names_; }

  struct Pattern;
  struct Resolution;

private:
  const Pattern &pattern(uint32_t index);
  uint32_t scope_index(const std::string &name);

  std::shared_ptr<Query> query_;
  std::vector<std::unique_ptr<Pattern>> patterns_;
  std::vector<std::string> scope_names_;
  std::unordered_map<std::string, uint32_t> scope_indexes_;
};

}  // namespace tree_sitter

#endif  // SUPERSTRING_TREE_SITTER_SCOPE_RESOLVER_H_
