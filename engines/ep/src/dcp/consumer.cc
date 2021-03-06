/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc
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

#include "dcp/consumer.h"

#include "checkpoint_manager.h"
#include "dcp/dcpconnmap.h"
#include "dcp/passive_stream.h"
#include "dcp/response.h"
#include "ep_engine.h"
#include "ep_time.h"
#include "executorpool.h"
#include "failover-table.h"
#include "kv_bucket.h"
#include "replicationthrottle.h"

#include <phosphor/phosphor.h>

const std::string DcpConsumer::noopCtrlMsg = "enable_noop";
const std::string DcpConsumer::noopIntervalCtrlMsg = "set_noop_interval";
const std::string DcpConsumer::connBufferCtrlMsg = "connection_buffer_size";
const std::string DcpConsumer::priorityCtrlMsg = "set_priority";
const std::string DcpConsumer::extMetadataCtrlMsg = "enable_ext_metadata";
const std::string DcpConsumer::forceCompressionCtrlMsg = "force_value_compression";
// from vulcan onwards we only use the _vulcan control message
const std::string DcpConsumer::cursorDroppingCtrlMsg =
        "supports_cursor_dropping_vulcan";
const std::string DcpConsumer::sendStreamEndOnClientStreamCloseCtrlMsg =
        "send_stream_end_on_client_close_stream";
const std::string DcpConsumer::hifiMFUCtrlMsg = "supports_hifi_MFU";

class DcpConsumerTask : public GlobalTask {
public:
    DcpConsumerTask(EventuallyPersistentEngine* e,
                    std::shared_ptr<DcpConsumer> c,
                    double sleeptime = 1,
                    bool completeBeforeShutdown = true)
        : GlobalTask(e,
                     TaskId::DcpConsumerTask,
                     sleeptime,
                     completeBeforeShutdown),
          consumerPtr(c),
          description("DcpConsumerTask, processing buffered items for " +
                      c->getName()) {
    }

    ~DcpConsumerTask() {
        auto consumer = consumerPtr.lock();
        if (consumer) {
            consumer->taskCancelled();
        }
    }

    bool run() {
        TRACE_EVENT0("ep-engine/task", "DcpConsumerTask");
        auto consumer = consumerPtr.lock();
        if (!consumer) {
            return false;
        }

        if (consumer->doDisconnect()) {
            return false;
        }

        double sleepFor = 0.0;
        enum process_items_error_t state = consumer->processBufferedItems();
        switch (state) {
            case all_processed:
                sleepFor = INT_MAX;
                break;
            case more_to_process:
                sleepFor = 0.0;
                break;
            case cannot_process:
                sleepFor = 5.0;
                break;
            case stop_processing:
                return false;
        }

        // Check if we've been notified of more work to do - if not then sleep;
        // if so then wakeup and re-run the task.
        // Note: The order of the wakeUp / snooze here is *critical* - another
        // thread may concurrently notify us (set processorNotification=true)
        // while we are performing the checks, so we need to ensure we don't
        // loose a wakeup as that would result in this Task sleeping forever
        // (and DCP hanging).
        // To prevent this, we perform an initial check of notifiedProcessor(),
        // which if false we initially sleep, and then check a second time.
        // We could race if the other actor sets processorNotification=true
        // between the second `if(consumer->notifiedProcessor)` and us calling
        // `wakeUp()`; but that's essentially a benign race as it will just
        // result in wakeUp() being called twice which is benign.
        if (consumer->notifiedProcessor(false)) {
            wakeUp();
            state = more_to_process;
        } else {
            snooze(sleepFor);
            // Check if the processor was notified again,
            // in which case the task should wake immediately.
            if (consumer->notifiedProcessor(false)) {
                wakeUp();
                state = more_to_process;
            }
        }

        consumer->setProcessorTaskState(state);

        return true;
    }

    std::string getDescription() {
        return description;
    }

    std::chrono::microseconds maxExpectedDuration() {
        // This should be a very fast operation (p50 under 10us), however we
        // have observed long tails: p99.9 of 20ms; so use a threshold of 100ms.
        return std::chrono::milliseconds(100);
    }

private:
    /* we have one task per consumer. the task only needs a reference to the
       consumer object and does not own it. Hence std::weak_ptr should be used*/
    const std::weak_ptr<DcpConsumer> consumerPtr;
    const std::string description;
};

DcpConsumer::DcpConsumer(EventuallyPersistentEngine& engine,
                         const void* cookie,
                         const std::string& name)
    : ConnHandler(engine, cookie, name),
      lastMessageTime(ep_current_time()),
      engine(engine),
      opaqueCounter(0),
      processorTaskId(0),
      processorTaskState(all_processed),
      processorNotification(false),
      backoffs(0),
      dcpIdleTimeout(engine.getConfiguration().getDcpIdleTimeout()),
      dcpNoopTxInterval(engine.getConfiguration().getDcpNoopTxInterval()),
      pendingSendStreamEndOnClientStreamClose(true),
      producerIsVersion5orHigher(false),
      processorTaskRunning(false),
      flowControl(engine, this),
      processBufferedMessagesYieldThreshold(
              engine.getConfiguration()
                      .getDcpConsumerProcessBufferedMessagesYieldLimit()),
      processBufferedMessagesBatchSize(
              engine.getConfiguration()
                      .getDcpConsumerProcessBufferedMessagesBatchSize()) {
    Configuration& config = engine.getConfiguration();
    setSupportAck(false);
    logger.setId(engine.getServerApi()->cookie->get_log_info(cookie).first);
    setLogHeader("DCP (Consumer) " + getName() + " -");
    setReserved(true);

    pendingEnableNoop = config.isDcpEnableNoop();
    getErrorMapState = pendingEnableNoop ? GetErrorMapState::PendingRequest
                                         : GetErrorMapState::Skip;
    pendingSendNoopInterval = config.isDcpEnableNoop();
    pendingSetPriority = true;
    pendingEnableExtMetaData = true;
    pendingSupportCursorDropping = true;
    pendingSupportHifiMFU =
            (config.getHtEvictionPolicy() == "hifi_mfu");
}

DcpConsumer::~DcpConsumer() {
    cancelTask();
}


void DcpConsumer::cancelTask() {
    bool exp = true;
    if (processorTaskRunning.compare_exchange_strong(exp, false)) {
        ExecutorPool::get()->cancel(processorTaskId);
    }
}

