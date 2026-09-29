/* GATT server: the Heart Rate service a chest strap presents, plus the battery
 * service that clients like to probe for liveness. */
#ifndef GATT_HR_H
#define GATT_HR_H

#include <stdbool.h>
#include <stdint.h>

/* Adds the services to the GATT database.  Call after nimble_port_init() and
 * after ble_svc_gap_init()/ble_svc_gatt_init(). */
int gatt_hr_init(void);

/* Queues one Heart Rate Measurement notification.  Safe to call from any task:
 * it only posts to the NimBLE event queue. */
void gatt_hr_kick_tx(void);

/* GAP bookkeeping. */
void gatt_hr_connected(uint16_t conn_handle);
void gatt_hr_disconnected(uint16_t conn_handle);
void gatt_hr_on_subscribe(uint16_t attr_handle, bool notify, bool indicate);

/* Sends the current simulated battery level if someone subscribed to it. */
void gatt_hr_notify_battery(void);

#endif /* GATT_HR_H */
