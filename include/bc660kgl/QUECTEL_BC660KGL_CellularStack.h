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

#ifndef QUECTEL_BC660KGL_CELLULARSTACK_H_
#define QUECTEL_BC660KGL_CELLULARSTACK_H_

#include "mbed.h"
#include "AT_CellularStack.h"
#include "QUECTEL_BC660KGL_SocketBuffer.h"

#define BC660KGL_RECV_BUF_SIZE 1358

namespace mbed
{

    /**
     * @addtogroup at-hayes
     * @ingroup Cellular
     * @{
     */


    class QUECTEL_BC660KGL_CellularStack : public AT_CellularStack
    {
    public:
        enum urc_qiurc_type_t
        {
            URC_RECV,
            URC_CLOSED,
            URC_INCOMING
        };

        /** EventFlags bits for socket-open and data URCs. */
        enum SocketEventFlags {
            CONNECTED_FLAG = 0x1u, ///< +QIOPEN / +QSSLOPEN result received.
            DATA_FLAG      = 0x2u, ///< +QIURC / +QSSLURC payload received.
        };

        QUECTEL_BC660KGL_CellularStack(ATHandler &atHandler, int cid,
                                       nsapi_ip_stack_t stack_type, AT_CellularDevice &device);
        virtual ~QUECTEL_BC660KGL_CellularStack();

        virtual nsapi_error_t get_ip_address(SocketAddress *address) override;
        nsapi_error_t close_all_sockets();

    protected: // NetworkStack overrides
        virtual nsapi_error_t socket_listen(nsapi_socket_t handle, int backlog) override;
        virtual nsapi_error_t socket_accept(nsapi_socket_t server, nsapi_socket_t *handle,
                                            SocketAddress *address = 0) override;
        virtual nsapi_error_t socket_connect(nsapi_socket_t handle, const SocketAddress &address) override;

        virtual nsapi_size_or_error_t socket_sendto(nsapi_socket_t handle, const SocketAddress &addr,
                                                    const void *data, unsigned size) override;
        virtual nsapi_size_or_error_t socket_recvfrom(nsapi_socket_t handle, SocketAddress *address,
                                                      void *buffer, nsapi_size_t size) override;

#if defined(MBED_CONF_NSAPI_OFFLOAD_TLSSOCKET) && (MBED_CONF_NSAPI_OFFLOAD_TLSSOCKET)
        virtual nsapi_error_t setsockopt(nsapi_socket_t handle, int level, int optname,
                                         const void *optval, unsigned optlen) override;
#endif

    protected: // AT_CellularStack overrides
        virtual nsapi_error_t socket_close_impl(int sock_id) override;
        virtual nsapi_error_t create_socket_impl(CellularSocket *socket) override;
        virtual nsapi_size_or_error_t socket_sendto_impl(CellularSocket *socket, const SocketAddress &address,
                                                         const void *data, nsapi_size_t size) override;
        virtual nsapi_size_or_error_t socket_recvfrom_impl(CellularSocket *socket, SocketAddress *address,
                                                           void *buffer, nsapi_size_t size) override;

    private:


        // UDP/TCP socket open helpers
        nsapi_error_t open_udp_socket(CellularSocket *socket, int request_connect_id);
        void handle_open_socket_response(int &modem_connect_id, int &err);

        // Close any modem-side sockets reported open by AT+QISTATE.
        void reap_orphaned_modem_sockets();

        // URC data ingestion: decode hex payload and push it into SocketBuffer chunks.
        void receive_into_buffer(QUECTEL_BC660KGL_SocketBuffer *buffer, nsapi_size_t length,
                                 bool mark_boundary);

        QUECTEL_BC660KGL_SocketBuffer *get_or_create_socket_buffer(int id);

        // URC handlers
        void urc_qiurc(urc_qiurc_type_t urc_type);
        void urc_qiurc_recv();
        void urc_qiurc_closed();
        void urc_qiurc_incoming();
        void urc_qiopen();
        void urc_send_fail();

        int _last_modem_connect_id;
        nsapi_error_t _last_connect_error;

        EventFlags _event_flag;
        QUECTEL_BC660KGL_SocketBuffer **_socket_buffers;

        // Decode buffer for hex-mode URC data reception (max 1024 bytes per URC per docs).
        uint8_t _recv_buf[BC660KGL_RECV_BUF_SIZE];

#if defined(MBED_CONF_NSAPI_OFFLOAD_TLSSOCKET) && (MBED_CONF_NSAPI_OFFLOAD_TLSSOCKET)
        // SSL/TLS offload (AT+QSSL* command set).
        // BC660K-GL supports contextid=0, connectid=0 only (one SSL socket at a time).
        static const int SSL_CONTEXT_ID = 0;
        static const int SSL_CONNECT_ID = 0;

        nsapi_error_t ssl_socket_connect(CellularSocket *socket, const SocketAddress &address,
                                         int request_connect_id);
        nsapi_size_or_error_t ssl_socket_sendto_impl(CellularSocket *socket,
                                                     const void *data, nsapi_size_t size);
        nsapi_error_t ssl_socket_close_impl(int sock_id);
        nsapi_error_t ssl_upload_credential(const char *type, const void *data, unsigned len);

        void urc_qsslurc_recv();
        void urc_qsslurc_closed();
        void urc_qsslopen();
        void urc_qsslsend();

        uint8_t _tls_sec_level;    // 0=none, 1=server auth, 2=mutual auth
        int _ssl_socket_id;        // Framework socket index for active SSL connection (-1 = none)
        int _last_ssl_connect_err; // Result from +QSSLOPEN URC
#endif
    };

    /** @} */

} // namespace mbed

#endif // QUECTEL_BC660KGL_CELLULARSTACK_H_
