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

#include "CellularLog.h"
#include "CellularUtil.h"

#include "QUECTEL_BC660KGL_CellularNetwork.h"

using namespace mbed;
using namespace mbed_cellular_util;

namespace
{
    struct at_bc660_reg_t
    {
        const CellularNetwork::RegistrationType type;
        const char *const cmd;
        const char *const urc_prefix;
        const char *const urc_prefix_remove;
    };

    const at_bc660_reg_t at_bc660_reg[] = {
        {CellularNetwork::C_EREG, "+CEREG", "+CEREG: ", "+CEREG:"},
        {CellularNetwork::C_GREG, "+CGREG", "+CGREG: ", "+CGREG:"},
        {CellularNetwork::C_REG,  "+CREG",  "+CREG: ",  "+CREG:"}};
}

QUECTEL_BC660KGL_CellularNetwork::QUECTEL_BC660KGL_CellularNetwork(ATHandler &atHandler, AT_CellularDevice &device)
    : AT_CellularNetwork(atHandler, device) {
    _op_act = RAT_NB1;

    // Replace the base class URC handlers. For C_EREG we install our own
    // handler that reads <cause_type>/<reject_cause> instead of discarding them.
    for (int type = 0; type < CellularNetwork::C_MAX; type++) {
        if (_device.get_property((AT_CellularDevice::CellularProperty)type) != RegistrationModeDisable) {
            _at.set_urc_handler(at_bc660_reg[type].urc_prefix_remove, nullptr);
            if (type == CellularNetwork::C_EREG) {
                _at.set_urc_handler(at_bc660_reg[type].urc_prefix,
                                    callback(this, &QUECTEL_BC660KGL_CellularNetwork::urc_cereg_with_cause));
            } else {
                _at.set_urc_handler(at_bc660_reg[type].urc_prefix, _urc_funcs[type]);
            }
        }
    }

    _at.set_urc_handler("+CCIOTOPTI:", nullptr);
    _at.set_urc_handler("+CCIOTOPTI: ", callback(this, &QUECTEL_BC660KGL_CellularNetwork::urc_cciotopti));
}

QUECTEL_BC660KGL_CellularNetwork::~QUECTEL_BC660KGL_CellularNetwork() {
    for (int type = 0; type < CellularNetwork::C_MAX; type++) {
        if (_device.get_property((AT_CellularDevice::CellularProperty)type) != RegistrationModeDisable) {
            _at.set_urc_handler(at_bc660_reg[type].urc_prefix, nullptr);
        }
    }

    _at.set_urc_handler("+CCIOTOPTI: ", nullptr);
}

nsapi_error_t QUECTEL_BC660KGL_CellularNetwork::set_registration(const char *plmn) {
    if (!plmn) {
        tr_debug("Automatic network registration");
        NWRegisteringMode mode = NWModeAutomatic;
        if (get_network_registering_mode(mode) != NSAPI_ERROR_OK) {
            return NSAPI_ERROR_DEVICE_ERROR;
        }
        if (mode != NWModeAutomatic) {
            return _at.at_cmd_discard("+COPS", "=0");
        }
        return NSAPI_ERROR_OK;
    }

    tr_debug("Manual network registration to %s", plmn);
    NWRegisteringMode mode = NWModeManual;
    OperatorNameFormat format = OperatorNameNumeric;
#ifdef MBED_CONF_CELLULAR_PLMN_FALLBACK_AUTO
    if (_device.get_property(AT_CellularDevice::PROPERTY_AT_COPS_FALLBACK_AUTO)) {
        mode = NWModeManualAutomatic;
    }
#endif
    if (_op_act != RAT_UNKNOWN) {
        return _at.at_cmd_discard("+COPS", "=", "%d,%d,%s,%d", mode, format, plmn, _op_act);
    }
    return _at.at_cmd_discard("+COPS", "=", "%d,%d,%s", mode, format, plmn);
}

