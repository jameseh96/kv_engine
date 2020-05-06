/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2020 Couchbase, Inc
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

#include <prometheus/exposer.h>

#include <unordered_set>

class EventuallyPersistentEngine;

// global, shared across all buckets
class PrometheusStatistics : public prometheus::Collectable {
public:
    PrometheusStatistics(const PrometheusStatistics&) = delete;
    PrometheusStatistics(PrometheusStatistics&&) = delete;

    PrometheusStatistics& operator=(const PrometheusStatistics&) = delete;
    PrometheusStatistics& operator=(PrometheusStatistics&&) = delete;

    static PrometheusStatistics* get();

    void registerEngine(EventuallyPersistentEngine* epe) {
        engines.insert(epe);
    }

    void unregisterEngine(EventuallyPersistentEngine* epe) {
        engines.erase(epe);
    }

    [[nodiscard]] std::vector<prometheus::MetricFamily> Collect()
            const override;

private:
    PrometheusStatistics() = default;

    void registerForExposition() {
        exposer.RegisterCollectable(instance);
    }

    // todo: catch errors like socket in use
    prometheus::Exposer exposer{"127.0.0.1:8080"};

    std::unordered_set<EventuallyPersistentEngine*> engines;

    static std::mutex creationGuard;
    // needs to be stored in a shared ptr, prometheus::Exporter
    // wants a weak ptr.
    static std::shared_ptr<PrometheusStatistics> instance;
    // just a raw pointer to the instance above.
    // could use std::atomic<std::shared_ptr<...>> rather
    // than a separate raw pointer, but load() would
    // mean bumping the refcount
    static std::atomic<PrometheusStatistics*> instancePtr;
};