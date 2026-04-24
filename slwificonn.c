/*
 * Copyright (c) 2016, Texas Instruments Incorporated
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * *  Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * *  Neither the name of Texas Instruments Incorporated nor the names of
 *    its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*****************************************************************************

   Application Name     - Provisioning application
   Application Overview - This application demonstrates how to use 
                          the provisioning method
                        in order to establish connection to the AP.
                        The application
                        connects to an AP and ping's the gateway to
                        verify the connection.

   Application Details  - Refer to 'Provisioning' README.html

*****************************************************************************/
//****************************************************************************
//
//! \addtogroup
//! @{
//
//****************************************************************************

/* Standard Include */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

/* TI-DRIVERS Header files */
#include <ti/drivers/net/wifi/simplelink.h>
#include "slwificonn.h"
#include <ti/net/slnetif.h>
#include <ti/net/slneterr.h>
#include <ti/drivers/SPI.h>
#include <ti/drivers/GPIO.h>

#include <pthread.h>
#include "mqueue.h"
#include "assert.h"
#include <time.h>
#include "semaphore.h"

#include <services/debug/debug.h>

/****************************************************************************
                      MACRO DEFINITIONS
****************************************************************************/
/* Error checking definition for external API */
#define VERIFY_INITIALIZED()                if(g_ctx == NULL) return SLNETERR_API_ABORTED;
#define VERIFY_NOT_INITIALIZED()            if(g_ctx != NULL) return SLNETERR_RET_CODE_DEV_ALREADY_STARTED;
#define VERIFY_NO_ERROR(rc)                 if(rc < 0) return rc;

/* USER's defines */
#define TIMER_TASK_STACK_SIZE                   (1024)
#define SL_STOP_TIMEOUT                         (200)


/* NON PERSISTENT definition for slWlanSetPolicy */
#define NON_PERSISTANT_FLAG 1

/* General WIFI definition */
#define MAX_SSID_LEN           (32)
#define MAX_KEY_LEN            (64)
#define MAX_USER_LEN           (64)
#define MAX_ANON_USER_LEN      (64)

/* Profile structure access definition */
#define PROFILE_SSID(pProf)       ((int8_t*)&pProf[1])
#define PROFILE_PASSWORD(pProf)   (PROFILE_SSID(pProf) + pProf->ssidLen)
#define PROFILE_USER(pProf)       (PROFILE_PASSWORD(pProf) + pProf->secParams.KeyLen)
#define PROFILE_ANON_USER(pProf)  (PROFILE_USER(pProf) + pProf->extSecParams.UserLen)

/* Critical Section Lock */
#define CRITICAL_SECTION_CREATE()                         sem_init(&g_csLock, 0, 1)
#define CRITICAL_SECTION_ENTER()                          sem_wait(&g_csLock)
#define CRITICAL_SECTION_EXIT()                           sem_post(&g_csLock)
#define CRITICAL_SECTION_DESTROY()                        sem_destroy(&g_csLock)

/****************************************************************************
                      TYPE DEFINITIONS (STRUCT & ENUM)
****************************************************************************/
/* Application's states */
typedef enum
{
    SLWIFICONN_STATE_OFF,
    SLWIFICONN_STATE_IDLE,
    SLWIFICONN_STATE_WAIT_FOR_CONN,
    SLWIFICONN_STATE_WAIT_FOR_IP,
    SLWIFICONN_STATE_CONNECTED,
    SLWIFICONN_STATE_PROVISIONING,
    SLWIFICONN_STATE_PROV_STOPPING,
    SLWIFICONN_STATE_EXT_PROVISIONING,

    SLWIFICONN_STATE_MAX
}SLWIFICONN_STATE_e;


/* Application's events */
typedef enum
{
    SLWIFICONN_EVENT_ENABLE,
    SLWIFICONN_EVENT_DISABLE,
    SLWIFICONN_EVENT_CONNECTED,
    SLWIFICONN_EVENT_DISCONNECT,
    SLWIFICONN_EVENT_IP_ACQUIRED,
    SLWIFICONN_EVENT_IP_LOST,

    SLWIFICONN_EVENT_PROV_SUCCESS,
    SLWIFICONN_EVENT_PROV_STOPPED,
    SLWIFICONN_EVENT_PROV_EXT_CONFIG_READY,
    SLWIFICONN_EVENT_PROV_CLOUD_CONFIRM_READY,

    SLWIFICONN_EVENT_ENABLE_PROVISIONING,
    SLWIFICONN_EVENT_ADD_PROFILE,
    SLWIFICONN_EVENT_EXT_PROV_COMPLETED,

    SLWIFICONN_EVENT_TIMEOUT,
    SLWIFICONN_EVENT_ERROR,
    SLWIFICONN_EVENT_RESET_REQUEST,
    SLWIFICONN_EVENT_DESTROY,

    SLWIFICONN_EVENT_MAX,
}SLWIFICONN_EVENT_e;

typedef int (*SMHandler_f)(SLWIFICONN_EVENT_e);  /* SM-Specific event handler */

#ifdef WIFI_STATS
/* WiFi statistics result data - sent via callback to application */
typedef struct {
    uint8_t     bssid[6];
    int16_t     mgMntRssi;      // Management RSSI
    int16_t     dataCtrlRssi;   // Data/Control RSSI
} SlWifiConnStats_t;

/* Extended event ID for stats callback (use high number to avoid SDK conflicts) */
//#define WifiConnEvent_STATS_RESULT  100
#endif

typedef struct{
    uint8_t                 ssidLen;
    uint8_t                 bSetBssid; /* 1- if bssid was provided, 0 - else */
    uint8_t                 bssid[6];
    SlWlanSecParams_t       secParams;
    SlWlanSecParamsExt_t    extSecParams;
    uint8_t                 priority;
} profile_t;


/* User Commands that are received during active provisioning are being deferred
 * (since the SimpleLink is locked). in case of SL_RET_CODE_PROVISIONING_IN_PROGRESS,
 * the command will enqueued and provisioning STOP will be issued.
 * Once stopped, the command will get dequeued and executed.
 */
typedef struct
{
    SLWIFICONN_EVENT_e   event;
    void                *pParams;
} DeferredEvent_t;

typedef struct
{
    SLWIFICONN_STATE_e      state;
    mqd_t                   eventQ;
    mqd_t                   DeferredEventQ;
    timer_t                 timer;
    SlWifiConn_AppEventCB_f fWifiAppCB;
    uint8_t                 stopInProgress;
    uint8_t                 role;
    WifiConnProvMode_e      provMode;
    uint8_t                 provCmd;
    uint8_t                 provFlags;
    bool                    bDeleteAllProfiles;
    uint16_t                idleInterval;
    char                    connectionSSID[MAX_SSID_LEN+1];
    uint8_t                 connectionBSSID[6];
    SlIpV4AcquiredAsync_t   IpAcquiredV4;           /* SL_NETAPP_EVENT_IPV4_ACQUIRED */
    SlIpV6AcquiredAsync_t   IpAcquiredV6;           /* SL_NETAPP_EVENT_IPV6_ACQUIRED */
    profile_t               *pNonPersistentProfile;
    bool                    bWlanConnectUsed;
    uint8_t                 disabledPolicy; // Wifi Disabled - connection Policy
    uint8_t                 enabledPolicy;  // Wifi Enabled - connection policy
#ifdef WIFI_STATS
    bool                    bStatsEnabled;      // Statistics collection enabled
    uint16_t                statsInterval;      // Collection interval in seconds
#endif
    /* Error tracking for reliability improvements */
    uint8_t                 fatalErrorCount;    // Consecutive fatal errors
    uint8_t                 recoveryDelayLevel; // Current backoff level (0-4)
} SlWifiConn_t;

/* Recovery delay levels (seconds) for exponential backoff after fatal errors */
static const uint8_t g_recoveryDelays[] = { 2, 5, 10, 20, 30 };
#define MAX_RECOVERY_LEVEL  (sizeof(g_recoveryDelays) / sizeof(g_recoveryDelays[0]) - 1)

/* NetIf registration can be called before SlNetConn_init, i.e. before the context
 * get allocated */
SlNetIf_Event_t         g_fNetIfCB = NULL;
uint16_t                g_ifId = 0;

/* Critical Section protection for this module */
sem_t                   g_csLock;

/****************************************************************************
                      LOCAL FUNCTION PROTOTYPES
****************************************************************************/
/* SM Event and State handlers */
static int SetState(SLWIFICONN_STATE_e state);
static int SignalEvent(SLWIFICONN_EVENT_e event);
static int ProcessEvent(SLWIFICONN_EVENT_e event);

/* SM States handlers */
static int smState_OFF(SLWIFICONN_EVENT_e event);
static int smState_IDLE(SLWIFICONN_EVENT_e event);
static int smState_WAIT_FOR_CONN(SLWIFICONN_EVENT_e event);
static int smState_PROVISIONING(SLWIFICONN_EVENT_e event);
static int smState_PROV_STOPPING(SLWIFICONN_EVENT_e event);
static int smState_EXT_PROVISIONING(SLWIFICONN_EVENT_e event);
static int smState_WAIT_FOR_IP(SLWIFICONN_EVENT_e event);
static int smState_CONNECTED(SLWIFICONN_EVENT_e event);

/* SM transition handlers */
static void EnterIDLE();
static void EnterWAIT_FOR_CONN(uint16_t timeout, bool bWlanDisconnect);
static void EnterWAIT_FOR_IP();
static void EnterCONNECTED(bool bNotify);
static int  EnterPROVISIONING();
static int  EnterPROV_STOPPING(SLWIFICONN_EVENT_e eventId, void *pParams);

/* Timer handlers */
static void AsyncEvtTimerHandler(sigval arg);
static void StartAsyncEvtTimer(uint32_t duration);
static void StopAsyncEvtTimer(void);

#ifdef WIFI_STATS
/* Statistics collection function */
static int CollectAndNotifyStats(void);
#endif

/* Sending Event Notifications (to NetIF / Wi-Fi app) */
static int NotifyNetIf(SlNetIfEventId_e EventId, void *pData);
static int NotifyWifiApp(WifiConnEventId_e EventId, WifiConnEventData_u *pData);

/* NWP (AUTO-CONNECT) suspend and resume */
static int SuspendNWP(bool bNotify);
static int ResumeNWP(bool bNotify);
static int ResetNWP();
static int Deinit();
static int SetRole(SlWlanMode_e role);

