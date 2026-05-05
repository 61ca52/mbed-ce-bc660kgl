/*
 * Copyright (c) 2026 Jan Gerrit Gers
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <new>

#include "rtos/ThisThread.h"

#include "CellularLog.h"
#include "CellularUtil.h"

#include "QUECTEL_BC660KGL_CellularStack.h"
#include "QUECTEL_BC660KGL_SocketBuffer.h"

using namespace std::chrono_literals;
using namespace mbed;
using namespace mbed_cellular_util;

// Quectel_BC660K-GL_TCPIP_Application_Note_V1.1 p.10:
// Wait up to 60 s for the URC +QIOPEN: <connectID>,<result>.
#define OP_QIOPEN_TIMEOUT 60s

// Largest UDP datagram / TCP chunk the modem accepts via AT+QISEND.
#define OP_PACKET_SIZE_MAX 1358

// Modem-specific error code surfaced via the +QIOPEN URC.
#define OP_QIOPEN_BIND_FAIL 556

QUECTEL_BC660KGL_CellularStack::QUECTEL_BC660KGL_CellularStack(ATHandler &atHandler, int cid,
                                                                nsapi_ip_stack_t stack_type,
                                                                AT_CellularDevice &device)
    : AT_CellularStack(atHandler, cid, stack_type, device),
      _last_modem_connect_id(-1),
      _last_connect_error(NSAPI_ERROR_NO_CONNECTION)
#if defined(MBED_CONF_NSAPI_OFFLOAD_TLSSOCKET) && (MBED_CONF_NSAPI_OFFLOAD_TLSSOCKET)
      , _tls_sec_level(0),
      _ssl_socket_id(-1),
      _last_ssl_connect_err(-1)
#endif
{
    _at.set_urc_handler("+QIURC: \"recv",     mbed::Callback<void()>(this, &QUECTEL_BC660KGL_CellularStack::urc_qiurc_recv));
    _at.set_urc_handler("+QIURC: \"closed",   mbed::Callback<void()>(this, &QUECTEL_BC660KGL_CellularStack::urc_qiurc_closed));
    _at.set_urc_handler("+QIURC: \"incoming", mbed::Callback<void()>(this, &QUECTEL_BC660KGL_CellularStack::urc_qiurc_incoming));
    _at.set_urc_handler("+QIOPEN: ",          mbed::Callback<void()>(this, &QUECTEL_BC660KGL_CellularStack::urc_qiopen));
    _at.set_urc_handler("SEND FAIL",          mbed::Callback<void()>(this, &QUECTEL_BC660KGL_CellularStack::urc_send_fail));

    // Data format: text-mode send (0), hex-mode receive (1).
    // Hex recv is required because binary payloads (e.g. TLS handshakes) contain
    // bytes that the AT parser would misread as protocol delimiters.
    _at.at_cmd_discard("+QICFG", "=", "%s,%d,%d", "dataformat", 0, 1);

    // Close any modem sockets left over from a previous session.
    reap_orphaned_modem_sockets();

#if defined(MBED_CONF_NSAPI_OFFLOAD_TLSSOCKET) && (MBED_CONF_NSAPI_OFFLOAD_TLSSOCKET)
    _at.set_urc_handler("+QSSLURC: \"recv",   mbed::Callback<void()>(this, &QUECTEL_BC660KGL_CellularStack::urc_qsslurc_recv));
    _at.set_urc_handler("+QSSLURC: \"closed", mbed::Callback<void()>(this, &QUECTEL_BC660KGL_CellularStack::urc_qsslurc_closed));
    _at.set_urc_handler("+QSSLOPEN: ",        mbed::Callback<void()>(this, &QUECTEL_BC660KGL_CellularStack::urc_qsslopen));
    _at.set_urc_handler("+QSSLSEND: ",        mbed::Callback<void()>(this, &QUECTEL_BC660KGL_CellularStack::urc_qsslsend));

    _at.at_cmd_discard("+QSSLCLOSE", "=", "%d,%d", SSL_CONTEXT_ID, SSL_CONNECT_ID);
#endif

    _at.clear_error();

    _socket_buffers = new QUECTEL_BC660KGL_SocketBuffer *[MBED_CONF_QUECTEL_BC660KGL_SOCKET_COUNT]();
}

QUECTEL_BC660KGL_CellularStack::~QUECTEL_BC660KGL_CellularStack() {
    _at.set_urc_handler("+QIURC: \"recv",     nullptr);
    _at.set_urc_handler("+QIURC: \"closed",   nullptr);
    _at.set_urc_handler("+QIURC: \"incoming", nullptr);
    _at.set_urc_handler("+QIOPEN: ",          nullptr);
    _at.set_urc_handler("SEND FAIL",          nullptr);

#if defined(MBED_CONF_NSAPI_OFFLOAD_TLSSOCKET) && (MBED_CONF_NSAPI_OFFLOAD_TLSSOCKET)
    _at.set_urc_handler("+QSSLURC: \"recv",   nullptr);
    _at.set_urc_handler("+QSSLURC: \"closed", nullptr);
    _at.set_urc_handler("+QSSLOPEN: ",        nullptr);
    _at.set_urc_handler("+QSSLSEND: ",        nullptr);
#endif

    for (int i = 0; i < MBED_CONF_QUECTEL_BC660KGL_SOCKET_COUNT; i++) {
        delete _socket_buffers[i];
        _socket_buffers[i] = nullptr;
    }
    delete[] _socket_buffers;
}

nsapi_error_t QUECTEL_BC660KGL_CellularStack::get_ip_address(SocketAddress *address) {
    if (!address) {
        return NSAPI_ERROR_PARAMETER;
    }

    bool ipv4 = false, ipv6 = false;

    _at.lock();
    _at.cmd_start_stop("+CGPADDR", "=", "%d", _cid);
    _at.resp_start("+CGPADDR: ");

    if (_at.info_resp()) {
        _at.skip_param();

        if (_at.read_string(_ip, PDP_IPV6_SIZE) != -1) {
            convert_ipv6(_ip);
            address->set_ip_address(_ip);
            ipv4 = (address->get_ip_version() == NSAPI_IPv4);
            ipv6 = (address->get_ip_version() == NSAPI_IPv6);

            // Only read a second address if the modem supports dual stack.
            if (_device.get_property(AT_CellularDevice::PROPERTY_IPV4V6_PDP_TYPE) &&
                _at.read_string(_ip, PDP_IPV6_SIZE) != -1) {
                convert_ipv6(_ip);
                address->set_ip_address(_ip);
                ipv6 = (address->get_ip_version() == NSAPI_IPv6);
            }
        }
    }
    _at.resp_stop();
    _at.unlock();

    if (ipv4 && ipv6)      _stack_type = IPV4V6_STACK;
    else if (ipv4)         _stack_type = IPV4_STACK;
    else if (ipv6)         _stack_type = IPV6_STACK;

    return (ipv4 || ipv6) ? NSAPI_ERROR_OK : NSAPI_ERROR_NO_ADDRESS;
}

nsapi_error_t QUECTEL_BC660KGL_CellularStack::close_all_sockets() {
    // Close framework-tracked sockets, then mop up anything the modem still has.
    for (int i = 0; i < _device.get_property(AT_CellularDevice::PROPERTY_SOCKET_COUNT); i++) {
        CellularSocket *socket = _socket[i];
        if (socket == nullptr || socket->closed) {
            continue;
        }
        tr_info("Closing %d", socket->id);
        socket_close(socket);
    }
    reap_orphaned_modem_sockets();
    return NSAPI_ERROR_OK;
}

void QUECTEL_BC660KGL_CellularStack::reap_orphaned_modem_sockets() {
    // AT+QISTATE=0,<cid> reply (TCPIP App Note 2.3.3): one line per open
    //   connection: <connectid>,<service>,<host>,<rport>,<lport>,
    //   <state>,<contextid>,<access_mode>. No lines = no connections.
    bool active[MBED_CONF_QUECTEL_BC660KGL_SOCKET_COUNT] = {false};
    int count = 0;

    _at.lock();
    _at.cmd_start_stop("+QISTATE", "=", "%d,%d", 0, _cid);
    _at.resp_start("+QISTATE: ");
    while (_at.info_resp() && count < MBED_CONF_QUECTEL_BC660KGL_SOCKET_COUNT) {
        count++;
        int id = _at.read_int();
        _at.skip_param(7);
        if (id >= 0 && id < MBED_CONF_QUECTEL_BC660KGL_SOCKET_COUNT) {
            active[id] = true;
        }
    }
    _at.resp_stop();

    if (_at.unlock_return_error() != NSAPI_ERROR_OK) {
        // QISTATE failed; nothing safe to close.
        _at.clear_error();
        return;
    }

    for (int i = 0; i < MBED_CONF_QUECTEL_BC660KGL_SOCKET_COUNT; i++) {
        if (active[i]) {
            tr_info("Closing orphaned modem socket %d", i);
            _at.at_cmd_discard("+QICLOSE", "=", "%d", i);
        }
    }
    _at.clear_error();
}

nsapi_error_t QUECTEL_BC660KGL_CellularStack::socket_listen(nsapi_socket_t, int) {
    return NSAPI_ERROR_UNSUPPORTED;
}

nsapi_error_t QUECTEL_BC660KGL_CellularStack::socket_accept(nsapi_socket_t, nsapi_socket_t *, SocketAddress *) {
    return NSAPI_ERROR_UNSUPPORTED;
}

nsapi_error_t QUECTEL_BC660KGL_CellularStack::socket_connect(nsapi_socket_t handle, const SocketAddress &address) {
    CellularSocket *socket = (CellularSocket *)handle;
    int request_connect_id = find_socket_index(socket);
    MBED_ASSERT(request_connect_id != -1);

#if defined(MBED_CONF_NSAPI_OFFLOAD_TLSSOCKET) && (MBED_CONF_NSAPI_OFFLOAD_TLSSOCKET)
    if (socket->tls_socket) {
        return ssl_socket_connect(socket, address, request_connect_id);
    }
#else
    if (socket->tls_socket) {
        return NSAPI_ERROR_UNSUPPORTED;
    }
#endif

    if (socket->proto != NSAPI_TCP) {
        return NSAPI_ERROR_UNSUPPORTED;
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        _last_modem_connect_id = -1;
        _last_connect_error = NSAPI_ERROR_NO_CONNECTION;
        _event_flag.clear(CONNECTED_FLAG);

        // access_mode = 1 (direct push mode)
        nsapi_error_t err = _at.at_cmd_discard("+QIOPEN", "=",
                                                "%d,%d,%s,%s,%d,%d,%d",
                                                _cid, request_connect_id, "TCP",
                                                address.get_ip_address(),
                                                address.get_port(),
                                                socket->localAddress.get_port(),
                                                1);
        if (err != NSAPI_ERROR_OK) {
            tr_error("QIOPEN TCP %d failed: %d", request_connect_id, err);
            _at.at_cmd_discard("+QICLOSE", "=", "%d", request_connect_id);
            return err;
        }

        uint32_t flag = _event_flag.wait_any_for(CONNECTED_FLAG, OP_QIOPEN_TIMEOUT, true);
        if (flag & osFlagsError) {
            tr_error("QIOPEN URC timeout (sock %d)", request_connect_id);
            _at.at_cmd_discard("+QICLOSE", "=", "%d", request_connect_id);
            return NSAPI_ERROR_TIMEOUT;
        }

        int modem_connect_id = _last_modem_connect_id;
        int connection_err = _last_connect_error;

        if (connection_err == OP_QIOPEN_BIND_FAIL || modem_connect_id < 0) {
            tr_error("QIOPEN bind fail (sock %d, err %d, modem_id %d)",
                     request_connect_id, connection_err, modem_connect_id);
            socket->id = -1;
            return NSAPI_ERROR_PARAMETER;
        }

        if (modem_connect_id != request_connect_id) {
            tr_error("QIOPEN id mismatch (req %d, got %d)",
                     request_connect_id, modem_connect_id);
            _at.at_cmd_discard("+QICLOSE", "=", "%d", modem_connect_id);
            return NSAPI_ERROR_NO_SOCKET;
        }

        if (connection_err == NSAPI_ERROR_OK) {
            tr_info("Socket %d open", request_connect_id);
            _ip_ver_sendto = NSAPI_IPv4;
            socket->id = request_connect_id;
            socket->remoteAddress = address;
            socket->connected = true;
            return NSAPI_ERROR_OK;
        }
        // else: one retry
    }

    // BC660K-GL docs: "If the connection fails, AT+QICLOSE=<connectID> should be executed."
    tr_error("TCP connect failed after retries (sock %d)", request_connect_id);
    _at.at_cmd_discard("+QICLOSE", "=", "%d", request_connect_id);
    return NSAPI_ERROR_NO_CONNECTION;
}

nsapi_size_or_error_t QUECTEL_BC660KGL_CellularStack::socket_sendto(nsapi_socket_t handle, const SocketAddress &addr,
                                                                     const void *data, unsigned size) {
    CellularSocket *socket = (CellularSocket *)handle;
    if (!socket) {
        return NSAPI_ERROR_NO_SOCKET;
    }

    if (socket->closed && !socket->pending_bytes) {
        tr_info("sendto socket %d closed", socket->id);
        return NSAPI_ERROR_NO_CONNECTION;
    }

    if (size == 0) {
        return (socket->proto == NSAPI_UDP) ? NSAPI_ERROR_UNSUPPORTED : (nsapi_size_or_error_t)0;
    }

    if (!is_addr_stack_compatible(addr)) {
        return NSAPI_ERROR_PARAMETER;
    }

    nsapi_size_or_error_t ret_val = NSAPI_ERROR_OK;
    if (socket->id == -1) {
        _ip_ver_sendto = addr.get_ip_version();

        // DO NOT hold the AT lock around create_socket_impl: it issues AT+QIOPEN
        // (returns OK immediately) and then waits for the async +QIOPEN URC, whose
        // handler (urc_qiopen) needs the AT lock. The base class
        // implementation holds the lock through this wait, causing a deadlock.
        ret_val = create_socket_impl(socket);

        if (ret_val != NSAPI_ERROR_OK) {
            tr_error("Socket %d create %s error %d", find_socket_index(socket), addr.get_ip_address(), ret_val);
            return ret_val;
        }
    }

    _at.lock();

    // For UDP (used by DTLS), discard any stale recv data before sending. On DTLS
    // retransmission, the SocketBuffer may still hold records from a previous
    // server flight with a different server_random; reading them would stall the
    // handshake. First-send: buffer is empty (no-op). Post-handshake echo: recv()
    // drains the buffer before the next send.
    if (socket->proto == NSAPI_UDP) {
        QUECTEL_BC660KGL_SocketBuffer *buf = get_or_create_socket_buffer(socket->id);
        if (buf && !buf->empty()) {
            tr_info("Socket %d flushing stale recv data before send", find_socket_index(socket));
            buf->flush();
            // Clear DATA_FLAG so the next recv() correctly waits for new data
            // instead of returning 0 (which mbedtls treats as EOF).
            _event_flag.clear(DATA_FLAG);
        }
    }

    ret_val = socket_sendto_impl(socket, addr, data, size);
    _at.unlock();

    if (ret_val >= 0) {
        tr_info("Socket %d sent %d bytes to %s port %d", find_socket_index(socket), ret_val, addr.get_ip_address(), addr.get_port());
    } else if (ret_val != NSAPI_ERROR_WOULD_BLOCK) {
        tr_error("Socket %d sendto %s error %d", find_socket_index(socket), addr.get_ip_address(), ret_val);
    }

    return ret_val;
}

nsapi_size_or_error_t QUECTEL_BC660KGL_CellularStack::socket_recvfrom(nsapi_socket_t handle, SocketAddress *addr,
                                                                       void *buffer, unsigned size) {
    CellularSocket *socket = (CellularSocket *)handle;
    if (!socket) {
        return NSAPI_ERROR_NO_SOCKET;
    }

    if (socket->closed) {
        tr_info("recvfrom socket %d closed", socket->id);
        return 0;
    }

    if (socket->id == -1) {
        // Same rationale as in socket_sendto: do NOT hold the AT lock here.
        nsapi_size_or_error_t create_ret = create_socket_impl(socket);
        if (create_ret != NSAPI_ERROR_OK) {
            tr_error("Socket %d create error %d", find_socket_index(socket), create_ret);
            return create_ret;
        }
    }

    // Data retrieval is event-driven (no AT commands), so no at.lock() is needed.
    nsapi_size_or_error_t ret_val = socket_recvfrom_impl(socket, addr, buffer, size);

    if (socket->closed) {
        tr_info("recvfrom socket %d closed", socket->id);
        return 0;
    }

    if (ret_val >= 0) {
        if (addr) {
            tr_info("Socket %d recv %d bytes from %s port %d", find_socket_index(socket), ret_val, addr->get_ip_address(), addr->get_port());
        } else {
            tr_info("Socket %d recv %d bytes", find_socket_index(socket), ret_val);
        }
    } else if (ret_val != NSAPI_ERROR_WOULD_BLOCK) {
        tr_error("Socket %d recv error %d", find_socket_index(socket), ret_val);
    }

    return ret_val;
}

nsapi_error_t QUECTEL_BC660KGL_CellularStack::socket_close_impl(int sock_id) {
    CellularSocket *socket = find_socket(sock_id);
    if (socket == nullptr) {
        return NSAPI_ERROR_NO_SOCKET;
    }

#if defined(MBED_CONF_NSAPI_OFFLOAD_TLSSOCKET) && (MBED_CONF_NSAPI_OFFLOAD_TLSSOCKET)
    if (socket->tls_socket) {
        return ssl_socket_close_impl(sock_id);
    }
#endif

    _at.set_at_timeout(OP_QIOPEN_TIMEOUT);
    nsapi_error_t err = _at.at_cmd_discard("+QICLOSE", "=", "%d", sock_id);
    _at.restore_at_timeout();
    return err;
}

nsapi_error_t QUECTEL_BC660KGL_CellularStack::create_socket_impl(CellularSocket *socket) {
    // TCP opens lazily in socket_connect(); only UDP needs work here.
    if (socket->proto != NSAPI_UDP) {
        return NSAPI_ERROR_OK;
    }
    int request_connect_id = find_socket_index(socket);
    MBED_ASSERT(request_connect_id != -1);
    return open_udp_socket(socket, request_connect_id);
}

nsapi_size_or_error_t QUECTEL_BC660KGL_CellularStack::socket_sendto_impl(CellularSocket *socket, const SocketAddress &address,
                                                                          const void *data, nsapi_size_t size) {
#if defined(MBED_CONF_NSAPI_OFFLOAD_TLSSOCKET) && (MBED_CONF_NSAPI_OFFLOAD_TLSSOCKET)
    if (socket->tls_socket) {
        return ssl_socket_sendto_impl(socket, data, size);
    }
#endif

    if (_ip_ver_sendto != address.get_ip_version()) {
        _ip_ver_sendto = address.get_ip_version();
        socket_close_impl(socket->id);
        create_socket_impl(socket);
    }

    // AT+QISEND reply is \r\nOK\r\n\r\nSEND OK\r\n (success) or \r\nOK\r\n\r\nSEND FAIL\r\n.
    // Using resp_stop() after write_bytes() consumes the "OK" via the normal
    // response terminator; "SEND OK"/"SEND FAIL" is handled by the next resp()
    // iteration or by process_oob(). The older resp_start("\r\n") approach race-
    // condition'd the parser and could wait 10 s for a second OK that never came.

    if (socket->proto == NSAPI_UDP) {
        if (size > OP_PACKET_SIZE_MAX) {
            return NSAPI_ERROR_PARAMETER;
        }

        _at.cmd_start_stop("+QISEND", "=", "%d,%s,%d,%d",
                           socket->id, address.get_ip_address(), address.get_port(), size);
        _at.resp_start(">");
        nsapi_size_or_error_t ret = _at.write_bytes((uint8_t *)data, size);
        _at.resp_stop();

        if (_at.get_last_error() != NSAPI_ERROR_OK || ret == 0) {
            tr_error("UDP send failed");
            _at.clear_error();
            _at.flush();
            return NSAPI_ERROR_DEVICE_ERROR;
        }
        return ret;
    }

    // TCP: chunk at OP_PACKET_SIZE_MAX
    const char *buf = (const char *)data;
    nsapi_size_t remaining = size;
    nsapi_size_or_error_t total = 0;
    while (remaining > 0) {
        nsapi_size_t blk = (remaining < OP_PACKET_SIZE_MAX) ? remaining : OP_PACKET_SIZE_MAX;

        _at.cmd_start_stop("+QISEND", "=", "%d,%d", socket->id, blk);
        _at.resp_start(">");
        nsapi_size_t sent = _at.write_bytes((uint8_t *)buf, blk);
        _at.resp_stop();

        if (_at.get_last_error() != NSAPI_ERROR_OK || sent == 0) {
            tr_error("TCP send failed");
            _at.clear_error();
            _at.flush();
            return NSAPI_ERROR_DEVICE_ERROR;
        }

        total += sent;
        buf += sent;
        remaining -= sent;
    }
    return total;
}

nsapi_size_or_error_t QUECTEL_BC660KGL_CellularStack::socket_recvfrom_impl(CellularSocket *socket, SocketAddress *address,
                                                                            void *buffer, nsapi_size_t size) {
    QUECTEL_BC660KGL_SocketBuffer *socket_buffer = get_or_create_socket_buffer(socket->id);
    if (!socket_buffer) {
        return NSAPI_ERROR_NO_MEMORY;
    }

    if (socket_buffer->empty()) {
        // NB-IoT round-trip latency is typically 3-5 s for UDP; TCP can be faster.
        auto wait = (socket->proto == NSAPI_UDP) ? 5s : 1s;
        uint32_t flag = _event_flag.wait_any_for(DATA_FLAG, wait, true);
        if (flag & osFlagsError) {
            return NSAPI_ERROR_WOULD_BLOCK;
        }
    } else {
        _event_flag.clear(DATA_FLAG);
    }

    // UDP: one datagram per call (preserves DTLS record framing).
    // TCP: as many bytes as available up to 'size' (byte-stream).
    nsapi_size_or_error_t result = (socket->proto == NSAPI_UDP)
        ? (nsapi_size_or_error_t)socket_buffer->get_datagram((uint8_t *)buffer, size, address)
        : (nsapi_size_or_error_t)socket_buffer->get_data((uint8_t *)buffer, size);

    // A zero-byte result from a signaled DATA_FLAG (e.g. stale flag after a flush)
    // would be interpreted by mbedtls as EOF, aborting the DTLS handshake with
    // MBEDTLS_ERR_SSL_CONN_EOF. Surface as WOULD_BLOCK instead.
    return (result == 0) ? NSAPI_ERROR_WOULD_BLOCK : result;
}

QUECTEL_BC660KGL_SocketBuffer *QUECTEL_BC660KGL_CellularStack::get_or_create_socket_buffer(int id) {
    MBED_ASSERT(id < MBED_CONF_QUECTEL_BC660KGL_SOCKET_COUNT);
    if (!_socket_buffers[id]) {
        _socket_buffers[id] = new (std::nothrow) QUECTEL_BC660KGL_SocketBuffer();
    }
    return _socket_buffers[id];
}

nsapi_error_t QUECTEL_BC660KGL_CellularStack::open_udp_socket(CellularSocket *socket, int request_connect_id) {
    int modem_connect_id = -1;
    int err = -1;

    for (int attempt = 0; attempt < 2; attempt++) {
        _event_flag.clear(CONNECTED_FLAG);

        if (socket->connected) {
            // UDP connected: bind to a specific remote endpoint.
            _at.at_cmd_discard("+QIOPEN", "=", "%d,%d,%s,%s,%d",
                               _cid, request_connect_id, "UDP",
                               socket->remoteAddress.get_ip_address(),
                               socket->remoteAddress.get_port());
        } else {
            // UDP SERVICE (unconnected): loopback placeholder for remote addr.
            _at.at_cmd_discard("+QIOPEN", "=", "%d,%d,%s,%s,%d,%d,%d",
                               _cid, request_connect_id, "UDP SERVICE",
                               (_ip_ver_sendto == NSAPI_IPv4) ? "127.0.0.1" : "0:0:0:0:0:0:0:1",
                               1,
                               socket->localAddress.get_port(),
                               1);
        }

        handle_open_socket_response(modem_connect_id, err);

        if (_at.get_last_error() != NSAPI_ERROR_OK || err == NSAPI_ERROR_OK) {
            break;
        }

        if (err == OP_QIOPEN_BIND_FAIL) {
            tr_error("UDP bind fail (sock %d, err %d)", request_connect_id, err);
            socket->id = -1;
            return NSAPI_ERROR_PARAMETER;
        }

        socket_close_impl(modem_connect_id);
    }

    if (err == NSAPI_ERROR_OK && modem_connect_id != request_connect_id) {
        tr_warn("QIOPEN id mismatch (req %d, got %d), closing",
                request_connect_id, modem_connect_id);
        socket_close_impl(modem_connect_id);
    }

    nsapi_error_t ret_val = _at.get_last_error();
    if (ret_val == NSAPI_ERROR_OK && modem_connect_id == request_connect_id) {
        socket->id = request_connect_id;
    }
    return ret_val;
}

void QUECTEL_BC660KGL_CellularStack::handle_open_socket_response(int &modem_connect_id, int &err) {
    modem_connect_id = -1;
    err = NSAPI_ERROR_NO_CONNECTION;
    uint32_t flag = _event_flag.wait_any_for(CONNECTED_FLAG, OP_QIOPEN_TIMEOUT, true);
    if (flag & osFlagsError) {
        err = NSAPI_ERROR_TIMEOUT;
    } else {
        modem_connect_id = _last_modem_connect_id;
        err = _last_connect_error;
    }
}

void QUECTEL_BC660KGL_CellularStack::receive_into_buffer(QUECTEL_BC660KGL_SocketBuffer *buffer,
                                                         nsapi_size_t length, bool mark_boundary) {
    // Data arrives as hex-encoded string (QICFG dataformat 0,1). read_hex_string
    // decodes hex pairs ("4156" -> "AV") into binary bytes.
    nsapi_size_t to_read = (length < BC660KGL_RECV_BUF_SIZE) ? length : BC660KGL_RECV_BUF_SIZE;
    ssize_t decoded = _at.read_hex_string((char *)_recv_buf, to_read);

    if (decoded <= 0) {
        return;
    }

    // Distribute decoded bytes into SocketBuffer chunks (max 255 bytes each).
    // The last chunk may be marked as a datagram boundary so get_datagram()
    // returns exactly one UDP datagram per call (required for DTLS framing).
    nsapi_size_t offset = 0;
    while (offset < (nsapi_size_t)decoded) {
        sb_chunk_t *chunk = buffer->get_chunk();
        if (!chunk) {
            break;
        }

        nsapi_size_t chunk_len = (nsapi_size_t)decoded - offset;
        if (chunk_len > MBED_CONF_QUECTEL_BC660KGL_SOCKET_BUFFER_CHUNK_SIZE) {
            chunk_len = MBED_CONF_QUECTEL_BC660KGL_SOCKET_BUFFER_CHUNK_SIZE;
        }

        memcpy(chunk->data, _recv_buf + offset, chunk_len);
        chunk->size = (uint8_t)chunk_len;
        chunk->boundary = mark_boundary && (offset + chunk_len >= (nsapi_size_t)decoded);
        buffer->put_chunk(chunk);
        offset += chunk_len;
    }
}

void QUECTEL_BC660KGL_CellularStack::urc_qiurc(urc_qiurc_type_t urc_type) {
    _at.lock();
    _at.skip_param();
    const int socket_id = _at.read_int();

    if (_at.get_last_error() != NSAPI_ERROR_OK) {
        _at.unlock();
        return;
    }

    CellularSocket *socket = find_socket(socket_id);
    if (!socket) {
        _at.flush();
        _at.unlock();
        return;
    }

    switch (urc_type) {
    case URC_RECV:
    {
        QUECTEL_BC660KGL_SocketBuffer *buffer = get_or_create_socket_buffer(socket_id);
        if (!buffer) {
            _at.flush();
            _at.unlock();
            return;
        }

        nsapi_size_t size = _at.read_int();

        // UDP SERVICE URC: +QIURC: "recv",<id>,<len>,<remote_ip>,<remote_port>,<data>
        // TCP URC:         +QIURC: "recv",<id>,<len>,<data>
        if (socket->proto == NSAPI_UDP) {
            char remote_ip[NSAPI_IP_SIZE] = {};
            _at.read_string(remote_ip, sizeof(remote_ip));
            int remote_port = _at.read_int();
            buffer->set_source_address(SocketAddress(remote_ip, remote_port));
        }

        receive_into_buffer(buffer, size, /*mark_boundary=*/true);

        tr_info("Receiving %d byte on %d", size, socket_id);
        _event_flag.set(DATA_FLAG);
        break;
    }
    case URC_CLOSED:
        tr_info("Socket closed %d", socket_id);
        socket->closed = true;
        break;

    case URC_INCOMING:
        tr_info("Incoming connection %d", socket_id);
        break;
    }
    _at.unlock();

    if (socket->_cb) {
        socket->_cb(socket->_data);
    }
}

