#include "display-layer.h"
#include <algorithm>
#include <iterator>
#include "character-classes.h"

using std::u16string;
using std::vector;
using ClipDirection = DisplayLayer::ClipDirection;

namespace {

const uint32_t INF = UINT32_MAX;
const int32_t MAX_BUILT_IN_SCOPE_ID = 256;

const int32_t HARD_TAB = 1 << 0;
const int32_t LEADING_WHITESPACE = 1 << 2;
const int32_t TRAILING_WHITESPACE = 1 << 3;
const int32_t INVISIBLE_CHARACTER = 1 << 4;
const int32_t INDENT_GUIDE = 1 << 5;
const int32_t LINE_ENDING = 1 << 6;
const int32_t FOLD = 1 << 7;

uint32_t next_screen_line_id = 1;

// JS points can hold `Infinity`; natively it is represented as UINT32_MAX, so
// arithmetic on coordinates saturates instead of overflowing.
inline uint32_t saturating_add(uint32_t a, uint32_t b) {
  return a > INF - b ? INF : a + b;
}

inline Point traverse(Point start, Point distance) {
  if (distance.row == 0) {
    return Point(start.row, saturating_add(start.column, distance.column));
  } else {
    return Point(saturating_add(start.row, distance.row), distance.column);
  }
}

inline Point traversal(Point end, Point start) {
  if (end.row == start.row) {
    return Point(0, end.column == INF ? INF : end.column - start.column);
  } else {
    return Point(end.row == INF ? INF : end.row - start.row, end.column);
  }
}

inline bool is_soft_wrap_hunk(const Patch::Change &hunk) {
  return hunk.old_start == hunk.old_end;
}

// Converts a JS number to a row/column, treating Infinity as UINT32_MAX.
inline uint32_t to_coordinate(double value) {
  if (!(value > 0)) return 0;
  if (value >= static_cast<double>(INF)) return INF;
  return static_cast<uint32_t>(value);
}

inline bool is_open_tag(int32_t tag) { return tag < 0 && (tag & 1) == 1; }

// Mirrors `classNameForBuiltInFlags` in text-buffer's display-layer.js.
u16string class_name_for_built_in_flags(int32_t flags) {
  u16string class_name;
  auto add = [&](const char16_t *name) {
    if (!class_name.empty()) class_name += u' ';
    class_name += name;
  };
  if (flags & INVISIBLE_CHARACTER) add(u"invisible-character");
  if (flags & HARD_TAB) add(u"hard-tab");
  if (flags & LEADING_WHITESPACE) add(u"leading-whitespace");
  if (flags & TRAILING_WHITESPACE) add(u"trailing-whitespace");
  if (flags & LINE_ENDING) add(u"eol");
  if (flags & INDENT_GUIDE) add(u"indent-guide");
  if (flags & FOLD) add(u"fold-marker");
  return class_name;
}

void append_escaped_html(u16string *html, const char16_t *text, size_t length) {
  for (size_t i = 0; i < length; i++) {
    char16_t c = text[i];
    switch (c) {
      case '&': *html += u"&amp;"; break;
      case '<': *html += u"&lt;"; break;
      case '>': *html += u"&gt;"; break;
      case '"': *html += u"&quot;"; break;
      default: html->push_back(c);
    }
  }
}
inline int32_t open_tag_for_scope_id(int32_t scope_id) { return -scope_id; }
inline int32_t close_tag_for_scope_id(int32_t scope_id) { return -scope_id - 1; }

}  // namespace

DisplayLayer::DisplayLayer(TextBuffer *buffer, FoldsIndexProvider folds_index, Params params) :
  buffer_{buffer},
  folds_index_{std::move(folds_index)},
  spatial_index_{false},
  rightmost_screen_position_{0, 0},
  indexed_buffer_row_count_{0},
  next_built_in_scope_id_{1},
  built_in_scope_ids_exhausted_{false},
  fold_character_ratio_{NAN},
  cached_line_row_{INF} {
  set_params(std::move(params));
}

DisplayLayer::DisplayLayer(const DisplayLayer &source, FoldsIndexProvider folds_index, Params params) :
  buffer_{source.buffer_},
  folds_index_{std::move(folds_index)},
  spatial_index_{const_cast<Patch &>(source.spatial_index_).copy()},
  screen_line_lengths_{source.screen_line_lengths_},
  tab_counts_{source.tab_counts_},
  wrap_overflow_offsets_{source.wrap_overflow_offsets_},
  wrap_carried_widths_{source.wrap_carried_widths_},
  rightmost_screen_position_{source.rightmost_screen_position_},
  indexed_buffer_row_count_{source.indexed_buffer_row_count_},
  next_built_in_scope_id_{1},
  built_in_scope_ids_exhausted_{false},
  fold_character_ratio_{NAN},
  cached_line_row_{INF} {
  set_params(std::move(params));
}

DisplayLayer::~DisplayLayer() {}

void DisplayLayer::set_params(Params params) {
  params_ = std::move(params);
  if (params_.tab_length == 0) params_.tab_length = 1;
  if (params_.soft_wrap_column < 1) params_.soft_wrap_column = 1;
  ratio_cache_.clear();
  fold_character_ratio_ = NAN;
  wrap_boundary_cache_.clear();
}

void DisplayLayer::clear_spatial_index() {
  invalidate_line_cache();
  indexed_buffer_row_count_ = 0;
  spatial_index_.clear();
  cached_screen_lines_.clear();
  screen_line_lengths_.clear();
  tab_counts_.clear();
  wrap_overflow_offsets_.clear();
  wrap_carried_widths_.clear();
  rightmost_screen_position_ = Point(0, 0);
}

void DisplayLayer::clear_screen_line_cache() {
  cached_screen_lines_.clear();
}

// Buffer access

uint32_t DisplayLayer::line_count() const {
  return buffer_->extent().row + 1;
}

uint32_t DisplayLayer::last_row() const {
  return buffer_->extent().row;
}

uint32_t DisplayLayer::line_length(uint32_t row) {
  if (row == cached_line_row_) return cached_line_.size();
  auto length = buffer_->line_length_for_row(row);
  return length ? *length : 0;
}

const u16string &DisplayLayer::line_for_row(uint32_t row) {
  if (row != cached_line_row_) {
    cached_line_.clear();
    if (row <= last_row()) {
      buffer_->with_line_for_row(row, [this](const char16_t *data, uint32_t size) {
        cached_line_.assign(data, size);
      });
    }
    cached_line_row_ = row;
  }
  return cached_line_;
}

void DisplayLayer::invalidate_line_cache() {
  cached_line_row_ = INF;
}

// The cached line survives across calls, so that e.g. translating many
// positions on one long line copies it once. It is dropped whenever we are
// told about a buffer change, and as a safety net whenever the buffer's size
// or extent differ from when it was cached.
void DisplayLayer::validate_line_cache() {
  uint32_t size = buffer_->size();
  Point extent = buffer_->extent();
  if (size != cached_line_buffer_size_ || extent != cached_line_buffer_extent_) {
    cached_line_row_ = INF;
    cached_line_buffer_size_ = size;
    cached_line_buffer_extent_ = extent;
  }
}

// Character metrics

double DisplayLayer::ratio_for_character(uint16_t character) {
  switch (params_.width_mode) {
    case WidthMode::Unit:
      return 1;
    case WidthMode::Ratios:
      if (character_classes::is_korean(character)) return params_.korean_ratio;
      if (character_classes::is_half_width(character)) return params_.half_width_ratio;
      if (character_classes::is_double_width(character)) return params_.double_width_ratio;
      return 1;
    case WidthMode::Callback:
      if (ratio_cache_.empty()) ratio_cache_.assign(0x10000, NAN);
      double &ratio = ratio_cache_[character];
      if (std::isnan(ratio)) ratio = params_.ratio_for_character(u16string(1, character));
      return ratio;
  }
  return 1;
}

double DisplayLayer::ratio_for_fold_character() {
  if (params_.width_mode == WidthMode::Callback) {
    if (std::isnan(fold_character_ratio_)) {
      fold_character_ratio_ = params_.ratio_for_character(params_.fold_character);
    }
    return fold_character_ratio_;
  }
  // JS classifies with `charCodeAt(0)`, which is NaN for an empty string.
  if (params_.fold_character.empty()) return 1;
  return ratio_for_character(params_.fold_character[0]);
}

bool DisplayLayer::is_wrap_boundary(uint16_t previous, uint16_t character, bool character_is_fold) {
  switch (params_.wrap_boundary_mode) {
    case WrapBoundaryMode::WordStart:
      return character_classes::is_word_start(previous, character);
    case WrapBoundaryMode::Pulsar:
      return character_classes::is_pulsar_wrap_boundary(previous, character);
    case WrapBoundaryMode::Callback:
      if (character_is_fold) {
        return params_.is_wrap_boundary(u16string(1, previous), params_.fold_character);
      }
      uint32_t key = (static_cast<uint32_t>(previous) << 16) | character;
      auto entry = wrap_boundary_cache_.find(key);
      if (entry != wrap_boundary_cache_.end()) return entry->second;
      bool result = params_.is_wrap_boundary(u16string(1, previous), u16string(1, character));
      wrap_boundary_cache_.emplace(key, result);
      return result;
  }
  return false;
}

// Coordinate translation

Point DisplayLayer::clip_buffer_position(double row, double column) {
  // Mirrors TextBuffer::clipPosition without options.
  if (row < 0) return Point(0, 0);
  uint32_t last = last_row();
  if (row > last) return Point(last, line_length(last));
  uint32_t clipped_row = to_coordinate(row);
  if (column < 0) return Point(clipped_row, 0);
  uint32_t length = line_length(clipped_row);
  if (column > length) return Point(clipped_row, length);
  return Point(clipped_row, to_coordinate(column));
}

Point DisplayLayer::translate_buffer_position(double row, double column, ClipDirection clip_direction) {
  validate_line_cache();
  Point buffer_position = clip_buffer_position(row, column);
  populate_spatial_index_if_needed(saturating_add(buffer_position.row, 1), INF);

  int64_t column_delta = get_clip_column_delta(buffer_position, clip_direction);
  if (column_delta != 0) {
    buffer_position.column = static_cast<uint32_t>(buffer_position.column + column_delta);
  }

  Point screen_position = translate_buffer_position_with_spatial_index(buffer_position, clip_direction);
  if (screen_position.row < tab_counts_.size()) {
    uint32_t tab_count = tab_counts_[screen_position.row];
    if (tab_count > 0) screen_position = expand_hard_tabs(screen_position, tab_count);
  }
  return screen_position;
}

Point DisplayLayer::translate_buffer_position_with_spatial_index(Point position, ClipDirection clip_direction) {
  auto hunk = spatial_index_.grab_change_starting_before_old_position(position);
  if (hunk) {
    if (position < hunk->old_end) {
      if (hunk->old_start == position) {
        return hunk->new_start;
      } else {  // hunk is a fold
        if (clip_direction == ClipDirection::Backward) {
          return hunk->new_start;
        } else if (clip_direction == ClipDirection::Forward) {
          return hunk->new_end;
        } else {
          Point distance_from_fold_start = traversal(position, hunk->old_start);
          Point distance_to_fold_end = traversal(hunk->old_end, position);
          return distance_from_fold_start <= distance_to_fold_end ? hunk->new_start : hunk->new_end;
        }
      }
    } else {
      return traverse(hunk->new_end, traversal(position, hunk->old_end));
    }
  }
  return position;
}

Point DisplayLayer::translate_screen_position(double row, double column, ClipDirection clip_direction,
                                              bool skip_soft_wrap_indentation, bool *out_of_range) {
  validate_line_cache();
  *out_of_range = false;
  if (row + 1 > 0) {
    populate_spatial_index_if_needed(line_count(), row + 1 >= INF ? INF : to_coordinate(row + 1));
  }
  Point screen_position = constrain_screen_position(row, column, clip_direction);
  if (screen_position.row < tab_counts_.size()) {
    uint32_t tab_count = tab_counts_[screen_position.row];
    if (tab_count > 0) screen_position = collapse_hard_tabs(screen_position, tab_count, clip_direction);
  }
  Point buffer_position = translate_screen_position_with_spatial_index(
    screen_position, clip_direction, skip_soft_wrap_indentation
  );

  if (buffer_position.row >= line_count()) {
    *out_of_range = true;
    return Point(last_row(), line_length(last_row()));
  }

  int64_t column_delta = get_clip_column_delta(buffer_position, clip_direction);
  if (column_delta != 0) {
    buffer_position.column = static_cast<uint32_t>(buffer_position.column + column_delta);
  }
  return buffer_position;
}

