#include <cmath>
#include "tree-sitter-wrapper.h"
#include "addon-data.h"
#include "text-buffer-wrapper.h"
#include "string-conversion.h"

using namespace Napi;
using std::shared_ptr;
using std::string;
using std::vector;
using tree_sitter::Grammar;
using tree_sitter::LanguageMode;
using tree_sitter::Query;
using tree_sitter::QueryCapture;
using tree_sitter::TreeHolder;

namespace {

// web-tree-sitter hands positions and indexes to its C glue as wasm i32s,
// which converts them like ToInt32 (so e.g. Infinity becomes 0), and the glue
// then doubles code units into bytes as uint32. Do the same.
uint32_t i32_from_js(const Napi::Value &value) {
  if (!value.IsNumber()) return 0;
  double number = value.As<Number>().DoubleValue();
  if (!std::isfinite(number)) return 0;
  double truncated = std::trunc(number);
  double modulo = std::fmod(truncated, 4294967296.0);
  if (modulo < 0) modulo += 4294967296.0;
  return static_cast<uint32_t>(modulo);
}

uint32_t code_unit_to_byte(uint32_t unit) { return unit << 1; }
uint32_t byte_to_code_unit(uint32_t byte) { return byte >> 1; }

TSPoint point_from_js(const Napi::Value &value) {
  if (!value.IsObject()) return {0, 0};
  Object object = value.As<Object>();
  return {i32_from_js(object.Get("row")), code_unit_to_byte(i32_from_js(object.Get("column")))};
}

Object point_to_js(Napi::Env env, TSPoint point) {
  Object result = Object::New(env);
  result.Set("row", Number::New(env, point.row));
  result.Set("column", Number::New(env, byte_to_code_unit(point.column)));
  return result;
}

bool point_lte(TSPoint a, TSPoint b) {
  return a.row < b.row || (a.row == b.row && a.column <= b.column);
}

}  // namespace

// TreeSitterGrammar

void TreeSitterGrammarWrapper::init(Napi::Env env, Object exports) {
  Function func = DefineClass(env, "TreeSitterGrammar", {
    InstanceAccessor<&TreeSitterGrammarWrapper::get_name>("name"),
    InstanceMethod<&TreeSitterGrammarWrapper::query>("query", napi_default_method),
    InstanceMethod<&TreeSitterGrammarWrapper::id_for_node_type>("idForNodeType", napi_default_method),
  });
  exports.Set("TreeSitterGrammar", func);
}

// new TreeSitterGrammar(wasmBytes: Uint8Array)
TreeSitterGrammarWrapper::TreeSitterGrammarWrapper(const CallbackInfo &info) : ObjectWrap<TreeSitterGrammarWrapper>(info) {
  Napi::Env env = info.Env();
  if (!info[0].IsTypedArray()) {
    Error::New(env, "Expected the grammar's WASM bytes").ThrowAsJavaScriptException();
    return;
  }
  auto bytes = info[0].As<Uint8Array>();
  string error;
  grammar = Grammar::load(bytes.Data(), bytes.ByteLength(), &error);
  if (!grammar) Error::New(env, error).ThrowAsJavaScriptException();
}

shared_ptr<Grammar> TreeSitterGrammarWrapper::from_js(Napi::Value value) {
  if (!value.IsObject()) return nullptr;
  auto wrapper = Unwrap(value.As<Object>());
  return wrapper ? wrapper->grammar : nullptr;
}

Napi::Value TreeSitterGrammarWrapper::get_name(const CallbackInfo &info) {
  return String::New(info.Env(), grammar ? grammar->name() : "");
}

// grammar.query(source) -> TreeSitterQuery (throws on invalid queries)
Napi::Value TreeSitterGrammarWrapper::query(const CallbackInfo &info) {
  auto *data = info.Env().GetInstanceData<AddonData>();
  return data->tree_sitter_query_constructor.New({info.This(), info[0]});
}

// grammar.idForNodeType(type, named) -> number | null
Napi::Value TreeSitterGrammarWrapper::id_for_node_type(const CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (!grammar || !info[0].IsString()) return env.Null();
  string type = info[0].As<String>().Utf8Value();
  TSSymbol symbol = ts_language_symbol_for_name(grammar->language(), type.data(), type.size(), info[1].ToBoolean());
  if (symbol == 0) return env.Null();
  return Number::New(env, symbol);
}