void QUECTEL_BC660KGL_CellularStack::urc_qiurc_recv()     { urc_qiurc(URC_RECV); }
void QUECTEL_BC660KGL_CellularStack::urc_qiurc_closed()   { urc_qiurc(URC_CLOSED); }
void QUECTEL_BC660KGL_CellularStack::urc_qiurc_incoming() { urc_qiurc(URC_INCOMING); }

void QUECTEL_BC660KGL_CellularStack::urc_send_fail() {
    tr_error("SEND FAIL received from modem");
}

void QUECTEL_BC660KGL_CellularStack::urc_qiopen() {
    _at.lock();
    _last_modem_connect_id = _at.read_int();
    _last_connect_error    = _at.read_int();

    if (_at.unlock_return_error() != NSAPI_ERROR_OK) {
        return;
    }
    _event_flag.set(CONNECTED_FLAG);
}

// SSL/TLS offload via Quectel_BC660K-GL_SSL_Application_Note_V1-1 constrains
// us to: contextid=0 + connectid=0 (one SSL socket at a time), TLS 1.0/1.1/1.2
// and DTLS 1.0/1.2, PEM-only credentials, 1024 B max send (text mode), and
// 1400 B max recv per URC.

#if defined(MBED_CONF_NSAPI_OFFLOAD_TLSSOCKET) && (MBED_CONF_NSAPI_OFFLOAD_TLSSOCKET)

