#ifndef SUPERSTRING_TREE_SITTER_STRINGS_H_
#define SUPERSTRING_TREE_SITTER_STRINGS_H_

#include <cstdint>
#include <string>

namespace tree_sitter {

// UTF-8 (Tree-sitter's query strings) to UTF-16 (buffer text) and back.
inline std::u16string utf8_to_utf16(const std::string &input) {
  std::u16string output;
  output.reserve(input.size());
  for (size_t i = 0; i < input.size();) {
    uint32_t code_point;
    unsigned char c = input[i];
    size_t length;
    if (c < 0x80) { code_point = c; length = 1; }
    else if ((c >> 5) == 0x6) { code_point = c & 0x1f; length = 2; }
    else if ((c >> 4) == 0xe) { code_point = c & 0x0f; length = 3; }
    else if ((c >> 3) == 0x1e) { code_point = c & 0x07; length = 4; }
    else { output.push_back(0xfffd); i++; continue; }
    if (i + length > input.size()) { output.push_back(0xfffd); break; }
    for (size_t j = 1; j < length; j++) code_point = (code_point << 6) | (input[i + j] & 0x3f);
    if (code_point >= 0x10000) {
      code_point -= 0x10000;
      output.push_back(0xd800 + (code_point >> 10));
      output.push_back(0xdc00 + (code_point & 0x3ff));
    } else {
      output.push_back(code_point);
    }
    i += length;
  }
  return output;
}

inline std::string utf16_to_utf8(const std::u16string &input) {
  std::string output;
  for (size_t i = 0; i < input.size(); i++) {
    uint32_t c = input[i];
    if (c >= 0xd800 && c <= 0xdbff && i + 1 < input.size() && input[i + 1] >= 0xdc00 && input[i + 1] <= 0xdfff) {
      c = 0x10000 + ((c - 0xd800) << 10) + (input[i + 1] - 0xdc00);
      i++;
    }
    if (c < 0x80) {
      output.push_back(c);
    } else if (c < 0x800) {
      output.push_back(0xc0 | (c >> 6));
      output.push_back(0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
      output.push_back(0xe0 | (c >> 12));
      output.push_back(0x80 | ((c >> 6) & 0x3f));
      output.push_back(0x80 | (c & 0x3f));
    } else {
      output.push_back(0xf0 | (c >> 18));
      output.push_back(0x80 | ((c >> 12) & 0x3f));
      output.push_back(0x80 | ((c >> 6) & 0x3f));
      output.push_back(0x80 | (c & 0x3f));
    }
  }
  return output;
}

}  // namespace tree_sitter

#endif  // SUPERSTRING_TREE_SITTER_STRINGS_H_
