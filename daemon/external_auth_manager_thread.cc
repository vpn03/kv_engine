/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2018-Present Couchbase, Inc.
 *
 *   Use of this software is governed by the Business Source License included
 *   in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
 *   in that file, in accordance with the Business Source License, use of this
 *   software will be governed by the Apache License, Version 2.0, included in
 *   the file licenses/APL2.txt.
 */
#include "external_auth_manager_thread.h"

#include "connection.h"
#include "front_end_thread.h"
#include "get_authorization_task.h"
#include "server_event.h"
#include "start_sasl_auth_task.h"

#include <logger/logger.h>
#include <mcbp/protocol/framebuilder.h>
#include <nlohmann/json.hpp>
#include <platform/base64.h>
#include <algorithm>

/// The one and only handle to the external authentication manager
std::unique_ptr<ExternalAuthManagerThread> externalAuthManager;

/**
 * The AuthenticationRequestServerEvent is responsible for injecting
 * the Authentication Request packet onto the connections stream.
 */
class AuthenticationRequestServerEvent : public ServerEvent {
public:
    AuthenticationRequestServerEvent(uint32_t id,
                                     StartSaslAuthTask& req,
                                     bool authenticateOnly)
        : id(id) {
        nlohmann::json json;
        json["mechanism"] = req.getMechanism();
        json["challenge"] = cb::base64::encode(req.getChallenge(), false);
        json["authentication-only"] = authenticateOnly;
        payload = json.dump();
    }

    std::string getDescription() const override {
        return "AuthenticationRequestServerEvent";
    }

    bool execute(Connection& connection) override {
        using namespace cb::mcbp;

        const size_t needed = sizeof(cb::mcbp::Request) + payload.size();
        std::string buffer;
        buffer.resize(needed);
        RequestBuilder builder(buffer);
        builder.setMagic(Magic::ServerRequest);
        builder.setDatatype(cb::mcbp::Datatype::JSON);
        builder.setOpcode(ServerOpcode::Authenticate);
        builder.setOpaque(id);

        // The extras contains the cluster revision number as an uint32_t
        builder.setValue({reinterpret_cast<const uint8_t*>(payload.data()),
                          payload.size()});

        // Inject our packet into the stream!
        connection.copyToOutputStream(builder.getFrame()->getFrame());
        return true;
    }

protected:
    const uint32_t id;
    std::string payload;
};

/**
 * The GetAuthorizatonServerEvent is responsible for injecting
 * the GetAuthorization Request packet onto the connections stream.
 */
class GetAuthorizationServerEvent : public ServerEvent {
public:
    GetAuthorizationServerEvent(uint32_t id, GetAuthorizationTask& req)
        : id(id), user(req.getUsername()) {
    }

    std::string getDescription() const override {
        return "GetAuthorizatonServerEvent";
    }

    bool execute(Connection& connection) override {
        using namespace cb::mcbp;

        const size_t needed = sizeof(cb::mcbp::Request) + user.size();
        std::string buffer;
        buffer.resize(needed);
        RequestBuilder builder(buffer);
        builder.setMagic(Magic::ServerRequest);
        builder.setDatatype(cb::mcbp::Datatype::Raw);
        builder.setOpcode(ServerOpcode::GetAuthorization);
        builder.setOpaque(id);
        builder.setKey(user);

        // Inject our packet into the stream!
        connection.copyToOutputStream(builder.getFrame()->getFrame());
        return true;
    }

protected:
    const uint32_t id;
    const std::string user;
};

/**
 * The ActiveExternalUsersServerEvent is responsible for injecting
 * the ActiveExternalUsers packet onto the connections stream.
 */
class ActiveExternalUsersServerEvent : public ServerEvent {
public:
    explicit ActiveExternalUsersServerEvent(std::string payload)
        : payload(std::move(payload)) {
    }

    std::string getDescription() const override {
        return "ActiveExternalUsersServerEvent";
    }

