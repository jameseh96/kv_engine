/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2019 Couchbase, Inc
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

#include "dcp_durability_stream_test.h"

#include "../../src/dcp/backfill-manager.h"
#include "checkpoint_utils.h"
#include "dcp/response.h"
#include "dcp_utils.h"
#include "durability/active_durability_monitor.h"
#include "durability/durability_monitor.h"
#include "durability/passive_durability_monitor.h"
#include "test_helpers.h"
#include "vbucket_utils.h"

#include "../mock/mock_dcp_consumer.h"
#include "../mock/mock_stream.h"
#include "../mock/mock_synchronous_ep_engine.h"

void DurabilityActiveStreamTest::SetUp() {
    SingleThreadedActiveStreamTest::SetUp();
    setUp(false /*startCheckpointProcessorTask*/);
}

void DurabilityActiveStreamTest::TearDown() {
    SingleThreadedActiveStreamTest::TearDown();
}

void DurabilityActiveStreamTest::setUp(bool startCheckpointProcessorTask) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology", nlohmann::json::array({{active, replica}})}});

    // Enable SyncReplication and flow-control (Producer BufferLog)
    setupProducer({{"enable_synchronous_replication", "true"},
                   {"connection_buffer_size", "52428800"},
                   {"consumer_name", "test_consumer"}},
                  startCheckpointProcessorTask);
    ASSERT_TRUE(stream->public_supportSyncReplication());
}

void DurabilityActiveStreamTest::testSendDcpPrepare() {
    auto vb = engine->getVBucket(vbid);
    auto& ckptMgr = *vb->checkpointManager;
    // Get rid of set_vb_state and any other queue_op we are not interested in
    ckptMgr.clear(*vb, 0 /*seqno*/);

    const auto key = makeStoredDocKey("key");
    const std::string value = "value";
    auto item = makePendingItem(
            key,
            value,
            cb::durability::Requirements(cb::durability::Level::Majority,
                                         1 /*timeout*/));
    VBQueueItemCtx ctx;
    ctx.durability =
            DurabilityItemCtx{item->getDurabilityReqs(), nullptr /*cookie*/};
    {
        auto cHandle = vb->lockCollections(item->getKey());
        EXPECT_EQ(ENGINE_EWOULDBLOCK,
                  vb->set(*item, cookie, *engine, {}, cHandle));
    }
    vb->notifyActiveDMOfLocalSyncWrite();

    // We don't account Prepares in VB stats
    EXPECT_EQ(0, vb->getNumItems());
    // We do in HT stats
    EXPECT_EQ(1, vb->ht.getNumItems());

    auto prepareSeqno = 1;
    uint64_t cas;
    {
        const auto sv = vb->ht.findForWrite(key);
        ASSERT_TRUE(sv.storedValue);
        ASSERT_EQ(CommittedState::Pending, sv.storedValue->getCommitted());
        ASSERT_EQ(prepareSeqno, sv.storedValue->getBySeqno());
        cas = sv.storedValue->getCas();
    }

    const auto& ckptList =
            CheckpointManagerTestIntrospector::public_getCheckpointList(
                    ckptMgr);
    // 1 checkpoint
    ASSERT_EQ(1, ckptList.size());
    const auto* ckpt = ckptList.front().get();
    ASSERT_EQ(checkpoint_state::CHECKPOINT_OPEN, ckpt->getState());
    // empty-item
    auto it = ckpt->begin();
    ASSERT_EQ(queue_op::empty, (*it)->getOperation());
    // 1 metaitem (checkpoint-start)
    it++;
    ASSERT_EQ(1, ckpt->getNumMetaItems());
    EXPECT_EQ(queue_op::checkpoint_start, (*it)->getOperation());
    // 1 non-metaitem is pending and contains the expected value
    it++;
    ASSERT_EQ(1, ckpt->getNumItems());
    EXPECT_EQ(queue_op::pending_sync_write, (*it)->getOperation());
    EXPECT_EQ(key, (*it)->getKey());
    EXPECT_EQ(value, (*it)->getValue()->to_s());

    // We must have ckpt-start + Prepare
    auto outstandingItemsResult = stream->public_getOutstandingItems(*vb);
    ASSERT_EQ(2, outstandingItemsResult.items.size());
    ASSERT_EQ(queue_op::checkpoint_start,
              outstandingItemsResult.items.at(0)->getOperation());
    ASSERT_EQ(queue_op::pending_sync_write,
              outstandingItemsResult.items.at(1)->getOperation());
    // Stream::readyQ still empty
    ASSERT_EQ(0, stream->public_readyQSize());
    // Push items into the Stream::readyQ
    stream->public_processItems(outstandingItemsResult);

    // No message processed, BufferLog empty
    ASSERT_EQ(0, producer->getBytesOutstanding());

    // readyQ must contain a SnapshotMarker (+ a Prepare)
    ASSERT_EQ(2, stream->public_readyQSize());
    auto resp = stream->public_nextQueuedItem();
    ASSERT_TRUE(resp);
    EXPECT_EQ(DcpResponse::Event::SnapshotMarker, resp->getEvent());

    // Simulate the Replica ack'ing the SnapshotMarker's bytes
    auto bytesOutstanding = producer->getBytesOutstanding();
    ASSERT_GT(bytesOutstanding, 0);
    producer->ackBytesOutstanding(bytesOutstanding);
    ASSERT_EQ(0, producer->getBytesOutstanding());

    // readyQ must contain a DCP_PREPARE
    ASSERT_EQ(1, stream->public_readyQSize());
    resp = stream->public_nextQueuedItem();
    ASSERT_TRUE(resp);
    EXPECT_EQ(DcpResponse::Event::Prepare, resp->getEvent());
    EXPECT_EQ(prepareSeqno, *resp->getBySeqno());
    auto& prepare = static_cast<MutationResponse&>(*resp);
    EXPECT_EQ(key, prepare.getItem()->getKey());
    EXPECT_EQ(value, prepare.getItem()->getValue()->to_s());
    EXPECT_EQ(cas, prepare.getItem()->getCas());

    // The expected size of a DCP_PREPARE is 57 + key-size + value-size.
    // Note that the base-size=57 is similar to the one of a DCP_MUTATION
    // (55), + 1 for delete-flag, + 3 for durability-requirements, - 2 for
    // missing optional-extra-length.
    bytesOutstanding =
            57 + key.makeDocKeyWithoutCollectionID().size() + value.size();
    ASSERT_EQ(bytesOutstanding, producer->getBytesOutstanding());
    // Simulate the Replica ack'ing the Prepare's bytes
    producer->ackBytesOutstanding(bytesOutstanding);
    ASSERT_EQ(0, producer->getBytesOutstanding());

    // readyQ empty now
    ASSERT_EQ(0, stream->public_readyQSize());
    resp = stream->public_nextQueuedItem();
    ASSERT_FALSE(resp);
}

TEST_P(DurabilityActiveStreamTest, SendDcpPrepare) {
    testSendDcpPrepare();
}

void DurabilityActiveStreamTest::testSendCompleteSyncWrite(Resolution res) {
    // First, we need to enqueue a Prepare.
    testSendDcpPrepare();
    auto vb = engine->getVBucket(vbid);
    const auto key = makeStoredDocKey("key");
    const uint64_t prepareSeqno = 1;
    {
        ASSERT_FALSE(vb->ht.findForRead(key).storedValue);
        const auto sv = vb->ht.findForWrite(key);
        ASSERT_TRUE(sv.storedValue);
        ASSERT_EQ(CommittedState::Pending, sv.storedValue->getCommitted());
        ASSERT_EQ(prepareSeqno, sv.storedValue->getBySeqno());
    }

    // Now we proceed with testing the Commit/Abort of that Prepare
    auto& ckptMgr = *vb->checkpointManager;

    // The seqno of the Committed/Aborted item
    const auto completedSeqno = prepareSeqno + 1;

    switch (res) {
    case Resolution::Commit: {
        // FirstChain on Active has been set to {active, replica}. Given that
        // active has already implicitly ack'ed (as we have queued a Level
        // Majority Prepare), simulating a SeqnoAck received from replica
        // satisfies Durability Requirements and triggers Commit. So, the
        // following indirectly calls VBucket::commit
        stream->seqnoAck(replica, prepareSeqno);
        // Note: At FE we have an exact item count only at persistence.
        auto evictionType = std::get<1>(GetParam());
        if (evictionType == "value_only" || !persistent()) {
            EXPECT_EQ(1, vb->getNumItems());
        } else {
            EXPECT_EQ(0, vb->getNumItems());
        }
        ASSERT_TRUE(vb->ht.findForWrite(key).storedValue);
        const auto sv = vb->ht.findForRead(key);
        ASSERT_TRUE(sv.storedValue);
        ASSERT_EQ(CommittedState::CommittedViaPrepare,
                  sv.storedValue->getCommitted());
        ASSERT_EQ(completedSeqno, sv.storedValue->getBySeqno());
        break;
    }
    case Resolution::Abort:
        // Simulate timeout, indirectly calls VBucket::abort
        vb->processDurabilityTimeout(std::chrono::steady_clock::now() +
                                     std::chrono::milliseconds(1000));
        EXPECT_EQ(0, vb->getNumItems());
        break;
    }

    // Verify state of the checkpoint(s).
    const auto& ckptList =
            CheckpointManagerTestIntrospector::public_getCheckpointList(
                    ckptMgr);
    if (res == Resolution::Abort) {
        // Note: We avoid de-duplication of durability-items (Prepare/Abort)
        // by:
        // (1) closing the open checkpoint (the one that contains the Prepare)
        // (2) creating a new open checkpoint
        // (3) queueing the Commit/Abort in the new open checkpoint
        // So we must have 2 checkpoints now.
        ASSERT_EQ(2, ckptList.size());
    }

    const auto* ckpt = ckptList.back().get();
    EXPECT_EQ(checkpoint_state::CHECKPOINT_OPEN, ckpt->getState());
    // empty-item
    auto it = ckpt->begin();
    EXPECT_EQ(queue_op::empty, (*it)->getOperation());
    // 1 metaitem (checkpoint-start)
    it++;
    ASSERT_EQ(1, ckpt->getNumMetaItems());
    EXPECT_EQ(queue_op::checkpoint_start, (*it)->getOperation());
    it++;

    switch (res) {
    case Resolution::Commit:
        // For Commit, Prepare is in the same checkpoint.
        ASSERT_EQ(2, ckpt->getNumItems());
        EXPECT_EQ(queue_op::pending_sync_write, (*it)->getOperation());
        it++;
        EXPECT_EQ(queue_op::commit_sync_write, (*it)->getOperation());
        EXPECT_TRUE((*it)->getValue()) << "Commit should carry a value";
        break;
    case Resolution::Abort:
        // For Abort, the prepare is in the previous checkpoint.
        ASSERT_EQ(1, ckpt->getNumItems());
        EXPECT_EQ(queue_op::abort_sync_write, (*it)->getOperation());
        EXPECT_FALSE((*it)->getValue()) << "Abort should carry no value";
        break;
    }

    // Fetch items via DCP stream.
    auto outstandingItemsResult = stream->public_getOutstandingItems(*vb);
    switch (res) {
    case Resolution::Commit:
        ASSERT_EQ(1, outstandingItemsResult.items.size())
                << "Expected 1 item (Commit)";
        EXPECT_EQ(queue_op::commit_sync_write,
                  outstandingItemsResult.items.at(0)->getOperation());
        break;
    case Resolution::Abort:
        ASSERT_EQ(2, outstandingItemsResult.items.size())
                << "Expected 2 items (CkptStart, Abort)";
        EXPECT_EQ(queue_op::checkpoint_start,
                  outstandingItemsResult.items.at(0)->getOperation());
        EXPECT_EQ(queue_op::abort_sync_write,
                  outstandingItemsResult.items.at(1)->getOperation());
        break;
    }

    // readyQ still empty
    ASSERT_EQ(0, stream->public_readyQSize());

    // Push items into readyQ
    stream->public_processItems(outstandingItemsResult);

    // No message processed, BufferLog empty
    ASSERT_EQ(0, producer->getBytesOutstanding());

    // readyQ must contain SnapshotMarker
    ASSERT_EQ(2, stream->public_readyQSize());
    auto resp = stream->public_nextQueuedItem();
    ASSERT_TRUE(resp);
    EXPECT_EQ(DcpResponse::Event::SnapshotMarker, resp->getEvent());

    // Simulate the Replica ack'ing the SnapshotMarker's bytes
    auto bytesOutstanding = producer->getBytesOutstanding();
    ASSERT_GT(bytesOutstanding, 0);
    producer->ackBytesOutstanding(bytesOutstanding);
    ASSERT_EQ(0, producer->getBytesOutstanding());

    // readyQ must contain DCP_COMMIT/DCP_ABORT
    resp = stream->public_nextQueuedItem();
    ASSERT_TRUE(resp);
    switch (res) {
    case Resolution::Commit: {
        EXPECT_EQ(DcpResponse::Event::Commit, resp->getEvent());
        const auto& commit = dynamic_cast<CommitSyncWrite&>(*resp);
        EXPECT_EQ(key, commit.getKey());
        EXPECT_EQ(completedSeqno, *commit.getBySeqno());
        break;
    }
    case Resolution::Abort: {
        EXPECT_EQ(DcpResponse::Event::Abort, resp->getEvent());
        const auto& abort = dynamic_cast<AbortSyncWrite&>(*resp);
        EXPECT_EQ(key, abort.getKey());
        EXPECT_EQ(prepareSeqno, abort.getPreparedSeqno());
        EXPECT_EQ(completedSeqno, abort.getAbortSeqno());
        break;
    }
    }

    // The expected size of a DCP_COMMT / DCP_ABORT is:
    // + 24 (header)
    // + 8  (prepare seqno)
    // + 8 (Commit/Abort seqno)
    // + key size
    EXPECT_EQ(24 + 8 + 8 + key.size(), producer->getBytesOutstanding());

    // readyQ empty now
    EXPECT_EQ(0, stream->public_readyQSize());
    resp = stream->public_popFromReadyQ();
    EXPECT_FALSE(resp);
}

/*
 * This test checks that the ActiveStream::readyQ contains the right DCP
 * messages during the journey of a Committed sync-write.
 */
TEST_P(DurabilityActiveStreamTest, SendDcpCommit) {
    testSendCompleteSyncWrite(Resolution::Commit);
}

/*
 * This test checks that the ActiveStream::readyQ contains the right DCP
 * messages during the journey of an Aborted sync-write.
 */
