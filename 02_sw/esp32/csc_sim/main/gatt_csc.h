/* GATT server: the CSC service a cadence meter presents, plus the battery
 * service that CSC-only clients like to probe for liveness.
 */
#ifndef GATT_CSC_H
#define GATT_CSC_H

#include <stdbool.h>
#include <stdint.h>

/* Adds the services to the GATT database.  Call after nimble_port_init() and
 * after ble_svc_gap_init()/ble_svc_gatt_init(). */
int gatt_csc_init(void);

/* Queues one CSC Measurement notification.  Safe to call from any task: it only
 * posts to the NimBLE event queue. */
void gatt_csc_kick_tx(void);

/* GAP bookkeeping. */
void gatt_csc_connected(uint16_t conn_handle);
void gatt_csc_disconnected(uint16_t conn_handle);
void gatt_csc_on_subscribe(uint16_t attr_handle, bool notify, bool indicate);

/* Sends the current simulated battery level if someone subscribed to it. */
void gatt_csc_notify_battery(void);

#endif /* GATT_CSC_H */
