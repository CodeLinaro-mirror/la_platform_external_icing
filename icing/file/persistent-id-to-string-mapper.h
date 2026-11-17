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

#ifndef ICING_FILE_PERSISTENT_ID_TO_STRING_MAPPER_H_
#define ICING_FILE_PERSISTENT_ID_TO_STRING_MAPPER_H_

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "icing/text_classifier/lib3/utils/base/status.h"
#include "icing/text_classifier/lib3/utils/base/statusor.h"
#include "icing/file/file-backed-vector.h"
#include "icing/file/filesystem.h"
#include "icing/file/memory-mapped-file.h"
#include "icing/file/persistent-storage.h"
#include "icing/util/crc32.h"

namespace icing {
namespace lib {

// PersistentIdToStringMapper: a persistent mapping from a fixed size integer id
// (IdType) to an arbitrary length string. This is the reverse direction of the
// key mappers (e.g. DynamicTrieKeyMapper, PersistentHashMapKeyMapper), which
// map arbitrary length strings to fixed size values.
//
// IdType must be a signed integral type. Explicit instantiations are provided
// for int8_t, int16_t, int32_t and int64_t in
// persistent-id-to-string-mapper.cc. Valid ids are in the range [0, kMaxId].
//
// PersistentIdToStringMapper manages 3 files under working_path:
// - "<working_path>/metadata": contains Crcs and Info sections.
// - "<working_path>/id_to_str_offset": a FileBackedVector<int32_t> mapping
//   each id to the starting byte offset of its string in str_storage_, or
//   kInvalidStrOffset (-1) if the id is deleted or unpopulated.
// - "<working_path>/str_storage": a FileBackedVector<char> storing strings
//   sequentially. Each entry is a length-prefixed byte sequence:
//   [int32_t str_len][str_len bytes]. Strings are not null-terminated, so they
//   may contain arbitrary bytes, including '\0'.
template <typename IdType>
class PersistentIdToStringMapper : public PersistentStorage {
 public:
  static_assert(std::is_integral_v<IdType> && std::is_signed_v<IdType>,
                "IdType must be a signed integral type");

  struct Info {
    static constexpr int32_t kMagic = 0x5d198a3f;

    int32_t magic;
    int32_t num_strings;

    Crc32 GetChecksum() const {
      return Crc32(
          std::string_view(reinterpret_cast<const char*>(this), sizeof(Info)));
    }
  } __attribute__((packed));
  static_assert(sizeof(Info) == 8, "");

  // Metadata file layout: <Crcs><Info>
  static constexpr int32_t kCrcsMetadataFileOffset = 0;
  static constexpr int32_t kInfoMetadataFileOffset =
      static_cast<int32_t>(sizeof(Crcs));
  static constexpr int32_t kMetadataFileSize = sizeof(Crcs) + sizeof(Info);
  static_assert(kMetadataFileSize == 20, "");

  static constexpr int32_t kInvalidStrOffset = -1;

  // Size of the length prefix stored before each string in str_storage_.
  static constexpr int32_t kStrLengthSize = sizeof(int32_t);

  // Max valid id. Ids are used as indices of id_to_str_offset_, so they
  // are also bounded by the max # of elements of FileBackedVector.
  static constexpr IdType kMaxId = static_cast<IdType>(
      std::min<int64_t>(std::numeric_limits<IdType>::max(),
                        FileBackedVector<int32_t>::kMaxNumElements - 1));

  static constexpr WorkingPathType kWorkingPathType =
      WorkingPathType::kDirectory;

  // Creates or reloads a PersistentIdToStringMapper at working_path.
  //
  // filesystem: Object to make system level calls
  // working_path: Directory path where metadata and storage files are kept.
  //
  // Returns:
  //   - A PersistentIdToStringMapper instance on success
  //   - FAILED_PRECONDITION_ERROR if the file checksum doesn't match the stored
  //     checksum, magic mismatches, or files are in an inconsistent state
  //   - INTERNAL_ERROR on I/O errors
  //   - Any FileBackedVector errors
  static libtextclassifier3::StatusOr<
      std::unique_ptr<PersistentIdToStringMapper>>
  Create(const Filesystem& filesystem, std::string working_path);

