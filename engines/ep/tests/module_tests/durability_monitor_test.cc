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

#include "durability_monitor_test.h"

#include "checkpoint_manager.h"
#include "test_helpers.h"

#include "../mock/mock_synchronous_ep_engine.h"

void DurabilityMonitorTest::addSyncWrite(int64_t seqno,
                                         cb::durability::Requirements req) {
    auto numTracked = monitor->public_getNumTracked();
    auto item = Item(makeStoredDocKey("key" + std::to_string(seqno)),
                     0 /*flags*/,
                     0 /*exp*/,
                     "value",
                     5 /*valueSize*/,
                     PROTOCOL_BINARY_RAW_BYTES,
                     0 /*cas*/,
                     seqno);
    using namespace cb::durability;
    item.setPendingSyncWrite(req);
    // Note: necessary for non-auto-generated seqno
    vb->checkpointManager->createSnapshot(seqno, seqno);
    // Note: need to go through VBucket::processSet to set the given bySeqno
    ASSERT_EQ(MutationStatus::WasClean, processSet(item));
    ASSERT_EQ(numTracked + 1, monitor->public_getNumTracked());
}

size_t DurabilityMonitorTest::addSyncWrites(int64_t seqnoStart,
                                            int64_t seqnoEnd,
                                            cb::durability::Requirements req) {
    size_t expectedNumTracked = monitor->public_getNumTracked();
    size_t added = 0;
    for (auto seqno = seqnoStart; seqno <= seqnoEnd; seqno++) {
        addSyncWrite(seqno, req);
        added++;
        expectedNumTracked++;
        EXPECT_EQ(expectedNumTracked, monitor->public_getNumTracked());
    }
    return added;
}

size_t DurabilityMonitorTest::addSyncWrites(const std::vector<int64_t>& seqnos,
                                            cb::durability::Requirements req) {
    if (seqnos.empty()) {
        throw std::logic_error(
                "DurabilityMonitorTest::addSyncWrites: seqnos list is empty");
    }
    size_t expectedNumTracked = monitor->public_getNumTracked();
    size_t added = 0;
    for (auto seqno : seqnos) {
        addSyncWrite(seqno, req);
        added++;
        expectedNumTracked++;
        EXPECT_EQ(expectedNumTracked, monitor->public_getNumTracked());
    }
    return added;
}

MutationStatus DurabilityMonitorTest::processSet(Item& item) {
    auto htRes = vb->ht.findForWrite(item.getKey());
    VBQueueItemCtx ctx;
    ctx.genBySeqno = GenerateBySeqno::No;
    ctx.durability =
            DurabilityItemCtx{item.getDurabilityReqs(), /*cookie*/ nullptr};
    return vb
            ->processSet(htRes.lock,
                         htRes.storedValue,
                         item,
                         item.getCas(),
                         true /*allow_existing*/,
                         false /*has_metadata*/,
                         ctx,
                         {/*no predicate*/})
            .first;
}

TEST_F(DurabilityMonitorTest, AddSyncWrite) {
    EXPECT_EQ(3, addSyncWrites(1 /*seqnoStart*/, 3 /*seqnoEnd*/));
}

TEST_F(DurabilityMonitorTest, SeqnoAckReceivedNoTrackedSyncWrite) {
    try {
        monitor->seqnoAckReceived(replica, 1 /*memSeqno*/, 0 /*diskSeqno*/);
    } catch (const std::logic_error& e) {
        EXPECT_TRUE(std::string(e.what()).find("No tracked SyncWrite") !=
                    std::string::npos);
        return;
    }
    FAIL();
}

TEST_F(DurabilityMonitorTest, SeqnoAckReceivedSmallerThanLastAcked) {
    addSyncWrites({1, 2} /*seqnos*/);

    // This call removes seqno:1
    ASSERT_NO_THROW(monitor->seqnoAckReceived(
            replica, 1 /*memSeqno*/, 0 /*diskSeqno*/));
    ASSERT_EQ(1, monitor->public_getNumTracked());
    ASSERT_EQ(1, monitor->public_getNodeWriteSeqnos(replica).memory);
    ASSERT_EQ(1, monitor->public_getNodeAckSeqnos(replica).memory);

    try {
        monitor->seqnoAckReceived(replica, 0 /*memSeqno*/, 0 /*diskSeqno*/);
    } catch (const std::logic_error& e) {
        EXPECT_TRUE(std::string(e.what()).find("Monotonic") !=
                    std::string::npos);
        return;
    }
    FAIL();
}

