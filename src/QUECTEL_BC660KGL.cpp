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

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "rtos/ThisThread.h"

#include "AT_CellularDevice.h"
#include "BufferedSerial.h"
#include "CellularLog.h"
#include "CellularNetwork.h"

#include "QUECTEL_BC660KGL.h"
#include "QUECTEL_BC660KGL_CellularContext.h"
#include "QUECTEL_BC660KGL_CellularInformation.h"
#include "QUECTEL_BC660KGL_CellularNetwork.h"

using namespace std::chrono_literals;
using namespace mbed;

static const uint16_t RETRY_TIMEOUT_SECONDS[] = {2, 4, 8, 16, 32};
static const intptr_t CELLULAR_PROPERTIES[AT_CellularDevice::PROPERTY_MAX] = {
    5,                                           // C_EREG changed from RegistrationModeLAC to 5
    AT_CellularNetwork::RegistrationModeDisable, // C_GREG
    AT_CellularNetwork::RegistrationModeDisable, // C_REG
    1,                                           // AT_CGSN_WITH_TYPE
    1,                                           // AT_CGDATA
    1,                                           // AT_CGAUTH,
    1,                                           // AT_CNMI
    1,                                           // AT_CSMP
    1,                                           // AT_CMGF
    1,                                           // AT_CSDH
    1,                                           // PROPERTY_IPV4_STACK
    1,                                           // PROPERTY_IPV6_STACK
    1,                                           // PROPERTY_IPV4V6_STACK
    0,                                           // PROPERTY_NON_IP_PDP_TYPE changed from 1 to 0
    0,                                           // PROPERTY_AT_CGEREP,
    0,                                           // PROPERTY_AT_COPS_FALLBACK_AUTO
    5,                                           // PROPERTY_SOCKET_COUNT - BC660K-GL supports socket IDs 0-4
    1,                                           // PROPERTY_IP_TCP
    1,                                           // PROPERTY_IP_UDP
    0,                                           // PROPERTY_AT_SEND_DELAY
};

#define OP_CPSMS_TAU         "11011111"
#define OP_CPSMS_ACTIVE_TIME "00000000"

// Inter-command pacing. 20 ms is fine in steady state; configure() bumps to
// 50 ms because back-to-back NVRAM writes (QCFG/QBAND/QCGDEFCONT) drop.
static const uint16_t OP_AT_SEND_DELAY_MS = 20;
static const uint16_t OP_AT_SEND_DELAY_MS_CONFIGURE = 50;

QUECTEL_BC660KGL::QUECTEL_BC660KGL(FileHandle *fh, bool active_high, PinName rst,
                                   PinName boot, PinName psm_exit, PinName vdd_ext_ind)
    : AT_CellularDevice(fh),
      _active_high(active_high),
      _rst(rst, !_active_high),
      _boot(boot, !_active_high),
      _psm_exit(psm_exit, !_active_high),
      _vdd_ext_ind(vdd_ext_ind),
      _network(nullptr)
#if MBED_CONF_QUECTEL_BC660KGL_CONFIGURE
      ,
      _configured(false)
#endif
{
    set_cellular_properties(CELLULAR_PROPERTIES);
#ifdef MBED_CONF_NSAPI_DEFAULT_CELLULAR_PLMN
    tr_debug("set_plmn: %s", MBED_CONF_NSAPI_DEFAULT_CELLULAR_PLMN);
    set_plmn(MBED_CONF_NSAPI_DEFAULT_CELLULAR_PLMN);
#endif
    set_retry_timeout_array(RETRY_TIMEOUT_SECONDS, sizeof(RETRY_TIMEOUT_SECONDS) / sizeof(RETRY_TIMEOUT_SECONDS[0]));
}

