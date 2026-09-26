#include <cmath>
#include "display-layer-wrapper.h"
#include "marker-index-wrapper.h"
#include "text-buffer-wrapper.h"

using namespace Napi;
using std::u16string;
using std::vector;

namespace {

const uint32_t INF = UINT32_MAX;

// Converts a JS number (possibly Infinity or negative) to a row/column.
uint32_t coordinate_from_js(const Napi::Value &value) {
  if (!value.IsNumber()) return 0;
  double number = value.As<Number>().DoubleValue();
  if (!(number > 0)) return 0;
  if (number >= static_cast<double>(INF)) return INF;
  return static_cast<uint32_t>(number);
}

double double_from_js(const Napi::Value &value, double fallback = 0) {
  return value.IsNumber() ? value.As<Number>().DoubleValue() : fallback;
}

double coordinate_to_js(uint32_t value) {
  return value == INF ? INFINITY : static_cast<double>(value);
}

// JS invisibles are "set" when truthy; only strings are meaningful here.
u16string invisible_from_js(const Napi::Value &value) {
  if (!value.IsString()) return u16string();
  return value.As<String>().Utf16Value();
}

DisplayLayer::ClipDirection clip_direction_from_js(const Napi::Value &value) {
  switch (value.IsNumber() ? value.As<Number>().Int32Value() : 0) {
    case 1: return DisplayLayer::ClipDirection::Backward;
    case 2: return DisplayLayer::ClipDirection::Forward;
    case 3: return DisplayLayer::ClipDirection::Unspecified;
    default: return DisplayLayer::ClipDirection::Closest;
  }
}

template <typename T, typename Array>
Array typed_array_from_vector(Napi::Env env, const vector<T> &values) {
  auto array = Array::New(env, values.size());
  if (!values.empty()) std::copy(values.begin(), values.end(), array.Data());
  return array;
}

// Decodes the highlight batch built by text-buffer (see
// `DisplayLayer::collectHighlightBatch` in text-buffer/src/display-layer.js):
//
//   segmentCount,
//   (startRow, startColumn, isReseek, containingCount, ...containingIds,
//    nextRow, nextColumn, boundaryCount,
//    (row, column, closeCount, ...closeIds, openCount, ...openIds)*)*
//
// Positions of -1 stand for Infinity.
bool decode_highlight_batch(const Napi::Value &value, vector<DisplayLayer::HighlightSegment> *segments) {
  if (!value.IsTypedArray()) return true;
  auto array = value.As<Int32Array>();
  const int32_t *data = array.Data();
  size_t length = array.ElementLength();
  size_t i = 0;
  auto next = [&](int32_t *out) -> bool {
    if (i >= length) return false;
    *out = data[i++];
    return true;
  };
  auto coordinate = [](int32_t value) -> uint32_t {
    return value < 0 ? INF : static_cast<uint32_t>(value);
  };
  auto read_ids = [&](vector<int32_t> *ids) -> bool {
    int32_t count;
    if (!next(&count) || count < 0 || i + count > length) return false;
    ids->assign(data + i, data + i + count);
    i += count;
    return true;
  };

  int32_t segment_count;
  if (!next(&segment_count)) return false;
  segments->resize(segment_count);
  for (auto &segment : *segments) {
    int32_t row, column, boundary_count;
    if (!next(&row) || !next(&column)) return false;
    segment.start = Point(coordinate(row), coordinate(column));
    int32_t reseek;
    if (!next(&reseek)) return false;
    segment.reseek = reseek != 0;
    if (!read_ids(&segment.containing_scope_ids)) return false;
    if (!next(&row) || !next(&column)) return false;
    segment.next_position = Point(coordinate(row), coordinate(column));
    if (!next(&boundary_count) || boundary_count < 0) return false;
    segment.boundaries.resize(boundary_count);
    for (auto &boundary : segment.boundaries) {
      if (!next(&row) || !next(&column)) return false;
      boundary.position = Point(coordinate(row), coordinate(column));
      if (!read_ids(&boundary.close_scope_ids)) return false;
      if (!read_ids(&boundary.open_scope_ids)) return false;
    }
  }
  return true;
}

}  // namespace

