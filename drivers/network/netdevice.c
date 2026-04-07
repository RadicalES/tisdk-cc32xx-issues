/*
 * netdevice.c
 *
 *  Created on: 08 Jun 2022
 *      Author: janz
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <os/xdc.h>
#include <os/os.h>

#include <ti/net/slnetif.h>
#include <ti/net/slnetutils.h>
#include <ti/net/slnetsock.h>
#include <ti/net/slnet.h>
#include <ti/drivers/net/wifi/simplelink.h>

#include "includes/errors.h"
#include <platform/platform.h>
#include <drivers/drivers.h>
#include <services/debug/debug.h>

#include "netdevice.h"

#define NETDEVICE_MAX_CERT_LENGTH 1800

extern network_dev_t NetworkDevs[];
extern uint32_t netdev_count;
typedef struct networkEventListener {
    eventNetworkCallback listener;
    void *object;
    uint8_t interface;
    struct networkEventListener *next;
} networkEventListener_t;

static struct networkEventListener *_netEventList = NULL;
static char _network_tls_certificate[NETDEVICE_MAX_CERT_LENGTH + 1];

static int32_t network_dev_eventCallback( void *object, uint32_t code, uint32_t value )
{
    struct networkEventListener *nl = _netEventList;
    while(nl != NULL) {

         if((nl->object != NULL) && (nl->listener != NULL)) {
             nl->listener(nl->object, code, value);
         }

         nl = nl->next;
     }

    return 0;
}

static void network_dev_threadWorkerStub(UArg arg0, UArg arg1)
{
    network_thread_t *t = (network_thread_t*)arg0;
    int32_t ret;

    do {
        ret = t->func(t);
        if((ret >= 0) && (t->delay > 0)) {
            Task_sleep(t->delay);
            t->timer += t->delay;
        }
    } while(ret >= 0);

//    Task_delete(&t->taskHandle);
}

static void network_dev_threadMainStub(UArg arg0, UArg arg1)
{
    network_dev_t *d = (network_dev_t*)arg0;

    if(d->ops->start(d) == 0) {

        d->ops->notify(d, d, network_dev_eventCallback);

        while(1) {
            if(d->ops->exec(d, d->delay) < 0) {
                break;
            }
            if(d->delay > 0) {
                Task_sleep(d->delay);
                d->timer += d->delay;
            }
        }
    }
    d->ops->stop(d);

//    Task_delete(&d->taskHandle);
}

static void network_dev_create_mainThread(network_dev_t *netdev, uint32_t priority, uint32_t stacksize)
{
    Error_Block eb;
    Task_Params params;

    Error_init(&eb);
    Task_Params_init(&params);

    netdev->priority = (uint8_t)priority;
    params.name = (char *)netdev->name;
    params.priority = -1;
    params.stackSize = stacksize;
    params.arg0 = (uintptr_t)netdev;
    netdev->taskHandle = Task_create((Task_FuncPtr)network_dev_threadMainStub, &params, &eb);
}

struct network_thread *network_dev_create_workerThread(network_dev_t *netdev, const char *name, uint32_t delay, uint32_t priority, uint32_t stacksize, funcNetworkThread function, void *object)
{
    Error_Block eb;
    Task_Params params;
    struct network_thread *nt;

    nt = (struct network_thread *)malloc(sizeof(struct network_thread));
    if(nt == NULL) {
        return NULL;
    }

    nt->netdev = netdev;
    nt->priority = priority;
    nt->stackSize = stacksize;
    nt->timer = 0;
    nt->delay = delay;
    nt->func = function;
    nt->next = NULL;
    nt->private = object;

    Error_init(&eb);
    Task_Params_init(&params);

    params.name = (char *)name;
    params.priority = priority;
    params.stackSize = stacksize;
    params.arg0 = (uintptr_t)nt;
    nt->taskHandle = Task_create((Task_FuncPtr)network_dev_threadWorkerStub, &params, &eb);

    return nt;
}

void network_dev_init( void )
{
    char msg[64];
    network_dev_t *ndev = &NetworkDevs[0];

    LOG_INFO("Loading network device drivers: \r\n");

    if(SlNetIf_init(0) != 0) {
        LOG_FATAL("\tFailed to initialize interfaces, aborting.\r\n");
        return;
    }

    if(SlNetSock_init(0) != 0) {
        LOG_FATAL("\tFailed to initialize s, aborting.\r\n");
        return;
    }

    if(SlNetUtil_init(0) != 0) {
        LOG_FATAL("\tFailed to initialize utilities, aborting.\r\n");
        return;
    }

    while(ndev->type != NETDEV_TYPE_NONE) {
        ndev->timer = 0;
        ndev->delay = 0;
        ndev->stackSize = NETWORK_DEFAULT_STACK_SIZE;
        ndev->priority = NETWORK_DEFAULT_PRIORITY;
        strcpy(msg, "\tProbing ");
        if(ndev->ops->probe(ndev, NULL, 0) == 0) {
#ifndef RESFW_NO_RTOS
            if(ndev->delay > 0) {
                LOG_INFO("%s %s (Threaded): OK\r\n", msg, ndev->name);
                network_dev_create_mainThread(ndev, ndev->priority, ndev->stackSize);
            }
            else {
                LOG_INFO("%s %s: OK\r\n", msg, ndev->name);
            }
#endif
        }
        else {
            LOG_ERROR("%s %s: FAIL\r\n", msg, ndev->name);
        }
        ndev++;
    }
}

network_dev_t *network_dev_getDevice( uint8_t interface )
{
    if(interface < netdev_count) {
        return &NetworkDevs[interface];
    }

    return NULL;
}

void network_dev_start( uint32_t provisioning )
{
    network_dev_t *ndev = &NetworkDevs[0];

//    LOG_DEBUG("Starting %s...", ndev->name);

    while(ndev->type != NETDEV_TYPE_NONE) {

        LOG_DEBUG("Starting %s, prov = %d", ndev->name, provisioning);

        ndev->ops->ioctl(ndev, WLAN_IOCTL_SET_MODE, ((provisioning & 0x01) == 0x01) ? WLAN_MODE_PROVISION : WLAN_MODE_STATION);
        Task_setPri(ndev->taskHandle, ndev->priority);
        ndev++;

    }
}

void network_dev_stop()
{
    network_dev_t *ndev = &NetworkDevs[0];

    LOG_DEBUG("Stopping %s...", ndev->name);

    while(ndev->type != NETDEV_TYPE_NONE) {
        ndev->ops->stop(ndev);
        ndev++;
    }
}

int32_t network_dev_register_eventListener(uint8_t interface, void *object, eventNetworkCallback event)
{
    struct networkEventListener *nl = _netEventList;
    struct networkEventListener *n = (struct networkEventListener *)malloc(sizeof(struct networkEventListener));

    if(n != NULL) {

        n->listener = event;
        n->object = object;
        n->interface = interface;
        n->next = NULL;

        if(_netEventList == NULL) {
            _netEventList = n;
        }
        else {
            while(nl->next != NULL) {
                nl = nl->next;
            }
            nl->next = n;
        }
    }

    return (n == NULL) ? 0 : -1;
}

static void network_dev_fix_certificate(  char *certificate, uint32_t length )
{
    uint32_t i;

    for(i=0; i<(length-2); i++) {

        if((certificate[i] == '\\') && (certificate[i + 1] == 'n')) {
            certificate[i] = '\r';
            certificate[i + 1] = '\n';
            i++;
        }

    }

}

int32_t network_dev_install_certificate( const char *certificate)
{
    int32_t ret = -1;
    uint32_t len = strlen(certificate);

    if(len < NETDEVICE_MAX_CERT_LENGTH) {
        strcpy(_network_tls_certificate, certificate);
        _network_tls_certificate[len + 1] = '\0';
        network_dev_fix_certificate(_network_tls_certificate, len);
        ret = SlNetIf_loadSecObj(SLNETIF_SEC_OBJ_TYPE_CERTIFICATE, "RootCA",
                       strlen("RootCA"), (uint8_t *)_network_tls_certificate, len + 1, SLNETIF_ID_2);
    }

    return ret;
}
