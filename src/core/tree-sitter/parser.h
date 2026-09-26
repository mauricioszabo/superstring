#ifndef SUPERSTRING_TREE_SITTER_PARSER_H_
#define SUPERSTRING_TREE_SITTER_PARSER_H_

#include <memory>
#include <string>
#include <vector>
#include <tree_sitter/api.h>
#include "point.h"
#include "range.h"

class TextBuffer;

namespace tree_sitter {

class Grammar;

// Owns a tree, which nodes and tree wrappers share. Tree-sitter positions are
// in bytes of UTF-16 text: offsets and columns are twice the buffer's
// code-unit values.
struct TreeHolder {
  TSTree *tree;
  explicit TreeHolder(TSTree *tree) : tree{tree} {}
  ~TreeHolder() { ts_tree_delete(tree); }
};

inline Point point_from_ts(TSPoint point) { return Point(point.row, point.column / 2); }
inline TSPoint point_to_ts(Point point) {
  return {point.row, point.column >= UINT32_MAX / 2 ? UINT32_MAX : point.column * 2};
}

// The buffer's text for a node, read by offset.
std::u16string text_for_node(TextBuffer *buffer, TSNode node);

// The symbols of the named node types, as web-tree-sitter's
// `descendantsOfType` resolves them (including "ERROR").
std::vector<TSSymbol> symbols_for_types(const TSLanguage *language, const std::vector<std::string> &types);

// Mirrors web-tree-sitter's `descendantsOfType`, including the node itself
// and its handling of positions (bytes; a zero end point means no limit).
// With `first_only`, stops at the first result.
void descendants_of_type(TSNode node, const std::vector<TSSymbol> &symbols, TSPoint start, TSPoint end,
                         std::vector<TSNode> *result, bool first_only = false);

// A Tree-sitter parser for one grammar, with a wasm store of its own.
// Parsing reads a native TextBuffer's text directly.
class Parser {
public:
  explicit Parser(std::shared_ptr<Grammar> grammar);
  ~Parser();

  // Returns null if parsing failed. `old_tree` must have been edited to match
  // the buffer; empty `included_ranges` means the whole buffer.
  TSTree *parse(TextBuffer *buffer, const TSTree *old_tree, const std::vector<TSRange> &included_ranges);

  const std::shared_ptr<Grammar> &grammar() const { return grammar_; }

private:
  std::shared_ptr<Grammar> grammar_;
  TSParser *parser_;
};

}  // namespace tree_sitter

#endif  // SUPERSTRING_TREE_SITTER_PARSER_H_
