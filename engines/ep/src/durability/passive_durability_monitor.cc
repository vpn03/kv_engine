/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2019 Couchbase, Inc.
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

#include "passive_durability_monitor.h"
#include "active_durability_monitor.h"
#include "durability_monitor_impl.h"

#include "bucket_logger.h"
#include "item.h"
#include "rollback_result.h"
#include "statwriter.h"
#include "stored-value.h"
#include "vbucket.h"
#include "vbucket_state.h"

#include <boost/range/adaptor/reversed.hpp>
#include <gsl.h>
#include <utilities/logtags.h>
#include <unordered_map>

PassiveDurabilityMonitor::PassiveDurabilityMonitor(VBucket& vb)
    : vb(vb), state(std::make_unique<State>(*this)) {
    // By design, instances of Container::Position can never be invalid
    auto s = state.wlock();
    s->highPreparedSeqno.it = s->trackedWrites.end();
    s->highCompletedSeqno.it = s->trackedWrites.end();
}

PassiveDurabilityMonitor::PassiveDurabilityMonitor(VBucket& vb,
                                                   int64_t highPreparedSeqno,
                                                   int64_t highCompletedSeqno)
    : PassiveDurabilityMonitor(vb) {
    auto s = state.wlock();
    s->highPreparedSeqno.lastWriteSeqno.reset(highPreparedSeqno);
    s->highCompletedSeqno.lastWriteSeqno.reset(highCompletedSeqno);
}

PassiveDurabilityMonitor::PassiveDurabilityMonitor(
        VBucket& vb,
        int64_t highPreparedSeqno,
        int64_t highCompletedSeqno,
        std::vector<queued_item>&& outstandingPrepares)
    : PassiveDurabilityMonitor(vb, highPreparedSeqno, highCompletedSeqno) {
    auto s = state.wlock();
    for (auto& prepare : outstandingPrepares) {
        // Construct the SyncWrites and request an infinite timeout
        s->trackedWrites.emplace_back(nullptr,
                                      std::move(prepare),
                                      nullptr,
                                      nullptr,
                                      SyncWrite::InfiniteTimeout{});
        // Advance the highPreparedSeqno iterator to point to the highest
        // SyncWrite which has been prepared.
        auto lastIt = std::prev(s->trackedWrites.end());
        if (lastIt->getBySeqno() <= highPreparedSeqno) {
            s->highPreparedSeqno.it = lastIt;
        }

        // Advance the highCompletedSeqno iterator to point to the highest
        // SyncWrite which has been completed.
        //
        // Note: One might assume that this would always point to
        // trackedWrites.begin(), given that we are a newly minted PassiveDM and
        // hence would only be tracking incomplete SyncWrites. However, we
        // _could_ have been converted from an ActiveDM with null topology which
        // itself was converted from a previous PassiveDM which _did_ have
        // completed SyncWrites still in trackedWrites (because they haven't
        // been persisted locally yet).
        if (lastIt->getBySeqno() <= highCompletedSeqno) {
            s->highCompletedSeqno.it = lastIt;
        }
    }
}

PassiveDurabilityMonitor::~PassiveDurabilityMonitor() = default;

void PassiveDurabilityMonitor::addStats(const AddStatFn& addStat,
                                        const void* cookie) const {
    char buf[256];

    try {
        const auto vbid = vb.getId().get();

        checked_snprintf(buf, sizeof(buf), "vb_%d:state", vbid);
        add_casted_stat(buf, VBucket::toString(vb.getState()), addStat, cookie);

        checked_snprintf(buf, sizeof(buf), "vb_%d:high_prepared_seqno", vbid);
        add_casted_stat(buf, getHighPreparedSeqno(), addStat, cookie);

        checked_snprintf(buf, sizeof(buf), "vb_%d:high_completed_seqno", vbid);
        add_casted_stat(buf, getHighCompletedSeqno(), addStat, cookie);

    } catch (const std::exception& e) {
        EP_LOG_WARN(
                "PassiveDurabilityMonitor::addStats: error building stats: {}",
                e.what());
    }
}

int64_t PassiveDurabilityMonitor::getHighPreparedSeqno() const {
    return state.rlock()->highPreparedSeqno.lastWriteSeqno;
}

int64_t PassiveDurabilityMonitor::getHighCompletedSeqno() const {
    return state.rlock()->highCompletedSeqno.lastWriteSeqno;
}