TEST_F(DurabilityMonitorTest, SeqnoAckReceivedEqualPending) {
    int64_t seqnoStart = 1;
    int64_t seqnoEnd = 3;
    auto numItems = addSyncWrites(seqnoStart, seqnoEnd);
    ASSERT_EQ(3, numItems);
    ASSERT_EQ(0, monitor->public_getNodeWriteSeqnos(replica).memory);
    ASSERT_EQ(0, monitor->public_getNodeAckSeqnos(replica).memory);

    for (int64_t seqno = seqnoStart; seqno <= seqnoEnd; seqno++) {
        EXPECT_NO_THROW(monitor->seqnoAckReceived(
                replica, seqno /*memSeqno*/, 0 /*diskSeqno*/));
        // Check that the tracking advances by 1 at each cycle
        EXPECT_EQ(seqno, monitor->public_getNodeWriteSeqnos(replica).memory);
        EXPECT_EQ(seqno, monitor->public_getNodeAckSeqnos(replica).memory);
        // Check that we committed and removed 1 SyncWrite
        EXPECT_EQ(--numItems, monitor->public_getNumTracked());
        // Check that seqno-tracking is not lost after commit+remove
        EXPECT_EQ(seqno, monitor->public_getNodeWriteSeqnos(replica).memory);
        EXPECT_EQ(seqno, monitor->public_getNodeAckSeqnos(replica).memory);
    }

    // All ack'ed, committed and removed.
    try {
        monitor->seqnoAckReceived(
                replica, seqnoEnd + 1 /*memSeqno*/, 0 /*diskSeqno*/);
    } catch (const std::logic_error& e) {
        EXPECT_TRUE(std::string(e.what()).find("No tracked SyncWrite") !=
                    std::string::npos);
        return;
    }
    FAIL();
}

TEST_F(DurabilityMonitorTest,
       SeqnoAckReceivedGreaterThanPending_ContinuousSeqnos) {
    ASSERT_EQ(3, addSyncWrites(1 /*seqnoStart*/, 3 /*seqnoEnd*/));
    ASSERT_EQ(0, monitor->public_getNodeWriteSeqnos(replica).memory);

    int64_t memoryAckSeqno = 2;
    // Receive a seqno-ack in the middle of tracked seqnos
    EXPECT_EQ(ENGINE_SUCCESS,
              monitor->seqnoAckReceived(
                      replica, memoryAckSeqno, 0 /*diskSeqno*/));
    // Check that the tracking has advanced to the ack'ed seqno
    EXPECT_EQ(memoryAckSeqno,
              monitor->public_getNodeWriteSeqnos(replica).memory);
    EXPECT_EQ(memoryAckSeqno, monitor->public_getNodeAckSeqnos(replica).memory);
    // Check that we committed and removed 2 SyncWrites
    EXPECT_EQ(1, monitor->public_getNumTracked());
    // Check that seqno-tracking is not lost after commit+remove
    EXPECT_EQ(memoryAckSeqno,
              monitor->public_getNodeWriteSeqnos(replica).memory);
    EXPECT_EQ(memoryAckSeqno, monitor->public_getNodeAckSeqnos(replica).memory);
}

TEST_F(DurabilityMonitorTest, SeqnoAckReceivedGreaterThanPending_SparseSeqnos) {
    ASSERT_EQ(3, addSyncWrites({1, 3, 5} /*seqnos*/));
    ASSERT_EQ(0, monitor->public_getNodeWriteSeqnos(replica).memory);

    int64_t memoryAckSeqno = 4;
    // Receive a seqno-ack in the middle of tracked seqnos
    EXPECT_EQ(ENGINE_SUCCESS,
              monitor->seqnoAckReceived(
                      replica, memoryAckSeqno, 0 /*diskSeqno*/));
    // Check that the tracking has advanced to the last tracked seqno before
    // the ack'ed seqno
    EXPECT_EQ(3, monitor->public_getNodeWriteSeqnos(replica).memory);
    // Check that the ack-seqno has been updated correctly
    EXPECT_EQ(memoryAckSeqno, monitor->public_getNodeAckSeqnos(replica).memory);
    // Check that we committed and removed 2 SyncWrites
    EXPECT_EQ(1, monitor->public_getNumTracked());
    // Check that seqno-tracking is not lost after commit+remove
    EXPECT_EQ(3, monitor->public_getNodeWriteSeqnos(replica).memory);
    EXPECT_EQ(memoryAckSeqno, monitor->public_getNodeAckSeqnos(replica).memory);
}

