/******************************************************************************
 *
 *  Copyright (C) 2009-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/


/*****************************************************************************
 *
 *  Filename:      btif_av.c
 *
 *  Description:   Bluedroid AV implementation
 *
 *****************************************************************************/

#include <hardware/bluetooth.h>
#include "hardware/bt_av.h"

#define LOG_TAG "BTIF_AV"

#include "btif_av.h"
#include "btif_util.h"
#include "btif_profile_queue.h"
#include "bta_api.h"
#include "btif_media.h"
#include "btif_storage.h"
#include "bta_av_api.h"
#include "gki.h"
#include "bd.h"
#include "btu.h"

/*****************************************************************************
**  Constants & Macros
******************************************************************************/
#define BTIF_AV_SERVICE_NAME "Advanced Audio"

#define BTIF_TIMEOUT_AV_OPEN_ON_RC_SECS  2

typedef enum {
    BTIF_AV_STATE_IDLE = 0x0,
    BTIF_AV_STATE_OPENING,
    BTIF_AV_STATE_OPENED,
    BTIF_AV_STATE_STARTED,
    BTIF_AV_STATE_CLOSING
} btif_av_state_t;

/* Should not need dedicated suspend state as actual actions are no
   different than open state. Suspend flags are needed however to prevent
   media task from trying to restart stream during remote suspend or while
   we are in the process of a local suspend */

#define BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING 0x1
#define BTIF_AV_FLAG_REMOTE_SUSPEND        0x2
#define BTIF_AV_FLAG_PENDING_START         0x4
#define BTIF_AV_FLAG_PENDING_STOP          0x8

/*****************************************************************************
**  Local type definitions
******************************************************************************/
typedef enum
{
    SEP_SRC = 0x0,
    SEP_SNK,
    SEP_NOT_OPENED
}tbtif_AV_SEP_TYPE;

typedef struct
{
    tBTA_AV_HNDL bta_handle;
    bt_bdaddr_t peer_bda;
    btif_sm_handle_t sm_handle;
    UINT8 flags;
    tBTA_AV_EDR edr;
    UINT8 edr_3mbps;
    tbtif_AV_SEP_TYPE   sep;  /* sep type of peer device */
} btif_av_cb_t;

/*****************************************************************************
**  Static variables
******************************************************************************/
static btav_callbacks_t *bt_av_callbacks = NULL;
static btif_av_cb_t btif_av_cb;
static TIMER_LIST_ENT tle_av_open_on_rc;
static btif_sm_event_t idle_rc_event;
static tBTA_AV idle_rc_data;


/* both interface and media task needs to be ready to alloc incoming request */
#define CHECK_BTAV_INIT() if ((bt_av_callbacks == NULL) || (btif_av_cb.sm_handle == NULL))\
{\
     BTIF_TRACE_WARNING1("%s: BTAV not initialized", __FUNCTION__);\
     return BT_STATUS_NOT_READY;\
}\
else\
{\
     BTIF_TRACE_EVENT1("%s", __FUNCTION__);\
}

/* Helper macro to avoid code duplication in the state machine handlers */
#define CHECK_RC_EVENT(e, d) \
    case BTA_AV_RC_OPEN_EVT: \
    case BTA_AV_RC_CLOSE_EVT: \
    case BTA_AV_REMOTE_CMD_EVT: \
    case BTA_AV_VENDOR_CMD_EVT: \
    case BTA_AV_META_MSG_EVT: \
    case BTA_AV_BROWSE_MSG_EVT: \
    case BTA_AV_RC_FEAT_EVT: \
    case BTA_AV_REMOTE_RSP_EVT: \
    { \
         btif_rc_handler(e, d);\
    }break; \

static BOOLEAN btif_av_state_idle_handler(btif_sm_event_t event, void *data);
static BOOLEAN btif_av_state_opening_handler(btif_sm_event_t event, void *data);
static BOOLEAN btif_av_state_opened_handler(btif_sm_event_t event, void *data);
static BOOLEAN btif_av_state_started_handler(btif_sm_event_t event, void *data);
static BOOLEAN btif_av_state_closing_handler(btif_sm_event_t event, void *data);

static const btif_sm_handler_t btif_av_state_handlers[] =
{
    btif_av_state_idle_handler,
    btif_av_state_opening_handler,
    btif_av_state_opened_handler,
    btif_av_state_started_handler,
    btif_av_state_closing_handler
};

/*************************************************************************
** Extern functions
*************************************************************************/
extern void btif_rc_handler(tBTA_AV_EVT event, tBTA_AV *p_data);
extern BOOLEAN btif_rc_get_connected_peer(BD_ADDR peer_addr);
extern void btif_rc_check_handle_pending_play (BD_ADDR peer_addr, BOOLEAN bSendToApp);
extern BOOLEAN btif_hf_is_call_idle();
extern BOOLEAN btif_multihf_is_call_idle();

/*****************************************************************************
** Local helper functions
******************************************************************************/

const char *dump_av_sm_state_name(btif_av_state_t state)
{
    switch (state)
    {
        CASE_RETURN_STR(BTIF_AV_STATE_IDLE)
        CASE_RETURN_STR(BTIF_AV_STATE_OPENING)
        CASE_RETURN_STR(BTIF_AV_STATE_OPENED)
        CASE_RETURN_STR(BTIF_AV_STATE_STARTED)
        CASE_RETURN_STR(BTIF_AV_STATE_CLOSING)
        default: return "UNKNOWN_STATE";
    }
}

const char *dump_av_sm_event_name(btif_av_sm_event_t event)
{
    switch((int)event)
    {
        CASE_RETURN_STR(BTA_AV_ENABLE_EVT)
        CASE_RETURN_STR(BTA_AV_REGISTER_EVT)
        CASE_RETURN_STR(BTA_AV_OPEN_EVT)
        CASE_RETURN_STR(BTA_AV_CLOSE_EVT)
        CASE_RETURN_STR(BTA_AV_START_EVT)
        CASE_RETURN_STR(BTA_AV_STOP_EVT)
        CASE_RETURN_STR(BTA_AV_PROTECT_REQ_EVT)
        CASE_RETURN_STR(BTA_AV_PROTECT_RSP_EVT)
        CASE_RETURN_STR(BTA_AV_RC_OPEN_EVT)
        CASE_RETURN_STR(BTA_AV_RC_CLOSE_EVT)
        CASE_RETURN_STR(BTA_AV_REMOTE_CMD_EVT)
        CASE_RETURN_STR(BTA_AV_REMOTE_RSP_EVT)
        CASE_RETURN_STR(BTA_AV_VENDOR_CMD_EVT)
        CASE_RETURN_STR(BTA_AV_VENDOR_RSP_EVT)
        CASE_RETURN_STR(BTA_AV_RECONFIG_EVT)
        CASE_RETURN_STR(BTA_AV_SUSPEND_EVT)
        CASE_RETURN_STR(BTA_AV_PENDING_EVT)
        CASE_RETURN_STR(BTA_AV_META_MSG_EVT)
        CASE_RETURN_STR(BTA_AV_REJECT_EVT)
        CASE_RETURN_STR(BTA_AV_RC_FEAT_EVT)
        CASE_RETURN_STR(BTIF_SM_ENTER_EVT)
        CASE_RETURN_STR(BTIF_SM_EXIT_EVT)
        CASE_RETURN_STR(BTIF_AV_CONNECT_REQ_EVT)
        CASE_RETURN_STR(BTIF_AV_DISCONNECT_REQ_EVT)
        CASE_RETURN_STR(BTIF_AV_START_STREAM_REQ_EVT)
        CASE_RETURN_STR(BTIF_AV_STOP_STREAM_REQ_EVT)
        CASE_RETURN_STR(BTIF_AV_SUSPEND_STREAM_REQ_EVT)
        CASE_RETURN_STR(BTIF_AV_RECONFIGURE_REQ_EVT)
        CASE_RETURN_STR(BTIF_AV_REQUEST_AUDIO_FOCUS_EVT)

        default: return "UNKNOWN_EVENT";
   }
}

