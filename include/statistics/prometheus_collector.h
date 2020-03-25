/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc
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

#include "all_stats.h"
#include "collector.h"
#include <prometheus/client_metric.h>
#include <prometheus/metric_family.h>

class PrometheusStatCollector : public StatCollector {
public:
    PrometheusStatCollector(std::string prefix = "kv_"): prefix(std::move(prefix)) {
    }

    // Allow usage of the "helper" methods defined in the base type.
    // They would otherwise be shadowed
    using StatCollector::addStat;

    void addDefaultLabel(std::string_view k, std::string_view v) override {
        defaultLabels[std::string(k)] = v;
    }
    void removeDefaultLabel(std::string_view k) override {
        defaultLabels.erase(std::string(k));
    }

    void addStat(const cb::stats::StatSpec& k, std::string_view v) override {
        // silently discard text stats (for now). Prometheus can't expose them
    }

    void addStat(const cb::stats::StatSpec& k, bool v) override {
        addStat(k, double(v));
    }

    void addStat(const cb::stats::StatSpec& k, int64_t v) override {
        addStat(k, double(v));
    }

    void addStat(const cb::stats::StatSpec& k, uint64_t v) override {
        addStat(k, double(v));
    }

    void addStat(const cb::stats::StatSpec& k, double v) override {
        prometheus::ClientMetric metric;
        metric.untyped.value = v;
        addClientMetric(k, std::move(metric), prometheus::MetricType::Untyped);
    }

    void addStat(const cb::stats::StatSpec& k,
                 const HistogramData& hist) override;

    auto& getCollectedStats() {
        return metricFamilies;
    }

protected:
    void addClientMetric(const cb::stats::StatSpec& key,
                         prometheus::ClientMetric metric,
                         prometheus::MetricType metricType =
                                 prometheus::MetricType::Untyped);

    std::string prefix;

    std::unordered_map<std::string, prometheus::MetricFamily> metricFamilies{};
    std::unordered_map<std::string, std::string> defaultLabels;
};
