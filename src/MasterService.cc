/* Copyright (c) 2009-2012 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <unordered_map>
#include <unordered_set>

#include "Buffer.h"
#include "ClientException.h"
#include "Cycles.h"
#include "Dispatch.h"
#include "Enumeration.h"
#include "EnumerationIterator.h"
#include "LogIterator.h"
#include "ShortMacros.h"
#include "MasterClient.h"
#include "MasterService.h"
#include "RawMetrics.h"
#include "Tub.h"
#include "ProtoBuf.h"
#include "Segment.h"
#include "ServiceManager.h"
#include "Transport.h"
#include "WallTime.h"

namespace RAMCloud {

/// An instance of RejectRules that does not reject anything. 
static RejectRules defaultRejectRules;

// struct MasterService::Replica

/**
 * Constructor.
 * \param backupId
 *      See #backupId member.
 * \param segmentId
 *      See #segmentId member.
 * \param state
 *      See #state member. The default (NOT_STARTED) is usually what you want
 *      here, but other values are allowed for testing.
 */
MasterService::Replica::Replica(uint64_t backupId, uint64_t segmentId,
                                State state)
    : backupId(backupId)
    , segmentId(segmentId)
    , state(state)
{
}

// --- MasterService ---

/**
 * Construct a MasterService.
 *
 * \param context
 *      Overall information about the RAMCloud server or client.
 * \param config
 *      Contains various parameters that configure the operation of
 *      this server.
 */
MasterService::MasterService(Context* context,
                             const ServerConfig* config)
    : context(context)
    , config(config)
    , objectManager()
    , initCalled(false)
    , maxMultiReadResponseSize(Transport::MAX_RPC_LEN)
    , disableCount(0)
{
}

MasterService::~MasterService()
{
}

void
MasterService::dispatch(WireFormat::Opcode opcode, Rpc* rpc)
{
    assert(initCalled);

    if (disableCount  > 0) {
        LOG(NOTICE, "requesting retry of %s request (master disable count %d)",
                WireFormat::opcodeSymbol(opcode),
                disableCount.load());
        prepareErrorResponse(rpc->replyPayload, STATUS_RETRY);
        return;
    }

    switch (opcode) {
        case WireFormat::DropTabletOwnership::opcode:
            callHandler<WireFormat::DropTabletOwnership, MasterService,
                        &MasterService::dropTabletOwnership>(rpc);
            break;
        case WireFormat::Enumerate::opcode:
            callHandler<WireFormat::Enumerate, MasterService,
                        &MasterService::enumerate>(rpc);
            break;
        case WireFormat::FillWithTestData::opcode:
            callHandler<WireFormat::FillWithTestData, MasterService,
                        &MasterService::fillWithTestData>(rpc);
            break;
        case WireFormat::GetLogMetrics::opcode:
            callHandler<WireFormat::GetLogMetrics, MasterService,
                        &MasterService::getLogMetrics>(rpc);
            break;
        case WireFormat::Increment::opcode:
            callHandler<WireFormat::Increment, MasterService,
                        &MasterService::increment>(rpc);
            break;
        case WireFormat::IsReplicaNeeded::opcode:
            callHandler<WireFormat::IsReplicaNeeded, MasterService,
                        &MasterService::isReplicaNeeded>(rpc);
            break;
        case WireFormat::GetServerStatistics::opcode:
            callHandler<WireFormat::GetServerStatistics, MasterService,
                        &MasterService::getServerStatistics>(rpc);
            break;
        case WireFormat::GetHeadOfLog::opcode:
            callHandler<WireFormat::GetHeadOfLog, MasterService,
                        &MasterService::getHeadOfLog>(rpc);
            break;
        case WireFormat::MigrateTablet::opcode:
            callHandler<WireFormat::MigrateTablet, MasterService,
                        &MasterService::migrateTablet>(rpc);
            break;
        case WireFormat::MultiRead::opcode:
            callHandler<WireFormat::MultiRead, MasterService,
                        &MasterService::multiRead>(rpc);
            break;
        case WireFormat::MultiWrite::opcode:
            callHandler<WireFormat::MultiWrite, MasterService,
                        &MasterService::multiWrite>(rpc);
            break;
        case WireFormat::PrepForMigration::opcode:
            callHandler<WireFormat::PrepForMigration, MasterService,
                        &MasterService::prepForMigration>(rpc);
            break;
        case WireFormat::Read::opcode:
            callHandler<WireFormat::Read, MasterService,
                        &MasterService::read>(rpc);
            break;
        case WireFormat::ReceiveMigrationData::opcode:
            callHandler<WireFormat::ReceiveMigrationData, MasterService,
                        &MasterService::receiveMigrationData>(rpc);
            break;
        case WireFormat::Recover::opcode:
            callHandler<WireFormat::Recover, MasterService,
                        &MasterService::recover>(rpc);
            break;
        case WireFormat::Remove::opcode:
            callHandler<WireFormat::Remove, MasterService,
                        &MasterService::remove>(rpc);
            break;
        case WireFormat::SplitMasterTablet::opcode:
            callHandler<WireFormat::SplitMasterTablet, MasterService,
                        &MasterService::splitMasterTablet>(rpc);
            break;
        case WireFormat::TakeTabletOwnership::opcode:
            callHandler<WireFormat::TakeTabletOwnership, MasterService,
                        &MasterService::takeTabletOwnership>(rpc);
            break;
        case WireFormat::Write::opcode:
            callHandler<WireFormat::Write, MasterService,
                        &MasterService::write>(rpc);
            break;
        default:
            prepareErrorResponse(rpc->replyPayload,
                                 STATUS_UNIMPLEMENTED_REQUEST);
    }
}


/**
 * Perform once-only initialization for the master service after having
 * enlisted the process with the coordinator.
 */
void
MasterService::init(ServerId id)
{
    assert(!initCalled);

    serverId = id;
    LOG(NOTICE, "My server ID is %s", serverId.toString().c_str());
    metrics->serverId = serverId.getId();
    objectManager.construct(context, serverId, config);
    initCalled = true;
}

/**
 * Top-level server method to handle the ENUMERATE request.
 *
 * \copydetails Service::ping
 */
void
MasterService::enumerate(const WireFormat::Enumerate::Request* reqHdr,
                           WireFormat::Enumerate::Response* respHdr,
                           Rpc* rpc)
{
    bool validTablet = false;
    // In some cases, actualTabletStartHash may differ from
    // reqHdr->tabletFirstHash, e.g. when a tablet is merged in between
    // RPCs made to enumerate that tablet. If that happens, we must
    // filter by reqHdr->tabletFirstHash, NOT the actualTabletStartHash
    // for the tablet we own.
    uint64_t actualTabletStartHash = 0, actualTabletEndHash = 0;
    ProtoBuf::Tablets* tablets = &objectManager->tablets;
    foreach (auto& tablet, tablets->tablet()) {
        if (tablet.table_id() == reqHdr->tableId &&
            tablet.start_key_hash() <= reqHdr->tabletFirstHash &&
            reqHdr->tabletFirstHash <= tablet.end_key_hash()) {
            validTablet = true;
            actualTabletStartHash = tablet.start_key_hash();
            actualTabletEndHash = tablet.end_key_hash();
            break;
        }
    }
    if (!validTablet) {
        respHdr->common.status = STATUS_UNKNOWN_TABLET;
        return;
    }

    EnumerationIterator iter(
        *rpc->requestPayload,
        downCast<uint32_t>(sizeof(*reqHdr)), reqHdr->iteratorBytes);

    Buffer payload;
    // A rough upper bound on how much space will be available in the response.
    uint32_t maxPayloadBytes =
            downCast<uint32_t>(Transport::MAX_RPC_LEN - sizeof(*respHdr)
                               - reqHdr->iteratorBytes);
    Enumeration enumeration(reqHdr->tableId, reqHdr->tabletFirstHash,
                            actualTabletStartHash, actualTabletEndHash,
                            &respHdr->tabletFirstHash, iter,
                            objectManager->log, objectManager->objectMap,
                            *rpc->replyPayload, maxPayloadBytes);
    enumeration.complete();
    respHdr->payloadBytes = rpc->replyPayload->getTotalLength()
            - downCast<uint32_t>(sizeof(*respHdr));

    // Add new iterator to the end of the response.
    uint32_t iteratorBytes = iter.serialize(*rpc->replyPayload);
    respHdr->iteratorBytes = iteratorBytes;
}

/**
 * Obtain various metrics from the log and return to the caller. Used to
 * remotely monitor the log's utilization and performance.
 *
 * \copydetails Service::ping
 */
void
MasterService::getLogMetrics(
    const WireFormat::GetLogMetrics::Request* reqHdr,
    WireFormat::GetLogMetrics::Response* respHdr,
    Rpc* rpc)
{
    ProtoBuf::LogMetrics logMetrics;
    objectManager->log.getMetrics(logMetrics);
    respHdr->logMetricsLength = ProtoBuf::serializeToResponse(rpc->replyPayload,
                                                             &logMetrics);
}

