/**
 * @file myvendor_epic_blit.c
 * @brief EPIC 2D 搬运（拷贝 / 纯色填充）的板级封装。
 *
 * 为什么要有这一层 —— 都是被真实编译错误逼出来的：
 *   1. **芯片层头文件在 NSH 应用目标里不可见**：`bf0_hal_epic.h` 会拉进
 *      `register.h` / `bf0_hal_def.h`，而应用目标（如 `bicycle`）的 include
 *      路径里没有 `chips` 那一层（只有 `board` 目标有）⇒ 应用侧拿不到
 *      `EPIC_LayerConfigTypeDef` 这类类型；
 *   2. **LVGL 适配层的头文件同理**（`lv_sifli_epic_cfg.h` 也拉 `bf0_hal.h`）。
 *      所以这里按树里的既有做法（`ctl_main.c` / `sys_main.c` 用芯片层 API 时
 *      的处理）用**本地 extern** 声明那几个函数；签名照抄
 *      `apps/graphics/lvgl/lvgl/src/draw/sifli/epic/lv_sifli_epic_cfg.h`。
 *      ⚠ 那几个签名变了这里不会报错（没有编译期检查），改的时候一起改。
 *
 * 硬件约定都在这一层收敛（应用侧只认 RGB565 + 矩形）：
 *   - EPIC 是**独占硬件**，全树只有 `lv_sifli_epic_cfg.c` 里那一个 static
 *     `epic_handle`（`lv_epic_get_handle()`）。自己再建句柄会和 LVGL 自己的
 *     绘制路径抢同一堆寄存器，所以一律复用它；
 *   - 走 `lv_epic_blend()` / `lv_epic_fill()` 这两个包装：它们在起动前会
 *     等硬件空闲、卡住就复位、把 State 置回 READY，并 clean 一遍要读的区间；
 *   - 这两个包装底下是**同步**的（NuttX 上 `HAL_EPIC_BlendStartEx` /
 *     `HAL_EPIC_FillStart` 是 polling 版，内部 `EPIC_WaitDone()`），
 *     **返回时硬件必定已经停** —— 调用方不需要再等。
 */

#include <nuttx/config.h>
#include <nuttx/cache.h>

#include <stdbool.h>
#include <stdint.h>

#include "bf0_hal_epic.h"
#include "myvendor_epic_blit.h"
#include <syslog.h>

/* 本地 extern：声明在 LVGL 侧，见文件头注释。 */
EPIC_HandleTypeDef * lv_epic_get_handle(void);
HAL_StatusTypeDef lv_epic_fill(EPIC_LayerConfigTypeDef * input_layers,
  uint8_t input_layer_cnt, EPIC_LayerConfigTypeDef * output_layer);
HAL_StatusTypeDef lv_epic_blend(EPIC_LayerConfigTypeDef * input_layers,
  uint8_t input_layer_cnt, EPIC_LayerConfigTypeDef * output_layer);
void lv_epic_wait(void);

/** @brief 行跨度换算成 EPIC 要的"像素数"（本产品画布固定 RGB565 = 2 B/px）。 */
#define MYVENDOR_EPIC_PX_PER_ROW(stride)   ((uint16_t)((stride) / sizeof(uint16_t)))

/*
 * RGB565 → RGB888 的**位复制**展开（EPIC 的填充色分量是 RGB888）。
 *
 * 位复制在"截断"与"四舍五入"两种硬件做法下都能精确回读原来的 5/6 位：
 *   r5=31 → r8=(31<<3)|(31>>2)=255 → 255>>3=31；四舍五入 31*31/255=31 ✓
 *   g6=63 → g8=(63<<2)|(63>>4)=255 → 255>>2=63；四舍五入 63*63/255=63 ✓
 * 而直接 `r5<<3`（低位补 0）在四舍五入的做法下会掉到 30，所以不用它。
 * ⚠ LVGL 的 `lv_color_to_epic_color()` 是把 5/6 位**原样**塞进 8 位字段的，
 * 与本文件约定不同；谁对由 vmap_blit.c 的开机自检（fill 用例）实测。
 */
static uint8_t epic_expand5(uint16_t v)
{
  return (uint8_t)((v << 3) | (v >> 2));
}

static uint8_t epic_expand6(uint16_t v)
{
  return (uint8_t)((v << 2) | (v >> 4));
}

static uint8_t * epic_at(void * buf, uint32_t stride, int32_t x, int32_t y)
{
  return (uint8_t *)buf + (size_t)y * stride + (size_t)x * sizeof(uint16_t);
}

bool myvendor_epic_ready(void)
{
  return lv_epic_get_handle() != NULL;
}