void PassiveDurabilityMonitor::addSyncWrite(
        queued_item item, boost::optional<int64_t> overwritingPrepareSeqno) {
    auto durReq = item->getDurabilityReqs();

    if (durReq.getLevel() == cb::durability::Level::None) {
        throwException<std::invalid_argument>(__func__, "Level::None");
    }
    if (durReq.getTimeout().isDefault()) {
        throwException<std::invalid_argument>(
                __func__,
                "timeout is default (explicit value should have been specified "
                "by Active node)");
    }

    auto s = state.wlock();
    if (overwritingPrepareSeqno) {
        // Remove any trackedWrites with the same key.
        auto itr = s->trackedWrites.begin();
        while (itr != s->trackedWrites.end() &&
               itr->getKey() != item->getKey()) {
            itr = s->getIteratorNext(itr);
        }
        if (itr != s->trackedWrites.end()) {
            Expects(itr->getBySeqno() == overwritingPrepareSeqno);
            // We have found a trackedWrite with the same key to remove. Update
            // the HCS and HPS iterators and then remove the SyncWrite.
            if (itr == s->highCompletedSeqno.it) {
                s->highCompletedSeqno.it = s->trackedWrites.end();
            }
            if (itr == s->highPreparedSeqno.it) {
                s->highPreparedSeqno.it = s->trackedWrites.end();
            }

            s->trackedWrites.erase(itr);
        }
    }

#if CB_DEVELOPMENT_ASSERTS
    // Additional error checking for dev builds to validate that we don't have
    // any duplicate SyncWrites in trackedWrites. Only done for dev builds
    // as this is likely expensive.
    auto itr = std::find_if(s->trackedWrites.begin(),
                            s->trackedWrites.end(),
                            [item](const SyncWrite& write) {
                                // Skip any completed SyncWrites
                                return !write.isCompleted() &&
                                       write.getKey() == item->getKey();
                            });
    if (itr != s->trackedWrites.end()) {
        std::stringstream ss;
        ss << "Found SyncWrite '" << *itr
           << "', whilst attempting to add new SyncWrite for key "
           << cb::tagUserData(item->getKey().to_string())
           << " with prepare seqno " << item->getBySeqno();
        throwException<std::logic_error>(__func__, "" + ss.str());
    }
#endif

    // Need to specify defaultTimeout for SyncWrite ctor, but we've already
    // checked just above the requirements have a non-default value,
    // just pass dummy value here.
    std::chrono::milliseconds dummy{};
    s->trackedWrites.emplace_back(nullptr /*cookie*/,
                                  std::move(item),
                                  dummy,
                                  nullptr /*firstChain*/,
                                  nullptr /*secondChain*/);
    s->totalAccepted++;
}

size_t PassiveDurabilityMonitor::getNumTracked() const {
    return state.rlock()->trackedWrites.size();
}

size_t PassiveDurabilityMonitor::getNumAccepted() const {
    return state.rlock()->totalAccepted;
}
size_t PassiveDurabilityMonitor::getNumCommitted() const {
    return state.rlock()->totalCommitted;
}
size_t PassiveDurabilityMonitor::getNumAborted() const {
    return state.rlock()->totalAborted;
}

void PassiveDurabilityMonitor::notifySnapshotEndReceived(uint64_t snapEnd) {
    { // state locking scope
        auto s = state.wlock();
        s->receivedSnapshotEnds.push({int64_t(snapEnd),
                                      vb.isReceivingDiskSnapshot()
                                              ? CheckpointType::Disk
                                              : CheckpointType::Memory});
        // Maybe the new tracked Prepare is already satisfied and could be
        // ack'ed back to the Active.
        auto prevHps = s->highPreparedSeqno.lastWriteSeqno;
        s->updateHighPreparedSeqno();

        // Store the seqno ack to send after we drop the state lock
        storeSeqnoAck(prevHps, s->highPreparedSeqno.lastWriteSeqno);
    }

    if (notifySnapEndSeqnoAckPreProcessHook) {
        notifySnapEndSeqnoAckPreProcessHook();
    }

    sendSeqnoAck();
}

void PassiveDurabilityMonitor::notifyLocalPersistence() {
    { // state locking scope
        auto s = state.wlock();
        auto prevHps = s->highPreparedSeqno.lastWriteSeqno;
        s->updateHighPreparedSeqno();

        // Store the seqno ack to send after we drop the state lock
        storeSeqnoAck(prevHps, s->highPreparedSeqno.lastWriteSeqno);
    }

    sendSeqnoAck();
}

void PassiveDurabilityMonitor::storeSeqnoAck(int64_t prevHps, int64_t newHps) {
    if (prevHps != newHps) {
        auto seqno = seqnoToAck.wlock();
        if (*seqno < newHps) {
            *seqno = newHps;
        }
    }
}

