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

#ifndef QUECTEL_BC660KGL_CELLULARNETWORK_H_
#define QUECTEL_BC660KGL_CELLULARNETWORK_H_

#include "AT_CellularNetwork.h"

namespace mbed
{

/**
 * @addtogroup at-hayes
 * @ingroup Cellular
 * @{
 */

    class QUECTEL_BC660KGL_CellularNetwork : public AT_CellularNetwork
    {
    public:
        QUECTEL_BC660KGL_CellularNetwork(ATHandler &atHandler, AT_CellularDevice &device);
        virtual ~QUECTEL_BC660KGL_CellularNetwork();

        // Registration / operator
        virtual nsapi_error_t set_registration(const char *plmn = 0) override;
        virtual nsapi_error_t get_network_registering_mode(NWRegisteringMode &mode) override;
        virtual nsapi_error_t get_operator_params(int &format, operator_t &operator_params) override;
        virtual nsapi_error_t get_operator_names(operator_names_list &op_names) override;
        virtual nsapi_error_t scan_plmn(operList_t &operators, int &ops_count) override;
        virtual nsapi_error_t get_registration_params(RegistrationType type, registration_params_t &reg_params) override;

        // CIoT / session state
        virtual nsapi_error_t set_ciot_optimization_config(CIoT_Supported_Opt supported_opt,
                                                           CIoT_Preferred_UE_Opt preferred_opt,
                                                           Callback<void(CIoT_Supported_Opt)> network_support_cb) override;
        virtual nsapi_error_t get_ciot_ue_optimization_config(CIoT_Supported_Opt &supported_opt,
                                                              CIoT_Preferred_UE_Opt &preferred_opt) override;
        virtual bool is_active_context(int *number_of_active_contexts = nullptr, int cid = -1) override;
        virtual nsapi_error_t get_signal_quality(int &rssi, int *ber = nullptr) override;

        /** Query AT+QESMC and log the latest ESM (Session Management) reject
         *  cause. The CEREG URC only carries the EMM wrapper (e.g. EMM #19
         *  "ESM failure"); the actual ESM reject cause sits in a separate
         *  Quectel-specific table (3GPP TS 24.301 Annex C).
         */
        nsapi_error_t query_and_log_esm_cause();

#if MBED_CONF_QUECTEL_BC660KGL_RSSI
#if MBED_CONF_QUECTEL_BC660KGL_GET_OP
        virtual void get_info(int *rssis, int &rs_size, operator_t &op);
#else
        virtual void get_info(int *rssis, int &rs_size);
#endif
#endif

    protected:
        virtual nsapi_error_t read_cops(int &format, int &mode, operator_t &op);
        virtual void get_context_state_command() override;
        virtual nsapi_error_t set_access_technology_impl(RadioAccessTechnology opRat) override;

    private:
        /** Replacement for AT_CellularNetwork::urc_cereg(). The base class
         *  discards <cause_type>/<reject_cause> (skip_param(2)), so EMM/ESM
         *  rejects from CEREG URCs are invisible. This override reads them,
         *  logs them on RegistrationDenied, and replicates the state-machine
         *  notification the base handler issued.
         */
        void urc_cereg_with_cause();
        void urc_cciotopti();

        // Shared EMM cause decoder used by urc_cereg_with_cause() and
        // get_registration_params(). 'prefix' is either "+CEREG URC denied"
        // or the AT command string for registration-denied context.
        void log_emm_cause(const char *prefix, int cause_type, int reject_cause);

#if MBED_CONF_QUECTEL_BC660KGL_RSSI
#if MBED_CONF_QUECTEL_BC660KGL_GET_OP
        operator_t _currentOp;
#endif
        CircularBuffer<int, 6> _rssis;
#endif
    };

/** @} */

} // namespace mbed

#endif // QUECTEL_BC660KGL_CELLULARNETWORK_H_
