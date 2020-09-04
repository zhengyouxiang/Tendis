#include <list>
#include <chrono>
#include <fstream>
#include <string>
#include <memory>

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "glog/logging.h"
#include "tendisplus/replication/repl_manager.h"
#include "tendisplus/utils/scopeguard.h"

namespace tendisplus {

bool ReplManager::supplyFullSync(asio::ip::tcp::socket sock,
                        const std::string& storeIdArg,
                        const std::string& slaveIpArg,
                        const std::string& slavePortArg) {
    std::shared_ptr<BlockingTcpClient> client =
        std::move(_svr->getNetwork()->createBlockingClient(
            std::move(sock), 64*1024*1024));

    // NOTE(deyukong): this judge is not precise
    // even it's not full at this time, it can be full during schedule.
    if (isFullSupplierFull()) {
        LOG(WARNING) << "ReplManager::supplyFullSync fullPusher isFull.";
        client->writeLine("-ERR workerpool full");
        return false;
    }

    auto expStoreId = tendisplus::stoul(storeIdArg);
    if (!expStoreId.ok()) {
        LOG(ERROR) << "ReplManager::supplyFullSync storeIdArg error:" << storeIdArg;
        client->writeLine("-ERR invalid storeId");
        return false;
    }
    uint32_t storeId = static_cast<uint32_t>(expStoreId.value());

    auto expSlavePort = tendisplus::stoul(slavePortArg);
    if (!expSlavePort.ok()) {
        LOG(ERROR) << "ReplManager::supplyFullSync expSlavePort error:" << slavePortArg;
        client->writeLine("-ERR invalid expSlavePort");
        return false;
    }
    LOG(INFO) << "ReplManager::supplyFullSync storeId:" << storeIdArg
        << " " << slaveIpArg << ":" << slavePortArg;
    uint16_t slavePort = static_cast<uint16_t>(expSlavePort.value());
    _fullPusher->schedule(
        [this, storeId, client(std::move(client)), slaveIpArg, slavePort]() mutable {
            supplyFullSyncRoutine(std::move(client), storeId, slaveIpArg, slavePort);
    });

    return true;
}

bool ReplManager::isFullSupplierFull() const {
    return _fullPusher->isFull();
}

void ReplManager::masterPushRoutine(uint32_t storeId, uint64_t clientId) {
    SCLOCK::time_point nextSched = SCLOCK::now();
    SCLOCK::time_point lastSend = SCLOCK::time_point::min();
    auto guard = MakeGuard([this, &nextSched, &lastSend, storeId, clientId] {
        std::lock_guard<std::mutex> lk(_mutex);
        auto& mpov = _pushStatus[storeId];
        if (mpov.find(clientId) == mpov.end()) {
            return;
        }
        INVARIANT_D(mpov[clientId]->isRunning);
        mpov[clientId]->isRunning = false;
        if (nextSched > mpov[clientId]->nextSchedTime) {
            mpov[clientId]->nextSchedTime = nextSched;
        }
        if (lastSend > mpov[clientId]->lastSendBinlogTime) {
            mpov[clientId]->lastSendBinlogTime = lastSend;
        }
        // currently nothing waits for master's push process
        // _cv.notify_all();
    });

    uint64_t binlogPos = 0;
    BlockingTcpClient *client = nullptr;
    uint32_t dstStoreId = 0;
    bool needHeartbeat = false;
    {
        std::lock_guard<std::mutex> lk(_mutex);
        if (_incrPaused ||
                _pushStatus[storeId].find(clientId) ==
                            _pushStatus[storeId].end()) {
            nextSched = nextSched + std::chrono::seconds(1);
            lastSend = _pushStatus[storeId][clientId]->lastSendBinlogTime;
            return;
        }
        binlogPos = _pushStatus[storeId][clientId]->binlogPos;
        client = _pushStatus[storeId][clientId]->client.get();
        dstStoreId = _pushStatus[storeId][clientId]->dstStoreId;
        lastSend = _pushStatus[storeId][clientId]->lastSendBinlogTime;
    }
#ifdef BINLOG_V1
    Expected<uint64_t> newPos =
            masterSendBinlog(client, storeId, dstStoreId, binlogPos);
#else
    if (lastSend + std::chrono::seconds(gBinlogHeartbeatSecs)
                < SCLOCK::now()) {
        needHeartbeat = true;
    }

    Expected<uint64_t> newPos =
            masterSendBinlogV2(client, storeId, dstStoreId,
                        binlogPos, needHeartbeat, _svr, _cfg);
#endif
    if (!newPos.ok()) {
        LOG(WARNING) << "masterSendBinlog to client:"
                << client->getRemoteRepr() << " failed:"
                << newPos.status().toString();
        std::lock_guard<std::mutex> lk(_mutex);
#if defined(WIN32) && _MSC_VER > 1900
        if (_pushStatus[storeId][clientId] != nullptr) {
            delete _pushStatus[storeId][clientId];
        }
#endif
        // it is safe to remove a non-exist key
        _pushStatus[storeId].erase(clientId);
        return;
    } else {
        if (newPos.value() > binlogPos) {
            nextSched = SCLOCK::now();
            lastSend = nextSched;
        } else {
            nextSched = SCLOCK::now() + std::chrono::seconds(1);
            if (needHeartbeat) {
                lastSend = SCLOCK::now();
            }
        }
        std::lock_guard<std::mutex> lk(_mutex);
        _pushStatus[storeId][clientId]->binlogPos = newPos.value();
    }
}

#ifdef BINLOG_V1
Expected<uint64_t> ReplManager::masterSendBinlog(BlockingTcpClient* client,
                uint32_t storeId, uint32_t dstStoreId, uint64_t binlogPos) {
    constexpr uint32_t suggestBatch = 64;
    constexpr size_t suggestBytes = 16*1024*1024;

    LocalSessionGuard sg(_svr.get());
    sg.getSession()->setArgs(
        {"mastersendlog",
         std::to_string(storeId),
         client->getRemoteRepr(),
         std::to_string(dstStoreId),
         std::to_string(binlogPos)});

    auto expdb = _svr->getSegmentMgr()->getDb(sg.getSession(),
                    storeId, mgl::LockMode::LOCK_IS);
    if (!expdb.ok()) {
        return expdb.status();
    }
    auto store = std::move(expdb.value().store);
    INVARIANT(store != nullptr);

    auto ptxn = store->createTransaction(sg.getSession());
    if (!ptxn.ok()) {
        return ptxn.status();
    }

    std::unique_ptr<Transaction> txn = std::move(ptxn.value());
    std::unique_ptr<BinlogCursor> cursor =
        txn->createBinlogCursor(binlogPos+1);

    std::vector<ReplLog> binlogs;
    uint32_t cnt = 0;
    uint64_t nowId = 0;
    size_t estimateSize = 0;

    while (true) {
        Expected<ReplLog> explog = cursor->next();
        if (explog.ok()) {
            cnt += 1;
            const ReplLogKey& rlk = explog.value().getReplLogKey();
            const ReplLogValue& rlv = explog.value().getReplLogValue();
            estimateSize += rlv.getOpValue().size();
            if (nowId == 0 || nowId != rlk.getTxnId()) {
                nowId = rlk.getTxnId();
                if (cnt >= suggestBatch || estimateSize >= suggestBytes) {
                    break;
                } else {
                    binlogs.emplace_back(std::move(explog.value()));
                }
            } else {
                binlogs.emplace_back(std::move(explog.value()));
            }
        } else if (explog.status().code() == ErrorCodes::ERR_EXHAUST) {
            // no more data
            break;
        } else {
            LOG(ERROR) << "iter binlog failed:"
                        << explog.status().toString();
            return explog.status();
        }
    }

    std::stringstream ss;
    Command::fmtMultiBulkLen(ss, binlogs.size()*2 + 2);
    Command::fmtBulk(ss, "applybinlogs");
    Command::fmtBulk(ss, std::to_string(dstStoreId));
    for (auto& v : binlogs) {
        ReplLog::KV kv = v.encode();
        Command::fmtBulk(ss, kv.first);
        Command::fmtBulk(ss, kv.second);
    }
    std::string stringtoWrite = ss.str();
    uint32_t secs = 1;
    if (stringtoWrite.size() > 1024*1024) {
        secs = 2;
    } else if (stringtoWrite.size() > 1024*1024*10) {
        secs = 4;
    }
    Status s = client->writeData(stringtoWrite);
    if (!s.ok()) {
        return s;
    }

    // TODO(vinchen): NO NEED TO READ OK
    Expected<std::string> exptOK = client->readLine(std::chrono::seconds(secs));
    if (!exptOK.ok()) {
        return exptOK.status();
    } else if (exptOK.value() != "+OK") {
        LOG(WARNING) << "store:" << storeId << " dst Store:" << dstStoreId
                     << " apply binlogs failed:" << exptOK.value();
        return {ErrorCodes::ERR_NETWORK, "bad return string"};
    }

    if (binlogs.size() == 0) {
        return binlogPos;
    } else {
        return binlogs[binlogs.size()-1].getReplLogKey().getTxnId();
    }
}
#else

#endif

//  1) s->m INCRSYNC (m side: session2Client)
//  2) m->s +OK
//  3) s->m +PONG (s side: client2Session)
//  4) m->s periodly send binlogs
//  the 3) step is necessary, if ignored, the +OK in step 2) and binlogs
//  in step 4) may sticky together. and redis-resp protocal is not fixed-size
//  That makes client2Session complicated.

// NOTE(deyukong): we define binlogPos the greatest id that has been applied.
// "NOT" the smallest id that has not been applied. keep the same with
// BackupInfo's setCommitId
bool ReplManager::registerIncrSync(asio::ip::tcp::socket sock,
            const std::string& storeIdArg,
            const std::string& dstStoreIdArg,
            const std::string& binlogPosArg,
            const std::string& listenIpArg,
            const std::string& listenPortArg) {
    std::shared_ptr<BlockingTcpClient> client =
        std::move(_svr->getNetwork()->createBlockingClient(
            std::move(sock), 64*1024*1024));

    uint32_t storeId;
    uint32_t  dstStoreId;
    uint64_t binlogPos;
    uint16_t listen_port;
    try {
        storeId = std::stoul(storeIdArg);
        dstStoreId = std::stoul(dstStoreIdArg);
        binlogPos = std::stoull(binlogPosArg);
        listen_port = std::stoull(listenPortArg);
    } catch (const std::exception& ex) {
        std::stringstream ss;
        ss << "-ERR parse opts failed:" << ex.what();
        client->writeLine(ss.str());
        return false;
    }

    if (storeId >= _svr->getKVStoreCount() ||
            dstStoreId >= _svr->getKVStoreCount()) {
        client->writeLine("-ERR invalid storeId");
        return false;
    }

    // NOTE(vinchen): In the cluster view, storeID of source and dest must be
    // same.
    if (storeId != dstStoreId) {
        client->writeLine("-ERR source storeId is different from dstStoreId ");
        return false;
    }

    LocalSessionGuard sg(_svr.get());
    auto expdb = _svr->getSegmentMgr()->getDb(sg.getSession(),
        storeId, mgl::LockMode::LOCK_IS);
    if (!expdb.ok()) {
        std::stringstream ss;
        ss << "-ERR store " << storeId << " error: "
            << expdb.status().toString();
        client->writeLine(ss.str());
        return false;
    }

    uint64_t firstPos = 0;
    uint64_t lastFlushBinlogId = 0;
    {
        std::lock_guard<std::mutex> lk(_mutex);
        firstPos = _logRecycStatus[storeId]->firstBinlogId;
        lastFlushBinlogId = _logRecycStatus[storeId]->lastFlushBinlogId;
    }

    // NOTE(deyukong): this check is not precise
    // (not in the same critical area with the modification to _pushStatus),
    // but it does not harm correctness.
    // A strict check may be too complicated to read.
    // NOTE(takenliu): 1.recycleBinlog use firstPos, and incrSync use binlogPos+1
    //     2. slave do command slaveof master, master do flushall and truncateBinlogV2,
    //        slave send binlogpos will smaller than master.
    if (firstPos > (binlogPos + 1) && firstPos != lastFlushBinlogId) {
        std::stringstream ss;
        ss << "-ERR invalid binlogPos,storeId:" << storeId
            << ",master firstPos:" << firstPos
            << ",slave binlogPos:" << binlogPos
            << ",lastFlushBinlogId:" << lastFlushBinlogId;
        client->writeLine(ss.str());
        LOG(ERROR) << ss.str();
        return false;
    }
    client->writeLine("+OK");
    Expected<std::string> exptPong = client->readLine(std::chrono::seconds(1));
    if (!exptPong.ok()) {
        LOG(WARNING) << "slave incrsync handshake failed:"
                << exptPong.status().toString();
        return false;
    } else if (exptPong.value() != "+PONG") {
        LOG(WARNING) << "slave incrsync handshake not +PONG:"
                << exptPong.value();
        return false;
    }

    std::string remoteHost = client->getRemoteRepr();
    bool registPosOk =
            [this,
             storeId,
             dstStoreId,
             binlogPos,
             client = std::move(client),
             listenIpArg,
             listen_port]() mutable {
        std::lock_guard<std::mutex> lk(_mutex);
        // takenliu: recycleBinlog use firstPos, and incrSync use binlogPos+1
        if (_logRecycStatus[storeId]->firstBinlogId > (binlogPos+1) &&
            _logRecycStatus[storeId]->firstBinlogId !=
            _logRecycStatus[storeId]->lastFlushBinlogId) {
            std::stringstream ss;
            ss << "-ERR invalid binlogPos,storeId:" << storeId
                << ",master firstPos:" << _logRecycStatus[storeId]->firstBinlogId
                << ",slave binlogPos:" << binlogPos
                << ",lastFlushBinlogId:" << _logRecycStatus[storeId]->lastFlushBinlogId;
            LOG(ERROR) << ss.str();
            return false;
        }

        string slaveNode = listenIpArg + ":" + to_string(listen_port);
        auto iter = _fullPushStatus[storeId].find(slaveNode);
        if (iter != _fullPushStatus[storeId].end()) {
            LOG(INFO) << "registerIncrSync erase _fullPushStatus, " << iter->second->toString();
            _fullPushStatus[storeId].erase(iter);
        }

        uint64_t clientId = _clientIdGen.fetch_add(1);
#if defined(_WIN32) && _MSC_VER > 1900
        _pushStatus[storeId][clientId] =
                new MPovStatus {
                     false,
                     static_cast<uint32_t>(dstStoreId),
                     binlogPos,
                     SCLOCK::now(),
                     SCLOCK::time_point::min(),
                     std::move(client),
                     clientId,
                     listenIpArg,
                     listen_port};
#else
        _pushStatus[storeId][clientId] =
            std::move(std::unique_ptr<MPovStatus>(
                new MPovStatus {
                     false,
                     static_cast<uint32_t>(dstStoreId),
                     binlogPos,
                     SCLOCK::now(),
                     SCLOCK::time_point::min(),
                     std::move(client),
                     clientId,
                     listenIpArg,
                     listen_port}));
#endif
        return true;
    }();
    LOG(INFO) << "slave:" << remoteHost
            << " registerIncrSync " << (registPosOk ? "ok" : "failed");

    return registPosOk;
}

// mpov's network communicate procedure
// send binlogpos low watermark
// send filelist={filename->filesize}
// foreach file
//     send filename
//     send content
//     read +OK
// read +OK
void ReplManager::supplyFullSyncRoutine(
            std::shared_ptr<BlockingTcpClient> client, uint32_t storeId,
            const string& slave_listen_ip, uint16_t slave_listen_port) {
    LocalSessionGuard sg(_svr.get());
    sg.getSession()->setArgs(
        {"masterfullsync",
         client->getRemoteRepr(),
         std::to_string(storeId)});
    LOG(INFO) << "client:" << client->getRemoteRepr()
              << ",storeId:" << storeId
              << ",begins fullsync";
    auto expdb = _svr->getSegmentMgr()->getDb(sg.getSession(),
                storeId, mgl::LockMode::LOCK_IS);
    if (!expdb.ok()) {
        std::stringstream ss;
        ss << "-ERR store " << storeId << " error: "
            << expdb.status().toString();
        client->writeLine(ss.str());
        LOG(ERROR) << "getDb failed:" << expdb.status().toString();
        return;
    }
    auto store = std::move(expdb.value().store);
    INVARIANT(store != nullptr);

    if (!store->isRunning()) {
        client->writeLine("-ERR store is not running");
        LOG(ERROR) << "store is not running.";
        return;
    }

    {
        std::lock_guard<std::mutex> lk(_mutex);
        uint64_t highestBinlogid = store->getHighestBinlogId();
        string slaveNode = slave_listen_ip + ":" + to_string(slave_listen_port);
        auto iter = _fullPushStatus[storeId].find(slaveNode);
        if (iter != _fullPushStatus[storeId].end()) {
            LOG(INFO) << "supplyFullSyncRoutine already have _fullPushStatus, "
                << iter->second->toString();
            if (iter->second->state == FullPushState::ERR) {
                _fullPushStatus[storeId].erase(iter);
            } else {
                client->writeLine("-ERR already have _fullPushStatus, " + iter->second->toString());
                return;
            }
        }

        uint64_t clientId = _clientIdGen.fetch_add(1);
#if defined(_WIN32) && _MSC_VER > 1900
        _fullPushStatus[storeId][slaveNode] =
            new MPovFullPushStatus{ storeId,
             FullPushState::PUSHING,
                   highestBinlogid,
                   SCLOCK::now(),
                   SCLOCK::time_point::min(),
                   client,
                   clientId,
                   slave_listen_ip,
                   slave_listen_port };
#else
        _fullPushStatus[storeId][slaveNode] =
            std::move(std::unique_ptr<MPovFullPushStatus>(
            new MPovFullPushStatus{storeId,
             FullPushState::PUSHING,
                   highestBinlogid,
                   SCLOCK::now(),
                   SCLOCK::time_point::min(),
                   client,
                   clientId,
                   slave_listen_ip,
                   slave_listen_port
                   } ));
#endif
    }
    bool hasError = true;
    auto guard_0 = MakeGuard([this, store, storeId, &hasError,
                              slave_listen_ip, slave_listen_port]() {
        std::lock_guard<std::mutex> lk(_mutex);
        string slaveNode = slave_listen_ip + ":" + to_string(slave_listen_port);
        auto iter = _fullPushStatus[storeId].find(slaveNode);
        if (iter != _fullPushStatus[storeId].end()) {
            if (hasError) {
                LOG(INFO) << "supplyFullSyncRoutine hasError, _fullPushStatus erase, "
                    << iter->second->toString();
                _fullPushStatus[storeId].erase(iter);
            } else {
                iter->second->endTime =  SCLOCK::now();
                iter->second->state = FullPushState::SUCESS;
            }
        } else {
            LOG(ERROR) << "supplyFullSyncRoutine, _fullPushStatus find node failed, storeid:"
                << storeId << " slave node:" << slaveNode;
        }
    });

    uint64_t currTime = nsSinceEpoch();
    Expected<BackupInfo> bkInfo = store->backup(
        store->dftBackupDir(),
        KVStore::BackupMode::BACKUP_CKPT_INTER,
        _svr->getCatalog()->getBinlogVersion());
    if (!bkInfo.ok()) {
        std::stringstream ss;
        ss << "-ERR backup failed:" << bkInfo.status().toString();
        client->writeLine(ss.str());
        LOG(ERROR) << "backup failed:" << bkInfo.status().toString();
        return;
    } else {
        LOG(INFO) << "storeId:" << storeId
                  << ",backup cost:" << (nsSinceEpoch() - currTime) << "ns"
                  << ",pos:" << bkInfo.value().getBinlogPos();
    }

    auto guard = MakeGuard([this, store, storeId]() {
        Status s = store->releaseBackup();
        if (!s.ok()) {
            LOG(ERROR) << "supplyFullSync end clean store:"
                    << storeId << " error:" << s.toString();
        }
    });

    // send binlogPos
    Status s = client->writeLine(
            std::to_string(bkInfo.value().getBinlogPos()));
    if (!s.ok()) {
        LOG(ERROR) << "store:" << storeId
                   << " fullsync send binlogpos failed:" << s.toString();
        return;
    }
    LOG(INFO) << "fullsync " << storeId << " send binlogPos success:"
        << bkInfo.value().getBinlogPos();

    // send fileList
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    writer.StartObject();
    for (const auto& kv : bkInfo.value().getFileList()) {
        writer.Key(kv.first.c_str());
        writer.Uint64(kv.second);
    }
    writer.EndObject();
    uint32_t secs = 10;
    s = client->writeLine(sb.GetString());
    if (!s.ok()) {
        LOG(ERROR) << "store:" << storeId
                   << " fullsync send filelist failed:" << s.toString();
        return;
    }
    LOG(INFO) << "fullsync " << storeId << " send fileList success:" << sb.GetString();

    std::string readBuf;
    size_t fileBatch = (_cfg->binlogRateLimitMB * 1024 * 1024) / 10;
    readBuf.reserve(fileBatch);
    for (auto& fileInfo : bkInfo.value().getFileList()) {
        s = client->writeLine(fileInfo.first);
        if (!s.ok()) {
            LOG(ERROR) << "write fname:" << fileInfo.first
                        << " to client failed:" << s.toString();
            return;
        }
        LOG(INFO) << "fulsync send filename success:" << fileInfo.first;
        std::string fname = store->dftBackupDir() + "/" + fileInfo.first;
        auto myfile = std::ifstream(fname, std::ios::binary);
        if (!myfile.is_open()) {
            LOG(ERROR) << "open file:" << fname << " for read failed";
            return;
        }
        size_t remain = fileInfo.second;
        while (remain) {
            size_t batchSize = std::min(remain, fileBatch);
            _rateLimiter->Request(batchSize);
            readBuf.resize(batchSize);
            remain -= batchSize;
            myfile.read(&readBuf[0], batchSize);
            if (!myfile) {
                LOG(ERROR) << "read file:" << fname
                            << " failed with err:" << strerror(errno);
                return;
            }
            s = client->writeData(readBuf);
            if (!s.ok()) {
                LOG(ERROR) << "write bulk to client failed:" << s.toString();
                return;
            }
            secs = _cfg->timeoutSecBinlogWaitRsp;  // 10
            auto rpl = client->readLine(std::chrono::seconds(secs));
            if (!rpl.ok() || rpl.value() != "+OK") {
                LOG(ERROR) << "send client:" << client->getRemoteRepr()
                           << "file:" << fileInfo.first
                           << ",size:" << fileInfo.second
                           << " failed:"
                           << (rpl.ok() ? rpl.value() : rpl.status().toString());      // NOLINT
                return;
            }
        }
        LOG(INFO) << "fulsync send file success:" << fname;
    }
    secs = _cfg->timeoutSecBinlogWaitRsp;  // 10
    Expected<std::string> reply = client->readLine(std::chrono::seconds(secs));
    if (!reply.ok()) {
        LOG(ERROR) << "fullsync done read "
                   << client->getRemoteRepr() << " reply failed:"
                   << reply.status().toString();
    } else {
        LOG(INFO) << "fullsync storeid:" << storeId << " done, read "
                  << client->getRemoteRepr() << "port" <<  slave_listen_port
                  << " reply:" << reply.value();
        hasError = false;
    }
}

}  // namespace tendisplus
