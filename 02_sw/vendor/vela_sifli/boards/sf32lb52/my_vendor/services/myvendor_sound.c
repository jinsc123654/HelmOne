/**
 * @file myvendor_sound.c
 * @brief PA40 无源陶瓷蜂鸣片：GPTIM2 CH1 PWM 方波 + 队列播放表。
 *
 * 两路 GPTIM 分开：背光 GPTIM1 CH4 / PA01（20 kHz），蜂鸣器 GPTIM2 CH1 /
 * PA40（约 5 kHz）。音符时长 usleep。BLE companion 等界面起来再开，
 * 避免 LCPU RF 校准把开机音拉成一声长鸣。Startup 等
 * myvendor_sound_idle() 再进白屏。空闲把 PA40 拉回 GPIO 低。
 * 线程启动不 mux、不 PWM_Start，第一声才打开输出。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "myvendor_sound.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "bf0_hal.h"
#include "drv_io.h"
#include "myvendor_devctl.h"
#include "tim_config.h"

#define SOUND_PAD           PAD_PA40
#define SOUND_GPIO_PIN      40
#define SOUND_PWM_CHANNEL   GPT_CHANNEL_1
#define SOUND_PWM_FUNC      GPTIM2_CH1
#define SOUND_PWM_DUTY_PCT  50u
#define SOUND_PWM_HZ_MIN    500u
#define SOUND_PWM_HZ_MAX    12000u
/* 陶瓷片实测谐振约 5 kHz；提示音都绕它附近。 */
#define SOUND_F0            5000u
#define SOUND_F(pct)        ((uint16_t)((uint32_t)SOUND_F0 * (uint32_t)(pct) / 100u))
#define SOUND_QUEUE_N       8
#define SOUND_STACK_SIZE    2048
#define SOUND_PRIO          120

/** BSS 栈：bringup 一次 pthread_attr_setstack，空闲时 cond_wait 挂起。 */
static uint8_t g_sound_stack[SOUND_STACK_SIZE]
    __attribute__((aligned(16)));

typedef void (*sound_play_fn_t)(void);

typedef struct {
  uint16_t hz; /**< 0 为静音间隔。 */
  uint16_t ms;
} sound_note_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static myvendor_sound_id_t g_q[SOUND_QUEUE_N];
static uint8_t g_q_head;
static uint8_t g_q_tail;
static uint8_t g_q_n;
static bool g_started;
static bool g_playing;
static volatile uint8_t g_cur_id;
static volatile uint16_t g_cur_hz;
static volatile uint16_t g_cur_ms;
static bool g_pwm_ready;
static bool g_pwm_running;
static uint8_t g_pwm_duty_pct = SOUND_PWM_DUTY_PCT;
static GPT_HandleTypeDef g_pwm;

static const char *const g_sound_name[MYVENDOR_SOUND_COUNT] = {
  [MYVENDOR_SOUND_KEY]     = "key",
  [MYVENDOR_SOUND_OK]      = "ok",
  [MYVENDOR_SOUND_LONG]    = "long",
  [MYVENDOR_SOUND_BACK]    = "back",
  [MYVENDOR_SOUND_START]   = "start",
  [MYVENDOR_SOUND_PAUSE]   = "pause",
  [MYVENDOR_SOUND_SAVE]    = "save",
  [MYVENDOR_SOUND_DISCARD] = "discard",
  [MYVENDOR_SOUND_ARRIVE]  = "arrive",
  [MYVENDOR_SOUND_WARN]    = "warn",
  [MYVENDOR_SOUND_BOOT]    = "boot",
  [MYVENDOR_SOUND_PROMPT]  = "prompt",
};

static uint32_t sound_pwm_timclk(void)
{
#ifdef SF32LB52X
  return 24000000u;
#else
  return HAL_RCC_GetPCLKFreq(GPTIM2_CORE, 1);
#endif
}

static void sound_pwm_mux(void)
{
  /* PA40_TIM + GPTIM2_PINR CH1。背光走 GPTIM1 CH4 / PA01。 */
  HAL_PIN_Set(SOUND_PAD, SOUND_PWM_FUNC, PIN_NOPULL, 1);
  HAL_PIN_Set_DS0(SOUND_PAD, 1, 1);
  HAL_PIN_Set_DS1(SOUND_PAD, 1, 1);
}