nsapi_error_t QUECTEL_BC660KGL::init() {
    setup_at_handler();

    _at.lock();
    _at.flush();
    _at.clear_error();

    // AT sync. No link to the modem -> nothing else can work.
    nsapi_error_t err = _at.at_cmd_discard("", "");
    if (err != NSAPI_ERROR_OK) {
        tr_error("AT sync failed (%d)", err);
        _at.unlock();
        return err;
    }

    _at.at_cmd_discard("E", "0");
    _at.at_cmd_discard("+CMEE", "=1");
    _at.at_cmd_discard("+CSCON", "=1");
    _at.clear_error();

    // Keep the modem awake during init
    err = set_sleep_mode(0);
    if (err != NSAPI_ERROR_OK) {
        _at.unlock();
        return err;
    }

#if MBED_CONF_QUECTEL_BC660KGL_CONFIGURE
    if (!_configured) {
        err = configure();
        if (err != NSAPI_ERROR_OK) {
            tr_error("configure() failed: %d", err);
            _at.unlock();
            return err;
        }
        _configured = true;
    }
#endif

    // CFUN=1: enable the radio
    for (int retry = 1; retry <= 3; retry++) {
        _at.flush();
        err = _at.at_cmd_discard("+CFUN", "=1");
        if (err == NSAPI_ERROR_OK) break;
        rtos::ThisThread::sleep_for(std::chrono::seconds(retry));
    }
    if (err != NSAPI_ERROR_OK) {
        _at.unlock();
        return err;
    }

    _at.at_cmd_discard("+CPSMS", "=1,,,", "%s%s", OP_CPSMS_TAU, OP_CPSMS_ACTIVE_TIME);
#ifndef NDEBUG
    dump_modem_state();
#endif
    _at.unlock();
    return NSAPI_ERROR_OK;
}

nsapi_error_t QUECTEL_BC660KGL::configure() {
    _at.set_send_delay(OP_AT_SEND_DELAY_MS_CONFIGURE);
    _at.flush();

    // CFUN=0: every NVRAM-altering command below requires minimum
    // functionality.
    nsapi_error_t err = _at.at_cmd_discard("+CFUN", "=0");
    if (err != NSAPI_ERROR_OK) {
        tr_error("CFUN=0 failed (%d)", err);
        goto exit;
    }
    ThisThread::sleep_for(20ms);
    
    // clearing the cached NB-IoT EARFCN list forces a full
    // scan on next attach
    _at.at_cmd_discard("+QCSEARFCN", "");
    _at.clear_error();

    err = qcfg_settings();
    if (err != NSAPI_ERROR_OK) goto exit;

    // CIPCA=3,0: attach WITH bundled PDN connectivity (manual section 5.5).
    // Without this the modem may attach with no active PDP context, so
    // sockets opened later fail with no obvious error!
    err = _at.at_cmd_discard("+CIPCA", "=", "%d,%d", 3, 0);
    if (err != NSAPI_ERROR_OK) goto exit;

#ifdef MBED_CONF_QUECTEL_BC660KGL_BANDS
    // Configure NB-IoT bands via AT+QBAND=<count>,<band>[,<band>,...]
    // (manual section 7.5). Wrong / missing bands = no RF coverage; this
    // is one of the most common silent attach failures.
    {
        uint32_t band_count = 0;
        const char *p = MBED_CONF_QUECTEL_BC660KGL_BANDS;
        while (*p) {
            strtoul(p, const_cast<char **>(&p), 10);
            band_count++;
            if (*p == ',') {
                p++;
            }
        }
        char band_cmd[64];
        snprintf(band_cmd, sizeof(band_cmd), "AT+QBAND=%lu,%s",
                 (unsigned long)band_count, MBED_CONF_QUECTEL_BC660KGL_BANDS);
        _at.clear_error();
        err = cmd_start(band_cmd);
        if (err != NSAPI_ERROR_OK) goto exit;
    }
#endif

    // QCGDEFCONT (CID 0). Wrong PDP type causes ESM #33 (single-address
    // bearer only) on attach, so an unapplied write blocks data.
    err = ensure_default_pdp_context();
    if (err != NSAPI_ERROR_OK) goto exit;

    // Apply all NVRAM changes with a single reboot.
    err = reboot_and_resync();

exit:
    _at.set_send_delay(OP_AT_SEND_DELAY_MS);
    _at.clear_error();
    return err;
}