/**
 * Fill a master server with the given number of objects, each of the
 * same given size. Objects are added to all tables in the master in
 * a round-robin fashion. This method exists simply to quickly fill a
 * master for experiments.
 *
 * See MasterClient::fillWithTestData() for more information.
 *
 * \bug Will return an error if the master only owns part of a table
 * (because the hash of the fabricated keys may land in a region it
 * doesn't own).
 *
 * \copydetails Service::ping
 */
void
MasterService::fillWithTestData(
    const WireFormat::FillWithTestData::Request* reqHdr,
    WireFormat::FillWithTestData::Response* respHdr,
    Rpc* rpc)
{
    ProtoBuf::Tablets* tablets = &objectManager->tablets;

    LOG(NOTICE, "Filling with %u objects of %u bytes each in %u tablets",
        reqHdr->numObjects, reqHdr->objectSize, tablets->tablet_size());

    TabletsOnMaster* tables[tablets->tablet_size()];
    uint32_t numTablets = 0;
    foreach (const ProtoBuf::Tablets::Tablet& tablet, tablets->tablet()) {
        // Only use tables where we store the entire table here.
        // The key calculation is not safe otherwise.
        if ((tablet.start_key_hash() == 0)
                && (tablet.end_key_hash() == ~0LU)) {
            tables[numTablets++] =
                    reinterpret_cast<TabletsOnMaster*>(tablet.user_data());
        }
    }
    if (numTablets == 0)
        throw ObjectDoesntExistException(HERE);

    RejectRules rejectRules;
    memset(&rejectRules, 0, sizeof(RejectRules));
    rejectRules.exists = 1;

    for (uint32_t objects = 0; objects < reqHdr->numObjects; objects++) {
        Buffer buffer;

        int t = objects % numTablets;

        // safe? doubtful. simple? you bet.
        uint8_t data[reqHdr->objectSize];
        memset(data, 0xcc, reqHdr->objectSize);
        buffer.append(data, reqHdr->objectSize);

        string keyString = format("%d", objects / numTablets);
        Key key(tables[t]->getId(),
                keyString.c_str(),
                downCast<uint16_t>(keyString.length()));

        uint64_t newVersion;
        Status status = objectManager->writeObject(key,
                                                   buffer,
                                                   rejectRules,
                                                   &newVersion);
        if (status != STATUS_OK) {
            respHdr->common.status = status;
            return;
        }
        if ((objects % 50) == 0) {
            objectManager->replicaManager.proceed();
        }
    }

    objectManager->syncWrites();

    LOG(NOTICE, "Done writing objects.");
}

/**
 * Top-level server method to handle the GET_HEAD_OF_LOG request.
 */
void
MasterService::getHeadOfLog(const WireFormat::GetHeadOfLog::Request* reqHdr,
                            WireFormat::GetHeadOfLog::Response* respHdr,
                            Rpc* rpc)
{
    Log::Position head = objectManager->log.rollHeadOver();
    respHdr->headSegmentId = head.getSegmentId();
    respHdr->headSegmentOffset = head.getSegmentOffset();
}

/**
 * Top-level server method to handle the GET_SERVER_STATISTICS request.
 */
void
MasterService::getServerStatistics(
    const WireFormat::GetServerStatistics::Request* reqHdr,
    WireFormat::GetServerStatistics::Response* respHdr,
    Rpc* rpc)
{
    ProtoBuf::ServerStatistics serverStats;

    ProtoBuf::Tablets* tablets = &objectManager->tablets;
    foreach (const ProtoBuf::Tablets::Tablet& i, tablets->tablet()) {
        TabletsOnMaster* table =
                reinterpret_cast<TabletsOnMaster*>(i.user_data());
        *serverStats.add_tabletentry() = table->statEntry;
    }

    respHdr->serverStatsLength = serializeToResponse(rpc->replyPayload,
                                                    &serverStats);
}

/**
 * Top-level server method to handle the MULTI_READ request.
 *
 * \param reqHdr
 *      Header from the incoming RPC request; contains the parameters
 *      for this operation except the tableId, key, keyLength for each
 *      of the objects to be read.
 * \param[out] respHdr
 *      Header for the response that will be returned to the client.
 *      The caller has pre-allocated the right amount of space in the
 *      response buffer for this type of request, and has zeroed out
 *      its contents (so, for example, status is already zero).
 * \param[out] rpc
 *      Complete information about the remote procedure call.
 *      It contains the tableId, key and keyLength for each of the
 *      objects to be read. It can also be used to read additional
 *      information beyond the request header and/or append additional
 *      information to the response buffer.
 */
void
MasterService::multiRead(const WireFormat::MultiRead::Request* reqHdr,
                         WireFormat::MultiRead::Response* respHdr,
                         Rpc* rpc)
{
    uint32_t numRequests = reqHdr->count;
    uint32_t reqOffset = sizeof32(*reqHdr);

    respHdr->count = numRequests;
    uint32_t oldResponseLength = rpc->replyPayload->getTotalLength();

    // Each iteration extracts one request from request rpc, finds the
    // corresponding object, and appends the response to the response rpc.
    for (uint32_t i = 0; ; i++) {
        // If the RPC response has exceeded the legal limit, truncate it
        // to the last object that fits below the limit (the client will
        // retry the objects we don't return).
        uint32_t newLength = rpc->replyPayload->getTotalLength();
        if (newLength > maxMultiReadResponseSize) {
            rpc->replyPayload->truncateEnd(newLength - oldResponseLength);
            respHdr->count = i-1;
            break;
        } else {
            oldResponseLength = newLength;
        }
        if (i >= numRequests) {
            // The loop-termination check is done here rather than in the
            // "for" statement above so that we have a chance to do the
            // size check above even for every object inserted, including
            // the last object and those with STATUS_OBJECT_DOESNT_EXIST.
            break;
        }

        const WireFormat::MultiRead::Request::Part *currentReq =
            rpc->requestPayload->getOffset<
                WireFormat::MultiRead::Request::Part>(reqOffset);
        reqOffset += sizeof32(WireFormat::MultiRead::Request::Part);
        const void* stringKey = rpc->requestPayload->getRange(
            reqOffset, currentReq->keyLength);
        reqOffset += currentReq->keyLength;
        Key key(currentReq->tableId, stringKey, currentReq->keyLength);

        Status* status = new(rpc->replyPayload, APPEND) Status(STATUS_OK);

        WireFormat::MultiRead::Response::Part* currentResp =
            new(rpc->replyPayload, APPEND)
                WireFormat::MultiRead::Response::Part();

        Buffer buffer;
        *status = objectManager->readObject(key,
                                            &buffer,
                                            defaultRejectRules,
                                            &currentResp->version);
        if (*status == STATUS_OK) {
            Object object(buffer);
            currentResp->length = object.getDataLength();
            object.appendDataToBuffer(*rpc->replyPayload);
        }
    }
}

/**
 * Top-level server method to handle the MULTI_WRITE request.
 *
 * \param reqHdr
 *      Header from the incoming RPC request. Lists the number of writes
 *      contained in this request.
 * \param[out] respHdr
 *      Header for the response that will be returned to the client.
 *      The caller has pre-allocated the right amount of space in the
 *      response buffer for this type of request, and has zeroed out
 *      its contents (so, for example, status is already zero).
 * \param[out] rpc
 *      Complete information about the remote procedure call.
 *      It contains the the key and value for each object, as well as
 *      RejectRules to support conditional writes.
 */
void
MasterService::multiWrite(const WireFormat::MultiWrite::Request* reqHdr,
                          WireFormat::MultiWrite::Response* respHdr,
                          Rpc* rpc)
{
    uint32_t numRequests = reqHdr->count;
    uint32_t reqOffset = sizeof32(*reqHdr);

    respHdr->count = numRequests;

    // Each iteration extracts one request from the rpc, writes the object
    // if possible, and appends a status and version to the response buffer.
    for (uint32_t i = 0; i < numRequests; i++) {
        const WireFormat::MultiWrite::Request::Part *currentReq =
            rpc->requestPayload->getOffset<
                WireFormat::MultiWrite::Request::Part>(reqOffset);

        if (currentReq == NULL) {
            respHdr->common.status = STATUS_REQUEST_FORMAT_ERROR;
            break;
        }

        reqOffset += sizeof32(WireFormat::MultiWrite::Request::Part);
        const void* stringKey = rpc->requestPayload->getRange(
            reqOffset, currentReq->keyLength);
        reqOffset += currentReq->keyLength;
        const void* value = rpc->requestPayload->getRange(
            reqOffset, currentReq->valueLength);
        reqOffset += currentReq->valueLength;

        if (stringKey == NULL || value == NULL) {
            respHdr->common.status = STATUS_REQUEST_FORMAT_ERROR;
            break;
        }

        Key key(currentReq->tableId, stringKey, currentReq->keyLength);
        Buffer buffer;
        buffer.append(value, currentReq->valueLength);

        WireFormat::MultiWrite::Response::Part* currentResp =
            new(rpc->replyPayload, APPEND)
                WireFormat::MultiWrite::Response::Part();

        RejectRules rejectRules = currentReq->rejectRules;
        currentResp->status = objectManager->writeObject(key,
                                                         buffer,
                                                         rejectRules,
                                                         &currentResp->version);
    }

    // By design, our response will be shorter than the request. This ensures
    // that the response can go back in a single RPC.
    assert(rpc->replyPayload->getTotalLength() <= Transport::MAX_RPC_LEN);

    // All of the individual writes were done asynchronously. Sync the objects
    // now to propagate them in bulk to backups.
    objectManager->syncWrites();
}

