#ifndef SUPERSTRING_DISPLAY_LAYER_H_
#define SUPERSTRING_DISPLAY_LAYER_H_

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "marker-index.h"
#include "optional.h"
#include "patch.h"
#include "point.h"
#include "text-buffer.h"

// A native port of text-buffer's `DisplayLayer` + `ScreenLineBuilder`. It maps
// buffer coordinates to screen coordinates (soft wraps, folds, hard tabs) and
// builds screen lines (text + tag stream), reading the native `TextBuffer` and
// the folds `MarkerIndex` directly.
//
// Syntax highlighting is supplied in batches: `prepare_screen_lines` reports
// which buffer segments need highlight boundaries, the caller collects them
// from the language mode and passes them to `build_screen_lines`.
class DisplayLayer {
public:
  enum class ClipDirection { Closest, Backward, Forward, Unspecified };
  enum class WidthMode { Unit, Ratios, Callback };
  enum class WrapBoundaryMode { WordStart, Pulsar, Callback };

  struct Params {
    uint32_t tab_length = 4;
    double soft_wrap_column = INFINITY;
    uint32_t soft_wrap_hanging_indent = 0;
    bool show_indent_guides = false;
    bool atomic_soft_tabs = true;
    // Empty strings mean "no invisible" (they are falsy in JS).
    std::u16string invisible_space;
    std::u16string invisible_tab;
    std::u16string eol_invisible_lf;
    std::u16string eol_invisible_crlf;
    std::u16string fold_character = u"⋯";
    WidthMode width_mode = WidthMode::Unit;
    double double_width_ratio = 1;
    double half_width_ratio = 1;
    double korean_ratio = 1;
    std::function<double(const std::u16string &)> ratio_for_character;
    WrapBoundaryMode wrap_boundary_mode = WrapBoundaryMode::WordStart;
    std::function<bool(const std::u16string &, const std::u16string &)> is_wrap_boundary;
  };

  struct ScreenLine {
    uint32_t id;
    std::u16string text;
    std::vector<int32_t> tags;
    int32_t soft_wrap_indent;
  };

  // A change to screen rows, as reported by JS `DisplayLayer::didChange`.
  struct ScreenChange {
    uint32_t start;
    uint32_t old_extent;
    uint32_t new_extent;
  };

  struct HighlightSegmentRequest {
    Point start;
    uint32_t end_row;
  };

  struct HighlightBoundary {
    Point position;
    std::vector<int32_t> close_scope_ids;
    std::vector<int32_t> open_scope_ids;
  };

  struct HighlightSegment {
    Point start;
    // Segments collected at positions where the JS iterator would re-seek
    // mid-run because a boundary lay past the end of its line.
    bool reseek = false;
    std::vector<int32_t> containing_scope_ids;
    std::vector<HighlightBoundary> boundaries;
    // The iterator's position after the last collected boundary.
    Point next_position;
  };

  struct BuiltInScope {
    int32_t scope_id;
    int32_t flags;
  };

  // Render plan operations, emitted as (op, line id, screen row, old line id).
  enum RenderOp : uint32_t {
    RENDER_OP_KEEP = 0,     // Same line at the same row.
    RENDER_OP_INSERT = 1,   // New line; its HTML is included.
    RENDER_OP_REPLACE = 2,  // New line reusing the node of `old line id`, which left this row; HTML included.
    RENDER_OP_MOVE = 3,     // Same line at a different row.
    RENDER_OP_REMOVE = 4    // Line no longer rendered.
  };

  struct RenderPlan {
    std::vector<uint32_t> ops;
    std::vector<uint32_t> html_line_ids;
    std::vector<std::u16string> html;
  };

  using FoldsIndexProvider = std::function<MarkerIndex *()>;
  // Returns the milliseconds left before the caller's deadline.
  using TimeRemaining = std::function<double()>;

  DisplayLayer(TextBuffer *buffer, FoldsIndexProvider folds_index, Params params);
  // Copies the spatial index of `source` (but not its screen line cache),
  // like JS `DisplayLayer::copy`.
  DisplayLayer(const DisplayLayer &source, FoldsIndexProvider folds_index, Params params);
  ~DisplayLayer();

  void set_params(Params params);
  void clear_spatial_index();
  void clear_screen_line_cache();

