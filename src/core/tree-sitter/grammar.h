#ifndef SUPERSTRING_TREE_SITTER_GRAMMAR_H_
#define SUPERSTRING_TREE_SITTER_GRAMMAR_H_

#include <memory>
#include <string>
#include <tree_sitter/api.h>

namespace tree_sitter {

// A Tree-sitter language loaded from a WASM grammar. Languages loaded this
// way can be used by parsers with any wasm store of the shared engine.
class Grammar {
public:
  // Returns null and fills `error` if the module can't be loaded.
  static std::shared_ptr<Grammar> load(const uint8_t *wasm, size_t length, std::string *error);
  ~Grammar();

  const TSLanguage *language() const { return language_; }
  const std::string &name() const { return name_; }

private:
  Grammar(const TSLanguage *language, TSWasmStore *store, std::string name);

  const TSLanguage *language_;
  TSWasmStore *loading_store_;
  std::string name_;
};

}  // namespace tree_sitter

#endif  // SUPERSTRING_TREE_SITTER_GRAMMAR_H_
