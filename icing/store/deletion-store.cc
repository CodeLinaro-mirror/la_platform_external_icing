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

#include "icing/text_classifier/lib3/utils/base/status.h"
#include "icing/text_classifier/lib3/utils/base/statusor.h"
#include "icing/absl_ports/annotate.h"
#include "icing/absl_ports/str_cat.h"
#include "icing/file/file-backed-vector.h"
#include "icing/file/filesystem.h"
#include "icing/file/memory-mapped-file.h"
#include "icing/store/deletion-index-entry.h"
#include "icing/store/deletion-log-creator.h"
#include "icing/util/logging.h"
#include "icing/util/status-macros.h"

namespace icing {
namespace lib {

namespace {

constexpr char kDeletionIndexFilename[] = "deletion_index";

std::string MakeDeletionIndexFilename(const std::string& base_dir) {
  return absl_ports::StrCat(base_dir, "/", kDeletionIndexFilename);
}

}  // namespace

std::string DeletionStore::GetDeletionIndexFilename() {
  return kDeletionIndexFilename;
}

libtextclassifier3::StatusOr<DeletionStore::CreateResult> DeletionStore::Create(
    const Filesystem* filesystem, const std::string& base_dir,
    int32_t compression_level, uint32_t compression_threshold_bytes,
    int32_t compression_mem_level) {
  ICING_RETURN_ERROR_IF_NULL(filesystem);

  auto deletion_store = std::unique_ptr<DeletionStore>(
      new DeletionStore(filesystem, base_dir, compression_level,
                        compression_threshold_bytes, compression_mem_level));
  ICING_ASSIGN_OR_RETURN(InitializeResult initialize_result,
                         deletion_store->Initialize());

  CreateResult create_result;
  create_result.deletion_store = std::move(deletion_store);
  create_result.data_loss = initialize_result.data_loss;
  return create_result;
}

DeletionStore::DeletionStore(const Filesystem* filesystem, std::string base_dir,
                             int32_t compression_level,
                             uint32_t compression_threshold_bytes,
                             int32_t compression_mem_level)
    : filesystem_(filesystem),
      base_dir_(std::move(base_dir)),
      compression_level_(compression_level),
      compression_threshold_bytes_(compression_threshold_bytes),
      compression_mem_level_(compression_mem_level) {}

DeletionStore::~DeletionStore() {
  if (initialized_) {
    if (!PersistToDisk().ok()) {
      ICING_LOG(ERROR)
          << "Error persisting to disk in DeletionStore destructor";
    }
  }
}

libtextclassifier3::StatusOr<DeletionStore::InitializeResult>
DeletionStore::Initialize() {
  auto create_result_or = DeletionLogCreator::Create(
      filesystem_, base_dir_, compression_level_, compression_threshold_bytes_,
      compression_mem_level_);
  if (!create_result_or.ok()) {
    return absl_ports::Annotate(create_result_or.status(),
                                "Failed to initialize DeletionLog.");
  }
  DeletionLogCreator::CreateResult create_result =
      std::move(create_result_or).ValueOrDie();
  deletion_log_ = std::move(create_result.log_create_result.proto_log);

  // TODO(b/568469091): Initialize the DeletionIndex from existing files and
  // repair it against the DeletionLog instead of always creating a new one.
  libtextclassifier3::Status status = ResetDeletionIndex();
  if (!status.ok()) {
    return absl_ports::Annotate(status, "Failed to initialize DeletionIndex.");
  }

  initialized_ = true;
  return InitializeResult{.data_loss =
                              create_result.log_create_result.data_loss};
}

libtextclassifier3::Status DeletionStore::ResetDeletionIndex() {
  deletion_index_.reset();
  ICING_RETURN_IF_ERROR(FileBackedVector<DeletionIndexEntry>::Delete(
      *filesystem_, MakeDeletionIndexFilename(base_dir_)));
  ICING_ASSIGN_OR_RETURN(deletion_index_,
                         FileBackedVector<DeletionIndexEntry>::Create(
                             *filesystem_, MakeDeletionIndexFilename(base_dir_),
                             MemoryMappedFile::READ_WRITE_AUTO_SYNC));
  return libtextclassifier3::Status::OK;
}

libtextclassifier3::Status DeletionStore::PersistToDisk() {
  ICING_RETURN_IF_ERROR(deletion_log_->PersistToDisk());
  return deletion_index_->PersistToDisk();
}

}  // namespace lib
}  // namespace icing
