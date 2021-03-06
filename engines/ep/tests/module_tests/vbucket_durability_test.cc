/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2018 Couchbase, Inc
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

#include "vbucket_durability_test.h"

#include "checkpoint.h"
#include "checkpoint_manager.h"
#include "checkpoint_utils.h"
#include "test_helpers.h"

#include "../mock/mock_durability_monitor.h"

#include <gmock/gmock.h>

using namespace std::string_literals;

void VBucketDurabilityTest::SetUp() {
    VBucketTest::SetUp();
    ht = &vbucket->ht;
    ckptMgr = vbucket->checkpointManager.get();
    // Note: MockDurabilityMonitor is used only for accessing the base
    //     class protected members, it doesn't change the base class layout
    monitor = reinterpret_cast<MockDurabilityMonitor*>(
            vbucket->durabilityMonitor.get());
    ASSERT_GT(monitor->public_getReplicationChainSize(), 0);
}

size_t VBucketDurabilityTest::storeSyncWrites(
        const std::vector<int64_t>& seqnos) {
    if (seqnos.empty()) {
        throw std::logic_error(
                "VBucketDurabilityTest::addSyncWrites: seqnos list is empty");
    }

    // @todo: For now this function is supposed to be called once per test,
    //     expand if necessary
    ht->clear();
    ckptMgr->clear(*vbucket, 0 /*lastBySeqno*/);

    // In general we need to test SyncWrites at sparse seqnos. To achieve that
    // we have 2 options (e.g., if we want to add SyncWrites with seqnos
    // {1, 3, 5}):
    // 1) We can use the VBucket::set interface. Given that bySeqno is
    //     auto-generated then we have to add non-sync mutations with seqnos
    //     {2, 4}.
    // 2) We can call directly VBucket::processSet and provide our bySeqno. We
    //     need to set the Checkpoint snapshot boundaries manually in this case
    //     (e.g., [1, 5]), the set fails otherwise.
    // I go with the latter way, that is the reason of the the following call.
    ckptMgr->createSnapshot(seqnos.at(0), seqnos.at(seqnos.size() - 1));
    EXPECT_EQ(1, ckptMgr->getNumCheckpoints());

    size_t numStored = ht->getNumItems();
    size_t numCkptItems = ckptMgr->getNumItems();
    size_t numTracked = monitor->public_getNumTracked();
    for (auto seqno : seqnos) {
        auto item = Item(makeStoredDocKey("key" + std::to_string(seqno)),
                         0 /*flags*/,
                         0 /*exp*/,
                         "value",
                         5 /*valueSize*/,
                         PROTOCOL_BINARY_RAW_BYTES,
                         0 /*cas*/,
                         seqno);
        using namespace cb::durability;
        item.setPendingSyncWrite(Requirements(Level::Majority, 0 /*timeout*/));
        VBQueueItemCtx ctx;
        ctx.genBySeqno = GenerateBySeqno::No;
        ctx.durability = DurabilityItemCtx{item.getDurabilityReqs(), cookie};

        EXPECT_EQ(MutationStatus::WasClean,
                  public_processSet(item, 0 /*cas*/, ctx));

        EXPECT_EQ(++numStored, ht->getNumItems());
        EXPECT_EQ(++numTracked, monitor->public_getNumTracked());
        EXPECT_EQ(++numCkptItems, ckptMgr->getNumItems());
    }
    return numStored;
}

void VBucketDurabilityTest::testSyncWrites(const std::vector<int64_t>& seqnos) {
    auto numStored = storeSyncWrites(seqnos);
    ASSERT_EQ(seqnos.size(), numStored);

    for (auto seqno : seqnos) {
        auto key = makeStoredDocKey("key" + std::to_string(seqno));

        EXPECT_EQ(nullptr, ht->findForRead(key).storedValue);
        const auto sv = ht->findForWrite(key).storedValue;
        EXPECT_NE(nullptr, sv);
        EXPECT_EQ(CommittedState::Pending, sv->getCommitted());
    }

    const auto& ckptList =
            CheckpointManagerTestIntrospector::public_getCheckpointList(
                    *ckptMgr);
    ASSERT_EQ(1, ckptList.size());
    EXPECT_EQ(numStored, ckptList.front()->getNumItems());
    for (const auto& qi : *ckptList.front()) {
        if (!qi->isCheckPointMetaItem()) {
            EXPECT_EQ(CommittedState::Pending, qi->getCommitted());
            EXPECT_EQ(queue_op::pending_sync_write, qi->getOperation());
        }
    }

    // Simulate flush + checkpoint-removal
    ckptMgr->clear(*vbucket, 0 /*lastBySeqno*/);

    // The active sends DCP_PREPARE messages to the replica, here I simulate
    // the replica DCP_SEQNO_ACK response
    vbucket->seqnoAcknowledged(replica,
                               seqnos.at(seqnos.size() - 1) /*memorySeqno*/,
                               0 /*diskSeqno*/);

    for (auto seqno : seqnos) {
        auto key = makeStoredDocKey("key" + std::to_string(seqno));

        const auto sv = ht->findForRead(key).storedValue;
        EXPECT_NE(nullptr, sv);
        EXPECT_NE(nullptr, ht->findForWrite(key).storedValue);
        EXPECT_EQ(CommittedState::CommittedViaPrepare, sv->getCommitted());
    }

    ASSERT_EQ(1, ckptList.size());
    EXPECT_EQ(numStored, ckptList.front()->getNumItems());
    for (const auto& qi : *ckptList.front()) {
        if (!qi->isCheckPointMetaItem()) {
            EXPECT_EQ(CommittedState::CommittedViaPrepare, qi->getCommitted());
            EXPECT_EQ(queue_op::commit_sync_write, qi->getOperation());
        }
    }
}

