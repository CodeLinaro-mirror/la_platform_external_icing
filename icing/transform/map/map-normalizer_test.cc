// Copyright (C) 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <memory>
#include <string>

#include "icing/text_classifier/lib3/utils/base/status.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "icing/testing/common-matchers.h"
#include "icing/transform/normalizer-factory.h"
#include "icing/transform/normalizer-options.h"
#include "icing/transform/normalizer.h"
#include "icing/util/character-iterator.h"

namespace icing {
namespace lib {

namespace {

using ::testing::Eq;

TEST(MapNormalizerTest, Creation) {
  NormalizerOptions options1(/*max_term_byte_size=*/5);
  EXPECT_THAT(normalizer_factory::Create(options1), IsOk());

  NormalizerOptions options2(/*max_term_byte_size=*/0);
  EXPECT_THAT(normalizer_factory::Create(options2),
              StatusIs(libtextclassifier3::StatusCode::INVALID_ARGUMENT));

  NormalizerOptions options3(/*max_term_byte_size=*/-1);

  EXPECT_THAT(normalizer_factory::Create(options3),
              StatusIs(libtextclassifier3::StatusCode::INVALID_ARGUMENT));
}

// Strings that are already normalized won't change if normalized again.
TEST(MapNormalizerTest, AlreadyNormalized) {
  NormalizerOptions options(/*max_term_byte_size=*/1000);
  ICING_ASSERT_OK_AND_ASSIGN(auto normalizer,
                             normalizer_factory::Create(options));

  EXPECT_THAT(normalizer->NormalizeTerm("", /*is_query_term=*/false),
              EqualsNormalizedTerm(""));
  EXPECT_THAT(normalizer->NormalizeTerm("hello world", /*is_query_term=*/false),
              EqualsNormalizedTerm("hello world"));
  EXPECT_THAT(normalizer->NormalizeTerm("你好", /*is_query_term=*/false),
              EqualsNormalizedTerm("你好"));
  EXPECT_THAT(normalizer->NormalizeTerm("キャンパス", /*is_query_term=*/false),
              EqualsNormalizedTerm("キャンパス"));
  EXPECT_THAT(normalizer->NormalizeTerm("안녕하세요", /*is_query_term=*/false),
              EqualsNormalizedTerm("안녕하세요"));
}

TEST(MapNormalizerTest, UppercaseToLowercase) {
  NormalizerOptions options(/*max_term_byte_size=*/1000);
  ICING_ASSERT_OK_AND_ASSIGN(auto normalizer,
                             normalizer_factory::Create(options));

  EXPECT_THAT(normalizer->NormalizeTerm("MDI", /*is_query_term=*/false),
              EqualsNormalizedTerm("mdi"));
  EXPECT_THAT(normalizer->NormalizeTerm("Icing", /*is_query_term=*/false),
              EqualsNormalizedTerm("icing"));
}

TEST(MapNormalizerTest, LatinLetterRemoveAccent) {
  NormalizerOptions options(/*max_term_byte_size=*/1000);
  ICING_ASSERT_OK_AND_ASSIGN(auto normalizer,
                             normalizer_factory::Create(options));

  EXPECT_THAT(normalizer->NormalizeTerm("Zürich", /*is_query_term=*/false),
              EqualsNormalizedTerm("zurich"));
  EXPECT_THAT(normalizer->NormalizeTerm("après-midi", /*is_query_term=*/false),
              EqualsNormalizedTerm("apres-midi"));
  EXPECT_THAT(normalizer->NormalizeTerm("Buenos días", /*is_query_term=*/false),
              EqualsNormalizedTerm("buenos dias"));
  EXPECT_THAT(normalizer->NormalizeTerm("ÀÁÂÃÄÅĀĂĄḀḁàáâãäåāăą",
                                        /*is_query_term=*/false),
              EqualsNormalizedTerm("aaaaaaaaaaaaaaaaaaaa"));
  EXPECT_THAT(normalizer->NormalizeTerm("ḂḄḆḃḅḇ", /*is_query_term=*/false),
              EqualsNormalizedTerm("bbbbbb"));
  EXPECT_THAT(normalizer->NormalizeTerm("ÇĆĈĊČḈḉćĉċčç",
                                        /*is_query_term=*/false),
              EqualsNormalizedTerm("cccccccccccc"));
  EXPECT_THAT(normalizer->NormalizeTerm("ÐĎĐḊḌḎḐḒḋḍḏḑḓďđ",
                                        /*is_query_term=*/false),
              EqualsNormalizedTerm("ddddddddddddddd"));
  EXPECT_THAT(normalizer->NormalizeTerm("ÈÉÊËĒĔĖĘḔḖḘḚḜḕḗḙḛḝèéêëēĕėęě",
                                        /*is_query_term=*/false),
              EqualsNormalizedTerm("eeeeeeeeeeeeeeeeeeeeeeeeeee"));
  EXPECT_THAT(normalizer->NormalizeTerm("Ḟḟ", /*is_query_term=*/false),
              EqualsNormalizedTerm("ff"));
  EXPECT_THAT(normalizer->NormalizeTerm("ĜĞĠĢḠḡĝğġģ", /*is_query_term=*/false),
              EqualsNormalizedTerm("gggggggggg"));
  EXPECT_THAT(normalizer->NormalizeTerm("ĤḢḤḦḨḪḣḥḧḩḫĥẖ",
                                        /*is_query_term=*/false),
              EqualsNormalizedTerm("hhhhhhhhhhhhh"));
  EXPECT_THAT(normalizer->NormalizeTerm("ÌÍÎÏĨĪĬḬḭḯìíîïĩīĭ",
                                        /*is_query_term=*/false),
              EqualsNormalizedTerm("iiiiiiiiiiiiiiiii"));
  EXPECT_THAT(normalizer->NormalizeTerm("Ĵĵ", /*is_query_term=*/false),
              EqualsNormalizedTerm("jj"));
  EXPECT_THAT(normalizer->NormalizeTerm("ĶḰḲḴḵḱḳķ", /*is_query_term=*/false),
              EqualsNormalizedTerm("kkkkkkkk"));
  EXPECT_THAT(normalizer->NormalizeTerm("ĹĻĽḶḸḼḷḹḻḽĺļľ",
                                        /*is_query_term=*/false),
              EqualsNormalizedTerm("lllllllllllll"));
  EXPECT_THAT(normalizer->NormalizeTerm("ḾṀṂḿṁṃ", /*is_query_term=*/false),
              EqualsNormalizedTerm("mmmmmm"));
  EXPECT_THAT(normalizer->NormalizeTerm("ÑŃŅŇṄṆṈṊṅṇṉṋñńņň",
                                        /*is_query_term=*/false),
              EqualsNormalizedTerm("nnnnnnnnnnnnnnnn"));
  EXPECT_THAT(normalizer->NormalizeTerm("ŌŎŐÒÓÔÕÖṌṎṐṒṍṏṑṓòóôõöōŏő",
                                        /*is_query_term=*/false),
              EqualsNormalizedTerm("oooooooooooooooooooooooo"));
  EXPECT_THAT(normalizer->NormalizeTerm("ṔṖṕṗ", /*is_query_term=*/false),
              EqualsNormalizedTerm("pppp"));
  EXPECT_THAT(normalizer->NormalizeTerm("ŔŖŘṘṚṜṞṙṛṝṟŕŗř",
                                        /*is_query_term=*/false),
              EqualsNormalizedTerm("rrrrrrrrrrrrrr"));
  EXPECT_THAT(normalizer->NormalizeTerm("ŚŜŞŠȘṠṢṤṦṨṡṣṥṧṩșśŝşš",
                                        /*is_query_term=*/false),
              EqualsNormalizedTerm("ssssssssssssssssssss"));
  EXPECT_THAT(normalizer->NormalizeTerm("ŢŤȚṪṬṮṰṫṭṯṱțţť",
                                        /*is_query_term=*/false),
              EqualsNormalizedTerm("tttttttttttttt"));
  EXPECT_THAT(normalizer->NormalizeTerm("ŨŪŬÙÚÛÜṲṴṶṸṺṳṵṷṹṻùúûüũūŭ",
                                        /*is_query_term=*/false),
              EqualsNormalizedTerm("uuuuuuuuuuuuuuuuuuuuuuuu"));
  EXPECT_THAT(normalizer->NormalizeTerm("ṼṾṽṿ", /*is_query_term=*/false),
              EqualsNormalizedTerm("vvvv"));
  EXPECT_THAT(normalizer->NormalizeTerm("ŴẀẂẄẆẈẁẃẅẇẉŵ",
                                        /*is_query_term=*/false),
              EqualsNormalizedTerm("wwwwwwwwwwww"));
  EXPECT_THAT(normalizer->NormalizeTerm("ẊẌẋẍ", /*is_query_term=*/false),
              EqualsNormalizedTerm("xxxx"));
  EXPECT_THAT(normalizer->NormalizeTerm("ÝŶŸẎẏŷýÿ", /*is_query_term=*/false),
              EqualsNormalizedTerm("yyyyyyyy"));
  EXPECT_THAT(normalizer->NormalizeTerm("ŹŻŽẐẒẔẑẓẕźżž",
                                        /*is_query_term=*/false),
              EqualsNormalizedTerm("zzzzzzzzzzzz"));
}

// Accent / diacritic marks won't be removed in non-latin chars, e.g. in
// Japanese and Greek
TEST(MapNormalizerTest, NonLatinLetterNotRemoveAccent) {
  NormalizerOptions options(/*max_term_byte_size=*/1000);
  ICING_ASSERT_OK_AND_ASSIGN(auto normalizer,
                             normalizer_factory::Create(options));

  // Katakana
  EXPECT_THAT(normalizer->NormalizeTerm("ダヂヅデド", /*is_query_term=*/false),
              EqualsNormalizedTerm("ダヂヅデド"));
  // Greek
  EXPECT_THAT(normalizer->NormalizeTerm("kαλημέρα", /*is_query_term=*/false),
              EqualsNormalizedTerm("kαλημέρα"));
  EXPECT_THAT(normalizer->NormalizeTerm("εγγραφή", /*is_query_term=*/false),
              EqualsNormalizedTerm("εγγραφή"));
  // Hebrew
  EXPECT_THAT(normalizer->NormalizeTerm("אָלֶף־בֵּית עִבְרִי",
                                        /*is_query_term=*/false),
              EqualsNormalizedTerm("אָלֶף־בֵּית עִבְרִי"));
}

TEST(MapNormalizerTest, FullWidthCharsToASCII) {
  NormalizerOptions options(/*max_term_byte_size=*/1000);
  ICING_ASSERT_OK_AND_ASSIGN(auto normalizer,
                             normalizer_factory::Create(options));

  // Full-width punctuation to ASCII punctuation
  EXPECT_THAT(normalizer->NormalizeTerm("‘’．，！？：“”", /*is_query_term=*/false),
              EqualsNormalizedTerm("''.,!?:\"\""));
  // Full-width 0-9
  EXPECT_THAT(normalizer->NormalizeTerm("０１２３４５６７８９",
                                        /*is_query_term=*/false),
              EqualsNormalizedTerm("0123456789"));
  // Full-width A-Z
  EXPECT_THAT(normalizer->NormalizeTerm(
                  "ＡＢＣＤＥＦＧＨＩＪＫＬＭＮＯＰＱＲＳＴＵＶＷＸＹＺ",
                  /*is_query_term=*/false),
              EqualsNormalizedTerm("abcdefghijklmnopqrstuvwxyz"));
  // Full-width a-z
  EXPECT_THAT(normalizer->NormalizeTerm(
                  "ａｂｃｄｅｆｇｈｉｊｋｌｍｎｏｐｑｒｓｔｕｖｗｘｙｚ",
                  /*is_query_term=*/false),
              EqualsNormalizedTerm("abcdefghijklmnopqrstuvwxyz"));
}

TEST(MapNormalizerTest, IdeographicToASCII) {
  NormalizerOptions options(/*max_term_byte_size=*/1000);
  ICING_ASSERT_OK_AND_ASSIGN(auto normalizer,
                             normalizer_factory::Create(options));

  EXPECT_THAT(normalizer->NormalizeTerm("，。", /*is_query_term=*/false),
              EqualsNormalizedTerm(",."));
}

TEST(MapNormalizerTest, HiraganaToKatakana) {
  NormalizerOptions options(/*max_term_byte_size=*/1000);
  ICING_ASSERT_OK_AND_ASSIGN(auto normalizer,
                             normalizer_factory::Create(options));

  EXPECT_THAT(normalizer->NormalizeTerm("あいうえお", /*is_query_term=*/false),
              EqualsNormalizedTerm("アイウエオ"));
  EXPECT_THAT(normalizer->NormalizeTerm("かきくけこ", /*is_query_term=*/false),
              EqualsNormalizedTerm("カキクケコ"));
  EXPECT_THAT(normalizer->NormalizeTerm("さしすせそ", /*is_query_term=*/false),
              EqualsNormalizedTerm("サシスセソ"));
  EXPECT_THAT(normalizer->NormalizeTerm("たちつてと", /*is_query_term=*/false),
              EqualsNormalizedTerm("タチツテト"));
  EXPECT_THAT(normalizer->NormalizeTerm("なにぬねの", /*is_query_term=*/false),
              EqualsNormalizedTerm("ナニヌネノ"));
  EXPECT_THAT(normalizer->NormalizeTerm("はひふへほ", /*is_query_term=*/false),
              EqualsNormalizedTerm("ハヒフヘホ"));
  EXPECT_THAT(normalizer->NormalizeTerm("まみむめも", /*is_query_term=*/false),
              EqualsNormalizedTerm("マミムメモ"));
  EXPECT_THAT(normalizer->NormalizeTerm("やゆよ", /*is_query_term=*/false),
              EqualsNormalizedTerm("ヤユヨ"));
  EXPECT_THAT(normalizer->NormalizeTerm("らりるれろ", /*is_query_term=*/false),
              EqualsNormalizedTerm("ラリルレロ"));
  EXPECT_THAT(normalizer->NormalizeTerm("わゐゑを", /*is_query_term=*/false),
              EqualsNormalizedTerm("ワヰヱヲ"));
  EXPECT_THAT(normalizer->NormalizeTerm("ん", /*is_query_term=*/false),
              EqualsNormalizedTerm("ン"));
  EXPECT_THAT(normalizer->NormalizeTerm("がぎぐげご", /*is_query_term=*/false),
              EqualsNormalizedTerm("ガギグゲゴ"));
  EXPECT_THAT(normalizer->NormalizeTerm("ざじずぜぞ", /*is_query_term=*/false),
              EqualsNormalizedTerm("ザジズゼゾ"));
  EXPECT_THAT(normalizer->NormalizeTerm("だぢづでど", /*is_query_term=*/false),
              EqualsNormalizedTerm("ダヂヅデド"));
  EXPECT_THAT(normalizer->NormalizeTerm("ばびぶべぼ", /*is_query_term=*/false),
              EqualsNormalizedTerm("バビブベボ"));
  EXPECT_THAT(normalizer->NormalizeTerm("ぱぴぷぺぽ", /*is_query_term=*/false),
              EqualsNormalizedTerm("パピプペポ"));
}

TEST(MapNormalizerTest, Truncate) {
  {
    NormalizerOptions options(/*max_term_byte_size=*/5);
    ICING_ASSERT_OK_AND_ASSIGN(auto normalizer,
                               normalizer_factory::Create(options));

    // Won't be truncated
    EXPECT_THAT(normalizer->NormalizeTerm("hi", /*is_query_term=*/false),
                EqualsNormalizedTerm("hi"));
    EXPECT_THAT(normalizer->NormalizeTerm("hello", /*is_query_term=*/false),
                EqualsNormalizedTerm("hello"));

    // Truncated to length 5.
    EXPECT_THAT(normalizer->NormalizeTerm("hello!", /*is_query_term=*/false),
                EqualsNormalizedTerm("hello"));

    // Each Japanese character has 3 bytes, so truncating to length 5 results in
    // only 1 character.
    EXPECT_THAT(normalizer->NormalizeTerm("キャンパス", /*is_query_term=*/false),
                EqualsNormalizedTerm("キ"));

    // Each Greek character has 2 bytes, so truncating to length 5 results in 2
    // character.
    EXPECT_THAT(normalizer->NormalizeTerm("αβγδε", /*is_query_term=*/false),
                EqualsNormalizedTerm("αβ"));
  }

  {
    NormalizerOptions options(/*max_term_byte_size=*/2);
    ICING_ASSERT_OK_AND_ASSIGN(auto normalizer,
                               normalizer_factory::Create(options));
    // The Japanese character has 3 bytes, truncating it results in an empty
    // string.
    EXPECT_THAT(normalizer->NormalizeTerm("キ", /*is_query_term=*/false),
                EqualsNormalizedTerm(""));
  }
}

TEST(MapNormalizerTest, InvalidUtf8) {
  NormalizerOptions options(/*max_term_byte_size=*/1000);
  ICING_ASSERT_OK_AND_ASSIGN(auto normalizer,
                             normalizer_factory::Create(options));

  // "\xff" is an invalid UTF-8 character. MapNormalizer should skip it and
  // continue, resulting in an empty normalizer term if only "\xff" is passed,
  // or filtering it out of a larger string.
  EXPECT_THAT(normalizer->NormalizeTerm("\xff", /*is_query_term=*/false),
              EqualsNormalizedTerm(""));
  EXPECT_THAT(normalizer->NormalizeTerm("foo\xff"
                                        "bar",
                                        /*is_query_term=*/false),
              EqualsNormalizedTerm("foobar"));
}

TEST(MapNormalizerTest, PrefixMatchLength) {
  // Verify that FindNormalizedMatchEndPosition will properly find the length of
  // the prefix match when given a non-normalized term and a normalized term
  // is a prefix of the non-normalized one.
  NormalizerOptions options(/*max_term_byte_size=*/1000);
  ICING_ASSERT_OK_AND_ASSIGN(auto normalizer,
                             normalizer_factory::Create(options));

  // Upper to lower
  std::string term = "MDI";
  CharacterIterator match_end =
      normalizer->FindNormalizedMatchEndPosition(term, "md");
  EXPECT_THAT(term.substr(0, match_end.utf8_index()), Eq("MD"));

  term = "Icing";
  match_end = normalizer->FindNormalizedMatchEndPosition(term, "icin");
  EXPECT_THAT(term.substr(0, match_end.utf8_index()), Eq("Icin"));

  // Full-width
  term = "５２５６００";
  match_end = normalizer->FindNormalizedMatchEndPosition(term, "525");
  EXPECT_THAT(term.substr(0, match_end.utf8_index()), Eq("５２５"));

  term = "ＦＵＬＬＷＩＤＴＨ";
  match_end = normalizer->FindNormalizedMatchEndPosition(term, "full");
  EXPECT_THAT(term.substr(0, match_end.utf8_index()), Eq("ＦＵＬＬ"));

  // Hiragana to Katakana
  term = "あいうえお";
  match_end = normalizer->FindNormalizedMatchEndPosition(term, "アイ");
  EXPECT_THAT(term.substr(0, match_end.utf8_index()), Eq("あい"));

  term = "かきくけこ";
  match_end = normalizer->FindNormalizedMatchEndPosition(term, "カ");
  EXPECT_THAT(term.substr(0, match_end.utf8_index()), Eq("か"));

  // Latin accents
  term = "Zürich";
  match_end = normalizer->FindNormalizedMatchEndPosition(term, "zur");
  EXPECT_THAT(term.substr(0, match_end.utf8_index()), Eq("Zür"));

  term = "après-midi";
  match_end = normalizer->FindNormalizedMatchEndPosition(term, "apre");
  EXPECT_THAT(term.substr(0, match_end.utf8_index()), Eq("aprè"));

  term = "Buenos días";
  match_end = normalizer->FindNormalizedMatchEndPosition(term, "buenos di");
  EXPECT_THAT(term.substr(0, match_end.utf8_index()), Eq("Buenos dí"));
}

TEST(MapNormalizerTest, SharedPrefixMatchLength) {
  // Verify that FindNormalizedMatchEndPosition will properly find the length of
  // the prefix match when given a non-normalized term and a normalized term
  // that share a common prefix.
  NormalizerOptions options(/*max_term_byte_size=*/1000);
  ICING_ASSERT_OK_AND_ASSIGN(auto normalizer,
                             normalizer_factory::Create(options));

  // Upper to lower
  std::string term = "MDI";
  CharacterIterator match_end =
      normalizer->FindNormalizedMatchEndPosition(term, "mgm");
  EXPECT_THAT(term.substr(0, match_end.utf8_index()), Eq("M"));

  term = "Icing";
  match_end = normalizer->FindNormalizedMatchEndPosition(term, "icky");
  EXPECT_THAT(term.substr(0, match_end.utf8_index()), Eq("Ic"));

  // Full-width
  term = "５２５６００";
  match_end = normalizer->FindNormalizedMatchEndPosition(term, "525788");
  EXPECT_THAT(term.substr(0, match_end.utf8_index()), Eq("５２５"));

  term = "ＦＵＬＬＷＩＤＴＨ";
  match_end = normalizer->FindNormalizedMatchEndPosition(term, "fully");
  EXPECT_THAT(term.substr(0, match_end.utf8_index()), Eq("ＦＵＬＬ"));

  // Hiragana to Katakana
  term = "あいうえお";
  match_end = normalizer->FindNormalizedMatchEndPosition(term, "アイエオ");
  EXPECT_THAT(term.substr(0, match_end.utf8_index()), Eq("あい"));

  term = "かきくけこ";
  match_end = normalizer->FindNormalizedMatchEndPosition(term, "カケコ");
  EXPECT_THAT(term.substr(0, match_end.utf8_index()), Eq("か"));

  // Latin accents
  term = "Zürich";
  match_end = normalizer->FindNormalizedMatchEndPosition(term, "zurg");
  EXPECT_THAT(term.substr(0, match_end.utf8_index()), Eq("Zür"));

  term = "après-midi";
  match_end = normalizer->FindNormalizedMatchEndPosition(term, "apreciate");
  EXPECT_THAT(term.substr(0, match_end.utf8_index()), Eq("aprè"));

  term = "días";
  match_end = normalizer->FindNormalizedMatchEndPosition(term, "diamond");
  EXPECT_THAT(term.substr(0, match_end.utf8_index()), Eq("día"));
}

}  // namespace

}  // namespace lib
}  // namespace icing
