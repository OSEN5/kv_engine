/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2010 Couchbase, Inc
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

#include "atomic.h"
#include "atomicqueue.h"
#include "dcp/dcp-types.h"

#include <climits>
#include <iterator>
#include <list>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declaration
class ConnNotifier;
class EventuallyPersistentEngine;
class Vbid;

/**
 * A collection of dcp connections.
 */
class ConnMap {
public:
    ConnMap(EventuallyPersistentEngine &theEngine);
    virtual ~ConnMap();

    void initialize();

    /**
     * Purge dead connections or identify paused connections that should send
     * NOOP messages to their destinations.
     */
    virtual void manageConnections() = 0;

    /**
     * Returns true if a dead connections list is not maintained,
     * or the list is empty.
     */
    virtual bool isDeadConnectionsEmpty() {
        return true;
    }

    /**
     * Returns true if there are existing connections.
     */
    virtual bool isConnections() = 0;

    /**
     * Adds the given connection to the set of connections associated
     * with the given vbucket.
     * @param conn Connection to add to the set. Refcount is retained.
     * @param vbid vBucket to add to.
     */
    void addVBConnByVBId(std::shared_ptr<ConnHandler> conn, Vbid vbid);

    void removeVBConnByVBId_UNLOCKED(const void* connCookie, Vbid vbid);

    void removeVBConnByVBId(const void* connCookie, Vbid vbid);

    /**
     * Notifies the front-end synchronously on this thread that this paused
     * connection should be re-considered for work.
     *
     * @param conn connection to be notified.
     */
    void notifyPausedConnection(const std::shared_ptr<ConnHandler>& conn);

    /**
     * Schedule a notify by adding it to the pendingNotifications queue.
     * It will be processed later by the ConnNotifer (in a separate thread)
     * by the processPendingNotifications method.
     *
     * @param conn connection to be scheduled for notification.
     */
    void addConnectionToPending(const std::shared_ptr<ConnHandler>& conn);

    /**
     * Notifies the front-end for all the connections in the
     * pendingNotifications queue that they should now be re-considered for
     * work.
     */
    void processPendingNotifications();

    EventuallyPersistentEngine& getEngine() {
        return engine;
    }

protected:

    // Synchronises notifying and releasing connections.
    // Guards modifications to std::shared_ptr<ConnHandler> objects in {map_}.
    // See also: {connLock}
    std::mutex                                    releaseLock;

    // Synchonises access to the {map_} members, i.e. adding
    // removing connections.
    // Actual modification of the underlying
    // ConnHandler objects is guarded by {releaseLock}.
    std::mutex                                    connsLock;

    using CookieToConnectionMap =
            std::unordered_map<const void*, std::shared_ptr<ConnHandler>>;
    CookieToConnectionMap map_;

    std::vector<std::mutex> vbConnLocks;
    std::vector<std::list<std::shared_ptr<ConnHandler>>> vbConns;

    /* Handle to the engine who owns us */
    EventuallyPersistentEngine &engine;

    AtomicQueue<std::shared_ptr<ConnHandler>> pendingNotifications;
    std::shared_ptr<ConnNotifier> connNotifier_;

    static size_t vbConnLockNum;
};
