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
  //   INTERNAL on I/O error.
  libtextclassifier3::Status PersistToDisk();

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
  };

  // Initializes the DeletionLog and the DeletionIndex.
  //
  // Returns:
  //   An InitializeResult on success.
  //   INTERNAL on I/O error.
  //   INVALID_ARGUMENT on invalid compression options.
  libtextclassifier3::StatusOr<InitializeResult> Initialize();

  // Resets the unique_ptr to the DeletionIndex, deletes the underlying file,
  // and re-creates a new, empty instance of the DeletionIndex.
  //
  // Returns:
  //   OK on success.
  //   INTERNAL on I/O error.
  libtextclassifier3::Status ResetDeletionIndex();

  const Filesystem* const filesystem_;
  const std::string base_dir_;

  // Compression options for the DeletionLog.
  const int32_t compression_level_;
  const uint32_t compression_threshold_bytes_;
  const int32_t compression_mem_level_;

  // Used to guard against persisting to disk in the destructor if
  // initialization failed.
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