static int sound_pwm_calc(uint16_t hz, uint32_t *prescaler, uint32_t *period,
                          uint32_t *pulse)
{
  uint32_t timclk;
  uint64_t ticks;
  uint32_t psc;
  uint32_t arr;
  uint32_t ccr;

  if (hz < SOUND_PWM_HZ_MIN) {
    hz = SOUND_PWM_HZ_MIN;
  } else if (hz > SOUND_PWM_HZ_MAX) {
    hz = SOUND_PWM_HZ_MAX;
  }

  timclk = sound_pwm_timclk();
  if (timclk == 0) {
    return -EIO;
  }

  ticks = (uint64_t)timclk / (uint64_t)hz;
  if (ticks == 0) {
    ticks = 1;
  }

  psc = (uint32_t)((ticks + 65535ULL - 1ULL) / 65535ULL);
  if (psc == 0) {
    psc = 1;
  }

  arr = (uint32_t)(ticks / psc);
  if (arr < 2) {
    arr = 2;
  } else if (arr > 65535) {
    arr = 65535;
  }

  ccr = (uint32_t)(((uint64_t)g_pwm_duty_pct * (uint64_t)arr) / 100ULL);
  if (ccr == 0) {
    ccr = 1;
  } else if (ccr >= arr) {
    ccr = arr - 1;
  }

  *prescaler = psc;
  *period = arr;
  *pulse = ccr;
  return 0;
}

static void sound_pin_idle(void);

static int sound_pwm_init_once(void)
{
  GPT_OC_InitTypeDef oc_cfg;
  uint32_t psc;
  uint32_t arr;
  uint32_t ccr;

  if (g_pwm_ready) {
    return 0;
  }

  memset(&g_pwm, 0, sizeof(g_pwm));
  g_pwm.Instance = GPTIM2;
  g_pwm.core = GPTIM2_CORE;
  HAL_RCC_EnableModule(RCC_MOD_GPTIM2);

  if (sound_pwm_calc(SOUND_F0, &psc, &arr, &ccr) != 0) {
    return -EIO;
  }

  g_pwm.Init.Prescaler = psc - 1;
  g_pwm.Init.CounterMode = GPT_COUNTERMODE_UP;
  g_pwm.Init.Period = arr - 1;
  g_pwm.Init.RepetitionCounter = 0;
  if (HAL_GPT_PWM_Init(&g_pwm) != HAL_OK) {
    return -EIO;
  }

  memset(&oc_cfg, 0, sizeof(oc_cfg));
  oc_cfg.OCMode = GPT_OCMODE_PWM1;
  oc_cfg.Pulse = 0;
  oc_cfg.OCPolarity = GPT_OCPOLARITY_HIGH;
  oc_cfg.OCNPolarity = GPT_OCNPOLARITY_LOW;
  oc_cfg.OCFastMode = GPT_OCFAST_DISABLE;
  oc_cfg.OCIdleState = GPT_OCIDLESTATE_RESET;
  oc_cfg.OCNIdleState = GPT_OCNIDLESTATE_RESET;
  if (HAL_GPT_PWM_ConfigChannel(&g_pwm, &oc_cfg, SOUND_PWM_CHANNEL) != HAL_OK) {
    return -EIO;
  }

  g_pwm_running = false;
  g_pwm_ready = true;
  sound_pin_idle();
  return 0;
}

static void sound_pwm_silence(void)
{
  if (!g_pwm_ready) {
    return;
  }

  g_pwm.Instance->CCR1 = 0;
}

/** 空闲 PA40 回 GPIO 低。GPTIM2 不驱动脚，背光 GPTIM1 不受影响。 */
static void sound_pin_idle(void)
{
  sound_pwm_silence();
  HAL_PIN_Set(SOUND_PAD, GPIO_A40, PIN_NOPULL, 1);
  BSP_GPIO_Set(SOUND_GPIO_PIN, 0, 1);
}

static int sound_pwm_hz(uint16_t hz)
{
  uint32_t psc;
  uint32_t arr;
  uint32_t ccr;

  if (sound_pwm_init_once() != 0) {
    return -EIO;
  }

  if (sound_pwm_calc(hz, &psc, &arr, &ccr) != 0) {
    return -EIO;
  }

  sound_pwm_mux();
  __HAL_GPT_DISABLE(&g_pwm);
  g_pwm.Instance->PSC = psc - 1;
  g_pwm.Instance->ARR = arr - 1;
  g_pwm.Instance->CCR1 = ccr;
  g_pwm.Instance->EGR = GPT_EGR_UG;
  if (HAL_GPT_PWM_Start(&g_pwm, SOUND_PWM_CHANNEL) != HAL_OK) {
    return -EIO;
  }

  g_pwm_running = true;
  return 0;
}