/* Deferred Events handling */
static int DeferredEvent_enqueue(SLWIFICONN_EVENT_e event, void *pParams);
static int DeferredEvent_dequeue(SLWIFICONN_EVENT_e *pEvent, void **ppParams);
static int DeferredEvent_process();

/****************************************************************************
                      GLOBAL VARIABLES
****************************************************************************/

static SlWifiConn_t *g_ctx = NULL;


static WifiConnSettings_t g_settings =
{
 3, 6, 60, 300,                           // Timeout: CONN, IP, RECONN, PROV
 20, 120,                                // Idle Timeout: MIN, MAX
 NULL, SL_WLAN_SEC_TYPE_OPEN, NULL,      // PROV AP Settings: SSID, SEC-TYPE, PASSWORD
 NULL,                                   // PROV SC Key
};

static SMHandler_f g_fWifiSM[SLWIFICONN_STATE_MAX] = {0};

/* The following will be register only when provisioning is enabled */
static int (*g_fEnterPROVISIONING)() = NULL;
static void (*g_fProvStatusHandler)(SlWlanEventProvisioningStatus_t *pWlanEvent) = NULL;

/* Profile status - checked during init before state machine starts */
static int8_t g_profileCount = -1;  /* -1 = not checked, 0 = none, >0 = count */

/* User Debug Print callback */
static void DummyPrintf(const char *pFormat, ...) {};
static SlWifiConn_Debug_f g_fDebugPrint = DummyPrintf;
static SlEventsListNode_t SlEventHandlers[3];


static const char *strState[] =
{
 "OFF",
 "IDLE",
 "WAIT_FOR_CONN",
 "WAIT_FOR_IP",
 "CONNECTED",
 "PROVISIONING",
 "PROV_STOPPING",
 "EXT_PROV",
};

static const char *strEvent[] =
{
 "ENABLE",
 "DISABLE",
 "CONNECTED",
 "DISCONNECT",
 "IP_ACQUIRED",
 "IP_LOST",

 "PROV_SUCCESS",
 "PROV_STOPPED",
 "PROV_EXT_CONF_READY",
 "PROV_CLOUD_CONF_READY",

 "ENABLE_PROV",
 "ADD_PROFILE",
 "EXT_PROV_COMPLETED",

 "TIMEOUT",
 "ERROR",
 "RESET_REQ",
 "DESTROY",
#ifdef WIFI_STATS
 "STATS_TIMER",
#endif
};

/****************************************************************************
                      LOCAL (STATIC) FUNCTIONS
****************************************************************************/


//*****************************************************************************
//
//! \brief  Push an event to deferred execution queue
//!
//*****************************************************************************
static int DeferredEvent_enqueue(SLWIFICONN_EVENT_e event, void *pParams)
{
    int retVal;
    DeferredEvent_t msg;
    msg.event = event;
    msg.pParams = pParams;

    retVal = mq_send(g_ctx->DeferredEventQ, (char *)&msg, sizeof(DeferredEvent_t), 0);
    assert(retVal >= 0);
    return retVal;
}

//*****************************************************************************
//
//! \brief  REtrieve an event from the deferred execution queue
//!
//*****************************************************************************
static int DeferredEvent_dequeue(SLWIFICONN_EVENT_e *pEvent, void **ppParams)
{
    int retVal;
    DeferredEvent_t msg;
    struct mq_attr mqstat;

    retVal = mq_getattr(g_ctx->DeferredEventQ, &mqstat);

    if(retVal == 0 && mqstat.mq_curmsgs)
    {
        retVal = mq_receive(g_ctx->DeferredEventQ, (char *)&msg, sizeof(DeferredEvent_t), 0);
        assert(retVal > 0);
        if(retVal > 0)
        {
            *pEvent = msg.event;
            *ppParams = msg.pParams;
        }
        // return number of pending messages (after this one was read)
        retVal = mqstat.mq_curmsgs - 1;
    }
    else
    {
        retVal = -1;
    }
    return retVal;
}


//*****************************************************************************
//
//! \brief  Handle non persistent profile (calling sl_WlanConnect)
//!
//*****************************************************************************
static int NonPersistentProfile_process(bool bWlanDisconnect)
{
    int retVal = 0;

    if(bWlanDisconnect)
    {
        retVal = sl_WlanDisconnect();
        g_ctx->bWlanConnectUsed = false;

    }
    else
    {
        profile_t *pProf = (profile_t *)g_ctx->pNonPersistentProfile;

        if(pProf)
        {
            uint8_t *pBssid = NULL;
            SlWlanSecParamsExt_t *pSecParamsExt = NULL;
            assert(pProf);
            if(pProf->bSetBssid)
            {
                pBssid = pProf->bssid;
            }
            if(pProf->secParams.Type == SL_WLAN_SEC_TYPE_WPA_ENT)
            {
                pSecParamsExt = &pProf->extSecParams;
            }
            retVal = sl_WlanConnect(PROFILE_SSID(pProf), pProf->ssidLen, pBssid, &pProf->secParams, pSecParamsExt);
            g_ctx->bWlanConnectUsed = true;
        }
    }
    return retVal;
}

//*****************************************************************************
//
//! \brief  Handle deferred events (ADD_PROFILE or DISABLE)
//!
//*****************************************************************************
static int DeferredEvent_process()
{
    int retVal = SL_RET_OBJ_NOT_SET;
    SLWIFICONN_EVENT_e deferredEvent;
    void *pParams;
    while (DeferredEvent_dequeue(&deferredEvent, &pParams) >= 0)
    {
        if(deferredEvent == SLWIFICONN_EVENT_ADD_PROFILE)
        {
            if(pParams)
            {
                uint8_t *pBssid = NULL;
                SlWlanSecParamsExt_t *pSecParamsExt = NULL;
                profile_t *pProf = (profile_t *)pParams;
                if(pProf->bSetBssid)
                {
                    pBssid = pProf->bssid;
                }
                if(pProf->secParams.Type == SL_WLAN_SEC_TYPE_WPA_ENT)
                {
                    pSecParamsExt = &pProf->extSecParams;
                }
                retVal = sl_WlanProfileAdd(PROFILE_SSID(pProf), pProf->ssidLen, pBssid, &pProf->secParams, pSecParamsExt, pProf->priority, 0);
                free(pProf);
            }
        }
        retVal = SignalEvent(deferredEvent);
        assert(retVal == 0);
    }
    return retVal;
}

//*****************************************************************************
//
//! \brief  Initiate the the Wifi-Enabled and Wifi-Disabled connection policies
//!
//*****************************************************************************
static int InitNWPConnPolicy()
{
    int retVal;
    uint8_t policy = 0;
    uint8_t length = 1;

    retVal = sl_WlanPolicyGet(SL_WLAN_POLICY_CONNECTION ,&policy, NULL, &length);
    if(retVal == 0)
    {
        /* Wi-Fi Enabled Policy:
         * Enable AUTO-CONN, Disable AUTO-PROVISIONING */
        g_ctx->enabledPolicy = (policy | SL_WLAN_CONNECTION_POLICY(1,0,0,0)) & ~(SL_WLAN_CONNECTION_POLICY(0,0,0,1));
        /* Wi-Fi Disabled Policy:
         * Disable AUTO-CONN, FAST-CONN, AUTO-PROVISIONING */
        g_ctx->disabledPolicy = policy & ~(SL_WLAN_CONNECTION_POLICY(1,1,0,1));
    }
    return retVal;
}

void SimpleLinkSocketTriggerEventHandler(SlSockTriggerEvent_t* pSlSockTriggerEvent);
void SimpleLinkSocketTriggerEventHandler(SlSockTriggerEvent_t* pSlSockTriggerEvent)
{
    LOG_DEBUG("EVENT HANDLER");
}

//*****************************************************************************
//
//! \brief  reset the NWP, and return the sl_Start return code
//!
//! Uses exponential backoff based on consecutive fatal errors to prevent
//! rapid reset cycles that can destabilize the NWP.
//!
//*****************************************************************************
int ResetNWP()
{
    uint8_t recoveryDelay;

    /* Increment fatal error count and calculate recovery delay */
    g_ctx->fatalErrorCount++;
    if(g_ctx->recoveryDelayLevel < MAX_RECOVERY_LEVEL)
    {
        g_ctx->recoveryDelayLevel++;
    }
    recoveryDelay = g_recoveryDelays[g_ctx->recoveryDelayLevel];

    g_fDebugPrint("SlWifiConn: Reset NWP (error #%d, delay %ds)\n\r",
                  g_ctx->fatalErrorCount, recoveryDelay);

    int retVal = sl_Stop(SL_STOP_TIMEOUT);
    assert(retVal == 0);

    /* Apply exponential backoff delay before restarting NWP.
     * This gives the hardware time to stabilize and prevents rapid
     * reset loops that can exacerbate SPI communication issues.
     */
    sleep(recoveryDelay);

    retVal = sl_Start(0,0,0);
    assert(retVal >= 0);

    /* Store the new role from sl_Start return value */
    g_ctx->role = retVal;

    /* Additional stabilization delay after sl_Start */
    sleep(2);

    /* Reinitialize connection policy values after NWP reset.
     * This is critical because after sl_Stop()/sl_Start() the NWP
     * may have different policy settings.
     */
    InitNWPConnPolicy();

    /* Set power policy to always-on for maximum stability.
     * This prevents power management transitions that can cause
     * SPI communication issues with the NWP.
     */
    sl_WlanPolicySet(SL_WLAN_POLICY_PM, SL_WLAN_LOW_LATENCY_POLICY, NULL, 0);

    if(g_ctx->state == SLWIFICONN_STATE_OFF ||
       g_ctx->state == SLWIFICONN_STATE_IDLE)
    {
        SuspendNWP(false);
    }
    else
    {
        ResumeNWP(false);
    }

    return retVal;
}

