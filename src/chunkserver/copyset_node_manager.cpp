/*
 *  Copyright (c) 2020 NetEase Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/*
 * Project: curve
 * Created Date: 18-8-23
 * Author: wudemiao
 */

#include "src/chunkserver/copyset_node_manager.h"

#include <glog/logging.h>
#include <braft/file_service.h>
#include <braft/node_manager.h>

#include <vector>
#include <string>
#include <utility>

#include "src/common/string_util.h"
#include "src/common/timeutility.h"
#include "src/chunkserver/chunk_service.h"
#include "src/chunkserver/op_request.h"
#include "src/chunkserver/copyset_service.h"
#include "src/chunkserver/braft_cli_service.h"
#include "src/chunkserver/braft_cli_service2.h"
#include "src/chunkserver/uri_paser.h"
#include "src/chunkserver/raftsnapshot/curve_file_service.h"


namespace curve {
namespace chunkserver {

using curve::common::TimeUtility;

std::once_flag addServiceFlag;

int CopysetNodeManager::Init(const CopysetNodeOptions &copysetNodeOptions) {
    copysetNodeOptions_ = copysetNodeOptions;
    if (copysetNodeOptions_.loadConcurrency > 0) {
        copysetLoader_ = std::make_shared<TaskThreadPool<>>();
    } else {
        copysetLoader_ = nullptr;
    }
    return 0;
}

int CopysetNodeManager::Run() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return 0;
    }

    int ret = 0;
    // Start the thread pool
    if (copysetLoader_ != nullptr) {
        ret = copysetLoader_->Start(
            copysetNodeOptions_.loadConcurrency);
        if (ret < 0) {
            LOG(ERROR) << "CopysetLoadThrottle start error. ThreadNum: "
                       << copysetNodeOptions_.loadConcurrency;
            return -1;
        }
    }

    // Load existing copyset
    ret = ReloadCopysets();
    if (ret == 0) {
        loadFinished_.exchange(true, std::memory_order_acq_rel);
        LOG(INFO) << "Reload copysets success.";
    }
    return ret;
}

int CopysetNodeManager::Fini() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return 0;
    }
    loadFinished_.exchange(false, std::memory_order_acq_rel);

    if (copysetLoader_ != nullptr) {
        copysetLoader_->Stop();
        copysetLoader_ = nullptr;
    }

    {
        ReadLockGuard readLockGuard(rwLock_);
        for (auto& copysetNode : copysetNodeMap_) {
            copysetNode.second->Fini();
        }
    }

    WriteLockGuard writeLockGuard(rwLock_);
    copysetNodeMap_.clear();

    return 0;
}

int CopysetNodeManager::ReloadCopysets() {
    std::string datadir =
        UriParser::GetPathFromUri(copysetNodeOptions_.chunkDataUri);
    if (!copysetNodeOptions_.localFileSystem->DirExists(datadir)) {
        LOG(INFO) << datadir << " not exist. copysets was never created";
        return 0;
    }

    vector<std::string> items;
    if (copysetNodeOptions_.localFileSystem->List(datadir, &items) != 0) {
        LOG(ERROR) << "Failed to get copyset list from data directory "
                   << datadir;
        return -1;
    }

    vector<std::string>::iterator it = items.begin();
    for (; it != items.end(); ++it) {
        LOG(INFO) << "Found copyset dir " << *it;

        uint64_t groupId;
        if (false == ::curve::common::StringToUll(*it, &groupId)) {
            LOG(ERROR) << "parse " << *it << " to graoupId err";
            return -1;
        }
        uint64_t poolId = GetPoolID(groupId);
        uint64_t copysetId = GetCopysetID(groupId);
        LOG(INFO) << "Parsed groupid " << groupId
                  << " as " << ToGroupIdString(poolId, copysetId);

        if (copysetLoader_ == nullptr) {
            LoadCopyset(poolId, copysetId, false);
        } else {
            copysetLoader_->Enqueue(
                std::bind(&CopysetNodeManager::LoadCopyset,
                          this,
                          poolId,
                          copysetId,
                          true));
        }
    }

    // If the load is successful, wait for all copysets to finish loading and
    // close the thread pool
    if (copysetLoader_ != nullptr) {
        while (copysetLoader_->QueueSize() != 0) {
            ::sleep(1);
        }
        // The queue size is 0, but the threads in the pool may still be
        // executing.
        // Stop will internally join thread to ensure that all tasks are
        // executed before exiting.
        copysetLoader_->Stop();
        copysetLoader_ = nullptr;
    }

    return 0;
}

bool CopysetNodeManager::LoadFinished() {
    return loadFinished_.load(std::memory_order_acquire);
}

