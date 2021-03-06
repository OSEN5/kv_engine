/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2018 Couchbase, Inc.
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

#include "ep_types.h"

#include "memcached/durability_spec.h"
#include "memcached/engine_common.h"
#include "memcached/engine_error.h"

#include <list>
#include <mutex>

class StoredDocKey;
class StoredValue;
class VBucket;

/*
 * The DurabilityMonitor (DM) drives the finalization (commit/abort) of a
 * SyncWrite request.
 *
 * To do that, the DM tracks the pending SyncWrites and the replica
 * acknowledgements to verify if the Durability Requirement is satisfied for
 * the tracked mutations.
 *
 * DM internals (describing by example).
 *
 * @todo: Update deferred, internals changing frequently these days :)
 */
class DurabilityMonitor {
public:
    struct NodeSeqnos {
        int64_t memory;
        int64_t disk;
    };

    // Note: constructor and destructor implementation in the .cc file to allow
    // the forward declaration of ReplicationChain in the header
    DurabilityMonitor(VBucket& vb);
    ~DurabilityMonitor();

    /**
     * Registers the Replication Chain.
     *
     * @param nodes the set of replicas representing the chain
     * @return ENGINE_SUCCESS if the operation succeeds, an error code otherwise
     */
    ENGINE_ERROR_CODE registerReplicationChain(
            const std::vector<std::string>& nodes);

    /**
     * Start tracking a new SyncWrite.
     * Expected to be called by VBucket::add/update/delete after a new SyncWrite
     * has been inserted into the HashTable and enqueued into the
     * CheckpointManager.
     *
     * @param cookie Optional client cookie which will be notified the SyncWrite
     *        completes.
     * @param item the queued_item
     * @return ENGINE_SUCCESS if the operation succeeds, an error code otherwise
     */
    ENGINE_ERROR_CODE addSyncWrite(const void* cookie, queued_item item);

    /**
     * Expected to be called by memcached at receiving a DCP_SEQNO_ACK packet.
     *
     * @param replica The replica that sent the ACK
     * @param memorySeqno The ack'ed memory-seqno
     * @param diskSeqno The ack'ed disk-seqno
     * @return ENGINE_SUCCESS if the operation succeeds, an error code otherwise
     * @throw std::logic_error if the received seqno is unexpected
     *
     * @todo: Expand for  supporting a full {memorySeqno, diskSeqno} ACK.
     */
    ENGINE_ERROR_CODE seqnoAckReceived(const std::string& replica,
                                       int64_t memorySeqno,
                                       int64_t diskSeqno);

    /**
     * Output DurabiltyMonitor stats.
     *
     * @param addStat the callback to memcached
     * @param cookie
     */
    void addStats(const AddStatFn& addStat, const void* cookie) const;

protected:
    class SyncWrite;
    struct ReplicationChain;
    struct Position;
    struct NodePosition;

    using Container = std::list<SyncWrite>;

    enum class Tracking : uint8_t { Memory, Disk };

    static std::string to_string(Tracking tracking);

    /**
     * @param lg the object lock
     * @return the number of pending SyncWrite(s) currently tracked
     */
    size_t getNumTracked(const std::lock_guard<std::mutex>& lg) const;

    /**
     * @param lg the object lock
     * @return the size of the replication chain
     */
    size_t getReplicationChainSize(const std::lock_guard<std::mutex>& lg) const;

    /**
     * Returns the next position for a node iterator.
     *
     * @param lg the object lock
     * @param node
     * @param tracking Memory or Disk?
     * @return the iterator to the next position for the given node
     */
    Container::iterator getNodeNext(const std::lock_guard<std::mutex>& lg,
                                    const std::string& node,
                                    Tracking tracking);

