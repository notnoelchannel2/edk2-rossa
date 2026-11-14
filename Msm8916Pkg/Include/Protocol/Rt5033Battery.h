/*
 * RT5033 battery/charger status, published by Rt5033ChargerDxe (rossa).
 */
#ifndef __RT5033_BATTERY_PROTOCOL_H__
#define __RT5033_BATTERY_PROTOCOL_H__

#define RT5033_BATTERY_PROTOCOL_GUID \
  { 0x3f6c2a1e, 0x8b4d, 0x4e7a, { 0x9c, 0x15, 0x6d, 0x2e, 0x0b, 0x7f, 0x41, 0xa8 } }

typedef enum {
  Rt5033ChgDischarging = 0,
  Rt5033ChgFull,
  Rt5033ChgCharging,
  Rt5033ChgNotCharging
} RT5033_CHG_STATE;

typedef struct {
  BOOLEAN          ChargerFound;
  BOOLEAN          GaugeFound;
  BOOLEAN          BatteryPresent;
  BOOLEAN          PowerSourceOnline; /* charger reports anything but "discharging" */
  BOOLEAN          FastCharge;        /* fast (CC/CV) vs pre-charge */
  BOOLEAN          SafetyStop;        /* driver put the charger in Hi-Z (over-voltage) */
  RT5033_CHG_STATE State;
  UINT8            ChargerId;         /* CHG reg 0x03 */
  UINT8            Capacity;          /* % (0-100) */
  UINT32           VoltageMv;         /* VBAT */
  UINT32           AvgVoltageMv;
  UINT32           OcvMv;
  UINT8            Regs[7];           /* charger 0x00-0x06 */
} RT5033_BATTERY_STATUS;

typedef struct _RT5033_BATTERY_PROTOCOL RT5033_BATTERY_PROTOCOL;

typedef EFI_STATUS (EFIAPI *RT5033_GET_STATUS)(
  IN  RT5033_BATTERY_PROTOCOL *This,
  OUT RT5033_BATTERY_STATUS   *Status
  );

struct _RT5033_BATTERY_PROTOCOL {
  UINT32            Revision;
  RT5033_GET_STATUS GetStatus;
};

extern EFI_GUID gRt5033BatteryProtocolGuid;

#endif