/*******************************************************************************
**
** Function         btif_av_request_audio_focus
**
** Description      send request to gain audio focus
**
** Returns          void
**
*******************************************************************************/
void btif_av_request_audio_focus( BOOLEAN enable)
{
    btif_sm_state_t state;
    state= btif_sm_get_state(btif_av_cb.sm_handle);
    /* We shld be in started state */
    if (state != BTIF_AV_STATE_STARTED)
        return;
    /* If we are in started state, suspend shld not have been initiated */
    if ((btif_av_cb.flags & BTIF_AV_FLAG_REMOTE_SUSPEND )||
        (btif_av_cb.flags & BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING))
        return;
    if(enable)
    {
         btif_dispatch_sm_event(BTIF_AV_REQUEST_AUDIO_FOCUS_EVT, NULL, 0);
    }
}

/****************************************************************************
**  Local helper functions
*****************************************************************************/
/*******************************************************************************
**
** Function         btif_initiate_av_open_tmr_hdlr
**
** Description      Timer to trigger AV open if the remote headset establishes
**                  RC connection w/o AV connection. The timer is needed to IOP
**                  with headsets that do establish AV after RC connection.
**
** Returns          void
**
*******************************************************************************/
static void btif_initiate_av_open_tmr_hdlr(TIMER_LIST_ENT *tle)
{
    BD_ADDR peer_addr;

    /* is there at least one RC connection - There should be */
    if (btif_rc_get_connected_peer(peer_addr)) {
       BTIF_TRACE_DEBUG1("%s Issuing connect to the remote RC peer", __FUNCTION__);
       btif_sm_dispatch(btif_av_cb.sm_handle, BTIF_AV_CONNECT_REQ_EVT, (void*)&peer_addr);
    }
    else
    {
        BTIF_TRACE_ERROR1("%s No connected RC peers", __FUNCTION__);
    }
}

/*****************************************************************************
**  Static functions
******************************************************************************/

/*****************************************************************************
**
** Function     btif_av_state_idle_handler
**
** Description  State managing disconnected AV link
**
** Returns      TRUE if event was processed, FALSE otherwise
**
*******************************************************************************/

static BOOLEAN btif_av_state_idle_handler(btif_sm_event_t event, void *p_data)
{
    tBTA_AV *p_bta_data;

    BTIF_TRACE_DEBUG3("%s event:%s flags %x", __FUNCTION__,
                     dump_av_sm_event_name(event), btif_av_cb.flags);

    switch (event)
    {
        case BTIF_SM_ENTER_EVT:
            /* clear the peer_bda */
            memset(&btif_av_cb.peer_bda, 0, sizeof(bt_bdaddr_t));
            btif_av_cb.flags = 0;
            btif_av_cb.edr = 0;
            btif_av_cb.edr_3mbps = 0;
            btif_av_cb.sep = SEP_NOT_OPENED;
            btif_a2dp_on_idle();
            break;

        case BTIF_SM_EXIT_EVT:
            break;

        case BTA_AV_ENABLE_EVT:
            break;

        case BTA_AV_REGISTER_EVT:
            btif_av_cb.bta_handle = ((tBTA_AV*)p_data)->registr.hndl;
            btif_queue_pending_retry();
            break;

        case BTIF_AV_CONNECT_REQ_EVT:
             /* For outgoing connect stack and app are in sync.
              */
             memcpy(&btif_av_cb.peer_bda, (bt_bdaddr_t*)p_data, sizeof(bt_bdaddr_t));
             BTA_AvOpen(btif_av_cb.peer_bda.address, btif_av_cb.bta_handle,
                    TRUE, BTA_SEC_NONE);
             btif_sm_change_state(btif_av_cb.sm_handle, BTIF_AV_STATE_OPENING);
             break;

        case BTA_AV_OPEN_EVT:
             /* We get this event in Idle State if Signalling channel was not closed
              * only Streaming channel was closed, and stream setup process was initiated
              */
             p_bta_data = (tBTA_AV*)p_data;
             BTIF_TRACE_DEBUG2("status:%d, edr 0x%x", p_bta_data->open.status,
                               p_bta_data->open.edr);

             if (p_bta_data->open.status == BTA_AV_SUCCESS)
             {
                  btif_av_cb.edr = p_bta_data->open.edr;
                  if (p_bta_data->open.edr & BTA_AV_EDR_3MBPS)
                  {
                      BTIF_TRACE_DEBUG0("remote supports 3 mbps");
                      btif_av_cb.edr_3mbps = TRUE;
                  }

                  if (p_bta_data->open.sep == AVDT_TSEP_SRC)
                     btif_av_cb.sep = SEP_SRC;
                  else if (p_bta_data->open.sep == AVDT_TSEP_SNK)
                      btif_av_cb.sep = SEP_SNK;

                  bdcpy(btif_av_cb.peer_bda.address, ((tBTA_AV*)p_data)->open.bd_addr);

                  /* inform the application of the event */
                  HAL_CBACK(bt_av_callbacks, connection_state_cb,
                        BTAV_CONNECTION_STATE_CONNECTED, &(btif_av_cb.peer_bda));
                  /* change state to open/idle based on the status */
                  btif_sm_change_state(btif_av_cb.sm_handle, BTIF_AV_STATE_OPENED);
             }
             break;

        case BTA_AV_PENDING_EVT:
        case BTA_AV_RC_OPEN_EVT:
            /* IOP_FIX: Jabra 620 only does RC open without AV open whenever it connects. So
             * as per the AV WP, an AVRC connection cannot exist without an AV connection. Therefore,
             * we initiate an AV connection if an RC_OPEN_EVT is received when we are in AV_CLOSED state.
             * We initiate the AV connection after a small 3s timeout to avoid any collisions from the
             * headsets, as some headsets initiate the AVRC connection first and then
             * immediately initiate the AV connection
             *
             * TODO: We may need to do this only on an AVRCP Play. FixMe
             */

            /* Check if connection allowed with this device */
            BTIF_TRACE_DEBUG0("Check A2dp priority of device");
            if (idle_rc_event != 0)
            {
                BTIF_TRACE_DEBUG0("Processing another RC Event ");
                return FALSE;
            }
            idle_rc_event = event;
            memcpy(&idle_rc_data, ((tBTA_AV*)p_data), sizeof(tBTA_AV));
            if (event == BTA_AV_RC_OPEN_EVT )
            {
                bdcpy(btif_av_cb.peer_bda.address, ((tBTA_AV*)p_data)->rc_open.peer_addr);
            }
            else
            {
                bdcpy(btif_av_cb.peer_bda.address, ((tBTA_AV*)p_data)->pend.bd_addr);
            }
            // Only for AVDTP connection request move to opening state
            if (event == BTA_AV_PENDING_EVT)
                btif_sm_change_state(btif_av_cb.sm_handle, BTIF_AV_STATE_OPENING);
            HAL_CBACK(bt_av_callbacks, connection_priority_cb, &(btif_av_cb.peer_bda));
            break;

        case BTA_AV_REMOTE_CMD_EVT:
        case BTA_AV_VENDOR_CMD_EVT:
        case BTA_AV_META_MSG_EVT:
        case BTA_AV_RC_FEAT_EVT:
        case BTA_AV_BROWSE_MSG_EVT:
        case BTA_AV_REMOTE_RSP_EVT:
            btif_rc_handler(event, (tBTA_AV*)p_data);
            break;

        case BTA_AV_RC_CLOSE_EVT:
            if (tle_av_open_on_rc.in_use) {
                BTIF_TRACE_DEBUG0("BTA_AV_RC_CLOSE_EVT: Stopping AV timer.");
                btu_stop_timer(&tle_av_open_on_rc);
            }
            btif_rc_handler(event, p_data);
            break;

        default:
            BTIF_TRACE_WARNING2("%s : unhandled event:%s", __FUNCTION__,
                                dump_av_sm_event_name(event));
            return FALSE;

    }
    return TRUE;
}
/*****************************************************************************
**
** Function        btif_av_state_opening_handler
**
** Description     Intermediate state managing events during establishment
**                 of avdtp channel
**
** Returns         TRUE if event was processed, FALSE otherwise
**
*******************************************************************************/

