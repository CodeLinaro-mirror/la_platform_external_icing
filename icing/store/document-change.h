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

#ifndef ICING_STORE_DOCUMENT_CHANGE_H_
#define ICING_STORE_DOCUMENT_CHANGE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "icing/text_classifier/lib3/utils/base/statusor.h"
#include "icing/proto/change_notification.pb.h"

namespace icing {
namespace lib {

// A single document change (add, update or delete), identified by the
// namespace, schema and uri of the changed document, and the time at which the
// change operation occurred.
//
// This is the common representation that every source of changes (e.g. the
// DocumentLog for adds and updates, and the DeletionStore for deletes) converts
// its changes into, so that changes from all sources can be merged into a
// single DocumentChangeResponseProto.
struct DocumentChange {
  std::string name_space;
  std::string schema;
  std::string uri;
  int64_t operation_timestamp_ms;

  bool operator==(const DocumentChange& other) const {
    return name_space == other.name_space && schema == other.schema &&
           uri == other.uri &&
           operation_timestamp_ms == other.operation_timestamp_ms;
  }
};

// A page of DocumentChanges retrieved from a single source, or merged from
// several sources.
//
// Invariants:
//   - changes is sorted by operation_timestamp_ms in non-decreasing order.
//   - next_change_timestamp_ms is set iff there are more changes after the ones
//     in changes. If set, every change not included in changes has an
//     operation_timestamp_ms greater than or equal to next_change_timestamp_ms,
//     which is itself greater than or equal to the operation_timestamp_ms of
//     every change in changes. changes is only empty while there are more
//     changes if the page size limit is 0.
//
// Pages strictly respect their size limit, so a page may end in the middle of
// changes that share the same operation_timestamp_ms. In that case
// next_change_timestamp_ms equals the last operation_timestamp_ms in changes,
// meaning that the page is only complete up to the timestamp before it. See
// BuildDocumentChangeResponse for how that affects the next page.
//
// TODO(b/585622362): Return more precise pagination state (e.g. indices) to
//   avoid re-returning changes.
struct DocumentChangeResult {
  // Changes in the result, sorted by operation_timestamp_ms in non-decreasing
  // order.
  std::vector<DocumentChange> changes;
  // If set, there are more changes, and none of them has an
  // operation_timestamp_ms smaller than this. When merged from multiple
  // sources, this is a lower bound rather than the timestamp of a known change.
  std::optional<int64_t> next_change_timestamp_ms;

  bool has_more_changes() const { return next_change_timestamp_ms.has_value(); }
};

// Truncates changes, which must be sorted by operation_timestamp_ms in
// non-decreasing order, to at most max_changes changes. If max_changes is 0,
// all changes are removed.
//
// Returns:
//   The operation_timestamp_ms of the first removed change, or nullopt if no
//     changes were removed, on success
//   INVALID_ARGUMENT if max_changes is negative.
//   FAILED_PRECONDITION if changes is null.
libtextclassifier3::StatusOr<std::optional<int64_t>> TruncateDocumentChanges(
    int max_changes, std::vector<DocumentChange>* changes);

// Merges DocumentChangeResults from multiple sources into a single
// DocumentChangeResult of at most max_changes changes that satisfies the
// DocumentChangeResult invariants.
//
// Each input result must satisfy the DocumentChangeResult invariants. An input
// result with more changes has only returned its changes up to its
// next_change_timestamp_ms, so the merged result only includes changes up to
// the smallest next_change_timestamp_ms across all inputs. The merged result is
// then truncated to max_changes with TruncateDocumentChanges.
//
// Returns:
//   The merged DocumentChangeResult on success
//   INVALID_ARGUMENT if max_changes is negative
libtextclassifier3::StatusOr<DocumentChangeResult> MergeDocumentChangeResults(
    std::vector<DocumentChangeResult> results, int max_changes);

// Builds a DocumentChangeResponseProto from result. The status of the response
// is not set.
//
// If changes come from multiple sources, result must be the output of
// MergeDocumentChangeResults rather than any single source's result. The merge
// cuts the page off at the smallest next_change_timestamp_ms across sources,
// so the next_page_token computed here never moves past changes that another
// source hasn't returned yet.
//
// Changes are grouped into DocumentChangeSetProtos by namespace and schema.
// If the same (namespace, schema, uri) appears multiple times in result, only
// the change with the largest operation_timestamp_ms is included.
//
// next_page_token.last_sync_timestamp_ms is the latest timestamp that the
// caller is fully synced up to after this page, i.e. every change at or before
// it has been returned in this page or an earlier one. The next call returns
// changes strictly after it.
//   - If there are no more changes, it is the largest operation_timestamp_ms in
//     result, or request_timestamp_ms if result has no changes.
//   - If there are more changes and the page ended between two timestamps, it
//     is the largest operation_timestamp_ms in result.
//   - If there are more changes and the page ended in the middle of changes
//     that share the same timestamp T, only changes up to T - 1 are complete,
//     so it is T - 1. The next page then returns all changes at T, including
//     the ones already returned in this page.
//   - Exception: if every change in the page is at T, returning T - 1 would
//     return the same page forever, so it is T instead, and the remaining
//     changes at T are skipped.
//   - TODO(b/585622362): Handle page breaks in the middle of a timestamp either
//     by returning an index token or by cutting off at the last whole set of
//     changes by timestamp.
DocumentChangeResponseProto BuildDocumentChangeResponse(
    const DocumentChangeResult& result, int64_t request_timestamp_ms);

}  // namespace lib
}  // namespace icing

#endif  // ICING_STORE_DOCUMENT_CHANGE_H_