  void populate_spatial_index_if_needed(uint32_t end_buffer_row, uint32_t end_screen_row,
                                        const TimeRemaining *time_remaining = nullptr);
  // Returns true if there is still work left to do.
  bool do_background_work(const TimeRemaining &time_remaining);
  ScreenChange update_spatial_index(uint32_t start_buffer_row, uint32_t old_end_buffer_row,
                                    uint32_t new_end_buffer_row, uint32_t end_screen_row,
                                    const TimeRemaining *time_remaining = nullptr);
  void buffer_will_change(uint32_t old_end_row);
  ScreenChange buffer_did_change(uint32_t start_row, uint32_t start_column,
                                 uint32_t old_end_row, uint32_t old_end_column,
                                 uint32_t new_end_row, uint32_t new_end_column);
  ScreenChange invalidate_highlighting(uint32_t start_row, uint32_t end_row);

  Point translate_buffer_position(double row, double column, ClipDirection clip_direction);
  // Sets `*out_of_range` when the translated buffer row is past the end of
  // the buffer (JS asserts in that case).
  Point translate_screen_position(double row, double column, ClipDirection clip_direction,
                                  bool skip_soft_wrap_indentation, bool *out_of_range);

  uint32_t get_screen_line_count();
  uint32_t get_approximate_screen_line_count();
  Point get_rightmost_screen_position();
  Point get_approximate_rightmost_screen_position();
  optional<uint32_t> line_length_for_screen_row(uint32_t screen_row);
  std::vector<uint32_t> buffer_rows_for_screen_rows(uint32_t start_row, uint32_t end_row);
  uint32_t find_boundary_preceding_buffer_row(uint32_t buffer_row);
  uint32_t find_boundary_following_buffer_row(uint32_t buffer_row);
  std::pair<uint32_t, uint32_t> find_boundary_following_screen_row(uint32_t screen_row);
  int64_t indexed_buffer_row_count() const { return indexed_buffer_row_count_; }
  uint32_t computed_screen_line_count() const { return screen_line_lengths_.size(); }

  // Updates covering at least this many buffer rows compute soft wraps on
  // worker threads (when widths and wrap boundaries aren't JS callbacks).
  static void set_parallel_indexing_threshold(uint32_t rows);

  // Phase 1 of building screen lines: returns the buffer segments whose
  // highlight boundaries are needed, and the end buffer row to seek with.
  std::vector<HighlightSegmentRequest> prepare_screen_lines(uint32_t start_screen_row,
                                                            uint32_t end_screen_row,
                                                            uint32_t *end_buffer_row);
  // Phase 2. In strict mode, returns false if `segments` does not match the
  // segments this build needs (highlighting was invalidated in between) and
  // the caller should prepare again. Otherwise mismatched segments are
  // rendered without highlighting.
  bool build_screen_lines(uint32_t start_screen_row, uint32_t end_screen_row,
                          const std::vector<HighlightSegment> &segments, bool strict,
                          std::vector<const ScreenLine *> *result);
  const ScreenLine *cached_screen_line(uint32_t screen_row) const;

  // Render planners remember the rows each viewer rendered last, so that
  // `build_render_plan` can describe only what changed.
  uint32_t create_render_planner();
  void destroy_render_planner(uint32_t planner_id);
  // Diffs `lines` (the screen lines for rows starting at `start_row`) against
  // what the planner rendered last and generates HTML for new lines. Returns
  // false, leaving the planner untouched, if a class name is unknown; the
  // missing scope ids are added to `missing_scope_ids`.
  bool build_render_plan(uint32_t planner_id, uint32_t start_row,
                         const std::vector<const ScreenLine *> &lines,
                         RenderPlan *plan, std::vector<int32_t> *missing_scope_ids);
  void set_class_name(int32_t scope_id, std::u16string class_name);
  std::vector<BuiltInScope> take_new_built_in_scopes();
  bool built_in_scope_ids_exhausted() const { return built_in_scope_ids_exhausted_; }

private:
  class ScreenLineBuilder;
  friend class ScreenLineBuilder;

  struct Fold {
    Point start;
    Point end;
  };

  // Soft-wrap layouts of a range of buffer rows without folds, computed off
  // the main thread. Screen rows of buffer row `first_row + i` are
  // [screen_row_starts[i], screen_row_starts[i + 1]); `wrap_columns` and
  // `wrap_indents` describe the wrap that starts each screen row (unused for
  // a buffer row's first screen row), with the column relative to the
  // previous screen row.
  struct RowLayouts {
    uint32_t first_row = 0;
    std::vector<uint32_t> screen_row_starts;
    std::vector<uint32_t> lengths;
    std::vector<uint32_t> tab_counts;
    std::vector<uint32_t> wrap_columns;
    std::vector<uint32_t> wrap_indents;
    std::vector<int32_t> overflow_offsets;
    std::vector<double> carried_widths;
  };

