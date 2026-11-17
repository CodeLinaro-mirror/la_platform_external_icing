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

#include "icing/store/deletion-store.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "icing/text_classifier/lib3/utils/base/status.h"
#include "icing/text_classifier/lib3/utils/base/statusor.h"
#include "icing/text_classifier/lib3/utils/hash/farmhash.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "icing/absl_ports/str_cat.h"
#include "icing/file/file-backed-vector.h"
#include "icing/file/filesystem.h"
#include "icing/file/memory-mapped-file.h"
#include "icing/file/mock-filesystem.h"
#include "icing/file/portable-file-backed-proto-log.h"
#include "icing/portable/equals-proto.h"
#include "icing/portable/gzip_stream.h"
#include "icing/proto/deletion.pb.h"
#include "icing/store/deletion-index-entry.h"
#include "icing/store/deletion-log-creator.h"
#include "icing/testing/common-matchers.h"
#include "icing/testing/tmp-directory.h"
#include "icing/util/data-loss.h"

namespace icing {
namespace lib {

namespace {

using ::icing::lib::portable_equals_proto::EqualsProto;
using ::testing::AnyNumber;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::IsFalse;
using ::testing::IsTrue;
using ::testing::NotNull;
using ::testing::Return;
using ::testing::StrEq;

// DeletionIndexEntries store timestamps in minutes, so tests use timestamps
// that are multiples of (or offsets within) a minute to avoid truncating them
// all to 0.
constexpr int64_t kMillisPerMinute = 60 * 1000;

DeletionEvent CreateDeletionEvent(const std::string& uri,
                                  const std::string& schema,
                                  int64_t deletion_timestamp_ms,
                                  int64_t update_timestamp_ms) {
  DeletionEvent deletion_event;
  deletion_event.set_name_space("namespace");
  deletion_event.set_uri(uri);
  deletion_event.set_schema(schema);
  deletion_event.set_deletion_timestamp_ms(deletion_timestamp_ms);
  deletion_event.set_last_update_timestamp_ms(update_timestamp_ms);
  return deletion_event;
}

DeletionIndexEntry ExpectedDeletionIndexEntry(
    const DeletionEvent& deletion_event, int64_t offset) {
  return DeletionIndexEntry(
      offset,
      static_cast<int32_t>(deletion_event.deletion_timestamp_ms() /
                           (60 * 1000)),
      static_cast<int32_t>(deletion_event.last_update_timestamp_ms() /
                           (60 * 1000)),
      tc3farmhash::Fingerprint32(deletion_event.schema()));
}

class DeletionStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    base_dir_ = GetTestTempDir() + "/deletion_store_test";
    filesystem_.DeleteDirectoryRecursively(base_dir_.c_str());
    ASSERT_TRUE(filesystem_.CreateDirectoryRecursively(base_dir_.c_str()));
  }

  void TearDown() override {
    filesystem_.DeleteDirectoryRecursively(base_dir_.c_str());
    filesystem_.DeleteDirectoryRecursively(OptimizeTempDir().c_str());
    filesystem_.DeleteDirectoryRecursively(
        absl_ports::StrCat(OptimizeTempDir(), ".tmp").c_str());
  }

  // The temporary directory that Optimize builds the new DeletionStore in.
  std::string OptimizeTempDir() const {
    return absl_ports::StrCat(base_dir_, "_optimize_tmp");
  }

  // Returns a SwapFiles action that does the first two renames of
  // Filesystem::SwapFiles, then fails as if the last rename failed. This leaves
  // the old files in `one` and no directory at `two`.
  auto PartialSwapAction() {
    return [this](const char* one, const char* two) {
      std::string tmp = absl_ports::StrCat(one, ".tmp");
      EXPECT_TRUE(filesystem_.RenameFile(one, tmp.c_str()));
      EXPECT_TRUE(filesystem_.RenameFile(two, one));
      return false;
    };
  }

  libtextclassifier3::StatusOr<DeletionStore::CreateResult> CreateStore() {
    return DeletionStore::Create(&filesystem_, base_dir_, kCompressionLevel,
                                 kCompressionThresholdBytes,
                                 protobuf_ports::kDefaultMemLevel);
  }

  std::string DeletionIndexPath() const {
    return absl_ports::StrCat(base_dir_, "/",
                              DeletionStore::GetDeletionIndexFilename());
  }

  // Writes the DeletionEvents directly into the DeletionLog, bypassing the
  // DeletionStore, and returns their offsets.
  libtextclassifier3::StatusOr<std::vector<int64_t>> WriteToDeletionLog(
      const std::vector<DeletionEvent>& deletion_events) {
    ICING_ASSIGN_OR_RETURN(
        DeletionLogCreator::CreateResult create_result,
        DeletionLogCreator::Create(&filesystem_, base_dir_, kCompressionLevel,
                                   kCompressionThresholdBytes,
                                   protobuf_ports::kDefaultMemLevel));
    std::unique_ptr<PortableFileBackedProtoLog<DeletionEvent>> deletion_log =
        std::move(create_result.log_create_result.proto_log);
    std::vector<int64_t> offsets;
    for (const DeletionEvent& deletion_event : deletion_events) {
      ICING_ASSIGN_OR_RETURN(int64_t offset,
                             deletion_log->WriteProto(deletion_event));
      offsets.push_back(offset);
    }
    ICING_RETURN_IF_ERROR(deletion_log->PersistToDisk());
    return offsets;
  }

  // Overwrites the DeletionIndex file with the given DeletionIndexEntries.
  libtextclassifier3::Status WriteDeletionIndex(
      const std::vector<DeletionIndexEntry>& entries) {
    ICING_RETURN_IF_ERROR(FileBackedVector<DeletionIndexEntry>::Delete(
        filesystem_, DeletionIndexPath()));
    ICING_ASSIGN_OR_RETURN(
        std::unique_ptr<FileBackedVector<DeletionIndexEntry>> deletion_index,
        FileBackedVector<DeletionIndexEntry>::Create(
            filesystem_, DeletionIndexPath(),
            MemoryMappedFile::READ_WRITE_AUTO_SYNC));
    for (const DeletionIndexEntry& entry : entries) {
      ICING_RETURN_IF_ERROR(deletion_index->Append(entry));
    }
    return deletion_index->PersistToDisk();
  }

  // Reads all DeletionIndexEntries from the DeletionIndex file.
  libtextclassifier3::StatusOr<std::vector<DeletionIndexEntry>>
  ReadDeletionIndex() {
    ICING_ASSIGN_OR_RETURN(
        std::unique_ptr<FileBackedVector<DeletionIndexEntry>> deletion_index,
        FileBackedVector<DeletionIndexEntry>::Create(
            filesystem_, DeletionIndexPath(),
            MemoryMappedFile::READ_WRITE_AUTO_SYNC));
    return std::vector<DeletionIndexEntry>(
        deletion_index->array(),
        deletion_index->array() + deletion_index->num_elements());
  }