TEST_P(DurabilityActiveStreamTest, SendDcpAbort) {
    testSendCompleteSyncWrite(Resolution::Abort);
}

TEST_P(DurabilityActiveStreamEphemeralTest, BackfillDurabilityLevel) {
    auto vb = engine->getVBucket(vbid);
    auto& ckptMgr = *vb->checkpointManager;
    // Get rid of set_vb_state and any other queue_op we are not interested in
    ckptMgr.clear(*vb, 0 /*seqno*/);

    const auto key = makeStoredDocKey("key");
    const auto& value = "value";
    auto item = makePendingItem(
            key,
            value,
            cb::durability::Requirements(cb::durability::Level::Majority,
                                         1 /*timeout*/));
    VBQueueItemCtx ctx;
    ctx.durability =
            DurabilityItemCtx{item->getDurabilityReqs(), nullptr /*cookie*/};

    EXPECT_EQ(MutationStatus::WasClean, public_processSet(*vb, *item, ctx));

    // We don't account Prepares in VB stats
    EXPECT_EQ(0, vb->getNumItems());

    stream->transitionStateToBackfilling();
    ASSERT_TRUE(stream->isBackfilling());

    // Run the backfill we scheduled when we transitioned to the backfilling
    // state
    auto& bfm = producer->getBFM();
    bfm.backfill();

    const auto& readyQ = stream->public_readyQ();
    EXPECT_EQ(2, readyQ.size());

    // First item is a snapshot marker so just skip it
    auto resp = stream->public_popFromReadyQ();
    resp = stream->public_popFromReadyQ();
    ASSERT_TRUE(resp);
    EXPECT_EQ(DcpResponse::Event::Prepare, resp->getEvent());
    const auto& prep = static_cast<MutationResponse&>(*resp);
    const auto respItem = prep.getItem();
    EXPECT_EQ(cb::durability::Level::Majority,
              respItem->getDurabilityReqs().getLevel());
    EXPECT_TRUE(respItem->getDurabilityReqs().getTimeout().isInfinite());
}

TEST_P(DurabilityActiveStreamTest, BackfillAbort) {
    producer->createCheckpointProcessorTask();
    producer->scheduleCheckpointProcessorTask();

    auto vb = engine->getVBucket(vbid);

    auto& ckptMgr = *vb->checkpointManager;

    // Get rid of set_vb_state and any other queue_op we are not interested in
    ckptMgr.clear(*vb, 0 /*seqno*/);

    const auto key = makeStoredDocKey("key");
    const auto& value = "value";
    auto item = makePendingItem(
            key,
            value,
            cb::durability::Requirements(cb::durability::Level::Majority,
                                         1 /*timeout*/));
    VBQueueItemCtx ctx;
    ctx.durability =
            DurabilityItemCtx{item->getDurabilityReqs(), nullptr /*cookie*/};
    EXPECT_EQ(MutationStatus::WasClean, public_processSet(*vb, *item, ctx));
    EXPECT_EQ(ENGINE_SUCCESS,
              vb->abort(key,
                        vb->getHighSeqno(),
                        {} /*abortSeqno*/,
                        vb->lockCollections(key)));

    flushVBucketToDiskIfPersistent(vbid, 1);

    stream->transitionStateToBackfilling();
    ASSERT_TRUE(stream->isBackfilling());

    auto& bfm = producer->getBFM();
    bfm.backfill();
    bfm.backfill();
    const auto& readyQ = stream->public_readyQ();
    EXPECT_EQ(2, readyQ.size());

    // First item is a snapshot marker so just skip it
    auto resp = stream->public_popFromReadyQ();
    resp = stream->public_popFromReadyQ();
    ASSERT_TRUE(resp);
    EXPECT_EQ(DcpResponse::Event::Abort, resp->getEvent());
    const auto& abrt = static_cast<AbortSyncWrite&>(*resp);
    ASSERT_TRUE(abrt.getBySeqno());
    EXPECT_EQ(2, *abrt.getBySeqno());
    EXPECT_EQ(1, abrt.getPreparedSeqno());

    producer->cancelCheckpointCreatorTask();
}

TEST_P(DurabilityActiveStreamTest, RemoveUnknownSeqnoAckAtDestruction) {
    testSendDcpPrepare();
    flushVBucketToDiskIfPersistent(vbid, 1);

    // We don't include prepares in the numItems stat (should not exist in here)
    auto vb = engine->getVBucket(vbid);
    EXPECT_EQ(0, vb->getNumItems());

    // Our topology gives replica name as "replica" an our producer/stream has
    // name "test_producer". Simulate a seqno ack by calling the vBucket level
    // function.
    stream->seqnoAck(producer->getConsumerName(), 1);

    // An unknown seqno ack should not have committed the item
    EXPECT_EQ(0, vb->getNumItems());

    // Disconnect the ActiveStream
    stream->setDead(END_STREAM_DISCONNECTED);

    // Attempt to ack the seqno again. The stream is dead so we should not
    // process the ack although we return SUCCESS to avoid tearing down any
    // connections. We verify that the seqno ack does not exist in the map
    // by performing the topology change that would commit the prepare if it
    // did.
    EXPECT_EQ(ENGINE_SUCCESS, stream->seqnoAck(producer->getConsumerName(), 1));

    // If the seqno ack still existed in the queuedSeqnoAcks map then it would
    // result in a commit on topology change
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology",
              nlohmann::json::array(
                      {{"active", "replica1", producer->getConsumerName()}})}});

    EXPECT_EQ(0, vb->getNumItems());
}

TEST_P(DurabilityActiveStreamTest, RemoveCorrectQueuedAckAtStreamSetDead) {
    testSendDcpPrepare();
    flushVBucketToDiskIfPersistent(vbid, 1);

    // We don't include prepares in the numItems stat (should not exist in here)
    auto vb = engine->getVBucket(vbid);
    EXPECT_EQ(0, vb->getNumItems());

    // Our topology gives replica name as "replica" an our producer/stream has
    // name "test_producer". Simulate a seqno ack by calling the vBucket level
    // function.
    stream->seqnoAck(producer->getConsumerName(), 1);

    // Disconnect the ActiveStream. Should remove the queued seqno ack
    stream->setDead(END_STREAM_DISCONNECTED);

    stream = std::make_shared<MockActiveStream>(engine.get(),
                                                producer,
                                                0 /*flags*/,
                                                0 /*opaque*/,
                                                *vb,
                                                0 /*st_seqno*/,
                                                ~0 /*en_seqno*/,
                                                0x0 /*vb_uuid*/,
                                                0 /*snap_start_seqno*/,
                                                ~0 /*snap_end_seqno*/);
    producer->createCheckpointProcessorTask();
    producer->scheduleCheckpointProcessorTask();
    stream->setActive();

    // Process items to ensure that lastSentSeqno is GE the seqno that we will
    // ack
    stream->transitionStateToBackfilling();
    ASSERT_TRUE(stream->isBackfilling());

    auto& bfm = producer->getBFM();
    bfm.backfill();
    bfm.backfill();
    EXPECT_EQ(2, stream->public_readyQSize());
    stream->consumeBackfillItems(2);

    // Should not throw a monotonic exception as the ack should have been
    // removed by setDead.
    stream->seqnoAck(producer->getConsumerName(), 1);

    producer->cancelCheckpointCreatorTask();
}

void DurabilityActiveStreamTest::setUpSendSetInsteadOfCommitTest() {
    auto vb = engine->getVBucket(vbid);

    const auto key = makeStoredDocKey("key");
    const auto& value = "value";
    auto item = makePendingItem(
            key,
            value,
            cb::durability::Requirements(cb::durability::Level::Majority,
                                         1 /*timeout*/));
    VBQueueItemCtx ctx;
    ctx.durability =
            DurabilityItemCtx{item->getDurabilityReqs(), nullptr /*cookie*/};

    // Seqno 1 - First prepare (the consumer streams this)
    EXPECT_EQ(MutationStatus::WasClean, public_processSet(*vb, *item, ctx));
    flushVBucketToDiskIfPersistent(vbid, 1);

    // Seqno 2 - Followed by a commit (the consumer does not get this)
    vb->commit(key, vb->getHighSeqno(), {}, vb->lockCollections(key));
    flushVBucketToDiskIfPersistent(vbid, 1);

    auto mutationResult =
            persistent() ? MutationStatus::WasClean : MutationStatus::WasDirty;
    // Seqno 3 - A prepare that is deduped
    EXPECT_EQ(mutationResult, public_processSet(*vb, *item, ctx));
    flushVBucketToDiskIfPersistent(vbid, 1);

    // Seqno 4 - A commit that the consumer would receive when reconnecting with
    // seqno 1
    vb->commit(key, vb->getHighSeqno(), {}, vb->lockCollections(key));
    flushVBucketToDiskIfPersistent(vbid, 1);

    // Seqno 5 - A prepare to dedupe the prepare at seqno 3.
    EXPECT_EQ(mutationResult, public_processSet(*vb, *item, ctx));
    flushVBucketToDiskIfPersistent(vbid, 1);

    EXPECT_EQ(2, vb->ht.getNumItems());

    // Drop the stream cursor so that we can drop the closed checkpoints
    EXPECT_TRUE(stream->handleSlowStream());
    auto& mockCkptMgr =
            *(static_cast<MockCheckpointManager*>(vb->checkpointManager.get()));
    auto expectedCursors = persistent() ? 1 : 0;
    ASSERT_EQ(expectedCursors, mockCkptMgr.getNumOfCursors());

    // Need to close the previously existing checkpoints so that we can backfill
    // from disk
    bool newCkpt = false;
    auto size =
            mockCkptMgr.removeClosedUnrefCheckpoints(*vb, newCkpt, 5 /*limit*/);
    ASSERT_FALSE(newCkpt);
    ASSERT_EQ(4, size);
}

TEST_P(DurabilityActiveStreamTest, SendSetInsteadOfCommitForReconnectWindow) {
    setUpSendSetInsteadOfCommitTest();

    auto vb = engine->getVBucket(vbid);
    const auto key = makeStoredDocKey("key");

    // Disconnect and resume from our prepare
    stream = std::make_shared<MockActiveStream>(engine.get(),
                                                producer,
                                                0 /*flags*/,
                                                0 /*opaque*/,
                                                *vb,
                                                1 /*st_seqno*/,
                                                ~0 /*en_seqno*/,
                                                0x0 /*vb_uuid*/,
                                                1 /*snap_start_seqno*/,
                                                ~1 /*snap_end_seqno*/);

    stream->transitionStateToBackfilling();
    ASSERT_TRUE(stream->isBackfilling());
    auto& bfm = producer->getBFM();
    bfm.backfill();
    // First backfill only sends the SnapshotMarker so repeat
    bfm.backfill();

    // Stream::readyQ must contain SnapshotMarker
    ASSERT_EQ(3, stream->public_readyQSize());
    auto resp = stream->public_popFromReadyQ();
    ASSERT_TRUE(resp);
    EXPECT_EQ(DcpResponse::Event::SnapshotMarker, resp->getEvent());

    // Followed by a mutation instead of a commit
    resp = stream->public_popFromReadyQ();
    ASSERT_TRUE(resp);
    ASSERT_EQ(DcpResponse::Event::Mutation, resp->getEvent());
    const auto& set = static_cast<MutationResponse&>(*resp);
    EXPECT_EQ(key, set.getItem()->getKey());
    EXPECT_EQ(4, set.getItem()->getBySeqno());

    // Followed by a prepare
    resp = stream->public_popFromReadyQ();
    ASSERT_TRUE(resp);
    ASSERT_EQ(DcpResponse::Event::Prepare, resp->getEvent());
    const auto& prepare = static_cast<MutationResponse&>(*resp);
    EXPECT_EQ(key, prepare.getItem()->getKey());
    EXPECT_TRUE(prepare.getItem()->isPending());
    EXPECT_EQ(5, prepare.getItem()->getBySeqno());
}

TEST_P(DurabilityActiveStreamTest, SendSetInsteadOfCommitForNewVB) {
    setUpSendSetInsteadOfCommitTest();

    auto vb = engine->getVBucket(vbid);
    const auto key = makeStoredDocKey("key");

    // Disconnect and resume from our prepare
    stream = std::make_shared<MockActiveStream>(engine.get(),
                                                producer,
                                                0 /*flags*/,
                                                0 /*opaque*/,
                                                *vb,
                                                0 /*st_seqno*/,
                                                ~0 /*en_seqno*/,
                                                0x0 /*vb_uuid*/,
                                                0 /*snap_start_seqno*/,
                                                ~0 /*snap_end_seqno*/);

    stream->transitionStateToBackfilling();
    ASSERT_TRUE(stream->isBackfilling());
    auto& bfm = producer->getBFM();
    bfm.backfill();
    // First backfill only sends the SnapshotMarker so repeat
    bfm.backfill();

    // Stream::readyQ must contain SnapshotMarker
    ASSERT_EQ(3, stream->public_readyQSize());
    auto resp = stream->public_popFromReadyQ();
    ASSERT_TRUE(resp);
    EXPECT_EQ(DcpResponse::Event::SnapshotMarker, resp->getEvent());

    // Followed by a mutation instead of a commit
    resp = stream->public_popFromReadyQ();
    ASSERT_TRUE(resp);
    ASSERT_EQ(DcpResponse::Event::Mutation, resp->getEvent());
    const auto& set = static_cast<MutationResponse&>(*resp);
    EXPECT_EQ(key, set.getItem()->getKey());
    EXPECT_EQ(4, set.getItem()->getBySeqno());

    // Followed by a prepare
    resp = stream->public_popFromReadyQ();
    ASSERT_TRUE(resp);
    ASSERT_EQ(DcpResponse::Event::Prepare, resp->getEvent());
    const auto& prepare = static_cast<MutationResponse&>(*resp);
    EXPECT_EQ(key, prepare.getItem()->getKey());
    EXPECT_TRUE(prepare.getItem()->isPending());
    EXPECT_EQ(5, prepare.getItem()->getBySeqno());
}