// SSL connection timeout: 120 s covers slow NB-IoT TLS handshakes.
// Modem's own SSL timeout defaults to 90 s (AT+QSSLCFG "timeout").
#define OP_QSSLOPEN_TIMEOUT 120s

// Max payload per AT+QSSLSEND in text mode (SSL App Note section 3.3).
#define OP_SSL_PACKET_SIZE_MAX 1024

nsapi_error_t QUECTEL_BC660KGL_CellularStack::setsockopt(nsapi_socket_t handle, int level,
                                                          int optname, const void *optval, unsigned optlen) {
    if (level != NSAPI_TLSSOCKET_LEVEL) {
        return AT_CellularStack::setsockopt(handle, level, optname, optval, optlen);
    }

    CellularSocket *socket = (CellularSocket *)handle;

    switch (optname) {
    case NSAPI_TLSSOCKET_SET_HOSTNAME:
        // BC660K-GL resolves hostnames in AT+QSSLOPEN; no separate config needed.
        return NSAPI_ERROR_OK;

    case NSAPI_TLSSOCKET_SET_CACERT:
    {
        nsapi_error_t err = ssl_upload_credential("cacert", optval, optlen);
        if (err == NSAPI_ERROR_OK && _tls_sec_level < 1) {
            _tls_sec_level = 1;
        }
        return err;
    }

    case NSAPI_TLSSOCKET_SET_CLCERT:
    {
        nsapi_error_t err = ssl_upload_credential("clientcert", optval, optlen);
        if (err == NSAPI_ERROR_OK) {
            _tls_sec_level = 2;
        }
        return err;
    }

    case NSAPI_TLSSOCKET_SET_CLKEY:
    {
        nsapi_error_t err = ssl_upload_credential("clientkey", optval, optlen);
        if (err == NSAPI_ERROR_OK) {
            _tls_sec_level = 2;
        }
        return err;
    }

    case NSAPI_TLSSOCKET_ENABLE:
    {
        bool enabled = optval ? *(const bool *)optval : false;
        socket->tls_socket = enabled;

        if (enabled) {
            // SSL context parameters are NOT persisted; set them every session.
            _at.at_cmd_discard("+QSSLCFG", "=", "%d,%d,%s,%d",
                               SSL_CONTEXT_ID, SSL_CONNECT_ID, "seclevel", _tls_sec_level);
            _at.at_cmd_discard("+QSSLCFG", "=", "%d,%d,%s,%d",
                               SSL_CONTEXT_ID, SSL_CONNECT_ID, "sslversion", 4); // all; server negotiates
            _at.at_cmd_discard("+QSSLCFG", "=", "%d,%d,%s,%d,%d",
                               SSL_CONTEXT_ID, SSL_CONNECT_ID, "dataformat", 0, 1);

            if (_at.get_last_error() != NSAPI_ERROR_OK) {
                tr_error("SSL context configuration failed");
                _at.clear_error();
                return NSAPI_ERROR_DEVICE_ERROR;
            }
        }
        return NSAPI_ERROR_OK;
    }

    default:
        return NSAPI_ERROR_UNSUPPORTED;
    }
}

