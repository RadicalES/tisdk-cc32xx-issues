/*
 * cc3120mod.h
 *
 *  Created on: 08 Jun 2022
 *      Author: janz
 */

#ifndef RESFW_DRIVERS_NETWORK_WLAN_CC3120MOD_CC3120MOD_H_
#define RESFW_DRIVERS_NETWORK_WLAN_CC3120MOD_CC3120MOD_H_


#define SSID_LEN_MAX            32
#define BSSID_LEN_MAX           6

void netdev_wlan_cc3220sf_slEventHandler(SlWlanEvent_t *pSlWlanEvent);
void netdev_wlan_cc3220sf_slFatalErrorEventHandler(SlDeviceFatal_t *slFatalErrorEvent);
void netdev_wlan_cc3220sf_slNetAppEventHandler(SlNetAppEvent_t *pNetAppEvent);
void netdev_wlan_cc3220sf_slNetAppRequestHandler(SlNetAppRequest_t *pNetAppRequest, SlNetAppResponse_t *pNetAppResponse);
void netdev_wlan_cc3220sf_slHttpServerCallback(SlNetAppHttpServerEvent_t *pHttpEvent, SlNetAppHttpServerResponse_t *pHttpResponse);
void netdev_wlan_cc3220sf_slGeneralEventHandler(SlDeviceEvent_t *pDevEvent);
void netdev_wlan_cc3220sf_slSockEventHandler(SlSockEvent_t *pSock);
void netdev_wlan_cc3220sf_slNetAppRequestMemFreeEventHandler(uint8_t *buffer);


#endif /* RESFW_DRIVERS_NETWORK_WLAN_CC3120MOD_CC3120MOD_H_ */