  struct LogContents {
    std::vector<DeletionEvent> events;
    std::vector<int64_t> offsets;
  };

  // Reads all DeletionEvents and their offsets from the DeletionLog file.
  libtextclassifier3::StatusOr<LogContents> ReadDeletionLog() {
    ICING_ASSIGN_OR_RETURN(
        DeletionLogCreator::CreateResult create_result,
        DeletionLogCreator::Create(&filesystem_, base_dir_, kCompressionLevel,
                                   kCompressionThresholdBytes,
                                   protobuf_ports::kDefaultMemLevel));
    std::unique_ptr<PortableFileBackedProtoLog<DeletionEvent>> deletion_log =
        std::move(create_result.log_create_result.proto_log);
    LogContents log_contents;
    PortableFileBackedProtoLog<DeletionEvent>::Iterator iterator =
        deletion_log->GetIterator();
    while (iterator.Advance().ok()) {
      ICING_ASSIGN_OR_RETURN(DeletionEvent deletion_event,
                             deletion_log->ReadProto(iterator.GetOffset()));
      log_contents.events.push_back(std::move(deletion_event));
      log_contents.offsets.push_back(iterator.GetOffset());
    }
    return log_contents;
  }

  static constexpr int32_t kCompressionLevel =
      PortableFileBackedProtoLog<DeletionEvent>::kDefaultCompressionLevel;
  static constexpr uint32_t kCompressionThresholdBytes =
      PortableFileBackedProtoLog<
          DeletionEvent>::kDefaultCompressionThresholdBytes;

  const Filesystem filesystem_;
  std::string base_dir_;
};

TEST_F(DeletionStoreTest, CreateWithNullFilesystemFails) {
  EXPECT_THAT(DeletionStore::Create(
                  /*filesystem=*/nullptr, base_dir_, kCompressionLevel,
                  kCompressionThresholdBytes, protobuf_ports::kDefaultMemLevel),
              StatusIs(libtextclassifier3::StatusCode::FAILED_PRECONDITION));
}

TEST_F(DeletionStoreTest, CreateNewStoreCreatesDeletionLog) {
  ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                             CreateStore());
  EXPECT_THAT(create_result.deletion_store, NotNull());
  EXPECT_THAT(create_result.data_loss, Eq(DataLoss::NONE));

  std::string deletion_log_path = absl_ports::StrCat(
      base_dir_, "/", DeletionLogCreator::GetDeletionLogFilename());
  EXPECT_THAT(filesystem_.FileExists(deletion_log_path.c_str()), IsTrue());
}

TEST_F(DeletionStoreTest, CreateNewStoreCreatesDeletionIndex) {
  ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                             CreateStore());

  std::string deletion_index_path = absl_ports::StrCat(
      base_dir_, "/", DeletionStore::GetDeletionIndexFilename());
  EXPECT_THAT(filesystem_.FileExists(deletion_index_path.c_str()), IsTrue());
}

TEST_F(DeletionStoreTest, PersistToDisk) {
  ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                             CreateStore());
  ICING_EXPECT_OK(create_result.deletion_store->PersistToDisk());
}

TEST_F(DeletionStoreTest, RecreateStore) {
  {
    ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                               CreateStore());
    ICING_ASSERT_OK(create_result.deletion_store->PersistToDisk());
  }

  ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                             CreateStore());
  EXPECT_THAT(create_result.deletion_store, NotNull());
  EXPECT_THAT(create_result.data_loss, Eq(DataLoss::NONE));
  EXPECT_THAT(create_result.derived_files_regenerated, IsFalse());
}

TEST_F(DeletionStoreTest, InitializeRegeneratesIfIndexIsEmpty) {
  DeletionEvent event1 = CreateDeletionEvent("uri1", "schema1",
                                             /*deletion_timestamp_ms=*/120000,
                                             /*update_timestamp_ms=*/60000);
  DeletionEvent event2 = CreateDeletionEvent("uri2", "schema2",
                                             /*deletion_timestamp_ms=*/270000,
                                             /*update_timestamp_ms=*/210000);
  ICING_ASSERT_OK_AND_ASSIGN(std::vector<int64_t> offsets,
                             WriteToDeletionLog({event1, event2}));
  ICING_ASSERT_OK(WriteDeletionIndex({}));

  {
    ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                               CreateStore());
    EXPECT_THAT(create_result.derived_files_regenerated, IsTrue());
  }

  EXPECT_THAT(ReadDeletionIndex(),
              IsOkAndHolds(
                  ElementsAre(ExpectedDeletionIndexEntry(event1, offsets[0]),
                              ExpectedDeletionIndexEntry(event2, offsets[1]))));
}

TEST_F(DeletionStoreTest, InitializeRegeneratesIfIndexDoesNotExist) {
  DeletionEvent event1 = CreateDeletionEvent("uri1", "schema1",
                                             /*deletion_timestamp_ms=*/120000,
                                             /*update_timestamp_ms=*/60000);
  DeletionEvent event2 = CreateDeletionEvent("uri2", "schema2",
                                             /*deletion_timestamp_ms=*/270000,
                                             /*update_timestamp_ms=*/210000);
  ICING_ASSERT_OK_AND_ASSIGN(std::vector<int64_t> offsets,
                             WriteToDeletionLog({event1, event2}));

  {
    ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                               CreateStore());
    EXPECT_THAT(create_result.derived_files_regenerated, IsTrue());
  }

  EXPECT_THAT(ReadDeletionIndex(),
              IsOkAndHolds(
                  ElementsAre(ExpectedDeletionIndexEntry(event1, offsets[0]),
                              ExpectedDeletionIndexEntry(event2, offsets[1]))));
}

TEST_F(DeletionStoreTest, InitializeAppendsMissingEntries) {
  DeletionEvent event1 = CreateDeletionEvent("uri1", "schema1",
                                             /*deletion_timestamp_ms=*/120000,
                                             /*update_timestamp_ms=*/60000);
  DeletionEvent event2 = CreateDeletionEvent("uri2", "schema2",
                                             /*deletion_timestamp_ms=*/270000,
                                             /*update_timestamp_ms=*/210000);
  DeletionEvent event3 = CreateDeletionEvent("uri3", "schema1",
                                             /*deletion_timestamp_ms=*/360000,
                                             /*update_timestamp_ms=*/300000);
  ICING_ASSERT_OK_AND_ASSIGN(std::vector<int64_t> offsets,
                             WriteToDeletionLog({event1, event2, event3}));
  // Simulate the DeletionIndex missing the last two DeletionIndexEntries.
  ICING_ASSERT_OK(
      WriteDeletionIndex({ExpectedDeletionIndexEntry(event1, offsets[0])}));

  {
    ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                               CreateStore());
    EXPECT_THAT(create_result.derived_files_regenerated, IsFalse());
  }

  EXPECT_THAT(ReadDeletionIndex(),
              IsOkAndHolds(
                  ElementsAre(ExpectedDeletionIndexEntry(event1, offsets[0]),
                              ExpectedDeletionIndexEntry(event2, offsets[1]),
                              ExpectedDeletionIndexEntry(event3, offsets[2]))));
}

