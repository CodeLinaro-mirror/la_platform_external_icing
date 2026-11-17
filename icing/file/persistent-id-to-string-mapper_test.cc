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

#include "icing/file/persistent-id-to-string-mapper.h"

#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

#include "icing/text_classifier/lib3/utils/base/status.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "icing/absl_ports/str_cat.h"
#include "icing/file/file-backed-vector.h"
#include "icing/file/filesystem.h"
#include "icing/file/memory-mapped-file.h"
#include "icing/file/mock-filesystem.h"
#include "icing/file/persistent-storage.h"
#include "icing/portable/platform.h"
#include "icing/testing/common-matchers.h"
#include "icing/testing/tmp-directory.h"
#include "icing/util/crc32.h"

namespace icing {
namespace lib {

namespace {

using ::testing::Eq;
using ::testing::Gt;
using ::testing::HasSubstr;
using ::testing::IsFalse;
using ::testing::IsTrue;
using ::testing::Matcher;
using ::testing::Ne;
using ::testing::NiceMock;
using ::testing::Not;
using ::testing::Pointee;
using ::testing::Return;
using ::testing::StrEq;

using Crcs = PersistentStorage::Crcs;
using IdToStringMapper = PersistentIdToStringMapper<int32_t>;
using Info = IdToStringMapper::Info;

constexpr int32_t kStrLengthSize = IdToStringMapper::kStrLengthSize;

// Returns the size of the str_storage entry for str: [int32_t len][bytes].
int32_t EntrySize(std::string_view str) {
  return kStrLengthSize + static_cast<int32_t>(str.length());
}

// Reads the length prefix of the str_storage entry starting at offset.
int32_t ReadEntryLength(const FileBackedVector<char>& str_storage,
                        int32_t offset) {
  int32_t len;
  std::memcpy(&len, str_storage.array() + offset, kStrLengthSize);
  return len;
}

// Reads the string of the str_storage entry starting at offset.
std::string_view ReadEntryStr(const FileBackedVector<char>& str_storage,
                              int32_t offset) {
  return std::string_view(str_storage.array() + offset + kStrLengthSize,
                          ReadEntryLength(str_storage, offset));
}

class PersistentIdToStringMapperTest : public ::testing::Test {
 protected:
  void SetUp() override {
    base_dir_ = GetTestTempDir() + "/icing";
    ASSERT_THAT(filesystem_.CreateDirectoryRecursively(base_dir_.c_str()),
                IsTrue());
    working_path_ = base_dir_ + "/persistent_id_to_string_mapper_test";
  }

  void TearDown() override {
    filesystem_.DeleteDirectoryRecursively(base_dir_.c_str());
  }