TEST_P(VBucketDurabilityTest, SyncWrites_ContinuousSeqnos) {
    testSyncWrites({1, 2, 3});
}

TEST_P(VBucketDurabilityTest, SyncWrites_SparseSeqnos) {
    testSyncWrites({1, 3, 10, 20, 30});
}

// Test cases which run in both Full and Value eviction
INSTANTIATE_TEST_CASE_P(
        FullAndValueEviction,
        VBucketDurabilityTest,
        ::testing::Values(VALUE_ONLY, FULL_EVICTION),
        [](const ::testing::TestParamInfo<item_eviction_policy_t>& info) {
            if (info.param == VALUE_ONLY) {
                return "VALUE_ONLY";
            } else {
                return "FULL_EVICTION";
            }
        });

// Positive test for validateSetStateMeta 'topology' key - check that
// valid topology values are accepted.
TEST(VBucketDurabilityTest, validateSetStateMetaTopology) {
    using nlohmann::json;

    // Single chain, one node
    EXPECT_EQ(""s,
              VBucket::validateSetStateMeta(
                      {{"topology", json::array({{"active"}})}}));

    // Single chain, two nodes.
    EXPECT_EQ(""s,
              VBucket::validateSetStateMeta(
                      {{"topology", json::array({{"active", "replica1"}})}}));

    // Single chain, three nodes.
    EXPECT_EQ(""s,
              VBucket::validateSetStateMeta(
                      {{"topology",
                        json::array({{"active", "replica1", "replica2"}})}}));

    // Single chain, four nodes.
    EXPECT_EQ(""s,
              VBucket::validateSetStateMeta({{"topology",
                                              json::array({{"active",
                                                            "replica1",
                                                            "replica2",
                                                            "replica3"}})}}));

    // Two chains, one node
    EXPECT_EQ(""s,
              VBucket::validateSetStateMeta(
                      {{"topology", json::array({{"activeA"}, {"activeB"}})}}));

    // Two chains, two nodes.
    EXPECT_EQ(""s,
              VBucket::validateSetStateMeta(
                      {{"topology",
                        json::array({{"activeA", "replicaA1"},
                                     {"activeB", "replicaB1"}})}}));

    // Two chains, three nodes.
    EXPECT_EQ(
            ""s,
            VBucket::validateSetStateMeta(
                    {{"topology",
                      json::array({{"activeA", "replicaA1", "replicaA2"},
                                   {"activeB", "replicaB1", "replicaB2"}})}}));

    // Two chains, four nodes.
    EXPECT_EQ(""s,
              VBucket::validateSetStateMeta({{"topology",
                                              json::array({{"activeA",
                                                            "replicaA1",
                                                            "replicaA2",
                                                            "replicaA3"},
                                                           {"activeB",
                                                            "replicaB1",
                                                            "replicaB2",
                                                            "replicaB3"}})}}));
}

TEST(VBucketDurabilityTest, validateSetStateMetaTopologyNegative) {
    using nlohmann::json;
    using testing::HasSubstr;

    // Too few (0) chains (empty json::array)
    EXPECT_THAT(VBucket::validateSetStateMeta({{"topology", json::array({})}}),
                HasSubstr("topology' must contain 1..2 elements"));

    // Too many (>2) chains
    EXPECT_THAT(
            VBucket::validateSetStateMeta(
                    {{"topology",
                      json::array({{"activeA"}, {"activeB"}, {"activeC"}})}}),
            HasSubstr("topology' must contain 1..2 elements"));

    // Two chains, second contains too many (5) nodes.
    EXPECT_THAT(
            VBucket::validateSetStateMeta({{"topology",
                                            json::array({{"active", "replica"},
                                                         {"active",
                                                          "replica1",
                                                          "replica2",
                                                          "replica3",
                                                          "replica4"}})}}),
            HasSubstr("chain[1] must contain 1..4 nodes"));

    // Incorrect structure - flat array not nested.
    EXPECT_THAT(VBucket::validateSetStateMeta(
                        {{"topology", json::array({"activeA", "replica"})}}),
                HasSubstr("chain[0] must be an array"));

    // Incorrect structure - elements are not strings.
    EXPECT_THAT(VBucket::validateSetStateMeta(
                        {{"topology",
                          json::array({{"activeA", "replicaA1"},
                                       {"activeB", 1.1}})}}),
                HasSubstr("chain[1] node[1] must be a string"));
}
