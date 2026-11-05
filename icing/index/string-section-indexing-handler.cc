// Copyright (C) 2022 Google LLC
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

#include "icing/index/string-section-indexing-handler.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "icing/text_classifier/lib3/utils/base/status.h"
#include "icing/text_classifier/lib3/utils/base/statusor.h"
#include "icing/absl_ports/canonical_errors.h"
#include "icing/absl_ports/str_cat.h"
#include "icing/absl_ports/str_join.h"
#include "icing/index/index.h"
#include "icing/proto/logging.pb.h"
#include "icing/proto/schema.pb.h"
#include "icing/proto/term.pb.h"
#include "icing/schema/section.h"
#include "icing/store/document-id.h"
#include "icing/transform/normalizer.h"
#include "icing/util/logging.h"
#include "icing/util/status-macros.h"
#include "icing/util/tokenized-document.h"

namespace icing {
namespace lib {

namespace {

// Generates Pinyin variant terms from a space-separated `pinyin_text` (e.g.
// "yi xin yi yi") and appends them in-place to `pinyin_terms`:
//   1. Full Pinyin term with spaces (e.g. "yi xin yi yi").
//   2. Continuous Pinyin without spaces (e.g. "yixinyiyi").
//   3. If `term_match_type` is `TermMatchType::PREFIX`, individual Pinyin
//      syllables (e.g. "yi", "xin") and cumulative syllable prefixes (e.g.
//      "yi", "yi xin", "yi xin yi") so syllable-boundary queries can match when
//      buffered with `TermMatchType::EXACT_ONLY`.
void GeneratePinyinTerms(std::string_view pinyin_text,
                         TermMatchType::Code term_match_type,
                         std::vector<std::string>* pinyin_terms) {
  std::unordered_set<std::string> seen;
  auto add_term = [&](std::string term) {
    if (!term.empty() && seen.insert(term).second) {
      pinyin_terms->push_back(std::move(term));
    }
  };

  // 1. Full Pinyin term with spaces (e.g. "mei tian").
  add_term(std::string(pinyin_text));

  std::vector<std::string_view> syllables =
      absl_ports::StrSplit(pinyin_text, " ");

  // 2. Continuous Pinyin without spaces (e.g. "meitian").
  add_term(absl_ports::StrCatPieces(syllables));

  // 3. Individual Pinyin syllables (e.g. "mei", "tian") and cumulative syllable
  // prefixes (e.g. "yi", "yi xin", "yi xin yi" for "yi xin yi yi"), added only
  // for PREFIX sections.
  if (term_match_type == TermMatchType::PREFIX) {
    for (std::string_view syllable : syllables) {
      add_term(std::string(syllable));
    }
    std::string prefix;
    for (size_t i = 0; i + 1 < syllables.size(); ++i) {
      if (syllables[i].empty()) {
        continue;
      }
      if (prefix.empty()) {
        prefix = std::string(syllables[i]);
      } else {
        absl_ports::StrAppend(&prefix, " ", syllables[i]);
      }
      add_term(prefix);
    }
  }
}

}  // namespace

/* static */ libtextclassifier3::StatusOr<
    std::unique_ptr<StringSectionIndexingHandler>>
StringSectionIndexingHandler::Create(const Normalizer* normalizer,
                                     Index* index) {
  ICING_RETURN_ERROR_IF_NULL(normalizer);
  ICING_RETURN_ERROR_IF_NULL(index);

  return std::unique_ptr<StringSectionIndexingHandler>(
      new StringSectionIndexingHandler(normalizer, index));
}

libtextclassifier3::Status StringSectionIndexingHandler::Handle(
    const TokenizedDocument& tokenized_document, DocumentId document_id,
    DocumentId /*old_document_id*/ _,
    PutDocumentStatsProto* put_document_stats) {
  uint32_t num_tokens = 0;
  libtextclassifier3::Status status;
  for (const TokenizedSection& section :
       tokenized_document.tokenized_string_sections()) {
    if (section.metadata.tokenizer ==
        StringIndexingConfig::TokenizerType::NONE) {
      ICING_LOG(WARNING)
          << "Unexpected TokenizerType::NONE found when indexing document.";
    }
    // TODO(b/152934343): pass real namespace ids in
    Index::Editor editor =
        index_.Edit(document_id, section.metadata.id, /*namespace_id=*/0);
    for (std::string_view token : section.token_sequence) {
      ++num_tokens;

      switch (section.metadata.tokenizer) {
        case StringIndexingConfig::TokenizerType::VERBATIM:
          status = editor.BufferTerm(token, section.metadata.term_match_type);
          break;
        case StringIndexingConfig::TokenizerType::NONE:
          [[fallthrough]];
        case StringIndexingConfig::TokenizerType::RFC822:
          [[fallthrough]];
        case StringIndexingConfig::TokenizerType::URL:
          [[fallthrough]];
        case StringIndexingConfig::TokenizerType::PLAIN:
          status =
              BufferPlainToken(editor, token, section.metadata.term_match_type);
          break;
      }

      if (absl_ports::IsInvalidArgument(status)) {
        ICING_LOG(WARNING) << "Failed to add an invalid token to the index: "
                           << status.error_message() << ". Skip this token.";
        status = libtextclassifier3::Status::OK;
      }

      if (!status.ok()) {
        // We've encountered a failure. Bail out. We'll mark this doc as deleted
        // and signal a failure to the client.
        ICING_LOG(WARNING) << "Failed to buffer term in lite lexicon due to: "
                           << status.error_message();
        break;
      }
    }
    if (!status.ok()) {
      break;
    }
    // Add all the seen terms to the index with their term frequency.
    status = editor.IndexAllBufferedTerms();
    if (!status.ok()) {
      ICING_LOG(WARNING) << "Failed to add hits in lite index due to: "
                         << status.error_message();
      break;
    }
  }

  if (put_document_stats != nullptr) {
    put_document_stats->mutable_tokenization_stats()->set_num_tokens_indexed(
        num_tokens);
  }

  return status;
}

libtextclassifier3::Status StringSectionIndexingHandler::BufferPlainToken(
    Index::Editor& editor, std::string_view token,
    TermMatchType::Code term_match_type) {
  Normalizer::NormalizedTerm normalized_term =
      normalizer_.NormalizeTerm(token, /*is_query_term=*/false);
  ICING_RETURN_IF_ERROR(
      editor.BufferTerm(normalized_term.text, term_match_type));
  if (normalized_term.pinyin_term.has_value()) {
    std::vector<std::string> pinyin_terms;
    GeneratePinyinTerms(*normalized_term.pinyin_term, term_match_type,
                        &pinyin_terms);
    for (const std::string& pinyin_term : pinyin_terms) {
      ICING_RETURN_IF_ERROR(
          editor.BufferTerm(pinyin_term, TermMatchType::EXACT_ONLY));
    }
  }
  return libtextclassifier3::Status::OK;
}

}  // namespace lib
}  // namespace icing
