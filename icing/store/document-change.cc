// Copyright (C) 2026 Google LLC
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

#include "icing/store/document-change.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "icing/text_classifier/lib3/utils/base/statusor.h"
#include "icing/absl_ports/canonical_errors.h"
#include "icing/legacy/core/icing-string-util.h"
#include "icing/proto/change_notification.pb.h"
#include "icing/util/status-macros.h"

namespace icing {
namespace lib {

namespace {

// Returns the latest timestamp that the caller is fully synced up to after the
// page in result, to be used as next_page_token.last_sync_timestamp_ms. See
// BuildDocumentChangeResponse for details. max_timestamp_ms is the largest of
// the request timestamp and every operation timestamp in result.
int64_t GetNextPageTimestampMs(const DocumentChangeResult& result,
                               int64_t max_timestamp_ms) {
  if (!result.has_more_changes() || result.changes.empty()) {
    // No more changes, so every change up to the latest one is synced.
    return max_timestamp_ms;
  }
  int64_t last_timestamp_ms = result.changes.back().operation_timestamp_ms;
  if (*result.next_change_timestamp_ms > last_timestamp_ms) {
    // The page ended between two timestamps, so every change up to the last one
    // in the page is synced.
    return last_timestamp_ms;
  }
  if (result.changes.front().operation_timestamp_ms == last_timestamp_ms) {
    // Every change in the page shares the timestamp of the remaining changes,
    // so returning last_timestamp_ms - 1 would return the same page forever.
    // Skip the remaining changes at this timestamp instead.
    // TODO(b/585622362): Avoid skipping changes once the pagination token can
    //   carry more precise state.
    return last_timestamp_ms;
  }
  // The page ended in the middle of changes at last_timestamp_ms, so changes
  // are only fully synced up to the timestamp before it. The next page returns
  // all changes at last_timestamp_ms, including ones already in this page.
  return last_timestamp_ms - 1;
}

}  // namespace

libtextclassifier3::StatusOr<std::optional<int64_t>> TruncateDocumentChanges(
    int max_changes, std::vector<DocumentChange>* changes) {
  if (max_changes < 0) {
    return absl_ports::InvalidArgumentError(IcingStringUtil::StringPrintf(
        "max_changes must be non-negative, but was %d", max_changes));
  }
  ICING_RETURN_ERROR_IF_NULL(changes);
  if (changes->size() <= static_cast<size_t>(max_changes)) {
    return std::nullopt;
  }
  int64_t first_removed_timestamp_ms =
      (*changes)[max_changes].operation_timestamp_ms;
  changes->resize(max_changes);
  return first_removed_timestamp_ms;
}

libtextclassifier3::StatusOr<DocumentChangeResult> MergeDocumentChangeResults(
    std::vector<DocumentChangeResult> results, int max_changes) {
  if (max_changes < 0) {
    return absl_ports::InvalidArgumentError(IcingStringUtil::StringPrintf(
        "max_changes must be non-negative, but was %d", max_changes));
  }

  // An input with more changes has only returned its changes up to its
  // next_change_timestamp_ms, so only merge changes up to the smallest such
  // timestamp. Any change after it is left for the next page.
  std::optional<int64_t> next_change_timestamp_ms;
  size_t total_changes = 0;
  for (const DocumentChangeResult& result : results) {
    total_changes += result.changes.size();
    if (result.next_change_timestamp_ms.has_value()) {
      next_change_timestamp_ms =
          next_change_timestamp_ms.has_value()
              ? std::min(*next_change_timestamp_ms,
                         *result.next_change_timestamp_ms)
              : *result.next_change_timestamp_ms;
    }
  }

  DocumentChangeResult merged;
  merged.changes.reserve(total_changes);
  for (DocumentChangeResult& result : results) {
    for (DocumentChange& change : result.changes) {
      if (!next_change_timestamp_ms.has_value() ||
          change.operation_timestamp_ms <= *next_change_timestamp_ms) {
        merged.changes.push_back(std::move(change));
      }
    }
  }
  std::stable_sort(merged.changes.begin(), merged.changes.end(),
                   [](const DocumentChange& lhs, const DocumentChange& rhs) {
                     return lhs.operation_timestamp_ms <
                            rhs.operation_timestamp_ms;
                   });

  ICING_ASSIGN_OR_RETURN(std::optional<int64_t> first_truncated_timestamp_ms,
                         TruncateDocumentChanges(max_changes, &merged.changes));
  if (first_truncated_timestamp_ms.has_value()) {
    // Every truncated change is at or before next_change_timestamp_ms.
    next_change_timestamp_ms = first_truncated_timestamp_ms;
  }
  merged.next_change_timestamp_ms = next_change_timestamp_ms;
  return merged;
}

DocumentChangeResponseProto BuildDocumentChangeResponse(
    const DocumentChangeResult& result, int64_t request_timestamp_ms) {
  // Groups changes by (namespace, schema), then by uri, keeping only the
  // latest change for each uri. std::map keeps the output order deterministic.
  std::map<std::pair<std::string, std::string>,
           std::unordered_map<std::string, int64_t>>
      grouped_changes;
  int64_t last_sync_timestamp_ms = request_timestamp_ms;
  for (const DocumentChange& change : result.changes) {
    auto [itr, inserted] =
        grouped_changes[{change.name_space, change.schema}].insert(
            {change.uri, change.operation_timestamp_ms});
    if (!inserted) {
      itr->second = std::max(itr->second, change.operation_timestamp_ms);
    }
    last_sync_timestamp_ms =
        std::max(last_sync_timestamp_ms, change.operation_timestamp_ms);
  }

  DocumentChangeResponseProto response;
  for (const auto& [key, uri_to_timestamp] : grouped_changes) {
    DocumentChangeResponseProto::DocumentChangeSetProto* change_set =
        response.add_document_change_sets();
    change_set->set_name_space(key.first);
    change_set->set_schema(key.second);

    // Order change tokens by timestamp, then by uri for determinism.
    std::vector<std::pair<int64_t, std::string>> tokens;
    tokens.reserve(uri_to_timestamp.size());
    for (const auto& [uri, timestamp_ms] : uri_to_timestamp) {
      tokens.push_back({timestamp_ms, uri});
    }
    std::sort(tokens.begin(), tokens.end());
    for (auto& [timestamp_ms, uri] : tokens) {
      DocumentChangeResponseProto::DocumentChangeSetProto::ChangeToken* token =
          change_set->add_change_tokens();
      token->set_uri(std::move(uri));
      token->set_operation_timestamp_ms(timestamp_ms);
    }
  }
  response.set_has_more_changes(result.has_more_changes());
  response.mutable_next_page_token()->set_last_sync_timestamp_ms(
      GetNextPageTimestampMs(result, last_sync_timestamp_ms));
  return response;
}

}  // namespace lib
}  // namespace icing
