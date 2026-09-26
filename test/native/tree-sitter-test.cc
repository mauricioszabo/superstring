#include "test-helpers.h"
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include "tree-sitter/wasm-engine.h"

using std::string;
using std::vector;

namespace {

void find_wasm_files(const string &directory, vector<string> *result) {
  DIR *dir = opendir(directory.c_str());
  if (!dir) return;
  while (dirent *entry = readdir(dir)) {
    string name = entry->d_name;
    if (name == "." || name == ".." || name == "node_modules" || name == "spec") continue;
    string path = directory + "/" + name;
    struct stat info;
    if (stat(path.c_str(), &info) != 0) continue;
    if (S_ISDIR(info.st_mode)) {
      find_wasm_files(path, result);
    } else if (name.size() > 5 && name.compare(name.size() - 5, 5, ".wasm") == 0) {
      result->push_back(path);
    }
  }
  closedir(dir);
}

vector<uint8_t> read_file(const string &path) {
  std::ifstream file(path, std::ios::binary);
  return vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

}  // namespace

// Loads every grammar under $SUPERSTRING_GRAMMARS_DIR (e.g. Pulsar's
// `packages` directory) through the wasm store, including grammars with
// legacy `dylink` sections and GOT imports, and parses a small input.
TEST_CASE("tree-sitter - loads bundled WASM grammars") {
  const char *grammars_directory = getenv("SUPERSTRING_GRAMMARS_DIR");
  if (!grammars_directory) {
    WARN("SUPERSTRING_GRAMMARS_DIR is not set; skipping");
    return;
  }

  vector<string> paths;
  find_wasm_files(grammars_directory, &paths);
  REQUIRE(!paths.empty());

  for (auto &path : paths) {
    INFO(path);
    auto bytes = read_file(path);
    TSWasmStore *loading_store = nullptr;
    string error;
    const TSLanguage *language = tree_sitter::load_wasm_language(bytes.data(), bytes.size(), &loading_store, &error);
    INFO(error);
    REQUIRE(language != nullptr);

    // Parse with a parser that has a store of its own.
    TSWasmError wasm_error = {TSWasmErrorKindNone, nullptr};
    TSWasmStore *parser_store = ts_wasm_store_new(tree_sitter::wasm_engine(), &wasm_error);
    REQUIRE(parser_store != nullptr);
    TSParser *parser = ts_parser_new();
    ts_parser_set_wasm_store(parser, parser_store);
    REQUIRE(ts_parser_set_language(parser, language));

    const char *source = "a = 1\n# comment\n<b>x</b> \"s\" (c) { d: [1, 2] }\n";
    TSTree *tree = ts_parser_parse_string(parser, nullptr, source, strlen(source));
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    REQUIRE(ts_node_end_byte(root) > 0);

    ts_tree_delete(tree);
    ts_parser_delete(parser);
    ts_language_delete(language);
    ts_wasm_store_delete(loading_store);
  }
}
