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

#include "QUECTEL_BC660KGL_CellularContext.h"
#include "QUECTEL_BC660KGL_CellularStack.h"

using namespace std::chrono_literals;

namespace mbed
{
    QUECTEL_BC660KGL_CellularContext::QUECTEL_BC660KGL_CellularContext(ATHandler &at, CellularDevice *device,
                                                                       const char *apn, bool cp_req, bool nonip_req)
        : AT_CellularContext(at, device, apn, cp_req, nonip_req) {
    }

    QUECTEL_BC660KGL_CellularContext::~QUECTEL_BC660KGL_CellularContext() {
    }

    nsapi_error_t QUECTEL_BC660KGL_CellularContext::get_ip_address(SocketAddress *address) {
        if (!address) {
            return NSAPI_ERROR_PARAMETER;
        }
#if NSAPI_PPP_AVAILABLE
        address->set_ip_address(nsapi_ppp_get_ip_addr(_at.get_file_handle()));
        return NSAPI_ERROR_OK;
#else
        if (!_stack) {
            _stack = get_stack();
        }
        if (_stack) {
            static_cast<AT_CellularStack *>(_stack)->set_cid(_cid);
            _stack->get_ip_address(address);
            return NSAPI_ERROR_OK;
        }
        return NSAPI_ERROR_NO_CONNECTION;
#endif
    }

    nsapi_error_t QUECTEL_BC660KGL_CellularContext::close_all_sockets() {
        if (!_stack) {
            return NSAPI_ERROR_OK;
        }
        return static_cast<QUECTEL_BC660KGL_CellularStack *>(_stack)->close_all_sockets();
    }

#if !NSAPI_PPP_AVAILABLE
    NetworkStack *QUECTEL_BC660KGL_CellularContext::get_stack() {
        if (_pdp_type == NON_IP_PDP_TYPE || _cp_in_use) {
            tr_error("Requesting stack for NON-IP context! Not supported.");
            return nullptr;
        }

        if (!_stack) {
            _stack = new QUECTEL_BC660KGL_CellularStack(_at, _cid, (nsapi_ip_stack_t)_pdp_type, *get_device());
        } else {
            static_cast<AT_CellularStack *>(_stack)->set_cid(_cid);
        }
        return _stack;
    }
#endif

    nsapi_error_t QUECTEL_BC660KGL_CellularContext::do_user_authentication() {
        // TCP/IP stack not used for Non-IP PDP, so skip QICSGP.
        if (_nonip_req && _cp_in_use) {
            return NSAPI_ERROR_OK;
        }

        if (!_pwd || !_uname) {
            return NSAPI_ERROR_OK;
        }

        if (!get_device()->get_property(AT_CellularDevice::PROPERTY_AT_CGAUTH)) {
            return NSAPI_ERROR_UNSUPPORTED;
        }

        const bool stored_debug_state = _at.get_debug();
        _at.set_debug(false);
        _at.at_cmd_discard("+CGAUTH", "=", "%d,%d,%s,%s", _cid, _authentication_type, _uname, _pwd);
        _at.set_debug(stored_debug_state);

        return (_at.get_last_error() == NSAPI_ERROR_OK) ? NSAPI_ERROR_OK : NSAPI_ERROR_AUTH_FAILURE;
    }