  Filesystem filesystem_;
  std::string base_dir_;
  std::string working_path_;
};

TEST_F(PersistentIdToStringMapperTest, InvalidWorkingPath) {
  EXPECT_THAT(IdToStringMapper::Create(
                  filesystem_, "/dev/null/persistent_id_to_string_mapper_test"),
              StatusIs(libtextclassifier3::StatusCode::INTERNAL));
}

TEST_F(PersistentIdToStringMapperTest, InitializeNewFiles) {
  {
    ASSERT_THAT(filesystem_.DirectoryExists(working_path_.c_str()), IsFalse());

    ICING_ASSERT_OK_AND_ASSIGN(
        std::unique_ptr<IdToStringMapper> mapper,
        IdToStringMapper::Create(filesystem_, working_path_));
    EXPECT_THAT(mapper->empty(), IsTrue());
    EXPECT_THAT(mapper->num_elements(), Eq(0));
    EXPECT_THAT(mapper->num_strings(), Eq(0));

    ICING_ASSERT_OK_AND_ASSIGN(Crc32 crc, mapper->GetChecksum());
    EXPECT_THAT(mapper->UpdateChecksums(), IsOkAndHolds(crc));

    ICING_ASSERT_OK(mapper->PersistToDisk());
  }

  const std::string metadata_file_path =
      absl_ports::StrCat(working_path_, "/metadata");
  ScopedFd metadata_sfd(filesystem_.OpenForWrite(metadata_file_path.c_str()));
  ASSERT_THAT(metadata_sfd.is_valid(), IsTrue());

  Info info;
  ASSERT_THAT(filesystem_.PRead(metadata_sfd.get(), &info, sizeof(Info),
                                IdToStringMapper::kInfoMetadataFileOffset),
              Eq(sizeof(Info)));
  EXPECT_THAT(info.magic, Eq(Info::kMagic));
  EXPECT_THAT(info.num_strings, Eq(0));

  Crcs crcs;
  ASSERT_THAT(filesystem_.PRead(metadata_sfd.get(), &crcs, sizeof(Crcs),
                                IdToStringMapper::kCrcsMetadataFileOffset),
              Eq(sizeof(Crcs)));
  EXPECT_THAT(crcs.component_crcs.info_crc, Eq(info.GetChecksum().Get()));
  EXPECT_THAT(crcs.all_crc, Eq(crcs.component_crcs.GetChecksum().Get()));
}

TEST_F(PersistentIdToStringMapperTest, CreateFailsOnPartialFiles) {
  {
    ICING_ASSERT_OK_AND_ASSIGN(
        std::unique_ptr<IdToStringMapper> mapper,
        IdToStringMapper::Create(filesystem_, working_path_));
    ICING_ASSERT_OK(mapper->Put(0, "uri0"));
    ICING_ASSERT_OK(mapper->PersistToDisk());
  }

  // Delete str_storage file to simulate an inconsistent partial state.
  std::string str_storage_path =
      absl_ports::StrCat(working_path_, "/str_storage");
  ASSERT_THAT(filesystem_.DeleteFile(str_storage_path.c_str()), IsTrue());

  EXPECT_THAT(IdToStringMapper::Create(filesystem_, working_path_),
              StatusIs(libtextclassifier3::StatusCode::FAILED_PRECONDITION));
}

TEST_F(PersistentIdToStringMapperTest, CreateFailsOnCorruptedMetadata) {
  {
    ICING_ASSERT_OK_AND_ASSIGN(
        std::unique_ptr<IdToStringMapper> mapper,
        IdToStringMapper::Create(filesystem_, working_path_));
    ICING_ASSERT_OK(mapper->Put(0, "uri0"));
    ICING_ASSERT_OK(mapper->PersistToDisk());
  }

  const std::string metadata_file_path =
      absl_ports::StrCat(working_path_, "/metadata");
  ScopedFd metadata_sfd(filesystem_.OpenForWrite(metadata_file_path.c_str()));
  ASSERT_THAT(metadata_sfd.is_valid(), IsTrue());

  Crcs original_crcs;
  ASSERT_THAT(
      filesystem_.PRead(metadata_sfd.get(), &original_crcs, sizeof(Crcs),
                        IdToStringMapper::kCrcsMetadataFileOffset),
      Eq(sizeof(Crcs)));
  Info original_info;
  ASSERT_THAT(
      filesystem_.PRead(metadata_sfd.get(), &original_info, sizeof(Info),
                        IdToStringMapper::kInfoMetadataFileOffset),
      Eq(sizeof(Info)));

  // 1. Corrupt all_crc.
  {
    Crcs corrupted_crcs = original_crcs;
    corrupted_crcs.all_crc ^= 1;
    ASSERT_THAT(filesystem_.PWrite(metadata_sfd.get(),
                                   IdToStringMapper::kCrcsMetadataFileOffset,
                                   &corrupted_crcs, sizeof(Crcs)),
                IsTrue());
    EXPECT_THAT(IdToStringMapper::Create(filesystem_, working_path_),
                StatusIs(libtextclassifier3::StatusCode::FAILED_PRECONDITION));
  }

  // 2. Corrupt info.num_strings without updating crcs.
  {
    ASSERT_THAT(filesystem_.PWrite(metadata_sfd.get(),
                                   IdToStringMapper::kCrcsMetadataFileOffset,
                                   &original_crcs, sizeof(Crcs)),
                IsTrue());
    Info corrupted_info = original_info;
    corrupted_info.num_strings = 99;
    ASSERT_THAT(filesystem_.PWrite(metadata_sfd.get(),
                                   IdToStringMapper::kInfoMetadataFileOffset,
                                   &corrupted_info, sizeof(Info)),
                IsTrue());
    EXPECT_THAT(IdToStringMapper::Create(filesystem_, working_path_),
                StatusIs(libtextclassifier3::StatusCode::FAILED_PRECONDITION));
  }

  // 3. Corrupt info.magic and recompute valid crcs (should still fail magic
  // check).
  {
    Info bad_magic_info = original_info;
    bad_magic_info.magic = Info::kMagic ^ 0x1234;
    Crcs matching_crcs = original_crcs;
    matching_crcs.component_crcs.info_crc = bad_magic_info.GetChecksum().Get();
    matching_crcs.all_crc = matching_crcs.component_crcs.GetChecksum().Get();
    ASSERT_THAT(filesystem_.PWrite(metadata_sfd.get(),
                                   IdToStringMapper::kCrcsMetadataFileOffset,
                                   &matching_crcs, sizeof(Crcs)),
                IsTrue());
    ASSERT_THAT(filesystem_.PWrite(metadata_sfd.get(),
                                   IdToStringMapper::kInfoMetadataFileOffset,
                                   &bad_magic_info, sizeof(Info)),
                IsTrue());
    EXPECT_THAT(IdToStringMapper::Create(filesystem_, working_path_),
                StatusIs(libtextclassifier3::StatusCode::FAILED_PRECONDITION));
  }

  // 4. Truncate metadata file to an invalid size.
  {
    ASSERT_THAT(filesystem_.Truncate(metadata_sfd.get(),
                                     IdToStringMapper::kMetadataFileSize - 1),
                IsTrue());
    EXPECT_THAT(IdToStringMapper::Create(filesystem_, working_path_),
                StatusIs(libtextclassifier3::StatusCode::FAILED_PRECONDITION));
  }

  // Restore original metadata and verify it loads cleanly.
  ASSERT_THAT(filesystem_.PWrite(metadata_sfd.get(),
                                 IdToStringMapper::kCrcsMetadataFileOffset,
                                 &original_crcs, sizeof(Crcs)),
              IsTrue());
  ASSERT_THAT(filesystem_.PWrite(metadata_sfd.get(),
                                 IdToStringMapper::kInfoMetadataFileOffset,
                                 &original_info, sizeof(Info)),
              IsTrue());
  ICING_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IdToStringMapper> mapper,
      IdToStringMapper::Create(filesystem_, working_path_));
  EXPECT_THAT(mapper->Get(0), IsOkAndHolds("uri0"));
}

TEST_F(PersistentIdToStringMapperTest, PutAndGetSequential) {
  {
    ICING_ASSERT_OK_AND_ASSIGN(
        std::unique_ptr<IdToStringMapper> mapper,
        IdToStringMapper::Create(filesystem_, working_path_));

    ICING_EXPECT_OK(mapper->Put(0, "http://example.com/doc0"));
    ICING_EXPECT_OK(mapper->Put(1, "http://example.com/doc1"));
    ICING_EXPECT_OK(mapper->Put(2, "http://example.com/doc2"));

    EXPECT_THAT(mapper->num_elements(), Eq(3));
    EXPECT_THAT(mapper->num_strings(), Eq(3));
    EXPECT_THAT(mapper->empty(), IsFalse());

    EXPECT_THAT(mapper->Get(0), IsOkAndHolds("http://example.com/doc0"));
    EXPECT_THAT(mapper->Get(1), IsOkAndHolds("http://example.com/doc1"));
    EXPECT_THAT(mapper->Get(2), IsOkAndHolds("http://example.com/doc2"));
    EXPECT_THAT(mapper->Get(3),
                StatusIs(libtextclassifier3::StatusCode::NOT_FOUND));
  }

  // Reopen and verify persistence.
  ICING_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IdToStringMapper> mapper,
      IdToStringMapper::Create(filesystem_, working_path_));
  EXPECT_THAT(mapper->num_elements(), Eq(3));
  EXPECT_THAT(mapper->num_strings(), Eq(3));
  EXPECT_THAT(mapper->Get(0), IsOkAndHolds("http://example.com/doc0"));
  EXPECT_THAT(mapper->Get(1), IsOkAndHolds("http://example.com/doc1"));
  EXPECT_THAT(mapper->Get(2), IsOkAndHolds("http://example.com/doc2"));
}

TEST_F(PersistentIdToStringMapperTest, PutAndGetNonSequentialSkippedIds) {
  ICING_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IdToStringMapper> mapper,
      IdToStringMapper::Create(filesystem_, working_path_));