static int ResetNWP2AP()
{
    int retVal;

    /* Stop NWP and restart in AP mode for clean profile deletion.
     * Based on TI out_of_box provisioning_task.c pattern —
     * profiles must be deleted while NWP is in AP mode. */
    retVal = sl_Stop(SL_STOP_TIMEOUT);
    sleep(2);

    retVal = sl_Start(0, 0, 0);
    if(retVal < 0) return retVal;

    /* Switch to AP mode if not already */
    if(ROLE_AP != retVal)
    {
        sl_WlanSetMode(ROLE_AP);
        sl_Stop(SL_STOP_TIMEOUT);
        sleep(1);
        retVal = sl_Start(0, 0, 0);
        if(ROLE_AP != retVal)
        {
            g_fDebugPrint("SlWifiConn: Failed to enter AP mode (%d)\n\r", retVal);
            return -1;
        }
    }

    /* Delete profiles while in AP mode */
    sl_WlanProfileDel(SL_WLAN_DEL_ALL_PROFILES);
    g_fDebugPrint("SlWifiConn: Profiles deleted in AP mode\n\r");

    /* Set connection policy to Auto */
    sl_WlanPolicySet(SL_WLAN_POLICY_CONNECTION,
                     SL_WLAN_CONNECTION_POLICY(1, 0, 0, 0), NULL, 0);

    /* Stop NWP — the normal init flow will restart it in STA mode */
    sl_Stop(SL_STOP_TIMEOUT);
    sleep(1);

    return 0;
}

//*****************************************************************************
//
//! \brief  Sets WLAN Role (+ NWP reset), stores the active role
//!
//*****************************************************************************
static int SetRole(SlWlanMode_e role)
{
    int retVal = 0;
    if(role != g_ctx->role)
    {
        /* Enter here upon sl_Start error (typically this will be the ALREADY_STARTED code),
         * or if current role needs to be updated
         */
        sl_WlanSetMode(role);
        g_ctx->role = ResetNWP();
        assert(role == g_ctx->role);
    }
    return retVal;
}

//*****************************************************************************
//
//! \brief  free module's resources (including Critical Section, but not the
//!         thread context).
//!
//*****************************************************************************
int Deinit()
{
    sl_UnregisterLibsEventHandler(SL_EVENT_HDL_WLAN, &SlEventHandlers[0]);
    sl_UnregisterLibsEventHandler(SL_EVENT_HDL_NETAPP, &SlEventHandlers[1]);
    sl_UnregisterLibsEventHandler(SL_EVENT_HDL_DEVICE_GENERAL, &SlEventHandlers[2]);

    mq_close(g_ctx->DeferredEventQ);
    mq_close(g_ctx->eventQ);

    timer_delete(g_ctx->timer);

    if(g_ctx->pNonPersistentProfile)
    {
        free(g_ctx->pNonPersistentProfile);
    }
    sl_Stop(SL_STOP_TIMEOUT);

    free(g_ctx);
    g_ctx = NULL;

    CRITICAL_SECTION_EXIT();
    CRITICAL_SECTION_DESTROY();

    return 0;
}

//*****************************************************************************
//
//! \brief  Suspend the NWP Auto-Connect functionality (entering IDLE or OFF
//!         states)
//!
//*****************************************************************************
static int SuspendNWP(bool bNotify)
{
    int retVal;
    uint32_t flags = NON_PERSISTANT_FLAG;
    retVal = sl_WlanPolicySet(SL_WLAN_POLICY_CONNECTION, g_ctx->disabledPolicy, (uint8_t*)&flags, sizeof(uint32_t));
     if(retVal == 0 && bNotify)
     {
         NotifyWifiApp(WifiConnEvent_POWERED_DOWN, 0);
     }
     return retVal;
}

//*****************************************************************************
//
//! \brief  Resume the NWP Auto-Connect functionality (getting out of IDLE or
//!         OFF states)
//!
//*****************************************************************************
static int ResumeNWP(bool bNotify)
{
    int retVal;
    uint32_t flags = NON_PERSISTANT_FLAG;

    retVal = sl_WlanPolicySet(SL_WLAN_POLICY_CONNECTION, g_ctx->enabledPolicy, (uint8_t*)&flags, sizeof(uint32_t));
    if(retVal >= 0 && bNotify)
    {
        NotifyWifiApp(WifiConnEvent_POWERED_UP, 0);
    }
    return retVal;
}

//*****************************************************************************
//
//! \brief  SM main event processing handler
//!
//! \param  event - incoming SlWifiConn event
//!
//! \return 0 - if the event was successfully handled in the handler,
//!             or negative number in case of an error or unsupported event.
//!
//*****************************************************************************
static int ProcessEvent(SLWIFICONN_EVENT_e event)
{
    int retVal = 0;

    g_fDebugPrint("SlWifiConn(SM): %s / %s \n\r", strState[g_ctx->state], strEvent[event]);
    /* Find Next event entry */
    SMHandler_f pEntry = g_fWifiSM[g_ctx->state];

    assert (NULL != pEntry);
    retVal = pEntry(event);
    if (retVal < 0)
    {
        // Global Event Handling
        if(event == SLWIFICONN_EVENT_DISABLE)
        {
            SuspendNWP(true);
            if(g_ctx->state == SLWIFICONN_STATE_WAIT_FOR_IP ||
               g_ctx->state == SLWIFICONN_STATE_CONNECTED)
            {
                sl_WlanDisconnect();
                NotifyNetIf(SLNETIF_DISCONNECT, 0);
            }
            SetState(SLWIFICONN_STATE_OFF);
        }
        else
        {

            g_fDebugPrint("SlWifiConn(SM): [WARN] Event not supported (%d) !\n\r", retVal);
            retVal = -1;
        }
    }
    return retVal;
}

//*****************************************************************************
//
//! \brief  Send event to the SlWiConn State Machine
//!
//*****************************************************************************
int  SignalEvent(SLWIFICONN_EVENT_e event)
{
    //signal provisioning task about SL Event
    VERIFY_INITIALIZED();
    if((uint32_t)event < (uint32_t)SLWIFICONN_EVENT_MAX)
    {
        return mq_send(g_ctx->eventQ, (char *)&event, 1, 0);
    }
    else
    {
        g_fDebugPrint("SlWifiConn(SM): [ERROR] Invalid Event (%d) !!!!!!!!!\n\r");
        return SLNETERR_BSD_EINVAL;
    }
}


//*****************************************************************************
//
//! \brief  Set the SlWiFiConn SM state
//!
//*****************************************************************************
static int SetState(SLWIFICONN_STATE_e state)
{
    int retVal = 0;
    if((uint32_t)state < (uint32_t)SLWIFICONN_STATE_MAX)
    {
        g_fDebugPrint("SlWifiConn(SM): ==> %s \n\r", strState[state]);
        g_ctx->state = state;
    }
    else
    {
        g_fDebugPrint("SlWifiConn(SM): [ERROR] Invalid state (%d) !!!!!!!!!\n\r", state);
        retVal = -1;
    }
    return retVal;
}

//*****************************************************************************
//
//! \brief  Notify the NetIf on connection status changes, supported events are:
//!          SLNETIF_DISCONNECT, SLNETIF_MAC_CONNECT, SLNETIF_IP_AQUIRED
//!
//! \note   Must be called within a Critical Section (lock is released just
//!         before the event is issued)
//!
//*****************************************************************************
static int NotifyNetIf(SlNetIfEventId_e EventId, void *pData)
{
    if(g_fNetIfCB)
    {
        CRITICAL_SECTION_EXIT();
        g_fNetIfCB(EventId, g_ifId, pData);
        CRITICAL_SECTION_ENTER();
    }
    return 0;
}

//*****************************************************************************
//
//! \brief  Notify the Wifi App on status changes, supported events are:
//!          WifiConnEvent_POWERED_UP,
//!          WifiConnEvent_POWERED_DOWN,
//!          WifiConnEVENT_PROV_STARTED,
//!          WifiConnEVENT_PROV_STOPPED
//!
//! \note   Must be called within a Critical Section (lock is released just
//!         before the event is issued)
//!
//*****************************************************************************
static int NotifyWifiApp(WifiConnEventId_e EventId, WifiConnEventData_u *pData)
{
    if(g_ctx->fWifiAppCB)
    {
        CRITICAL_SECTION_EXIT();
        g_ctx->fWifiAppCB(EventId, pData);
        CRITICAL_SECTION_ENTER();
    }
    return 0;
}

//*****************************************************************************
//
//! \brief  Upon provisioning stop, notify the completion status:
//!         SUCCEED, FAILED or STOPPED (due to external event)
//!
//*****************************************************************************
static int NotifyProvisioningStatus(WifiConnProvStatus_e status)
{
    WifiConnEventData_u data;
    data.status = status;
    NotifyWifiApp(WifiConnEvent_PROVISIONING_STOPPED, &data);
    return 0;
}

// Init callback info
void InitCallback(_u32 Status, SlDeviceInitInfo_t *DeviceInitInfo)
{

}

//*****************************************************************************
//
//! \brief Set AP Role security parameters (before provisioning)
//!
//! \param  None
//!
//! \return None
//!
//*****************************************************************************
static int SetApParams()
{
    uint16_t len = 1;

    if(g_settings.provisioningAP_ssid)
    {
        len = strlen((char *)g_settings.provisioningAP_ssid);
        sl_WlanSet(SL_WLAN_CFG_AP_ID, SL_WLAN_AP_OPT_SSID, len,
                   (uint8_t *)g_settings.provisioningAP_ssid);
    }
    sl_WlanSet(SL_WLAN_CFG_AP_ID, SL_WLAN_AP_OPT_SECURITY_TYPE, 1,
               &g_settings.provisioningAP_secType);

    if(g_settings.provisioningAP_secType != SL_WLAN_SEC_TYPE_OPEN)
    {
        len = strlen((char *)g_settings.provisioningAP_password);
        sl_WlanSet(SL_WLAN_CFG_AP_ID, SL_WLAN_AP_OPT_PASSWORD, len,
                   (uint8_t *)g_settings.provisioningAP_password);
        g_fDebugPrint(" Setting AP secured parameters\n\r");
    }

    if (ROLE_AP == g_ctx->role)
    {
        /* Reset NWP to start secured AP */
        g_ctx->role = ResetNWP();
    }
    return(0);
}



//*****************************************************************************
//
//! \brief  Enter "IDLE" State - can be called from various SM
//!         transitions.
//!
//! \param  None
//!
//! \return None
//!
//*****************************************************************************
static void EnterIDLE()
{
    SuspendNWP(true);
    StartAsyncEvtTimer(g_ctx->idleInterval);
    g_ctx->idleInterval *= 2;
    if(g_ctx->idleInterval > g_settings.idleMaxInterval)
    {
        g_ctx->idleInterval = g_settings.idleMaxInterval;
    }
    SetState(SLWIFICONN_STATE_IDLE);
}

