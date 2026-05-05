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

#ifndef QUECTEL_BC660KGL_SOCKETBUFFER_H_
#define QUECTEL_BC660KGL_SOCKETBUFFER_H_

#include <cstdint>

#include "mbed.h"
#include <Callback.h>
#include <MemoryPool.h>
#include <Queue.h>

#include "mbed_assert.h"
#include "netsocket/SocketAddress.h"
#include "AT_CellularStack.h"

#if MBED_CONF_MBED_TRACE_ENABLE
#define MBED_SOCKET_BUFFER_TRACE 1
#else
#define MBED_SOCKET_BUFFER_TRACE 0
#endif

namespace mbed
{

/**
 * @addtogroup at-hayes
 * @ingroup Cellular
 * @{
 */

    typedef struct sb_chunk
    {
        uint8_t size;
        uint8_t position;
        bool boundary; // true = last chunk of a UDP datagram
        uint8_t data[MBED_CONF_QUECTEL_BC660KGL_SOCKET_BUFFER_CHUNK_SIZE];
    } sb_chunk_t;

    // NOT thread-safe on its own. _current and _source_addr are plain members
    // shared between the URC writer (put_chunk/get_chunk/set_source_address)
    // and the recv reader (get_data/get_datagram/flush/empty). Serialization
    // relies on ATHandler::lock() in CellularStack so DO NOT call across
    // threads without that guarantee. Mutex here DEADLOCKS against at.lock().
    class QUECTEL_BC660KGL_SocketBuffer
    {
    public:
#if MBED_SOCKET_BUFFER_TRACE
        static int instance_count;
#endif
        QUECTEL_BC660KGL_SocketBuffer();
        ~QUECTEL_BC660KGL_SocketBuffer();

        bool empty();
        void flush();
        nsapi_size_t get_data(uint8_t *buffer, nsapi_size_t size);
        nsapi_size_t get_datagram(uint8_t *buffer, nsapi_size_t size, SocketAddress *source = nullptr);
        void put_chunk(sb_chunk_t *chunk);
        sb_chunk_t *get_chunk();
        void set_source_address(const SocketAddress &addr);

    private:
        sb_chunk_t *_current;
        Queue<sb_chunk, MBED_CONF_QUECTEL_BC660KGL_SOCKET_BUFFER_QUEUE_DEPTH> _recv_data;
        MemoryPool<sb_chunk, MBED_CONF_QUECTEL_BC660KGL_SOCKET_BUFFER_QUEUE_DEPTH> _mpool;
        SocketAddress _source_addr;
    };

/** @} */

} // namespace mbed

#endif // QUECTEL_BC660KGL_SOCKETBUFFER_H_