/**
 * Top-level server method to handle the READ request.
 *
 * \param reqHdr
 *      Header from the incoming RPC request; contains all the
 *      parameters for this operation except the key of the object.
 * \param[out] respHdr
 *      Header for the response that will be returned to the client.
 *      The caller has pre-allocated the right amount of space in the
 *      response buffer for this type of request, and has zeroed out
 *      its contents (so, for example, status is already zero).
 * \param[out] rpc
 *      Complete information about the remote procedure call.
 *      It contains the key for the object. It can also be used to
 *      read additional information beyond the request header and/or
 *      append additional information to the response buffer.
 */
void
MasterService::read(const WireFormat::Read::Request* reqHdr,
                    WireFormat::Read::Response* respHdr,
                    Rpc* rpc)
{
    uint32_t reqOffset = sizeof32(*reqHdr);
    const void* stringKey = rpc->requestPayload->getRange(reqOffset,
                                                        reqHdr->keyLength);
    Key key(reqHdr->tableId, stringKey, reqHdr->keyLength);

    RejectRules rejectRules = reqHdr->rejectRules;
    Buffer buffer;
    respHdr->common.status = objectManager->readObject(key,
                                                       &buffer,
                                                       rejectRules,
                                                       &respHdr->version);
    Object object(buffer);
    respHdr->length = object.getDataLength();
    object.appendDataToBuffer(*rpc->replyPayload);
}

/**
 * Top-level server method to handle the DROP_TABLET_OWNERSHIP request.
 *
 * This RPC is issued by the coordinator when a table is dropped and all
 * tablets are being destroyed. This is not currently used in migration,
 * since the source master knows that it no longer owns the tablet when
 * the coordinator has responded to its REASSIGN_TABLET_OWNERSHIP rpc.
 *
 * \copydetails Service::ping
 */
void
MasterService::dropTabletOwnership(
    const WireFormat::DropTabletOwnership::Request* reqHdr,
    WireFormat::DropTabletOwnership::Response* respHdr,
    Rpc* rpc)
{
    int index = 0;
    ProtoBuf::Tablets* tablets = &objectManager->tablets;
    foreach (ProtoBuf::Tablets::Tablet& i, *tablets->mutable_tablet()) {
        if (reqHdr->tableId == i.table_id() &&
          reqHdr->firstKeyHash == i.start_key_hash() &&
          reqHdr->lastKeyHash == i.end_key_hash()) {
            LOG(NOTICE, "Dropping ownership of tablet (%lu, range [%lu,%lu])",
                reqHdr->tableId, reqHdr->firstKeyHash, reqHdr->lastKeyHash);
            TabletsOnMaster* table =
                    reinterpret_cast<TabletsOnMaster*>(i.user_data());
            delete table;
            tablets->mutable_tablet()->SwapElements(
                tablets->tablet_size() - 1, index);
            tablets->mutable_tablet()->RemoveLast();
            return;
        }

        index++;
    }

    LOG(WARNING, "Could not drop ownership on unknown tablet (%lu, range "
        "[%lu,%lu])!", reqHdr->tableId, reqHdr->firstKeyHash,
        reqHdr->lastKeyHash);
    respHdr->common.status = STATUS_UNKNOWN_TABLET;
}

/**
 * Top-level server method to handle the SPLIT_MASTER_TABLET_OWNERSHIP request.
 *
 * This RPC is issued by the coordinator when a tablet should be splitted. The
 * coordinator specifies the to be splitted tablet and at which point the split
 * should occur (splitKeyHash).
 *
 * \copydetails Service::ping
 */
void
MasterService::splitMasterTablet(
    const WireFormat::SplitMasterTablet::Request* reqHdr,
    WireFormat::SplitMasterTablet::Response* respHdr,
    Rpc* rpc)
{
    ProtoBuf::Tablets_Tablet newTablet;

    ProtoBuf::Tablets* tablets = &objectManager->tablets;
    foreach (ProtoBuf::Tablets::Tablet& i, *tablets->mutable_tablet()) {
        if (reqHdr->tableId == i.table_id() &&
          reqHdr->firstKeyHash == i.start_key_hash() &&
          reqHdr->lastKeyHash == i.end_key_hash()) {

            newTablet = i;

            TabletsOnMaster* newTable =
                    new TabletsOnMaster(reqHdr->tableId, reqHdr->firstKeyHash,
                                 reqHdr->splitKeyHash - 1);
            i.set_user_data(reinterpret_cast<uint64_t>(newTable));
            i.set_end_key_hash(reqHdr->splitKeyHash - 1);
        }

    }

    newTablet.set_start_key_hash(reqHdr->splitKeyHash);
    TabletsOnMaster* newTable =
            new TabletsOnMaster(reqHdr->tableId, reqHdr->splitKeyHash,
                                 reqHdr->lastKeyHash);
    newTablet.set_user_data(reinterpret_cast<uint64_t>(newTable));

    *tablets->add_tablet() = newTablet;

    LOG(NOTICE, "In table '%lu' I split the tablet that started at key %lu and "
                "ended at key %lu", reqHdr->tableId, reqHdr->firstKeyHash,
                reqHdr->lastKeyHash);
}

/**
 * Top-level server method to handle the TAKE_TABLET_OWNERSHIP request.
 *
 * This RPC is issued by the coordinator when assigning ownership of a
 * tablet. This can occur due to both tablet creation and to complete
 * migration. As far as the coordinator is concerned, the master
 * receiving this rpc owns the tablet specified and all requests for it
 * will be directed here from now on.
 *
 * \copydetails Service::ping
 */
void
MasterService::takeTabletOwnership(
    const WireFormat::TakeTabletOwnership::Request* reqHdr,
    WireFormat::TakeTabletOwnership::Response* respHdr,
    Rpc* rpc)
{
    // Before any tablets can be assigned to this master it must have at
    // least one segment on backups, otherwise it is impossible to
    // distinguish between the loss of its entire log and the case where no
    // data was ever written to it. The log's constructor does not create a
    // head segment because doing so can lead to deadlock: the first master
    // blocks, waiting to hear about enough backup servers, meanwhile the
    // coordinator is trying to issue an RPC to the master, but it isn't
    // even servicing transports yet!
    objectManager->syncWrites();

    ProtoBuf::Tablets::Tablet* tablet = NULL;
    ProtoBuf::Tablets* tablets = &objectManager->tablets;
    foreach (ProtoBuf::Tablets::Tablet& i, *tablets->mutable_tablet()) {
        if (reqHdr->tableId == i.table_id() &&
          reqHdr->firstKeyHash == i.start_key_hash() &&
          reqHdr->lastKeyHash == i.end_key_hash()) {
            tablet = &i;
            break;
       }
    }

    if (tablet == NULL) {
        // Sanity check that this tablet doesn't overlap with an existing one.
        if (objectManager->getTable(reqHdr->tableId, reqHdr->firstKeyHash) != NULL ||
          objectManager->getTable(reqHdr->tableId, reqHdr->lastKeyHash) != NULL) {
            LOG(WARNING, "Tablet being assigned (%lu, range [%lu,%lu]) "
                "partially overlaps an existing tablet!", reqHdr->tableId,
                reqHdr->firstKeyHash, reqHdr->lastKeyHash);
            // TODO(anybody): Do we want a more meaningful error code?
            respHdr->common.status = STATUS_INTERNAL_ERROR;
            return;
        }

        LOG(NOTICE, "Taking ownership of new tablet (%lu, range "
            "[%lu,%lu])", reqHdr->tableId, reqHdr->firstKeyHash,
            reqHdr->lastKeyHash);


        ProtoBuf::Tablets_Tablet& newTablet(*tablets->add_tablet());
        newTablet.set_table_id(reqHdr->tableId);
        newTablet.set_start_key_hash(reqHdr->firstKeyHash);
        newTablet.set_end_key_hash(reqHdr->lastKeyHash);
        newTablet.set_state(ProtoBuf::Tablets_Tablet_State_NORMAL);

        TabletsOnMaster* table =
                new TabletsOnMaster(reqHdr->tableId, reqHdr->firstKeyHash,
                                    reqHdr->lastKeyHash);
        newTablet.set_user_data(reinterpret_cast<uint64_t>(table));
    } else {
        LOG(NOTICE, "Taking ownership of existing tablet (%lu, range "
            "[%lu,%lu]) in state %d", reqHdr->tableId, reqHdr->firstKeyHash,
            reqHdr->lastKeyHash, tablet->state());

        if (tablet->state() != ProtoBuf::Tablets_Tablet_State_RECOVERING) {
            LOG(WARNING, "Taking ownership when existing tablet is in "
                "unexpected state (%d)!", tablet->state());
        }

        tablet->set_state(ProtoBuf::Tablets_Tablet_State_NORMAL);

        // If we took ownership after migration, then recoverSegment() may have
        // added tombstones to the hash table. Clean them up.
        removeTombstones();
    }
}