nsapi_error_t QUECTEL_BC660KGL::reset() {
    tr_warn("Triggering reset");
    // Pulse length must be >= 50 ms (datasheet 2.4.1).
    if (!press_button(_rst, _active_high, 55ms)) {
        tr_error("RESET pin not wired");
        return NSAPI_ERROR_NO_CONNECTION;
    }

    _at.lock();
    _at.set_at_timeout(6s);
    _at.resp_start();
    _at.set_stop_tag("RDY");
    bool rdy = _at.consume_to_stop_tag();
    _at.set_stop_tag(OK);
    _at.restore_at_timeout();
    nsapi_error_t at_err = _at.unlock_return_error();

    if (!rdy) {
        tr_error("RDY not received in 6 s (at_err=%d)", at_err);
        return NSAPI_ERROR_DEVICE_ERROR;
    }
    return NSAPI_ERROR_OK;
}

nsapi_error_t QUECTEL_BC660KGL::reboot_and_resync() {
    tr_info("Rebooting modem via AT+QRST=1");
    _at.at_cmd_discard("+QRST", "=1");
    // Drop pending IO/error state from the reboot itself.
    _at.clear_error();

    // BC660K-GL needs ~9 s after QRST before it answers AT again.
    rtos::ThisThread::sleep_for(9s);

    if (!wake_up()) {
        tr_error("Modem did not wake up after reboot");
        return NSAPI_ERROR_DEVICE_ERROR;
    }

    // Restore the post-boot defaults we rely on.
    set_sleep_mode(0);
    _at.clear_error();
    _at.at_cmd_discard("E", "0");              // ATE0
    _at.at_cmd_discard("+CMEE", "=1"); // verbose errors
    _at.at_cmd_discard("+CSCON", "=1");
    return _at.get_last_error();
}

nsapi_error_t QUECTEL_BC660KGL::qcfg_settings() {

    // R13 vs R14 protocol release (manual 11.1.9). Telekom DE NB-IoT
    // requires R14 features; advertising R13 capability can lead to
    // EMM #19 / ESM failure on attach.
    nsapi_error_t err;
    if ((err = configure_qcfg_parameter("relversion", 14)) != NSAPI_ERROR_OK) return err;

    // NB1 vs NB2 (manual 11.1.10). Setting relversion=14 implicitly pushes
    // this to NB2 per the manual, but we set it explicitly so a stale
    // NVRAM value cannot cause problems.
    if ((err = configure_qcfg_parameter("NBcategory", 2)) != NSAPI_ERROR_OK) return err;

    // Extended Protocol Configuration Options (manual 11.1.1). Networks
    // that send the APN / PCO via the EPCO IE need this enabled or the
    // bundled PDN Connectivity Request may be silently dropped.
    if ((err = configure_qcfg_parameter("EPCO", 1)) != NSAPI_ERROR_OK) return err;

    // MAC-layer Release Assistance Indication (manual 11.1.8). Only
    // meaningful in R14; required for the bundled-PDN data path on
    // some operators.
    if ((err = configure_qcfg_parameter("MacRAI", 1)) != NSAPI_ERROR_OK) return err;

    configure_qcfg_parameter("slplocktimes", 1);     // manual 11.1.5
    configure_qcfg_parameter("OOSScheme", 1);        // manual 11.1.3
    configure_qcfg_parameter("DataInactTimer", 15);  // manual 11.1.2
    _at.clear_error();
    return NSAPI_ERROR_OK;
}

nsapi_error_t QUECTEL_BC660KGL::configure_qcfg_parameter(const char *cmd, int value) {
    return _at.at_cmd_discard("+QCFG", "=", "%s,%d", cmd, value);
}

