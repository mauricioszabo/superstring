#include "parser.h"
#include <algorithm>
#include "grammar.h"
#include "text-buffer.h"
#include "text-slice.h"
#include "wasm-engine.h"

using std::string;
using std::vector;

namespace tree_sitter {

namespace {

// The buffer's chunks of text from code unit `offset` on: `*skip` is how many
// code units of the first chunks precede `offset`. Buffer points can't
// address the position between a `\r` and a `\n`, so this starts at the
// closest point before `offset`.
void chunks_from_offset(TextBuffer *buffer, uint32_t offset, vector<TextSlice> *chunks, uint32_t *skip) {
  Point start = buffer->position_for_offset(offset);
  uint32_t start_offset = buffer->clip_position(start).offset;
  while (start_offset > offset && start.row > 0) {
    start = Point(start.row - 1, 0);
    start_offset = buffer->clip_position(start).offset;
  }
  *chunks = buffer->chunks_in_range(Range{start, buffer->extent()});
  *skip = offset - start_offset;
}

// Feeds the buffer's text to Tree-sitter as UTF-16LE, a chunk at a time,
// without copying. The chunk pointers stay valid during the (synchronous)
// parse because the buffer can't change meanwhile.
struct BufferInput {
  TextBuffer *buffer;
  vector<TextSlice> chunks;

  static const char *read(void *payload, uint32_t byte_index, TSPoint position, uint32_t *bytes_read) {
    auto *input = static_cast<BufferInput *>(payload);
    uint32_t offset = byte_index / 2;
    *bytes_read = 0;
    if (offset >= input->buffer->size()) return "";
    uint32_t skip;
    chunks_from_offset(input->buffer, offset, &input->chunks, &skip);
    for (auto &chunk : input->chunks) {
      if (skip >= chunk.size()) {
        skip -= chunk.size();
        continue;
      }
      *bytes_read = (chunk.size() - skip) * 2;
      return reinterpret_cast<const char *>(chunk.data() + skip);
    }
    return "";
  }
};

}  // namespace

// Reads by offset rather than by point, for the same reason as
// `chunks_from_offset`.
std::u16string text_for_node(TextBuffer *buffer, TSNode node) {
  uint32_t start = ts_node_start_byte(node) / 2;
  uint32_t end = ts_node_end_byte(node) / 2;
  std::u16string result;
  if (end <= start) return result;
  result.reserve(end - start);
  vector<TextSlice> chunks;
  uint32_t skip;
  chunks_from_offset(buffer, start, &chunks, &skip);
  for (auto &chunk : chunks) {
    if (skip >= chunk.size()) {
      skip -= chunk.size();
      continue;
    }
    uint32_t available = chunk.size() - skip;
    uint32_t needed = end - start - result.size();
    result.append(chunk.data() + skip, std::min(available, needed));
    skip = 0;
    if (result.size() >= end - start) break;
  }
  return result;
}

vector<TSSymbol> symbols_for_types(const TSLanguage *language, const vector<string> &types) {
  vector<TSSymbol> symbols;
  for (auto &type : types) {
    if (type == "ERROR") symbols.push_back(65535);
  }
  uint32_t symbol_count = ts_language_symbol_count(language);
  for (uint32_t symbol = 0; symbol < symbol_count; symbol++) {
    const char *name = ts_language_symbol_name(language, symbol);
    if (!name) continue;
    for (auto &type : types) {
      if (type == name) {
        symbols.push_back(symbol);
        break;
      }
    }
  }
  return symbols;
}

namespace {

bool point_lte(TSPoint a, TSPoint b) {
  return a.row < b.row || (a.row == b.row && a.column <= b.column);
}

}  // namespace

void descendants_of_type(TSNode node, const vector<TSSymbol> &symbols, TSPoint start_point, TSPoint end_point,
                         vector<TSNode> *result, bool first_only) {
  auto contains = [&](TSSymbol symbol) {
    for (TSSymbol s : symbols) if (s == symbol) return true;
    return false;
  };
  if (end_point.row == 0 && end_point.column == 0) end_point = {UINT32_MAX, UINT32_MAX};

  TSTreeCursor cursor = ts_tree_cursor_new(node);
  bool already_visited_children = false;
  while (true) {
    TSNode descendant = ts_tree_cursor_current_node(&cursor);
    if (!already_visited_children) {
      // Skip nodes before the range without descending into them.
      if (point_lte(ts_node_end_point(descendant), start_point)) {
        if (ts_tree_cursor_goto_next_sibling(&cursor)) {
          already_visited_children = false;
        } else {
          if (!ts_tree_cursor_goto_parent(&cursor)) break;
          already_visited_children = true;
        }
        continue;
      }
      // Stop at the first node after the range.
      if (point_lte(end_point, ts_node_start_point(descendant))) break;
      if (contains(ts_node_symbol(descendant))) {
        result->push_back(descendant);
        if (first_only) break;
      }
      if (ts_tree_cursor_goto_first_child(&cursor)) {
        already_visited_children = false;
      } else if (ts_tree_cursor_goto_next_sibling(&cursor)) {
        already_visited_children = false;
      } else {
        if (!ts_tree_cursor_goto_parent(&cursor)) break;
        already_visited_children = true;
      }
    } else {
      if (ts_tree_cursor_goto_next_sibling(&cursor)) {
        already_visited_children = false;
      } else {
        if (!ts_tree_cursor_goto_parent(&cursor)) break;
      }
    }
  }
  ts_tree_cursor_delete(&cursor);
}

Parser::Parser(std::shared_ptr<Grammar> grammar) : grammar_{std::move(grammar)}, parser_{ts_parser_new()} {
  TSWasmError error = {TSWasmErrorKindNone, nullptr};
  TSWasmStore *store = ts_wasm_store_new(wasm_engine(), &error);
  free(error.message);
  if (store) ts_parser_set_wasm_store(parser_, store);
  ts_parser_set_language(parser_, grammar_->language());
}

Parser::~Parser() {
  ts_parser_delete(parser_);
}

TSTree *Parser::parse(TextBuffer *buffer, const TSTree *old_tree, const vector<TSRange> &included_ranges) {
  if (included_ranges.empty()) {
    ts_parser_set_included_ranges(parser_, nullptr, 0);
  } else {
    ts_parser_set_included_ranges(parser_, included_ranges.data(), included_ranges.size());
  }
  BufferInput input{buffer, {}};
  TSInput ts_input = {&input, BufferInput::read, TSInputEncodingUTF16LE, nullptr};
  return ts_parser_parse(parser_, old_tree, ts_input);
}

}  // namespace tree_sitter