// TreeSitterQuery

void TreeSitterQueryWrapper::init(Napi::Env env, Object exports) {
  auto *data = env.GetInstanceData<AddonData>();
  Function func = DefineClass(env, "TreeSitterQuery", {
    InstanceAccessor<&TreeSitterQueryWrapper::get_capture_names>("captureNames"),
    InstanceMethod<&TreeSitterQueryWrapper::captures>("captures", napi_default_method),
  });
  data->tree_sitter_query_constructor = Persistent(func);
  exports.Set("TreeSitterQuery", func);
}

// new TreeSitterQuery(grammar, source)
TreeSitterQueryWrapper::TreeSitterQueryWrapper(const CallbackInfo &info) : ObjectWrap<TreeSitterQueryWrapper>(info) {
  Napi::Env env = info.Env();
  auto grammar = TreeSitterGrammarWrapper::from_js(info[0]);
  if (!grammar || !info[1].IsString()) {
    Error::New(env, "Expected (grammar, source)").ThrowAsJavaScriptException();
    return;
  }
  string error;
  query = Query::create(grammar, info[1].As<String>().Utf8Value(), &error);
  if (!query) {
    Error::New(env, error).ThrowAsJavaScriptException();
    return;
  }
  uint32_t pattern_count = query->pattern_count();
  set_properties_objects.resize(pattern_count);
  asserted_properties_objects.resize(pattern_count);
  refuted_properties_objects.resize(pattern_count);
  Array names = Array::New(env, query->capture_count());
  for (uint32_t i = 0; i < query->capture_count(); i++) names[i] = String::New(env, query->capture_name(i));
  capture_names = Persistent(names.As<Object>());
}

Napi::Value TreeSitterQueryWrapper::get_capture_names(const CallbackInfo &info) {
  Napi::Env env = info.Env();
  Object cached = capture_names.Value();
  uint32_t count = cached.As<Array>().Length();
  Array names = Array::New(env, count);
  for (uint32_t i = 0; i < count; i++) names[i] = cached.Get(i);
  return names;
}

// A frozen `{key: value | null}` object per pattern, shared by its captures
// (like web-tree-sitter's).
Napi::Value TreeSitterQueryWrapper::properties_object(Napi::Env env, vector<ObjectReference> *cache, uint32_t pattern,
                                                      const vector<tree_sitter::QueryProperty> &properties) {
  ObjectReference &reference = (*cache)[pattern];
  if (reference.IsEmpty()) {
    Object object = Object::New(env);
    for (auto &property : properties) {
      if (property.value) {
        object.Set(property.key, String::New(env, *property.value));
      } else {
        object.Set(property.key, env.Null());
      }
    }
    object.Freeze();
    reference = Persistent(object);
  }
  return reference.Value();
}

