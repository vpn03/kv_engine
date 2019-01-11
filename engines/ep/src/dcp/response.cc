/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2014 Couchbase, Inc
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

#include "config.h"

#include "dcp/response.h"

#include <xattr/utils.h>

/*
 * These constants are calculated from the size of the packets that are
 * created by each message when it gets sent over the wire. The packet
 * structures are located in the protocol_binary.h file in the memcached
 * project.
 */

const uint32_t StreamRequest::baseMsgBytes = 72;
const uint32_t AddStreamResponse::baseMsgBytes = 28;
const uint32_t SnapshotMarkerResponse::baseMsgBytes = 24;
const uint32_t SetVBucketStateResponse::baseMsgBytes = 24;
const uint32_t StreamEndResponse::baseMsgBytes = 28;
const uint32_t SetVBucketState::baseMsgBytes = 25;
const uint32_t SnapshotMarker::baseMsgBytes = 44;

const char* DcpResponse::to_string() const {
    switch (event_) {
    case Event::Mutation:
        return "mutation";
    case Event::Deletion:
        return "deletion";
    case Event::Expiration:
        return "expiration";
    case Event::Prepare:
        return "prepare";
    case Event::Commit:
        return "commit";
    case Event::SetVbucket:
        return "set vbucket";
    case Event::StreamReq:
        return "stream req";
    case Event::StreamEnd:
        return "stream end";
    case Event::SnapshotMarker:
        return "snapshot marker";
    case Event::AddStream:
        return "add stream";
    case Event::SystemEvent:
        return "system event";
    case Event::SeqnoAcknowledgement:
        return "seqno acknowledgement";
    }
    throw std::logic_error(
        "DcpResponse::to_string(): " + std::to_string(int(event_)));
}

uint32_t MutationResponse::getDeleteLength() const {
    if ((enableExpiryOutput == EnableExpiryOutput::Yes) &&
        (item_->deletionSource() == DeleteSource::TTL)) {
        return expirationBaseMsgBytes;
    }
    if (includeDeleteTime == IncludeDeleteTime::Yes) {
        return deletionV2BaseMsgBytes;
    }
    return deletionBaseMsgBytes;
}

uint32_t MutationResponse::getHeaderSize() const {
    switch (item_->getCommitted()) {
    case CommittedState::CommittedViaMutation:
    case CommittedState::CommittedViaPrepare:
        return item_->isDeleted() ? getDeleteLength() : mutationBaseMsgBytes;

    case CommittedState::Pending:
        return prepareBaseMsgBytes;
    }
    throw std::logic_error(
            "MutationResponse: Unhandled CommittedState in switch (" +
            std::to_string(int(item_->getCommitted())) + ")");
}

uint32_t MutationResponse::getMessageSize() const {
    const uint32_t header = getHeaderSize();
    uint32_t keySize = 0;
    if (includeCollectionID == DocKeyEncodesCollectionId::Yes) {
        keySize = item_->getKey().size();
    } else {
        keySize = item_->getKey().makeDocKeyWithoutCollectionID().size();
    }
    uint32_t body = keySize + item_->getNBytes();

    return header + body;
}

uint32_t MutationConsumerMessage::getMessageSize() const {
    uint32_t body = MutationResponse::getMessageSize();

    // Check to see if we need to include the extended meta data size.
    if (emd) {
        body += emd->getExtMeta().second;
    }

    return body;
}

std::ostream& operator<<(std::ostream& os, const DcpResponse& r) {
    os << "DcpResponse[" << &r << "] with"
       << " event:" << r.to_string();
    return os;
}

CommitSyncWrite::CommitSyncWrite(uint32_t opaque,
                                 Vbid vbucket,
                                 uint64_t preparedSeqno,
                                 uint64_t commitSeqno,
                                 const DocKey& key)
    /// @todo-durability: Remove key, use pending seqno to identify instead.
    : DcpResponse(Event::Commit, opaque, cb::mcbp::DcpStreamId{}),
      vbucket(vbucket),
      key(key),
      payload(preparedSeqno, commitSeqno) {
}

uint32_t CommitSyncWrite::getMessageSize() const {
    return commitBaseMsgBytes + key.size();
}
