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

#include "vbucket_state.h"
#include "ep_types.h"
#include "vbucket.h"

vbucket_state::vbucket_state() : hlcCasEpochSeqno(HlcCasSeqnoUninitialised) {
}

vbucket_state::vbucket_state(vbucket_state_t _state,
                             uint64_t _chkid,
                             uint64_t _maxDelSeqNum,
                             int64_t _highSeqno,
                             uint64_t _purgeSeqno,
                             uint64_t _lastSnapStart,
                             uint64_t _lastSnapEnd,
                             uint64_t _maxCas,
                             int64_t _hlcCasEpochSeqno,
                             bool _mightContainXattrs,
                             std::string _failovers,
                             bool _supportsCollections)
    : state(_state),
      checkpointId(_chkid),
      maxDeletedSeqno(_maxDelSeqNum),
      highSeqno(_highSeqno),
      purgeSeqno(_purgeSeqno),
      lastSnapStart(_lastSnapStart),
      lastSnapEnd(_lastSnapEnd),
      maxCas(_maxCas),
      hlcCasEpochSeqno(_hlcCasEpochSeqno),
      mightContainXattrs(_mightContainXattrs),
      failovers(std::move(_failovers)),
      supportsCollections(_supportsCollections) {
}

std::string vbucket_state::toJSON() const {
    std::stringstream jsonState;
    jsonState << "{\"state\": \"" << VBucket::toString(state) << "\""
              << ",\"checkpoint_id\": \"" << checkpointId << "\""
              << ",\"max_deleted_seqno\": \"" << maxDeletedSeqno << "\""
              << ",\"failover_table\": " << failovers << ",\"snap_start\": \""
              << lastSnapStart << "\""
              << ",\"snap_end\": \"" << lastSnapEnd << "\""
              << ",\"max_cas\": \"" << maxCas << "\""
              << ",\"might_contain_xattrs\": "
              << (mightContainXattrs ? "true" : "false")
              << ",\"supports_collections\": "
              << (supportsCollections ? "true" : "false") << "}";

    return jsonState.str();
}

bool vbucket_state::needsToBePersisted(const vbucket_state& vbstate) {
    /**
     * The vbucket state information is to be persisted
     * only if a change is detected in the state or the
     * failovers fields.
     */
    if (state != vbstate.state || failovers.compare(vbstate.failovers) != 0) {
        return true;
    }
    return false;
}

void vbucket_state::reset() {
    checkpointId = 0;
    maxDeletedSeqno = 0;
    highSeqno = 0;
    purgeSeqno = 0;
    lastSnapStart = 0;
    lastSnapEnd = 0;
    maxCas = 0;
    hlcCasEpochSeqno = HlcCasSeqnoUninitialised;
    mightContainXattrs = false;
    failovers.clear();
}