nsapi_error_t QUECTEL_BC660KGL_CellularStack::ssl_upload_credential(const char *type, const void *data, unsigned len) {
    if (!data || len == 0) {
        return NSAPI_ERROR_PARAMETER;
    }

    _at.lock();

    // AT+QSSLCFG=<contextid>,<connectid>,"<type>" -> ">" prompt, write PEM + Ctrl+Z
    // Reply: +QSSLCFG: <contextid>,<connectid>,"<type>",<checksum>\r\nOK\r\n
    _at.cmd_start_stop("+QSSLCFG", "=", "%d,%d,%s", SSL_CONTEXT_ID, SSL_CONNECT_ID, type);
    _at.resp_start(">");

    if (_at.get_last_error() != NSAPI_ERROR_OK) {
        tr_error("SSL %s upload: no prompt", type);
        return _at.unlock_return_error();
    }

    _at.write_bytes((const uint8_t *)data, len);
    const uint8_t ctrlz = 0x1A;
    _at.write_bytes(&ctrlz, 1);
    _at.resp_stop();

    nsapi_error_t err = _at.unlock_return_error();
    if (err != NSAPI_ERROR_OK) {
        tr_error("SSL %s upload failed: %d", type, err);
    } else {
        tr_info("SSL %s uploaded (%u bytes)", type, len);
    }
    return err;
}