void DcpConsumer::taskCancelled() {
    processorTaskRunning.store(false);
}

std::shared_ptr<PassiveStream> DcpConsumer::makePassiveStream(
        EventuallyPersistentEngine& e,
        std::shared_ptr<DcpConsumer> consumer,
        const std::string& name,
        uint32_t flags,
        uint32_t opaque,
        uint16_t vb,
        uint64_t start_seqno,
        uint64_t end_seqno,
        uint64_t vb_uuid,
        uint64_t snap_start_seqno,
        uint64_t snap_end_seqno,
        uint64_t vb_high_seqno) {
    return std::make_shared<PassiveStream>(&e,
                                           consumer,
                                           name,
                                           flags,
                                           opaque,
                                           vb,
                                           start_seqno,
                                           end_seqno,
                                           vb_uuid,
                                           snap_start_seqno,
                                           snap_end_seqno,
                                           vb_high_seqno);
}

ENGINE_ERROR_CODE DcpConsumer::addStream(uint32_t opaque, uint16_t vbucket,
                                         uint32_t flags) {
    lastMessageTime = ep_current_time();
    LockHolder lh(readyMutex);
    if (doDisconnect()) {
        return ENGINE_DISCONNECT;
    }

    VBucketPtr vb = engine_.getVBucket(vbucket);
    if (!vb) {
        logger.log(EXTENSION_LOG_WARNING,
            "(vb %d) Add stream failed because this vbucket doesn't exist",
            vbucket);
        return ENGINE_NOT_MY_VBUCKET;
    }

    if (vb->getState() == vbucket_state_active) {
        logger.log(EXTENSION_LOG_WARNING,
            "(vb %d) Add stream failed because this vbucket happens to be in "
            "active state", vbucket);
        return ENGINE_NOT_MY_VBUCKET;
    }

    snapshot_info_t info = vb->checkpointManager->getSnapshotInfo();
    if (info.range.end == info.start) {
        info.range.start = info.start;
    }

    uint32_t new_opaque = ++opaqueCounter;
    failover_entry_t entry = vb->failovers->getLatestEntry();
    uint64_t start_seqno = info.start;
    uint64_t end_seqno = std::numeric_limits<uint64_t>::max();
    uint64_t vbucket_uuid = entry.vb_uuid;
    uint64_t snap_start_seqno = info.range.start;
    uint64_t snap_end_seqno = info.range.end;
    uint64_t high_seqno = vb->getHighSeqno();

    auto stream = findStream(vbucket);
    if (stream) {
        if(stream->isActive()) {
            EP_LOG_WARN(
                    "{} (vb:{}) Cannot add stream because "
                    "one already exists",
                    logHeader(),
                    vbucket);
            return ENGINE_KEY_EEXISTS;
        } else {
            streams.erase(vbucket);
        }
    }

    /* We need 'Processor' task only when we have a stream. Hence create it
     only once when the first stream is added */
    bool exp = false;
    if (processorTaskRunning.compare_exchange_strong(exp, true)) {
        ExTask task = std::make_shared<DcpConsumerTask>(
                &engine, shared_from_this(), 1);
        processorTaskId = ExecutorPool::get()->schedule(task);
    }

    streams.insert({vbucket,
                    makePassiveStream(engine_,
                                      shared_from_this(),
                                      getName(),
                                      flags,
                                      new_opaque,
                                      vbucket,
                                      start_seqno,
                                      end_seqno,
                                      vbucket_uuid,
                                      snap_start_seqno,
                                      snap_end_seqno,
                                      high_seqno)});
    ready.push_back(vbucket);
    opaqueMap_[new_opaque] = std::make_pair(opaque, vbucket);

    return ENGINE_SUCCESS;
}

ENGINE_ERROR_CODE DcpConsumer::closeStream(uint32_t opaque, uint16_t vbucket) {
    lastMessageTime = ep_current_time();
    if (doDisconnect()) {
        streams.erase(vbucket);
        return ENGINE_DISCONNECT;
    }

    opaque_map::iterator oitr = opaqueMap_.find(opaque);
    if (oitr != opaqueMap_.end()) {
        opaqueMap_.erase(oitr);
    }

    auto stream = findStream(vbucket);
    if (!stream) {
        EP_LOG_WARN(
                "{} (vb:{}) Cannot close stream because no "
                "stream exists for this vbucket",
                logHeader(),
                vbucket);
        return ENGINE_KEY_ENOENT;
    }

    uint32_t bytesCleared = stream->setDead(END_STREAM_CLOSED);
    flowControl.incrFreedBytes(bytesCleared);
    streams.erase(vbucket);
    notifyConsumerIfNecessary(true/*schedule*/);

    return ENGINE_SUCCESS;
}

ENGINE_ERROR_CODE DcpConsumer::streamEnd(uint32_t opaque, uint16_t vbucket,
                                         uint32_t flags) {
    lastMessageTime = ep_current_time();
    UpdateFlowControl ufc(*this, StreamEndResponse::baseMsgBytes);
    if (doDisconnect()) {
        return ENGINE_DISCONNECT;
    }

    auto stream = findStream(vbucket);
    if (!stream) {
        EP_LOG_WARN(
                "{} (vb:{}) End stream received but no such stream for this "
                "vBucket",
                logHeader(),
                vbucket);
        return ENGINE_KEY_ENOENT;
    }

    if (!stream->isActive()) {
        EP_LOG_WARN("{} (vb:{}) End stream received but stream is not active",
                    logHeader(),
                    vbucket);
        return ENGINE_KEY_ENOENT;
    }

    if (stream->getOpaque() != opaque) {
        EP_LOG_WARN(
                "{} (vb:{}) End stream received with opaque {} but expected {}",
                logHeader(),
                vbucket,
                opaque,
                stream->getOpaque());
        return ENGINE_KEY_ENOENT;
    }

    EP_LOG_INFO("{} (vb:{}) End stream received with reason {}",
                logHeader(),
                vbucket,
                flags);

    ENGINE_ERROR_CODE err = ENGINE_KEY_ENOENT;
    try {
        err = stream->messageReceived(std::make_unique<StreamEndResponse>(
                opaque, static_cast<end_stream_status_t>(flags), vbucket));
    } catch (const std::bad_alloc&) {
        return ENGINE_ENOMEM;
    }

    // The item was buffered and will be processed later
    if (err == ENGINE_TMPFAIL) {
        ufc.release();
        notifyVbucketReady(vbucket);
        return ENGINE_SUCCESS;
    }

    return err;
}