nsapi_error_t QUECTEL_BC660KGL::ensure_default_pdp_context() {
    // Write QCGDEFCONT only when the current value differs.
    // APN is normally omitted so the bundled PDN Connectivity Request uses
    // the SIM's HSS-subscribed APN (avoids roaming ESM #33 rejects).
    // default-pdp-context-with-apn=true forces the APN to be written for
    // SIMs that don't advertise one or networks that need it explicit.
    // Must run after qcfg_settings(): relversion=14 can rewrite NVRAM.

#if defined(MBED_CONF_QUECTEL_BC660KGL_DEFAULT_PDP_TYPE)
    const char *desired_pdp_type = MBED_CONF_QUECTEL_BC660KGL_DEFAULT_PDP_TYPE;
#else
    const char *desired_pdp_type = nullptr;
#endif

    if (!desired_pdp_type) {
        return NSAPI_ERROR_OK;
    }

    const char *desired_apn = nullptr;
#if MBED_CONF_QUECTEL_BC660KGL_DEFAULT_PDP_CONTEXT_WITH_APN
#if defined(MBED_CONF_NSAPI_DEFAULT_CELLULAR_APN)
    desired_apn = MBED_CONF_NSAPI_DEFAULT_CELLULAR_APN;
    if (desired_apn && desired_apn[0] == '\0') {
        desired_apn = nullptr;
    }
#else
    tr_warn("default-pdp-context-with-apn set but no APN configured, omitting APN");
#endif
#endif

    if (default_pdp_context_matches(desired_pdp_type, desired_apn)) {
        return NSAPI_ERROR_OK;
    }

    nsapi_error_t err;
    if (desired_apn) {
        tr_warn("Provisioning QCGDEFCONT=\"%s\",\"%s\" (reboot required)",
                desired_pdp_type, desired_apn);
        err = _at.at_cmd_discard("+QCGDEFCONT", "=", "%s,%s",
                                 desired_pdp_type, desired_apn);
    } else {
        tr_warn("Provisioning QCGDEFCONT=\"%s\" (reboot required)", desired_pdp_type);
        err = _at.at_cmd_discard("+QCGDEFCONT", "=", "%s", desired_pdp_type);
    }
    if (err != NSAPI_ERROR_OK) {
        tr_error("QCGDEFCONT write failed: %d", err);
    }

    return err;
}

bool QUECTEL_BC660KGL::default_pdp_context_matches(const char *desired_pdp_type,
                                                   const char *desired_apn) {
    if (!desired_pdp_type) {
        return true; // nothing requested -> nothing to enforce
    }

    char current_pdp_type[10] = {0};
    char current_apn[MAX_ACCESSPOINT_NAME_LENGTH] = {0};

    _at.clear_error();
    _at.cmd_start_stop("+QCGDEFCONT", "?");
    _at.resp_start("+QCGDEFCONT:");

    // <pdp_type>[,<APN>[,<user_name>,<password>[,<auth_type>]]]
    int pdp_len = _at.read_string(current_pdp_type, sizeof(current_pdp_type));
    int apn_len = _at.read_string(current_apn, sizeof(current_apn));
    _at.resp_stop();

    nsapi_error_t err = _at.get_last_error();
    if (err != NSAPI_ERROR_OK || pdp_len <= 0) {
        _at.clear_error();
        return false;
    }

    bool type_ok = (strcasecmp(current_pdp_type, desired_pdp_type) == 0);
    bool current_apn_present = (apn_len > 0 && current_apn[0] != '\0');
    // Manual 5.6: APNs are stored uppercase, so compare case-insensitively.
    bool apn_ok = desired_apn
                      ? (current_apn_present && strcasecmp(current_apn, desired_apn) == 0)
                      : !current_apn_present;

    if (!type_ok || !apn_ok) {
        tr_info("QCGDEFCONT mismatch: have \"%s\"/\"%s\" want \"%s\"/\"%s\"",
                current_pdp_type, current_apn,
                desired_pdp_type, desired_apn ? desired_apn : "");
        return false;
    }

    return true;
}

nsapi_error_t QUECTEL_BC660KGL::hard_power_on() {
    tr_info("Modem hard_power_on");

    if (!wake_up()) {
        return NSAPI_ERROR_DEVICE_ERROR;
    }

    // Keep UART active when coming up, to avoid race with sleep mode.
    nsapi_error_t err = set_sleep_mode(0);
    if (err != NSAPI_ERROR_OK) {
        tr_warn("hard_power_on: unable to disable sleep mode");
        return err;
    }

    set_iohandler(true);
    return NSAPI_ERROR_OK;
}