static BOOLEAN btif_av_state_opening_handler(btif_sm_event_t event, void *p_data)
{
    BTIF_TRACE_IMP3("%s event:%s flags %x", __FUNCTION__,
                     dump_av_sm_event_name(event), btif_av_cb.flags);

    switch (event)
    {
        case BTIF_SM_ENTER_EVT:
            /* inform the application that we are entering connecting state */
            HAL_CBACK(bt_av_callbacks, connection_state_cb,
                      BTAV_CONNECTION_STATE_CONNECTING, &(btif_av_cb.peer_bda));
            break;

        case BTIF_SM_EXIT_EVT:
            break;

        case BTA_AV_REJECT_EVT:
            BTIF_TRACE_DEBUG0(" Received  BTA_AV_REJECT_EVT ");
            HAL_CBACK(bt_av_callbacks, connection_state_cb,
                BTAV_CONNECTION_STATE_DISCONNECTED, &(btif_av_cb.peer_bda));
            btif_sm_change_state(btif_av_cb.sm_handle, BTIF_AV_STATE_IDLE);
            break;

        case BTA_AV_OPEN_EVT:
        {
            tBTA_AV *p_bta_data = (tBTA_AV*)p_data;
            btav_connection_state_t state;
            btif_sm_state_t av_state;
            BTIF_TRACE_DEBUG2("status:%d, edr 0x%x",p_bta_data->open.status,
                               p_bta_data->open.edr);

            if (p_bta_data->open.status == BTA_AV_SUCCESS)
            {
                 state = BTAV_CONNECTION_STATE_CONNECTED;
                 av_state = BTIF_AV_STATE_OPENED;
                 btif_av_cb.edr = p_bta_data->open.edr;
                 if (p_bta_data->open.edr & BTA_AV_EDR_3MBPS)
                 {
                     BTIF_TRACE_DEBUG0("remote supports 3 mbps");
                     btif_av_cb.edr_3mbps = TRUE;
                 }

                 if (p_bta_data->open.sep == AVDT_TSEP_SRC)
                    btif_av_cb.sep = SEP_SRC;
                 else if (p_bta_data->open.sep == AVDT_TSEP_SNK)
                     btif_av_cb.sep = SEP_SNK;
            }
            else
            {
                BTIF_TRACE_WARNING1("BTA_AV_OPEN_EVT::FAILED status: %d",
                                     p_bta_data->open.status );
                state = BTAV_CONNECTION_STATE_DISCONNECTED;
                av_state  = BTIF_AV_STATE_IDLE;
            }

            /* inform the application of the event */
            HAL_CBACK(bt_av_callbacks, connection_state_cb,
                             state, &(btif_av_cb.peer_bda));
            /* change state to open/idle based on the status */
            btif_sm_change_state(btif_av_cb.sm_handle, av_state);
            /* if queued PLAY command,  send it now */
            btif_rc_check_handle_pending_play(p_bta_data->open.bd_addr,
                                             (p_bta_data->open.status == BTA_AV_SUCCESS));
            btif_queue_advance();
        } break;

        case BTIF_AV_CONNECT_REQ_EVT:
            // Check for device, if same device which moved to opening then ignore callback
            if (memcmp ((bt_bdaddr_t*)p_data, &(btif_av_cb.peer_bda),
                sizeof(btif_av_cb.peer_bda)) == 0)
            {
                BTIF_TRACE_WARNING0("Same device moved to Opening state,ignore Connect Req");
                break;
            }
            BTIF_TRACE_WARNING0("Moved from idle by Incoming Connection request");
            HAL_CBACK(bt_av_callbacks, connection_state_cb,
                        BTAV_CONNECTION_STATE_DISCONNECTED, (bt_bdaddr_t*)p_data);

            break;
        case BTA_AV_PENDING_EVT:
            // Check for device, if same device which moved to opening then ignore callback
            if (memcmp (((tBTA_AV*)p_data)->pend.bd_addr, &(btif_av_cb.peer_bda),
                sizeof(btif_av_cb.peer_bda)) == 0)
            {
                BTIF_TRACE_WARNING0("Same device moved to Opening state,ignore Pending Req");
                break;
            }
            BTIF_TRACE_WARNING0("Moved from idle by outgoing Connection request");
            BTA_AvDisconnect(((tBTA_AV*)p_data)->pend.bd_addr);
            break;
        case BTIF_AV_DISCONNECT_REQ_EVT:
            HAL_CBACK(bt_av_callbacks, connection_state_cb,
                BTAV_CONNECTION_STATE_DISCONNECTED, &(btif_av_cb.peer_bda));
            btif_sm_change_state(btif_av_cb.sm_handle, BTIF_AV_STATE_IDLE);
            break;
        CHECK_RC_EVENT(event, p_data);

        default:
            BTIF_TRACE_WARNING2("%s : unhandled event:%s", __FUNCTION__,
                                dump_av_sm_event_name(event));
            return FALSE;

   }
   return TRUE;
}