#if MBED_CONF_QUECTEL_BC660KGL_RSSI && MBED_CONF_QUECTEL_BC660KGL_GET_OP
nsapi_error_t QUECTEL_BC660KGL_CellularNetwork::get_network_registering_mode(NWRegisteringMode &mode) {
    int ret = -1;
    int format;
    nsapi_error_t error = read_cops(format, ret, _currentOp);
    if (ret != -1) {
        mode = (NWRegisteringMode)ret;
        return NSAPI_ERROR_OK;
    }
    return error;
}
#else
nsapi_error_t QUECTEL_BC660KGL_CellularNetwork::get_network_registering_mode(NWRegisteringMode &mode) {
    int ret;
    nsapi_error_t error = _at.at_cmd_int("+COPS", "?", ret);
    if (error == NSAPI_ERROR_OK) {
        mode = (NWRegisteringMode)ret;
    }
    return error;
}
#endif

nsapi_error_t QUECTEL_BC660KGL_CellularNetwork::get_operator_params(int &format, operator_t &operator_params) {
    int mode;
    return read_cops(format, mode, operator_params);
}

nsapi_error_t QUECTEL_BC660KGL_CellularNetwork::get_operator_names(operator_names_list &op_names) {
    return NSAPI_ERROR_UNSUPPORTED;
}

nsapi_error_t QUECTEL_BC660KGL_CellularNetwork::scan_plmn(operList_t &operators, int &opsCount) {
    int idx = 0;
    const int error_code = -1;

    _at.lock();
    _at.cmd_start_stop("+COPS", "=?");
    _at.resp_start("+COPS: ");

    while (_at.info_elem('(')) {
        operator_t *op = operators.add_new();
        op->op_status = (operator_t::Status)_at.read_int();
        _at.read_string(op->op_long, sizeof(op->op_long));
        _at.read_string(op->op_short, sizeof(op->op_short));
        _at.read_string(op->op_num, sizeof(op->op_num));

        int ret = _at.read_int();
        op->op_rat = (ret == error_code) ? RAT_UNKNOWN : (RadioAccessTechnology)ret;

        if ((_op_act == RAT_UNKNOWN) ||
            ((op->op_rat != RAT_UNKNOWN) && (op->op_rat == _op_act))) {
            idx++;
        } else {
            operators.delete_last();
        }
    }

    _at.resp_stop();
    opsCount = idx;
    return _at.unlock_return_error();
}