void CopysetNodeManager::LoadCopyset(const LogicPoolID &logicPoolId,
                                     const CopysetID &copysetId,
                                     bool needCheckLoadFinished) {
    LOG(INFO) << "Begin to load copyset "
              << ToGroupIdString(logicPoolId, copysetId)
              << ". check load finished? : "
              << (needCheckLoadFinished ? "Yes." : "No.");

    uint64_t beginTime = TimeUtility::GetTimeofDayMs();
    // External requests to create a copyset are rejected when the
    // chunkserver  starts loading the copyset.
    // So no other threads will load or create the same copyset, no lock is
    // needed at this point.
    Configuration conf;
    std::shared_ptr<CopysetNode> copysetNode =
        CreateCopysetNodeUnlocked(logicPoolId, copysetId, conf);
    if (copysetNode == nullptr) {
        LOG(ERROR) << "Failed to create copyset "
                   << ToGroupIdString(logicPoolId, copysetId);
        return;
    }
    if (!InsertCopysetNodeIfNotExist(logicPoolId, copysetId, copysetNode)) {
        LOG(ERROR) << "Failed to insert copyset "
                   << ToGroupIdString(logicPoolId, copysetId);
        return;
    }
    if (needCheckLoadFinished) {
        std::shared_ptr<CopysetNode> node =
            GetCopysetNode(logicPoolId, copysetId);
        CheckCopysetUntilLoadFinished(node);
    }
    LOG(INFO) << "Load copyset " << ToGroupIdString(logicPoolId, copysetId)
              << " end, time used (ms): "
              <<  TimeUtility::GetTimeofDayMs() - beginTime;
}

bool CopysetNodeManager::CheckCopysetUntilLoadFinished(
    std::shared_ptr<CopysetNode> node) {
    if (node == nullptr) {
        LOG(WARNING) << "CopysetNode ptr is null.";
        return false;
    }
    uint32_t retryTimes = 0;
    LogicPoolID logicPoolId = node->GetLogicPoolId();
    CopysetID copysetId = node->GetCopysetId();

    while (retryTimes < copysetNodeOptions_.checkRetryTimes) {
        if (!running_.load(std::memory_order_acquire)) {
            return false;
        }
        NodeStatus leaderStaus;
        bool getSuccess = node->GetLeaderStatus(&leaderStaus);
        // Failure to get the leader status is usually due to the fact that
        // no leader has been elected or the leader heartbeat has not been sent
        // to the current node.
        // The leader information can be obtained through several retries,
        // if it is not obtained through several retries, the copyset may not
        // be able to select a leader at the moment and will exit directly.
        if (!getSuccess) {
            ++retryTimes;
            ::usleep(1000 * copysetNodeOptions_.electionTimeoutMs);
            continue;
        }

        NodeStatus status;
        node->GetStatus(&status);
        // The last log of the current copy lags behind the first log saved
        // on the leader
        // In this case the copy will be restored by installing a snapshot,
        // which can be ignored to avoid blocking the check thread
        bool mayInstallSnapshot = leaderStaus.first_index > status.last_index;
        if (mayInstallSnapshot) {
            LOG(WARNING) << "Copyset "
                         << ToGroupIdString(logicPoolId, copysetId)
                         << " may installing snapshot, "
                         << "stop checking. "
                         << "fist log index on leader: "
                         << leaderStaus.first_index
                         << ", last log index on current node: "
                         << status.last_index;
            return false;
        }

        // Check if the applied log of the current copy is close to the one
        // that has been committed
        int64_t margin = leaderStaus.committed_index
                       - status.known_applied_index;
        bool catchupLeader = margin
                           < (int64_t)copysetNodeOptions_.finishLoadMargin;
        if (catchupLeader) {
            LOG(INFO) << "Load copyset "
                      << ToGroupIdString(logicPoolId, copysetId)
                      << " finished, "
                      << "leader CommittedIndex: "
                      << leaderStaus.committed_index
                      << ", node appliedIndex: "
                      << status.known_applied_index;
            return true;
        }
        retryTimes = 0;
        ::usleep(1000 * copysetNodeOptions_.checkLoadMarginIntervalMs);
    }
    LOG(WARNING) << "check copyset "
                 << ToGroupIdString(logicPoolId, copysetId)
                 << " failed.";
    return false;
}

std::shared_ptr<CopysetNode> CopysetNodeManager::GetCopysetNode(
    const LogicPoolID &logicPoolId, const CopysetID &copysetId) const {
    /* Add read lock */
    ReadLockGuard readLockGuard(rwLock_);
    GroupId groupId = ToGroupId(logicPoolId, copysetId);
    auto it = copysetNodeMap_.find(groupId);
    if (copysetNodeMap_.end() != it)
        return it->second;

    return nullptr;
}