//*****************************************************************************
//
//! \brief  Enter "WAIT_FOR_CONN" State - can be called from various SM
//!         transitions.
//!
//! \param  None
//!
//! \return None
//!
//*****************************************************************************
static void EnterWAIT_FOR_CONN(uint16_t timeout, bool bWlanDisconnect)
{
    DeferredEvent_process();
    NonPersistentProfile_process(bWlanDisconnect);
    StartAsyncEvtTimer(timeout);
    SetState(SLWIFICONN_STATE_WAIT_FOR_CONN);
}

//*****************************************************************************
//
//! \brief  Enter "WAIT_FOR_IP" State - can be called from various SM
//!         transitions.
//!
//! \param  None
//!
//! \return None
//!
//*****************************************************************************
static void EnterWAIT_FOR_IP()
{
    int retVal;
    uint16_t len , ConfigOpt;
    SlNetCfgIpV4Args_t ipV4;

    NotifyNetIf(SLNETIF_MAC_CONNECT, 0);

    len = sizeof(SlNetCfgIpV4Args_t);
    ConfigOpt = 0;
    retVal = sl_NetCfgGet(SL_NETCFG_IPV4_STA_ADDR_MODE, &ConfigOpt, &len, (uint8_t *)&ipV4);
    if(retVal == 0 && ConfigOpt == SL_NETCFG_ADDR_STATIC && ipV4.Ip == 0)
    {
        /* in case of static IP == 0 (bypass mode),
         * The IP ACQUIRED event will not be received.
         * Switch directly to the steady CONNECTED state
         * (without IP ACQUIRED Indication).
         */
        EnterCONNECTED(false);
    }
    else
    {
        StartAsyncEvtTimer(g_settings.ipTimeout);
        SetState(SLWIFICONN_STATE_WAIT_FOR_IP);
    }
}

//*****************************************************************************
//
//! \brief  Enter "CONNECTED" State - can be called when ip is acquired or when
//!                 provisioing succeed.
//!
//! \param  None
//!
//! \return None
//!
//*****************************************************************************
static void EnterCONNECTED(bool bNotify)
{
    StopAsyncEvtTimer();

    /* Reset error tracking on successful connection.
     * This allows the backoff to reset after stability is restored.
     */
    if(g_ctx->fatalErrorCount > 0)
    {
        g_fDebugPrint("SlWifiConn: Connection restored after %d error(s)\n\r",
                      g_ctx->fatalErrorCount);
        g_ctx->fatalErrorCount = 0;
        g_ctx->recoveryDelayLevel = 0;
    }

    if(bNotify)
    {
        NotifyNetIf(SLNETIF_IP_AQUIRED, 0);
    }
    g_ctx->idleInterval = g_settings.idleMinInterval;
#ifdef WIFI_STATS
    /* Start async timer for statistics if enabled */
    if(g_ctx->bStatsEnabled && g_ctx->statsInterval > 0)
    {
        StartAsyncEvtTimer(g_ctx->statsInterval);
    }
#endif
    SetState(SLWIFICONN_STATE_CONNECTED);
}

//*****************************************************************************
//
//! \brief  Enter "PROVISIONING" State
//!
//! \param  None
//!
//! \return None
//!
//*****************************************************************************
static int EnterPROVISIONING(void)
{
    int  retVal;
    uint8_t  provisioningCmd = g_ctx->provCmd;

    if(g_ctx->provCmd == SL_WLAN_PROVISIONING_CMD_START_MODE_EXTERNAL_CONFIGURATION)
    {
        /* Enable External Provisioning method */
        NotifyWifiApp(WifiConnEvent_EXTERNAL_PROVISIONING_START_REQ, 0);
        SetState(SLWIFICONN_STATE_EXT_PROVISIONING);
    }
    else
    {
        retVal = SetApParams();

        if(retVal == 0)
        {
            /* start NWP provisioning */
            retVal = sl_WlanProvisioning(provisioningCmd, ROLE_STA,
                                    g_settings.provisioningTimeout,
                                    (char *)g_settings.provisioningSC_key,
                                    (uint32_t)g_ctx->provFlags);
            assert(retVal == 0);
            SetState(SLWIFICONN_STATE_PROVISIONING);
            if(g_ctx->provMode == WifiProvMode_ONE_SHOT)
            {
                g_ctx->provMode = WifiProvMode_OFF;
            }
        }
    }
    /* Watchdog timer is needed even in the the case NWP provisioning due to a bug
     * (once it is fixed and internal timeout works - the external timer can be
     * used only in the external Configuration cases)
     */
    StartAsyncEvtTimer(g_settings.provisioningTimeout);
    return retVal;
}

//*****************************************************************************
//
//! \brief  Enter "PROVISIONING" State
//!
//! \param  None
//!
//! \return None
//!
//*****************************************************************************
static int EnterPROV_STOPPING(SLWIFICONN_EVENT_e eventId, void *pParams)
{
    int retVal = 0;

    StopAsyncEvtTimer();
    if(eventId < SLWIFICONN_EVENT_MAX)
        retVal = DeferredEvent_enqueue(eventId, pParams);
    assert(retVal == 0);

    retVal = sl_WlanProvisioning(SL_WLAN_PROVISIONING_CMD_STOP,0, 0, NULL, 0);
    SetState(SLWIFICONN_STATE_PROV_STOPPING);

    return retVal;
}

//*****************************************************************************
//
//! \brief  Handler for the SlWifiConn "OFF" State
//!
//! \param  event - incoming SlWifiConn event
//!
//! \return 0 - if the event was successfully handled in the handler,
//!             or negative number in case of an error or unsupported event.
//!
//*****************************************************************************
static int smState_OFF(SLWIFICONN_EVENT_e event)
{
    int retVal = 0;
    switch (event)
    {
    case SLWIFICONN_EVENT_ENABLE:
        g_ctx->idleInterval = g_settings.idleMinInterval;
        if(g_ctx->bDeleteAllProfiles )
        {
            /* Delete all profile to force provisioning
             * (typically, this would be used in development mode only
             */
            retVal = sl_WlanProfileDel(SL_WLAN_DEL_ALL_PROFILES);
            assert(retVal == 0);
            g_fDebugPrint("SlWifiConn(SM): DELETE ALL PROFILES !!!!!!!!!\n\r");

            /* Do this only once per run */
            g_ctx->bDeleteAllProfiles = false;
        }
        ResumeNWP(true);
        EnterWAIT_FOR_CONN(g_settings.connectTimeout, false);
        break;

    case SLWIFICONN_EVENT_DISABLE:
    case SLWIFICONN_EVENT_ENABLE_PROVISIONING:
    case SLWIFICONN_EVENT_ADD_PROFILE:
    case SLWIFICONN_EVENT_DISCONNECT:
    case SLWIFICONN_EVENT_CONNECTED: /* this is an "obsolete" event received before the was disabled */
        // Do Nothing
        break;

    default:
        retVal = -1;
    }
    return retVal;
}


//*****************************************************************************
//
//! \brief  Handler for the SlWifiConn "IDLE" State
//!
//! \param  event - incoming SlWifiConn event
//!
//! \return 0 - if the event was successfully handled in the handler,
//!             or negative number in case of an error or unsupported event.
//!
//*****************************************************************************
static int smState_IDLE(SLWIFICONN_EVENT_e event)
{
    int retVal = 0;
    switch (event)
    {
    case SLWIFICONN_EVENT_ENABLE_PROVISIONING:
    case SLWIFICONN_EVENT_ADD_PROFILE:
    case SLWIFICONN_EVENT_TIMEOUT:
        ResumeNWP(true);
        EnterWAIT_FOR_CONN(g_settings.connectTimeout, false);
        break;

        /* in case CONNECTED will follow the connection timeout (before the NWP is suspended)
         * Resume NWP and Move to WAIT_FOR_IP
         */
    case SLWIFICONN_EVENT_CONNECTED:
        StopAsyncEvtTimer();
        StartAsyncEvtTimer(g_settings.ipTimeout);
        SetState(SLWIFICONN_STATE_WAIT_FOR_IP);
        break;
    default:
        retVal = -1;
    }
    return retVal;
}

//*****************************************************************************
//
//! \brief  Handler for the SlWifiConn "WAIT FOR CONN" State
//!
//! \param  event - incoming SlWifiConn event
//!
//! \return 0 - if the event was successfully handled in the handler,
//!             or negative number in case of an error or unsupported event.
//!
//*****************************************************************************
static int smState_WAIT_FOR_CONN(SLWIFICONN_EVENT_e event)
{
    int retVal = 0;

    switch (event)
    {
    case SLWIFICONN_EVENT_ADD_PROFILE:
        /* in case a profile was added - restart the connection watchdog */
        EnterWAIT_FOR_CONN(g_settings.connectTimeout, false);
    break;

    case SLWIFICONN_EVENT_CONNECTED:
        EnterWAIT_FOR_IP();
        break;

    case SLWIFICONN_EVENT_IP_ACQUIRED:
        EnterCONNECTED(true);
        break;

    case SLWIFICONN_EVENT_TIMEOUT:
        if(g_ctx->bWlanConnectUsed)
        {
            /* if Non Persistent profile was used, disconnect and wait for
             * a connection based on a stored profile
             */
            g_fDebugPrint("Remove non-persistent profile and try again\n\r");
            EnterWAIT_FOR_CONN(g_settings.connectTimeout, true);
        }
        else if(g_ctx->provMode != WifiProvMode_OFF)
        {
            retVal = g_fEnterPROVISIONING();
            if(retVal == 0)
            {
                WifiConnEventData_u data;
                data.provisioningCmd = g_ctx->provCmd;
                NotifyWifiApp(WifiConnEvent_PROVISIONING_STARTED, &data);
            }
        }
        else
        {
            EnterIDLE();
        }
        break;
    default:
        retVal = -1;
    }
    return retVal;
}