Point DisplayLayer::translate_screen_position_with_spatial_index(Point position, ClipDirection clip_direction,
                                                                 bool skip_soft_wrap_indentation) {
  auto hunk = spatial_index_.grab_change_starting_before_new_position(position);
  if (hunk) {
    if (position < hunk->new_end) {
      if (is_soft_wrap_hunk(*hunk)) {
        if ((clip_direction == ClipDirection::Backward && !skip_soft_wrap_indentation) ||
            (clip_direction == ClipDirection::Closest && hunk->new_start == position)) {
          Point previous(hunk->new_start.row, hunk->new_start.column == 0 ? 0 : hunk->new_start.column - 1);
          return translate_screen_position_with_spatial_index(previous, clip_direction, skip_soft_wrap_indentation);
        } else {
          return hunk->old_start;
        }
      } else {  // Hunk is a fold. Since folds are 1 character on screen, we're at the start.
        return hunk->old_start;
      }
    } else {
      return traverse(hunk->old_end, traversal(position, hunk->new_end));
    }
  }
  return position;
}

Point DisplayLayer::constrain_screen_position(double row, double column, ClipDirection clip_direction) {
  if (row < 0) return Point(0, 0);
  if (screen_line_lengths_.empty()) return Point(0, 0);

  uint32_t max_row = screen_line_lengths_.size() - 1;
  if (row > max_row) return Point(max_row, screen_line_lengths_[max_row]);

  uint32_t screen_row = to_coordinate(row);
  if (column < 0) return Point(screen_row, 0);

  uint32_t max_column = screen_line_lengths_[screen_row];
  if (column > max_column) {
    if (clip_direction == ClipDirection::Forward && screen_row < max_row) {
      return Point(screen_row + 1, 0);
    } else {
      return Point(screen_row, max_column);
    }
  }

  return Point(screen_row, to_coordinate(column));
}

Point DisplayLayer::expand_hard_tabs(Point target_screen_position, uint32_t tab_count) {
  Point screen_row_start(target_screen_position.row, 0);
  auto hunks = spatial_index_.grab_changes_in_new_range(screen_row_start, target_screen_position);
  size_t hunk_index = 0;
  int64_t unexpanded_screen_column = 0;
  int64_t expanded_screen_column = 0;
  Point buffer_position = translate_screen_position_with_spatial_index(
    screen_row_start, ClipDirection::Unspecified, false
  );
  uint32_t buffer_row = buffer_position.row;
  uint32_t buffer_column = buffer_position.column;
  u16string buffer_line = line_for_row(buffer_row);
  int64_t tab_length = params_.tab_length;

  while (tab_count > 0) {
    if (unexpanded_screen_column == target_screen_position.column) break;

    if (hunk_index < hunks.size() &&
        hunks[hunk_index].old_start.row == buffer_row &&
        hunks[hunk_index].old_start.column == buffer_column) {
      auto &next_hunk = hunks[hunk_index];
      if (is_soft_wrap_hunk(next_hunk)) {
        unexpanded_screen_column = hunks[0].new_end.column;
        expanded_screen_column = unexpanded_screen_column;
      } else {
        buffer_row = next_hunk.old_end.row;
        buffer_column = next_hunk.old_end.column;
        buffer_line = line_for_row(buffer_row);
        unexpanded_screen_column++;
        expanded_screen_column++;
      }
      hunk_index++;
      continue;
    }

    if (buffer_column < buffer_line.size() && buffer_line[buffer_column] == '\t') {
      expanded_screen_column += tab_length - (expanded_screen_column % tab_length);
      tab_count--;
    } else {
      expanded_screen_column++;
    }
    unexpanded_screen_column++;
    buffer_column++;
  }

  expanded_screen_column += static_cast<int64_t>(target_screen_position.column) - unexpanded_screen_column;
  if (expanded_screen_column == target_screen_position.column) {
    return target_screen_position;
  } else {
    return Point(target_screen_position.row, static_cast<uint32_t>(expanded_screen_column));
  }
}

Point DisplayLayer::collapse_hard_tabs(Point target_screen_position, uint32_t tab_count,
                                       ClipDirection clip_direction) {
  Point screen_row_start(target_screen_position.row, 0);
  Point screen_row_end(target_screen_position.row, screen_line_lengths_[target_screen_position.row]);

  auto hunks = spatial_index_.grab_changes_in_new_range(screen_row_start, screen_row_end);
  size_t hunk_index = 0;
  int64_t unexpanded_screen_column = 0;
  int64_t expanded_screen_column = 0;
  Point buffer_position = translate_screen_position_with_spatial_index(
    screen_row_start, ClipDirection::Unspecified, false
  );
  uint32_t buffer_row = buffer_position.row;
  uint32_t buffer_column = buffer_position.column;
  u16string buffer_line = line_for_row(buffer_row);
  int64_t tab_length = params_.tab_length;
  int64_t target_column = target_screen_position.column;

  while (tab_count > 0) {
    if (expanded_screen_column == target_column) break;

    if (hunk_index < hunks.size() &&
        hunks[hunk_index].old_start.row == buffer_row &&
        hunks[hunk_index].old_start.column == buffer_column) {
      auto &next_hunk = hunks[hunk_index];
      if (is_soft_wrap_hunk(next_hunk)) {
        unexpanded_screen_column = std::min<int64_t>(target_column, next_hunk.new_end.column);
        expanded_screen_column = unexpanded_screen_column;
      } else {
        buffer_row = next_hunk.old_end.row;
        buffer_column = next_hunk.old_end.column;
        buffer_line = line_for_row(buffer_row);
        unexpanded_screen_column++;
        expanded_screen_column++;
      }
      hunk_index++;
      continue;
    }

    if (buffer_column < buffer_line.size() && buffer_line[buffer_column] == '\t') {
      int64_t next_tab_stop_column = expanded_screen_column + tab_length - (expanded_screen_column % tab_length);
      if (next_tab_stop_column > target_column) {
        uint32_t row = target_screen_position.row;
        if (clip_direction == ClipDirection::Backward) {
          return Point(row, static_cast<uint32_t>(unexpanded_screen_column));
        } else if (clip_direction == ClipDirection::Forward) {
          return Point(row, static_cast<uint32_t>(unexpanded_screen_column + 1));
        } else {
          // JS: Math.ceil((nextTabStopColumn + expandedScreenColumn) / 2)
          int64_t sum = next_tab_stop_column + expanded_screen_column;
          int64_t midpoint = sum / 2 + (sum % 2 != 0 ? 1 : 0);
          if (target_column > midpoint) {
            return Point(row, static_cast<uint32_t>(unexpanded_screen_column + 1));
          } else {
            return Point(row, static_cast<uint32_t>(unexpanded_screen_column));
          }
        }
      }
      expanded_screen_column = next_tab_stop_column;
      tab_count--;
    } else {
      expanded_screen_column++;
    }
    unexpanded_screen_column++;
    buffer_column++;
  }

  unexpanded_screen_column += target_column - expanded_screen_column;
  if (unexpanded_screen_column == target_column) {
    return target_screen_position;
  } else {
    return Point(target_screen_position.row, static_cast<uint32_t>(unexpanded_screen_column));
  }
}

int64_t DisplayLayer::get_clip_column_delta(Point buffer_position, ClipDirection clip_direction) {
  uint32_t row = buffer_position.row;
  int64_t column = buffer_position.column;
  const u16string &line = line_for_row(row);
  int64_t length = line.size();

  // Like TextBuffer::getCharacterAtPosition, positions at or past the end of
  // the line yield a line-ending (or NUL) character, never a space.
  auto character_at = [&](int64_t c) -> uint16_t {
    if (c < 0) c = 0;
    return c < length ? line[c] : 0;
  };

  // Treat paired unicode characters as atomic...
  uint16_t character = character_at(column);
  uint16_t previous_character = character_at(column - 1);
  if (character_classes::is_character_pair(previous_character, character)) {
    if (clip_direction == ClipDirection::Closest || clip_direction == ClipDirection::Backward) {
      return -1;
    } else {
      return 1;
    }
  }

  // Clip atomic soft tabs...
  if (!params_.atomic_soft_tabs) return 0;
  if (column * ratio_for_character(' ') > params_.soft_wrap_column) return 0;

  for (int64_t c = column; c >= 0; c--) {
    if (character_at(c) != ' ') return 0;
  }

  int64_t tab_length = params_.tab_length;
  int64_t previous_tab_stop = column - (column % tab_length);
  if (column == previous_tab_stop) return 0;
  int64_t next_tab_stop = previous_tab_stop + tab_length;

  // If there is a non-whitespace character before the next tab stop,
  // don't treat this whitespace as a soft tab
  for (int64_t c = column; c < next_tab_stop; c++) {
    if (character_at(c) != ' ') return 0;
  }

  int64_t clipped_column;
  if (clip_direction == ClipDirection::Backward) {
    clipped_column = previous_tab_stop;
  } else if (clip_direction == ClipDirection::Forward) {
    clipped_column = next_tab_stop;
  } else {
    clipped_column = (column - previous_tab_stop > tab_length / 2.0) ? next_tab_stop : previous_tab_stop;
  }
  return clipped_column - column;
}

// Queries

uint32_t DisplayLayer::get_screen_line_count() {
  validate_line_cache();
  populate_spatial_index_if_needed(line_count(), INF);
  return screen_line_lengths_.size();
}

uint32_t DisplayLayer::get_approximate_screen_line_count() {
  if (indexed_buffer_row_count_ > 0) {
    return static_cast<uint32_t>(
      std::floor(static_cast<double>(line_count()) * screen_line_lengths_.size() / indexed_buffer_row_count_)
    );
  } else {
    return line_count();
  }
}

Point DisplayLayer::get_rightmost_screen_position() {
  validate_line_cache();
  populate_spatial_index_if_needed(line_count(), INF);
  return rightmost_screen_position_;
}

Point DisplayLayer::get_approximate_rightmost_screen_position() {
  return rightmost_screen_position_;
}

optional<uint32_t> DisplayLayer::line_length_for_screen_row(uint32_t screen_row) {
  validate_line_cache();
  populate_spatial_index_if_needed(line_count(), saturating_add(screen_row, 1));
  if (screen_row < screen_line_lengths_.size()) return screen_line_lengths_[screen_row];
  return optional<uint32_t>{};
}

vector<uint32_t> DisplayLayer::buffer_rows_for_screen_rows(uint32_t start_row, uint32_t end_row) {
  validate_line_cache();
  populate_spatial_index_if_needed(line_count(), end_row);

  Point start_position(start_row, 0);
  vector<uint32_t> buffer_rows;
  uint32_t last_screen_row = start_row;
  uint32_t last_buffer_row = translate_screen_position_with_spatial_index(
    start_position, ClipDirection::Unspecified, false
  ).row;
  auto hunks = spatial_index_.grab_changes_in_new_range(start_position, Point(end_row, 0));
  for (auto &hunk : hunks) {
    while (last_screen_row <= hunk.new_start.row) {
      buffer_rows.push_back(last_buffer_row);
      last_screen_row++;
      last_buffer_row++;
    }
    last_buffer_row = is_soft_wrap_hunk(hunk) ? hunk.old_end.row : hunk.old_end.row + 1;
  }

  while (last_screen_row < end_row) {
    buffer_rows.push_back(last_buffer_row);
    last_screen_row++;
    last_buffer_row++;
  }

  return buffer_rows;
}

