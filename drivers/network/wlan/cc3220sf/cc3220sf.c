/*
 * cc3120mod.c
 *
 *  Created on: 08 Jun 2022
 *      Author: janz
 */



#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <os/xdc.h>
#include <os/os.h>

#include <includes/status.h>
#include <platform/platform.h>
#include <services/netlogger/netlogger.h>
#include <hal/hal.h>
#include <services/debug/debug.h>
#include <process/mqtt/mqtt_api.h>
#include <drivers/network/network.h>
#include <drivers/network/netdevice.h>
#include <drivers/network/wlan/wlannet.h>

#ifdef _NETWORK_DRIVER_TI_CC3320SF

/* TI-DRIVERS Header files */
#include <ti/drivers/net/wifi/simplelink.h>
#include "tifw/net/wifi/slnetif/slwificonn.h"
#include "tifw/net/wifi/slnetif/slnetifwifi.h"
#include <ti/net/slnet.h>
#include <ti/net/slnetif.h>
#include <ti/net/slnetconn.h>

#include <ti/devices/cc32xx/inc/hw_ocp_shared.h>
#include <ti/devices/cc32xx/inc/hw_types.h>
#include <ti/devices/cc32xx/inc/hw_memmap.h>
#include <services/http/http_handlers.h>
#include <services/ota/ota_if.h>

/* OCP Spare registers for persisting NWP stats across warm reboots.
 * SPARE_REG_4 is used by ITM, SPARE_REG_5 is used by NWP - avoid those. */
#define NWP_STATS_REG_FATAL    (OCP_SHARED_BASE + OCP_SHARED_O_SPARE_REG_0)
#define NWP_STATS_REG_RECOVER  (OCP_SHARED_BASE + OCP_SHARED_O_SPARE_REG_1)
#define NWP_STATS_REG_DISCON   (OCP_SHARED_BASE + OCP_SHARED_O_SPARE_REG_2)

#include "cc3220sf.h"

#undef DEBUG_IF_NAME
#undef DEBUG_IF_SEVERITY
#define DEBUG_IF_NAME       "WIFI"
#define DEBUG_IF_SEVERITY   WIFI_IF_DEBUG_LEVEL

#define SL_STOP_TIMEOUT         200
/* Provisioning inactivity timeout in seconds (20 min)*/
#define PROVISIONING_INACTIVITY_TIMEOUT         (1200)
#define EVENTQ_SIZE 4
#define GENERAL_TIMEOUT 2000

/* Status bits - used to set/reset the corresponding bits in given a variable */
typedef enum
{
    STATUS_BIT_RUNNING, // MWP is running

    STATUS_BIT_CHECKED_PROFILES, // profiles already checked

    STATUS_BIT_PROV_MODE, // in provision mode

    STATUS_BIT_GOT_MODE,

    STATUS_BIT_CONNECTED, // connected to MAC

    STATUS_BIT_IP_ACQUIRED, // got an IP

    STATUS_BIT_PROV_CONNECT,
    STATUS_BIT_PROV_IP_ACQUIRED,
    STATUS_BIT_PROV_SUCCESS,
    STATUS_BIT_PROV_STOPPED,
    STATUS_BIT_PROV_PROF_ADDED,
    STATUS_BIT_PROV_FAILED,

    STATUS_BIT_PING_DONE,

    // firmware events
    STATUS_BIT_FW_DL_START,
    STATUS_BIT_FW_DL_DONE,
    STATUS_BIT_FW_DL_ERROR,
    STATUS_BIT_FW_UPGRADE,
    STATUS_BIT_FW_INSTALL,

    STATUS_BIT_AUTHENTICATION_FAILED,
    STATUS_BIT_RESET_REQUIRED,
    STATUS_BIT_REBOOT_REQUIRED,
    STATUS_BIT_FATAL_ERROR,
} STATUS_BITS_e;


#define CLR_STATUS_BIT_ALL(status_variable)  (status_variable = 0)
#define SET_STATUS_BIT(status_variable, bit) (status_variable |= (1 << (bit)))
#define CLR_STATUS_BIT(status_variable, bit) (status_variable &= ~(1 << (bit)))
#define GET_STATUS_BIT(status_variable, bit) (0 != \
                                              (status_variable & (1 << (bit))))

#define IS_CONNECTED(status_variable)        GET_STATUS_BIT( \
        status_variable, \
        STATUS_BIT_CONNECTED)

#define IS_PROV_MODE(status_variable)        GET_STATUS_BIT( \
        status_variable, \
        STATUS_BIT_PROV_MODE)

#define IS_IP_ACQUIRED(status_variable)      GET_STATUS_BIT( \
        status_variable, \
        STATUS_BIT_IP_ACQUIRED)

#define IS_RUNNING(status_variable)        GET_STATUS_BIT( \
        status_variable, \
        STATUS_BIT_RUNNING)

#define IS_REBOOT(status_variable)        GET_STATUS_BIT( \
        status_variable, \
        STATUS_BIT_REBOOT_REQUIRED)

#define IS_FW_DOWNLOAD_START(status_variable)        GET_STATUS_BIT( \
        status_variable, \
        STATUS_BIT_FW_DL_START)

#define IS_FW_UPGRADE(status_variable)        GET_STATUS_BIT( \
        status_variable, \
        STATUS_BIT_FW_UPGRADE)

#define IS_FW_DOWNLOAD_DONE(status_variable)        GET_STATUS_BIT( \
        status_variable, \
        STATUS_BIT_FW_DL_DONE)

#define IS_FW_DOWNLOAD_ERROR(status_variable)        GET_STATUS_BIT( \
        status_variable, \
        STATUS_BIT_FW_DL_ERROR)

typedef enum CC3220SF_STATE {
    CC3220SF_STATE_INIT = 0, //1
    CC3220SF_STATE_START, //3
    CC3220SF_STATE_CHECK_PROFILES, //4
    CC3220SF_STATE_WAIT_4_START, //5
    CC3220SF_STATE_STOP, //7
    CC3220SF_STATE_WAIT_CONNECTAP, //11
    CC3220SF_STATE_WAIT_IP, //12
    CC3220SF_STATE_START_NETWORK, //14
    CC3220SF_STATE_START_HTTP, //15
    CC3220SF_STATE_NOTIFY_UP, //16
    CC3220SF_STATE_NOTIFY_DOWN, //17
    CC3220SF_STATE_READY, //18
    CC3220SF_STATE_IDLE, //12
    CC3220SF_STATE_SLEEP, //21
    CC3220SF_STATE_CHECK_CONNECTION, //22
    CC3220SF_STATE_GET_STATISTICS, //23
    CC3220SF_STATE_ERROR, // 24

    // provisioning
    CC3220SF_STATE_START_PROV, //25
    CC3220SF_STATE_HTTP_MESSAGE,
    CC3220SF_STATE_HANDLE_EVENT,

    // firmware
    CC3220SF_STATE_WAIT_FW_INSTALL,
    // provisioning
    CC3220SF_STATE_WAIT_PROVISION,

} CC3220SF_STATE_t;

/* EVENTs get send from any of the callbacks to the main thread */
typedef enum CC3220SF_EVENT
{
    CC3220SF_EVENT_STOPPED,
    CC3220SF_EVENT_RUNNING,
    CC3220SF_EVENT_CONNECTED,
    CC3220SF_EVENT_IP_ACQUIRED,
    CC3220SF_EVENT_IP_LOST,
    CC3220SF_EVENT_IP_LEASED,
    CC3220SF_EVENT_IP_RELEASED,
    CC3220SF_EVENT_DISCONNECT,

    CC3220SF_EVENT_STA_ADDED,
    CC3220SF_EVENT_STA_REMOVED,
    CC3220SF_EVENT_PROF_ADDED, // 10

    CC3220SF_EVENT_PING_COMPLETE,

    CC3220SF_EVENT_PROVISIONING_STARTED,
    CC3220SF_EVENT_PROVISIONING_SUCCESS, // 13
    CC3220SF_EVENT_PROVISIONING_STOPPED, // 14
    CC3220SF_EVENT_PROVISIONING_WAIT_CONN,
    CC3220SF_EVENT_PROVISIONING_CONNECT_FAILED,
    CC3220SF_EVENT_PROVISIONING_CONNECT_SUCCESS,
    CC3220SF_EVENT_PROVISIONING_TIMEOUT,
    CC3220SF_EVENT_PROVISIONING_IP_ACQUIRED,
    CC3220SF_EVENT_PROVISIONING_IP_FAILED,
    CC3220SF_EVENT_PROVISIONING_CONFIRM_FAILED,

    CC3220SF_EVENT_FW_DOWNLOAD_START,
    CC3220SF_EVENT_FW_DOWNLOAD_DONE,
    CC3220SF_EVENT_FW_DOWNLOAD_ERROR,
    CC3220SF_EVENT_FW_UPGRADE,
    CC3220SF_EVENT_FW_INSTALL,

    CC3220SF_EVENT_TIMEOUT,
    CC3220SF_EVENT_ERROR,
    CC3220SF_EVENT_FATAL_ERROR,
    CC3220SF_EVENT_RESTART,
    CC3220SF_EVENT_MAX,
    CC3220SF_EVENT_INVALID
} CC3220SF_EVENT_t;


/* Fatal error threshold - system reboot after this many consecutive errors */
#define FATAL_ERROR_REBOOT_THRESHOLD    10

/* DRIVER DESCRIPTOR */
typedef struct CC3220SF_Desc
{
    pthread_t slTask;
    pthread_t wifiTask;
    CC3220SF_STATE_t state; // current state
    CC3220SF_STATE_t nstate; // next state
    CC3220SF_STATE_t pstate; // previosu state
    uint8_t open;
    uint32_t timer;
    uint32_t sleepTime;
    uint32_t retries;
    uint8_t running;
    uint8_t fatalErrorCount; // consecutive fatal errors for reboot decision
    uint32_t totalFatalErrors; // lifetime fatal error count
    uint32_t totalRecoveries; // lifetime successful recoveries
    uint32_t totalDisconnects; // lifetime disconnect count
    uint32_t connectedTime; // accumulated connection time in ms

    SlWlanMode_e wlanMode;
    uint32_t wlanStatus;
    uint32_t provStatus;
    SlWlanMode_e wlanRole;
    int16_t lastError;
    SlNetIf_t *netIf;
    SlDeviceGetStat_t rxStatistics;

    mqd_t eventQ;
    mqd_t httpserverQueue;

    struct bus *parent;
    struct network_dev *netdev;
    uint8_t macAddress[6];

    char name[17];
    uint8_t netId;
    uint8_t netPriority;

    SlWlanSecParams_t securityParams;
    char SSIDremoteName[SSID_LEN_MAX + 1];
    char SecurityKey[SSID_LEN_MAX + 1];
    uint8_t SSIDconnection[SSID_LEN_MAX + 1];
    uint8_t BSSIDconnection[BSSID_LEN_MAX];
    char SSID[SSID_LEN_MAX + 1];
    uint8_t wlanEventReasonCode;
    uint32_t stationIP;
    uint32_t netmask;
    uint32_t gatewayIP;
    uint32_t dnsIP;
    bool doProvisioning;

    eventNetdevCallback event;
    void *object;

    char loggerBuffer[64];
} CC3220SF_Desc_t;

