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
#pragma once

#include <memcached/mcd_util-visibility.h>
#include <platform/sized_buffer.h>
#include <spdlog/fmt/ostr.h>
#include <string>

/**
 * UserDataView technically makes tagUserData obsolete, but tagUserData
 * is used elsewhere for purposes other than logging, so have not been
 * changed.
 */

namespace cb {
/**
 * Wrap user/customer specific data with specific tags so that these data can
 * be scrubbed away during log collection.
 */

const std::string userdataStartTag = "<ud>";
const std::string userdataEndTag = "</ud>";

/**
 * Tag user data with the surrounding userdata tags
 *
 * @param data The string to tag
 * @return A tagged string in the form: <ud>string</ud>
 */
static inline std::string tagUserData(const std::string& data) {
    return userdataStartTag + data + userdataEndTag;
}

/**
 * Tag user data when objects of this type are printed, with surrounding
 * userdata tags
 */
class UserDataView {
public:
    explicit UserDataView(const uint8_t* dataParam, size_t dataLen)
        : data(cb::const_char_buffer{(const char*)dataParam, dataLen}){};

    explicit UserDataView(cb::const_char_buffer dataParam) : data(dataParam){};

    MCD_UTIL_PUBLIC_API
    friend std::ostream& operator<<(std::ostream& os, const UserDataView& d);

private:
    cb::const_char_buffer data;
};
} // namespace cb
