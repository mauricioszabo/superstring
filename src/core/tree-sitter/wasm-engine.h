#ifndef SUPERSTRING_TREE_SITTER_WASM_ENGINE_H_
#define SUPERSTRING_TREE_SITTER_WASM_ENGINE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <tree_sitter/api.h>

namespace tree_sitter {

// The process-wide Wasmtime engine that runs WASM grammars. Created on first
// use and never destroyed (stores and compiled modules reference it).
TSWasmEngine *wasm_engine();

// Finds the grammar name `N` of a grammar module from its exported
// `tree_sitter_N` function. Returns an empty string if there is none.
std::string wasm_language_name(const uint8_t *bytes, size_t length);

// Loads a WASM grammar into a new store. On failure returns null and fills
// `error`. The store is returned through `store` and must be kept alive (and
// attached to a parser with ts_parser_set_wasm_store) while the language is
// used; languages can be used with other stores of the same engine.
const TSLanguage *load_wasm_language(const uint8_t *bytes, size_t length, TSWasmStore **store,
                                     std::string *error);

}  // namespace tree_sitter

#endif  // SUPERSTRING_TREE_SITTER_WASM_ENGINE_H_