    bool execute(Connection& connection) override {
        using namespace cb::mcbp;

        const size_t needed = sizeof(cb::mcbp::Request) + payload.size();
        std::string buffer;
        buffer.resize(needed);
        RequestBuilder builder(buffer);
        builder.setMagic(Magic::ServerRequest);
        builder.setDatatype(cb::mcbp::Datatype::JSON);
        builder.setOpcode(ServerOpcode::ActiveExternalUsers);
        builder.setValue({reinterpret_cast<const uint8_t*>(payload.data()),
                          payload.size()});

        // Inject our packet into the stream!
        connection.copyToOutputStream(builder.getFrame()->getFrame());
        return true;
    }

protected:
    const std::string payload;
};

void ExternalAuthManagerThread::add(Connection& connection) {
    std::lock_guard<std::mutex> guard(mutex);

    connection.incrementRefcount();
    connections.push_back(&connection);
}

void ExternalAuthManagerThread::remove(Connection& connection) {
    std::lock_guard<std::mutex> guard(mutex);

    auto iter = std::find(connections.begin(), connections.end(), &connection);
    if (iter != connections.end()) {
        pendingRemoveConnection.push_back(&connection);
        connections.erase(iter);
        condition_variable.notify_all();
    }
}

void ExternalAuthManagerThread::enqueueRequest(AuthnAuthzServiceTask& request) {
    // We need to make sure that the lock ordering for these
    // mutexes is the same. Let's unlock the task (and the executor thread
    // is currently blocked waiting for this method to return. It won't
    // touch the mutex until we return.
    // Then we'll grab the external auth manager mutex and get our mutex
    // back (no one else knows about that mutex yet so it should never
    // block). Then we'll just release the external auth manager lock,
    // and the external auth thread may start processing these events,
    // but it'll have to wait until we release the request mutex
    // before it may signal the task.
    request.getMutex().unlock();
    std::lock_guard<std::mutex> guard(mutex);
    request.getMutex().lock();
    incomingRequests.push(&request);
    condition_variable.notify_all();
}

void ExternalAuthManagerThread::responseReceived(
        const cb::mcbp::Response& response) {
    // We need to keep the RBAC db in sync to avoid race conditions where
    // the response message is delayed and not handled until the auth
    // thread is scheduled. The reason we set it here is because
    // if we receive an update on the same connection the last one wins
    if (cb::mcbp::isStatusSuccess(response.getStatus())) {
        // Note that this may cause an exception to be thrown
        // and the connection closed..
        auto value = response.getValue();
        const auto payload = std::string{
                reinterpret_cast<const char*>(value.data()), value.size()};
        auto decoded = nlohmann::json::parse(payload);
        auto rbac = decoded.find("rbac");
        if (rbac != decoded.end()) {
            const auto username = rbac->begin().key();
            cb::rbac::updateExternalUser(rbac->dump());
        }
    }

    // Enqueue the response and let the auth thread deal with it
    std::lock_guard<std::mutex> guard(mutex);
    incommingResponse.emplace(std::make_unique<AuthResponse>(
            response.getOpaque(), response.getStatus(), response.getValue()));
    condition_variable.notify_all();
}

void ExternalAuthManagerThread::run() {
    setRunning();

    std::unique_lock<std::mutex> lock(mutex);
    activeUsersLastSent = std::chrono::steady_clock::now();
    while (running) {
        if (incomingRequests.empty() && incommingResponse.empty()) {
            // We need to wake up the next time we want to push the
            // new active users list
            const auto now = std::chrono::steady_clock::now();
            const auto sleeptime = activeUsersPushInterval.load() -
                                   (now - activeUsersLastSent);
            condition_variable.wait_for(lock, sleeptime);
            if (!running) {
                // We're supposed to terminate
                return;
            }
        }

        // Purge the pending remove lists
        purgePendingDeadConnections();

        if (!incomingRequests.empty()) {
            processRequestQueue();
        }

        if (!incommingResponse.empty()) {
            processResponseQueue();
        }

        const auto now = std::chrono::steady_clock::now();
        if ((now - activeUsersLastSent) >= activeUsersPushInterval.load()) {
            pushActiveUsers();
            activeUsersLastSent = now;
        }
    }
}

