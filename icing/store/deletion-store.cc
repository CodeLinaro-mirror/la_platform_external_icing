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

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "icing/text_classifier/lib3/utils/base/status.h"
#include "icing/text_classifier/lib3/utils/base/statusor.h"
#include "icing/text_classifier/lib3/utils/hash/farmhash.h"
#include "icing/absl_ports/annotate.h"
#include "icing/absl_ports/canonical_errors.h"
#include "icing/absl_ports/str_cat.h"
#include "icing/file/file-backed-vector.h"
#include "icing/file/filesystem.h"
#include "icing/file/memory-mapped-file.h"
#include "icing/file/portable-file-backed-proto-log.h"
#include "icing/proto/deletion.pb.h"
#include "icing/store/deletion-index-entry.h"
#include "icing/store/deletion-log-creator.h"
#include "icing/util/data-loss.h"
#include "icing/util/logging.h"
#include "icing/util/status-macros.h"

namespace icing {
namespace lib {

namespace {

constexpr char kDeletionIndexFilename[] = "deletion_index";

constexpr int64_t kMillisPerMinute = 60 * 1000;

std::string MakeDeletionIndexFilename(const std::string& base_dir) {
  return absl_ports::StrCat(base_dir, "/", kDeletionIndexFilename);
}

std::string MakeOptimizeTempDirectoryName(const std::string& base_dir) {
  return absl_ports::StrCat(base_dir, "_optimize_tmp");
}

DeletionIndexEntry CreateDeletionIndexEntry(const DeletionEvent& deletion_event,
                                            int64_t offset) {
  return DeletionIndexEntry(
      offset,
      static_cast<int32_t>(deletion_event.deletion_timestamp_ms() /
                           kMillisPerMinute),
      static_cast<int32_t>(deletion_event.last_update_timestamp_ms() /
                           kMillisPerMinute),
      tc3farmhash::Fingerprint32(deletion_event.schema()));
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
  create_result.derived_files_regenerated =
      initialize_result.derived_files_regenerated;
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
  DataLoss data_loss = create_result.log_create_result.data_loss;

  bool derived_files_regenerated = false;
  if (create_result.new_file || data_loss == DataLoss::COMPLETE) {
    // Either the DeletionLog is new or there is nothing in the DeletionLog that
    // an existing DeletionIndex could be consistent with, so generate the
    // DeletionIndex from scratch.
    libtextclassifier3::Status status = RegenerateDeletionIndex();
    if (!status.ok()) {
      return absl_ports::Annotate(status,
                                  "Failed to regenerate DeletionIndex.");
    }
    // Generating the DeletionIndex for a new DeletionLog isn't a recovery.
    derived_files_regenerated = !create_result.new_file;
  } else {
    libtextclassifier3::Status status = InitializeExistingDeletionIndex();
    if (!status.ok()) {
      ICING_LOG(WARNING)
          << status.error_message()
          << "\nFailed to initialize existing DeletionIndex, regenerating it.";
      status = RegenerateDeletionIndex();
      if (!status.ok()) {
        return absl_ports::Annotate(status,
                                    "Failed to regenerate DeletionIndex.");
      }
      derived_files_regenerated = true;
    }
  }

  initialized_ = true;
  return InitializeResult{
      .data_loss = data_loss,
      .derived_files_regenerated = derived_files_regenerated};
}

libtextclassifier3::Status DeletionStore::InitializeExistingDeletionIndex() {
  ICING_ASSIGN_OR_RETURN(deletion_index_,
                         FileBackedVector<DeletionIndexEntry>::Create(
                             *filesystem_, MakeDeletionIndexFilename(base_dir_),
                             MemoryMappedFile::READ_WRITE_AUTO_SYNC));

  // 1. Truncate DeletionIndexEntries pointing past the end of the DeletionLog.
  ICING_ASSIGN_OR_RETURN(int32_t num_entries,
                         GetNumDeletionIndexEntriesWithinDeletionLog());
  if (num_entries < deletion_index_->num_elements()) {
    // Less valid DeletionIndexEntries than the size of the DeletionIndex.
    ICING_LOG(WARNING) << "Truncating DeletionIndex from "
                       << deletion_index_->num_elements() << " to "
                       << num_entries << " entries.";
    ICING_RETURN_IF_ERROR(deletion_index_->TruncateTo(num_entries));
  }

  if (num_entries == 0) {
    // No DeletionIndexEntries are consistent with the DeletionLog, e.g. the
    // DeletionIndex file didn't exist or every entry pointed past the end of
    // the DeletionLog.
    PortableFileBackedProtoLog<DeletionEvent>::Iterator iterator =
        deletion_log_->GetIterator();
    libtextclassifier3::Status status = iterator.Advance();
    if (absl_ports::IsOutOfRange(status)) {
      // The DeletionLog was originally empty without data loss, so the empty
      // DeletionIndex is consistent with it.
      return libtextclassifier3::Status::OK;
    }
    ICING_RETURN_IF_ERROR(status);
    // The DeletionLog has DeletionEvents, so the whole DeletionIndex has to be
    // rebuilt from scratch. Let the caller regenerate it so that it's reported
    // as a recovery.
    return absl_ports::NotFoundError(
        "DeletionIndex has no entries for the non-empty DeletionLog.");
  }

  // 2. Verify that the last DeletionIndexEntry points to a valid DeletionEvent,
  // regardless of whether we truncated any entries.
  const DeletionIndexEntry& last_entry =
      deletion_index_->array()[num_entries - 1];
  ICING_RETURN_IF_ERROR(VerifyDeletionIndexEntry(last_entry));

  // 3. Append DeletionIndexEntries for any DeletionEvents after the last
  // DeletionIndexEntry.
  PortableFileBackedProtoLog<DeletionEvent>::Iterator iterator =
      deletion_log_->GetIterator(last_entry.offset());
  // Positions the iterator at the last DeletionEvent with a DeletionIndexEntry.
  ICING_RETURN_IF_ERROR(iterator.Advance());
  return AppendDeletionIndexEntries(&iterator);
}

libtextclassifier3::StatusOr<int32_t>
DeletionStore::GetNumDeletionIndexEntriesWithinDeletionLog() const {
  int32_t num_entries = deletion_index_->num_elements();
  const DeletionIndexEntry* entries = deletion_index_->array();
  while (num_entries > 0) {
    // Get an iterator to the DeletionEvent pointed to by the last
    // DeletionIndexEntry.
    PortableFileBackedProtoLog<DeletionEvent>::Iterator iterator =
        deletion_log_->GetIterator(entries[num_entries - 1].offset());
    libtextclassifier3::Status status = iterator.Advance();
    if (status.ok()) {
      // Found a valid DeletionEvent, so for the current num_entries, the
      // DeletionIndex is consistent with the DeletionLog.
      break;
    }
    if (!absl_ports::IsOutOfRange(status)) {
      // Encountered an error other than OutOfRange, so don't try to fix the
      // DeletionIndex.
      return status;
    }
    // DeletionIndexEntry points past the end of the DeletionLog.
    // Keep iterating until we hit a DeletionIndexEntry pointing to a valid
    // DeletionEvent or we run out of entries.
    --num_entries;
  }
  return num_entries;
}

libtextclassifier3::Status DeletionStore::VerifyDeletionIndexEntry(
    const DeletionIndexEntry& entry) const {
  libtextclassifier3::StatusOr<DeletionEvent> deletion_event_or =
      deletion_log_->ReadProto(entry.offset());
  if (!deletion_event_or.ok()) {
    return absl_ports::Annotate(
        deletion_event_or.status(),
        "DeletionIndexEntry doesn't point to a valid DeletionEvent.");
  }
  if (!(CreateDeletionIndexEntry(deletion_event_or.ValueOrDie(),
                                 entry.offset()) == entry)) {
    return absl_ports::InternalError(
        "DeletionIndexEntry doesn't match its DeletionEvent.");
  }
  return libtextclassifier3::Status::OK;
}

libtextclassifier3::Status DeletionStore::RegenerateDeletionIndex() {
  ICING_RETURN_IF_ERROR(ResetDeletionIndex());
  PortableFileBackedProtoLog<DeletionEvent>::Iterator iterator =
      deletion_log_->GetIterator();
  return AppendDeletionIndexEntries(&iterator);
}

libtextclassifier3::Status DeletionStore::AppendDeletionIndexEntries(
    PortableFileBackedProtoLog<DeletionEvent>::Iterator* iterator) {
  libtextclassifier3::Status status = iterator->Advance();
  while (status.ok()) {
    int64_t offset = iterator->GetOffset();
    ICING_ASSIGN_OR_RETURN(DeletionEvent deletion_event,
                           deletion_log_->ReadProto(offset));
    ICING_RETURN_IF_ERROR(deletion_index_->Append(
        CreateDeletionIndexEntry(deletion_event, offset)));
    status = iterator->Advance();
  }
  if (!absl_ports::IsOutOfRange(status)) {
    return status;
  }
  return libtextclassifier3::Status::OK;
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

libtextclassifier3::Status DeletionStore::CheckInitialized() const {
  if (!initialized_) {
    return absl_ports::FailedPreconditionError(
        "DeletionStore is not initialized, e.g. a previous Optimize failed.");
  }
  return libtextclassifier3::Status::OK;
}

libtextclassifier3::Status DeletionStore::PersistToDisk() {
  ICING_RETURN_IF_ERROR(CheckInitialized());
  ICING_RETURN_IF_ERROR(deletion_log_->PersistToDisk());
  return deletion_index_->PersistToDisk();
}

libtextclassifier3::Status DeletionStore::Put(
    const DeletionEvent& deletion_event) {
  ICING_RETURN_IF_ERROR(CheckInitialized());
  if (deletion_index_->num_elements() > 0) {
    const DeletionIndexEntry& last_entry =
        deletion_index_->array()[deletion_index_->num_elements() - 1];
    // The DeletionIndex must stay sorted by deletion_timestamp_m so that it can
    // be binary searched. DeletionIndexEntries only store minutes, so
    // out-of-order DeletionEvents within the same minute can't be detected
    // here.
    int32_t deletion_timestamp_m = static_cast<int32_t>(
        deletion_event.deletion_timestamp_ms() / kMillisPerMinute);
    if (deletion_timestamp_m < last_entry.deletion_timestamp_m()) {
      return absl_ports::InvalidArgumentError(absl_ports::StrCat(
          "DeletionEvent has deletion_timestamp_ms ",
          std::to_string(deletion_event.deletion_timestamp_ms()),
          ", which is in an earlier minute than the last DeletionEvent."));
    }
  }
  ICING_ASSIGN_OR_RETURN(int64_t offset,
                         deletion_log_->WriteProto(deletion_event));
  // If this fails, the DeletionIndex will be missing this DeletionEvent's
  // DeletionIndexEntry until it is repaired on the next Initialize.
  return deletion_index_->Append(
      CreateDeletionIndexEntry(deletion_event, offset));
}

libtextclassifier3::StatusOr<std::vector<DeletionEvent>> DeletionStore::Get(
    int64_t timestamp_ms,
    const std::unordered_set<std::string>& schemas) const {
  ICING_RETURN_IF_ERROR(CheckInitialized());
  int32_t timestamp_m = static_cast<int32_t>(timestamp_ms / kMillisPerMinute);
  std::unordered_set<uint32_t> schema_fps;
  for (const std::string& schema : schemas) {
    schema_fps.insert(tc3farmhash::Fingerprint32(schema));
  }

  const DeletionIndexEntry* entries_begin = deletion_index_->array();
  const DeletionIndexEntry* entries_end =
      entries_begin + deletion_index_->num_elements();
  // DeletionIndexEntries are sorted by deletion timestamp. Any entry with
  // deletion_timestamp_m < timestamp_m was deleted before timestamp_m, so
  // start from the first entry with deletion_timestamp_m >= timestamp_m.
  const DeletionIndexEntry* entry = std::lower_bound(
      entries_begin, entries_end, timestamp_m,
      [](const DeletionIndexEntry& index_entry, int32_t target_m) {
        return index_entry.deletion_timestamp_m() < target_m;
      });

  std::vector<DeletionEvent> deletion_events;
  for (; entry != entries_end; ++entry) {
    // Pre-filter using the DeletionIndexEntry to avoid reading DeletionEvents
    // from the DeletionLog. last_update_timestamp_m > timestamp_m implies that
    // last_update_timestamp_ms > timestamp_ms.
    if (entry->last_update_timestamp_m() > timestamp_m) {
      continue;
    }
    if (!schema_fps.empty() &&
        schema_fps.find(entry->schema_fp()) == schema_fps.end()) {
      continue;
    }

    // DeletionIndexEntries have lossy timestamps and schema fingerprints, so
    // check the DeletionEvent itself.
    ICING_ASSIGN_OR_RETURN(DeletionEvent deletion_event,
                           deletion_log_->ReadProto(entry->offset()));
    if (deletion_event.deletion_timestamp_ms() <= timestamp_ms ||
        deletion_event.last_update_timestamp_ms() > timestamp_ms) {
      // Exclude documents not deleted after the timestamp or documents added or
      // updated after the timestamp.
      continue;
    }
    if (!schemas.empty() &&
        schemas.find(deletion_event.schema()) == schemas.end()) {
      continue;
    }
    deletion_events.push_back(std::move(deletion_event));
  }
  return deletion_events;
}

libtextclassifier3::Status DeletionStore::Optimize(int64_t timestamp_ms) {
  ICING_RETURN_IF_ERROR(CheckInitialized());
  int32_t timestamp_m = static_cast<int32_t>(timestamp_ms / kMillisPerMinute);
  const DeletionIndexEntry* entries_begin = deletion_index_->array();
  const DeletionIndexEntry* entries_end =
      entries_begin + deletion_index_->num_elements();

  // DeletionIndexEntries are sorted by deletion timestamp. Every entry before
  // the first one with deletion_timestamp_m >= timestamp_m was deleted before
  // timestamp_ms, so it should be purged.
  const DeletionIndexEntry* first_kept = std::lower_bound(
      entries_begin, entries_end, timestamp_m,
      [](const DeletionIndexEntry& index_entry, int32_t target_m) {
        return index_entry.deletion_timestamp_m() < target_m;
      });
  // Entries with deletion_timestamp_m == timestamp_m may have been deleted
  // before or after timestamp_ms. Since DeletionEvents are also sorted by
  // deletion_timestamp_ms, read them to find the exact cutoff.
  while (first_kept != entries_end &&
         first_kept->deletion_timestamp_m() == timestamp_m) {
    ICING_ASSIGN_OR_RETURN(DeletionEvent deletion_event,
                           deletion_log_->ReadProto(first_kept->offset()));
    if (deletion_event.deletion_timestamp_ms() > timestamp_ms) {
      break;
    }
    ++first_kept;
  }

  if (first_kept == entries_begin) {
    // Nothing to purge.
    return libtextclassifier3::Status::OK;
  }

  // Copy the remaining DeletionEvents into a new DeletionStore in a temporary
  // directory.
  std::string temp_dir = MakeOptimizeTempDirectoryName(base_dir_);
  // TODO(b/568469091): If a previous Optimize failed to swap
  // and couldn't restore base_dir, temp_dir holds the only copy of the old
  // DeletionLog, but it is deleted here anyway. Detect and recover it, e.g. in
  // Create/Initialize, before it can be deleted.
  if (!filesystem_->DeleteDirectoryRecursively(temp_dir.c_str()) ||
      !filesystem_->CreateDirectoryRecursively(temp_dir.c_str())) {
    return absl_ports::InternalError(
        absl_ports::StrCat("Failed to create temporary directory: ", temp_dir));
  }
  {
    ICING_ASSIGN_OR_RETURN(
        CreateResult temp_create_result,
        Create(filesystem_, temp_dir, compression_level_,
               compression_threshold_bytes_, compression_mem_level_));
    DeletionStore* temp_store = temp_create_result.deletion_store.get();
    for (const DeletionIndexEntry* entry = first_kept; entry != entries_end;
         ++entry) {
      ICING_ASSIGN_OR_RETURN(DeletionEvent deletion_event,
                             deletion_log_->ReadProto(entry->offset()));
      ICING_RETURN_IF_ERROR(temp_store->Put(deletion_event));
    }
    ICING_RETURN_IF_ERROR(temp_store->PersistToDisk());
  }

  // Close the current files and swap in the new ones. Persist first so that
  // the current files are consistent in case the swap fails.
  ICING_RETURN_IF_ERROR(PersistToDisk());
  initialized_ = false;
  deletion_log_.reset();
  deletion_index_.reset();

  libtextclassifier3::Status swap_status = libtextclassifier3::Status::OK;
  // Whether temp_dir may hold the only copy of the old DeletionLog, in which
  // case it must not be deleted.
  bool old_files_stranded = false;
  if (!filesystem_->SwapFiles(temp_dir.c_str(), base_dir_.c_str())) {
    ICING_LOG(ERROR) << "Failed to swap DeletionStore directory.";
    swap_status =
        absl_ports::InternalError("Failed to swap DeletionStore directory.");
    // SwapFiles renames temp_dir -> temp_dir.tmp, base_dir -> temp_dir, then
    // temp_dir.tmp -> base_dir. If the last rename fails, base_dir is gone and
    // temp_dir holds the old files, so try to move them back.
    if (!filesystem_->DirectoryExists(base_dir_.c_str())) {
      // base_dir doesn't exist, so we must restore from temp_dir.
      if (filesystem_->RenameFile(temp_dir.c_str(), base_dir_.c_str())) {
        // The temp_dir still exists, so the swap failed but the store was
        // restored.
        ICING_LOG(WARNING) << "Restored DeletionStore directory after failed "
                              "swap.";
      } else {
        // The old DeletionLog is stranded in temp_dir. Keep temp_dir and fall
        // back to an empty store so the caller can keep going, but report that
        // the deletion history may be incomplete.
        ICING_LOG(ERROR) << "Failed to restore DeletionStore directory from "
                         << temp_dir;
        old_files_stranded = true;
        swap_status = absl_ports::DataLossError(absl_ports::StrCat(
            "Failed to swap DeletionStore directory, deletion history may be "
            "incomplete. Old files are in: ",
            temp_dir));
        if (!filesystem_->CreateDirectoryRecursively(base_dir_.c_str())) {
          return absl_ports::InternalError(absl_ports::StrCat(
              "Failed to recreate DeletionStore directory: ", base_dir_));
        }
      }
    }
  }
  if (!old_files_stranded) {
    // No old files are stranded, so we can delete the temporary directory.
    if (!filesystem_->DeleteDirectoryRecursively(temp_dir.c_str())) {
      ICING_LOG(WARNING) << "Failed to delete temporary directory: "
                         << temp_dir;
    }
    // SwapFiles may leave its intermediate directory behind if it fails.
    std::string swap_temp_dir = absl_ports::StrCat(temp_dir, ".tmp");
    if (!filesystem_->DeleteDirectoryRecursively(swap_temp_dir.c_str())) {
      ICING_LOG(WARNING) << "Failed to delete temporary directory: "
                         << swap_temp_dir;
    }
  }

  // Re-initialize from whatever is now in base_dir.
  // Either
  // - the new (optimized) DeletionStore files, if the swap succeeded, or
  // - the old (unoptimized) DeletionStore files, if the swap failed but we
  //   restored the DeletionStore directory, or
  // - an empty DeletionStore, if the swap failed and we couldn't restore the
  //   DeletionStore directory.
  ICING_RETURN_IF_ERROR(Initialize().status());
  return swap_status;
}

}  // namespace lib
}  // namespace icing