nsapi_error_t QUECTEL_BC660KGL::hard_power_off() {
    tr_info("Modem hard_power_off");

    // Allow module to enter sleep after hard off request.
    nsapi_error_t err = set_sleep_mode(1);
    if (err != NSAPI_ERROR_OK) {
        tr_warn("hard_power_off: unable to enable sleep mode");
    }

    set_iohandler(false);
    return err;
}

void QUECTEL_BC660KGL::wait_power_down() {
    if (!_vdd_ext_ind.is_connected())
        return;

    // Wait for VDD_EXT_IND to go low (false)
    if (!wait_for_pin_state(_vdd_ext_ind, false, 20s)) {
        tr_warn("Timeout waiting for VDD_EXT_IND low");
    }
}

nsapi_error_t QUECTEL_BC660KGL::soft_power_on() {
    // This path is used when device was not ready and attempts retry from
    // STATE_DEVICE_READY. Ensure that modem is awake and sleep mode is blocked.
    if (!wake_up()) {
        return NSAPI_ERROR_DEVICE_ERROR;
    }

    nsapi_error_t err = set_sleep_mode(0);
    if (err != NSAPI_ERROR_OK) {
        tr_warn("soft_power_on: cannot disable sleep mode");
        return err;
    }

    set_iohandler(true);

#ifndef NDEBUG
    if (_vdd_ext_ind.is_connected()) {
        tr_info("VDD_EXT_IND = %d", _vdd_ext_ind.read());
    }
#endif

    return NSAPI_ERROR_OK;
}

nsapi_error_t QUECTEL_BC660KGL::soft_power_off() {
    nsapi_error_t err = NSAPI_ERROR_OK;
    CellularContext *curr = get_context_list();
    while (curr) {
        QUECTEL_BC660KGL_CellularContext *context = static_cast<QUECTEL_BC660KGL_CellularContext *>(curr);
        err = context->close_all_sockets();
        curr = curr->_next;
    }

#if MBED_CONF_QUECTEL_BC660KGL_RSSI
    // Grab a final signal quality sample before going to sleep
    if (_network != nullptr) {
        int rssi;
        _network->get_signal_quality(rssi);
#if MBED_CONF_QUECTEL_BC660KGL_GET_OP
        CellularNetwork::NWRegisteringMode mode;
        _network->get_network_registering_mode(mode);
#endif
    }
#endif

    err = set_sleep_mode(1); // Re-enable deep sleep

    if (err == NSAPI_ERROR_OK) {
        set_iohandler(false);
    }

    return err;
}

nsapi_error_t QUECTEL_BC660KGL::set_sleep_mode(int mode) {
    // Quectel_BC660K-GL_AT_Commands_Manual_V1.1-1.pdf P. 109:
    // 0: UART always enabled, disable sleep mode
    // 1: enable sleep mode
    // 2: light sleep mode (may require AT wake-up before every command)
    if (mode < 0 || mode > 2) {
        return NSAPI_ERROR_PARAMETER;
    }

    nsapi_error_t err = _at.at_cmd_discard("+QSCLK", "=", "%d", mode);

    if (err != NSAPI_ERROR_OK) {
        tr_warn("Failed to set QSCLK=%d", mode);
    }
    return err;
}

CellularNetwork *QUECTEL_BC660KGL::open_network() {
    // Cache the derived pointer; the base class _network member is private.
    _network = (QUECTEL_BC660KGL_CellularNetwork *)AT_CellularDevice::open_network();
    return _network;
}

void QUECTEL_BC660KGL::close_network() {
    _network = nullptr;
    AT_CellularDevice::close_network();
}