TEST_P(DurabilityActiveStreamTest, SendCommitForResumeIfPrepareReceived) {
    setUpSendSetInsteadOfCommitTest();

    auto vb = engine->getVBucket(vbid);
    const auto key = makeStoredDocKey("key");

    // Disconnect and resume from our prepare. We resume from prepare 3 in this
    // case so that the producers data will just be [4: Commit, 5:Prepare].
    stream = std::make_shared<MockActiveStream>(engine.get(),
                                                producer,
                                                0 /*flags*/,
                                                0 /*opaque*/,
                                                *vb,
                                                3 /*st_seqno*/,
                                                ~0 /*en_seqno*/,
                                                0x0 /*vb_uuid*/,
                                                3 /*snap_start_seqno*/,
                                                ~3 /*snap_end_seqno*/);

    stream->transitionStateToBackfilling();
    ASSERT_TRUE(stream->isBackfilling());
    auto& bfm = producer->getBFM();
    bfm.backfill();
    // First backfill only sends the SnapshotMarker so repeat
    bfm.backfill();

    // Stream::readyQ must contain SnapshotMarker
    ASSERT_EQ(3, stream->public_readyQSize());
    auto resp = stream->public_popFromReadyQ();
    ASSERT_TRUE(resp);
    EXPECT_EQ(DcpResponse::Event::SnapshotMarker, resp->getEvent());

    // Followed by a commit because the producer knows we are not missing a
    // prepare.
    resp = stream->public_popFromReadyQ();
    ASSERT_TRUE(resp);
    ASSERT_EQ(DcpResponse::Event::Commit, resp->getEvent());
    const auto& commit = static_cast<CommitSyncWrite&>(*resp);
    EXPECT_EQ(key, commit.getKey());
    EXPECT_EQ(4, *commit.getBySeqno());
    EXPECT_EQ(3, commit.getPreparedSeqno());

    // Followed by a prepare
    resp = stream->public_popFromReadyQ();
    ASSERT_TRUE(resp);
    ASSERT_EQ(DcpResponse::Event::Prepare, resp->getEvent());
    const auto& prepare = static_cast<MutationResponse&>(*resp);
    EXPECT_EQ(key, prepare.getItem()->getKey());
    EXPECT_TRUE(prepare.getItem()->isPending());
    EXPECT_EQ(5, prepare.getItem()->getBySeqno());
}

/**
 * This test checks that we can deal with a seqno ack from a replica going
 * "backwards" when it shuts down and warms back up. This can happen when a
 * replica acks a Majority level prepare that has not yet been persisted before
 * it shuts down. When it warms up, it will ack the persisted HPS.
 */
TEST_P(DurabilityActiveStreamTest,
       ActiveDealsWithNonMonotonicSeqnoAckOnReconnect) {
    auto vb = engine->getVBucket(vbid);
    const auto key = makeStoredDocKey("key");
    const std::string value = "value";
    auto item = makePendingItem(
            key,
            value,
            cb::durability::Requirements(cb::durability::Level::Majority,
                                         1 /*timeout*/));
    VBQueueItemCtx ctx;
    ctx.durability =
            DurabilityItemCtx{item->getDurabilityReqs(), nullptr /*cookie*/};
    {
        auto cHandle = vb->lockCollections(item->getKey());
        EXPECT_EQ(ENGINE_EWOULDBLOCK,
                  vb->set(*item, cookie, *engine, {}, cHandle));
    }
    vb->notifyActiveDMOfLocalSyncWrite();

    auto items = stream->getOutstandingItems(*vb);
    stream->public_processItems(items);
    stream->consumeBackfillItems(1);
    stream->public_nextQueuedItem();

    EXPECT_EQ(ENGINE_SUCCESS, stream->seqnoAck(replica, 1 /*prepareSeqno*/));
    EXPECT_EQ(1, vb->getHighPreparedSeqno());
    EXPECT_EQ(1, vb->getHighCompletedSeqno());

    {
        auto cHandle = vb->lockCollections(item->getKey());
        EXPECT_EQ(ENGINE_EWOULDBLOCK,
                  vb->set(*item, cookie, *engine, {}, cHandle));
    }
    vb->notifyActiveDMOfLocalSyncWrite();

    items = stream->getOutstandingItems(*vb);
    stream->public_processItems(items);
    stream->consumeBackfillItems(3);
    stream->public_nextQueuedItem();

    EXPECT_EQ(ENGINE_SUCCESS, stream->seqnoAck(replica, 3 /*prepareSeqno*/));
    EXPECT_EQ(3, vb->getHighPreparedSeqno());
    EXPECT_EQ(3, vb->getHighCompletedSeqno());

    stream = std::make_shared<MockActiveStream>(engine.get(),
                                                producer,
                                                0 /*flags*/,
                                                0 /*opaque*/,
                                                *vb,
                                                0 /*st_seqno*/,
                                                ~0 /*en_seqno*/,
                                                0x0 /*vb_uuid*/,
                                                0 /*snap_start_seqno*/,
                                                ~0 /*snap_end_seqno*/);
    producer->createCheckpointProcessorTask();
    producer->scheduleCheckpointProcessorTask();
    stream->setActive();

    // Process items to ensure that lastSentSeqno is GE the seqno that we will
    // ack
    flushVBucketToDiskIfPersistent(vbid, 2);
    stream->transitionStateToBackfilling();
    ASSERT_TRUE(stream->isBackfilling());

    auto& bfm = producer->getBFM();
    bfm.backfill();
    bfm.backfill();
    EXPECT_EQ(3, stream->public_readyQSize());
    stream->consumeBackfillItems(3);

    EXPECT_EQ(ENGINE_SUCCESS, stream->seqnoAck(replica, 1 /*prepareSeqno*/));

    producer->cancelCheckpointCreatorTask();
}

void DurabilityPassiveStreamTest::SetUp() {
    SingleThreadedPassiveStreamTest::SetUp();
    consumer->enableSyncReplication();
    ASSERT_TRUE(consumer->isSyncReplicationEnabled());
}

void DurabilityPassiveStreamTest::TearDown() {
    SingleThreadedPassiveStreamTest::TearDown();
}

TEST_P(DurabilityPassiveStreamTest, SendSeqnoAckOnStreamAcceptance) {
    // 1) Put something in the vBucket as we won't send a seqno ack if there are
    // no items
    testReceiveDcpPrepare();

    consumer->closeAllStreams();
    uint32_t opaque = 0;
    consumer->addStream(opaque, vbid, 0 /*flags*/);
    stream = static_cast<MockPassiveStream*>(
            (consumer->getVbucketStream(vbid)).get());
    stream->acceptStream(cb::mcbp::Status::Success, opaque);

    EXPECT_EQ(3, stream->public_readyQ().size());
    auto resp = stream->public_popFromReadyQ();
    EXPECT_EQ(DcpResponse::Event::StreamReq, resp->getEvent());
    resp = stream->public_popFromReadyQ();
    EXPECT_EQ(DcpResponse::Event::AddStream, resp->getEvent());
    resp = stream->public_popFromReadyQ();
    EXPECT_EQ(DcpResponse::Event::SeqnoAcknowledgement, resp->getEvent());
    const auto& ack = static_cast<SeqnoAcknowledgement&>(*resp);
    EXPECT_EQ(1, ack.getPreparedSeqno());
}

/**
 * This test demonstrates what happens to the acked seqno on the replica when
 * we shutdown and restart having previously acked a seqno that is not flushed.
 * This can cause the acked seqno to go "backwards".
 */
TEST_P(DurabilityPassiveStreamPersistentTest,
       ReplicaSeqnoAckNonMonotonicIfBounced) {
    // 1) Receive 2 majority prepares but only flush 1
    auto key = makeStoredDocKey("key");
    makeAndReceiveDcpPrepare(key, 0 /*cas*/, 1 /*seqno*/);

    // Flush only the first prepare so when we warmup later we won't have the
    // second
    flushVBucketToDiskIfPersistent(vbid, 1);

    key = makeStoredDocKey("key2");
    makeAndReceiveDcpPrepare(key, 0 /*cas*/, 2 /*seqno*/);

    // 2) Check that we have acked twice, once for each prepare, as each is in
    // it's own snapshot
    ASSERT_EQ(2, stream->public_readyQ().size());
    auto resp = stream->public_popFromReadyQ();
    EXPECT_EQ(DcpResponse::Event::SeqnoAcknowledgement, resp->getEvent());
    auto ack = static_cast<SeqnoAcknowledgement&>(*resp);
    EXPECT_EQ(1, ack.getPreparedSeqno());
    resp = stream->public_popFromReadyQ();
    EXPECT_EQ(DcpResponse::Event::SeqnoAcknowledgement, resp->getEvent());
    ack = static_cast<SeqnoAcknowledgement&>(*resp);
    EXPECT_EQ(2, ack.getPreparedSeqno());

    // 3) Shutdown and warmup again
    consumer->closeAllStreams();
    consumer.reset();
    resetEngineAndWarmup();

    // 4) Test that the stream now sends a seqno ack of 1 when we reconnect
    uint32_t opaque = 0;
    // Recreate the consumer
    consumer =
            std::make_shared<MockDcpConsumer>(*engine, cookie, "test_consumer");
    consumer->enableSyncReplication();
    consumer->addStream(opaque, vbid, 0 /*flags*/);
    stream = static_cast<MockPassiveStream*>(
            (consumer->getVbucketStream(vbid)).get());
    stream->acceptStream(cb::mcbp::Status::Success, opaque);

    ASSERT_EQ(3, stream->public_readyQ().size());
    resp = stream->public_popFromReadyQ();
    EXPECT_EQ(DcpResponse::Event::StreamReq, resp->getEvent());
    resp = stream->public_popFromReadyQ();
    EXPECT_EQ(DcpResponse::Event::AddStream, resp->getEvent());
    resp = stream->public_popFromReadyQ();
    EXPECT_EQ(DcpResponse::Event::SeqnoAcknowledgement, resp->getEvent());
    ack = static_cast<SeqnoAcknowledgement&>(*resp);
    EXPECT_EQ(1, ack.getPreparedSeqno());
}

TEST_P(DurabilityPassiveStreamTest,
       NoSeqnoAckOnStreamAcceptanceIfNotSupported) {
    consumer->disableSyncReplication();

    // 1) Put something in the vBucket as we won't send a seqno ack if there are
    // no items
    testReceiveDcpPrepare();

    consumer->closeAllStreams();
    uint32_t opaque = 0;
    consumer->addStream(opaque, vbid, 0 /*flags*/);
    stream = static_cast<MockPassiveStream*>(
            (consumer->getVbucketStream(vbid)).get());
    stream->acceptStream(cb::mcbp::Status::Success, opaque);

    ASSERT_EQ(2, stream->public_readyQ().size());
    auto resp = stream->public_popFromReadyQ();
    EXPECT_EQ(DcpResponse::Event::StreamReq, resp->getEvent());
    resp = stream->public_popFromReadyQ();
    EXPECT_EQ(DcpResponse::Event::AddStream, resp->getEvent());
    resp = stream->public_popFromReadyQ();
    EXPECT_FALSE(resp);
}

void DurabilityPassiveStreamTest::
        testReceiveMutationOrDeletionInsteadOfCommitWhenStreamingFromDisk(
                DocumentState docState) {
    auto vb = store->getVBucket(vbid);
    ASSERT_TRUE(vb);
    auto& ckptMgr = *vb->checkpointManager;
    // Get rid of set_vb_state and any other queue_op we are not interested in
    ckptMgr.clear(*vb, 0 /*seqno*/);
    uint32_t opaque = 1;

    SnapshotMarker marker(opaque,
                          vbid,
                          2 /*snapStart*/,
                          4 /*snapEnd*/,
                          dcp_marker_flag_t::MARKER_FLAG_DISK | MARKER_FLAG_CHK,
                          {} /*streamId*/);
    stream->processMarker(&marker);

    auto key = makeStoredDocKey("key");
    using namespace cb::durability;
    auto item = makePendingItem(
            key, "value", Requirements(Level::Majority, Timeout::Infinity()));
    item->setBySeqno(2);
    item->setCas(999);

    // Send the prepare
    EXPECT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<MutationConsumerMessage>(
                      item,
                      opaque,
                      IncludeValue::Yes,
                      IncludeXattrs::Yes,
                      IncludeDeleteTime::No,
                      DocKeyEncodesCollectionId::No,
                      nullptr,
                      cb::mcbp::DcpStreamId{})));

    item = makeCommittedItem(key, "committed");
    item->setBySeqno(4);

    if (docState == DocumentState::Deleted) {
        item->setDeleted(DeleteSource::Explicit);
        item->replaceValue(nullptr);
    }

    // Send the logical commit
    EXPECT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<MutationConsumerMessage>(
                      std::move(item),
                      opaque,
                      IncludeValue::Yes,
                      IncludeXattrs::Yes,
                      IncludeDeleteTime::No,
                      DocKeyEncodesCollectionId::No,
                      nullptr,
                      cb::mcbp::DcpStreamId{})));

    // Test the HashTable state
    {
        // findForCommit will return both pending and committed perspectives
        auto res = vb->ht.findForCommit(key);
        ASSERT_TRUE(res.committed);
        EXPECT_EQ(4, res.committed->getBySeqno());
        if (docState == DocumentState::Alive) {
            EXPECT_TRUE(res.committed->getValue());
        }
        if (persistent()) {
            EXPECT_FALSE(res.pending);
        } else {
            ASSERT_TRUE(res.pending);
            EXPECT_EQ(2, res.pending->getBySeqno());
            EXPECT_EQ(CommittedState::PrepareCommitted,
                      res.pending->getCommitted());
        }
    }

    // Test the checkpoint manager state
    const auto& ckptList =
            CheckpointManagerTestIntrospector::public_getCheckpointList(
                    ckptMgr);

    const auto* ckpt = ckptList.back().get();
    EXPECT_EQ(checkpoint_state::CHECKPOINT_OPEN, ckpt->getState());
    // empty-item
    auto it = ckpt->begin();
    EXPECT_EQ(queue_op::empty, (*it)->getOperation());
    // 1 metaitem (checkpoint-start)
    it++;
    ASSERT_EQ(1, ckpt->getNumMetaItems());
    EXPECT_EQ(queue_op::checkpoint_start, (*it)->getOperation());
    it++;

    ASSERT_EQ(2, ckpt->getNumItems());
    EXPECT_EQ(queue_op::pending_sync_write, (*it)->getOperation());
    it++;

    // The logical commit is a mutation in the checkpoint manager, not a commit.
    EXPECT_EQ(queue_op::mutation, (*it)->getOperation());
}

TEST_P(DurabilityPassiveStreamTest,
       ReceiveMutationInsteadOfCommitWhenStreamingFromDisk) {
    testReceiveMutationOrDeletionInsteadOfCommitWhenStreamingFromDisk(
            DocumentState::Alive);
}