/**
 * @brief  EPIC 写完目的矩形后，把**被写到的那些行**从 CPU 缓存里作废。
 * @param  rect_tl 目的矩形左上角像素指针（本层约定：`out.data` 就是它）。
 * @param  stride   目的缓冲的行跨度（字节）。
 * @param  w, h     矩形宽高（像素）。
 *
 * @details 为什么必须做（2026-09-27，PSRAM 写回审计的 G1）：EPIC 是**绕过 CPU 缓存**
 *          直接写内存的 master。CPU 只要在那些像素上有缓存行（写过或读过就会留下），
 *          读回来就是**旧像素**。目的缓冲在 PSRAM（画布 0x60E00000）时这条一定成立 ——
 *          PSRAM 是 cacheable，且产品默认已是写回（`CONFIG_MYVENDOR_PSRAM_CACHE_WB=y`）。
 *
 * @warning ① **只作废矩形内的行**，不要图省事整层作废 —— 矩形外的像素 CPU 可能正握着刚
 *          写的数据（写回模式下是脏行），一并作废就是**丢数据**；
 * @warning ② 只在缓冲位于 PSRAM 时做（判据同芯片的 `IS_DCACHED_RAM`：地址 ≥ 0x60000000；
 *          SRAM 不在缓存范围内，做了纯属浪费）；
 * @warning ③ 依赖"包装返回时硬件必定已停"这个既有约定（见文件头：NuttX 上的
 *          `HAL_EPIC_*Start*` 是 polling 版）—— 换成 `_IT` 版就必须挪到回调里。
 */
static void epic_invalidate_rect_rows(const void * rect_tl, uint32_t stride,
  int32_t w, int32_t h)
{
  int32_t i;

  if ((uintptr_t)rect_tl < 0x60000000u || w <= 0 || h <= 0)
    {
      return;
    }

  for (i = 0; i < h; i++)
    {
      uintptr_t row = (uintptr_t)rect_tl + (uintptr_t)i * stride;

      up_invalidate_dcache(row, row + (uintptr_t)w * sizeof(uint16_t));
    }
}

/* EPIC 等待超时 ⇒ 本次当作**失败**返回：上层 vmap_blit 的 CPU 兜底会把这一块
 * 重做一遍，所以既不会卡死、也不会在半搬完的画布上留错像素（用户 2026-09-26）。 */
extern volatile uint32_t g_epic_wait_timeouts;

static bool epic_result(uint32_t wt_before, bool hal_ok, const char * what)
{
  if (g_epic_wait_timeouts != wt_before)
    {
      syslog(LOG_WARNING,
             "[vblit] EPIC 等待超时（%s；累计 %u 次）⇒ 本次退回 CPU 重做\n",
             what, (unsigned)g_epic_wait_timeouts);
      return false;
    }

  return hal_ok;
}