ENGINE_ERROR_CODE DcpConsumer::mutation(uint32_t opaque,
                                        const DocKey& key,
                                        cb::const_byte_buffer value,
                                        size_t priv_bytes,
                                        uint8_t datatype,
                                        uint64_t cas,
                                        uint16_t vbucket,
                                        uint32_t flags,
                                        uint64_t bySeqno,
                                        uint64_t revSeqno,
                                        uint32_t exptime,
                                        uint32_t lock_time,
                                        cb::const_byte_buffer meta,
                                        uint8_t nru) {
    lastMessageTime = ep_current_time();
    UpdateFlowControl ufc(*this,
                          MutationResponse::mutationBaseMsgBytes + key.size() +
                                  meta.size() + value.size());

    if (doDisconnect()) {
        return ENGINE_DISCONNECT;
    }

    if (bySeqno == 0) {
        EP_LOG_WARN(
                "{} (vb:{}) Invalid sequence number(0) "
                "for mutation!",
                logHeader(),
                vbucket);
        return ENGINE_EINVAL;
    }

    ENGINE_ERROR_CODE err = ENGINE_KEY_ENOENT;
    auto stream = findStream(vbucket);
    if (stream && stream->getOpaque() == opaque && stream->isActive()) {
        queued_item item(new Item(key,
                                  flags,
                                  exptime,
                                  value.data(),
                                  value.size(),
                                  datatype,
                                  cas,
                                  bySeqno,
                                  vbucket,
                                  revSeqno));
        item->setNRUValue(nru);

        std::unique_ptr<ExtendedMetaData> emd;
        if (meta.size() > 0) {
            emd = std::make_unique<ExtendedMetaData>(meta.data(), meta.size());
            if (emd->getStatus() == ENGINE_EINVAL) {
                return ENGINE_EINVAL;
            }
        }

        try {
            err = stream->messageReceived(
                    std::make_unique<MutationResponse>(item,
                                                       opaque,
                                                       IncludeValue::Yes,
                                                       IncludeXattrs::Yes,
                                                       IncludeDeleteTime::No,
                                                       emd.release()));
        } catch (const std::bad_alloc&) {
            return ENGINE_ENOMEM;
        }

        // The item was buffered and will be processed later
        if (err == ENGINE_TMPFAIL) {
            ufc.release();
            notifyVbucketReady(vbucket);
            return ENGINE_SUCCESS;
        }
    }

    return err;
}

ENGINE_ERROR_CODE DcpConsumer::deletion(uint32_t opaque,
                                        const DocKey& key,
                                        cb::const_byte_buffer value,
                                        size_t priv_bytes,
                                        uint8_t datatype,
                                        uint64_t cas,
                                        uint16_t vbucket,
                                        uint64_t bySeqno,
                                        uint64_t revSeqno,
                                        cb::const_byte_buffer meta) {
    UpdateFlowControl ufc(*this,
                          MutationResponse::deletionBaseMsgBytes + key.size() +
                                  meta.size() + value.size());
    auto err = deletion(opaque,
                        key,
                        value,
                        datatype,
                        cas,
                        vbucket,
                        bySeqno,
                        revSeqno,
                        meta,
                        0,
                        IncludeDeleteTime::No);

    // TMPFAIL means the stream has buffered the message for later processing
    // so skip flowControl, success or any other error, we still need to ack
    if (err == ENGINE_TMPFAIL) {
        ufc.release();
        // Mask the TMPFAIL
        return ENGINE_SUCCESS;
    }

    return err;
}

ENGINE_ERROR_CODE DcpConsumer::deletionV2(uint32_t opaque,
                                          const DocKey& key,
                                          cb::const_byte_buffer value,
                                          size_t priv_bytes,
                                          uint8_t datatype,
                                          uint64_t cas,
                                          uint16_t vbucket,
                                          uint64_t bySeqno,
                                          uint64_t revSeqno,
                                          uint32_t deleteTime) {
    UpdateFlowControl ufc(*this,
                          MutationResponse::deletionV2BaseMsgBytes +
                                  key.size() + value.size());
    auto err = deletion(opaque,
                        key,
                        value,
                        datatype,
                        cas,
                        vbucket,
                        bySeqno,
                        revSeqno,
                        {},
                        deleteTime,
                        IncludeDeleteTime::Yes);

    // TMPFAIL means the stream has buffered the message for later processing
    // so skip flowControl, success or any other error, we still need to ack
    if (err == ENGINE_TMPFAIL) {
        ufc.release();
        // Mask the TMPFAIL
        return ENGINE_SUCCESS;
    }

    return err;
}

ENGINE_ERROR_CODE DcpConsumer::deletion(uint32_t opaque,
                                        const DocKey& key,
                                        cb::const_byte_buffer value,
                                        uint8_t datatype,
                                        uint64_t cas,
                                        uint16_t vbucket,
                                        uint64_t bySeqno,
                                        uint64_t revSeqno,
                                        cb::const_byte_buffer meta,
                                        uint32_t deleteTime,
                                        IncludeDeleteTime includeDeleteTime) {
    lastMessageTime = ep_current_time();

    if (doDisconnect()) {
        return ENGINE_DISCONNECT;
    }

    if (bySeqno == 0) {
        EP_LOG_WARN(
                "{} (vb:{}) Invalid sequence number(0)"
                "for deletion!",
                logHeader(),
                vbucket);
        return ENGINE_EINVAL;
    }

    ENGINE_ERROR_CODE err = ENGINE_KEY_ENOENT;
    auto stream = findStream(vbucket);
    if (stream && stream->getOpaque() == opaque && stream->isActive()) {
        queued_item item(new Item(key,
                                  0,
                                  deleteTime,
                                  value.data(),
                                  value.size(),
                                  datatype,
                                  cas,
                                  bySeqno,
                                  vbucket,
                                  revSeqno));
        item->setDeleted();

        // MB-29040: Producer may send deleted doc with value that still has
        // the user xattrs and the body. Fix up that mistake by running the
        // expiry hook which will correctly process the document
        if (mcbp::datatype::is_xattr(datatype) && value.size()) {
            auto vb = engine_.getVBucket(vbucket);
            if (vb) {
                engine_.getKVBucket()->runPreExpiryHook(*vb, *item);
            }
        }

        std::unique_ptr<ExtendedMetaData> emd;
        if (meta.size() > 0) {
            emd = std::make_unique<ExtendedMetaData>(meta.data(), meta.size());
            if (emd->getStatus() == ENGINE_EINVAL) {
                err = ENGINE_EINVAL;
            }
        }

        try {
            err = stream->messageReceived(
                    std::make_unique<MutationResponse>(item,
                                                       opaque,
                                                       IncludeValue::Yes,
                                                       IncludeXattrs::Yes,
                                                       includeDeleteTime,
                                                       emd.release()));
        } catch (const std::bad_alloc&) {
            err = ENGINE_ENOMEM;
        }

        // The item was buffered and will be processed later
        if (err == ENGINE_TMPFAIL) {
            notifyVbucketReady(vbucket);
        }
    }

    return err;
}

