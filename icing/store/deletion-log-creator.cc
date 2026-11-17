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
#include <string>
#include <utility>

#include "icing/text_classifier/lib3/utils/base/statusor.h"
#include "icing/absl_ports/str_cat.h"
#include "icing/file/constants.h"
#include "icing/file/filesystem.h"
#include "icing/file/portable-file-backed-proto-log.h"
#include "icing/proto/deletion.pb.h"
#include "icing/util/status-macros.h"

namespace icing {
namespace lib {

namespace {

constexpr char kDeletionLogFilename[] = "deletion_log";

std::string MakeDeletionLogFilename(const std::string& base_dir) {
  return absl_ports::StrCat(base_dir, "/", kDeletionLogFilename);
}

}  // namespace

std::string DeletionLogCreator::GetDeletionLogFilename() {
  return kDeletionLogFilename;
}

libtextclassifier3::StatusOr<DeletionLogCreator::CreateResult>
DeletionLogCreator::Create(const Filesystem* filesystem,
                           const std::string& base_dir,
                           int32_t compression_level,
                           uint32_t compression_threshold_bytes,
                           int32_t compression_mem_level) {
  std::string deletion_log_filename = MakeDeletionLogFilename(base_dir);
  bool new_file = !filesystem->FileExists(deletion_log_filename.c_str());

  ICING_ASSIGN_OR_RETURN(
      PortableFileBackedProtoLog<DeletionEvent>::CreateResult log_create_result,
      PortableFileBackedProtoLog<DeletionEvent>::Create(
          filesystem, deletion_log_filename,
          PortableFileBackedProtoLog<DeletionEvent>::Options(
              /*compress_in=*/true, constants::kMaxProtoSize, compression_level,
              compression_threshold_bytes, compression_mem_level)));

  CreateResult create_result = {std::move(log_create_result), new_file};
  return create_result;
}

}  // namespace lib
}  // namespace icing
