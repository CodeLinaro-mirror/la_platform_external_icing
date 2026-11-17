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
// - "<working_path>/str_storage": a FileBackedVector<char> storing
//   null-terminated C-strings sequentially.
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

  // Max valid id. Ids are used as indices of id_to_str_offset_, so they
  // are also bounded by the max # of elements of FileBackedVector.
  static constexpr IdType kMaxId = static_cast<IdType>(
      std::min<int64_t>(std::numeric_limits<IdType>::max(),
                        FileBackedVector<int32_t>::kMaxNumElements - 1));

  static constexpr WorkingPathType kWorkingPathType =
      WorkingPathType::kDirectory;

  PersistentIdToStringMapper(const PersistentIdToStringMapper&) = delete;
  PersistentIdToStringMapper& operator=(const PersistentIdToStringMapper&) =
      delete;

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