/**
 * Top-level server method to handle the PREP_FOR_MIGRATION request.
 *
 * This is used during tablet migration to request that a destination
 * master take on a tablet from the current owner. The receiver may
 * accept or refuse.
 *
 * \copydetails Service::ping
 */
void
MasterService::prepForMigration(
    const WireFormat::PrepForMigration::Request* reqHdr,
    WireFormat::PrepForMigration::Response* respHdr,
    Rpc* rpc)
{
    // Decide if we want to decline this request.

    // Ensure that there's no tablet overlap, just in case.
    bool overlap =
        (objectManager->getTable(reqHdr->tableId, reqHdr->firstKeyHash) != NULL ||
         objectManager->getTable(reqHdr->tableId, reqHdr->lastKeyHash) != NULL);
    if (overlap) {
        LOG(WARNING, "already have tablet in range [%lu, %lu] for tableId %lu",
            reqHdr->firstKeyHash, reqHdr->lastKeyHash, reqHdr->tableId);
        respHdr->common.status = STATUS_OBJECT_EXISTS;
        return;
    }

    // Add the tablet to our map and mark it as RECOVERING so that no requests
    // are served on it.
    ProtoBuf::Tablets* tablets = &objectManager->tablets;
    ProtoBuf::Tablets_Tablet& tablet(*tablets->add_tablet());
    tablet.set_table_id(reqHdr->tableId);
    tablet.set_start_key_hash(reqHdr->firstKeyHash);
    tablet.set_end_key_hash(reqHdr->lastKeyHash);
    tablet.set_state(ProtoBuf::Tablets_Tablet_State_RECOVERING);

    TabletsOnMaster* table =
            new TabletsOnMaster(reqHdr->tableId, reqHdr->firstKeyHash,
                                reqHdr->lastKeyHash);
    tablet.set_user_data(reinterpret_cast<uint64_t>(table));

    // TODO(rumble) would be nice to have a method to get a SL from an Rpc
    // object.
    LOG(NOTICE, "Ready to receive tablet from \"??\". Table %lu, "
        "range [%lu,%lu]", reqHdr->tableId, reqHdr->firstKeyHash,
        reqHdr->lastKeyHash);
}

/**
 * Top-level server method to handle the MIGRATE_TABLET request.
 *
 * This is used to manually initiate the migration of a tablet (or piece of a
 * tablet) that this master owns to another master.
 *
 * \copydetails Service::ping
 */
void
MasterService::migrateTablet(const WireFormat::MigrateTablet::Request* reqHdr,
                             WireFormat::MigrateTablet::Response* respHdr,
                             Rpc* rpc)
{
    uint64_t tableId = reqHdr->tableId;
    uint64_t firstKeyHash = reqHdr->firstKeyHash;
    uint64_t lastKeyHash = reqHdr->lastKeyHash;
    ServerId newOwnerMasterId(reqHdr->newOwnerMasterId);

    // Find the tablet we're trying to move. We only support migration
    // when the tablet to be migrated consists of a range within a single,
    // contiguous tablet of ours.
    const ProtoBuf::Tablets::Tablet* tablet = NULL;
    int tabletIndex = 0;
    ProtoBuf::Tablets* tablets = &objectManager->tablets;
    foreach (const ProtoBuf::Tablets::Tablet& i, tablets->tablet()) {
        if (tableId == i.table_id() &&
          firstKeyHash >= i.start_key_hash() &&
          lastKeyHash <= i.end_key_hash()) {
            tablet = &i;
            break;
        }
        tabletIndex++;
    }

    if (tablet == NULL) {
        LOG(WARNING, "Migration request for range this master does not "
            "own. TableId %lu, range [%lu,%lu]",
            tableId, firstKeyHash, lastKeyHash);
        respHdr->common.status = STATUS_UNKNOWN_TABLET;
        return;
    }

    if (newOwnerMasterId == serverId) {
        LOG(WARNING, "Migrating to myself doesn't make much sense");
        respHdr->common.status = STATUS_REQUEST_FORMAT_ERROR;
        return;
    }

    TabletsOnMaster* table =
            reinterpret_cast<TabletsOnMaster*>(tablet->user_data());

    // TODO(rumble/slaughter) what if we end up splitting?!?

    // TODO(rumble/slaughter) add method to query for # objs, # bytes in a
    // range in order for this to really work, we'll need to split on a bucket
    // boundary. Otherwise we can't tell where bytes are in the chosen range.
    MasterClient::prepForMigration(context, newOwnerMasterId, tableId,
                                   firstKeyHash, lastKeyHash, 0, 0);
    Log::Position newOwnerLogHead = MasterClient::getHeadOfLog(context,
                                                              newOwnerMasterId);

    LOG(NOTICE, "Migrating tablet (id %lu, first %lu, last %lu) to "
        "ServerId %s (\"%s\")", tableId, firstKeyHash, lastKeyHash,
        newOwnerMasterId.toString().c_str(),
        context->serverList->getLocator(newOwnerMasterId));

    // We'll send over objects in Segment containers for better network
    // efficiency and convenience.
    Tub<Segment> transferSeg;

    // TODO(rumble/slaughter): These should probably be metrics.
    uint64_t totalObjects = 0;
    uint64_t totalTombstones = 0;
    uint64_t totalBytes = 0;

    // Hold on to the iterator since it locks the head Segment, avoiding any
    // additional appends once we've finished iterating.
    LogIterator it(objectManager->log);
    for (; !it.isDone(); it.next()) {
        LogEntryType type = it.getType();
        if (type != LOG_ENTRY_TYPE_OBJ && type != LOG_ENTRY_TYPE_OBJTOMB) {
            // We aren't interested in any other types.
            continue;
        }

        Buffer buffer;
        it.appendToBuffer(buffer);
        Key key(type, buffer);

        // Skip if not applicable.
        if (key.getTableId() != tableId)
            continue;

        if (key.getHash() < firstKeyHash || key.getHash() > lastKeyHash)
            continue;

        if (type == LOG_ENTRY_TYPE_OBJ) {
#if 0 // XXXX- fix this
            // Only send objects when they're currently in the hash table.
            // Otherwise they're dead.
            LogEntryType currentType;
            Buffer currentBuffer;
            if (lookup(key, currentType, currentBuffer) == false)
                continue;

            // NB: The cleaner is currently locked out due to the global
            //     objectUpdateLock. In the future this may not be the
            //     case and objects may be moved forward during iteration.
            if (buffer.getStart<uint8_t>() != currentBuffer.getStart<uint8_t>())
                continue;

            totalObjects++;
#endif
        } else {
            // We must always send tombstones, since an object we may have sent
            // could have been deleted more recently. We could be smarter and
            // more selective here, but that'd require keeping extra state to
            // know what we've already sent.
            //
            // TODO(rumble/slaughter) Actually, we can do better. The stupid way
            //      is to track each object or tombstone we've sent. The smarter
            //      way is to just record the Log::Position when we started
            //      iterating and only send newer tombstones.

            totalTombstones++;
        }

        totalBytes += buffer.getTotalLength();

        if (!transferSeg)
            transferSeg.construct();

        // If we can't fit it, send the current buffer and retry.
        if (!transferSeg->append(type, buffer)) {
            transferSeg->close();
            LOG(DEBUG, "Sending migration segment");
            MasterClient::receiveMigrationData(context, newOwnerMasterId,
                tableId, firstKeyHash, transferSeg.get());

            transferSeg.destroy();
            transferSeg.construct();

            // If it doesn't fit this time, we're in trouble.
            if (!transferSeg->append(type, buffer)) {
                LOG(ERROR, "Tablet migration failed: could not fit object "
                    "into empty segment (obj bytes %u)",
                    buffer.getTotalLength());
                respHdr->common.status = STATUS_INTERNAL_ERROR;
                return;
            }
        }
    }

    if (transferSeg) {
        transferSeg->close();
        LOG(DEBUG, "Sending last migration segment");
        MasterClient::receiveMigrationData(context, newOwnerMasterId,
            tableId, firstKeyHash, transferSeg.get());
        transferSeg.destroy();
    }

    // Now that all data has been transferred, we can reassign ownership of
    // the tablet. If this succeeds, we are free to drop the tablet. The
    // data is all on the other machine and the coordinator knows to use it
    // for any recoveries.
    CoordinatorClient::reassignTabletOwnership(context,
        tableId, firstKeyHash, lastKeyHash, newOwnerMasterId,
        newOwnerLogHead.getSegmentId(), newOwnerLogHead.getSegmentOffset());

    LOG(NOTICE, "Tablet migration succeeded. Sent %lu objects and %lu "
        "tombstones. %lu bytes in total.", totalObjects, totalTombstones,
        totalBytes);

    tablets->mutable_tablet()->SwapElements(tablets->tablet_size() - 1,
                                           tabletIndex);
    tablets->mutable_tablet()->RemoveLast();
    free(table);
}