ENGINE_ERROR_CODE DcpConsumer::expiration(uint32_t opaque,
                                          const DocKey& key,
                                          cb::const_byte_buffer value,
                                          size_t priv_bytes,
                                          uint8_t datatype,
                                          uint64_t cas,
                                          uint16_t vbucket,
                                          uint64_t by_seqno,
                                          uint64_t rev_seqno,
                                          cb::const_byte_buffer meta) {
    // lastMessageTime is set in deletion function
    return deletion(opaque, key, value, priv_bytes, datatype, cas, vbucket,
                    by_seqno, rev_seqno, meta);
}

ENGINE_ERROR_CODE DcpConsumer::snapshotMarker(uint32_t opaque,
                                              uint16_t vbucket,
                                              uint64_t start_seqno,
                                              uint64_t end_seqno,
                                              uint32_t flags) {
    lastMessageTime = ep_current_time();
    UpdateFlowControl ufc(*this, SnapshotMarker::baseMsgBytes);

    if (doDisconnect()) {
        return ENGINE_DISCONNECT;
    }

    if (start_seqno > end_seqno) {
        EP_LOG_WARN(
                "{} (vb:{}) Invalid snapshot marker "
                "received, snap_start ({}) <= snap_end ({})",
                logHeader(),
                vbucket,
                start_seqno,
                end_seqno);
        return ENGINE_EINVAL;
    }

    ENGINE_ERROR_CODE err = ENGINE_KEY_ENOENT;
    auto stream = findStream(vbucket);
    if (stream && stream->getOpaque() == opaque && stream->isActive()) {
        try {
            err = stream->messageReceived(std::make_unique<SnapshotMarker>(
                    opaque, vbucket, start_seqno, end_seqno, flags));

        } catch (const std::bad_alloc&) {
            return ENGINE_ENOMEM;
        }

        // The item was buffered and will be processed later
        if (err == ENGINE_TMPFAIL) {
            notifyVbucketReady(vbucket);
            ufc.release();
            return ENGINE_SUCCESS;
        }
    }

    return err;
}

ENGINE_ERROR_CODE DcpConsumer::noop(uint32_t opaque) {
    lastMessageTime = ep_current_time();
    return ENGINE_SUCCESS;
}

ENGINE_ERROR_CODE DcpConsumer::setVBucketState(uint32_t opaque,
                                               uint16_t vbucket,
                                               vbucket_state_t state) {
    lastMessageTime = ep_current_time();
    UpdateFlowControl ufc(*this, SetVBucketState::baseMsgBytes);
    if (doDisconnect()) {
        return ENGINE_DISCONNECT;
    }

    ENGINE_ERROR_CODE err = ENGINE_KEY_ENOENT;
    auto stream = findStream(vbucket);
    if (stream && stream->getOpaque() == opaque && stream->isActive()) {
        try {
            err = stream->messageReceived(
                    std::make_unique<SetVBucketState>(opaque, vbucket, state));
        } catch (const std::bad_alloc&) {
            return ENGINE_ENOMEM;
        }

        // The item was buffered and will be processed later
        if (err == ENGINE_TMPFAIL) {
            ufc.release();
            notifyVbucketReady(vbucket);
            return ENGINE_SUCCESS;
        }
    }

    return err;
}

ENGINE_ERROR_CODE DcpConsumer::step(struct dcp_message_producers* producers) {

    if (doDisconnect()) {
        return ENGINE_DISCONNECT;
    }

    ENGINE_ERROR_CODE ret;
    if ((ret = flowControl.handleFlowCtl(producers)) != ENGINE_FAILED) {
        return ret;
    }

    // MB-29441: Send a GetErrorMap to the producer to determine if it
    // is a pre-5.0.0 node. The consumer will set the producer's noop-interval
    // accordingly in 'handleNoop()', so 'handleGetErrorMap()' *must* execute
    // before 'handleNoop()'.
    if ((ret = handleGetErrorMap(producers)) != ENGINE_FAILED) {
        return ret;
    }

    if ((ret = handleNoop(producers)) != ENGINE_FAILED) {
        return ret;
    }

    if ((ret = handlePriority(producers)) != ENGINE_FAILED) {
        return ret;
    }

    if ((ret = handleExtMetaData(producers)) != ENGINE_FAILED) {
        return ret;
    }

    if ((ret = supportCursorDropping(producers)) != ENGINE_FAILED) {
        return ret;
    }

    if ((ret = supportHifiMFU(producers)) != ENGINE_FAILED) {
        return ret;
    }

    if ((ret = sendStreamEndOnClientStreamClose(producers)) != ENGINE_FAILED) {
        return ret;
    }

    auto resp = getNextItem();
    if (resp == nullptr) {
        return ENGINE_EWOULDBLOCK;
    }

    EventuallyPersistentEngine *epe = ObjectRegistry::onSwitchThread(NULL, true);
    switch (resp->getEvent()) {
        case DcpResponse::Event::AddStream:
        {
            AddStreamResponse* as = static_cast<AddStreamResponse*>(resp.get());
            ret = producers->add_stream_rsp(
                    as->getOpaque(), as->getStreamOpaque(), as->getStatus());
            break;
        }
        case DcpResponse::Event::StreamReq:
        {
            StreamRequest* sr = static_cast<StreamRequest*>(resp.get());
            ret = producers->stream_req(sr->getOpaque(),
                                        sr->getVBucket(),
                                        sr->getFlags(),
                                        sr->getStartSeqno(),
                                        sr->getEndSeqno(),
                                        sr->getVBucketUUID(),
                                        sr->getSnapStartSeqno(),
                                        sr->getSnapEndSeqno());
            break;
        }
        case DcpResponse::Event::SetVbucket:
        {
            SetVBucketStateResponse* vs =
                    static_cast<SetVBucketStateResponse*>(resp.get());
            ret = producers->set_vbucket_state_rsp(vs->getOpaque(),
                                                   vs->getStatus());
            break;
        }
        case DcpResponse::Event::SnapshotMarker:
        {
            SnapshotMarkerResponse* mr =
                    static_cast<SnapshotMarkerResponse*>(resp.get());
            ret = producers->marker_rsp(mr->getOpaque(), mr->getStatus());
            break;
        }
        default:
            EP_LOG_WARN(
                    "{} Unknown consumer event ({}), "
                    "disconnecting",
                    logHeader(),
                    int(resp->getEvent()));
            ret = ENGINE_DISCONNECT;
    }
    ObjectRegistry::onSwitchThread(epe);

    return ret;
}