  // Put id 3 first, skipping 0, 1, 2.
  ICING_ASSERT_OK(mapper->Put(3, "uri3"));
  EXPECT_THAT(mapper->num_elements(), Eq(4));
  EXPECT_THAT(mapper->num_strings(), Eq(1));

  // Skipped ids 0, 1, 2 must return NOT_FOUND (not offset 0).
  EXPECT_THAT(mapper->Get(0),
              StatusIs(libtextclassifier3::StatusCode::NOT_FOUND));
  EXPECT_THAT(mapper->Get(1),
              StatusIs(libtextclassifier3::StatusCode::NOT_FOUND));
  EXPECT_THAT(mapper->Get(2),
              StatusIs(libtextclassifier3::StatusCode::NOT_FOUND));
  EXPECT_THAT(mapper->Get(3), IsOkAndHolds("uri3"));
  EXPECT_THAT(mapper->Get(4),
              StatusIs(libtextclassifier3::StatusCode::NOT_FOUND));

  // Populate skipped id 1.
  ICING_ASSERT_OK(mapper->Put(1, "uri1"));
  EXPECT_THAT(mapper->num_elements(), Eq(4));
  EXPECT_THAT(mapper->num_strings(), Eq(2));
  EXPECT_THAT(mapper->Get(0),
              StatusIs(libtextclassifier3::StatusCode::NOT_FOUND));
  EXPECT_THAT(mapper->Get(1), IsOkAndHolds("uri1"));
  EXPECT_THAT(mapper->Get(2),
              StatusIs(libtextclassifier3::StatusCode::NOT_FOUND));
  EXPECT_THAT(mapper->Get(3), IsOkAndHolds("uri3"));
}

TEST_F(PersistentIdToStringMapperTest,
       PutOverwritesExistingIdAndZeroesOldString) {
  constexpr std::string_view kOldStr = "secret_old_uri";
  constexpr std::string_view kNewLongerStr = "much_longer_replacement_uri";
  {
    ICING_ASSERT_OK_AND_ASSIGN(
        std::unique_ptr<IdToStringMapper> mapper,
        IdToStringMapper::Create(filesystem_, working_path_));

    ICING_ASSERT_OK(mapper->Put(0, kOldStr));
    EXPECT_THAT(mapper->Get(0), IsOkAndHolds(kOldStr));
    EXPECT_THAT(mapper->num_strings(), Eq(1));

    // Overwrite id 0 with a longer string, which zeroes out the old string and
    // appends the new string.
    ICING_ASSERT_OK(mapper->Put(0, kNewLongerStr));
    EXPECT_THAT(mapper->num_elements(), Eq(1));
    EXPECT_THAT(mapper->num_strings(), Eq(1));

    ICING_ASSERT_OK(mapper->PersistToDisk());
  }

  // Inspect str_storage directly to verify the old string bytes were zeroed
  // out and the longer string was appended.
  const std::string str_storage_path =
      absl_ports::StrCat(working_path_, "/str_storage");
  ICING_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<FileBackedVector<char>> str_storage,
      FileBackedVector<char>::Create(filesystem_, str_storage_path,
                                     MemoryMappedFile::Strategy::READ_ONLY,
                                     FileBackedVector<char>::kMaxFileSize,
                                     /*pre_mapping_mmap_size=*/0));

  int32_t old_entry_size = EntrySize(kOldStr);
  ASSERT_THAT(str_storage->num_elements(),
              Eq(old_entry_size + EntrySize(kNewLongerStr)));

  // The old entry (length prefix and string bytes) is fully zeroed out.
  for (int32_t i = 0; i < old_entry_size; ++i) {
    EXPECT_THAT(str_storage->Get(i), IsOkAndHolds(Pointee(Eq('\0'))));
  }
  EXPECT_THAT(ReadEntryStr(*str_storage, old_entry_size), Eq(kNewLongerStr));
}

