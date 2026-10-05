/****************************************************************************
 * arch/arm/src/SF32LB/sifli_i2c.c
 * SF32LB I2C driver - based on SF32LB I2C Hardware Layer - Device Driver
 *
*/
/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <sfconfig.h>

#include <sys/types.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <errno.h>
#include <syslog.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/clock.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/kmalloc.h>
#include <nuttx/power/pm.h>
#include <nuttx/i2c/i2c_master.h>
#include <arch/board/board.h>

#include "arm_internal.h"

#include "register.h"
#include "bf0_hal.h"
#include "sf32lb_i2c.h"
#include "i2c_config.h"
#include "dma_config.h"

#define I2C_XFER_TIMEOUT  5000

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Interrupt state */

enum sifli_intstate_e
{
    INTSTATE_IDLE = 0,      /* No I2C activity */
    INTSTATE_WAITING,       /* Waiting for completion of interrupt activity */
    INTSTATE_DONE,          /* Interrupt activity complete */
};

/* I2C Device hardware configuration */
typedef struct bf0_i2c_config
{
    const char *device_name;
    I2C_TypeDef *Instance;
    IRQn_Type irq_type;
    uint8_t core;
    struct dma_config *dma_rx;
    struct dma_config *dma_tx;
} bf0_i2c_config_t;

/* I2C Device, Instance */
struct sifli_i2c_master_s
{
    const struct i2c_ops_s    *ops;   /* Standard I2C operations */
    mutex_t lock;                     /* Mutual exclusion mutex */
    int frequency;
#ifndef CONFIG_I2C_POLLED
    sem_t sem_isr;                    /* Interrupt wait semaphore */
#endif
#ifdef CONFIG_PM
    struct pm_callback_s pm_cb;       /* PM callbacks */
#endif
    I2C_HandleTypeDef handle;
    bf0_i2c_config_t *bf0_i2c_cfg;
    struct
    {
        DMA_HandleTypeDef dma_rx;
        DMA_HandleTypeDef dma_tx;
    } dma;
    uint8_t i2c_dma_flag;
    uint8_t port;
    RCC_MODULE_TYPE mod;
};

static bf0_i2c_config_t bf0_i2c_cfg[] =
{
#ifdef BSP_USING_I2C1
    BF0_I2C1_CFG,
#endif

#ifdef BSP_USING_I2C2
    BF0_I2C2_CFG,
#endif

#ifdef BSP_USING_I2C3
    BF0_I2C3_CFG,
#endif

#ifdef BSP_USING_I2C4
    BF0_I2C4_CFG,
#endif

#ifdef BSP_USING_I2C5
    BF0_I2C5_CFG,
#endif

#ifdef BSP_USING_I2C6
    BF0_I2C6_CFG,
#endif

#ifdef BSP_USING_I2C7
    BF0_I2C7_CFG,
#endif
};