//*****************************************************************************
//
//! \brief  Handler for the SlWifiConn "WAIT FOR IP" State
//!
//! \param  event - incoming SlWifiConn event
//!
//! \return 0 - if the event was successfully handled in the handler,
//!             or negative number in case of an error or unsupported event.
//!
//*****************************************************************************
static int smState_WAIT_FOR_IP(SLWIFICONN_EVENT_e event)
{
    int retVal = 0;
    switch (event)
    {
    case SLWIFICONN_EVENT_ADD_PROFILE:
        break;
    case SLWIFICONN_EVENT_IP_ACQUIRED:
        EnterCONNECTED(true);
        break;
    case SLWIFICONN_EVENT_TIMEOUT:
        sl_WlanDisconnect();
    case SLWIFICONN_EVENT_DISCONNECT:
        NotifyNetIf(SLNETIF_DISCONNECT, 0);
        EnterWAIT_FOR_CONN(g_settings.reconnectTimeout, false);
        break;
    default:
        retVal = -1;
    }
    return retVal;
}


//*****************************************************************************
//
//! \brief  Handler for the SlWifiConn "CONNECTED" State
//!
//! \param  event - incoming SlWifiConn event
//!
//! \return 0 - if the event was successfully handled in the handler,
//!             or negative number in case of an error or unsupported event.
//!
//*****************************************************************************
static int smState_CONNECTED(SLWIFICONN_EVENT_e event)
{
    int retVal = 0;
    switch (event)
    {
    case SLWIFICONN_EVENT_IP_ACQUIRED:
    case SLWIFICONN_EVENT_ADD_PROFILE:
        break;
    case SLWIFICONN_EVENT_DISCONNECT:
#ifdef WIFI_STATS
        StopAsyncEvtTimer();
#endif
        NotifyNetIf(SLNETIF_DISCONNECT, 0);
        EnterWAIT_FOR_CONN(g_settings.reconnectTimeout, false);
        break;
    case SLWIFICONN_EVENT_IP_LOST:
#ifdef WIFI_STATS
        StopAsyncEvtTimer();
#endif
        StartAsyncEvtTimer(g_settings.ipTimeout);
        SetState(SLWIFICONN_STATE_WAIT_FOR_IP);
        break;
    case SLWIFICONN_EVENT_RESET_REQUEST:
#ifdef WIFI_STATS
        StopAsyncEvtTimer();
#endif
        NotifyNetIf(SLNETIF_DISCONNECT, 0);
        retVal = ResetNWP();
        assert(retVal == 0 || retVal == SL_RET_CODE_PROVISIONING_IN_PROGRESS);
        /* Use EnterWAIT_FOR_CONN instead of SetState to properly initialize
         * the state including starting the connection timeout timer.
         * This ensures the state machine can handle connection failures.
         */
        EnterWAIT_FOR_CONN(g_settings.reconnectTimeout, false);
        break;
#ifdef WIFI_STATS
    case SLWIFICONN_EVENT_TIMEOUT:
        if(g_ctx->bStatsEnabled)
        {
            CollectAndNotifyStats();
        }
        break;
#endif
    default:
        retVal = -1;
    }
    return retVal;
}

//*****************************************************************************
//
//! \brief  Handler for the SlWifiConn "PROVISIONING" State
//!
//! \param  event - incoming SlWifiConn event
//!
//! \return 0 - if the event was successfully handled in the handler,
//!             or negative number in case of an error or unsupported event.
//!
//*****************************************************************************
static int smState_PROVISIONING(SLWIFICONN_EVENT_e event)
{
    int retVal = 0;

    switch (event)
    {
    case SLWIFICONN_EVENT_PROV_SUCCESS:
        StopAsyncEvtTimer();
        SetState(SLWIFICONN_STATE_PROV_STOPPING);
        break;

    case SLWIFICONN_EVENT_DISABLE:
        /* Disbale require sending SL command which is blocked during provisioning,
         * The event will be deferred and process once provisioning is stopped
         */
        EnterPROV_STOPPING(SLWIFICONN_EVENT_DISABLE, NULL);
        break;

    case SLWIFICONN_EVENT_ADD_PROFILE:
        EnterPROV_STOPPING(SLWIFICONN_EVENT_MAX, NULL);
        break;

    case SLWIFICONN_EVENT_TIMEOUT:
        /* Typically prov timeout will be received as PROV_STOPPED, but due to a bug in
         * the activity timeout handling, the timeout can be received from a the SM timer
         * (as a timeout event) */
        EnterPROV_STOPPING(SLWIFICONN_EVENT_TIMEOUT, NULL);
        break;

    case SLWIFICONN_EVENT_ERROR:
        /* Provisioning ERROR events - Stop and Retry */
        /* We need to enable provisioning at least for one shot to complete this */
        if(g_ctx->provMode == WifiProvMode_OFF)
        {
            g_ctx->provMode = WifiProvMode_ONE_SHOT;
        }
        EnterPROV_STOPPING(SLWIFICONN_EVENT_ERROR, NULL);
        break;

    case SLWIFICONN_EVENT_PROV_STOPPED:
        StopAsyncEvtTimer();
        NotifyProvisioningStatus(WifiProvStatus_FAILED);
        EnterIDLE();
        break;


    case SLWIFICONN_EVENT_PROV_EXT_CONFIG_READY:
        NotifyWifiApp(WifiConnEvent_EXTERNAL_PROVISIONING_START_REQ, 0);
        SetState(SLWIFICONN_STATE_EXT_PROVISIONING);
        break;

    case SLWIFICONN_EVENT_PROV_CLOUD_CONFIRM_READY:
        NotifyWifiApp(WifiConnEvent_CLOUD_CONFIRMATION_START_REQ, 0);
        SetState(SLWIFICONN_STATE_EXT_PROVISIONING);
        break;


    default:
        retVal = -1;
    }
    return retVal;
}


//*****************************************************************************
//
//! \brief  Handler for the SlWifiConn "EXT_PROVISIONING" State
//!
//! \param  event - incoming SlWifiConn event
//!
//! \return 0 - if the event was successfully handled in the handler,
//!             or negative number in case of an error or unsupported event.
//!
//*****************************************************************************
static int smState_EXT_PROVISIONING(SLWIFICONN_EVENT_e event)
{
    int retVal = 0;

     switch (event)
     {
     case SLWIFICONN_EVENT_EXT_PROV_COMPLETED:
     case SLWIFICONN_EVENT_ADD_PROFILE:
         EnterPROV_STOPPING(SLWIFICONN_EVENT_EXT_PROV_COMPLETED, NULL);
        break;

     case SLWIFICONN_EVENT_TIMEOUT:
         if(g_ctx->provFlags == SL_WLAN_PROVISIONING_CMD_FLAG_EXTERNAL_CONFIRMATION)
         {
             NotifyWifiApp(WifiConnEvent_CLOUD_CONFIRMATION_STOP_REQ, 0);
         }
         else
         {
             NotifyWifiApp(WifiConnEvent_EXTERNAL_PROVISIONING_STOP_REQ, 0);
         }
         NotifyProvisioningStatus(WifiProvStatus_FAILED);
         EnterPROV_STOPPING(SLWIFICONN_EVENT_TIMEOUT, NULL);
         break;

     case SLWIFICONN_EVENT_RESET_REQUEST:
         /* Disable external provisioning, reset NWP - back to provisioning state */
         NotifyWifiApp(WifiConnEvent_EXTERNAL_PROVISIONING_STOP_REQ, 0);
         retVal = ResetNWP();
         assert(retVal == 0 || retVal == SL_RET_CODE_PROVISIONING_IN_PROGRESS);
         SetState(SLWIFICONN_STATE_PROVISIONING);
         break;
     default:
         retVal = -1;
     }
     return retVal;

}




//*****************************************************************************
//
//! \brief  Handler for the SlWifiConn "PROV_STOPPING" State
//!
//! \param  event - incoming SlWifiConn event
//!
//! \return 0 - if the event was successfully handled in the handler,
//!             or negative number in case of an error or unsupported event.
//!
//*****************************************************************************
static int smState_PROV_STOPPING(SLWIFICONN_EVENT_e event)
{
    int retVal = 0;

    switch (event)
    {
    case SLWIFICONN_EVENT_CONNECTED:
        NotifyProvisioningStatus(WifiProvStatus_SUCCEED);
        NotifyNetIf(SLNETIF_MAC_CONNECT, 0);
        EnterCONNECTED(true);
        break;

    case SLWIFICONN_EVENT_PROV_STOPPED:
        NotifyProvisioningStatus(WifiProvStatus_STOPPED);
        if(DeferredEvent_process() == SL_RET_OBJ_NOT_SET)
        {
            EnterWAIT_FOR_CONN(g_settings.connectTimeout, false);
        }
        /* Stay here - wait for deferred event(s) - see handlers below */
        break;

    case SLWIFICONN_EVENT_EXT_PROV_COMPLETED:
        SetRole(ROLE_STA);
    case SLWIFICONN_EVENT_ADD_PROFILE:
        EnterWAIT_FOR_CONN(g_settings.connectTimeout, false);
        break;

    case SLWIFICONN_EVENT_ERROR:
        /* provisioning error occured - restart provisioning */
        EnterPROVISIONING();
        break;

    case SLWIFICONN_EVENT_TIMEOUT:
        /* provisioning error occured - restart provisioning */
        EnterIDLE();
        break;

    default:
        retVal = -1;
    }
    return retVal;
}



