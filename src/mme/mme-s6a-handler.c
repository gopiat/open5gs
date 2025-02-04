/*
 * Copyright (C) 2019 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "nas-path.h"
#include "s1ap-path.h"

#include "mme-sm.h"
#include "mme-s6a-handler.h"

/* Unfortunately fd doesn't distinguish
 * between result-code and experimental-result-code.
 *
 * However, e.g. 5004 has different meaning
 * if used in result-code than in experimental-result-code */
static uint8_t emm_cause_from_diameter(
                const uint32_t *dia_err, const uint32_t *dia_exp_err);

uint8_t mme_s6a_handle_aia(
        mme_ue_t *mme_ue, ogs_diam_s6a_message_t *s6a_message)
{
    ogs_diam_s6a_aia_message_t *aia_message = NULL;
    ogs_diam_e_utran_vector_t *e_utran_vector = NULL;

    ogs_assert(mme_ue);
    ogs_assert(s6a_message);
    aia_message = &s6a_message->aia_message;
    ogs_assert(aia_message);
    e_utran_vector = &aia_message->e_utran_vector;
    ogs_assert(e_utran_vector);

    if (s6a_message->result_code != ER_DIAMETER_SUCCESS) {
        ogs_warn("Authentication Information failed [%d]",
                    s6a_message->result_code);
        return emm_cause_from_diameter(s6a_message->err, s6a_message->exp_err);
    }

    mme_ue->xres_len = e_utran_vector->xres_len;
    memcpy(mme_ue->xres, e_utran_vector->xres, mme_ue->xres_len);
    memcpy(mme_ue->kasme, e_utran_vector->kasme, OGS_SHA256_DIGEST_SIZE);
    memcpy(mme_ue->rand, e_utran_vector->rand, OGS_RAND_LEN);
    memcpy(mme_ue->autn, e_utran_vector->autn, OGS_AUTN_LEN);

    CLEAR_MME_UE_TIMER(mme_ue->t3460);

    if (mme_ue->nas_eps.ksi == OGS_NAS_KSI_NO_KEY_IS_AVAILABLE)
        mme_ue->nas_eps.ksi = 0;

    ogs_assert(OGS_OK == nas_eps_send_authentication_request(mme_ue));

    return OGS_NAS_EMM_CAUSE_REQUEST_ACCEPTED;
}

uint8_t mme_s6a_handle_ula(
        mme_ue_t *mme_ue, ogs_diam_s6a_message_t *s6a_message)
{
    ogs_diam_s6a_ula_message_t *ula_message = NULL;
    ogs_subscription_data_t *subscription_data = NULL;
    ogs_slice_data_t *slice_data = NULL;
    int i, rv;

    ogs_assert(mme_ue);
    ogs_assert(s6a_message);
    ula_message = &s6a_message->ula_message;
    ogs_assert(ula_message);
    subscription_data = &ula_message->subscription_data;
    ogs_assert(subscription_data);

    if (s6a_message->result_code != ER_DIAMETER_SUCCESS) {
        ogs_error("Update Location failed [%d]", s6a_message->result_code);
        return emm_cause_from_diameter(s6a_message->err, s6a_message->exp_err);
    }

    ogs_assert(subscription_data->num_of_slice == 1);
    slice_data = &subscription_data->slice[0];

    memcpy(&mme_ue->ambr, &subscription_data->ambr, sizeof(ogs_bitrate_t));

    mme_session_remove_all(mme_ue);

    for (i = 0; i < slice_data->num_of_session; i++) {
        if (i >= OGS_MAX_NUM_OF_SESS) {
            ogs_warn("Ignore max session count overflow [%d>=%d]",
                    slice_data->num_of_session, OGS_MAX_NUM_OF_SESS);
            break;
        }

        mme_ue->session[i].name = ogs_strdup(slice_data->session[i].name);
        ogs_assert(mme_ue->session[i].name);

        mme_ue->session[i].context_identifier =
            slice_data->session[i].context_identifier;

        mme_ue->session[i].session_type = slice_data->session[i].session_type;
        memcpy(&mme_ue->session[i].paa, &slice_data->session[i].paa,
                sizeof(mme_ue->session[i].paa));

        memcpy(&mme_ue->session[i].qos, &slice_data->session[i].qos,
                sizeof(mme_ue->session[i].qos));
        memcpy(&mme_ue->session[i].ambr, &slice_data->session[i].ambr,
                sizeof(mme_ue->session[i].ambr));

        memcpy(&mme_ue->session[i].smf_ip, &slice_data->session[i].smf_ip,
                sizeof(mme_ue->session[i].smf_ip));

        memcpy(&mme_ue->session[i].charging_characteristics,
                &slice_data->session[i].charging_characteristics,
                sizeof(mme_ue->session[i].charging_characteristics));
        mme_ue->session[i].charging_characteristics_presence =
            slice_data->session[i].charging_characteristics_presence;
    }

    mme_ue->num_of_session = i;
    mme_ue->context_identifier = slice_data->context_identifier;

    if (mme_ue->nas_eps.type == MME_EPS_TYPE_ATTACH_REQUEST) {
        rv = nas_eps_send_emm_to_esm(mme_ue,
                &mme_ue->pdn_connectivity_request);
        if (rv != OGS_OK) {
            ogs_error("nas_eps_send_emm_to_esm() failed");
            return OGS_NAS_EMM_CAUSE_PROTOCOL_ERROR_UNSPECIFIED;
        }
    } else if (mme_ue->nas_eps.type == MME_EPS_TYPE_TAU_REQUEST) {
        ogs_assert(OGS_OK ==
            nas_eps_send_tau_accept(mme_ue,
                S1AP_ProcedureCode_id_InitialContextSetup));
    } else if (mme_ue->nas_eps.type == MME_EPS_TYPE_SERVICE_REQUEST) {
        ogs_error("[%s] Service request", mme_ue->imsi_bcd);
        return OGS_NAS_EMM_CAUSE_PROTOCOL_ERROR_UNSPECIFIED;
    } else if (mme_ue->nas_eps.type ==
            MME_EPS_TYPE_DETACH_REQUEST_FROM_UE) {
        ogs_error("[%s] Detach request", mme_ue->imsi_bcd);
        return OGS_NAS_EMM_CAUSE_PROTOCOL_ERROR_UNSPECIFIED;
    } else {
        ogs_fatal("Invalid Type[%d]", mme_ue->nas_eps.type);
        ogs_assert_if_reached();
    }

    return OGS_NAS_EMM_CAUSE_REQUEST_ACCEPTED;
}