void PassiveDurabilityMonitor::sendSeqnoAck() {
    // Hold the lock throughout to ensure that we do not race with another ack
    auto seqno = seqnoToAck.wlock();
    if (*seqno != 0) {
        vb.sendSeqnoAck(*seqno);
    }
    *seqno = 0;
}

std::string PassiveDurabilityMonitor::to_string(Resolution res) {
    switch (res) {
    case Resolution::Commit:
        return "commit";
    case Resolution::Abort:
        return "abort";
    case Resolution::CompletionWasDeduped:
        return "completionWasDeduped";
    }
    folly::assume_unreachable();
}

void PassiveDurabilityMonitor::completeSyncWrite(
        const StoredDocKey& key,
        Resolution res,
        boost::optional<uint64_t> prepareSeqno) {
    auto s = state.wlock();

    if (s->trackedWrites.empty()) {
        throwException<std::logic_error>(
                __func__,
                "No tracked, but received " + to_string(res) + " for key " +
                        cb::tagUserData(key.to_string()));
    }

    // If we are receiving a disk snapshot, we need to relax a few checks
    // to account for deduplication. E.g., commits may appear to be out
    // of order
    bool enforceOrderedCompletion = !vb.isReceivingDiskSnapshot();

    // If we can complete out of order, we have to check from the start of
    // tracked writes as the HCS may have advanced past a prepare we have not
    // seen a completion for
    auto next = enforceOrderedCompletion
                        ? s->getIteratorNext(s->highCompletedSeqno.it)
                        : s->trackedWrites.begin();

    if (!enforceOrderedCompletion) {
        // Advance the iterator to the right item, it might not be the first
        while (next != s->trackedWrites.end() && next->getKey() != key) {
            next = s->getIteratorNext(next);
        }
    }

    if (next == s->trackedWrites.end()) {
        throwException<std::logic_error>(
                __func__,
                "No Prepare waiting for completion, but received " +
                        to_string(res) + " for key " +
                        cb::tagUserData(key.to_string()));
    }

    // Sanity checks for In-Order Commit
    if (next->getKey() != key) {
        std::stringstream ss;
        ss << "Pending resolution for '" << *next
           << "', but received unexpected " + to_string(res) + " for key "
           << cb::tagUserData(key.to_string());
        throwException<std::logic_error>(__func__, "" + ss.str());
    }

    if (prepareSeqno && next->getBySeqno() != static_cast<int64_t>(*prepareSeqno)) {
        std::stringstream ss;
        ss << "Pending resolution for '" << *next
           << "', but received unexpected " + to_string(res) + " for key "
           << cb::tagUserData(key.to_string())
           << " different prepare seqno: " << *prepareSeqno;
        throwException<std::logic_error>(__func__, "" + ss.str());
    }

    if (enforceOrderedCompletion ||
        next->getBySeqno() > s->highCompletedSeqno.lastWriteSeqno) {
        // Note: Update last-write-seqno first to enforce monotonicity and
        //     avoid any state-change if monotonicity checks fail
        // Do *not* update hcs if this is a commit for a prepare with seqno <=
        // hcs from disk backfill (can be seen due to a deduped commit) as that
        // would move us *backwards* and the monotonic would throw
        s->highCompletedSeqno.lastWriteSeqno = next->getBySeqno();
        s->highCompletedSeqno.it = next;
    }

    // Mark this prepare as completed so that we can allow non-completed
    // duplicates in trackedWrites in case it is not removed because it requires
    // persistence.
    Expects(!next->isCompleted());
    next->setCompleted();

    // HCS has moved, which could make some Prepare eligible for removal.
    s->checkForAndRemovePrepares();

    switch (res) {
    case Resolution::Commit:
        s->totalCommitted++;
        return;
    case Resolution::Abort:
        s->totalAborted++;
        return;
    case Resolution::CompletionWasDeduped:
        return;
    }
    folly::assume_unreachable();
}

int64_t PassiveDurabilityMonitor::getHighestTrackedSeqno() const {
    auto s = state.rlock();
    if (!s->trackedWrites.empty()) {
        return s->trackedWrites.back().getBySeqno();
    } else {
        return 0;
    }
}

void PassiveDurabilityMonitor::toOStream(std::ostream& os) const {
    os << "PassiveDurabilityMonitor[" << this << "] " << *state.rlock();
}