uint32_t DisplayLayer::leading_whitespace_length_for_surrounding_lines(uint32_t start_buffer_row) {
  uint32_t length = 0;
  for (int64_t buffer_row = static_cast<int64_t>(start_buffer_row) - 1; buffer_row >= 0; buffer_row--) {
    const u16string &line = line_for_row(buffer_row);
    if (!line.empty()) {
      length = leading_whitespace_length_for_non_empty_line(line);
      break;
    }
  }

  uint32_t count = line_count();
  for (uint32_t buffer_row = start_buffer_row + 1; buffer_row < count; buffer_row++) {
    const u16string &line = line_for_row(buffer_row);
    if (!line.empty()) {
      length = std::max(length, leading_whitespace_length_for_non_empty_line(line));
      break;
    }
  }

  return length;
}

uint32_t DisplayLayer::leading_whitespace_length_for_non_empty_line(const u16string &line) {
  uint32_t length = 0;
  for (char16_t character : line) {
    if (character == ' ') {
      length++;
    } else if (character == '\t') {
      length += params_.tab_length - (length % params_.tab_length);
    } else {
      break;
    }
  }
  return length;
}

uint32_t DisplayLayer::find_trailing_whitespace_start_column(uint32_t buffer_row) {
  const u16string &line = line_for_row(buffer_row);
  int64_t column = static_cast<int64_t>(line.size()) - 1;
  for (; column >= 0; column--) {
    if (line[column] != ' ' && line[column] != '\t') break;
  }
  return column + 1;
}

// Built-in scopes

int32_t DisplayLayer::built_in_scope_id_for_flags(int32_t flags) {
  if (flags == 0) return 0;

  auto entry = built_in_scope_ids_by_flags_.find(flags);
  if (entry != built_in_scope_ids_by_flags_.end()) return entry->second;

  if (next_built_in_scope_id_ > MAX_BUILT_IN_SCOPE_ID) {
    built_in_scope_ids_exhausted_ = true;
    return 0;
  }

  // Every flag we emit maps to a non-empty class name, so this always
  // allocates a new id (see JS `registerBuiltInScope`).
  next_built_in_scope_id_ += 2;
  int32_t scope_id = next_built_in_scope_id_;
  built_in_scope_ids_by_flags_.emplace(flags, scope_id);
  new_built_in_scopes_.push_back({scope_id, flags});
  class_names_by_scope_id_[scope_id] = class_name_for_built_in_flags(flags);
  return scope_id;
}

// Render plans

void DisplayLayer::set_class_name(int32_t scope_id, u16string class_name) {
  class_names_by_scope_id_[scope_id] = std::move(class_name);
}

uint32_t DisplayLayer::create_render_planner() {
  uint32_t id = next_render_planner_id_++;
  render_planners_[id];
  return id;
}

void DisplayLayer::destroy_render_planner(uint32_t planner_id) {
  render_planners_.erase(planner_id);
}

// Produces the same DOM that Pulsar's LineComponent builds from the tags:
// a root <span> with a nested <span class="..."> per scope and text nodes for
// the text runs, plus a space for empty lines and a zero-width no-break space
// after a trailing fold marker.
bool DisplayLayer::append_line_html(const ScreenLine &line, u16string *html,
                                    vector<int32_t> *missing_scope_ids) {
  bool complete = true;
  *html += u"<span>";
  size_t column = 0;
  for (int32_t tag : line.tags) {
    if (tag > 0) {
      size_t length = std::min<size_t>(tag, line.text.size() - column);
      append_escaped_html(html, line.text.data() + column, length);
      column += length;
    } else if (tag < 0) {
      if (is_open_tag(tag)) {
        int32_t scope_id = -tag;
        auto entry = class_names_by_scope_id_.find(scope_id);
        if (entry == class_names_by_scope_id_.end()) {
          missing_scope_ids->push_back(scope_id);
          complete = false;
          *html += u"<span>";
        } else {
          *html += u"<span class=\"";
          append_escaped_html(html, entry->second.data(), entry->second.size());
          *html += u"\">";
        }
      } else {
        *html += u"</span>";
      }
    }
  }
  *html += u"</span>";
  if (column == 0) *html += u' ';
  auto &fold_character = params_.fold_character;
  if (!fold_character.empty() && line.text.size() >= fold_character.size() &&
      line.text.compare(line.text.size() - fold_character.size(), fold_character.size(), fold_character) == 0) {
    *html += u'\uFEFF';
  }
  return complete;
}

bool DisplayLayer::build_render_plan(uint32_t planner_id, uint32_t start_row,
                                     const vector<const ScreenLine *> &lines,
                                     RenderPlan *plan, vector<int32_t> *missing_scope_ids) {
  auto &previous = render_planners_[planner_id];

  std::unordered_map<uint32_t, uint32_t> previous_row_by_id;
  std::unordered_map<uint32_t, uint32_t> previous_id_by_row;
  previous_row_by_id.reserve(previous.size());
  previous_id_by_row.reserve(previous.size());
  for (auto &rendered : previous) {
    previous_row_by_id[rendered.id] = rendered.row;
    previous_id_by_row[rendered.row] = rendered.id;
  }

  std::unordered_map<uint32_t, bool> current_ids;
  current_ids.reserve(lines.size());
  for (auto line : lines) current_ids[line->id] = true;

  std::unordered_map<uint32_t, bool> replaced_ids;
  for (size_t i = 0; i < lines.size(); i++) {
    const ScreenLine &line = *lines[i];
    uint32_t row = start_row + i;
    auto previous_row = previous_row_by_id.find(line.id);
    if (previous_row != previous_row_by_id.end()) {
      uint32_t op = previous_row->second == row ? RENDER_OP_KEEP : RENDER_OP_MOVE;
      plan->ops.insert(plan->ops.end(), {op, line.id, row, 0});
      continue;
    }

    uint32_t op = RENDER_OP_INSERT;
    uint32_t old_id = 0;
    auto previous_id = previous_id_by_row.find(row);
    if (previous_id != previous_id_by_row.end() &&
        !current_ids.count(previous_id->second) &&
        !replaced_ids.count(previous_id->second)) {
      op = RENDER_OP_REPLACE;
      old_id = previous_id->second;
      replaced_ids[old_id] = true;
    }
    plan->ops.insert(plan->ops.end(), {op, line.id, row, old_id});

    u16string html;
    append_line_html(line, &html, missing_scope_ids);
    plan->html_line_ids.push_back(line.id);
    plan->html.push_back(std::move(html));
  }

  if (!missing_scope_ids->empty()) {
    std::sort(missing_scope_ids->begin(), missing_scope_ids->end());
    missing_scope_ids->erase(
      std::unique(missing_scope_ids->begin(), missing_scope_ids->end()),
      missing_scope_ids->end()
    );
    return false;
  }

  for (auto &rendered : previous) {
    if (!current_ids.count(rendered.id) && !replaced_ids.count(rendered.id)) {
      plan->ops.insert(plan->ops.end(), {RENDER_OP_REMOVE, rendered.id, rendered.row, 0});
    }
  }

  previous.clear();
  for (size_t i = 0; i < lines.size(); i++) {
    previous.push_back({static_cast<uint32_t>(start_row + i), lines[i]->id});
  }
  return true;
}

vector<DisplayLayer::BuiltInScope> DisplayLayer::take_new_built_in_scopes() {
  vector<BuiltInScope> result;
  result.swap(new_built_in_scopes_);
  return result;
}

// Change handling

void DisplayLayer::buffer_will_change(uint32_t old_end_row) {
  invalidate_line_cache();
  uint32_t count = line_count();
  uint32_t end_row = old_end_row;
  while (end_row + 1 < count && line_length(end_row + 1) == 0) {
    end_row++;
  }
  populate_spatial_index_if_needed(end_row + 1, INF);
  // The buffer changes next; don't let anything read the old lines.
  invalidate_line_cache();
}

DisplayLayer::ScreenChange DisplayLayer::buffer_did_change(uint32_t start_row, uint32_t start_column,
                                                           uint32_t old_end_row, uint32_t old_end_column,
                                                           uint32_t new_end_row, uint32_t new_end_column) {
  invalidate_line_cache();

  if (start_row == old_end_row && start_row == new_end_row) {
    auto change = update_soft_wraps_in_row(start_row, start_column, old_end_column, new_end_column);
    if (change) return *change;
  }

  // Indent guides on sequences of blank lines are affected by the content of
  // adjacent lines.
  if (params_.show_indent_guides) {
    while (start_row > 0) {
      if (line_length(start_row - 1) > 0) break;
      start_row--;
    }

    uint32_t last = last_row();
    while (new_end_row < last) {
      if (line_length(new_end_row + 1) > 0) break;
      old_end_row++;
      new_end_row++;
    }
  }

  indexed_buffer_row_count_ += static_cast<int64_t>(new_end_row) - static_cast<int64_t>(old_end_row);
  return update_spatial_index(start_row, old_end_row + 1, new_end_row + 1, INF);
}

DisplayLayer::ScreenChange DisplayLayer::invalidate_highlighting(uint32_t start_row, uint32_t end_row) {
  validate_line_cache();
  populate_spatial_index_if_needed(saturating_add(end_row, 1), INF);
  uint32_t start_buffer_row = find_boundary_preceding_buffer_row(start_row);
  uint32_t end_buffer_row = find_boundary_following_buffer_row(saturating_add(end_row, 1));
  uint32_t start_screen_row = translate_buffer_position_with_spatial_index(
    Point(start_buffer_row, 0), ClipDirection::Backward
  ).row;
  uint32_t end_screen_row = translate_buffer_position_with_spatial_index(
    Point(end_buffer_row, 0), ClipDirection::Backward
  ).row;
  uint32_t extent = end_screen_row - start_screen_row;
  for (uint32_t row = start_screen_row; row < end_screen_row && row < cached_screen_lines_.size(); row++) {
    cached_screen_lines_[row].reset();
  }
  return {start_screen_row, extent, extent};
}

void DisplayLayer::splice_cache(uint32_t start, uint32_t old_count, uint32_t new_count) {
  // Mirrors `spliceArray` on JS's sparse `cachedScreenLines` array: splicing
  // at or past the end has no observable effect.
  if (start >= cached_screen_lines_.size()) return;
  uint32_t removed = std::min<uint32_t>(old_count, cached_screen_lines_.size() - start);
  auto position = cached_screen_lines_.begin() + start;
  position = cached_screen_lines_.erase(position, position + removed);
  vector<std::unique_ptr<ScreenLine>> inserted(new_count);
  cached_screen_lines_.insert(
    position,
    std::make_move_iterator(inserted.begin()),
    std::make_move_iterator(inserted.end())
  );
}

// Spatial index

bool DisplayLayer::do_background_work(const TimeRemaining &time_remaining) {
  validate_line_cache();
  populate_spatial_index_if_needed(line_count(), INF, &time_remaining);
  return indexed_buffer_row_count_ < line_count();
}

void DisplayLayer::populate_spatial_index_if_needed(uint32_t end_buffer_row, uint32_t end_screen_row,
                                                    const TimeRemaining *time_remaining) {
  end_buffer_row = std::min(line_count(), end_buffer_row);
  if (end_buffer_row > indexed_buffer_row_count_ && end_screen_row > screen_line_lengths_.size()) {
    update_spatial_index(
      static_cast<uint32_t>(std::max<int64_t>(0, indexed_buffer_row_count_)),
      end_buffer_row,
      end_buffer_row,
      end_screen_row,
      time_remaining
    );
  }
}

uint32_t DisplayLayer::find_boundary_preceding_buffer_row(uint32_t buffer_row) {
  while (true) {
    if (buffer_row == 0) return 0;
    Point screen_position = translate_buffer_position_with_spatial_index(
      Point(buffer_row, 0), ClipDirection::Backward
    );
    Point buffer_position = translate_screen_position_with_spatial_index(
      Point(screen_position.row, 0), ClipDirection::Backward, false
    );
    if (screen_position.column == 0 && buffer_position.column == 0) {
      return buffer_position.row;
    } else {
      buffer_row = buffer_position.row;
    }
  }
}

