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

#include "logger_test_fixture.h"

#include <memcached/engine.h>
#include <memcached/extension.h>
#include <platform/cbassert.h>

#include <valgrind/valgrind.h>

#ifndef WIN32
#include <sys/resource.h>
#endif

/**
 * Test that the printf-style of the logger still works
 */
TEST_F(SpdloggerTest, OldStylePrintf) {
    auto& logger = cb::logger::getLoggerDescriptor();
    const uint32_t value = 0xdeadbeef;
    logger.log(EXTENSION_LOG_INFO, nullptr, "OldStylePrintf %x", value);
    cb::logger::shutdown();
    files = cb::io::findFilesWithPrefix(filename);
    ASSERT_EQ(1, files.size()) << "We should only have a single logfile";
    EXPECT_EQ(1, countInFile(files.front(), "INFO OldStylePrintf deadbeef"));
}

/**
 * Test that the new fmt-style formatting works
 */
TEST_F(SpdloggerTest, FmtStyleFormatting) {
    const uint32_t value = 0xdeadbeef;
    LOG_INFO("FmtStyleFormatting {:x}", value);
    cb::logger::shutdown();
    files = cb::io::findFilesWithPrefix(filename);
    ASSERT_EQ(1, files.size()) << "We should only have a single logfile";
    EXPECT_EQ(1,
              countInFile(files.front(), "INFO FmtStyleFormatting deadbeef"));
}

/**
 * Tests writing the maximum allowed message to file. Messages are held in
 * a buffer of size 2048, which allows for a message of size 2047 characters
 * (excluding logger formatting and null terminator).
 *
 * (old printf style)
 */
TEST_F(SpdloggerTest, LargeMessageTest) {
    const std::string message(2047,
                              'x'); // max message size is 2047 + 1 for '\0'
    auto& logger = cb::logger::getLoggerDescriptor();
    logger.log(EXTENSION_LOG_DEBUG, nullptr, message.c_str());
    cb::logger::shutdown();

    files = cb::io::findFilesWithPrefix(filename);
    ASSERT_EQ(1, files.size()) << "We should only have a single logfile";
    EXPECT_EQ(1, countInFile(files.front(), message));
}

/**
 * Tests the message cropping feature.
 * Crops a message which wouldn't fit in the message buffer.
 *
 * (old printf style)
 */
TEST_F(SpdloggerTest, LargeMessageWithCroppingTest) {
    const std::string message(2048, 'x'); // just 1 over max message size
    std::string cropped(2047 - strlen(" [cut]"), 'x');
    cropped.append(" [cut]");

    auto& logger = cb::logger::getLoggerDescriptor();
    logger.log(EXTENSION_LOG_DEBUG, nullptr, message.c_str());
    cb::logger::shutdown();

    files = cb::io::findFilesWithPrefix(filename);
    ASSERT_EQ(1, files.size()) << "We should only have a single logfile";
    EXPECT_EQ(1, countInFile(files.front(), cropped));
}

/**
 * Most basic test. Open a logfile, write a log message, close the logfile and
 * check if the hooks appear in the file.
 */
TEST_F(SpdloggerTest, BasicHooksTest) {
    cb::logger::shutdown();

    files = cb::io::findFilesWithPrefix(filename);
    ASSERT_EQ(1, files.size()) << "We should only have a single logfile";
    EXPECT_EQ(1, countInFile(files.front(), openingHook));
    EXPECT_EQ(1, countInFile(files.front(), closingHook));
}

/**
 * Test class for tests which wants to operate on multiple log files
 *
 * Initialize the logger with a 2k file rotation threshold
 */
class FileRotationTest : public SpdloggerTest {
protected:
    void SetUp() override {
        RemoveFiles();
        // Use a 2 k file size to make sure that we rotate :)
        setUpLogger(spdlog::level::level_enum::debug, 2048);
    }
};

/**
 * Log multiple messages, which will causes the files to rotate a few times.
 * Test if the hooks appear in each file.
 */