void DurabilityPassiveStreamTest::
        receiveMutationOrDeletionInsteadOfCommitWhenStreamingFromDiskMutationFirst(
                DocumentState docState) {
    uint32_t opaque = 1;
    SnapshotMarker marker(opaque,
                          vbid,
                          1 /*snapStart*/,
                          3 /*snapEnd*/,
                          dcp_marker_flag_t::MARKER_FLAG_DISK,
                          {} /*streamId*/);
    stream->processMarker(&marker);

    auto key = makeStoredDocKey("key");
    auto item = makeCommittedItem(key, "mutation");
    item->setBySeqno(1);

    EXPECT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<MutationConsumerMessage>(
                      std::move(item),
                      opaque,
                      IncludeValue::Yes,
                      IncludeXattrs::Yes,
                      IncludeDeleteTime::No,
                      DocKeyEncodesCollectionId::No,
                      nullptr,
                      cb::mcbp::DcpStreamId{})));
    testReceiveMutationOrDeletionInsteadOfCommitWhenStreamingFromDisk(docState);
}

TEST_P(DurabilityPassiveStreamTest,
       ReceiveMutationInsteadOfCommitOnTopOfMutation) {
    receiveMutationOrDeletionInsteadOfCommitWhenStreamingFromDiskMutationFirst(
            DocumentState::Alive);
}

TEST_P(DurabilityPassiveStreamTest,
       ReceiveDeletionInsteadOfCommitOnTopOfMutation) {
    receiveMutationOrDeletionInsteadOfCommitWhenStreamingFromDiskMutationFirst(
            DocumentState::Deleted);
}

void DurabilityPassiveStreamTest::
        testReceiveMutationOrDeletionInsteadOfCommitForReconnectWindowWithPrepareLast(
                DocumentState docState) {
    // 1) Receive DCP Prepare
    auto key = makeStoredDocKey("key");
    uint64_t prepareSeqno = 1;
    uint64_t cas = 0;
    makeAndReceiveDcpPrepare(key, cas, prepareSeqno);

    // 2) Fake disconnect and reconnect, importantly, this sets up the valid
    // window for ignoring DCPAborts.
    consumer->closeAllStreams();
    uint32_t opaque = 0;
    consumer->addStream(opaque, vbid, 0 /*flags*/);
    stream = static_cast<MockPassiveStream*>(
            (consumer->getVbucketStream(vbid)).get());
    stream->acceptStream(cb::mcbp::Status::Success, opaque);

    // 3) Receive overwriting set instead of commit
    uint64_t streamStartSeqno = 4;
    SnapshotMarker marker(opaque,
                          vbid,
                          streamStartSeqno /*snapStart*/,
                          streamStartSeqno /*snapEnd*/,
                          dcp_marker_flag_t::MARKER_FLAG_DISK,
                          {} /*streamId*/);
    stream->processMarker(&marker);

    const std::string value = "overwritingValue";
    auto item = makeCommittedItem(key, value);
    item->setBySeqno(streamStartSeqno);

    if (docState == DocumentState::Deleted) {
        item->setDeleted(DeleteSource::Explicit);
        item->replaceValue(nullptr);
    }

    EXPECT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<MutationConsumerMessage>(
                      std::move(item),
                      opaque,
                      IncludeValue::Yes,
                      IncludeXattrs::Yes,
                      IncludeDeleteTime::No,
                      DocKeyEncodesCollectionId::No,
                      nullptr,
                      cb::mcbp::DcpStreamId{})));

    // 4) Verify doc state
    auto vb = store->getVBucket(vbid);
    ASSERT_TRUE(vb);

    // tell the DM that this snapshot was persisted
    vb->setPersistenceSeqno(streamStartSeqno);
    vb->notifyPersistenceToDurabilityMonitor();

    {
        // findForCommit will return both pending and committed perspectives
        auto res = vb->ht.findForCommit(key);
        if (persistent()) {
            EXPECT_FALSE(res.pending);
        } else {
            ASSERT_TRUE(res.pending);
            EXPECT_EQ(1, res.pending->getBySeqno());
            EXPECT_EQ(CommittedState::PrepareCommitted,
                      res.pending->getCommitted());
        }
        ASSERT_TRUE(res.committed);
        EXPECT_EQ(4, res.committed->getBySeqno());
        EXPECT_EQ(CommittedState::CommittedViaMutation,
                  res.committed->getCommitted());
        if (docState == DocumentState::Alive) {
            ASSERT_TRUE(res.committed->getValue());
            EXPECT_EQ(value, res.committed->getValue()->to_s());
        }
    }

    // Should have removed all sync writes
    EXPECT_EQ(0, vb->getDurabilityMonitor().getNumTracked());

    // We should now be able to do a sync write to a different key
    key = makeStoredDocKey("newkey");
    makeAndReceiveDcpPrepare(key, cas, 10);
    prepareSeqno = vb->getHighSeqno();
    marker = SnapshotMarker(
            opaque,
            vbid,
            streamStartSeqno + 2 /*snapStart*/,
            streamStartSeqno + 2 /*snapEnd*/,
            dcp_marker_flag_t::MARKER_FLAG_MEMORY | MARKER_FLAG_CHK,
            {} /*streamId*/);
    stream->processMarker(&marker);
    EXPECT_EQ(ENGINE_SUCCESS,
              vb->commit(key, prepareSeqno, {}, vb->lockCollections(key)));
    EXPECT_EQ(0, vb->getDurabilityMonitor().getNumTracked());
}

TEST_P(DurabilityPassiveStreamTest,
       ReceiveMutationInsteadOfCommitForReconnectWindowWithPrepareLast) {
    testReceiveMutationOrDeletionInsteadOfCommitForReconnectWindowWithPrepareLast(
            DocumentState::Alive);
}

TEST_P(DurabilityPassiveStreamTest,
       ReceiveAbortOnTopOfCommittedDueToDedupedPrepare) {
    uint32_t opaque = 0;
    SnapshotMarker marker(
            opaque,
            vbid,
            1 /*snapStart*/,
            1 /*snapEnd*/,
            dcp_marker_flag_t::MARKER_FLAG_MEMORY | MARKER_FLAG_CHK,
            {} /*streamId*/);
    stream->processMarker(&marker);

    auto key = makeStoredDocKey("key");
    const std::string value = "overwritingValue";
    auto item = makeCommittedItem(key, value);
    item->setBySeqno(1);
    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<MutationConsumerMessage>(
                      std::move(item),
                      opaque,
                      IncludeValue::Yes,
                      IncludeXattrs::Yes,
                      IncludeDeleteTime::No,
                      DocKeyEncodesCollectionId::No,
                      nullptr,
                      cb::mcbp::DcpStreamId{})));

    marker = SnapshotMarker(opaque,
                            vbid,
                            1 /*snapStart*/,
                            ~1 /*snapEnd*/,
                            dcp_marker_flag_t::MARKER_FLAG_DISK,
                            {} /*streamId*/);
    stream->processMarker(&marker);

    EXPECT_EQ(
            ENGINE_SUCCESS,
            stream->messageReceived(std::make_unique<AbortSyncWrite>(
                    opaque, vbid, key, 3 /*prepareSeqno*/, 4 /*abortSeqno*/)));
}

TEST_P(DurabilityPassiveStreamTest, SeqnoAckAtSnapshotEndReceived) {
    // The consumer receives mutations {s:1, s:2, s:3}, with only s:2 durable
    // with Level:Majority. We have to check that we do send a SeqnoAck, but
    // only when the Replica receives the snapshot-end mutation.

    // The consumer receives the snapshot-marker
    uint32_t opaque = 0;
    const uint64_t snapEnd = 3;
    SnapshotMarker snapshotMarker(opaque,
                                  vbid,
                                  1 /*snapStart*/,
                                  snapEnd,
                                  dcp_marker_flag_t::MARKER_FLAG_MEMORY,
                                  {});
    stream->processMarker(&snapshotMarker);
    const auto& readyQ = stream->public_readyQ();
    EXPECT_EQ(0, readyQ.size());

    const std::string value("value");

    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(makeMutationConsumerMessage(
                      1 /*seqno*/, vbid, value, opaque)));
    EXPECT_EQ(0, readyQ.size());

    using namespace cb::durability;
    const uint64_t swSeqno = 2;
    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(makeMutationConsumerMessage(
                      swSeqno,
                      vbid,
                      value,
                      opaque,
                      Requirements(Level::Majority, Timeout::Infinity()))));
    // readyQ still empty, we have not received the snap-end mutation yet
    EXPECT_EQ(0, readyQ.size());

    // snapshot-end
    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(makeMutationConsumerMessage(
                      snapEnd, vbid, value, opaque)));
    // Verify that we have the expected SeqnoAck in readyQ now
    ASSERT_EQ(1, readyQ.size());
    ASSERT_EQ(DcpResponse::Event::SeqnoAcknowledgement,
              readyQ.front()->getEvent());
    const auto* seqnoAck =
            static_cast<const SeqnoAcknowledgement*>(readyQ.front().get());
    EXPECT_EQ(swSeqno, seqnoAck->getPreparedSeqno());
}

TEST_P(DurabilityPassiveStreamPersistentTest, SeqnoAckAtPersistedSeqno) {
    // The consumer receives mutations {s:1, s:2, s:3} in the snapshot:[1, 4],
    // with only s:2 durable with Level:PersistToMajority.
    // We have to check that we do send a SeqnoAck for s:2, but only after:
    // (1) the snapshot-end mutation (s:4) is received
    // (2) the complete snapshot is persisted

    // The consumer receives the snapshot-marker [1, 4]
    uint32_t opaque = 0;
    SnapshotMarker snapshotMarker(opaque,
                                  vbid,
                                  1 /*snapStart*/,
                                  4 /*snapEnd*/,
                                  dcp_marker_flag_t::MARKER_FLAG_MEMORY,
                                  {});
    stream->processMarker(&snapshotMarker);
    const auto& readyQ = stream->public_readyQ();
    EXPECT_EQ(0, readyQ.size());

    const std::string value("value");

    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(makeMutationConsumerMessage(
                      1 /*seqno*/, vbid, value, opaque)));
    EXPECT_EQ(0, readyQ.size());

    const int64_t swSeqno = 2;
    using namespace cb::durability;
    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(makeMutationConsumerMessage(
                      swSeqno,
                      vbid,
                      value,
                      opaque,
                      Requirements(Level::PersistToMajority,
                                   Timeout::Infinity()))));
    // No SeqnoAck, HPS has not moved as Level:PersistToMajority requires to be
    // persisted for being locally-satisfied
    EXPECT_EQ(0, readyQ.size());

    // Flush (in the middle of the snapshot, which can happen at replica)
    flushVBucketToDiskIfPersistent(vbid, 2 /*expectedNumFlushed*/);
    // No SeqnoAck, HPS has not moved as Level:PersistToMajority Prepare has
    // been persisted but at any Level we require that the complete snapshot is
    // received before moving the HPS into the snapshot.
    EXPECT_EQ(0, readyQ.size());

    // Non-durable s:3 received
    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(makeMutationConsumerMessage(
                      3 /*seqno*/, vbid, value, opaque)));
    // No ack yet, we have not yet received the complete snapshot
    EXPECT_EQ(0, readyQ.size());

    // Non-durable s:4 (snapshot-end) received
    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(makeMutationConsumerMessage(
                      4 /*seqno*/, vbid, value, opaque)));
    // No ack yet, we have received the snap-end mutation but we have not yet
    // persisted the complete snapshot
    EXPECT_EQ(0, readyQ.size());

    // Flush, complete snapshot persisted after this call
    flushVBucketToDiskIfPersistent(vbid, 2 /*expectedNumFlushed*/);

    // HPS must have moved to the (already) persisted s:2 and we must have a
    // SeqnoAck with payload HPS in readyQ.
    // Note that s:3 and s:4 (which is a non-sync write) don't affect HPS, which
    // is set to the last locally-satisfied Prepare.
    ASSERT_EQ(1, readyQ.size());
    ASSERT_EQ(DcpResponse::Event::SeqnoAcknowledgement,
              readyQ.front()->getEvent());
    const auto* seqnoAck =
            static_cast<const SeqnoAcknowledgement*>(readyQ.front().get());
    EXPECT_EQ(swSeqno, seqnoAck->getPreparedSeqno());
}

/**
 * The test simulates a Replica receiving:
 *
 * snapshot-marker [1, 10] -> no-ack
 * s:1 non-durable -> no ack
 * s:2 Level:Majority -> no ack
 * s:3 non-durable -> no ack
 * s:4 Level:MajorityAndPersistOnMaster -> no ack
 * s:5 non-durable -> no ack
 * s:6 Level:PersistToMajority (durability-fence) -> no ack
 * s:7 Level-Majority -> no ack
 * s:8 Level:MajorityAndPersistOnMaster -> no ack
 * s:9 non-durable -> no ack
 * s:9 non-durable -> no ack
 * s:10 non-durable (snapshot-end) -> ack (HPS=4)
 *
 * Last step: flusher persists all -> ack (HPS=8)
 */