bool myvendor_epic_copy(uint16_t * dst_buf, uint32_t dst_stride, int32_t dx,
  int32_t dy, const uint16_t * src_buf, uint32_t src_stride, int32_t sx,
  int32_t sy, int32_t w, int32_t h)
{
  const uint32_t wt_before = g_epic_wait_timeouts;
  EPIC_LayerConfigTypeDef in;
  EPIC_LayerConfigTypeDef out;

  if (lv_epic_get_handle() == NULL)
    {
      return false;
    }

  /* HAL 明确要求这两个偏移非负（`EPIC_ConfigLayer()` 里判负即 HAL_ERROR）。 */
  if (sx < 0 || sy < 0 || dx < 0 || dy < 0)
    {
      return false;
    }

  /* 坐标/尺寸寄存器是 16 位，且上层另有溢出检查（EPIC_COORDINATES_MAX）。 */
  if (sx + w > EPIC_COORDINATES_MAX || sy + h > EPIC_COORDINATES_MAX
      || dx + w > EPIC_COORDINATES_MAX || dy + h > EPIC_COORDINATES_MAX)
    {
      return false;
    }

  /*
   * ⚠ 这里的摆法是**踩过坑**的，改动前先看这段：
   *
   * EPIC 的混合语义是"**图层内容留在它自己的画布位置**"：对画布点 (X,Y)，
   * 它读 `in.data[X - in.x_offset]` 写到 `out.data[X - out.x_offset]`
   * （见 `EPIC_ConfigOutputLayer()` 里 `offset = (y0 - output->y_offset) *
   * output->total_width + (x0 - output->x_offset)` 那一段）。
   * 也就是说：**平移必须体现在 data 指针上，不能体现在 x_offset/y_offset 上**
   * —— 后者会让内容停在原地（那就成了"原地混合"，不是搬运）。
   * 实测（2026-09-25 开机自检）：把位移放进 offset 时，九个方向**全部**与 CPU
   * 不一致（EPIC 只在两个矩形的交集里写，落点还差了一个位移）。
   *
   * 所以：
   *   - 两个 `data` 各自指向自己矩形的左上角像素（位移在这里）；
   *   - 两个 `x_offset/y_offset` **取同一个值**（用目的矩形的坐标），
   *     这样图层矩形与输出矩形完全重合 ⇒ 交集 = 整个矩形，
   *     EPIC 才会对每一列都读一次源；否则不重合的那部分会被 `continue` 掉
   *     （`EPIC_ConfigBlendEx()` 里"输入与输出不相交就跳过该层"）。
   *   - `total_width` = 各自的行跨度（像素），矩形尺寸另给。
   *   - 单输入层 + 不透明 alpha（`LayerConfigInit` 置 255）⇒ 逐位拷贝。
   */
  HAL_EPIC_LayerConfigInit(&in);
  in.data = epic_at((void *)src_buf, src_stride, sx, sy);
  in.color_mode = EPIC_COLOR_RGB565;
  in.width = (uint16_t)w;
  in.height = (uint16_t)h;
  in.x_offset = (int16_t)dx;
  in.y_offset = (int16_t)dy;
  in.total_width = MYVENDOR_EPIC_PX_PER_ROW(src_stride);

  HAL_EPIC_LayerConfigInit(&out);
  out.data = epic_at((void *)dst_buf, dst_stride, dx, dy);
  out.color_mode = EPIC_COLOR_RGB565;
  out.width = (uint16_t)w;
  out.height = (uint16_t)h;
  out.x_offset = (int16_t)dx;
  out.y_offset = (int16_t)dy;
  out.total_width = MYVENDOR_EPIC_PX_PER_ROW(dst_stride);

  bool ok = epic_result(wt_before, (lv_epic_blend(&in, 1, &out)) == HAL_OK, "copy");

  if (ok)
    {
      /* 目的侧作废（见 epic_invalidate_rect_rows 的说明）；out.data 就是矩形左上角
       * —— 这条摆放约定在 2026-09-25 的开机自检里钉过（见上面那段 ⚠ 注释）。 */
      epic_invalidate_rect_rows(out.data, dst_stride, w, h);
    }

  return ok;
}

bool myvendor_epic_fill(uint16_t * dst_buf, uint32_t dst_stride, int32_t x,
  int32_t y, int32_t w, int32_t h, uint16_t color565)
{
  const uint32_t wt_before = g_epic_wait_timeouts;
  EPIC_LayerConfigTypeDef out;

  if (lv_epic_get_handle() == NULL)
    {
      return false;
    }

  if (x < 0 || y < 0)
    {
      return false;
    }

  if (x + w > EPIC_COORDINATES_MAX || y + h > EPIC_COORDINATES_MAX)
    {
      return false;
    }

  HAL_EPIC_LayerConfigInit(&out);
  out.data = epic_at((void *)dst_buf, dst_stride, x, y);
  out.color_mode = EPIC_COLOR_RGB565;
  out.width = (uint16_t)w;
  out.height = (uint16_t)h;
  out.x_offset = (int16_t)x;
  out.y_offset = (int16_t)y;
  out.total_width = MYVENDOR_EPIC_PX_PER_ROW(dst_stride);
  out.color_en = true;
  out.color_r = epic_expand5((uint16_t)((color565 >> 11) & 0x1fu));
  out.color_g = epic_expand6((uint16_t)((color565 >> 5) & 0x3fu));
  out.color_b = epic_expand5((uint16_t)(color565 & 0x1fu));

  /* 输入层数 = 0 ⇒ `lv_epic_fill()` 走 HAL_EPIC_fill(NULL, 0, &out) == HAL_OK; 这条路径 */
  bool ok = epic_result(wt_before, (lv_epic_fill(NULL, 0, &out)) == HAL_OK, "fill");

  if (ok)
    {
      epic_invalidate_rect_rows(out.data, dst_stride, w, h);
    }

  return ok;
}

bool myvendor_epic_busy(void)
{
  EPIC_HandleTypeDef *epic = lv_epic_get_handle();

  if (epic == NULL || epic->Instance == NULL)
    {
      return false;
    }

  /* 只看 IA_BUSY：HAL 的 State 在 IRQ 没人收时可能比硬件"晚"停下
     （LVGL 适配层 `lv_epic_is_hardware_active()` 的注释就是这么说的）。 */
  return (epic->Instance->STATUS & EPIC_STATUS_IA_BUSY_Msk) != 0U;
}

void myvendor_epic_wait(void)
{
  /*
   * 这条路上其实没什么可等的（包装底下是同步 HAL）。留着它是为了守住
   * "返回前等到完成"这个约定，日后换成 _IT 版或别的引擎时不会漏。
   */
  lv_epic_wait();
}
