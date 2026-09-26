#ifndef SUPERSTRING_CHARACTER_CLASSES_H_
#define SUPERSTRING_CHARACTER_CLASSES_H_

#include <cstdint>

// Ports of the character classification helpers used by the display layer:
// text-buffer's `is-character-pair.js` and Pulsar's `text-utils.js`. They all
// operate on single UTF-16 code units, like their JS counterparts (which call
// `charCodeAt(0)`).
namespace character_classes {

inline bool is_high_surrogate(uint16_t c) { return 0xD800 <= c && c <= 0xDBFF; }
inline bool is_low_surrogate(uint16_t c) { return 0xDC00 <= c && c <= 0xDFFF; }
inline bool is_variation_selector(uint16_t c) { return 0xFE00 <= c && c <= 0xFE0F; }

inline bool is_combining_character(uint16_t c) {
  return (0x0300 <= c && c <= 0x036F) ||
    (0x1AB0 <= c && c <= 0x1AFF) ||
    (0x1DC0 <= c && c <= 0x1DFF) ||
    (0x20D0 <= c && c <= 0x20FF) ||
    (0xFE20 <= c && c <= 0xFE2F);
}

inline bool is_character_pair(uint16_t a, uint16_t b) {
  return (is_high_surrogate(a) && is_low_surrogate(b)) ||
    (!is_variation_selector(a) && is_variation_selector(b)) ||
    (!is_combining_character(a) && is_combining_character(b));
}

inline bool is_japanese_kana(uint16_t c) { return 0x3000 <= c && c <= 0x30FF; }
inline bool is_cjk_unified_ideograph(uint16_t c) { return 0x4E00 <= c && c <= 0x9FFF; }
inline bool is_full_width_form(uint16_t c) {
  return (0xFF01 <= c && c <= 0xFF5E) || (0xFFE0 <= c && c <= 0xFFE6);
}

inline bool is_double_width(uint16_t c) {
  return is_japanese_kana(c) || is_cjk_unified_ideograph(c) || is_full_width_form(c);
}

inline bool is_half_width(uint16_t c) {
  return (0xFF65 <= c && c <= 0xFFDC) || (0xFFE8 <= c && c <= 0xFFEE);
}

inline bool is_korean(uint16_t c) {
  return (0xAC00 <= c && c <= 0xD7A3) ||
    (0x1100 <= c && c <= 0x11FF) ||
    (0x3130 <= c && c <= 0x318F) ||
    (0xA960 <= c && c <= 0xA97F) ||
    (0xD7B0 <= c && c <= 0xD7FF);
}

inline bool is_cjk(uint16_t c) {
  return is_double_width(c) || is_half_width(c) || is_korean(c);
}

// text-buffer's default `isWordStart`.
inline bool is_word_start(uint16_t previous, uint16_t c) {
  return (previous == ' ' || previous == '\t') && (c != ' ' && c != '\t');
}

// Pulsar's `isWrapBoundary`: word starts (also after '-' and '/') or CJK.
inline bool is_pulsar_wrap_boundary(uint16_t previous, uint16_t c) {
  bool word_start =
    (previous == ' ' || previous == '\t' || previous == '-' || previous == '/') &&
    (c != ' ' && c != '\t');
  return word_start || is_cjk(c);
}

}  // namespace character_classes

#endif  // SUPERSTRING_CHARACTER_CLASSES_H_
