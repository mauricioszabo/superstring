#include "test-helpers.h"
#include "display-layer.h"
#include "character-classes.h"

using std::u16string;
using std::vector;
using ClipDirection = DisplayLayer::ClipDirection;

namespace {

vector<u16string> build_lines(DisplayLayer &layer, uint32_t start, uint32_t end,
                              vector<const DisplayLayer::ScreenLine *> *result = nullptr) {
  uint32_t end_buffer_row;
  layer.prepare_screen_lines(start, end, &end_buffer_row);
  vector<const DisplayLayer::ScreenLine *> lines;
  REQUIRE(layer.build_screen_lines(start, end, {}, false, &lines));
  vector<u16string> texts;
  for (auto line : lines) texts.push_back(line->text);
  if (result) *result = lines;
  return texts;
}

DisplayLayer::Params default_params() {
  DisplayLayer::Params params;
  params.tab_length = 4;
  return params;
}

}  // namespace

TEST_CASE("character_classes - character pairs and widths") {
  using namespace character_classes;
  REQUIRE(is_character_pair(0xD83D, 0xDE00));   // surrogate pair
  REQUIRE(is_character_pair('a', 0xFE0F));      // variation sequence
  REQUIRE(is_character_pair('e', 0x0301));      // combining acute accent
  REQUIRE(!is_character_pair('a', 'b'));
  REQUIRE(is_double_width(0x4E2D));             // 中
  REQUIRE(is_half_width(0xFF76));               // ｶ
  REQUIRE(is_korean(0xAC00));                   // 가
  REQUIRE(is_pulsar_wrap_boundary('-', 'a'));
  REQUIRE(!is_word_start('-', 'a'));
}

TEST_CASE("DisplayLayer - expands hard tabs") {
  TextBuffer buffer{u"a\tb\n\tc"};
  DisplayLayer layer{&buffer, nullptr, default_params()};

  REQUIRE(build_lines(layer, 0, 2) == vector<u16string>({u"a   b", u"    c"}));
  REQUIRE(layer.translate_buffer_position(0, 2, ClipDirection::Closest) == Point(0, 4));
  bool out_of_range;
  REQUIRE(layer.translate_screen_position(0, 4, ClipDirection::Closest, false, &out_of_range) == Point(0, 2));
  REQUIRE(layer.translate_screen_position(0, 2, ClipDirection::Backward, false, &out_of_range) == Point(0, 1));
  REQUIRE(layer.translate_screen_position(0, 2, ClipDirection::Forward, false, &out_of_range) == Point(0, 2));
}

TEST_CASE("DisplayLayer - soft wraps at word boundaries") {
  TextBuffer buffer{u"abc def ghi jkl"};
  auto params = default_params();
  params.soft_wrap_column = 8;
  DisplayLayer layer{&buffer, nullptr, params};

  REQUIRE(layer.get_screen_line_count() == 2);
  REQUIRE(build_lines(layer, 0, 2) == vector<u16string>({u"abc def ", u"ghi jkl"}));
  REQUIRE(layer.translate_buffer_position(0, 9, ClipDirection::Closest) == Point(1, 1));
  bool out_of_range;
  REQUIRE(layer.translate_screen_position(1, 1, ClipDirection::Closest, false, &out_of_range) == Point(0, 9));
}

TEST_CASE("DisplayLayer - folds") {
  TextBuffer buffer{u"abc\ndef\nghi\njkl"};
  MarkerIndex folds;
  folds.insert(1, Point(0, 1), Point(2, 1));
  DisplayLayer layer{&buffer, [&folds]() { return &folds; }, default_params()};

  REQUIRE(layer.get_screen_line_count() == 2);
  REQUIRE(build_lines(layer, 0, 2) == vector<u16string>({u"a⋯hi", u"jkl"}));
  REQUIRE(layer.translate_buffer_position(2, 2, ClipDirection::Closest) == Point(0, 3));
  REQUIRE(layer.translate_buffer_position(1, 1, ClipDirection::Backward) == Point(0, 1));
  REQUIRE(layer.translate_buffer_position(1, 1, ClipDirection::Forward) == Point(0, 2));
}

TEST_CASE("DisplayLayer - reuses cached screen lines until the buffer changes") {
  TextBuffer buffer{u"abc\ndef\nghi"};
  DisplayLayer layer{&buffer, nullptr, default_params()};

  vector<const DisplayLayer::ScreenLine *> first, second;
  build_lines(layer, 0, 3, &first);
  build_lines(layer, 0, 3, &second);
  REQUIRE(first[1]->id == second[1]->id);
  // Edits free replaced lines, so remember the ids rather than the pointers.
  uint32_t first_id = second[0]->id, second_id = second[1]->id;

  layer.buffer_will_change(1);
  buffer.set_text_in_range({{1, 0}, {1, 3}}, u"xyz");
  auto change = layer.buffer_did_change(1, 0, 1, 3, 1, 3);
  REQUIRE(change.start == 1);
  REQUIRE(change.old_extent == 1);
  REQUIRE(change.new_extent == 1);

  vector<const DisplayLayer::ScreenLine *> third;
  REQUIRE(build_lines(layer, 0, 3, &third) == vector<u16string>({u"abc", u"xyz", u"ghi"}));
  REQUIRE(third[0]->id == first_id);
  REQUIRE(third[1]->id != second_id);
}

TEST_CASE("DisplayLayer - applies highlight segments") {
  TextBuffer buffer{u"abc"};
  DisplayLayer layer{&buffer, nullptr, default_params()};

  uint32_t end_buffer_row;
  auto requests = layer.prepare_screen_lines(0, 1, &end_buffer_row);
  REQUIRE(requests.size() == 1);
  REQUIRE(requests[0].start == Point(0, 0));

  DisplayLayer::HighlightSegment segment;
  segment.start = Point(0, 0);
  segment.next_position = Point(UINT32_MAX, UINT32_MAX);
  segment.boundaries.push_back({Point(0, 1), {}, {257}});
  segment.boundaries.push_back({Point(0, 2), {257}, {}});

  vector<const DisplayLayer::ScreenLine *> lines;
  REQUIRE(layer.build_screen_lines(0, 1, {segment}, true, &lines));
  REQUIRE(lines.size() == 1);
  REQUIRE(lines[0]->tags == vector<int32_t>({1, -257, 1, -258, 1}));
}