TEST_F(PersistentIdToStringMapperTest,
       PutOverwritesExistingIdInPlaceWhenEqualLength) {
  constexpr std::string_view kOldStr = "secret_old_uri";
  constexpr std::string_view kEqualLenStr = "equal_size_uri";
  NiceMock<MockFilesystem> mock_filesystem;
  {
    ICING_ASSERT_OK_AND_ASSIGN(
        std::unique_ptr<IdToStringMapper> mapper,
        IdToStringMapper::Create(mock_filesystem, working_path_));

    ICING_ASSERT_OK(mapper->Put(0, kOldStr));
    EXPECT_THAT(mapper->num_strings(), Eq(1));
    ICING_ASSERT_OK(mapper->PersistToDisk());

    // Overwriting with the identical string is a no-op and does not mark the
    // mapper dirty, so PersistToDisk() does not sync any of the underlying
    // files (which would query their file sizes via GetFileSize).
    EXPECT_CALL(mock_filesystem, GetFileSize(A<const char*>())).Times(0);
    ICING_ASSERT_OK(mapper->Put(0, kOldStr));
    EXPECT_THAT(mapper->num_elements(), Eq(1));
    EXPECT_THAT(mapper->num_strings(), Eq(1));
    ICING_ASSERT_OK(mapper->PersistToDisk());
    testing::Mock::VerifyAndClearExpectations(&mock_filesystem);

    // Overwrite id 0 with a different equal-length string in-place. This marks
    // the mapper dirty, so PersistToDisk() syncs all 3 underlying files
    // (metadata, id_to_str_offset, and str_storage).
    EXPECT_CALL(mock_filesystem, GetFileSize(A<const char*>())).Times(3);
    ICING_ASSERT_OK(mapper->Put(0, kEqualLenStr));
    EXPECT_THAT(mapper->num_elements(), Eq(1));
    EXPECT_THAT(mapper->num_strings(), Eq(1));
    ICING_ASSERT_OK(mapper->PersistToDisk());
    testing::Mock::VerifyAndClearExpectations(&mock_filesystem);
  }

  // Inspect str_storage directly to verify the string was overwritten in-place
  // at offset 0 without growing str_storage.
  const std::string str_storage_path =
      absl_ports::StrCat(working_path_, "/str_storage");
  ICING_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<FileBackedVector<char>> str_storage,
      FileBackedVector<char>::Create(filesystem_, str_storage_path,
                                     MemoryMappedFile::Strategy::READ_ONLY,
                                     FileBackedVector<char>::kMaxFileSize,
                                     /*pre_mapping_mmap_size=*/0));

  ASSERT_THAT(str_storage->num_elements(), Eq(EntrySize(kOldStr)));
  EXPECT_THAT(ReadEntryStr(*str_storage, 0), Eq(kEqualLenStr));
}

TEST_F(PersistentIdToStringMapperTest,
       PutOverwritesExistingIdInPlaceWhenShorter) {
  constexpr std::string_view kOldStr = "secret_old_uri";
  constexpr std::string_view kShorterStr = "new_uri";
  {
    ICING_ASSERT_OK_AND_ASSIGN(
        std::unique_ptr<IdToStringMapper> mapper,
        IdToStringMapper::Create(filesystem_, working_path_));

    ICING_ASSERT_OK(mapper->Put(0, kOldStr));
    EXPECT_THAT(mapper->num_strings(), Eq(1));

    // Overwrite id 0 with a shorter string in-place.
    ICING_ASSERT_OK(mapper->Put(0, kShorterStr));
    EXPECT_THAT(mapper->num_elements(), Eq(1));
    EXPECT_THAT(mapper->num_strings(), Eq(1));

    ICING_ASSERT_OK(mapper->PersistToDisk());
  }

  // Inspect str_storage directly to verify the string was overwritten in-place
  // at offset 0 and the leftover tail bytes were zeroed out without growing
  // str_storage.
  const std::string str_storage_path =
      absl_ports::StrCat(working_path_, "/str_storage");
  ICING_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<FileBackedVector<char>> str_storage,
      FileBackedVector<char>::Create(filesystem_, str_storage_path,
                                     MemoryMappedFile::Strategy::READ_ONLY,
                                     FileBackedVector<char>::kMaxFileSize,
                                     /*pre_mapping_mmap_size=*/0));

  int32_t old_entry_size = EntrySize(kOldStr);
  ASSERT_THAT(str_storage->num_elements(), Eq(old_entry_size));

  // The length prefix is updated to the shorter length.
  EXPECT_THAT(ReadEntryLength(*str_storage, 0),
              Eq(static_cast<int32_t>(kShorterStr.length())));
  EXPECT_THAT(ReadEntryStr(*str_storage, 0), Eq(kShorterStr));

  // The leftover tail bytes of the old string are zeroed out.
  for (int32_t i = EntrySize(kShorterStr); i < old_entry_size; ++i) {
    EXPECT_THAT(str_storage->Get(i), IsOkAndHolds(Pointee(Eq('\0'))));
  }
}