    /**
     * Advance a node tracking to the next Position in the tracked Container.
     * Note that a Position tracks a node in terms of both:
     * - iterator to a SyncWrite in the tracked Container
     * - seqno of the last SyncWrite ack'ed by the node
     * This function advances both iterator and seqno.
     *
     * @param lg the object lock
     * @param node
     * @param tracking Memory or Disk?
     */
    void advanceNodePosition(const std::lock_guard<std::mutex>& lg,
                             const std::string& node,
                             Tracking tracking);

    /**
     * We track both the memory/disk seqnos ack'ed by nodes.
     * Note that this may be different from the current SyncWrite tracked for
     * the node.
     * E.g., if we have one tracked SyncWrite{seqno:1, Level:Majority}, then
     * the DurabilityMonitor may receive a SeqnoAck{mem:1000, disk:0}.
     * At that point the memory-tracking for that node will be:
     *
     *     {writeSeqno:1, ackSeqno:1000}
     *
     * This function updates the tracking with the last seqno ack'ed by node.
     *
     * @param lg the object lock
     * @param node
     * @param tracking Memory or Disk?
     * @param seqno New ack seqno
     */
    void updateNodeAck(const std::lock_guard<std::mutex>& lg,
                       const std::string& node,
                       Tracking tracking,
                       int64_t seqno);

    /**
     * Returns the seqnos of the SyncWrites currently pointed by the internal
     * memory/disk tracking for Node.
     * E.g., if we have a tracked SyncWrite list like {s:1, s:2} and we receive
     * a SeqnoAck{mem:2, disk:1}, then the internal memory/disk tracking
     * will be {mem:2, disk:1}, which is what this function returns.
     * Note that this may differ from Replica AckSeqno. Using the same example,
     * if we receive a SeqnoAck{mem:100, disk:100} then the internal tracking
     * will still point to {mem:2, disk:1}, which is what this function will
     * return again.
     *
     * @param lg the object lock
     * @param node
     * @return the {memory, disk} seqnos of the tracked writes for node
     */
    NodeSeqnos getNodeWriteSeqnos(const std::lock_guard<std::mutex>& lg,
                                  const std::string& node) const;

    /**
     * Returns the last {memSeqno, diskSeqno} ack'ed by Node.
     * Note that this may differ from Node WriteSeqno.
     *
     * @param lg the object lock
     * @param node
     * @return the last {memory, disk} seqnos ack'ed by Node
     */
    NodeSeqnos getNodeAckSeqnos(const std::lock_guard<std::mutex>& lg,
                                const std::string& node) const;

    /**
     * Remove the given SyncWrte from tracking.
     *
     * @param lg the object lock
     * @param pos the Position of the SyncWrite to be removed
     * @return single-element list of the removed SyncWrite.
     */
    Container removeSyncWrite(const std::lock_guard<std::mutex>& lg,
                              const Position& pos);

    /**
     * Commit the given SyncWrite.
     *
     * @param key the key of the SyncWrite to be committed
     * @param seqno the seqno of the SyncWrite to be committed
     * @param cookie The cookie of the connection to notify.
     */
    void commit(const StoredDocKey& key, int64_t seqno, const void* cookie);

    /**
     * Updates a node memory/disk tracking as driven by the new ack-seqno.
     *
     * @param lg The object lock
     * @param node The node that ack'ed the given seqno
     * @param tracking Memory or Disk?
     * @param ackSeqno
     * @param [out] toCommit
     */
    void processSeqnoAck(const std::lock_guard<std::mutex>& lg,
                         const std::string& node,
                         Tracking tracking,
                         int64_t ackSeqno,
                         Container& toCommit);

    // The VBucket owning this DurabilityMonitor instance
    VBucket& vb;

    // Represents the internal state of a DurabilityMonitor instance.
    // Any state change must happen under lock(state.m).
    struct {
        mutable std::mutex m;
        // @todo: Expand for supporting the SecondChain.
        std::unique_ptr<ReplicationChain> firstChain;
        Container trackedWrites;
    } state;

    const size_t maxReplicas = 3;
};
