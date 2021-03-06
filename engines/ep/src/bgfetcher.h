/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2012 Couchbase, Inc
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

#include "config.h"

#include <list>
#include <set>
#include <string>

#include "item.h"
#include "stats.h"
#include "vbucket.h"

// Forward declarations.
class KVBucket;
class KVShard;
class GlobalTask;

/**
 * Dispatcher job responsible for batching data reads and push to
 * underlying storage
 */
class BgFetcher {
public:
    /**
     * Construct a BgFetcher
     *
     * @param s  The store
     * @param k  The shard to which this background fetcher belongs
     * @param st reference to statistics
     */
    BgFetcher(KVBucket& s, KVShard& k, EPStats& st)
        : store(s), shard(k), taskId(0), stats(st), pendingFetch(false) {
    }

    /**
     * Construct a BgFetcher
     *
     * Equivalent to above constructor except stats reference is obtained
     * from KVBucket's reference to EPEngine's epstats.
     *
     * @param s The store
     * @param k The shard to which this background fetcher belongs
     */
    BgFetcher(KVBucket& s, KVShard& k);

    ~BgFetcher();

    void start(void);
    void stop(void);
    bool run(GlobalTask *task);
    bool pendingJob(void) const;
    void notifyBGEvent(void);
    void setTaskId(size_t newId) { taskId = newId; }
    void addPendingVB(Vbid vbId) {
        LockHolder lh(queueMutex);
        pendingVbs.insert(vbId);
    }

private:
    size_t doFetch(Vbid vbId, vb_bgfetch_queue_t& items);

    /// If the BGFetch task is currently snoozed (not scheduled to
    /// run), wake it up. Has no effect the if the task has already
    /// been woken.
    void wakeUpTaskIfSnoozed();

    KVBucket& store;
    KVShard& shard;
    size_t taskId;
    std::mutex queueMutex;
    EPStats &stats;

    std::atomic<bool> pendingFetch;
    std::set<Vbid> pendingVbs;
};