nsapi_error_t QUECTEL_BC660KGL::get_sim_state(SimState &state) {
#ifdef MBED_CONF_NSAPI_DEFAULT_CELLULAR_SIM_PIN
    return AT_CellularDevice::get_sim_state(state);
#else
    // No PIN: AT+QCCID is enough; it returns no data if the USIM is
    // absent or unreadable
    _at.lock();
    _at.flush();
    nsapi_error_t err = _at.at_cmd_discard("+QCCID", "");
    _at.unlock();

    state = SimStateReady;
    if (err != NSAPI_ERROR_OK) {
        tr_warn("SIM not readable.");
        state = SimStateUnknown;
    }
    return err;
#endif
}

nsapi_error_t QUECTEL_BC660KGL::get_net_time(char *date_buf, size_t date_size,
                                             char *time_buf, size_t time_size) {
    // "yy/MM/dd" needs >=9 bytes, "hh:mm:ss+zz" needs >=12 (incl. NUL).
    if (date_buf == nullptr || time_buf == nullptr || date_size < 9 || time_size < 12) {
        tr_error("get_net_time: bad buffer");
        return NSAPI_ERROR_PARAMETER;
    }

    date_buf[0] = '\0';
    time_buf[0] = '\0';

    _at.lock();
    _at.clear_error();
    _at.flush();
    _at.cmd_start_stop("+CCLK", "?");
    _at.resp_start("+CCLK:");

    // +CCLK?: "yy/MM/dd,hh:mm:ss+zz" (3GPP TS 27.007).
    char buf[32] = {0};
    _at.read_string(buf, sizeof(buf));
    _at.resp_stop();
    nsapi_error_t err = _at.unlock_return_error();

    if (err != NSAPI_ERROR_OK) {
        tr_error("AT+CCLK? failed: %d", err);
        return err;
    }

    char *comma = strchr(buf, ',');
    if (comma == nullptr) {
        tr_error("AT+CCLK? malformed: \"%s\"", buf);
        return NSAPI_ERROR_DEVICE_ERROR;
    }
    *comma = '\0';

    // strncpy may not NUL-terminate; force it.
    strncpy(date_buf, buf, date_size - 1);
    date_buf[date_size - 1] = '\0';
    strncpy(time_buf, comma + 1, time_size - 1);
    time_buf[time_size - 1] = '\0';

    return NSAPI_ERROR_OK;
}

bool QUECTEL_BC660KGL::is_attach() {
    if (!_network)
        return false;

    CellularNetwork::AttachStatus status;
    _network->get_attach(status);
    return (status == CellularNetwork::Attached);
}

void QUECTEL_BC660KGL::set_iohandler(bool enable) {
    FileHandle *handler = _at.get_file_handle();
    handler->enable_input(enable);
    handler->enable_output(enable);
}

#if MBED_CONF_QUECTEL_BC660KGL_RSSI
#if MBED_CONF_QUECTEL_BC660KGL_GET_OP
void QUECTEL_BC660KGL::get_info(int *rssis, int &size, CellularNetwork::operator_t &op)
#else
void QUECTEL_BC660KGL::get_info(int *rssis, int &size)
#endif
{
    if (!_network) {
        size = 0;
        return;
    }
#if MBED_CONF_QUECTEL_BC660KGL_GET_OP
    _network->get_info(rssis, size, op);
#else
    _network->get_info(rssis, size);
#endif
}
#endif

#if MBED_CONF_QUECTEL_BC660KGL_GET_ENGINEERING_INFO
nsapi_error_t QUECTEL_BC660KGL::get_engineering_info(char *psrp, char *rsrq, char *rssi, char *sinr) {
    _at.lock();
    _at.cmd_start_stop("+QENG", "=", "%d", 0);
    _at.resp_start("+QENG: ");
    int mode = _at.read_int();
    if (mode != 0) {
        _at.unlock();
        return NSAPI_ERROR_DEVICE_ERROR;
    }

    _at.skip_param(4);        // Skip EARFCN | EARFCN_offset | sc_pci |
    _at.read_string(psrp, 5); // RSRP value in dBm (signed)
    _at.read_string(rsrq, 5); // RSRQ value in dB (signed)
    _at.read_string(rssi, 5); // RSSI value in dBm (signed)
    _at.read_string(sinr, 5); // SINR value in dB (signed)
    _at.skip_param(5);        // Skip band | TAC | ECL | Tx_pwr | OP mode
    _at.resp_stop();

    return _at.unlock_return_error();
}
#endif

