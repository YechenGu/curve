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
 * Created Date: 2019-12-03
 * Author: lixiaocui
 */

#include <butil/endpoint.h>
#include <brpc/channel.h>
#include <brpc/controller.h>
#include <string>
#include "src/chunkserver/heartbeat_helper.h"
#include "include/chunkserver/chunkserver_common.h"
#include "proto/chunkserver.pb.h"

namespace curve {
namespace chunkserver {
bool HeartbeatHelper::BuildNewPeers(
    const CopySetConf &conf, std::vector<Peer> *newPeers) {
    // Check if the target node and the node to be deleted are valid
    std::string target(conf.configchangeitem().address());
    std::string old(conf.oldpeer().address());
    if (!PeerVaild(target) || !PeerVaild(old)) {
        return false;
    }

    // Generate newPeers
    for (int i = 0; i < conf.peers_size(); i++) {
        std::string peer = conf.peers(i).address();
        // Check if peer in conf is valid
        if (!PeerVaild(peer)) {
            return false;
        }

        // newPeers does not contain old copies
        if (conf.peers(i).address() != old) {
            newPeers->emplace_back(conf.peers(i));
        }
    }

    newPeers->emplace_back(conf.configchangeitem());
    return true;
}

bool HeartbeatHelper::PeerVaild(const std::string &peer) {
    PeerId peerId;
    return 0 == peerId.parse(peer);
}

bool HeartbeatHelper::CopySetConfValid(
    const CopySetConf &conf, const CopysetNodePtr &copyset) {
    // The copyset to be changed does not exist in the chunkserver, alarm is
    // raised
    if (copyset == nullptr) {
        LOG(ERROR) << "Failed to find copyset(" << conf.logicalpoolid()
            << "," <<  conf.copysetid() << "), groupId: "
            << ToGroupIdStr(conf.logicalpoolid(), conf.copysetid());
        return false;
    }

    // The change epoch issued < copyset's actual epoch, error reported
    if (conf.epoch() < copyset->GetConfEpoch()) {
        LOG(WARNING) << "Config change epoch:" << conf.epoch()
                << " is smaller than current:" << copyset->GetConfEpoch()
                << " on copyset("
                << conf.logicalpoolid() << "," <<  conf.copysetid()
                << "), groupId: "
                << ToGroupIdStr(conf.logicalpoolid(), conf.copysetid())
                << ", refuse change";
        return false;
    }

    return true;
}

bool HeartbeatHelper::NeedPurge(const butil::EndPoint &csEp,
    const CopySetConf &conf, const CopysetNodePtr &copyset) {
    // CLDCFS-1004 bug-fix: mds sends copyset, whose epoch is 0,
    // configuration is empty
    if (0 == conf.epoch() && conf.peers().empty()) {
        LOG(INFO) << "Clean copyset "
            << ToGroupIdStr(conf.logicalpoolid(), conf.copysetid())
            << "in peer " << csEp
            << ", witch is not exist in mds record";
        return true;
    }

    // This chunkserrver is not in the copyset configuration and needs to be
    // cleaned up
    std::string chunkserverEp = std::string(butil::endpoint2str(csEp).c_str());
    for (int i = 0; i < conf.peers_size(); i++) {
        if (conf.peers(i).address().find(chunkserverEp) != std::string::npos) {
            return false;
        }
    }
    return true;
}

bool HeartbeatHelper::ChunkServerLoadCopySetFin(const std::string peerId) {
    if (!PeerVaild(peerId)) {
        LOG(WARNING) << peerId << " is invalid";
        return false;
    }

    PeerId peer;
    peer.parse(peerId);
    const char *ip = butil::ip2str(peer.addr.ip).c_str();
    int port = peer.addr.port;
    brpc::Channel channel;
    if (channel.Init(ip, port, NULL) != 0) {
        LOG(ERROR) << "Fail to init channel to ip:" << ip << " port:" << port;
        return false;
    }
    ChunkServerService_Stub stub(&channel);

    brpc::Controller cntl;
    cntl.set_timeout_ms(500);
    ChunkServerStatusRequest req;
    ChunkServerStatusResponse rep;
    stub.ChunkServerStatus(&cntl, &req, &rep, nullptr);
    if (cntl.Failed()) {
        LOG(WARNING) << "Send ChunkServerStatusRequest failed, cntl.errorText ="
            << cntl.ErrorText();
        return false;
    }

    return rep.copysetloadfin();
}

}  // namespace chunkserver
}  // namespace curve

