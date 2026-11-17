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

#ifndef ICING_STORE_DELETION_LOG_CREATOR_H_
#define ICING_STORE_DELETION_LOG_CREATOR_H_

#include <cstdint>
#include <string>

#include "icing/text_classifier/lib3/utils/base/statusor.h"
#include "icing/file/filesystem.h"
#include "icing/file/portable-file-backed-proto-log.h"
#include "icing/proto/deletion.pb.h"

namespace icing {
namespace lib {

// Handles creation of the deletion log.
class DeletionLogCreator {
 public:
  struct CreateResult {
    // The create result passed up from the PortableFileBackedProtoLog::Create.
    // Contains the deletion log.
    PortableFileBackedProtoLog<DeletionEvent>::CreateResult log_create_result;

    // Whether the created file is new.
    // Callers should also check that log_create_result.data_loss == NONE before
    // using derived data.
    bool new_file;
  };

  // Creates the deletion log in the base_dir. Will create one if it doesn't
  // already exist.
  //
  // Returns:
  //   CreateResult on success.
  //   INTERNAL on any I/O error.
  //   INVALID_ARGUMENT on invalid options.
  static libtextclassifier3::StatusOr<DeletionLogCreator::CreateResult> Create(
      const Filesystem* filesystem, const std::string& base_dir,
      int32_t compression_level, uint32_t compression_threshold_bytes,
      int32_t compression_mem_level);

  // Returns the filename of the deletion log, without any directory prefixes.
  // Used mainly for testing purposes.
  static std::string GetDeletionLogFilename();
};

}  // namespace lib
}  // namespace icing

#endif  // ICING_STORE_DELETION_LOG_CREATOR_H_