nsapi_error_t QUECTEL_BC660KGL_CellularNetwork::get_registration_params(RegistrationType type, registration_params_t &reg_params) {
    int i = (int)type;
    MBED_ASSERT(i >= 0 && i < C_MAX);

    if (!_device.get_property((AT_CellularDevice::CellularProperty)at_bc660_reg[i].type)) {
        return NSAPI_ERROR_UNSUPPORTED;
    }

    _at.lock();

    _at.cmd_start_stop(at_bc660_reg[i].cmd, "?");
    _at.resp_start(at_bc660_reg[i].urc_prefix);
    (void)_at.read_int(); // URC mode (unused)

    const int MAX_STRING_LENGTH = 9;
    char string_param[MAX_STRING_LENGTH] = {0};

    reg_params._type = type;

    int int_param = _at.read_int();
    reg_params._status = (int_param >= 0 && int_param < RegistrationStatusMax)
                            ? (RegistrationStatus)int_param : NotRegistered;

    int len = _at.read_string(string_param, TWO_BYTES_HEX + 1);
    reg_params._lac = (len > 0) ? hex_str_to_int(string_param, TWO_BYTES_HEX) : -1;

    len = _at.read_string(string_param, FOUR_BYTES_HEX + 1);
    reg_params._cell_id = (len > 0) ? hex_str_to_int(string_param, FOUR_BYTES_HEX) : -1;

    int_param = _at.read_int();
    reg_params._act = (int_param >= 0 && int_param < RAT_MAX)
                          ? (RadioAccessTechnology)int_param : RAT_UNKNOWN;

    // Read <cause_type>/<reject_cause> instead of skipping them (the base
    // class discards this diagnostic information).
    int cause_type = _at.read_int();
    int reject_cause = _at.read_int();
    if (reg_params._status == RegistrationDenied && reject_cause >= 0) {
        log_emm_cause(at_bc660_reg[i].cmd, cause_type, reject_cause);
    }

    // PSM: active time
    len = _at.read_string(string_param, ONE_BYTE_BINARY + 1);
    if (len != ONE_BYTE_BINARY) {
        reg_params._active_time = -1;
    } else {
        uint32_t ie_unit  = binary_str_to_uint(string_param, TIMER_UNIT_LENGTH);
        uint32_t ie_value = binary_str_to_uint(string_param + TIMER_UNIT_LENGTH, len - TIMER_UNIT_LENGTH);
        switch (ie_unit) {
        case 0: reg_params._active_time = 2 * ie_value; break;       // 2-second units
        case 1: reg_params._active_time = 60 * ie_value; break;      // 1-minute units
        case 2: reg_params._active_time = 6 * 60 * ie_value; break;  // decihour units
        case 7: reg_params._active_time = 0; break;                  // deactivated
        default: reg_params._active_time = 60 * ie_value; break;
        }
    }

    // PSM: periodic TAU
    len = _at.read_string(string_param, ONE_BYTE_BINARY + 1);
    if (len != ONE_BYTE_BINARY) {
        reg_params._periodic_tau = -1;
    } else {
        uint32_t ie_unit  = binary_str_to_uint(string_param, TIMER_UNIT_LENGTH);
        uint32_t ie_value = binary_str_to_uint(string_param + TIMER_UNIT_LENGTH, len - TIMER_UNIT_LENGTH);
        switch (ie_unit) {
        case 0: reg_params._periodic_tau = 60 * 10 * ie_value; break;     // 10-minute units
        case 1: reg_params._periodic_tau = 60 * 60 * ie_value; break;     // 1-hour units
        case 2: reg_params._periodic_tau = 10 * 60 * 60 * ie_value; break; // 10-hour units
        case 3: reg_params._periodic_tau = 2 * ie_value; break;           // 2-second units
        case 4: reg_params._periodic_tau = 30 * ie_value; break;          // 30-second units
        case 5: reg_params._periodic_tau = 60 * ie_value; break;          // 1-minute units
        case 6: reg_params._periodic_tau = 320 * 60 * 60 * ie_value; break; // 320-hour units
        default: reg_params._periodic_tau = 0; break;                     // deactivated
        }
    }

    _at.resp_stop();
    _reg_params = reg_params;

    return _at.unlock_return_error();
}

nsapi_error_t QUECTEL_BC660KGL_CellularNetwork::set_ciot_optimization_config(CIoT_Supported_Opt supported_opt,
                                                                             CIoT_Preferred_UE_Opt preferred_opt,
                                                                             Callback<void(CIoT_Supported_Opt)> network_support_cb) {
    _ciotopt_network_support_cb = network_support_cb;
    nsapi_error_t err = _at.at_cmd_discard("+CRTDCP", "=", "%d", 1);
    if (err == NSAPI_ERROR_OK) {
        err = _at.at_cmd_discard("+CCIOTOPT", "=1,", "%d,%d", supported_opt, preferred_opt);
    }
    return err;
}

nsapi_error_t QUECTEL_BC660KGL_CellularNetwork::get_ciot_ue_optimization_config(CIoT_Supported_Opt &supported_opt,
                                                                                CIoT_Preferred_UE_Opt &preferred_opt) {
    _at.lock();
    _at.cmd_start_stop("+CCIOTOPT", "?");
    _at.resp_start("+CCIOTOPT: ");
    _at.read_int();
    if (_at.get_last_error() == NSAPI_ERROR_OK) {
        supported_opt = (CIoT_Supported_Opt)_at.read_int();
        preferred_opt = (CIoT_Preferred_UE_Opt)_at.read_int();
    }
    _at.resp_stop();

    return _at.unlock_return_error();
}