nsapi_error_t QUECTEL_BC660KGL_CellularStack::ssl_socket_connect(CellularSocket *socket,
                                                                   const SocketAddress &address,
                                                                   int request_connect_id) {
    if (_tls_sec_level == 0) {
        tr_warn("SSL connect with seclevel=0 (no certificate verification)");
    }

    _ssl_socket_id = request_connect_id;
    _last_ssl_connect_err = -1;
    _event_flag.clear(CONNECTED_FLAG);

    // AT+QSSLOPEN: connect_mode=0 is the only mode supported by BC660K-GL.
    nsapi_error_t err = _at.at_cmd_discard("+QSSLOPEN", "=", "%d,%d,%s,%d,%d",
                                            SSL_CONTEXT_ID, SSL_CONNECT_ID,
                                            address.get_ip_address(),
                                            address.get_port(),
                                            0);
    if (err != NSAPI_ERROR_OK) {
        tr_error("AT+QSSLOPEN command failed: %d", err);
        goto close_exit;
    }

    {
        uint32_t flag = _event_flag.wait_any_for(CONNECTED_FLAG, OP_QSSLOPEN_TIMEOUT, true);
        if (flag & osFlagsError) {
            tr_error("SSL connect timeout");
            err = NSAPI_ERROR_TIMEOUT;
            goto close_exit;
        }
    }

    if (_last_ssl_connect_err != 0) {
        tr_error("SSL connect error: %d", _last_ssl_connect_err);
        switch (_last_ssl_connect_err) {
        case -3:
        case -4:
        case -5: err = NSAPI_ERROR_AUTH_FAILURE; break;
        case -7: err = NSAPI_ERROR_TIMEOUT;      break;
        default: err = NSAPI_ERROR_CONNECTION_LOST; break;
        }
        goto close_exit;
    }

    tr_info("SSL socket connected to %s:%d", address.get_ip_address(), address.get_port());
    socket->id = request_connect_id;
    socket->remoteAddress = address;
    socket->connected = true;
    return NSAPI_ERROR_OK;

close_exit:
    _at.at_cmd_discard("+QSSLCLOSE", "=", "%d,%d", SSL_CONTEXT_ID, SSL_CONNECT_ID);
    _at.clear_error();
    _ssl_socket_id = -1;
    return err;
}

