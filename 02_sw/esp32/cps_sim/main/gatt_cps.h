/* GATT server: the Cycling Power service a power meter presents, plus the
 * battery service that power-meter clients like to probe for liveness. */
#ifndef GATT_CPS_H
#define GATT_CPS_H

#include <stdbool.h>
#include <stdint.h>

/* Adds the services to the GATT database.  Call after nimble_port_init() and
 * after ble_svc_gap_init()/ble_svc_gatt_init(). */
int gatt_cps_init(void);

/* Queues one Cycling Power Measurement notification.  Safe to call from any
 * task: it only posts to the NimBLE event queue. */
void gatt_cps_kick_tx(void);

/* GAP bookkeeping. */
void gatt_cps_connected(uint16_t conn_handle);
void gatt_cps_disconnected(uint16_t conn_handle);
void gatt_cps_on_subscribe(uint16_t attr_handle, bool notify, bool indicate);

/* Sends the current simulated battery level if someone subscribed to it. */
void gatt_cps_notify_battery(void);

#endif /* GATT_CPS_H */