TEST_F(DeletionStoreTest, InitializeTruncatesEntriesPastEndOfLog) {
  DeletionEvent event1 = CreateDeletionEvent("uri1", "schema1",
                                             /*deletion_timestamp_ms=*/120000,
                                             /*update_timestamp_ms=*/60000);
  ICING_ASSERT_OK_AND_ASSIGN(std::vector<int64_t> offsets,
                             WriteToDeletionLog({event1}));
  // Simulate the DeletionLog having been rewound past the last two
  // DeletionIndexEntries.
  ICING_ASSERT_OK(WriteDeletionIndex(
      {ExpectedDeletionIndexEntry(event1, offsets[0]),
       DeletionIndexEntry(/*offset=*/1 << 20, /*deletion_timestamp_m=*/4,
                          /*last_update_timestamp_m=*/3, /*schema_fp=*/1),
       DeletionIndexEntry(/*offset=*/1 << 21, /*deletion_timestamp_m=*/6,
                          /*last_update_timestamp_m=*/5, /*schema_fp=*/2)}));

  {
    ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                               CreateStore());
    EXPECT_THAT(create_result.derived_files_regenerated, IsFalse());
  }

  EXPECT_THAT(ReadDeletionIndex(),
              IsOkAndHolds(
                  ElementsAre(ExpectedDeletionIndexEntry(event1, offsets[0]))));
}

TEST_F(DeletionStoreTest, InitializeRegeneratesIfAllEntriesArePastEndOfLog) {
  DeletionEvent event1 = CreateDeletionEvent("uri1", "schema1",
                                             /*deletion_timestamp_ms=*/120000,
                                             /*update_timestamp_ms=*/60000);
  ICING_ASSERT_OK_AND_ASSIGN(std::vector<int64_t> offsets,
                             WriteToDeletionLog({event1}));
  // Every DeletionIndexEntry points past the end of the non-empty DeletionLog.
  ICING_ASSERT_OK(WriteDeletionIndex(
      {DeletionIndexEntry(/*offset=*/1 << 20, /*deletion_timestamp_m=*/4,
                          /*last_update_timestamp_m=*/3, /*schema_fp=*/1),
       DeletionIndexEntry(/*offset=*/1 << 21, /*deletion_timestamp_m=*/6,
                          /*last_update_timestamp_m=*/5, /*schema_fp=*/2)}));

  {
    ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                               CreateStore());
    EXPECT_THAT(create_result.derived_files_regenerated, IsTrue());
  }

  EXPECT_THAT(ReadDeletionIndex(),
              IsOkAndHolds(
                  ElementsAre(ExpectedDeletionIndexEntry(event1, offsets[0]))));
}

TEST_F(DeletionStoreTest, InitializeTruncatesAllEntriesForEmptyLog) {
  {
    // Create an empty DeletionLog.
    ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                               CreateStore());
  }
  ICING_ASSERT_OK(WriteDeletionIndex(
      {DeletionIndexEntry(/*offset=*/1 << 20, /*deletion_timestamp_m=*/2,
                          /*last_update_timestamp_m=*/1, /*schema_fp=*/1)}));

  {
    ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                               CreateStore());
    EXPECT_THAT(create_result.derived_files_regenerated, IsFalse());
  }

  EXPECT_THAT(ReadDeletionIndex(), IsOkAndHolds(IsEmpty()));
}

TEST_F(DeletionStoreTest, InitializeRegeneratesIfLastEntryDoesNotMatch) {
  DeletionEvent event1 = CreateDeletionEvent("uri1", "schema1",
                                             /*deletion_timestamp_ms=*/120000,
                                             /*update_timestamp_ms=*/60000);
  DeletionEvent event2 = CreateDeletionEvent("uri2", "schema2",
                                             /*deletion_timestamp_ms=*/270000,
                                             /*update_timestamp_ms=*/210000);
  ICING_ASSERT_OK_AND_ASSIGN(std::vector<int64_t> offsets,
                             WriteToDeletionLog({event1, event2}));
  // The last DeletionIndexEntry points to event2 but has the wrong schema
  // fingerprint.
  ICING_ASSERT_OK(WriteDeletionIndex(
      {ExpectedDeletionIndexEntry(event1, offsets[0]),
       DeletionIndexEntry(offsets[1], /*deletion_timestamp_m=*/4,
                          /*last_update_timestamp_m=*/3,
                          /*schema_fp=*/12345)}));

  {
    ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                               CreateStore());
    EXPECT_THAT(create_result.derived_files_regenerated, IsTrue());
  }

  EXPECT_THAT(ReadDeletionIndex(),
              IsOkAndHolds(
                  ElementsAre(ExpectedDeletionIndexEntry(event1, offsets[0]),
                              ExpectedDeletionIndexEntry(event2, offsets[1]))));
}

TEST_F(DeletionStoreTest, InitializeRegeneratesIfIndexIsCorrupted) {
  DeletionEvent event1 = CreateDeletionEvent("uri1", "schema1",
                                             /*deletion_timestamp_ms=*/120000,
                                             /*update_timestamp_ms=*/60000);
  ICING_ASSERT_OK_AND_ASSIGN(std::vector<int64_t> offsets,
                             WriteToDeletionLog({event1}));
  ICING_ASSERT_OK(
      WriteDeletionIndex({ExpectedDeletionIndexEntry(event1, offsets[0])}));
  // Corrupt the DeletionIndex file so that its checksum no longer matches.
  std::string garbage = "garbage";
  ASSERT_TRUE(filesystem_.PWrite(DeletionIndexPath().c_str(),
                                 /*offset=*/0, garbage.data(), garbage.size()));

  {
    ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                               CreateStore());
    EXPECT_THAT(create_result.derived_files_regenerated, IsTrue());
  }

  EXPECT_THAT(ReadDeletionIndex(),
              IsOkAndHolds(
                  ElementsAre(ExpectedDeletionIndexEntry(event1, offsets[0]))));
}