static const RCC_MODULE_TYPE bf0_i2c_mod[] =
{
#ifdef BSP_USING_I2C1
    RCC_MOD_I2C1,
#endif

#ifdef BSP_USING_I2C2
    RCC_MOD_I2C2,
#endif

#ifdef BSP_USING_I2C3
    RCC_MOD_I2C3,
#endif

#ifdef BSP_USING_I2C4
    RCC_MOD_I2C4,
#endif

#ifdef BSP_USING_I2C5
    RCC_MOD_I2C5,
#endif

#ifdef BSP_USING_I2C6
    RCC_MOD_I2C6,
#endif

#ifdef BSP_USING_I2C7
    RCC_MOD_I2C7,
#endif
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/
static int sifli_i2c_transfer(struct i2c_master_s *dev,
                                    struct i2c_msg_s *msgs,
                                    int count);

#ifdef CONFIG_I2C_RESET
static int sifli_i2c_reset(struct i2c_master_s *dev);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Device Structures, Instantiation */
static const struct i2c_ops_s sifli_i2c_ops =
{
  .transfer = sifli_i2c_transfer
#ifdef CONFIG_I2C_RESET
  , .reset  = sifli_i2c_reset
#endif
};


static void i2c_get_dma_info(void)
{
#ifdef BSP_I2C1_USING_DMA
    static struct dma_config i2c1_trx_dma = I2C1_TRX_DMA_CONFIG;
    i2c_obj[I2C1_INDEX].i2c_dma_flag = 1;
    bf0_i2c_cfg[I2C1_INDEX].dma_rx = &i2c1_trx_dma;
    bf0_i2c_cfg[I2C1_INDEX].dma_tx = &i2c1_trx_dma;
#endif
#ifdef BSP_I2C2_USING_DMA
    static struct dma_config i2c2_trx_dma = I2C2_TRX_DMA_CONFIG;
    i2c_obj[I2C2_INDEX].i2c_dma_flag = 1;
    bf0_i2c_cfg[I2C2_INDEX].dma_rx = &i2c2_trx_dma;
    bf0_i2c_cfg[I2C2_INDEX].dma_tx = &i2c2_trx_dma;
#endif
#ifdef BSP_I2C3_USING_DMA
    static struct dma_config i2c3_trx_dma = I2C3_TRX_DMA_CONFIG;
    i2c_obj[I2C3_INDEX].i2c_dma_flag = 1;
    bf0_i2c_cfg[I2C3_INDEX].dma_rx = &i2c3_trx_dma;
    bf0_i2c_cfg[I2C3_INDEX].dma_tx = &i2c3_trx_dma;
#endif
#ifdef BSP_I2C4_USING_DMA
    static struct dma_config i2c4_trx_dma = I2C4_TRX_DMA_CONFIG;
    i2c_obj[I2C4_INDEX].i2c_dma_flag = 1;
    bf0_i2c_cfg[I2C4_INDEX].dma_rx = &i2c4_trx_dma;
    bf0_i2c_cfg[I2C4_INDEX].dma_tx = &i2c4_trx_dma;
#endif
#ifdef BSP_I2C5_USING_DMA
    static struct dma_config i2c5_trx_dma = I2C5_TRX_DMA_CONFIG;
    i2c_obj[I2C5_INDEX].i2c_dma_flag = 1;
    bf0_i2c_cfg[I2C5_INDEX].dma_rx = &i2c5_trx_dma;
    bf0_i2c_cfg[I2C5_INDEX].dma_tx = &i2c5_trx_dma;
#endif
#ifdef BSP_I2C6_USING_DMA
    static struct dma_config i2c6_trx_dma = I2C6_TRX_DMA_CONFIG;
    i2c_obj[I2C6_INDEX].i2c_dma_flag = 1;
    bf0_i2c_cfg[I2C6_INDEX].dma_rx = &i2c6_trx_dma;
    bf0_i2c_cfg[I2C6_INDEX].dma_tx = &i2c6_trx_dma;
#endif
#ifdef BSP_I2C7_USING_DMA
    static struct dma_config i2c7_trx_dma = I2C7_TRX_DMA_CONFIG;
    i2c_obj[I2C7_INDEX].i2c_dma_flag = 1;
    bf0_i2c_cfg[I2C7_INDEX].dma_rx = &i2c7_trx_dma;
    bf0_i2c_cfg[I2C7_INDEX].dma_tx = &i2c7_trx_dma;
#endif
}


int bf0_hw_i2c_init(struct sifli_i2c_master_s *obj)
{
    obj->bf0_i2c_cfg = &bf0_i2c_cfg[obj->port];
    obj->handle.Instance = bf0_i2c_cfg[obj->port].Instance;
    if (obj->i2c_dma_flag)
    {
        i2c_get_dma_info();
        __HAL_LINKDMA(&obj->handle, hdmarx, obj->dma.dma_rx);
        __HAL_LINKDMA(&obj->handle, hdmatx, obj->dma.dma_tx);
        HAL_I2C_DMA_Init(&obj->handle, obj->bf0_i2c_cfg->dma_rx, obj->bf0_i2c_cfg->dma_tx);
    }
    return OK;
}

#ifndef CONFIG_I2C_POLLED
static int sifli_i2c_isr(int irq, FAR void *context, FAR void *arg)
{
    struct sifli_i2c_master_s *priv=(struct sifli_i2c_master_s *)arg;
    I2C_HandleTypeDef *handle=&priv->handle;

    if (handle->XferISR != NULL)
        handle->XferISR(handle, 0, 0);

    if ((HAL_I2C_STATE_BUSY_TX != handle->State) && (HAL_I2C_STATE_BUSY_RX != handle->State))
        nxsem_post(&priv->sem_isr);
    return OK;
}
#endif

static int sifli_i2cdma_isr(int irq, FAR void *context, FAR void *arg)
{
    struct sifli_i2c_master_s *priv=(struct sifli_i2c_master_s *)arg;
    I2C_HandleTypeDef *handle=&priv->handle;

    if (handle->State == HAL_I2C_STATE_BUSY_TX)
        HAL_DMA_IRQHandler(handle->hdmatx);
    else if (handle->State == HAL_I2C_STATE_BUSY_RX)
        HAL_DMA_IRQHandler(handle->hdmarx);
    return OK;
}


/****************************************************************************
 * Name: sifli_i2c_init
 *
 * Description:
 *   Setup the I2C hardware, ready for operation with defaults
 *
 ****************************************************************************/

static int sifli_i2c_init(struct sifli_i2c_master_s *priv)
{
    
    HAL_RCC_EnableModule(bf0_i2c_mod[priv->port]);

    bf0_hw_i2c_init(priv);
    priv->ops=&sifli_i2c_ops;
#ifndef CONFIG_I2C_POLLED
    /* Attach error and event interrupts to the ISRs */
    irq_attach(priv->bf0_i2c_cfg->irq_type+16, sifli_i2c_isr, priv);
    up_enable_irq(priv->bf0_i2c_cfg->irq_type+16);
#endif
    if (priv->i2c_dma_flag) {
        /* Attach error and event interrupts to the ISRs */
        irq_attach(priv->bf0_i2c_cfg->dma_rx->dma_irq+16, sifli_i2cdma_isr, priv);
        up_enable_irq(priv->bf0_i2c_cfg->dma_rx->dma_irq+16);
    }

    return OK;
}

/****************************************************************************
 * Name: sifli_i2c_deinit
 *
 * Description:
 *   Shutdown the I2C hardware
 *
 ****************************************************************************/

static int sifli_i2c_deinit(struct sifli_i2c_master_s *priv)
{
    /* Disable I2C */
    /* Unconfigure GPIO pins */
#ifndef CONFIG_I2C_POLLED
    /* Disable and detach interrupts */
    up_disable_irq(priv->bf0_i2c_cfg->irq_type+16);
    irq_detach(priv->bf0_i2c_cfg->irq_type+16);
#endif
    if (priv->i2c_dma_flag) {
        up_disable_irq(priv->bf0_i2c_cfg->dma_rx->dma_irq+16);
        irq_detach(priv->bf0_i2c_cfg->dma_rx->dma_irq+16);
    }

    HAL_RCC_DisableModule(bf0_i2c_mod[priv->port]);
    return OK;
}

static int sifli_i2c_configure(struct sifli_i2c_master_s *bf0_i2c, int addr, int freq, int addrmode)
{
    int ret=OK;

    i2cinfo("i2c_bus_configure start");
    DEBUGASSERT(bf0_i2c != NULL);

    //if (bf0_i2c->frequency==addr)
        //return ret;

    if (addrmode & I2C_M_TEN)
    {
        bf0_i2c->handle.Init.AddressingMode = I2C_ADDRESSINGMODE_10BIT;
        bf0_i2c->handle.Init.OwnAddress1 = (addr & 0x7fff);
    }
    else
    {
        bf0_i2c->handle.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
        bf0_i2c->handle.Init.OwnAddress1 = (addr & 0x7fff) << 1;
    }

    bf0_i2c->handle.Init.ClockSpeed = freq;
    bf0_i2c->handle.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    bf0_i2c->handle.core = bf0_i2c->bf0_i2c_cfg->core;
    bf0_i2c->handle.Mode = HAL_I2C_MODE_MASTER;

    ret = HAL_I2C_Init(&(bf0_i2c->handle));
    i2cinfo("i2c_bus_configure end");

    return ret;
}

/**
 * @brief Normal i2c operations(without I2C restart)
 * @param bf0_i2c -
 * @param msg -
 * @return
 */
static int i2c_normal_ops(struct sifli_i2c_master_s *bf0_i2c, struct i2c_msg_s * msg)
{
    int ret;

    if (msg->flags & I2C_M_READ)
    {
        if (bf0_i2c->i2c_dma_flag)
        {
            HAL_DMA_Init(&bf0_i2c->dma.dma_rx);
            ret = HAL_I2C_Master_Receive_DMA(&bf0_i2c->handle, msg->addr, msg->buffer, msg->length);
        }
        else
#ifdef CONFIG_I2C_POLLED
            ret = HAL_I2C_Master_Receive(&bf0_i2c->handle, msg->addr, msg->buffer, msg->length, MSEC2TICK(I2C_XFER_TIMEOUT));
#else
            ret = HAL_I2C_Master_Receive_IT(&bf0_i2c->handle, msg->addr, msg->buffer, msg->length);
#endif
    }
    else
    {
        if (bf0_i2c->i2c_dma_flag)
        {
            HAL_DMA_Init(&bf0_i2c->dma.dma_tx);
            ret = HAL_I2C_Master_Transmit_DMA(&bf0_i2c->handle, msg->addr, msg->buffer, msg->length);
        }
        else
#ifdef CONFIG_I2C_POLLED
            ret = HAL_I2C_Master_Transmit(&bf0_i2c->handle, msg->addr, msg->buffer, msg->length, MSEC2TICK(I2C_XFER_TIMEOUT));
#else
            ret = HAL_I2C_Master_Transmit_IT(&bf0_i2c->handle, msg->addr, msg->buffer, msg->length);
#endif
    }

    return ret;
}

/**
 * @brief I2C memory access (There is an extra 'I2C restart' + 'I2C DevAddress' between memory address and r/w buf for memory read operation )
 * @param bf0_i2c -
 * @param msg_nostop  - Msg only contains memory address, msg length is memory address length
 * @param msg_nostart - Msg contains read or write buffer
 * @return
 */
static int i2c_mem_ops(struct sifli_i2c_master_s *bf0_i2c, struct i2c_msg_s * msg_nostop, struct i2c_msg_s * msg_nostart)
{
    int ret;
    uint16_t DevAddress, MemAddress, MemAddSize;

    DEBUGASSERT((1 == msg_nostop->length)||(2 == msg_nostop->length));
    if (1 == msg_nostop->length)
    {
        MemAddSize = I2C_MEMADD_SIZE_8BIT;
        MemAddress = *msg_nostop->buffer;
    }
    else
    {
        MemAddSize = I2C_MEMADD_SIZE_16BIT;

        MemAddress = *(msg_nostop->buffer+1);
        MemAddress = MemAddress << 8;
        MemAddress |= (*msg_nostop->buffer);
    }

    DevAddress = msg_nostop->addr;

    if (msg_nostart->flags & I2C_M_READ)
    {
        if (bf0_i2c->i2c_dma_flag)
        {
            HAL_DMA_Init(&bf0_i2c->dma.dma_rx);

            ret = HAL_I2C_Mem_Read_DMA(&bf0_i2c->handle, DevAddress, MemAddress, MemAddSize, msg_nostart->buffer, msg_nostart->length);
        }
        else
        {
            #ifdef CONFIG_I2C_POLLED
            ret = HAL_I2C_Mem_Read(&bf0_i2c->handle, DevAddress, MemAddress, MemAddSize, msg_nostart->buffer, msg_nostart->length, MSEC2TICK(I2C_XFER_TIMEOUT));
            #else
            ret = HAL_I2C_Mem_Read_IT(&bf0_i2c->handle, DevAddress, MemAddress, MemAddSize, msg_nostart->buffer, msg_nostart->length);
            #endif /* CONFIG_I2C_POLLED */
        }
    }
    else
    {
        if (bf0_i2c->i2c_dma_flag)
        {
            HAL_DMA_Init(&bf0_i2c->dma.dma_tx);
            ret = HAL_I2C_Mem_Write_DMA(&bf0_i2c->handle, DevAddress, MemAddress, MemAddSize, msg_nostart->buffer, msg_nostart->length);
        }
        else
        {
            #ifdef CONFIG_I2C_POLLED
            ret = HAL_I2C_Mem_Write(&bf0_i2c->handle, DevAddress, MemAddress, MemAddSize, msg_nostart->buffer, msg_nostart->length, MSEC2TICK(I2C_XFER_TIMEOUT));
            #else
            ret = HAL_I2C_Mem_Write_IT(&bf0_i2c->handle, DevAddress, MemAddress, MemAddSize, msg_nostart->buffer, msg_nostart->length);
            #endif /* CONFIG_I2C_POLLED */
        }
    }

    return ret;
}


/****************************************************************************
 * Name: sifli_i2c_transfer
 *
 * Description:
 *   Generic I2C transfer function
 *
 ****************************************************************************/
static int sifli_i2c_transfer(struct i2c_master_s *dev,
                                struct i2c_msg_s *msgs,
                                int count)
{
    struct sifli_i2c_master_s *bf0_i2c;
    HAL_StatusTypeDef status;
    int ret;

    struct i2c_msg_s *nostop_msg = NULL;

    DEBUGASSERT(dev != NULL);
    bf0_i2c = (struct sifli_i2c_master_s *)dev;

    ret = nxmutex_lock(&bf0_i2c->lock);
    if (ret < 0)
    {
        i2cerr("i2c mutex lock failed: %d", ret);
        return ret;
    }

    for (int index = 0; index < count; index++)
    {
        struct i2c_msg_s * msg = &msgs[index];

        if(msg->flags & I2C_M_NOSTOP)
        {
            DEBUGASSERT(nostop_msg == NULL);
            nostop_msg = msg;
            continue;//Memory access msg start
        }
        else
        {
            sifli_i2c_configure(bf0_i2c, 0, msg->frequency, msg->flags);
            __HAL_I2C_ENABLE(&bf0_i2c->handle);

            /* NOSTOP + READ/WRITE is the common NuttX register-access pattern.
             * Only the explicit NOSTART flag was handled before, so the address
             * phase was dropped and long writes (e.g. BMI270 config) could hang.
             */

            if (nostop_msg != NULL)
            {
                DEBUGASSERT(nostop_msg->frequency == msg->frequency);
                DEBUGASSERT(nostop_msg->addr == msg->addr);

                if (msg->length >= 256)
                  {
                    syslog(LOG_INFO,
                           "BRINGUP: i2c%d mem xfer addr=0x%02x len=%d\n",
                           bf0_i2c->port, nostop_msg->addr, msg->length);
                  }

                ret = i2c_mem_ops(bf0_i2c, nostop_msg, msg);
            }
            else
            {
                ret = i2c_normal_ops(bf0_i2c, msg);
            }
        }

        while (1)
        {
            HAL_I2C_StateTypeDef i2c_state = HAL_I2C_GetState(&bf0_i2c->handle);

            if (HAL_I2C_STATE_READY == i2c_state)
            {
                status = HAL_OK;
            }
            else if (HAL_I2C_STATE_TIMEOUT == i2c_state)
            {
                status = HAL_TIMEOUT;
            }
#ifndef  CONFIG_I2C_POLLED
            else if ((HAL_I2C_STATE_BUSY_TX == i2c_state) || (HAL_I2C_STATE_BUSY_RX == i2c_state)) //Interrupt or DMA mode, wait semaphore
            {
                int wait = nxsem_tickwait_uninterruptible(&(bf0_i2c->sem_isr), MSEC2TICK(I2C_XFER_TIMEOUT));
                if (wait)
                {
                    syslog(LOG_ERR,
                           "BRINGUP: i2c%d ISR wait timeout idx=%d/%d\n",
                           bf0_i2c->port, index, count);
                    i2cerr("i2c wait error!");
                    status = HAL_TIMEOUT;
                }
                else
                {
                    continue;
                }
            }
#endif
            else
            {
                status = HAL_ERROR;
            }

            break;
        }

        nostop_msg = NULL;

        if (bf0_i2c->handle.ErrorCode||HAL_OK != status) {

            i2cerr("bus err:%d, xfer:%d/%d, i2c_stat:%x, i2c_errcode=%lx", status, index, count, HAL_I2C_GetState(&bf0_i2c->handle), bf0_i2c->handle.ErrorCode);
            HAL_I2C_Reset(&bf0_i2c->handle);
            ret = -EIO;
            break;
        }
    }
    __HAL_I2C_DISABLE(&bf0_i2c->handle);
    i2cinfo("master_xfer end");
    /* Release the port for re-use by other clients */
    nxmutex_unlock(&bf0_i2c->lock);
    return ret;
}

/****************************************************************************
 * Name: sifli_i2c_reset
 *
 * Description:
 *   Reset an I2C bus
 *
 ****************************************************************************/

#ifdef CONFIG_I2C_RESET
static int sifli_i2c_reset(struct i2c_master_s *dev)
{
    struct sifli_i2c_master_s *bf0_i2c=(struct sifli_i2c_master_s *)dev;

    nxmutex_lock(&bf0_i2c->lock);
    HAL_I2C_Reset(&bf0_i2c->handle);
    nxmutex_unlock(&bf0_i2c->lock);
    return OK;
}
#endif /* CONFIG_I2C_RESET */

/****************************************************************************
 * Name: sifli_i2c_pm_prepare
 *
 * Description:
 *   Request the driver to prepare for a new power state. This is a
 *   warning that the system is about to enter into a new power state.  The
 *   driver should begin whatever operations that may be required to enter
 *   power state.  The driver may abort the state change mode by returning
 *   a non-zero value from the callback function.
 *
 * Input Parameters:
 *   cb      - Returned to the driver.  The driver version of the callback
 *             structure may include additional, driver-specific state
 *             data at the end of the structure.
 *   domain  - Identifies the activity domain of the state change
 *   pmstate - Identifies the new PM state
 *
 * Returned Value:
 *   0 (OK) means the event was successfully processed and that the driver
 *   is prepared for the PM state change.  Non-zero means that the driver
 *   is not prepared to perform the tasks needed achieve this power setting
 *   and will cause the state change to be aborted.  NOTE:  The prepare
 *   method will also be recalled when reverting from lower back to higher
 *   power consumption modes (say because another driver refused a lower
 *   power state change).  Drivers are not permitted to return non-zero
 *   values when reverting back to higher power consumption modes!
 *
 ****************************************************************************/

#ifdef CONFIG_PM
static int sifli_i2c_pm_prepare(struct pm_callback_s *cb, int domain,
                                  enum pm_state_e pmstate)
{
    struct sifli_i2c_master_s *priv =
                           (struct sifli_i2c_master_s *)((char *)cb -
                            offsetof(struct sifli_i2c_master_s, pm_cb));

    /* Logic to prepare for a reduced power state goes here. */
    switch (pmstate)
    {
        case PM_NORMAL:
        case PM_IDLE:
            break;
        case PM_STANDBY:
        case PM_SLEEP:
            /* Check if exclusive lock for I2C bus is held. */
            if (nxmutex_is_locked(&priv->lock))
            {
                /* Exclusive lock is held, do not allow entry to deeper PM states.*/
                return -EBUSY;
            }
          break;
        default:
          /* Should not get here */
          break;
    }

    return OK;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/
static struct sifli_i2c_master_s i2c_obj[I2C_MAX];

/****************************************************************************
 * Name: sifli_i2cbus_initialize
 *
 * Description:
 *   Initialize one I2C bus
 *
 ****************************************************************************/
struct i2c_master_s *sifli_i2cbus_initialize(int port)
{
    /* Init private data for the first time, increment refs count,
    * power-up hardware and configure GPIOs.
    */
    struct sifli_i2c_master_s * priv=(struct sifli_i2c_master_s *)&i2c_obj[port];

    syslog(LOG_INFO, "BRINGUP: i2c%d bus init enter\n", port);

    nxmutex_init(&priv->lock);
#ifndef CONFIG_I2C_POLLED
    nxsem_init(&(priv->sem_isr), 0, 0);
#endif
    nxmutex_lock(&priv->lock);
    priv->port=port;
    sifli_i2c_init(priv);

#ifdef CONFIG_PM
    /* Register to receive power management callbacks */
    DEBUGVERIFY(pm_register(&priv->pm_cb));
#endif

    nxmutex_unlock(&priv->lock);
    syslog(LOG_INFO, "BRINGUP: i2c%d bus init done\n", port);
    return (struct i2c_master_s *)priv;
}

/****************************************************************************
 * Name: sifli_i2cbus_uninitialize
 *
 * Description:
 *   Uninitialize an I2C bus
 *
 ****************************************************************************/

int sifli_i2cbus_uninitialize(struct i2c_master_s *dev)
{
    struct sifli_i2c_master_s *priv;

    DEBUGASSERT(dev);
    priv = (struct sifli_i2c_master_s *)dev;

    nxmutex_lock(&priv->lock);
#ifdef CONFIG_PM
    /* Unregister power management callbacks */
    pm_unregister(&priv->pm_cb);
#endif

    /* Disable power and other HW resource (GPIO's) */
    sifli_i2c_deinit(priv);
    nxmutex_unlock(&priv->lock);

    return OK;
}