static void sound_tone(uint16_t hz, uint16_t ms)
{
  if (ms == 0) {
    return;
  }

  if (hz == 0) {
    sound_pwm_silence();
    usleep((useconds_t)ms * 1000u);
    return;
  }

  g_cur_hz = hz;
  g_cur_ms = ms;
  if (sound_pwm_hz(hz) != 0) {
    syslog(LOG_ERR, "sound: PWM %u Hz failed\n", (unsigned)hz);
    return;
  }

  usleep((useconds_t)ms * 1000u);
  sound_pwm_silence();
}

static void sound_notes(const sound_note_t *notes, uint8_t n)
{
  uint8_t i;

  for (i = 0; i < n; i++) {
    sound_tone(notes[i].hz, notes[i].ms);
  }

  sound_pin_idle();
}

static void play_key(void)
{
  static const sound_note_t n[] = { { SOUND_F(100), 50 } };

  sound_notes(n, 1);
}

static void play_ok(void)
{
  static const sound_note_t n[] = { { SOUND_F(105), 60 } };

  sound_notes(n, 1);
}

static void play_long(void)
{
  static const sound_note_t n[] = { { SOUND_F(95), 110 } };

  sound_notes(n, 1);
}

static void play_back(void)
{
  static const sound_note_t n[] = { { SOUND_F(100), 45 }, { SOUND_F(90), 70 } };

  sound_notes(n, 2);
}

static void play_start(void)
{
  static const sound_note_t n[] = {
    { SOUND_F(92), 55 }, { SOUND_F(100), 55 }, { SOUND_F(108), 90 }
  };

  sound_notes(n, 3);
}

static void play_pause(void)
{
  static const sound_note_t n[] = { { SOUND_F(100), 50 }, { SOUND_F(90), 90 } };

  sound_notes(n, 2);
}

static void play_save(void)
{
  static const sound_note_t n[] = {
    { SOUND_F(92), 45 }, { SOUND_F(100), 45 }, { SOUND_F(108), 100 }
  };

  sound_notes(n, 3);
}

static void play_discard(void)
{
  static const sound_note_t n[] = { { SOUND_F(100), 45 }, { SOUND_F(88), 100 } };

  sound_notes(n, 2);
}

static void play_arrive(void)
{
  static const sound_note_t n[] = {
    { SOUND_F(90), 50 }, { SOUND_F(97), 50 }, { SOUND_F(104), 50 },
    { SOUND_F(110), 100 }
  };

  sound_notes(n, 4);
}

static void play_warn(void)
{
  static const sound_note_t n[] = {
    { SOUND_F(92), 80 }, { 0, 50 }, { SOUND_F(92), 80 }
  };

  sound_notes(n, 3);
}

static void play_boot(void)
{
  static const sound_note_t n[] = {
    { SOUND_F(90), 60 }, { SOUND_F(97), 60 }, { SOUND_F(104), 80 },
    { SOUND_F(110), 110 }
  };

  sound_notes(n, 4);
}

static void play_prompt(void)
{
  static const sound_note_t n[] = {
    { SOUND_F(100), 45 }, { 0, 35 }, { SOUND_F(100), 55 }
  };

  sound_notes(n, 3);
}

/* 换 MP3：改这些函数或改表指针，调用点保持 myvendor_sound_*()。 */
static const sound_play_fn_t g_sound_play[MYVENDOR_SOUND_COUNT] = {
  [MYVENDOR_SOUND_KEY]     = play_key,
  [MYVENDOR_SOUND_OK]      = play_ok,
  [MYVENDOR_SOUND_LONG]    = play_long,
  [MYVENDOR_SOUND_BACK]    = play_back,
  [MYVENDOR_SOUND_START]   = play_start,
  [MYVENDOR_SOUND_PAUSE]   = play_pause,
  [MYVENDOR_SOUND_SAVE]    = play_save,
  [MYVENDOR_SOUND_DISCARD] = play_discard,
  [MYVENDOR_SOUND_ARRIVE]  = play_arrive,
  [MYVENDOR_SOUND_WARN]    = play_warn,
  [MYVENDOR_SOUND_BOOT]    = play_boot,
  [MYVENDOR_SOUND_PROMPT]  = play_prompt,
};