void DisplayLayerWrapper::init(Napi::Env env, Object exports) {
  Napi::Function func = DefineClass(env, "DisplayLayer", {
    InstanceMethod<&DisplayLayerWrapper::set_params>("setParams", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::clear_spatial_index>("clearSpatialIndex", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::clear_screen_line_cache>("clearScreenLineCache", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::populate_spatial_index_if_needed>("populateSpatialIndexIfNeeded", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::do_background_work>("doBackgroundWork", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::update_spatial_index>("updateSpatialIndex", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::buffer_will_change>("bufferWillChange", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::buffer_did_change>("bufferDidChange", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::invalidate_highlighting>("invalidateHighlighting", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::translate_buffer_position>("translateBufferPosition", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::translate_screen_position>("translateScreenPosition", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::get_screen_line_count>("getScreenLineCount", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::get_approximate_screen_line_count>("getApproximateScreenLineCount", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::get_rightmost_screen_position>("getRightmostScreenPosition", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::get_approximate_rightmost_screen_position>("getApproximateRightmostScreenPosition", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::line_length_for_screen_row>("lineLengthForScreenRow", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::buffer_rows_for_screen_rows>("bufferRowsForScreenRows", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::find_boundary_preceding_buffer_row>("findBoundaryPrecedingBufferRow", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::find_boundary_following_buffer_row>("findBoundaryFollowingBufferRow", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::find_boundary_following_screen_row>("findBoundaryFollowingScreenRow", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::get_indexed_buffer_row_count>("getIndexedBufferRowCount", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::get_computed_screen_line_count>("getComputedScreenLineCount", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::prepare_screen_lines>("prepareScreenLines", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::build_screen_lines>("buildScreenLines", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::create_render_planner>("createRenderPlanner", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::destroy_render_planner>("destroyRenderPlanner", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::set_class_names>("setClassNames", napi_default_method),
    InstanceMethod<&DisplayLayerWrapper::build_render_plan>("buildRenderPlan", napi_default_method),
  });

  exports.Set("DisplayLayer", func);
}

// new DisplayLayer(nativeTextBuffer, foldsMarkerLayer, params, scratch, [sourceDisplayLayer])
DisplayLayerWrapper::DisplayLayerWrapper(const CallbackInfo &info) : ObjectWrap<DisplayLayerWrapper>(info), scratch{nullptr} {
  Napi::Env env = info.Env();

  if (info.Length() < 4 || !info[0].IsObject() || !info[1].IsObject() || !info[3].IsTypedArray()) {
    Error::New(env, "Expected (textBuffer, foldsMarkerLayer, params, scratch)").ThrowAsJavaScriptException();
    return;
  }

  auto text_buffer_wrapper = TextBufferWrapper::Unwrap(info[0].As<Object>());
  if (!text_buffer_wrapper) {
    Error::New(env, "Expected a native TextBuffer").ThrowAsJavaScriptException();
    return;
  }
  text_buffer_reference = Napi::Weak(info[0].As<Object>());
  folds_marker_layer_reference = Napi::Weak(info[1].As<Object>());

  auto scratch_array = info[3].As<Float64Array>();
  if (scratch_array.ElementLength() < 4) {
    Error::New(env, "Expected a scratch Float64Array of length >= 4").ThrowAsJavaScriptException();
    return;
  }
  scratch_reference = Napi::Persistent(scratch_array);
  scratch = scratch_array.Data();

  // The folds layer's `index` is replaced by `MarkerLayer::clear`, so look it
  // up whenever folds are needed.
  DisplayLayer::FoldsIndexProvider folds_index = [this]() -> MarkerIndex * {
    if (folds_marker_layer_reference.IsEmpty()) return nullptr;
    Object layer = folds_marker_layer_reference.Value();
    if (layer.IsEmpty()) return nullptr;
    Napi::Value index = layer.Get("index");
    if (!index.IsObject()) return nullptr;
    return MarkerIndexWrapper::from_js(index);
  };

  auto params = params_from_js(env, info[2]);

  if (info.Length() > 4 && info[4].IsObject()) {
    auto source = DisplayLayerWrapper::Unwrap(info[4].As<Object>());
    if (source && source->display_layer) {
      display_layer.reset(new DisplayLayer(*source->display_layer, folds_index, std::move(params)));
      return;
    }
  }

  display_layer.reset(new DisplayLayer(&text_buffer_wrapper->text_buffer, folds_index, std::move(params)));
}

DisplayLayer::Params DisplayLayerWrapper::params_from_js(Napi::Env env, Napi::Value value) {
  DisplayLayer::Params params;
  if (!value.IsObject()) return params;
  Object object = value.As<Object>();

  params.tab_length = coordinate_from_js(object.Get("tabLength"));
  params.soft_wrap_column = double_from_js(object.Get("softWrapColumn"), INFINITY);
  params.soft_wrap_hanging_indent = coordinate_from_js(object.Get("softWrapHangingIndent"));
  params.show_indent_guides = object.Get("showIndentGuides").ToBoolean();
  params.atomic_soft_tabs = object.Get("atomicSoftTabs").ToBoolean();
  params.invisible_space = invisible_from_js(object.Get("invisibleSpace"));
  params.invisible_tab = invisible_from_js(object.Get("invisibleTab"));
  params.eol_invisible_lf = invisible_from_js(object.Get("eolInvisibleLf"));
  params.eol_invisible_crlf = invisible_from_js(object.Get("eolInvisibleCrlf"));
  Napi::Value fold_character = object.Get("foldCharacter");
  params.fold_character = fold_character.IsString() ? fold_character.As<String>().Utf16Value() : u16string();

  Napi::Value ratios = object.Get("characterWidthRatios");
  Napi::Value ratio_for_character = object.Get("ratioForCharacter");
  if (ratios.IsObject()) {
    Object ratios_object = ratios.As<Object>();
    params.width_mode = DisplayLayer::WidthMode::Ratios;
    params.double_width_ratio = double_from_js(ratios_object.Get("doubleWidth"), 1);
    params.half_width_ratio = double_from_js(ratios_object.Get("halfWidth"), 1);
    params.korean_ratio = double_from_js(ratios_object.Get("korean"), 1);
  } else if (ratio_for_character.IsFunction()) {
    params.width_mode = DisplayLayer::WidthMode::Callback;
    ratio_for_character_reference = Napi::Weak(ratio_for_character.As<Function>());
    params.ratio_for_character = [this, env](const u16string &character) -> double {
      if (ratio_for_character_reference.IsEmpty()) return 1;
      Function callback = ratio_for_character_reference.Value();
      if (callback.IsEmpty()) return 1;
      Napi::Value result = callback.Call({String::New(env, character)});
      return result.IsNumber() ? result.As<Number>().DoubleValue() : NAN;
    };
  }

  Napi::Value wrap_boundary_mode = object.Get("wrapBoundaryMode");
  Napi::Value is_wrap_boundary = object.Get("isWrapBoundary");
  if (wrap_boundary_mode.IsString()) {
    std::string mode = wrap_boundary_mode.As<String>().Utf8Value();
    params.wrap_boundary_mode = mode == "cjk"
      ? DisplayLayer::WrapBoundaryMode::Pulsar
      : DisplayLayer::WrapBoundaryMode::WordStart;
  } else if (is_wrap_boundary.IsFunction()) {
    params.wrap_boundary_mode = DisplayLayer::WrapBoundaryMode::Callback;
    is_wrap_boundary_reference = Napi::Weak(is_wrap_boundary.As<Function>());
    params.is_wrap_boundary = [this, env](const u16string &previous, const u16string &character) -> bool {
      if (is_wrap_boundary_reference.IsEmpty()) return false;
      Function callback = is_wrap_boundary_reference.Value();
      if (callback.IsEmpty()) return false;
      return callback.Call({String::New(env, previous), String::New(env, character)}).ToBoolean();
    };
  }

  return params;
}

void DisplayLayerWrapper::write_change(const DisplayLayer::ScreenChange &change) {
  scratch[0] = change.start;
  scratch[1] = change.old_extent;
  scratch[2] = change.new_extent;
}

void DisplayLayerWrapper::write_point(const Point &point, size_t offset) {
  scratch[offset] = coordinate_to_js(point.row);
  scratch[offset + 1] = coordinate_to_js(point.column);
}

void DisplayLayerWrapper::set_params(const CallbackInfo &info) {
  display_layer->set_params(params_from_js(info.Env(), info[0]));
}

void DisplayLayerWrapper::clear_spatial_index(const CallbackInfo &info) {
  display_layer->clear_spatial_index();
}

void DisplayLayerWrapper::clear_screen_line_cache(const CallbackInfo &info) {
  display_layer->clear_screen_line_cache();
}

void DisplayLayerWrapper::populate_spatial_index_if_needed(const CallbackInfo &info) {
  display_layer->populate_spatial_index_if_needed(coordinate_from_js(info[0]), coordinate_from_js(info[1]));
}

// doBackgroundWork(deadline): calls `deadline.timeRemaining()` before each
// row, like the JS implementation did.
Napi::Value DisplayLayerWrapper::do_background_work(const CallbackInfo &info) {
  Napi::Env env = info.Env();
  DisplayLayer::TimeRemaining time_remaining = []() { return INFINITY; };
  Object deadline;
  Function time_remaining_function;
  if (info[0].IsObject()) {
    deadline = info[0].As<Object>();
    Napi::Value function = deadline.Get("timeRemaining");
    if (function.IsFunction()) {
      time_remaining_function = function.As<Function>();
      time_remaining = [&]() {
        Napi::Value result = time_remaining_function.Call(deadline, {});
        return result.IsNumber() ? result.As<Number>().DoubleValue() : 0.0;
      };
    }
  }
  return Boolean::New(env, display_layer->do_background_work(time_remaining));
}

Napi::Value DisplayLayerWrapper::get_computed_screen_line_count(const CallbackInfo &info) {
  return Number::New(info.Env(), display_layer->computed_screen_line_count());
}

void DisplayLayerWrapper::update_spatial_index(const CallbackInfo &info) {
  write_change(display_layer->update_spatial_index(
    coordinate_from_js(info[0]),
    coordinate_from_js(info[1]),
    coordinate_from_js(info[2]),
    coordinate_from_js(info[3])
  ));
}

void DisplayLayerWrapper::buffer_will_change(const CallbackInfo &info) {
  display_layer->buffer_will_change(coordinate_from_js(info[0]));
}

void DisplayLayerWrapper::buffer_did_change(const CallbackInfo &info) {
  write_change(display_layer->buffer_did_change(
    coordinate_from_js(info[0]),
    coordinate_from_js(info[1]),
    coordinate_from_js(info[2])
  ));
}

void DisplayLayerWrapper::invalidate_highlighting(const CallbackInfo &info) {
  write_change(display_layer->invalidate_highlighting(coordinate_from_js(info[0]), coordinate_from_js(info[1])));
}

void DisplayLayerWrapper::translate_buffer_position(const CallbackInfo &info) {
  write_point(display_layer->translate_buffer_position(
    double_from_js(info[0]),
    double_from_js(info[1]),
    clip_direction_from_js(info[2])
  ));
}

void DisplayLayerWrapper::translate_screen_position(const CallbackInfo &info) {
  bool out_of_range = false;
  write_point(display_layer->translate_screen_position(
    double_from_js(info[0]),
    double_from_js(info[1]),
    clip_direction_from_js(info[2]),
    info[3].ToBoolean(),
    &out_of_range
  ));
  scratch[2] = out_of_range ? 1 : 0;
}

Napi::Value DisplayLayerWrapper::get_screen_line_count(const CallbackInfo &info) {
  return Number::New(info.Env(), display_layer->get_screen_line_count());
}

Napi::Value DisplayLayerWrapper::get_approximate_screen_line_count(const CallbackInfo &info) {
  return Number::New(info.Env(), display_layer->get_approximate_screen_line_count());
}

void DisplayLayerWrapper::get_rightmost_screen_position(const CallbackInfo &info) {
  write_point(display_layer->get_rightmost_screen_position());
}

void DisplayLayerWrapper::get_approximate_rightmost_screen_position(const CallbackInfo &info) {
  write_point(display_layer->get_approximate_rightmost_screen_position());
}

Napi::Value DisplayLayerWrapper::line_length_for_screen_row(const CallbackInfo &info) {
  auto length = display_layer->line_length_for_screen_row(coordinate_from_js(info[0]));
  if (length) return Number::New(info.Env(), *length);
  return info.Env().Undefined();
}

Napi::Value DisplayLayerWrapper::buffer_rows_for_screen_rows(const CallbackInfo &info) {
  auto rows = display_layer->buffer_rows_for_screen_rows(coordinate_from_js(info[0]), coordinate_from_js(info[1]));
  return typed_array_from_vector<uint32_t, Uint32Array>(info.Env(), rows);
}

Napi::Value DisplayLayerWrapper::find_boundary_preceding_buffer_row(const CallbackInfo &info) {
  return Number::New(info.Env(), display_layer->find_boundary_preceding_buffer_row(coordinate_from_js(info[0])));
}

Napi::Value DisplayLayerWrapper::find_boundary_following_buffer_row(const CallbackInfo &info) {
  return Number::New(info.Env(), display_layer->find_boundary_following_buffer_row(coordinate_from_js(info[0])));
}

void DisplayLayerWrapper::find_boundary_following_screen_row(const CallbackInfo &info) {
  auto result = display_layer->find_boundary_following_screen_row(coordinate_from_js(info[0]));
  scratch[0] = result.first;
  scratch[1] = result.second;
}

Napi::Value DisplayLayerWrapper::get_indexed_buffer_row_count(const CallbackInfo &info) {
  return Number::New(info.Env(), static_cast<double>(display_layer->indexed_buffer_row_count()));
}

// prepareScreenLines(startRow, endRow) -> Int32Array | null
//   [endBufferRow, (startRow, startColumn, endRow)*], or null when every
//   requested line is cached and no highlighting is needed.
Napi::Value DisplayLayerWrapper::prepare_screen_lines(const CallbackInfo &info) {
  Napi::Env env = info.Env();
  uint32_t end_buffer_row = 0;
  auto segments = display_layer->prepare_screen_lines(
    coordinate_from_js(info[0]), coordinate_from_js(info[1]), &end_buffer_row
  );
  if (segments.empty()) return env.Null();

  auto result = Int32Array::New(env, 1 + segments.size() * 3);
  int32_t *data = result.Data();
  data[0] = static_cast<int32_t>(end_buffer_row);
  size_t i = 1;
  for (auto &segment : segments) {
    data[i++] = static_cast<int32_t>(segment.start.row);
    data[i++] = static_cast<int32_t>(segment.start.column);
    data[i++] = static_cast<int32_t>(segment.end_row);
  }
  return result;
}

// buildScreenLines(startRow, endRow, highlightBatch, knownIds, strict)
//   -> null (segments did not match; prepare again) or
//      [ids, dataIndexes, texts, tagOffsets, tags, softWrapIndents, newBuiltInScopes]
//
// `ids[i]` is the id of the screen line at `startRow + i`. Full contents are
// only included for lines whose id differs from `knownIds[i]`; line `j` of
// the contents is for `ids[dataIndexes[j]]`, with tags
// `tags[tagOffsets[j]..tagOffsets[j + 1]]`.
Napi::Value DisplayLayerWrapper::build_screen_lines(const CallbackInfo &info) {
  vector<const DisplayLayer::ScreenLine *> lines;
  Napi::Value result = build_lines(info, 0, &lines);
  if (!result.IsArray()) return result;
  return screen_lines_to_js(info.Env(), lines, info[3]);
}

// Builds screen lines from (startRow, endRow, highlightBatch, knownIds,
// strict) starting at `info[offset]`. Returns an empty array on success,
// null if highlight segments did not match, or undefined after throwing.
Napi::Value DisplayLayerWrapper::build_lines(const CallbackInfo &info, size_t offset,
                                             vector<const DisplayLayer::ScreenLine *> *lines) {
  Napi::Env env = info.Env();
  uint32_t start_row = coordinate_from_js(info[offset]);
  uint32_t end_row = coordinate_from_js(info[offset + 1]);

  vector<DisplayLayer::HighlightSegment> segments;
  if (!decode_highlight_batch(info[offset + 2], &segments)) {
    Error::New(env, "Invalid highlight batch").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  bool strict = info[offset + 4].ToBoolean();
  if (!display_layer->build_screen_lines(start_row, end_row, segments, strict, lines)) {
    return env.Null();
  }

  if (display_layer->built_in_scope_ids_exhausted()) {
    Error::New(env, "Built in scope ids exhausted").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Array::New(env, 0);
}

// Encodes screen lines as [ids, dataIndexes, texts, tagOffsets, tags,
// softWrapIndents, newBuiltInScopes]; see `build_screen_lines`.
Napi::Array DisplayLayerWrapper::screen_lines_to_js(Napi::Env env, const vector<const DisplayLayer::ScreenLine *> &lines,
                                                    Napi::Value known_ids_value) {
  const uint32_t *known_ids = nullptr;
  size_t known_id_count = 0;
  if (known_ids_value.IsTypedArray()) {
    auto known = known_ids_value.As<Uint32Array>();
    known_ids = known.Data();
    known_id_count = known.ElementLength();
  }

  auto ids = Uint32Array::New(env, lines.size());
  vector<uint32_t> data_indexes;
  size_t total_tag_count = 0;
  for (size_t i = 0; i < lines.size(); i++) {
    ids[i] = lines[i]->id;
    if (i >= known_id_count || known_ids[i] != lines[i]->id) {
      data_indexes.push_back(i);
      total_tag_count += lines[i]->tags.size();
    }
  }

  auto texts = Array::New(env, data_indexes.size());
  auto tag_offsets = Uint32Array::New(env, data_indexes.size() + 1);
  auto tags = Int32Array::New(env, total_tag_count);
  auto soft_wrap_indents = Int32Array::New(env, data_indexes.size());
  size_t tag_offset = 0;
  for (size_t j = 0; j < data_indexes.size(); j++) {
    auto line = lines[data_indexes[j]];
    texts[j] = String::New(env, line->text);
    tag_offsets[j] = tag_offset;
    std::copy(line->tags.begin(), line->tags.end(), tags.Data() + tag_offset);
    tag_offset += line->tags.size();
    soft_wrap_indents[j] = line->soft_wrap_indent;
  }
  tag_offsets[data_indexes.size()] = tag_offset;

  Napi::Value new_built_in_scopes = env.Undefined();
  auto scopes = display_layer->take_new_built_in_scopes();
  if (!scopes.empty()) {
    auto array = Int32Array::New(env, scopes.size() * 2);
    for (size_t i = 0; i < scopes.size(); i++) {
      array[i * 2] = scopes[i].scope_id;
      array[i * 2 + 1] = scopes[i].flags;
    }
    new_built_in_scopes = array;
  }

  auto result = Array::New(env, 7);
  result[0u] = ids;
  result[1u] = typed_array_from_vector<uint32_t, Uint32Array>(env, data_indexes);
  result[2u] = texts;
  result[3u] = tag_offsets;
  result[4u] = tags;
  result[5u] = soft_wrap_indents;
  result[6u] = new_built_in_scopes;
  return result;
}

Napi::Value DisplayLayerWrapper::create_render_planner(const CallbackInfo &info) {
  return Number::New(info.Env(), display_layer->create_render_planner());
}

void DisplayLayerWrapper::destroy_render_planner(const CallbackInfo &info) {
  display_layer->destroy_render_planner(coordinate_from_js(info[0]));
}

// setClassNames(scopeIds, classNames)
void DisplayLayerWrapper::set_class_names(const CallbackInfo &info) {
  if (!info[0].IsTypedArray() || !info[1].IsArray()) return;
  auto scope_ids = info[0].As<Int32Array>();
  auto class_names = info[1].As<Array>();
  for (size_t i = 0; i < scope_ids.ElementLength() && i < class_names.Length(); i++) {
    Napi::Value class_name = class_names[i];
    display_layer->set_class_name(
      scope_ids[i],
      class_name.IsString() ? class_name.As<String>().Utf16Value() : u16string()
    );
  }
}

// buildRenderPlan(plannerId, startRow, endRow, highlightBatch, knownIds, strict)
//   -> null (highlight segments did not match; prepare again),
//      an Int32Array of scope ids whose class names must be set first, or
//      the `buildScreenLines` result followed by
//      [ops, htmlLineIds, html]: ops are (op, lineId, row, oldLineId)
//      quadruples (see DisplayLayer::RenderOp) and html[i] is the contents
//      of the line element for htmlLineIds[i].
Napi::Value DisplayLayerWrapper::build_render_plan(const CallbackInfo &info) {
  Napi::Env env = info.Env();
  uint32_t planner_id = coordinate_from_js(info[0]);
  uint32_t start_row = coordinate_from_js(info[1]);

  vector<const DisplayLayer::ScreenLine *> lines;
  Napi::Value built = build_lines(info, 1, &lines);
  if (!built.IsArray()) return built;

  DisplayLayer::RenderPlan plan;
  vector<int32_t> missing_scope_ids;
  if (!display_layer->build_render_plan(planner_id, start_row, lines, &plan, &missing_scope_ids)) {
    return typed_array_from_vector<int32_t, Int32Array>(env, missing_scope_ids);
  }

  Napi::Array result = screen_lines_to_js(env, lines, info[4]);
  auto html = Array::New(env, plan.html.size());
  for (size_t i = 0; i < plan.html.size(); i++) {
    html[i] = String::New(env, plan.html[i]);
  }
  result[7u] = typed_array_from_vector<uint32_t, Uint32Array>(env, plan.ops);
  result[8u] = typed_array_from_vector<uint32_t, Uint32Array>(env, plan.html_line_ids);
  result[9u] = html;
  return result;
}
