/*
 * wlannet.h
 *
 *  Created on: 08 Jun 2022
 *      Author: janz
 */

#ifndef RESFW_DRIVERS_NETWORK_WLAN_WLANNET_H_
#define RESFW_DRIVERS_NETWORK_WLAN_WLANNET_H_


#define WLAN_MODE_NONE 0
#define WLAN_MODE_STATION 1
#define WLAN_MODE_AP 2
#define WLAN_MODE_P2P 3
#define WLAN_MODE_PROVISION 4

#define WLAN_IOCTL_SET_MODE 0x10

#define WLAN_IOTCTL_FW_UPGRADE 0x31
#define WLAN_IOTCTL_FW_INSTALL 0x32
#define WLAN_IOTCTL_WIFI_STATS 0x33

#ifdef NETDEV_TYPE_WIRELESS
#ifdef _NETWORK_DRIVER_TI_CC3320SF
extern struct network_dev_operations NetDevWlanCC3220SF;
#endif
#endif

/*
 * Defines the minimum severity level allowed.
 * Use E_DEBUG to enable Wifi internal messages
 * Options: E_TRACE, E_DEBUG, E_INFO, E_WARNING, E_ERROR, E_FATAL
 */
#define WIFI_IF_DEBUG_LEVEL         E_DEBUG


#endif /* RESFW_DRIVERS_NETWORK_WLAN_WLANNET_H_ */
