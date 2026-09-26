#include "wasm-engine.h"
#include <mutex>
#include <wasm.h>

namespace tree_sitter {

TSWasmEngine *wasm_engine() {
  static std::once_flag once;
  static wasm_engine_t *engine = nullptr;
  std::call_once(once, []() { engine = wasm_engine_new(); });
  return reinterpret_cast<TSWasmEngine *>(engine);
}

namespace {

bool read_uleb128(const uint8_t *&p, const uint8_t *end, uint32_t *value) {
  uint64_t result = 0;
  unsigned shift = 0;
  while (p < end) {
    uint8_t byte = *p++;
    result |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if (byte < 0x80) {
      *value = static_cast<uint32_t>(result);
      return true;
    }
    shift += 7;
    if (shift > 35) return false;
  }
  return false;
}

}  // namespace

std::string wasm_language_name(const uint8_t *bytes, size_t length) {
  const uint8_t EXPORT_SECTION = 7;
  const uint8_t EXPORT_KIND_FUNCTION = 0;
  const std::string prefix = "tree_sitter_";
  const std::string scanner_marker = "_external_scanner_";

  if (length < 8) return "";
  const uint8_t *p = bytes + 8;
  const uint8_t *end = bytes + length;
  while (p < end) {
    uint8_t section_id = *p++;
    uint32_t section_length;
    if (!read_uleb128(p, end, &section_length)) return "";
    const uint8_t *section_end = p + section_length;
    if (section_end > end) return "";
    if (section_id == EXPORT_SECTION) {
      uint32_t count;
      if (!read_uleb128(p, section_end, &count)) return "";
      for (uint32_t i = 0; i < count; i++) {
        uint32_t name_length;
        if (!read_uleb128(p, section_end, &name_length) || p + name_length + 1 > section_end) return "";
        std::string name(reinterpret_cast<const char *>(p), name_length);
        p += name_length;
        uint8_t kind = *p++;
        uint32_t index;
        if (!read_uleb128(p, section_end, &index)) return "";
        if (kind == EXPORT_KIND_FUNCTION && name.compare(0, prefix.size(), prefix) == 0 &&
            name.find(scanner_marker) == std::string::npos) {
          return name.substr(prefix.size());
        }
      }
      return "";
    }
    p = section_end;
  }
  return "";
}

const TSLanguage *load_wasm_language(const uint8_t *bytes, size_t length, TSWasmStore **store,
                                     std::string *error) {
  *store = nullptr;
  std::string name = wasm_language_name(bytes, length);
  if (name.empty()) {
    *error = "no tree_sitter_<name> export found in grammar module";
    return nullptr;
  }

  TSWasmError wasm_error = {TSWasmErrorKindNone, nullptr};
  TSWasmStore *new_store = ts_wasm_store_new(wasm_engine(), &wasm_error);
  if (!new_store) {
    *error = std::string("failed to create wasm store: ") + (wasm_error.message ? wasm_error.message : "");
    free(wasm_error.message);
    return nullptr;
  }

  const TSLanguage *language = ts_wasm_store_load_language(
    new_store, name.c_str(), reinterpret_cast<const char *>(bytes), length, &wasm_error
  );
  if (!language) {
    *error = std::string("failed to load grammar '") + name + "': " + (wasm_error.message ? wasm_error.message : "");
    free(wasm_error.message);
    ts_wasm_store_delete(new_store);
    return nullptr;
  }

  *store = new_store;
  return language;
}

}  // namespace tree_sitter