bool QUECTEL_BC660KGL_CellularNetwork::is_active_context(int *number_of_active_contexts, int cid) {
    // Not supported on this modem type.
    if (number_of_active_contexts) {
        *number_of_active_contexts = 0;
    }
    return false;
}

nsapi_error_t QUECTEL_BC660KGL_CellularNetwork::get_signal_quality(int &rssi, int *ber) {
    _at.lock();
    _at.cmd_start_stop("+CSQ", "");
    _at.resp_start("+CSQ: ");
    int t_rssi = _at.read_int();
    int t_ber  = _at.read_int();
    _at.resp_stop();

#if MBED_CONF_QUECTEL_BC660KGL_RSSI
    _rssis.push(t_rssi);
#endif

    if (t_rssi < 0 || t_ber < 0) {
        _at.unlock();
        return NSAPI_ERROR_DEVICE_ERROR;
    }

    int ret = _at.unlock_return_error();

    // 3GPP TS 27.007: RSSI in dBm from -51 to -113.
    rssi = (t_rssi == 99) ? SignalQualityUnknown : (-113 + 2 * t_rssi);

    if (ber) {
        *ber = (t_ber == 99) ? SignalQualityUnknown : t_ber;
    }

    return ret;
}

// Manual section 4.9. Reply: +QESMC: <rejcausepresent>,<causetype>,<rejcausevalue>
nsapi_error_t QUECTEL_BC660KGL_CellularNetwork::query_and_log_esm_cause() {
    int present = -1;
    int causetype = -1;
    int rejvalue = -1;

    _at.lock();
    _at.cmd_start_stop("+QESMC", "?");
    _at.resp_start("+QESMC:");
    present   = _at.read_int();
    causetype = _at.read_int();
    rejvalue  = _at.read_int();
    _at.resp_stop();
    nsapi_error_t err = _at.unlock_return_error();

    if (err != NSAPI_ERROR_OK) {
        tr_warn("AT+QESMC? failed: %d", err);
        return err;
    }

    if (present != 1 || rejvalue < 0) {
        tr_info("QESMC: no ESM reject cause stored (present=%d)", present);
        return NSAPI_ERROR_OK;
    }

    const char *kind = (causetype == 0) ? "ESM (3GPP TS 24.301)" : "manufacturer-specific";
    tr_warn("QESMC: ESM reject cause = %d (%s)", rejvalue, kind);

    if (causetype == 0) {
        // Most relevant ESM causes for NB-IoT bring-up
        switch (rejvalue) {
        case 8:  tr_warn("  -> ESM #8  Operator Determined Barring"); break;
        case 26: tr_warn("  -> ESM #26 Insufficient resources"); break;
        case 27: tr_warn("  -> ESM #27 Missing or unknown APN"); break;
        case 28: tr_warn("  -> ESM #28 Unknown PDN type"); break;
        case 29: tr_warn("  -> ESM #29 User authentication failed"); break;
        case 30: tr_warn("  -> ESM #30 Request rejected by Serving GW or PDN GW"); break;
        case 31: tr_warn("  -> ESM #31 Request rejected, unspecified"); break;
        case 32: tr_warn("  -> ESM #32 Service option not supported"); break;
        case 33: tr_warn("  -> ESM #33 Requested service option not subscribed"); break;
        case 34: tr_warn("  -> ESM #34 Service option temporarily out of order"); break;
        case 35: tr_warn("  -> ESM #35 PTI already in use"); break;
        case 50: tr_warn("  -> ESM #50 PDN type IPv4 only allowed"); break;
        case 51: tr_warn("  -> ESM #51 PDN type IPv6 only allowed"); break;
        case 52: tr_warn("  -> ESM #52 single addr bearers only"); break;
        case 53: tr_warn("  -> ESM #53 ESM info not received"); break;
        case 54: tr_warn("  -> ESM #54 PDN connection does not exist"); break;
        case 55: tr_warn("  -> ESM #55 Multiple PDN connections for given APN not allowed"); break;
        case 66: tr_warn("  -> ESM #66 PDN type IPv4 only allowed (network)"); break;
        case 67: tr_warn("  -> ESM #67 PDN type IPv6 only allowed (network)"); break;
        default: break;
        }
    }

    return NSAPI_ERROR_OK;
}