static struct CC3220SF_Desc cc3220sfDevice;

struct CC3220SF_Desc *pWlanDev = NULL;

extern int SlWifiConn_requestReset();

#ifdef WIFI_STATS
/* WiFi statistics API - implemented in slwificonn.c */
extern int SlWifiConn_enableStats(uint16_t interval);
extern int SlWifiConn_disableStats(void);

/* WiFi statistics result data - must match SlWifiConnStats_t in slwificonn.c */
typedef struct {
    uint8_t     bssid[6];
    int16_t     mgMntRssi;
    int16_t     dataCtrlRssi;
} SlWifiConnStats_t;

#endif

const char *MODES[] = { "ROLE_STA",
                       "ROLE_RESERVED",
                       "ROLE_AP",
                       "ROLE_P2P",
                       "ROLE_TAG" };

/* used to send event to the main thread */
static void netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_t event)
{
    char evt = event;
    mq_send(pWlanDev->eventQ, (const char *)&evt, 1, 0);
}

/* used to receive events from the callbacks
 * will also manage the status bits
 * */
static CC3220SF_EVENT_t netdev_wlan_cc3220sf_recv_event( void )
{
    char event;
    CC3220SF_EVENT_t evt = CC3220SF_EVENT_INVALID;
    int retval;
    struct mq_attr mqstat;

    retval = mq_getattr(pWlanDev->eventQ, &mqstat);

    if(retval == 0 && mqstat.mq_curmsgs > 0) {

        retval = mq_receive(pWlanDev->eventQ, (char *)&event, 1, NULL);

        if(retval > 0) {
            evt = (CC3220SF_EVENT_t)event;
            LOG_DEBUG("GOT EVENT %d", event);

            switch(evt) {

            case CC3220SF_EVENT_STOPPED:
                CLR_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_RUNNING);
                break;

            case CC3220SF_EVENT_RUNNING:
                SET_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_RUNNING);
                break;

            case CC3220SF_EVENT_CONNECTED:
                SET_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_CONNECTED);
                break;

            case CC3220SF_EVENT_IP_ACQUIRED:
                SET_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_IP_ACQUIRED);
                break;

            case CC3220SF_EVENT_DISCONNECT:
                CLR_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_CONNECTED);
                pWlanDev->totalDisconnects++;
                pWlanDev->connectedTime = 0;
                HWREG(NWP_STATS_REG_DISCON) = pWlanDev->totalDisconnects;
                break;

            case CC3220SF_EVENT_PING_COMPLETE:
                break;

            case CC3220SF_EVENT_PROVISIONING_STARTED:
                SET_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_PROV_MODE);
                break;

            case CC3220SF_EVENT_PROVISIONING_SUCCESS:
                SET_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_PROV_SUCCESS);
                break;

            case CC3220SF_EVENT_PROVISIONING_STOPPED:
                SET_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_PROV_STOPPED);
                break;

            case CC3220SF_EVENT_PROVISIONING_WAIT_CONN:
                break;

            case CC3220SF_EVENT_PROVISIONING_CONNECT_FAILED:
                SET_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_PROV_FAILED);
                break;

            case CC3220SF_EVENT_PROVISIONING_CONNECT_SUCCESS:
                SET_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_PROV_CONNECT);
                break;

            case CC3220SF_EVENT_PROVISIONING_TIMEOUT:
                SET_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_PROV_FAILED);
                break;

            case CC3220SF_EVENT_PROVISIONING_IP_ACQUIRED:
                SET_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_PROV_IP_ACQUIRED);
                break;

            case CC3220SF_EVENT_PROVISIONING_IP_FAILED:
                SET_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_PROV_FAILED);
                break;

            case CC3220SF_EVENT_PROVISIONING_CONFIRM_FAILED:
                SET_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_PROV_FAILED);
                break;

            case CC3220SF_EVENT_TIMEOUT:
                break;

            case CC3220SF_EVENT_ERROR:
                CLR_STATUS_BIT_ALL(pWlanDev->wlanStatus);
                SET_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_FATAL_ERROR);
                break;

            case CC3220SF_EVENT_RESTART:
                SET_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_REBOOT_REQUIRED);
                break;

            case CC3220SF_EVENT_PROF_ADDED:
                SET_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_PROV_PROF_ADDED);
                break;

            case CC3220SF_EVENT_FATAL_ERROR:
                CLR_STATUS_BIT_ALL(pWlanDev->wlanStatus);
                SET_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_FATAL_ERROR);
                pWlanDev->fatalErrorCount++;
                pWlanDev->totalFatalErrors++;
                HWREG(NWP_STATS_REG_FATAL) = pWlanDev->totalFatalErrors;
                LOG_WARNING("Fatal error count: %d/%d (total: %d)",
                            pWlanDev->fatalErrorCount,
                            FATAL_ERROR_REBOOT_THRESHOLD,
                            pWlanDev->totalFatalErrors);
#ifdef WIFI_REBOOT_ON_FATAL_ERROR
                if(pWlanDev->fatalErrorCount >= FATAL_ERROR_REBOOT_THRESHOLD)
                {
                    LOG_ERROR("Too many fatal errors, requesting system reboot");
                    hal_reset();
                    /* Hang here - RTS will handle graceful shutdown */
                    while(1)
                    {
                        Task_sleep(1000);
                    }
                }
#endif
                break;

            // firmware events
            case CC3220SF_EVENT_FW_DOWNLOAD_START:
                SET_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_FW_DL_START);
                CLR_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_FW_DL_DONE);
                CLR_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_FW_DL_ERROR);
                break;

            case CC3220SF_EVENT_FW_DOWNLOAD_DONE:
                SET_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_FW_DL_DONE);
                break;

            case CC3220SF_EVENT_FW_DOWNLOAD_ERROR:
                CLR_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_FW_DL_START);
                CLR_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_FW_DL_DONE);
                SET_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_FW_DL_ERROR);
                break;


            case CC3220SF_EVENT_FW_UPGRADE:
                SET_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_FW_UPGRADE);
                CLR_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_FW_DL_START);
                CLR_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_FW_DL_DONE);
                CLR_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_FW_DL_ERROR);
                CLR_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_FW_INSTALL);
                break;

            case CC3220SF_EVENT_FW_INSTALL:
                CLR_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_FW_UPGRADE);
                CLR_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_FW_DL_START);
                CLR_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_FW_DL_DONE);
                CLR_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_FW_DL_ERROR);
                SET_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_FW_INSTALL);
                break;


            default:
                LOG_DEBUG("EVENT NOT HANDLED %d", evt);
            }
        }
    }

    return evt;
}

static const char *netdev_wlan_cc3220sf_error_toString(int16_t error)
{
    switch(error) {
    case SL_ERROR_ROLE_STA_ERR: return "ROLE-STATION-ERROR";
    case SL_ERROR_ROLE_AP_ERR: return "ROLE-AP-ERROR";
    case SL_ERROR_ROLE_P2P_ERR: return "ROLE-P2P-ERROR";
    case SL_ERROR_FS_CORRUPTED_ERR: return "FS-CORRUPT";
    case SL_ERROR_FS_ALERT_ERR: return "FS-ALERT-ERROR";
    case SL_ERROR_RESTORE_IMAGE_COMPLETE: return "RESTORE-IMAGE-COMPLETE";
    case SL_ERROR_INCOMPLETE_PROGRAMMING: return "INCOMPLETE-PROGRAMMING";
    case SL_ERROR_ROLE_TAG_ERR: return "ROLE-TAG-ERROR";
    case SL_ERROR_FIPS_ERR: return "ROLE-FIPS-ERROR";
    case SL_ERROR_GENERAL_ERR: return "ROLE-GENERAL-ERROR";
    case SL_API_ABORTED: return "ABORTED";
    case SL_RET_CODE_DEV_ALREADY_STARTED: return "ALREADY-STARTED";
    case SL_ERROR_BSD_ETIMEDOUT: return "TIMEOUT";
    }

    return "UNKNOWN";
}

static SlWlanMode_e netdev_wlan_cc3220sf_to_wlanRole(uint32_t mode)
{
    SlWlanMode_e ret = ROLE_STA;

    switch(mode) {
    case WLAN_MODE_NONE:
        ret = ROLE_STA;
        break;
    case WLAN_MODE_STATION:
        ret = ROLE_STA;
        break;
    case WLAN_MODE_AP:
        ret = ROLE_AP;
        break;
    case WLAN_MODE_P2P:
        ret = ROLE_P2P;
        break;
    case WLAN_MODE_PROVISION:
        ret = ROLE_STA;
        break;
    }

    return ret;
}

static int32_t netdev_wlan_cc3220s_getConnectionStatus()
{
    SlWlanConnStatusParam_t connectionParams;
    uint16_t                Opt    = 0;
    int32_t                 retVal = 0;
    uint16_t                Size   = 0;

    Size = sizeof(SlWlanConnStatusParam_t);
    memset(&connectionParams, 0, Size);

    retVal = sl_WlanGet(SL_WLAN_CONNECTION_INFO, &Opt, &Size, (uint8_t *)&connectionParams);

    /* Check if the function returned an error                               */
    if (retVal < SLNETERR_RET_CODE_OK)
    {
        /* Return error code                                                 */
        return retVal;
    }

    return connectionParams.ConnStatus;
}

static int32_t netdev_wlan_cc3220s_getConnectionStats()
{
    _u16 length = sizeof(SlDeviceGetStat_t);
    _i16 ret = sl_DeviceStatGet(SL_DEVICE_STAT_WLAN_RX, length, &pWlanDev->rxStatistics);

    return 0;
}