/*****************************************************************************
                  SimpleLink Callback Functions
*****************************************************************************/
void SLProvisioningStatusHandler(SlWlanEventProvisioningStatus_t *pProvStatus)
{
    switch(pProvStatus->ProvisioningStatus)
    {
    case SL_WLAN_PROVISIONING_GENERAL_ERROR:
    case SL_WLAN_PROVISIONING_ERROR_ABORT:
    case SL_WLAN_PROVISIONING_ERROR_ABORT_INVALID_PARAM:
    case SL_WLAN_PROVISIONING_ERROR_ABORT_HTTP_SERVER_DISABLED:
    case SL_WLAN_PROVISIONING_ERROR_ABORT_PROFILE_LIST_FULL:
    case SL_WLAN_PROVISIONING_ERROR_ABORT_PROVISIONING_ALREADY_STARTED:
        /* Provisioning error - need to restart */
        SignalEvent(SLWIFICONN_EVENT_ERROR);
        break;

    case SL_WLAN_PROVISIONING_CONFIRMATION_STATUS_FAIL_NETWORK_NOT_FOUND:
    case SL_WLAN_PROVISIONING_CONFIRMATION_STATUS_FAIL_CONNECTION_FAILED:
    case SL_WLAN_PROVISIONING_CONFIRMATION_STATUS_CONNECTION_SUCCESS_IP_NOT_ACQUIRED:
    case SL_WLAN_PROVISIONING_CONFIRMATION_STATUS_SUCCESS_FEEDBACK_FAILED:
        /* Nothing to do - provisioning continues */
        break;


    case SL_WLAN_PROVISIONING_CONFIRMATION_STATUS_SUCCESS:
        SignalEvent(SLWIFICONN_EVENT_PROV_SUCCESS);
        break;

    case SL_WLAN_PROVISIONING_CONFIRMATION_IP_ACQUIRED:
        if(g_ctx->provFlags == SL_WLAN_PROVISIONING_CMD_FLAG_EXTERNAL_CONFIRMATION)
        {
            SignalEvent(SLWIFICONN_EVENT_PROV_CLOUD_CONFIRM_READY);
        }
    break;

    case SL_WLAN_PROVISIONING_AUTO_STARTED:
        break;

    case SL_WLAN_PROVISIONING_STOPPED:
        if(ROLE_STA == pProvStatus->Role)
        {
            if(SL_WLAN_STATUS_CONNECTED ==
                    pProvStatus->WlanStatus)
            {
                SignalEvent(SLWIFICONN_EVENT_CONNECTED);
            }
            else
                SignalEvent(SLWIFICONN_EVENT_PROV_STOPPED);
        }
        else
        {
            SignalEvent(SLWIFICONN_EVENT_PROV_STOPPED);
        }
        break;

    case SL_WLAN_PROVISIONING_EXTERNAL_CONFIGURATION_READY:
        /* [External configuration]: External configuration is ready,
            start the external configuration process.
            In case of using the external provisioning
            enable the function below which will trigger StartExternalProvisioning() */
        SignalEvent(SLWIFICONN_EVENT_PROV_EXT_CONFIG_READY);
        break;

    default:
        break;
    }
}

//*****************************************************************************
//
//! \brief The Function Handles WLAN Events
//!
//! \param[in]  pWlanEvent - Pointer to WLAN Event Info
//!
//! \return None
//!
//*****************************************************************************
_SlEventPropogationStatus_e SLWlanEventHandler(SlWlanEvent_t *pWlanEvent)
{
    switch(pWlanEvent->Id)
    {
    case SL_WLAN_EVENT_CONNECT:
        memcpy(g_ctx->connectionBSSID, pWlanEvent->Data.Connect.Bssid, 6);
        memcpy(g_ctx->connectionSSID, pWlanEvent->Data.Connect.SsidName, pWlanEvent->Data.Connect.SsidLen);
        g_ctx->connectionSSID[pWlanEvent->Data.Connect.SsidLen] = 0;
        SignalEvent(SLWIFICONN_EVENT_CONNECTED);
        break;

    case SL_WLAN_EVENT_DISCONNECT:
        SignalEvent(SLWIFICONN_EVENT_DISCONNECT);
        break;

    case SL_WLAN_EVENT_PROVISIONING_STATUS:
    {
        if(g_fProvStatusHandler)
            g_fProvStatusHandler(&pWlanEvent->Data.ProvisioningStatus);
    }
    break;

    default:
        break;
    }
    return (EVENT_PROPAGATION_CONTINUE);
}



//*****************************************************************************
//
//! \brief This function handles network events such as IP acquisition, IP
//!           leased, IP released etc.
//!
//! \param[in]  pNetAppEvent - Pointer to NetApp Event Info
//!
//! \return None
//!
//*****************************************************************************
_SlEventPropogationStatus_e SLNetAppEventHandler(SlNetAppEvent_t *pNetAppEvent)
{
    if(g_ctx->role == ROLE_STA)
    {
        switch(pNetAppEvent->Id)
        {
        case SL_NETAPP_EVENT_IPV4_ACQUIRED:
            g_ctx->IpAcquiredV4 = pNetAppEvent->Data.IpAcquiredV4;
            SignalEvent(SLWIFICONN_EVENT_IP_ACQUIRED);
            break;

        case SL_NETAPP_EVENT_IPV6_ACQUIRED:
            g_ctx->IpAcquiredV6 = pNetAppEvent->Data.IpAcquiredV6;
            SignalEvent(SLWIFICONN_EVENT_IP_ACQUIRED);
            break;
        case SL_NETAPP_EVENT_IPV4_LOST:
        case SL_NETAPP_EVENT_NO_IPV4_COLLISION_DETECTED:
            memset(&g_ctx->IpAcquiredV4, 0, sizeof(g_ctx->IpAcquiredV4));
            SignalEvent(SLWIFICONN_EVENT_IP_LOST);
            break;

        case SL_NETAPP_EVENT_IPV6_LOST:
        case SL_NETAPP_EVENT_NO_GLOBAL_IPV6_COLLISION_DETECTED:
        case SL_NETAPP_EVENT_NO_LOCAL_IPV6_COLLISION_DETECTED:
            memset(&g_ctx->IpAcquiredV6, 0, sizeof(g_ctx->IpAcquiredV6));
            SignalEvent(SLWIFICONN_EVENT_IP_LOST);
            break;

        default:
            break;
        }
    }
    return (EVENT_PROPAGATION_CONTINUE);
}

//*****************************************************************************
//
//! \brief This function handles general NWP events such as reset request.
//!
//! \param[in]  pDevEvent - Pointer to General Event Info
//!
//! \return None
//!
//*****************************************************************************
_SlEventPropogationStatus_e SLGeneralEventHandler(SlDeviceEvent_t *pDevEvent)
{
    switch(pDevEvent->Id)
    {
    case SL_DEVICE_EVENT_RESET_REQUEST:
        SignalEvent(SLWIFICONN_EVENT_RESET_REQUEST);
        break;
    }
    return (EVENT_PROPAGATION_CONTINUE);
}


//*****************************************************************************
//                 Local Functions
//*****************************************************************************


//*****************************************************************************
//
//! \brief The interrupt handler for the async-evt timer
//!
//! \param  none
//!
//! \return none
//!
//*****************************************************************************
static void AsyncEvtTimerHandler(sigval arg)
{
    SignalEvent(SLWIFICONN_EVENT_TIMEOUT);
}

//*****************************************************************************
//
//! \brief Call Host Timer
//!
//! \param  Duration in mSec
//!
//! \return none
//!
//*****************************************************************************
static void StartAsyncEvtTimer(uint32_t duration)
{
    struct itimerspec value;

    /* Start timer */
    value.it_interval.tv_sec = 0;
    value.it_interval.tv_nsec = 0;
    value.it_value.tv_sec = duration;
    value.it_value.tv_nsec = 0;

    timer_settime(g_ctx->timer, 0, &value, NULL);
}

//*****************************************************************************
//
//! \brief Stop Timer
//!
//! \param  None
//!
//! \return none
//!
//*****************************************************************************
void StopAsyncEvtTimer(void)
{
    struct itimerspec value;

    /* Stop timer */
    value.it_interval.tv_sec = 0;
    value.it_interval.tv_nsec = 0;
    value.it_value.tv_sec = 0;
    value.it_value.tv_nsec = 0;

    timer_settime(g_ctx->timer, 0, &value, NULL);
}

#ifdef WIFI_STATS
//*****************************************************************************
//
//! \brief Collect WiFi statistics and notify via callback
//!
//! Uses pause/resume to temporarily stop network clients, then collects
//! accurate RSSI statistics using sl_DeviceStat* functions.
//!
//! \param  None
//!
//! \return 0 on success
//!
//*****************************************************************************
static int CollectAndNotifyStats(void)
{
    SlDeviceGetStat_t rxStats;
    _u16 len = sizeof(SlDeviceGetStat_t);
    int ret;

    // Only proceed if still in CONNECTED state
    if(g_ctx->state != SLWIFICONN_STATE_CONNECTED)
    {
        // Reschedule timer if stats still enabled
        if(g_ctx->bStatsEnabled)
        {
            StartAsyncEvtTimer(g_ctx->statsInterval);
        }
        return -1;
    }

    // Notify app to pause network operations
    NotifyWifiApp(WifiConnEvent_STATS_PAUSE, NULL);

    // Wait for pending network operations to complete
    // HTTP transactions can take several seconds, so we need a longer delay
    sleep(1);  // 1 second

    // Verify still connected after waiting
    if(g_ctx->state != SLWIFICONN_STATE_CONNECTED)
    {
        NotifyWifiApp(WifiConnEvent_STATS_RESUME, NULL);
        if(g_ctx->bStatsEnabled)
        {
            StartAsyncEvtTimer(g_ctx->statsInterval);
        }
        return -1;
    }

    // Start statistics collection
    ret = sl_DeviceStatStart(0);
    if(ret < 0)
    {
        NotifyWifiApp(WifiConnEvent_STATS_RESUME, NULL);
        if(g_ctx->bStatsEnabled)
        {
            StartAsyncEvtTimer(g_ctx->statsInterval);
        }
        return ret;
    }

    // Allow time for stats to accumulate
    sleep(5);  // 2 sec needed for valid RSSI readings

    // Get and stop statistics
    ret = sl_DeviceStatGet(SL_DEVICE_STAT_WLAN_RX, len, &rxStats);
    sl_DeviceStatStop(0);

    // Notify app to resume network operations
    NotifyWifiApp(WifiConnEvent_STATS_RESUME, NULL);

    // Verify we're still connected before sending results
    if(ret >= 0 && g_ctx->state == SLWIFICONN_STATE_CONNECTED)
    {
        // Send results via callback
        SlWifiConnStats_t stats;
        memcpy(stats.bssid, g_ctx->connectionBSSID, 6);
        stats.mgMntRssi = rxStats.AvarageMgMntRssi;
        stats.dataCtrlRssi = rxStats.AvarageDataCtrlRssi;

        NotifyWifiApp(WifiConnEvent_STATS_RESULT, (WifiConnEventData_u*)&stats);
    }

    // Schedule next collection if still enabled and connected
    if(g_ctx->bStatsEnabled && g_ctx->state == SLWIFICONN_STATE_CONNECTED)
    {
        StartAsyncEvtTimer(g_ctx->statsInterval);
    }
    return 0;
}
#endif /* WIFI_STATS */

