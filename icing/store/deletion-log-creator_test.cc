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

#include "icing/store/deletion-log-creator.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "icing/text_classifier/lib3/utils/base/status.h"
#include "icing/text_classifier/lib3/utils/base/statusor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "icing/absl_ports/str_cat.h"
#include "icing/file/filesystem.h"
#include "icing/file/portable-file-backed-proto-log.h"
#include "icing/portable/equals-proto.h"
#include "icing/portable/gzip_stream.h"
#include "icing/proto/deletion.pb.h"
#include "icing/testing/common-matchers.h"
#include "icing/testing/tmp-directory.h"

namespace icing {
namespace lib {

namespace {

using ::icing::lib::portable_equals_proto::EqualsProto;
using ::testing::Eq;
using ::testing::IsFalse;
using ::testing::IsTrue;
using ::testing::NotNull;

class DeletionLogCreatorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    base_dir_ = GetTestTempDir() + "/deletion_log_creator_test";
    filesystem_.DeleteDirectoryRecursively(base_dir_.c_str());
    ASSERT_TRUE(filesystem_.CreateDirectoryRecursively(base_dir_.c_str()));
  }

  void TearDown() override {
    filesystem_.DeleteDirectoryRecursively(base_dir_.c_str());
  }

  libtextclassifier3::StatusOr<DeletionLogCreator::CreateResult> Create() {
    return DeletionLogCreator::Create(
        &filesystem_, base_dir_,
        PortableFileBackedProtoLog<DeletionEvent>::kDefaultCompressionLevel,
        PortableFileBackedProtoLog<
            DeletionEvent>::kDefaultCompressionThresholdBytes,
        protobuf_ports::kDefaultMemLevel);
  }

  const Filesystem filesystem_;
  std::string base_dir_;
};

TEST_F(DeletionLogCreatorTest, CreateFailsIfBaseDirDoesNotExist) {
  filesystem_.DeleteDirectoryRecursively(base_dir_.c_str());
  EXPECT_THAT(Create(), StatusIs(libtextclassifier3::StatusCode::INTERNAL));
}

TEST_F(DeletionLogCreatorTest, CreateNewFile) {
  ICING_ASSERT_OK_AND_ASSIGN(DeletionLogCreator::CreateResult create_result,
                             Create());

  EXPECT_THAT(create_result.new_file, IsTrue());
  EXPECT_THAT(create_result.log_create_result.proto_log, NotNull());
  EXPECT_THAT(create_result.log_create_result.has_data_loss(), IsFalse());

  std::string deletion_log_path = absl_ports::StrCat(
      base_dir_, "/", DeletionLogCreator::GetDeletionLogFilename());
  EXPECT_THAT(filesystem_.FileExists(deletion_log_path.c_str()), IsTrue());
}

TEST_F(DeletionLogCreatorTest, CreateExistingFile) {
  DeletionEvent deletion_event;
  deletion_event.set_name_space("namespace");
  deletion_event.set_uri("uri");
  deletion_event.set_schema("schema");
  deletion_event.set_deletion_timestamp_ms(2000);
  deletion_event.set_last_update_timestamp_ms(1000);

  int64_t offset;
  {
    ICING_ASSERT_OK_AND_ASSIGN(DeletionLogCreator::CreateResult create_result,
                               Create());
    ASSERT_THAT(create_result.new_file, IsTrue());
    std::unique_ptr<PortableFileBackedProtoLog<DeletionEvent>> deletion_log =
        std::move(create_result.log_create_result.proto_log);
    ICING_ASSERT_OK_AND_ASSIGN(offset,
                               deletion_log->WriteProto(deletion_event));
    ICING_ASSERT_OK(deletion_log->PersistToDisk());
  }

  ICING_ASSERT_OK_AND_ASSIGN(DeletionLogCreator::CreateResult create_result,
                             Create());
  EXPECT_THAT(create_result.new_file, IsFalse());
  EXPECT_THAT(create_result.log_create_result.has_data_loss(), IsFalse());
  ICING_ASSERT_OK_AND_ASSIGN(
      DeletionEvent read_event,
      create_result.log_create_result.proto_log->ReadProto(offset));
  EXPECT_THAT(read_event, EqualsProto(deletion_event));
}

TEST_F(DeletionLogCreatorTest, GetDeletionLogFilename) {
  EXPECT_THAT(DeletionLogCreator::GetDeletionLogFilename(), Eq("deletion_log"));
}

}  // namespace

}  // namespace lib
}  // namespace icing
