/*
 * network.h
 *
 *  Created on: May 25, 2022
 *      Author: jan
 */

#ifndef SRC_RESFW_DRIVERS_NETWORK_NETWORK_H_
#define SRC_RESFW_DRIVERS_NETWORK_NETWORK_H_


#define _NETWORK_DRIVER_TI_NDK 0x20
#define _NETWORK_DRIVER_TI_CC3120MOD 0x21

// I/O Control codes
#define NETWORK_IOCTL_SET_PORT 1
#define NETWORK_IOCTL_SET_TIMEOUT 2
#define NETWORK_IOCTL_SET_TX_TIMEOUT 3
#define NETWORK_IOCTL_SET_RX_TIMEOUT 4
#define NETWORK_IOCTL_CONNECT 5
#define NETWORK_IOCTL_DISCONNECT 6

// Control Words
#define NETWORK_CW_TRANSACT 0x0010 /*!< Connection type is transactional, connect, request, response, discon */
#define NETWORK_CW_CLIENT_TCP 0x0001
#define NETWORK_CW_CLIENT_UDP 0x0002
#define NETWORK_CW_SERVER_UDP 0x0004
#define NETWORK_CW_SERVER_TCP 0x0008

// General definitions
#define NETWORK_MODE_STATIC 0x00
#define NETWORK_MODE_DHCP 0x01
#define NETWORK_MODE_UNDEFINED 0xff

#define NETWORK_STATUS_OFFLINE 0x00
#define NETWORK_STATUS_STARTED 0x01
#define NETWORK_STATUS_WAITAP 0x02
#define NETWORK_STATUS_CONNECTED 0x03
#define NETWORK_STATUS_DHCP_WAIT 0x04
#define NETWORK_STATUS_READY 0x05
#define NETWORK_STATUS_PROVISIONING_MODE 0x10
#define NETWORK_STATUS_PROVISIONING_START 0x11
#define NETWORK_STATUS_PROVISIONING_CONNECT 0x12
#define NETWORK_STATUS_PROVISIONING_IP_ACQUIRED 0x13
#define NETWORK_STATUS_PROVISIONING_WAIT 0x14
#define NETWORK_STATUS_PROVISIONING_SUCCESS 0x15
#define NETWORK_STATUS_PROVISIONING_FAIL 0x16

#define NETWORK_STATUS_REBOOT 0x20

#define NETWORK_STATUS_FW_DOWNLOAD_START 0x30
#define NETWORK_STATUS_FW_DOWNLOAD_COMPLETE 0x31
#define NETWORK_STATUS_FW_DOWNLOAD_ERROR 0x32
#define NETWORK_STATUS_FW_UPGRADE_START 0x33
#define NETWORK_STATUS_FW_UPGRADE_COMPLETE 0x34


#define NETWORK_LINK_DOWN 0x00
#define NETWORK_LINK_UP 0x01

// Events
#define NETWORK_EVENT_CONNECT 0x01
#define NETWORK_EVENT_CONNECTED 0x02
#define NETWORK_EVENT_IP_ADD 0x03
#define NETWORK_EVENT_IP_REMOVE 0x04
#define NETWORK_EVENT_DISCONNECTED 0x05
#define NETWORK_EVENT_PAUSE 0x06      // Pause network operations (for stats collection)
#define NETWORK_EVENT_RESUME 0x07     // Resume network operations

typedef int32_t (*eventNetworkCallback)( void *object, uint32_t code, uint32_t value );

// exported API
extern int32_t network_get_status( uint8_t interface );
extern int32_t network_is_online( uint8_t interface );
extern int32_t network_iotcl( uint8_t interface, uint32_t code, uint32_t param );
extern int32_t network_get_MacAddress( uint8_t interface, uint8_t address[] );
extern int32_t network_get_addresses( uint8_t interface, uint8_t *macAddress, uint8_t *ipAddress, uint8_t *netmask, uint8_t *gateway, uint8_t *dns, uint8_t *ntp );
extern int32_t network_get_mode( uint8_t interface );
extern int32_t network_install_certificate( const char *certificate);
extern int32_t network_register_eventListener(uint8_t interface, void *object, eventNetworkCallback event);

#endif /* SRC_RESFW_DRIVERS_NETWORK_NETWORK_H_ */