/* SimpleLink Event Handlers */
//*****************************************************************************
//!
//! On Successful completion of Wlan Connect, This function triggers connection
//! status to be set.
//!
//! \param[in]  pSlWlanEvent    - pointer indicating Event type
//!
//! \return None
//!
//*****************************************************************************
void SimpleLinkWlanEventHandler(SlWlanEvent_t *pSlWlanEvent)
{

    static const char *Roles[] = {"STA","STA","AP","P2P"};
    static const char *WlanStatus[] = {"DISCONNECTED","SCANING","CONNECTING","CONNECTED"};

    SlWlanEventDisconnect_t* pEventData = NULL;

    switch(pSlWlanEvent->Id) {
    case SL_WLAN_EVENT_CONNECT:
        memcpy(pWlanDev->SSIDconnection, pSlWlanEvent->Data.Connect.SsidName, pSlWlanEvent->Data.Connect.SsidLen);
        memcpy(pWlanDev->BSSIDconnection, pSlWlanEvent->Data.Connect.Bssid,SL_WLAN_BSSID_LENGTH);

        LOG_DEBUG(" [Event] STA connected to AP "
                         "- BSSID:%.2x:%.2x:%.2x:%.2x:%.2x:%.2x, SSID:%s",
                         pSlWlanEvent->Data.Connect.Bssid[0],
                         pSlWlanEvent->Data.Connect.Bssid[1],
                         pSlWlanEvent->Data.Connect.Bssid[2],
                         pSlWlanEvent->Data.Connect.Bssid[3],
                         pSlWlanEvent->Data.Connect.Bssid[4],
                         pSlWlanEvent->Data.Connect.Bssid[5],
                         pSlWlanEvent->Data.Connect.SsidName);

        netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_CONNECTED);
        break;

    case SL_WLAN_EVENT_DISCONNECT:
        pEventData = &pSlWlanEvent->Data.Disconnect;
        pWlanDev->wlanEventReasonCode = pEventData->ReasonCode;
        /* WLAN Disconnect Reason Codes, i.e. SL_WLAN_DISCONNECT_USER_INITIATED                           */

        LOG_DEBUG(" [Event] STA disconnected from AP (Reason Code = %d)",
                 pSlWlanEvent->Data.Disconnect.ReasonCode);

        netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_DISCONNECT);
        break;

    case SL_WLAN_EVENT_STA_ADDED:
        /* when device is in AP mode and any client connects to it.       */
        netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_STA_ADDED);
        break;

    case SL_WLAN_EVENT_STA_REMOVED:
        /* when device is in AP mode and any client disconnects from it.  */
        netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_STA_REMOVED);
        break;

    case SL_WLAN_EVENT_PROVISIONING_PROFILE_ADDED:
            LOG_DEBUG(" [Provisioning] Profile Added: SSID: %s",
                      pSlWlanEvent->Data.ProvisioningProfileAdded.Ssid);
            if(pSlWlanEvent->Data.ProvisioningProfileAdded.ReservedLen > 0)
            {
                LOG_DEBUG(" [Provisioning] Profile Added: PrivateToken:%s",
                          pSlWlanEvent->Data.ProvisioningProfileAdded.Reserved);
            }
            netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_PROF_ADDED);
            break;

    case SL_WLAN_EVENT_LINK_QUALITY_TRIGGER:
        {
            int Rssi = (int)((signed char)pSlWlanEvent->Data.LinkQualityTrigger.Data);
        }
        break;

    case SL_WLAN_EVENT_PROVISIONING_STATUS:
       {
           switch(pSlWlanEvent->Data.ProvisioningStatus.ProvisioningStatus)
           {
           case SL_WLAN_PROVISIONING_GENERAL_ERROR:
           case SL_WLAN_PROVISIONING_ERROR_ABORT:
           case SL_WLAN_PROVISIONING_ERROR_ABORT_INVALID_PARAM:
           case SL_WLAN_PROVISIONING_ERROR_ABORT_HTTP_SERVER_DISABLED:
           case SL_WLAN_PROVISIONING_ERROR_ABORT_PROFILE_LIST_FULL:
           case SL_WLAN_PROVISIONING_ERROR_ABORT_PROVISIONING_ALREADY_STARTED:
               LOG_DEBUG(" [Provisioning] Provisioning Error status=%d",
                         pSlWlanEvent->Data.ProvisioningStatus.ProvisioningStatus);
               break;

           case SL_WLAN_PROVISIONING_CONFIRMATION_STATUS_FAIL_NETWORK_NOT_FOUND:
               LOG_DEBUG(" [Provisioning] Profile confirmation failed: "
                         "network not found");
               netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_PROVISIONING_CONFIRM_FAILED);
               break;

           case SL_WLAN_PROVISIONING_CONFIRMATION_STATUS_FAIL_CONNECTION_FAILED:
               LOG_DEBUG(" [Provisioning] Profile confirmation failed:"
                         " Connection failed");
               netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_PROVISIONING_CONNECT_FAILED);
               break;

           case
           SL_WLAN_PROVISIONING_CONFIRMATION_STATUS_CONNECTION_SUCCESS_IP_NOT_ACQUIRED:
               LOG_DEBUG(" [Provisioning] Profile confirmation failed:"
                         " IP address not acquired");
               netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_PROVISIONING_IP_FAILED);
               break;

           case SL_WLAN_PROVISIONING_CONFIRMATION_STATUS_SUCCESS_FEEDBACK_FAILED:
               LOG_DEBUG(" [Provisioning] Profile Confirmation failed "
                         " (Connection Success, feedback to Smartphone app failed)");
               netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_PROVISIONING_CONFIRM_FAILED);
               break;

           case SL_WLAN_PROVISIONING_CONFIRMATION_STATUS_SUCCESS:
               LOG_DEBUG(" [Provisioning] Profile Confirmation Success!");
               netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_PROVISIONING_SUCCESS);
               break;

           case SL_WLAN_PROVISIONING_AUTO_STARTED:
               LOG_INFO(" [Provisioning] Auto-Provisioning Started");
               netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_PROVISIONING_STARTED);
               break;

           case SL_WLAN_PROVISIONING_STOPPED:
               LOG_INFO(" Provisioning stopped: Current Role: %s",
                           Roles[pSlWlanEvent->Data.ProvisioningStatus.Role]);

               if(ROLE_STA == pSlWlanEvent->Data.ProvisioningStatus.Role)
               {
                   LOG_DEBUG("WLAN Status: %s",
                               WlanStatus[pSlWlanEvent->Data.ProvisioningStatus.
                                          WlanStatus]);

                   if(SL_WLAN_STATUS_CONNECTED ==
                           pSlWlanEvent->Data.ProvisioningStatus.WlanStatus)
                   {
                       LOG_DEBUG("Connected to SSID: %s",
                                 pSlWlanEvent->Data.ProvisioningStatus.Ssid);
                   }
               }
               netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_PROVISIONING_STOPPED);
               break;

           case SL_WLAN_PROVISIONING_SMART_CONFIG_SYNCED:
               LOG_DEBUG(" [Provisioning] Smart Config Synced!");
               break;

           case SL_WLAN_PROVISIONING_SMART_CONFIG_SYNC_TIMEOUT:
               LOG_DEBUG(" [Provisioning] Smart Config Sync Timeout!");
               netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_PROVISIONING_TIMEOUT);
               break;

           case SL_WLAN_PROVISIONING_CONFIRMATION_WLAN_CONNECT:
               LOG_DEBUG(
                       " [Provisioning] Profile confirmation: WLAN Connected!");
               netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_PROVISIONING_CONNECT_SUCCESS);
               break;

           case SL_WLAN_PROVISIONING_CONFIRMATION_IP_ACQUIRED:
               LOG_DEBUG(
                       " [Provisioning] Profile confirmation: IP Acquired!");
               netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_PROVISIONING_IP_ACQUIRED);
               break;

           case SL_WLAN_PROVISIONING_EXTERNAL_CONFIGURATION_READY:
               LOG_DEBUG(" [Provisioning] External configuration is ready! ");
               /* [External configuration]: External configuration is ready,
   start the external configuration process.
           In case of using the external provisioning
   enable the function below which will trigger StartExternalProvisioning() */
               break;

           default:
               LOG_ERROR(" [Provisioning] Unknown Provisioning Status: %d",
                         pSlWlanEvent->Data.ProvisioningStatus.ProvisioningStatus);
               break;
           }
       }
       break;


    default:
        LOG_WARNING(" [Event] - WlanEventHandler has received %d !!!!",
                  pSlWlanEvent->Id);
        break;
    }
}

//*****************************************************************************
//
//! \brief The Function Handles the Fatal errors
//!
//! \param[in]  slFatalErrorEvent - Pointer to Fatal Error Event info
//!
//! \return None
//!
//*****************************************************************************
void SimpleLinkFatalErrorEventHandler(SlDeviceFatal_t *slFatalErrorEvent)
{

    switch (slFatalErrorEvent->Id)
    {
    case SL_DEVICE_EVENT_FATAL_DEVICE_ABORT:
    {
        LOG_ERROR("FATAL ERROR: Abort NWP event detected: "
                "AbortType=%d, AbortData=0x%x\n\r",
                slFatalErrorEvent->Data.DeviceAssert.Code,
                slFatalErrorEvent->Data.DeviceAssert.Value);
    }
    break;

    case SL_DEVICE_EVENT_FATAL_DRIVER_ABORT:
    {
        LOG_ERROR("FATAL ERROR: Driver Abort detected\n\r");
    }
    break;

    case SL_DEVICE_EVENT_FATAL_NO_CMD_ACK:
    {
        LOG_ERROR("FATAL ERROR: No Cmd Ack detected "
                "[cmd opcode = 0x%x]\n\r",
                slFatalErrorEvent->Data.NoCmdAck.Code);
    }
    break;

    case SL_DEVICE_EVENT_FATAL_SYNC_LOSS:
    {
        LOG_ERROR("FATAL ERROR: Sync loss detected\n\r");
    }
    break;

    case SL_DEVICE_EVENT_FATAL_CMD_TIMEOUT:
    {
        LOG_ERROR("FATAL ERROR: Async event timeout detected "
                "[event opcode = 0x%x]\n\r",
                slFatalErrorEvent->Data.CmdTimeout.Code);
    }
    break;

    default:
        LOG_ERROR("FATAL ERROR: Unspecified error detected\n\r");
        break;
    }

    netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_FATAL_ERROR);
}

