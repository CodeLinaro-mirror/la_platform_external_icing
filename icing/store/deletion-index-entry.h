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

#ifndef ICING_STORE_DELETION_INDEX_ENTRY_H_
#define ICING_STORE_DELETION_INDEX_ENTRY_H_

#include <cstdint>

#include "icing/legacy/core/icing-packed-pod.h"

namespace icing {
namespace lib {

// A fixed-width entry in the DeletionIndex, which is chronologically ordered
// (with offset breaking ties). Each entry corresponds to one DeletionEvent in
// the DeletionLog and holds the information needed to filter DeletionEvents
// without having to read them from the DeletionLog.
class DeletionIndexEntry {
 public:
  explicit DeletionIndexEntry(int64_t offset, int32_t deletion_timestamp_m,
                              int32_t last_update_timestamp_m,
                              uint32_t schema_fp)
      : offset_(offset),
        deletion_timestamp_m_(deletion_timestamp_m),
        last_update_timestamp_m_(last_update_timestamp_m),
        schema_fp_(schema_fp) {}

  bool operator==(const DeletionIndexEntry& other) const {
    return offset_ == other.offset() &&
           deletion_timestamp_m_ == other.deletion_timestamp_m() &&
           last_update_timestamp_m_ == other.last_update_timestamp_m() &&
           schema_fp_ == other.schema_fp();
  }

  int64_t offset() const { return offset_; }

  int32_t deletion_timestamp_m() const { return deletion_timestamp_m_; }

  int32_t last_update_timestamp_m() const { return last_update_timestamp_m_; }

  uint32_t schema_fp() const { return schema_fp_; }

 private:
  // The byte offset of the corresponding DeletionEvent in the DeletionLog.
  int64_t offset_;

  // The time, in minutes since epoch, at which the document was deleted. Used
  // to binary search the DeletionIndex.
  // The deletion_timestamp_m is populated by reducing the granularity of the
  // deletion_timestamp_ms field in the DeletionEvent, which is in milliseconds
  // since epoch, to minutes since epoch by dividing by 60*1000, before casting
  // to int32_t.
  // This should be safe up to the year 6053.
  int32_t deletion_timestamp_m_;

  // The time, in minutes since epoch, at which the deleted document was last
  // created or updated. Used to filter out deletions of documents that did not
  // exist before the query timestamp.
  // The last_update_timestamp_m is populated by reducing the granularity of the
  // last_update_timestamp_ms field in the DeletionEvent, which is in
  // milliseconds since epoch, to minutes since epoch by dividing by 60*1000,
  // before casting to int32_t. This should be safe up to the year 6053.
  int32_t last_update_timestamp_m_;

  // The 32-bit fingerprint of the deleted document's full schema type string.
  uint32_t schema_fp_;
} __attribute__((packed));

static_assert(sizeof(DeletionIndexEntry) == 20, "");
static_assert(icing_is_packed_pod<DeletionIndexEntry>::value, "go/icing-ubsan");

}  // namespace lib
}  // namespace icing

#endif  // ICING_STORE_DELETION_INDEX_ENTRY_H_
