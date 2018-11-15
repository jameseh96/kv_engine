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
#pragma once

#include "config.h"

#include "magic.h"

#include <nlohmann/json_fwd.hpp>

#ifndef WIN32
#include <arpa/inet.h>
#endif
#include <stdexcept>

namespace cb {
namespace mcbp {

#define MCBP_TRACING_RESPONSE_SIZE 0x03

class Request;
class Response;

/**
 * The header struct is a representation of the header in a binary protocol.
 * It is (slightly) different for a request and a response packet, but the
 * size is the same.
 *
 * The header struct allows us to pick out the header, and inspect the
 * common fields (without first determining if the packet is a request
 * or a response).
 */
class Header {
public:
    uint8_t getMagic() const {
        return magic;
    }

    uint8_t getOpcode() const {
        return opcode;
    }

    uint16_t getKeylen() const {
        return ntohs(keylen);
    }

    uint8_t getExtlen() const {
        return extlen;
    }

    uint8_t getDatatype() const {
        return datatype;
    }

    uint16_t getSpecific() const {
        return ntohs(specific);
    }

    uint32_t getBodylen() const {
        return ntohl(bodylen);
    }

    uint32_t getOpaque() const {
        return opaque;
    }

    uint64_t getCas() const {
        return ntohll(cas);
    }

    /**
     * Does this packet represent a request packet?
     */
    bool isRequest() const;

    /**
     * Get a request object from this packet. Note that the entire
     * object may not be present (if called while we're still spooling
     * data for the object). The entire header is however available
     */
    const cb::mcbp::Request& getRequest() const;

    /**
     * Does this packet represent a response packet?
     */
    bool isResponse() const;

    /**
     * Get a response object from this packet. Note that the entire
     * object may not be present (if called while we're still spooling
     * data for the object). The entire header is however available
     */
    const cb::mcbp::Response& getResponse() const;

    bool isValid() const;

    /**
     * Create a JSON representation of the header
     */
    nlohmann::json toJSON() const;

protected:
    /*
     * This is the binary representation of the packet as described in
     * the binary protocol (see Packet Structure in docs/BinaryProtocol.md).
     *
     * All of the fields is stored in network byte order, and for all of the
     * "multibyte" fields there is an accessory function which perform the
     * "correct" translation (if needed). (Some fields (like opaque) isn't
     * being translated, so it does not do any conversion).
     */
    uint8_t magic;
    uint8_t opcode;
    uint16_t keylen;
    uint8_t extlen;
    uint8_t datatype;
    uint16_t specific;
    uint32_t bodylen;
    uint32_t opaque;
    uint64_t cas;
};

static_assert(sizeof(Header) == 24, "Incorrect compiler padding");

std::ostream& operator<<(std::ostream& os, const Header& header);

} // namespace mcbp
} // namespace cb