// query.captures(node, {startPosition, endPosition}) -> [{name, node, patternIndex,
//   setProperties, assertedProperties, refutedProperties}]
Napi::Value TreeSitterQueryWrapper::captures(const CallbackInfo &info) {
  Napi::Env env = info.Env();
  SyntaxNodeWrapper *node = SyntaxNodeWrapper::from_js(info[0]);
  if (!node) {
    Error::New(env, "Expected a syntax node").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  TSPoint start = {0, 0};
  TSPoint end = {0, 0};
  if (info[1].IsObject()) {
    Object options = info[1].As<Object>();
    Napi::Value start_position = options.Get("startPosition");
    Napi::Value end_position = options.Get("endPosition");
    if (start_position.IsObject()) start = point_from_js(start_position);
    if (end_position.IsObject()) end = point_from_js(end_position);
    if (!(end.row == 0 && end.column == 0) && !point_lte(start, end)) {
      Error::New(env, "`startPosition` cannot be greater than `endPosition`").ThrowAsJavaScriptException();
      return env.Undefined();
    }
  }

  TextBuffer *buffer = node->buffer->buffer;
  vector<QueryCapture> captures;
  query->captures(node->node, start, end, [buffer](TSNode captured) {
    return tree_sitter::text_for_node(buffer, captured);
  }, &captures);

  Array names = capture_names.Value().As<Array>();
  Array result = Array::New(env, captures.size());
  String name_key = String::New(env, "name");
  String node_key = String::New(env, "node");
  String pattern_index_key = String::New(env, "patternIndex");
  String set_key = String::New(env, "setProperties");
  String asserted_key = String::New(env, "assertedProperties");
  String refuted_key = String::New(env, "refutedProperties");
  for (size_t i = 0; i < captures.size(); i++) {
    const QueryCapture &capture = captures[i];
    Object object = Object::New(env);
    object.Set(pattern_index_key, Number::New(env, capture.pattern_index));
    object.Set(name_key, names.Get(capture.capture_id));
    object.Set(node_key, SyntaxNodeWrapper::from_node(env, capture.node, node->tree, node->buffer));
    uint32_t pattern = capture.pattern_index;
    object.Set(set_key, query->has_set_properties(pattern)
      ? properties_object(env, &set_properties_objects, pattern, query->set_properties(pattern))
      : env.Undefined());
    object.Set(asserted_key, query->has_asserted_properties(pattern)
      ? properties_object(env, &asserted_properties_objects, pattern, query->asserted_properties(pattern))
      : env.Undefined());
    object.Set(refuted_key, query->has_refuted_properties(pattern)
      ? properties_object(env, &refuted_properties_objects, pattern, query->refuted_properties(pattern))
      : env.Undefined());
    result[i] = object;
  }
  return result;
}

// TreeSitterLanguageMode

void TreeSitterLanguageModeWrapper::init(Napi::Env env, Object exports) {
  Function func = DefineClass(env, "TreeSitterLanguageMode", {
    InstanceMethod<&TreeSitterLanguageModeWrapper::create_layer>("createLayer", napi_default_method),
    InstanceMethod<&TreeSitterLanguageModeWrapper::destroy_layer>("destroyLayer", napi_default_method),
    InstanceMethod<&TreeSitterLanguageModeWrapper::set_included_ranges>("setIncludedRanges", napi_default_method),
    InstanceMethod<&TreeSitterLanguageModeWrapper::edit>("edit", napi_default_method),
    InstanceMethod<&TreeSitterLanguageModeWrapper::parse>("parse", napi_default_method),
    InstanceMethod<&TreeSitterLanguageModeWrapper::root_node>("rootNode", napi_default_method),
  });
  exports.Set("TreeSitterLanguageMode", func);
}

// new TreeSitterLanguageMode(nativeTextBuffer)
TreeSitterLanguageModeWrapper::TreeSitterLanguageModeWrapper(const CallbackInfo &info) :
  ObjectWrap<TreeSitterLanguageModeWrapper>(info) {
  Napi::Env env = info.Env();
  TextBufferWrapper *text_buffer = info[0].IsObject() ? TextBufferWrapper::Unwrap(info[0].As<Object>()) : nullptr;
  if (!text_buffer) {
    Error::New(env, "Expected a native TextBuffer").ThrowAsJavaScriptException();
    return;
  }
  buffer = std::make_shared<TextBufferHandle>();
  buffer->reference = Persistent(info[0].As<Object>());
  buffer->buffer = &text_buffer->text_buffer;
  language_mode.reset(new LanguageMode(buffer->buffer));
}

// createLayer(grammar) -> layer id
Napi::Value TreeSitterLanguageModeWrapper::create_layer(const CallbackInfo &info) {
  Napi::Env env = info.Env();
  auto grammar = TreeSitterGrammarWrapper::from_js(info[0]);
  if (!grammar) {
    Error::New(env, "Expected a TreeSitterGrammar").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Number::New(env, language_mode->create_layer(grammar));
}

void TreeSitterLanguageModeWrapper::destroy_layer(const CallbackInfo &info) {
  language_mode->destroy_layer(i32_from_js(info[0]));
}

// setIncludedRanges(layerId, ranges): `ranges` holds
// (startIndex, endIndex, startRow, startColumn, endRow, endColumn) sextuples
// in code units; an empty list means the whole buffer.
void TreeSitterLanguageModeWrapper::set_included_ranges(const CallbackInfo &info) {
  vector<TSRange> ranges;
  if (info[1].IsTypedArray()) {
    auto values = info[1].As<Uint32Array>();
    for (size_t i = 0; i + 5 < values.ElementLength(); i += 6) {
      ranges.push_back({
        {values[i + 2], code_unit_to_byte(values[i + 3])},
        {values[i + 4], code_unit_to_byte(values[i + 5])},
        code_unit_to_byte(values[i]),
        code_unit_to_byte(values[i + 1])
      });
    }
  }
  language_mode->set_included_ranges(i32_from_js(info[0]), std::move(ranges));
}

// edit(startIndex, oldEndIndex, newEndIndex, startRow, startColumn,
//      oldEndRow, oldEndColumn, newEndRow, newEndColumn), in code units.
void TreeSitterLanguageModeWrapper::edit(const CallbackInfo &info) {
  language_mode->edit(
    i32_from_js(info[0]), i32_from_js(info[1]), i32_from_js(info[2]),
    Point(i32_from_js(info[3]), i32_from_js(info[4])),
    Point(i32_from_js(info[5]), i32_from_js(info[6])),
    Point(i32_from_js(info[7]), i32_from_js(info[8]))
  );
}

// parse(layerId) -> Uint32Array of changed (startRow, startColumn, endRow,
// endColumn) ranges, or null if parsing failed.
Napi::Value TreeSitterLanguageModeWrapper::parse(const CallbackInfo &info) {
  Napi::Env env = info.Env();
  vector<Range> changed_ranges;
  if (!language_mode->parse(i32_from_js(info[0]), &changed_ranges)) return env.Null();
  auto result = Uint32Array::New(env, changed_ranges.size() * 4);
  for (size_t i = 0; i < changed_ranges.size(); i++) {
    result[i * 4] = changed_ranges[i].start.row;
    result[i * 4 + 1] = changed_ranges[i].start.column;
    result[i * 4 + 2] = changed_ranges[i].end.row;
    result[i * 4 + 3] = changed_ranges[i].end.column;
  }
  return result;
}

// rootNode(layerId) -> SyntaxNode | null
Napi::Value TreeSitterLanguageModeWrapper::root_node(const CallbackInfo &info) {
  Napi::Env env = info.Env();
  auto tree = language_mode->tree(i32_from_js(info[0]));
  if (!tree) return env.Null();
  return SyntaxNodeWrapper::from_node(env, ts_tree_root_node(tree->tree), tree, buffer);
}

// SyntaxNode

void SyntaxNodeWrapper::init(Napi::Env env, Object exports) {
  auto *data = env.GetInstanceData<AddonData>();
  Function func = DefineClass(env, "SyntaxNode", {
    InstanceAccessor<&SyntaxNodeWrapper::get_id>("id"),
    InstanceAccessor<&SyntaxNodeWrapper::get_type>("type"),
    InstanceAccessor<&SyntaxNodeWrapper::get_type_id>("typeId"),
    InstanceAccessor<&SyntaxNodeWrapper::get_text>("text"),
    InstanceAccessor<&SyntaxNodeWrapper::get_start_index>("startIndex"),
    InstanceAccessor<&SyntaxNodeWrapper::get_end_index>("endIndex"),
    InstanceAccessor<&SyntaxNodeWrapper::get_start_position>("startPosition"),
    InstanceAccessor<&SyntaxNodeWrapper::get_end_position>("endPosition"),
    InstanceAccessor<&SyntaxNodeWrapper::get_is_named>("isNamed"),
    InstanceAccessor<&SyntaxNodeWrapper::get_is_missing>("isMissing"),
    InstanceAccessor<&SyntaxNodeWrapper::get_is_extra>("isExtra"),
    InstanceAccessor<&SyntaxNodeWrapper::get_is_error>("isError"),
    InstanceAccessor<&SyntaxNodeWrapper::get_has_error>("hasError"),
    InstanceAccessor<&SyntaxNodeWrapper::get_has_changes>("hasChanges"),
    InstanceAccessor<&SyntaxNodeWrapper::get_parent>("parent"),
    InstanceAccessor<&SyntaxNodeWrapper::get_child_count>("childCount"),
    InstanceAccessor<&SyntaxNodeWrapper::get_named_child_count>("namedChildCount"),
    InstanceAccessor<&SyntaxNodeWrapper::get_children>("children"),
    InstanceAccessor<&SyntaxNodeWrapper::get_named_children>("namedChildren"),
    InstanceAccessor<&SyntaxNodeWrapper::get_first_child>("firstChild"),
    InstanceAccessor<&SyntaxNodeWrapper::get_last_child>("lastChild"),
    InstanceAccessor<&SyntaxNodeWrapper::get_first_named_child>("firstNamedChild"),
    InstanceAccessor<&SyntaxNodeWrapper::get_last_named_child>("lastNamedChild"),
    InstanceAccessor<&SyntaxNodeWrapper::get_next_sibling>("nextSibling"),
    InstanceAccessor<&SyntaxNodeWrapper::get_previous_sibling>("previousSibling"),
    InstanceAccessor<&SyntaxNodeWrapper::get_next_named_sibling>("nextNamedSibling"),
    InstanceAccessor<&SyntaxNodeWrapper::get_previous_named_sibling>("previousNamedSibling"),
    InstanceMethod<&SyntaxNodeWrapper::child>("child", napi_default_method),
    InstanceMethod<&SyntaxNodeWrapper::named_child>("namedChild", napi_default_method),
    InstanceMethod<&SyntaxNodeWrapper::child_for_field_name>("childForFieldName", napi_default_method),
    InstanceMethod<&SyntaxNodeWrapper::descendants_of_type>("descendantsOfType", napi_default_method),
    InstanceMethod<&SyntaxNodeWrapper::descendant_for_index>("descendantForIndex", napi_default_method),
    InstanceMethod<&SyntaxNodeWrapper::named_descendant_for_index>("namedDescendantForIndex", napi_default_method),
    InstanceMethod<&SyntaxNodeWrapper::descendant_for_position>("descendantForPosition", napi_default_method),
    InstanceMethod<&SyntaxNodeWrapper::named_descendant_for_position>("namedDescendantForPosition", napi_default_method),
    InstanceMethod<&SyntaxNodeWrapper::equals>("equals", napi_default_method),
    InstanceMethod<&SyntaxNodeWrapper::to_string>("toString", napi_default_method),
  });
  data->syntax_node_constructor = Persistent(func);
  exports.Set("SyntaxNode", func);
}

SyntaxNodeWrapper::SyntaxNodeWrapper(const CallbackInfo &info) : ObjectWrap<SyntaxNodeWrapper>(info) {
  node = TSNode{};
}

Napi::Value SyntaxNodeWrapper::from_node(Napi::Env env, TSNode node, const shared_ptr<TreeHolder> &tree,
                                         const shared_ptr<TextBufferHandle> &buffer) {
  if (ts_node_is_null(node)) return env.Null();
  auto *data = env.GetInstanceData<AddonData>();
  Object object = data->syntax_node_constructor.New({});
  SyntaxNodeWrapper *wrapper = Unwrap(object);
  wrapper->node = node;
  wrapper->tree = tree;
  wrapper->buffer = buffer;
  return object;
}

SyntaxNodeWrapper *SyntaxNodeWrapper::from_js(Napi::Value value) {
  if (!value.IsObject()) return nullptr;
  auto *data = value.Env().GetInstanceData<AddonData>();
  Object object = value.As<Object>();
  if (!object.InstanceOf(data->syntax_node_constructor.Value())) return nullptr;
  SyntaxNodeWrapper *wrapper = Unwrap(object);
  return wrapper && wrapper->tree ? wrapper : nullptr;
}

Napi::Value SyntaxNodeWrapper::wrap(Napi::Env env, TSNode other) {
  return from_node(env, other, tree, buffer);
}

Napi::Value SyntaxNodeWrapper::get_id(const CallbackInfo &info) {
  return Number::New(info.Env(), static_cast<double>(reinterpret_cast<uintptr_t>(node.id)));
}

Napi::Value SyntaxNodeWrapper::get_type(const CallbackInfo &info) {
  return String::New(info.Env(), ts_node_type(node));
}

Napi::Value SyntaxNodeWrapper::get_type_id(const CallbackInfo &info) {
  return Number::New(info.Env(), ts_node_symbol(node));
}

Napi::Value SyntaxNodeWrapper::get_text(const CallbackInfo &info) {
  return String::New(info.Env(), tree_sitter::text_for_node(buffer->buffer, node));
}

Napi::Value SyntaxNodeWrapper::get_start_index(const CallbackInfo &info) {
  return Number::New(info.Env(), byte_to_code_unit(ts_node_start_byte(node)));
}

Napi::Value SyntaxNodeWrapper::get_end_index(const CallbackInfo &info) {
  return Number::New(info.Env(), byte_to_code_unit(ts_node_end_byte(node)));
}

Napi::Value SyntaxNodeWrapper::get_start_position(const CallbackInfo &info) {
  return point_to_js(info.Env(), ts_node_start_point(node));
}

Napi::Value SyntaxNodeWrapper::get_end_position(const CallbackInfo &info) {
  return point_to_js(info.Env(), ts_node_end_point(node));
}

Napi::Value SyntaxNodeWrapper::get_is_named(const CallbackInfo &info) {
  return Boolean::New(info.Env(), ts_node_is_named(node));
}

Napi::Value SyntaxNodeWrapper::get_is_missing(const CallbackInfo &info) {
  return Boolean::New(info.Env(), ts_node_is_missing(node));
}

Napi::Value SyntaxNodeWrapper::get_is_extra(const CallbackInfo &info) {
  return Boolean::New(info.Env(), ts_node_is_extra(node));
}

Napi::Value SyntaxNodeWrapper::get_is_error(const CallbackInfo &info) {
  return Boolean::New(info.Env(), ts_node_is_error(node));
}

Napi::Value SyntaxNodeWrapper::get_has_error(const CallbackInfo &info) {
  return Boolean::New(info.Env(), ts_node_has_error(node));
}

Napi::Value SyntaxNodeWrapper::get_has_changes(const CallbackInfo &info) {
  return Boolean::New(info.Env(), ts_node_has_changes(node));
}

Napi::Value SyntaxNodeWrapper::get_parent(const CallbackInfo &info) {
  return wrap(info.Env(), ts_node_parent(node));
}

Napi::Value SyntaxNodeWrapper::get_child_count(const CallbackInfo &info) {
  return Number::New(info.Env(), ts_node_child_count(node));
}

Napi::Value SyntaxNodeWrapper::get_named_child_count(const CallbackInfo &info) {
  return Number::New(info.Env(), ts_node_named_child_count(node));
}

Napi::Value SyntaxNodeWrapper::get_children(const CallbackInfo &info) {
  Napi::Env env = info.Env();
  uint32_t count = ts_node_child_count(node);
  Array result = Array::New(env, count);
  for (uint32_t i = 0; i < count; i++) result[i] = wrap(env, ts_node_child(node, i));
  return result;
}

Napi::Value SyntaxNodeWrapper::get_named_children(const CallbackInfo &info) {
  Napi::Env env = info.Env();
  uint32_t count = ts_node_named_child_count(node);
  Array result = Array::New(env, count);
  for (uint32_t i = 0; i < count; i++) result[i] = wrap(env, ts_node_named_child(node, i));
  return result;
}

Napi::Value SyntaxNodeWrapper::get_first_child(const CallbackInfo &info) {
  return wrap(info.Env(), ts_node_child(node, 0));
}

Napi::Value SyntaxNodeWrapper::get_last_child(const CallbackInfo &info) {
  uint32_t count = ts_node_child_count(node);
  return count ? wrap(info.Env(), ts_node_child(node, count - 1)) : info.Env().Null();
}

Napi::Value SyntaxNodeWrapper::get_first_named_child(const CallbackInfo &info) {
  return wrap(info.Env(), ts_node_named_child(node, 0));
}

Napi::Value SyntaxNodeWrapper::get_last_named_child(const CallbackInfo &info) {
  uint32_t count = ts_node_named_child_count(node);
  return count ? wrap(info.Env(), ts_node_named_child(node, count - 1)) : info.Env().Null();
}

Napi::Value SyntaxNodeWrapper::get_next_sibling(const CallbackInfo &info) {
  return wrap(info.Env(), ts_node_next_sibling(node));
}

Napi::Value SyntaxNodeWrapper::get_previous_sibling(const CallbackInfo &info) {
  return wrap(info.Env(), ts_node_prev_sibling(node));
}

Napi::Value SyntaxNodeWrapper::get_next_named_sibling(const CallbackInfo &info) {
  return wrap(info.Env(), ts_node_next_named_sibling(node));
}

Napi::Value SyntaxNodeWrapper::get_previous_named_sibling(const CallbackInfo &info) {
  return wrap(info.Env(), ts_node_prev_named_sibling(node));
}

Napi::Value SyntaxNodeWrapper::child(const CallbackInfo &info) {
  return wrap(info.Env(), ts_node_child(node, i32_from_js(info[0])));
}

Napi::Value SyntaxNodeWrapper::named_child(const CallbackInfo &info) {
  return wrap(info.Env(), ts_node_named_child(node, i32_from_js(info[0])));
}

Napi::Value SyntaxNodeWrapper::child_for_field_name(const CallbackInfo &info) {
  if (!info[0].IsString()) return info.Env().Null();
  string name = info[0].As<String>().Utf8Value();
  return wrap(info.Env(), ts_node_child_by_field_name(node, name.data(), name.size()));
}

// descendantsOfType(types, startPosition?, endPosition?): mirrors
// web-tree-sitter (including its handling of ERROR and of positions).
Napi::Value SyntaxNodeWrapper::descendants_of_type(const CallbackInfo &info) {
  Napi::Env env = info.Env();
  vector<string> types;
  if (info[0].IsArray()) {
    Array array = info[0].As<Array>();
    for (uint32_t i = 0; i < array.Length(); i++) {
      Napi::Value type = array[i];
      if (type.IsString()) types.push_back(type.As<String>().Utf8Value());
    }
  } else if (info[0].IsString()) {
    types.push_back(info[0].As<String>().Utf8Value());
  }

  const TSLanguage *language = ts_tree_language(tree->tree);
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
  auto contains = [&](TSSymbol symbol) {
    for (TSSymbol s : symbols) if (s == symbol) return true;
    return false;
  };

  TSPoint start_point = info[1].IsObject() ? point_from_js(info[1]) : TSPoint{0, 0};
  TSPoint end_point = info[2].IsObject() ? point_from_js(info[2]) : TSPoint{0, 0};
  if (end_point.row == 0 && end_point.column == 0) end_point = {UINT32_MAX, UINT32_MAX};

  vector<TSNode> result;
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
      if (contains(ts_node_symbol(descendant))) result.push_back(descendant);
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

  Array array = Array::New(env, result.size());
  for (size_t i = 0; i < result.size(); i++) array[i] = wrap(env, result[i]);
  return array;
}

Napi::Value SyntaxNodeWrapper::descendant_for_index(const CallbackInfo &info) {
  uint32_t start = code_unit_to_byte(i32_from_js(info[0]));
  uint32_t end = info.Length() > 1 && !info[1].IsUndefined() ? code_unit_to_byte(i32_from_js(info[1])) : start;
  return wrap(info.Env(), ts_node_descendant_for_byte_range(node, start, end));
}

Napi::Value SyntaxNodeWrapper::named_descendant_for_index(const CallbackInfo &info) {
  uint32_t start = code_unit_to_byte(i32_from_js(info[0]));
  uint32_t end = info.Length() > 1 && !info[1].IsUndefined() ? code_unit_to_byte(i32_from_js(info[1])) : start;
  return wrap(info.Env(), ts_node_named_descendant_for_byte_range(node, start, end));
}

Napi::Value SyntaxNodeWrapper::descendant_for_position(const CallbackInfo &info) {
  TSPoint start = point_from_js(info[0]);
  TSPoint end = info.Length() > 1 && info[1].IsObject() ? point_from_js(info[1]) : start;
  return wrap(info.Env(), ts_node_descendant_for_point_range(node, start, end));
}

Napi::Value SyntaxNodeWrapper::named_descendant_for_position(const CallbackInfo &info) {
  TSPoint start = point_from_js(info[0]);
  TSPoint end = info.Length() > 1 && info[1].IsObject() ? point_from_js(info[1]) : start;
  return wrap(info.Env(), ts_node_named_descendant_for_point_range(node, start, end));
}

Napi::Value SyntaxNodeWrapper::equals(const CallbackInfo &info) {
  SyntaxNodeWrapper *other = from_js(info[0]);
  return Boolean::New(info.Env(), other && ts_node_eq(node, other->node));
}

Napi::Value SyntaxNodeWrapper::to_string(const CallbackInfo &info) {
  char *string = ts_node_string(node);
  Napi::String result = String::New(info.Env(), string);
  free(string);
  return result;
}