TEST_F(PersistentIdToStringMapperTest, PutStringWithEmbeddedNull) {
  constexpr std::string_view kEmbeddedNullStr("ab\0cd", 5);
  constexpr std::string_view kShorterEmbeddedNullStr("\0x\0", 3);
  {
    ICING_ASSERT_OK_AND_ASSIGN(
        std::unique_ptr<IdToStringMapper> mapper,
        IdToStringMapper::Create(filesystem_, working_path_));

    ICING_ASSERT_OK(mapper->Put(0, kEmbeddedNullStr));
    ICING_ASSERT_OK(mapper->Put(1, kEmbeddedNullStr));
    EXPECT_THAT(mapper->num_strings(), Eq(2));

    // Putting the identical string is a no-op, which requires comparing all
    // bytes rather than stopping at the first '\0'.
    ICING_ASSERT_OK(mapper->Put(0, kEmbeddedNullStr));
    // Overwrite id 1 in-place with a shorter string that also contains '\0'.
    ICING_ASSERT_OK(mapper->Put(1, kShorterEmbeddedNullStr));
    EXPECT_THAT(mapper->num_elements(), Eq(2));
    EXPECT_THAT(mapper->num_strings(), Eq(2));

    // Get must return the full stored length, not stop at the first '\0'.
    EXPECT_THAT(mapper->Get(0), IsOkAndHolds(Eq(kEmbeddedNullStr)));
    EXPECT_THAT(mapper->Get(1), IsOkAndHolds(Eq(kShorterEmbeddedNullStr)));

    ICING_ASSERT_OK(mapper->PersistToDisk());
  }

  // Verify the mapper can be reinitialized from the persisted files.
  ICING_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IdToStringMapper> mapper,
      IdToStringMapper::Create(filesystem_, working_path_));
  EXPECT_THAT(mapper->num_strings(), Eq(2));
  EXPECT_THAT(mapper->Get(0), IsOkAndHolds(Eq(kEmbeddedNullStr)));
  EXPECT_THAT(mapper->Get(1), IsOkAndHolds(Eq(kShorterEmbeddedNullStr)));
  mapper.reset();

  const std::string str_storage_path =
      absl_ports::StrCat(working_path_, "/str_storage");
  ICING_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<FileBackedVector<char>> str_storage,
      FileBackedVector<char>::Create(filesystem_, str_storage_path,
                                     MemoryMappedFile::Strategy::READ_ONLY,
                                     FileBackedVector<char>::kMaxFileSize,
                                     /*pre_mapping_mmap_size=*/0));
  ASSERT_THAT(str_storage->num_elements(), Eq(2 * EntrySize(kEmbeddedNullStr)));
  EXPECT_THAT(ReadEntryStr(*str_storage, 0), Eq(kEmbeddedNullStr));
  EXPECT_THAT(ReadEntryStr(*str_storage, EntrySize(kEmbeddedNullStr)),
              Eq(kShorterEmbeddedNullStr));
}

TEST_F(PersistentIdToStringMapperTest, PutInvalidArguments) {
  ICING_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IdToStringMapper> mapper,
      IdToStringMapper::Create(filesystem_, working_path_));

  // Invalid ids.
  EXPECT_THAT(mapper->Put(-1, "uri"),
              StatusIs(libtextclassifier3::StatusCode::INVALID_ARGUMENT));
  EXPECT_THAT(mapper->Put(IdToStringMapper::kMaxId + 1, "uri"),
              StatusIs(libtextclassifier3::StatusCode::INVALID_ARGUMENT));

  EXPECT_THAT(mapper->Get(-1),
              StatusIs(libtextclassifier3::StatusCode::INVALID_ARGUMENT));
  EXPECT_THAT(mapper->Get(IdToStringMapper::kMaxId + 1),
              StatusIs(libtextclassifier3::StatusCode::INVALID_ARGUMENT));

  // Empty string.
  EXPECT_THAT(mapper->Put(0, ""),
              StatusIs(libtextclassifier3::StatusCode::INVALID_ARGUMENT));
}