void ExternalAuthManagerThread::shutdown() {
    std::lock_guard<std::mutex> guard(mutex);
    running = false;
    condition_variable.notify_all();
}

void ExternalAuthManagerThread::pushActiveUsers() {
    if (connections.empty()) {
        return;
    }

    const std::string payload = activeUsers.to_json().dump();

    // We cannot hold the internal lock when we try to lock the front
    // end thread as that'll cause a potential deadlock with the "add",
    // "remove" and "responseReceived" as they'll hold the thread
    // mutex and try to acquire the auth mutex in order to enqueue
    // a new connection / response.
    auto* provider = connections.front();

    mutex.unlock();
    {
        // Lock the authentication provider (we're holding a
        // reference counter to the provider, so it can't go away while we're
        // doing this).
        std::lock_guard<std::mutex> guard(provider->getThread().mutex);
        provider->enqueueServerEvent(
                std::make_unique<ActiveExternalUsersServerEvent>(payload));
        provider->signalIfIdle();
    }
    // Acquire the lock
    mutex.lock();
}

void ExternalAuthManagerThread::processRequestQueue() {
    if (connections.empty()) {
        // we don't have a provider, we need to cancel the request!
        while (!incomingRequests.empty()) {
            const std::string msg =
                    R"({"error":{"context":"External auth service is down"}})";
            incommingResponse.emplace(
                    std::make_unique<AuthResponse>(next, msg));
            requestMap[next++] =
                    std::make_pair(nullptr, incomingRequests.front());
            incomingRequests.pop();
        }
        return;
    }

    // We'll be using the first connection in the list of connections.
    auto* provider = connections.front();

    // Ok, build up a list of all of the server events before locking
    // the provider, so that I don't need to block the provider for a long
    // period of time.
    std::vector<std::unique_ptr<ServerEvent>> events;
    while (!incomingRequests.empty()) {
        auto* startSaslTask =
                dynamic_cast<StartSaslAuthTask*>(incomingRequests.front());
        if (startSaslTask == nullptr) {
            auto* getAuthz = dynamic_cast<GetAuthorizationTask*>(
                    incomingRequests.front());
            if (getAuthz == nullptr) {
                LOG_CRITICAL(
                        "ExternalAuthManagerThread::processRequestQueue(): "
                        "Invalid entry found in request queue!");
                incomingRequests.pop();
                continue;
            }
            events.emplace_back(std::make_unique<GetAuthorizationServerEvent>(
                    next, *getAuthz));
        } else {
            events.emplace_back(
                    std::make_unique<AuthenticationRequestServerEvent>(
                            next,
                            *startSaslTask,
                            haveRbacEntryForUser(
                                    startSaslTask->getUsername())));
        }
        requestMap[next++] = std::make_pair(provider, incomingRequests.front());
        incomingRequests.pop();
    }

    // We cannot hold the internal lock when we try to lock the front
    // end thread as that'll cause a potential deadlock with the "add",
    // "remove" and "responseReceived" as they'll hold the thread
    // mutex and try to acquire the auth mutex in order to enqueue
    // a new connection / response. We've already copied out the
    // entire list of incomming requests so we can release the lock
    // while processing them.
    mutex.unlock();

    // Lock the authentication provider (we're holding a
    // reference counter to the provider, so it can't go away while we're
    // doing this).
    {
        std::lock_guard<std::mutex> guard(provider->getThread().mutex);
        // The provider is locked, so I can move all of the server events
        // over to the providers connection
        for (auto& ev : events) {
            provider->enqueueServerEvent(std::move(ev));
        }
        provider->signalIfIdle();
    }

    // Acquire the lock
    mutex.lock();
}