  // Deletes PersistentIdToStringMapper under working_path.
  //
  // Returns:
  //   - OK on success
  //   - INTERNAL_ERROR on I/O error
  static libtextclassifier3::Status Discard(const Filesystem& filesystem,
                                            const std::string& working_path) {
    return PersistentStorage::Discard(filesystem, working_path,
                                      kWorkingPathType);
  }

  ~PersistentIdToStringMapper() override;

  PersistentIdToStringMapper(const PersistentIdToStringMapper&) = delete;
  PersistentIdToStringMapper& operator=(const PersistentIdToStringMapper&) =
      delete;

  // Associates id with str.
  // - If id > num_elements(), intermediate unpopulated ids in
  //   [num_elements(), id) are initialized to kInvalidStrOffset (-1).
  // - If id already has a mapped string:
  //   - If str fits within the existing string's length, it is overwritten
  //     in-place (including its length prefix) and any trailing bytes are
  //     zeroed out.
  //   - Otherwise, the old entry (length prefix and string bytes) in
  //     str_storage_ is zeroed out and str is appended at a new offset.
  //
  // str may contain arbitrary bytes, including '\0'.
  //
  // Returns:
  //   - OK on success
  //   - INVALID_ARGUMENT_ERROR if id is invalid, or if str is empty or too long
  //   - INTERNAL_ERROR if the existing entry for id is corrupted
  //   - Any FileBackedVector errors
  libtextclassifier3::Status Put(IdType id, std::string_view str);

  // Retrieves the string associated with id.
  //
  // Note: The returned std::string_view points directly into memory-mapped
  // storage and is only valid until the next mutating call (Put or Delete) or
  // until this PersistentIdToStringMapper instance is destroyed.
  //
  // Returns:
  //   - std::string_view on success
  //   - INVALID_ARGUMENT_ERROR if id is invalid
  //   - NOT_FOUND_ERROR if id has been deleted or not populated (including
  //     id >= num_elements())
  //   - INTERNAL_ERROR on data corruption
  //   - Any FileBackedVector errors
  libtextclassifier3::StatusOr<std::string_view> Get(IdType id) const;

  // Deletes the string mapping for id and zeroes out the old string bytes in
  // str_storage_.
  // - If id is already deleted or has never been populated (including
  //   id >= num_elements()), this is a no-op that returns OK (idempotent).
  //
  // Returns:
  //   - OK on success
  //   - INVALID_ARGUMENT_ERROR if id is invalid
  //   - INTERNAL_ERROR on data corruption
  //   - Any FileBackedVector errors
  libtextclassifier3::Status Delete(IdType id);

  // Calculates and returns the disk usage in bytes.
  //
  // Returns:
  //   - Disk usage on success
  //   - INTERNAL_ERROR on I/O error
  libtextclassifier3::StatusOr<int64_t> GetDiskUsage() const;

  // Returns the total size of the elements held in the mapper in bytes. This
  // excludes the size of any internal metadata (e.g. crcs/info and
  // FileBackedVector headers).
  //
  // Returns:
  //   - Elements byte size on success
  //   - INTERNAL_ERROR on I/O error
  libtextclassifier3::StatusOr<int64_t> GetElementsSize() const;

  // Returns the number of elements in the id_to_str_offset_ vector, which
  // is the maximum id that can be used.
  // There will at least be as many elements as num_strings(). There could be
  // more if there are deleted strings.
  int32_t num_elements() const { return id_to_str_offset_->num_elements(); }

  // Returns the number of strings stored in the mapper.
  int32_t num_strings() const { return info().num_strings; }