TEST_F(DeletionStoreTest, InitializeRegeneratesIndexOnCompleteDataLoss) {
  DeletionEvent event1 = CreateDeletionEvent("uri1", "schema1",
                                             /*deletion_timestamp_ms=*/120000,
                                             /*update_timestamp_ms=*/60000);
  DeletionEvent event2 = CreateDeletionEvent("uri2", "schema2",
                                             /*deletion_timestamp_ms=*/270000,
                                             /*update_timestamp_ms=*/210000);
  ICING_ASSERT_OK_AND_ASSIGN(std::vector<int64_t> offsets,
                             WriteToDeletionLog({event1, event2}));
  {
    // Build a DeletionIndex that's consistent with the DeletionLog.
    ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                               CreateStore());
  }
  ASSERT_THAT(ReadDeletionIndex(),
              IsOkAndHolds(
                  ElementsAre(ExpectedDeletionIndexEntry(event1, offsets[0]),
                              ExpectedDeletionIndexEntry(event2, offsets[1]))));

  // Corrupt the DeletionLog's contents and set its dirty flag, so that the
  // recomputed log checksum doesn't match and the DeletionLog throws out all
  // of its data on the next initialization.
  using Header = PortableFileBackedProtoLog<DeletionEvent>::Header;
  std::string deletion_log_path = absl_ports::StrCat(
      base_dir_, "/", DeletionLogCreator::GetDeletionLogFilename());
  std::string garbage = "garbage";
  ASSERT_TRUE(filesystem_.PWrite(
      deletion_log_path.c_str(),
      PortableFileBackedProtoLog<DeletionEvent>::kHeaderReservedBytes,
      garbage.data(), garbage.size()));
  Header header;
  ASSERT_TRUE(filesystem_.PRead(deletion_log_path.c_str(), &header,
                                sizeof(Header), /*offset=*/0));
  header.SetDirtyFlag(true);
  header.UpdateHeaderChecksums();
  ASSERT_TRUE(filesystem_.PWrite(deletion_log_path.c_str(), /*offset=*/0,
                                 &header, sizeof(Header)));

  {
    ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                               CreateStore());
    EXPECT_THAT(create_result.data_loss, Eq(DataLoss::COMPLETE));
    // The existing DeletionIndex was thrown out and regenerated, which counts
    // as a recovery since this isn't a new DeletionLog.
    EXPECT_THAT(create_result.derived_files_regenerated, IsTrue());
  }

  // The DeletionLog is now empty, so the regenerated DeletionIndex should be
  // too.
  EXPECT_THAT(ReadDeletionIndex(), IsOkAndHolds(IsEmpty()));
}

TEST_F(DeletionStoreTest, CreateResetsPreexistingIndexWhenLogIsNew) {
  std::string deletion_index_path = absl_ports::StrCat(
      base_dir_, "/", DeletionStore::GetDeletionIndexFilename());

  // Write some entries into an existing DeletionIndex file.
  {
    ICING_ASSERT_OK_AND_ASSIGN(
        std::unique_ptr<FileBackedVector<DeletionIndexEntry>> deletion_index,
        FileBackedVector<DeletionIndexEntry>::Create(
            filesystem_, deletion_index_path,
            MemoryMappedFile::READ_WRITE_AUTO_SYNC));
    ICING_ASSERT_OK(deletion_index->Append(DeletionIndexEntry(
        /*offset=*/0, /*deletion_timestamp_m=*/1,
        /*last_update_timestamp_m=*/1, /*schema_fp=*/123)));
    ICING_ASSERT_OK(deletion_index->Append(DeletionIndexEntry(
        /*offset=*/100, /*deletion_timestamp_m=*/2,
        /*last_update_timestamp_m=*/2, /*schema_fp=*/456)));
    ICING_ASSERT_OK(deletion_index->PersistToDisk());
  }
  // Sanity check that the entries were persisted, so that reloading the file
  // without deleting it would see them.
  {
    ICING_ASSERT_OK_AND_ASSIGN(
        std::unique_ptr<FileBackedVector<DeletionIndexEntry>> deletion_index,
        FileBackedVector<DeletionIndexEntry>::Create(
            filesystem_, deletion_index_path,
            MemoryMappedFile::READ_WRITE_AUTO_SYNC));
    ASSERT_THAT(deletion_index->num_elements(), Eq(2));
  }

  {
    ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                               CreateStore());
    EXPECT_THAT(create_result.deletion_store, NotNull());
    EXPECT_THAT(create_result.derived_files_regenerated, IsFalse());
  }

  // The old DeletionIndex file should have been deleted and replaced with a
  // new, empty DeletionIndex.
  ASSERT_THAT(filesystem_.FileExists(deletion_index_path.c_str()), IsTrue());
  ICING_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<FileBackedVector<DeletionIndexEntry>> deletion_index,
      FileBackedVector<DeletionIndexEntry>::Create(
          filesystem_, deletion_index_path,
          MemoryMappedFile::READ_WRITE_AUTO_SYNC));
  EXPECT_THAT(deletion_index->num_elements(), Eq(0));
}

TEST_F(DeletionStoreTest, CreateFailsIfDeletionIndexCannotBeReset) {
  // Put a directory at the DeletionIndex path so that deleting the old
  // DeletionIndex file fails.
  std::string deletion_index_path = absl_ports::StrCat(
      base_dir_, "/", DeletionStore::GetDeletionIndexFilename());
  ASSERT_THAT(filesystem_.CreateDirectory(deletion_index_path.c_str()),
              IsTrue());

  libtextclassifier3::StatusOr<DeletionStore::CreateResult> create_result_or =
      CreateStore();
  EXPECT_THAT(create_result_or,
              StatusIs(libtextclassifier3::StatusCode::INTERNAL));
  EXPECT_THAT(create_result_or.status().error_message(),
              HasSubstr("Failed to regenerate DeletionIndex."));
}

TEST_F(DeletionStoreTest, PutWritesToDeletionLogAndDeletionIndex) {
  DeletionEvent event1 = CreateDeletionEvent(
      "uri1", "schema1", /*deletion_timestamp_ms=*/2 * kMillisPerMinute,
      /*update_timestamp_ms=*/1 * kMillisPerMinute);
  DeletionEvent event2 = CreateDeletionEvent(
      "uri2", "schema2",
      /*deletion_timestamp_ms=*/4 * kMillisPerMinute + 30000,
      /*update_timestamp_ms=*/3 * kMillisPerMinute + 30000);
  {
    ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                               CreateStore());
    ICING_ASSERT_OK(create_result.deletion_store->Put(event1));
    ICING_ASSERT_OK(create_result.deletion_store->Put(event2));
  }

  ICING_ASSERT_OK_AND_ASSIGN(LogContents log_contents, ReadDeletionLog());
  EXPECT_THAT(log_contents.events,
              ElementsAre(EqualsProto(event1), EqualsProto(event2)));
  EXPECT_THAT(
      ReadDeletionIndex(),
      IsOkAndHolds(ElementsAre(
          ExpectedDeletionIndexEntry(event1, log_contents.offsets[0]),
          ExpectedDeletionIndexEntry(event2, log_contents.offsets[1]))));
}