TEST_P(DurabilityPassiveStreamPersistentTest, DurabilityFence) {
    const auto& readyQ = stream->public_readyQ();
    auto checkSeqnoAckInReadyQ = [this, &readyQ](int64_t seqno) -> void {
        ASSERT_EQ(1, readyQ.size());
        ASSERT_EQ(DcpResponse::Event::SeqnoAcknowledgement,
                  readyQ.front()->getEvent());
        const auto& seqnoAck =
                static_cast<const SeqnoAcknowledgement&>(*readyQ.front());
        EXPECT_EQ(seqno, seqnoAck.getPreparedSeqno());
        // Clear readyQ
        ASSERT_TRUE(stream->public_popFromReadyQ());
        ASSERT_FALSE(readyQ.size());
    };

    // snapshot-marker [1, 10] -> no-ack
    uint32_t opaque = 0;
    SnapshotMarker snapshotMarker(opaque,
                                  vbid,
                                  1 /*snapStart*/,
                                  10 /*snapEnd*/,
                                  dcp_marker_flag_t::MARKER_FLAG_MEMORY,
                                  {});
    stream->processMarker(&snapshotMarker);
    EXPECT_EQ(0, readyQ.size());

    // s:1 non-durable -> no ack
    const std::string value("value");
    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(makeMutationConsumerMessage(
                      1 /*seqno*/, vbid, value, opaque)));
    EXPECT_EQ(0, readyQ.size());

    // s:2 Level:Majority -> no ack
    using namespace cb::durability;
    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(makeMutationConsumerMessage(
                      2 /*seqno*/,
                      vbid,
                      value,
                      opaque,
                      Requirements(Level::Majority, Timeout::Infinity()))));
    EXPECT_EQ(0, readyQ.size());

    // s:3 non-durable -> no ack
    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(makeMutationConsumerMessage(
                      3 /*seqno*/, vbid, value, opaque)));
    EXPECT_EQ(0, readyQ.size());

    // s:4 Level:MajorityAndPersistOnMaster -> no ack
    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(makeMutationConsumerMessage(
                      4 /*seqno*/,
                      vbid,
                      value,
                      opaque,
                      Requirements(Level::MajorityAndPersistOnMaster,
                                   Timeout::Infinity()))));
    EXPECT_EQ(0, readyQ.size());

    // s:5 non-durable -> no ack
    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(makeMutationConsumerMessage(
                      5 /*seqno*/, vbid, value, opaque)));
    EXPECT_EQ(0, readyQ.size());

    // s:6 Level:PersistToMajority -> no ack (durability-fence)
    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(makeMutationConsumerMessage(
                      6 /*seqno*/,
                      vbid,
                      value,
                      opaque,
                      Requirements(Level::PersistToMajority,
                                   Timeout::Infinity()))));
    EXPECT_EQ(0, readyQ.size());

    // s:7 Level-Majority -> no ack
    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(makeMutationConsumerMessage(
                      7 /*seqno*/,
                      vbid,
                      value,
                      opaque,
                      Requirements(Level::Majority, Timeout::Infinity()))));
    EXPECT_EQ(0, readyQ.size());

    // s:8 Level:MajorityAndPersistOnMaster -> no ack
    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(makeMutationConsumerMessage(
                      8 /*seqno*/,
                      vbid,
                      value,
                      opaque,
                      Requirements(Level::MajorityAndPersistOnMaster,
                                   Timeout::Infinity()))));
    EXPECT_EQ(0, readyQ.size());

    // s:9 non-durable -> no ack
    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(makeMutationConsumerMessage(
                      9 /*seqno*/, vbid, value, opaque)));
    EXPECT_EQ(0, readyQ.size());

    // s:10 non-durable (snapshot-end) -> ack (HPS=4, durability-fence at 6)
    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(
                      makeMutationConsumerMessage(10, vbid, value, opaque)));
    checkSeqnoAckInReadyQ(4 /*HPS*/);

    // Flusher persists all -> ack (HPS=8)
    flushVBucketToDiskIfPersistent(vbid, 10 /*expectedNumFlushed*/);
    checkSeqnoAckInReadyQ(8 /*HPS*/);
}

queued_item DurabilityPassiveStreamTest::makeAndReceiveDcpPrepare(
        const StoredDocKey& key,
        uint64_t cas,
        uint64_t seqno,
        cb::durability::Level level) {
    using namespace cb::durability;

    // The consumer receives snapshot-marker [seqno, seqno]
    uint32_t opaque = 0;
    SnapshotMarker marker(
            opaque,
            vbid,
            seqno,
            seqno,
            dcp_marker_flag_t::MARKER_FLAG_MEMORY | MARKER_FLAG_CHK,
            {} /*streamId*/);
    stream->processMarker(&marker);

    queued_item qi = makePendingItem(
            key, "value", Requirements(level, Timeout::Infinity()));
    qi->setBySeqno(seqno);
    qi->setCas(cas);

    EXPECT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<MutationConsumerMessage>(
                      qi,
                      opaque,
                      IncludeValue::Yes,
                      IncludeXattrs::Yes,
                      IncludeDeleteTime::No,
                      DocKeyEncodesCollectionId::No,
                      nullptr,
                      cb::mcbp::DcpStreamId{})));
    return qi;
}

void DurabilityPassiveStreamTest::testReceiveDcpPrepare() {
    auto vb = engine->getVBucket(vbid);
    auto& ckptMgr = *vb->checkpointManager;
    // Get rid of set_vb_state and any other queue_op we are not interested in
    ckptMgr.clear(*vb, 0 /*seqno*/);

    const auto key = makeStoredDocKey("key");
    const uint64_t cas = 999;
    const uint64_t prepareSeqno = 1;
    auto qi = makeAndReceiveDcpPrepare(key, cas, prepareSeqno);

    EXPECT_EQ(0, vb->getNumItems());
    EXPECT_EQ(1, vb->ht.getNumItems());
    {
        const auto sv = vb->ht.findForWrite(key);
        ASSERT_TRUE(sv.storedValue);
        EXPECT_EQ(CommittedState::Pending, sv.storedValue->getCommitted());
        EXPECT_EQ(prepareSeqno, sv.storedValue->getBySeqno());
        EXPECT_EQ(cas, sv.storedValue->getCas());
    }
    const auto& ckptList =
            CheckpointManagerTestIntrospector::public_getCheckpointList(
                    ckptMgr);
    // 1 checkpoint
    ASSERT_EQ(1, ckptList.size());
    const auto* ckpt = ckptList.front().get();
    ASSERT_EQ(checkpoint_state::CHECKPOINT_OPEN, ckpt->getState());
    // empty-item
    auto it = ckpt->begin();
    ASSERT_EQ(queue_op::empty, (*it)->getOperation());
    // 1 metaitem (checkpoint-start)
    it++;
    ASSERT_EQ(1, ckpt->getNumMetaItems());
    EXPECT_EQ(queue_op::checkpoint_start, (*it)->getOperation());
    // 1 non-metaitem is pending and contains the expected prepared item.
    it++;
    ASSERT_EQ(1, ckpt->getNumItems());
    EXPECT_EQ(*qi, **it) << "Item in Checkpoint doesn't match queued_item";

    EXPECT_EQ(1, vb->getDurabilityMonitor().getNumTracked());
    // Level:Majority + snap-end received -> HPS has moved
    EXPECT_EQ(1, vb->getDurabilityMonitor().getHighPreparedSeqno());
}

TEST_P(DurabilityPassiveStreamTest, ReceiveDcpPrepare) {
    testReceiveDcpPrepare();
}

void DurabilityPassiveStreamTest::testReceiveDuplicateDcpPrepare(
        uint64_t prepareSeqno) {
    // Consumer receives [1, 1] snapshot with just a prepare
    testReceiveDcpPrepare();

    // The consumer now "disconnects" then "re-connects" and misses a commit for
    // the given key at seqno 2. It instead receives the following snapshot
    // [3, 3] for the same key containing a prepare, followed by a second
    // snapshot [4, 4] with the corresponding commit.
    uint32_t opaque = 0;

    // Fake disconnect and reconnect, importantly, this sets up the valid window
    // for replacing the old prepare.
    consumer->closeAllStreams();
    consumer->addStream(opaque, vbid, 0 /*flags*/);
    stream = static_cast<MockPassiveStream*>(
            (consumer->getVbucketStream(vbid)).get());
    stream->acceptStream(cb::mcbp::Status::Success, opaque);

    ASSERT_TRUE(stream->isActive());
    // At Replica we don't expect multiple Durability items (for the same key)
    // within the same snapshot. That is because the Active prevents that for
    // avoiding de-duplication.
    // So, we need to simulate a Producer sending another SnapshotMarker with
    // the MARKER_FLAG_CHK set before the Consumer receives the Commit. That
    // will force the Consumer closing the open checkpoint (which Contains the
    // Prepare) and creating a new open one for queueing the Commit.
    SnapshotMarker marker(
            opaque,
            vbid,
            prepareSeqno /*snapStart*/,
            prepareSeqno /*snapEnd*/,
            dcp_marker_flag_t::MARKER_FLAG_MEMORY | MARKER_FLAG_CHK,
            {} /*streamId*/);
    stream->processMarker(&marker);

    const std::string value("value");
    auto key = makeStoredDocKey("key");
    using namespace cb::durability;

    const uint64_t cas = 999;
    queued_item qi(new Item(key,
                            0 /*flags*/,
                            0 /*expiry*/,
                            value.c_str(),
                            value.size(),
                            PROTOCOL_BINARY_RAW_BYTES,
                            cas /*cas*/,
                            prepareSeqno,
                            vbid));
    qi->setPendingSyncWrite(Requirements(Level::Majority, Timeout::Infinity()));

    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<MutationConsumerMessage>(
                      std::move(qi),
                      opaque,
                      IncludeValue::Yes,
                      IncludeXattrs::Yes,
                      IncludeDeleteTime::No,
                      DocKeyEncodesCollectionId::No,
                      nullptr,
                      cb::mcbp::DcpStreamId{})));

    auto commitSeqno = prepareSeqno + 1;
    marker = SnapshotMarker(
            opaque,
            vbid,
            commitSeqno /*snapStart*/,
            commitSeqno /*snapEnd*/,
            dcp_marker_flag_t::MARKER_FLAG_MEMORY | MARKER_FLAG_CHK,
            {} /*streamId*/);
    stream->processMarker(&marker);

    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<CommitSyncWrite>(
                      opaque, vbid, prepareSeqno, commitSeqno, key)));
}

void DurabilityPassiveStreamTest::testReceiveMultipleDuplicateDcpPrepares() {
    // This simulates the state in which the active has:
    // PRE1 PRE2 PRE3 CMT1 CMT2 CMT3 PRE1 PRE2 PRE3 CMT1 CMT2 CMT3
    // the replica sees:
    // PRE1 PRE2 PRE3 ||Disconnect|| PRE1 PRE2 PRE3 CMT1 CMT2 CMT3
    // All 3 duplicate prepares should be accepted by
    // allowedDuplicatePrepareSeqnos

    // NB: A mix of prepares levels is used intentionally - they allow
    // us to test that duplicate prepares are permitted:
    // - regardless of level of the replaced prepare
    // - regardless of persistence of the replaced prepare
    // - regardless of the HPS
    // They should only be rejected if they would replace a prepare with a seqno
    // outside the "allowed window". This window is specified as the range
    // [highCompletedSeqno+1, highSeqno] at the time the snapshot marker
    // is received.
    // No prepare with seqno <= highCompletedSeqno should ever be replaced,
    // because it has already been completed and should not be being tracked any
    // more No prepare with seqno > highSeqno (latest seqno seen by VB) should
    // be replaced, because these were received *after* the snapshot marker.
    const uint64_t cas = 999;
    uint64_t seqno = 1;
    std::vector<StoredDocKey> keys = {makeStoredDocKey("key1"),
                                      makeStoredDocKey("key2"),
                                      makeStoredDocKey("key3")};

    // Send the first prepare for each of three keys
    // PRE1 PRE2 PRE3 CMT1 CMT2 CMT3 PRE1 PRE2 PRE3 CMT1 CMT2 CMT3
    // ^^^^ ^^^^ ^^^^
    std::vector<queued_item> queued_items;
    queued_items.push_back(makeAndReceiveDcpPrepare(
            keys[0], cas, seqno++, cb::durability::Level::Majority));
    queued_items.push_back(makeAndReceiveDcpPrepare(
            keys[1],
            cas,
            seqno++,
            cb::durability::Level::MajorityAndPersistOnMaster));
    queued_items.push_back(makeAndReceiveDcpPrepare(
            keys[2], cas, seqno++, cb::durability::Level::PersistToMajority));

    // The consumer now "disconnects" then "re-connects" and misses the commits
    // at seqnos 4, 5, 6.
    // PRE1 PRE2 PRE3 CMT1 CMT2 CMT3 PRE1 PRE2 PRE3 CMT1 CMT2 CMT3
    //                xxxx xxxx xxxx
    // It instead receives the following snapshot [7, 9] containing prepares
    // (for the same 3 keys), followed by a second snapshot [10, 12] with the
    // corresponding commits.
    uint32_t opaque = 0;

    // Fake disconnect and reconnect, importantly, this sets up the valid window
    // for replacing the old prepare.
    consumer->closeAllStreams();
    consumer->addStream(opaque, vbid, 0 /*flags*/);
    stream = static_cast<MockPassiveStream*>(
            (consumer->getVbucketStream(vbid)).get());
    stream->acceptStream(cb::mcbp::Status::Success, opaque);

    ASSERT_TRUE(stream->isActive());
    // At Replica we don't expect multiple Durability items (for the same key)
    // within the same snapshot. That is because the Active prevents that for
    // avoiding de-duplication.
    // So, we need to simulate a Producer sending another SnapshotMarker with
    // the MARKER_FLAG_CHK set before the Consumer receives the Commit. That
    // will force the Consumer closing the open checkpoint (which Contains the
    // Prepare) and creating a new open one for queueing the Commit.
    SnapshotMarker marker(
            opaque,
            vbid,
            7 /*snapStart*/,
            9 /*snapEnd*/,
            dcp_marker_flag_t::MARKER_FLAG_MEMORY | MARKER_FLAG_CHK,
            {} /*streamId*/);
    stream->processMarker(&marker);

    // Do second prepare for each of three keys
    // PRE1 PRE2 PRE3 CMT1 CMT2 CMT3 PRE1 PRE2 PRE3 CMT1 CMT2 CMT3
    //                               ^^^^ ^^^^ ^^^^
    seqno = 7;
    for (const auto& key : keys) {
        queued_items.push_back(makeAndReceiveDcpPrepare(key, cas, seqno++));
    }

    marker = SnapshotMarker(
            opaque,
            vbid,
            10 /*snapStart*/,
            12 /*snapEnd*/,
            dcp_marker_flag_t::MARKER_FLAG_MEMORY | MARKER_FLAG_CHK,
            {} /*streamId*/);
    stream->processMarker(&marker);

    // Commit each of the keys
    // PRE1 PRE2 PRE3 CMT1 CMT2 CMT3 PRE1 PRE2 PRE3 CMT1 CMT2 CMT3
    //                                              ^^^^ ^^^^ ^^^^

    uint64_t prepareSeqno = 7;
    seqno = 10;
    for (const auto& key : keys) {
        ASSERT_EQ(ENGINE_SUCCESS,
                  stream->messageReceived(std::make_unique<CommitSyncWrite>(
                          opaque, vbid, prepareSeqno++, seqno++, key)));
    }
}

TEST_P(DurabilityPassiveStreamTest, ReceiveDuplicateDcpPrepare) {
    testReceiveDuplicateDcpPrepare(3);
}

TEST_P(DurabilityPassiveStreamTest, ReceiveMultipleDuplicateDcpPrepares) {
    testReceiveMultipleDuplicateDcpPrepares();
}