void CopysetNodeManager::GetAllCopysetNodes(
    std::vector<CopysetNodePtr> *nodes) const {
    /* Add read lock */
    ReadLockGuard readLockGuard(rwLock_);
    for (auto it = copysetNodeMap_.begin(); it != copysetNodeMap_.end(); ++it) {
        nodes->push_back(it->second);
    }
}

bool CopysetNodeManager::CreateCopysetNode(const LogicPoolID &logicPoolId,
                                           const CopysetID &copysetId,
                                           const Configuration &conf) {
    GroupId groupId = ToGroupId(logicPoolId, copysetId);
    // External copyset creation is not allowed if the local copyset has not
    // yet been fully loaded
    if (!loadFinished_.load(std::memory_order_acquire)) {
        LOG(WARNING) << "Create copyset failed: load unfinished "
                     << ToGroupIdString(logicPoolId, copysetId);
        return false;
    }
    // The copysetnode will call shutdown when it is destructed, which may
    // cause the concurrent process to cut out
    // So when the creation of a copysetnode fails, you can't hold the write
    // lock and wait for it to be released before destructing
    std::shared_ptr<CopysetNode> copysetNode = nullptr;
    /* Add write lock */
    WriteLockGuard writeLockGuard(rwLock_);
    if (copysetNodeMap_.end() == copysetNodeMap_.find(groupId)) {
        copysetNode = std::make_shared<CopysetNode>(logicPoolId,
                                                    copysetId,
                                                    conf);
        if (0 != copysetNode->Init(copysetNodeOptions_)) {
            LOG(ERROR) << "Copyset " << ToGroupIdString(logicPoolId, copysetId)
                    << " init failed";
            return false;
        }
        if (0 != copysetNode->Run()) {
            LOG(ERROR) << "Copyset " << ToGroupIdString(logicPoolId, copysetId)
                       << " run failed";
            return false;
        }

        copysetNodeMap_.insert(std::pair<GroupId, std::shared_ptr<CopysetNode>>(
            groupId,
            copysetNode));
        LOG(INFO) << "Create copyset success "
                  << ToGroupIdString(logicPoolId, copysetId);
        return true;
    }
    LOG(WARNING) << "Copyset node is already exists "
                 << ToGroupIdString(logicPoolId, copysetId);
    return false;
}

bool CopysetNodeManager::CreateCopysetNode(const LogicPoolID &logicPoolId,
                                           const CopysetID &copysetId,
                                           const std::vector<Peer> peers) {
    Configuration conf;
    for (Peer peer : peers) {
        conf.add_peer(PeerId(peer.address()));
    }

    return CreateCopysetNode(logicPoolId, copysetId, conf);
}

std::shared_ptr<CopysetNode> CopysetNodeManager::CreateCopysetNodeUnlocked(
    const LogicPoolID &logicPoolId,
    const CopysetID &copysetId,
    const Configuration &conf) {
    std::shared_ptr<CopysetNode> copysetNode =
        std::make_shared<CopysetNode>(logicPoolId,
                                        copysetId,
                                        conf);
    if (0 != copysetNode->Init(copysetNodeOptions_)) {
        LOG(ERROR) << "Copyset " << ToGroupIdString(logicPoolId, copysetId)
                   << " init failed";
        return nullptr;
    }
    if (0 != copysetNode->Run()) {
        copysetNode->Fini();
        LOG(ERROR) << "Copyset " << ToGroupIdString(logicPoolId, copysetId)
                   << " run failed";
        return nullptr;
    }

    return copysetNode;
}

int CopysetNodeManager::AddService(brpc::Server *server,
                                   const butil::EndPoint &listenAddress) {
    int ret = 0;
    uint64_t maxInflight = 100;
    std::shared_ptr<InflightThrottle> inflightThrottle
        = std::make_shared<InflightThrottle>(maxInflight);
    CopysetNodeManager *copysetNodeManager = this;
    ChunkServiceOptions chunkServiceOptions;
    chunkServiceOptions.copysetNodeManager = copysetNodeManager;
    chunkServiceOptions.inflightThrottle = inflightThrottle;

    do {
        if (nullptr == server) {
            LOG(ERROR) << "server is NULL";
            ret = -1;
            break;
        }
        // We need call braft::add_service to add endPoint to braft::NodeManager
        braft::add_service(server, listenAddress);
        // We need to replace braft::CliService with our own implementation
        auto service = server->FindServiceByName("CliService");
        ret = server->RemoveService(service);
        CHECK(0 == ret) << "Fail to remove braft::CliService";
        ret = server->AddService(new BRaftCliServiceImpl,
                                 brpc::SERVER_OWNS_SERVICE);
        CHECK(0 == ret) << "Fail to add BRaftCliService";
        // We need to replace braft::FileServiceImpl with our own implementation
        service = server->FindServiceByName("FileService");
        ret = server->RemoveService(service);
        CHECK(0 == ret) << "Fail to remove braft::FileService";
        ret = server->AddService(&kCurveFileService,
        brpc::SERVER_DOESNT_OWN_SERVICE);
        CHECK(0 == ret) << "Fail to add CurveFileService";

        // add other services
        ret = server->AddService(new BRaftCliServiceImpl2,
                                 brpc::SERVER_OWNS_SERVICE);
        CHECK(0 == ret) << "Fail to add BRaftCliService2";

        ret = server->AddService(new CopysetServiceImpl(copysetNodeManager),
                                 brpc::SERVER_OWNS_SERVICE);
        CHECK(0 == ret) << "Fail to add CopysetService";
        ret = server->AddService(new ChunkServiceImpl(chunkServiceOptions),
                                 brpc::SERVER_OWNS_SERVICE);
        CHECK(0 == ret) << "Fail to add ChunkService";
    } while (false);

    return ret;
}