/**
 * Top-level server method to handle the RECEIVE_MIGRATION_DATA request.
 *
 * This RPC delivers tablet data to be added to a master during migration.
 * It must have been preceeded by an appropriate PREP_FOR_MIGRATION rpc.
 *
 * \copydetails Service::ping
 */
void
MasterService::receiveMigrationData(
    const WireFormat::ReceiveMigrationData::Request* reqHdr,
    WireFormat::ReceiveMigrationData::Response* respHdr,
    Rpc* rpc)
{
    uint64_t tableId = reqHdr->tableId;
    uint64_t firstKeyHash = reqHdr->firstKeyHash;
    uint32_t segmentBytes = reqHdr->segmentBytes;

    // TODO(rumble/slaughter) need to make sure we already have a table
    // created that was previously prepped for migration.
    const ProtoBuf::Tablets::Tablet* tablet = NULL;
    ProtoBuf::Tablets* tablets = &objectManager->tablets;
    foreach (const ProtoBuf::Tablets::Tablet& i, tablets->tablet()) {
        if (tableId == i.table_id() && firstKeyHash == i.start_key_hash()) {
            tablet = &i;
            break;
        }
    }

    if (tablet == NULL) {
        LOG(WARNING, "migration data received for unknown tablet %lu, "
            "firstKeyHash %lu", tableId, firstKeyHash);
        respHdr->common.status = STATUS_UNKNOWN_TABLET;
        return;
    }

    if (tablet->state() != ProtoBuf::Tablets_Tablet_State_RECOVERING) {
        LOG(WARNING, "migration data received for tablet not in the "
            "RECOVERING state (state = %s)!",
            ProtoBuf::Tablets_Tablet_State_Name(tablet->state()).c_str());
        // TODO(rumble/slaughter): better error code here?
        respHdr->common.status = STATUS_INTERNAL_ERROR;
        return;
    }

    LOG(NOTICE, "RECEIVED MIGRATION DATA (tbl %lu, fk %lu, bytes %u)!\n",
        tableId, firstKeyHash, segmentBytes);

    Segment::Certificate certificate = reqHdr->certificate;
    rpc->requestPayload->truncateFront(sizeof(*reqHdr));
    if (rpc->requestPayload->getTotalLength() != segmentBytes) {
        LOG(ERROR, "RPC size (%u) does not match advertised length (%u)",
            rpc->requestPayload->getTotalLength(),
            segmentBytes);
        respHdr->common.status = STATUS_REQUEST_FORMAT_ERROR;
        return;
    }
    const void* segmentMemory = rpc->requestPayload->getStart<const void*>();
    SegmentIterator it(segmentMemory, segmentBytes, certificate);
    it.checkMetadataIntegrity();
    SideLog sideLog(&objectManager->log);
    objectManager->replaySegment(&sideLog, it);
    sideLog.commit();

    // TODO(rumble/slaughter) what about tablet version numbers?
    //          - need to be made per-server now, no? then take max of two?
    //            but this needs to happen at the end (after head on orig.
    //            master is locked)
    //    - what about autoincremented keys?
    //    - what if we didn't send a whole tablet, but rather split one?!
    //      how does this affect autoincr. keys and the version number(s),
    //      if at all?
}

/**
 * Callback used to purge the tombstones from the hash table. Invoked by
 * HashTable::forEach.
 */
void
recoveryCleanup(uint64_t maybeTomb, void *cookie)
{
#if 0
    MasterService *server = reinterpret_cast<MasterService*>(cookie);
    LogEntryType type;
    Buffer buffer;

    type = server->log->getEntry(Log::Reference(maybeTomb), buffer);
    if (type == LOG_ENTRY_TYPE_OBJTOMB) {
        Key key(type, buffer);

        bool r = server->objectMap->remove(key);
        assert(r);

        // Tombstones are not explicitly freed in the log-> The cleaner will
        // figure out that they're dead.
    }
#endif
}

/**
 * A Dispatch::Poller which lazily removes tombstones from the main HashTable.
 */
class RemoveTombstonePoller : public Dispatch::Poller {
  public:
    /**
     * Clean tombstones from #objectMap lazily and in the background.
     *
     * Instances of this class must be allocated with new since they
     * delete themselves when the #objectMap scan is completed which
     * automatically deregisters it from Dispatch.
     *
     * \param masterService
     *      The instance of MasterService which owns the #objectMap->
     * \param objectMap
     *      The HashTable which will be purged of tombstones.
     */
    RemoveTombstonePoller(MasterService& masterService, HashTable& objectMap)
        : Dispatch::Poller(*masterService.context->dispatch)
        , currentBucket(0)
        , masterService(masterService)
        , objectMap(objectMap)
    {
        LOG(NOTICE, "Starting cleanup of tombstones in background");
    }

    /**
     * Remove tombstones from a single bucket and yield to other work
     * in the system.
     */
    virtual void
    poll()
    {
        // This method runs in the dispatch thread, so it isn't safe to
        // manipulate any of the objectMap state if any RPCs are currently
        // executing.
        if (!masterService.context->serviceManager->idle())
            return;
        objectMap.forEachInBucket(
            recoveryCleanup, &masterService, currentBucket);
        ++currentBucket;
        if (currentBucket == objectMap.getNumBuckets()) {
            LOG(NOTICE, "Cleanup of tombstones complete");
            delete this;
        }
    }

  private:
    /// Which bucket of #objectMap should be cleaned out next.
    uint64_t currentBucket;

    /// The MasterService used by the #recoveryCleanup callback.
    MasterService& masterService;

    /// The hash table to be purged of tombstones.
    HashTable& objectMap;

    DISALLOW_COPY_AND_ASSIGN(RemoveTombstonePoller);
};

/**
 * Remove leftover tombstones in the hash table added during recovery.
 * This method exists independently for testing purposes.
 */
void
MasterService::removeTombstones()
{
#if 0
    CycleCounter<RawMetric> _(&metrics->master.removeTombstoneTicks);
#if TESTING
    // Asynchronous tombstone removal raises hell in unit tests.
    objectMap->forEach(recoveryCleanup, this);
#else
    Dispatch::Lock lock(context->dispatch);
    new RemoveTombstonePoller(*this, *objectMap);
#endif
#endif
}

namespace MasterServiceInternal {
/**
 * Each object of this class is responsible for fetching recovery data
 * for a single segment from a single backup.
 */
class RecoveryTask {
  PUBLIC:
    RecoveryTask(Context* context,
                 uint64_t recoveryId,
                 ServerId masterId,
                 uint64_t partitionId,
                 MasterService::Replica& replica)
        : context(context)
        , recoveryId(recoveryId)
        , masterId(masterId)
        , partitionId(partitionId)
        , replica(replica)
        , response()
        , startTime(Cycles::rdtsc())
        , rpc()
    {
        rpc.construct(context, replica.backupId,
                      recoveryId, masterId, replica.segmentId,
                      partitionId, &response);
    }
    ~RecoveryTask()
    {
        if (rpc && !rpc->isReady()) {
            LOG(WARNING, "Task destroyed while RPC active: segment %lu, "
                    "server %s", replica.segmentId,
                    context->serverList->toString(replica.backupId).c_str());
        }
    }
    void resend() {
        LOG(DEBUG, "Resend %lu", replica.segmentId);
        response.reset();
        rpc.construct(context, replica.backupId,
                      recoveryId, masterId, replica.segmentId,
                      partitionId, &response);
    }
    Context* context;
    uint64_t recoveryId;
    ServerId masterId;
    uint64_t partitionId;
    MasterService::Replica& replica;
    Buffer response;
    const uint64_t startTime;
    Tub<GetRecoveryDataRpc> rpc;
    DISALLOW_COPY_AND_ASSIGN(RecoveryTask);
};
} // namespace MasterServiceInternal
using namespace MasterServiceInternal; // NOLINT

