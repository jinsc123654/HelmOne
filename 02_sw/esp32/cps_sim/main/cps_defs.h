/* BLE Cycling Power service - the parts a power meter uses.
 *
 * Values are from the adopted CPS profile (service 0x1818).
 */
#ifndef CPS_DEFS_H
#define CPS_DEFS_H

/* --- Service 0x1818: Cycling Power ------------------------------------- */
#define UUID_CPS_SVC    0x1818
#define UUID_CPS_MEAS   0x2A63  /* Cycling Power Measurement : notify (+read) */
#define UUID_CPS_FEAT   0x2A65  /* Cycling Power Feature     : read            */
#define UUID_CPS_LOC    0x2A5D  /* Sensor Location           : read            */
#define UUID_CPS_CP     0x2A55  /* SC Control Point          : write, indicate */

/* Cycling Power Measurement, 16 bit flags (little endian, bytes 0..1).
 * The instantaneous power always follows them at bytes 2..3. */
#define CPS_FLAG_PEDAL_BALANCE     0x0001
#define CPS_FLAG_PEDAL_BALANCE_REF 0x0002
#define CPS_FLAG_TORQUE            0x0004
#define CPS_FLAG_TORQUE_SRC         0x0008
#define CPS_FLAG_WHEEL_REV         0x0010
#define CPS_FLAG_CRANK_REV         0x0020
#define CPS_FLAG_EXTREME_FORCE     0x0040
#define CPS_FLAG_EXTREME_TORQUE    0x0080
#define CPS_FLAG_EXTREME_ANGLES    0x0100
#define CPS_FLAG_TOP_DEAD_SPOT     0x0200
#define CPS_FLAG_BOTTOM_DEAD_SPOT  0x0400
#define CPS_FLAG_ACCUM_ENERGY      0x0800
#define CPS_FLAG_OFFSET_INDICATOR  0x1000

/* Cycling Power Feature, 32 bit little endian */
#define CPS_FEAT_PEDAL_BALANCE     0x00000001
#define CPS_FEAT_ACCUM_TORQUE      0x00000002
#define CPS_FEAT_WHEEL_REV         0x00000004
#define CPS_FEAT_CRANK_REV         0x00000008
#define CPS_FEAT_EXTREME_MAG       0x00000010
#define CPS_FEAT_DEAD_SPOT         0x00000020
#define CPS_FEAT_ACCUM_ENERGY      0x00000040
#define CPS_FEAT_OFFSET_COMP       0x00000080

/* Sensor Location (0x2A5D) */
#define CPS_LOC_OTHER         0x00
#define CPS_LOC_LEFT_CRANK    0x05
#define CPS_LOC_RIGHT_CRANK   0x06
#define CPS_LOC_LEFT_PEDAL    0x07
#define CPS_LOC_RIGHT_PEDAL   0x08
#define CPS_LOC_CHAINSTAY     0x0B
#define CPS_LOC_REAR_HUB      0x0D
#define CPS_LOC_SPIDER        0x0F

/* SC Control Point (0x2A55) op codes and response codes. */
#define CPS_CP_OP_SET_WHEEL_REVS        0x01
#define CPS_CP_OP_UPDATE_SENSOR_LOC     0x02
#define CPS_CP_OP_REQ_SUPPORTED_LOCS    0x03
#define CPS_CP_OP_SET_CRANK_LENGTH      0x04
#define CPS_CP_OP_REQ_CRANK_LENGTH      0x05
#define CPS_CP_OP_SET_CHAIN_LENGTH      0x06
#define CPS_CP_OP_REQ_CHAIN_LENGTH      0x07
#define CPS_CP_OP_SET_CHAIN_WEIGHT      0x08
#define CPS_CP_OP_REQ_CHAIN_WEIGHT      0x09
#define CPS_CP_OP_SET_SPAN_LENGTH       0x0A
#define CPS_CP_OP_REQ_SPAN_LENGTH       0x0B
#define CPS_CP_OP_START_OFFSET_COMP     0x0C
#define CPS_CP_OP_MASK_MEAS_CONTENT     0x0D
#define CPS_CP_OP_REQ_SAMPLING_RATE     0x0E
#define CPS_CP_OP_REQ_FACTORY_CAL_DATE  0x0F
#define CPS_CP_OP_START_ENH_OFFSET_COMP 0x10
#define CPS_CP_OP_RESPONSE              0x20

#define CPS_CP_RESP_SUCCESS             0x01
#define CPS_CP_RESP_OP_NOT_SUPPORTED    0x02
#define CPS_CP_RESP_INVALID_PARAM       0x03
#define CPS_CP_RESP_OPERATION_FAILED    0x04
#define CPS_CP_RESP_CCC_IMPROPERLY_CFG  0x05

/* ATT error used by the profile when the control point indication CCC is off */
#define CPS_ATT_ERR_CCC_IMPROPERLY_CFG  0x81

/* --- Service 0x180F: Battery, service 0x180A: Device Information -------- */
#define UUID_BAS_SVC    0x180F
#define UUID_BAS_LEVEL  0x2A19  /* Battery Level : read, notify */

#define UUID_DIS_SVC    0x180A
#define UUID_DIS_MANUF   0x2A29
#define UUID_DIS_MODEL   0x2A24
#define UUID_DIS_FW_REV  0x2A26

#endif /* CPS_DEFS_H */