TEST_F(DurabilityMonitorTest,
       SeqnoAckReceivedGreaterThanLastTracked_ContinuousSeqnos) {
    ASSERT_EQ(3, addSyncWrites(1 /*seqnoStart*/, 3 /*seqnoEnd*/));
    ASSERT_EQ(0, monitor->public_getNodeWriteSeqnos(replica).memory);

    int64_t memoryAckSeqno = 4;
    // Receive a seqno-ack greater than the last tracked seqno
    EXPECT_EQ(ENGINE_SUCCESS,
              monitor->seqnoAckReceived(
                      replica, memoryAckSeqno, 0 /*diskSeqno*/));
    // Check that the tracking has advanced to the last tracked seqno
    EXPECT_EQ(3, monitor->public_getNodeWriteSeqnos(replica).memory);
    // Check that the ack-seqno has been updated correctly
    EXPECT_EQ(memoryAckSeqno, monitor->public_getNodeAckSeqnos(replica).memory);
    // Check that we committed and removed all SyncWrites
    EXPECT_EQ(0, monitor->public_getNumTracked());
    // Check that seqno-tracking is not lost after commit+remove
    EXPECT_EQ(3, monitor->public_getNodeWriteSeqnos(replica).memory);
    EXPECT_EQ(memoryAckSeqno, monitor->public_getNodeAckSeqnos(replica).memory);

    // All ack'ed, committed and removed.
    try {
        monitor->seqnoAckReceived(replica, 20 /*memSeqno*/, 0 /*diskSeqno*/);
    } catch (const std::logic_error& e) {
        EXPECT_TRUE(std::string(e.what()).find("No tracked SyncWrite") !=
                    std::string::npos);
        return;
    }
    FAIL();
}

TEST_F(DurabilityMonitorTest,
       SeqnoAckReceivedGreaterThanLastTracked_SparseSeqnos) {
    ASSERT_EQ(3, addSyncWrites({1, 3, 5} /*seqnos*/));
    ASSERT_EQ(0, monitor->public_getNodeWriteSeqnos(replica).memory);

    int64_t memoryAckSeqno = 10;
    // Receive a seqno-ack greater than the last tracked seqno
    EXPECT_EQ(ENGINE_SUCCESS,
              monitor->seqnoAckReceived(
                      replica, memoryAckSeqno, 0 /*diskSeqno*/));
    // Check that the tracking has advanced to the last tracked seqno
    EXPECT_EQ(5, monitor->public_getNodeWriteSeqnos(replica).memory);
    // Check that the ack-seqno has been updated correctly
    EXPECT_EQ(memoryAckSeqno, monitor->public_getNodeAckSeqnos(replica).memory);
    // Check that we committed and removed all SyncWrites
    EXPECT_EQ(0, monitor->public_getNumTracked());
    // Check that seqno-tracking is not lost after commit+remove
    EXPECT_EQ(5, monitor->public_getNodeWriteSeqnos(replica).memory);
    EXPECT_EQ(memoryAckSeqno, monitor->public_getNodeAckSeqnos(replica).memory);

    // All ack'ed, committed and removed.
    try {
        monitor->seqnoAckReceived(replica, 20 /*memSeqno*/, 0 /*diskSeqno*/);
    } catch (const std::logic_error& e) {
        EXPECT_TRUE(std::string(e.what()).find("No tracked SyncWrite") !=
                    std::string::npos);
        return;
    }
    FAIL();
}

TEST_F(DurabilityMonitorTest,
       SeqnoAckReceived_MemorySeqnoSmallerThanDiskSeqno) {
    addSyncWrites({1} /*seqnos*/);
    try {
        monitor->seqnoAckReceived(replica, 0 /*memSeqno*/, 1 /*diskSeqno*/);
    } catch (const std::logic_error& e) {
        EXPECT_TRUE(std::string(e.what()).find("memorySeqno < diskSeqno") !=
                    std::string::npos);
        return;
    }
    FAIL();
}

// @todo: Refactor test suite and expand test cases
TEST_F(DurabilityMonitorTest, SeqnoAckReceived_PersistToMajority) {
    ASSERT_EQ(3,
              addSyncWrites({1, 3, 5} /*seqnos*/,
                            {cb::durability::Level::PersistToMajority,
                             0 /*timeout*/}));
    ASSERT_EQ(0, monitor->public_getNodeWriteSeqnos(replica).disk);
    EXPECT_EQ(0, monitor->public_getNodeAckSeqnos(replica).disk);

    int64_t memAckSeqno = 10, diskAckSeqno = 10;

    // Receive a seqno-ack greater than the last tracked seqno
    EXPECT_EQ(ENGINE_SUCCESS,
              monitor->seqnoAckReceived(replica, memAckSeqno, diskAckSeqno));

    // Check that we have not committed as the active has not ack'ed the
    // persisted seqno
    EXPECT_EQ(3, monitor->public_getNumTracked());

    // Check that the tracking for Replica has been updated correctly
    EXPECT_EQ(5, monitor->public_getNodeWriteSeqnos(replica).disk);
    EXPECT_EQ(diskAckSeqno, monitor->public_getNodeAckSeqnos(replica).disk);

    // Check that the tracking for Active has not moved yet
    EXPECT_EQ(0, monitor->public_getNodeWriteSeqnos(active).disk);
    EXPECT_EQ(0, monitor->public_getNodeAckSeqnos(active).disk);

    // @todo: Simulating the active->active disk-seqno ack with the next call.
    //     Note that this feature has not been implemented yet, and probably
    //     I will implement it using a different code path (in some way I have
    //     to notify the DurabilityMonitor at persistence).
    EXPECT_EQ(ENGINE_SUCCESS,
              monitor->seqnoAckReceived(active, memAckSeqno, diskAckSeqno));

    // Check that we committed and removed all SyncWrites
    EXPECT_EQ(0, monitor->public_getNumTracked());

    // Check that the tracking for Active has been updated correctly
    EXPECT_EQ(5, monitor->public_getNodeWriteSeqnos(active).disk);
    EXPECT_EQ(diskAckSeqno, monitor->public_getNodeAckSeqnos(active).disk);

    // All ack'ed, committed and removed.
    try {
        monitor->seqnoAckReceived(replica, 20 /*memSeqno*/, 20 /*diskSeqno*/);
    } catch (const std::logic_error& e) {
        EXPECT_TRUE(std::string(e.what()).find("No tracked SyncWrite") !=
                    std::string::npos);
        return;
    }
    FAIL();
}