/*****************************************************************************
**
** Function        btif_av_state_closing_handler
**
** Description     Intermediate state managing events during closing
**                 of avdtp channel
**
** Returns         TRUE if event was processed, FALSE otherwise
**
*******************************************************************************/

static BOOLEAN btif_av_state_closing_handler(btif_sm_event_t event, void *p_data)
{
    BTIF_TRACE_IMP3("%s event:%s flags %x", __FUNCTION__,
                     dump_av_sm_event_name(event), btif_av_cb.flags);

    switch (event)
    {
        case BTIF_SM_ENTER_EVT:
            if (btif_av_cb.sep == SEP_SNK)
            {
                /* immediately stop transmission of frames */
                btif_a2dp_set_tx_flush(TRUE);
                /* wait for audioflinger to stop a2dp */
            }
            if (btif_av_cb.sep == SEP_SRC)
            {
                btif_a2dp_set_rx_flush(TRUE);
            }
            break;

        case BTA_AV_STOP_EVT:
        case BTIF_AV_STOP_STREAM_REQ_EVT:
            if (btif_av_cb.sep == SEP_SNK)
            {
              /* immediately flush any pending tx frames while suspend is pending */
              btif_a2dp_set_tx_flush(TRUE);
            }
            if (btif_av_cb.sep == SEP_SRC)
            {
                btif_a2dp_set_rx_flush(TRUE);
            }
            /* inform the application that we are audio is stopped */
            HAL_CBACK(bt_av_callbacks, audio_state_cb,
                BTAV_AUDIO_STATE_STOPPED, &(btif_av_cb.peer_bda));

            btif_a2dp_on_stopped(NULL);
              break;

        case BTIF_SM_EXIT_EVT:
            break;

        case BTA_AV_CLOSE_EVT:

            /* inform the application that we are disconnecting */
            HAL_CBACK(bt_av_callbacks, connection_state_cb,
                BTAV_CONNECTION_STATE_DISCONNECTED, &(btif_av_cb.peer_bda));

            btif_sm_change_state(btif_av_cb.sm_handle, BTIF_AV_STATE_IDLE);
            break;

        /* Handle the RC_CLOSE event for the cleanup */
        case BTA_AV_RC_CLOSE_EVT:
            btif_rc_handler(event, (tBTA_AV*)p_data);
            break;

        default:
            BTIF_TRACE_WARNING2("%s : unhandled event:%s", __FUNCTION__,
                                dump_av_sm_event_name(event));
            return FALSE;
   }
   return TRUE;
}


/*****************************************************************************
**
** Function     btif_av_state_opened_handler
**
** Description  Handles AV events while AVDTP is in OPEN state
**
** Returns      TRUE if event was processed, FALSE otherwise
**
*******************************************************************************/

static BOOLEAN btif_av_state_opened_handler(btif_sm_event_t event, void *p_data)
{
    tBTA_AV *p_av = (tBTA_AV*)p_data;
    tBTIF_STATUS status = BTIF_SUCCESS;

    BTIF_TRACE_IMP3("%s event:%s flags %x", __FUNCTION__,
                     dump_av_sm_event_name(event), btif_av_cb.flags);

    if ( (event == BTA_AV_REMOTE_CMD_EVT) && (btif_av_cb.flags & BTIF_AV_FLAG_REMOTE_SUSPEND) &&
         (p_av->remote_cmd.rc_id == BTA_AV_RC_PLAY) )
    {
        BTIF_TRACE_EVENT1("%s: Resetting remote suspend flag on RC PLAY", __FUNCTION__);
        btif_av_cb.flags &= ~BTIF_AV_FLAG_REMOTE_SUSPEND;
    }

    switch (event)
    {
        case BTIF_SM_ENTER_EVT:
            btif_av_cb.flags &= ~BTIF_AV_FLAG_PENDING_STOP;
            btif_av_cb.flags &= ~BTIF_AV_FLAG_PENDING_START;
            break;

        case BTIF_SM_EXIT_EVT:
            btif_av_cb.flags &= ~BTIF_AV_FLAG_PENDING_START;
            break;

        case BTIF_AV_START_STREAM_REQ_EVT:
            if (btif_av_cb.sep == SEP_SRC)
            {
                BTA_AvStart();
                btif_av_cb.flags |= BTIF_AV_FLAG_PENDING_START;
                break;
            }
            status = btif_a2dp_setup_codec();
            if (status == BTIF_SUCCESS)
            {
                BTA_AvStart();
                btif_av_cb.flags |= BTIF_AV_FLAG_PENDING_START;
            }
            else
            {
                BTIF_TRACE_ERROR1("## AV Disconnect## status : %x",status);
                BTA_AvDisconnect(btif_av_cb.peer_bda.address);
            }
            break;

        case BTA_AV_START_EVT:
        {
            BTIF_TRACE_EVENT3("BTA_AV_START_EVT status %d, suspending %d, init %d",
                p_av->start.status, p_av->start.suspending, p_av->start.initiator);

            if ((p_av->start.status == BTA_SUCCESS) && (p_av->start.suspending == TRUE))
                return TRUE;

            /* if remote tries to start a2dp when DUT is a2dp source
             * then suspend. In case a2dp is sink and call is active
             * then disconnect the AVDTP channel
             */
            if (!(btif_av_cb.flags & BTIF_AV_FLAG_PENDING_START))
            {
                if (btif_av_cb.sep == SEP_SNK)
                {
                    BTIF_TRACE_EVENT1("%s: trigger suspend as remote initiated!!", __FUNCTION__);
                    btif_dispatch_sm_event(BTIF_AV_SUSPEND_STREAM_REQ_EVT, NULL, 0);
                }
                else if (!btif_multihf_is_call_idle())
                {
                    BTIF_TRACE_WARNING0(" Peer is SRC, Disc the Link ");
                    btif_dispatch_sm_event(BTIF_AV_DISCONNECT_REQ_EVT, NULL, 0);
                }
            }

            /*  In case peer is A2DP SRC we do not want to ack commands on UIPC*/
            if (btif_av_cb.sep == SEP_SNK)
            {
                if (btif_a2dp_on_started(&p_av->start,
                    ((btif_av_cb.flags & BTIF_AV_FLAG_PENDING_START) != 0)))
                {
                    /* only clear pending flag after acknowledgement */
                    btif_av_cb.flags &= ~BTIF_AV_FLAG_PENDING_START;
                }
            }

            /* remain in open state if status failed */
            if (p_av->start.status != BTA_AV_SUCCESS)
                return FALSE;

            if (btif_av_cb.sep == SEP_SRC)
            {
                btif_a2dp_set_rx_flush(FALSE); /*  remove flush state, ready for streaming*/
            }

            /* change state to started, send acknowledgement if start is pending */
            if (btif_av_cb.flags & BTIF_AV_FLAG_PENDING_START) {
                if (btif_av_cb.sep == SEP_SNK)
                    btif_a2dp_on_started(NULL, TRUE);
                /* pending start flag will be cleared when exit current state */
            }
            btif_sm_change_state(btif_av_cb.sm_handle, BTIF_AV_STATE_STARTED);

        } break;

        case BTIF_AV_DISCONNECT_REQ_EVT:
            BTA_AvClose(btif_av_cb.bta_handle);

            /* inform the application that we are disconnecting */
            HAL_CBACK(bt_av_callbacks, connection_state_cb,
               BTAV_CONNECTION_STATE_DISCONNECTING, &(btif_av_cb.peer_bda));
            break;

        case BTA_AV_CLOSE_EVT:
             /* avdtp link is closed */
            btif_a2dp_on_stopped(NULL);

            /* inform the application that we are disconnected */
            HAL_CBACK(bt_av_callbacks, connection_state_cb,
                BTAV_CONNECTION_STATE_DISCONNECTED, &(btif_av_cb.peer_bda));

            /* change state to idle, send acknowledgement if start is pending */
            if (btif_av_cb.flags & BTIF_AV_FLAG_PENDING_START) {
                btif_a2dp_ack_fail();
                /* pending start flag will be cleared when exit current state */
            }
            btif_sm_change_state(btif_av_cb.sm_handle, BTIF_AV_STATE_IDLE);
            break;

        case BTA_AV_RECONFIG_EVT:
            if((btif_av_cb.flags & BTIF_AV_FLAG_PENDING_START) &&
                (p_av->reconfig.status == BTA_AV_SUCCESS))
            {
               APPL_TRACE_WARNING0("reconfig done BTA_AVstart()");
               BTA_AvStart();
            }
            else if(btif_av_cb.flags & BTIF_AV_FLAG_PENDING_START)
            {
               btif_av_cb.flags &= ~BTIF_AV_FLAG_PENDING_START;
               btif_a2dp_ack_fail();
            }
            break;

        case BTIF_AV_CONNECT_REQ_EVT:
            if (memcmp ((bt_bdaddr_t*)p_data, &(btif_av_cb.peer_bda),
                sizeof(btif_av_cb.peer_bda)) == 0)
            {
                BTIF_TRACE_WARNING0("Btif moved to opened by incoming request");
                BTIF_TRACE_WARNING0("Ignore BTIF_AV_CONNECT_REQ_EVT for same device");

            }
            else
            {
                BTIF_TRACE_WARNING0("Moved to opened by Other Incoming Conn req");
                HAL_CBACK(bt_av_callbacks, connection_state_cb,
                            BTAV_CONNECTION_STATE_DISCONNECTED, (bt_bdaddr_t*)p_data);
            }
            break;

        CHECK_RC_EVENT(event, p_data);

        default:
            BTIF_TRACE_WARNING2("%s : unhandled event:%s", __FUNCTION__,
                               dump_av_sm_event_name(event));
            return FALSE;

    }
    return TRUE;
}