//*****************************************************************************
//
//! This function handles network events such as IP acquisition, IP leased, IP
//! released etc.
//!
//! \param[in]  pNetAppEvent - Pointer to NetApp Event Info
//!
//! \return None
//!
//*****************************************************************************
void SimpleLinkNetAppEventHandler(SlNetAppEvent_t *pNetAppEvent)
{
    switch(pNetAppEvent->Id) {
    case SL_NETAPP_EVENT_IPV4_ACQUIRED:
    case SL_NETAPP_EVENT_IPV6_ACQUIRED:

        netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_IP_ACQUIRED);
        pWlanDev->stationIP = (pNetAppEvent)->Data.IpAcquiredV4.Ip;
        pWlanDev->gatewayIP = (pNetAppEvent)->Data.IpAcquiredV4.Gateway;
        pWlanDev->dnsIP = (pNetAppEvent)->Data.IpAcquiredV4.Dns;

        LOG_INFO("[NETAPP EVENT] IP Acquired: IP=%d.%d.%d.%d , "
                         "Gateway=%d.%d.%d.%d",
                         SL_IPV4_BYTE(pNetAppEvent->Data.IpAcquiredV4.Ip,3),
                         SL_IPV4_BYTE(pNetAppEvent->Data.IpAcquiredV4.Ip,2),
                         SL_IPV4_BYTE(pNetAppEvent->Data.IpAcquiredV4.Ip,1),
                         SL_IPV4_BYTE(pNetAppEvent->Data.IpAcquiredV4.Ip,0),
                         SL_IPV4_BYTE(pNetAppEvent->Data.IpAcquiredV4.Gateway,3),
                         SL_IPV4_BYTE(pNetAppEvent->Data.IpAcquiredV4.Gateway,2),
                         SL_IPV4_BYTE(pNetAppEvent->Data.IpAcquiredV4.Gateway,1),
                         SL_IPV4_BYTE(pNetAppEvent->Data.IpAcquiredV4.Gateway,0));

        break;


    case SL_NETAPP_EVENT_IPV4_LOST:
    case SL_NETAPP_EVENT_IPV6_LOST:
        LOG_DEBUG("[NETAPP EVENT] IPV4 Lost");
        netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_IP_LOST);
        pWlanDev->stationIP = 0;
        pWlanDev->gatewayIP = 0;
        pWlanDev->dnsIP = 0;
        break;

    case SL_NETAPP_EVENT_DHCPV4_LEASED:
        netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_IP_LEASED);
        LOG_DEBUG("[NETAPP Event] IP Leased: %d.%d.%d.%d",
                           SL_IPV4_BYTE(pNetAppEvent->Data.IpLeased.IpAddress,3),
                           SL_IPV4_BYTE(pNetAppEvent->Data.IpLeased.IpAddress,2),
                           SL_IPV4_BYTE(pNetAppEvent->Data.IpLeased.IpAddress,1),
                           SL_IPV4_BYTE(pNetAppEvent->Data.IpLeased.IpAddress,0));
        break;

    case SL_NETAPP_EVENT_DHCPV4_RELEASED:
        netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_IP_RELEASED);
//        Display_printf(display, 0, 0,"[NETAPP EVENT] IP Released for Client: "
//                       "IP=%d.%d.%d.%d , ", SL_IPV4_BYTE(g_ulStaIp,
//                                                         3),
//                       SL_IPV4_BYTE(g_ulStaIp,
//                                    2),
//                       SL_IPV4_BYTE(g_ulStaIp, 1), SL_IPV4_BYTE(g_ulStaIp, 0));
        break;

    default:
        LOG_DEBUG("APPEVENT: OTHER %d", pNetAppEvent->Id);
//        Display_printf(display, 0, 0,
//                       "[NETAPP EVENT] Unexpected event [0x%x] \n\r",
//                       pNetAppEvent->Id);
        break;
    }

//    return (EVENT_PROPAGATION_CONTINUE);
}

//*****************************************************************************
//
//! This function handles General Events
//!
//! \param[in]  pDevEvent - Pointer to General Event Info
//!
//! \return None
//!
//*****************************************************************************
void SimpleLinkGeneralEventHandler(SlDeviceEvent_t *pDevEvent)
{
    /* Most of the general errors are not FATAL. are to be handled            */
    /* appropriately by the application.                                      */
//    Display_printf(display, 0, 0,"[GENERAL EVENT] - ID=[%d] Sender=[%d]\n\n",
//                   pDevEvent->Data.Error.Code,
//                   pDevEvent->Data.Error.Source);
    LOG_DEBUG("[GENERAL EVENT] - ID=[%d] Sender=[%d]",
           pDevEvent->Data.Error.Code,
           pDevEvent->Data.Error.Source);

//    return (EVENT_PROPAGATION_CONTINUE);
}

void SimpleLinkOtaEventHandler(otaNotif_e notification, OtaEventParam_u *pParams)
{
    SlNetConnStatus_e status;
    int retVal;

    switch(notification) {

    case OTA_NOTIF_IMAGE_PENDING_COMMIT:
//         LOG_INFO("OTA_NOTIF_IMAGE_PENDING_COMMIT");
         OTA_IF_commit();
//         if (retVal == 0 && status == SLNETCONN_STATUS_CONNECTED_IP)
//         {
//             OTA_IF_commit();
//         }
//         else
//         {
//             OTA_IF_rollback();
//             LOG_ERROR("Error Testing the new version - reverting to old version (%d)", retVal);
//         }
         break;

    case OTA_NOTIF_IMAGE_DOWNLOAD:
        netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_FW_DOWNLOAD_START);
        break;

    case OTA_NOTIF_IMAGE_PROGRESS:
        LOG_INFO("Download progress: %d", pParams->progressPercent);
        break;

    case OTA_NOTIF_IMAGE_DOWNLOADED:
        netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_FW_DOWNLOAD_DONE);
        break;

    case OTA_NOTIF_GETLINK_ERROR:
        LOG_ERROR("OTA_NOTIF_GETLINK_ERROR (%d)", pParams->err.errorCode);
        break;

    case OTA_NOTIF_DOWNLOAD_ERROR:
        LOG_ERROR("OTA_NOTIF_DOWNLOAD_ERROR (%d)", pParams->err.errorCode);
        netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_FW_DOWNLOAD_ERROR);
        break;

    case OTA_NOTIF_INSTALL_ERROR:
        LOG_ERROR("OTA_NOTIF_INSTALL_ERROR (%d)", pParams->err.errorCode);
        break;

    case OTA_NOTIF_COMMIT_ERROR:
         LOG_ERROR("OTA_NOTIF_COMMIT_ERROR {%d)", pParams->err.errorCode);
         break;

    default:
        LOG_INFO("OTA_NOTIF not handled Id %d", notification);
        break;
    }
}

static void NetAppRequestErrorResponse(SlNetAppResponse_t *pNetAppResponse)
{
    LOG_WARNING("[Http server task] could not allocate memory for netapp request");

    /* Prepare error response */
    pNetAppResponse->Status = SL_NETAPP_HTTP_RESPONSE_500_INTERNAL_SERVER_ERROR;
    pNetAppResponse->ResponseData.pMetadata = NULL;
    pNetAppResponse->ResponseData.MetadataLen = 0;
    pNetAppResponse->ResponseData.pPayload = NULL;
    pNetAppResponse->ResponseData.PayloadLen = 0;
    pNetAppResponse->ResponseData.Flags = 0;
}

//*****************************************************************************
//
//! This function handles resource request
//!
//! \param[in]  pNetAppRequest  - Contains the resource requests
//! \param[in]  pNetAppResponse - Should be filled by the user with the
//!                               relevant response information
//!
//! \return     None
//!
//*****************************************************************************
void SimpleLinkNetAppRequestEventHandler(SlNetAppRequest_t *pNetAppRequest, SlNetAppResponse_t *pNetAppResponse)
{
    SlNetAppRequest_t *netAppRequest;
    int32_t msgqRetVal;

    LOG_DEBUG("APPREQUES HANDLER");

    if ((pNetAppRequest->Type == SL_NETAPP_REQUEST_HTTP_GET) || (pNetAppRequest->Type == SL_NETAPP_REQUEST_HTTP_DELETE) ||
            (pNetAppRequest->Type == SL_NETAPP_REQUEST_HTTP_POST) || (pNetAppRequest->Type == SL_NETAPP_REQUEST_HTTP_PUT))
    {
        /* Prepare pending response */
        pNetAppResponse->Status = SL_NETAPP_RESPONSE_PENDING;
        pNetAppResponse->ResponseData.pMetadata = NULL;
        pNetAppResponse->ResponseData.MetadataLen = 0;
        pNetAppResponse->ResponseData.pPayload = NULL;
        pNetAppResponse->ResponseData.PayloadLen = 0;
        pNetAppResponse->ResponseData.Flags = 0;
    }
    else
    {
        NetAppRequestErrorResponse(pNetAppResponse);

        return;
    }

    netAppRequest = (SlNetAppRequest_t *) malloc (sizeof(SlNetAppRequest_t));
    if (NULL == netAppRequest)
    {
        NetAppRequestErrorResponse(pNetAppResponse);

        return;
    }

    netAppRequest->AppId = pNetAppRequest->AppId;
    netAppRequest->Type = pNetAppRequest->Type;
    netAppRequest->Handle = pNetAppRequest->Handle;
    netAppRequest->requestData.Flags = pNetAppRequest->requestData.Flags;

    /* Copy Metadata */
    if (pNetAppRequest->requestData.MetadataLen > 0)
    {
        netAppRequest->requestData.pMetadata = (uint8_t *) malloc (pNetAppRequest->requestData.MetadataLen);
        if (NULL == netAppRequest->requestData.pMetadata)
        {
            NetAppRequestErrorResponse(pNetAppResponse);

            return;
        }
        sl_Memcpy(netAppRequest->requestData.pMetadata, pNetAppRequest->requestData.pMetadata, pNetAppRequest->requestData.MetadataLen);
        netAppRequest->requestData.MetadataLen = pNetAppRequest->requestData.MetadataLen;
    }
    else
    {
        netAppRequest->requestData.MetadataLen = 0;
    }

    /* Copy the payload */
    if (pNetAppRequest->requestData.PayloadLen > 0)
    {
        netAppRequest->requestData.pPayload = (uint8_t *) malloc (pNetAppRequest->requestData.PayloadLen);
        if (NULL == netAppRequest->requestData.pPayload)
        {
            NetAppRequestErrorResponse(pNetAppResponse);

            return;
        }
        sl_Memcpy (netAppRequest->requestData.pPayload, pNetAppRequest->requestData.pPayload, pNetAppRequest->requestData.PayloadLen);
        netAppRequest->requestData.PayloadLen = pNetAppRequest->requestData.PayloadLen;
    }
    else
    {
        netAppRequest->requestData.PayloadLen = 0;
    }

    msgqRetVal = mq_send(pWlanDev->httpserverQueue, (char *)&netAppRequest, 1, 0);
    if(msgqRetVal < 0) {
        LOG_WARNING("[Http server task] could not send element to msg queue");
    }

}

void SimpleLinkNetAppRequestMemFreeEventHandler(uint8_t *buffer)
{
    LOG_DEBUG("APPREQUEST FREE MEM");
}

//*****************************************************************************
//
//! This function handles HTTP server events
//!
//! \param[in]  pServerEvent     - Contains the relevant event information
//! \param[in]  pServerResponse  - Should be filled by the user with the
//!                                relevant response information
//!
//! \return None
//!
//****************************************************************************
void SimpleLinkHttpServerEventHandler(SlNetAppHttpServerEvent_t *pHttpEvent, SlNetAppHttpServerResponse_t *pHttpResponse)
{
    /* Unused in this application                                             */
    LOG_DEBUG("HTTP SERVER\r\n");
}