nsapi_error_t QUECTEL_BC660KGL_CellularNetwork::read_cops(int &format, int &mode, operator_t &operator_params) {
    _at.lock();
    _at.cmd_start_stop("+COPS", "?");

    _at.resp_start("+COPS: ");
    mode = _at.read_int();
    format = _at.read_int();

    if (_at.get_last_error() == NSAPI_ERROR_OK) {
        switch (format) {
        case 0:
            _at.read_string(operator_params.op_long, sizeof(operator_params.op_long));
            break;
        case 1:
            _at.read_string(operator_params.op_short, sizeof(operator_params.op_short));
            break;
        default:
            _at.read_string(operator_params.op_num, sizeof(operator_params.op_num));
            break;
        }
        operator_params.op_rat = (RadioAccessTechnology)_at.read_int();
    }

    _at.resp_stop();
    return _at.unlock_return_error();
}

void QUECTEL_BC660KGL_CellularNetwork::get_context_state_command() {
    // Not supported on this modem type.
}

nsapi_error_t QUECTEL_BC660KGL_CellularNetwork::set_access_technology_impl(RadioAccessTechnology opRat) {
    if (opRat != RAT_NB1) {
        _op_act = RAT_NB1;
        return NSAPI_ERROR_UNSUPPORTED;
    }
    return NSAPI_ERROR_OK;
}

// +CEREG URC wire format:
//   +CEREG: <stat>[,<lac>,<ci>[,<AcT>[,<cause_type>,<reject_cause>[,<Active-Time>,<Periodic-TAU>]]]]
void QUECTEL_BC660KGL_CellularNetwork::urc_cereg_with_cause() {
    registration_params_t reg_params;
    reg_params._type = C_EREG;

    int status = _at.read_int();
    reg_params._status = (status >= 0 && status < RegistrationStatusMax)
                             ? (RegistrationStatus)status : NotRegistered;

    const int MAX_STRING_LENGTH = 9;
    char string_param[MAX_STRING_LENGTH] = {0};

    int len = _at.read_string(string_param, TWO_BYTES_HEX + 1);
    reg_params._lac = (len > 0) ? hex_str_to_int(string_param, TWO_BYTES_HEX) : -1;

    len = _at.read_string(string_param, FOUR_BYTES_HEX + 1);
    reg_params._cell_id = (len > 0) ? hex_str_to_int(string_param, FOUR_BYTES_HEX) : -1;

    int act = _at.read_int();
    reg_params._act = (act >= 0 && act < RAT_MAX) ? (RadioAccessTechnology)act : RAT_UNKNOWN;

    int cause_type = _at.read_int();
    int reject_cause = _at.read_int();
    _at.skip_param(2); // <Active-Time>, <Periodic-TAU>

    tr_debug("+CEREG URC: stat=%d lac=%d ci=%d act=%d cause_type=%d reject=%d",
             status, reg_params._lac, reg_params._cell_id, act, cause_type, reject_cause);

    if (reg_params._status == RegistrationDenied && reject_cause >= 0) {
        log_emm_cause("+CEREG URC", cause_type, reject_cause);
    }

    // Replicate AT_CellularNetwork::read_reg_params_and_compare() so the
    // cellular state machine still receives the update notification.
    if (_at.get_last_error() == NSAPI_ERROR_OK && _connection_status_cb) {
        cell_callback_data_t data;
        data.error = NSAPI_ERROR_OK;

        if (reg_params._act != _reg_params._act) {
            _reg_params._act = reg_params._act;
            data.status_data = reg_params._act;
            _connection_status_cb((nsapi_event_t)CellularRadioAccessTechnologyChanged, (intptr_t)&data);
        }

        if (reg_params._status != _reg_params._status || C_EREG != _reg_params._type) {
            RegistrationStatus prev = _reg_params._status;
            _reg_params._status = reg_params._status;
            data.status_data = reg_params._status;
            _connection_status_cb((nsapi_event_t)CellularRegistrationStatusChanged, (intptr_t)&data);
            if (reg_params._status == NotRegistered &&
                (prev == RegisteredHomeNetwork || prev == RegisteredRoaming)) {
                _connection_status_cb(NSAPI_EVENT_CONNECTION_STATUS_CHANGE, NSAPI_STATUS_DISCONNECTED);
            }
        }

        if (reg_params._cell_id != -1 && reg_params._cell_id != _reg_params._cell_id) {
            _reg_params._cell_id = reg_params._cell_id;
            _reg_params._lac = reg_params._lac;
            data.status_data = reg_params._cell_id;
            _connection_status_cb((nsapi_event_t)CellularCellIDChanged, (intptr_t)&data);
        }

        _reg_params._type = C_EREG;
    }
}