TEST_F(DeletionStoreTest, PutAfterReinitialize) {
  DeletionEvent event1 = CreateDeletionEvent(
      "uri1", "schema1", /*deletion_timestamp_ms=*/2 * kMillisPerMinute,
      /*update_timestamp_ms=*/1 * kMillisPerMinute);
  DeletionEvent event2 = CreateDeletionEvent(
      "uri2", "schema2",
      /*deletion_timestamp_ms=*/4 * kMillisPerMinute + 30000,
      /*update_timestamp_ms=*/3 * kMillisPerMinute + 30000);
  {
    ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                               CreateStore());
    ICING_ASSERT_OK(create_result.deletion_store->Put(event1));
  }
  {
    ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                               CreateStore());
    EXPECT_THAT(create_result.data_loss, Eq(DataLoss::NONE));
    EXPECT_THAT(create_result.derived_files_regenerated, IsFalse());
    ICING_ASSERT_OK(create_result.deletion_store->Put(event2));
  }

  ICING_ASSERT_OK_AND_ASSIGN(LogContents log_contents, ReadDeletionLog());
  EXPECT_THAT(log_contents.events,
              ElementsAre(EqualsProto(event1), EqualsProto(event2)));
  EXPECT_THAT(
      ReadDeletionIndex(),
      IsOkAndHolds(ElementsAre(
          ExpectedDeletionIndexEntry(event1, log_contents.offsets[0]),
          ExpectedDeletionIndexEntry(event2, log_contents.offsets[1]))));
}

TEST_F(DeletionStoreTest, PutRejectsDeletionEventFromEarlierMinute) {
  // 2 minutes and 1 minute respectively.
  DeletionEvent event1 = CreateDeletionEvent("uri1", "schema1",
                                             /*deletion_timestamp_ms=*/120000,
                                             /*update_timestamp_ms=*/1000);
  DeletionEvent event2 = CreateDeletionEvent("uri2", "schema2",
                                             /*deletion_timestamp_ms=*/60000,
                                             /*update_timestamp_ms=*/1000);
  {
    ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                               CreateStore());
    ICING_ASSERT_OK(create_result.deletion_store->Put(event1));
    EXPECT_THAT(create_result.deletion_store->Put(event2),
                StatusIs(libtextclassifier3::StatusCode::INVALID_ARGUMENT));
  }

  // The rejected DeletionEvent should not have been written anywhere.
  ICING_ASSERT_OK_AND_ASSIGN(LogContents log_contents, ReadDeletionLog());
  EXPECT_THAT(log_contents.events, ElementsAre(EqualsProto(event1)));
  EXPECT_THAT(ReadDeletionIndex(),
              IsOkAndHolds(ElementsAre(ExpectedDeletionIndexEntry(
                  event1, log_contents.offsets[0]))));
}

TEST_F(DeletionStoreTest, PutRejectsEarlierMinuteAfterReinitialize) {
  DeletionEvent event1 = CreateDeletionEvent("uri1", "schema1",
                                             /*deletion_timestamp_ms=*/120000,
                                             /*update_timestamp_ms=*/1000);
  DeletionEvent event2 = CreateDeletionEvent("uri2", "schema2",
                                             /*deletion_timestamp_ms=*/60000,
                                             /*update_timestamp_ms=*/1000);
  {
    ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                               CreateStore());
    ICING_ASSERT_OK(create_result.deletion_store->Put(event1));
  }
  ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                             CreateStore());
  EXPECT_THAT(create_result.deletion_store->Put(event2),
              StatusIs(libtextclassifier3::StatusCode::INVALID_ARGUMENT));
}

TEST_F(DeletionStoreTest, PutAcceptsNonDecreasingDeletionTimestamps) {
  DeletionEvent event1 = CreateDeletionEvent("uri1", "schema1",
                                             /*deletion_timestamp_ms=*/61000,
                                             /*update_timestamp_ms=*/1000);
  DeletionEvent event2 = CreateDeletionEvent("uri2", "schema2",
                                             /*deletion_timestamp_ms=*/61000,
                                             /*update_timestamp_ms=*/1000);
  DeletionEvent event3 = CreateDeletionEvent("uri3", "schema3",
                                             /*deletion_timestamp_ms=*/90000,
                                             /*update_timestamp_ms=*/1000);
  ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                             CreateStore());
  ICING_EXPECT_OK(create_result.deletion_store->Put(event1));
  // Equal deletion timestamp.
  ICING_EXPECT_OK(create_result.deletion_store->Put(event2));
  // Later deletion timestamp within the same minute.
  ICING_EXPECT_OK(create_result.deletion_store->Put(event3));
}

TEST_F(DeletionStoreTest, GetFromEmptyStore) {
  ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                             CreateStore());
  EXPECT_THAT(create_result.deletion_store->Get(/*timestamp_ms=*/0,
                                                /*schemas=*/{}),
              IsOkAndHolds(IsEmpty()));
}

TEST_F(DeletionStoreTest, GetReturnsEventsDeletedAfterTimestamp) {
  DeletionEvent event1 = CreateDeletionEvent(
      "uri1", "schema", /*deletion_timestamp_ms=*/2 * kMillisPerMinute,
      /*update_timestamp_ms=*/1 * kMillisPerMinute);
  DeletionEvent event2 = CreateDeletionEvent(
      "uri2", "schema", /*deletion_timestamp_ms=*/4 * kMillisPerMinute,
      /*update_timestamp_ms=*/1 * kMillisPerMinute);
  DeletionEvent event3 = CreateDeletionEvent(
      "uri3", "schema", /*deletion_timestamp_ms=*/6 * kMillisPerMinute,
      /*update_timestamp_ms=*/1 * kMillisPerMinute);
  ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                             CreateStore());
  DeletionStore* deletion_store = create_result.deletion_store.get();
  ICING_ASSERT_OK(deletion_store->Put(event1));
  ICING_ASSERT_OK(deletion_store->Put(event2));
  ICING_ASSERT_OK(deletion_store->Put(event3));

  EXPECT_THAT(deletion_store->Get(/*timestamp_ms=*/1 * kMillisPerMinute,
                                  /*schemas=*/{}),
              IsOkAndHolds(ElementsAre(EqualsProto(event1), EqualsProto(event2),
                                       EqualsProto(event3))));
  EXPECT_THAT(
      deletion_store->Get(/*timestamp_ms=*/3 * kMillisPerMinute,
                          /*schemas=*/{}),
      IsOkAndHolds(ElementsAre(EqualsProto(event2), EqualsProto(event3))));
  // A deletion at exactly the timestamp is not returned.
  EXPECT_THAT(deletion_store->Get(/*timestamp_ms=*/4 * kMillisPerMinute,
                                  /*schemas=*/{}),
              IsOkAndHolds(ElementsAre(EqualsProto(event3))));
  EXPECT_THAT(deletion_store->Get(/*timestamp_ms=*/6 * kMillisPerMinute,
                                  /*schemas=*/{}),
              IsOkAndHolds(IsEmpty()));
}