nsapi_size_or_error_t QUECTEL_BC660KGL_CellularStack::ssl_socket_sendto_impl(CellularSocket *socket,
                                                                               const void *data,
                                                                               nsapi_size_t size) {
    const uint8_t *buf = (const uint8_t *)data;
    nsapi_size_or_error_t total_sent = 0;
    nsapi_size_t remaining = size;

    while (remaining > 0) {
        nsapi_size_t blk = (remaining > OP_SSL_PACKET_SIZE_MAX) ? OP_SSL_PACKET_SIZE_MAX : remaining;

        _at.cmd_start_stop("+QSSLSEND", "=", "%d,%d,%d", SSL_CONTEXT_ID, SSL_CONNECT_ID, blk);
        _at.resp_start(">");
        nsapi_size_t sent = _at.write_bytes(buf, blk);
        _at.resp_stop();

        if (_at.get_last_error() != NSAPI_ERROR_OK || sent == 0) {
            tr_error("SSL send failed");
            _at.clear_error();
            _at.flush();
            return NSAPI_ERROR_DEVICE_ERROR;
        }

        total_sent += sent;
        buf += sent;
        remaining -= sent;
    }
    return total_sent;
}

nsapi_error_t QUECTEL_BC660KGL_CellularStack::ssl_socket_close_impl(int sock_id) {
    nsapi_error_t err = _at.at_cmd_discard("+QSSLCLOSE", "=", "%d,%d", SSL_CONTEXT_ID, SSL_CONNECT_ID);
    _tls_sec_level = 0;
    _ssl_socket_id = -1;
    return err;
}