bool CopysetNodeManager::DeleteCopysetNode(const LogicPoolID &logicPoolId,
                                           const CopysetID &copysetId) {
    bool ret = false;
    GroupId groupId = ToGroupId(logicPoolId, copysetId);

    {
        // Add read lock
        ReadLockGuard readLockGuard(rwLock_);
        auto it = copysetNodeMap_.find(groupId);
        if (copysetNodeMap_.end() != it) {
            // TODO(yyk) This part may be at risk of deadlock and will need
            //  to be assessed subsequently
            it->second->Fini();
            ret = true;
        }
    }

    {
        // Add write lock
        WriteLockGuard writeLockGuard(rwLock_);
        auto it = copysetNodeMap_.find(groupId);
        if (copysetNodeMap_.end() != it) {
            copysetNodeMap_.erase(it);
            ret = true;
            LOG(INFO) << "Delete copyset "
                      << ToGroupIdString(logicPoolId, copysetId)
                      <<" success.";
        }
    }

    return ret;
}

bool CopysetNodeManager::PurgeCopysetNodeData(const LogicPoolID &logicPoolId,
                                              const CopysetID &copysetId) {
    bool ret = false;
    GroupId groupId = ToGroupId(logicPoolId, copysetId);

    {
        // Add read lock
        ReadLockGuard readLockGuard(rwLock_);
        auto it = copysetNodeMap_.find(groupId);
        if (copysetNodeMap_.end() != it) {
            // TODO(yyk) 这This part may be at risk of deadlock and will need
            //  to be assessed subsequently
            it->second->Fini();
            ret = true;
        }
    }

    {
        // Add write lock
        WriteLockGuard writeLockGuard(rwLock_);
        auto it = copysetNodeMap_.find(groupId);
        if (copysetNodeMap_.end() != it) {
            if (0 != copysetNodeOptions_.trash->RecycleCopySet(
                it->second->GetCopysetDir())) {
                LOG(ERROR) << "Failed to remove copyset "
                           << ToGroupIdString(logicPoolId, copysetId)
                           << " persistently.";
                ret = false;
            }
            LOG(INFO) << "Move copyset"
                      << ToGroupIdString(logicPoolId, copysetId)
                      << "to trash success.";
            copysetNodeMap_.erase(it);
            ret = true;
        }
    }

    return ret;
}

bool CopysetNodeManager::IsExist(const LogicPoolID &logicPoolId,
                                 const CopysetID &copysetId) {
    /* Add read lock */
    ReadLockGuard readLockGuard(rwLock_);
    GroupId groupId = ToGroupId(logicPoolId, copysetId);
    return copysetNodeMap_.end() != copysetNodeMap_.find(groupId);
}

bool CopysetNodeManager::InsertCopysetNodeIfNotExist(
    const LogicPoolID &logicPoolId, const CopysetID &copysetId,
    std::shared_ptr<CopysetNode> node) {
    /* Add write lock */
    WriteLockGuard writeLockGuard(rwLock_);
    GroupId groupId = ToGroupId(logicPoolId, copysetId);
    auto it = copysetNodeMap_.find(groupId);
    if (copysetNodeMap_.end() == it) {
        copysetNodeMap_.insert(
            std::pair<GroupId, std::shared_ptr<CopysetNode>>(groupId, node));
        LOG(INFO) << "Insert copyset success "
                  << ToGroupIdString(logicPoolId, copysetId);
        return true;
    }
    LOG(WARNING) << "Copyset node is already exists "
                 << ToGroupIdString(logicPoolId, copysetId);
    return false;
}

}  // namespace chunkserver
}  // namespace curve
