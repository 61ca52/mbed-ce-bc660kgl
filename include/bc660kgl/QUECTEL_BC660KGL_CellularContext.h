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

#ifndef QUECTEL_BC660KGL_CELLULARCONTEXT_H_
#define QUECTEL_BC660KGL_CELLULARCONTEXT_H_

#include "AT_CellularContext.h"

namespace mbed
{

/**
 * @addtogroup at-hayes
 * @ingroup Cellular
 * @{
 */

    class QUECTEL_BC660KGL_CellularContext : public AT_CellularContext
    {
    public:
        QUECTEL_BC660KGL_CellularContext(ATHandler &at, CellularDevice *device,
                                         const char *apn, bool cp_req = false, bool nonip_req = false);
        virtual ~QUECTEL_BC660KGL_CellularContext();

        virtual nsapi_error_t get_ip_address(SocketAddress *address) override;
        nsapi_error_t close_all_sockets();

    protected:
#if !NSAPI_PPP_AVAILABLE
        virtual NetworkStack *get_stack() override;
#endif
        virtual nsapi_error_t do_user_authentication() override;
        virtual bool get_context() override;
        virtual void set_plmn(const char *plmn) override;
        virtual const char *get_nonip_context_type_str() override;
        virtual void activate_context() override;
        virtual void deactivate_context() override;

        /** Release: 11 s for all ops. Debug: base defaults (trace printing
         *  blows the 11 s budget).
         */
        virtual uint32_t get_timeout_for_operation(ContextOperation op) const override;

    private:
        bool set_new_context(int cid);
        void update_cid(int cid);
    };

/** @} */

} // namespace mbed

#endif // QUECTEL_BC660KGL_CELLULARCONTEXT_H_