//*****************************************************************************
//
// connMngTask - Connection manager thread, handles all connection requests
//
//*****************************************************************************
void* SlWifiConn_process(void* pvParameters)
{
    SLWIFICONN_EVENT_e event;
    while (!g_ctx)
    {
        sleep(1);
    }
    while(1)
    {
        mq_receive(g_ctx->eventQ, (char *)&event, 1, NULL);
        CRITICAL_SECTION_ENTER();
        if(event == SLWIFICONN_EVENT_DESTROY)
        {
            break;
        }
        ProcessEvent(event);
        CRITICAL_SECTION_EXIT();
    }
    /* Free all resources */
    Deinit();
    return NULL;
 }

//*****************************************************************************
//
//! \brief  allocate context and resources initialize NWP in station role
//!
//! \param  None
//!
//! \return None
//!
//*****************************************************************************
/* Static pthread attributes for timer thread - must persist after init returns */
static pthread_attr_t g_timerThreadAttr;

int SlWifiConn_init(SlWifiConn_AppEventCB_f fWifiAppCB, uint8_t deleteProfiles)
{
    mq_attr            attr;
    sigevent           sev;
    int                retVal;

    VERIFY_NOT_INITIALIZED();

    CRITICAL_SECTION_CREATE();
    CRITICAL_SECTION_ENTER();

     /* Allocate module context */
    g_ctx = (SlWifiConn_t*)calloc(1, sizeof(SlWifiConn_t));
    if (g_ctx == NULL)
    {
        CRITICAL_SECTION_EXIT();
        return SLNETERR_RET_CODE_MALLOC_ERROR;
    }

    g_ctx->fWifiAppCB = fWifiAppCB;

    /* Create message queue */
    attr.mq_curmsgs = 0;
    attr.mq_flags = 0;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = sizeof( unsigned char );
    g_ctx->eventQ = mq_open("SMQueue", O_CREAT, 0, &attr);

    /* Create pending Cmd queue */
    attr.mq_maxmsg = 3;
    attr.mq_msgsize = sizeof( DeferredEvent_t );
    g_ctx->DeferredEventQ = mq_open("ProfileQueue", O_CREAT, 0, &attr);

    /* Create Timer Thread - use static pthread_attr that persists */
    pthread_attr_init(&g_timerThreadAttr);
    pthread_attr_setstacksize(&g_timerThreadAttr, TIMER_TASK_STACK_SIZE);
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = &AsyncEvtTimerHandler;
    sev.sigev_notify_attributes = &g_timerThreadAttr;
    timer_create(CLOCK_MONOTONIC, &sev, &g_ctx->timer);

#ifdef WIFI_STATS
    /* Initialize stats state - uses existing async timer, no separate timer needed */
    g_ctx->bStatsEnabled = false;
    g_ctx->statsInterval = 10;  /* Default 10 seconds */
#endif

    /* Register SimpleLink Event Handlers */
    SlEventHandlers[0].event = (void *)SLWlanEventHandler;
    SlEventHandlers[1].event = (void *)SLNetAppEventHandler;
    SlEventHandlers[2].event = (void *)SLGeneralEventHandler;

    sl_RegisterLibsEventHandler(SL_EVENT_HDL_WLAN, &SlEventHandlers[0]);
    sl_RegisterLibsEventHandler(SL_EVENT_HDL_NETAPP, &SlEventHandlers[1]);
    sl_RegisterLibsEventHandler(SL_EVENT_HDL_DEVICE_GENERAL, &SlEventHandlers[2]);

    /* Initiate all the non-provisioning states */
    g_fWifiSM[SLWIFICONN_STATE_OFF] = smState_OFF;
    g_fWifiSM[SLWIFICONN_STATE_IDLE] = smState_IDLE;
    g_fWifiSM[SLWIFICONN_STATE_WAIT_FOR_CONN] = smState_WAIT_FOR_CONN;
    g_fWifiSM[SLWIFICONN_STATE_WAIT_FOR_IP] = smState_WAIT_FOR_IP;
    g_fWifiSM[SLWIFICONN_STATE_CONNECTED] = smState_CONNECTED;

     g_ctx->state = SLWIFICONN_STATE_OFF;
     /* starting the device in station role */
     retVal = sl_Start(0, 0, 0);
     /* Based on sl_Start return code, decide on role and state */
     if(retVal == SL_RET_CODE_PROVISIONING_IN_PROGRESS)
     {
         /* SlWifiConn  doesn't handle MCU reset during provisiong,
          * it will stop the provisioning in such case and wait for
          * SlWifiConn_enable
          */
         retVal = sl_WlanProvisioning(SL_WLAN_PROVISIONING_CMD_STOP,0, 0, NULL, 0);
         if(retVal == 0)
         {
             retVal = InitNWPConnPolicy();
         }
         g_ctx->role = ROLE_STA;
     }
     else
     {
         g_ctx->role = retVal;
          /* RetVal >= 0 -> Make sure it is ROLE_STA */
         retVal = InitNWPConnPolicy();
         if(retVal == 0)
         {
             if(g_ctx->role != ROLE_STA)
             {
                 SetRole(ROLE_STA);
             }
             retVal = 0;
         }
     }

     /* Reset NWP to AP mode, delete all profiles, then stop.
      * Restart in STA mode for the profile check and normal operation. */
     if(deleteProfiles) {
         ResetNWP2AP();
         sl_Start(0, 0, 0);
     }

     /* Check for stored WLAN profiles before state machine starts.
      * This is safe here since NWP is initialized but SM hasn't started yet.
      */
     {
         uint8_t id;
         int16_t status;
         int16_t nmlen;
         uint8_t mac[6];
         _u32 priority;
         SlWlanSecParams_t secParams;
         SlWlanGetSecParamsExt_t extParams;
         char ssidName[33];

         g_profileCount = 0;
         for(id = 0; id < 8; id++) {
             status = sl_WlanProfileGet(id, (int8_t *)ssidName, &nmlen, mac,
                                        &secParams, &extParams, &priority);
             if(status >= 0) {
                 g_profileCount++;
             }
         }
         g_fDebugPrint("SlWifiConn: Found %d stored profile(s)\n\r", g_profileCount);
     }

     /* Set power policy to always-on for maximum stability.
      * Power management transitions can cause SPI timing issues with the NWP
      * which lead to SYNC_LOSS and DRIVER_ABORT fatal errors.
      * Trade-off: Increased power consumption for better reliability.
      */
     sl_WlanPolicySet(SL_WLAN_POLICY_PM, SL_WLAN_LOW_LATENCY_POLICY, NULL, 0);
     g_fDebugPrint("SlWifiConn: Power policy set to LOW_LATENCY\n\r");

     /* Initialize error tracking - calloc already zeroes these but be explicit */
     g_ctx->fatalErrorCount = 0;
     g_ctx->recoveryDelayLevel = 0;

     SuspendNWP(false);

     if(retVal < 0)
     {
         Deinit();
     }
     else
     {
         CRITICAL_SECTION_EXIT();
     }
     return retVal;
}

//*****************************************************************************
//
//! \brief  Free all SlWifiConn resources (exit the SM task)
//!
//! \param  None
//!
//! \return None
//!
//*****************************************************************************
int SlWifiConn_deinit()
{
    int retVal = 0;
    VERIFY_INITIALIZED();

    if(g_ctx->state == SLWIFICONN_STATE_OFF)
    {
        SignalEvent(SLWIFICONN_EVENT_DESTROY);
    }
    else
    {
        retVal = SLNETERR_BSD_CONNECTION_PENDING;
    }
    return retVal;
}

//*****************************************************************************
//
//! \brief  Enable the Wifi module
//!
//! \param  None
//!
//! \return None
//!
//*****************************************************************************
void SlWifiConn_setDeleteAllProfiles(void)
{
    if(g_ctx != NULL) {
        g_ctx->bDeleteAllProfiles = true;
    }
}

int SlWifiConn_enable(void)
{
    int retVal;
    VERIFY_INITIALIZED();
    if(g_ctx->role != ROLE_STA)
    {
        retVal =  SLNETERR_WLAN_INVALID_ROLE;
    }
    else
    {
        /* NOTE: sl_Start should be called by now (so NWP is accessible) */
        g_fDebugPrint("SlWifiConn: Starting...\n\r");

        retVal = SignalEvent(SLWIFICONN_EVENT_ENABLE);
    }
    return retVal;
}

//*****************************************************************************
//
//! \brief  Disable the Wifi module
//!
//! \param  None
//!
//! \return None
//!
//*****************************************************************************
int SlWifiConn_disable(void)
{
    int retVal;
    VERIFY_INITIALIZED();
    g_fDebugPrint("SlWifiConn: Stopping...\n\r");
    retVal = SignalEvent(SLWIFICONN_EVENT_DISABLE);
    return retVal;
}



//*****************************************************************************
//
//! \brief  Enable the provisioning, notify the SM
//!
//*****************************************************************************
int SlWifiConn_enableProvisioning(WifiConnProvMode_e mode, uint8_t command, uint32_t flags)
{
    VERIFY_INITIALIZED();

    if((mode > WifiProvMode_ON) ||
       ((command > SL_WLAN_PROVISIONING_CMD_START_MODE_APSC_EXTERNAL_CONFIGURATION) &&
        (command != SL_WLAN_PROVISIONING_CMD_START_MODE_EXTERNAL_CONFIGURATION)) )

    {
        return SLNETERR_BSD_EINVAL;
    }

    CRITICAL_SECTION_ENTER();

    g_fWifiSM[SLWIFICONN_STATE_PROVISIONING] = smState_PROVISIONING;
    g_fWifiSM[SLWIFICONN_STATE_PROV_STOPPING] = smState_PROV_STOPPING;
    g_fWifiSM[SLWIFICONN_STATE_EXT_PROVISIONING] = smState_EXT_PROVISIONING;
    g_fEnterPROVISIONING = EnterPROVISIONING;
    g_fProvStatusHandler = SLProvisioningStatusHandler;

    g_ctx->provCmd = command;
    g_ctx->provMode = mode;
    if(flags & SLWIFICONN_PROV_FLAG_FORCE_PROVISIONING)
    {
        g_ctx->bDeleteAllProfiles = true;
    }
    if(flags & SLWIFICONN_PROV_FLAG_EXTERNAL_CONFIRMATION)
    {
        g_ctx->provFlags = SL_WLAN_PROVISIONING_CMD_FLAG_EXTERNAL_CONFIRMATION;
    }
    SignalEvent(SLWIFICONN_EVENT_ENABLE_PROVISIONING);

    CRITICAL_SECTION_EXIT();
    return 0;
}