TEST_F(DeletionStoreTest, GetFiltersOutEventsWithinSameMinute) {
  // Both DeletionEvents have a deletion_timestamp_m of 2, so the
  // DeletionIndex alone can't tell them apart.
  DeletionEvent event1 = CreateDeletionEvent(
      "uri1", "schema", /*deletion_timestamp_ms=*/2 * kMillisPerMinute + 6000,
      /*update_timestamp_ms=*/1 * kMillisPerMinute);
  DeletionEvent event2 = CreateDeletionEvent(
      "uri2", "schema", /*deletion_timestamp_ms=*/2 * kMillisPerMinute + 54000,
      /*update_timestamp_ms=*/1 * kMillisPerMinute);
  ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                             CreateStore());
  DeletionStore* deletion_store = create_result.deletion_store.get();
  ICING_ASSERT_OK(deletion_store->Put(event1));
  ICING_ASSERT_OK(deletion_store->Put(event2));

  EXPECT_THAT(
      deletion_store->Get(/*timestamp_ms=*/2 * kMillisPerMinute + 30000,
                          /*schemas=*/{}),
      IsOkAndHolds(ElementsAre(EqualsProto(event2))));
}

TEST_F(DeletionStoreTest, GetFiltersOutDocumentsUpdatedAfterTimestamp) {
  // Updated before the timestamp.
  DeletionEvent event1 = CreateDeletionEvent(
      "uri1", "schema", /*deletion_timestamp_ms=*/5 * kMillisPerMinute,
      /*update_timestamp_ms=*/1 * kMillisPerMinute);
  // Updated at exactly the timestamp.
  DeletionEvent event2 = CreateDeletionEvent(
      "uri2", "schema", /*deletion_timestamp_ms=*/5 * kMillisPerMinute,
      /*update_timestamp_ms=*/3 * kMillisPerMinute);
  // Updated after the timestamp, but within the same minute.
  DeletionEvent event3 = CreateDeletionEvent(
      "uri3", "schema", /*deletion_timestamp_ms=*/5 * kMillisPerMinute,
      /*update_timestamp_ms=*/3 * kMillisPerMinute + 30000);
  // Updated after the timestamp.
  DeletionEvent event4 = CreateDeletionEvent(
      "uri4", "schema", /*deletion_timestamp_ms=*/5 * kMillisPerMinute,
      /*update_timestamp_ms=*/4 * kMillisPerMinute);
  ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                             CreateStore());
  DeletionStore* deletion_store = create_result.deletion_store.get();
  ICING_ASSERT_OK(deletion_store->Put(event1));
  ICING_ASSERT_OK(deletion_store->Put(event2));
  ICING_ASSERT_OK(deletion_store->Put(event3));
  ICING_ASSERT_OK(deletion_store->Put(event4));

  EXPECT_THAT(
      deletion_store->Get(/*timestamp_ms=*/3 * kMillisPerMinute,
                          /*schemas=*/{}),
      IsOkAndHolds(ElementsAre(EqualsProto(event1), EqualsProto(event2))));
}

TEST_F(DeletionStoreTest, GetFiltersBySchema) {
  DeletionEvent event1 = CreateDeletionEvent(
      "uri1", "schema1", /*deletion_timestamp_ms=*/2 * kMillisPerMinute,
      /*update_timestamp_ms=*/1 * kMillisPerMinute);
  DeletionEvent event2 = CreateDeletionEvent(
      "uri2", "schema2", /*deletion_timestamp_ms=*/3 * kMillisPerMinute,
      /*update_timestamp_ms=*/1 * kMillisPerMinute);
  DeletionEvent event3 = CreateDeletionEvent(
      "uri3", "schema3", /*deletion_timestamp_ms=*/4 * kMillisPerMinute,
      /*update_timestamp_ms=*/1 * kMillisPerMinute);
  ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                             CreateStore());
  DeletionStore* deletion_store = create_result.deletion_store.get();
  ICING_ASSERT_OK(deletion_store->Put(event1));
  ICING_ASSERT_OK(deletion_store->Put(event2));
  ICING_ASSERT_OK(deletion_store->Put(event3));

  EXPECT_THAT(
      deletion_store->Get(/*timestamp_ms=*/1 * kMillisPerMinute,
                          /*schemas=*/{"schema1", "schema3"}),
      IsOkAndHolds(ElementsAre(EqualsProto(event1), EqualsProto(event3))));
  EXPECT_THAT(deletion_store->Get(/*timestamp_ms=*/1 * kMillisPerMinute,
                                  /*schemas=*/{"nonexistent"}),
              IsOkAndHolds(IsEmpty()));
}

TEST_F(DeletionStoreTest, GetAfterReinitialize) {
  DeletionEvent event1 = CreateDeletionEvent(
      "uri1", "schema", /*deletion_timestamp_ms=*/2 * kMillisPerMinute,
      /*update_timestamp_ms=*/1 * kMillisPerMinute);
  DeletionEvent event2 = CreateDeletionEvent(
      "uri2", "schema", /*deletion_timestamp_ms=*/4 * kMillisPerMinute,
      /*update_timestamp_ms=*/1 * kMillisPerMinute);
  {
    ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                               CreateStore());
    ICING_ASSERT_OK(create_result.deletion_store->Put(event1));
    ICING_ASSERT_OK(create_result.deletion_store->Put(event2));
  }

  ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                             CreateStore());
  EXPECT_THAT(create_result.deletion_store->Get(
                  /*timestamp_ms=*/3 * kMillisPerMinute, /*schemas=*/{}),
              IsOkAndHolds(ElementsAre(EqualsProto(event2))));
}

TEST_F(DeletionStoreTest, OptimizeEmptyStore) {
  ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                             CreateStore());
  ICING_EXPECT_OK(create_result.deletion_store->Optimize(
      /*timestamp_ms=*/1 * kMillisPerMinute));
  EXPECT_THAT(create_result.deletion_store->Get(/*timestamp_ms=*/0,
                                                /*schemas=*/{}),
              IsOkAndHolds(IsEmpty()));
}

TEST_F(DeletionStoreTest, OptimizeWithNothingToPurge) {
  DeletionEvent event1 = CreateDeletionEvent(
      "uri1", "schema", /*deletion_timestamp_ms=*/2 * kMillisPerMinute,
      /*update_timestamp_ms=*/1 * kMillisPerMinute);
  ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                             CreateStore());
  DeletionStore* deletion_store = create_result.deletion_store.get();
  ICING_ASSERT_OK(deletion_store->Put(event1));

  ICING_ASSERT_OK(
      deletion_store->Optimize(/*timestamp_ms=*/2 * kMillisPerMinute - 1));
  EXPECT_THAT(deletion_store->Get(/*timestamp_ms=*/1 * kMillisPerMinute,
                                  /*schemas=*/{}),
              IsOkAndHolds(ElementsAre(EqualsProto(event1))));
}