TEST_F(PersistentIdToStringMapperTest, PutStringTooLongReturnsInvalidArgument) {
#ifndef ICING_ARCH_BIT_64
  GTEST_SKIP() << "Requires a 64-bit address space to reserve a "
                  "kMaxNumElements-byte buffer.";
#else
  ICING_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IdToStringMapper> mapper,
      IdToStringMapper::Create(filesystem_, working_path_));

  // Reserve, but don't commit, ~2 GiB of address space so that the string_view
  // refers to a valid range without allocating 2 GiB of memory. Put should
  // reject the string based on its length alone, before reading any of its
  // bytes. This is the shortest length whose entry (length prefix + bytes)
  // would exceed kMaxNumElements.
  constexpr size_t kTooLongLength =
      FileBackedVector<char>::kMaxNumElements - kStrLengthSize + 1;
  void* buffer = mmap(/*addr=*/nullptr, kTooLongLength, PROT_READ,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, /*fd=*/-1,
                      /*offset=*/0);
  ASSERT_THAT(buffer, Ne(MAP_FAILED));
  libtextclassifier3::Status status = mapper->Put(
      0, std::string_view(static_cast<const char*>(buffer), kTooLongLength));
  munmap(buffer, kTooLongLength);

  EXPECT_THAT(status,
              StatusIs(libtextclassifier3::StatusCode::INVALID_ARGUMENT));
  EXPECT_THAT(status.error_message(), HasSubstr("kMaxNumElements"));
  EXPECT_THAT(mapper->num_elements(), Eq(0));
  EXPECT_THAT(mapper->num_strings(), Eq(0));
#endif  // ICING_ARCH_BIT_64
}

TEST_F(PersistentIdToStringMapperTest, DeleteExistingId) {
  constexpr std::string_view kStr0 = "str_to_be_deleted";
  constexpr std::string_view kStr1 = "str_kept";
  {
    ICING_ASSERT_OK_AND_ASSIGN(
        std::unique_ptr<IdToStringMapper> mapper,
        IdToStringMapper::Create(filesystem_, working_path_));

    ICING_ASSERT_OK(mapper->Put(0, kStr0));
    ICING_ASSERT_OK(mapper->Put(1, kStr1));
    EXPECT_THAT(mapper->num_elements(), Eq(2));
    EXPECT_THAT(mapper->num_strings(), Eq(2));

    // Delete existing id 0.
    ICING_EXPECT_OK(mapper->Delete(0));
    EXPECT_THAT(mapper->Get(0),
                StatusIs(libtextclassifier3::StatusCode::NOT_FOUND));
    EXPECT_THAT(mapper->Get(1), IsOkAndHolds(kStr1));
    EXPECT_THAT(mapper->num_elements(), Eq(2));
    EXPECT_THAT(mapper->num_strings(), Eq(1));

    ICING_ASSERT_OK(mapper->PersistToDisk());
  }

  // Verify the deleted entry's length prefix and string bytes in str_storage
  // were zeroed out on disk.
  const std::string str_storage_path =
      absl_ports::StrCat(working_path_, "/str_storage");
  ICING_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<FileBackedVector<char>> str_storage,
      FileBackedVector<char>::Create(filesystem_, str_storage_path,
                                     MemoryMappedFile::Strategy::READ_ONLY,
                                     FileBackedVector<char>::kMaxFileSize,
                                     /*pre_mapping_mmap_size=*/0));

  for (int32_t i = 0; i < EntrySize(kStr0); ++i) {
    EXPECT_THAT(str_storage->Get(i), IsOkAndHolds(Pointee(Eq('\0'))));
  }
}

TEST_F(PersistentIdToStringMapperTest, DeleteAlreadyDeletedAndSkippedIds) {
  NiceMock<MockFilesystem> mock_filesystem;
  ICING_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IdToStringMapper> mapper,
      IdToStringMapper::Create(mock_filesystem, working_path_));

  ICING_ASSERT_OK(mapper->Put(0, "str_to_be_deleted"));
  ICING_ASSERT_OK(mapper->Put(2, "str_kept"));
  ICING_ASSERT_OK(mapper->Delete(0));
  ICING_ASSERT_OK(mapper->PersistToDisk());

  // Deleting an already-deleted id (0) or skipped id (1) is a no-op that
  // returns OK and does not mark the mapper dirty.
  EXPECT_CALL(mock_filesystem, GetFileSize(A<const char*>())).Times(0);
  ICING_EXPECT_OK(mapper->Delete(0));
  ICING_EXPECT_OK(mapper->Delete(1));
  EXPECT_THAT(mapper->num_elements(), Eq(3));
  EXPECT_THAT(mapper->num_strings(), Eq(1));
  ICING_ASSERT_OK(mapper->PersistToDisk());
  testing::Mock::VerifyAndClearExpectations(&mock_filesystem);
}

TEST_F(PersistentIdToStringMapperTest, DeleteInvalidIds) {
  ICING_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IdToStringMapper> mapper,
      IdToStringMapper::Create(filesystem_, working_path_));

  EXPECT_THAT(mapper->Delete(-1),
              StatusIs(libtextclassifier3::StatusCode::INVALID_ARGUMENT));
  EXPECT_THAT(mapper->Delete(IdToStringMapper::kMaxId + 1),
              StatusIs(libtextclassifier3::StatusCode::INVALID_ARGUMENT));
}

