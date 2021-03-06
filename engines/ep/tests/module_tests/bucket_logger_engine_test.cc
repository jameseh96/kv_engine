/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2018 Couchbase, Inc
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

#include "bucket_logger_engine_test.h"

#include <engines/ep/src/bucket_logger.h>
#include <engines/ep/src/logger.h>

void BucketLoggerEngineTest::SetUp() {
    // Write to a different file in case other parent class fixtures are
    // running in parallel
    filename = "spdlogger_engine_test";

    // Store the oldLogLevel for tearDown
    oldLogLevel = Logger::getGlobalLogLevel();

    // Set up the logger with a greater file size so logs are output to a
    // single file
    setUpLogger(spdlog::level::level_enum::trace, 100 * 1024);

    EventuallyPersistentEngineTest::SetUp();
}

void BucketLoggerEngineTest::TearDown() {
    EventuallyPersistentEngineTest::TearDown();
    BucketLoggerTest::TearDown();
}

/**
 * Test that the bucket name is logged out at the start of the message
 */
TEST_F(BucketLoggerEngineTest, EngineTest) {
    // Flush the logger to ensure log file has required contents. Flushing
    // instead of shutting it down as the engine is still running
    cb::logger::flush();
    files = cb::io::findFilesWithPrefix(filename);
    EXPECT_EQ(1,
              countInFile(files.back(),
                          "INFO (default) EPEngine::initialize: using "
                          "configuration:\"dbname=ep_engine_ep_unit_tests_db"
                          ";bucket_type=persistent\""));
}
