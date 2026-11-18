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

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "icing/text_classifier/lib3/utils/base/status.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "icing/portable/equals-proto.h"
#include "icing/proto/change_notification.pb.h"
#include "icing/testing/common-matchers.h"

namespace icing {
namespace lib {

namespace {

using ::icing::lib::portable_equals_proto::EqualsProto;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::IsTrue;
using ::testing::Optional;

DocumentChange MakeChange(std::string uri, int64_t operation_timestamp_ms,
                          std::string name_space = "namespace",
                          std::string schema = "schema") {
  return DocumentChange{.name_space = std::move(name_space),
                        .schema = std::move(schema),
                        .uri = std::move(uri),
                        .operation_timestamp_ms = operation_timestamp_ms};
}

TEST(DocumentChangeTest, TruncateDocumentChanges_noTruncation) {
  std::vector<DocumentChange> changes = {MakeChange("a", 1),
                                         MakeChange("b", 2)};
  EXPECT_THAT(TruncateDocumentChanges(/*max_changes=*/2, &changes),
              IsOkAndHolds(Eq(std::nullopt)));
  EXPECT_THAT(changes, ElementsAre(MakeChange("a", 1), MakeChange("b", 2)));

  EXPECT_THAT(TruncateDocumentChanges(/*max_changes=*/5, &changes),
              IsOkAndHolds(Eq(std::nullopt)));
  EXPECT_THAT(changes, ElementsAre(MakeChange("a", 1), MakeChange("b", 2)));

  std::vector<DocumentChange> empty;
  EXPECT_THAT(TruncateDocumentChanges(/*max_changes=*/1, &empty),
              IsOkAndHolds(Eq(std::nullopt)));
  EXPECT_THAT(empty, IsEmpty());
}

TEST(DocumentChangeTest, TruncateDocumentChanges_truncates) {
  std::vector<DocumentChange> changes = {MakeChange("a", 1), MakeChange("b", 2),
                                         MakeChange("c", 3)};
  EXPECT_THAT(TruncateDocumentChanges(/*max_changes=*/2, &changes),
              IsOkAndHolds(Optional(3)));
  EXPECT_THAT(changes, ElementsAre(MakeChange("a", 1), MakeChange("b", 2)));
}

TEST(DocumentChangeTest, TruncateDocumentChanges_splitsTiedTimestamps) {
  std::vector<DocumentChange> changes = {MakeChange("a", 1), MakeChange("b", 2),
                                         MakeChange("c", 2),
                                         MakeChange("d", 3)};
  // The page size is strictly respected, even though "c" shares the timestamp
  // of "b".
  EXPECT_THAT(TruncateDocumentChanges(/*max_changes=*/2, &changes),
              IsOkAndHolds(Optional(2)));
  EXPECT_THAT(changes, ElementsAre(MakeChange("a", 1), MakeChange("b", 2)));
}

TEST(DocumentChangeTest, TruncateDocumentChanges_zeroMaxRemovesAll) {
  std::vector<DocumentChange> changes = {MakeChange("a", 1),
                                         MakeChange("b", 2)};
  EXPECT_THAT(TruncateDocumentChanges(/*max_changes=*/0, &changes),
              IsOkAndHolds(Optional(1)));
  EXPECT_THAT(changes, IsEmpty());

  std::vector<DocumentChange> empty;
  EXPECT_THAT(TruncateDocumentChanges(/*max_changes=*/0, &empty),
              IsOkAndHolds(Eq(std::nullopt)));
}

TEST(DocumentChangeTest, TruncateDocumentChanges_negativeMaxIsInvalid) {
  std::vector<DocumentChange> changes = {MakeChange("a", 1)};
  EXPECT_THAT(TruncateDocumentChanges(/*max_changes=*/-1, &changes),
              StatusIs(libtextclassifier3::StatusCode::INVALID_ARGUMENT));
  EXPECT_THAT(changes, ElementsAre(MakeChange("a", 1)));
}

TEST(DocumentChangeTest, MergeDocumentChangeResults_empty) {
  ICING_ASSERT_OK_AND_ASSIGN(
      DocumentChangeResult merged,
      MergeDocumentChangeResults({}, /*max_changes=*/10));
  EXPECT_THAT(merged.changes, IsEmpty());
  EXPECT_THAT(merged.next_change_timestamp_ms, Eq(std::nullopt));

  ICING_ASSERT_OK_AND_ASSIGN(
      merged, MergeDocumentChangeResults(
                  {DocumentChangeResult(), DocumentChangeResult()},
                  /*max_changes=*/10));
  EXPECT_THAT(merged.changes, IsEmpty());
  EXPECT_THAT(merged.next_change_timestamp_ms, Eq(std::nullopt));
}

TEST(DocumentChangeTest, MergeDocumentChangeResults_singleResult) {
  DocumentChangeResult result;
  result.changes = {MakeChange("a", 1), MakeChange("b", 2)};
  result.next_change_timestamp_ms = 3;

  ICING_ASSERT_OK_AND_ASSIGN(
      DocumentChangeResult merged,
      MergeDocumentChangeResults({result}, /*max_changes=*/10));
  EXPECT_THAT(merged.changes,
              ElementsAre(MakeChange("a", 1), MakeChange("b", 2)));
  EXPECT_THAT(merged.next_change_timestamp_ms, Optional(3));
}

TEST(DocumentChangeTest, MergeDocumentChangeResults_interleavesByTimestamp) {
  DocumentChangeResult added;
  added.changes = {MakeChange("a", 1), MakeChange("c", 3)};
  DocumentChangeResult deleted;
  deleted.changes = {MakeChange("b", 2), MakeChange("d", 4)};

  ICING_ASSERT_OK_AND_ASSIGN(
      DocumentChangeResult merged,
      MergeDocumentChangeResults({added, deleted}, /*max_changes=*/10));
  EXPECT_THAT(merged.changes,
              ElementsAre(MakeChange("a", 1), MakeChange("b", 2),
                          MakeChange("c", 3), MakeChange("d", 4)));
  EXPECT_THAT(merged.next_change_timestamp_ms, Eq(std::nullopt));
}

TEST(DocumentChangeTest, MergeDocumentChangeResults_truncatesToMaxChanges) {
  DocumentChangeResult added;
  added.changes = {MakeChange("a", 1), MakeChange("c", 3)};
  DocumentChangeResult deleted;
  deleted.changes = {MakeChange("b", 2), MakeChange("d", 4)};

  ICING_ASSERT_OK_AND_ASSIGN(
      DocumentChangeResult merged,
      MergeDocumentChangeResults({added, deleted}, /*max_changes=*/3));
  EXPECT_THAT(
      merged.changes,
      ElementsAre(MakeChange("a", 1), MakeChange("b", 2), MakeChange("c", 3)));
  EXPECT_THAT(merged.next_change_timestamp_ms, Optional(4));
}

TEST(DocumentChangeTest,
     MergeDocumentChangeResults_cutsOffAtNextChangeOfIncompleteResult) {
  // added's remaining changes are at timestamp 2 or later.
  DocumentChangeResult added;
  added.changes = {MakeChange("a", 1), MakeChange("b", 2)};
  added.next_change_timestamp_ms = 2;
  // All of deleted's changes are at or after added's next_change_timestamp_ms.
  DocumentChangeResult deleted;
  deleted.changes = {MakeChange("c", 2), MakeChange("d", 4),
                     MakeChange("e", 5)};

  ICING_ASSERT_OK_AND_ASSIGN(
      DocumentChangeResult merged,
      MergeDocumentChangeResults({added, deleted}, /*max_changes=*/10));
  // The merged result has no changes after timestamp 2
  EXPECT_THAT(
      merged.changes,
      ElementsAre(MakeChange("a", 1), MakeChange("b", 2), MakeChange("c", 2)));
  // There might be more changes at or after timestamp 2 in added, so the
  // next_change_timestamp_ms is set to 2.
  EXPECT_THAT(merged.next_change_timestamp_ms, Optional(2));
}

TEST(DocumentChangeTest,
     MergeDocumentChangeResults_cutsOffAtSmallestIncompleteResult) {
  DocumentChangeResult added;
  added.changes = {MakeChange("a", 1), MakeChange("b", 5)};
  added.next_change_timestamp_ms = 6;
  DocumentChangeResult deleted;
  deleted.changes = {MakeChange("c", 2), MakeChange("d", 3)};
  deleted.next_change_timestamp_ms = 4;

  ICING_ASSERT_OK_AND_ASSIGN(
      DocumentChangeResult merged,
      MergeDocumentChangeResults({added, deleted}, /*max_changes=*/10));
  EXPECT_THAT(
      merged.changes,
      ElementsAre(MakeChange("a", 1), MakeChange("c", 2), MakeChange("d", 3)));
  EXPECT_THAT(merged.next_change_timestamp_ms, Optional(4));
}

TEST(DocumentChangeTest,
     MergeDocumentChangeResults_truncationBeforeIncompleteResultCutoff) {
  DocumentChangeResult added;
  added.changes = {MakeChange("a", 1), MakeChange("b", 2), MakeChange("c", 3)};
  added.next_change_timestamp_ms = 4;

  ICING_ASSERT_OK_AND_ASSIGN(
      DocumentChangeResult merged,
      MergeDocumentChangeResults({added}, /*max_changes=*/2));
  EXPECT_THAT(merged.changes,
              ElementsAre(MakeChange("a", 1), MakeChange("b", 2)));
  EXPECT_THAT(merged.next_change_timestamp_ms, Optional(3));
}

TEST(DocumentChangeTest, MergeDocumentChangeResults_zeroMaxReturnsNoChanges) {
  DocumentChangeResult added;
  added.changes = {MakeChange("a", 1), MakeChange("b", 2)};

  ICING_ASSERT_OK_AND_ASSIGN(
      DocumentChangeResult merged,
      MergeDocumentChangeResults({added}, /*max_changes=*/0));
  EXPECT_THAT(merged.changes, IsEmpty());
  EXPECT_THAT(merged.next_change_timestamp_ms, Optional(1));
}

TEST(DocumentChangeTest, MergeDocumentChangeResults_negativeMaxIsInvalid) {
  EXPECT_THAT(MergeDocumentChangeResults({}, /*max_changes=*/-1),
              StatusIs(libtextclassifier3::StatusCode::INVALID_ARGUMENT));
}

TEST(DocumentChangeTest,
     BuildDocumentChangeResponse_nextPageTokenWhenPageEndsBetweenTimestamps) {
  // The remaining changes are all after 102, so every change up to 102 has
  // been returned.
  DocumentChangeResult result;
  result.changes = {MakeChange("a", 101), MakeChange("b", 102)};
  result.next_change_timestamp_ms = 103;

  DocumentChangeResponseProto response =
      BuildDocumentChangeResponse(result, /*request_timestamp_ms=*/100);
  EXPECT_THAT(response.has_more_changes(), IsTrue());
  EXPECT_THAT(response.next_page_token().last_sync_timestamp_ms(), Eq(102));
}

TEST(DocumentChangeTest,
     BuildDocumentChangeResponse_nextPageTokenWhenTimestampIsSplit) {
  // There are more changes at 102 that aren't in this page, so the caller is
  // only fully synced up to 101. The next page returns everything after 101,
  // i.e. all changes at 102, including "b" again.
  DocumentChangeResult result;
  result.changes = {MakeChange("a", 101), MakeChange("b", 102)};
  result.next_change_timestamp_ms = 102;

  DocumentChangeResponseProto response =
      BuildDocumentChangeResponse(result, /*request_timestamp_ms=*/100);
  EXPECT_THAT(response.has_more_changes(), IsTrue());
  EXPECT_THAT(response.next_page_token().last_sync_timestamp_ms(), Eq(101));
}

TEST(
    DocumentChangeTest,
    BuildDocumentChangeResponse_nextPageTokenProgressesWhenPageIsOneTimestamp) {
  // There are more changes at 102, but the caller can't be fully synced up to
  // 101 without returning this same page forever. So the token moves to 102,
  // skipping the remaining changes at 102 (b/585622362).
  DocumentChangeResult result;
  result.changes = {MakeChange("a", 102), MakeChange("b", 102)};
  result.next_change_timestamp_ms = 102;

  DocumentChangeResponseProto response =
      BuildDocumentChangeResponse(result, /*request_timestamp_ms=*/100);
  EXPECT_THAT(response.has_more_changes(), IsTrue());
  EXPECT_THAT(response.next_page_token().last_sync_timestamp_ms(), Eq(102));
}

TEST(DocumentChangeTest, BuildDocumentChangeResponse_empty) {
  DocumentChangeResponseProto expected;
  expected.set_has_more_changes(false);
  expected.mutable_next_page_token()->set_last_sync_timestamp_ms(100);

  EXPECT_THAT(BuildDocumentChangeResponse(DocumentChangeResult(),
                                          /*request_timestamp_ms=*/100),
              EqualsProto(expected));
}

TEST(DocumentChangeTest, BuildDocumentChangeResponse_groupsByNamespaceSchema) {
  DocumentChangeResult result;
  result.changes = {MakeChange("uri1", 101, "ns1", "schema1"),
                    MakeChange("uri2", 102, "ns2", "schema1"),
                    MakeChange("uri3", 103, "ns1", "schema2"),
                    MakeChange("uri4", 104, "ns1", "schema1")};
  result.next_change_timestamp_ms = 105;

  DocumentChangeResponseProto expected;
  DocumentChangeResponseProto::DocumentChangeSetProto* ns1_schema1 =
      expected.add_document_change_sets();
  ns1_schema1->set_name_space("ns1");
  ns1_schema1->set_schema("schema1");
  auto* token = ns1_schema1->add_change_tokens();
  token->set_uri("uri1");
  token->set_operation_timestamp_ms(101);
  token = ns1_schema1->add_change_tokens();
  token->set_uri("uri4");
  token->set_operation_timestamp_ms(104);

  DocumentChangeResponseProto::DocumentChangeSetProto* ns1_schema2 =
      expected.add_document_change_sets();
  ns1_schema2->set_name_space("ns1");
  ns1_schema2->set_schema("schema2");
  token = ns1_schema2->add_change_tokens();
  token->set_uri("uri3");
  token->set_operation_timestamp_ms(103);

  DocumentChangeResponseProto::DocumentChangeSetProto* ns2_schema1 =
      expected.add_document_change_sets();
  ns2_schema1->set_name_space("ns2");
  ns2_schema1->set_schema("schema1");
  token = ns2_schema1->add_change_tokens();
  token->set_uri("uri2");
  token->set_operation_timestamp_ms(102);

  expected.set_has_more_changes(true);
  expected.mutable_next_page_token()->set_last_sync_timestamp_ms(104);

  EXPECT_THAT(BuildDocumentChangeResponse(result, /*request_timestamp_ms=*/100),
              EqualsProto(expected));
}

TEST(DocumentChangeTest, BuildDocumentChangeResponse_dedupesUriKeepsLatest) {
  // e.g. a document that was deleted (from the DeletionStore) and then re-added
  // (from the DocumentLog) after the request timestamp.
  DocumentChangeResult result;
  result.changes = {MakeChange("uri", 101), MakeChange("other", 102),
                    MakeChange("uri", 103)};

  DocumentChangeResponseProto expected;
  DocumentChangeResponseProto::DocumentChangeSetProto* change_set =
      expected.add_document_change_sets();
  change_set->set_name_space("namespace");
  change_set->set_schema("schema");
  auto* token = change_set->add_change_tokens();
  token->set_uri("other");
  token->set_operation_timestamp_ms(102);
  token = change_set->add_change_tokens();
  token->set_uri("uri");
  token->set_operation_timestamp_ms(103);
  expected.set_has_more_changes(false);
  expected.mutable_next_page_token()->set_last_sync_timestamp_ms(103);

  EXPECT_THAT(BuildDocumentChangeResponse(result, /*request_timestamp_ms=*/100),
              EqualsProto(expected));
}

}  // namespace

}  // namespace lib
}  // namespace icing