//*****************************************************************************
//
//! This function handles socket events indication
//!
//! \param[in]  pSock - Pointer to Socket Event Info
//!
//! \return None
//!
//*****************************************************************************
void SimpleLinkSockEventHandler(SlSockEvent_t *pSock)
{
    /* This application doesn't work w/ socket - Events are not expected      */
    switch(pSock->Event)
    {
    case SL_SOCKET_TX_FAILED_EVENT:
        switch(pSock->SocketAsyncEvent.SockTxFailData.Status)
        {
        case SL_ERROR_BSD_ECLOSE:
            LOG_DEBUG("CLOSE ERROR");
//            Display_printf(display, 0, 0,
//                           "[SOCK ERROR] - close socket (%d) operation "
//                           "failed to transmit all queued packets\n\r",
//                           pSock->SocketAsyncEvent.SockTxFailData.Sd);
            break;
        default:
            LOG_DEBUG("OTHER ERROR %d", pSock->SocketAsyncEvent.SockTxFailData.Status);
//            Display_printf(display, 0, 0,
//                           "[SOCK ERROR] - TX FAILED  :  socket %d , "
//                           "reason (%d) \n\n",
//                           pSock->SocketAsyncEvent.SockTxFailData.Sd,
//                           pSock->SocketAsyncEvent.SockTxFailData.Status);
            break;
        }
        break;

    default:
        LOG_DEBUG("unexpected event: %d", pSock->Event);
//        LOG_DEBUG_long_hex(pSock->Event);
//        LOG_DEBUG("\r\n");
//        Display_printf(display, 0, 0,
//                       "[SOCK EVENT] - Unexpected Event [%x0x]\n\n",
//                       pSock->Event);
        break;
    }
}

//*****************************************************************************
//
//! \brief  SlWifiConn Event Handler
//!
//*****************************************************************************
static void SlWifiConnEventHandler(WifiConnEventId_e eventId , WifiConnEventData_u *pData)
{
    uint8_t  mac[SL_MAC_ADDR_LEN];
    uint16_t macAddressLen;

    switch(eventId) {
    case WifiConnEvent_POWERED_UP:
    {
        LOG_INFO("[SlWifiConnEventHandler] POWERED_UP ");
        macAddressLen = sizeof(mac);
        sl_NetCfgGet(SL_NETCFG_MAC_ADDRESS_GET,NULL,&macAddressLen,
                     (unsigned char *)mac);
        LOG_INFO("  MAC address: %x:%x:%x:%x:%x:%x",
                    mac[0],
                    mac[1],
                    mac[2],
                    mac[3],
                    mac[4],
                    mac[5]                            );
        memcpy(pWlanDev->macAddress, mac, SL_MAC_ADDR_LEN);
        netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_RUNNING);
    }
    break;

    case WifiConnEvent_POWERED_DOWN:
        netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_STOPPED);
        LOG_INFO("[SlWifiConnEventHandler] POWERED DOWN "_CLR_RESET_);
        break;

    case WifiConnEvent_PROVISIONING_STARTED:
//        gIsProvsioning = 1;
        LOG_INFO("[SlWifiConnEventHandler] PROVISIONING STARTED !\n\r"
                    "      mode=%d (0-AP, 1-SC, 2-AP+SC, 3-AP+SC+EXT)" _CLR_RESET_, pData->provisioningCmd);
        break;

    case WifiConnEvent_PROVISIONING_STOPPED:
//        gIsProvsioning = 0;
        LOG_INFO("[SlWifiConnEventHandler] PROVISIONING_STOPPED !\n\r"
                    "      status = %d (0-SUCCESS, 1-FAILED, 2-STOPPED)"_CLR_RESET_, pData->status);
        break;

    case WifiConnEvent_EXTERNAL_PROVISIONING_START_REQ:
        LOG_INFO("[SlWifiConnEventHandler] START EXT PROVISIONING !"_CLR_RESET_);
        break;

    case WifiConnEvent_EXTERNAL_PROVISIONING_STOP_REQ:
        LOG_INFO("[SlWifiConnEventHandler] STOP EXT PROVISIONING !"_CLR_RESET_);
        break;

#ifdef WIFI_STATS
    case WifiConnEvent_STATS_PAUSE:
        LOG_DEBUG("STATS PAUSE - pausing network clients");
        pWlanDev->event(pWlanDev->object, NETWORK_EVENT_PAUSE, 0);
        break;

    case WifiConnEvent_STATS_RESUME:
        LOG_DEBUG("STATS RESUME - resuming network clients");
        pWlanDev->event(pWlanDev->object, NETWORK_EVENT_RESUME, 0);
        break;

    case WifiConnEvent_STATS_RESULT:
    {
        /* Statistics received from slwificonn - post directly to MQTT */
        SlWifiConnStats_t *pStats = (SlWifiConnStats_t*)pData;
        LOG_DEBUG("STATS: CTRL=%d, MGMT=%d", pStats->dataCtrlRssi, pStats->mgMntRssi);

        mqtt_post_wlan_data(pStats->bssid, pStats->mgMntRssi, pStats->dataCtrlRssi,
                            pWlanDev->totalFatalErrors, pWlanDev->totalRecoveries, pWlanDev->totalDisconnects,
                            pWlanDev->connectedTime / 1000);
    }
    break;
#endif

    default:
        LOG_INFO("[SlWifiConnEventHandler] UNKNOWN EVENT "_CLR_RESET_);
    }
}

//*****************************************************************************
//
//! \brief  Thread context for the SlWifiConn
//!
//! \note   The SlWifiConn_pocess only returns when the module is destoryed
//!         (see WIFI_IF_deinit)
//!
//*****************************************************************************
static void *SlWifiConnTask(void *pvParameters)
{
    void* retVal = SlWifiConn_process(pvParameters);
    pthread_exit(NULL);
    return retVal;
}

/* state machine */
static CC3220SF_STATE_t netdev_wlan_cc3220sf_sm_init( void )
{
    int32_t ret;

    LOG_DEBUG("INIT");
    pWlanDev->pstate = CC3220SF_STATE_INIT;

    if(IS_RUNNING(pWlanDev->wlanStatus)) {
        SlWifiConn_disable();
        LOG_DEBUG("INIT -> stopping");
        do {
            Task_sleep(1000);
        } while(IS_RUNNING(pWlanDev->wlanStatus));
    }

    memset(pWlanDev->macAddress, 0x00, 6);
    memset(&pWlanDev->securityParams, 0, sizeof(SlWlanSecParams_t));
    memset(pWlanDev->SSIDremoteName, '\0', sizeof(pWlanDev->SSIDremoteName));
    pWlanDev->stationIP = 0;
    pWlanDev->gatewayIP = 0;
    pWlanDev->netIf = SlNetIf_getIfByID(SLNETIF_ID_1);

    pWlanDev->SecurityKey[0] = '\0';
    pWlanDev->securityParams.Key = (signed char *)pWlanDev->SecurityKey;
    pWlanDev->securityParams.KeyLen = strlen(pWlanDev->SecurityKey);
    pWlanDev->securityParams.Type = SL_WLAN_SEC_TYPE_WPA_WPA2;

    return CC3220SF_STATE_START;
}


static CC3220SF_STATE_t netdev_wlan_cc3220sf_sm_start( void )
{
    CC3220SF_EVENT_t event = netdev_wlan_cc3220sf_recv_event();
    int32_t ret = 0;

    LOG_DEBUG("START");
    pWlanDev->pstate = CC3220SF_STATE_START;

    if(GET_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_FW_INSTALL)) {
        OTA_IF_install();
        SlWifiConn_deinit();
        pWlanDev->timer = 0;
        return CC3220SF_STATE_WAIT_FW_INSTALL;
    }

    // we might already be running
    if(IS_RUNNING(pWlanDev->wlanStatus) == 0) {
        SlWifiConn_enable();
    }

    pWlanDev->nstate = CC3220SF_STATE_CHECK_PROFILES;
    pWlanDev->timer = 0;
    return CC3220SF_STATE_WAIT_4_START;
}

static CC3220SF_STATE_t netdev_wlan_cc3220sf_sm_wait_4_start( void )
{
    CC3220SF_EVENT_t event = netdev_wlan_cc3220sf_recv_event();

    if((pWlanDev->timer % 1000) == 0) {
        LOG_DEBUG("WAIT 4 START");
    }

    if(IS_RUNNING(pWlanDev->wlanStatus)) {
        pWlanDev->timer = 0;
        return pWlanDev->nstate;
    }

    if((pWlanDev->timer >= GENERAL_TIMEOUT) && (IS_RUNNING(pWlanDev->wlanStatus) == 0)) {
        // something else
        pWlanDev->nstate = CC3220SF_STATE_INIT;
        pWlanDev->timer = 0;
        pWlanDev->sleepTime = 1000;
        return CC3220SF_STATE_SLEEP;
    }

    return CC3220SF_STATE_WAIT_4_START;
}

static CC3220SF_STATE_t netdev_wlan_cc3220sf_sm_stop( void )
{
    CC3220SF_EVENT_t event = netdev_wlan_cc3220sf_recv_event();
    int32_t ret = 0;
    uint8_t to = 10;

    LOG_DEBUG("STOPPING");

    sl_NetAppStop(SL_NETAPP_HTTP_SERVER_ID);
    SlWifiConn_disable();

    pWlanDev->pstate = CC3220SF_STATE_STOP;

    while(IS_RUNNING(pWlanDev->wlanStatus) && (to > 0)) {
        Task_sleep(500);
        event = netdev_wlan_cc3220sf_recv_event();
        to--;
    }

    LOG_DEBUG("STOPPED");
    // reset status, we need preserve FW_INSTALL
    pWlanDev->wlanStatus &= (1 << STATUS_BIT_FW_INSTALL);
    return CC3220SF_STATE_INIT;
}

static CC3220SF_STATE_t netdev_wlan_cc3220sf_sm_check_profiles( void )
{
    /* Profile checking is now done synchronously in netdev_wlan_cc3220sf_start()
     * after SlWifiConn_init() returns. The sl_WlanProfileGet calls happen in
     * slwificonn.c during init where sl_* functions are safe to use.
     * This state just transitions to START_NETWORK.
     */
    LOG_DEBUG("CHECK PROFILES");
    pWlanDev->timer = 0;
    return CC3220SF_STATE_START_NETWORK;
}

static CC3220SF_STATE_t netdev_wlan_cc3220sf_sm_start_network( void )
{
    uint8_t len;
    pWlanDev->pstate = CC3220SF_STATE_START_NETWORK;

    LOG_DEBUG("START NETWORK");

    if(pWlanDev->doProvisioning) {

        LOG_DEBUG("START PROVISIONING");

        str_assy_hostname(pWlanDev->SSID, ROBOT_HOSTNAME_BASE, &pWlanDev->macAddress[3]);
        len = strlen((const char *)pWlanDev->SSID);
        sl_WlanSet(SL_WLAN_CFG_AP_ID, SL_WLAN_AP_OPT_SSID, len, (uint8_t *)pWlanDev->SSID);

        SlWifiConn_disable();
        return CC3220SF_STATE_WAIT_PROVISION;
    }

    return CC3220SF_STATE_WAIT_CONNECTAP;
}