/*****************************************************************************
**
** Function     btif_av_state_started_handler
**
** Description  Handles AV events while A2DP stream is started
**
** Returns      TRUE if event was processed, FALSE otherwise
**
*******************************************************************************/

static BOOLEAN btif_av_state_started_handler(btif_sm_event_t event, void *p_data)
{
    tBTA_AV *p_av = (tBTA_AV*)p_data;

    BTIF_TRACE_IMP3("%s event:%s flags %x", __FUNCTION__,
                     dump_av_sm_event_name(event), btif_av_cb.flags);

    switch (event)
    {
        case BTIF_SM_ENTER_EVT:

            /* we are again in started state, clear any remote suspend flags */
            btif_av_cb.flags &= ~BTIF_AV_FLAG_REMOTE_SUSPEND;
            HAL_CBACK(bt_av_callbacks, audio_state_cb,
                BTAV_AUDIO_STATE_STARTED, &(btif_av_cb.peer_bda));
            break;

        case BTIF_SM_EXIT_EVT:
            break;

        case BTIF_AV_START_STREAM_REQ_EVT:
            /* we were remotely started, just ack back the local request */
            if (btif_av_cb.sep == SEP_SNK)
                btif_a2dp_on_started(NULL, TRUE);
            break;

        /* fixme -- use suspend = true always to work around issue with BTA AV */
        case BTIF_AV_STOP_STREAM_REQ_EVT:
        case BTIF_AV_SUSPEND_STREAM_REQ_EVT:

            /* set pending flag to ensure btif task is not trying to restart
               stream while suspend is in progress */
            btif_av_cb.flags |= BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING;

            /* if we were remotely suspended but suspend locally, local suspend
               always overrides */
            btif_av_cb.flags &= ~BTIF_AV_FLAG_REMOTE_SUSPEND;

            if (btif_av_cb.sep == SEP_SNK)
            {
            /* immediately stop transmission of frames while suspend is pending */
                btif_a2dp_set_tx_flush(TRUE);
            }

            if (btif_av_cb.sep == SEP_SRC)
                btif_a2dp_set_rx_flush(TRUE);

            BTA_AvStop(TRUE);
            break;

        case BTIF_AV_DISCONNECT_REQ_EVT:

            /* request avdtp to close */
            BTA_AvClose(btif_av_cb.bta_handle);

            /* inform the application that we are disconnecting */
            HAL_CBACK(bt_av_callbacks, connection_state_cb,
                BTAV_CONNECTION_STATE_DISCONNECTING, &(btif_av_cb.peer_bda));

            /* wait in closing state until fully closed */
            btif_sm_change_state(btif_av_cb.sm_handle, BTIF_AV_STATE_CLOSING);
            break;

        case BTA_AV_SUSPEND_EVT:

            BTIF_TRACE_EVENT2("BTA_AV_SUSPEND_EVT status %d, init %d",
                 p_av->suspend.status, p_av->suspend.initiator);

            /* a2dp suspended, stop media task until resumed */
            btif_a2dp_on_suspended(&p_av->suspend);

            /* if not successful, remain in current state */
            if (p_av->suspend.status != BTA_AV_SUCCESS)
            {
                btif_av_cb.flags &= ~BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING;

               if (btif_av_cb.sep == SEP_SNK)
               {
                /* suspend failed, reset back tx flush state */
                    btif_a2dp_set_tx_flush(FALSE);
               }
                return FALSE;
            }

            if (p_av->suspend.initiator != TRUE)
            {
                /* remote suspend, notify HAL and await audioflinger to
                   suspend/stop stream */

                /* set remote suspend flag to block media task from restarting
                   stream only if we did not already initiate a local suspend */
                if ((btif_av_cb.flags & BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING) == 0)
                    btif_av_cb.flags |= BTIF_AV_FLAG_REMOTE_SUSPEND;

                HAL_CBACK(bt_av_callbacks, audio_state_cb,
                        BTAV_AUDIO_STATE_REMOTE_SUSPEND, &(btif_av_cb.peer_bda));
            }
            else
            {
                HAL_CBACK(bt_av_callbacks, audio_state_cb,
                        BTAV_AUDIO_STATE_STOPPED, &(btif_av_cb.peer_bda));
            }

            btif_sm_change_state(btif_av_cb.sm_handle, BTIF_AV_STATE_OPENED);

            /* suspend completed and state changed, clear pending status */
            btif_av_cb.flags &= ~BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING;
            break;

        case BTA_AV_STOP_EVT:

            btif_av_cb.flags |= BTIF_AV_FLAG_PENDING_STOP;
            btif_a2dp_on_stopped(&p_av->suspend);

            HAL_CBACK(bt_av_callbacks, audio_state_cb,
                      BTAV_AUDIO_STATE_STOPPED, &(btif_av_cb.peer_bda));

            /* if stop was successful, change state to open */
            if (p_av->suspend.status == BTA_AV_SUCCESS)
                btif_sm_change_state(btif_av_cb.sm_handle, BTIF_AV_STATE_OPENED);

            break;

            case BTIF_AV_REQUEST_AUDIO_FOCUS_EVT:
                HAL_CBACK(bt_av_callbacks, audio_focus_request_cb,
                                       1, &(btif_av_cb.peer_bda));
                break;

        case BTA_AV_CLOSE_EVT:

             btif_av_cb.flags |= BTIF_AV_FLAG_PENDING_STOP;

            /* avdtp link is closed */
            btif_a2dp_on_stopped(NULL);

            /* inform the application that we are disconnected */
            HAL_CBACK(bt_av_callbacks, connection_state_cb,
                BTAV_CONNECTION_STATE_DISCONNECTED, &(btif_av_cb.peer_bda));

            btif_sm_change_state(btif_av_cb.sm_handle, BTIF_AV_STATE_IDLE);
            break;

        CHECK_RC_EVENT(event, p_data);

        default:
            BTIF_TRACE_WARNING2("%s : unhandled event:%s", __FUNCTION__,
                                 dump_av_sm_event_name(event));
            return FALSE;

    }
    return TRUE;
}

