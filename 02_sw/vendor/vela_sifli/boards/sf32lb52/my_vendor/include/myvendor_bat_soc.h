/**
 * @file myvendor_bat_soc.h
 * @brief 单节锂电 SOC：软件 3.00 V～满电校准，开路电压分段表。
 *
 * NuttX 与 2SFBL 共用。电芯厂家截止 2.80 V，TPS63802 还能更低；产品 0%
 * 故意停在 3.00 V，留 200 mV 余量。满电点仍是 persist.battery.full_mv
 * （相对 4.20 V 参考曲线按比例拉伸）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MYVENDOR_BAT_SOC_H
#define MYVENDOR_BAT_SOC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYVENDOR_BAT_SOC_EMPTY_MV       3000  /**< 0%：软件截止（电芯 2.80 V）。 */
#define MYVENDOR_BAT_SOC_FULL_REF_MV    4200  /**< 曲线参考满电（未校准）。 */
#define MYVENDOR_BAT_SOC_CHG_DROP_MV      80  /**< 充电 IR/端电压抬升，仅用于 SOC。 */
#define MYVENDOR_BAT_SOC_STALE_FULL_MAX 4200  /**< 低于此的 KV 视为旧默认。 */
#define MYVENDOR_BAT_SOC_STALE_OBS_MIN  4300  /**< 本 ADC 已在 CV 附近。 */

/**
 * @brief 轻载 OCV 点（毫伏 @ 4.20 V 满电）。中间段平、两端陡。
 */
typedef struct myvendor_bat_soc_pt_s
{
  uint16_t mv;
  uint8_t  pct;
} myvendor_bat_soc_pt_t;

static const myvendor_bat_soc_pt_t MYVENDOR_BAT_SOC_CURVE[] =
{
  { 3000,   0 },
  { 3270,   5 },
  { 3500,  10 },
  { 3610,  20 },
  { 3690,  30 },
  { 3730,  40 },
  { 3780,  50 },
  { 3820,  60 },
  { 3870,  70 },
  { 3940,  80 },
  { 4030,  90 },
  { 4200, 100 },
};

/**
 * @brief 在 3.00～4.20 V 参考曲线上查 SOC。
 */
static inline int myvendor_bat_soc_lookup(int ref_mv)
{
  const myvendor_bat_soc_pt_t *t = MYVENDOR_BAT_SOC_CURVE;
  const unsigned n =
      (unsigned)(sizeof(MYVENDOR_BAT_SOC_CURVE) / sizeof(t[0]));
  unsigned i;
  int mv0;
  int mv1;
  int p0;
  int p1;

  if (ref_mv <= (int)t[0].mv)
    {
      return (int)t[0].pct;
    }

  if (ref_mv >= (int)t[n - 1u].mv)
    {
      return (int)t[n - 1u].pct;
    }

  for (i = 1; i < n; i++)
    {
      if (ref_mv > (int)t[i].mv)
        {
          continue;
        }

      mv0 = (int)t[i - 1u].mv;
      mv1 = (int)t[i].mv;
      p0 = (int)t[i - 1u].pct;
      p1 = (int)t[i].pct;
      if (mv1 <= mv0)
        {
          return p1;
        }

      return p0 + (int)(((int32_t)(ref_mv - mv0) * (p1 - p0)) / (mv1 - mv0));
    }

  return 100;
}

/**
 * @brief 把实测毫伏按当前满电校准映射到参考曲线再查表。
 */
static inline int myvendor_bat_soc_map_mv(int mv, int full)
{
  int ref_span;
  int span;
  int ref_mv;

  if (mv < 0)
    {
      return -1;
    }

  if (full <= MYVENDOR_BAT_SOC_EMPTY_MV)
    {
      full = MYVENDOR_BAT_SOC_FULL_REF_MV;
    }

  if (mv <= MYVENDOR_BAT_SOC_EMPTY_MV)
    {
      return 0;
    }

  if (mv >= full)
    {
      return 100;
    }

  ref_span = MYVENDOR_BAT_SOC_FULL_REF_MV - MYVENDOR_BAT_SOC_EMPTY_MV;
  span = full - MYVENDOR_BAT_SOC_EMPTY_MV;
  if (span <= 0)
    {
      return 100;
    }

  ref_mv = MYVENDOR_BAT_SOC_EMPTY_MV +
           (int)(((int32_t)(mv - MYVENDOR_BAT_SOC_EMPTY_MV) * ref_span) /
                 span);
  return myvendor_bat_soc_lookup(ref_mv);
}

/**
 * @brief 产品 SOC。充电且未充满时扣压降；满电校准仍用原始毫伏。
 *
 * @param charging 非 0：ETA9184 充电中。
 * @param charge_full 非 0：充满 IO（不扣压降）。
 */
static inline int myvendor_bat_soc_pct(int mv, int full, int charging,
                                       int charge_full)
{
  int soc_mv = mv;

  if (charging && !charge_full && soc_mv >= 0)
    {
      soc_mv -= MYVENDOR_BAT_SOC_CHG_DROP_MV;
      if (soc_mv < MYVENDOR_BAT_SOC_EMPTY_MV)
        {
          soc_mv = MYVENDOR_BAT_SOC_EMPTY_MV;
        }
    }

  return myvendor_bat_soc_map_mv(soc_mv, full);
}

#ifdef __cplusplus
}
#endif

#endif /* MYVENDOR_BAT_SOC_H */
