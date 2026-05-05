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

#ifndef QUECTEL_BC660KGL_H_
#define QUECTEL_BC660KGL_H_

#include <chrono>

#include "mbed.h"
#include "AT_CellularDevice.h"
#include "QUECTEL_BC660KGL_CellularNetwork.h"

namespace mbed
{

/**
 * @addtogroup at-hayes
 * @ingroup Cellular
 * @{
 */

    class QUECTEL_BC660KGL : public AT_CellularDevice
    {
    public:
        QUECTEL_BC660KGL(FileHandle *fh, bool active_high, PinName rst,
                         PinName boot, PinName psm_exit, PinName vdd_ext_ind);

#if MBED_CONF_QUECTEL_BC660KGL_PROVIDE_DEFAULT
        static QUECTEL_BC660KGL *get_default_instance();
        static CellularContext *get_default_nonip_context();
        static CellularContext *get_default_context();
#endif

        // Initialization and configuration
        virtual nsapi_error_t init() override;
        virtual nsapi_error_t configure();
        /** Pulse RESET and wait for "RDY". Returns NSAPI_ERROR_NO_CONNECTION
         *  if the pin is unwired, NSAPI_ERROR_DEVICE_ERROR on RDY timeout. */
        nsapi_error_t reset();

        // Power management
        virtual nsapi_error_t hard_power_on() override;
        virtual nsapi_error_t hard_power_off() override;
        virtual void wait_power_down();
        virtual nsapi_error_t soft_power_on() override;
        virtual nsapi_error_t soft_power_off() override;
        nsapi_error_t set_sleep_mode(int mode);

        // Network interface overrides
        virtual CellularNetwork *open_network() override;
        virtual void close_network() override;

        // Device state queries
        virtual nsapi_error_t get_sim_state(SimState &state) override;
        /** Read network time via AT+CCLK?. Writes "yy/MM/dd" and
         *  "hh:mm:ss+zz". Buffers must be >=9 and >=12 bytes. */
        virtual nsapi_error_t get_net_time(char *date_buf, size_t date_size,
                                           char *time_buf, size_t time_size);
        virtual bool is_attach();
        virtual void set_iohandler(bool enable);

#if MBED_CONF_QUECTEL_BC660KGL_RSSI
#if MBED_CONF_QUECTEL_BC660KGL_GET_OP
        virtual void get_info(int *rssis, int &size, CellularNetwork::operator_t &op);
#else
        virtual void get_info(int *rssis, int &rs_size);
#endif
#endif

#if MBED_CONF_QUECTEL_BC660KGL_GET_ENGINEERING_INFO
        nsapi_error_t get_engineering_info(char *psrp, char *rsrq, char *rssi, char *sinr);
#endif

    protected:
        virtual nsapi_error_t set_baud_rate_impl(int baud_rate) override;
        virtual AT_CellularNetwork *open_network_impl(ATHandler &at) override;
        virtual AT_CellularContext *create_context_impl(ATHandler &at, const char *apn, bool cp_req = false, bool nonip_req = false) override;
        virtual AT_CellularInformation *open_information_impl(ATHandler &at) override;

    private:
        bool _active_high;
        DigitalOut _rst;
        DigitalOut _boot;
        DigitalOut _psm_exit;
        DigitalIn _vdd_ext_ind;

        QUECTEL_BC660KGL_CellularNetwork *_network;

#if MBED_CONF_QUECTEL_BC660KGL_CONFIGURE
        bool _configured;
#endif

        // AT synchronization and modem lifecycle
        bool sync(std::chrono::duration<int, std::milli> timeout);
        bool wake_up();
        nsapi_error_t cmd_start(const char *cmd);
        nsapi_error_t reboot_and_resync();

        // QCFG write helper. Caller decides whether to bail on the
        // returned error (attach-critical params) or ignore it (power/perf
        // tuning).
        nsapi_error_t configure_qcfg_parameter(const char *cmd, int value);
        nsapi_error_t qcfg_settings();

        // QCGDEFCONT (CID 0) helpers. Caller must hold the AT lock and have
        // the modem in CFUN=0. desired_apn == nullptr means "must have no
        // APN"; non-null requires a case-insensitive match (modem stores
        // APNs uppercased).
        bool default_pdp_context_matches(const char *desired_pdp_type, const char *desired_apn);
        nsapi_error_t ensure_default_pdp_context();

        // Hardware pin helpers
        bool press_button(DigitalOut &button, bool active_high,
                          std::chrono::duration<uint32_t, std::milli> duration);
        bool wait_for_pin_state(DigitalIn &pin, bool state,
                                std::chrono::duration<uint32_t, std::milli> timeout);

#ifndef NDEBUG
        /** Dump the current modem configuration for diagnostics. */
        void dump_modem_state();
#endif
    };

/** @} */

} // namespace mbed

#endif // QUECTEL_BC660KGL_H_