TEST_F(DeletionStoreTest, OptimizePurgesEventsDeletedAtOrBeforeTimestamp) {
  DeletionEvent event1 = CreateDeletionEvent(
      "uri1", "schema", /*deletion_timestamp_ms=*/1 * kMillisPerMinute,
      /*update_timestamp_ms=*/0);
  DeletionEvent event2 = CreateDeletionEvent(
      "uri2", "schema", /*deletion_timestamp_ms=*/2 * kMillisPerMinute + 6000,
      /*update_timestamp_ms=*/0);
  DeletionEvent event3 = CreateDeletionEvent(
      "uri3", "schema", /*deletion_timestamp_ms=*/2 * kMillisPerMinute + 30000,
      /*update_timestamp_ms=*/0);
  DeletionEvent event4 = CreateDeletionEvent(
      "uri4", "schema", /*deletion_timestamp_ms=*/2 * kMillisPerMinute + 54000,
      /*update_timestamp_ms=*/0);
  DeletionEvent event5 = CreateDeletionEvent(
      "uri5", "schema", /*deletion_timestamp_ms=*/4 * kMillisPerMinute,
      /*update_timestamp_ms=*/0);
  ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                             CreateStore());
  DeletionStore* deletion_store = create_result.deletion_store.get();
  ICING_ASSERT_OK(deletion_store->Put(event1));
  ICING_ASSERT_OK(deletion_store->Put(event2));
  ICING_ASSERT_OK(deletion_store->Put(event3));
  ICING_ASSERT_OK(deletion_store->Put(event4));
  ICING_ASSERT_OK(deletion_store->Put(event5));

  // event2, event3 and event4 all have a deletion_timestamp_m of 2, but only
  // event2 and event3 were deleted at or before the timestamp.
  ICING_ASSERT_OK(deletion_store->Optimize(
      /*timestamp_ms=*/2 * kMillisPerMinute + 30000));

  EXPECT_THAT(
      deletion_store->Get(/*timestamp_ms=*/0, /*schemas=*/{}),
      IsOkAndHolds(ElementsAre(EqualsProto(event4), EqualsProto(event5))));
  EXPECT_THAT(filesystem_.DirectoryExists(
                  absl_ports::StrCat(base_dir_, "_optimize_tmp").c_str()),
              IsFalse());

  // The DeletionLog and DeletionIndex were rewritten with the remaining
  // DeletionEvents.
  create_result.deletion_store.reset();
  ICING_ASSERT_OK_AND_ASSIGN(LogContents log_contents, ReadDeletionLog());
  EXPECT_THAT(log_contents.events,
              ElementsAre(EqualsProto(event4), EqualsProto(event5)));
  EXPECT_THAT(
      ReadDeletionIndex(),
      IsOkAndHolds(ElementsAre(
          ExpectedDeletionIndexEntry(event4, log_contents.offsets[0]),
          ExpectedDeletionIndexEntry(event5, log_contents.offsets[1]))));
}

TEST_F(DeletionStoreTest, OptimizePurgesAllEvents) {
  DeletionEvent event1 = CreateDeletionEvent(
      "uri1", "schema", /*deletion_timestamp_ms=*/2 * kMillisPerMinute,
      /*update_timestamp_ms=*/1 * kMillisPerMinute);
  DeletionEvent event2 = CreateDeletionEvent(
      "uri2", "schema", /*deletion_timestamp_ms=*/3 * kMillisPerMinute,
      /*update_timestamp_ms=*/1 * kMillisPerMinute);
  ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                             CreateStore());
  DeletionStore* deletion_store = create_result.deletion_store.get();
  ICING_ASSERT_OK(deletion_store->Put(event1));
  ICING_ASSERT_OK(deletion_store->Put(event2));

  ICING_ASSERT_OK(
      deletion_store->Optimize(/*timestamp_ms=*/3 * kMillisPerMinute));
  EXPECT_THAT(deletion_store->Get(/*timestamp_ms=*/1 * kMillisPerMinute,
                                  /*schemas=*/{}),
              IsOkAndHolds(IsEmpty()));

  // The DeletionStore is still usable after purging everything.
  DeletionEvent event3 = CreateDeletionEvent(
      "uri3", "schema", /*deletion_timestamp_ms=*/4 * kMillisPerMinute,
      /*update_timestamp_ms=*/1 * kMillisPerMinute);
  ICING_ASSERT_OK(deletion_store->Put(event3));
  EXPECT_THAT(deletion_store->Get(/*timestamp_ms=*/1 * kMillisPerMinute,
                                  /*schemas=*/{}),
              IsOkAndHolds(ElementsAre(EqualsProto(event3))));
}

TEST_F(DeletionStoreTest, PutAndGetAfterOptimizeAndReinitialize) {
  DeletionEvent event1 = CreateDeletionEvent(
      "uri1", "schema", /*deletion_timestamp_ms=*/2 * kMillisPerMinute,
      /*update_timestamp_ms=*/1 * kMillisPerMinute);
  DeletionEvent event2 = CreateDeletionEvent(
      "uri2", "schema", /*deletion_timestamp_ms=*/4 * kMillisPerMinute,
      /*update_timestamp_ms=*/1 * kMillisPerMinute);
  DeletionEvent event3 = CreateDeletionEvent(
      "uri3", "schema", /*deletion_timestamp_ms=*/6 * kMillisPerMinute,
      /*update_timestamp_ms=*/1 * kMillisPerMinute);
  {
    ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                               CreateStore());
    ICING_ASSERT_OK(create_result.deletion_store->Put(event1));
    ICING_ASSERT_OK(create_result.deletion_store->Put(event2));
    ICING_ASSERT_OK(create_result.deletion_store->Optimize(
        /*timestamp_ms=*/3 * kMillisPerMinute));
    ICING_ASSERT_OK(create_result.deletion_store->Put(event3));
  }

  ICING_ASSERT_OK_AND_ASSIGN(DeletionStore::CreateResult create_result,
                             CreateStore());
  EXPECT_THAT(create_result.data_loss, Eq(DataLoss::NONE));
  EXPECT_THAT(create_result.derived_files_regenerated, IsFalse());
  EXPECT_THAT(
      create_result.deletion_store->Get(/*timestamp_ms=*/1 * kMillisPerMinute,
                                        /*schemas=*/{}),
      IsOkAndHolds(ElementsAre(EqualsProto(event2), EqualsProto(event3))));
}