/**
 * Look through \a backups and ensure that for each segment id that appears
 * in the list that at least one copy of that segment was replayed.
 *
 * \param masterId
 *      The id of the crashed master this recovery master is recovering for.
 *      Only used for logging detailed log information on failure.
 * \param partitionId
 *      The id of the partition of the crashed master this recovery master is
 *      recovering. Only used for logging detailed log information on failure.
 * \param replicas
 *      The list of replicas and their statuses to be checked to ensure
 *      recovery of this partition was successful.
 * \throw SegmentRecoveryFailedException
 *      If some segment was not recovered and the recovery master is not
 *      a valid replacement for the crashed master.
 */
void
MasterService::detectSegmentRecoveryFailure(
            const ServerId masterId,
            const uint64_t partitionId,
            const vector<MasterService::Replica>& replicas)
{
    std::unordered_set<uint64_t> failures;
    foreach (const auto& replica, replicas) {
        switch (replica.state) {
        case MasterService::Replica::State::OK:
            failures.erase(replica.segmentId);
            break;
        case MasterService::Replica::State::FAILED:
            failures.insert(replica.segmentId);
            break;
        case MasterService::Replica::State::WAITING:
        case MasterService::Replica::State::NOT_STARTED:
        default:
            assert(false);
            break;
        }
    }
    if (!failures.empty()) {
        LOG(ERROR, "Recovery master failed to recover master %lu "
            "partition %lu", *masterId, partitionId);
        foreach (auto segmentId, failures)
            LOG(ERROR, "Unable to recover segment %lu", segmentId);
        throw SegmentRecoveryFailedException(HERE);
    }
}

/**
 * Helper for public recover() method.
 * Collect all the filtered log segments from backups for a set of tablets
 * formerly belonging to a crashed master which is being recovered and pass
 * them to the recovery master to have them replayed.
 *
 * \param recoveryId
 *      Id of the recovery this recovery master was performing.
 * \param masterId
 *      The id of the crashed master for which recoveryMaster will be taking
 *      over ownership of tablets.
 * \param partitionId
 *      The partition id of tablets of the crashed master that this master
 *      is recovering.
 * \param replicas
 *      A list specifying for each segmentId a backup who can provide a
 *      filtered recovery data segment. A particular segment may be listed more
 *      than once if it has multiple viable backups.
 * \throw SegmentRecoveryFailedException
 *      If some segment was not recovered and the recovery master is not
 *      a valid replacement for the crashed master.
 */
void
MasterService::recover(uint64_t recoveryId,
                       ServerId masterId,
                       uint64_t partitionId,
                       vector<Replica>& replicas)
{
    /* Overview of the internals of this method and its structures.
     *
     * The main data structure is "replicas".  It works like a
     * scoreboard, tracking which segments have requests to backup
     * servers in-flight for data, which have been replayed, and
     * which have failed and must be replayed by another entry in
     * the table.
     *
     * replicasEnd is an iterator to the end of the segment replica list
     * which aids in tracking when the function is out of work.
     *
     * notStarted tracks the furtherest entry into the list which
     * has not been requested from a backup yet (State::NOT_STARTED).
     *
     * Here is a sample of what the structure might look like
     * during execution:
     *
     * backupId     segmentId  state
     * --------     ---------  -----
     *   8            99       OK
     *   3            88       FAILED
     *   1            77       OK
     *   2            77       OK
     *   6            88       WAITING
     *   2            66       NOT_STARTED  <- notStarted
     *   3            55       WAITING
     *   1            66       NOT_STARTED
     *   7            66       NOT_STARTED
     *   3            99       OK
     *
     * The basic idea is, the code kicks off up to some fixed
     * number worth of RPCs marking them WAITING starting from the
     * top of the list working down.  When a response comes it
     * marks the entry as FAILED if there was an error fetching or
     * replaying it. If it succeeded in replaying, though then ALL
     * entries for that segment_id are marked OK. (This is done
     * by marking the entry itself and then iterating starting
     * at "notStarted" and checking each row for a match).
     *
     * One other structure "runningSet" tracks which segment_ids
     * have RPCs in-flight.  When starting new RPCs rows that
     * have a segment_id that is in the set are skipped over.
     * However, since the row is still NOT_STARTED, notStarted
     * must point to it or to an earlier entry, so the entry
     * will be revisited in the case the other in-flight request
     * fails.  If the other request succeeds then the previously
     * skipped entry is marked OK and notStarted is advanced (if
     * possible).
     */
    uint64_t usefulTime = 0;
    uint64_t start = Cycles::rdtsc();
    LOG(NOTICE, "Recovering master %s, partition %lu, %lu replicas available",
        masterId.toString().c_str(), partitionId, replicas.size());

    std::unordered_set<uint64_t> runningSet;
    Tub<RecoveryTask> tasks[4];
    uint32_t activeRequests = 0;

    auto notStarted = replicas.begin();
    auto replicasEnd = replicas.end();

    // The SideLog we'll append recovered entries to. It will be committed after
    // replay completes on all segments, making all of the recovered data
    // durable.
    SideLog sideLog(&objectManager->log);

    // Start RPCs
    auto replicaIt = notStarted;
    foreach (auto& task, tasks) {
        while (!task) {
            if (replicaIt == replicasEnd)
                goto doneStartingInitialTasks;
            auto& replica = *replicaIt;
            LOG(DEBUG, "Starting getRecoveryData from %s for segment %lu "
                "on channel %ld (initial round of RPCs)",
                context->serverList->toString(replica.backupId).c_str(),
                replica.segmentId,
                &task - &tasks[0]);
            task.construct(context, recoveryId, masterId, partitionId,
                           replica);
            replica.state = Replica::State::WAITING;
            runningSet.insert(replica.segmentId);
            ++metrics->master.segmentReadCount;
            ++activeRequests;
            ++replicaIt;
            while (replicaIt != replicasEnd &&
                   contains(runningSet, replicaIt->segmentId)) {
                ++replicaIt;
            }
        }
    }
  doneStartingInitialTasks:

    // As RPCs complete, process them and start more
    Tub<CycleCounter<RawMetric>> readStallTicks;

    bool gotFirstGRD = false;

    std::unordered_multimap<uint64_t, Replica*> segmentIdToBackups;
    foreach (Replica& replica, replicas)
        segmentIdToBackups.insert({replica.segmentId, &replica});

    while (activeRequests) {
        if (!readStallTicks)
            readStallTicks.construct(&metrics->master.segmentReadStallTicks);
        objectManager->replicaManager.proceed();
        foreach (auto& task, tasks) {
            if (!task)
                continue;
            if (!task->rpc->isReady())
                continue;
            readStallTicks.destroy();
            LOG(DEBUG, "Waiting on recovery data for segment %lu from %s",
                task->replica.segmentId,
                context->serverList->toString(task->replica.backupId).c_str());
            try {
                Segment::Certificate certificate = task->rpc->wait();
                uint64_t grdTime = Cycles::rdtsc() - task->startTime;
                metrics->master.segmentReadTicks += grdTime;

                if (!gotFirstGRD) {
                    metrics->master.replicationBytes =
                        0 - metrics->transport.transmit.byteCount;
                    metrics->master.replicationTransmitCopyTicks =
                        0 - metrics->transport.transmit.copyTicks;
                    metrics->master.replicationTransmitActiveTicks =
                        0 - metrics->transport.infiniband.transmitActiveTicks;
                    metrics->master.replicationPostingWriteRpcTicks = 0;
                    metrics->master.replayMemoryReadBytes = 0 -
                        // tx
                        (metrics->master.replicationBytes +
                        // tx copy
                         metrics->master.replicationBytes +
                        // backup write copy
                         metrics->backup.writeCopyBytes +
                        // read from filtering objects
                         metrics->backup.storageReadBytes +
                        // log append copy
                         metrics->master.liveObjectBytes);
                    metrics->master.replayMemoryWrittenBytes = 0 -
                        // tx copy
                        (metrics->master.replicationBytes +
                        // backup write copy
                         metrics->backup.writeCopyBytes +
                        // disk read into memory
                         metrics->backup.storageReadBytes +
                        // copy from filtering objects
                         metrics->backup.storageReadBytes +
                        // rx into memory
                         metrics->transport.receive.byteCount +
                        // log append copy
                         metrics->master.liveObjectBytes);
                    gotFirstGRD = true;
                }
                if (LOG_RECOVERY_REPLICATION_RPC_TIMING) {
                    LOG(DEBUG, "@%7lu: Got getRecoveryData response from %s, "
                        "took %.1f us on channel %ld",
                        Cycles::toMicroseconds(Cycles::rdtsc() -
                            ReplicatedSegment::recoveryStart),
                        context->serverList->toString(
                            task->replica.backupId).c_str(),
                        Cycles::toSeconds(grdTime)*1e06,
                        &task - &tasks[0]);
                }

                uint32_t responseLen = task->response.getTotalLength();
                metrics->master.segmentReadByteCount += responseLen;
                uint64_t startUseful = Cycles::rdtsc();
                SegmentIterator it(task->response.getRange(0, responseLen),
                                   responseLen, certificate);
                it.checkMetadataIntegrity();
                if (LOG_RECOVERY_REPLICATION_RPC_TIMING) {
                    LOG(DEBUG, "@%7lu: Replaying segment %lu with length %u",
                        Cycles::toMicroseconds(Cycles::rdtsc() -
                            ReplicatedSegment::recoveryStart),
                        task->replica.segmentId, responseLen);
                }
                objectManager->replaySegment(&sideLog, it);
                usefulTime += Cycles::rdtsc() - startUseful;
                TEST_LOG("Segment %lu replay complete",
                         task->replica.segmentId);
                if (LOG_RECOVERY_REPLICATION_RPC_TIMING) {
                    LOG(DEBUG, "@%7lu: Replaying segment %lu done",
                        Cycles::toMicroseconds(Cycles::rdtsc() -
                            ReplicatedSegment::recoveryStart),
                        task->replica.segmentId);
                }

                runningSet.erase(task->replica.segmentId);
                // Mark this and any other entries for this segment as OK.
                LOG(DEBUG, "Checking %s off the list for %lu",
                    context->serverList->toString(
                        task->replica.backupId).c_str(),
                    task->replica.segmentId);
                task->replica.state = Replica::State::OK;
                foreach (auto it, segmentIdToBackups.equal_range(
                                        task->replica.segmentId)) {
                    Replica& otherReplica = *it.second;
                    LOG(DEBUG, "Checking %s off the list for %lu",
                        context->serverList->toString(
                            otherReplica.backupId).c_str(),
                        otherReplica.segmentId);
                    otherReplica.state = Replica::State::OK;
                }
            } catch (const SegmentIteratorException& e) {
                LOG(WARNING, "Recovery segment for segment %lu corrupted; "
                    "trying next backup: %s", task->replica.segmentId,
                    e.what());
                task->replica.state = Replica::State::FAILED;
                runningSet.erase(task->replica.segmentId);
            } catch (const ServerNotUpException& e) {
                LOG(WARNING, "No record of backup %s, trying next backup",
                    task->replica.backupId.toString().c_str());
                task->replica.state = Replica::State::FAILED;
                runningSet.erase(task->replica.segmentId);
            } catch (const ClientException& e) {
                LOG(WARNING, "getRecoveryData failed on %s, "
                    "trying next backup; failure was: %s",
                    context->serverList->toString(
                        task->replica.backupId).c_str(),
                    e.str().c_str());
                task->replica.state = Replica::State::FAILED;
                runningSet.erase(task->replica.segmentId);
            }

            task.destroy();

            // move notStarted up as far as possible
            while (notStarted != replicasEnd &&
                   notStarted->state != Replica::State::NOT_STARTED) {
                ++notStarted;
            }

            // Find the next NOT_STARTED entry that isn't in-flight
            // from another entry.
            auto replicaIt = notStarted;
            while (!task && replicaIt != replicasEnd) {
                while (replicaIt->state != Replica::State::NOT_STARTED ||
                       contains(runningSet, replicaIt->segmentId)) {
                    ++replicaIt;
                    if (replicaIt == replicasEnd)
                        goto outOfHosts;
                }
                Replica& replica = *replicaIt;
                LOG(DEBUG, "Starting getRecoveryData from %s for segment %lu "
                    "on channel %ld (after RPC completion)",
                    context->serverList->toString(replica.backupId).c_str(),
                    replica.segmentId,
                    &task - &tasks[0]);
                task.construct(context, recoveryId,
                               masterId, partitionId, replica);
                replica.state = Replica::State::WAITING;
                runningSet.insert(replica.segmentId);
                ++metrics->master.segmentReadCount;
            }
          outOfHosts:
            if (!task)
                --activeRequests;
        }
    }
    readStallTicks.destroy();

    detectSegmentRecoveryFailure(masterId, partitionId, replicas);

    {
        CycleCounter<RawMetric> logSyncTicks(&metrics->master.logSyncTicks);
        LOG(NOTICE, "Committing the SideLog...");
        metrics->master.logSyncBytes =
            0 - metrics->transport.transmit.byteCount;
        metrics->master.logSyncTransmitCopyTicks =
            0 - metrics->transport.transmit.copyTicks;
        metrics->master.logSyncTransmitActiveTicks =
            0 - metrics->transport.infiniband.transmitActiveTicks;
        metrics->master.logSyncPostingWriteRpcTicks =
            0 - metrics->master.replicationPostingWriteRpcTicks;
        sideLog.commit();
        metrics->master.logSyncBytes += metrics->transport.transmit.byteCount;
        metrics->master.logSyncTransmitCopyTicks +=
            metrics->transport.transmit.copyTicks;
        metrics->master.logSyncTransmitActiveTicks +=
            metrics->transport.infiniband.transmitActiveTicks;
        metrics->master.logSyncPostingWriteRpcTicks +=
            metrics->master.replicationPostingWriteRpcTicks;
        LOG(NOTICE, "SideLog finished committing (data is durable).");
    }

    metrics->master.replicationBytes += metrics->transport.transmit.byteCount;
    metrics->master.replicationTransmitCopyTicks +=
        metrics->transport.transmit.copyTicks;
    // See the lines with "0 -" above to get the purpose of each of these
    // fields in this metric.
    metrics->master.replayMemoryReadBytes +=
        (metrics->master.replicationBytes +
         metrics->master.replicationBytes +
         metrics->backup.writeCopyBytes +
         metrics->backup.storageReadBytes +
         metrics->master.liveObjectBytes);
    metrics->master.replayMemoryWrittenBytes +=
        (metrics->master.replicationBytes +
         metrics->backup.writeCopyBytes +
         metrics->backup.storageReadBytes +
         metrics->transport.receive.byteCount +
         metrics->backup.storageReadBytes +
         metrics->master.liveObjectBytes);
    metrics->master.replicationTransmitActiveTicks +=
        metrics->transport.infiniband.transmitActiveTicks;

    double totalSecs = Cycles::toSeconds(Cycles::rdtsc() - start);
    double usefulSecs = Cycles::toSeconds(usefulTime);
    LOG(NOTICE, "Recovery complete, took %.1f ms, useful replaying "
        "time %.1f ms (%.1f%% effective)",
        totalSecs * 1e03,
        usefulSecs * 1e03,
        100 * usefulSecs / totalSecs);
}