TEST_P(DurabilityPassiveStreamTest, ReceiveDuplicateDcpPrepareRemoveFromSet) {
    testReceiveDuplicateDcpPrepare(3);

    const std::string value("value");
    auto key = makeStoredDocKey("key");
    const uint64_t cas = 999;
    queued_item qi(new Item(key,
                            0 /*flags*/,
                            0 /*expiry*/,
                            value.c_str(),
                            value.size(),
                            PROTOCOL_BINARY_RAW_BYTES,
                            cas /*cas*/,
                            3,
                            vbid));
    using namespace cb::durability;
    qi->setPendingSyncWrite(Requirements(Level::Majority, Timeout::Infinity()));

    uint32_t opaque = 0;
    ASSERT_EQ(ENGINE_ERANGE,
              stream->messageReceived(std::make_unique<MutationConsumerMessage>(
                      std::move(qi),
                      opaque,
                      IncludeValue::Yes,
                      IncludeXattrs::Yes,
                      IncludeDeleteTime::No,
                      DocKeyEncodesCollectionId::No,
                      nullptr,
                      cb::mcbp::DcpStreamId{})));
}

TEST_P(DurabilityPassiveStreamTest, ReceiveDuplicateDcpPrepareRemoveFromPDM) {
    testReceiveDuplicateDcpPrepare(3);

    auto vb = engine->getVBucket(vbid);
    const auto& pdm = VBucketTestIntrospector::public_getPassiveDM(*vb);

    ASSERT_EQ(0, pdm.getNumTracked());
}

TEST_P(DurabilityPassiveStreamTest, DeDupedPrepareWindowDoubleDisconnect) {
    testReceiveDcpPrepare();

    // Send another prepare for our second sequential prepare to overwrite.
    auto key = makeStoredDocKey("key1");
    const uint64_t cas = 999;
    uint64_t prepareSeqno = 2;
    makeAndReceiveDcpPrepare(key, cas, prepareSeqno);

    // The consumer now "disconnects" then "re-connects" and misses a commit for
    // the given key at seqno 3.
    uint32_t opaque = 0;

    // Fake disconnect and reconnect, importantly, this sets up the valid window
    // for replacing the old prepare.
    consumer->closeAllStreams();
    consumer->addStream(opaque, vbid, 0 /*flags*/);
    stream = static_cast<MockPassiveStream*>(
            (consumer->getVbucketStream(vbid)).get());
    stream->acceptStream(cb::mcbp::Status::Success, opaque);
    ASSERT_TRUE(stream->isActive());

    // Receive a snapshot marker [4, 4] for what would be a sequential prepare
    // on the same key This should set the valid sequential prepare window for
    // the vBucket (just seqno 4).
    // At Replica we don't expect multiple Durability items (for the same key)
    // within the same snapshot. That is because the Active prevents that for
    // avoiding de-duplication.
    // So, we need to simulate a Producer sending another SnapshotMarker with
    // the MARKER_FLAG_CHK set before the Consumer receives the Commit. That
    // will force the Consumer closing the open checkpoint (which Contains the
    // Prepare) and creating a new open one for queueing the Commit.
    SnapshotMarker marker(
            opaque,
            vbid,
            5 /*snapStart*/,
            5 /*snapEnd*/,
            dcp_marker_flag_t::MARKER_FLAG_MEMORY | MARKER_FLAG_CHK,
            {} /*streamId*/);
    stream->processMarker(&marker);

    // Now disconnect again.
    consumer->closeAllStreams();
    consumer->addStream(opaque, vbid, 0 /*flags*/);
    stream = static_cast<MockPassiveStream*>(
            (consumer->getVbucketStream(vbid)).get());
    stream->acceptStream(cb::mcbp::Status::Success, opaque);
    ASSERT_TRUE(stream->isActive());

    // We should now expand the previously existing sequential prepare window to
    // seqno 4 and 5.
    marker = SnapshotMarker(
            opaque,
            vbid,
            5 /*snapStart*/,
            6 /*snapEnd*/,
            dcp_marker_flag_t::MARKER_FLAG_MEMORY | MARKER_FLAG_CHK,
            {} /*streamId*/);
    stream->processMarker(&marker);

    // We should now successfully overwrite the existing prepares at seqno 1
    // and seqno 2 with new prepares that exist at seqno 4 and seqno 5.
    key = makeStoredDocKey("key");
    const std::string value("value");
    using namespace cb::durability;
    prepareSeqno = 5;
    queued_item qi(new Item(key,
                            0 /*flags*/,
                            0 /*expiry*/,
                            value.c_str(),
                            value.size(),
                            PROTOCOL_BINARY_RAW_BYTES,
                            cas /*cas*/,
                            prepareSeqno,
                            vbid));
    qi->setPendingSyncWrite(Requirements(Level::Majority, Timeout::Infinity()));

    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<MutationConsumerMessage>(
                      std::move(qi),
                      opaque,
                      IncludeValue::Yes,
                      IncludeXattrs::Yes,
                      IncludeDeleteTime::No,
                      DocKeyEncodesCollectionId::No,
                      nullptr,
                      cb::mcbp::DcpStreamId{})));

    key = makeStoredDocKey("key1");
    prepareSeqno = 6;
    qi = queued_item(new Item(key,
                              0 /*flags*/,
                              0 /*expiry*/,
                              value.c_str(),
                              value.size(),
                              PROTOCOL_BINARY_RAW_BYTES,
                              cas /*cas*/,
                              prepareSeqno,
                              vbid));
    qi->setPendingSyncWrite(Requirements(Level::Majority, Timeout::Infinity()));

    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<MutationConsumerMessage>(
                      std::move(qi),
                      opaque,
                      IncludeValue::Yes,
                      IncludeXattrs::Yes,
                      IncludeDeleteTime::No,
                      DocKeyEncodesCollectionId::No,
                      nullptr,
                      cb::mcbp::DcpStreamId{})));
}

void DurabilityPassiveStreamTest::testReceiveDcpPrepareCommit() {
    // First, simulate the Consumer receiving a Prepare
    testReceiveDcpPrepare();

    auto vb = engine->getVBucket(vbid);
    const uint64_t prepareSeqno = 1;

    // Record CAS for comparison with the later Commit.
    uint64_t cas;
    auto key = makeStoredDocKey("key");
    {
        const auto sv = vb->ht.findForWrite(key);
        ASSERT_TRUE(sv.storedValue);
        ASSERT_EQ(CommittedState::Pending, sv.storedValue->getCommitted());
        ASSERT_EQ(prepareSeqno, sv.storedValue->getBySeqno());
        cas = sv.storedValue->getCas();
    }

    // At Replica we don't expect multiple Durability items (for the same key)
    // within the same snapshot. That is because the Active prevents that for
    // avoiding de-duplication.
    // So, we need to simulate a Producer sending another SnapshotMarker with
    // the MARKER_FLAG_CHK set before the Consumer receives the Commit. That
    // will force the Consumer closing the open checkpoint (which Contains the
    // Prepare) and creating a new open one for queueing the Commit.
    uint32_t opaque = 0;
    auto commitSeqno = prepareSeqno + 1;
    SnapshotMarker marker(
            opaque,
            vbid,
            commitSeqno,
            commitSeqno,
            dcp_marker_flag_t::MARKER_FLAG_MEMORY | MARKER_FLAG_CHK,
            {} /*streamId*/);
    stream->processMarker(&marker);

    // 2 checkpoints
    const auto& ckptList =
            CheckpointManagerTestIntrospector::public_getCheckpointList(
                    *vb->checkpointManager);
    ASSERT_EQ(2, ckptList.size());
    auto* ckpt = ckptList.front().get();
    ASSERT_EQ(checkpoint_state::CHECKPOINT_CLOSED, ckpt->getState());
    ckpt = ckptList.back().get();
    ASSERT_EQ(checkpoint_state::CHECKPOINT_OPEN, ckpt->getState());
    ASSERT_EQ(0, ckpt->getNumItems());

    // Now simulate the Consumer receiving Commit for that Prepare
    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<CommitSyncWrite>(
                      opaque, vbid, prepareSeqno, commitSeqno, key)));

    // Ephemeral keeps the prepare in the hash table whilst ep modifies the
    // existing prepare
    if (persistent()) {
        EXPECT_EQ(1, vb->ht.getNumItems());
    } else {
        EXPECT_EQ(2, vb->ht.getNumItems());
    }

    {
        const auto sv = vb->ht.findForWrite(key).storedValue;
        EXPECT_TRUE(sv);
        EXPECT_EQ(CommittedState::CommittedViaPrepare, sv->getCommitted());
        ASSERT_TRUE(vb->ht.findForWrite(key).storedValue);
    }

    // empty-item
    auto it = ckpt->begin();
    ASSERT_EQ(queue_op::empty, (*it)->getOperation());
    // 1 metaitem (checkpoint-start)
    it++;
    ASSERT_EQ(1, ckpt->getNumMetaItems());
    EXPECT_EQ(queue_op::checkpoint_start, (*it)->getOperation());
    // 1 non-metaitem is Commit and contains the expected value
    it++;
    ASSERT_EQ(1, ckpt->getNumItems());
    EXPECT_EQ(queue_op::commit_sync_write, (*it)->getOperation());
    EXPECT_EQ(key, (*it)->getKey());
    EXPECT_TRUE((*it)->getValue());
    EXPECT_EQ("value", (*it)->getValue()->to_s());
    EXPECT_EQ(commitSeqno, (*it)->getBySeqno());
    EXPECT_EQ(cas, (*it)->getCas());

    EXPECT_EQ(0, vb->getDurabilityMonitor().getNumTracked());
}

/*
 * This test checks that a DCP Consumer receives and processes correctly a
 * DCP_PREPARE followed by a DCP_COMMIT message.
 */
TEST_P(DurabilityPassiveStreamTest, ReceiveDcpCommit) {
    testReceiveDcpPrepareCommit();
}

/*
 * This test checks that a DCP Consumer receives and processes correctly the
 * following sequence (to the same key):
 * - DCP_PREPARE
 * - DCP_COMMIT
 * - DCP_PREPARE
 */
TEST_P(DurabilityPassiveStreamTest, ReceiveDcpPrepareCommitPrepare) {
    // First setup the first DCP_PREPARE and DCP_COMMIT.
    testReceiveDcpPrepareCommit();

    // Process the 2nd Prepare.
    auto key = makeStoredDocKey("key");
    const uint64_t cas = 1234;
    const uint64_t prepare2ndSeqno = 3;
    makeAndReceiveDcpPrepare(key, cas, prepare2ndSeqno);

    // 3 checkpoints
    auto vb = engine->getVBucket(vbid);
    const auto& ckptList =
            CheckpointManagerTestIntrospector::public_getCheckpointList(
                    *vb->checkpointManager);
    ASSERT_EQ(3, ckptList.size());
    auto ckptIt = ckptList.begin();
    ASSERT_EQ(checkpoint_state::CHECKPOINT_CLOSED, (*ckptIt)->getState());
    ASSERT_EQ(1, (*ckptIt)->getNumItems());

    ckptIt++;
    ASSERT_EQ(checkpoint_state::CHECKPOINT_CLOSED, (*ckptIt)->getState());
    ASSERT_EQ(1, (*ckptIt)->getNumItems());

    ckptIt++;
    ASSERT_EQ(checkpoint_state::CHECKPOINT_OPEN, (*ckptIt)->getState());
    ASSERT_EQ(1, (*ckptIt)->getNumItems());

    // 2 Items in HashTable.
    EXPECT_EQ(2, vb->ht.getNumItems())
            << "Should have one Committed and one Prepared items in HashTable";
}

void DurabilityPassiveStreamTest::testReceiveDcpAbort() {
    // First, simulate the Consumer receiving a Prepare
    testReceiveDcpPrepare();
    auto vb = engine->getVBucket(vbid);
    uint64_t prepareSeqno = 1;
    const auto key = makeStoredDocKey("key");

    // Now simulate the Consumer receiving Abort for that Prepare
    uint32_t opaque = 0;
    auto abortReceived = [this, opaque, &key](
                                 uint64_t prepareSeqno,
                                 uint64_t abortSeqno) -> ENGINE_ERROR_CODE {
        return stream->messageReceived(std::make_unique<AbortSyncWrite>(
                opaque, vbid, key, prepareSeqno, abortSeqno));
    };

    // Check a negative first: at Replica we don't expect multiple Durable
    // items within the same checkpoint. That is to avoid Durable items de-dupe
    // at Producer.
    uint64_t abortSeqno = prepareSeqno + 1;
    auto thrown{false};
    try {
        abortReceived(prepareSeqno, abortSeqno);
    } catch (const std::logic_error& e) {
        EXPECT_TRUE(std::string(e.what()).find("duplicate item") !=
                    std::string::npos);
        thrown = true;
    }
    if (!thrown) {
        FAIL();
    }

    // So, we need to simulate a Producer sending another SnapshotMarker with
    // the MARKER_FLAG_CHK set before the Consumer receives the Abort. That
    // will force the Consumer closing the open checkpoint (which Contains the
    // Prepare) and cretaing a new open one for queueing the Abort.
    SnapshotMarker marker(
            opaque,
            vbid,
            3 /*snapStart*/,
            4 /*snapEnd*/,
            dcp_marker_flag_t::MARKER_FLAG_MEMORY | MARKER_FLAG_CHK,
            {} /*streamId*/);
    stream->processMarker(&marker);

    // 2 checkpoints
    const auto& ckptList =
            CheckpointManagerTestIntrospector::public_getCheckpointList(
                    *vb->checkpointManager);
    ASSERT_EQ(2, ckptList.size());
    auto* ckpt = ckptList.front().get();
    ASSERT_EQ(checkpoint_state::CHECKPOINT_CLOSED, ckpt->getState());
    ckpt = ckptList.back().get();
    ASSERT_EQ(checkpoint_state::CHECKPOINT_OPEN, ckpt->getState());
    ASSERT_EQ(0, ckpt->getNumItems());

    // The consumer receives an Abort for the previous Prepare.
    // Note: The call to abortReceived() above throws /after/
    //     PassiveStream::last_seqno has been incremented, so we need to
    //     abortSeqno to bypass ENGINE_ERANGE checks.
    abortSeqno++;
    prepareSeqno++;
    ASSERT_EQ(ENGINE_SUCCESS, abortReceived(prepareSeqno, abortSeqno));

    EXPECT_EQ(0, vb->getNumItems());
    // Ephemeral keeps the completed prepare in the HashTable
    if (persistent()) {
        EXPECT_EQ(0, vb->ht.getNumItems());
    } else {
        EXPECT_EQ(1, vb->ht.getNumItems());
    }
    {
        const auto sv = vb->ht.findForWrite(key);
        ASSERT_FALSE(sv.storedValue);
    }

    // empty-item
    auto it = ckpt->begin();
    ASSERT_EQ(queue_op::empty, (*it)->getOperation());
    // 1 metaitem (checkpoint-start)
    it++;
    ASSERT_EQ(1, ckpt->getNumMetaItems());
    EXPECT_EQ(queue_op::checkpoint_start, (*it)->getOperation());
    // 1 non-metaitem is Abort and carries no value
    it++;
    ASSERT_EQ(1, ckpt->getNumItems());
    EXPECT_EQ(queue_op::abort_sync_write, (*it)->getOperation());
    EXPECT_EQ(key, (*it)->getKey());
    EXPECT_FALSE((*it)->getValue());
    EXPECT_EQ(abortSeqno, (*it)->getBySeqno());

    EXPECT_EQ(0, vb->getDurabilityMonitor().getNumTracked());
}

