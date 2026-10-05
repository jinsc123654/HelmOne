/****************************************************************************
 * vendor/sifli/chips/sf32lb52/sf32lb_usbdev.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/kmalloc.h>
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>
#include <nuttx/usb/usb.h>
#include <nuttx/usb/usbdev.h>
#include <nuttx/usb/usbdev_trace.h>

#include "arm_internal.h"
#include "chip.h"
#include "bf0_hal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_USBDEV_MAXPOWER
#  define CONFIG_USBDEV_MAXPOWER 100  /* mA */
#endif

/* SF32LB52 MUSB USB controller base address */

#define SF32LB52_USB_BASE           USBC_BASE

/* NuttX IRQ number for USB controller */

#define SF32LB52_IRQ_USB            (NVIC_IRQ_FIRST + 92)

/* Hardware dependent sizes and numbers */

#define SF32LB52_EP0MAXPACKET       64          /* EP0 max packet size */
#define SF32LB52_BULKMAXPACKET      64          /* Bulk endpoint max packet */
#define SF32LB52_INTRMAXPACKET      64          /* Interrupt endpoint max packet */
#define SF32LB52_NENDPOINTS         4           /* EP0 + 3 IN + .. we use 4 */

/* Endpoint numbers */

#define SF32LB52_EP0                0           /* Control endpoint */
#define SF32LB52_EPBULKIN           1           /* Bulk EP for send to host */
#define SF32LB52_EPBULKOUT          2           /* Bulk EP for recv from host */
#define SF32LB52_EPINTRIN           3           /* Intr EP for host poll */

/* FIFO size encoding: log2(size) - 3
 * 8=0, 16=1, 32=2, 64=3, 128=4, 256=5, 512=6, 1024=7
 */

#define USB_FIFOSZ_64               3
#define USB_FIFOSZ_128              4

/* Trace error codes */

#define SF32LB52_TRACEERR_ALLOCFAIL         0x0001
#define SF32LB52_TRACEERR_BINDFAILED        0x0002
#define SF32LB52_TRACEERR_COREIRQREG        0x0003
#define SF32LB52_TRACEERR_DRIVER            0x0004
#define SF32LB52_TRACEERR_DRIVERREGISTERED  0x0005
#define SF32LB52_TRACEERR_EPREAD            0x0006
#define SF32LB52_TRACEERR_EWRITE            0x0007
#define SF32LB52_TRACEERR_INVALIDPARMS      0x0008
#define SF32LB52_TRACEERR_NOEP              0x0009
#define SF32LB52_TRACEERR_NOTCONFIGURED     0x000a
#define SF32LB52_TRACEERR_NULLPACKET        0x000b
#define SF32LB52_TRACEERR_NULLREQUEST       0x000c
#define SF32LB52_TRACEERR_REQABORTED        0x000d
#define SF32LB52_TRACEERR_STALLEDCLRFEATURE 0x000e
#define SF32LB52_TRACEERR_STALLEDISPATCH    0x000f
#define SF32LB52_TRACEERR_STALLEDGETST      0x0010
#define SF32LB52_TRACEERR_STALLEDGETSTEP    0x0011
#define SF32LB52_TRACEERR_STALLEDGETSTRECIP 0x0012
#define SF32LB52_TRACEERR_STALLEDREQUEST    0x0013
#define SF32LB52_TRACEERR_STALLEDSETFEATURE 0x0014

/* Trace interrupt codes */

#define SF32LB52_TRACEINTID_CLEARFEATURE    0x0001
#define SF32LB52_TRACEINTID_CONTROL         0x0002
#define SF32LB52_TRACEINTID_DISPATCH        0x0003
#define SF32LB52_TRACEINTID_GETENDPOINT     0x0004
#define SF32LB52_TRACEINTID_GETIFDEV        0x0005
#define SF32LB52_TRACEINTID_GETSETDESC      0x0006
#define SF32LB52_TRACEINTID_GETSETIFCONFIG  0x0007
#define SF32LB52_TRACEINTID_GETSTATUS       0x0008
#define SF32LB52_TRACEINTID_RESET           0x0009
#define SF32LB52_TRACEINTID_RESUME          0x000a
#define SF32LB52_TRACEINTID_RXFIFO          0x000b
#define SF32LB52_TRACEINTID_RXPKTRDY        0x000c
#define SF32LB52_TRACEINTID_SETADDRESS      0x000d
#define SF32LB52_TRACEINTID_SETFEATURE      0x000e
#define SF32LB52_TRACEINTID_SOF             0x000f
#define SF32LB52_TRACEINTID_SUSPEND         0x0010
#define SF32LB52_TRACEINTID_SYNCHFRAME      0x0011
#define SF32LB52_TRACEINTID_TESTMODE        0x0012
#define SF32LB52_TRACEINTID_TXFIFO          0x0013
#define SF32LB52_TRACEINTID_TXFIFOSETEND    0x0014
#define SF32LB52_TRACEINTID_TXFIFOSTALL     0x0015
#define SF32LB52_TRACEINTID_TXPKTRDY        0x0016
#define SF32LB52_TRACEINTID_UNKNOWN         0x0017
#define SF32LB52_TRACEINTID_USBCTLR         0x0018
#define SF32LB52_TRACEINTID_DISCONNECT      0x0019

/* Request queue operations */

#define sf32lb52_rqempty(ep)       ((ep)->head == NULL)
#define sf32lb52_rqpeek(ep)        ((ep)->head)

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* A container for a request so that the request may be retained in a list */

struct sf32lb52_req_s
{
  struct usbdev_req_s     req;          /* Standard USB request */
  struct sf32lb52_req_s  *flink;        /* Supports a singly linked list */
};

/* This is the internal representation of an endpoint */

struct sf32lb52_ep_s
{
  /* Common endpoint fields.  This must be the first thing defined in the
   * structure so that it is possible to simply cast from struct usbdev_ep_s
   * to struct sf32lb52_ep_s.
   */

  struct usbdev_ep_s        ep;         /* Standard endpoint structure */

  /* SF32LB52-specific fields */

  struct sf32lb52_usbdev_s *dev;        /* Reference to private driver data */
  struct sf32lb52_req_s    *head;       /* Request list for this endpoint */
  struct sf32lb52_req_s    *tail;
  uint8_t                   epphy;      /* Physical EP address/index */
  uint8_t                   stalled:1;  /* Endpoint is halted */
  uint8_t                   in:1;       /* Endpoint is IN only */
  uint8_t                   halted:1;   /* Endpoint feature halted */
  uint8_t                   txnullpkt:1; /* Null packet needed at end */
  uint8_t                   zlparmed:1;  /* Explicit ZLP request armed */
};

/* This structure encapsulates the overall driver state */

struct sf32lb52_usbdev_s
{
  /* Common device fields.  This must be the first thing defined in the
   * structure so that it is possible to simply cast from struct usbdev_s
   * to struct sf32lb52_usbdev_s.
   */

  struct usbdev_s                usbdev;

  /* The bound device class driver */

  struct usbdevclass_driver_s   *driver;

  /* SF32LB52-specific fields */

  volatile USBC_X_Typedef       *hw;     /* USB controller registers */
  uint8_t                        stalled:1;
  uint8_t                        selfpowered:1;
  uint8_t                        paddrset:1;
  uint8_t                        rxpending:1;
  uint8_t                        paddr;

  /* The endpoint list */

  struct sf32lb52_ep_s           eplist[SF32LB52_NENDPOINTS];
};

/* For maintaining tables of endpoint info */

