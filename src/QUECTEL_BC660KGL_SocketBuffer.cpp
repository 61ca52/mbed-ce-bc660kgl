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

#include "QUECTEL_BC660KGL_SocketBuffer.h"

#if MBED_SOCKET_BUFFER_TRACE
#include "CellularLog.h"
#endif

namespace mbed
{
#if MBED_SOCKET_BUFFER_TRACE
    int QUECTEL_BC660KGL_SocketBuffer::instance_count = 0;
#endif
    QUECTEL_BC660KGL_SocketBuffer::QUECTEL_BC660KGL_SocketBuffer() : _current(nullptr) {
#if MBED_SOCKET_BUFFER_TRACE
        instance_count++;
        tr_debug("SocketBuffer created, instances: %d", instance_count);
#endif
    }

    QUECTEL_BC660KGL_SocketBuffer::~QUECTEL_BC660KGL_SocketBuffer() {
#if MBED_SOCKET_BUFFER_TRACE
        tr_debug("Destroying SocketBuffer, instances before destruction: %d", instance_count);
#endif
        if (_current != nullptr) {
            _mpool.free(_current);
            _current = nullptr;
        }

        sb_chunk_t *chunk;
        while (_recv_data.try_get(&chunk)) {
            _mpool.free(chunk);
        }
#if MBED_SOCKET_BUFFER_TRACE
        instance_count--;
        tr_debug("SocketBuffer destroyed, remaining instances: %d", instance_count);
#endif
    }

    bool QUECTEL_BC660KGL_SocketBuffer::empty() {
        return _recv_data.empty() && _current == nullptr;
    }

    void QUECTEL_BC660KGL_SocketBuffer::flush() {
        if (_current != nullptr) {
            _mpool.free(_current);
            _current = nullptr;
        }

        sb_chunk_t *chunk;
        while (_recv_data.try_get(&chunk)) {
            _mpool.free(chunk);
        }
    }

    nsapi_size_t QUECTEL_BC660KGL_SocketBuffer::get_data(uint8_t *buffer, nsapi_size_t size) {
        nsapi_size_t remaining = size;
        uint8_t *dest = buffer;

        while (remaining > 0) {
            if (_current == nullptr && (_recv_data.empty() || !_recv_data.try_get(&_current))) {
                return size - remaining;
            }

            uint8_t *src = &(_current->data[_current->position]);
            while (_current->position < _current->size && remaining > 0) {
                *dest++ = *src++;
                _current->position++;
                remaining--;
            }

            if (_current->size <= _current->position) {
                _mpool.free(_current);
                _current = nullptr;
            }
        }

        return size - remaining;
    }

    nsapi_size_t QUECTEL_BC660KGL_SocketBuffer::get_datagram(uint8_t *buffer, nsapi_size_t size, SocketAddress *source) {
        nsapi_size_t written = 0;
        bool found_boundary = false;

        while (!found_boundary) {
            if (_current == nullptr && (_recv_data.empty() || !_recv_data.try_get(&_current))) {
                break;
            }

            while (_current->position < _current->size) {
                if (written < size) {
                    buffer[written] = _current->data[_current->position];
                    written++;
                }
                // Bytes beyond 'size' are silently discarded (UDP truncation)
                _current->position++;
            }

            found_boundary = _current->boundary;
            _mpool.free(_current);
            _current = nullptr;
        }

        if (source) {
            *source = _source_addr;
        }

        return written;
    }

    void QUECTEL_BC660KGL_SocketBuffer::set_source_address(const SocketAddress &addr) {
        _source_addr = addr;
    }

    void QUECTEL_BC660KGL_SocketBuffer::put_chunk(sb_chunk_t *chunk) {
        _recv_data.try_put(chunk);
    }

    sb_chunk_t *QUECTEL_BC660KGL_SocketBuffer::get_chunk() {
        sb_chunk_t *chunk = _mpool.try_alloc();
        if (chunk) {
            chunk->size = 0;
            chunk->position = 0;
            chunk->boundary = false;
        }
        return chunk;
    }
}