/*****************************************************************************
**  Local event handlers
******************************************************************************/

static void btif_av_handle_event(UINT16 event, char* p_param)
{
    btif_sm_dispatch(btif_av_cb.sm_handle, event, (void*)p_param);
}

static void bte_av_callback(tBTA_AV_EVT event, tBTA_AV *p_data)
{
    /* Switch to BTIF context */
    btif_transfer_context(btif_av_handle_event, event,
                          (char*)p_data, sizeof(tBTA_AV), NULL);
}

static void bte_av_media_callback(tBTA_AV_EVT event, tBTA_AV_MEDIA *p_data)
{
    btif_sm_state_t state;
    UINT8 que_len;

    if (event == BTA_AV_MEDIA_DATA_EVT)/* Switch to BTIF_MEDIA context */
    {
        state= btif_sm_get_state(btif_av_cb.sm_handle);
        if ( (state == BTIF_AV_STATE_STARTED) || /* send SBC packets only in Started State */
             (state == BTIF_AV_STATE_OPENED) )
        {
            que_len = btif_media_sink_enque_buf((BT_HDR *)p_data);
            BTIF_TRACE_DEBUG1(" Packets in Que %d",que_len);
        }
        else
            return;
    }

    if (event == BTA_AV_MEDIA_SINK_CFG_EVT) /* send a command to BT Media Task */
        btif_reset_decoder((UINT8*)p_data);
}
/*******************************************************************************
**
** Function         btif_av_init
**
** Description      Initializes btif AV if not already done
**
** Returns          bt_status_t
**
*******************************************************************************/

bt_status_t btif_av_init(void)
{
    if (btif_av_cb.sm_handle == NULL)
    {
        if (btif_a2dp_start_media_task() != GKI_SUCCESS)
            return BT_STATUS_FAIL;

        btif_enable_service(BTA_A2DP_SERVICE_ID);

        /* Also initialize the AV state machine */
        btif_av_cb.sm_handle = btif_sm_init((const btif_sm_handler_t*)btif_av_state_handlers, BTIF_AV_STATE_IDLE);

        btif_a2dp_on_init();

        return BT_STATUS_SUCCESS;
    }

    return BT_STATUS_DONE;
}

/*******************************************************************************
**
** Function         init
**
** Description      Initializes the AV interface
**
** Returns          bt_status_t
**
*******************************************************************************/

static bt_status_t init(btav_callbacks_t* callbacks )
{
    int status;

    BTIF_TRACE_EVENT1("%s", __FUNCTION__);

    if (bt_av_callbacks)
        return BT_STATUS_DONE;

    bt_av_callbacks = callbacks;

    memset(&btif_av_cb, 0, sizeof(btif_av_cb_t));

    return btif_av_init();
}

/*******************************************************************************
**
** Function         connect
**
** Description      Establishes the AV signalling channel with the remote headset
**
** Returns          bt_status_t
**
*******************************************************************************/

static bt_status_t connect_int(bt_bdaddr_t *bd_addr)
{
    BTIF_TRACE_EVENT1("%s", __FUNCTION__);

    if(btif_storage_is_device_bonded (bd_addr))
    {
        btif_sm_dispatch(btif_av_cb.sm_handle, BTIF_AV_CONNECT_REQ_EVT, (char*)bd_addr);
    }
    else
    {
        bdstr_t bdstr;

        BTIF_TRACE_ERROR1("## connect_int ## Device Not Bonded : %s", bd2str (bd_addr, &bdstr));
        /* inform the application of the disconnection as the connection is not processed */
        HAL_CBACK(bt_av_callbacks, connection_state_cb,
                         BTAV_CONNECTION_STATE_DISCONNECTED, bd_addr);
        btif_queue_advance();
    }

    return BT_STATUS_SUCCESS;
}

static bt_status_t connect(bt_bdaddr_t *bd_addr)
{
    CHECK_BTAV_INIT();

    btif_queue_remove_connect(UUID_SERVCLASS_AUDIO_SOURCE, BTIF_QUEUE_CHECK_CONNECT_REQ);

    if(btif_av_cb.bta_handle)
       return btif_queue_connect(UUID_SERVCLASS_AUDIO_SOURCE, bd_addr, connect_int, BTIF_QUEUE_CONNECT_EVT);
    else
       return btif_queue_connect(UUID_SERVCLASS_AUDIO_SOURCE, bd_addr, connect_int, BTIF_QUEUE_PENDING_CONECT_EVT);
}

/*******************************************************************************
**
** Function         disconnect
**
** Description      Tears down the AV signalling channel with the remote headset
**
** Returns          bt_status_t
**
*******************************************************************************/
static bt_status_t disconnect(bt_bdaddr_t *bd_addr)
{
    BTIF_TRACE_EVENT1("%s", __FUNCTION__);

    CHECK_BTAV_INIT();

    /* Switch to BTIF context */
    return btif_transfer_context(btif_av_handle_event, BTIF_AV_DISCONNECT_REQ_EVT,
                                 (char*)bd_addr, sizeof(bt_bdaddr_t), NULL);
}

