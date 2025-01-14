/*
 *     Copyright 2020-Present Couchbase, Inc.
 *
 *   Use of this software is governed by the Business Source License included
 *   in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
 *   in that file, in accordance with the Business Source License, use of this
 *   software will be governed by the Apache License, Version 2.0, included in
 *   the file licenses/APL2.txt.
 */

#pragma once

#include "network_interface.h"
#include "server_socket.h"

#include <event2/util.h>
#include <libevent/utilities.h>
#include <platform/socket.h>
#include <array>
#include <atomic>

/**
 * The NetworkInterfaceManager will eventually be responsible for adding /
 * removing network interfaces and keep control of all of the network
 * interfaces memcached currently expose.
 *
 * Right now it is just a copy of what used to be the old dispatcher
 * logic
 */
class NetworkInterfaceManager {
public:
    /**
     * Create a new instance and bind it to a given event base (the same
     * base as all of the listening sockets use)
     */
    explicit NetworkInterfaceManager(event_base* base);

    /**
     * Signal the network interface from any other thread (by sending
     * a message over the notification pipe)
     */
    void signal();

protected:
    /**
     * Create the file containing all of the interfaces we're currently
     * listening to.
     *
     * @param terminate set to true if the program should terminate if an
     *                  error occurs while writing the file (normally this
     *                  is during bootstrap)
     */
    void writeInterfaceFile(bool terminate);

    /**
     * Create a new interface by using the provided attributes
     *
     * @return true if success, false otherwise
     */
    bool createInterface(const std::string& tag,
                         const std::string& host,
                         in_port_t port,
                         bool system_port,
                         const std::string& sslkey,
                         const std::string& sslcert,
                         NetworkInterface::Protocol iv4,
                         NetworkInterface::Protocol iv6);

    /// The event handler called from libevent
    void event_handler();

    /**
     * The event_handler function called from libevent
     *
     * @param fd The file descriptor where the event happened
     * @param mask The type of event
     * @param arg pointer to "this"
     */
    static void event_handler(evutil_socket_t fd, short mask, void* arg);

    std::array<SOCKET, 2> pipe = {{INVALID_SOCKET, INVALID_SOCKET}};
    cb::libevent::unique_event_ptr event;
    std::atomic_bool check_listen_conn{};
    std::vector<std::unique_ptr<ServerSocket>> listen_conn;
    std::pair<in_port_t, sa_family_t> prometheus_conn;
};

/// The one and only instance of the network interface manager.
extern std::unique_ptr<NetworkInterfaceManager> networkInterfaceManager;
