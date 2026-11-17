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

#ifndef ICING_STORE_DELETION_STORE_H_
#define ICING_STORE_DELETION_STORE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "icing/text_classifier/lib3/utils/base/status.h"
#include "icing/text_classifier/lib3/utils/base/statusor.h"
#include "icing/file/file-backed-vector.h"
#include "icing/file/filesystem.h"
#include "icing/file/portable-file-backed-proto-log.h"
#include "icing/proto/deletion.pb.h"
#include "icing/store/deletion-index-entry.h"
#include "icing/util/data-loss.h"

namespace icing {
namespace lib {

// Stores DeletionEvents, which record document deletions, so that they can
// later be retrieved (e.g. by offline observers).
//
// The DeletionStore is made up of:
//   - The DeletionLog: the ground truth, an append-only
//     PortableFileBackedProtoLog of DeletionEvents.
//   - The DeletionIndex: a derived FileBackedVector of DeletionIndexEntries,
//     one per DeletionEvent and in the same order as the DeletionLog, used to
//     filter DeletionEvents without reading them from the DeletionLog.
class DeletionStore {
 public:
  struct CreateResult {
    // A successfully initialized DeletionStore.
    std::unique_ptr<DeletionStore> deletion_store;

    // The data status of the DeletionLog after initializing from a previous
    // state. Data loss can happen if the file is corrupted or some previously
    // added data was unpersisted.
    DataLoss data_loss;

    // Whether the DeletionIndex had to be fully regenerated from the
    // DeletionLog. This does not include generating the DeletionIndex for the
    // first time for a new DeletionLog.
    bool derived_files_regenerated;
  };

  // Creates a DeletionStore in base_dir, creating the underlying files if they
  // don't already exist. base_dir must already exist.
  //
  // Returns:
  //   A DeletionStore::CreateResult on success.
  //   FAILED_PRECONDITION if filesystem is null.
  //   INTERNAL on any I/O error.
  //   INVALID_ARGUMENT on invalid compression options.
  static libtextclassifier3::StatusOr<DeletionStore::CreateResult> Create(
      const Filesystem* filesystem, const std::string& base_dir,
      int32_t compression_level, uint32_t compression_threshold_bytes,
      int32_t compression_mem_level);

  // Not copyable.
  DeletionStore(const DeletionStore&) = delete;
  DeletionStore& operator=(const DeletionStore&) = delete;

  // Persists all data to disk if the DeletionStore was successfully
  // initialized.
  ~DeletionStore();

  // Persists all data to disk.
  //
  // Returns:
  //   OK on success.
  //   FAILED_PRECONDITION if the DeletionStore is not initialized, e.g. after
  //     a failed Optimize.
  //   INTERNAL on I/O error.
  libtextclassifier3::Status PersistToDisk();

  // Puts the DeletionEvent into the DeletionLog and appends its
  // DeletionIndexEntry to the DeletionIndex.
  //
  // TODO(b/568469091) Handle and document the case where adding to DeletionLog
  // succeeds but adding to DeletionIndex fails, but a subsequent Put call
  // succeeds.
  //
  // REQUIRES: deletion_event.deletion_timestamp_ms() is greater than or equal
  //   to that of every DeletionEvent previously put into the DeletionStore.
  //   The DeletionIndex relies on this ordering to binary search by deletion
  //   timestamp. This is only checked at minute granularity (the precision of
  //   the DeletionIndex), so violations within the same minute are not
  //   detected.
  //
  // Returns:
  //   OK on success.
  //   INVALID_ARGUMENT if deletion_event's deletion timestamp is in an earlier
  //     minute than that of the last DeletionEvent in the DeletionStore.
  //   FAILED_PRECONDITION if the DeletionStore is not initialized, e.g. after
  //     a failed Optimize.
  //   INTERNAL on I/O error.
  libtextclassifier3::Status Put(const DeletionEvent& deletion_event);

  // Returns all DeletionEvents for documents that existed as of timestamp_ms
  // (update_timestamp_ms <= timestamp_ms) and were deleted after timestamp_ms
  // (deletion_timestamp_ms > timestamp_ms), in the order they were put.
  //
  // If schemas is non-empty, only DeletionEvents whose schema is in schemas
  // are returned. If schemas is empty, DeletionEvents of all schemas are
  // returned.
  //
  // Returns:
  //   A vector of matching DeletionEvents on success.
  //   FAILED_PRECONDITION if the DeletionStore is not initialized, e.g. after
  //     a failed Optimize.
  //   INTERNAL on I/O error.
  libtextclassifier3::StatusOr<std::vector<DeletionEvent>> Get(
      int64_t timestamp_ms,
      const std::unordered_set<std::string>& schemas) const;

  // Purges all DeletionEvents with deletion_timestamp_ms <= timestamp_ms, along
  // with their DeletionIndexEntries.
  //
  // The remaining DeletionEvents are copied into a new DeletionStore in a
  // temporary directory, which is then swapped with base_dir. If the swap
  // fails, the old files are restored to base_dir if needed and the
  // DeletionStore is re-initialized from them. If the old files can't be
  // restored, they are left in the temporary directory and the DeletionStore
  // is re-initialized as empty. If the DeletionStore can't be re-initialized,
  // it is left uninitialized and all subsequent calls return
  // FAILED_PRECONDITION.
  //
  // TODO(b/568469091): Persist and report cutoff timestamp_ms so that clients
  // know the cutoff timestamp of DeletionLog between optimizes and crashes.
  //
  // Returns:
  //   OK on success.
  //   FAILED_PRECONDITION if the DeletionStore is not initialized, e.g. after
  //     a previous failed Optimize.
  //   DATA_LOSS if the swap failed and the old files couldn't be restored, so
  //     the DeletionStore was re-initialized as empty and the deletion history
  //     may be incomplete.
  //   INTERNAL on I/O error.
  libtextclassifier3::Status Optimize(int64_t timestamp_ms);

