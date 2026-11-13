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

#include "icing/text_classifier/lib3/utils/base/status.h"
#include "icing/text_classifier/lib3/utils/base/statusor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "icing/absl_ports/str_cat.h"
#include "icing/file/file-backed-vector.h"
#include "icing/file/filesystem.h"
#include "icing/file/memory-mapped-file.h"
#include "icing/file/portable-file-backed-proto-log.h"
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

using ::testing::Eq;
using ::testing::HasSubstr;
using ::testing::IsTrue;
using ::testing::NotNull;

class DeletionStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    base_dir_ = GetTestTempDir() + "/deletion_store_test";
    filesystem_.DeleteDirectoryRecursively(base_dir_.c_str());
    ASSERT_TRUE(filesystem_.CreateDirectoryRecursively(base_dir_.c_str()));
  }

  void TearDown() override {
    filesystem_.DeleteDirectoryRecursively(base_dir_.c_str());
  }

  libtextclassifier3::StatusOr<DeletionStore::CreateResult> CreateStore() {
    return DeletionStore::Create(&filesystem_, base_dir_, kCompressionLevel,
                                 kCompressionThresholdBytes,
                                 protobuf_ports::kDefaultMemLevel);
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
}

TEST_F(DeletionStoreTest, CreateResetsExistingDeletionIndex) {
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
              HasSubstr("Failed to initialize DeletionIndex."));
}

}  // namespace

}  // namespace lib
}  // namespace icing
