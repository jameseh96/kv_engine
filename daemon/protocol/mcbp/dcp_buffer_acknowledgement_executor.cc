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

#include <daemon/mcbp.h>
#include "engine_wrapper.h"
#include "utilities.h"

void dcp_buffer_acknowledgement_executor(Cookie& cookie) {
    auto ret = cookie.swapAiostat(ENGINE_SUCCESS);

    auto& connection = cookie.getConnection();
    if (ret == ENGINE_SUCCESS) {
        ret = mcbp::haveDcpPrivilege(cookie);

        if (ret == ENGINE_SUCCESS) {
            const auto& header = cookie.getRequest();
            const auto* req = reinterpret_cast<
                    const protocol_binary_request_dcp_buffer_acknowledgement*>(
                    &header);

            uint32_t bbytes;
            memcpy(&bbytes, &req->message.body.buffer_bytes, 4);
            ret = dcpBufferAcknowledgement(cookie,
                                           header.getOpaque(),
                                           header.getVBucket(),
                                           ntohl(bbytes));
        }
    }

    ret = connection.remapErrorCode(ret);
    switch (ret) {
    case ENGINE_SUCCESS:
        connection.setState(StateMachine::State::new_cmd);
        break;

    case ENGINE_DISCONNECT:
        connection.setState(StateMachine::State::closing);
        break;

    case ENGINE_EWOULDBLOCK:
        cookie.setEwouldblock(true);
        break;

    default:
        cookie.sendResponse(cb::engine_errc(ret));
    }
}
