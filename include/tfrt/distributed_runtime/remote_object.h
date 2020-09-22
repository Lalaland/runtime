/*
 * Copyright 2020 The TensorFlow Runtime Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//===- remote_object.h - Remote Object --------------------------*- C++ -*-===//
//
// This file defines RemoteObjectId which is a unique ID for a remote object.
//
//===----------------------------------------------------------------------===//
#ifndef TFRT_DISTRIBUTED_RUNTIME_REMOTE_OBJECT_H_
#define TFRT_DISTRIBUTED_RUNTIME_REMOTE_OBJECT_H_

#include "tfrt/distributed_runtime/distributed_context.h"
#include "tfrt/host_context/device.h"

namespace tfrt {
// Globally unique identifier for a remote object
struct RemoteObjectId {
  RemoteObjectId(int32_t prefix_id, int64_t local_id,
                 RCReference<Device> device)
      : prefix_id(prefix_id), local_id(local_id), device(device.CopyRef()) {}

  RemoteObjectId(const RemoteObjectId& other)
      : prefix_id(other.prefix_id),
        local_id(other.local_id),
        device(other.device.CopyRef()) {}

  RemoteObjectId& operator=(const RemoteObjectId& other) {
    prefix_id = other.prefix_id;
    local_id = other.local_id;
    device = other.device.CopyRef();
    return *this;
  }
  // Unique ID is pair of a unique prefix id (for instance, it can be the host
  // generates the id) and the unique id within that host.
  int32_t prefix_id;
  int64_t local_id;

  // The device where this object lives.
  RCReference<Device> device;
};

}  // namespace tfrt
#endif  // TFRT_DISTRIBUTED_RUNTIME_REMOTE_OBJECT_H_