TEST_P(DurabilityPassiveStreamTest, ReceiveDcpAbort) {
    testReceiveDcpAbort();
}

/**
 * Test that we do not accept an abort without a corresponding prepare if we
 * are receiving an in-memory snapshot.
 */
TEST_P(DurabilityPassiveStreamTest, ReceiveAbortWithoutPrepare) {
    uint32_t opaque = 0;

    SnapshotMarker marker(
            opaque,
            vbid,
            2 /*snapStart*/,
            2 /*snapEnd*/,
            dcp_marker_flag_t::MARKER_FLAG_MEMORY | MARKER_FLAG_CHK,
            {} /*streamId*/);
    stream->processMarker(&marker);

    auto key = makeStoredDocKey("key1");
    auto prepareSeqno = 1;
    auto abortSeqno = 2;
    EXPECT_EQ(ENGINE_EINVAL,
              stream->messageReceived(std::make_unique<AbortSyncWrite>(
                      opaque, vbid, key, prepareSeqno, abortSeqno)));
}

/**
 * Test that we can accept an abort without a correponding prepare if we
 * are receiving a disk snapshot and the prepare seqno is greater than or
 * equal to the snapshot start seqno.
 */
TEST_P(DurabilityPassiveStreamTest, ReceiveAbortWithoutPrepareFromDisk) {
    uint32_t opaque = 0;

    SnapshotMarker marker(opaque,
                          vbid,
                          3 /*snapStart*/,
                          4 /*snapEnd*/,
                          dcp_marker_flag_t::MARKER_FLAG_DISK,
                          {} /*streamId*/);
    stream->processMarker(&marker);

    auto key = makeStoredDocKey("key1");
    auto prepareSeqno = 3;
    auto abortSeqno = 4;
    EXPECT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<AbortSyncWrite>(
                      opaque, vbid, key, prepareSeqno, abortSeqno)));
}

/**
 * Test that we do not accept an abort without a correponding prepare if we
 * are receiving a disk snapshot and the prepare seqno is less than the current
 * snapshot start.
 */
TEST_P(DurabilityPassiveStreamTest,
       ReceiveAbortWithoutPrepareFromDiskInvalidPrepareSeqno) {
    uint32_t opaque = 0;

    SnapshotMarker marker(opaque,
                          vbid,
                          3 /*snapStart*/,
                          4 /*snapEnd*/,
                          dcp_marker_flag_t::MARKER_FLAG_DISK,
                          {} /*streamId*/);
    stream->processMarker(&marker);

    auto key = makeStoredDocKey("key1");
    auto prepareSeqno = 2;
    auto abortSeqno = 4;
    EXPECT_EQ(ENGINE_EINVAL,
              stream->messageReceived(std::make_unique<AbortSyncWrite>(
                      opaque, vbid, key, prepareSeqno, abortSeqno)));
}

void DurabilityPassiveStreamTest::setUpHandleSnapshotEndTest() {
    auto key1 = makeStoredDocKey("key1");
    uint64_t cas = 1;
    uint64_t prepareSeqno = 1;
    makeAndReceiveDcpPrepare(key1, cas, prepareSeqno);

    uint32_t opaque = 0;
    SnapshotMarker marker(
            opaque,
            vbid,
            prepareSeqno + 1 /*snapStart*/,
            prepareSeqno + 2 /*snapEnd*/,
            dcp_marker_flag_t::MARKER_FLAG_MEMORY | MARKER_FLAG_CHK,
            {} /*streamId*/);
    stream->processMarker(&marker);

    auto key2 = makeStoredDocKey("key2");
    using namespace cb::durability;
    auto pending = makePendingItem(
            key2, "value2", Requirements(Level::Majority, Timeout::Infinity()));
    pending->setCas(1);
    pending->setBySeqno(2);

    EXPECT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<MutationConsumerMessage>(
                      pending,
                      opaque,
                      IncludeValue::Yes,
                      IncludeXattrs::Yes,
                      IncludeDeleteTime::No,
                      DocKeyEncodesCollectionId::No,
                      nullptr,
                      cb::mcbp::DcpStreamId{})));

    ASSERT_EQ(true, stream->getCurSnapshotPrepare());
}

TEST_P(DurabilityPassiveStreamTest, HandleSnapshotEndOnCommit) {
    setUpHandleSnapshotEndTest();
    uint32_t opaque;
    auto key = makeStoredDocKey("key1");

    // Commit the original prepare
    auto prepareSeqno = 2;
    auto commitSeqno = prepareSeqno + 1;
    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<CommitSyncWrite>(
                      opaque, vbid, prepareSeqno, commitSeqno, key)));

    // We should have unset (acked the second prepare) the bool flag if we
    // handled the snapshot end
    EXPECT_EQ(false, stream->getCurSnapshotPrepare());
}

TEST_P(DurabilityPassiveStreamTest, HandleSnapshotEndOnAbort) {
    setUpHandleSnapshotEndTest();
    uint32_t opaque;
    auto key = makeStoredDocKey("key1");

    // Abort the original prepare
    auto abortSeqno = 3;
    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<AbortSyncWrite>(
                      opaque, vbid, key, 1 /*prepareSeqno*/, abortSeqno)));

    // We should have unset (acked the second prepare) the bool flag if we
    // handled the snapshot end
    EXPECT_EQ(false, stream->getCurSnapshotPrepare());
}

TEST_P(DurabilityPassiveStreamTest, ReceiveBackfilledDcpCommit) {
    // Need to use actual opaque of the stream as we hit the consumer level
    // function.
    uint32_t opaque = 1;

    SnapshotMarker marker(opaque,
                          vbid,
                          1 /*snapStart*/,
                          2 /*snapEnd*/,
                          dcp_marker_flag_t::MARKER_FLAG_DISK,
                          {} /*streamId*/);
    stream->processMarker(&marker);

    auto key = makeStoredDocKey("key");
    using namespace cb::durability;
    auto prepare = makePendingItem(
            key, "value", Requirements(Level::Majority, Timeout::Infinity()));
    prepare->setBySeqno(1);
    prepare->setCas(999);

    EXPECT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<MutationConsumerMessage>(
                      prepare,
                      opaque,
                      IncludeValue::Yes,
                      IncludeXattrs::Yes,
                      IncludeDeleteTime::No,
                      DocKeyEncodesCollectionId::No,
                      nullptr,
                      cb::mcbp::DcpStreamId{})));

    // Hit the consumer level function (not the stream level) for additional
    // error checking.
    EXPECT_EQ(ENGINE_SUCCESS,
              consumer->commit(opaque, vbid, key, prepare->getBySeqno(), 2));
}

TEST_P(DurabilityPassiveStreamTest, AllowsDupePrepareNamespaceInCheckpoint) {
    uint32_t opaque = 0;

    // 1) Send disk snapshot marker
    SnapshotMarker marker(opaque,
                          vbid,
                          1 /*snapStart*/,
                          2 /*snapEnd*/,
                          dcp_marker_flag_t::MARKER_FLAG_DISK,
                          {} /*streamId*/);
    stream->processMarker(&marker);

    // 2) Send prepare
    auto key = makeStoredDocKey("key");
    using namespace cb::durability;
    auto pending = makePendingItem(
            key, "value", Requirements(Level::Majority, Timeout::Infinity()));
    pending->setBySeqno(1);
    pending->setCas(1);

    EXPECT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<MutationConsumerMessage>(
                      pending,
                      opaque,
                      IncludeValue::Yes,
                      IncludeXattrs::Yes,
                      IncludeDeleteTime::No,
                      DocKeyEncodesCollectionId::No,
                      nullptr,
                      cb::mcbp::DcpStreamId{})));
    auto vb = engine->getVBucket(vbid);
    const auto& pdm = VBucketTestIntrospector::public_getPassiveDM(*vb);
    ASSERT_EQ(1, pdm.getNumTracked());

    // 3) Send commit - should not throw
    auto commitSeqno = pending->getBySeqno() + 1;
    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<CommitSyncWrite>(
                      opaque, vbid, pending->getBySeqno(), commitSeqno, key)));
    flushVBucketToDiskIfPersistent(vbid, 2);
    ASSERT_EQ(0, pdm.getNumTracked());

    // 5) Send next in memory snapshot
    marker = SnapshotMarker(
            opaque,
            vbid,
            3 /*snapStart*/,
            4 /*snapEnd*/,
            dcp_marker_flag_t::MARKER_FLAG_MEMORY | MARKER_FLAG_CHK,
            {} /*streamID*/);
    stream->processMarker(&marker);

    // 6) Send prepare
    pending->setBySeqno(commitSeqno + 1);
    EXPECT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<MutationConsumerMessage>(
                      pending,
                      opaque,
                      IncludeValue::Yes,
                      IncludeXattrs::Yes,
                      IncludeDeleteTime::No,
                      DocKeyEncodesCollectionId::No,
                      nullptr,
                      cb::mcbp::DcpStreamId{})));
    ASSERT_EQ(1, pdm.getNumTracked());

    // 7) Send commit - allowed to exist in same checkpoint
    commitSeqno = pending->getBySeqno() + 1;

    EXPECT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<CommitSyncWrite>(
                      opaque, vbid, pending->getBySeqno(), commitSeqno, key)));
    EXPECT_EQ(0, pdm.getNumTracked());
}

/**
 * This is a valid scenario that we can get in and must deal with.
 *
 * 1) Replica is streaming from the active and receives a partial snapshot:
 *        [1:PRE(k1), 2:NOT RECEIVED(k2)]
 *    Importantly, not receiving the item at seqno 2 means that we do not move
 *    the HPS as we never received the snapshot end so this blocks us from
 *    removing 1:PRE at step 3a. It does not matter what sort of item we have
 *    at seqno 2.
 *
 * 2) Replica disconnects and reconnects which sets the
 *    allowedDuplicatePrepareSeqnos window to 1
 *
 * 3) Replica receives the following disk snapshot:
 *        [4:PRE(k1), 5:MUT(k1)]
 *    We have deduped the initial prepare and the commit at seqno 3.
 *
 *    a) 4:PRE(k1)
 *       We replace 1:PRE in the HashTable with 4:PRE and add 4:PRE to
 *       trackedWrites in the PDM.
 *       This prepare logically completes 1:PRE in the PDM but 1:PRE is not
 *       removed from trackedWrites as the HPS used in the fence to remove the
 *       SyncWrites is still 0 and won't be moved until the snapshot end.
 *    b) 5:MUT(k1)
 *       We find 4:PRE in the HashTable and use this seqno when we attempt to
 *       complete the SyncWrite in the PDM. The PDM starts searching for the
 *       SyncWrite to complete at the beginning of trackedWrites as we are
 *       in a disk snapshot and must allow out of order completion. We then find
 *       the trackedWrite for 1:PRE that still exists in the PDM.
 */
TEST_P(DurabilityPassiveStreamTest, MismatchingPreInHTAndPdm) {
    using namespace cb::durability;

    // 1) Consumer receives [1, 2] snapshot marker but only 1:PRE.
    uint32_t opaque = 0;
    SnapshotMarker marker(
            opaque,
            vbid,
            1 /*snapStart*/,
            2 /*snapEnd*/,
            dcp_marker_flag_t::MARKER_FLAG_MEMORY | MARKER_FLAG_CHK,
            {} /*streamId*/);
    stream->processMarker(&marker);

    auto key = makeStoredDocKey("key");
    std::string value("value1");
    const uint64_t cas = 999;
    queued_item qi = makePendingItem(
            key, value, Requirements(Level::Majority, Timeout::Infinity()));
    qi->setBySeqno(1);
    qi->setCas(cas);

    EXPECT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<MutationConsumerMessage>(
                      qi,
                      opaque,
                      IncludeValue::Yes,
                      IncludeXattrs::Yes,
                      IncludeDeleteTime::No,
                      DocKeyEncodesCollectionId::No,
                      nullptr,
                      cb::mcbp::DcpStreamId{})));

    auto vb = engine->getVBucket(vbid);
    const auto& pdm = VBucketTestIntrospector::public_getPassiveDM(*vb);
    ASSERT_EQ(1, pdm.getNumTracked());

    // 2) Disconnect and reconnect (sets allowedDuplicatePrepareSeqnos to {1}).
    consumer->closeAllStreams();
    consumer->addStream(opaque, vbid, 0 /*flags*/);
    stream = static_cast<MockPassiveStream*>(
            (consumer->getVbucketStream(vbid)).get());
    stream->acceptStream(cb::mcbp::Status::Success, opaque);

    // 3a) Receive 4:PRE
    ASSERT_TRUE(stream->isActive());
    marker = SnapshotMarker(
            opaque,
            vbid,
            1 /*snapStart*/,
            5 /*snapEnd*/,
            dcp_marker_flag_t::MARKER_FLAG_DISK | MARKER_FLAG_CHK,
            {} /*streamId*/);
    stream->processMarker(&marker);

    value = "value4";
    qi = queued_item(new Item(key,
                              0 /*flags*/,
                              0 /*expiry*/,
                              value.c_str(),
                              value.size(),
                              PROTOCOL_BINARY_RAW_BYTES,
                              cas /*cas*/,
                              4 /*seqno*/,
                              vbid));
    qi->setPendingSyncWrite(Requirements(Level::Majority, Timeout::Infinity()));
    ASSERT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<MutationConsumerMessage>(
                      std::move(qi),
                      opaque,
                      IncludeValue::Yes,
                      IncludeXattrs::Yes,
                      IncludeDeleteTime::No,
                      DocKeyEncodesCollectionId::No,
                      nullptr,
                      cb::mcbp::DcpStreamId{})));

    // We remove the SyncWrite corresponding to 1:PRE when we receive 4:PRE
    // even though we have not reached the snap end and moved the HPS because
    // we do not want to keep duplicate keys in trackedWrites.
    EXPECT_EQ(1, pdm.getNumTracked());

    // 3b) Receive 5:PRE
    value = "value5";
    auto item = makeCommittedItem(key, value);
    item->setBySeqno(5);
    EXPECT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<MutationConsumerMessage>(
                      std::move(item),
                      opaque,
                      IncludeValue::Yes,
                      IncludeXattrs::Yes,
                      IncludeDeleteTime::No,
                      DocKeyEncodesCollectionId::No,
                      nullptr,
                      cb::mcbp::DcpStreamId{})));

    // Persist and notify the PDM as 4:PRE requires persistence to complete due
    // to possibly deduping a persist level prepare. We flush 3 items because
    // the two prepares are in different checkpoint types.
    flushVBucketToDiskIfPersistent(vbid, 3);

    EXPECT_EQ(0, pdm.getNumTracked());
}