  static uint32_t parallel_indexing_threshold_;
  bool can_index_in_parallel() const;
  std::vector<RowLayouts> compute_row_layouts_in_parallel(uint32_t start_row, uint32_t end_row);
  void layout_row(const char16_t *line, uint32_t line_length, RowLayouts *layouts);

  uint32_t line_count() const;
  uint32_t last_row() const;
  uint32_t line_length(uint32_t row);
  const std::u16string &line_for_row(uint32_t row);
  void invalidate_line_cache();
  void validate_line_cache();

  double ratio_for_character(uint16_t character);
  double ratio_for_fold_character();
  bool is_wrap_boundary(uint16_t previous, uint16_t character, bool character_is_fold);

  Point clip_buffer_position(double row, double column);
  Point translate_buffer_position_with_spatial_index(Point position, ClipDirection clip_direction);
  Point translate_screen_position_with_spatial_index(Point position, ClipDirection clip_direction,
                                                     bool skip_soft_wrap_indentation);
  Point constrain_screen_position(double row, double column, ClipDirection clip_direction);
  Point expand_hard_tabs(Point target_screen_position, uint32_t tab_count);
  Point collapse_hard_tabs(Point target_screen_position, uint32_t tab_count,
                           ClipDirection clip_direction);
  int64_t get_clip_column_delta(Point buffer_position, ClipDirection clip_direction);
  uint32_t leading_whitespace_length_for_surrounding_lines(uint32_t buffer_row);
  uint32_t leading_whitespace_length_for_non_empty_line(const std::u16string &line);
  uint32_t find_trailing_whitespace_start_column(uint32_t buffer_row);
  std::map<Point, Point> compute_folds_in_buffer_row_range(uint32_t start_buffer_row,
                                                           uint32_t end_buffer_row);
  std::vector<MarkerIndex::MarkerId> find_folds_intersecting(MarkerIndex *index, uint32_t start_row,
                                                             uint32_t end_row);
  int32_t built_in_scope_id_for_flags(int32_t flags);
  bool append_line_html(const ScreenLine &line, std::u16string *html, std::vector<int32_t> *missing_scope_ids);
  void splice_cache(uint32_t start, uint32_t old_count, uint32_t new_count);
  void splice_screen_rows(uint32_t start_screen_row, uint32_t old_screen_row_count,
                          const std::vector<uint32_t> &inserted_screen_line_lengths,
                          const std::vector<uint32_t> &inserted_tab_counts,
                          const std::vector<int32_t> &inserted_wrap_overflow_offsets,
                          const std::vector<double> &inserted_wrap_carried_widths,
                          int64_t rightmost_inserted_row, int64_t rightmost_inserted_column);
  optional<ScreenChange> update_soft_wraps_in_row(uint32_t row, uint32_t edit_start,
                                                  uint32_t old_edit_end, uint32_t new_edit_end);

  TextBuffer *buffer_;
  FoldsIndexProvider folds_index_;
  Params params_;
  Patch spatial_index_;
  std::vector<uint32_t> screen_line_lengths_;
  std::vector<uint32_t> tab_counts_;
  // For each screen row that starts at a soft wrap, how many unexpanded
  // columns past the wrap the character that overflowed the previous row is
  // (the wrapping state after a wrap depends on it); -1 for rows that start a
  // buffer row.
  std::vector<int32_t> wrap_overflow_offsets_;
  // For the same rows, the width carried over from the previous row at the
  // wrap (the full algorithm keeps the widths tabs had before the wrap).
  std::vector<double> wrap_carried_widths_;
  Point rightmost_screen_position_;
  int64_t indexed_buffer_row_count_;
  std::vector<std::unique_ptr<ScreenLine>> cached_screen_lines_;

  std::unordered_map<int32_t, int32_t> built_in_scope_ids_by_flags_;
  int32_t next_built_in_scope_id_;
  std::vector<BuiltInScope> new_built_in_scopes_;
  std::unordered_map<int32_t, std::u16string> class_names_by_scope_id_;

  struct RenderedLine {
    uint32_t row;
    uint32_t id;
  };
  std::unordered_map<uint32_t, std::vector<RenderedLine>> render_planners_;
  uint32_t next_render_planner_id_ = 1;
  bool built_in_scope_ids_exhausted_;

  std::vector<double> ratio_cache_;
  double fold_character_ratio_;
  std::unordered_map<uint32_t, bool> wrap_boundary_cache_;

  uint32_t cached_line_row_;
  std::u16string cached_line_;
  uint32_t cached_line_buffer_size_ = UINT32_MAX;
  Point cached_line_buffer_extent_;
};

#endif  // SUPERSTRING_DISPLAY_LAYER_H_
