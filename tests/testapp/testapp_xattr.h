/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Couchbase, Inc.
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

#include "testapp_client_test.h"
#include "xattr/blob.h"

#include <platform/cb_malloc.h>

class XattrTest : public TestappXattrClientTest {
public:
    void SetUp() override {
        TestappXattrClientTest::SetUp();

        // Create the document to operate on (with some compressible data).
        document.info.id = name;
        document.info.datatype = cb::mcbp::Datatype::Raw;
        document.value =
                R"({"couchbase": {"version": "spock", "next_version": "vulcan"}})";
        if (hasSnappySupport() == ClientSnappySupport::Yes) {
            // Compress the complete body.
            document.compress();
        }
        getConnection().mutate(document, 0, MutationType::Set);
    }

protected:
    void doArrayInsertTest(const std::string& path) {
        auto resp = subdoc(PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_PUSH_LAST,
                           name, path, "\"Smith\"",
                           SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
        EXPECT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS, resp.getStatus());

        resp = subdoc(PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_INSERT,
                      name, path + "[0]", "\"Bart\"",
                      SUBDOC_FLAG_XATTR_PATH);
        EXPECT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS, resp.getStatus());

        resp = subdoc(PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_INSERT,
                      name, path + "[1]", "\"Jones\"",
                      SUBDOC_FLAG_XATTR_PATH);
        EXPECT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS, resp.getStatus());

        resp = subdoc_get(path, SUBDOC_FLAG_XATTR_PATH);
        ASSERT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS, resp.getStatus());
        EXPECT_EQ("[\"Bart\",\"Jones\",\"Smith\"]", resp.getValue());
    }

    void doArrayPushLastTest(const std::string& path) {
        auto resp = subdoc(PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_PUSH_LAST,
                           name, path, "\"Smith\"",
                           SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
        ASSERT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS, resp.getStatus());

        resp = subdoc_get(path, SUBDOC_FLAG_XATTR_PATH);
        ASSERT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS, resp.getStatus());
        EXPECT_EQ("[\"Smith\"]", resp.getValue());

        // Add a second one so we know it was added last ;-)
        resp = subdoc(PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_PUSH_LAST,
                      name, path, "\"Jones\"",
                      SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
        ASSERT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS, resp.getStatus());

        resp = subdoc_get(path, SUBDOC_FLAG_XATTR_PATH);
        ASSERT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS, resp.getStatus());
        EXPECT_EQ("[\"Smith\",\"Jones\"]", resp.getValue());
    }

    void doArrayPushFirstTest(const std::string& path) {
        auto resp = subdoc(PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_PUSH_FIRST,
                           name, path, "\"Smith\"",
                           SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
        ASSERT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS, resp.getStatus());

        resp = subdoc_get(path, SUBDOC_FLAG_XATTR_PATH);
        ASSERT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS, resp.getStatus());
        EXPECT_EQ("[\"Smith\"]", resp.getValue());

        // Add a second one so we know it was added first ;-)
        resp = subdoc(PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_PUSH_FIRST,
                      name, path, "\"Jones\"",
                      SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
        ASSERT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS, resp.getStatus());

        resp = subdoc_get(path, SUBDOC_FLAG_XATTR_PATH);
        ASSERT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS, resp.getStatus());
        EXPECT_EQ("[\"Jones\",\"Smith\"]", resp.getValue());
    }

    void doAddUniqueTest(const std::string& path) {
        auto resp = subdoc(PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_ADD_UNIQUE,
                           name, path, "\"Smith\"",
                           SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
        ASSERT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS, resp.getStatus());

        resp = subdoc_get(path, SUBDOC_FLAG_XATTR_PATH);
        ASSERT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS, resp.getStatus());
        EXPECT_EQ("[\"Smith\"]", resp.getValue());

        resp = subdoc(PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_ADD_UNIQUE,
                      name, path, "\"Jones\"",
                      SUBDOC_FLAG_XATTR_PATH);
        ASSERT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS, resp.getStatus());

        resp = subdoc_get(path, SUBDOC_FLAG_XATTR_PATH);
        ASSERT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS, resp.getStatus());
        EXPECT_EQ("[\"Smith\",\"Jones\"]", resp.getValue());

        resp = subdoc(PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_ADD_UNIQUE,
                      name, path, "\"Jones\"",
                      SUBDOC_FLAG_XATTR_PATH);
        ASSERT_EQ(PROTOCOL_BINARY_RESPONSE_SUBDOC_PATH_EEXISTS, resp.getStatus());

        resp = subdoc_get(path, SUBDOC_FLAG_XATTR_PATH);
        ASSERT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS, resp.getStatus());
        EXPECT_EQ("[\"Smith\",\"Jones\"]", resp.getValue());

    }

    void doCounterTest(const std::string& path) {
        auto resp = subdoc(PROTOCOL_BINARY_CMD_SUBDOC_COUNTER,
                           name, path, "1",
                           SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
        ASSERT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS, resp.getStatus());

        resp = subdoc_get(path, SUBDOC_FLAG_XATTR_PATH);
        ASSERT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS, resp.getStatus());
        EXPECT_EQ("1", resp.getValue());

        resp = subdoc(PROTOCOL_BINARY_CMD_SUBDOC_COUNTER,
                      name, path, "1",
                      SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
        ASSERT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS, resp.getStatus());

        resp = subdoc_get(path, SUBDOC_FLAG_XATTR_PATH);
        ASSERT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS, resp.getStatus());
        EXPECT_EQ("2", resp.getValue());

    }

    // Test replacing a compressed/uncompressed value (based on test
    // variant) with an compressed/uncompressed value (based on input
    // paramter). XATTRs should be correctly merged.
    void doReplaceWithXattrTest(bool compress) {
        // Set initial body+XATTR, compressed depending on test variant.
        setBodyAndXattr(value, {{sysXattr, xattrVal}});

        // Replace body with new body.
        const std::string replacedValue = "\"JSON string\"";
        document.value = replacedValue;
        document.info.cas = mcbp::cas::Wildcard;
        document.info.datatype = cb::mcbp::Datatype::Raw;
        if (compress) {
            document.compress();
        }
        getConnection().mutate(document, 0, MutationType::Replace);

        // Validate contents.
        EXPECT_EQ(xattrVal, getXattr(sysXattr).getDataString());
        auto response = getConnection().get(name, 0);
        EXPECT_EQ(replacedValue, response.value);
        // Combined result will not be compressed; so just check for
        // JSON / not JSON.
        EXPECT_EQ(expectedJSONDatatype(), response.info.datatype);
    }

    BinprotSubdocResponse subdoc_get(
            const std::string& path,
            protocol_binary_subdoc_flag flag = SUBDOC_FLAG_NONE,
            mcbp::subdoc::doc_flag docFlag = mcbp::subdoc::doc_flag::None) {
        return subdoc(
                PROTOCOL_BINARY_CMD_SUBDOC_GET, name, path, {}, flag, docFlag);
    }

    /**
     * Takes a subdoc multimutation command, sends it and checks that the
     * values set correctly
     * @param cmd The command to send
     * @return Returns the response from the multi-mutation
     */
    BinprotSubdocMultiMutationResponse testBodyAndXattrCmd(
            BinprotSubdocMultiMutationCommand& cmd) {
        auto& conn = getConnection();
        conn.sendCommand(cmd);

        BinprotSubdocMultiMutationResponse multiResp;
        conn.recvResponse(multiResp);
        EXPECT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS, multiResp.getStatus());

        // Check the body was set correctly
        auto doc = getConnection().get(name, 0);
        EXPECT_EQ(value, doc.value);

        // Check the xattr was set correctly
        auto resp = subdoc_get(sysXattr, SUBDOC_FLAG_XATTR_PATH);
        EXPECT_EQ(xattrVal, resp.getValue());

        return multiResp;
    }

    void verify_xtoc_user_system_xattr() {
        // Test to check that we can get both an xattr and the main body in
        // subdoc multi-lookup
        setBodyAndXattr(value, {{sysXattr, xattrVal}});

        // Sanity checks and setup done lets try the multi-lookup

        BinprotSubdocMultiLookupCommand cmd;
        cmd.setKey(name);
        cmd.addGet("$XTOC", SUBDOC_FLAG_XATTR_PATH);
        cmd.addLookup("", PROTOCOL_BINARY_CMD_GET, SUBDOC_FLAG_NONE);

        auto& conn = getConnection();
        conn.sendCommand(cmd);

        BinprotSubdocMultiLookupResponse multiResp;
        conn.recvResponse(multiResp);
        EXPECT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS, multiResp.getStatus());
        EXPECT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS,
                  multiResp.getResults()[0].status);
        EXPECT_EQ(R"(["_sync"])", multiResp.getResults()[0].value);
        EXPECT_EQ(value, multiResp.getResults()[1].value);

        xattr_upsert("userXattr", R"(["Test"])");
        conn.sendCommand(cmd);
        multiResp.clear();
        conn.recvResponse(multiResp);
        EXPECT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS, multiResp.getStatus());
        EXPECT_EQ(PROTOCOL_BINARY_RESPONSE_SUCCESS,
                  multiResp.getResults()[0].status);
        EXPECT_EQ(R"(["_sync","userXattr"])", multiResp.getResults()[0].value);
    }

    std::string value = "{\"Field\":56}";
    const std::string sysXattr = "_sync";
    const std::string xattrVal = "{\"eg\":99}";
};

/// Explicit text fixutre for tests which want Xattr support disabled.
class XattrDisabledTest : public XattrTest {};
