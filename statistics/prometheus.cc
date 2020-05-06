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

#include "statistics/prometheus.h"

#include "ep_engine.h"
#include <statistics/prometheus_collector.h>

std::mutex PrometheusStatistics::creationGuard;
std::atomic<PrometheusStatistics*> PrometheusStatistics::instancePtr;
std::shared_ptr<PrometheusStatistics> PrometheusStatistics::instance;

PrometheusStatistics* PrometheusStatistics::get() {
    auto* tmp = instancePtr.load();
    if (tmp == nullptr) {
        std::lock_guard<std::mutex> lh(creationGuard);
        tmp = instancePtr.load();
        // recheck, it may have been created while we blocked
        // waiting for the lock
        if (tmp == nullptr) {
            instance.reset(new PrometheusStatistics());
            tmp = instance.get();
            instancePtr.store(tmp);
            instance->registerForExposition();
        }
    }
    return tmp;
}

std::vector<prometheus::MetricFamily> PrometheusStatistics::Collect() const {
    PrometheusStatCollector collector;
    for (auto* engine : engines) {
        // do each engine stats
        engine->doPrometheusStats(collector);
    }
    const auto& statsMap = collector.getCollectedStats();
    std::vector<prometheus::MetricFamily> result;

    for (auto& pair : statsMap) {
        result.emplace_back(pair.second);
    }

    return result;
}