bool QUECTEL_BC660KGL::sync(std::chrono::duration<int, std::milli> timeout) {
    tr_debug("AT sync");
    nsapi_error_t err = NSAPI_ERROR_OK;

    for (int i = 0; i < 10; i++) {
        // For sync use an AT command that is supported by all modems and likely not used frequently,
        // especially a common response like OK could be response to previous request.
        _at.lock();
        _at.set_at_timeout(timeout);
        _at.cmd_start("AT+CGMI");
        _at.cmd_stop();
        _at.resp_start();
        _at.set_stop_tag("Quectel");
        _at.consume_to_stop_tag();
        _at.set_stop_tag(OK);
        _at.consume_to_stop_tag();
        err = _at.get_last_error();
        _at.restore_at_timeout();
        _at.unlock();

        if (err == NSAPI_ERROR_OK)
            return true;
    }

    tr_error("AT sync failed");
    return false;
}

bool QUECTEL_BC660KGL::wake_up() {
    set_iohandler(true);

    // Try to pulse PSM_EXIT to wake from deep/light sleep.
    if (!press_button(_psm_exit, _active_high, 50ms)) {
        tr_debug("PSM_EXIT pin not connected, skipping wake pulse");
    }

    // Sync to check that AT is really responsive and to clear garbage
    _at.at_cmd_discard("", ""); // Send AT to wake up if needed
    if (_at.sync(500))          // 300ms max response time + 50 ms tolerance
        return true;

    // Modem is not responding, try full reset
    tr_warn("Modem is not responding, attempting reset");
    if (reset() != NSAPI_ERROR_OK) {
        return false;
    }

    return _at.sync(500);
}

nsapi_error_t QUECTEL_BC660KGL::cmd_start(const char *cmd) {
    _at.cmd_start(cmd);
    _at.cmd_stop_read_resp();
    return _at.get_last_error();
}

bool QUECTEL_BC660KGL::press_button(DigitalOut &button, bool active_high,
                                    std::chrono::duration<uint32_t, std::milli> duration) {
    if (!button.is_connected()) {
        return false;
    }
    button = active_high;
    ThisThread::sleep_for(duration);
    button = !active_high;
    return true;
}

bool QUECTEL_BC660KGL::wait_for_pin_state(DigitalIn &pin, bool state,
                                          std::chrono::duration<uint32_t, std::milli> timeout) {
    if (!pin.is_connected()) {
        return false;
    }

    Timer timer;
    timer.start();

    while (timer.elapsed_time() < timeout) {
        if (pin.read() == state) {
            return true;
        }
        ThisThread::sleep_for(10ms);
    }

    return false;
}

nsapi_error_t QUECTEL_BC660KGL::set_baud_rate_impl(int baud_rate) {
    return NSAPI_ERROR_UNSUPPORTED;
}

AT_CellularNetwork *QUECTEL_BC660KGL::open_network_impl(ATHandler &at) {
    return new QUECTEL_BC660KGL_CellularNetwork(at, *this);
}

AT_CellularContext *QUECTEL_BC660KGL::create_context_impl(ATHandler &at, const char *apn, bool cp_req, bool nonip_req) {
#ifdef MBED_CONF_NSAPI_DEFAULT_CELLULAR_APN
    if (!apn || apn[0] == '\0') {
        apn = MBED_CONF_NSAPI_DEFAULT_CELLULAR_APN;
    }
#endif
    return new QUECTEL_BC660KGL_CellularContext(at, this, apn, cp_req, nonip_req);
}

AT_CellularInformation *QUECTEL_BC660KGL::open_information_impl(ATHandler &at) {
    return new QUECTEL_BC660KGL_CellularInformation(at, *this);
}

