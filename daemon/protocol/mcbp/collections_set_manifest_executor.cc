/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "executors.h"

#include <daemon/cookie.h>

void collections_set_manifest_executor(Cookie& cookie) {
    auto ret = cookie.swapAiostat(ENGINE_SUCCESS);

    if (ret == ENGINE_SUCCESS) {
        auto& connection = cookie.getConnection();
        auto packet = cookie.getPacket(Cookie::PacketContent::Full);
        const auto* req = reinterpret_cast<
                const protocol_binary_collections_set_manifest*>(packet.data());
        const uint32_t valuelen = ntohl(req->message.header.request.bodylen);
        cb::const_char_buffer jsonBuffer{
                reinterpret_cast<const char*>(req->bytes + sizeof(req->bytes)),
                valuelen};
        ret = ENGINE_ERROR_CODE(
                connection.getBucketEngine()
                        ->collections
                        .set_manifest(connection.getBucketEngine(), jsonBuffer)
                        .code()
                        .value());
    }

    if (ret == ENGINE_SUCCESS) {
        cookie.sendResponse(cb::mcbp::Status::Success);
    } else {
        cookie.sendResponse(cb::engine_errc(ret));
    }
}