/**
 * Removes an object from the hashtable and frees it from the log if
 * it belongs to a tablet that isn't listed in the master's tablets.
 * Used by purgeObjectsFromUnknownTablets().
 *
 * \param reference
 *      Reference into the log for an object as returned from the master's
 *      objectMap->lookup() or on callback from objectMap->forEach(). This
 *      object is removed from the objectMap and freed from the log if it
 *      doesn't belong to any tablet the master lists among its tablets.
 * \param cookie
 *      Pointer to the MasterService where this object is currently
 *      stored.
 */
void
removeObjectIfFromUnknownTablet(uint64_t reference, void *cookie)
{
#if 0
    MasterService* master = reinterpret_cast<MasterService*>(cookie);
    LogEntryType type;
    Buffer buffer;

    type = master->log->getEntry(Log::Reference(reference), buffer);
    if (type != LOG_ENTRY_TYPE_OBJ)
        return;

    Key key(type, buffer);
    if (!master->getTable(key)) {
        bool r = master->objectMap->remove(key);
        assert(r);
        master->log->free(Log::Reference(reference));
    }
#endif
}

/**
 * Scan the hashtable and remove all objects that do not belong to a
 * tablet currently owned by this master. Used to clean up any objects
 * created as part of an aborted recovery.
 */
void
MasterService::purgeObjectsFromUnknownTablets()
{
#if 0
    objectMap->forEach(removeObjectIfFromUnknownTablet, this);
#endif
}

/**
 * Top-level server method to handle the RECOVER request.
 * \copydetails Service::ping
 */