  bool empty() const { return num_strings() == 0; }

 private:
  explicit PersistentIdToStringMapper(
      const Filesystem& filesystem, std::string&& working_path,
      std::unique_ptr<MemoryMappedFile> metadata_mmapped_file,
      std::unique_ptr<FileBackedVector<int32_t>> id_to_str_offset,
      std::unique_ptr<FileBackedVector<char>> str_storage)
      : PersistentStorage(filesystem, std::move(working_path),
                          kWorkingPathType),
        metadata_mmapped_file_(std::move(metadata_mmapped_file)),
        id_to_str_offset_(std::move(id_to_str_offset)),
        str_storage_(std::move(str_storage)),
        is_info_dirty_(false),
        is_storage_dirty_(false) {}

  static libtextclassifier3::StatusOr<
      std::unique_ptr<PersistentIdToStringMapper>>
  InitializeNewFiles(const Filesystem& filesystem, std::string&& working_path);

  static libtextclassifier3::StatusOr<
      std::unique_ptr<PersistentIdToStringMapper>>
  InitializeExistingFiles(const Filesystem& filesystem,
                          std::string&& working_path);

  // Extends id_to_str_offset_ up to id + 1 if necessary,
  // filling any newly allocated slots with kInvalidStrOffset (-1).
  libtextclassifier3::Status ExtendIdToStrOffsetIfNecessary(IdType id);

  // Reads the length prefix of the str_storage_ entry starting at offset.
  //
  // Returns:
  //   - The string length (> 0) on success
  //   - INTERNAL_ERROR if offset or the stored length is out of bounds
  libtextclassifier3::StatusOr<int32_t> GetStrLength(int32_t offset) const;

  libtextclassifier3::Status PersistStoragesToDisk() override;

  libtextclassifier3::Status PersistMetadataToDisk() override;

  libtextclassifier3::Status WriteMetadata() override {
    // PersistentIdToStringMapper metadata is mmapped. Therefore, writes occur
    // when the metadata is modified. So just return OK.
    return libtextclassifier3::Status::OK;
  }

  libtextclassifier3::StatusOr<Crc32> UpdateStoragesChecksum() override;

  libtextclassifier3::StatusOr<Crc32> GetInfoChecksum() const override;

  libtextclassifier3::StatusOr<Crc32> GetStoragesChecksum() const override;

  Crcs& crcs() override {
    return *reinterpret_cast<Crcs*>(metadata_mmapped_file_->mutable_region() +
                                    kCrcsMetadataFileOffset);
  }

  const Crcs& crcs() const override {
    return *reinterpret_cast<const Crcs*>(metadata_mmapped_file_->region() +
                                          kCrcsMetadataFileOffset);
  }

  Info& info() {
    return *reinterpret_cast<Info*>(metadata_mmapped_file_->mutable_region() +
                                    kInfoMetadataFileOffset);
  }

  const Info& info() const {
    return *reinterpret_cast<const Info*>(metadata_mmapped_file_->region() +
                                          kInfoMetadataFileOffset);
  }

  void SetInfoDirty() { is_info_dirty_ = true; }

  void SetDirty() {
    SetInfoDirty();
    is_storage_dirty_ = true;
  }

  bool is_info_dirty() const { return is_info_dirty_; }
  bool is_storage_dirty() const { return is_storage_dirty_; }

  static bool IsIdValid(IdType id) { return id >= 0 && id <= kMaxId; }

  std::unique_ptr<MemoryMappedFile> metadata_mmapped_file_;
  std::unique_ptr<FileBackedVector<int32_t>> id_to_str_offset_;
  std::unique_ptr<FileBackedVector<char>> str_storage_;

  bool is_info_dirty_;
  bool is_storage_dirty_;
};

}  // namespace lib
}  // namespace icing

#endif  // ICING_FILE_PERSISTENT_ID_TO_STRING_MAPPER_H_
