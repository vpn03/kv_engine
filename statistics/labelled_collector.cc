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

#include <statistics/labelled_collector.h>

#include <memcached/dockey.h>

LabelledStatCollector::LabelledStatCollector(const StatCollector& parent,
                                             const Labels& labels)
    : parent(parent), defaultLabels(labels.begin(), labels.end()) {
}

LabelledStatCollector LabelledStatCollector::withLabels(Labels&& labels) const {
    // take the specific labels passed as parameters
    Labels mergedLabels{labels.begin(), labels.end()};
    // add in the labels stored in this collector
    // (will not overwrite labels passed as parameters)
    mergedLabels.insert(defaultLabels.begin(), defaultLabels.end());

    // create a LabelledStatCollector directly wrapping the parent collector,
    // with the merged set of labels. This avoids chaining through multiple
    // LabelledStatCollectors.
    return {parent, mergedLabels};
}

void LabelledStatCollector::addStat(const cb::stats::StatDef& k,
                                    std::string_view v,
                                    const Labels& labels) const {
    forwardToParent(k, v, labels);
}

void LabelledStatCollector::addStat(const cb::stats::StatDef& k,
                                    bool v,
                                    const Labels& labels) const {
    forwardToParent(k, v, labels);
}

void LabelledStatCollector::addStat(const cb::stats::StatDef& k,
                                    int64_t v,
                                    const Labels& labels) const {
    forwardToParent(k, v, labels);
}
void LabelledStatCollector::addStat(const cb::stats::StatDef& k,
                                    uint64_t v,
                                    const Labels& labels) const {
    forwardToParent(k, v, labels);
}
void LabelledStatCollector::addStat(const cb::stats::StatDef& k,
                                    double v,
                                    const Labels& labels) const {
    forwardToParent(k, v, labels);
}

void LabelledStatCollector::addStat(const cb::stats::StatDef& k,
                                    const HistogramData& v,
                                    const Labels& labels) const {
    forwardToParent(k, v, labels);
}

BucketStatCollector::BucketStatCollector(const StatCollector& parent,
                                         std::string_view bucket)
    : LabelledStatCollector(parent, {{"bucket", bucket}}) {
}
ScopeStatCollector BucketStatCollector::forScope(ScopeID scope) const {
    return {*this, scope};
}

ScopeStatCollector::ScopeStatCollector(const BucketStatCollector& parent,
                                       ScopeID scope)
    : LabelledStatCollector(parent.withLabels({{"scope", scope.to_string()}})) {
}
ColStatCollector ScopeStatCollector::forCollection(
        CollectionID collection) const {
    return {*this, collection};
}

ColStatCollector::ColStatCollector(const ScopeStatCollector& parent,
                                   CollectionID collection)
    : LabelledStatCollector(
              parent.withLabels({{"collection", collection.to_string()}})) {
}