TEST_F(PersistentIdToStringMapperTest, DeleteOutOfRangeIdsIsNoOp) {
  NiceMock<MockFilesystem> mock_filesystem;
  ICING_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IdToStringMapper> mapper,
      IdToStringMapper::Create(mock_filesystem, working_path_));

  // Deleting on an empty mapper is a no-op that returns OK.
  ICING_EXPECT_OK(mapper->Delete(0));
  EXPECT_THAT(mapper->num_elements(), Eq(0));
  EXPECT_THAT(mapper->num_strings(), Eq(0));

  ICING_ASSERT_OK(mapper->Put(0, "str0"));
  EXPECT_THAT(mapper->num_elements(), Eq(1));
  EXPECT_THAT(mapper->num_strings(), Eq(1));
  ICING_ASSERT_OK(mapper->PersistToDisk());

  // Deleting out-of-range valid ids is a no-op that returns OK, does not grow
  // id_to_str_offset, and does not mark the mapper dirty.
  EXPECT_CALL(mock_filesystem, GetFileSize(A<const char*>())).Times(0);
  ICING_EXPECT_OK(mapper->Delete(1));
  ICING_EXPECT_OK(mapper->Delete(5));
  EXPECT_THAT(mapper->num_elements(), Eq(1));
  EXPECT_THAT(mapper->num_strings(), Eq(1));
  EXPECT_THAT(mapper->Get(0), IsOkAndHolds(Eq("str0")));
  EXPECT_THAT(mapper->Get(1),
              StatusIs(libtextclassifier3::StatusCode::NOT_FOUND));
  ICING_ASSERT_OK(mapper->PersistToDisk());
  testing::Mock::VerifyAndClearExpectations(&mock_filesystem);
}

TEST_F(PersistentIdToStringMapperTest, ChecksumsAndDiskUsage) {
  ICING_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IdToStringMapper> mapper,
      IdToStringMapper::Create(filesystem_, working_path_));

  EXPECT_THAT(mapper->GetElementsSize(), IsOkAndHolds(0));
  ICING_ASSERT_OK_AND_ASSIGN(Crc32 initial_crc, mapper->GetChecksum());
  EXPECT_THAT(mapper->UpdateChecksums(), IsOkAndHolds(initial_crc));

  constexpr std::string_view kStr0 = "uri0";
  ICING_ASSERT_OK(mapper->Put(0, kStr0));

  EXPECT_THAT(mapper->GetElementsSize(), IsOkAndHolds(Gt(0)));

  ICING_ASSERT_OK_AND_ASSIGN(Crc32 updated_crc, mapper->GetChecksum());
  EXPECT_THAT(updated_crc, Not(Eq(initial_crc)));
  EXPECT_THAT(mapper->UpdateChecksums(), IsOkAndHolds(updated_crc));

  ICING_ASSERT_OK(mapper->PersistToDisk());

  // Disk usage is block-based and depends on the underlying filesystem, so
  // compare against the sum of the actual on-disk usage of the mapper's files
  // rather than a hardcoded value.
  int64_t expected_disk_usage =
      filesystem_.GetDiskUsage(
          absl_ports::StrCat(working_path_, "/metadata").c_str()) +
      filesystem_.GetDiskUsage(
          absl_ports::StrCat(working_path_, "/id_to_str_offset").c_str()) +
      filesystem_.GetDiskUsage(
          absl_ports::StrCat(working_path_, "/str_storage").c_str());
  ASSERT_THAT(expected_disk_usage, Gt(0));
  EXPECT_THAT(mapper->GetDiskUsage(), IsOkAndHolds(Eq(expected_disk_usage)));
}

TEST_F(PersistentIdToStringMapperTest, GetDiskUsageError) {
  NiceMock<MockFilesystem> mock_filesystem;
  ICING_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IdToStringMapper> mapper,
      IdToStringMapper::Create(mock_filesystem, working_path_));
  ICING_ASSERT_OK(mapper->PersistToDisk());

  const std::string metadata_path =
      absl_ports::StrCat(working_path_, "/metadata");
  const std::string offset_path =
      absl_ports::StrCat(working_path_, "/id_to_str_offset");
  const std::string str_storage_path =
      absl_ports::StrCat(working_path_, "/str_storage");

  // 1. Metadata file GetDiskUsage returns kBadFileSize -> total == kBadFileSize
  ON_CALL(mock_filesystem,
          GetDiskUsage(Matcher<const char*>(StrEq(metadata_path.c_str()))))
      .WillByDefault(Return(Filesystem::kBadFileSize));
  EXPECT_THAT(mapper->GetDiskUsage(),
              StatusIs(libtextclassifier3::StatusCode::INTERNAL));

  // 2. Metadata file GetDiskUsage returns negative value -> total < 0
  ON_CALL(mock_filesystem,
          GetDiskUsage(Matcher<const char*>(StrEq(metadata_path.c_str()))))
      .WillByDefault(Return(-1000000));
  EXPECT_THAT(mapper->GetDiskUsage(),
              StatusIs(libtextclassifier3::StatusCode::INTERNAL));

  // Restore metadata file GetDiskUsage to delegate to real filesystem.
  ON_CALL(mock_filesystem,
          GetDiskUsage(Matcher<const char*>(StrEq(metadata_path.c_str()))))
      .WillByDefault(
          [this](const char* path) { return filesystem_.GetDiskUsage(path); });
  EXPECT_THAT(mapper->GetDiskUsage(), IsOkAndHolds(Gt(0)));

  // 3. id_to_str_offset file GetDiskUsage fails.
  ON_CALL(mock_filesystem,
          GetDiskUsage(Matcher<const char*>(StrEq(offset_path.c_str()))))
      .WillByDefault(Return(Filesystem::kBadFileSize));
  EXPECT_THAT(mapper->GetDiskUsage(),
              StatusIs(libtextclassifier3::StatusCode::INTERNAL));
  ON_CALL(mock_filesystem,
          GetDiskUsage(Matcher<const char*>(StrEq(offset_path.c_str()))))
      .WillByDefault(
          [this](const char* path) { return filesystem_.GetDiskUsage(path); });

  // 4. str_storage file GetDiskUsage fails.
  ON_CALL(mock_filesystem,
          GetDiskUsage(Matcher<const char*>(StrEq(str_storage_path.c_str()))))
      .WillByDefault(Return(Filesystem::kBadFileSize));
  EXPECT_THAT(mapper->GetDiskUsage(),
              StatusIs(libtextclassifier3::StatusCode::INTERNAL));
}