    bool QUECTEL_BC660KGL_CellularContext::get_context() {
        _at.cmd_start_stop("+CGDCONT", "?");
        _at.resp_start("+CGDCONT: ");

        _cid = -1;
        int matched_cid = -1;
        pdp_type_t matched_pdp_type = DEFAULT_PDP_TYPE;

        int cid_max = 0;
        char apn[MAX_ACCESSPOINT_NAME_LENGTH];
        int apn_len = 0;
        while (_at.info_resp()) {
            int cid = _at.read_int();
            if (cid > cid_max) {
                cid_max = cid;
            }
            char pdp_type_from_context[10];
            int pdp_type_len = _at.read_string(pdp_type_from_context, sizeof(pdp_type_from_context));
            if (pdp_type_len <= 0) {
                continue;
            }
            apn_len = _at.read_string(apn, sizeof(apn));
            if (apn_len < 0) {
                continue;
            }

            tr_info("CID %d APN \"%s\" type \"%s\"", cid, apn, pdp_type_from_context);

            if (_apn && (strcasecmp(apn, _apn) != 0)) {
                continue;
            }

            pdp_type_t pdp_type = string_to_pdp_type(pdp_type_from_context);

            // Accept exact match, or dual-stack for modems that support both v4/v6.
            if (get_device()->get_property(pdp_type_t_to_cellular_property(pdp_type)) ||
                ((pdp_type == IPV4V6_PDP_TYPE &&
                  get_device()->get_property(AT_CellularDevice::PROPERTY_IPV4_PDP_TYPE) &&
                  get_device()->get_property(AT_CellularDevice::PROPERTY_IPV6_PDP_TYPE)) &&
                 !_nonip_req)) {
                matched_pdp_type = pdp_type;
                matched_cid = cid;
            }
        }

        _at.resp_stop();

        if (_at.get_last_error() != NSAPI_ERROR_OK) {
            return false;
        }

#ifdef MBED_CONF_NSAPI_DEFAULT_CELLULAR_APN
        // When this context uses the configured default APN, always select CID 0
        // (the initial default bearer, auto-activated by the bundled EPS Attach).
        // Do NOT call set_new_context(0) here - that would write QCGDEFCONT to
        // NVRAM, potentially re-inserting an explicit APN that init() cleared.
        // QCGDEFCONT management belongs solely in init().
        if (_apn && strcasecmp(_apn, MBED_CONF_NSAPI_DEFAULT_CELLULAR_APN) == 0) {
            pdp_type_t pdp_type = IPV4_PDP_TYPE;
#ifdef MBED_CONF_QUECTEL_BC660KGL_DEFAULT_PDP_TYPE
            pdp_type = string_to_pdp_type(MBED_CONF_QUECTEL_BC660KGL_DEFAULT_PDP_TYPE);
#endif
            _pdp_type = pdp_type;
            update_cid(0);
            _new_context_set = true;
            tr_info("Using default PDP context CID 0 (default APN)");
            return true;
        }
#endif

        if (matched_cid != -1) {
            _pdp_type = matched_pdp_type;
            update_cid(matched_cid);
        } else {
            tr_info("No matching PDP context found");
            int new_cid = (cid_max == 0) ? 0 : cid_max + 1;
            if (!set_new_context(new_cid)) {
                return false;
            }
        }

        if (apn_len > 0 && !_apn) {
            memcpy(_found_apn, apn, apn_len + 1);
        }

        tr_info("Found PDP context %d", _cid);
        return true;
    }

    void QUECTEL_BC660KGL_CellularContext::set_plmn(const char *plmn) {
        AT_CellularContext::set_plmn(plmn);
        if (_device) {
            // Trigger re-registration so the state machine picks up the new PLMN
            // even if this context is already created.
            _device->register_to_network();
        }
    }

    const char *QUECTEL_BC660KGL_CellularContext::get_nonip_context_type_str() {
        return "NONIP";
    }

    void QUECTEL_BC660KGL_CellularContext::activate_context() {
        tr_info("Activate PDP context %d", _cid);
        _is_context_activated = true;
    }

    void QUECTEL_BC660KGL_CellularContext::deactivate_context() {
        // No explicit deactivation on BC660K-GL; keep bearer alive.
    }

    uint32_t QUECTEL_BC660KGL_CellularContext::get_timeout_for_operation(ContextOperation op) const
    {
        // Declare unconditionally so debug and release share the same vtable.
#ifdef NDEBUG
        (void)op;
        return std::chrono::duration<uint32_t, std::milli>(11000ms).count();
#else
        return AT_CellularContext::get_timeout_for_operation(op);
#endif
    }