//*****************************************************************************
//
//! \brief  Add profile, notify the SM
//!
//! \param  pSSID - SSID of AP to connect to
//!         pSecParams - pointer to AP security parameters
//!         pSecExtParams - pointer to an enterprise AP extended security params
//!
//! \return None
//!
//*****************************************************************************
int SlWifiConn_addProfile(const char *pSSID, uint16_t ssidLen, const _u8 *pMacAddr,
                          SlWlanSecParams_t  *pSecParams, SlWlanSecParamsExt_t* pSecExtParams,
                          int prio, uint32_t flags)
{
    int retVal = 0;
    bool bNonPersistent = (flags & SLWIFICONN_PROFILE_FLAG_NON_PERSISTENT);

    VERIFY_INITIALIZED();

    CRITICAL_SECTION_ENTER();
    if(bNonPersistent == 0)
    {
        retVal = sl_WlanProfileAdd((signed char *)pSSID, ssidLen, pMacAddr, pSecParams, pSecExtParams, prio, 0);
    }
    if(retVal == SL_RET_CODE_PROVISIONING_IN_PROGRESS || bNonPersistent)
    {
        /* Profile need to be saved in 2 cases:
         * 1. In case provisioning is in process we need to send the profile in
         * deferred event queue ( it will be added once the provisioning stops).
         * 2. In case on non persistent (static profile), the profile will kept and be
         * used upon entering WAIT_FOR_CONN
         */
        profile_t *pProf;
        int totalLen = sizeof(profile_t) + ssidLen;

        if(pSecParams)
        {
            totalLen += pSecParams->KeyLen;
            if(pSecExtParams)
            {
                totalLen += pSecExtParams->UserLen;
                totalLen += pSecExtParams->AnonUserLen;
            }
        }
        pProf = (profile_t*)malloc(totalLen);
        if(pProf)
        {
            pProf->priority = prio;
            pProf->ssidLen = ssidLen;
            if(pMacAddr)
            {
                pProf->bSetBssid = 1;
                memcpy(pProf->bssid, pMacAddr, 6);
            }
            else
            {
                pProf->bSetBssid = 0;
            }
            memcpy(PROFILE_SSID(pProf), pSSID, ssidLen);
            if(pSecParams)
            {
                pProf->secParams = *pSecParams;
                memcpy(PROFILE_PASSWORD(pProf), pSecParams->Key, pSecParams->KeyLen);
                pProf->secParams.Key = PROFILE_PASSWORD(pProf);
                if( pSecExtParams)
                {
                    assert(pSecParams->Type == SL_WLAN_SEC_TYPE_WPA_ENT);
                    pProf->extSecParams = *pSecExtParams;
                    memcpy(PROFILE_USER(pProf), pSecExtParams->User, pSecExtParams->UserLen);
                    pProf->extSecParams.User = PROFILE_USER(pProf);
                    memcpy(PROFILE_ANON_USER(pProf), pSecExtParams->AnonUser, pSecExtParams->AnonUserLen);
                    pProf->extSecParams.AnonUser = PROFILE_ANON_USER(pProf);
                }
            }
            else
            {
                pProf->secParams.Type = SL_WLAN_SEC_TYPE_OPEN;
                pProf->secParams.KeyLen = 0;
            }
        }
        else
        {
            CRITICAL_SECTION_EXIT();
            return SLNETERR_BSD_ENOMEM;
        }
        if(bNonPersistent)
        {
            /* only one non-persistent profile can be set (so free pevious one if exists) */
            if(g_ctx->pNonPersistentProfile)
            {
                free(g_ctx->pNonPersistentProfile);
            }
            g_ctx->pNonPersistentProfile = pProf;
        }
        else
        {
            /* push the add profile request to the deferred event queue */
            retVal = DeferredEvent_enqueue(SLWIFICONN_EVENT_ADD_PROFILE, pProf);
       }
    }
    if(retVal >= 0)
    {
        SignalEvent(SLWIFICONN_EVENT_ADD_PROFILE);
    }


    CRITICAL_SECTION_EXIT();
    return retVal;
}

//*****************************************************************************
//
//! \brief  Indication of a completion of an external provisioning or cloud
//!         confirmation.
//!         This will be used to stop the provisioning processing
//!
//! \param  None
//!
//! \return None
//!
//*****************************************************************************
int SlWifiConn_externalProvsioningCompleted(void)
{
    int retVal;
    VERIFY_INITIALIZED();
    retVal = SignalEvent(SLWIFICONN_EVENT_EXT_PROV_COMPLETED);
    return retVal;
}

//*****************************************************************************
//
//! \brief  Register SlNetIf Callback (invoked by SlNetIf_add)
//!
//! \param  fNetIfCB - SlNetIf Callback
//!         ifId - SlNetIf Interface Id
//!
//! \return None
//!
//*****************************************************************************
int    SlWifiConn_registerNetIFCallback(SlNetIf_Event_t fNetIfCB, uint16_t ifId)
{
    /* This can be called before SlNetConn_init */
    g_fNetIfCB = fNetIfCB;
    g_ifId = ifId;

    return 0;
}

//*****************************************************************************
//
//! \brief  get SlWifiConn Settings
//!
//! \param  pWifiConnSettings - pointer to output settings structure
//!
//! \return 0 on success, or negative error code
//!
//*****************************************************************************
int    SlWifiConn_getConfiguration(WifiConnSettings_t *pWifiConnSettings)
{
    int retVal = 0;
    if(!pWifiConnSettings)
        retVal = SLNETERR_RET_CODE_INVALID_INPUT;
    else
        *pWifiConnSettings = g_settings;
    return retVal;
}

//*****************************************************************************
//
//! \brief  Set SlWifiConn Settings
//!
//! \param  pWifiConnSettings - pointer to input settings structure
//!
//! \return 0 on success, or negative error code
//!
//*****************************************************************************
int    SlWifiConn_setConfiguration(WifiConnSettings_t *pWifiConnSettings)
{
    int retVal = 0;
    if(!pWifiConnSettings ||
        pWifiConnSettings->provisioningTimeout < 30)
        retVal = SLNETERR_RET_CODE_INVALID_INPUT;
    else
        g_settings = *pWifiConnSettings;
    return retVal;
}

//*****************************************************************************
//
//! \brief  Set WLAN Role (enabled only during OFF or External Provisioning states)
//!
//! \param  role - requested role
//!
//! \return 0 on success, or negative error code
//!
//*****************************************************************************
int SlWifiConn_setRole(SlWlanMode_e role)
{
    int retVal = SLNETERR_BSD_CONNECTION_PENDING;
    VERIFY_INITIALIZED();

    CRITICAL_SECTION_ENTER();
    if((g_ctx->state == SLWIFICONN_STATE_OFF) ||
       (g_ctx->state == SLWIFICONN_STATE_EXT_PROVISIONING))
    {
        g_ctx->role = SetRole(role);
        retVal = 0;
    }
    CRITICAL_SECTION_EXIT();
    return retVal;
}

//*****************************************************************************
//
//! \brief  An external request to reset the Network Processor (only allowed
//!         in OFF state.
//!
//! \param  none
//!
//! \return Active Role on success, or negative error code
//!
//*****************************************************************************
int SlWifiConn_reset()
{
    int retVal = SLNETERR_BSD_CONNECTION_PENDING;
    VERIFY_INITIALIZED();

    CRITICAL_SECTION_ENTER();
    if(g_ctx->state == SLWIFICONN_STATE_OFF)
    {
        retVal = ResetNWP();
    }
    CRITICAL_SECTION_EXIT();

    return retVal;
}

int SlWifiConn_requestReset()
{
    int retVal;
    VERIFY_INITIALIZED();
    g_fDebugPrint("SlWifiConn: resetting...\n\r");
    retVal = SignalEvent(SLWIFICONN_EVENT_RESET_REQUEST);
    return retVal;
}


#if SLWIFICONN_DEBUG_ENABLE
//*****************************************************************************
//
//! \brief  register an application-level debug (printf) method.
//!
//! \param  fDebugPrint - Application level debug method.
//!
//! \return 0 on success, or negative error code
//!
//*****************************************************************************
void SlWifiConn_registerDebugCallback(SlWifiConn_Debug_f fDebugPrint)
{
    g_fDebugPrint = fDebugPrint;
}
#endif

//*****************************************************************************
//
//! \brief  Get the number of stored WLAN profiles
//!
//! \param  None
//!
//! \return Number of profiles (0 if none), or -1 if not yet checked
//!
//*****************************************************************************
int8_t SlWifiConn_getProfileCount(void)
{
    return g_profileCount;
}

#ifdef WIFI_STATS
//*****************************************************************************
//
//! \brief  Enable WiFi statistics collection
//!
//! \param  interval - Collection interval in seconds (minimum 10)
//!
//! \return 0 on success, negative error code on failure
//!
//*****************************************************************************
int SlWifiConn_enableStats(uint16_t interval)
{
    VERIFY_INITIALIZED();

    if(interval < 10)
        return SLNETERR_RET_CODE_INVALID_INPUT;

    CRITICAL_SECTION_ENTER();
    g_ctx->statsInterval = interval;
    g_ctx->bStatsEnabled = true;

    /* If already connected, start the async timer immediately */
    if(g_ctx->state == SLWIFICONN_STATE_CONNECTED)
    {
        StartAsyncEvtTimer(interval);
    }
    CRITICAL_SECTION_EXIT();

    return 0;
}

//*****************************************************************************
//
//! \brief  Disable WiFi statistics collection
//!
//! \param  None
//!
//! \return 0 on success
//!
//*****************************************************************************
int SlWifiConn_disableStats(void)
{
    VERIFY_INITIALIZED();

    CRITICAL_SECTION_ENTER();
    g_ctx->bStatsEnabled = false;
    if(g_ctx->state == SLWIFICONN_STATE_CONNECTED)
    {
        StopAsyncEvtTimer();
    }
    CRITICAL_SECTION_EXIT();

    return 0;
}
#endif /* WIFI_STATS */