bool RollbackTask::run() {
    TRACE_EVENT0("ep-engine/task", "RollbackTask");
    if (cons->doDisconnect()) {
        return false;
    }
    if (cons->doRollback(opaque, vbid, rollbackSeqno)) {
        return true;
    }
    ++(engine->getEpStats().rollbackCount);
    return false;
}

bool DcpConsumer::handleResponse(const protocol_binary_response_header* resp) {
    if (doDisconnect()) {
        return false;
    }

    uint8_t opcode = resp->response.opcode;
    uint32_t opaque = resp->response.opaque;

    if (opcode == PROTOCOL_BINARY_CMD_DCP_STREAM_REQ) {
        opaque_map::iterator oitr = opaqueMap_.find(opaque);
        if (oitr == opaqueMap_.end()) {
            EP_LOG_WARN(
                    "Received response with opaque {} and that opaque "
                    "does not exist in opaqueMap",
                    opaque);
            return false;
        } else if (!isValidOpaque(opaque, oitr->second.second)) {
            EP_LOG_WARN(
                    "Received response with opaque {} and that stream does not "
                    "exist for vb:{}",
                    opaque,
                    oitr->second.second);
            return false;
        }
        const auto* pkt = reinterpret_cast<
                const protocol_binary_response_dcp_stream_req*>(resp);

        uint16_t vbid = oitr->second.second;
        uint16_t status = ntohs(pkt->message.header.response.status);
        uint64_t bodylen = pkt->message.header.response.getBodylen();
        const uint8_t* body =
                pkt->bytes + sizeof(protocol_binary_response_header);

        if (status == PROTOCOL_BINARY_RESPONSE_ROLLBACK) {
            if (bodylen != sizeof(uint64_t)) {
                EP_LOG_WARN(
                        "{} (vb:{}) Received rollback "
                        "request with incorrect bodylen of {}, disconnecting",
                        logHeader(),
                        vbid,
                        bodylen);
                return false;
            }
            uint64_t rollbackSeqno = 0;
            memcpy(&rollbackSeqno, body, sizeof(uint64_t));
            rollbackSeqno = ntohll(rollbackSeqno);
            return handleRollbackResponse(vbid, opaque, rollbackSeqno);
        }

        if (((bodylen % 16) != 0 || bodylen == 0) && status == ENGINE_SUCCESS) {
            EP_LOG_WARN(
                    "{} (vb:{})Got a stream response with a "
                    "bad failover log (length {}), disconnecting",
                    logHeader(),
                    vbid,
                    bodylen);
            return false;
        }

        streamAccepted(opaque, status, body, bodylen);
        return true;
    } else if (opcode == PROTOCOL_BINARY_CMD_DCP_BUFFER_ACKNOWLEDGEMENT ||
               opcode == PROTOCOL_BINARY_CMD_DCP_CONTROL) {
        return true;
    } else if (opcode == PROTOCOL_BINARY_CMD_GET_ERROR_MAP) {
        uint16_t status = ntohs(resp->response.status);
        // GetErrorMap is supported on versions >= 5.0.0.
        // "Unknown Command" is returned on pre-5.0.0 versions.
        producerIsVersion5orHigher =
                status != PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND;
        getErrorMapState = GetErrorMapState::Skip;
        return true;
    }

    EP_LOG_WARN(
            "{} Trying to handle an unknown response {}, "
            "disconnecting",
            logHeader(),
            opcode);

    return false;
}

bool DcpConsumer::handleRollbackResponse(uint16_t vbid,
                                         uint32_t opaque,
                                         uint64_t rollbackSeqno) {
    auto vb = engine_.getVBucket(vbid);
    auto stream = findStream(vbid);

    if (!(vb && stream)) {
        EP_LOG_WARN("{} (vb:{}) handleRollbackResponse: vb:{}, stream:{}",
                    logHeader(),
                    vbid,
                    vb.get() ? "ok" : "nullptr",
                    stream.get() ? "ok" : "nullptr");
        return false;
    }

    auto entries = vb->failovers->getNumEntries();
    if (rollbackSeqno == 0 && entries > 1) {
        EP_LOG_INFO(
                "{} (vb:{}) Received rollback request. Rollback to 0 yet have "
                "{} "
                "entries remaining. Retrying with previous failover entry",
                logHeader(),
                vbid,
                entries);
        vb->failovers->removeLatestEntry();

        stream->streamRequest(vb->failovers->getLatestEntry().vb_uuid);
    } else {
        EP_LOG_INFO(
                "{} (vb:{}) Received rollback request. Rolling back to "
                "seqno:{}",
                logHeader(),
                vbid,
                rollbackSeqno);
        ExTask task = std::make_shared<RollbackTask>(
                &engine_, opaque, vbid, rollbackSeqno, shared_from_this());
        ExecutorPool::get()->schedule(task);
    }
    return true;
}