uint32_t DisplayLayer::find_boundary_following_buffer_row(uint32_t buffer_row) {
  while (true) {
    Point screen_position = translate_buffer_position_with_spatial_index(
      Point(buffer_row, 0), ClipDirection::Forward
    );
    if (screen_position.column == 0) {
      return buffer_row;
    } else {
      uint32_t row_length = screen_position.row < screen_line_lengths_.size()
        ? screen_line_lengths_[screen_position.row]
        : INF;
      Point end_of_screen_row(screen_position.row, row_length);
      buffer_row = translate_screen_position_with_spatial_index(
        end_of_screen_row, ClipDirection::Forward, false
      ).row + 1;
    }
  }
}

std::pair<uint32_t, uint32_t> DisplayLayer::find_boundary_following_screen_row(uint32_t screen_row) {
  validate_line_cache();
  while (true) {
    Point buffer_position = translate_screen_position_with_spatial_index(
      Point(screen_row, 0), ClipDirection::Forward, false
    );
    if (buffer_position.column == 0) {
      return {buffer_position.row, screen_row};
    } else {
      Point end_of_buffer_row(buffer_position.row, line_length(buffer_position.row));
      screen_row = translate_buffer_position_with_spatial_index(
        end_of_buffer_row, ClipDirection::Forward
      ).row + 1;
    }
  }
}

vector<MarkerIndex::MarkerId> DisplayLayer::find_folds_intersecting(MarkerIndex *index, uint32_t start_row,
                                                                    uint32_t end_row) {
  auto ids = index->find_intersecting(Point(start_row, 0), Point(end_row, INF));
  vector<MarkerIndex::MarkerId> result(ids.begin(), ids.end());
  // Like MarkerLayer::findMarkers, which sorts with MarkerIndex::compare.
  std::stable_sort(result.begin(), result.end(), [index](MarkerIndex::MarkerId a, MarkerIndex::MarkerId b) {
    return index->compare(a, b) < 0;
  });
  return result;
}

std::map<Point, Point> DisplayLayer::compute_folds_in_buffer_row_range(uint32_t start_buffer_row,
                                                                      uint32_t end_buffer_row) {
  std::map<Point, Point> folds;
  MarkerIndex *index = folds_index_ ? folds_index_() : nullptr;
  if (!index) return folds;

  // JS clamps the (possibly negative) end row of `intersectsRowRange` to 0.
  auto fold_markers = find_folds_intersecting(
    index, start_buffer_row, end_buffer_row == 0 ? 0 : end_buffer_row - 1
  );

  // If the given buffer range exceeds the indexed range, we need to ensure
  // we consider any folds that intersect the combined row range of the
  // initially-queried folds, since we couldn't use the index to expand the
  // row range to account for these extra folds ahead of time.
  if (end_buffer_row >= indexed_buffer_row_count_) {
    for (size_t i = 0; i < fold_markers.size(); i++) {
      Point marker_end = index->get_end(fold_markers[i]);
      bool has_next = i + 1 < fold_markers.size();
      if (marker_end.row >= end_buffer_row &&
          (!has_next || index->get_end(fold_markers[i + 1]).row < marker_end.row)) {
        auto intersecting = find_folds_intersecting(index, marker_end.row, marker_end.row);
        end_buffer_row = marker_end.row + 1;
        fold_markers.erase(fold_markers.begin() + i, fold_markers.end());
        fold_markers.insert(fold_markers.end(), intersecting.begin(), intersecting.end());
      }
    }
  }

  for (size_t i = 0; i < fold_markers.size(); i++) {
    Point fold_start = index->get_start(fold_markers[i]);
    Point fold_end = index->get_end(fold_markers[i]);

    // Merge overlapping folds
    while (i + 1 < fold_markers.size()) {
      auto next_fold_marker = fold_markers[i + 1];
      if (index->get_start(next_fold_marker) < fold_end) {
        Point next_end = index->get_end(next_fold_marker);
        if (fold_end < next_end) fold_end = next_end;
        i++;
      } else {
        break;
      }
    }

    // Add non-empty folds to the returned result
    if (fold_start < fold_end) folds[fold_start] = fold_end;
  }

  return folds;
}

DisplayLayer::ScreenChange DisplayLayer::update_spatial_index(uint32_t start_buffer_row, uint32_t old_end_buffer_row,
                                                              uint32_t new_end_buffer_row, uint32_t end_screen_row,
                                                              const TimeRemaining *time_remaining) {
  uint32_t original_old_end_buffer_row = old_end_buffer_row;
  start_buffer_row = find_boundary_preceding_buffer_row(start_buffer_row);
  old_end_buffer_row = find_boundary_following_buffer_row(old_end_buffer_row);
  new_end_buffer_row += old_end_buffer_row - original_old_end_buffer_row;

  uint32_t start_screen_row = translate_buffer_position_with_spatial_index(
    Point(start_buffer_row, 0), ClipDirection::Backward
  ).row;
  uint32_t old_end_screen_row = translate_buffer_position_with_spatial_index(
    Point(old_end_buffer_row, 0), ClipDirection::Backward
  ).row;
  spatial_index_.splice_old(
    Point(start_buffer_row, 0),
    Point(old_end_buffer_row - start_buffer_row, 0),
    Point(new_end_buffer_row - start_buffer_row, 0)
  );

  auto folds = compute_folds_in_buffer_row_range(start_buffer_row, new_end_buffer_row);
  auto next_fold = folds.begin();

  vector<uint32_t> inserted_screen_line_lengths;
  vector<uint32_t> inserted_tab_counts;
  vector<int32_t> inserted_wrap_overflow_offsets;
  vector<double> inserted_wrap_carried_widths;
  vector<int64_t> current_screen_line_tab_columns;
  int64_t rightmost_inserted_row = 0;
  int64_t rightmost_inserted_column = -1;
  int32_t current_wrap_overflow_offset = -1;
  double current_wrap_carried_width = 0;
  uint32_t buffer_row = start_buffer_row;
  uint32_t screen_row = start_screen_row;
  uint32_t buffer_column = 0;
  int64_t unexpanded_screen_column = 0;
  int64_t expanded_screen_column = 0;

  const double soft_wrap_column = params_.soft_wrap_column;
  const int64_t tab_length = params_.tab_length;
  const double space_ratio = ratio_for_character(' ');
  const u16string &fold_character = params_.fold_character;
  const bool fold_has_character = !fold_character.empty();
  const uint16_t fold_code_unit = fold_has_character ? fold_character[0] : 0;
  const bool fold_is_space = fold_character == u" ";
  const bool fold_is_tab = fold_character == u"\t";
  const uint32_t buffer_line_count = line_count();

  u16string buffer_line;

  while (true) {
    if (buffer_row >= new_end_buffer_row) break;
    if (screen_row >= end_screen_row && buffer_column == 0) break;
    if (time_remaining && (*time_remaining)() < 2) break;
    if (buffer_row >= buffer_line_count) break;
    buffer_line = line_for_row(buffer_row);
    int64_t buffer_line_length = buffer_line.size();
    current_screen_line_tab_columns.clear();
    double screen_line_width = 0;
    int64_t last_wrap_boundary_unexpanded_screen_column = 0;
    int64_t last_wrap_boundary_expanded_screen_column = 0;
    double last_wrap_boundary_screen_line_width = 0;
    int64_t first_non_whitespace_screen_column = -1;

    while (buffer_column <= buffer_line_length) {
      Point position(buffer_row, buffer_column);
      while (next_fold != folds.end() && next_fold->first < position) ++next_fold;
      const Point *fold_end = (next_fold != folds.end() && next_fold->first == position) ? &next_fold->second : nullptr;

      bool has_previous_character = buffer_column > 0;
      uint16_t previous_character = has_previous_character ? buffer_line[buffer_column - 1] : 0;
      bool has_character;
      uint16_t character;
      bool character_is_space;
      bool character_is_tab;
      if (fold_end) {
        has_character = fold_has_character;
        character = fold_code_unit;
        character_is_space = fold_is_space;
        character_is_tab = fold_is_tab;
      } else {
        has_character = buffer_column < buffer_line_length;
        character = has_character ? buffer_line[buffer_column] : 0;
        character_is_space = has_character && character == ' ';
        character_is_tab = has_character && character == '\t';
      }

      // Are we in leading whitespace? If yes, record the *end* of the leading
      // whitespace if we've reached a non whitespace character. If no, record
      // the current column if it is a viable soft wrap boundary.
      if (first_non_whitespace_screen_column < 0) {
        if (!character_is_space && !character_is_tab) {
          first_non_whitespace_screen_column = expanded_screen_column;
        }
      } else {
        if (has_previous_character && has_character &&
            is_wrap_boundary(previous_character, character, fold_end != nullptr)) {
          last_wrap_boundary_unexpanded_screen_column = unexpanded_screen_column;
          last_wrap_boundary_expanded_screen_column = expanded_screen_column;
          last_wrap_boundary_screen_line_width = screen_line_width;
        }
      }

      // Determine the on-screen width of the character for soft-wrap calculations
      double character_width;
      if (character_is_tab) {
        int64_t distance_to_next_tab_stop = tab_length - (expanded_screen_column % tab_length);
        character_width = space_ratio * distance_to_next_tab_stop;
      } else if (has_character) {
        character_width = fold_end ? ratio_for_fold_character() : ratio_for_character(character);
      } else {
        character_width = 0;
      }

      bool insert_soft_line_break =
        screen_line_width > 0 && character_width > 0 &&
        screen_line_width + character_width > soft_wrap_column &&
        has_previous_character && has_character &&
        !character_classes::is_character_pair(previous_character, character);

      if (insert_soft_line_break) {
        int64_t indent_length = (first_non_whitespace_screen_column < soft_wrap_column)
          ? std::max<int64_t>(0, first_non_whitespace_screen_column)
          : 0;
        if (indent_length + params_.soft_wrap_hanging_indent < soft_wrap_column) {
          indent_length += params_.soft_wrap_hanging_indent;
        }

        int64_t unexpanded_wrap_column = last_wrap_boundary_unexpanded_screen_column
          ? last_wrap_boundary_unexpanded_screen_column : unexpanded_screen_column;
        int64_t expanded_wrap_column = last_wrap_boundary_expanded_screen_column
          ? last_wrap_boundary_expanded_screen_column : expanded_screen_column;
        double wrap_width = last_wrap_boundary_screen_line_width
          ? last_wrap_boundary_screen_line_width : screen_line_width;
        spatial_index_.splice(
          Point(screen_row, static_cast<uint32_t>(unexpanded_wrap_column)),
          Point(0, 0),
          Point(1, static_cast<uint32_t>(indent_length))
        );

        inserted_screen_line_lengths.push_back(static_cast<uint32_t>(expanded_wrap_column));
        inserted_wrap_overflow_offsets.push_back(current_wrap_overflow_offset);
        inserted_wrap_carried_widths.push_back(current_wrap_carried_width);
        current_wrap_overflow_offset = static_cast<int32_t>(unexpanded_screen_column - unexpanded_wrap_column);
        current_wrap_carried_width = screen_line_width - wrap_width;
        if (expanded_wrap_column > rightmost_inserted_column) {
          rightmost_inserted_row = screen_row;
          rightmost_inserted_column = expanded_wrap_column;
        }
        screen_row++;

        // To determine the expanded screen column following the wrap, we need
        // to re-expand each tab following the wrap boundary, because tabs may
        // take on different lengths due to starting at different screen columns.
        int64_t unexpanded_screen_column_after_last_tab = indent_length;
        int64_t expanded_screen_column_after_last_tab = indent_length;
        size_t tab_count_preceding_wrap = 0;
        for (size_t i = 0; i < current_screen_line_tab_columns.size(); i++) {
          int64_t tab_column = current_screen_line_tab_columns[i];
          if (tab_column < unexpanded_wrap_column) {
            tab_count_preceding_wrap++;
          } else {
            int64_t tab_column_after_wrap = indent_length + tab_column - unexpanded_wrap_column;
            expanded_screen_column_after_last_tab += tab_column_after_wrap - unexpanded_screen_column_after_last_tab;
            expanded_screen_column_after_last_tab += tab_length - (expanded_screen_column_after_last_tab % tab_length);
            unexpanded_screen_column_after_last_tab = tab_column_after_wrap + 1;
            current_screen_line_tab_columns[i - tab_count_preceding_wrap] = tab_column_after_wrap;
          }
        }
        inserted_tab_counts.push_back(tab_count_preceding_wrap);
        current_screen_line_tab_columns.resize(current_screen_line_tab_columns.size() - tab_count_preceding_wrap);

        unexpanded_screen_column = unexpanded_screen_column - unexpanded_wrap_column + indent_length;
        expanded_screen_column = expanded_screen_column_after_last_tab + unexpanded_screen_column - unexpanded_screen_column_after_last_tab;
        screen_line_width = (indent_length * space_ratio) + (screen_line_width - wrap_width);

        last_wrap_boundary_unexpanded_screen_column = 0;
        last_wrap_boundary_expanded_screen_column = 0;
        last_wrap_boundary_screen_line_width = 0;
      }

      // If there is a fold at this position, splice it into the spatial index
      // and jump to the end of the fold.
      if (fold_end) {
        Point end = *fold_end;
        spatial_index_.splice(
          Point(screen_row, static_cast<uint32_t>(unexpanded_screen_column)),
          traversal(end, Point(buffer_row, buffer_column)),
          Point(0, 1)
        );
        unexpanded_screen_column++;
        expanded_screen_column++;
        screen_line_width += character_width;
        buffer_row = end.row;
        buffer_column = end.column;
        buffer_line = line_for_row(buffer_row);
        buffer_line_length = buffer_line.size();
      } else {
        // If there is no fold at this position, check if we need to handle
        // a hard tab at this position and advance by a single buffer column.
        if (character_is_tab) {
          current_screen_line_tab_columns.push_back(unexpanded_screen_column);
          int64_t distance_to_next_tab_stop = tab_length - (expanded_screen_column % tab_length);
          expanded_screen_column += distance_to_next_tab_stop;
          screen_line_width += distance_to_next_tab_stop * space_ratio;
        } else {
          expanded_screen_column++;
          screen_line_width += character_width;
        }
        unexpanded_screen_column++;
        buffer_column++;
      }
    }

    expanded_screen_column--;
    inserted_screen_line_lengths.push_back(static_cast<uint32_t>(expanded_screen_column));
    inserted_tab_counts.push_back(current_screen_line_tab_columns.size());
    inserted_wrap_overflow_offsets.push_back(current_wrap_overflow_offset);
    inserted_wrap_carried_widths.push_back(current_wrap_carried_width);
    current_wrap_overflow_offset = -1;
    current_wrap_carried_width = 0;
    if (expanded_screen_column > rightmost_inserted_column) {
      rightmost_inserted_row = screen_row;
      rightmost_inserted_column = expanded_screen_column;
    }

    buffer_row++;
    buffer_column = 0;

    screen_row++;
    unexpanded_screen_column = 0;
    expanded_screen_column = 0;
  }

  if (buffer_row > indexed_buffer_row_count_) {
    indexed_buffer_row_count_ = buffer_row;
    if (buffer_row == buffer_line_count) {
      spatial_index_.rebalance();
    }
  }

  uint32_t old_screen_row_count = old_end_screen_row - start_screen_row;
  splice_screen_rows(start_screen_row, old_screen_row_count, inserted_screen_line_lengths,
                     inserted_tab_counts, inserted_wrap_overflow_offsets, inserted_wrap_carried_widths,
                     rightmost_inserted_row, rightmost_inserted_column);
  return {start_screen_row, old_screen_row_count, static_cast<uint32_t>(inserted_screen_line_lengths.size())};
}