static CC3220SF_STATE_t netdev_wlan_cc3220sf_sm_wait_connect_ap( void )
{
    CC3220SF_EVENT_t event = netdev_wlan_cc3220sf_recv_event();
    pWlanDev->pstate = CC3220SF_STATE_WAIT_CONNECTAP;

    if((pWlanDev->timer % 1000) == 0) {
        LOG_DEBUG("CONNECT AP WAIT");
    }

    if(IS_CONNECTED(pWlanDev->wlanStatus)) {
        pWlanDev->timer = 0;
        LOG_DEBUG("->WAIT IP");
        return CC3220SF_STATE_WAIT_IP;

    }

    if(pWlanDev->timer > 10000) {
        // we might retry first??
        pWlanDev->lastError = SL_ERROR_BSD_ETIMEDOUT;
        return CC3220SF_STATE_ERROR;
    }

    return CC3220SF_STATE_WAIT_CONNECTAP;
}

static CC3220SF_STATE_t netdev_wlan_cc3220sf_sm_wait_ip( void )
{
    CC3220SF_EVENT_t event = netdev_wlan_cc3220sf_recv_event();
    pWlanDev->pstate = CC3220SF_STATE_WAIT_IP;

    if((pWlanDev->timer % 1000) == 0) {
        LOG_DEBUG("WAIT IP");
    }

    if(IS_IP_ACQUIRED(pWlanDev->wlanStatus)) {
        LOG_DEBUG("->START NET");
        return CC3220SF_STATE_START_HTTP;
    }

    if(pWlanDev->timer > 5000) {
        // we might retry first??
        pWlanDev->lastError = SL_ERROR_BSD_ETIMEDOUT;
        return CC3220SF_STATE_ERROR;
    }

    return CC3220SF_STATE_WAIT_IP;
}

static CC3220SF_STATE_t netdev_wlan_cc3220sf_sm_start_httpserver( void )
{
    int32_t retVal = 0;

    LOG_DEBUG("START HTTP");

    http_init_handlers();

    retVal = sl_NetAppStart(SL_NETAPP_HTTP_SERVER_ID);
    if(retVal < 0) {
        retVal = sl_NetAppStart(SL_NETAPP_HTTP_SERVER_ID);
    }

    if(retVal < 0) {
        LOG_WARNING("HTTP SERVER FAILED");
    }

    pWlanDev->nstate = CC3220SF_STATE_IDLE;
    return CC3220SF_STATE_NOTIFY_UP;
}

static CC3220SF_STATE_t netdev_wlan_cc3220sf_sm_notifyup( void )
{
    LOG_DEBUG("NOTIFY UP");

    pWlanDev->timer = 0;

    /* Reset fatal error count on successful connection.
     * This allows the system to recover from transient issues
     * without triggering unnecessary reboots.
     */
    if(pWlanDev->fatalErrorCount > 0)
    {
        pWlanDev->totalRecoveries++;
        HWREG(NWP_STATS_REG_RECOVER) = pWlanDev->totalRecoveries;
        LOG_INFO("Connection restored after %d fatal error(s) (recoveries: %d, total errors: %d)",
                 pWlanDev->fatalErrorCount, pWlanDev->totalRecoveries, pWlanDev->totalFatalErrors);
        pWlanDev->fatalErrorCount = 0;
    }

    pWlanDev->connectedTime = 0;
    pWlanDev->event(pWlanDev->object, NETWORK_EVENT_CONNECTED, 0);

    return pWlanDev->nstate;
}

static CC3220SF_STATE_t netdev_wlan_cc3220sf_sm_idle( void )
{
    struct mq_attr mqstat;
    int32_t retval = mq_getattr(pWlanDev->httpserverQueue, &mqstat);

    if(retval == 0 && mqstat.mq_curmsgs > 0) {
        return CC3220SF_STATE_HTTP_MESSAGE;
    }

    retval = mq_getattr(pWlanDev->eventQ, &mqstat);
    if(retval == 0 && mqstat.mq_curmsgs > 0) {
        return CC3220SF_STATE_HANDLE_EVENT;
    }

    /* WIFI_STATS: Statistics collection is now handled by slwificonn.c
     * via SlWifiConn_enableStats(). Results are delivered via callback
     * to SlWifiConnEventHandler() which calls mqtt_post_wlan_data().
     */

    /* Publish NWP connection stats every 30 seconds */
    if(pWlanDev->timer >= 30000) {
        mqtt_post_wlan_data(pWlanDev->BSSIDconnection, 0, 0,
                            pWlanDev->totalFatalErrors, pWlanDev->totalRecoveries, pWlanDev->totalDisconnects,
                            pWlanDev->connectedTime / 1000);
        pWlanDev->timer = 0;
    }

    return CC3220SF_STATE_IDLE;
}

static CC3220SF_STATE_t netdev_wlan_cc3220sf_sm_handle_event( void )
{
    CC3220SF_EVENT_t event = netdev_wlan_cc3220sf_recv_event();

    if(event == CC3220SF_EVENT_ERROR) {
        // we need to restart comms
        LOG_ERROR("RESTART NEEDED");
    }
    else if(event == CC3220SF_EVENT_FW_UPGRADE) {
        LOG_DEBUG("EVENT FW UPGRADE");
    }
    else if(event == CC3220SF_EVENT_FW_INSTALL) {
        LOG_DEBUG("EVENT FW INSTALL");
        pWlanDev->timer = 0;
        pWlanDev->nstate = CC3220SF_STATE_STOP;
        return CC3220SF_STATE_NOTIFY_DOWN;
    }
    else if(event == CC3220SF_EVENT_FATAL_ERROR) {
        LOG_DEBUG("EVENT FATAL ERROR");
        SlWifiConn_requestReset();
        pWlanDev->timer = 0;
        pWlanDev->nstate = CC3220SF_STATE_STOP;
        return CC3220SF_STATE_NOTIFY_DOWN;
    }
    else {
        LOG_DEBUG("SM EVENT %d NOT HANDLED", event);
    }

    if(IS_REBOOT(pWlanDev->wlanStatus)) {
        pWlanDev->timer = 0;
        pWlanDev->nstate = CC3220SF_STATE_STOP;
        return CC3220SF_STATE_NOTIFY_DOWN;
    }

    return CC3220SF_STATE_IDLE;
}

static CC3220SF_STATE_t netdev_wlan_cc3220sf_sm_http_message( void )
{
    int32_t msgqRetVal;
    SlNetAppRequest_t *netAppRequest;

    msgqRetVal = mq_receive(pWlanDev->httpserverQueue, (char *)&netAppRequest, sizeof( SlNetAppRequest_t* ), NULL);
    if(msgqRetVal >= 0) {

        if ((netAppRequest->Type == SL_NETAPP_REQUEST_HTTP_GET) || (netAppRequest->Type == SL_NETAPP_REQUEST_HTTP_DELETE )) {

            if (netAppRequest->Type == SL_NETAPP_REQUEST_HTTP_GET) {
                LOG_DEBUG("HTTP SERVER GET");
            }
            else {
                LOG_DEBUG("HTTP SERVER DELETE");
            }

            http_get_handler(netAppRequest);
        }
        else if ((netAppRequest->Type == SL_NETAPP_REQUEST_HTTP_POST) || (netAppRequest->Type == SL_NETAPP_REQUEST_HTTP_PUT)) {

            if (netAppRequest->Type == SL_NETAPP_REQUEST_HTTP_POST) {
                LOG_DEBUG("HTTP SERVER POST");
            }
            else {
                LOG_DEBUG("HTTP SERVER PUT");
            }
            http_post_handler(netAppRequest);
        }

        if (netAppRequest->requestData.MetadataLen > 0) {
            free (netAppRequest->requestData.pMetadata);
        }

        if (netAppRequest->requestData.PayloadLen > 0) {
            free (netAppRequest->requestData.pPayload);
        }

        free (netAppRequest);
    }

    return CC3220SF_STATE_IDLE;
}


static CC3220SF_STATE_t netdev_wlan_cc3220sf_sm_check_connection( void )
{
    int32_t status = netdev_wlan_cc3220s_getConnectionStatus();

    if(status == SLNETIF_STATUS_DISCONNECTED) {
        LOG_DEBUG("DISCONNECTED");
    }
    else {
        LOG_DEBUG("CONENCTED");
    }

    pWlanDev->timer = 0;

    return CC3220SF_STATE_IDLE;
}

/* Statistics collection is now handled by slwificonn.c via callback.
 * This function is no longer used - kept for reference only.
 */
#if 0
static CC3220SF_STATE_t netdev_wlan_cc3220sf_sm_get_statistics( void )
{

    if(pWlanDev->timer > 2000) {
        int32_t status = netdev_wlan_cc3220s_getConnectionStats();

        sl_DeviceStatStop(0);

        LOG_DEBUG("STATUS : CTRL RSSI = %d, MGMT RSSI = %d",
                 pWlanDev->rxStatistics.AvarageDataCtrlRssi,
                 pWlanDev->rxStatistics.AvarageMgMntRssi);


//        sprintf(pWlanDev->loggerBuffer, "CTRL RSSI = %d, MGMT RSSI = %d",
//                pWlanDev->rxStatistics.AvarageDataCtrlRssi,
//                pWlanDev->rxStatistics.AvarageMgMntRssi);

        netlogger_send(pWlanDev->loggerBuffer);

        mqtt_post_wlan_data(pWlanDev->BSSIDconnection,
                            pWlanDev->rxStatistics.AvarageMgMntRssi,
                            pWlanDev->rxStatistics.AvarageDataCtrlRssi,
                            pWlanDev->totalFatalErrors, pWlanDev->totalRecoveries, pWlanDev->totalDisconnects,
                            pWlanDev->connectedTime / 1000);

        pWlanDev->timer = 0;
        return CC3220SF_STATE_IDLE;
    }

    return CC3220SF_STATE_GET_STATISTICS;
}
#endif

static CC3220SF_STATE_t netdev_wlan_cc3220sf_sm_notifydown( void )
{
    LOG_DEBUG("NOTIFY DOWN");
    pWlanDev->event(pWlanDev->object, NETWORK_EVENT_DISCONNECTED, 0);
    pWlanDev->timer = 0;
    pWlanDev->sleepTime = 2000;
    // pWlanDev->nstate must be set by previous state
    return pWlanDev->nstate;
}

static CC3220SF_STATE_t netdev_wlan_cc3220sf_sm_sleep( void )
{
    if(pWlanDev->timer >= pWlanDev->sleepTime) {
        return pWlanDev->nstate;
    }

    return CC3220SF_STATE_SLEEP;
}

