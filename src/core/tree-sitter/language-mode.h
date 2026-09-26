#ifndef SUPERSTRING_TREE_SITTER_LANGUAGE_MODE_H_
#define SUPERSTRING_TREE_SITTER_LANGUAGE_MODE_H_

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <tree_sitter/api.h>
#include "point.h"
#include "range.h"

class TextBuffer;

namespace tree_sitter {

class Grammar;

// Keeps a tree alive for as long as nodes of it are referenced, and knows the
// buffer its text comes from. Tree-sitter positions are in bytes of UTF-16
// text: offsets and columns are twice the buffer's code-unit values.
struct TreeHolder {
  TSTree *tree;
  TextBuffer *buffer;

  TreeHolder(TSTree *tree, TextBuffer *buffer) : tree{tree}, buffer{buffer} {}
  ~TreeHolder() { ts_tree_delete(tree); }
};

// Conversions between Tree-sitter (UTF-16 bytes) and buffer (code units)
// coordinates.
inline Point point_from_ts(TSPoint point) { return Point(point.row, point.column / 2); }
inline TSPoint point_to_ts(Point point) {
  return {point.row, point.column >= UINT32_MAX / 2 ? UINT32_MAX : point.column * 2};
}

std::u16string text_for_node(TextBuffer *buffer, TSNode node);

// Parsers, trees and included ranges for the language layers of one buffer.
// Parsing reads the buffer's text directly; edits are applied to every
// layer's tree and take effect on the next parse of that layer.
class LanguageMode {
public:
  explicit LanguageMode(TextBuffer *buffer);
  ~LanguageMode();

  uint32_t create_layer(std::shared_ptr<Grammar> grammar);
  void destroy_layer(uint32_t layer_id);
  bool has_layer(uint32_t layer_id) const;

  // Ranges of buffer text the layer parses; empty means the whole buffer.
  void set_included_ranges(uint32_t layer_id, std::vector<TSRange> ranges);

  // Offsets and points in buffer code units.
  void edit(uint32_t start_index, uint32_t old_end_index, uint32_t new_end_index,
            Point start, Point old_end, Point new_end);

  // Parses (or reparses) a layer. Returns false if parsing failed. Ranges
  // whose syntax changed since the previous parse (in buffer coordinates)
  // are appended to `changed_ranges`; the first parse reports the whole
  // tree.
  bool parse(uint32_t layer_id, std::vector<Range> *changed_ranges);

  std::shared_ptr<TreeHolder> tree(uint32_t layer_id) const;
  std::shared_ptr<Grammar> grammar(uint32_t layer_id) const;
  TextBuffer *buffer() const { return buffer_; }

private:
  struct Layer {
    std::shared_ptr<Grammar> grammar;
    TSParser *parser;
    std::shared_ptr<TreeHolder> tree;
    std::vector<TSRange> included_ranges;
    bool included_ranges_changed = true;
  };

  TextBuffer *buffer_;
  std::unordered_map<uint32_t, Layer> layers_;
  uint32_t next_layer_id_ = 1;
};

}  // namespace tree_sitter

#endif  // SUPERSTRING_TREE_SITTER_LANGUAGE_MODE_H_