TEST_F(DeletionStoreTest, CallsAfterFailedOptimizeReturnFailedPrecondition) {
  DeletionEvent event1 = CreateDeletionEvent(
      "uri1", "schema", /*deletion_timestamp_ms=*/2 * kMillisPerMinute,
      /*update_timestamp_ms=*/1 * kMillisPerMinute);
  DeletionEvent event2 = CreateDeletionEvent(
      "uri2", "schema", /*deletion_timestamp_ms=*/4 * kMillisPerMinute,
      /*update_timestamp_ms=*/1 * kMillisPerMinute);
  MockFilesystem mock_filesystem;
  ICING_ASSERT_OK_AND_ASSIGN(
      DeletionStore::CreateResult create_result,
      DeletionStore::Create(&mock_filesystem, base_dir_, kCompressionLevel,
                            kCompressionThresholdBytes,
                            protobuf_ports::kDefaultMemLevel));
  DeletionStore* deletion_store = create_result.deletion_store.get();
  ICING_ASSERT_OK(deletion_store->Put(event1));
  ICING_ASSERT_OK(deletion_store->Put(event2));

  // Fail the swap after base_dir is moved away, then fail both restoring it and
  // recreating it so that Optimize returns before re-initializing.
  EXPECT_CALL(mock_filesystem, SwapFiles).WillOnce(PartialSwapAction());
  // Other calls use the default action.
  EXPECT_CALL(mock_filesystem, RenameFile).Times(AnyNumber());
  EXPECT_CALL(mock_filesystem,
              RenameFile(StrEq(OptimizeTempDir()), StrEq(base_dir_)))
      .WillOnce(Return(false));
  EXPECT_CALL(mock_filesystem, CreateDirectoryRecursively).Times(AnyNumber());
  EXPECT_CALL(mock_filesystem, CreateDirectoryRecursively(StrEq(base_dir_)))
      .WillOnce(Return(false));
  EXPECT_THAT(deletion_store->Optimize(/*timestamp_ms=*/3 * kMillisPerMinute),
              StatusIs(libtextclassifier3::StatusCode::INTERNAL));

  EXPECT_THAT(deletion_store->Put(event2),
              StatusIs(libtextclassifier3::StatusCode::FAILED_PRECONDITION));
  EXPECT_THAT(deletion_store->Get(/*timestamp_ms=*/0, /*schemas=*/{}),
              StatusIs(libtextclassifier3::StatusCode::FAILED_PRECONDITION));
  EXPECT_THAT(deletion_store->PersistToDisk(),
              StatusIs(libtextclassifier3::StatusCode::FAILED_PRECONDITION));
  EXPECT_THAT(deletion_store->Optimize(/*timestamp_ms=*/3 * kMillisPerMinute),
              StatusIs(libtextclassifier3::StatusCode::FAILED_PRECONDITION));
}

TEST_F(DeletionStoreTest, OptimizeSwapFailureKeepsOldDeletionLog) {
  DeletionEvent event1 = CreateDeletionEvent(
      "uri1", "schema", /*deletion_timestamp_ms=*/2 * kMillisPerMinute,
      /*update_timestamp_ms=*/1 * kMillisPerMinute);
  DeletionEvent event2 = CreateDeletionEvent(
      "uri2", "schema", /*deletion_timestamp_ms=*/4 * kMillisPerMinute,
      /*update_timestamp_ms=*/1 * kMillisPerMinute);
  MockFilesystem mock_filesystem;
  ICING_ASSERT_OK_AND_ASSIGN(
      DeletionStore::CreateResult create_result,
      DeletionStore::Create(&mock_filesystem, base_dir_, kCompressionLevel,
                            kCompressionThresholdBytes,
                            protobuf_ports::kDefaultMemLevel));
  DeletionStore* deletion_store = create_result.deletion_store.get();
  ICING_ASSERT_OK(deletion_store->Put(event1));
  ICING_ASSERT_OK(deletion_store->Put(event2));

  // Fail the swap after base_dir is moved away. Optimize should move the old
  // files back.
  EXPECT_CALL(mock_filesystem, SwapFiles).WillOnce(PartialSwapAction());
  EXPECT_THAT(deletion_store->Optimize(/*timestamp_ms=*/3 * kMillisPerMinute),
              StatusIs(libtextclassifier3::StatusCode::INTERNAL));

  // Nothing was purged.
  EXPECT_THAT(
      deletion_store->Get(/*timestamp_ms=*/1 * kMillisPerMinute,
                          /*schemas=*/{}),
      IsOkAndHolds(ElementsAre(EqualsProto(event1), EqualsProto(event2))));
  EXPECT_THAT(filesystem_.DirectoryExists(OptimizeTempDir().c_str()),
              IsFalse());
  EXPECT_THAT(filesystem_.DirectoryExists(
                  absl_ports::StrCat(OptimizeTempDir(), ".tmp").c_str()),
              IsFalse());
}

TEST_F(DeletionStoreTest,
       OptimizeSwapAndRestoreFailureReturnsDataLossAndKeepsOldFiles) {
  DeletionEvent event1 = CreateDeletionEvent(
      "uri1", "schema", /*deletion_timestamp_ms=*/2 * kMillisPerMinute,
      /*update_timestamp_ms=*/1 * kMillisPerMinute);
  DeletionEvent event2 = CreateDeletionEvent(
      "uri2", "schema", /*deletion_timestamp_ms=*/4 * kMillisPerMinute,
      /*update_timestamp_ms=*/1 * kMillisPerMinute);
  MockFilesystem mock_filesystem;
  ICING_ASSERT_OK_AND_ASSIGN(
      DeletionStore::CreateResult create_result,
      DeletionStore::Create(&mock_filesystem, base_dir_, kCompressionLevel,
                            kCompressionThresholdBytes,
                            protobuf_ports::kDefaultMemLevel));
  DeletionStore* deletion_store = create_result.deletion_store.get();
  ICING_ASSERT_OK(deletion_store->Put(event1));
  ICING_ASSERT_OK(deletion_store->Put(event2));

  // Fail the swap after base_dir is moved away, and fail moving it back.
  EXPECT_CALL(mock_filesystem, SwapFiles).WillOnce(PartialSwapAction());
  EXPECT_CALL(mock_filesystem, RenameFile).Times(AnyNumber());
  EXPECT_CALL(mock_filesystem,
              RenameFile(StrEq(OptimizeTempDir()), StrEq(base_dir_)))
      .WillOnce(Return(false));
  EXPECT_THAT(deletion_store->Optimize(/*timestamp_ms=*/3 * kMillisPerMinute),
              StatusIs(libtextclassifier3::StatusCode::DATA_LOSS));

  // The store is usable but empty.
  EXPECT_THAT(deletion_store->Get(/*timestamp_ms=*/1 * kMillisPerMinute,
                                  /*schemas=*/{}),
              IsOkAndHolds(IsEmpty()));

  // The old DeletionLog is not deleted.
  ICING_ASSERT_OK_AND_ASSIGN(
      DeletionStore::CreateResult old_create_result,
      DeletionStore::Create(&filesystem_, OptimizeTempDir(), kCompressionLevel,
                            kCompressionThresholdBytes,
                            protobuf_ports::kDefaultMemLevel));
  EXPECT_THAT(
      old_create_result.deletion_store->Get(
          /*timestamp_ms=*/1 * kMillisPerMinute, /*schemas=*/{}),
      IsOkAndHolds(ElementsAre(EqualsProto(event1), EqualsProto(event2))));
}

}  // namespace

}  // namespace lib
}  // namespace icing