bool DcpConsumer::doRollback(uint32_t opaque,
                             uint16_t vbid,
                             uint64_t rollbackSeqno) {
    TaskStatus status = engine_.getKVBucket()->rollback(vbid, rollbackSeqno);

    switch (status) {
    case TaskStatus::Reschedule:
        return true; // Reschedule the rollback.
    case TaskStatus::Abort:
        logger.log(EXTENSION_LOG_WARNING,
                   "vb:%" PRIu16 " Rollback failed on the vbucket",
                   vbid);
        break;
    case TaskStatus::Complete: {
        VBucketPtr vb = engine_.getVBucket(vbid);
        if (!vb) {
            logger.log(EXTENSION_LOG_WARNING,
                       "vb:%" PRIu16
                       " Aborting rollback task as the vbucket "
                       "was deleted after rollback",
                       vbid);
            break;
        }
        auto stream = findStream(vbid);
        if (stream) {
            stream->reconnectStream(vb, opaque, vb->getHighSeqno());
        }
        break;
    }
    }
    return false; // Do not reschedule the rollback
}

void DcpConsumer::addStats(ADD_STAT add_stat, const void *c) {
    ConnHandler::addStats(add_stat, c);

    // Make a copy of all valid streams (under lock), and then call addStats
    // for each one. (Done in two stages to minmise how long we have the
    // streams map locked for).
    std::vector<PassiveStreamMap::mapped_type> valid_streams;

    streams.for_each(
        [&valid_streams](const PassiveStreamMap::value_type& element) {
            valid_streams.push_back(element.second);
        }
    );
    for (const auto& stream : valid_streams) {
        stream->addStats(add_stat, c);
    }

    addStat("total_backoffs", backoffs, add_stat, c);
    addStat("processor_task_state", getProcessorTaskStatusStr(), add_stat, c);
    flowControl.addStats(add_stat, c);

    vbReady.addStats(getName() + ":dcp_buffered_ready_queue_", add_stat, c);
    addStat("processor_notification",
            processorNotification.load(),
            add_stat,
            c);
}

void DcpConsumer::aggregateQueueStats(ConnCounter& aggregator) {
    aggregator.conn_queueBackoff += backoffs;
}

process_items_error_t DcpConsumer::drainStreamsBufferedItems(
        std::shared_ptr<PassiveStream> stream, size_t yieldThreshold) {
    process_items_error_t rval = all_processed;
    uint32_t bytesProcessed = 0;
    size_t iterations = 0;
    do {
        switch (engine_.getReplicationThrottle().getStatus()) {
        case ReplicationThrottle::Status::Pause:
            backoffs++;
            vbReady.pushUnique(stream->getVBucket());
            return cannot_process;

        case ReplicationThrottle::Status::Disconnect:
            backoffs++;
            vbReady.pushUnique(stream->getVBucket());
            logger.log(EXTENSION_LOG_WARNING,
                       "vb:%" PRIu16
                       " Processor task indicating disconnection as "
                       "there is no memory to complete replication",
                       stream->getVBucket());
            return stop_processing;

        case ReplicationThrottle::Status::Process:
            bytesProcessed = 0;
            rval = stream->processBufferedMessages(
                    bytesProcessed, processBufferedMessagesBatchSize);
            if ((rval == cannot_process) || (rval == stop_processing)) {
                backoffs++;
            }
            flowControl.incrFreedBytes(bytesProcessed);

            // Notifying memcached on clearing items for flow control
            notifyConsumerIfNecessary(false /*schedule*/);

            iterations++;
            break;
        }
    } while (bytesProcessed > 0 &&
             rval == all_processed &&
             iterations <= yieldThreshold);

    // The stream may not be done yet so must go back in the ready queue
    if (bytesProcessed > 0) {
        vbReady.pushUnique(stream->getVBucket());
        if (rval == stop_processing) {
            return stop_processing;
        }
        rval = more_to_process; // Return more_to_process to force a snooze(0.0)
    }

    return rval;
}

process_items_error_t DcpConsumer::processBufferedItems() {
    process_items_error_t process_ret = all_processed;
    uint16_t vbucket = 0;
    while (vbReady.popFront(vbucket)) {
        auto stream = findStream(vbucket);

        if (!stream) {
            continue;
        }

        process_ret = drainStreamsBufferedItems(stream,
                                                processBufferedMessagesYieldThreshold);

        switch (process_ret) {
        case all_processed:
            return more_to_process;
        case cannot_process:
            // If items for current vbucket weren't processed,
            // re-add current vbucket
            if (vbReady.size() > 0) {
                // If there are more vbuckets in queue, sleep(0).
                process_ret = more_to_process;
            }
            vbReady.pushUnique(vbucket);
            return process_ret;
        case more_to_process:
            return process_ret;
        case stop_processing:
            setDisconnect();
            return process_ret;
        }
    }
    return process_ret;
}

void DcpConsumer::notifyVbucketReady(uint16_t vbucket) {
    if (vbReady.pushUnique(vbucket) &&
        notifiedProcessor(true)) {
        ExecutorPool::get()->wake(processorTaskId);
    }
}

bool DcpConsumer::notifiedProcessor(bool to) {
    bool inverse = !to;
    return processorNotification.compare_exchange_strong(inverse, to);
}

void DcpConsumer::setProcessorTaskState(enum process_items_error_t to) {
    processorTaskState = to;
}

std::string DcpConsumer::getProcessorTaskStatusStr() {
    switch (processorTaskState.load()) {
        case all_processed:
            return "ALL_PROCESSED";
        case more_to_process:
            return "MORE_TO_PROCESS";
        case cannot_process:
            return "CANNOT_PROCESS";
        case stop_processing:
            return "STOP_PROCESSING";
    }

    return "UNKNOWN";
}

std::unique_ptr<DcpResponse> DcpConsumer::getNextItem() {
    LockHolder lh(readyMutex);

    unPause();
    while (!ready.empty()) {
        uint16_t vbucket = ready.front();
        ready.pop_front();

        auto stream = findStream(vbucket);
        if (!stream) {
            continue;
        }

        auto response = stream->next();
        if (!response) {
            continue;
        }
        switch (response->getEvent()) {
        case DcpResponse::Event::StreamReq:
        case DcpResponse::Event::AddStream:
        case DcpResponse::Event::SetVbucket:
        case DcpResponse::Event::SnapshotMarker:
            break;
        default:
            throw std::logic_error(
                    std::string("DcpConsumer::getNextItem: ") + logHeader() +
                    " is attempting to write an unexpected event: " +
                    response->to_string());
        }

        ready.push_back(vbucket);
        return response;
    }
    pause("ready list empty");

    return nullptr;
}