TEST_F(PersistentIdToStringMapperTest, DiscardRemovesDirectory) {
  {
    ICING_ASSERT_OK_AND_ASSIGN(
        std::unique_ptr<IdToStringMapper> mapper,
        IdToStringMapper::Create(filesystem_, working_path_));
    ICING_ASSERT_OK(mapper->Put(0, "uri0"));
  }
  ASSERT_THAT(filesystem_.DirectoryExists(working_path_.c_str()), IsTrue());

  ICING_EXPECT_OK(IdToStringMapper::Discard(filesystem_, working_path_));
  EXPECT_THAT(filesystem_.DirectoryExists(working_path_.c_str()), IsFalse());

  // Discard should also succeed if the directory does not exist.
  ICING_EXPECT_OK(IdToStringMapper::Discard(filesystem_, working_path_));
}

template <typename IdType>
class PersistentIdToStringMapperTypedTest : public ::testing::Test {
 protected:
  void SetUp() override {
    base_dir_ = GetTestTempDir() + "/icing";
    ASSERT_THAT(filesystem_.CreateDirectoryRecursively(base_dir_.c_str()),
                IsTrue());
    working_path_ = base_dir_ + "/persistent_id_to_string_mapper_typed_test";
  }

  void TearDown() override {
    filesystem_.DeleteDirectoryRecursively(base_dir_.c_str());
  }

  Filesystem filesystem_;
  std::string base_dir_;
  std::string working_path_;
};

using IdTypes = ::testing::Types<int8_t, int16_t, int32_t, int64_t>;
TYPED_TEST_SUITE(PersistentIdToStringMapperTypedTest, IdTypes);

TYPED_TEST(PersistentIdToStringMapperTypedTest, PutAndGet) {
  using Mapper = PersistentIdToStringMapper<TypeParam>;
  {
    ICING_ASSERT_OK_AND_ASSIGN(
        std::unique_ptr<Mapper> mapper,
        Mapper::Create(this->filesystem_, this->working_path_));

    ICING_EXPECT_OK(mapper->Put(0, "str0"));
    ICING_EXPECT_OK(mapper->Put(5, "str5"));
    EXPECT_THAT(mapper->Get(0), IsOkAndHolds("str0"));
    EXPECT_THAT(mapper->Get(5), IsOkAndHolds("str5"));
    EXPECT_THAT(mapper->Get(3),
                StatusIs(libtextclassifier3::StatusCode::NOT_FOUND));

    EXPECT_THAT(mapper->Put(-1, "str"),
                StatusIs(libtextclassifier3::StatusCode::INVALID_ARGUMENT));
    EXPECT_THAT(mapper->Get(-1),
                StatusIs(libtextclassifier3::StatusCode::INVALID_ARGUMENT));
    if constexpr (Mapper::kMaxId < std::numeric_limits<TypeParam>::max()) {
      EXPECT_THAT(mapper->Put(Mapper::kMaxId + 1, "str"),
                  StatusIs(libtextclassifier3::StatusCode::INVALID_ARGUMENT));
      EXPECT_THAT(mapper->Get(Mapper::kMaxId + 1),
                  StatusIs(libtextclassifier3::StatusCode::INVALID_ARGUMENT));
    }
    if constexpr (sizeof(TypeParam) <= sizeof(int16_t)) {
      // The full id range is small enough to exercise kMaxId directly.
      ICING_EXPECT_OK(mapper->Put(Mapper::kMaxId, "str_max"));
      EXPECT_THAT(mapper->Get(Mapper::kMaxId), IsOkAndHolds("str_max"));
    }
  }

  // Reopen and verify persistence.
  ICING_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Mapper> mapper,
      Mapper::Create(this->filesystem_, this->working_path_));
  EXPECT_THAT(mapper->Get(0), IsOkAndHolds("str0"));
  EXPECT_THAT(mapper->Get(5), IsOkAndHolds("str5"));
}

}  // namespace

}  // namespace lib
}  // namespace icing
