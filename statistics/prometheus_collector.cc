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

#include <statistics/prometheus_collector.h>

void PrometheusStatCollector::addStat(const cb::stats::StatSpec& k,
                                      const HistogramData& hist) {
    prometheus::ClientMetric metric;

    metric.histogram.sample_count = hist.sampleCount;
    metric.histogram.sample_sum = hist.sampleSum;

    uint64_t cumulativeCount = 0;

    for (const auto& bucket : hist.buckets) {
        cumulativeCount += bucket.count;
        metric.histogram.bucket.push_back(
                {cumulativeCount, gsl::narrow_cast<double>(bucket.upperBound)});
    }

    addClientMetric(k, std::move(metric), prometheus::MetricType::Histogram);
}

void PrometheusStatCollector::addClientMetric(
        const cb::stats::StatSpec& key,
        prometheus::ClientMetric metric,
        prometheus::MetricType metricType) {
    auto name =
            key.metricFamilyKey.empty() ? key.uniqueKey : key.metricFamilyKey;

    auto [itr, inserted] = metricFamilies.try_emplace(
            std::string(name), prometheus::MetricFamily());
    auto& metricFamily = itr->second;
    if (inserted) {
        metricFamily.name = prefix + std::string(name);
        metricFamily.type = metricType;
    }

    metric.label.reserve(defaultLabels.size() + key.labels.size());

    // set the current default labels
    for (const auto& [label, value] : defaultLabels) {
        // don't want multiple copies of a label
        if (!key.labels.count(label)) {
            metric.label.push_back({label, value});
        }
    }

    // set the labels specific to this stat
    for (const auto& [label, value] : key.labels) {
        metric.label.push_back({label, value});
    }
    metricFamily.metric.emplace_back(std::move(metric));
}