TEST_F(FileRotationTest, MultipleFilesTest) {
    const char* message =
            "This is a textual log message that we want to repeat a number of "
            "times: {}";
    for (auto ii = 0; ii < 100; ii++) {
        LOG_DEBUG(message, ii);
    }
    cb::logger::shutdown();

    files = cb::io::findFilesWithPrefix(filename);
    EXPECT_LT(1, files.size());
    for (auto& file : files) {
        EXPECT_EQ(1, countInFile(file, openingHook))
                << "Missing open hook in file: " << file;
        EXPECT_EQ(1, countInFile(file, closingHook))
                << "Missing closing hook in file: " << file;
    }
}

#ifndef WIN32
/**
 * Test that it works as expected when running out of file
 * descriptors. This test won't run on Windows as they don't
 * have the same ulimit setting
 */
TEST_F(FileRotationTest, HandleOpenFileErrors) {
    if (RUNNING_ON_VALGRIND) {
        std::cerr << "Skipping test when running on valgrind" << std::endl;
        return;
    }

#ifdef UNDEFINED_SANITIZER
    // MB-28735: This test fails under UBSan, when spdlog fails to open a new
    // file (in custom_rotating_file_sink::_sink_it):
    //
    //     common.h:139:9: runtime error: member access within address <ADDR>
    //     which does not point to an object of type 'spdlog::spdlog_ex' <ADDR>:
    //     note: object has invalid vptr
    //
    // examing <ADDR> in a debugger indicates a valid object. Therefore skipping
    // this test under UBSan.
    std::cerr << "Skipping test when running on UBSan (MB-28735)\n";
    return;
#endif

    LOG_DEBUG("Hey, this is a test");
    cb::logger::flush();
    files = cb::io::findFilesWithPrefix(filename);
    EXPECT_EQ(1, files.size());

    // Bring down out open file limit to a more conservative level (to
    // save using up a huge number of user / system FDs (and speed up the test).
    rlimit rlim;
    ASSERT_EQ(0, getrlimit(RLIMIT_NOFILE, &rlim))
            << "Failed to get RLIMIT_NOFILE: " << strerror(errno);

    const auto current = rlim.rlim_cur;
    rlim.rlim_cur = 100;
    ASSERT_EQ(0, setrlimit(RLIMIT_NOFILE, &rlim))
            << "Failed to set RLIMIT_NOFILE: " << strerror(errno);

    // Eat up file descriptors
    std::vector<FILE*> fds;
    FILE* fp;
    while ((fp = fopen(files.front().c_str(), "r")) != nullptr) {
        fds.push_back(fp);
    }
    EXPECT_EQ(EMFILE, errno);

    // Keep on logging. This should cause the files to wrap
    const char* message =
            "This is a textual log message that we want to repeat a number of "
            "times {}";
    for (auto ii = 0; ii < 100; ii++) {
        LOG_DEBUG(message, ii);
    }

    LOG_DEBUG("HandleOpenFileErrors");
    cb::logger::flush();

    // We've just flushed the data to the file, so it should be possible
    // to find it in the file.
    char buffer[1024];
    bool found = false;
    while (fgets(buffer, sizeof(buffer), fds.front()) != nullptr) {
        if (strstr(buffer, "HandleOpenFileErrors") != nullptr) {
            found = true;
        }
    }

    EXPECT_TRUE(found) << files.front()
                       << " does not contain HandleOpenFileErrors";

    // close all of the file descriptors
    for (const auto& fp : fds) {
        fclose(fp);
    }
    fds.clear();

    // Verify that we didn't get a new file while we didn't have any
    // free file descriptors
    files = cb::io::findFilesWithPrefix(filename);
    EXPECT_EQ(1, files.size());

    // Add a log entry, and we should get a new file
    LOG_DEBUG("Logging to the next file");
    cb::logger::flush();

    files = cb::io::findFilesWithPrefix(filename);
    EXPECT_EQ(2, files.size());

    // Restore the filedescriptors
    rlim.rlim_cur = current;
    ASSERT_EQ(0, setrlimit(RLIMIT_NOFILE, &rlim))
            << "Failed to restore RLIMIT_NOFILE: " << strerror(errno);
}
#endif
