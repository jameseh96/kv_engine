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
#include "rbac_reload_command_context.h"

#include <daemon/connection.h>
#include <daemon/cookie.h>
#include <daemon/executorpool.h>
#include <daemon/memcached.h>

/**
 * A small task used to reload the RBAC configuration data (it cannot run
 * in the frontend threads as it use file io.
 */
class RbacConfigReloadTask : public Task {
public:
    explicit RbacConfigReloadTask(Cookie& cookie_)
        : cookie(cookie_), status(ENGINE_SUCCESS) {
        // Empty
    }

    Status execute() override {
        auto& connection = cookie.getConnection();
        try {
            LOG_INFO("{}: Loading RBAC configuration from [{}] {}",
                     connection.getId(),
                     settings.getRbacFile(),
                     connection.getDescription());
            cb::rbac::loadPrivilegeDatabase(settings.getRbacFile());
            LOG_INFO("{}: RBAC configuration updated {}",
                     connection.getId(),
                     connection.getDescription());
        } catch (const std::runtime_error& error) {
            LOG_WARNING(
                    "{}: RbacConfigReloadTask(): An error occured while "
                    "loading RBAC configuration from [{}] {}: {}",
                    connection.getId(),
                    settings.getRbacFile(),
                    connection.getDescription(),
                    error.what());
            status = ENGINE_FAILED;
        }

        return Status::Finished;
    }

    void notifyExecutionComplete() override {
        notify_io_complete(static_cast<void*>(&cookie), status);
    }

private:
    Cookie& cookie;
    ENGINE_ERROR_CODE status;
};

ENGINE_ERROR_CODE RbacReloadCommandContext::reload() {
    state = State::Done;
    task = std::make_shared<RbacConfigReloadTask>(cookie);
    std::lock_guard<std::mutex> guard(task->getMutex());
    executorPool->schedule(task);
    return ENGINE_EWOULDBLOCK;
}

void RbacReloadCommandContext::done() {
    cookie.sendResponse(cb::mcbp::Status::Success);
}