    bool QUECTEL_BC660KGL_CellularContext::set_new_context(int cid) {
        char pdp_type_str[8 + 1] = {0};
        pdp_type_t pdp_type = IPV4_PDP_TYPE;
        tr_info("set_new_context %u", cid);

        if (_nonip_req && _cp_in_use && get_device()->get_property(AT_CellularDevice::PROPERTY_NON_IP_PDP_TYPE)) {
            strncpy(pdp_type_str, get_nonip_context_type_str(), sizeof(pdp_type_str));
            pdp_type = NON_IP_PDP_TYPE;
        }
#ifdef MBED_CONF_QUECTEL_BC660KGL_DEFAULT_PDP_TYPE
        else if (cid == 0) {
            // PDP type configurable - many NB-IoT networks only support IPv4.
            strncpy(pdp_type_str, MBED_CONF_QUECTEL_BC660KGL_DEFAULT_PDP_TYPE, sizeof(pdp_type_str));
            pdp_type = string_to_pdp_type(pdp_type_str);
        }
#endif
        else if (get_device()->get_property(AT_CellularDevice::PROPERTY_IPV4V6_PDP_TYPE) ||
                 (get_device()->get_property(AT_CellularDevice::PROPERTY_IPV4_PDP_TYPE) &&
                  get_device()->get_property(AT_CellularDevice::PROPERTY_IPV6_PDP_TYPE))) {
            strncpy(pdp_type_str, "IPV4V6", sizeof(pdp_type_str));
            pdp_type = IPV4V6_PDP_TYPE;
        } else if (get_device()->get_property(AT_CellularDevice::PROPERTY_IPV6_PDP_TYPE)) {
            strncpy(pdp_type_str, "IPV6", sizeof(pdp_type_str));
            pdp_type = IPV6_PDP_TYPE;
        } else if (get_device()->get_property(AT_CellularDevice::PROPERTY_IPV4_PDP_TYPE)) {
            strncpy(pdp_type_str, "IP", sizeof(pdp_type_str));
            pdp_type = IPV4_PDP_TYPE;
        } else {
            return false;
        }

        bool success;
        if (cid == 0) {
            // AT+QCGDEFCONT implicitly targets CID 0 (the initial default
            // bearer). Omitting the APN makes the bundled EPS Attach use the
            // SIM's HSS-subscribed APN (3GPP TS 24.301 6.5.1.3); a literal APN
            // can trigger ESM #33 on roaming. default-pdp-context-with-apn
            // overrides this when an explicit APN is required.
#if MBED_CONF_QUECTEL_BC660KGL_DEFAULT_PDP_CONTEXT_WITH_APN
            if (_apn && _apn[0] != '\0') {
                success = (_at.at_cmd_discard("+QCGDEFCONT", "=", "%s,%s",
                                              pdp_type_str, _apn) == NSAPI_ERROR_OK);
            } else
#endif
            {
                success = (_at.at_cmd_discard("+QCGDEFCONT", "=", "%s", pdp_type_str) == NSAPI_ERROR_OK);
            }
        } else if (_apn) {
            success = (_at.at_cmd_discard("+CGDCONT", "=", "%d,%s,%s", cid, pdp_type_str, _apn) == NSAPI_ERROR_OK);
        } else {
            success = (_at.at_cmd_discard("+CGDCONT", "=", "%d,%s", cid, pdp_type_str) == NSAPI_ERROR_OK);
        }

        if (success) {
            _pdp_type = pdp_type;
            update_cid(cid);
            _new_context_set = true;
            tr_info("New PDP context %d, type %d", _cid, pdp_type);
        }

        return success;
    }

    void QUECTEL_BC660KGL_CellularContext::update_cid(int cid) {
        _cid = cid;
        if (!_stack) {
            _stack = get_stack();
        }
        if (_stack) {
            static_cast<AT_CellularStack *>(_stack)->set_cid(_cid);
        }
    }

} /* namespace mbed */
