/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2019-Present Couchbase, Inc.
 *
 *   Use of this software is governed by the Business Source License included
 *   in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
 *   in that file, in accordance with the Business Source License, use of this
 *   software will be governed by the Apache License, Version 2.0, included in
 *   the file licenses/APL2.txt.
 */

#include "mock_executor_pool.h"
#include "objectregistry.h"
#include "taskqueue.h"

void MockExecutorPool::replaceExecutorPoolWithMock() {
    LockHolder lh(initGuard);
    auto* executor = instance.load();
    if (executor) {
        executor->shutdown();
    }
    auto* epEngine = ObjectRegistry::onSwitchThread(nullptr, true);
    executor = new MockExecutorPool();
    ObjectRegistry::onSwitchThread(epEngine);
    instance.store(executor);
}

bool MockExecutorPool::isTaskScheduled(const task_type_t queueType,
                                       const std::string& taskName) {
    LockHolder lh(tMutex);
    for (const auto& it : taskLocator) {
        const auto& taskQueuePair = it.second;
        if (taskName == taskQueuePair.first->getDescription() &&
            taskQueuePair.second->getQueueType() == queueType) {
            return true;
        }
    }
    return false;
}