// Replaces the per-row data of `old_screen_row_count` screen rows starting at
// `start_screen_row` and updates the rightmost position and the screen line
// cache accordingly.
void DisplayLayer::splice_screen_rows(uint32_t start_screen_row, uint32_t old_screen_row_count,
                                      const vector<uint32_t> &inserted_screen_line_lengths,
                                      const vector<uint32_t> &inserted_tab_counts,
                                      const vector<int32_t> &inserted_wrap_overflow_offsets,
                                      const vector<double> &inserted_wrap_carried_widths,
                                      int64_t rightmost_inserted_row, int64_t rightmost_inserted_column) {
  uint32_t lengths_start = std::min<uint32_t>(start_screen_row, screen_line_lengths_.size());
  uint32_t lengths_removed = std::min<uint32_t>(old_screen_row_count, screen_line_lengths_.size() - lengths_start);
  screen_line_lengths_.erase(
    screen_line_lengths_.begin() + lengths_start,
    screen_line_lengths_.begin() + lengths_start + lengths_removed
  );
  screen_line_lengths_.insert(
    screen_line_lengths_.begin() + lengths_start,
    inserted_screen_line_lengths.begin(), inserted_screen_line_lengths.end()
  );
  uint32_t tabs_start = std::min<uint32_t>(start_screen_row, tab_counts_.size());
  uint32_t tabs_removed = std::min<uint32_t>(old_screen_row_count, tab_counts_.size() - tabs_start);
  tab_counts_.erase(tab_counts_.begin() + tabs_start, tab_counts_.begin() + tabs_start + tabs_removed);
  tab_counts_.insert(tab_counts_.begin() + tabs_start, inserted_tab_counts.begin(), inserted_tab_counts.end());
  uint32_t offsets_start = std::min<uint32_t>(start_screen_row, wrap_overflow_offsets_.size());
  uint32_t offsets_removed = std::min<uint32_t>(old_screen_row_count, wrap_overflow_offsets_.size() - offsets_start);
  wrap_overflow_offsets_.erase(
    wrap_overflow_offsets_.begin() + offsets_start,
    wrap_overflow_offsets_.begin() + offsets_start + offsets_removed
  );
  wrap_overflow_offsets_.insert(
    wrap_overflow_offsets_.begin() + offsets_start,
    inserted_wrap_overflow_offsets.begin(), inserted_wrap_overflow_offsets.end()
  );
  uint32_t widths_start = std::min<uint32_t>(start_screen_row, wrap_carried_widths_.size());
  uint32_t widths_removed = std::min<uint32_t>(old_screen_row_count, wrap_carried_widths_.size() - widths_start);
  wrap_carried_widths_.erase(
    wrap_carried_widths_.begin() + widths_start,
    wrap_carried_widths_.begin() + widths_start + widths_removed
  );
  wrap_carried_widths_.insert(
    wrap_carried_widths_.begin() + widths_start,
    inserted_wrap_carried_widths.begin(), inserted_wrap_carried_widths.end()
  );

  uint32_t last_removed_screen_row = start_screen_row + old_screen_row_count;
  if (rightmost_inserted_column > static_cast<int64_t>(rightmost_screen_position_.column)) {
    rightmost_screen_position_ = Point(rightmost_inserted_row, rightmost_inserted_column);
  } else if (last_removed_screen_row < rightmost_screen_position_.row) {
    rightmost_screen_position_.row += inserted_screen_line_lengths.size() - old_screen_row_count;
  } else if (start_screen_row <= rightmost_screen_position_.row) {
    rightmost_screen_position_ = Point(0, 0);
    for (uint32_t row = 0, row_count = screen_line_lengths_.size(); row < row_count; row++) {
      if (screen_line_lengths_[row] > rightmost_screen_position_.column) {
        rightmost_screen_position_ = Point(row, screen_line_lengths_[row]);
      }
    }
  }

  splice_cache(start_screen_row, old_screen_row_count, inserted_screen_line_lengths.size());
}

