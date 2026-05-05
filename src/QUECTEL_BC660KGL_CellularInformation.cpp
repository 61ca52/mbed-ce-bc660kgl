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

#include "QUECTEL_BC660KGL_CellularInformation.h"

namespace mbed
{
    QUECTEL_BC660KGL_CellularInformation::QUECTEL_BC660KGL_CellularInformation(ATHandler &at, AT_CellularDevice &device) : AT_CellularInformation(at, device)
    {
    }

    QUECTEL_BC660KGL_CellularInformation::~QUECTEL_BC660KGL_CellularInformation()
    {
    }

    nsapi_error_t QUECTEL_BC660KGL_CellularInformation::get_iccid(char *buf, size_t buf_size)
    {
        return get_info("AT+QCCID", buf, buf_size);
    }

    nsapi_error_t QUECTEL_BC660KGL_CellularInformation::get_revision(char *buf, size_t buf_size)
    {
        return get_info("AT+QGMR", buf, buf_size);
    }
} /* namespace mbed */