  // Returns the filename of the DeletionIndex, without any directory prefixes.
  // Used mainly for testing purposes.
  static std::string GetDeletionIndexFilename();

 private:
  // Use DeletionStore::Create() to instantiate.
  explicit DeletionStore(const Filesystem* filesystem, std::string base_dir,
                         int32_t compression_level,
                         uint32_t compression_threshold_bytes,
                         int32_t compression_mem_level);

  struct InitializeResult {
    DataLoss data_loss;
    bool derived_files_regenerated;
  };

  // Initializes the DeletionLog and the DeletionIndex.
  //
  // If the DeletionLog is new or completely lost, the DeletionIndex is
  // regenerated from the DeletionLog. Otherwise, the existing DeletionIndex is
  // initialized and repaired against the DeletionLog, falling back to fully
  // regenerating it if that fails.
  //
  // Returns:
  //   An InitializeResult on success.
  //   INTERNAL on I/O error.
  //   INVALID_ARGUMENT on invalid compression options.
  libtextclassifier3::StatusOr<InitializeResult> Initialize();

  // Initializes the existing DeletionIndex and repairs it so that it has
  // exactly one DeletionIndexEntry per DeletionEvent in the DeletionLog:
  //   1. Truncates DeletionIndexEntries whose offsets are past the end of the
  //      DeletionLog, e.g. if the DeletionLog was rewound after a crash.
  //   2. Verifies that the last remaining DeletionIndexEntry points to a valid
  //      DeletionEvent.
  //   3. Appends DeletionIndexEntries for any DeletionEvents after the last
  //      remaining DeletionIndexEntry, e.g. if we crashed after writing to the
  //      DeletionLog but before updating the DeletionIndex.
  //
  // Returns:
  //   OK on success.
  //   NOT_FOUND if no DeletionIndexEntries are consistent with a non-empty
  //     DeletionLog (e.g. the DeletionIndex file doesn't exist, is empty, or
  //     all of its entries point past the end of the DeletionLog), i.e. the
  //     DeletionIndex has to be regenerated from scratch.
  //   FAILED_PRECONDITION if the DeletionIndex's checksum doesn't match, e.g.
  //     if it wasn't persisted before a crash.
  //   INTERNAL on I/O error or if the DeletionIndex can't be repaired.
  libtextclassifier3::Status InitializeExistingDeletionIndex();

  // Returns the number of leading DeletionIndexEntries that point to offsets
  // within the DeletionLog, i.e. the number of entries to keep after
  // truncating the DeletionIndexEntries that point past the end of the
  // DeletionLog.
  //
  // Returns:
  //   The number of DeletionIndexEntries within the DeletionLog on success.
  //   INTERNAL on I/O error.
  libtextclassifier3::StatusOr<int32_t>
  GetNumDeletionIndexEntriesWithinDeletionLog() const;

  // Verifies that the offset in entry points to a valid DeletionEvent in the
  // DeletionLog, and that entry matches the DeletionIndexEntry created from
  // that DeletionEvent.
  //
  // Returns:
  //   OK if entry is consistent with its DeletionEvent.
  //   INTERNAL if entry doesn't match its DeletionEvent, or on I/O error.
  //   Any error from reading the DeletionEvent from the DeletionLog.
  libtextclassifier3::Status VerifyDeletionIndexEntry(
      const DeletionIndexEntry& entry) const;

  // Resets the DeletionIndex and regenerates it from the DeletionLog.
  //
  // Returns:
  //   OK on success.
  //   INTERNAL on I/O error.
  libtextclassifier3::Status RegenerateDeletionIndex();

  // Appends a DeletionIndexEntry to the DeletionIndex for every DeletionEvent
  // that the iterator advances over.
  //
  // Returns:
  //   OK on success.
  //   INTERNAL on I/O error.
  libtextclassifier3::Status AppendDeletionIndexEntries(
      PortableFileBackedProtoLog<DeletionEvent>::Iterator* iterator);

  // Resets the unique_ptr to the DeletionIndex, deletes the underlying file,
  // and re-creates a new, empty instance of the DeletionIndex.
  //
  // Returns:
  //   OK on success.
  //   INTERNAL on I/O error.
  libtextclassifier3::Status ResetDeletionIndex();

  // Returns:
  //   OK if the DeletionStore is initialized.
  //   FAILED_PRECONDITION otherwise, e.g. after a failed Optimize left
  //     deletion_log_ or deletion_index_ null.
  libtextclassifier3::Status CheckInitialized() const;

  const Filesystem* const filesystem_;
  const std::string base_dir_;

  // Compression options for the DeletionLog.
  const int32_t compression_level_;
  const uint32_t compression_threshold_bytes_;
  const int32_t compression_mem_level_;

  // Whether deletion_log_ and deletion_index_ are both valid. Public methods
  // return FAILED_PRECONDITION and the destructor skips persisting to disk if
  // this is false, e.g. if a failed Optimize couldn't re-initialize.
  bool initialized_ = false;

  // The ground truth log of DeletionEvents.
  std::unique_ptr<PortableFileBackedProtoLog<DeletionEvent>> deletion_log_;

  // Derived from deletion_log_. Holds one DeletionIndexEntry per DeletionEvent,
  // in the same order as deletion_log_.
  std::unique_ptr<FileBackedVector<DeletionIndexEntry>> deletion_index_;
};

}  // namespace lib
}  // namespace icing

#endif  // ICING_STORE_DELETION_STORE_H_
