#ifndef SUPERSTRING_TREE_SITTER_WRAPPER_H
#define SUPERSTRING_TREE_SITTER_WRAPPER_H

#include <memory>
#include <vector>
#include "napi.h"
#include "tree-sitter/grammar.h"
#include "tree-sitter/parser.h"
#include "tree-sitter/query.h"
#include "tree-sitter/scope-resolver.h"

// Bindings for superstring's native Tree-sitter support. Positions and
// indexes use the buffer's UTF-16 code units, like web-tree-sitter's; nodes
// implement the part of web-tree-sitter's Node API Pulsar and its packages
// use.

// Keeps the JS TextBuffer (and so the native one) alive for nodes, which
// read their text from it. One per language mode, shared by its nodes.
struct TextBufferHandle {
  Napi::ObjectReference reference;
  TextBuffer *buffer;
};

class TreeSitterGrammarWrapper : public Napi::ObjectWrap<TreeSitterGrammarWrapper> {
public:
  static void init(Napi::Env env, Napi::Object exports);
  explicit TreeSitterGrammarWrapper(const Napi::CallbackInfo &info);
  static std::shared_ptr<tree_sitter::Grammar> from_js(Napi::Value value);

  std::shared_ptr<tree_sitter::Grammar> grammar;

private:
  Napi::Value get_name(const Napi::CallbackInfo &info);
  Napi::Value query(const Napi::CallbackInfo &info);
  Napi::Value id_for_node_type(const Napi::CallbackInfo &info);
};

class TreeSitterQueryWrapper : public Napi::ObjectWrap<TreeSitterQueryWrapper> {
public:
  static void init(Napi::Env env, Napi::Object exports);
  explicit TreeSitterQueryWrapper(const Napi::CallbackInfo &info);

  std::shared_ptr<tree_sitter::Query> query;

private:
  Napi::Value get_capture_names(const Napi::CallbackInfo &info);
  Napi::Value captures(const Napi::CallbackInfo &info);
  Napi::Value highlights(const Napi::CallbackInfo &info);
  Napi::Value highlight_scope_names(const Napi::CallbackInfo &info);
  Napi::Value properties_object(Napi::Env env, std::vector<Napi::ObjectReference> *cache,
                                uint32_t pattern, const std::vector<tree_sitter::QueryProperty> &properties);

  // An array of the capture names (references can't hold strings).
  Napi::ObjectReference capture_names;
  std::vector<Napi::ObjectReference> set_properties_objects;
  std::vector<Napi::ObjectReference> asserted_properties_objects;
  std::vector<Napi::ObjectReference> refuted_properties_objects;
  // Created on the first `highlights` call.
  std::unique_ptr<tree_sitter::ScopeResolver> scope_resolver;
};

class TreeSitterParserWrapper : public Napi::ObjectWrap<TreeSitterParserWrapper> {
public:
  static void init(Napi::Env env, Napi::Object exports);
  explicit TreeSitterParserWrapper(const Napi::CallbackInfo &info);

private:
  Napi::Value parse(const Napi::CallbackInfo &info);

  std::unique_ptr<tree_sitter::Parser> parser;
};

// Like web-tree-sitter's Tree. Wrappers made by `copy()` own separate trees;
// `delete()` releases this wrapper's tree (nodes keep it alive if needed).
class TreeSitterTreeWrapper : public Napi::ObjectWrap<TreeSitterTreeWrapper> {
public:
  static void init(Napi::Env env, Napi::Object exports);
  explicit TreeSitterTreeWrapper(const Napi::CallbackInfo &info);

  static Napi::Value from_tree(Napi::Env env, std::shared_ptr<tree_sitter::TreeHolder> tree,
                               std::shared_ptr<TextBufferHandle> buffer);
  static TreeSitterTreeWrapper *from_js(Napi::Value value);

