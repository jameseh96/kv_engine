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

#include "statistics/collector.h"

#include <spdlog/fmt/fmt.h>
#include <string_view>

using namespace std::string_view_literals;
using namespace cb::stats;

LabelGuard::~LabelGuard() {
    collector.removeDefaultLabel(label);
}

const StatSpec& StatCollector::lookup(StatKey key) {
    Expects(size_t(key) < size_t(StatKey::enum_max));
    return statSpecs[size_t(key)];
}

void CBStatCollector::addStat(const cb::stats::StatSpec& k,
                              std::string_view v) {
    addStatFn(k.uniqueKey, v, cookie);
}

void CBStatCollector::addStat(const cb::stats::StatSpec& k, bool v) {
    addStat(k, v ? "true"sv : "false"sv);
}

void CBStatCollector::addStat(const cb::stats::StatSpec& k, int64_t v) {
    fmt::memory_buffer buf;
    format_to(buf, "{}", v);
    addStat(k, {buf.data(), buf.size()});
}

void CBStatCollector::addStat(const cb::stats::StatSpec& k, uint64_t v) {
    fmt::memory_buffer buf;
    format_to(buf, "{}", v);
    addStat(k, {buf.data(), buf.size()});
}

void CBStatCollector::addStat(const cb::stats::StatSpec& k, double v) {
    fmt::memory_buffer buf;
    format_to(buf, "{}", v);
    addStat(k, {buf.data(), buf.size()});
}

void CBStatCollector::addStat(const cb::stats::StatSpec& k,
                              const HistogramData& hist) {
    fmt::memory_buffer buf;
    format_to(buf, "{}_mean", k.uniqueKey);
    addStat(StatSpec({buf.data(), buf.size()}), hist.mean);

    for (const auto& bucket : hist.buckets) {
        buf.resize(0);
        format_to(buf,
                  "{}_{},{}",
                  k.uniqueKey,
                  bucket.lowerBound,
                  bucket.upperBound);
        addStat(StatSpec({buf.data(), buf.size()}), bucket.count);
    }
}