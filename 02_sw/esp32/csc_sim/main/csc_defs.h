/* BLE Cycling Speed and Cadence service - the parts a cadence-only sensor uses.
 *
 * Values are from the adopted CSCS profile (service 0x1816).  They are spelled
 * out here instead of using NimBLE's service headers because those live outside
 * the public include path.
 */
#ifndef CSC_DEFS_H
#define CSC_DEFS_H

/* --- Service 0x1816: Cycling Speed and Cadence ------------------------- */
#define UUID_CSC_SVC    0x1816
#define UUID_CSC_MEAS   0x2A5B  /* CSC Measurement      : notify (+ read) */
#define UUID_CSC_FEAT   0x2A5C  /* CSC Feature          : read            */
#define UUID_CSC_LOC    0x2A5D  /* Sensor Location      : read            */
#define UUID_CSC_CP     0x2A55  /* SC Control Point     : write, indicate */

/* CSC Measurement, byte 0 */
#define CSC_MEAS_FLAG_WHEEL_REV   0x01  /* 4 B wheel revs + 2 B wheel event time follow */
#define CSC_MEAS_FLAG_CRANK_REV   0x02  /* 2 B crank revs + 2 B crank event time follow */

/* CSC Feature, 16 bit little endian */
#define CSC_FEAT_WHEEL_REV_DATA   0x0001
#define CSC_FEAT_CRANK_REV_DATA   0x0002
#define CSC_FEAT_MULTI_SENSOR_LOC 0x0004

/* Sensor Location (0x2A5D) */
#define CSC_LOC_OTHER         0x00
#define CSC_LOC_LEFT_CRANK    0x05
#define CSC_LOC_RIGHT_CRANK   0x06
#define CSC_LOC_CHAINSTAY     0x0B
#define CSC_LOC_REAR_HUB      0x0D

/* SC Control Point (0x2A55) op codes */
#define CSC_CP_OP_SET_WHEEL_REVS        0x01
#define CSC_CP_OP_START_CALIBRATION     0x02
#define CSC_CP_OP_UPDATE_SENSOR_LOC     0x03
#define CSC_CP_OP_REQ_SUPPORTED_LOCS    0x04
#define CSC_CP_OP_RESPONSE              0x10

/* SC Control Point response values (CSCS 1.0.1, table 3.6) */
#define CSC_CP_RESP_SUCCESS             0x01
#define CSC_CP_RESP_OP_NOT_SUPPORTED    0x02
#define CSC_CP_RESP_INVALID_PARAM       0x03
#define CSC_CP_RESP_OPERATION_FAILED    0x04
#define CSC_CP_RESP_CCC_IMPROPERLY_CFG  0x05

/* ATT error used by this profile when the control point indication CCC is off */
#define CSC_ATT_ERR_CCC_IMPROPERLY_CFG  0x81

/* --- Service 0x180F: Battery, service 0x180A: Device Information -------- */
#define UUID_BAS_SVC    0x180F
#define UUID_BAS_LEVEL  0x2A19  /* Battery Level : read, notify */

#define UUID_DIS_SVC    0x180A
#define UUID_DIS_MANUF   0x2A29
#define UUID_DIS_MODEL   0x2A24
#define UUID_DIS_FW_REV  0x2A26

#endif /* CSC_DEFS_H */
