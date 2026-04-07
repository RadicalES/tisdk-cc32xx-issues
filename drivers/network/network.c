/*
 * network.c
 *
 *  Created on: Oct 29, 2022
 *      Author: jan
 */

#include <stdio.h>
#include <stdint.h>

#include <os/os.h>
#include <os/xdc.h>

#include "netdevice.h"
#include "network.h"

int32_t network_get_MacAddress( uint8_t interface, uint8_t address[] )
{
    network_dev_t *nd = network_dev_getDevice(interface);
    int32_t ret = -1;

    if(nd != NULL) {
        ret = nd->ops->getmacaddress(nd, address);
    }

    return ret;
}

int32_t network_get_addresses( uint8_t interface, uint8_t *macAddress, uint8_t *ipAddress, uint8_t *netmask, uint8_t *gateway, uint8_t *dns, uint8_t *ntp )
{
    network_dev_t *nd = network_dev_getDevice(interface);
    int32_t ret = -1;

    if(nd != NULL) {
        if(nd->ops->getmacaddress(nd, macAddress) == 6) {
            ret = nd->ops->ipconfig(nd, ipAddress, netmask, gateway, dns, ntp);
        }
    }

    return ret;
}

int32_t network_get_status( uint8_t interface )
{
    network_dev_t *nd = network_dev_getDevice(interface);
    int32_t ret = -1;

    if(nd != NULL) {
        ret = nd->ops->getstatus(nd);
    }

    return ret;
}

int32_t network_iotcl( uint8_t interface, uint32_t code, uint32_t param )
{
    network_dev_t *nd = network_dev_getDevice(interface);
    int32_t ret = -1;

    if(nd != NULL) {
        ret = nd->ops->ioctl(nd, code, param);
    }

    return ret;
}

int32_t network_get_mode( uint8_t interface )
{
    network_dev_t *nd = network_dev_getDevice(interface);
    int32_t ret = -1;

    if(nd != NULL) {
        ret = nd->ops->getmode(nd);
    }

    return ret;
}

int32_t network_is_online( uint8_t interface )
{
    network_dev_t *nd = network_dev_getDevice(interface);
    int32_t ret = -1;

    if(nd != NULL) {
        ret = nd->ops->getstatus(nd);
    }

    return (ret == NETWORK_STATUS_READY) ? 1 : 0;
}

int32_t network_install_certificate( const char *certificate)
{
    return network_dev_install_certificate(certificate);
}

int32_t network_register_eventListener(uint8_t interface, void *object, eventNetworkCallback event)
{
    network_dev_t *nd = network_dev_getDevice(interface);
    int32_t ret = -1;

    if(nd != NULL) {
        ret = network_dev_register_eventListener(interface, object, event);
    }

    return ret;
}