  std::shared_ptr<tree_sitter::TreeHolder> tree;
  std::shared_ptr<TextBufferHandle> buffer;

private:
  Napi::Value get_root_node(const Napi::CallbackInfo &info);
  void edit(const Napi::CallbackInfo &info);
  Napi::Value get_changed_ranges(const Napi::CallbackInfo &info);
  Napi::Value copy(const Napi::CallbackInfo &info);
  void delete_tree(const Napi::CallbackInfo &info);
};

class SyntaxNodeWrapper : public Napi::ObjectWrap<SyntaxNodeWrapper> {
public:
  static void init(Napi::Env env, Napi::Object exports);
  explicit SyntaxNodeWrapper(const Napi::CallbackInfo &info);

  // Returns null for a null node.
  static Napi::Value from_node(Napi::Env env, TSNode node, const std::shared_ptr<tree_sitter::TreeHolder> &tree,
                               const std::shared_ptr<TextBufferHandle> &buffer);
  static SyntaxNodeWrapper *from_js(Napi::Value value);

  TSNode node;
  std::shared_ptr<tree_sitter::TreeHolder> tree;
  std::shared_ptr<TextBufferHandle> buffer;

private:
  Napi::Value wrap(Napi::Env env, TSNode other);

  Napi::Value get_id(const Napi::CallbackInfo &info);
  Napi::Value get_type(const Napi::CallbackInfo &info);
  Napi::Value get_type_id(const Napi::CallbackInfo &info);
  Napi::Value get_text(const Napi::CallbackInfo &info);
  Napi::Value get_start_index(const Napi::CallbackInfo &info);
  Napi::Value get_end_index(const Napi::CallbackInfo &info);
  Napi::Value get_start_position(const Napi::CallbackInfo &info);
  Napi::Value get_end_position(const Napi::CallbackInfo &info);
  Napi::Value get_is_named(const Napi::CallbackInfo &info);
  Napi::Value get_is_missing(const Napi::CallbackInfo &info);
  Napi::Value get_is_extra(const Napi::CallbackInfo &info);
  Napi::Value get_is_error(const Napi::CallbackInfo &info);
  Napi::Value get_has_error(const Napi::CallbackInfo &info);
  Napi::Value get_has_changes(const Napi::CallbackInfo &info);
  Napi::Value get_parent(const Napi::CallbackInfo &info);
  Napi::Value get_tree(const Napi::CallbackInfo &info);
  Napi::Value get_child_count(const Napi::CallbackInfo &info);
  Napi::Value get_named_child_count(const Napi::CallbackInfo &info);
  Napi::Value get_children(const Napi::CallbackInfo &info);
  Napi::Value get_named_children(const Napi::CallbackInfo &info);
  Napi::Value get_first_child(const Napi::CallbackInfo &info);
  Napi::Value get_last_child(const Napi::CallbackInfo &info);
  Napi::Value get_first_named_child(const Napi::CallbackInfo &info);
  Napi::Value get_last_named_child(const Napi::CallbackInfo &info);
  Napi::Value get_next_sibling(const Napi::CallbackInfo &info);
  Napi::Value get_previous_sibling(const Napi::CallbackInfo &info);
  Napi::Value get_next_named_sibling(const Napi::CallbackInfo &info);
  Napi::Value get_previous_named_sibling(const Napi::CallbackInfo &info);
  Napi::Value child(const Napi::CallbackInfo &info);
  Napi::Value named_child(const Napi::CallbackInfo &info);
  Napi::Value child_for_field_name(const Napi::CallbackInfo &info);
  Napi::Value descendants_of_type(const Napi::CallbackInfo &info);
  Napi::Value descendant_for_index(const Napi::CallbackInfo &info);
  Napi::Value named_descendant_for_index(const Napi::CallbackInfo &info);
  Napi::Value descendant_for_position(const Napi::CallbackInfo &info);
  Napi::Value named_descendant_for_position(const Napi::CallbackInfo &info);
  Napi::Value equals(const Napi::CallbackInfo &info);
  Napi::Value to_string(const Napi::CallbackInfo &info);
};

#endif  // SUPERSTRING_TREE_SITTER_WRAPPER_H