// Re-wraps only the part of a soft-wrapped buffer row affected by an edit
// within that row, instead of the whole row (which is linear in the length of
// the line, and very slow when typing in long lines).
//
// Wrap decisions only depend on the text back to the previous wrap, so the
// first screen row that can change is the one before the row containing the
// edit. The wrapping state right after a wrap is determined by the wrap, its
// indent, the character that overflowed the previous row and the width
// carried over (which keeps the widths tabs had before the wrap); we store
// the last two per screen row, so we can restart right there exactly. We
// stop as soon as a wrap past the edit matches an old one (shifted by the
// edit) in all of these: everything after it is unchanged.
//
// Returns nothing when the edit needs a full update: the row has folds or
// isn't indexed yet, indent guides are shown, or the first non-whitespace
// character isn't on the row's first screen row before the edit.
optional<DisplayLayer::ScreenChange> DisplayLayer::update_soft_wraps_in_row(uint32_t row, uint32_t edit_start,
                                                                           uint32_t old_edit_end, uint32_t new_edit_end) {
  if (!std::isfinite(params_.soft_wrap_column) || params_.show_indent_guides) return optional<ScreenChange>{};
  if (static_cast<int64_t>(row) >= indexed_buffer_row_count_) return optional<ScreenChange>{};
  if (old_edit_end < edit_start || new_edit_end < edit_start) return optional<ScreenChange>{};

  const u16string &line = line_for_row(row);
  const int64_t line_length = line.size();
  const int64_t delta = static_cast<int64_t>(new_edit_end) - static_cast<int64_t>(old_edit_end);
  const int64_t old_line_length = line_length - delta;
  if (old_line_length < old_edit_end) return optional<ScreenChange>{};

  // The row's current layout, still in pre-edit buffer coordinates.
  const uint32_t start_screen_row = translate_buffer_position_with_spatial_index(
    Point(row, 0), ClipDirection::Backward
  ).row;
  const uint32_t end_screen_row = translate_buffer_position_with_spatial_index(
    Point(row, static_cast<uint32_t>(old_line_length)), ClipDirection::Forward
  ).row;
  if (end_screen_row >= wrap_overflow_offsets_.size()) return optional<ScreenChange>{};
  vector<Patch::Change> wraps;
  for (auto &hunk : spatial_index_.grab_changes_in_new_range(Point(start_screen_row, 0), Point(end_screen_row + 1, 0))) {
    if (hunk.old_start.row > row || hunk.old_end.row < row) continue;
    if (!is_soft_wrap_hunk(hunk)) return optional<ScreenChange>{};  // A fold touches this row.
    wraps.push_back(hunk);
  }
  if (end_screen_row - start_screen_row != wraps.size()) return optional<ScreenChange>{};
  // The overflow offset of the row starting at wraps[i] is at
  // wrap_overflow_offsets_[start_screen_row + i + 1].
  auto overflow_offset_after_wrap = [&](size_t i) {
    return wrap_overflow_offsets_[start_screen_row + i + 1];
  };
  auto carried_width_after_wrap = [&](size_t i) {
    return wrap_carried_widths_[start_screen_row + i + 1];
  };
  if (end_screen_row >= wrap_carried_widths_.size()) return optional<ScreenChange>{};

  const double soft_wrap_column = params_.soft_wrap_column;
  const int64_t tab_length = params_.tab_length;
  const double space_ratio = ratio_for_character(' ');

  // The screen row (within this buffer row) containing the edit, and the one
  // before it: the first that can change.
  size_t edit_screen_row = 0;
  while (edit_screen_row < wraps.size() && wraps[edit_screen_row].old_start.column <= edit_start) edit_screen_row++;
  size_t restart_screen_row = edit_screen_row > 0 ? edit_screen_row - 1 : 0;

  // Restarting after a wrap needs the expanded column of the first
  // non-whitespace character, which must lie before the first wrap and the
  // edit.
  int64_t first_non_whitespace_screen_column = -1;
  if (restart_screen_row > 0) {
    int64_t expanded = 0;
    int64_t column = 0;
    for (; column < line_length; column++) {
      if (line[column] == ' ') expanded++;
      else if (line[column] == '\t') expanded += tab_length - (expanded % tab_length);
      else break;
    }
    if (column >= line_length || column >= edit_start || column >= wraps[0].old_start.column ||
        overflow_offset_after_wrap(restart_screen_row - 1) < 0) {
      restart_screen_row = 0;
    } else {
      first_non_whitespace_screen_column = expanded;
    }
  }

  const int64_t restart_column = restart_screen_row > 0 ? wraps[restart_screen_row - 1].old_start.column : 0;
  const int64_t restart_indent = restart_screen_row > 0 ? wraps[restart_screen_row - 1].new_end.column : 0;
  const int32_t restart_overflow_offset = restart_screen_row > 0 ? overflow_offset_after_wrap(restart_screen_row - 1) : -1;
  const double restart_carried_width = restart_screen_row > 0 ? carried_width_after_wrap(restart_screen_row - 1) : 0;
  // After a wrap, the full algorithm continues with the overflowing character
  // (without checking it again), so that is where we resume.
  const int64_t scan_column = restart_screen_row > 0 ? restart_column + restart_overflow_offset : 0;
  if (scan_column > line_length) return optional<ScreenChange>{};

  struct NewWrap {
    uint32_t screen_row;
    uint32_t unexpanded_column;
    uint32_t indent;
  };
  vector<NewWrap> new_wraps;
  vector<uint32_t> inserted_screen_line_lengths;
  vector<uint32_t> inserted_tab_counts;
  vector<int32_t> inserted_wrap_overflow_offsets;
  vector<double> inserted_wrap_carried_widths;
  vector<int64_t> current_screen_line_tab_columns;
  int64_t rightmost_inserted_row = 0;
  int64_t rightmost_inserted_column = -1;
  size_t next_old_wrap = edit_screen_row;
  optional<size_t> resync_wrap;
  int32_t current_wrap_overflow_offset = restart_overflow_offset;
  double current_wrap_carried_width = restart_carried_width;

  // Rebuild the state right after the wrap starting the restart row, as the
  // full algorithm left it: the characters up to the overflowing one are on
  // this row, their tabs re-expanded after the indent.
  uint32_t screen_row = start_screen_row + restart_screen_row;
  int64_t unexpanded_screen_column = restart_indent + (scan_column - restart_column);
  int64_t expanded_screen_column;
  {
    int64_t unexpanded_screen_column_after_last_tab = restart_indent;
    int64_t expanded_screen_column_after_last_tab = restart_indent;
    for (int64_t column = restart_column; column < scan_column; column++) {
      if (line[column] == '\t') {
        int64_t tab_column = restart_indent + (column - restart_column);
        current_screen_line_tab_columns.push_back(tab_column);
        expanded_screen_column_after_last_tab += tab_column - unexpanded_screen_column_after_last_tab;
        expanded_screen_column_after_last_tab += tab_length - (expanded_screen_column_after_last_tab % tab_length);
        unexpanded_screen_column_after_last_tab = tab_column + 1;
      }
    }
    expanded_screen_column = expanded_screen_column_after_last_tab + unexpanded_screen_column - unexpanded_screen_column_after_last_tab;
  }
  double screen_line_width = (restart_indent * space_ratio) + restart_carried_width;
  int64_t last_wrap_boundary_unexpanded_screen_column = 0;
  int64_t last_wrap_boundary_expanded_screen_column = 0;
  double last_wrap_boundary_screen_line_width = 0;
  const int64_t unchecked_column = restart_screen_row > 0 ? scan_column : -1;

  // Mirrors the per-character loop of update_spatial_index (without folds).
  for (int64_t buffer_column = scan_column; buffer_column <= line_length; buffer_column++) {
    const bool checked = buffer_column != unchecked_column;
    bool has_previous_character = buffer_column > 0;
    uint16_t previous_character = has_previous_character ? line[buffer_column - 1] : 0;
    bool has_character = buffer_column < line_length;
    uint16_t character = has_character ? line[buffer_column] : 0;
    bool character_is_tab = has_character && character == '\t';
    bool character_is_space = has_character && character == ' ';

    if (first_non_whitespace_screen_column < 0) {
      if (!character_is_space && !character_is_tab) {
        first_non_whitespace_screen_column = expanded_screen_column;
      }
    } else if (checked && has_previous_character && has_character &&
               is_wrap_boundary(previous_character, character, false)) {
      last_wrap_boundary_unexpanded_screen_column = unexpanded_screen_column;
      last_wrap_boundary_expanded_screen_column = expanded_screen_column;
      last_wrap_boundary_screen_line_width = screen_line_width;
    }

    double character_width;
    if (character_is_tab) {
      character_width = space_ratio * (tab_length - (expanded_screen_column % tab_length));
    } else if (has_character) {
      character_width = ratio_for_character(character);
    } else {
      character_width = 0;
    }

    bool insert_soft_line_break =
      checked &&
      screen_line_width > 0 && character_width > 0 &&
      screen_line_width + character_width > soft_wrap_column &&
      has_previous_character && has_character &&
      !character_classes::is_character_pair(previous_character, character);

    if (insert_soft_line_break) {
      int64_t indent_length = (first_non_whitespace_screen_column < soft_wrap_column)
        ? std::max<int64_t>(0, first_non_whitespace_screen_column)
        : 0;
      if (indent_length + params_.soft_wrap_hanging_indent < soft_wrap_column) {
        indent_length += params_.soft_wrap_hanging_indent;
      }

      int64_t unexpanded_wrap_column = last_wrap_boundary_unexpanded_screen_column
        ? last_wrap_boundary_unexpanded_screen_column : unexpanded_screen_column;
      int64_t expanded_wrap_column = last_wrap_boundary_expanded_screen_column
        ? last_wrap_boundary_expanded_screen_column : expanded_screen_column;
      double wrap_width = last_wrap_boundary_screen_line_width
        ? last_wrap_boundary_screen_line_width : screen_line_width;
      int64_t wrap_buffer_column = buffer_column - (unexpanded_screen_column - unexpanded_wrap_column);
      int32_t overflow_offset = static_cast<int32_t>(unexpanded_screen_column - unexpanded_wrap_column);
      double carried_width = screen_line_width - wrap_width;

      size_t tab_count_preceding_wrap = 0;
      for (int64_t tab_column : current_screen_line_tab_columns) {
        if (tab_column < unexpanded_wrap_column) tab_count_preceding_wrap++;
      }

      inserted_screen_line_lengths.push_back(static_cast<uint32_t>(expanded_wrap_column));
      inserted_tab_counts.push_back(tab_count_preceding_wrap);
      inserted_wrap_overflow_offsets.push_back(current_wrap_overflow_offset);
      inserted_wrap_carried_widths.push_back(current_wrap_carried_width);
      if (expanded_wrap_column > rightmost_inserted_column) {
        rightmost_inserted_row = screen_row;
        rightmost_inserted_column = expanded_wrap_column;
      }

      // Past the edit, a wrap identical to an old one means the rest of the
      // row is unchanged.
      if (wrap_buffer_column >= new_edit_end) {
        int64_t old_column = wrap_buffer_column - delta;
        while (next_old_wrap < wraps.size() && wraps[next_old_wrap].old_start.column < old_column) next_old_wrap++;
        if (next_old_wrap < wraps.size() &&
            wraps[next_old_wrap].old_start.column == old_column &&
            wraps[next_old_wrap].new_end.column == indent_length &&
            overflow_offset_after_wrap(next_old_wrap) == overflow_offset &&
            carried_width_after_wrap(next_old_wrap) == carried_width) {
          resync_wrap = next_old_wrap;
          break;
        }
      }

      new_wraps.push_back({screen_row, static_cast<uint32_t>(unexpanded_wrap_column), static_cast<uint32_t>(indent_length)});
      current_wrap_overflow_offset = overflow_offset;
      current_wrap_carried_width = carried_width;
      screen_row++;

      int64_t unexpanded_screen_column_after_last_tab = indent_length;
      int64_t expanded_screen_column_after_last_tab = indent_length;
      tab_count_preceding_wrap = 0;
      for (size_t i = 0; i < current_screen_line_tab_columns.size(); i++) {
        int64_t tab_column = current_screen_line_tab_columns[i];
        if (tab_column < unexpanded_wrap_column) {
          tab_count_preceding_wrap++;
        } else {
          int64_t tab_column_after_wrap = indent_length + tab_column - unexpanded_wrap_column;
          expanded_screen_column_after_last_tab += tab_column_after_wrap - unexpanded_screen_column_after_last_tab;
          expanded_screen_column_after_last_tab += tab_length - (expanded_screen_column_after_last_tab % tab_length);
          unexpanded_screen_column_after_last_tab = tab_column_after_wrap + 1;
          current_screen_line_tab_columns[i - tab_count_preceding_wrap] = tab_column_after_wrap;
        }
      }
      current_screen_line_tab_columns.resize(current_screen_line_tab_columns.size() - tab_count_preceding_wrap);

      unexpanded_screen_column = unexpanded_screen_column - unexpanded_wrap_column + indent_length;
      expanded_screen_column = expanded_screen_column_after_last_tab + unexpanded_screen_column - unexpanded_screen_column_after_last_tab;
      screen_line_width = (indent_length * space_ratio) + (screen_line_width - wrap_width);

      last_wrap_boundary_unexpanded_screen_column = 0;
      last_wrap_boundary_expanded_screen_column = 0;
      last_wrap_boundary_screen_line_width = 0;
    }

    if (character_is_tab) {
      current_screen_line_tab_columns.push_back(unexpanded_screen_column);
      int64_t distance_to_next_tab_stop = tab_length - (expanded_screen_column % tab_length);
      expanded_screen_column += distance_to_next_tab_stop;
      screen_line_width += distance_to_next_tab_stop * space_ratio;
    } else {
      expanded_screen_column++;
      screen_line_width += character_width;
    }
    unexpanded_screen_column++;
  }

  if (!resync_wrap) {
    expanded_screen_column--;
    inserted_screen_line_lengths.push_back(static_cast<uint32_t>(expanded_screen_column));
    inserted_tab_counts.push_back(current_screen_line_tab_columns.size());
    inserted_wrap_overflow_offsets.push_back(current_wrap_overflow_offset);
    inserted_wrap_carried_widths.push_back(current_wrap_carried_width);
    if (expanded_screen_column > rightmost_inserted_column) {
      rightmost_inserted_row = screen_row;
      rightmost_inserted_column = expanded_screen_column;
    }
  }

  // Replace the wraps strictly between the restart point and the resync point
  // (or the end of the row); spliceOld keeps the wraps at both ends.
  int64_t old_end_column = resync_wrap ? static_cast<int64_t>(wraps[*resync_wrap].old_start.column) : old_line_length;
  spatial_index_.splice_old(
    Point(row, static_cast<uint32_t>(restart_column)),
    Point(0, static_cast<uint32_t>(old_end_column - restart_column)),
    Point(0, static_cast<uint32_t>(old_end_column + delta - restart_column))
  );
  for (auto &wrap : new_wraps) {
    spatial_index_.splice(Point(wrap.screen_row, wrap.unexpanded_column), Point(0, 0), Point(1, wrap.indent));
  }

  uint32_t first_changed_screen_row = start_screen_row + restart_screen_row;
  uint32_t old_screen_row_count = (resync_wrap ? *resync_wrap : wraps.size()) - restart_screen_row + 1;
  uint32_t new_screen_row_count = inserted_screen_line_lengths.size();
  splice_screen_rows(first_changed_screen_row, old_screen_row_count, inserted_screen_line_lengths,
                     inserted_tab_counts, inserted_wrap_overflow_offsets, inserted_wrap_carried_widths,
                     rightmost_inserted_row, rightmost_inserted_column);
  ScreenChange change{first_changed_screen_row, old_screen_row_count, new_screen_row_count};

  // Whether a space is rendered as trailing whitespace depends on the end of
  // the line. That can only change if nothing but whitespace follows the
  // edit, and then only for spaces after the whitespace preceding the edit:
  // re-render from there to the end of the row.
  int64_t trailing_whitespace_start = line_length;
  while (trailing_whitespace_start > 0 &&
         (line[trailing_whitespace_start - 1] == ' ' || line[trailing_whitespace_start - 1] == '\t')) {
    trailing_whitespace_start--;
  }
  if (trailing_whitespace_start <= new_edit_end) {
    int64_t whitespace_before_edit = edit_start;
    while (whitespace_before_edit > 0 &&
           (line[whitespace_before_edit - 1] == ' ' || line[whitespace_before_edit - 1] == '\t')) {
      whitespace_before_edit--;
    }
    uint32_t affected_start_row = translate_buffer_position_with_spatial_index(
      Point(row, static_cast<uint32_t>(std::min(whitespace_before_edit, trailing_whitespace_start))),
      ClipDirection::Backward
    ).row;
    uint32_t new_end_screen_row = translate_buffer_position_with_spatial_index(
      Point(row, static_cast<uint32_t>(line_length)), ClipDirection::Forward
    ).row + 1;
    uint32_t start = std::min(affected_start_row, change.start);
    uint32_t new_extent = new_end_screen_row - start;
    uint32_t old_extent = new_extent + old_screen_row_count - new_screen_row_count;
    for (uint32_t screen_row = start; screen_row < new_end_screen_row && screen_row < cached_screen_lines_.size(); screen_row++) {
      cached_screen_lines_[screen_row].reset();
    }
    change = ScreenChange{start, old_extent, new_extent};
  }
  return change;
}