static void sound_post(myvendor_sound_id_t id)
{
  if (!g_started || id >= MYVENDOR_SOUND_COUNT) {
    return;
  }

  pthread_mutex_lock(&g_lock);
  if (g_q_n >= SOUND_QUEUE_N) {
    pthread_mutex_unlock(&g_lock);
    return;
  }

  g_q[g_q_tail] = id;
  g_q_tail = (uint8_t)((g_q_tail + 1u) % SOUND_QUEUE_N);
  g_q_n++;
  pthread_cond_signal(&g_cv);
  pthread_mutex_unlock(&g_lock);
}

/**
 * UI 提示音：策略关则丢弃。NSH `test sound` 走 sound_post / diag，不受此限。
 */
static void sound_post_ui(myvendor_sound_id_t id)
{
  if (!myvendor_devctl_sound_get()) {
    return;
  }

  sound_post(id);
}

static void *sound_thread(void *arg)
{
  myvendor_sound_id_t id;
  sound_play_fn_t fn;

  (void)arg;
  if (sound_pwm_init_once() != 0) {
    syslog(LOG_ERR, "sound: PWM init failed\n");
  }

  for (;;) {
    pthread_mutex_lock(&g_lock);
    while (g_q_n == 0) {
      /* 队列空：挂起，不占 CPU，栈仍留在 BSS。 */
      pthread_cond_wait(&g_cv, &g_lock);
    }

    id = g_q[g_q_head];
    g_q_head = (uint8_t)((g_q_head + 1u) % SOUND_QUEUE_N);
    g_q_n--;
    g_playing = true;
    g_cur_id = (uint8_t)id;
    pthread_mutex_unlock(&g_lock);

    fn = (id < MYVENDOR_SOUND_COUNT) ? g_sound_play[id] : NULL;
    if (fn != NULL) {
      fn();
    }

    pthread_mutex_lock(&g_lock);
    g_playing = false;
    pthread_mutex_unlock(&g_lock);
  }

  return NULL;
}

int myvendor_sound_start(void)
{
  pthread_attr_t attr;
  struct sched_param sp;
  pthread_t th;
  int ret;

  if (g_started) {
    return 0;
  }

  if (sound_pwm_init_once() != 0) {
    syslog(LOG_ERR, "sound: PWM init failed\n");
    return -EIO;
  }
  pthread_attr_init(&attr);
  ret = pthread_attr_setstack(&attr, g_sound_stack, sizeof(g_sound_stack));
  sp.sched_priority = SOUND_PRIO;
#ifdef PTHREAD_EXPLICIT_SCHED
  (void)pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
#endif
  (void)pthread_attr_setschedparam(&attr, &sp);
  if (ret == 0) {
    ret = pthread_create(&th, &attr, sound_thread, NULL);
  }

  pthread_attr_destroy(&attr);
  if (ret != 0) {
    syslog(LOG_ERR, "sound: thread create failed %d\n", ret);
    return -ret;
  }

#ifndef CONFIG_DISABLE_PTHREAD
  pthread_setname_np(th, "sound");
#endif
  pthread_detach(th);
  g_started = true;
  syslog(LOG_INFO, "sound: PA40 GPTIM2 CH1 PWM ready prio=%d\n", SOUND_PRIO);
  return 0;
}

void myvendor_sound_play(myvendor_sound_id_t id)
{
  sound_post(id);
}

int myvendor_sound_play_name(const char *name)
{
  myvendor_sound_id_t i;

  if (name == NULL || name[0] == '\0') {
    return -EINVAL;
  }

  for (i = 0; i < MYVENDOR_SOUND_COUNT; i++) {
    if (strcmp(name, g_sound_name[i]) == 0) {
      sound_post(i);
      return 0;
    }
  }

  return -ENOENT;
}

int myvendor_sound_diag_pwm(uint16_t hz, uint16_t ms)
{
  return myvendor_sound_diag_pwm_duty(hz, ms, SOUND_PWM_DUTY_PCT);
}