// +QSSLURC: "recv",<contextid>,<connectid>,<length>,<hex_data>
void QUECTEL_BC660KGL_CellularStack::urc_qsslurc_recv() {
    _at.lock();
    _at.skip_param(); // closing quote of "recv"
    _at.read_int();   // contextid (discard)
    _at.read_int();   // connectid (discard)
    nsapi_size_t length = _at.read_int();

    if (_at.get_last_error() != NSAPI_ERROR_OK || _ssl_socket_id < 0) {
        _at.flush();
        _at.unlock();
        return;
    }

    CellularSocket *socket = find_socket(_ssl_socket_id);
    QUECTEL_BC660KGL_SocketBuffer *buffer = socket ? get_or_create_socket_buffer(_ssl_socket_id) : nullptr;
    if (!socket || !buffer) {
        _at.flush();
        _at.unlock();
        return;
    }

    receive_into_buffer(buffer, length, /*mark_boundary=*/false);

    tr_info("SSL recv %d bytes", (int)length);
    _event_flag.set(DATA_FLAG);
    _at.unlock();

    if (socket->_cb) {
        socket->_cb(socket->_data);
    }
}

// +QSSLURC: "closed",<contextid>,<connectid>
void QUECTEL_BC660KGL_CellularStack::urc_qsslurc_closed() {
    _at.lock();
    _at.skip_param();
    _at.read_int(); // contextid
    _at.read_int(); // connectid
    _at.unlock();

    if (_ssl_socket_id >= 0) {
        CellularSocket *socket = find_socket(_ssl_socket_id);
        if (socket) {
            tr_info("SSL socket closed by peer");
            socket->closed = true;
            if (socket->_cb) {
                socket->_cb(socket->_data);
            }
        }
    }
}

// +QSSLOPEN: <contextid>,<connectid>,<err>
void QUECTEL_BC660KGL_CellularStack::urc_qsslopen() {
    _at.lock();
    // Use read_int() instead of skip_param(): skip_param() does not reliably
    // advance the parser in URC context here, causing stale reads.
    _at.read_int(); // contextid
    _at.read_int(); // connectid
    _last_ssl_connect_err = _at.read_int();
    _at.unlock();

    _event_flag.set(CONNECTED_FLAG);
}

// +QSSLSEND: <contextid>,<connectid>,<err>
void QUECTEL_BC660KGL_CellularStack::urc_qsslsend() {
    _at.lock();
    _at.read_int(); // contextid
    _at.read_int(); // connectid
    int err = _at.read_int();
    _at.unlock();

    if (err != 0) {
        tr_error("SSL send result: err=%d", err);
    }
}

#endif // MBED_CONF_NSAPI_OFFLOAD_TLSSOCKET