struct sf32lb52_epinfo_s
{
  uint8_t                addr;          /* Logical endpoint address */
  uint8_t                attr;          /* Endpoint attributes */
  uint8_t                maxpacket;     /* Max packet size */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Request queue operations */

static struct sf32lb52_req_s *sf32lb52_rqdequeue(
                                struct sf32lb52_ep_s *privep);
static void sf32lb52_rqenqueue(struct sf32lb52_ep_s *privep,
                               struct sf32lb52_req_s *req);

/* Low level data transfers and request operations */

static int  sf32lb52_ep0write(struct sf32lb52_usbdev_s *priv,
                              uint8_t *buf, uint16_t nbytes);
static int  sf32lb52_epwrite(struct sf32lb52_usbdev_s *priv,
                             uint8_t epphy, uint8_t *buf, uint16_t nbytes);
static int  sf32lb52_epread(struct sf32lb52_usbdev_s *priv,
                            uint8_t epphy, uint8_t *buf, uint16_t nbytes);
static inline void sf32lb52_abortrequest(struct sf32lb52_ep_s *privep,
                                         struct sf32lb52_req_s *privreq,
                                         int16_t result);
static void sf32lb52_reqcomplete(struct sf32lb52_ep_s *privep,
                                 int16_t result);
static int  sf32lb52_wrrequest(struct sf32lb52_ep_s *privep);
static int  sf32lb52_rdrequest(struct sf32lb52_ep_s *privep);
static void sf32lb52_cancelrequests(struct sf32lb52_ep_s *privep);

/* Interrupt handling */

static struct sf32lb52_ep_s *sf32lb52_epfindbyaddr(
                                struct sf32lb52_usbdev_s *priv,
                                uint16_t eplog);
static void sf32lb52_dispatchrequest(struct sf32lb52_usbdev_s *priv,
                                     const struct usb_ctrlreq_s *ctrl);
static void sf32lb52_ep0setup(struct sf32lb52_usbdev_s *priv);
static int  sf32lb52_interrupt(int irq, void *context, void *arg);

/* Initialization operations */

static void sf32lb52_epreset(struct sf32lb52_usbdev_s *priv,
                             unsigned int index);
static void sf32lb52_epinitialize(struct sf32lb52_usbdev_s *priv);
static void sf32lb52_ctrlinitialize(struct sf32lb52_usbdev_s *priv);

/* Endpoint methods */

static int  sf32lb52_epconfigure(struct usbdev_ep_s *ep,
                                 const struct usb_epdesc_s *desc,
                                 bool last);
static int  sf32lb52_epdisable(struct usbdev_ep_s *ep);
static struct usbdev_req_s *sf32lb52_epallocreq(struct usbdev_ep_s *ep);
static void sf32lb52_epfreereq(struct usbdev_ep_s *ep,
                               struct usbdev_req_s *req);
static int  sf32lb52_epsubmit(struct usbdev_ep_s *ep,
                              struct usbdev_req_s *req);
static int  sf32lb52_epcancel(struct usbdev_ep_s *ep,
                              struct usbdev_req_s *req);
static int  sf32lb52_epstall(struct usbdev_ep_s *ep, bool resume);

/* USB device controller methods */

static struct usbdev_ep_s *sf32lb52_allocep(struct usbdev_s *dev,
                                            uint8_t epno, bool in,
                                            uint8_t eptype);
static void sf32lb52_freeep(struct usbdev_s *dev,
                            struct usbdev_ep_s *ep);
static int  sf32lb52_getframe(struct usbdev_s *dev);
static int  sf32lb52_wakeup(struct usbdev_s *dev);
static int  sf32lb52_selfpowered(struct usbdev_s *dev, bool selfpowered);
static int  sf32lb52_pullup(struct usbdev_s *dev, bool enable);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Endpoint methods */

static const struct usbdev_epops_s g_epops =
{
  .configure   = sf32lb52_epconfigure,
  .disable     = sf32lb52_epdisable,
  .allocreq    = sf32lb52_epallocreq,
  .freereq     = sf32lb52_epfreereq,
  .submit      = sf32lb52_epsubmit,
  .cancel      = sf32lb52_epcancel,
  .stall       = sf32lb52_epstall,
};

/* USB controller device methods */

static const struct usbdev_ops_s g_devops =
{
  .allocep     = sf32lb52_allocep,
  .freeep      = sf32lb52_freeep,
  .getframe    = sf32lb52_getframe,
  .wakeup      = sf32lb52_wakeup,
  .selfpowered = sf32lb52_selfpowered,
  .pullup      = sf32lb52_pullup,
};

/* Single pre-allocated driver instance */

static struct sf32lb52_usbdev_s g_usbdev;

/* Endpoint info table:
 * EP0: Control IN/OUT
 * EP1: Bulk IN
 * EP2: Bulk OUT
 * EP3: Interrupt IN
 */

static const struct sf32lb52_epinfo_s g_epinfo[SF32LB52_NENDPOINTS] =
{
  {
    .addr      = 0,
    .attr      = USB_EP_ATTR_XFER_CONTROL,
    .maxpacket = SF32LB52_EP0MAXPACKET,
  },
  {
    .addr      = SF32LB52_EPBULKIN | USB_DIR_IN,
    .attr      = USB_EP_ATTR_XFER_BULK,
    .maxpacket = SF32LB52_BULKMAXPACKET,
  },
  {
    .addr      = SF32LB52_EPBULKOUT | USB_DIR_OUT,
    .attr      = USB_EP_ATTR_XFER_BULK,
    .maxpacket = SF32LB52_BULKMAXPACKET,
  },
  {
    .addr      = SF32LB52_EPINTRIN | USB_DIR_IN,
    .attr      = USB_EP_ATTR_XFER_INT,
    .maxpacket = SF32LB52_INTRMAXPACKET,
  },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sf32lb52_rqdequeue
 ****************************************************************************/

static struct sf32lb52_req_s *sf32lb52_rqdequeue(
                                struct sf32lb52_ep_s *privep)
{
  struct sf32lb52_req_s *ret = privep->head;

  if (ret)
    {
      privep->head = ret->flink;
      if (!privep->head)
        {
          privep->tail = NULL;
        }

      ret->flink = NULL;
    }

  return ret;
}

/****************************************************************************
 * Name: sf32lb52_rqenqueue
 ****************************************************************************/

static void sf32lb52_rqenqueue(struct sf32lb52_ep_s *privep,
                               struct sf32lb52_req_s *req)
{
  req->flink = NULL;
  if (!privep->head)
    {
      privep->head = req;
      privep->tail = req;
    }
  else
    {
      privep->tail->flink = req;
      privep->tail        = req;
    }
}

/****************************************************************************
 * Name: sf32lb52_ep0write
 *
 * Description:
 *   Control endpoint write (IN). Uses indexed registers (index=0).
 *
 ****************************************************************************/

static int sf32lb52_ep0write(struct sf32lb52_usbdev_s *priv,
                             uint8_t *buf, uint16_t nbytes)
{
  volatile USBC_X_Typedef *hw = priv->hw;
  uint16_t bytesleft;
  uint16_t nwritten;
  uint16_t csr0;

  hw->index = 0;

  if (nbytes <= SF32LB52_EP0MAXPACKET)
    {
      bytesleft = nbytes;
      csr0 = USB_CSR0_TXPKTRDY | USB_CSR0_P_DATAEND;
    }
  else
    {
      bytesleft = SF32LB52_EP0MAXPACKET;
      csr0 = USB_CSR0_TXPKTRDY;
    }

  nwritten = bytesleft;

  /* Write data to FIFO using 32-bit access when possible */

  if (((uintptr_t)buf & 0x03) == 0)
    {
      uint32_t *buf32 = (uint32_t *)buf;
      while (bytesleft >= 4)
        {
          hw->fifox[0] = *buf32++;
          bytesleft -= 4;
        }

      buf = (uint8_t *)buf32;
    }

  while (bytesleft > 0)
    {
      *(volatile uint8_t *)&hw->fifox[0] = *buf++;
      bytesleft--;
    }

  hw->csr0_txcsr = csr0;
  return nwritten;
}

/****************************************************************************
 * Name: sf32lb52_epwrite
 *
 * Description:
 *   Non-control endpoint write (IN). Selects the endpoint via index
 *   register, writes to the FIFO, then sets TXPKTRDY.
 *
 ****************************************************************************/

static int sf32lb52_epwrite(struct sf32lb52_usbdev_s *priv,
                            uint8_t epphy, uint8_t *buf, uint16_t nbytes)
{
  volatile USBC_X_Typedef *hw = priv->hw;
  uint16_t bytesleft;
  int ret;

  if (epphy >= SF32LB52_NENDPOINTS)
    {
      return -EINVAL;
    }

  hw->index = epphy;

  if (epphy == 0)
    {
      return sf32lb52_ep0write(priv, buf, nbytes);
    }

  bytesleft = SF32LB52_BULKMAXPACKET;
  if (bytesleft > nbytes)
    {
      bytesleft = nbytes;
    }

  ret = bytesleft;

  /* Flush FIFO if not empty */

  if (hw->csr0_txcsr & USB_TXCSR_FIFONOTEMPTY)
    {
      hw->csr0_txcsr = (hw->csr0_txcsr & ~USB_TXCSR_P_WZC_BITS)
                        | USB_TXCSR_FLUSHFIFO;
    }

  /* Write data to FIFO using 32-bit access when possible */

  if (((uintptr_t)buf & 0x03) == 0)
    {
      uint32_t *buf32 = (uint32_t *)buf;
      while (bytesleft >= 4)
        {
          hw->fifox[epphy] = *buf32++;
          bytesleft -= 4;
        }

      buf = (uint8_t *)buf32;
    }

  while (bytesleft > 0)
    {
      *(volatile uint8_t *)&hw->fifox[epphy] = *buf++;
      bytesleft--;
    }

  /* Set TXPKTRDY to start transmission */

  hw->csr0_txcsr = (hw->csr0_txcsr & ~USB_TXCSR_P_WZC_BITS)
                    | USB_TXCSR_TXPKTRDY;
  return ret;
}

/****************************************************************************
 * Name: sf32lb52_epread
 *
 * Description:
 *   Endpoint read (OUT). Reads data from FIFO and clears RXPKTRDY.
 *
 ****************************************************************************/

static int sf32lb52_epread(struct sf32lb52_usbdev_s *priv,
                           uint8_t epphy, uint8_t *buf, uint16_t nbytes)
{
  volatile USBC_X_Typedef *hw = priv->hw;
  int bytesleft;

  if (epphy >= SF32LB52_NENDPOINTS)
    {
      return -EINVAL;
    }

  hw->index = epphy;

  if (epphy == 0)
    {
      /* EP0: read count from count0 field (lower byte of rxcount) */

      bytesleft = hw->rxcount & 0x7f;  /* count0 is 7 bits */
    }
  else
    {
      bytesleft = hw->rxcount;
    }

  if (bytesleft > nbytes)
    {
      bytesleft = nbytes;
    }

  int ret = bytesleft;

  /* Read data from FIFO using 32-bit access when possible */

  if (((uintptr_t)buf & 0x03) == 0)
    {
      uint32_t *buf32 = (uint32_t *)buf;
      while (bytesleft >= 4)
        {
          *buf32++ = hw->fifox[epphy];
          bytesleft -= 4;
        }

      buf = (uint8_t *)buf32;
    }

  while (bytesleft > 0)
    {
      *buf++ = *(volatile uint8_t *)&hw->fifox[epphy];
      bytesleft--;
    }

  /* Clear RXPKTRDY for non-EP0 endpoints.
   * For EP0, the caller (ep0setup) will clear RXPKTRDY with appropriate
   * flags (SVDRXPKTRDY alone or SVDRXPKTRDY | DATAEND) depending on
   * whether there is a data phase.
   */

  if (epphy != 0)
    {
      hw->rxcsr = hw->rxcsr & ~(USB_RXCSR_P_WZC_BITS | USB_RXCSR_RXPKTRDY);
    }

  return ret;
}

/****************************************************************************
 * Name: sf32lb52_abortrequest
 ****************************************************************************/

static inline void sf32lb52_abortrequest(struct sf32lb52_ep_s *privep,
                                         struct sf32lb52_req_s *privreq,
                                         int16_t result)
{
  usbtrace(TRACE_DEVERROR(SF32LB52_TRACEERR_REQABORTED),
           (uint16_t)privep->epphy);

  privreq->req.result = result;
  privreq->req.callback(&privep->ep, &privreq->req);
}

/****************************************************************************
 * Name: sf32lb52_reqcomplete
 ****************************************************************************/

static void sf32lb52_reqcomplete(struct sf32lb52_ep_s *privep,
                                 int16_t result)
{
  struct sf32lb52_req_s *privreq;
  int stalled = privep->stalled;
  irqstate_t flags;

  flags = enter_critical_section();
  privreq = sf32lb52_rqdequeue(privep);
  leave_critical_section(flags);

  if (privreq)
    {
      if (privep->epphy == 0)
        {
          if (privep->dev->stalled)
            {
              privep->stalled = 1;
            }
        }

      privreq->req.result = result;
      privreq->flink = NULL;
      privreq->req.callback(&privep->ep, &privreq->req);

      privep->stalled = stalled;
    }
}

/****************************************************************************
 * Name: sf32lb52_wrrequest
 *
 * Description:
 *   Send from the next queued write request
 *
 ****************************************************************************/

static int sf32lb52_wrrequest(struct sf32lb52_ep_s *privep)
{
  struct sf32lb52_req_s *privreq;
  uint8_t *buf;
  int nbytes;
  int bytesleft;
  int nbyteswritten;

  privreq = sf32lb52_rqpeek(privep);
  if (!privreq)
    {
      usbtrace(TRACE_DEVERROR(SF32LB52_TRACEERR_NULLREQUEST), 0);
      return OK;
    }

  bytesleft = privreq->req.len - privreq->req.xfrd;

  usbtrace(TRACE_WRITE(privep->epphy), privreq->req.xfrd);

  /* If a packet is still pending in the FIFO (TXPKTRDY set, i.e. hardware
   * has not yet handed it to the host), the endpoint is busy.  Do NOT write
   * anything now: the TX interrupt will call this function again once the
   * in-flight packet has been sent.
   *
   * This guard is essential because sf32lb52_ep_submit() calls this function
   * synchronously whenever the class driver enqueues a new request (e.g. the
   * MTP responder issuing the response container right after the data-phase
   * write()).  Without it, sf32lb52_epwrite() would see FIFONOTEMPTY and
   * FLUSH the not-yet-sent packet, corrupting the tail of a multi-packet IN
   * transfer (this broke MTP after GetDeviceInfo: the host never received the
   * final response, timed out and kept re-issuing OpenSession).
   */

  if (privep->epphy != 0)
    {
      privep->dev->hw->index = privep->epphy;
      if ((privep->dev->hw->csr0_txcsr & USB_TXCSR_TXPKTRDY) != 0)
        {
          return OK;
        }
    }

  /* Send at most ONE packet per call.  The MUSB FIFO holds a single
   * in-flight packet, so the next packet must wait until the current one
   * has been sent/ACKed, which is signalled by the EP TX interrupt that
   * calls this function again.  Writing several packets back-to-back here
   * would make sf32lb52_epwrite() FLUSH the not-yet-sent packet, so only
   * the last packet of a multi-packet IN transfer would reach the host
   * (this is what broke MTP GetDeviceInfo and any transfer > maxpacket).
   */

  if (bytesleft > 0 || privep->txnullpkt)
    {
      privep->txnullpkt = 0;
      if (bytesleft > privep->ep.maxpacket)
        {
          nbytes = privep->ep.maxpacket;
        }
      else
        {
          nbytes = bytesleft;
          if ((privreq->req.flags & USBDEV_REQFLAGS_NULLPKT) != 0)
            {
              privep->txnullpkt = (bytesleft == privep->ep.maxpacket);
            }
        }

      buf = privreq->req.buf + privreq->req.xfrd;
      nbyteswritten = sf32lb52_epwrite(privep->dev, privep->epphy,
                                       buf, nbytes);
      if (nbyteswritten < 0 || nbyteswritten != nbytes)
        {
          usbtrace(TRACE_DEVERROR(SF32LB52_TRACEERR_EWRITE),
                   nbyteswritten);
          return ERROR;
        }

      privreq->req.xfrd += nbytes;

      /* Packet queued; wait for the TX interrupt before sending the next. */

      return OK;
    }

  /* Explicit zero-length transfer request (e.g. the MTP ZLP that terminates
   * a data phase whose length is an exact multiple of maxpacket).  Emit
   * exactly one zero-length packet, then complete on the next TX interrupt.
   *
   * This is routed through the request queue (rather than poking TXPKTRDY
   * directly from sf32lb52_ep_submit and completing immediately) so that:
   *   - the ZLP is ordered AFTER any in-flight/queued data packets, and
   *   - the busy-guard above holds it off until the previous packet has
   *     actually been sent, and
   *   - the request only completes once the ZLP has really been handed to
   *     the hardware.
   * The old immediate-send path raced with in-flight data packets, so the
   * host never saw the terminating short packet and stalled until its bulk
   * timeout (~seconds) on every multiple-of-maxpacket transfer.
   */

  if (privreq->req.len == 0)
    {
      if (!privep->zlparmed)
        {
          privep->zlparmed = 1;
          privep->dev->hw->index = privep->epphy;
          privep->dev->hw->csr0_txcsr =
              (privep->dev->hw->csr0_txcsr & ~USB_TXCSR_P_WZC_BITS)
              | USB_TXCSR_TXPKTRDY;
          return OK;
        }

      privep->zlparmed = 0;
      usbtrace(TRACE_COMPLETE(privep->epphy), 0);
      sf32lb52_reqcomplete(privep, OK);

      if (!sf32lb52_rqempty(privep))
        {
          return sf32lb52_wrrequest(privep);
        }

      return OK;
    }

  /* No data left and no pending ZLP: the write request is complete. */

  if (privreq->req.xfrd >= privreq->req.len && !privep->txnullpkt)
    {
      usbtrace(TRACE_COMPLETE(privep->epphy), privreq->req.xfrd);
      privep->txnullpkt = 0;
      sf32lb52_reqcomplete(privep, OK);

      /* Start the next queued write request, if any. */

      if (!sf32lb52_rqempty(privep))
        {
          return sf32lb52_wrrequest(privep);
        }
    }

  return OK;
}

/****************************************************************************
 * Name: sf32lb52_rdrequest
 *
 * Description:
 *   Receive to the next queued read request
 *
 ****************************************************************************/

static int sf32lb52_rdrequest(struct sf32lb52_ep_s *privep)
{
  struct sf32lb52_req_s *privreq;
  uint8_t *buf;
  int nbytesread;

  privreq = sf32lb52_rqpeek(privep);
  if (!privreq)
    {
      usbtrace(TRACE_DEVERROR(SF32LB52_TRACEERR_NULLREQUEST), 0);
      return OK;
    }

  usbtrace(TRACE_READ(privep->epphy), privreq->req.xfrd);

  buf = privreq->req.buf + privreq->req.xfrd;
  nbytesread = sf32lb52_epread(privep->dev, privep->epphy,
                               buf, privep->ep.maxpacket);
  if (nbytesread < 0)
    {
      usbtrace(TRACE_DEVERROR(SF32LB52_TRACEERR_EPREAD), nbytesread);
      return ERROR;
    }

  privreq->req.xfrd += nbytesread;
  if (privreq->req.xfrd >= privreq->req.len ||
      nbytesread < privep->ep.maxpacket)
    {
      usbtrace(TRACE_COMPLETE(privep->epphy), privreq->req.xfrd);
      sf32lb52_reqcomplete(privep, OK);
    }

  return OK;
}

/****************************************************************************
 * Name: sf32lb52_cancelrequests
 ****************************************************************************/

static void sf32lb52_cancelrequests(struct sf32lb52_ep_s *privep)
{
  while (!sf32lb52_rqempty(privep))
    {
      usbtrace(TRACE_COMPLETE(privep->epphy),
               (sf32lb52_rqpeek(privep))->req.xfrd);
      sf32lb52_reqcomplete(privep, -ESHUTDOWN);
    }
}

/****************************************************************************
 * Name: sf32lb52_epfindbyaddr
 ****************************************************************************/

static struct sf32lb52_ep_s *sf32lb52_epfindbyaddr(
                                struct sf32lb52_usbdev_s *priv,
                                uint16_t eplog)
{
  struct sf32lb52_ep_s *privep;
  int i;

  if (USB_EPNO(eplog) == 0)
    {
      return &priv->eplist[0];
    }

  for (i = 1; i < SF32LB52_NENDPOINTS; i++)
    {
      privep = &priv->eplist[i];
      if (eplog == privep->ep.eplog)
        {
          return privep;
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: sf32lb52_dispatchrequest
 ****************************************************************************/

static void sf32lb52_dispatchrequest(struct sf32lb52_usbdev_s *priv,
                                     const struct usb_ctrlreq_s *ctrl)
{
  int ret;

  usbtrace(TRACE_INTDECODE(SF32LB52_TRACEINTID_DISPATCH), 0);
  if (priv && priv->driver)
    {
      ret = CLASS_SETUP(priv->driver, &priv->usbdev, ctrl, NULL, 0);
      if (ret < 0)
        {
          usbtrace(TRACE_DEVERROR(SF32LB52_TRACEERR_STALLEDISPATCH),
                   ctrl->req);
          priv->stalled = 1;
        }
    }
}

/****************************************************************************
 * Name: sf32lb52_ep0setup
 *
 * Description:
 *   USB Ctrl EP Setup Event. Reads the 8-byte setup packet from EP0 FIFO
 *   and handles standard requests or dispatches to class driver.
 *
 ****************************************************************************/

static void sf32lb52_ep0setup(struct sf32lb52_usbdev_s *priv)
{
  struct sf32lb52_ep_s *ep0 = &priv->eplist[SF32LB52_EP0];
  struct sf32lb52_req_s *privreq = sf32lb52_rqpeek(ep0);
  struct sf32lb52_ep_s *privep;
  struct usb_ctrlreq_s ctrl;
  volatile USBC_X_Typedef *hw = priv->hw;
  uint8_t ep0payload[SF32LB52_EP0MAXPACKET];
  uint16_t index;
  uint16_t value;
  uint16_t len;
  uint16_t count0;
  uint16_t csr0;
  int ret;

  if (priv->usbdev.speed == USB_SPEED_UNKNOWN)
    {
      priv->usbdev.speed = USB_SPEED_FULL;
    }

  /* Terminate any pending requests on EP0 */

  while (!sf32lb52_rqempty(ep0))
    {
      int16_t result = OK;
      if (privreq->req.xfrd != privreq->req.len)
        {
          result = -EPROTO;
        }

      usbtrace(TRACE_COMPLETE(ep0->epphy), privreq->req.xfrd);
      sf32lb52_reqcomplete(ep0, result);
      privreq = sf32lb52_rqpeek(ep0);
    }

  ep0->stalled  = 0;
  priv->stalled = 0;

  /* Clear SETUPEND if set */

  hw->index = 0;
  csr0 = hw->csr0_txcsr;
  if (csr0 & USB_CSR0_P_SETUPEND)
    {
      hw->csr0_txcsr = USB_CSR0_P_SVDSETUPEND;
    }

  /* Distinguish 8-byte setup packets from EP0 OUT data stage payload.
   * CDC ACM open may send class-specific OUT data (for example
   * SET_LINE_CODING). Do not parse that payload as a setup packet.
   */

  count0 = hw->rxcount & 0x7f;
  if (count0 != USB_SIZEOF_CTRLREQ)
    {
      if (count0 > 0)
        {
          if (count0 > sizeof(ep0payload))
            {
              count0 = sizeof(ep0payload);
            }

          (void)sf32lb52_epread(priv, 0, ep0payload, count0);
        }

      hw->index = 0;
      hw->csr0_txcsr = USB_CSR0_P_SVDRXPKTRDY;
      return;
    }

  /* Read the setup packet from EP0 FIFO (without clearing RXPKTRDY) */

  ret = sf32lb52_epread(priv, 0, (uint8_t *)&ctrl, USB_SIZEOF_CTRLREQ);
  if (ret <= 0)
    {
      /* Clear RXPKTRDY even on error */

      hw->index = 0;
      hw->csr0_txcsr = USB_CSR0_P_SVDRXPKTRDY;
      return;
    }

  index = GETUINT16(ctrl.index);
  value = GETUINT16(ctrl.value);
  len   = GETUINT16(ctrl.len);

  uinfo("type=%02x req=%02x value=%04x index=%04x len=%04x\n",
        ctrl.type, ctrl.req, value, index, len);

  /* Dispatch non-standard requests */

  ep0->in = (ctrl.type & USB_DIR_IN) != 0;
  if ((ctrl.type & USB_REQ_TYPE_MASK) != USB_REQ_TYPE_STANDARD)
    {
      /* For class/vendor requests, only clear RXPKTRDY and let CLASS_SETUP
       * submit the proper EP0 response/status stage.
       */

      hw->index = 0;
      hw->csr0_txcsr = USB_CSR0_P_SVDRXPKTRDY;

      sf32lb52_dispatchrequest(priv, &ctrl);
      return;
    }

  /* Handle standard requests */

  switch (ctrl.req)
    {
    case USB_REQ_GETSTATUS:
      {
        uint8_t status[2] =
        {
          0, 0
        };

        usbtrace(TRACE_INTDECODE(SF32LB52_TRACEINTID_GETSTATUS), 0);

        if (len != 2 || (ctrl.type & USB_REQ_DIR_IN) == 0 || value != 0)
          {
            usbtrace(TRACE_DEVERROR(SF32LB52_TRACEERR_STALLEDGETST),
                     ctrl.req);
            priv->stalled = 1;
          }
        else
          {
            switch (ctrl.type & USB_REQ_RECIPIENT_MASK)
              {
              case USB_REQ_RECIPIENT_ENDPOINT:
                {
                  usbtrace(TRACE_INTDECODE(
                    SF32LB52_TRACEINTID_GETENDPOINT), 0);
                  privep = sf32lb52_epfindbyaddr(priv, index);
                  if (!privep)
                    {
                      usbtrace(TRACE_DEVERROR(
                        SF32LB52_TRACEERR_STALLEDGETSTEP), ctrl.type);
                      priv->stalled = 1;
                    }
                  else
                    {
                      /* D0 = endpoint halt (stall) status */

                      status[0] = privep->halted ? 1 : 0;
                    }
                }
                break;

              case USB_REQ_RECIPIENT_DEVICE:
                usbtrace(TRACE_INTDECODE(SF32LB52_TRACEINTID_GETIFDEV), 0);

                /* D0 = self-powered, D1 = remote wakeup (unsupported) */

                status[0] = priv->selfpowered ? 1 : 0;
                break;

              case USB_REQ_RECIPIENT_INTERFACE:
                usbtrace(TRACE_INTDECODE(SF32LB52_TRACEINTID_GETIFDEV), 0);

                /* Reserved, must be returned as zero */

                break;

              default:
                {
                  usbtrace(TRACE_DEVERROR(
                    SF32LB52_TRACEERR_STALLEDGETSTRECIP), ctrl.type);
                  priv->stalled = 1;
                }
                break;
              }
          }

        /* GET_STATUS is a control-read: the controller MUST answer it
         * directly with the 2-byte status.  It must not be dispatched to the
         * class layer -- usbdev_fs forwards everything except SET_CONFIG to
         * the MTP responder in user space, which never answers a standard
         * GET_STATUS.  The host then blocks until its control-transfer
         * timeout (~seconds) on every endpoint-status poll, which is exactly
         * what made libmtp print
         *   "usb_get_endpoint_status(): Resource temporarily unavailable"
         * and stalled the file browser for seconds at a time.
         */

        if (!priv->stalled)
          {
            hw->index = 0;
            hw->csr0_txcsr = USB_CSR0_P_SVDRXPKTRDY;
            sf32lb52_ep0write(priv, status, sizeof(status));
          }
      }
      break;

    case USB_REQ_CLEARFEATURE:
      {
        usbtrace(TRACE_INTDECODE(SF32LB52_TRACEINTID_CLEARFEATURE),
                 (uint16_t)ctrl.req);

        if (ctrl.type != USB_REQ_RECIPIENT_ENDPOINT)
          {
            /* Class layer handles status stage */

            hw->index = 0;
            hw->csr0_txcsr = USB_CSR0_P_SVDRXPKTRDY;
            sf32lb52_dispatchrequest(priv, &ctrl);
          }
        else if (value == USB_FEATURE_ENDPOINTHALT && len == 0 &&
                 (privep = sf32lb52_epfindbyaddr(priv, index)) != NULL)
          {
            hw->index = 0;
            hw->csr0_txcsr = USB_CSR0_P_SVDRXPKTRDY | USB_CSR0_P_DATAEND;
            privep->halted = 0;
            sf32lb52_wrrequest(privep);
          }
        else
          {
            usbtrace(TRACE_DEVERROR(SF32LB52_TRACEERR_STALLEDCLRFEATURE),
                     ctrl.type);
            priv->stalled = 1;
          }
      }
      break;

    case USB_REQ_SETFEATURE:
      {
        usbtrace(TRACE_INTDECODE(SF32LB52_TRACEINTID_SETFEATURE), 0);

        if (ctrl.type == USB_REQ_RECIPIENT_DEVICE &&
            value == USB_FEATURE_TESTMODE)
          {
            hw->index = 0;
            hw->csr0_txcsr = USB_CSR0_P_SVDRXPKTRDY | USB_CSR0_P_DATAEND;
            usbtrace(TRACE_INTDECODE(SF32LB52_TRACEINTID_TESTMODE),
                     index);
          }
        else if (ctrl.type != USB_REQ_RECIPIENT_ENDPOINT)
          {
            /* Class layer handles status stage */

            hw->index = 0;
            hw->csr0_txcsr = USB_CSR0_P_SVDRXPKTRDY;
            sf32lb52_dispatchrequest(priv, &ctrl);
          }
        else if (value == USB_FEATURE_ENDPOINTHALT && len == 0 &&
                 (privep = sf32lb52_epfindbyaddr(priv, index)) != NULL)
          {
            hw->index = 0;
            hw->csr0_txcsr = USB_CSR0_P_SVDRXPKTRDY | USB_CSR0_P_DATAEND;
            privep->halted = 1;
          }
        else
          {
            usbtrace(TRACE_DEVERROR(SF32LB52_TRACEERR_STALLEDSETFEATURE),
                     ctrl.type);
            priv->stalled = 1;
          }
      }
      break;

    case USB_REQ_SETADDRESS:
      {
        usbtrace(TRACE_INTDECODE(SF32LB52_TRACEINTID_SETADDRESS), 0);

        /* SET_ADDRESS has no data phase -- must write SVDRXPKTRDY and
         * DATAEND atomically.
         */

        hw->index = 0;
        hw->csr0_txcsr = USB_CSR0_P_SVDRXPKTRDY | USB_CSR0_P_DATAEND;
        priv->paddr = value & 0x7f;
      }
      break;

    case USB_REQ_GETDESCRIPTOR:
    case USB_REQ_SETDESCRIPTOR:
      {
        usbtrace(TRACE_INTDECODE(SF32LB52_TRACEINTID_GETSETDESC), 0);

        /* GET_DESCRIPTOR has an IN data phase. Clear RXPKTRDY only
         * (without DATAEND) to allow the data phase to proceed.
         */

        hw->index = 0;
        hw->csr0_txcsr = USB_CSR0_P_SVDRXPKTRDY;
        sf32lb52_dispatchrequest(priv, &ctrl);
      }
      break;

    case USB_REQ_GETCONFIGURATION:
    case USB_REQ_GETINTERFACE:
      {
        usbtrace(TRACE_INTDECODE(SF32LB52_TRACEINTID_GETSETIFCONFIG), 0);

        /* IN data phase */

        hw->index = 0;
        hw->csr0_txcsr = USB_CSR0_P_SVDRXPKTRDY;
        sf32lb52_dispatchrequest(priv, &ctrl);
      }
      break;

    case USB_REQ_SETCONFIGURATION:
    case USB_REQ_SETINTERFACE:
      {
        usbtrace(TRACE_INTDECODE(SF32LB52_TRACEINTID_GETSETIFCONFIG), 0);

        /* Let class layer complete the status stage. */

        hw->index = 0;
        hw->csr0_txcsr = USB_CSR0_P_SVDRXPKTRDY;
        sf32lb52_dispatchrequest(priv, &ctrl);
      }
      break;

    case USB_REQ_SYNCHFRAME:
      {
        hw->index = 0;
        hw->csr0_txcsr = USB_CSR0_P_SVDRXPKTRDY | USB_CSR0_P_SENDSTALL;
        usbtrace(TRACE_INTDECODE(SF32LB52_TRACEINTID_SYNCHFRAME), 0);
      }
      break;

    default:
      {
        hw->index = 0;
        hw->csr0_txcsr = USB_CSR0_P_SVDRXPKTRDY | USB_CSR0_P_SENDSTALL;
        usbtrace(TRACE_DEVERROR(SF32LB52_TRACEERR_STALLEDREQUEST),
                 ctrl.req);
        priv->stalled = 1;
      }
      break;
    }

  /* If stalled, send STALL handshake */

  if (priv->stalled)
    {
      hw->index = 0;
      hw->csr0_txcsr = USB_CSR0_P_SENDSTALL;
    }
}

/****************************************************************************
 * Name: sf32lb52_interrupt
 *
 * Description:
 *   Handle USB controller interrupt
 *
 ****************************************************************************/

static int sf32lb52_interrupt(int irq, void *context, void *arg)
{
  struct sf32lb52_usbdev_s *priv = &g_usbdev;
  volatile USBC_X_Typedef *hw = priv->hw;
  struct sf32lb52_ep_s *privep;
  uint16_t intrtx;
  uint16_t intrrx;
  uint8_t  intrusb;
  uint16_t csr0;
  uint8_t  old_index;

  usbtrace(TRACE_INTENTRY(SF32LB52_TRACEINTID_USBCTLR), 0);

  /* Save current index register */

  old_index = hw->index;

  /* Read and clear interrupt status registers */

  intrtx  = hw->intrtx;
  intrrx  = hw->intrrx;
  intrusb = hw->intrusb;

  /* Handle USB core interrupts */

  if (intrusb & USB_INTR_RESET)
    {
      usbtrace(TRACE_INTENTRY(SF32LB52_TRACEINTID_RESET), 0);
      priv->paddrset = 0;
      priv->paddr    = 0;
      hw->faddr      = 0;

      /* Re-enable EP0 and USB core interrupts after reset */

      hw->intrtxe  = USBC_EP0_INTR_MASK;
      hw->intrrxe  = 0;
      hw->intrusbe = USB_INTR_RESET | USB_INTR_RESUME |
                     USB_INTR_SUSPEND;

      /* Reset endpoint state */

      for (int i = 0; i < SF32LB52_NENDPOINTS; i++)
        {
          sf32lb52_cancelrequests(&priv->eplist[i]);
          sf32lb52_epreset(priv, i);
        }

      /* Notify class driver of reset */

      if (priv->driver)
        {
          CLASS_DISCONNECT(priv->driver, &priv->usbdev);
        }
    }

  if (intrusb & USB_INTR_RESUME)
    {
      usbtrace(TRACE_INTENTRY(SF32LB52_TRACEINTID_RESUME), 0);
      if (priv->driver)
        {
          CLASS_RESUME(priv->driver, &priv->usbdev);
        }
    }

  if (intrusb & USB_INTR_SUSPEND)
    {
      usbtrace(TRACE_INTENTRY(SF32LB52_TRACEINTID_SUSPEND), 0);
      if (priv->driver)
        {
          CLASS_SUSPEND(priv->driver, &priv->usbdev);
        }
    }

  if (intrusb & USB_INTR_SOF)
    {
      usbtrace(TRACE_INTENTRY(SF32LB52_TRACEINTID_SOF), 0);
    }

  if (intrusb & USB_INTR_DISCONNECT)
    {
      usbtrace(TRACE_INTENTRY(SF32LB52_TRACEINTID_DISCONNECT), 0);
      if (priv->driver)
        {
          CLASS_DISCONNECT(priv->driver, &priv->usbdev);
        }
    }

  /* Handle EP0 (control) interrupt */

  if (intrtx & USBC_EP0_INTR_MASK)
    {
      hw->index = 0;
      csr0 = hw->csr0_txcsr;

      usbtrace(TRACE_INTENTRY(SF32LB52_TRACEINTID_CONTROL), csr0);

      if (csr0 & USB_CSR0_P_SENTSTALL)
        {
          usbtrace(TRACE_INTENTRY(SF32LB52_TRACEINTID_TXFIFOSTALL), csr0);

          /* Clear stall status */

          hw->csr0_txcsr = csr0 & ~USB_CSR0_P_SENTSTALL;
        }
      else if (csr0 & USB_CSR0_P_SETUPEND)
        {
          usbtrace(TRACE_INTENTRY(SF32LB52_TRACEINTID_TXFIFOSETEND),
                   csr0);

          /* Clear SetupEnd */

          hw->csr0_txcsr = USB_CSR0_P_SVDSETUPEND;
        }
      else if (csr0 & USB_CSR0_RXPKTRDY)
        {
          usbtrace(TRACE_INTENTRY(SF32LB52_TRACEINTID_RXPKTRDY), csr0);
          sf32lb52_ep0setup(priv);
        }
      else if ((csr0 & USB_CSR0_TXPKTRDY) == 0)
        {
          /* TXPKTRDY is clear -- either a previous IN transfer completed
           * or we're idle after status phase.
           */

          /* Set address after status stage completes */

          if (!priv->paddrset && priv->paddr != 0)
            {
              hw->faddr = priv->paddr;
              priv->paddrset = 1;
            }

          /* Continue any pending EP0 IN transfer */

          privep = &priv->eplist[SF32LB52_EP0];
          if (!sf32lb52_rqempty(privep))
            {
              usbtrace(TRACE_INTENTRY(SF32LB52_TRACEINTID_TXPKTRDY),
                       csr0);
              sf32lb52_wrrequest(privep);
            }
        }
    }

  /* Handle TX (IN) endpoint interrupts (EP1, EP3) */

  if (intrtx & ~USBC_EP0_INTR_MASK)
    {
      int i;
      for (i = 1; i < SF32LB52_NENDPOINTS; i++)
        {
          if (intrtx & (1 << i))
            {
              usbtrace(TRACE_INTENTRY(SF32LB52_TRACEINTID_TXFIFO), i);

              if (priv->usbdev.speed == USB_SPEED_UNKNOWN)
                {
                  priv->usbdev.speed = USB_SPEED_FULL;
                }

              privep = &priv->eplist[i];

              /* Check for stall */

              hw->index = i;
              if (hw->csr0_txcsr & USB_TXCSR_P_SENTSTALL)
                {
                  hw->csr0_txcsr = hw->csr0_txcsr & ~USB_TXCSR_P_SENTSTALL;
                }

              if (!sf32lb52_rqempty(privep))
                {
                  sf32lb52_wrrequest(privep);
                }
            }
        }
    }

  /* Handle RX (OUT) endpoint interrupts (EP2) */

  if (intrrx)
    {
      int i;
      for (i = 1; i < SF32LB52_NENDPOINTS; i++)
        {
          if (intrrx & (1 << i))
            {
              usbtrace(TRACE_INTENTRY(SF32LB52_TRACEINTID_RXFIFO), i);

              privep = &priv->eplist[i];
              if (!sf32lb52_rqempty(privep))
                {
                  sf32lb52_rdrequest(privep);
                }
              else
                {
                  uinfo("Pending data on OUT endpoint %d\n", i);
                  priv->rxpending = 1;
                }
            }
        }
    }

  /* Restore index register */

  hw->index = old_index;

  return OK;
}

/****************************************************************************
 * Name: sf32lb52_epreset
 ****************************************************************************/

static void sf32lb52_epreset(struct sf32lb52_usbdev_s *priv,
                             unsigned int index)
{
  volatile USBC_X_Typedef *hw = priv->hw;

  hw->index = index;
  if (index == 0)
    {
      hw->csr0_txcsr = USB_CSR0_FLUSHFIFO;
    }
  else
    {
      /* Flush TX FIFO */

      hw->csr0_txcsr = USB_TXCSR_FLUSHFIFO | USB_TXCSR_CLRDATATOG;

      /* Flush RX FIFO */

      hw->rxcsr = USB_RXCSR_FLUSHFIFO | USB_RXCSR_CLRDATATOG;
    }
}

/****************************************************************************
 * Name: sf32lb52_epinitialize
 *
 * Description:
 *   Initialize endpoints. Configure FIFO sizes and addresses.
 *
 ****************************************************************************/

static void sf32lb52_epinitialize(struct sf32lb52_usbdev_s *priv)
{
  volatile USBC_X_Typedef *hw = priv->hw;
  uint16_t offset;
  int i;

  /* Initialize EP0 */

  hw->index = 0;
  hw->csr0_txcsr = USB_CSR0_FLUSHFIFO;

  /* EP0 has a fixed 64-byte FIFO at offset 0 */

  /* Setup non-control endpoints.
   * FIFO address is in units of 8 bytes.
   */

  offset = SF32LB52_EP0MAXPACKET / 8;  /* Start after EP0 FIFO */

  for (i = 1; i < SF32LB52_NENDPOINTS; i++)
    {
      hw->index = g_epinfo[i].addr & 0x0f;

      if (USB_EPIN(g_epinfo[i].addr))
        {
          /* Configure TX (IN) endpoint */

          hw->csr0_txcsr = USB_TXCSR_FLUSHFIFO | USB_TXCSR_CLRDATATOG;
          hw->csr0_txcsr = USB_TXCSR_MODE;  /* TX mode */

          /* TX FIFO size and address */

          hw->txfifosz  = USB_FIFOSZ_64;
          hw->txfifoadd = offset;

          /* TX max packet size */

          hw->txmaxp = g_epinfo[i].maxpacket;
        }
      else
        {
          /* Configure RX (OUT) endpoint */

          hw->rxcsr = USB_RXCSR_FLUSHFIFO | USB_RXCSR_CLRDATATOG;

          /* RX FIFO size and address */

          hw->rxfifosz  = USB_FIFOSZ_64;
          hw->rxfifoadd = offset;

          /* RX max packet size */

          hw->rxmaxp = g_epinfo[i].maxpacket;
        }

      offset += g_epinfo[i].maxpacket / 8;
    }

  /* Select EP0 as default */

  hw->index = 0;
}

/****************************************************************************
 * Name: sf32lb52_ctrlinitialize
 *
 * Description:
 *   Initialize the USB controller for peripheral mode operation.
 *
 ****************************************************************************/

static void sf32lb52_ctrlinitialize(struct sf32lb52_usbdev_s *priv)
{
  volatile USBC_X_Typedef *hw = priv->hw;

  /* Enable USB clock */

  HAL_RCC_EnableModule(RCC_MOD_USBC);

  /* Enable USB PHY for SF32LB52X.
   * Note: Do NOT set DP_EN here. DP_EN enables the D+ pullup which
   * makes the device visible on the bus. We defer that to pullup()
   * so the device only appears after a class driver is bound.
   */

  hwp_hpsys_cfg->USBCR |= HPSYS_CFG_USBCR_DM_PD |
                           HPSYS_CFG_USBCR_USB_EN;

  /* Enable AVALID signals */

  hw->usbcfg |= (USB_USBCFG_AVALID | USB_USBCFG_AVALID_DR);

  /* Disable double-packet buffering for all endpoints */

  hw->dpbrxdisl = 0xfe;
  hw->dpbtxdisl = 0xfe;

  /* Reset USB controller registers */

  hw->faddr = 0x00;
  hw->power = 0;

  /* Ensure High-Speed is disabled (Full-Speed only) */

  hw->power &= ~USB_POWER_HSENAB;

  /* Start USB session (required for B-device operation) */

  hw->devctl |= USB_DEVCTL_SESSION;

  /* Read to clear pending interrupts */

  (void)hw->intrtx;
  (void)hw->intrrx;
  (void)hw->intrusb;

  /* Enable USB interrupts:
   * - Reset, Resume, Suspend
   */

  hw->intrusbe = USB_INTR_RESET | USB_INTR_RESUME |
                 USB_INTR_SUSPEND;

  /* Enable EP0 TX interrupt */

  hw->intrtxe = USBC_EP0_INTR_MASK;

  /* Enable RX interrupts for OUT endpoints */

  hw->intrrxe = 0;

  /* Initialize endpoints */

  sf32lb52_epinitialize(priv);

  /* Set peripheral address to 0 */

  priv->paddr = 0;
  hw->faddr = 0;

  /* Select EP0 */

  hw->index = 0;
}

/****************************************************************************
 * Endpoint Methods
 ****************************************************************************/

/****************************************************************************
 * Name: sf32lb52_epconfigure
 ****************************************************************************/

static int sf32lb52_epconfigure(struct usbdev_ep_s *ep,
                                const struct usb_epdesc_s *desc,
                                bool last)
{
  struct sf32lb52_ep_s *privep = (struct sf32lb52_ep_s *)ep;
  struct sf32lb52_usbdev_s *priv = privep->dev;
  volatile USBC_X_Typedef *hw = priv->hw;
  uint16_t maxpacket;
  uint8_t epno;
  bool isin;

  usbtrace(TRACE_EPCONFIGURE, privep->epphy);

  epno = USB_EPNO(desc->addr);
  isin = USB_ISEPIN(desc->addr);
  maxpacket = GETUINT16(desc->mxpacketsize);

  /* Update endpoint info */

  ep->eplog     = desc->addr;
  ep->maxpacket = maxpacket;
  privep->in    = isin;

  /* Configure the hardware endpoint */

  hw->index = epno;

  if (isin)
    {
      /* Configure TX endpoint */

      hw->txmaxp = maxpacket;
      hw->csr0_txcsr = USB_TXCSR_CLRDATATOG | USB_TXCSR_FLUSHFIFO;
      hw->csr0_txcsr = USB_TXCSR_MODE;  /* TX mode */

      /* Enable TX interrupt for this endpoint */

      hw->intrtxe |= (1 << epno);
    }
  else
    {
      /* Configure RX endpoint */

      hw->rxmaxp = maxpacket;
      hw->rxcsr = USB_RXCSR_CLRDATATOG | USB_RXCSR_FLUSHFIFO;

      /* Enable RX interrupt for this endpoint */

      hw->intrrxe |= (1 << epno);
    }

  return OK;
}

/****************************************************************************
 * Name: sf32lb52_epdisable
 ****************************************************************************/

static int sf32lb52_epdisable(struct usbdev_ep_s *ep)
{
  struct sf32lb52_ep_s *privep = (struct sf32lb52_ep_s *)ep;
  irqstate_t flags;

#ifdef CONFIG_DEBUG_FEATURES
  if (!ep)
    {
      usbtrace(TRACE_DEVERROR(SF32LB52_TRACEERR_INVALIDPARMS), 0);
      return -EINVAL;
    }
#endif

  usbtrace(TRACE_EPDISABLE, privep->epphy);

  flags = enter_critical_section();
  sf32lb52_cancelrequests(privep);
  sf32lb52_epreset(privep->dev, privep->epphy);
  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Name: sf32lb52_epallocreq
 ****************************************************************************/

static struct usbdev_req_s *sf32lb52_epallocreq(struct usbdev_ep_s *ep)
{
  struct sf32lb52_req_s *privreq;

#ifdef CONFIG_DEBUG_FEATURES
  if (!ep)
    {
      return NULL;
    }
#endif

  usbtrace(TRACE_EPALLOCREQ, ((struct sf32lb52_ep_s *)ep)->epphy);

  privreq = kmm_zalloc(sizeof(struct sf32lb52_req_s));
  if (!privreq)
    {
      usbtrace(TRACE_DEVERROR(SF32LB52_TRACEERR_ALLOCFAIL), 0);
      return NULL;
    }

  return &privreq->req;
}

/****************************************************************************
 * Name: sf32lb52_epfreereq
 ****************************************************************************/

static void sf32lb52_epfreereq(struct usbdev_ep_s *ep,
                               struct usbdev_req_s *req)
{
  struct sf32lb52_req_s *privreq = (struct sf32lb52_req_s *)req;

#ifdef CONFIG_DEBUG_FEATURES
  if (!ep || !req)
    {
      usbtrace(TRACE_DEVERROR(SF32LB52_TRACEERR_INVALIDPARMS), 0);
      return;
    }
#endif

  usbtrace(TRACE_EPFREEREQ, ((struct sf32lb52_ep_s *)ep)->epphy);
  kmm_free(privreq);
}

/****************************************************************************
 * Name: sf32lb52_epsubmit
 ****************************************************************************/

static int sf32lb52_epsubmit(struct usbdev_ep_s *ep,
                             struct usbdev_req_s *req)
{
  struct sf32lb52_req_s *privreq = (struct sf32lb52_req_s *)req;
  struct sf32lb52_ep_s *privep = (struct sf32lb52_ep_s *)ep;
  struct sf32lb52_usbdev_s *priv;
  irqstate_t flags;
  int ret = OK;

#ifdef CONFIG_DEBUG_FEATURES
  if (!req || !req->callback || !req->buf || !ep)
    {
      usbtrace(TRACE_DEVERROR(SF32LB52_TRACEERR_INVALIDPARMS), 0);
      return -EINVAL;
    }
#endif

  usbtrace(TRACE_EPSUBMIT, privep->epphy);
  priv = privep->dev;

  if (!priv->driver || priv->usbdev.speed == USB_SPEED_UNKNOWN)
    {
      usbtrace(TRACE_DEVERROR(SF32LB52_TRACEERR_NOTCONFIGURED), 0);
      return -ESHUTDOWN;
    }

  req->result = -EINPROGRESS;
  req->xfrd   = 0;

  /* Queue work under critical section; run wr/rd/complete outside so class
   * callbacks (usbdev_fs_wrcomplete) can use spin_lock_irqsave_nopreempt
   * without nesting against this critical section (MTP bulk assert). */

  flags = enter_critical_section();

  if (privep->stalled)
    {
      leave_critical_section(flags);
      sf32lb52_abortrequest(privep, privreq, -EBUSY);
      return -EBUSY;
    }

  if (req->len == 0 && privep->epphy == 0)
    {
      /* EP0 zero-length status stage */

      usbtrace(TRACE_COMPLETE(privep->epphy), privreq->req.xfrd);
      priv->hw->index = 0;
      priv->hw->csr0_txcsr = USB_CSR0_TXPKTRDY | USB_CSR0_P_DATAEND;
      leave_critical_section(flags);
      sf32lb52_abortrequest(privep, privreq, OK);
      return OK;
    }

  if (privep->in || privep->epphy == SF32LB52_EPINTRIN)
    {
      /* IN request */

      sf32lb52_rqenqueue(privep, privreq);
      usbtrace(TRACE_INREQQUEUED(privep->epphy), privreq->req.len);
      leave_critical_section(flags);
      return sf32lb52_wrrequest(privep);
    }

  /* OUT request */

  privep->txnullpkt = 0;
  sf32lb52_rqenqueue(privep, privreq);
  usbtrace(TRACE_OUTREQQUEUED(privep->epphy), privreq->req.len);

  if (priv->rxpending)
    {
      priv->rxpending = 0;
      leave_critical_section(flags);
      return sf32lb52_rdrequest(privep);
    }

  leave_critical_section(flags);
  return ret;
}

/****************************************************************************
 * Name: sf32lb52_epcancel
 ****************************************************************************/

static int sf32lb52_epcancel(struct usbdev_ep_s *ep,
                             struct usbdev_req_s *req)
{
  struct sf32lb52_ep_s *privep = (struct sf32lb52_ep_s *)ep;
  irqstate_t flags;

#ifdef CONFIG_DEBUG_FEATURES
  if (!ep || !req)
    {
      usbtrace(TRACE_DEVERROR(SF32LB52_TRACEERR_INVALIDPARMS), 0);
      return -EINVAL;
    }
#endif

  usbtrace(TRACE_EPCANCEL, privep->epphy);

  flags = enter_critical_section();
  sf32lb52_cancelrequests(privep);
  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Name: sf32lb52_epstall
 ****************************************************************************/

static int sf32lb52_epstall(struct usbdev_ep_s *ep, bool resume)
{
  struct sf32lb52_ep_s *privep = (struct sf32lb52_ep_s *)ep;
  struct sf32lb52_usbdev_s *priv = privep->dev;
  volatile USBC_X_Typedef *hw = priv->hw;
  irqstate_t flags;

  flags = enter_critical_section();

  hw->index = privep->epphy;

  if (resume)
    {
      /* Resume (clear stall) */

      privep->stalled = 0;
      privep->halted  = 0;

      if (privep->epphy == 0)
        {
          /* EP0: clear SendStall */

          hw->csr0_txcsr = hw->csr0_txcsr & ~USB_CSR0_P_SENDSTALL;
        }
      else if (privep->in)
        {
          hw->csr0_txcsr = (hw->csr0_txcsr & ~USB_TXCSR_P_WZC_BITS)
                           & ~USB_TXCSR_P_SENDSTALL;
          hw->csr0_txcsr |= USB_TXCSR_CLRDATATOG;
        }
      else
        {
          hw->rxcsr = (hw->rxcsr & ~USB_RXCSR_P_WZC_BITS)
                      & ~USB_RXCSR_P_SENDSTALL;
          hw->rxcsr |= USB_RXCSR_CLRDATATOG;
        }
    }
  else
    {
      /* Stall the endpoint */

      privep->stalled = 1;

      if (privep->epphy == 0)
        {
          hw->csr0_txcsr = hw->csr0_txcsr | USB_CSR0_P_SENDSTALL;
        }
      else if (privep->in)
        {
          hw->csr0_txcsr = (hw->csr0_txcsr & ~USB_TXCSR_P_WZC_BITS)
                           | USB_TXCSR_P_SENDSTALL;
        }
      else
        {
          hw->rxcsr = (hw->rxcsr & ~USB_RXCSR_P_WZC_BITS)
                      | USB_RXCSR_P_SENDSTALL;
        }
    }

  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Device Methods
 ****************************************************************************/

/****************************************************************************
 * Name: sf32lb52_allocep
 ****************************************************************************/

static struct usbdev_ep_s *sf32lb52_allocep(struct usbdev_s *dev,
                                            uint8_t eplog,
                                            bool in, uint8_t eptype)
{
  struct sf32lb52_usbdev_s *priv = (struct sf32lb52_usbdev_s *)dev;
  int ndx;

  usbtrace(TRACE_DEVALLOCEP, 0);

  eplog = USB_EPNO(eplog);

  for (ndx = 1; ndx < SF32LB52_NENDPOINTS; ndx++)
    {
      if (eplog != 0 && eplog != USB_EPNO(priv->eplist[ndx].ep.eplog))
        {
          continue;
        }

      if (in)
        {
          if (!USB_EPIN(g_epinfo[ndx].addr))
            {
              continue;
            }
        }
      else
        {
          if (!USB_EPOUT(g_epinfo[ndx].addr))
            {
              continue;
            }
        }

      if (g_epinfo[ndx].attr == eptype)
        {
          return &priv->eplist[ndx].ep;
        }
    }

  usbtrace(TRACE_DEVERROR(SF32LB52_TRACEERR_NOEP), 0);
  return NULL;
}

/****************************************************************************
 * Name: sf32lb52_freeep
 ****************************************************************************/

static void sf32lb52_freeep(struct usbdev_s *dev,
                            struct usbdev_ep_s *ep)
{
  struct sf32lb52_ep_s *privep = (struct sf32lb52_ep_s *)ep;
  usbtrace(TRACE_DEVFREEEP, (uint16_t)privep->epphy);
}

/****************************************************************************
 * Name: sf32lb52_getframe
 ****************************************************************************/

static int sf32lb52_getframe(struct usbdev_s *dev)
{
  struct sf32lb52_usbdev_s *priv = (struct sf32lb52_usbdev_s *)dev;
  irqstate_t flags;
  int ret;

  usbtrace(TRACE_DEVGETFRAME, 0);

  flags = enter_critical_section();
  ret = priv->hw->frame;
  leave_critical_section(flags);
  return ret;
}

/****************************************************************************
 * Name: sf32lb52_wakeup
 ****************************************************************************/

static int sf32lb52_wakeup(struct usbdev_s *dev)
{
  struct sf32lb52_usbdev_s *priv = (struct sf32lb52_usbdev_s *)dev;
  irqstate_t flags;

  usbtrace(TRACE_DEVWAKEUP, 0);

  flags = enter_critical_section();
  priv->hw->power |= USB_POWER_RESUME;
  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Name: sf32lb52_selfpowered
 ****************************************************************************/

static int sf32lb52_selfpowered(struct usbdev_s *dev, bool selfpowered)
{
  struct sf32lb52_usbdev_s *priv = (struct sf32lb52_usbdev_s *)dev;

  usbtrace(TRACE_DEVSELFPOWERED, (uint16_t)selfpowered);

  priv->selfpowered = selfpowered;
  return OK;
}

/****************************************************************************
 * Name: sf32lb52_pullup
 *
 * Description:
 *   Software-controlled connect to/disconnect from USB host via SOFTCONN
 *
 ****************************************************************************/

static int sf32lb52_pullup(struct usbdev_s *dev, bool enable)
{
  struct sf32lb52_usbdev_s *priv = (struct sf32lb52_usbdev_s *)dev;
  irqstate_t flags;

  usbtrace(TRACE_DEVPULLUP, (uint16_t)enable);

  flags = enter_critical_section();
  if (enable)
    {
      /* Enable PHY D+ pullup so the host detects the device */

      hwp_hpsys_cfg->USBCR |= HPSYS_CFG_USBCR_DP_EN;
      priv->hw->power |= USB_POWER_SOFTCONN;
    }
  else
    {
      priv->hw->power &= ~USB_POWER_SOFTCONN;
      hwp_hpsys_cfg->USBCR &= ~HPSYS_CFG_USBCR_DP_EN;
    }

  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: arm_usbinitialize
 *
 * Description:
 *   Initialize USB hardware. Called early during board initialization.
 *
 ****************************************************************************/

void arm_usbinitialize(void)
{
  struct sf32lb52_usbdev_s *priv = &g_usbdev;
  struct sf32lb52_ep_s *privep;
  int i;

  usbtrace(TRACE_DEVINIT, 0);

  /* Initialize the device state structure */

  memset(priv, 0, sizeof(struct sf32lb52_usbdev_s));
  priv->usbdev.ops = &g_devops;
  priv->hw = hwp_usbc;

  /* Initialize the USB controller hardware */

  sf32lb52_ctrlinitialize(priv);

  /* Attach USB controller core interrupt handler */

  if (irq_attach(SF32LB52_IRQ_USB, sf32lb52_interrupt, NULL) != 0)
    {
      usbtrace(TRACE_DEVERROR(SF32LB52_TRACEERR_COREIRQREG), 0);
      goto errout;
    }

  /* Initialize endpoint data structures */

  for (i = 0; i < SF32LB52_NENDPOINTS; i++)
    {
      privep           = &priv->eplist[i];
      memset(privep, 0, sizeof(struct sf32lb52_ep_s));
      privep->ep.ops   = &g_epops;
      privep->dev      = priv;
      privep->epphy    = i;
      privep->ep.eplog = g_epinfo[i].addr;
      privep->ep.maxpacket = g_epinfo[i].maxpacket;

      if (USB_EPIN(g_epinfo[i].addr))
        {
          privep->in = 1;
        }

      sf32lb52_epreset(priv, privep->epphy);
    }

  /* Expose only EP0 */

  priv->usbdev.ep0 = &priv->eplist[0].ep;

  /* Enable NVIC interrupt */

  up_enable_irq(SF32LB52_IRQ_USB);

  return;

errout:
  arm_usbuninitialize();
}

/****************************************************************************
 * Name: arm_usbuninitialize
 ****************************************************************************/

void arm_usbuninitialize(void)
{
  struct sf32lb52_usbdev_s *priv = &g_usbdev;

  usbtrace(TRACE_DEVUNINIT, 0);

  if (priv->driver)
    {
      usbtrace(TRACE_DEVERROR(SF32LB52_TRACEERR_DRIVERREGISTERED), 0);
      usbdev_unregister(priv->driver);
    }

  priv->usbdev.speed = USB_SPEED_UNKNOWN;

  /* Disconnect from bus */

  priv->hw->power &= ~USB_POWER_SOFTCONN;

  /* Disable and detach IRQ */

  up_disable_irq(SF32LB52_IRQ_USB);
  irq_detach(SF32LB52_IRQ_USB);

  /* Disable USB PHY */

  hwp_hpsys_cfg->USBCR &= ~(HPSYS_CFG_USBCR_DM_PD |
                             HPSYS_CFG_USBCR_DP_EN |
                             HPSYS_CFG_USBCR_USB_EN);

  /* Reset USB controller */

  hwp_hpsys_rcc->RSTR2 |= HPSYS_RCC_RSTR2_USBC;
  hwp_hpsys_rcc->RSTR2 &= ~HPSYS_RCC_RSTR2_USBC;

  /* Disable USB clock */

  HAL_RCC_DisableModule(RCC_MOD_USBC);
}

/****************************************************************************
 * Name: usbdev_register
 *
 * Description:
 *   Register a USB device class driver.
 *
 ****************************************************************************/

int usbdev_register(struct usbdevclass_driver_s *driver)
{
  int ret;

  usbtrace(TRACE_DEVREGISTER, 0);

#ifdef CONFIG_DEBUG_FEATURES
  if (!driver || !driver->ops->bind ||
      !driver->ops->unbind || !driver->ops->setup)
    {
      usbtrace(TRACE_DEVERROR(SF32LB52_TRACEERR_INVALIDPARMS), 0);
      return -EINVAL;
    }

  if (g_usbdev.driver)
    {
      usbtrace(TRACE_DEVERROR(SF32LB52_TRACEERR_DRIVER), 0);
      return -EBUSY;
    }
#endif

  /* Hook up the driver */

  g_usbdev.driver = driver;

  /* Bind the class driver */

  ret = CLASS_BIND(driver, &g_usbdev.usbdev);
  if (ret)
    {
      usbtrace(TRACE_DEVERROR(SF32LB52_TRACEERR_BINDFAILED),
               (uint16_t)-ret);
      g_usbdev.driver = NULL;
      return ret;
    }

  /* Reset and enable EP0 interrupt */

  sf32lb52_epreset(&g_usbdev, 0);
  g_usbdev.hw->intrtxe |= USBC_EP0_INTR_MASK;

  /* Enable USB core interrupts */

  g_usbdev.hw->intrusbe = USB_INTR_RESET | USB_INTR_RESUME |
                           USB_INTR_SUSPEND;

  /* Enable SOFTCONN and PHY D+ pullup to signal device connection */

  hwp_hpsys_cfg->USBCR |= HPSYS_CFG_USBCR_DP_EN;
  g_usbdev.hw->power |= USB_POWER_SOFTCONN;

  /* Enable IRQ */

  up_enable_irq(SF32LB52_IRQ_USB);
  return OK;
}

/****************************************************************************
 * Name: usbdev_unregister
 *
 * Description:
 *   Un-register usbdev class driver.
 *
 ****************************************************************************/

int usbdev_unregister(struct usbdevclass_driver_s *driver)
{
  usbtrace(TRACE_DEVUNREGISTER, 0);

#ifdef CONFIG_DEBUG_FEATURES
  if (driver != g_usbdev.driver)
    {
      usbtrace(TRACE_DEVERROR(SF32LB52_TRACEERR_INVALIDPARMS), 0);
      return -EINVAL;
    }
#endif

  /* Disconnect from bus */

  g_usbdev.hw->power &= ~USB_POWER_SOFTCONN;

  /* Unbind the class driver */

  CLASS_UNBIND(driver, &g_usbdev.usbdev);

  /* Disable IRQ */

  up_disable_irq(SF32LB52_IRQ_USB);

  /* Unhook the driver */

  g_usbdev.driver = NULL;
  return OK;
}