void QUECTEL_BC660KGL_CellularNetwork::urc_cciotopti() {
    _supported_network_opt = (CIoT_Supported_Opt)_at.read_int();
    if (_ciotopt_network_support_cb) {
        _ciotopt_network_support_cb(_supported_network_opt);
    }
}

void QUECTEL_BC660KGL_CellularNetwork::log_emm_cause(const char *prefix, int cause_type, int reject_cause) {
    const char *cause_kind = (cause_type == 0) ? "EMM" :
                             (cause_type == 1) ? "manual" : "unknown";
    tr_warn("%s denied: cause_type=%d (%s) reject_cause=%d",
            prefix, cause_type, cause_kind, reject_cause);

    if (cause_type != 0) {
        return;
    }

    // 3GPP TS 24.301 EMM cause codes most relevant for NB-IoT bring-up.
    switch (reject_cause) {
    case 7:  tr_warn("  -> EMM #7  EPS services not allowed"); break;
    case 8:  tr_warn("  -> EMM #8  EPS services and non-EPS services not allowed"); break;
    case 11: tr_warn("  -> EMM #11 PLMN not allowed"); break;
    case 12: tr_warn("  -> EMM #12 Tracking Area not allowed"); break;
    case 13: tr_warn("  -> EMM #13 Roaming not allowed in this TA"); break;
    case 14: tr_warn("  -> EMM #14 EPS services not allowed in this PLMN"); break;
    case 15: tr_warn("  -> EMM #15 No suitable cells in TA"); break;
    case 19: tr_warn("  -> EMM #19 ESM failure (bundled PDN Connectivity Request rejected)"); break;
    case 22: tr_warn("  -> EMM #22 Congestion"); break;
    case 25: tr_warn("  -> EMM #25 Not authorized for this CSG"); break;
    case 35: tr_warn("  -> EMM #35 Service option not authorized in this PLMN"); break;
    default: break;
    }
}

#if MBED_CONF_QUECTEL_BC660KGL_RSSI
#if MBED_CONF_QUECTEL_BC660KGL_GET_OP
void QUECTEL_BC660KGL_CellularNetwork::get_info(int *rssis, int &rs_size, operator_t &op)
#else
void QUECTEL_BC660KGL_CellularNetwork::get_info(int *rssis, int &rs_size)
#endif
{
    rs_size = 0;
    int rssi = -99;
    while (rs_size < 6 && !_rssis.empty()) {
        _rssis.pop(rssi);
        rssis[rs_size++] = rssi;
    }
#if MBED_CONF_QUECTEL_BC660KGL_GET_OP
    op.op_status = _currentOp.op_status;
    strncpy(op.op_long,  _currentOp.op_long,  MAX_OPERATOR_NAME_LONG + 1);
    strncpy(op.op_short, _currentOp.op_short, MAX_OPERATOR_NAME_SHORT + 1);
    strncpy(op.op_num,   _currentOp.op_num,   MAX_OPERATOR_NAME_SHORT + 1);
    op.op_rat = _currentOp.op_rat;
#endif
}
#endif