void ExternalAuthManagerThread::setRbacCacheEpoch(
        std::chrono::steady_clock::time_point tp) {
    using namespace std::chrono;
    const auto age = duration_cast<seconds>(tp.time_since_epoch()).count();
    rbacCacheEpoch.store(static_cast<uint64_t>(age), std::memory_order_release);
}

void ExternalAuthManagerThread::processResponseQueue() {
    auto responses = std::move(incommingResponse);
    while (!responses.empty()) {
        const auto& entry = responses.front();
        auto iter = requestMap.find(entry->opaque);
        if (iter == requestMap.end()) {
            // Unknown id.. ignore
            LOG_WARNING("processResponseQueue(): Ignoring unknown opaque: {}",
                        entry->opaque);
        } else {
            auto* task = iter->second.second;
            requestMap.erase(iter);
            mutex.unlock();
            task->externalResponse(entry->status, entry->payload);
            mutex.lock();
        }
        responses.pop();
    }
}
void ExternalAuthManagerThread::purgePendingDeadConnections() {
    auto pending = std::move(pendingRemoveConnection);
    for (const auto& connection : pending) {
        LOG_WARNING(
                "External authentication manager died. Expect "
                "authentication failures");
        const std::string msg =
                R"({"error":{"context":"External auth service is down"}})";

        for (auto& req : requestMap) {
            if (req.second.first == connection) {
                // We don't need to check if we've got a response queued
                // already, as we'll ignore unknown responses..
                // We need to fix this if we want to redistribute
                // them over to another provider
                incommingResponse.emplace(
                        std::make_unique<AuthResponse>(req.first, msg));
                req.second.first = nullptr;
            }
        }

        // Notify the thread so that it may complete it's shutdown logic
        mutex.unlock();
        {
            std::lock_guard<std::mutex> guard(connection->getThread().mutex);
            connection->decrementRefcount();
            connection->signalIfIdle();
        }
        mutex.lock();
    }
}

void ExternalAuthManagerThread::login(const std::string& user) {
    activeUsers.login(user);
}

void ExternalAuthManagerThread::logoff(const std::string& user) {
    activeUsers.logoff(user);
}

nlohmann::json ExternalAuthManagerThread::getActiveUsers() const {
    return activeUsers.to_json();
}

bool ExternalAuthManagerThread::haveRbacEntryForUser(
        const std::string& user) const {
    const auto then = std::chrono::steady_clock::now() -
                      2 * activeUsersPushInterval.load();
    using namespace std::chrono;
    const auto ts = cb::rbac::getExternalUserTimestamp(user);
    const auto timestamp = ts.value_or(steady_clock::time_point{});
    const uint64_t age = static_cast<uint64_t>(
            duration_cast<seconds>(timestamp.time_since_epoch()).count());

    return (timestamp > then) &&
           (age >= rbacCacheEpoch.load(std::memory_order_acquire));
}

void ExternalAuthManagerThread::ActiveUsers::login(const std::string& user) {
    std::lock_guard<std::mutex> guard(mutex);
    users[user]++;
}

void ExternalAuthManagerThread::ActiveUsers::logoff(const std::string& user) {
    std::lock_guard<std::mutex> guard(mutex);
    auto iter = users.find(user);
    if (iter == users.end()) {
        throw std::runtime_error("ActiveUsers::logoff: Failed to find user");
    }
    iter->second--;
    if (iter->second == 0) {
        users.erase(iter);
    }
}

nlohmann::json ExternalAuthManagerThread::ActiveUsers::to_json() const {
    std::lock_guard<std::mutex> guard(mutex);
    auto ret = nlohmann::json::array();

    for (const auto& entry : users) {
        ret.push_back(entry.first);
    }

    return ret;
}