void DcpConsumer::notifyStreamReady(uint16_t vbucket) {
    {
        std::lock_guard<std::mutex> lh(readyMutex);
        auto iter = std::find(ready.begin(), ready.end(), vbucket);
        if (iter != ready.end()) {
            return;
        }

        ready.push_back(vbucket);
    }


    notifyPaused(/*schedule*/true);
}

void DcpConsumer::streamAccepted(uint32_t opaque,
                                 uint16_t status,
                                 const uint8_t* body,
                                 uint32_t bodylen) {
    opaque_map::iterator oitr = opaqueMap_.find(opaque);
    if (oitr != opaqueMap_.end()) {
        uint32_t add_opaque = oitr->second.first;
        uint16_t vbucket = oitr->second.second;

        auto stream = findStream(vbucket);
        if (stream && stream->getOpaque() == opaque && stream->isPending()) {
            if (status == ENGINE_SUCCESS) {
                VBucketPtr vb = engine_.getVBucket(vbucket);
                vb->failovers->replaceFailoverLog(body, bodylen);
                KVBucketIface* kvBucket = engine_.getKVBucket();
                kvBucket->scheduleVBStatePersist(vbucket);
            }
            EP_LOG_DEBUG(
                    "{} (vb:{}) Add stream for opaque {} {} with error code {}",
                    logHeader(),
                    vbucket,
                    opaque,
                    status == ENGINE_SUCCESS ? "succeeded" : "failed",
                    status);
            stream->acceptStream(status, add_opaque);
        } else {
            EP_LOG_WARN(
                    "{} (vb:{}) Trying to add stream, but "
                    "none exists (opaque: {}, add_opaque: {})",
                    logHeader(),
                    vbucket,
                    opaque,
                    add_opaque);
        }
        opaqueMap_.erase(opaque);
    } else {
        EP_LOG_WARN(
                "{} No opaque found for add stream response "
                "with opaque {}",
                logHeader(),
                opaque);
    }
}

bool DcpConsumer::isValidOpaque(uint32_t opaque, uint16_t vbucket) {
    auto stream = findStream(vbucket);
    return stream && stream->getOpaque() == opaque;
}

void DcpConsumer::closeAllStreams() {

    // Need to synchronise the disconnect and clear, therefore use
    // external locking here.
    std::lock_guard<PassiveStreamMap> guard(streams);

    streams.for_each(
        [](PassiveStreamMap::value_type& iter) {
            iter.second->setDead(END_STREAM_DISCONNECTED);
        },
        guard);
    streams.clear(guard);
}

void DcpConsumer::closeStreamDueToVbStateChange(uint16_t vbucket,
                                                vbucket_state_t state) {
    auto it = streams.erase(vbucket);
    if (it.second) {
        EP_LOG_DEBUG(
                "{} (vb:{}) State changed to "
                "{}, closing passive stream!",
                logHeader(),
                vbucket,
                VBucket::toString(state));
        auto& stream = it.first;
        uint32_t bytesCleared = stream->setDead(END_STREAM_STATE);
        flowControl.incrFreedBytes(bytesCleared);
        notifyConsumerIfNecessary(true/*schedule*/);
    }
}

ENGINE_ERROR_CODE DcpConsumer::handleNoop(struct dcp_message_producers* producers) {
    if (pendingEnableNoop) {
        ENGINE_ERROR_CODE ret;
        uint32_t opaque = ++opaqueCounter;
        std::string val("true");
        EventuallyPersistentEngine *epe = ObjectRegistry::onSwitchThread(NULL, true);
        ret = producers->control(opaque,
                                 noopCtrlMsg.c_str(),
                                 noopCtrlMsg.size(),
                                 val.c_str(),
                                 val.size());
        ObjectRegistry::onSwitchThread(epe);
        pendingEnableNoop = false;
        return ret;
    }

    if (pendingSendNoopInterval) {
        ENGINE_ERROR_CODE ret;
        uint32_t opaque = ++opaqueCounter;

        // MB-29441: Set the noop-interval on the producer:
        //     - dcpNoopTxInterval, if the producer is a >=5.0.0 node
        //     - 180 seconds, if the producer is a pre-5.0.0 node
        //         (this is the expected value on a pre-5.0.0 producer)
        auto intervalCount =
                producerIsVersion5orHigher ? dcpNoopTxInterval.count() : 180;
        std::string interval = std::to_string(intervalCount);
        EventuallyPersistentEngine *epe = ObjectRegistry::onSwitchThread(NULL, true);
        ret = producers->control(opaque,
                                 noopIntervalCtrlMsg.c_str(),
                                 noopIntervalCtrlMsg.size(),
                                 interval.c_str(),
                                 interval.size());
        ObjectRegistry::onSwitchThread(epe);
        pendingSendNoopInterval = false;
        return ret;
    }

    const auto now = ep_current_time();
    if ((now - lastMessageTime) > dcpIdleTimeout.count()) {
        EP_LOG_INFO(
                "{} Disconnecting because a message has not been received for "
                "{}s. lastMessageTime:{}",
                logHeader(),
                uint64_t(dcpIdleTimeout.count()),
                (now - lastMessageTime));
        return ENGINE_DISCONNECT;
    }

    return ENGINE_FAILED;
}

ENGINE_ERROR_CODE DcpConsumer::handleGetErrorMap(
        struct dcp_message_producers* producers) {
    if (getErrorMapState == GetErrorMapState::PendingRequest) {
        ENGINE_ERROR_CODE ret;
        uint32_t opaque = ++opaqueCounter;
        EventuallyPersistentEngine* epe =
                ObjectRegistry::onSwitchThread(NULL, true);
        // Note: just send 0 as version to get the default error map loaded
        //     from file at startup. The error map returned is not used, we
        //     just want to issue a valid request.
        ret = producers->get_error_map(opaque, 0 /*version*/);
        ObjectRegistry::onSwitchThread(epe);
        getErrorMapState = GetErrorMapState::PendingResponse;
        return ret;
    }

    // We have to wait for the GetErrorMap response before proceeding
    if (getErrorMapState == GetErrorMapState::PendingResponse) {
        return ENGINE_EWOULDBLOCK;
    }

    return ENGINE_FAILED;
}