// Screen lines

const DisplayLayer::ScreenLine *DisplayLayer::cached_screen_line(uint32_t screen_row) const {
  return screen_row < cached_screen_lines_.size() ? cached_screen_lines_[screen_row].get() : nullptr;
}

// Port of text-buffer's ScreenLineBuilder. Instead of a JS highlight
// iterator it reads a pre-collected list of highlight segments; see
// `DisplayLayer::prepare_screen_lines`.
class DisplayLayer::ScreenLineBuilder {
public:
  ScreenLineBuilder(DisplayLayer &layer) : layer(layer) {}

  // Walks the rows that a build of [start, end) would visit, returning where
  // the highlight iterator would be (re)positioned: at the start of every run
  // of uncached screen rows and at the end of every fold within those runs.
  vector<HighlightSegmentRequest> collect_segments(uint32_t start_screen_row, uint32_t end_screen_row,
                                                   uint32_t *end_buffer_row) {
    vector<HighlightSegmentRequest> segments;
    begin(start_screen_row, end_screen_row);
    *end_buffer_row = this->end_buffer_row;
    bool in_run = false;
    uint32_t last_row = layer.last_row();

    while (screen_row < end_screen_row) {
      if (layer.cached_screen_line(screen_row)) {
        in_run = false;
        skip_cached_screen_row();
        continue;
      }

      if (buffer_position.row > last_row) break;
      if (!in_run) {
        segments.push_back({buffer_position, buffer_position.row});
        in_run = true;
      }

      uint32_t line_length = layer.line_length(buffer_position.row);
      while (true) {
        if (hunk_index < hunks.size() &&
            hunks[hunk_index].old_start.row == buffer_position.row &&
            hunks[hunk_index].old_start.column >= buffer_position.column &&
            hunks[hunk_index].old_start.column <= line_length) {
          auto &hunk = hunks[hunk_index];
          buffer_position.column = hunk.old_start.column;
          if (is_soft_wrap_hunk(hunk)) {
            screen_row++;
            if (screen_row == end_screen_row) {
              segments.back().end_row = buffer_position.row;
              return segments;
            }
          } else {
            segments.back().end_row = buffer_position.row;
            buffer_position = hunk.old_end;
            segments.push_back({buffer_position, buffer_position.row});
            line_length = layer.line_length(buffer_position.row);
          }
          hunk_index++;
        } else {
          segments.back().end_row = buffer_position.row;
          screen_row++;
          buffer_position.row++;
          buffer_position.column = 0;
          break;
        }
      }
    }

    return segments;
  }

  bool build(uint32_t start_screen_row, uint32_t end_screen_row,
             const vector<HighlightSegment> &segments, bool strict,
             vector<const ScreenLine *> *result) {
    this->segments = &segments;
    this->strict = strict;
    segment_valid = false;
    this->result = result;
    segment_index = -1;
    boundary_index = 0;
    requested_start_screen_row = start_screen_row;
    requested_end_screen_row = end_screen_row;
    begin(start_screen_row, end_screen_row);

    bool did_seek_decoration_iterator = false;
    containing_scope_ids.clear();
    scope_ids_to_reopen.clear();
    begin_line();
    uint32_t last_row = layer.last_row();
    bool in_run = false;

    // Loop through all characters spanning the given screen row range, building
    // up screen lines based on the contents of the spatial index and the
    // buffer.
    while (screen_row < end_screen_row) {
      const ScreenLine *cached_screen_line = layer.cached_screen_line(screen_row);
      if (cached_screen_line) {
        in_run = false;
        push_screen_line(cached_screen_line);
        skip_cached_screen_row();
        continue;
      }

      current_built_in_class_name_flags = 0;
      if (buffer_position.row > last_row) break;
      load_line(buffer_position.row);
      trailing_whitespace_start_column = layer.find_trailing_whitespace_start_column(buffer_position.row);
      in_leading_whitespace = true;
      in_trailing_whitespace = false;

      if (!in_run) {
        in_run = true;
        // The JS builder seeks its iterator when it is behind the current
        // position; otherwise it keeps iterating and keeps the scopes waiting
        // to be reopened. Either way the next segment's boundaries are the
        // ones the iterator would produce from here on.
        bool seek = !did_seek_decoration_iterator || buffer_position > iterator_position();
        if (!advance_segment()) return false;
        if (seek) {
          did_seek_decoration_iterator = true;
          if (segment_valid) {
            scope_ids_to_reopen = current_segment().containing_scope_ids;
          } else {
            scope_ids_to_reopen.clear();
          }
        }
      } else if (segment_valid && iterator_position() < buffer_position) {
        // The iterator is stuck on a boundary it could not reach (e.g. past
        // the end of its line), so the JS builder would re-seek here.
        size_t next_index = segment_index + 1;
        if (next_index < this->segments->size() &&
            (*this->segments)[next_index].reseek &&
            (*this->segments)[next_index].start == buffer_position) {
          segment_index = next_index;
          boundary_index = 0;
          scope_ids_to_reopen = current_segment().containing_scope_ids;
        } else {
          skip_boundaries_before(buffer_position);
        }
      }

      int32_t soft_wrap_indent = soft_wrap_indent_for_row(screen_row);
      if (soft_wrap_indent >= 0) {
        in_leading_whitespace = false;
        if (soft_wrap_indent > 0) emit_indent_whitespace(soft_wrap_indent);
      }

      // This loop may visit multiple buffer rows if there are folds and
      // multiple screen rows if there are soft wraps.
      while (buffer_position.column <= buffer_line_length) {
        // Handle folds or soft wraps at the current position.
        while (hunk_index < hunks.size() && hunks[hunk_index].old_start == buffer_position) {
          auto &next_hunk = hunks[hunk_index];
          if (is_soft_wrap_hunk(next_hunk)) {
            emit_soft_wrap(next_hunk);
            if (screen_row == end_screen_row) return true;
          } else {
            if (!emit_fold(next_hunk)) return false;
          }
          hunk_index++;
        }

        uint16_t next_character = buffer_position.column < buffer_line_length
          ? buffer_line[buffer_position.column]
          : 0;
        if (buffer_position.column >= trailing_whitespace_start_column) {
          in_trailing_whitespace = true;
          in_leading_whitespace = false;
        } else if (next_character != ' ' && next_character != '\t') {
          in_leading_whitespace = false;
        }

        // Compute a token flags describing built-in decorations for the token
        // containing the next character
        int32_t previous_built_in_tag_flags = current_built_in_class_name_flags;
        update_current_token_flags(next_character);

        if (emit_built_in_tag_boundary) {
          emit_close_tag(layer.built_in_scope_id_for_flags(previous_built_in_tag_flags));
        }

        emit_decoration_boundaries();

        // Are we at the end of the line?
        if (buffer_position.column == buffer_line_length) {
          emit_line_ending();
          break;
        }

        if (emit_built_in_tag_boundary) {
          emit_open_tag(layer.built_in_scope_id_for_flags(current_built_in_class_name_flags));
        }

        // Emit the next character, handling hard tabs whitespace invisibles
        // specially.
        if (next_character == '\t') {
          emit_hard_tab();
        } else if ((in_leading_whitespace || in_trailing_whitespace) &&
                   next_character == ' ' && !layer.params_.invisible_space.empty()) {
          emit_text(layer.params_.invisible_space);
        } else {
          emit_character(next_character);
        }
        buffer_position.column++;
      }
    }

    return true;
  }

private:
  DisplayLayer &layer;

  const vector<HighlightSegment> *segments;
  bool strict;
  bool segment_valid;
  int64_t segment_index;
  size_t boundary_index;
  vector<const ScreenLine *> *result;

  uint32_t requested_start_screen_row;
  uint32_t requested_end_screen_row;
  uint32_t end_buffer_row;
  vector<Patch::Change> hunks;
  size_t hunk_index;
  uint32_t first_screen_row;
  int32_t initial_soft_wrap_indent;

  Point buffer_position;
  uint32_t screen_row;
  uint32_t screen_column;
  u16string buffer_line;
  uint32_t buffer_line_length;
  uint32_t trailing_whitespace_start_column;
  bool in_leading_whitespace;
  bool in_trailing_whitespace;
  int32_t current_built_in_class_name_flags;
  bool emit_built_in_tag_boundary;

  u16string current_screen_line_text;
  vector<int32_t> current_screen_line_tags;
  uint32_t current_token_length;
  vector<int32_t> containing_scope_ids;
  vector<int32_t> scope_ids_to_reopen;

  void begin(uint32_t start_screen_row, uint32_t end_screen_row) {
    layer.populate_spatial_index_if_needed(layer.line_count(), end_screen_row);

    // If the first row continues a soft-wrapped buffer line, start right at
    // the wrap instead of at the start of the buffer line: on very long lines
    // that would mean building every screen row before the requested ones.
    initial_soft_wrap_indent = -1;
    first_screen_row = start_screen_row;
    Point screen_start(start_screen_row, 0);
    auto wrap = start_screen_row > 0
      ? layer.spatial_index_.grab_change_starting_before_new_position(screen_start)
      : optional<Patch::Change>{};
    if (wrap && is_soft_wrap_hunk(*wrap) &&
        wrap->new_start.row + 1 == start_screen_row &&
        wrap->new_end.row == start_screen_row) {
      buffer_position = wrap->old_start;
      screen_row = start_screen_row;
      initial_soft_wrap_indent = wrap->new_end.column;
    } else {
      buffer_position = Point(
        layer.find_boundary_preceding_buffer_row(
          layer.translate_screen_position_with_spatial_index(
            screen_start, ClipDirection::Unspecified, false
          ).row
        ),
        0
      );

      screen_row = layer.translate_buffer_position_with_spatial_index(
        Point(buffer_position.row, 0), ClipDirection::Unspecified
      ).row;
    }

    end_buffer_row = layer.translate_screen_position_with_spatial_index(
      Point(end_screen_row, INF), ClipDirection::Unspecified, false
    ).row;

    hunks = layer.spatial_index_.grab_changes_in_new_range(Point(screen_row, 0), Point(end_screen_row, 0));
    hunk_index = 0;
    // Skip the soft wrap we are starting after.
    while (hunk_index < hunks.size() && hunks[hunk_index].new_start < Point(screen_row, 0)) {
      hunk_index++;
    }
    screen_column = 0;
  }

  // The soft wrap indent of the screen row that starts a build, or -1.
  int32_t soft_wrap_indent_for_row(uint32_t row) const {
    if (row == first_screen_row && initial_soft_wrap_indent >= 0) return initial_soft_wrap_indent;
    const ScreenLine *previous = row > 0 ? layer.cached_screen_line(row - 1) : nullptr;
    return previous ? previous->soft_wrap_indent : -1;
  }

  // Mirrors how the JS builder advances past a cached screen row.
  void skip_cached_screen_row() {
    while (hunk_index < hunks.size() && hunks[hunk_index].new_start.row <= screen_row) {
      auto &next_hunk = hunks[hunk_index];
      if (next_hunk.new_start.row == screen_row) {
        if (next_hunk.new_end.row > next_hunk.new_start.row) {
          screen_row++;
          buffer_position.column = next_hunk.old_end.column;
          hunk_index++;
          return;
        } else {
          buffer_position = next_hunk.old_end;
        }
      }
      hunk_index++;
    }

    screen_row++;
    screen_column = 0;
    buffer_position.row++;
    buffer_position.column = 0;
  }

