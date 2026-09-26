#include "grammar.h"
#include "wasm-engine.h"

namespace tree_sitter {

std::shared_ptr<Grammar> Grammar::load(const uint8_t *wasm, size_t length, std::string *error) {
  TSWasmStore *store = nullptr;
  const TSLanguage *language = load_wasm_language(wasm, length, &store, error);
  if (!language) return nullptr;
  return std::shared_ptr<Grammar>(new Grammar(language, store, wasm_language_name(wasm, length)));
}

Grammar::Grammar(const TSLanguage *language, TSWasmStore *store, std::string name) :
  language_{language}, loading_store_{store}, name_{std::move(name)} {}

Grammar::~Grammar() {
  ts_language_delete(language_);
  ts_wasm_store_delete(loading_store_);
}

}  // namespace tree_sitter