/*******************************************************************************
**
** Function         cleanup
**
** Description      Shuts down the AV interface and does the cleanup
**
** Returns          None
**
*******************************************************************************/
static void cleanup(void)
{
    BTIF_TRACE_EVENT1("%s", __FUNCTION__);

    if (bt_av_callbacks)
    {
        btif_a2dp_stop_media_task();

        btif_disable_service(BTA_A2DP_SERVICE_ID);
        bt_av_callbacks = NULL;

        /* Also shut down the AV state machine */
        btif_sm_shutdown(btif_av_cb.sm_handle);
        btif_av_cb.sm_handle = NULL;
    }
    return;
}

/*******************************************************************************
**
** Function         is_src
**
** Description      Checks if peer device is A2DP SRC
**
** Returns          Success in case peer is A2DP Src, FAIL otherwise
**
*******************************************************************************/
bt_status_t is_src( bt_bdaddr_t *bd_addr )
{
    BTIF_TRACE_DEBUG0(" isSrc:  Check if peer device with bd_addr is audio src or sink");
    if (btif_av_cb.sep == SEP_SRC)
    {
        BTIF_TRACE_DEBUG0(" Current Peer is SRC");
        return BT_STATUS_SUCCESS;
    }
    else if (btif_av_cb.sep == SEP_SNK)
    {
        BTIF_TRACE_DEBUG0(" Current Peer is SNK");
        return BT_STATUS_FAIL;
    }
    else
    {
        BTIF_TRACE_DEBUG0(" Stream not opened till now");
        return BT_STATUS_NOT_READY;
    }
}

/*******************************************************************************
**
** Function         suspend_sink
**
** Description      Suspends stream  in case of A2DP Sink
**
** Returns          None
**
*******************************************************************************/
void suspend_sink()
{
    BTIF_TRACE_DEBUG0(" suspend Stream Suspend called");
    if (btif_av_cb.sep == SEP_SRC)
        btif_dispatch_sm_event(BTIF_AV_SUSPEND_STREAM_REQ_EVT, NULL, 0);
}

/*******************************************************************************
**
** Function         resume_sink
**
** Description      Resumes stream  in case of A2DP Sink
**
** Returns          None
**
*******************************************************************************/
void resume_sink()
{
    BTIF_TRACE_DEBUG0(" resume Stream called");
    if (btif_av_cb.sep == SEP_SRC)
        btif_dispatch_sm_event(BTIF_AV_START_STREAM_REQ_EVT, NULL, 0);
}

/*******************************************************************************
**
** Function         audio_focus_status
**
** Description      Updates audio focus state
**
** Returns          None
**
*******************************************************************************/
static void audio_focus_status(int state)
{
    BTIF_TRACE_DEBUG1(" Audio Focus granted %d",state);

    btif_a2dp_set_audio_focus_state(state);
}

static void allow_connection(int is_valid)
{
    BTIF_TRACE_EVENT3(" %s isValid is %d event %d", __FUNCTION__,is_valid,idle_rc_event);
    switch (idle_rc_event)
    {
        case BTA_AV_RC_OPEN_EVT:
            if (is_valid)
            {
                memset(&tle_av_open_on_rc, 0, sizeof(tle_av_open_on_rc));
                tle_av_open_on_rc.param = (UINT32)btif_initiate_av_open_tmr_hdlr;
                btu_start_timer(&tle_av_open_on_rc, BTU_TTYPE_USER_FUNC,
                        BTIF_TIMEOUT_AV_OPEN_ON_RC_SECS);
                btif_rc_handler(idle_rc_event, &idle_rc_data);
            }
            else
            {
                UINT8 rc_handle =  idle_rc_data.rc_open.rc_handle;
                BTA_AvCloseRc(rc_handle);
            }
            break;

        case BTA_AV_PENDING_EVT:
            if (is_valid)
            {
                BTA_AvOpen(btif_av_cb.peer_bda.address, btif_av_cb.bta_handle,
                       TRUE, BTA_SEC_NONE);
            }
            else
            {
                BTA_AvDisconnect(idle_rc_data.pend.bd_addr);
            }
            break;

        default:
            BTIF_TRACE_WARNING2("%s : unhandled event:%s", __FUNCTION__,
                                dump_av_sm_event_name(idle_rc_event));
    }
    idle_rc_event = 0;
    memset(&idle_rc_data, 0, sizeof(tBTA_AV));
}

static const btav_interface_t bt_av_interface = {
    sizeof(btav_interface_t),
    init,
    connect,
    disconnect,
    cleanup,
    allow_connection,
    is_src,
    suspend_sink,
    resume_sink,
    audio_focus_status,
};

/*******************************************************************************
**
** Function         btif_av_get_sm_handle
**
** Description      Fetches current av SM handle
**
** Returns          None
**
*******************************************************************************/

btif_sm_handle_t btif_av_get_sm_handle(void)
{
    return btif_av_cb.sm_handle;
}

/*******************************************************************************
**
** Function         btif_av_get_addr
**
** Description      Fetches current AV BD address
**
** Returns          BD address
**
*******************************************************************************/

bt_bdaddr_t btif_av_get_addr(void)
{
    return btif_av_cb.peer_bda;
}

/*******************************************************************************
**
** Function         btif_av_stream_ready
**
** Description      Checks whether AV is ready for starting a stream
**
** Returns          None
**
*******************************************************************************/

BOOLEAN btif_av_stream_ready(void)
{
    btif_sm_state_t state = btif_sm_get_state(btif_av_cb.sm_handle);

    BTIF_TRACE_DEBUG3("btif_av_stream_ready : sm hdl %d, state %d, flags %x",
                btif_av_cb.sm_handle, state, btif_av_cb.flags);

    /* also make sure main adapter is enabled */
    if (btif_is_enabled() == 0)
    {
        BTIF_TRACE_EVENT0("main adapter not enabled");
        return FALSE;
    }

    /* check if we are remotely suspended or stop is pending */
    if (btif_av_cb.flags & (BTIF_AV_FLAG_REMOTE_SUSPEND|BTIF_AV_FLAG_PENDING_STOP))
        return FALSE;

    return (state == BTIF_AV_STATE_OPENED);
}

/*******************************************************************************
**
** Function         btif_av_stream_started_ready
**
** Description      Checks whether AV ready for media start in streaming state
**
** Returns          None
**
*******************************************************************************/

BOOLEAN btif_av_stream_started_ready(void)
{
    btif_sm_state_t state = btif_sm_get_state(btif_av_cb.sm_handle);

    BTIF_TRACE_DEBUG3("btif_av_stream_started : sm hdl %d, state %d, flags %x",
                btif_av_cb.sm_handle, state, btif_av_cb.flags);

    /* disallow media task to start if we have pending actions */
    if (btif_av_cb.flags & (BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING | BTIF_AV_FLAG_REMOTE_SUSPEND
        | BTIF_AV_FLAG_PENDING_STOP))
        return FALSE;

    return (state == BTIF_AV_STATE_STARTED);
}