  void load_line(uint32_t row) {
    buffer_line = layer.line_for_row(row);
    buffer_line_length = buffer_line.size();
  }

  // Highlight segments

  const HighlightSegment &current_segment() const {
    return (*segments)[segment_index];
  }

  // Moves to the next highlight segment, which must start at the current
  // position. In non-strict mode a mismatch disables highlighting for the
  // rest of the segment instead of failing the build.
  bool advance_segment() {
    segment_index++;
    while (static_cast<size_t>(segment_index) < segments->size() && current_segment().reseek) {
      segment_index++;
    }
    boundary_index = 0;
    segment_valid = static_cast<size_t>(segment_index) < segments->size() &&
      current_segment().start == buffer_position;
    return segment_valid || !strict;
  }

  Point iterator_position() const {
    if (!segment_valid) return Point(INF, INF);
    auto &segment = current_segment();
    if (boundary_index < segment.boundaries.size()) return segment.boundaries[boundary_index].position;
    return segment.next_position;
  }

  void skip_boundaries_before(Point position) {
    if (!segment_valid) return;
    auto &boundaries = current_segment().boundaries;
    while (boundary_index < boundaries.size() && boundaries[boundary_index].position < position) {
      boundary_index++;
    }
  }

  void emit_decoration_boundaries() {
    if (!segment_valid) return;
    auto &boundaries = current_segment().boundaries;
    while (boundary_index < boundaries.size() && boundaries[boundary_index].position == buffer_position) {
      auto &boundary = boundaries[boundary_index];
      for (int32_t scope_id : boundary.close_scope_ids) emit_close_tag(scope_id);
      for (int32_t scope_id : boundary.open_scope_ids) emit_open_tag(scope_id);
      boundary_index++;
    }
  }

  // Emitting

  void begin_line() {
    current_screen_line_text.clear();
    current_screen_line_tags.clear();
    screen_column = 0;
    current_token_length = 0;
  }

  void update_current_token_flags(uint16_t next_character) {
    int32_t previous_built_in_tag_flags = current_built_in_class_name_flags;
    current_built_in_class_name_flags = 0;
    emit_built_in_tag_boundary = false;
    auto &params = layer.params_;

    if (next_character == ' ' || next_character == '\t') {
      bool show_indent_guides = params.show_indent_guides &&
        (in_leading_whitespace || trailing_whitespace_start_column == 0);
      if (in_leading_whitespace) current_built_in_class_name_flags |= LEADING_WHITESPACE;
      if (in_trailing_whitespace) current_built_in_class_name_flags |= TRAILING_WHITESPACE;

      if (next_character == ' ') {
        if ((in_leading_whitespace || in_trailing_whitespace) && !params.invisible_space.empty()) {
          current_built_in_class_name_flags |= INVISIBLE_CHARACTER;
        }

        if (show_indent_guides) {
          current_built_in_class_name_flags |= INDENT_GUIDE;
          if (screen_column % params.tab_length == 0) emit_built_in_tag_boundary = true;
        }
      } else {  // next_character == \t
        current_built_in_class_name_flags |= HARD_TAB;
        if (!params.invisible_tab.empty()) current_built_in_class_name_flags |= INVISIBLE_CHARACTER;
        if (show_indent_guides && screen_column % params.tab_length == 0) {
          current_built_in_class_name_flags |= INDENT_GUIDE;
        }

        emit_built_in_tag_boundary = true;
      }
    }

    if (!emit_built_in_tag_boundary) {
      emit_built_in_tag_boundary = current_built_in_class_name_flags != previous_built_in_tag_flags;
    }
  }

  bool emit_fold(const Patch::Change &next_hunk) {
    emit_close_tag(layer.built_in_scope_id_for_flags(current_built_in_class_name_flags));
    current_built_in_class_name_flags = 0;

    close_containing_scopes();
    scope_ids_to_reopen.clear();

    emit_open_tag(layer.built_in_scope_id_for_flags(FOLD));
    emit_text(layer.params_.fold_character);
    emit_close_tag(layer.built_in_scope_id_for_flags(FOLD));

    buffer_position = next_hunk.old_end;

    if (!advance_segment()) return false;
    if (segment_valid) {
      scope_ids_to_reopen = current_segment().containing_scope_ids;
    }

    load_line(buffer_position.row);
    trailing_whitespace_start_column = layer.find_trailing_whitespace_start_column(buffer_position.row);
    return true;
  }

  void emit_soft_wrap(const Patch::Change &next_hunk) {
    emit_close_tag(layer.built_in_scope_id_for_flags(current_built_in_class_name_flags));
    current_built_in_class_name_flags = 0;
    close_containing_scopes();
    emit_newline(next_hunk.new_end.column);
    emit_indent_whitespace(next_hunk.new_end.column);
  }

  void emit_line_ending() {
    auto &params = layer.params_;
    emit_close_tag(layer.built_in_scope_id_for_flags(current_built_in_class_name_flags));

    const uint16_t *line_ending = layer.buffer_->line_ending_for_row(buffer_position.row);
    const u16string *eol_invisible = nullptr;
    if (line_ending && line_ending[0] == '\n') {
      eol_invisible = &params.eol_invisible_lf;
    } else if (line_ending && line_ending[0] == '\r') {
      eol_invisible = &params.eol_invisible_crlf;
    }
    if (eol_invisible && !eol_invisible->empty()) {
      int32_t eol_flags = INVISIBLE_CHARACTER | LINE_ENDING;
      if (buffer_line_length == 0 && params.show_indent_guides) eol_flags |= INDENT_GUIDE;
      emit_open_tag(layer.built_in_scope_id_for_flags(eol_flags));
      emit_text(*eol_invisible, false);
      emit_close_tag(layer.built_in_scope_id_for_flags(eol_flags));
    }

    if (buffer_line_length == 0 && params.show_indent_guides) {
      uint32_t whitespace_length = layer.leading_whitespace_length_for_surrounding_lines(buffer_position.row);
      emit_indent_whitespace(whitespace_length);
    }

    close_containing_scopes();

    // Ensure empty lines have at least one empty token to make it easier on
    // the caller
    if (current_screen_line_tags.empty()) current_screen_line_tags.push_back(0);
    emit_newline();
    buffer_position.row++;
    buffer_position.column = 0;
  }

  void emit_newline(int32_t soft_wrap_indent = -1) {
    std::unique_ptr<ScreenLine> screen_line{new ScreenLine{
      next_screen_line_id++,
      std::move(current_screen_line_text),
      std::move(current_screen_line_tags),
      soft_wrap_indent
    }};
    push_screen_line(screen_line.get());
    auto &cache = layer.cached_screen_lines_;
    if (cache.size() <= screen_row) cache.resize(screen_row + 1);
    cache[screen_row] = std::move(screen_line);
    screen_row++;
    begin_line();
  }

  void emit_indent_whitespace(uint32_t end_column) {
    auto &params = layer.params_;
    if (params.show_indent_guides) {
      bool opened_indent_guide = false;
      while (screen_column < end_column) {
        if (screen_column % params.tab_length == 0) {
          if (opened_indent_guide) {
            emit_close_tag(layer.built_in_scope_id_for_flags(INDENT_GUIDE));
          }

          emit_open_tag(layer.built_in_scope_id_for_flags(INDENT_GUIDE));
          opened_indent_guide = true;
        }
        emit_text(u" ", false);
      }

      if (opened_indent_guide) emit_close_tag(layer.built_in_scope_id_for_flags(INDENT_GUIDE));
    } else if (end_column > screen_column) {
      emit_text(u16string(end_column - screen_column, ' '), false);
    } else {
      emit_text(u16string(), false);
    }
  }

  void emit_hard_tab() {
    auto &params = layer.params_;
    uint32_t distance_to_next_tab_stop = params.tab_length - (screen_column % params.tab_length);
    if (!params.invisible_tab.empty()) {
      emit_text(params.invisible_tab);
      emit_text(u16string(distance_to_next_tab_stop - 1, ' '));
    } else {
      emit_text(u16string(distance_to_next_tab_stop, ' '));
    }
  }

  void emit_text(const u16string &text, bool reopen_tags = true) {
    if (reopen_tags) this->reopen_tags();
    current_screen_line_text += text;
    screen_column += text.size();
    current_token_length += text.size();
  }

  void emit_character(uint16_t character) {
    reopen_tags();
    current_screen_line_text.push_back(character);
    screen_column++;
    current_token_length++;
  }

  void emit_token_boundary() {
    if (current_token_length > 0) {
      current_screen_line_tags.push_back(current_token_length);
      current_token_length = 0;
    }
  }

  void emit_empty_token_if_needed() {
    if (!current_screen_line_tags.empty() && is_open_tag(current_screen_line_tags.back())) {
      current_screen_line_tags.push_back(0);
    }
  }

  void emit_close_tag(int32_t scope_id) {
    emit_token_boundary();

    if (scope_id == 0) return;

    for (int64_t i = static_cast<int64_t>(scope_ids_to_reopen.size()) - 1; i >= 0; i--) {
      if (scope_ids_to_reopen[i] == scope_id) {
        scope_ids_to_reopen.erase(scope_ids_to_reopen.begin() + i);
        return;
      }
    }

    emit_empty_token_if_needed();

    while (!containing_scope_ids.empty()) {
      int32_t containing_scope_id = containing_scope_ids.back();
      containing_scope_ids.pop_back();
      // JS stops at falsy ids (`while ((id = stack.pop()))`).
      if (containing_scope_id == 0) break;
      current_screen_line_tags.push_back(close_tag_for_scope_id(containing_scope_id));
      if (containing_scope_id == scope_id) {
        return;
      } else {
        scope_ids_to_reopen.insert(scope_ids_to_reopen.begin(), containing_scope_id);
      }
    }
  }

  void emit_open_tag(int32_t scope_id, bool reopen_tags = true) {
    if (reopen_tags) this->reopen_tags();
    emit_token_boundary();
    if (scope_id > 0) {
      containing_scope_ids.push_back(scope_id);
      current_screen_line_tags.push_back(open_tag_for_scope_id(scope_id));
    }
  }

  void close_containing_scopes() {
    if (!containing_scope_ids.empty()) emit_empty_token_if_needed();

    for (int64_t i = static_cast<int64_t>(containing_scope_ids.size()) - 1; i >= 0; i--) {
      int32_t containing_scope_id = containing_scope_ids[i];
      current_screen_line_tags.push_back(close_tag_for_scope_id(containing_scope_id));
      scope_ids_to_reopen.insert(scope_ids_to_reopen.begin(), containing_scope_id);
    }
    containing_scope_ids.clear();
  }

  void reopen_tags() {
    for (int32_t scope_id_to_reopen : scope_ids_to_reopen) {
      containing_scope_ids.push_back(scope_id_to_reopen);
      current_screen_line_tags.push_back(open_tag_for_scope_id(scope_id_to_reopen));
    }
    scope_ids_to_reopen.clear();
  }

  void push_screen_line(const ScreenLine *screen_line) {
    if (requested_start_screen_row <= screen_row && screen_row < requested_end_screen_row) {
      result->push_back(screen_line);
    }
  }
};

vector<DisplayLayer::HighlightSegmentRequest> DisplayLayer::prepare_screen_lines(uint32_t start_screen_row,
                                                                                 uint32_t end_screen_row,
                                                                                 uint32_t *end_buffer_row) {
  validate_line_cache();
  ScreenLineBuilder builder(*this);
  return builder.collect_segments(start_screen_row, end_screen_row, end_buffer_row);
}

bool DisplayLayer::build_screen_lines(uint32_t start_screen_row, uint32_t end_screen_row,
                                      const vector<HighlightSegment> &segments, bool strict,
                                      vector<const ScreenLine *> *result) {
  validate_line_cache();
  ScreenLineBuilder builder(*this);
  return builder.build(start_screen_row, end_screen_row, segments, strict, result);
}
