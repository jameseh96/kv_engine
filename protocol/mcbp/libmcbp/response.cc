/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2018 Couchbase, Inc.
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

#include <mcbp/protocol/response.h>
#include <mcbp/protocol/status.h>
#include <nlohmann/json.hpp>

nlohmann::json cb::mcbp::Response::toJSON() const {
    if (!isValid()) {
        throw std::logic_error("Response::toJSON(): Invalid packet");
    }

    nlohmann::json ret;
    auto m = cb::mcbp::Magic(magic);
    ret["magic"] = ::to_string(m);

    if (m == Magic::ClientResponse || m == Magic::AltClientResponse) {
        ret["opcode"] = ::to_string(getClientOpcode());

    } else {
        ret["opcode"] = ::to_string(getServerOpcode());
    }

    ret["keylen"] = getKeylen();
    ret["extlen"] = getExtlen();

    if (m == Magic::AltClientResponse) {
        ret["framingextra"] = getFramingExtraslen();
    }

    ret["datatype"] = ::toJSON(getDatatype());
    ret["status"] = ::to_string(Status(getStatus()));
    ret["bodylen"] = getBodylen();
    ret["opaque"] = getOpaque();
    ret["cas"] = getCas();

    return ret;
}