// Test covers issue seen in MB-35062, we must be able to tolerate a prepare
// and a delete, the replica must accept both when in a disk snapshot
TEST_P(DurabilityPassiveStreamTest, BackfillPrepareDelete) {
    uint32_t opaque = 0;

    // Send a snapshot which is
    // seq:1 prepare(key)
    // seq:3 delete(key)
    // In this case seq:2 was the commit and is now represented by del seq:3

    // 1) Send disk snapshot marker
    SnapshotMarker marker(opaque,
                          vbid,
                          1 /*snapStart*/,
                          3 /*snapEnd*/,
                          dcp_marker_flag_t::MARKER_FLAG_DISK,
                          {} /*streamId*/);
    stream->processMarker(&marker);

    // 2) Send prepare
    auto key = makeStoredDocKey("key");
    using namespace cb::durability;
    auto pending = makePendingItem(
            key, "value", Requirements(Level::Majority, Timeout::Infinity()));
    pending->setBySeqno(1);
    pending->setCas(1);
    EXPECT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<MutationConsumerMessage>(
                      pending,
                      opaque,
                      IncludeValue::Yes,
                      IncludeXattrs::Yes,
                      IncludeDeleteTime::No,
                      DocKeyEncodesCollectionId::No,
                      nullptr,
                      cb::mcbp::DcpStreamId{})));

    // 3) Send delete of the prepared key
    auto deleted = makeCommittedItem(key, {});
    deleted->setDeleted(DeleteSource::Explicit);
    deleted->setBySeqno(3);
    queued_item qi{deleted};
    EXPECT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<MutationConsumerMessage>(
                      deleted,
                      opaque,
                      IncludeValue::Yes,
                      IncludeXattrs::Yes,
                      IncludeDeleteTime::No,
                      DocKeyEncodesCollectionId::No,
                      nullptr,
                      cb::mcbp::DcpStreamId{})));

    // Expect two items in the flush, prepare and delete
    flushVBucketToDiskIfPersistent(vbid, 2);

    // The vbucket must now be upto seqno 3
    auto vb = engine->getVBucket(vbid);
    EXPECT_EQ(3, vb->getHighSeqno());
}

/**
 * We have to treat all prepares in a disk snapshot as requiring persistence
 * due to them possibly de-duplicating a PersistToMajority SyncWrite. As this is
 * the case, we can keep completed SyncWrite objects in the PDM trackedWrites
 * even after one has been completed (a disk snapshot contains a prepare and a
 * commit for the same key). These prepares are logically completed and should
 * not trigger any sanity check exceptions due to having multiple prepares for
 * the same key in trackedWrites.
 */
TEST_P(DurabilityPassiveStreamTest, CompletedDiskPreIsIgnoredBySanityChecks) {
    uint32_t opaque = 0;

    // 1) Receive disk snapshot marker
    SnapshotMarker marker(opaque,
                          vbid,
                          1 /*snapStart*/,
                          2 /*snapEnd*/,
                          dcp_marker_flag_t::MARKER_FLAG_DISK,
                          {} /*streamId*/);
    stream->processMarker(&marker);

    // 2) Receive prepare
    auto key = makeStoredDocKey("key");
    using namespace cb::durability;
    auto pending = makePendingItem(
            key, "value", Requirements(Level::Majority, Timeout::Infinity()));
    pending->setBySeqno(1);
    pending->setCas(1);
    EXPECT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<MutationConsumerMessage>(
                      pending,
                      opaque,
                      IncludeValue::Yes,
                      IncludeXattrs::Yes,
                      IncludeDeleteTime::No,
                      DocKeyEncodesCollectionId::No,
                      nullptr,
                      cb::mcbp::DcpStreamId{})));

    // 3) Receive overwriting set instead of commit
    const std::string value = "commit";
    auto item = makeCommittedItem(key, value);
    item->setBySeqno(2);
    item->setCas(1);
    EXPECT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<MutationConsumerMessage>(
                      std::move(item),
                      opaque,
                      IncludeValue::Yes,
                      IncludeXattrs::Yes,
                      IncludeDeleteTime::No,
                      DocKeyEncodesCollectionId::No,
                      nullptr,
                      cb::mcbp::DcpStreamId{})));

    // 4) Receive memory snapshot marker
    marker = SnapshotMarker(
            opaque,
            vbid,
            3 /*snapStart*/,
            3 /*snapEnd*/,
            dcp_marker_flag_t::MARKER_FLAG_MEMORY | MARKER_FLAG_CHK,
            {} /*streamId*/);
    stream->processMarker(&marker);

    // 5) Receive prepare
    pending = makePendingItem(
            key, "value", Requirements(Level::Majority, Timeout::Infinity()));
    pending->setBySeqno(3);
    pending->setCas(2);
    EXPECT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<MutationConsumerMessage>(
                      pending,
                      opaque,
                      IncludeValue::Yes,
                      IncludeXattrs::Yes,
                      IncludeDeleteTime::No,
                      DocKeyEncodesCollectionId::No,
                      nullptr,
                      cb::mcbp::DcpStreamId{})));
}

TEST_P(DurabilityPassiveStreamTest,
       CompletedPersistPreIsIgnoredBySanityChecks) {
    uint32_t opaque = 0;

    // 1) Receive disk snapshot marker
    SnapshotMarker marker(
            opaque,
            vbid,
            1 /*snapStart*/,
            2 /*snapEnd*/,
            dcp_marker_flag_t::MARKER_FLAG_MEMORY | MARKER_FLAG_CHK,
            {} /*streamId*/);
    stream->processMarker(&marker);

    // 2) Receive prepare
    auto key = makeStoredDocKey("key");
    using namespace cb::durability;
    auto pending = makePendingItem(
            key,
            "value",
            Requirements(Level::PersistToMajority, Timeout::Infinity()));
    pending->setBySeqno(1);
    pending->setCas(1);
    EXPECT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<MutationConsumerMessage>(
                      pending,
                      opaque,
                      IncludeValue::Yes,
                      IncludeXattrs::Yes,
                      IncludeDeleteTime::No,
                      DocKeyEncodesCollectionId::No,
                      nullptr,
                      cb::mcbp::DcpStreamId{})));

    // 3) Receive commit
    ASSERT_EQ(
            ENGINE_SUCCESS,
            stream->messageReceived(std::make_unique<CommitSyncWrite>(
                    opaque, vbid, 1 /*prepareSeqno*/, 2 /*commitSeqno*/, key)));

    // 4) Receive memory snapshot marker
    marker = SnapshotMarker(
            opaque,
            vbid,
            3 /*snapStart*/,
            3 /*snapEnd*/,
            dcp_marker_flag_t::MARKER_FLAG_MEMORY | MARKER_FLAG_CHK,
            {} /*streamId*/);
    stream->processMarker(&marker);

    // 5) Receive prepare
    pending = makePendingItem(
            key, "value", Requirements(Level::Majority, Timeout::Infinity()));
    pending->setBySeqno(3);
    pending->setCas(2);
    EXPECT_EQ(ENGINE_SUCCESS,
              stream->messageReceived(std::make_unique<MutationConsumerMessage>(
                      pending,
                      opaque,
                      IncludeValue::Yes,
                      IncludeXattrs::Yes,
                      IncludeDeleteTime::No,
                      DocKeyEncodesCollectionId::No,
                      nullptr,
                      cb::mcbp::DcpStreamId{})));
}

void DurabilityPromotionStreamTest::SetUp() {
    // Set up as a replica
    DurabilityPassiveStreamTest::SetUp();
}

void DurabilityPromotionStreamTest::TearDown() {
    // Tear down as active
    DurabilityActiveStreamTest::TearDown();
}

void DurabilityPromotionStreamTest::testDiskCheckpointStreamedAsDiskSnapshot() {
    // 1) Receive a prepare followed by a commit in a disk checkpoint as a
    // replica then flush it
    DurabilityPassiveStreamTest::
            testReceiveMutationOrDeletionInsteadOfCommitWhenStreamingFromDisk(
                    DocumentState::Alive);
    flushVBucketToDiskIfPersistent(vbid, 2);

    // Remove the Consumer and PassiveStream
    ASSERT_EQ(ENGINE_SUCCESS, consumer->closeStream(0 /*opaque*/, vbid));
    consumer.reset();

    // 3) Set up the Producer and ActiveStream
    DurabilityActiveStreamTest::setUp(true /*startCheckpointProcessorTask*/);

    // 4) Write something to a different key. This should be written into a new
    // checkpoint as we would still be in a Disk checkpoint.
    auto vb = engine->getVBucket(vbid);
    const auto key = makeStoredDocKey("differentKey");
    const std::string value = "value";
    auto item = makeCommittedItem(key, value);
    {
        auto cHandle = vb->lockCollections(item->getKey());
        EXPECT_EQ(ENGINE_SUCCESS, vb->set(*item, cookie, *engine, {}, cHandle));
    }

    auto& stream = DurabilityActiveStreamTest::stream;
    ASSERT_TRUE(stream->public_supportSyncReplication());

    // 5) Test the checkpoint and stream output.

    // We must have ckpt-start + Prepare + Mutation + ckpt-end
    auto outItems = stream->public_getOutstandingItems(*vb);
    ASSERT_EQ(4, outItems.items.size());
    ASSERT_EQ(queue_op::checkpoint_start, outItems.items.at(0)->getOperation());
    ASSERT_EQ(queue_op::pending_sync_write,
              outItems.items.at(1)->getOperation());
    ASSERT_EQ(queue_op::mutation, outItems.items.at(2)->getOperation());

    // We create a new checkpoint as a result of the state change
    ASSERT_EQ(queue_op::checkpoint_end, outItems.items.at(3)->getOperation());

    // Stream::readyQ still empty
    ASSERT_EQ(0, stream->public_readyQSize());
    // Push items into the Stream::readyQ
    stream->public_processItems(outItems);

    // No message processed, BufferLog empty
    ASSERT_EQ(0, producer->getBytesOutstanding());

    // readyQ must contain a SnapshotMarker + Prepare + Mutation
    ASSERT_EQ(3, stream->public_readyQSize());
    auto resp = stream->public_nextQueuedItem();
    ASSERT_TRUE(resp);

    // Snapshot marker must have the disk flag set, not the memory flag
    EXPECT_EQ(DcpResponse::Event::SnapshotMarker, resp->getEvent());
    EXPECT_EQ(MARKER_FLAG_DISK | MARKER_FLAG_CHK,
              static_cast<SnapshotMarker&>(*resp).getFlags());

    // readyQ must contain a DCP_PREPARE
    ASSERT_EQ(2, stream->public_readyQSize());
    resp = stream->public_nextQueuedItem();
    EXPECT_EQ(DcpResponse::Event::Prepare, resp->getEvent());
    resp = stream->public_nextQueuedItem();
    EXPECT_EQ(DcpResponse::Event::Mutation, resp->getEvent());
    EXPECT_EQ(0, stream->public_readyQSize());

    // Simulate running the checkpoint processor task again
    outItems = stream->public_getOutstandingItems(*vb);
    ASSERT_EQ(2, outItems.items.size());
    // set_vbucket_state is from changing to active in the middle of this test
    ASSERT_EQ(queue_op::set_vbucket_state,
              outItems.items.at(0)->getOperation());
    ASSERT_EQ(queue_op::mutation, outItems.items.at(1)->getOperation());

    // Stream::readyQ still empty
    ASSERT_EQ(0, stream->public_readyQSize());
    // Push items into the Stream::readyQ
    stream->public_processItems(outItems);
    // readyQ must contain a SnapshotMarker (+ a Prepare)
    ASSERT_EQ(2, stream->public_readyQSize());
    resp = stream->public_nextQueuedItem();
    ASSERT_TRUE(resp);

    // Snapshot marker should now be memory flag
    EXPECT_EQ(DcpResponse::Event::SnapshotMarker, resp->getEvent());
    EXPECT_EQ(MARKER_FLAG_MEMORY | MARKER_FLAG_CHK,
              static_cast<SnapshotMarker&>(*resp).getFlags());

    resp = stream->public_nextQueuedItem();
    EXPECT_EQ(DcpResponse::Event::Mutation, resp->getEvent());
    ASSERT_EQ(0, stream->public_readyQSize());

    producer->cancelCheckpointCreatorTask();
}

TEST_P(DurabilityPromotionStreamTest,
       DiskCheckpointStreamedAsDiskSnapshotReplica) {
    // Should already be replica
    testDiskCheckpointStreamedAsDiskSnapshot();
}

TEST_P(DurabilityPromotionStreamTest,
       DiskCheckpointStreamedAsDiskSnapshotPending) {
    setVBucketStateAndRunPersistTask(vbid, vbucket_state_pending);
    testDiskCheckpointStreamedAsDiskSnapshot();
}

INSTANTIATE_TEST_CASE_P(AllBucketTypes,
                        DurabilityActiveStreamTest,
                        STParameterizedBucketTest::allConfigValues(),
                        STParameterizedBucketTest::PrintToStringParamName);

INSTANTIATE_TEST_CASE_P(AllBucketTypes,
                        DurabilityPassiveStreamTest,
                        STParameterizedBucketTest::allConfigValues(),
                        STParameterizedBucketTest::PrintToStringParamName);

INSTANTIATE_TEST_CASE_P(AllBucketTypes,
                        DurabilityPromotionStreamTest,
                        STParameterizedBucketTest::allConfigValues(),
                        STParameterizedBucketTest::PrintToStringParamName);

INSTANTIATE_TEST_CASE_P(
        AllBucketTypes,
        DurabilityPassiveStreamPersistentTest,
        STParameterizedBucketTest::persistentAllBackendsConfigValues(),
        STParameterizedBucketTest::PrintToStringParamName);

INSTANTIATE_TEST_CASE_P(AllBucketTypes,
                        DurabilityActiveStreamEphemeralTest,
                        STParameterizedBucketTest::ephConfigValues(),
                        STParameterizedBucketTest::PrintToStringParamName);