TEST_F(DurabilityMonitorTest, RegisterChain_Empty) {
    try {
        monitor->registerReplicationChain({});
    } catch (const std::logic_error& e) {
        EXPECT_TRUE(std::string(e.what()).find("Empty chain") !=
                    std::string::npos);
        return;
    }
    FAIL();
}

TEST_F(DurabilityMonitorTest, RegisterChain_TooManyNodes) {
    try {
        monitor->registerReplicationChain(
                {"active", "replica1", "replica2", "replica3", "replica4"});
    } catch (const std::logic_error& e) {
        EXPECT_TRUE(std::string(e.what()).find("Too many nodes in chain") !=
                    std::string::npos);
        return;
    }
    FAIL();
}

TEST_F(DurabilityMonitorTest, RegisterChain_NodeDuplicate) {
    try {
        monitor->registerReplicationChain({"node1", "node1"});
    } catch (const std::logic_error& e) {
        EXPECT_TRUE(std::string(e.what()).find("Duplicate node") !=
                    std::string::npos);
        return;
    }
    FAIL();
}

// @todo: Extend to disk-seqno
TEST_F(DurabilityMonitorTest, SeqnoAckReceived_MultipleReplica) {
    const std::string active = "active";
    const std::string replica1 = "replica1";
    const std::string replica2 = "replica2";
    const std::string replica3 = "replica3";

    ASSERT_NO_THROW(monitor->registerReplicationChain(
            {active, replica1, replica2, replica3}));
    ASSERT_EQ(4, monitor->public_getReplicationChainSize());

    addSyncWrite(1 /*seqno*/);

    // Active has implicitly ack'ed (SyncWrite added for tracking /after/ being
    // enqueued into the CheckpointManager)
    EXPECT_EQ(1, monitor->public_getNodeWriteSeqnos(active).memory);
    EXPECT_EQ(1, monitor->public_getNodeAckSeqnos(active).memory);

    // Nothing ack'ed yet for replica
    for (const auto& replica : {replica1, replica2, replica3}) {
        EXPECT_EQ(0, monitor->public_getNodeWriteSeqnos(replica).memory);
        EXPECT_EQ(0, monitor->public_getNodeAckSeqnos(replica).memory);
    }
    // Nothing committed
    EXPECT_EQ(1, monitor->public_getNumTracked());

    // replica2 acks
    EXPECT_EQ(ENGINE_SUCCESS,
              monitor->seqnoAckReceived(
                      replica2, 1 /*memSeqno*/, 0 /*diskSeqno*/));
    EXPECT_EQ(1, monitor->public_getNodeWriteSeqnos(replica2).memory);
    EXPECT_EQ(1, monitor->public_getNodeAckSeqnos(replica2).memory);
    // Nothing committed yet
    EXPECT_EQ(1, monitor->public_getNumTracked());

    // replica3 acks
    EXPECT_EQ(ENGINE_SUCCESS,
              monitor->seqnoAckReceived(
                      replica3, 1 /*memSeqno*/, 0 /*diskSeqno*/));
    EXPECT_EQ(1, monitor->public_getNodeWriteSeqnos(replica3).memory);
    EXPECT_EQ(1, monitor->public_getNodeAckSeqnos(replica3).memory);
    // Requirements verified, committed
    EXPECT_EQ(0, monitor->public_getNumTracked());

    // replica1 has not ack'ed yet
    EXPECT_EQ(0, monitor->public_getNodeWriteSeqnos(replica1).memory);
    EXPECT_EQ(0, monitor->public_getNodeAckSeqnos(replica1).memory);
}