static CC3220SF_STATE_t netdev_wlan_cc3220sf_sm_error( void )
{
    LOG_ERROR("ERROR: reason %s", netdev_wlan_cc3220sf_error_toString(pWlanDev->lastError));

    pWlanDev->pstate = CC3220SF_STATE_ERROR;
    pWlanDev->nstate = CC3220SF_STATE_STOP;
    pWlanDev->sleepTime = 2000;
    pWlanDev->timer = 0;

    return CC3220SF_STATE_SLEEP;
}

static CC3220SF_STATE_t netdev_wlan_cc3220sf_sm_wait_fw_install( void )
{
    CC3220SF_EVENT_t event = netdev_wlan_cc3220sf_recv_event();

    if((pWlanDev->timer % 1000) == 0) {
        LOG_DEBUG("WAIT FW INSTALL");
    }

    if(pWlanDev->timer > 5000) {
        OTA_IF_restart();
    }

    return CC3220SF_STATE_WAIT_FW_INSTALL;
}

static CC3220SF_STATE_t netdev_wlan_cc3220sf_sm_wait_provisioning( void )
{
    CC3220SF_EVENT_t event = netdev_wlan_cc3220sf_recv_event();
    uint8_t  provisioningCmd;
    uint32_t flags = 0;
    int32_t status;

    if((pWlanDev->timer % 1000) == 0) {
        LOG_DEBUG("WAIT PROVISIONING");
    }

    if(GET_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_PROV_MODE) == 0) {

        provisioningCmd = SL_WLAN_PROVISIONING_CMD_START_MODE_APSC;
        flags = SLWIFICONN_PROV_FLAG_FORCE_PROVISIONING; // this will delete all profiles
        status = SlWifiConn_enableProvisioning(WifiProvMode_ONE_SHOT, provisioningCmd, flags);
        SET_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_PROV_MODE);

        if(status != 0) {
            LOG_ERROR("FAILED TO START PROV: %d", status);
             // reboot
         }

        Task_sleep(500);
        SlWifiConn_enable();
    }


//    if(pWlanDev->timer > 2000) {
//        OTA_IF_restart();
//    }

    return CC3220SF_STATE_WAIT_PROVISION;
}


/* exported functions */
static int32_t netdev_wlan_cc3220sf_probe( struct network_dev *netdev, uint32_t param[], uint32_t params )
{
    //uint8_t i;
    pWlanDev = &cc3220sfDevice;

    netdev->delay = 100;
    netdev->priority = 5;
    netdev->stackSize = 4096;
    netdev->private = pWlanDev;
    pWlanDev->wlanRole = ROLE_STA;
    pWlanDev->wlanMode = ROLE_STA;
    pWlanDev->netdev = netdev;
    pWlanDev->parent = netdev->parent;
    pWlanDev->state = CC3220SF_STATE_INIT;
    pWlanDev->sleepTime = 5000;
    pWlanDev->lastError = 0;
    pWlanDev->doProvisioning = false;
    pWlanDev->fatalErrorCount = 0;

    /* Restore NWP stats from OCP spare registers (survive warm reboot) */
    pWlanDev->totalFatalErrors = HWREG(NWP_STATS_REG_FATAL);
    pWlanDev->totalRecoveries = HWREG(NWP_STATS_REG_RECOVER);
    pWlanDev->totalDisconnects = HWREG(NWP_STATS_REG_DISCON);
    LOG_INFO("NWP stats restored: errors=%d, recoveries=%d, disconnects=%d",
             pWlanDev->totalFatalErrors, pWlanDev->totalRecoveries, pWlanDev->totalDisconnects);

    return 0;
}

static int32_t netdev_wlan_cc3220sf_remove( struct network_dev *netdev )
{
    return 0;
}

static int32_t netdev_wlan_cc3220sf_notify( struct network_dev *netdev, void *object, eventNetdevCallback event )
{
    pWlanDev->object = object;
    pWlanDev->event = event;
    return 0;
}

#define SL_SPAWN_TASK_PRIORITY      (9)
#define SL_SPAWN_STACK_SIZE         (4096)
#define WIFI_CONN_TASK_PRIORITY     (7)
#define WIFI_CONN_STACK_SIZE        (2048)
#define EXT_PROV_TASK_PRIORITY      (7)
#define EXT_PROV_STACK_SIZE         (2048)

static int32_t netdev_wlan_cc3220sf_start( struct network_dev *netdev )
{
    //struct network_thread *nt = NULL;
    pthread_attr_t pAttrs_spawn;
    struct sched_param priParam;
    int32_t ret = STATUS_OK;
    mq_attr qAttr;

    LOG_DEBUG("START OPS");
    pWlanDev->wlanStatus = 0;

    /* Create message queue - zero init to avoid uninitialized fields */
    memset(&qAttr, 0, sizeof(qAttr));
    qAttr.mq_curmsgs = 0;
    qAttr.mq_flags = O_NONBLOCK;
    qAttr.mq_maxmsg = 10;
    qAttr.mq_msgsize = sizeof(uint8_t);
    pWlanDev->eventQ = mq_open("wlanQ", O_CREAT, 0, &qAttr);

    /* initializes mailbox for http messages */
    qAttr.mq_flags = 0;
    qAttr.mq_maxmsg = 1;         /* queue size */
    qAttr.mq_msgsize = sizeof( SlNetAppRequest_t* );      /* Size of message */
    pWlanDev->httpserverQueue = mq_open("httpserver msg q", O_CREAT, 0, &qAttr);
    if(pWlanDev->httpserverQueue == NULL) {
        LOG_ERROR("Failed to create http server Q");
        return STATUS_FAIL;
    }

    ret = SlNetIf_add(SLNETIF_ID_1, "CC32xx",
                        (const SlNetIf_Config_t *)&SlNetIfConfigWifi,
                        5);

    if(ret != 0) {
        LOG_ERROR("FAILED TO ADD INTERFACE");
        return STATUS_FAIL;
    }

    /* Create the sl_Task internal spawn thread */
    memset(&pAttrs_spawn, 0, sizeof(pAttrs_spawn));
    memset(&priParam, 0, sizeof(priParam));
    pthread_attr_init(&pAttrs_spawn);
    priParam.sched_priority = SL_SPAWN_TASK_PRIORITY;
    ret = pthread_attr_setschedparam(&pAttrs_spawn, &priParam);
    ret |= pthread_attr_setstacksize(&pAttrs_spawn, SL_SPAWN_STACK_SIZE);
    ret |= pthread_attr_setdetachstate(&pAttrs_spawn, PTHREAD_CREATE_DETACHED);

    ret = pthread_create(&cc3220sfDevice.slTask, &pAttrs_spawn, sl_Task, NULL);
    if(ret != 0) {
        LOG_ERROR("FAILED TO CREATE SL TASK");
        return STATUS_FAIL;
    }

    /* Create the wifi connection task */
    ret = SlWifiConn_init(SlWifiConnEventHandler);
    if(ret != 0) {
        LOG_ERROR("FAILED TO INIT WIFI CON : %d", ret);
        return STATUS_FAIL;
    }

    /* Check if profiles were found during SlWifiConn_init.
     * Profile checking is done in slwificonn.c after sl_Start() where sl_* calls are safe.
     */
    {
        extern int8_t SlWifiConn_getProfileCount(void);
        int8_t profileCount = SlWifiConn_getProfileCount();
        if(profileCount == 0) {
            LOG_WARNING("No WLAN profiles found - provisioning required");
            pWlanDev->doProvisioning = 1;
        } else if(profileCount > 0) {
            LOG_INFO("Found %d WLAN profile(s)", profileCount);
        }
        SET_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_CHECKED_PROFILES);
    }

    memset(&pAttrs_spawn, 0, sizeof(pAttrs_spawn));
    memset(&priParam, 0, sizeof(priParam));
    pthread_attr_init(&pAttrs_spawn);
    priParam.sched_priority = WIFI_CONN_TASK_PRIORITY;
    ret = pthread_attr_setschedparam(&pAttrs_spawn, &priParam);
    ret |= pthread_attr_setstacksize(&pAttrs_spawn, WIFI_CONN_STACK_SIZE);
    ret |= pthread_attr_setdetachstate(&pAttrs_spawn, PTHREAD_CREATE_DETACHED);

    ret = pthread_create(&cc3220sfDevice.wifiTask, &pAttrs_spawn, SlWifiConnTask, NULL);
    if(ret != 0) {
        LOG_ERROR("FAILED TO CREATE WIFI CON TASK");
        return STATUS_FAIL;
    }

     OTA_IF_init(NULL, SimpleLinkOtaEventHandler, 0, NULL);  // Disabled - out of heap memory

    pWlanDev->running = 1;
    return STATUS_OK;
}


static int32_t netdev_wlan_cc3220sf_stop( struct network_dev *netdev )
{
    LOG_DEBUG("STOP OPS");
    pWlanDev->running = 0;
    return 0;
}

static int32_t netdev_wlan_cc3220sf_exec( struct network_dev *netdev, uint32_t delay )
{
    CC3220SF_STATE_t s = pWlanDev->state;
    pWlanDev->timer += delay;

    switch(s) {
    case CC3220SF_STATE_INIT:
        s = netdev_wlan_cc3220sf_sm_init();
        break;
    case CC3220SF_STATE_START:
        s = netdev_wlan_cc3220sf_sm_start();
        break;
    case CC3220SF_STATE_WAIT_4_START:
        s = netdev_wlan_cc3220sf_sm_wait_4_start();
        break;
    case CC3220SF_STATE_STOP:
        s = netdev_wlan_cc3220sf_sm_stop();
        break;
    case CC3220SF_STATE_CHECK_PROFILES:
        s = netdev_wlan_cc3220sf_sm_check_profiles();
        break;
    case CC3220SF_STATE_WAIT_CONNECTAP:
        s = netdev_wlan_cc3220sf_sm_wait_connect_ap();
        break;
    case CC3220SF_STATE_WAIT_IP:
        s = netdev_wlan_cc3220sf_sm_wait_ip();
        break;
    case CC3220SF_STATE_START_NETWORK:
        s = netdev_wlan_cc3220sf_sm_start_network();
        break;
    case CC3220SF_STATE_START_HTTP:
        s = netdev_wlan_cc3220sf_sm_start_httpserver();
        break;
    case CC3220SF_STATE_NOTIFY_UP:
        s = netdev_wlan_cc3220sf_sm_notifyup();
        break;
    case CC3220SF_STATE_NOTIFY_DOWN:
        s = netdev_wlan_cc3220sf_sm_notifydown();
        break;
    case CC3220SF_STATE_IDLE:
        s = netdev_wlan_cc3220sf_sm_idle();
        break;
    case CC3220SF_STATE_SLEEP:
        s = netdev_wlan_cc3220sf_sm_sleep();
        break;
    case CC3220SF_STATE_ERROR:
        s = netdev_wlan_cc3220sf_sm_error();
        break;
    case CC3220SF_STATE_CHECK_CONNECTION:
        s = netdev_wlan_cc3220sf_sm_check_connection();
        break;
    case CC3220SF_STATE_HTTP_MESSAGE:
        s = netdev_wlan_cc3220sf_sm_http_message();
        break;
    case CC3220SF_STATE_HANDLE_EVENT:
        s = netdev_wlan_cc3220sf_sm_handle_event();
        break;
    /* Statistics collection now handled by slwificonn.c via callback */
#if 0
    case CC3220SF_STATE_GET_STATISTICS:
        s = netdev_wlan_cc3220sf_sm_get_statistics();
        break;
#endif
    case CC3220SF_STATE_WAIT_FW_INSTALL:
        s = netdev_wlan_cc3220sf_sm_wait_fw_install();
        break;
    case CC3220SF_STATE_WAIT_PROVISION:
        s = netdev_wlan_cc3220sf_sm_wait_provisioning();
        break;
    default:
        LOG_DEBUG("invalid state");
        s = CC3220SF_STATE_ERROR;
    }

    if(pWlanDev->state != s) {
        LOG_INFO("COS: %d -> %d", pWlanDev->state, s);
        pWlanDev->state = s;
    }

    if(IS_CONNECTED(pWlanDev->wlanStatus)) {
        pWlanDev->connectedTime += delay;
    }

    return 0;
}