void
MasterService::recover(const WireFormat::Recover::Request* reqHdr,
                       WireFormat::Recover::Response* respHdr,
                       Rpc* rpc)
{
    ReplicatedSegment::recoveryStart = Cycles::rdtsc();
    CycleCounter<RawMetric> recoveryTicks(&metrics->master.recoveryTicks);
    metrics->master.recoveryCount++;
    metrics->master.replicas = objectManager->replicaManager.numReplicas;

    uint64_t recoveryId = reqHdr->recoveryId;
    ServerId crashedServerId(reqHdr->crashedServerId);
    uint64_t partitionId = reqHdr->partitionId;
    if (partitionId == ~0u)
        DIE("Recovery master %s got super secret partition id; killing self.",
            serverId.toString().c_str());
    ProtoBuf::Tablets recoveryTablets;
    ProtoBuf::parseFromResponse(rpc->requestPayload, sizeof(*reqHdr),
                                reqHdr->tabletsLength, &recoveryTablets);

    uint32_t offset = sizeof32(*reqHdr) + reqHdr->tabletsLength;
    vector<Replica> replicas;
    replicas.reserve(reqHdr->numReplicas);
    for (uint32_t i = 0; i < reqHdr->numReplicas; ++i) {
        const WireFormat::Recover::Replica* replicaLocation =
            rpc->requestPayload->getOffset<WireFormat::Recover::Replica>(
            offset);
        offset += sizeof32(WireFormat::Recover::Replica);
        Replica replica(replicaLocation->backupId,
                        replicaLocation->segmentId);
        replicas.push_back(replica);
    }
    LOG(DEBUG, "Starting recovery %lu for crashed master %s; "
        "recovering partition %lu (see user_data) of the following "
        "partitions:\n%s",
        recoveryId, crashedServerId.toString().c_str(), partitionId,
        recoveryTablets.DebugString().c_str());
    rpc->sendReply();

    // reqHdr, respHdr, and rpc are off-limits now

    // Install tablets we are recovering and mark them as such (we don't
    // own them yet).
    vector<ProtoBuf::Tablets_Tablet*> newTablets;
    ProtoBuf::Tablets* tablets = &objectManager->tablets;
    foreach (const ProtoBuf::Tablets::Tablet& tablet,
             recoveryTablets.tablet()) {
        ProtoBuf::Tablets_Tablet& newTablet(*tablets->add_tablet());
        newTablet = tablet;
        TabletsOnMaster* table = new TabletsOnMaster(newTablet.table_id(),
                                 newTablet.start_key_hash(),
                                 newTablet.end_key_hash());
        newTablet.set_user_data(reinterpret_cast<uint64_t>(table));
        newTablet.set_state(ProtoBuf::Tablets::Tablet::RECOVERING);
        newTablets.push_back(&newTablet);
    }

    // Record the log position before recovery started.
    Log::Position headOfLog = objectManager->log.rollHeadOver();

    // Recover Segments, firing MasterService::recoverSegment for each one.
    bool successful = false;
    try {
        recover(recoveryId, crashedServerId, partitionId, replicas);
        successful = true;
    } catch (const SegmentRecoveryFailedException& e) {
        // Recovery wasn't successful.
    }

    // Free recovery tombstones left in the hash table.
    removeTombstones();

    // Once the coordinator and the recovery master agree that the
    // master has taken over for the tablets it can update its tables
    // and begin serving requests.

    // Update the recoveryTablets to reflect the fact that this master is
    // going to try to become the owner. The coordinator will assign final
    // ownership in response to the RECOVERY_MASTER_FINISHED rpc (i.e.
    // we'll only be owners if the call succeeds. It could fail if the
    // coordinator decided to recover these tablets elsewhere instead).
    foreach (ProtoBuf::Tablets::Tablet& tablet,
             *recoveryTablets.mutable_tablet()) {
        LOG(NOTICE, "set tablet %lu %lu %lu to locator %s, id %s",
                 tablet.table_id(), tablet.start_key_hash(),
                 tablet.end_key_hash(), config->localLocator.c_str(),
                 serverId.toString().c_str());
        tablet.set_service_locator(config->localLocator);
        tablet.set_server_id(serverId.getId());
        tablet.set_ctime_log_head_id(headOfLog.getSegmentId());
        tablet.set_ctime_log_head_offset(headOfLog.getSegmentOffset());
    }
    LOG(NOTICE, "Reporting completion of recovery %lu", reqHdr->recoveryId);
    CoordinatorClient::recoveryMasterFinished(context, recoveryId,
                                              serverId, &recoveryTablets,
                                              successful);

    // TODO(stutsman) Delete tablets if recoveryMasterFinished returns
    // failure by setting successful to false. Rest is handled below.

    if (successful) {
        // Ok - we're expected to be serving now. Mark recovered tablets
        // as normal so we can handle clients.
        foreach (ProtoBuf::Tablets_Tablet* newTablet, newTablets)
            newTablet->set_state(ProtoBuf::Tablets::Tablet::NORMAL);
    } else {
        LOG(WARNING, "Failed to recover partition for recovery %lu; "
            "aborting recovery on this recovery master", recoveryId);
        // If recovery failed then cleanup all objects written by
        // recovery before starting to serve requests again.
        foreach (ProtoBuf::Tablets_Tablet* newTablet, newTablets) {
            for (int i = 0; i < tablets->tablet_size(); ++i) {
                const auto& tablet = tablets->tablet(i);
                if (tablet.table_id() == newTablet->table_id() &&
                    tablet.start_key_hash() == newTablet->start_key_hash() &&
                    tablet.end_key_hash() == newTablet->end_key_hash())
                {
                    TabletsOnMaster* table =
                            reinterpret_cast<TabletsOnMaster*>(
                                        tablet.user_data());
                    delete table;
                    tablets->mutable_tablet()->
                        SwapElements(tablets->tablet_size() - 1, i);
                    tablets->mutable_tablet()->RemoveLast();
                    break;
                }
            }
        }
        purgeObjectsFromUnknownTablets();
    }
}

/**
 * Top-level server method to handle the REMOVE request.
 *
 * \copydetails MasterService::read
 */
void
MasterService::remove(const WireFormat::Remove::Request* reqHdr,
                      WireFormat::Remove::Response* respHdr,
                      Rpc* rpc)
{
    const void* stringKey = rpc->requestPayload->getRange(sizeof32(*reqHdr),
                                                        reqHdr->keyLength);
    Key key(reqHdr->tableId, stringKey, reqHdr->keyLength);

    RejectRules rejectRules = reqHdr->rejectRules;
        respHdr->common.status = objectManager->removeObject(key, rejectRules);
}

/**
 * Top-level server method to handle the INCREMENT request.
 *
 * \copydetails MasterService::read
 */
void
MasterService::increment(const WireFormat::Increment::Request* reqHdr,
                     WireFormat::Increment::Response* respHdr,
                     Rpc* rpc)
{
    // Read the current value of the object and add the increment value
    Key key(reqHdr->tableId, *rpc->requestPayload, sizeof32(*reqHdr),
            reqHdr->keyLength);

    Status *status = &respHdr->common.status;

    Buffer value;
    RejectRules rejectRules = reqHdr->rejectRules;
    *status = objectManager->readObject(key, &value, rejectRules);
    if (respHdr->common.status != STATUS_OK)
        return;

    if (value.getTotalLength() != sizeof(int64_t)) {
        *status = STATUS_INVALID_OBJECT;
        return;
    }
                        
    const int64_t oldValue = *value.getOffset<int64_t>(0);
    int64_t newValue = oldValue + reqHdr->incrementValue;

    // Write the new value back
    Buffer newValueBuffer;
    newValueBuffer.append(&newValue, sizeof(int64_t));     

    *status = objectManager->writeObject(key, newValueBuffer,
        rejectRules, &respHdr->version);
    if (*status != STATUS_OK)
        return;
    objectManager->syncWrites();

    // Return new value
    respHdr->newValue = newValue;
}

/**
 * RPC handler for IS_REPLICA_NEEDED; indicates to backup servers whether
 * a replica for a particular segment that this master generated is needed
 * for durability or that it can be safely discarded.
 */
void
MasterService::isReplicaNeeded(
    const WireFormat::IsReplicaNeeded::Request* reqHdr,
    WireFormat::IsReplicaNeeded::Response* respHdr,
    Rpc* rpc)
{
    ServerId backupServerId = ServerId(reqHdr->backupServerId);
    respHdr->needed = objectManager->replicaManager.isReplicaNeeded(backupServerId,
                                                    reqHdr->segmentId);
}

/**
 * Top-level server method to handle the WRITE request.
 *
 * \copydetails MasterService::read
 */
void
MasterService::write(const WireFormat::Write::Request* reqHdr,
                     WireFormat::Write::Response* respHdr,
                     Rpc* rpc)
{
    Buffer buffer;
    const void* objectData = rpc->requestPayload->getRange(
        sizeof32(*reqHdr) + reqHdr->keyLength, reqHdr->length);
    buffer.append(objectData, reqHdr->length);

    Key key(reqHdr->tableId,
            *rpc->requestPayload,
            sizeof32(*reqHdr),
            reqHdr->keyLength);

    RejectRules rejectRules = reqHdr->rejectRules;
    respHdr->common.status = objectManager->writeObject(key,
        buffer, rejectRules, &respHdr->version);
    objectManager->syncWrites();
}

/**
 * Construct a Disabler object (disable the associated master).
 *
 * \param service
 *      The MasterService that should be disabled.  If NULL, then no
 *      service is disabled.
 */
MasterService::Disabler::Disabler(MasterService* service)
    : service(service)
{
    if (service != NULL) {
        service->disableCount++;
    }
    TEST_LOG("master service disabled");
}
/**
 * Destroy a Disabler object (reenable the associated master).
 */
MasterService::Disabler::~Disabler()
{
    reenable();
}

/**
 * Reenable request servicing on the associated MasterService.
 */
void
MasterService::Disabler::reenable()
{
    if (service != NULL) {
        service->disableCount--;
        service = NULL;
    }
}

} // namespace RAMCloud
