#ifndef SUPERSTRING_DISPLAY_LAYER_WRAPPER_H
#define SUPERSTRING_DISPLAY_LAYER_WRAPPER_H

#include <memory>
#include "napi.h"
#include "display-layer.h"

// Low-level binding for the native DisplayLayer. It is driven by
// text-buffer's `DisplayLayer` class, which owns the public API: methods take
// plain numbers and write point/change results into a shared Float64Array
// ("scratch") instead of allocating result objects.
class DisplayLayerWrapper : public Napi::ObjectWrap<DisplayLayerWrapper> {
public:
  static void init(Napi::Env env, Napi::Object exports);
  static void set_parallel_indexing_threshold(const Napi::CallbackInfo &info);

  explicit DisplayLayerWrapper(const Napi::CallbackInfo &info);

private:
  DisplayLayer::Params params_from_js(Napi::Env env, Napi::Value value);
  void write_change(const DisplayLayer::ScreenChange &change);
  void write_point(const Point &point, size_t offset = 0);

  void set_params(const Napi::CallbackInfo &info);
  void clear_spatial_index(const Napi::CallbackInfo &info);
  void clear_screen_line_cache(const Napi::CallbackInfo &info);
  void populate_spatial_index_if_needed(const Napi::CallbackInfo &info);
  Napi::Value do_background_work(const Napi::CallbackInfo &info);
  void update_spatial_index(const Napi::CallbackInfo &info);
  void buffer_will_change(const Napi::CallbackInfo &info);
  void buffer_did_change(const Napi::CallbackInfo &info);
  void invalidate_highlighting(const Napi::CallbackInfo &info);
  void translate_buffer_position(const Napi::CallbackInfo &info);
  void translate_screen_position(const Napi::CallbackInfo &info);
  Napi::Value get_screen_line_count(const Napi::CallbackInfo &info);
  Napi::Value get_approximate_screen_line_count(const Napi::CallbackInfo &info);
  void get_rightmost_screen_position(const Napi::CallbackInfo &info);
  void get_approximate_rightmost_screen_position(const Napi::CallbackInfo &info);
  Napi::Value line_length_for_screen_row(const Napi::CallbackInfo &info);
  Napi::Value buffer_rows_for_screen_rows(const Napi::CallbackInfo &info);
  Napi::Value find_boundary_preceding_buffer_row(const Napi::CallbackInfo &info);
  Napi::Value find_boundary_following_buffer_row(const Napi::CallbackInfo &info);
  void find_boundary_following_screen_row(const Napi::CallbackInfo &info);
  Napi::Value get_indexed_buffer_row_count(const Napi::CallbackInfo &info);
  Napi::Value get_computed_screen_line_count(const Napi::CallbackInfo &info);
  Napi::Value prepare_screen_lines(const Napi::CallbackInfo &info);
  Napi::Value build_screen_lines(const Napi::CallbackInfo &info);
  Napi::Value create_render_planner(const Napi::CallbackInfo &info);
  void destroy_render_planner(const Napi::CallbackInfo &info);
  void set_class_names(const Napi::CallbackInfo &info);
  Napi::Value build_render_plan(const Napi::CallbackInfo &info);

  Napi::Value build_lines(const Napi::CallbackInfo &info, size_t offset,
                          std::vector<const DisplayLayer::ScreenLine *> *lines);
  Napi::Array screen_lines_to_js(Napi::Env env, const std::vector<const DisplayLayer::ScreenLine *> &lines,
                                 Napi::Value known_ids);

  std::unique_ptr<DisplayLayer> display_layer;
  // Weak: the JS DisplayLayer keeps these alive, and strong references would
  // form uncollectable cycles through the buffer and the editor.
  Napi::ObjectReference text_buffer_reference;
  Napi::ObjectReference folds_marker_layer_reference;
  Napi::FunctionReference ratio_for_character_reference;
  Napi::FunctionReference is_wrap_boundary_reference;
  Napi::Reference<Napi::Float64Array> scratch_reference;
  double *scratch;
};

#endif // SUPERSTRING_DISPLAY_LAYER_WRAPPER_H