/*******************************************************************************
**
** Function         btif_dispatch_sm_event
**
** Description      Send event to AV statemachine
**
** Returns          None
**
*******************************************************************************/

/* used to pass events to AV statemachine from other tasks */
void btif_dispatch_sm_event(btif_av_sm_event_t event, void *p_data, int len)
{
    /* Switch to BTIF context */
    btif_transfer_context(btif_av_handle_event, event,
                          (char*)p_data, len, NULL);
}

/*******************************************************************************
**
** Function         btif_av_execute_service
**
** Description      Initializes/Shuts down the service
**
** Returns          BT_STATUS_SUCCESS on success, BT_STATUS_FAIL otherwise
**
*******************************************************************************/
bt_status_t btif_av_execute_service(BOOLEAN b_enable)
{
     if (b_enable)
     {
         /* TODO: Removed BTA_SEC_AUTHORIZE since the Java/App does not
          * handle this request in order to allow incoming connections to succeed.
          * We need to put this back once support for this is added */

         /* Added BTA_AV_FEAT_NO_SCO_SSPD - this ensures that the BTA does not
          * auto-suspend av streaming on AG events(SCO or Call). The suspend shall
          * be initiated by the app/audioflinger layers */
#if (AVRC_METADATA_INCLUDED == TRUE)
         BTA_AvEnable(BTA_SEC_AUTHENTICATE,
             BTA_AV_FEAT_RCTG|BTA_AV_FEAT_METADATA|BTA_AV_FEAT_VENDOR|BTA_AV_FEAT_NO_SCO_SSPD
#if (AVRC_ADV_CTRL_INCLUDED == TRUE)
             |BTA_AV_FEAT_RCCT
             |BTA_AV_FEAT_ADV_CTRL
#endif
             ,bte_av_callback);
#else
         BTA_AvEnable(BTA_SEC_AUTHENTICATE, (BTA_AV_FEAT_RCTG | BTA_AV_FEAT_NO_SCO_SSPD),
                      bte_av_callback);
#endif
         BTA_AvRegister(BTA_AV_CHNL_AUDIO, BTIF_AV_SERVICE_NAME, 0, bte_av_media_callback);
     }
     else {
         BTA_AvDeregister(btif_av_cb.bta_handle);
         BTA_AvDisable();
     }
     return BT_STATUS_SUCCESS;
}

/*******************************************************************************
**
** Function         btif_av_get_interface
**
** Description      Get the AV callback interface
**
** Returns          btav_interface_t
**
*******************************************************************************/
const btav_interface_t *btif_av_get_interface(void)
{
    BTIF_TRACE_EVENT1("%s", __FUNCTION__);
    return &bt_av_interface;
}

/*******************************************************************************
**
** Function         btif_av_is_connected
**
** Description      Checks if av has a connected sink
**
** Returns          BOOLEAN
**
*******************************************************************************/
BOOLEAN btif_av_is_connected(void)
{
    btif_sm_state_t state = btif_sm_get_state(btif_av_cb.sm_handle);
    return ((state == BTIF_AV_STATE_OPENED) || (state ==  BTIF_AV_STATE_STARTED));
}

/*******************************************************************************
**
** Function         btif_av_is_peer_edr
**
** Description      Check if the connected a2dp device supports
**                  EDR or not. Only when connected this function
**                  will accurately provide a true capability of
**                  remote peer. If not connected it will always be false.
**
** Returns          TRUE if remote device is capable of EDR
**
*******************************************************************************/
BOOLEAN btif_av_is_peer_edr(void)
{
    ASSERTC(btif_av_is_connected(), "No active a2dp connection", 0);

    if (btif_av_cb.edr)
        return TRUE;
    else
        return FALSE;
}

/*******************************************************************************
**
** Function         btif_av_peer_supports_3mbps
**
** Description      check if the connected a2dp device supports
**                  3mbps edr. Only when connected this function
**                  will accurately provide a true capability of
**                  remote peer. If not connected it will always be false.
**
** Returns          TRUE if remote device is EDR and supports 3mbps
**
*******************************************************************************/
BOOLEAN btif_av_peer_supports_3mbps(void)
{
    ASSERTC(btif_av_is_connected(), "No active a2dp connection", 0);
    BTIF_TRACE_DEBUG1("btif_av_peer_supports_3mbps: %d", btif_av_cb.edr_3mbps);
    if(btif_av_cb.edr_3mbps)
        return TRUE;
    else
        return FALSE;
}

/******************************************************************************
**
** Function        btif_av_clear_remote_suspend_flag
**
** Description     Clears btif_av_cd.flags if BTIF_AV_FLAG_REMOTE_SUSPEND is set
**
** Returns          void
******************************************************************************/
void btif_av_clear_remote_suspend_flag(void)
{
    BTIF_TRACE_DEBUG1(" flag :%x",btif_av_cb.flags);
    btif_av_cb.flags  &= ~BTIF_AV_FLAG_REMOTE_SUSPEND;
}
/*******************************************************************************
**
** Function         btif_av_close_update
**
** Description      close audio and update to application layer
**
** Returns          boolean
**
*******************************************************************************/
void btif_av_close_update(void)
{

    /* inform the application that we are disconnected */
    BTIF_TRACE_DEBUG0("Inform application for disconection ");
    btif_a2dp_on_stopped(NULL);
    HAL_CBACK(bt_av_callbacks, connection_state_cb,
              BTAV_CONNECTION_STATE_DISCONNECTED, &(btif_av_cb.peer_bda));
}
/*******************************************************************************
**
** Function         btif_av_move_idle
**
** Description      Opening state is intermediate state. It cannot handle
**                  incoming/outgoing connect/disconnect requests.When ACL
**                  is disconnected and we are in opening state then move back
**                  to idle state which is proper to handle connections.
**
** Returns          Void
**
*******************************************************************************/
void btif_av_move_idle(bt_bdaddr_t bd_addr)
{
    /* inform the application that ACL is disconnected and move to idle state */
    btif_sm_state_t state = btif_sm_get_state(btif_av_cb.sm_handle);
    BTIF_TRACE_DEBUG2("ACL Disconnected state %d  is same device %d",state,
            memcmp (&bd_addr, &(btif_av_cb.peer_bda), sizeof(bd_addr)));
    if (state == BTIF_AV_STATE_OPENING &&
            (memcmp (&bd_addr, &(btif_av_cb.peer_bda), sizeof(bd_addr)) == 0))
    {
        BTIF_TRACE_DEBUG0("Moving State from Opening to Idle due to ACL disconnect");
        HAL_CBACK(bt_av_callbacks, connection_state_cb,
                  BTAV_CONNECTION_STATE_DISCONNECTED, &(btif_av_cb.peer_bda));
        btif_sm_change_state(btif_av_cb.sm_handle, BTIF_AV_STATE_IDLE);
        btif_queue_advance();
    }
}
