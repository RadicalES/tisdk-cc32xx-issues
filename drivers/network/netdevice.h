/*
 * netdevice.h
 *
 *  Created on: 08 Jun 2022
 *      Author: janz
 */

#ifndef RESFW_DRIVERS_NETWORK_NETDEVICE_H_
#define RESFW_DRIVERS_NETWORK_NETDEVICE_H_

#define _NETWORK_DRIVER_TI_NDK 0x20
#define _NETWORK_DRIVER_TI_CC3320SF 0x21

#define NETDEV_TYPE_NONE 0xff
#define NETDEV_TYPE_NOTSET 0x00
#define NETDEV_TYPE_ETHERNET 0x01
#define NETDEV_TYPE_WIRELESS 0x02

#define NETWORK_DEFAULT_STACK_SIZE 512
#define NETWORK_DEFAULT_PRIORITY 4

#include "wlan/wlannet.h"

struct network_dev;
struct network_helper;
struct network_thread;

typedef int32_t (*eventNetdevCallback)( void *object, uint32_t code, uint32_t value );
typedef int32_t (*funcNetworkThread)( struct network_thread *thread );

/*! \name Device exported operations
 */
//! @{
typedef struct network_dev_operations
{
    int32_t  (*probe)( struct network_dev *netdev, uint32_t param[], uint32_t params );
    int32_t  (*remove)( struct network_dev *netdev );
    int32_t  (*notify)( struct network_dev *netdev, void *object, eventNetdevCallback event );
    int32_t  (*ipconfig)( struct network_dev *netdev, uint8_t address[], uint8_t netmask[], uint8_t gateway[], uint8_t dns[], uint8_t ntp[] );
    int32_t  (*getmacaddress)( struct network_dev *netdev, uint8_t macaddress[] );
    int32_t  (*islinkup)( struct network_dev *netdev );
    int32_t  (*getstatus)( struct network_dev *netdev );
    int32_t  (*getmode)( struct network_dev *netdev );
    int32_t  (*start)( struct network_dev *netdev );
    int32_t  (*stop)( struct network_dev *netdev );
    int32_t  (*ioctl)( struct network_dev *netdev, uint32_t code, uint32_t param );
    int32_t  (*exec)( struct network_dev *netdev, uint32_t delay );
    int32_t  (*power)( struct network_dev *netdev );
} network_dev_operations_t;
//! @}

//! @{
typedef struct network_dev
{
   uint8_t type;                       /*!< type of device */
   char name[16];                  /*!< device name */
   uint8_t id;                      /* interface id */
   uint8_t offset;                  /* type offset */
   void *private;
   uint32_t delay;
   uint32_t timer;
   struct bus *parent;
#ifndef RESFW_NO_RTOS
   Task_Handle taskHandle;
   uint32_t stackSize;
   int8_t priority;
#endif
   struct network_dev_operations *ops;
   struct network_thread *threads;
} network_dev_t;
//! @}

//! \Network Descriptor Macro
// @{
#define NETWORK_DEV_DESC(NAME, ID, TYPE, OFFSET, PARENT, OPER) {         \
    .name   = NAME, \
    .id          = ID,                      \
    .type          = TYPE,                      \
    .offset          = OFFSET,                      \
    .parent      = PARENT,                      \
    .ops           = OPER \
}
// @}

struct network_thread;

typedef struct network_thread
{
    uint32_t delay;
    uint32_t timer;

#ifndef RESFW_NO_RTOS
    Task_Handle taskHandle;
   uint32_t stackSize;
   uint8_t priority;
#endif

    struct network_dev *netdev;
    void *private;
    funcNetworkThread func;
    struct network_thread *next;
} network_thread_t;

void network_dev_init( void );
void network_dev_start( uint32_t provisioning );
void network_dev_stop( void );
network_dev_t *network_dev_getDevice( uint8_t interface );
int32_t network_dev_register_eventListener(uint8_t interface, void *object, eventNetdevCallback event);
int32_t network_dev_install_certificate( const char *certificate);

struct network_thread *network_dev_create_workerThread(network_dev_t *netdev, const char *name, uint32_t delay, uint32_t priority, uint32_t stacksize, funcNetworkThread function, void *object);


#endif /* RESFW_DRIVERS_NETWORK_NETDEVICE_H_ */
