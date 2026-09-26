#include <algorithm>
#include <cmath>
#include <cstdlib>
#include "tree-sitter-wrapper.h"
#include "addon-data.h"
#include "text-buffer-wrapper.h"
#include "tree-sitter/parser.h"
#include "string-conversion.h"

using namespace Napi;
using std::shared_ptr;
using std::string;
using std::vector;
using tree_sitter::Grammar;
using tree_sitter::Parser;
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
    InstanceMethod<&TreeSitterQueryWrapper::highlights>("highlights", napi_default_method),
    InstanceMethod<&TreeSitterQueryWrapper::highlight_scope_names>("highlightScopeNames", napi_default_method),
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

// query.highlights(rootNode, {startPosition, endPosition, injection, config})
//   -> {boundaries, invalidatedNodeIds, scopeNameCount}
//
// Resolves a highlights query's captures within the range the way
// `ScopeResolver` does. `boundaries` is a Uint32Array of, for each position in
// order, `row, column, closeCount, openCount`, then the scopes to close and
// the scopes to open, as indexes into `highlightScopeNames()`.
// `config(key)` returns the config value that `test.config` checks.
Napi::Value TreeSitterQueryWrapper::highlights(const CallbackInfo &info) {
  Napi::Env env = info.Env();
  SyntaxNodeWrapper *node = SyntaxNodeWrapper::from_js(info[0]);
  if (!node || !info[1].IsObject()) {
    Error::New(env, "Expected (node, options)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Object options = info[1].As<Object>();
  auto position_from_js = [](const Napi::Value &value) {
    if (!value.IsObject()) return Point();
    Object object = value.As<Object>();
    return Point(i32_from_js(object.Get("row")), i32_from_js(object.Get("column")));
  };
  Point from = position_from_js(options.Get("startPosition"));
  Point to = position_from_js(options.Get("endPosition"));

  Napi::Value config = options.Get("config");
  tree_sitter::HighlightContext context;
  context.buffer = node->buffer->buffer;
  context.injection = options.Get("injection").ToBoolean();
  if (config.IsFunction()) {
    Function config_function = config.As<Function>();
    // `ScopeResolver.TESTS.config`, with `interpretPredicateValue`.
    context.config = [env, config_function](const string &key, const optional<string> &value) {
      if (env.IsExceptionPending()) return false;
      Napi::Value config_value = config_function.Call({String::New(env, key)});
      if (env.IsExceptionPending()) return false;
      if (config_value.IsNull() || config_value.IsUndefined()) config_value = Boolean::New(env, false);
      if (!value) return config_value.ToBoolean().Value();
      Napi::Value expected;
      if (*value == "true") {
        expected = Boolean::New(env, true);
      } else if (*value == "false") {
        expected = Boolean::New(env, false);
      } else if (!value->empty() && std::all_of(value->begin(), value->end(), [](char c) { return c >= '0' && c <= '9'; })) {
        expected = Number::New(env, std::strtod(value->c_str(), nullptr));
      } else {
        expected = String::New(env, *value);
      }
      return config_value.StrictEquals(expected);
    };
  }

  if (!scope_resolver) scope_resolver.reset(new tree_sitter::ScopeResolver(query));
  vector<tree_sitter::HighlightBoundary> boundaries;
  vector<const void *> invalidated_node_ids;
  scope_resolver->resolve(node->node, from, to, context, &boundaries, &invalidated_node_ids);
  if (env.IsExceptionPending()) return env.Undefined();

  size_t length = 0;
  for (auto &boundary : boundaries) length += 4 + boundary.close.size() + boundary.open.size();
  Uint32Array data = Uint32Array::New(env, length);
  uint32_t *cursor = data.Data();
  for (auto &boundary : boundaries) {
    *cursor++ = boundary.position.row;
    *cursor++ = boundary.position.column;
    *cursor++ = boundary.close.size();
    *cursor++ = boundary.open.size();
    for (uint32_t id : boundary.close) *cursor++ = id;
    for (uint32_t id : boundary.open) *cursor++ = id;
  }

  Array ids = Array::New(env, invalidated_node_ids.size());
  for (size_t i = 0; i < invalidated_node_ids.size(); i++) {
    ids[i] = Number::New(env, static_cast<double>(reinterpret_cast<uintptr_t>(invalidated_node_ids[i])));
  }

  Object result = Object::New(env);
  result.Set("boundaries", data);
  result.Set("invalidatedNodeIds", ids);
  result.Set("scopeNameCount", Number::New(env, scope_resolver->scope_names().size()));
  return result;
}

// query.highlightScopeNames(start = 0) -> the scope names `highlights`
// boundaries refer to, from index `start` on.
Napi::Value TreeSitterQueryWrapper::highlight_scope_names(const CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (!scope_resolver) return Array::New(env, 0);
  auto &names = scope_resolver->scope_names();
  uint32_t start = std::min<size_t>(info[0].IsNumber() ? i32_from_js(info[0]) : 0, names.size());
  Array result = Array::New(env, names.size() - start);
  for (size_t i = start; i < names.size(); i++) result[i - start] = String::New(env, names[i]);
  return result;
}

namespace {

// A web-tree-sitter range ({startIndex, endIndex, startPosition,
// endPosition}, in code units) as a Tree-sitter range.
TSRange range_from_js(const Napi::Value &value) {
  Object object = value.As<Object>();
  return {
    point_from_js(object.Get("startPosition")),
    point_from_js(object.Get("endPosition")),
    code_unit_to_byte(i32_from_js(object.Get("startIndex"))),
    code_unit_to_byte(i32_from_js(object.Get("endIndex")))
  };
}

Object range_to_js(Napi::Env env, const TSRange &range) {
  Object result = Object::New(env);
  result.Set("startIndex", Number::New(env, byte_to_code_unit(range.start_byte)));
  result.Set("endIndex", Number::New(env, byte_to_code_unit(range.end_byte)));
  result.Set("startPosition", point_to_js(env, range.start_point));
  result.Set("endPosition", point_to_js(env, range.end_point));
  return result;
}

}  // namespace

// TreeSitterParser

void TreeSitterParserWrapper::init(Napi::Env env, Object exports) {
  Function func = DefineClass(env, "TreeSitterParser", {
    InstanceMethod<&TreeSitterParserWrapper::parse>("parse", napi_default_method),
  });
  exports.Set("TreeSitterParser", func);
}

// new TreeSitterParser(grammar)
TreeSitterParserWrapper::TreeSitterParserWrapper(const CallbackInfo &info) : ObjectWrap<TreeSitterParserWrapper>(info) {
  auto grammar = TreeSitterGrammarWrapper::from_js(info[0]);
  if (!grammar) {
    Error::New(info.Env(), "Expected a TreeSitterGrammar").ThrowAsJavaScriptException();
    return;
  }
  parser.reset(new Parser(grammar));
}

// parse(nativeTextBuffer, oldTree, includedRanges) -> TreeSitterTree | null.
// `oldTree` (or null) must have been edited to match the buffer;
// `includedRanges` is an array of web-tree-sitter ranges, or null.
Napi::Value TreeSitterParserWrapper::parse(const CallbackInfo &info) {
  Napi::Env env = info.Env();
  TextBufferWrapper *text_buffer = info[0].IsObject() ? TextBufferWrapper::Unwrap(info[0].As<Object>()) : nullptr;
  if (!text_buffer || !parser) {
    Error::New(env, "Expected a native TextBuffer").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  TreeSitterTreeWrapper *old_tree = TreeSitterTreeWrapper::from_js(info[1]);

  vector<TSRange> included_ranges;
  if (info[2].IsArray()) {
    Array ranges = info[2].As<Array>();
    for (uint32_t i = 0; i < ranges.Length(); i++) {
      Napi::Value range = ranges[i];
      if (range.IsObject()) included_ranges.push_back(range_from_js(range));
    }
  }

  TSTree *tree = parser->parse(
    &text_buffer->text_buffer,
    old_tree && old_tree->tree ? old_tree->tree->tree : nullptr,
    included_ranges
  );
  if (!tree) return env.Null();

  auto buffer = std::make_shared<TextBufferHandle>();
  buffer->reference = Persistent(info[0].As<Object>());
  buffer->buffer = &text_buffer->text_buffer;
  return TreeSitterTreeWrapper::from_tree(env, std::make_shared<TreeHolder>(tree), buffer);
}

// TreeSitterTree

void TreeSitterTreeWrapper::init(Napi::Env env, Object exports) {
  auto *data = env.GetInstanceData<AddonData>();
  Function func = DefineClass(env, "TreeSitterTree", {
    InstanceAccessor<&TreeSitterTreeWrapper::get_root_node>("rootNode"),
    InstanceMethod<&TreeSitterTreeWrapper::edit>("edit", napi_default_method),
    InstanceMethod<&TreeSitterTreeWrapper::get_changed_ranges>("getChangedRanges", napi_default_method),
    InstanceMethod<&TreeSitterTreeWrapper::copy>("copy", napi_default_method),
    InstanceMethod<&TreeSitterTreeWrapper::delete_tree>("delete", napi_default_method),
  });
  data->tree_sitter_tree_constructor = Persistent(func);
  exports.Set("TreeSitterTree", func);
}

TreeSitterTreeWrapper::TreeSitterTreeWrapper(const CallbackInfo &info) : ObjectWrap<TreeSitterTreeWrapper>(info) {}

Napi::Value TreeSitterTreeWrapper::from_tree(Napi::Env env, shared_ptr<TreeHolder> tree,
                                             shared_ptr<TextBufferHandle> buffer) {
  auto *data = env.GetInstanceData<AddonData>();
  Object object = data->tree_sitter_tree_constructor.New({});
  TreeSitterTreeWrapper *wrapper = Unwrap(object);
  wrapper->tree = std::move(tree);
  wrapper->buffer = std::move(buffer);
  return object;
}

TreeSitterTreeWrapper *TreeSitterTreeWrapper::from_js(Napi::Value value) {
  if (!value.IsObject()) return nullptr;
  auto *data = value.Env().GetInstanceData<AddonData>();
  Object object = value.As<Object>();
  if (!object.InstanceOf(data->tree_sitter_tree_constructor.Value())) return nullptr;
  return Unwrap(object);
}

Napi::Value TreeSitterTreeWrapper::get_root_node(const CallbackInfo &info) {
  if (!tree) return info.Env().Null();
  return SyntaxNodeWrapper::from_node(info.Env(), ts_tree_root_node(tree->tree), tree, buffer);
}

// edit({startIndex, oldEndIndex, newEndIndex, startPosition, oldEndPosition,
// newEndPosition}), in code units, like web-tree-sitter's Tree#edit.
void TreeSitterTreeWrapper::edit(const CallbackInfo &info) {
  if (!tree || !info[0].IsObject()) return;
  Object object = info[0].As<Object>();
  TSInputEdit edit = {
    code_unit_to_byte(i32_from_js(object.Get("startIndex"))),
    code_unit_to_byte(i32_from_js(object.Get("oldEndIndex"))),
    code_unit_to_byte(i32_from_js(object.Get("newEndIndex"))),
    point_from_js(object.Get("startPosition")),
    point_from_js(object.Get("oldEndPosition")),
    point_from_js(object.Get("newEndPosition"))
  };
  ts_tree_edit(tree->tree, &edit);
}

// getChangedRanges(newTree) -> [web-tree-sitter ranges]
Napi::Value TreeSitterTreeWrapper::get_changed_ranges(const CallbackInfo &info) {
  Napi::Env env = info.Env();
  TreeSitterTreeWrapper *other = from_js(info[0]);
  if (!tree || !other || !other->tree) {
    Error::New(env, "Argument must be a Tree").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  uint32_t count = 0;
  TSRange *ranges = ts_tree_get_changed_ranges(tree->tree, other->tree->tree, &count);
  Array result = Array::New(env, count);
  for (uint32_t i = 0; i < count; i++) result[i] = range_to_js(env, ranges[i]);
  free(ranges);
  return result;
}

Napi::Value TreeSitterTreeWrapper::copy(const CallbackInfo &info) {
  if (!tree) return info.Env().Null();
  return from_tree(info.Env(), std::make_shared<TreeHolder>(ts_tree_copy(tree->tree)), buffer);
}

void TreeSitterTreeWrapper::delete_tree(const CallbackInfo &info) {
  tree.reset();
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
    InstanceAccessor<&SyntaxNodeWrapper::get_tree>("tree"),
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

Napi::Value SyntaxNodeWrapper::get_tree(const CallbackInfo &info) {
  return TreeSitterTreeWrapper::from_tree(info.Env(), tree, buffer);
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

  TSPoint start_point = info[1].IsObject() ? point_from_js(info[1]) : TSPoint{0, 0};
  TSPoint end_point = info[2].IsObject() ? point_from_js(info[2]) : TSPoint{0, 0};
  vector<TSNode> result;
  tree_sitter::descendants_of_type(node, tree_sitter::symbols_for_types(ts_tree_language(tree->tree), types),
                                   start_point, end_point, &result);

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