void mme_s6a_handle_clr(
        mme_ue_t *mme_ue, ogs_diam_s6a_clr_message_t *clr_message)
{
    uint8_t detach_type = 0;

    ogs_assert(mme_ue);
    ogs_assert(clr_message);    

    /* Set NAS EPS Type */
    mme_ue->nas_eps.type = MME_EPS_TYPE_DETACH_REQUEST_TO_UE;
    ogs_debug("    OGS_NAS_EPS TYPE[%d]", mme_ue->nas_eps.type);

    if (clr_message->clr_flags & OGS_DIAM_S6A_CLR_FLAGS_REATTACH_REQUIRED)
        detach_type = OGS_NAS_DETACH_TYPE_TO_UE_RE_ATTACH_REQUIRED;
    else
        detach_type = OGS_NAS_DETACH_TYPE_TO_UE_RE_ATTACH_NOT_REQUIRED;
    
    if (OGS_FSM_CHECK(&mme_ue->sm, emm_state_de_registered)) {
        /* Remove all trace of subscriber even when detached. */
        mme_ue_hash_remove(mme_ue);
        mme_ue_remove(mme_ue);
    } else if (ECM_IDLE(mme_ue)) {
        MME_STORE_PAGING_INFO(mme_ue,
                MME_PAGING_TYPE_DETACH_TO_UE, (void *)(uintptr_t)detach_type);
        ogs_assert(OGS_OK == s1ap_send_paging(mme_ue, S1AP_CNDomain_ps));
    } else {
        ogs_assert(OGS_OK == nas_eps_send_detach_request(mme_ue, detach_type));
    }
}

/* 3GPP TS 29.272 Annex A; Table !.a:
 * Mapping from S6a error codes to NAS Cause Codes */
static uint8_t emm_cause_from_diameter(
                const uint32_t *dia_err, const uint32_t *dia_exp_err)
{
    if (dia_exp_err) {
        switch (*dia_exp_err) {
        case OGS_DIAM_S6A_ERROR_USER_UNKNOWN:                   /* 5001 */
            return OGS_NAS_EMM_CAUSE_PLMN_NOT_ALLOWED;
        case OGS_DIAM_S6A_ERROR_UNKNOWN_EPS_SUBSCRIPTION:       /* 5420 */
            /* FIXME: Error diagnostic? */
            return OGS_NAS_EMM_CAUSE_NO_SUITABLE_CELLS_IN_TRACKING_AREA;
        case OGS_DIAM_S6A_ERROR_RAT_NOT_ALLOWED:                /* 5421 */
            return OGS_NAS_EMM_CAUSE_ROAMING_NOT_ALLOWED_IN_THIS_TRACKING_AREA;
        case OGS_DIAM_S6A_ERROR_ROAMING_NOT_ALLOWED:            /* 5004 */
            return OGS_NAS_EMM_CAUSE_PLMN_NOT_ALLOWED;
            /* return OGS_NAS_EMM_CAUSE_EPS_SERVICES_NOT_ALLOWED_IN_THIS_PLMN;
             * (ODB_HPLMN_APN) */
            /* return OGS_NAS_EMM_CAUSE_ESM_FAILURE; (ODB_ALL_APN) */
        case OGS_DIAM_S6A_AUTHENTICATION_DATA_UNAVAILABLE:      /* 4181 */
            return OGS_NAS_EMM_CAUSE_NETWORK_FAILURE;
        }
    }
    if (dia_err) {
        switch (*dia_err) {
        case ER_DIAMETER_AUTHORIZATION_REJECTED:                /* 5003 */
        case ER_DIAMETER_UNABLE_TO_DELIVER:                     /* 3002 */
        case ER_DIAMETER_REALM_NOT_SERVED:                      /* 3003 */
            return OGS_NAS_EMM_CAUSE_NO_SUITABLE_CELLS_IN_TRACKING_AREA;
        case ER_DIAMETER_UNABLE_TO_COMPLY:                      /* 5012 */
        case ER_DIAMETER_INVALID_AVP_VALUE:                     /* 5004 */
        case ER_DIAMETER_AVP_UNSUPPORTED:                       /* 5001 */
        case ER_DIAMETER_MISSING_AVP:                           /* 5005 */
        case ER_DIAMETER_RESOURCES_EXCEEDED:                    /* 5006 */
        case ER_DIAMETER_AVP_OCCURS_TOO_MANY_TIMES:             /* 5009 */
            return OGS_NAS_EMM_CAUSE_NETWORK_FAILURE;
        }
    }

    ogs_error("Unexpected Diameter Result Code %d/%d, defaulting to severe "
              "network failure",
              dia_err ? *dia_err : -1, dia_exp_err ? *dia_exp_err : -1);
    return OGS_NAS_EMM_CAUSE_SEVERE_NETWORK_FAILURE;
}
