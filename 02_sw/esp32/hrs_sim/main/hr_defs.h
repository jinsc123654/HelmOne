/* BLE Heart Rate service - the parts a chest strap uses.
 *
 * Values are from the adopted HRS profile (service 0x180D).
 */
#ifndef HR_DEFS_H
#define HR_DEFS_H

/* --- Service 0x180D: Heart Rate ---------------------------------------- */
#define UUID_HR_SVC     0x180D
#define UUID_HR_MEAS    0x2A37  /* Heart Rate Measurement   : notify (+read) */
#define UUID_HR_LOC      0x2A38  /* Body Sensor Location     : read            */
#define UUID_HR_CP       0x2A39  /* Heart Rate Control Point : write (optional) */

/* Heart Rate Measurement flags (byte 0) */
#define HR_FLAG_VALUE_U16           0x01  /* bpm is 16 bit instead of 8 bit   */
#define HR_FLAG_CONTACT_DETECTED    0x02  /* sensor contact currently detected */
#define HR_FLAG_CONTACT_SUPPORTED   0x04  /* this sensor reports contact      */
#define HR_FLAG_ENERGY_PRESENT      0x08  /* energy expended (kJ) follows     */
#define HR_FLAG_RR_PRESENT          0x10  /* RR intervals (1/1024 s) follow   */

/* Body Sensor Location (0x2A38) */
#define HR_LOC_OTHER        0x00
#define HR_LOC_CHEST        0x01
#define HR_LOC_WRIST        0x02
#define HR_LOC_FINGER       0x03
#define HR_LOC_HAND         0x04
#define HR_LOC_EARLOBE      0x05
#define HR_LOC_FOOT         0x06

/* Heart Rate Control Point (0x2A39) */
#define HR_CP_OP_RESET_ENERGY   0x01

/* --- Service 0x180F: Battery, service 0x180A: Device Information -------- */
#define UUID_BAS_SVC    0x180F
#define UUID_BAS_LEVEL  0x2A19  /* Battery Level : read, notify */

#define UUID_DIS_SVC    0x180A
#define UUID_DIS_MANUF   0x2A29
#define UUID_DIS_MODEL   0x2A24
#define UUID_DIS_FW_REV  0x2A26

#endif /* HR_DEFS_H */
