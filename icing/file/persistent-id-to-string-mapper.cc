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

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "icing/text_classifier/lib3/utils/base/status.h"
#include "icing/text_classifier/lib3/utils/base/statusor.h"
#include "icing/absl_ports/canonical_errors.h"
#include "icing/absl_ports/str_cat.h"
#include "icing/file/file-backed-vector.h"
#include "icing/file/filesystem.h"
#include "icing/file/memory-mapped-file.h"
#include "icing/util/crc32.h"
#include "icing/util/logging.h"
#include "icing/util/status-macros.h"

namespace icing {
namespace lib {

namespace {

std::string MakeMetadataFilePath(std::string_view working_path) {
  return absl_ports::StrCat(working_path, "/metadata");
}

std::string MakeIdToStrOffsetFilePath(std::string_view working_path) {
  return absl_ports::StrCat(working_path, "/id_to_str_offset");
}

std::string MakeStrStorageFilePath(std::string_view working_path) {
  return absl_ports::StrCat(working_path, "/str_storage");
}

libtextclassifier3::Status ValidateString(std::string_view str,
                                          int32_t str_length_size) {
  if (str.empty()) {
    return absl_ports::InvalidArgumentError("String cannot be empty");
  }
  // Each string is stored with a length prefix, so the total entry size must
  // fit in str_storage_.
  if (str.length() >
      static_cast<size_t>(FileBackedVector<char>::kMaxNumElements -
                          str_length_size)) {
    return absl_ports::InvalidArgumentError(
        "String cannot be longer than kMaxNumElements - kStrLengthSize");
  }
  return libtextclassifier3::Status::OK;
}

}  // namespace

template <typename IdType>
libtextclassifier3::StatusOr<int32_t>
PersistentIdToStringMapper<IdType>::GetStrLength(int32_t offset) const {
  if (offset < 0 || offset > str_storage_->num_elements() - kStrLengthSize) {
    return absl_ports::InternalError(
        "Corrupted string offset in PersistentIdToStringMapper");
  }
  int32_t str_len;
  // The length prefix is not guaranteed to be aligned, so memcpy it out.
  std::memcpy(&str_len, str_storage_->array() + offset, kStrLengthSize);
  if (str_len <= 0 ||
      str_len > str_storage_->num_elements() - offset - kStrLengthSize) {
    return absl_ports::InternalError(
        "Corrupted string length in PersistentIdToStringMapper");
  }
  return str_len;
}

template <typename IdType>
/* static */ libtextclassifier3::StatusOr<
    std::unique_ptr<PersistentIdToStringMapper<IdType>>>
PersistentIdToStringMapper<IdType>::Create(const Filesystem& filesystem,
                                           std::string working_path) {
  bool metadata_file_exists =
      filesystem.FileExists(MakeMetadataFilePath(working_path).c_str());
  bool id_to_str_offset_file_exists =
      filesystem.FileExists(MakeIdToStrOffsetFilePath(working_path).c_str());
  bool str_storage_file_exists =
      filesystem.FileExists(MakeStrStorageFilePath(working_path).c_str());

  // If all files exist, initialize from existing files.
  if (metadata_file_exists && id_to_str_offset_file_exists &&
      str_storage_file_exists) {
    return InitializeExistingFiles(filesystem, std::move(working_path));
  }

  // If none of the files exist, initialize new files.
  if (!metadata_file_exists && !id_to_str_offset_file_exists &&
      !str_storage_file_exists) {
    return InitializeNewFiles(filesystem, std::move(working_path));
  }

  // Otherwise, some files are missing and the mapper is in an inconsistent
  // state. Return error to let the caller discard and rebuild.
  return absl_ports::FailedPreconditionError(
      "Inconsistent state of PersistentIdToStringMapper");
}

template <typename IdType>
PersistentIdToStringMapper<IdType>::~PersistentIdToStringMapper() {
  if (!PersistToDisk().ok()) {
    ICING_LOG(WARNING) << "Failed to persist PersistentIdToStringMapper to "
                          "disk while destructing "
                       << working_path_;
  }
}

template <typename IdType>
libtextclassifier3::Status PersistentIdToStringMapper<IdType>::Put(
    IdType id, std::string_view str) {
  if (!IsIdValid(id)) {
    return absl_ports::InvalidArgumentError(
        absl_ports::StrCat("Id ", std::to_string(id), " is invalid"));
  }
  ICING_RETURN_IF_ERROR(ValidateString(str, kStrLengthSize));

  int32_t str_len = static_cast<int32_t>(str.length());
  int32_t old_offset = kInvalidStrOffset;
  int32_t old_str_len = 0;
  // Check if the id is already populated and if so, whether the new string can
  // be placed in the existing slot.
  if (id < id_to_str_offset_->num_elements()) {
    ICING_ASSIGN_OR_RETURN(const int32_t* old_offset_ptr,
                           id_to_str_offset_->Get(id));
    old_offset = *old_offset_ptr;
    if (old_offset != kInvalidStrOffset) {
      ICING_ASSIGN_OR_RETURN(old_str_len, GetStrLength(old_offset));
      // If the new string is the same as the old string, we can return early
      // and avoid any disk writes.
      if (std::string_view(str_storage_->array() + old_offset + kStrLengthSize,
                           old_str_len) == str) {
        return libtextclassifier3::Status::OK;
      }

      if (str_len <= old_str_len) {
        SetDirty();
        // Reuse the existing slot in-place: overwrite the length prefix and
        // the string bytes, then zero out any remaining tail bytes of the old
        // string.
        ICING_ASSIGN_OR_RETURN(
            FileBackedVector<char>::MutableArrayView mutable_arr,
            str_storage_->GetMutable(old_offset, kStrLengthSize + old_str_len));
        mutable_arr.SetArray(/*idx=*/0, reinterpret_cast<const char*>(&str_len),
                             kStrLengthSize);
        mutable_arr.SetArray(/*idx=*/kStrLengthSize, str.data(), str_len);
        mutable_arr.Fill(/*idx=*/kStrLengthSize + str_len,
                         /*len=*/old_str_len - str_len, '\0');
        return libtextclassifier3::Status::OK;
      }
    }
  }
  // Either the id is not in the mapper or the new string is longer than the
  // existing string. We will need to allocate a new slot for the string.
  SetDirty();
  ICING_RETURN_IF_ERROR(ExtendIdToStrOffsetIfNecessary(id));

  // Allocate space for the new string and copy it in at the new offset before
  // zeroing out any old string, so that if Allocate fails, the existing mapping
  // remains intact.
  int32_t new_offset = str_storage_->num_elements();
  ICING_ASSIGN_OR_RETURN(FileBackedVector<char>::MutableArrayView mutable_arr,
                         str_storage_->Allocate(kStrLengthSize + str_len));
  mutable_arr.SetArray(/*idx=*/0, reinterpret_cast<const char*>(&str_len),
                       kStrLengthSize);
  mutable_arr.SetArray(/*idx=*/kStrLengthSize, str.data(), str_len);

  if (old_offset != kInvalidStrOffset) {
    // If there was an old string, zero out its length prefix and bytes.
    ICING_RETURN_IF_ERROR(
        str_storage_->Set(old_offset, kStrLengthSize + old_str_len, '\0'));
  } else {
    // If there was no old string, this is a new string being added to the
    // mapper. Increment the number of strings.
    ++info().num_strings;
  }

  ICING_RETURN_IF_ERROR(id_to_str_offset_->Set(id, new_offset));

  return libtextclassifier3::Status::OK;
}

template <typename IdType>
libtextclassifier3::StatusOr<std::string_view>
PersistentIdToStringMapper<IdType>::Get(IdType id) const {
  if (!IsIdValid(id)) {
    return absl_ports::InvalidArgumentError(
        absl_ports::StrCat("Id ", std::to_string(id), " is invalid"));
  }
  // An id beyond the end of id_to_str_offset_ has simply never been populated,
  // which is indistinguishable (to the caller) from a skipped or deleted id.
  if (id >= id_to_str_offset_->num_elements()) {
    return absl_ports::NotFoundError(absl_ports::StrCat(
        "Id ", std::to_string(id), " not found in PersistentIdToStringMapper"));
  }

  ICING_ASSIGN_OR_RETURN(const int32_t* offset_ptr, id_to_str_offset_->Get(id));
  int32_t offset = *offset_ptr;
  if (offset == kInvalidStrOffset) {
    return absl_ports::NotFoundError(absl_ports::StrCat(
        "Id ", std::to_string(id), " not found in PersistentIdToStringMapper"));
  }
  // Strings are stored as [length prefix][bytes] and are not null-terminated.
  // GetStrLength validates both the offset and the stored length.
  ICING_ASSIGN_OR_RETURN(int32_t str_len, GetStrLength(offset));
  return std::string_view(str_storage_->array() + offset + kStrLengthSize,
                          str_len);
}

template <typename IdType>
libtextclassifier3::Status PersistentIdToStringMapper<IdType>::Delete(
    IdType id) {
  if (!IsIdValid(id)) {
    return absl_ports::InvalidArgumentError(
        absl_ports::StrCat("Id ", std::to_string(id), " is invalid"));
  }

  // An id beyond the end of id_to_str_offset_ has never been populated, so
  // there is nothing to delete. Treat it the same as an already-deleted id.
  if (id >= id_to_str_offset_->num_elements()) {
    return libtextclassifier3::Status::OK;
  }

  ICING_ASSIGN_OR_RETURN(const int32_t* offset_ptr, id_to_str_offset_->Get(id));
  int32_t offset = *offset_ptr;
  if (offset == kInvalidStrOffset) {
    // Id is already deleted. Return OK.
    return libtextclassifier3::Status::OK;
  }
  // GetStrLength validates both the offset and the stored length.
  ICING_ASSIGN_OR_RETURN(int32_t str_len, GetStrLength(offset));

  SetDirty();

  // Zero out the length prefix and the string bytes.
  ICING_RETURN_IF_ERROR(
      str_storage_->Set(offset, kStrLengthSize + str_len, '\0'));
  ICING_RETURN_IF_ERROR(id_to_str_offset_->Set(id, kInvalidStrOffset));
  --info().num_strings;

  return libtextclassifier3::Status::OK;
}

template <typename IdType>
libtextclassifier3::StatusOr<int64_t>
PersistentIdToStringMapper<IdType>::GetDiskUsage() const {
  ICING_ASSIGN_OR_RETURN(int64_t offset_disk_usage,
                         id_to_str_offset_->GetDiskUsage());
  ICING_ASSIGN_OR_RETURN(int64_t str_disk_usage, str_storage_->GetDiskUsage());

  int64_t total = offset_disk_usage + str_disk_usage;
  Filesystem::IncrementByOrSetInvalid(
      filesystem_.GetDiskUsage(MakeMetadataFilePath(working_path_).c_str()),
      &total);
  if (total < 0 || total == Filesystem::kBadFileSize) {
    return absl_ports::InternalError(
        "Failed to get disk usage of PersistentIdToStringMapper");
  }
  return total;
}

template <typename IdType>
libtextclassifier3::StatusOr<int64_t>
PersistentIdToStringMapper<IdType>::GetElementsSize() const {
  ICING_ASSIGN_OR_RETURN(int64_t offset_elements_size,
                         id_to_str_offset_->GetElementsFileSize());
  ICING_ASSIGN_OR_RETURN(int64_t str_elements_size,
                         str_storage_->GetElementsFileSize());
  return offset_elements_size + str_elements_size;
}

template <typename IdType>
/* static */ libtextclassifier3::StatusOr<
    std::unique_ptr<PersistentIdToStringMapper<IdType>>>
PersistentIdToStringMapper<IdType>::InitializeNewFiles(
    const Filesystem& filesystem, std::string&& working_path) {
  if (!filesystem.CreateDirectoryRecursively(working_path.c_str())) {
    return absl_ports::InternalError(
        absl_ports::StrCat("Failed to create directory: ", working_path));
  }

  ICING_ASSIGN_OR_RETURN(
      MemoryMappedFile metadata_mmapped_file,
      MemoryMappedFile::Create(filesystem, MakeMetadataFilePath(working_path),
                               MemoryMappedFile::Strategy::READ_WRITE_AUTO_SYNC,
                               /*max_file_size=*/kMetadataFileSize,
                               /*pre_mapping_file_offset=*/0,
                               /*pre_mapping_mmap_size=*/kMetadataFileSize));
  // GrowAndRemapIfNecessary zero-initializes the underlying file, and all Info
  // and Crcs fields are explicitly populated below.
  ICING_RETURN_IF_ERROR(metadata_mmapped_file.GrowAndRemapIfNecessary(
      /*file_offset=*/0, /*mmap_size=*/kMetadataFileSize));

  // By default, FileBackedVector::Create uses max_file_size = kMaxFileSize and
  // pre_mapping_mmap_size = 0.
  ICING_ASSIGN_OR_RETURN(
      std::unique_ptr<FileBackedVector<int32_t>> id_to_str_offset,
      FileBackedVector<int32_t>::Create(
          filesystem, MakeIdToStrOffsetFilePath(working_path),
          MemoryMappedFile::Strategy::READ_WRITE_AUTO_SYNC));

  ICING_ASSIGN_OR_RETURN(std::unique_ptr<FileBackedVector<char>> str_storage,
                         FileBackedVector<char>::Create(
                             filesystem, MakeStrStorageFilePath(working_path),
                             MemoryMappedFile::Strategy::READ_WRITE_AUTO_SYNC));

  auto new_mapper = std::unique_ptr<PersistentIdToStringMapper<IdType>>(
      new PersistentIdToStringMapper<IdType>(
          filesystem, std::move(working_path),
          std::make_unique<MemoryMappedFile>(std::move(metadata_mmapped_file)),
          std::move(id_to_str_offset), std::move(str_storage)));
  new_mapper->info().magic = Info::kMagic;
  new_mapper->info().num_strings = 0;

  ICING_RETURN_IF_ERROR(new_mapper->InitializeNewStorage());

  return new_mapper;
}

template <typename IdType>
/* static */ libtextclassifier3::StatusOr<
    std::unique_ptr<PersistentIdToStringMapper<IdType>>>
PersistentIdToStringMapper<IdType>::InitializeExistingFiles(
    const Filesystem& filesystem, std::string&& working_path) {
  ICING_ASSIGN_OR_RETURN(
      MemoryMappedFile metadata_mmapped_file,
      MemoryMappedFile::Create(filesystem, MakeMetadataFilePath(working_path),
                               MemoryMappedFile::Strategy::READ_WRITE_AUTO_SYNC,
                               /*max_file_size=*/kMetadataFileSize,
                               /*pre_mapping_file_offset=*/0,
                               /*pre_mapping_mmap_size=*/kMetadataFileSize));
  if (metadata_mmapped_file.available_size() != kMetadataFileSize) {
    return absl_ports::FailedPreconditionError("Incorrect metadata file size");
  }

  // By default, FileBackedVector::Create uses max_file_size = kMaxFileSize and
  // pre_mapping_mmap_size = 0.
  ICING_ASSIGN_OR_RETURN(
      std::unique_ptr<FileBackedVector<int32_t>> id_to_str_offset,
      FileBackedVector<int32_t>::Create(
          filesystem, MakeIdToStrOffsetFilePath(working_path),
          MemoryMappedFile::Strategy::READ_WRITE_AUTO_SYNC));

  ICING_ASSIGN_OR_RETURN(std::unique_ptr<FileBackedVector<char>> str_storage,
                         FileBackedVector<char>::Create(
                             filesystem, MakeStrStorageFilePath(working_path),
                             MemoryMappedFile::Strategy::READ_WRITE_AUTO_SYNC));

  auto mapper = std::unique_ptr<PersistentIdToStringMapper<IdType>>(
      new PersistentIdToStringMapper<IdType>(
          filesystem, std::move(working_path),
          std::make_unique<MemoryMappedFile>(std::move(metadata_mmapped_file)),
          std::move(id_to_str_offset), std::move(str_storage)));

  ICING_RETURN_IF_ERROR(mapper->InitializeExistingStorage());

  if (mapper->info().magic != Info::kMagic) {
    ICING_LOG(ERROR) << "Invalid header magic for PersistentIdToStringMapper "
                     << mapper->working_path_ << ". Expected: " << Info::kMagic
                     << ", actual: " << mapper->info().magic;
    return absl_ports::FailedPreconditionError(absl_ports::StrCat(
        "Invalid header magic for PersistentIdToStringMapper: ",
        mapper->working_path_));
  }

  return mapper;
}

template <typename IdType>
libtextclassifier3::Status
PersistentIdToStringMapper<IdType>::ExtendIdToStrOffsetIfNecessary(IdType id) {
  if (id >= id_to_str_offset_->num_elements()) {
    int32_t num_to_extend = (id + 1) - id_to_str_offset_->num_elements();
    ICING_ASSIGN_OR_RETURN(
        FileBackedVector<int32_t>::MutableArrayView mutable_arr,
        id_to_str_offset_->Allocate(num_to_extend));
    mutable_arr.Fill(/*idx=*/0, /*len=*/num_to_extend, kInvalidStrOffset);
  }
  return libtextclassifier3::Status::OK;
}

template <typename IdType>
libtextclassifier3::Status
PersistentIdToStringMapper<IdType>::PersistMetadataToDisk() {
  if (is_initialized_ && !is_info_dirty() && !is_storage_dirty()) {
    return libtextclassifier3::Status::OK;
  }

  ICING_RETURN_IF_ERROR(metadata_mmapped_file_->PersistToDisk());
  is_info_dirty_ = false;
  return libtextclassifier3::Status::OK;
}

template <typename IdType>
libtextclassifier3::Status
PersistentIdToStringMapper<IdType>::PersistStoragesToDisk() {
  if (is_initialized_ && !is_storage_dirty()) {
    return libtextclassifier3::Status::OK;
  }

  ICING_RETURN_IF_ERROR(id_to_str_offset_->PersistToDisk());
  ICING_RETURN_IF_ERROR(str_storage_->PersistToDisk());
  is_storage_dirty_ = false;
  return libtextclassifier3::Status::OK;
}

template <typename IdType>
libtextclassifier3::StatusOr<Crc32>
PersistentIdToStringMapper<IdType>::UpdateStoragesChecksum() {
  if (is_initialized_ && !is_storage_dirty()) {
    return Crc32(crcs().component_crcs.storages_crc);
  }

  ICING_ASSIGN_OR_RETURN(Crc32 offset_crc, id_to_str_offset_->UpdateChecksum());
  ICING_ASSIGN_OR_RETURN(Crc32 str_crc, str_storage_->UpdateChecksum());

  return Crc32(offset_crc.Get() ^ str_crc.Get());
}

template <typename IdType>
libtextclassifier3::StatusOr<Crc32>
PersistentIdToStringMapper<IdType>::GetInfoChecksum() const {
  if (is_initialized_ && !is_info_dirty()) {
    return Crc32(crcs().component_crcs.info_crc);
  }

  return info().GetChecksum();
}

template <typename IdType>
libtextclassifier3::StatusOr<Crc32>
PersistentIdToStringMapper<IdType>::GetStoragesChecksum() const {
  if (is_initialized_ && !is_storage_dirty()) {
    return Crc32(crcs().component_crcs.storages_crc);
  }

  Crc32 offset_crc = id_to_str_offset_->GetChecksum();
  Crc32 str_crc = str_storage_->GetChecksum();

  return Crc32(offset_crc.Get() ^ str_crc.Get());
}

template class PersistentIdToStringMapper<int8_t>;
template class PersistentIdToStringMapper<int16_t>;
template class PersistentIdToStringMapper<int32_t>;
template class PersistentIdToStringMapper<int64_t>;

}  // namespace lib
}  // namespace icing