int myvendor_sound_diag_pwm_duty(uint16_t hz, uint16_t ms, uint8_t duty_pct)
{
  uint8_t saved;

  if (sound_pwm_init_once() != 0) {
    return -EIO;
  }

  if (duty_pct < 10u) {
    duty_pct = 10u;
  } else if (duty_pct > 90u) {
    duty_pct = 90u;
  }

  saved = g_pwm_duty_pct;
  g_pwm_duty_pct = duty_pct;
  sound_tone(hz, ms);
  sound_pin_idle();
  g_pwm_duty_pct = saved;
  return 0;
}

int myvendor_sound_diag_dc(uint16_t ms)
{
  if (sound_pwm_init_once() != 0) {
    return -EIO;
  }

  if (ms == 0) {
    ms = 400;
  }

  sound_pwm_silence();
  HAL_PIN_Set(SOUND_PAD, GPIO_A40, PIN_NOPULL, 1);
  HAL_PIN_Set_DS0(SOUND_PAD, 1, 1);
  HAL_PIN_Set_DS1(SOUND_PAD, 1, 1);
  BSP_GPIO_Set(SOUND_GPIO_PIN, 1, 1);
  usleep((useconds_t)ms * 1000u);
  BSP_GPIO_Set(SOUND_GPIO_PIN, 0, 1);
  sound_pwm_mux();
  return 0;
}

int myvendor_sound_diag_sweep(void)
{
  uint16_t hz;

  if (sound_pwm_init_once() != 0) {
    return -EIO;
  }

  for (hz = 4000; hz <= 10000; hz = (uint16_t)(hz + 500)) {
    syslog(LOG_INFO, "sound: sweep %u Hz\n", (unsigned)hz);
    sound_tone(hz, 320);
    sound_pin_idle();
    usleep(180000);
  }

  return 0;
}

const char *myvendor_sound_id_name(myvendor_sound_id_t id)
{
  if (id >= MYVENDOR_SOUND_COUNT) {
    return NULL;
  }

  return g_sound_name[id];
}

void myvendor_sound_key(void)
{
  sound_post_ui(MYVENDOR_SOUND_KEY);
}

void myvendor_sound_ok(void)
{
  sound_post_ui(MYVENDOR_SOUND_OK);
}

void myvendor_sound_long(void)
{
  sound_post_ui(MYVENDOR_SOUND_LONG);
}

void myvendor_sound_back(void)
{
  sound_post_ui(MYVENDOR_SOUND_BACK);
}

void myvendor_sound_ride(void)
{
  sound_post_ui(MYVENDOR_SOUND_START);
}

void myvendor_sound_pause(void)
{
  sound_post_ui(MYVENDOR_SOUND_PAUSE);
}

void myvendor_sound_save(void)
{
  sound_post_ui(MYVENDOR_SOUND_SAVE);
}

void myvendor_sound_discard(void)
{
  sound_post_ui(MYVENDOR_SOUND_DISCARD);
}

void myvendor_sound_arrive(void)
{
  sound_post_ui(MYVENDOR_SOUND_ARRIVE);
}

void myvendor_sound_warn(void)
{
  sound_post_ui(MYVENDOR_SOUND_WARN);
}

void myvendor_sound_boot(void)
{
  sound_post_ui(MYVENDOR_SOUND_BOOT);
}

void myvendor_sound_prompt(void)
{
  sound_post_ui(MYVENDOR_SOUND_PROMPT);
}

bool myvendor_sound_idle(void)
{
  bool idle;

  if (!g_started) {
    return true;
  }

  pthread_mutex_lock(&g_lock);
  idle = (g_q_n == 0) && !g_playing;
  pthread_mutex_unlock(&g_lock);
  return idle;
}

int myvendor_sound_crash_format(char *buf, size_t n)
{
  const char *name;
  uint8_t id = g_cur_id;

  if (buf == NULL || n < 8)
    {
      return 0;
    }

  name = (id < MYVENDOR_SOUND_COUNT) ? g_sound_name[id] : "?";
  return snprintf(buf, n,
                  "sound playing=%d pwm=%d id=%s hz=%u ms=%u q=%u",
                  g_playing ? 1 : 0, g_pwm_running ? 1 : 0, name,
                  (unsigned)g_cur_hz, (unsigned)g_cur_ms,
                  (unsigned)g_q_n);
}