#ifndef NDEBUG
void QUECTEL_BC660KGL::dump_modem_state() {
    tr_info("==== BC660K-GL post-init state dump ====");
    _at.clear_error();

    // Poll +CPIN? until READY (up to ~3 s) so SIM-reading commands below
    // don't hit +CME ERROR 14 (SIM busy) right after CFUN=1.
    for (int i = 0; i < 30; i++) {
        char cpin[16] = {0};
        _at.clear_error();
        _at.cmd_start_stop("+CPIN", "?");
        _at.resp_start("+CPIN:");
        _at.read_string(cpin, sizeof(cpin));
        _at.resp_stop();
        if (_at.get_last_error() == NSAPI_ERROR_OK && strcmp(cpin, "READY") == 0) {
            break;
        }
        rtos::ThisThread::sleep_for(100ms);
    }
    _at.clear_error();

    auto dump = [&](const char *cmd, const char *suffix) {
        _at.clear_error();
        _at.at_cmd_discard(cmd, suffix);
    };

    // SIM
    dump("+CIMI", "");
    dump("+QCCID", "");

    // PDP
    dump("+QCGDEFCONT", "?");
    dump("+CGDCONT", "?");
    dump("+CIPCA", "?");
    dump("+CGATT", "?");

    // Modem capabilities in attach
    dump("+QCFG", "=\"EPCO\"");
    dump("+QCFG", "=\"relversion\"");
    dump("+QCFG", "=\"NBcategory\"");
    dump("+QCFG", "=\"MacRAI\"");
    dump("+QBAND", "?");  
    dump("+CCIOTOPT", "?"); // CIoT

    // Network state
    dump("+COPS", "?");
    dump("+CEREG", "?");

    // QESMC: reject cause from the last PDN Connectivity
    if (_network) {
        _network->query_and_log_esm_cause();
    } else {
        dump("+QESMC", "?");
    }

    _at.clear_error();
    tr_info("==== end state dump ====");
}
#endif

#if MBED_CONF_QUECTEL_BC660KGL_PROVIDE_DEFAULT
QUECTEL_BC660KGL *QUECTEL_BC660KGL::get_default_instance() {
    static BufferedSerial serial(MBED_CONF_QUECTEL_BC660KGL_TX, MBED_CONF_QUECTEL_BC660KGL_RX, MBED_CONF_QUECTEL_BC660KGL_BAUDRATE);
#if defined(MBED_CONF_QUECTEL_BC660KGL_RTS) && defined(MBED_CONF_QUECTEL_BC660KGL_CTS)
    tr_debug("QUECTEL_BC660KGL flow control: RTS %d CTS %d", MBED_CONF_QUECTEL_BC660KGL_RTS, MBED_CONF_QUECTEL_BC660KGL_CTS);
    serial.set_flow_control(SerialBase::RTSCTS, MBED_CONF_QUECTEL_BC660KGL_RTS, MBED_CONF_QUECTEL_BC660KGL_CTS);
#endif
    static QUECTEL_BC660KGL device(&serial,
                                   MBED_CONF_QUECTEL_BC660KGL_POLARITY,
                                   MBED_CONF_QUECTEL_BC660KGL_RESET,
                                   MBED_CONF_QUECTEL_BC660KGL_BOOT,
                                   MBED_CONF_QUECTEL_BC660KGL_PSM_EXIT,
                                   MBED_CONF_QUECTEL_BC660KGL_VDD_EXT_IND);
    return &device;
}

CellularContext *QUECTEL_BC660KGL::get_default_context() {
    QUECTEL_BC660KGL *dev = QUECTEL_BC660KGL::get_default_instance();
    if (!dev) {
        return nullptr;
    }

    static CellularContext *context = dev->create_context(nullptr, MBED_CONF_CELLULAR_CONTROL_PLANE_OPT);
    return context;
}

CellularContext *QUECTEL_BC660KGL::get_default_nonip_context() {
    QUECTEL_BC660KGL *dev = QUECTEL_BC660KGL::get_default_instance();
    if (!dev) {
        return nullptr;
    }

    static CellularContext *context = dev->create_context(nullptr, MBED_CONF_CELLULAR_CONTROL_PLANE_OPT, true);
    return context;
}

CellularDevice *CellularDevice::get_default_instance() {
    return QUECTEL_BC660KGL::get_default_instance();
}
#endif