static int32_t netdev_wlan_cc3220sf_power( struct network_dev *netdev )
{
    return 0;
}

static int32_t netdev_wlan_cc3220sf_ipconfig( struct network_dev *netdev, uint8_t address[], uint8_t netmask[], uint8_t gateway[], uint8_t dns[], uint8_t ntp[] )
{
    uint32_t status = GET_STATUS_BIT(pWlanDev->wlanStatus, STATUS_BIT_IP_ACQUIRED);
    uint32_t val;

    if(status) {
        val = SlNetUtil_ntohl(pWlanDev->stationIP);
        memcpy(address, &val, 4);
        val = SlNetUtil_ntohl(pWlanDev->gatewayIP);
        memcpy(gateway, &val, 4);
        memcpy(ntp, &val, 4);
        val = SlNetUtil_ntohl(pWlanDev->dnsIP);
        memcpy(dns, &val, 4);
        *(uint32_t *)netmask = 0x00ffffff;
    }
    else {
        *(uint32_t *)address = 0;
        *(uint32_t *)gateway = 0;
        *(uint32_t *)dns = 0;
        *(uint32_t *)netmask = 0;
        *(uint32_t *)ntp = 0;
    }

    return 0;
}

static int32_t netdev_wlan_cc3220sf_get_macaddress( struct network_dev *netdev, uint8_t macaddress[] )
{
    /*Get the device Mac if not previously set.*/
    if((pWlanDev->macAddress[0] == 0x00) &&
        (pWlanDev->macAddress[1] == 0x00) &&
        (pWlanDev->macAddress[2] == 0x00) &&
        (pWlanDev->macAddress[3] == 0x00) &&
        (pWlanDev->macAddress[4] == 0x00) &&
        (pWlanDev->macAddress[5] == 0x00)) {
         return STATUS_FAIL;
    }

    memcpy(macaddress, pWlanDev->macAddress, 6);
    return 6;
}

static int32_t netdev_wlan_cc3220sf_is_linkup( struct network_dev *netdev )
{
    if(IS_CONNECTED(pWlanDev->wlanStatus) && IS_IP_ACQUIRED(pWlanDev->wlanStatus)) {
        return 1;
    }

    return 0;
}

static int32_t netdev_wlan_cc3220sf_ioctl( struct network_dev *netdev, uint32_t code, uint32_t param )
{

//    LOG_DEBUG("IOCTL CODE=%d, PARM=%d", code, param);

    switch(code) {
    case WLAN_IOCTL_SET_MODE:
        pWlanDev->wlanMode = netdev_wlan_cc3220sf_to_wlanRole(param);

        if(param == WLAN_MODE_PROVISION) {
            LOG_DEBUG("PROVSIONING MODE!");
            pWlanDev->doProvisioning = true;
        }

        break;

    case WLAN_IOTCTL_FW_UPGRADE:
        netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_FW_UPGRADE);
        break;

    case WLAN_IOTCTL_FW_INSTALL:
        netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_FW_INSTALL);
        break;

    case WLAN_IOTCTL_WIFI_STATS:
#ifdef WIFI_STATS
        if(param > 0) {
            /* Enable WiFi statistics collection - runs in slwificonn context */
            SlWifiConn_enableStats(10);  /* 10 second interval */
        }
        else {
            SlWifiConn_disableStats();
        }
#endif
        break;
    }

    return 0;
}

static int32_t  netdev_wlan_cc3220sf_get_mode( struct network_dev *netdev )
{
    int32_t ret = NETWORK_MODE_DHCP;
    return ret;
}

static int32_t  netdev_wlan_cc3220sf_get_status( struct network_dev *netdev )
{
    uint32_t status = pWlanDev->wlanStatus;
    uint32_t provisioning = IS_PROV_MODE(status);
    uint32_t reboot = IS_REBOOT(status);
    uint32_t gotap = IS_CONNECTED(status);
    uint32_t gotip = IS_IP_ACQUIRED(status);
    uint32_t isrun = IS_RUNNING(status);
    uint32_t isfwdl_sta = IS_FW_DOWNLOAD_START(status);
    uint32_t isfwdl_end = IS_FW_DOWNLOAD_DONE(status);
    uint32_t isfwdl_err = IS_FW_DOWNLOAD_ERROR(status);
    int32_t ret = gotip ? NETWORK_STATUS_READY :
            gotap ?  NETWORK_STATUS_CONNECTED : isrun ?
                    NETWORK_STATUS_WAITAP : NETWORK_STATUS_OFFLINE;

    if(provisioning) {

        ret = NETWORK_STATUS_PROVISIONING_START;

        // in order of priority
        if(GET_STATUS_BIT(status, STATUS_BIT_PROV_SUCCESS)) {
            ret = NETWORK_STATUS_PROVISIONING_SUCCESS;
        }
        else if(GET_STATUS_BIT(status, STATUS_BIT_PROV_IP_ACQUIRED)) {
            ret = NETWORK_STATUS_PROVISIONING_IP_ACQUIRED;
        }
        else if(GET_STATUS_BIT(status, STATUS_BIT_PROV_CONNECT)) {
            ret = NETWORK_STATUS_PROVISIONING_CONNECT;
        }

//        if(GET_STATUS_BIT(status, STATUS_BIT_PROV_FAILED)) {
//            ret = NETWORK_STATUS_PROVISIONING_FAIL;
//        }

    }

//    if(reboot) {
//        ret = NETWORK_STATUS_REBOOT;
//    }

    if(isfwdl_sta | isfwdl_end | isfwdl_err) {

        // most to least priority
        if(isfwdl_err) {
            ret = NETWORK_STATUS_FW_DOWNLOAD_ERROR;
        }
        else if(isfwdl_end) {
            ret = NETWORK_STATUS_FW_DOWNLOAD_COMPLETE;
        }
        else if(isfwdl_sta) {
            ret = NETWORK_STATUS_FW_DOWNLOAD_START;
        }

    }

    return ret;
}

struct network_dev_operations NetDevWlanCC3220SF =
{
     .probe = netdev_wlan_cc3220sf_probe,
     .remove = netdev_wlan_cc3220sf_remove,
     .notify = netdev_wlan_cc3220sf_notify,
     .start = netdev_wlan_cc3220sf_start,
     .stop = netdev_wlan_cc3220sf_stop,
     .exec = netdev_wlan_cc3220sf_exec,
     .ioctl = netdev_wlan_cc3220sf_ioctl,
     .power = netdev_wlan_cc3220sf_power,
     .ipconfig = netdev_wlan_cc3220sf_ipconfig,
     .getmacaddress = netdev_wlan_cc3220sf_get_macaddress,
     .islinkup = netdev_wlan_cc3220sf_is_linkup,
     .getmode = netdev_wlan_cc3220sf_get_mode,
     .getstatus = netdev_wlan_cc3220sf_get_status
};


#endif

/*
 *
 *
 * [WIFI::DEBUG]  [Provisioning] Profile Added: SSID:
[WIFI::DEBUG] GOT EVENT 10
[WIFI::DEBUG] EVENT NOT HANDLED 10
[WIFI::DEBUG]  [Provisioning] Profile confirmation: WLAN Connected!
[WIFI::DEBUG] GOT EVENT 17
[WIFI::DEBUG]  [Provisioning] Profile confirmation: IP Acquired!
[WIFI::DEBUG] GOT EVENT 19
[WIFI::DEBUG]  [Provisioning] Profile Confirmation Success!
[WIFI::DEBUG] GOT EVENT 13
[WIFI::INFO]   Provisioning stopped: Current Role: STA
[WIFI::DEBUG] WLAN Status: CONNECTED
[WIFI::DEBUG] Connected to SSID: Defender
[WIFI::DEBUG] GOT EVENT 14
[WIFI::DEBUG] APPREQUES HANDLER
 *
 *
 * */


/*
 *  sl_WlanProfileAdd()
 *   lRetVal = sl_WlanPolicySet(SL_POLICY_CONNECTION, SL_CONNECTION_POLICY(0,0,0,0,0), 0, 0);
 *  // Clear all stored profiles and reset the policies
 *  lRetVal = sl_WlanProfileDel(0xFF);
 *
 *
//    sl_NetAppSet(SL_NETAPP_DEVICE_ID, SL_NETAPP_DEVICE_DOMAIN, len, (unsigned char*)str1);
 *
 *
 *  unsigned char str1[32] = "mynewsimplelink.net";
    unsigned char len = strlen((const char *)str1);
    // Use the correct API constants from the SimpleLink SDK
    retVal = sl_NetAppSet(SL_NETAPP_DEVICE_ID, SL_NETAPP_DEVICE_DOMAIN, len, (unsigned char*)str1);
 * */

/*
 *  But via URN API you can set name for mDNS and DHCP hostname (Option 12).
 * _i16 Status;
_u8 *device_name = "MY-SIMPLELINK-DEV";
Status = sl_NetAppSet (SL_NETAPP_DEVICE_ID,SL_NETAPP_DEVICE_URN, strlen(device_name), (_u8 *)
device_name);
 *
 *
 //    sl_WlanSet(SL_WLAN_CFG_GENERAL_PARAM_ID
//                            SL_WLAN_GENERAL_PARAM_OPT_SCAN_PARAMS_5G,
//                            sizeof(SetPolicyParams.ScanParamConfig5G),
//                            (uint8_t *)(&SetPolicyParams.ScanParamConfig5G));
 * */