PassiveDurabilityMonitor::State::State(const PassiveDurabilityMonitor& pdm)
    : pdm(pdm) {
    const auto prefix =
            "PassiveDM(" + pdm.vb.getId().to_string() + ")::State::";

    const auto hpsPrefix = prefix + "highPreparedSeqno";
    highPreparedSeqno.lastWriteSeqno.setLabel(hpsPrefix + ".lastWriteSeqno");
    highPreparedSeqno.lastAckSeqno.setLabel(hpsPrefix + ".lastAckSeqno");

    const auto hcsPrefix = prefix + "highCompletedSeqno";
    highCompletedSeqno.lastWriteSeqno.setLabel(hcsPrefix + ".lastWriteSeqno");
    highCompletedSeqno.lastAckSeqno.setLabel(hcsPrefix + ".lastAckSeqno");
}

DurabilityMonitor::Container::iterator
PassiveDurabilityMonitor::State::getIteratorNext(
        const Container::iterator& it) {
    // Note: Container::end could be the new position when the pointed SyncWrite
    //     is removed from Container and the iterator repositioned.
    //     In that case next=Container::begin
    return (it == trackedWrites.end()) ? trackedWrites.begin() : std::next(it);
}

void PassiveDurabilityMonitor::State::updateHighPreparedSeqno() {
    // The HPS moves (ie, Prepares are locally-satisfied and ack'ed to Master)
    // at PDM under the following constraints:
    //
    // (1) Nothing is ack'ed before the complete snapshot is received
    //     (I.e., do nothing if receivedSnapshotEnds is empty)
    //
    // (2) Majority and MajorityAndPersistOnMaster Prepares (which don't need to
    //     be persisted for being locally satisfied) may be satisfied as soon as
    //     the complete snapshot is received
    //
    // (3) PersistToMajority Prepares represent a durability-fence. So, at (2)
    //     we can satisfy only Prepares up to before the durability-fence (if
    //     any)
    //
    // (4) The durability-fence can move (ie, PersistToMajority Prepares are
    //     locally-satisfied) only when the complete snapshot is persisted.
    //
    // (5) Once a disk snapshot is fully persisted, the HPS is advanced to the
    //     snapshot end - even if no prepares were seen during the snapshot
    //     or if trackedWrites is empty. This accounts for deduping; there may
    //     have been prepares we have not seen, but they are definitely
    //     satisfied (they are persisted) and should be acked.
    //
    // This function implements all the logic necessary for moving the HPS by
    // enforcing the rules above. The function is called:
    //
    // (a) Every time a snapshot-end is received for the owning VBucket.
    //     That updates the PDM::snapshotEnd and calls down here, where the HPS
    //     is potentially moved (given that a new snapshot-end received may
    //     immediately unblock some pending (locally-unsatisfied) Prepares; eg,
    //     Majority / MajorityAndPersistOnMaster Prepare)
    //
    // (b) Every time the Flusher has run, as persistence may move the
    //     durability-fence (ie, unblock some PersistToMajority Prepares, if
    //     any) and unblock any other Prepare previously blocked on
    //     durability-fence. As already mentioned, we can move the
    //     durability-fence only if the complete snapshot is persisted.

    const auto prevHPS = highPreparedSeqno.lastWriteSeqno;

    // Helper to keep conditions short and meaningful
    const auto inSnapshot = [trackedWritesEnd = trackedWrites.end()](
                                    uint64_t snapshotEndSeqno,
                                    auto prepareItr) {
        return prepareItr != trackedWritesEnd &&
               static_cast<uint64_t>(prepareItr->getBySeqno()) <=
                       snapshotEndSeqno;
    };

    while (!receivedSnapshotEnds.empty()) {
        const auto snapshotEnd = receivedSnapshotEnds.front();

        const bool snapshotFullyPersisted =
                static_cast<int64_t>(pdm.vb.getPersistenceSeqno()) >=
                snapshotEnd.seqno;

        const bool isDiskSnapshot = snapshotEnd.type == CheckpointType::Disk;

        using namespace cb::durability;

        Level maxLevelCanAdvanceOver{};

        if (snapshotFullyPersisted) {
            // we have received and persisted an entire snapshot
            // All prepares from this snapshot are satisfied and the state
            // is consistent at snap end. The HPS can advance over Prepares of
            // PersistToMajority or lower (i.e., everything currently)
            maxLevelCanAdvanceOver = Level::PersistToMajority;
        } else if (!isDiskSnapshot) {
            // we have received but NOT persisted an entire snapshot
            //  We *may* be able to advance the HPS part way
            // into this snapshot - The HPS can be advanced over all Prepares of
            // MajorityAndPersistOnMaster level or lower, to the last Prepare
            // immediately preceding an *unpersisted* Prepare with Level ==
            // PersistToMajority. We cannot move the HPS past this Prepare until
            // it *is* persisted.
            maxLevelCanAdvanceOver = Level::MajorityAndPersistOnMaster;
        } else {
            // we have received but NOT persisted an entire *DISK* snapshot
            // we cannot ack anything until the entire snapshot has been
            // persisted because PersistToMajority level Prepares may have been
            // deduped by lower level prepares.
            // Therefore, the HPS cannot advance over *any* prepares.
            maxLevelCanAdvanceOver = Level::None;
        }

        // Advance the HPS, respecting maxLevelCanAdvanceOver
        if (!trackedWrites.empty()) {
            for (auto next = getIteratorNext(highPreparedSeqno.it);
                 inSnapshot(snapshotEnd.seqno, next) &&
                 next->getDurabilityReqs().getLevel() <= maxLevelCanAdvanceOver;
                 next = getIteratorNext(highPreparedSeqno.it)) {
                // Note: Update last-write-seqno first to enforce monotonicity
                // and avoid any state-change if monotonicity checks fail
                highPreparedSeqno.lastWriteSeqno = next->getBySeqno();
                highPreparedSeqno.it = next;
            }
        }

        if (isDiskSnapshot && snapshotFullyPersisted) {
            // Special case - prepares in disk snapshots may have been
            // deduplicated.
            // PRE(persistMajority), CMT, PRE(), ABORT, SET
            // may, after the abort has been purged be sent as:
            // SET
            // We would have no prepare for this op, but we still need to
            // seqno ack something. To resolve this, advance the HPS seqno to
            // the snapshotEndSeqno. There may not be an associated prepare.
            // NB: lastWriteSeqno is NOT guaranteed to match
            // highPreparedSeqno.it->getBySeqno()
            // because of this case
            highPreparedSeqno.lastWriteSeqno = snapshotEnd.seqno;
        }

        // Check if we could have acked everything within the snapshot and
        // might be able to continue checking the next one.
        if ((isDiskSnapshot && !snapshotFullyPersisted) ||
            inSnapshot(snapshotEnd.seqno,
                       getIteratorNext(highPreparedSeqno.it))) {
            // Either we have not fully persisted a disk snapshot and
            // the HPS is left <= the start of this snapshot
            // OR
            // we stopped advancing the HPS before the end of a memory
            // snapshot because we reached a PersistToMajority Prepare
            // HPS now points to the last Prepare before any
            // PersistToMajority
            break;
        }

        receivedSnapshotEnds.pop();
    }

    // We have now acked all the complete, persisted snapshots we received,
    // and advanced the HPS as far as it can go - cannot advance further into a
    // partial snapshot or past a PersistToMajority Prepare

    if (highPreparedSeqno.lastWriteSeqno != prevHPS) {
        Expects(highPreparedSeqno.lastWriteSeqno > prevHPS);
        // HPS has moved, which could make some Prepare eligible for removal.
        checkForAndRemovePrepares();
    }
}