ENGINE_ERROR_CODE DcpConsumer::handlePriority(struct dcp_message_producers* producers) {
    if (pendingSetPriority) {
        ENGINE_ERROR_CODE ret;
        uint32_t opaque = ++opaqueCounter;
        std::string val("high");
        EventuallyPersistentEngine *epe = ObjectRegistry::onSwitchThread(NULL, true);
        ret = producers->control(opaque,
                                 priorityCtrlMsg.c_str(),
                                 priorityCtrlMsg.size(),
                                 val.c_str(),
                                 val.size());
        ObjectRegistry::onSwitchThread(epe);
        pendingSetPriority = false;
        return ret;
    }

    return ENGINE_FAILED;
}

ENGINE_ERROR_CODE DcpConsumer::handleExtMetaData(struct dcp_message_producers* producers) {
    if (pendingEnableExtMetaData) {
        ENGINE_ERROR_CODE ret;
        uint32_t opaque = ++opaqueCounter;
        std::string val("true");
        EventuallyPersistentEngine *epe = ObjectRegistry::onSwitchThread(NULL, true);
        ret = producers->control(opaque,
                                 extMetadataCtrlMsg.c_str(),
                                 extMetadataCtrlMsg.size(),
                                 val.c_str(),
                                 val.size());
        ObjectRegistry::onSwitchThread(epe);
        pendingEnableExtMetaData = false;
        return ret;
    }

    return ENGINE_FAILED;
}

ENGINE_ERROR_CODE DcpConsumer::supportCursorDropping(struct dcp_message_producers* producers) {
    if (pendingSupportCursorDropping) {
        ENGINE_ERROR_CODE ret;
        uint32_t opaque = ++opaqueCounter;
        std::string val("true");
        EventuallyPersistentEngine *epe = ObjectRegistry::onSwitchThread(NULL, true);
        ret = producers->control(opaque,
                                 cursorDroppingCtrlMsg.c_str(),
                                 cursorDroppingCtrlMsg.size(),
                                 val.c_str(),
                                 val.size());
        ObjectRegistry::onSwitchThread(epe);
        pendingSupportCursorDropping = false;
        return ret;
    }

    return ENGINE_FAILED;
}

ENGINE_ERROR_CODE DcpConsumer::supportHifiMFU(
        struct dcp_message_producers* producers) {
    if (pendingSupportHifiMFU) {
        ENGINE_ERROR_CODE ret;
        uint32_t opaque = ++opaqueCounter;
        std::string val("true");
        EventuallyPersistentEngine* epe =
                ObjectRegistry::onSwitchThread(NULL, true);
        ret = producers->control(opaque,
                                 hifiMFUCtrlMsg.c_str(),
                                 hifiMFUCtrlMsg.size(),
                                 val.c_str(),
                                 val.size());
        ObjectRegistry::onSwitchThread(epe);
        pendingSupportHifiMFU = false;
        return ret;
    }

    return ENGINE_FAILED;
}

ENGINE_ERROR_CODE DcpConsumer::sendStreamEndOnClientStreamClose(
        struct dcp_message_producers* producers) {
    /* Sending this ctrl message tells the DCP producer that the consumer is
       expecting a "STREAM_END" message when it initiates a stream close */
    if (pendingSendStreamEndOnClientStreamClose) {
        uint32_t opaque = ++opaqueCounter;
        std::string val("true");
        EventuallyPersistentEngine* epe =
                ObjectRegistry::onSwitchThread(nullptr, true);
        ENGINE_ERROR_CODE ret = producers->control(
                opaque,
                sendStreamEndOnClientStreamCloseCtrlMsg.c_str(),
                sendStreamEndOnClientStreamCloseCtrlMsg.size(),
                val.c_str(),
                val.size());
        ObjectRegistry::onSwitchThread(epe);
        pendingSendStreamEndOnClientStreamClose = false;
        return ret;
    }
    return ENGINE_FAILED;
}

uint64_t DcpConsumer::incrOpaqueCounter()
{
    return (++opaqueCounter);
}

uint32_t DcpConsumer::getFlowControlBufSize()
{
    return flowControl.getFlowControlBufSize();
}

void DcpConsumer::setFlowControlBufSize(uint32_t newSize)
{
    flowControl.setFlowControlBufSize(newSize);
}

const std::string& DcpConsumer::getControlMsgKey(void)
{
    return connBufferCtrlMsg;
}

bool DcpConsumer::isStreamPresent(uint16_t vbucket)
{
    auto stream = findStream(vbucket);
    return stream && stream->isActive();
}

void DcpConsumer::notifyConsumerIfNecessary(bool schedule) {
    if (flowControl.isBufferSufficientlyDrained()) {
        /**
         * Notify memcached to get flow control buffer ack out.
         * We cannot wait till the ConnManager daemon task notifies
         * the memcached as it would cause delay in buffer ack being
         * sent out to the producer.
         */
        notifyPaused(schedule);
    }
}

std::shared_ptr<PassiveStream> DcpConsumer::findStream(uint16_t vbid) {
    auto it = streams.find(vbid);
    if (it.second) {
        return it.first;
    } else {
        return nullptr;
    }
}

void DcpConsumer::notifyPaused(bool schedule) {
    engine_.getDcpConnMap().notifyPausedConnection(shared_from_this(),
                                                   schedule);
}

ENGINE_ERROR_CODE DcpConsumer::systemEvent(uint32_t opaque,
                                           uint16_t vbucket,
                                           mcbp::systemevent::id event,
                                           uint64_t bySeqno,
                                           cb::const_byte_buffer key,
                                           cb::const_byte_buffer eventData) {
    lastMessageTime = ep_current_time();

    ENGINE_ERROR_CODE err = ENGINE_KEY_ENOENT;
    auto stream = findStream(vbucket);
    if (stream && stream->getOpaque() == opaque && stream->isActive()) {
        try {
            err = stream->messageReceived(
                    std::make_unique<SystemEventConsumerMessage>(
                            opaque,
                            event,
                            bySeqno,
                            vbucket,
                            key,
                            eventData));
        } catch (const std::bad_alloc&) {
            return ENGINE_ENOMEM;
        }

        // The item was buffered and will be processed later
        if (err == ENGINE_TMPFAIL) {
            notifyVbucketReady(vbucket);
            return ENGINE_SUCCESS;
        }
    }

    flowControl.incrFreedBytes(SystemEventMessage::baseMsgBytes + key.size() +
                               eventData.size());
    notifyConsumerIfNecessary(true /*schedule*/);

    return err;
}

void DcpConsumer::setDisconnect() {
    ConnHandler::setDisconnect();

    closeAllStreams();

    notifyPaused(/*schedule*/ true);
}
