#include "language-mode.h"
#include <algorithm>
#include "grammar.h"
#include "text-buffer.h"
#include "text-slice.h"
#include "wasm-engine.h"

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

LanguageMode::LanguageMode(TextBuffer *buffer) : buffer_{buffer} {}

LanguageMode::~LanguageMode() {
  for (auto &entry : layers_) ts_parser_delete(entry.second.parser);
}

uint32_t LanguageMode::create_layer(std::shared_ptr<Grammar> grammar) {
  TSWasmError error = {TSWasmErrorKindNone, nullptr};
  TSWasmStore *store = ts_wasm_store_new(wasm_engine(), &error);
  free(error.message);
  TSParser *parser = ts_parser_new();
  if (store) ts_parser_set_wasm_store(parser, store);
  ts_parser_set_language(parser, grammar->language());

  uint32_t id = next_layer_id_++;
  Layer layer;
  layer.grammar = std::move(grammar);
  layer.parser = parser;
  layers_.emplace(id, std::move(layer));
  return id;
}

void LanguageMode::destroy_layer(uint32_t layer_id) {
  auto entry = layers_.find(layer_id);
  if (entry == layers_.end()) return;
  ts_parser_delete(entry->second.parser);
  layers_.erase(entry);
}

bool LanguageMode::has_layer(uint32_t layer_id) const {
  return layers_.count(layer_id) > 0;
}

void LanguageMode::set_included_ranges(uint32_t layer_id, vector<TSRange> ranges) {
  auto entry = layers_.find(layer_id);
  if (entry == layers_.end()) return;
  entry->second.included_ranges = std::move(ranges);
  entry->second.included_ranges_changed = true;
}

void LanguageMode::edit(uint32_t start_index, uint32_t old_end_index, uint32_t new_end_index,
                        Point start, Point old_end, Point new_end) {
  TSInputEdit edit = {
    start_index * 2, old_end_index * 2, new_end_index * 2,
    point_to_ts(start), point_to_ts(old_end), point_to_ts(new_end)
  };
  for (auto &entry : layers_) {
    Layer &layer = entry.second;
    if (layer.tree) ts_tree_edit(layer.tree->tree, &edit);
    // Included ranges move with the text, like the tree does.
    for (TSRange &range : layer.included_ranges) {
      auto adjust = [&](uint32_t &byte, TSPoint &point) {
        if (byte >= edit.old_end_byte) {
          byte = byte - edit.old_end_byte + edit.new_end_byte;
          if (point.row == edit.old_end_point.row) {
            point.column = point.column - edit.old_end_point.column + edit.new_end_point.column;
          }
          point.row = point.row - edit.old_end_point.row + edit.new_end_point.row;
        } else if (byte > edit.start_byte) {
          byte = edit.start_byte;
          point = edit.start_point;
        }
      };
      adjust(range.start_byte, range.start_point);
      adjust(range.end_byte, range.end_point);
    }
  }
}

bool LanguageMode::parse(uint32_t layer_id, vector<Range> *changed_ranges) {
  auto entry = layers_.find(layer_id);
  if (entry == layers_.end()) return false;
  Layer &layer = entry->second;

  if (layer.included_ranges_changed) {
    if (layer.included_ranges.empty()) {
      ts_parser_set_included_ranges(layer.parser, nullptr, 0);
    } else {
      ts_parser_set_included_ranges(layer.parser, layer.included_ranges.data(), layer.included_ranges.size());
    }
    layer.included_ranges_changed = false;
  }

  BufferInput input{buffer_, {}};
  TSInput ts_input = {&input, BufferInput::read, TSInputEncodingUTF16LE, nullptr};
  TSTree *old_tree = layer.tree ? layer.tree->tree : nullptr;
  TSTree *new_tree = ts_parser_parse(layer.parser, old_tree, ts_input);
  if (!new_tree) return false;

  if (old_tree) {
    uint32_t count = 0;
    TSRange *ranges = ts_tree_get_changed_ranges(old_tree, new_tree, &count);
    for (uint32_t i = 0; i < count; i++) {
      changed_ranges->push_back(Range{point_from_ts(ranges[i].start_point), point_from_ts(ranges[i].end_point)});
    }
    free(ranges);
  } else {
    TSNode root = ts_tree_root_node(new_tree);
    changed_ranges->push_back(Range{point_from_ts(ts_node_start_point(root)), point_from_ts(ts_node_end_point(root))});
  }

  layer.tree = std::make_shared<TreeHolder>(new_tree, buffer_);
  return true;
}

std::shared_ptr<TreeHolder> LanguageMode::tree(uint32_t layer_id) const {
  auto entry = layers_.find(layer_id);
  return entry == layers_.end() ? nullptr : entry->second.tree;
}

std::shared_ptr<Grammar> LanguageMode::grammar(uint32_t layer_id) const {
  auto entry = layers_.find(layer_id);
  return entry == layers_.end() ? nullptr : entry->second.grammar;
}

}  // namespace tree_sitter