void PassiveDurabilityMonitor::State::checkForAndRemovePrepares() {
    if (trackedWrites.empty()) {
        return;
    }

    const auto fence = std::min(int64_t(highCompletedSeqno.lastWriteSeqno),
                                int64_t(highPreparedSeqno.lastWriteSeqno));

    auto it = trackedWrites.begin();
    while (it != trackedWrites.end() && it->getBySeqno() <= fence) {
        // In PassiveDM we have two iterators pointing to items in the tracked
        // Container: the HPS and the High Completed Seqno.
        // Ensure that iterators are never invalid by pointing them to
        // Container::end if the underlying item is removed.
        if (it == highCompletedSeqno.it) {
            highCompletedSeqno.it = trackedWrites.end();
        }
        if (it == highPreparedSeqno.it) {
            highPreparedSeqno.it = trackedWrites.end();
        }

        // Note: 'it' will be invalidated, so it will need to be reset
        const auto next = std::next(it);
        trackedWrites.erase(it);
        it = next;
    }
}

template <class exception>
[[noreturn]] void PassiveDurabilityMonitor::throwException(
        const std::string& thrower, const std::string& error) const {
    throw exception("PassiveDurabilityMonitor::" + thrower + " " +
                    vb.getId().to_string() + " " + error);
}

std::ostream& operator<<(std::ostream& os,
                         const PassiveDurabilityMonitor::State& state) {
    os << "State[" << &state << "] highPreparedSeqno:"
       << to_string(state.highPreparedSeqno, state.trackedWrites.end())
       << " highCompletedSeqno:"
       << to_string(state.highCompletedSeqno, state.trackedWrites.end())
       << "\ntrackedWrites:[\n";
    for (const auto& w : state.trackedWrites) {
        os << "    " << w << "\n";
    }
    os << "]\n";
    return os;
}
