/*
 * QEMU avalanche support
 * Copyright (c) 2006 Stefan Weil
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/* This code emulates specific parts of Texas Instruments AR7 processor.
 * AR7 is a chip with a MIPS 4KEc core and on-chip peripherals (avalanche).
 *
 * TODO:
 * - reboot loops endless reading device config latch (AVALANCHE_DCL_BASE)
 * - uart0, uart1 wrong type (is 16450, should be 16550)
 * - vlynq emulation only very rudimentary
 * - ethernet not stable
 * - much more
 *
 * Interrupts:
 *                 CPU0
 *        2:         64            MIPS  AR7 on hw0
 *        7:       1686            MIPS  timer
 *       15:         64             AR7  serial
 *       16:          0             AR7  serial
 *       27:          0             AR7  Cpmac Driver
 *       41:          0             AR7  Cpmac Driver
 *
 *      ERR:          0
 * 
 */

#include <assert.h>
#include <stddef.h>             /* offsetof */

#include <zlib.h>               /* crc32 */
#include <netinet/in.h>         /* htonl */

#include "vl.h"
#include "disas.h"              /* lookup_symbol */
#include "exec-all.h"           /* logfile */
#include "hw/ar7.h"             /* ar7_init */

static int bigendian;

#define MAX_ETH_FRAME_SIZE 1514

#if 0
struct IoState {
    target_ulong base;
    int it_shift;
};
#endif

/* Set flags to >0 to enable debug output. */
#define CLOCK   0
#define CPMAC   1
#define EMIF    0
#define GPIO    0
#define INTC    0
#define MDIO    0               /* polled, so very noisy */
#define RESET   0
#define UART0   0
#define UART1   0
#define VLYNQ   0
#define WDOG    0
#define OTHER   0
#define RXTX    1

#define DEBUG_AR7

#define TRACE(flag, command) ((flag) ? (command) : (void)0)

#ifdef DEBUG_AR7
#define logout(fmt, args...) fprintf(stderr, "AR7\t%-24s" fmt, __func__, ##args)
#else
#define logout(fmt, args...) ((void)0)
#endif

#define MISSING() logout("%s:%u missing, %s!!!\n", __FILE__, __LINE__, backtrace())
#define UNEXPECTED() logout("%s:%u unexpected, %s!!!\n", __FILE__, __LINE__, backtrace())

#define BIT(n) (1 << (n))
#define BITS(n, m) (((0xffffffffU << (31 - n)) >> (31 - n + m)) << m)

#if 0
#define BBIF_SPACE1                           (KSEG1ADDR(0x01800000))
#define PHY_BASE                              (KSEG1ADDR(0x1E000000))
#endif

#define OHIO_ADSLSS_BASE0       KERNEL_ADDR(0x01000000)
#define OHIO_ADSLSS_BASE1       KERNEL_ADDR(0x01800000)
#define OHIO_ADSLSS_BASE2       KERNEL_ADDR(0x01C00000)
#define OHIO_ATMSAR_BASE        KERNEL_ADDR(0x03000000)
#define OHIO_USB_BASE           KERNEL_ADDR(0x03400000)
#define OHIO_VLYNQ0_BASE        KERNEL_ADDR(0x04000000)

/*
Physical memory map
0x00000000      RAM start
0x00000fff      RAM end
0x08610000      I/O start
0x08613000      I/O end
0x10000000      Flash start
0x101fffff      Flash end (2 MiB)
0x103fffff      Flash end (4 MiB)
0x107fffff      Flash end (8 MiB)
0x14000000      RAM start
0x14ffffff      RAM end (16 MiB)
0x15ffffff      RAM end (32 MiB)
0x1e000000      ???
0x1fc00000      internal ROM start
0x1fc00fff      internal ROM end
*/

#define AVALANCHE_ADSLSSYS_MEM_BASE     0x01000000      /* ADSL subsystem mem base */
#define AVALANCHE_BBIF_BASE             0x02000000      /* broadband interface */
#define AVALANCHE_ATM_SAR_BASE          0x03000000      /* ATM SAR */
#define AVALANCHE_USB_MEM_BASE          0x03400000      /* USB slave mem map */
#define AVALANCHE_VLYNQ0_MEM_MAP_BASE   0x04000000      /* VLYNQ 0 memory mapped */
#define AVALANCHE_VLYNQ1_MEM_MAP_BASE   0x0c000000      /* VLYNQ 1 memory mapped */
#define AVALANCHE_CPMAC0_BASE           0x08610000
#define AVALANCHE_EMIF_BASE             0x08610800
#define AVALANCHE_GPIO_BASE             0x08610900
#define AVALANCHE_CLOCK_BASE            0x08610a00      /* Clock Control */
#define AVALANCHE_POWER_CTRL_PDCR     (KSEG1ADDR(0x08610A00))
#define AVALANCHE_WAKEUP_CTRL_WKCR    (KSEG1ADDR(0x08610A0C))
#define AVALANCHE_WATCHDOG_BASE         0x08610b00      /* Watchdog */
#define AVALANCHE_TIMER0_BASE           0x08610c00      /* Timer 1 */
#define AVALANCHE_TIMER1_BASE           0x08610d00      /* Timer 2 */
#define AVALANCHE_UART0_BASE            0x08610e00      /* UART 0 */
#define AVALANCHE_UART1_BASE            0x08610f00      /* UART 1 */
#define OHIO_I2C_BASE                   0x08610f00
#define AVALANCHE_I2C_BASE              0x08611000      /* I2C */
#define DEV_ID_BASE                     0x08611100
#define AVALANCHE_USB_SLAVE_BASE        0x08611200      /* USB DMA */
#define PCI_CONFIG_BASE                 0x08611300
#define AVALANCHE_MCDMA_BASE            0x08611400      /* MC DMA channels 0-3 */
#define TNETD73xx_VDMAVT_BASE           0x08611500      /* VDMAVT Control */
#define AVALANCHE_RESET_BASE            0x08611600
#define AVALANCHE_BIST_CONTROL_BASE     0x08611700      /* BIST Control */
#define AVALANCHE_VLYNQ0_BASE           0x08611800      /* VLYNQ0 port controller */
#define AVALANCHE_DCL_BASE              0x08611a00      /* Device Config Latch */
#define OHIO_MII_SEL_REG                0x08611a08
#define DSL_IF_BASE                     0x08611b00
#define AVALANCHE_VLYNQ1_BASE           0x08611c00      /* VLYNQ1 port controller */
#define AVALANCHE_MDIO_BASE             0x08611e00
#define OHIO_WDT_BASE                   0x08611f00
#define AVALANCHE_FSER_BASE             0x08612000      /* FSER base */
#define AVALANCHE_INTC_BASE             0x08612400
#define AVALANCHE_CPMAC1_BASE           0x08612800
#define AVALANCHE_END                   0x08613000

//~ typedef struct {
    //~ struct BUFF_DESC  *next;
    //~ char              *buff;
    //~ uint32_t           buff_params;
    //~ uint32_t           ctrl_n_len;
//~ } cpmac_buff_t;

typedef struct {
    uint32_t next;
    uint32_t buff;
    uint32_t length;
    uint32_t mode;
} cpphy_rcb_t;

/* Rcb/Tcb Constants */

#define CB_SOF_BIT         BIT(31)
#define CB_EOF_BIT         BIT(30)
#define CB_SOF_AND_EOF_BIT (CB_SOF_BIT|CB_EOF_BIT)
#define CB_OWNERSHIP_BIT   BIT(29)
#define CB_EOQ_BIT         BIT(28)
#define CB_SIZE_MASK       0x0000ffff
#define RCB_ERRORS_MASK    0x03fe0000

typedef struct {
    uint32_t next;
    uint32_t buff;
    uint32_t length;
    uint32_t mode;
} cpphy_tcb_t;

typedef struct {
    //~ uint8_t cmd;
    //~ uint32_t start;
    //~ uint32_t stop;
    //~ uint8_t boundary;
    //~ uint8_t tsr;
    //~ uint8_t tpsr;
    //~ uint16_t tcnt;
    //~ uint16_t rcnt;
    //~ uint32_t rsar;
    //~ uint8_t rsr;
    //~ uint8_t rxcr;
    //~ uint8_t isr;
    //~ uint8_t dcfg;
    //~ uint8_t imr;
    uint8_t phys[6];            /* mac address */
    //~ uint8_t curpag;
    //~ uint8_t mult[8]; /* multicast mask array */
    //~ int irq;
    VLANClientState *vc;
    //~ uint8_t macaddr[6];
    //~ uint8_t mem[1];
} NICState;

typedef struct {
    CPUState *cpu_env;
    NICState nic[2];
    uint32_t intmask[2];

    uint32_t adsl[0x8000];      // 0x01000000
    uint32_t bbif[1];           // 0x02000000
    uint32_t atmsar[0x2400];    // 0x03000000
    uint32_t usbslave[0x800];   // 0x03400000
    uint32_t vlynq0mem[0x10800];        // 0x04000000

    uint8_t cpmac0[0x800];      // 0x08610000
    uint32_t emif[0x40];        // 0x08610800
    uint32_t gpio[8];           // 0x08610900
    // data in, data out, dir, enable, -, cvr, didr1, didr2
    uint32_t gpio_dummy[0x38];
    uint32_t clock_control[0x40];       // 0x08610a00
    // 0x08610a80 struct _ohio_clock_pll
    uint32_t clock_dummy[0x18];
    uint32_t watchdog[0x20];    // 0x08610b00 struct _ohio_clock_pll
    uint32_t timer0[2];         // 0x08610c00
    uint32_t timer1[2];         // 0x08610d00
    uint32_t uart0[8];          // 0x08610e00
    uint32_t uart1[8];          // 0x08610f00
    uint32_t usb[20];           // 0x08611200
    uint32_t mc_dma[0x10][4];   // 0x08611400
    uint32_t reset_control[3];  // 0x08611600
    uint32_t reset_dummy[0x80 - 3];
    uint8_t vlynq0[0x100];      // 0x08611800
    // + 0xe0 interrupt enable bits
    uint32_t device_config_latch[5];    // 0x08611a00
    uint8_t vlynq1[0x100];      // 0x08611c00
    uint32_t mdio[0x22];        // 0x08611e00
    uint32_t wdt[8];            // 0x08611f00
    uint32_t intc[0xc0];        // 0x08612400
    //~ uint32_t exception_control[7];  //   +0x80
    //~ uint32_t pacing[3];             //   +0xa0
    //~ uint32_t channel_control[40];   //   +0x200
    uint8_t cpmac1[0x800];      // 0x08612800
    //~ uint32_t unknown[0x40]              // 0x08613000
} avalanche_t;

#define UART_MEM_TO_IO(addr)    (((addr) - AVALANCHE_UART0_BASE) / 4)

static avalanche_t av = {
  cpmac0:{0},
  emif:{0},
  gpio:{0x800, 0, 0, 0},
  clock_control:{0},
  timer0:{0},
  timer1:{0},
  uart0:{0, 0, 0, 0, 0, 0x20, 0},
    //~ reset_control: { 0x04720043 },
    //~ device_config_latch: 0x025d4297
    // 21-20 phy clk source
  device_config_latch:{0x025d4291},
  mdio:{0x00070101, 0, 0xffffffff}
};

/* Global variable avalanche can be used in debugger. */
avalanche_t *avalanche = &av;

static const char *backtrace(void)
{
    static char buffer[256];
    char *p = buffer;
    p += sprintf(p, "[%s]", lookup_symbol(av.cpu_env->PC));
    p += sprintf(p, "[%s]", lookup_symbol(av.cpu_env->gpr[31]));
    assert((p - buffer) < sizeof(buffer));
    return buffer;
}

static const char *dump(const uint8_t * buf, unsigned size)
{
    static char buffer[3 * 25 + 1];
    char *p = &buffer[0];
    if (size > 25)
        size = 25;
    while (size-- > 0) {
        p += sprintf(p, " %02x", *buf++);
    }
    return buffer;
}

/*****************************************************************************
 *
 * Helper functions.
 *
 ****************************************************************************/

static uint32_t reg_read(uint8_t * reg, uint32_t addr)
{
    if (addr & 3) {
        logout("0x%08x\n", addr);
        UNEXPECTED();
    }
    return le32_to_cpu(*(uint32_t *) (&reg[addr]));
}

static void reg_write(uint8_t * reg, uint32_t addr, uint32_t value)
{
    assert(!(addr & 3));
    *(uint32_t *) (&reg[addr]) = cpu_to_le32(value);
}

static void reg_inc(uint8_t * reg, uint32_t addr)
{
    assert(!(addr & 3));
    reg_write(reg, addr, reg_read(reg, addr) + 1);
}

static void reg_clear(uint8_t * reg, uint32_t addr, uint32_t value)
{
    assert(!(addr & 3));
    *(uint32_t *) (&reg[addr]) &= cpu_to_le32(~value);
}

static void reg_set(uint8_t * reg, uint32_t addr, uint32_t value)
{
    assert(!(addr & 3));
    *(uint32_t *) (&reg[addr]) |= cpu_to_le32(value);
}

/*****************************************************************************
 *
 * Interrupt emulation.
 *
 ****************************************************************************/

/* ar7_irq does not use the opaque parameter, so we set it to 0. */
#define IRQ_OPAQUE 0

static void ar7_irq(void *opaque, int irq_num, int level)
{
    CPUState *cpu_env = first_cpu;
    assert(cpu_env == av.cpu_env);

    switch (irq_num) {
    case 15:                   /* serial0 */
    case 16:                   /* serial1 */
    case 27:                   /* cpmac0 */
    case 41:                   /* cpmac1 */
        if (level) {
            unsigned channel = irq_num - 8;
            if (channel < 32) {
                if (av.intmask[0] & (1 << channel)) {
                    //~ logout("(%p,%d,%d)\n", opaque, irq_num, level);
                    av.intc[0x10] = (((irq_num - 8) << 16) | channel);
                    /* use hardware interrupt 0 */
                    cpu_env->CP0_Cause |= 0x00000400;
                    cpu_interrupt(cpu_env, CPU_INTERRUPT_HARD);
                } else {
                    //~ logout("(%p,%d,%d) is disabled\n", opaque, irq_num, level);
                }
            }
            // int line number
            //~ av.intc[0x10] |= (4 << 16);
            // int channel number
            // 2, 7, 15, 27, 80
            //~ av.intmask[0]
        } else {
            av.intc[0x10] = 0;
            cpu_env->CP0_Cause &= ~0x00000400;
            cpu_reset_interrupt(cpu_env, CPU_INTERRUPT_HARD);
        }
        break;
    default:
        logout("(%p,%d,%d)\n", opaque, irq_num, level);
    }
}

/*****************************************************************************
 *
 * CPMAC emulation.
 *
 ****************************************************************************/

#if 0

/*
08611600  43 00 72 04 00 00 00 00  00 00 00 00 00 00 00 00  |C.r.............|
08611610  43 00 72 04 00 00 00 00  00 00 00 00 00 00 00 00  |C.r.............|
08611a00  91 42 5d 02 00 00 00 00  00 00 00 00 00 00 00 00  |.B].............|
08611a10  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
08611b00  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
*/

cpmac_reg.h:
#define MACSTATUS(base)                ((MEM_PTR)(base+0x164))
#define EMCONTROL(base)                ((MEM_PTR)(base+0x168))
#define TX_INTSTAT_RAW(base)           ((MEM_PTR)(base+0x170))
#define TX_INTSTAT_MASKED(base)        ((MEM_PTR)(base+0x174))

#define RX_INTSTAT_RAW(base)           ((MEM_PTR)(base+0x190))
#define RX_INTSTAT_MASKED(base)        ((MEM_PTR)(base+0x194))

#define MAC_INTSTAT_RAW(base)          ((MEM_PTR)(base+0x1A0))
#define MAC_INTSTAT_MASKED(base)       ((MEM_PTR)(base+0x1A4))

#define MAC_INTMASK_CLEAR(base)        ((MEM_PTR)(base+0x1AC))

#define BOFFTEST(base)                 ((MEM_PTR)(base+0x1E0))
#define PACTEST(base)                  ((MEM_PTR)(base+0x1E4))
#define RXPAUSE(base)                  ((MEM_PTR)(base+0x1E8))
#define TXPAUSE(base)                  ((MEM_PTR)(base+0x1EC))

#define CPMAC_RX_INT_ACK(base,ch)             (*(MEM_PTR)(base+0x660+(4*ch)))
#define pCPMAC_RX0_INT_ACK(base)              ((MEM_PTR)(base+0x660))
...
#define pCPMAC_RX7_INT_ACK(base)              ((MEM_PTR)(base+0x67C))
#endif

typedef enum {
    CPMAC_TX_IDVER = 0x0000,
    CPMAC_TX_CONTROL = 0x0004,
    CPMAC_TX_TEARDOWN = 0x0008,
    CPMAC_RX_IDVER = 0x0010,
    CPMAC_RX_CONTROL = 0x0014,
    CPMAC_RX_TEARDOWN = 0x0018,
    CPMAC_RX_MBP_ENABLE = 0x0100,
    CPMAC_RX_UNICAST_SET = 0x0104,
    CPMAC_RX_UNICAST_CLEAR = 0x0108,
    CPMAC_RX_MAXLEN = 0x010c,
    CPMAC_RX_BUFFER_OFFSET = 0x0110,
    CPMAC_RX_FILTERLOWTHRESH = 0x0114,
    CPMAC_MACCONTROL = 0x0160,
    CPMAC_TX_INTSTAT_MASKED = 0x0174,
    CPMAC_TX_INTMASK_SET = 0x0178,
    CPMAC_TX_INTMASK_CLEAR = 0x017c,
    CPMAC_MAC_IN_VECTOR = 0x0180,
    CPMAC_MAC_EOI_VECTOR = 0x0184,
    CPMAC_RX_INTMASK_SET = 0x0198,
    CPMAC_RX_INTMASK_CLEAR = 0x019c,
    CPMAC_MAC_INTMASK_SET = 0x01a8,
    CPMAC_MACADDRLO_0 = 0x01b0,
    CPMAC_MACADDRLO_1 = 0x01b4,
    CPMAC_MACADDRLO_2 = 0x01b8,
    CPMAC_MACADDRLO_3 = 0x01bc,
    CPMAC_MACADDRLO_4 = 0x01c0,
    CPMAC_MACADDRLO_5 = 0x01c4,
    CPMAC_MACADDRLO_6 = 0x01c8,
    CPMAC_MACADDRLO_7 = 0x01cc,
    CPMAC_MACADDRMID = 0x01d0,
    CPMAC_MACADDRHI = 0x01d4,
    CPMAC_MACHASH1 = 0x01d8,
    CPMAC_MACHASH2 = 0x01dc,
    CPMAC_RXGOODFRAMES = 0x0200,
    CPMAC_RXBROADCASTFRAMES = 0x0204,
    CPMAC_RXMULTICASTFRAMES = 0x0208,
    CPMAC_RXDMAOVERRUNS = 0x028c,
    CPMAC_RXOVERSIZEDFRAMES = 0x0218,
    CPMAC_RXJABBERFRAMES = 0x021c,
    CPMAC_RXUNDERSIZEDFRAMES = 0x0220,
    CPMAC_TXGOODFRAMES = 0x234,
    CPMAC_TXBROADCASTFRAMES = 0x238,
    CPMAC_TXMULTICASTFRAMES = 0x23c,
    CPMAC_TX0_HDP = 0x0600,
    CPMAC_TX1_HDP = 0x0604,
    CPMAC_TX2_HDP = 0x0608,
    CPMAC_TX3_HDP = 0x060c,
    CPMAC_TX4_HDP = 0x0610,
    CPMAC_TX5_HDP = 0x0614,
    CPMAC_TX6_HDP = 0x0618,
    CPMAC_TX7_HDP = 0x061c,
    CPMAC_RX0_HDP = 0x0620,
    CPMAC_RX1_HDP = 0x0624,
    CPMAC_RX2_HDP = 0x0628,
    CPMAC_RX3_HDP = 0x062c,
    CPMAC_RX4_HDP = 0x0630,
    CPMAC_RX5_HDP = 0x0634,
    CPMAC_RX6_HDP = 0x0638,
    CPMAC_RX7_HDP = 0x063c,
    CPMAC_TX0_INT_ACK = 0x0640,
    CPMAC_TX1_INT_ACK = 0x0644,
    CPMAC_TX2_INT_ACK = 0x0648,
    CPMAC_TX3_INT_ACK = 0x064c,
    CPMAC_TX4_INT_ACK = 0x0650,
    CPMAC_TX5_INT_ACK = 0x0654,
    CPMAC_TX6_INT_ACK = 0x0658,
    CPMAC_TX7_INT_ACK = 0x065c,
    CPMAC_RX0_INT_ACK = 0x0660,
    CPMAC_RX1_INT_ACK = 0x0664,
    CPMAC_RX2_INT_ACK = 0x0668,
    CPMAC_RX3_INT_ACK = 0x066c,
    CPMAC_RX4_INT_ACK = 0x0670,
    CPMAC_RX5_INT_ACK = 0x0674,
    CPMAC_RX6_INT_ACK = 0x0678,
    CPMAC_RX7_INT_ACK = 0x067c,
} cpmac_register_t;

typedef enum {
    MAC_IN_VECTOR_STATUS_INT = BIT(19),
    MAC_IN_VECTOR_HOST_INT = BIT(18),
    MAC_IN_VECTOR_RX_INT_OR = BIT(17),
    MAC_IN_VECTOR_TX_INT_OR = BIT(16),
    MAC_IN_VECTOR_RX_INT_VEC = BITS(10, 8),
    MAC_IN_VECTOR_TX_INT_VEC = BITS(2, 0),
} mac_in_vec_bit_t;

/* STATISTICS */
static const char *const cpmac_statistics[] = {
    "RXGOODFRAMES",
    "RXBROADCASTFRAMES",
    "RXMULTICASTFRAMES",
    "RXPAUSEFRAMES",
    "RXCRCERRORS",
    "RXALIGNCODEERRORS",
    "RXOVERSIZEDFRAMES",
    "RXJABBERFRAMES",
    "RXUNDERSIZEDFRAMES",
    "RXFRAGMENTS",
    "RXFILTEREDFRAMES",
    "RXQOSFILTEREDFRAMES",
    "RXOCTETS",
    "TXGOODFRAMES",
    "TXBROADCASTFRAMES",
    "TXMULTICASTFRAMES",
    "TXPAUSEFRAMES",
    "TXDEFERREDFRAMES",
    "TXCOLLISIONFRAMES",
    "TXSINGLECOLLFRAMES",
    "TXMULTCOLLFRAMES",
    "TXEXCESSIVECOLLISIONS",
    "TXLATECOLLISIONS",
    "TXUNDERRUN",
    "TXCARRIERSENSEERRORS",
    "TXOCTETS",
    "64OCTETFRAMES",
    "65T127OCTETFRAMES",
    "128T255OCTETFRAMES",
    "256T511OCTETFRAMES",
    "512T1023OCTETFRAMES",
    "1024TUPOCTETFRAMES",
    "NETOCTETS",
    "RXSOFOVERRUNS",
    "RXMOFOVERRUNS",
    "RXDMAOVERRUNS"
};

static const char *i2cpmac(unsigned index)
{
    static char buffer[32];
    const char *text = 0;
    switch (index) {
    case 0x00:
        text = "TX_IDVER";
        break;
    case 0x01:
        text = "TX_CONTROL";
        break;
    case 0x02:
        text = "TX_TEARDOWN";
        break;
    case 0x04:
        text = "RX_IDVER";
        break;
    case 0x05:
        text = "RX_CONTROL";
        break;
    case 0x06:
        text = "RX_TEARDOWN";
        break;
    case 0x40:
        text = "RX_MBP_ENABLE";
        break;
    case 0x41:
        text = "RX_UNICAST_SET";
        break;
    case 0x42:
        text = "RX_UNICAST_CLEAR";
        break;
    case 0x43:
        text = "RX_MAXLEN";
        break;
    case 0x44:
        text = "RX_BUFFER_OFFSET";
        break;
    case 0x45:
        text = "RX_FILTERLOWTHRESH";
        break;
    case 0x58:
        text = "MACCONTROL";
        break;
    case 0x5c:
        text = "TX_INTSTAT_RAW";
        break;
    case 0x5d:
        text = "TX_INTSTAT_MASKED";
        break;
    case 0x5e:
        text = "TX_INTMASK_SET";
        break;
    case 0x5f:
        text = "TX_INTMASK_CLEAR";
        break;
    case 0x60:
        text = "MAC_IN_VECTOR";
        break;
    case 0x61:
        text = "MAC_EOI_VECTOR";
        break;
    case 0x66:
        text = "RX_INTMASK_SET";
        break;
    case 0x67:
        text = "RX_INTMASK_CLEAR";
        break;
    case 0x6a:
        text = "MAC_INTMASK_SET";
        break;
    case 0x74:
        text = "MACADDRMID";
        break;
    case 0x75:
        text = "MACADDRHI";
        break;
    case 0x76:
        text = "MACHASH1";
        break;
    case 0x77:
        text = "MACHASH2";
        break;
    }
    if (text != 0) {
    } else if (index >= 0x48 && index < 0x50) {
        text = buffer;
        sprintf(buffer, "RX%u_FLOWTHRESH", (unsigned)(index & 7));
    } else if (index >= 0x50 && index < 0x58) {
        text = buffer;
        sprintf(buffer, "RX%u_FREEBUFFER", (unsigned)(index & 7));
    } else if (index >= 0x6c && index < 0x74) {
        text = buffer;
        sprintf(buffer, "MACADDRLO_%u", (unsigned)(index - 0x6c));
    } else if (index >= 0x80 && index < 0xa4) {
        text = buffer;
        sprintf(buffer, "STAT_%s", cpmac_statistics[index - 0x80]);
    } else if (index >= 0x180 && index < 0x188) {
        text = buffer;
        sprintf(buffer, "TX%u_HDP", (unsigned)(index & 7));
    } else if (index >= 0x188 && index < 0x190) {
        text = buffer;
        sprintf(buffer, "RX%u_HDP", (unsigned)(index & 7));
    } else if (index >= 0x190 && index < 0x198) {
        text = buffer;
        sprintf(buffer, "TX%u_INT_ACK", (unsigned)(index & 7));
    } else if (index >= 0x198 && index < 0x1a0) {
        text = buffer;
        sprintf(buffer, "RX%u_INT_ACK", (unsigned)(index & 7));
    } else {
        text = buffer;
        sprintf(buffer, "0x%x", index);
    }
    assert(strlen(buffer) < sizeof(buffer));
    return text;
}

static const int cpmac_interrupt[] = { 27, 41 };

#define BD_SOP    MASK(31, 31)
#define BD_EOP    MASK(30, 30)
#define BD_OWNS   MASK(29, 29)

static uint32_t ar7_cpmac_read(uint8_t * cpmac, uint32_t offset)
{
    uint32_t val = reg_read(cpmac, offset);
    const char *text = i2cpmac(offset / 4);
    //~ do_raise_exception(EXCP_DEBUG)
    TRACE(CPMAC, logout("cpmac%u[%s] (0x%08lx) = 0x%08lx\n",
                        cpmac == av.cpmac1, text,
                        (unsigned long)(AVALANCHE_CPMAC0_BASE + offset),
                        (unsigned long)val));
    if (0) {
    } else if (offset == CPMAC_MAC_IN_VECTOR) {
        reg_write(cpmac, CPMAC_MAC_IN_VECTOR, 0);
    }
    return val;
}

/* Table of CRCs of all 8-bit messages. */
static uint32_t crc_table[256];

/* Flag: has the table been computed? Initially false. */
static int crc_table_computed = 0;

/* Make the table for a fast CRC. */
static void make_crc_table(void)
{
    int n, k;

    for (n = 0; n < 256; n++) {
        uint32_t c = (uint32_t) n;
        for (k = 0; k < 8; k++) {
            if (c & 1)
                c = 0xedb88320L ^ (c >> 1);
            else
                c = c >> 1;
        }
        crc_table[n] = c;
    }
    crc_table_computed = 1;
}

/* Update a running CRC with the bytes buf[0..len-1]--the CRC
  should be initialized to all 1's, and the transmitted value
  is the 1's complement of the final running CRC (see the
  crc() routine below). */

static uint32_t update_crc(uint32_t crc, const uint8_t * buf, int len)
{
    uint32_t c = crc;
    int n;

    if (!crc_table_computed)
        make_crc_table();
    for (n = 0; n < len; n++) {
        c = crc_table[(c ^ buf[n]) & 0xff] ^ (c >> 8);
    }
    return c;
}

/* Return the CRC of the bytes buf[0..len-1]. */
uint32_t fcs(const uint8_t * buf, int len)
{
    return update_crc(0xffffffffL, buf, len) ^ 0xffffffffL;
}

static void ar7_cpmac_write(uint8_t * cpmac, unsigned index, unsigned offset,
                            uint32_t val)
{
    assert((offset & 3) == 0);
    reg_write(cpmac, offset, val);
    TRACE(CPMAC, logout("cpmac%u[%s] (0x%08lx) = 0x%08lx\n",
                        index, i2cpmac(offset / 4),
                        (unsigned long)(AVALANCHE_CPMAC0_BASE +
                                        (AVALANCHE_CPMAC1_BASE -
                                         AVALANCHE_CPMAC0_BASE) * index +
                                        offset), (unsigned long)val));
    if (offset == 0x100) {
        /* 13 ... 8 = 0x20 enable broadcast */
    } else if (offset == 0x10c) {
        TRACE(CPMAC, logout("setting max packet length %u\n", (unsigned)val));
    } else if (offset == CPMAC_TX_INTMASK_SET) {
        /* val 2^i should set tx_int i !!! */
        if (val != 0) {
            unsigned channel = 0;
            while (val != 1) {
                channel++;
                val /= 2;
            }
            reg_set(cpmac, CPMAC_MAC_IN_VECTOR, MAC_IN_VECTOR_TX_INT_OR + channel);
            ar7_irq(0, cpmac_interrupt[index], 1);
        }
    } else if (offset == CPMAC_MACADDRHI) {
        /* set MAC address (4 high bytes) */
        uint8_t *phys = av.nic[index].phys;
        phys[5] = cpmac[CPMAC_MACADDRLO_0];
        phys[4] = cpmac[CPMAC_MACADDRMID];
        phys[3] = cpmac[CPMAC_MACADDRHI + 3];
        phys[2] = cpmac[CPMAC_MACADDRHI + 2];
        phys[1] = cpmac[CPMAC_MACADDRHI + 1];
        phys[0] = cpmac[CPMAC_MACADDRHI + 0];
        TRACE(CPMAC, logout("setting MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                            phys[0], phys[1], phys[2], phys[3], phys[4],
                            phys[5]));
    } else if (offset >= CPMAC_RXGOODFRAMES && offset <= CPMAC_RXDMAOVERRUNS) {
        /* Write access to readonly statistics register. */
        if (val == 0xffffffff) {
            /* Clear register. */
            reg_write(cpmac, offset, 0);
        } else {
            UNEXPECTED();
        }
    } else if (offset >= CPMAC_TX0_HDP && offset <= CPMAC_TX7_HDP) {
        /* Transmit buffer. !!! */
        uint8_t channel = (offset - CPMAC_TX0_HDP) / 4;
        while (val != 0) {
            uint32_t length = 0;
            uint8_t buffer[MAX_ETH_FRAME_SIZE + 4];
            cpphy_tcb_t tcb;
          sendloop:
            {
                cpu_physical_memory_read(val, (uint8_t *) & tcb, sizeof(tcb));
                uint32_t addr = le32_to_cpu(tcb.buff);
                uint32_t curlen = le32_to_cpu(tcb.length);
                uint32_t mode = le32_to_cpu(tcb.mode);
                TRACE(RXTX,
                      logout
                      ("buffer 0x%08x, next 0x%08x, buff 0x%08x, params 0x%08x, len 0x%08x, total 0x%08x\n",
                       val, (unsigned)tcb.next, addr, mode, curlen, length));
                assert(length + curlen <= MAX_ETH_FRAME_SIZE);
                cpu_physical_memory_read(addr, buffer + length, curlen);
                length += curlen;
                assert((mode & CB_SIZE_MASK) == curlen);
                assert(mode & CB_SOF_BIT);
                assert(mode & CB_EOF_BIT);
                assert(mode & CB_OWNERSHIP_BIT);
                mode &= ~(CB_OWNERSHIP_BIT);
                stl_phys(val + offsetof(cpphy_tcb_t, mode), mode);
                if ((mode & CB_EOQ_BIT)) {
                    val = le32_to_cpu(tcb.next);
                    goto sendloop;
                }
            }
            if (av.nic[index].vc != 0) {
#if 0
                uint32_t crc = fcs(buffer, length);
                TRACE(CPMAC,
                      logout("FCS 0x%04x 0x%04x\n",
                             (uint32_t) crc32(~0, buffer, length - 4), crc));
                crc = htonl(crc);
                memcpy(&buffer[length], &crc, 4);
                length += 4;
#endif
                TRACE(RXTX,
                      logout("CPMAC %u sent %u byte: %s\n", index, length,
                             dump(buffer, length)));
                qemu_send_packet(av.nic[index].vc, buffer, length);
                reg_inc(cpmac, CPMAC_TXGOODFRAMES);
                reg_set(cpmac, CPMAC_MAC_IN_VECTOR, MAC_IN_VECTOR_TX_INT_OR + channel);
                ar7_irq(0, cpmac_interrupt[index], 1);
                //~ break;
                //~ reg_inc(cpmac, CPMAC_TXBROADCASTFRAMES);
                //~ reg_inc(cpmac, CPMAC_TXMULTICASTFRAMES);
            }
            val = le32_to_cpu(tcb.next);
        }
    } else if (offset >= CPMAC_RX0_HDP && offset <= CPMAC_RX7_HDP) {
        /* Receive buffer. !!! */
        cpphy_rcb_t rcb;
        cpu_physical_memory_read(val, (uint8_t *) & rcb, sizeof(rcb));
        uint32_t addr = le32_to_cpu(rcb.buff);
        uint32_t length = le32_to_cpu(rcb.length);
        TRACE(CPMAC,
              logout
              ("buffer 0x%08x, next 0x%08x, buff 0x%08x, params 0x%08x, len 0x%08x\n",
               val, (unsigned)rcb.next, addr, (unsigned)rcb.mode, length));
    }
}

/*****************************************************************************
 *
 * Interrupt controller emulation.
 *
 ****************************************************************************/

typedef struct {                /* Avalanche Interrupt control registers */
    uint32_t intsr1;            /* Interrupt Status/Set Register 1   0x00 */
    uint32_t intsr2;            /* Interrupt Status/Set Register 2   0x04 */
    uint32_t unused1;           /* 0x08 */
    uint32_t unused2;           /* 0x0C */
    uint32_t intcr1;            /* Interrupt Clear Register 1        0x10 */
    uint32_t intcr2;            /* Interrupt Clear Register 2        0x14 */
    uint32_t unused3;           /* 0x18 */
    uint32_t unused4;           /* 0x1C */
    uint32_t intesr1;           /* Interrupt Enable (Set) Register 1 0x20 */
    uint32_t intesr2;           /* Interrupt Enable (Set) Register 2 0x24 */
    uint32_t unused5;           /* 0x28 */
    uint32_t unused6;           /* 0x2C */
    uint32_t intecr1;           /* Interrupt Enable Clear Register 1 0x30 */
    uint32_t intecr2;           /* Interrupt Enable Clear Register 2 0x34 */
    uint32_t unused7;           /* 0x38 */
    uint32_t unused8;           /* 0x3c */
    uint32_t pintir;            /* Priority Interrupt Index Register 0x40 */
    uint32_t intmsr;            /* Priority Interrupt Mask Index Reg 0x44 */
    uint32_t unused9;           /* 0x48 */
    uint32_t unused10;          /* 0x4C */
    uint32_t intpolr1;          /* Interrupt Polarity Mask Reg 1     0x50 */
    uint32_t intpolr2;          /* Interrupt Polarity Mask Reg 2     0x54 */
    uint32_t unused11;          /* 0x58 */
    uint32_t unused12;          /* 0x5C */
    uint32_t inttypr1;          /* Interrupt Type Mask Register 1    0x60 */
    uint32_t inttypr2;          /* Interrupt Type Mask Register 2    0x64 */

    /* Avalanche Exception control registers */
    uint32_t exsr;              /* Exceptions Status/Set register    0x80 */
    uint32_t reserved;          /*0x84 */
    uint32_t excr;              /* Exceptions Clear Register         0x88 */
    uint32_t reserved1;         /*0x8c */
    uint32_t exiesr;            /* Exceptions Interrupt Enable (set) 0x90 */
    uint32_t reserved2;         /*0x94 */
    uint32_t exiecr;            /* Exceptions Interrupt Enable(clear)0x98 */
    uint32_t dummy0x9c;

    /* Interrupt Pacing */
    uint32_t ipacep;            /* Interrupt pacing register         0xa0 */
    uint32_t ipacemap;          /* Interrupt Pacing Map Register     0xa4 */
    uint32_t ipacemax;          /* Interrupt Pacing Max Register     0xa8 */
    uint32_t dummy0xac[3 * 4];
    uint32_t dummy0x100[64];

    /* Interrupt Channel Control */
    uint32_t cintnr[40];        /* Channel Interrupt Number Reg     0x200 */
} ar7_intc_t;

static const char *const intc_names[] = {
    "Interrupt Status/Set 1",
    "Interrupt Status/Set 2",
    "0x08",
    "0x0c",
    "Interrupt Clear 1",
    "Interrupt Clear 2",
    "0x18",
    "0x1c",
    "Interrupt Enable Set 1",
    "Interrupt Enable Set 2",
    "0x28",
    "0x2c",
    "Interrupt Enable Clear 1",
    "Interrupt Enable Clear 2",
    "0x38",
    "0x3c",
    "Priority Interrupt Index",
    "Priority Interrupt Mask Index",
    "0x48",
    "0x4c",
    "Interrupt Polarity Mask 1",
    "Interrupt Polarity Mask 2",
    "0x58",
    "0x5c",
    "Interrupt Type Mask 1",
    "Interrupt Type Mask 2",
};

static const char *i2intc(unsigned index)
{
    static char buffer[32];
    const char *text = 0;
    switch (index) {
    case 0x20:
        text = "Exceptions Status/Set";
        break;
    case 0x22:
        text = "Exceptions Clear";
        break;
    case 0x24:
        text = "Exceptions Interrupt Enable (set)";
        break;
    case 0x26:
        text = "Exceptions Interrupt Enable (clear)";
        break;
    case 0x28:
        text = "Interrupt Pacing";
        break;
    case 0x29:
        text = "Interrupt Pacing Map";
        break;
    case 0x2a:
        text = "Interrupt Pacing Max";
        break;
    }
    if (text != 0) {
    } else if (index < 0x1a) {
        text = intc_names[index];
    } else if (index >= 128 && index < 168) {
        text = buffer;
        sprintf(buffer, "Channel Interrupt Number 0x%02x", index - 128);
    } else {
        text = buffer;
        sprintf(buffer, "0x%02x", index);
    }
    assert(strlen(buffer) < sizeof(buffer));
    return text;
}

static uint32_t ar7_intc_read(uint32_t intc[], unsigned index)
{
    uint32_t val = intc[index];
    if (0) {
        //~ } else if (index == 16) {
    } else {
        TRACE(INTC, logout("intc[%s] = %08x\n", i2intc(index), val));
    }
    return val;
}

static void ar7_intc_write(uint32_t intc[], unsigned index, uint32_t val)
{
    unsigned subindex = (index & 1);
    intc[index] = val;
    if (0) {
        //~ } else if (index == 4) {
    } else if (index == 8 || index == 9) {
        av.intmask[subindex] |= val;
        TRACE(INTC, logout("intc[%s] val 0x%08x, mask 0x%08x\n",
                           i2intc(index), val, av.intmask[subindex]));
    } else if (index == 12 || index == 13) {
        av.intmask[subindex] &= ~val;
        TRACE(INTC, logout("intc[%s] val 0x%08x, mask 0x%08x\n",
                           i2intc(index), val, av.intmask[subindex]));
    } else {
        TRACE(INTC, logout("intc[%s] val 0x%08x\n", i2intc(index), val));
    }
}

/*****************************************************************************
 *
 * MDIO emulation.
 *
 ****************************************************************************/

typedef struct {
    uint32_t ver;               /* 0x00 */
#define         MDIO_VER_MODID         (0xFFFF << 16)
#define         MDIO_VER_REVMAJ        (0xFF   << 8)
#define         MDIO_VER_REVMIN        (0xFF)
    uint32_t control;           /* 0x04 */
#define         MDIO_CONTROL_IDLE                 BIT(31)
#define         MDIO_CONTROL_ENABLE               BIT(30)
#define         MDIO_CONTROL_PREAMBLE             BIT(20)
#define         MDIO_CONTROL_FAULT                BIT(19)
#define         MDIO_CONTROL_FAULT_DETECT_ENABLE  BIT(18)
#define         MDIO_CONTROL_INT_TEST_ENABLE      BIT(17)
#define         MDIO_CONTROL_HIGHEST_USER_CHANNEL (0x1F << 8)
#define         MDIO_CONTROL_CLKDIV               (0xFF)
    uint32_t alive;             /* 0x08 */
    uint32_t link;              /* 0x0c */
    uint32_t linkintraw;        /* 0x10 */
    uint32_t linkintmasked;     /* 0x14 */
    uint32_t dummy18[2];
    uint32_t userintraw;        /* 0x20 */
    uint32_t userintmasked;     /* 0x24 */
    uint32_t userintmaskedset;  /* 0x28 */
    uint32_t userintmaskedclr;  /* 0x2c */
    uint32_t dummy30[20];
    uint32_t useraccess0;       /* 0x80 */
#define         MDIO_USERACCESS_GO     BIT(31)
#define         MDIO_USERACCESS_WRITE  BIT(30)
#define         MDIO_USERACCESS_READ   (0 << 30)
#define         MDIO_USERACCESS_ACK    BIT(29)
#define         MDIO_USERACCESS_REGADR (0x1F << 21)
#define         MDIO_USERACCESS_PHYADR (0x1F << 16)
#define         MDIO_USERACCESS_DATA   (0xFFFF)
    uint32_t userphysel0;       /* 0x84 */
#define         MDIO_USERPHYSEL_LINKSEL         BIT(7)
#define         MDIO_USERPHYSEL_LINKINT_ENABLE  BIT(6)
#define         MDIO_USERPHYSEL_PHYADR_MON      (0x1F)
} mdio_t;

#define pMDIO_USERACCESS(base, channel) ((volatile bit32u *)(base+(0x80+(channel*8))))
#define pMDIO_USERPHYSEL(base, channel) ((volatile bit32u *)(base+(0x84+(channel*8))))

typedef struct {
    uint32_t phy_control;
#define PHY_CONTROL_REG       0
#define PHY_RESET           BIT(15)
#define PHY_LOOP            BIT(14)
#define PHY_100             BIT(13)
#define AUTO_NEGOTIATE_EN   BIT(12)
#define PHY_PDOWN           BIT(11)
#define PHY_ISOLATE         BIT(10)
#define RENEGOTIATE         BIT(9)
#define PHY_FD              BIT(8)
    uint32_t phy_status;
#define PHY_STATUS_REG        1
#define NWAY_COMPLETE       BIT(5)
#define NWAY_CAPABLE        BIT(3)
#define PHY_LINKED          BIT(2)
    uint32_t dummy2;
    uint32_t dummy3;
    uint32_t nway_advertize;
    uint32_t nway_remadvertize;
#define NWAY_ADVERTIZE_REG    4
#define NWAY_REMADVERTISE_REG 5
#define NWAY_FD100          BIT(8)
#define NWAY_HD100          BIT(7)
#define NWAY_FD10           BIT(6)
#define NWAY_HD10           BIT(5)
#define NWAY_SEL            BIT(0)
#define NWAY_AUTO           BIT(0)
} mdio_user_t;

#if 0
bit32u control;

control = MDIO_USERACCESS_GO |
    (method) |
    (((regadr) << 21) & MDIO_USERACCESS_REGADR) |
    (((phyadr) << 16) & MDIO_USERACCESS_PHYADR) |
    ((data) & MDIO_USERACCESS_DATA);

myMDIO_USERACCESS = control;

static bit32u _mdioUserAccessRead(PHY_DEVICE * PhyDev, bit32u regadr,
                                  bit32u phyadr)
{

    _mdioWaitForAccessComplete(PhyDev); /* Wait until UserAccess ready */
    _mdioUserAccess(PhyDev, MDIO_USERACCESS_READ, regadr, phyadr, 0);
    _mdioWaitForAccessComplete(PhyDev); /* Wait for Read to complete */

    return (myMDIO_USERACCESS & MDIO_USERACCESS_DATA);
}

#endif

static uint32_t mdio_regaddr;
static uint32_t mdio_phyaddr;
static uint32_t mdio_data;

static uint16_t mdio_useraccess_data[1][6] = {
    {
     AUTO_NEGOTIATE_EN,
     0x7801 + NWAY_CAPABLE,     // + NWAY_COMPLETE + PHY_LINKED,
     0x00000000,
     0x00000000,
     NWAY_FD100 + NWAY_HD100 + NWAY_FD10 + NWAY_HD10 + NWAY_AUTO,
     NWAY_AUTO}
};

static uint32_t ar7_mdio_read(uint32_t mdio[], unsigned index)
{
    uint32_t val = av.mdio[index];
    if (index == 0) {
        /* MDIO_VER */
        TRACE(MDIO, logout("mdio[MDIO_VER] = 0x%08lx\n", (unsigned long)val));
//~ cpMacMdioInit(): MDIO_CONTROL = 0x40000138
//~ cpMacMdioInit(): MDIO_CONTROL < 0x40000037
    } else if (index == 1) {
        /* MDIO_CONTROL */
        TRACE(MDIO,
              logout("mdio[MDIO_CONTROL] = 0x%08lx\n", (unsigned long)val));
    } else if (index == 0x20) {
        //~ mdio_regaddr = (val & MDIO_USERACCESS_REGADR) >> 21;
        //~ mdio_phyaddr = (val & MDIO_USERACCESS_PHYADR) >> 16;
        mdio_data = (val & MDIO_USERACCESS_DATA);
        TRACE(MDIO,
              logout
              ("mdio[0x%02x] = 0x%08lx, reg = %u, phy = %u, data = 0x%04x\n",
               index, (unsigned long)val, mdio_regaddr, mdio_phyaddr,
               mdio_data));
    } else {
        TRACE(MDIO,
              logout("mdio[0x%02x] = 0x%08lx\n", index, (unsigned long)val));
    }
    return val;
}

static void ar7_mdio_write(uint32_t mdio[], unsigned index, unsigned val)
{
    if (index == 0) {
        /* MDIO_VER */
        TRACE(MDIO, logout("unexpected: mdio[0x%02x] = 0x%08lx\n",
                           index, (unsigned long)val));
    } else if (index == 1) {
        /* MDIO_CONTROL */
        TRACE(MDIO, logout("mdio[MDIO_CONTROL] = 0x%08lx\n",
                           (unsigned long)val));
    } else if (index == 0x20 && (val & MDIO_USERACCESS_GO)) {
        uint32_t write = (val & MDIO_USERACCESS_WRITE) >> 30;
        mdio_regaddr = (val & MDIO_USERACCESS_REGADR) >> 21;
        mdio_phyaddr = (val & MDIO_USERACCESS_PHYADR) >> 16;
        mdio_data = (val & MDIO_USERACCESS_DATA);
        TRACE(MDIO,
              logout
              ("mdio[0x%02x] = 0x%08lx, write = %u, reg = %u, phy = %u, data = 0x%04x\n",
               index, (unsigned long)val, write, mdio_regaddr, mdio_phyaddr,
               mdio_data));
        val &= MDIO_USERACCESS_DATA;
        if (mdio_phyaddr == 31 && mdio_regaddr < 6) {
            mdio_phyaddr = 0;
            if (write) {
                //~ if ((mdio_regaddr == PHY_CONTROL_REG) && (val & PHY_RESET)) {
                //~ 1000 7809 0000 0000 01e1 0001
                //~ mdio_useraccess_data[0][PHY_CONTROL_REG] = 0x1000;
                //~ mdio_useraccess_data[0][PHY_STATUS_REG] = 0x782d;
                //~ mdio_useraccess_data[0][NWAY_ADVERTIZE_REG] = 0x01e1;
                /* 100FD=Yes, 100HD=Yes, 10FD=Yes, 10HD=Yes */
                //~ mdio_useraccess_data[0][NWAY_REMADVERTISE_REG] = 0x85e1;
                //~ }
                mdio_useraccess_data[mdio_phyaddr][mdio_regaddr] = val;
            } else {
                val = mdio_useraccess_data[mdio_phyaddr][mdio_regaddr];
                if ((mdio_regaddr == PHY_CONTROL_REG) && (val & PHY_RESET)) {
                    mdio_useraccess_data[mdio_phyaddr][mdio_regaddr] =
                        ((val & ~PHY_RESET) | AUTO_NEGOTIATE_EN);
                } else if ((mdio_regaddr == PHY_CONTROL_REG)
                           && (val & RENEGOTIATE)) {
                    val &= ~RENEGOTIATE;
                    mdio_useraccess_data[mdio_phyaddr][mdio_regaddr] = val;
                    //~ 0x0000782d 0x00007809
                    mdio_useraccess_data[mdio_phyaddr][1] = 0x782d;
                    mdio_useraccess_data[mdio_phyaddr][5] =
                        mdio_useraccess_data[mdio_phyaddr][4] | PHY_ISOLATE |
                        PHY_RESET;
                    mdio[3] = 0x80000000;
                }
            }
        }
    } else {
        TRACE(MDIO,
              logout("mdio[0x%02x] = 0x%08lx\n", index, (unsigned long)val));
    }
    av.mdio[index] = val;
}

static void ar7_reset_write(uint32_t offset, uint32_t val)
{
    if (offset == 0) {
#if RESET
        static const char *resetdevice[] = {
            /* 00 */ "uart0", "uart1", "i2c", "timer0",
            /* 04 */ "timer1", "reserved05", "gpio", "adsl",
            /* 08 */ "usb", "atm", "reserved10", "vdma",
            /* 12 */ "fser", "reserved13", "reserved14", "reserved15",
            /* 16 */ "vlynq1", "cpmac0", "mcdma", "bist",
            /* 20 */ "vlynq0", "cpmac1", "mdio", "dsp",
            /* 24 */ "reserved24", "reserved25", "ephy", "reserved27",
            /* 28 */ "reserved28", "reserved29", "reserved30", "reserved31"
        };
        // Reset bit coded device(s). 0 = disabled (reset), 1 = enabled.
        static uint32_t oldval;
        uint32_t changed = (val ^ oldval);
        uint32_t enabled = (changed & val);
        //~ uint32_t disabled = (changed & oldval);
        unsigned i;
        oldval = val;
        for (i = 0; i < 32; i++) {
            if (changed & (1 << i)) {
                TRACE(RESET,
                      logout("reset %s %s\n",
                             (enabled & (1 << i)) ? "enabled" : "disabled",
                             resetdevice[i]));
            }
        }
#endif
    } else if (offset == 4) {
        TRACE(RESET, logout("reset\n"));
        qemu_system_reset_request();
        //~ CPUState *cpu_env = first_cpu;
        //~ cpu_env->PC = 0xbfc00000;
    } else {
        TRACE(RESET, logout("reset[%u]=0x%08x\n", offset, val));
    }
}

/*****************************************************************************
 *
 * VLYNQ emulation.
 *
 ****************************************************************************/

static const char *const vlynq_names[] = {
    /* 0x00 */
    "Revision",
    "Control",
    "Status",
    "Interrupt Priority Vector Status/Clear",
    /* 0x10 */
    "Interrupt Status/Clear",
    "Interrupt Pending/Set",
    "Interrupt Pointer",
    "Tx Address Map",
    /* 0x20 */
    "Rx Address Map Size 1",
    "Rx Address Map Offset 1",
    "Rx Address Map Size 2",
    "Rx Address Map Offset 2",
    /* 0x30 */
    "Rx Address Map Size 3",
    "Rx Address Map Offset 3",
    "Rx Address Map Size 4",
    "Rx Address Map Offset 4",
    /* 0x40 */
    "Chip Version",
    "Auto Negotiation",
    "Manual Negotiation",
    "Negotiation Status",
    /* 0x50 */
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    /* 0x60 */
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    /* 0x70 */
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    /* 0x80 */
    "Remote Revision",
    "Remote Control",
    "Remote Status",
    "Remote Interrupt Priority Vector Status/Clear",
    /* 0x90 */
    "Remote Interrupt Status/Clear",
    "Remote Interrupt Pending/Set",
    "Remote Interrupt Pointer",
    "Remote Tx Address Map",
    /* 0xa0 */
    "Remote Rx Address Map Size 1",
    "Remote Rx Address Map Offset 1",
    "Remote Rx Address Map Size 2",
    "Remote Rx Address Map Offset 2",
    /* 0xb0 */
    "Remote Rx Address Map Size 3",
    "Remote Rx Address Map Offset 3",
    "Remote Rx Address Map Size 4",
    "Remote Rx Address Map Offset 4",
    /* 0xc0 */
    "Remote Chip Version",
    "Remote Auto Negotiation",
    "Remote Manual Negotiation",
    "Remote Negotiation Status",
    /* 0xd0 */
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    /* 0xe0 */
    "Remote Interrupt Vector 3-0",
    "Remote Interrupt Vector 7-4",
};

typedef enum {
    VLYNQ_REVID = 0x00,
    VLYNQ_CTRL = 0x04,
    VLYNQ_STAT = 0x08,
    VLYNQ_INTPRI = 0x0c,
    VLYNQ_INTSTATCLR = 0x10,
    VLYNQ_INTPENDSET = 0x14,
    VLYNQ_INTPTR = 0x18,
    VLYNQ_XAM = 0x1c,
    VLYNQ_RAMS1 = 0x20,
    VLYNQ_RAMO1 = 0x24,
    VLYNQ_RAMS2 = 0x28,
    VLYNQ_RAMO2 = 0x2c,
    VLYNQ_RAMS3 = 0x30,
    VLYNQ_RAMO3 = 0x34,
    VLYNQ_RAMS4 = 0x38,
    VLYNQ_RAMO4 = 0x3c,
    VLYNQ_CHIPVER = 0x40,
    VLYNQ_AUTNGO = 0x44,
    VLYNQ_RREVID = 0x80,
    VLYNQ_RCTRL = 0x84,
    VLYNQ_RSTAT = 0x88,
    VLYNQ_RINTPRI = 0x8c,
    VLYNQ_RINTSTATCLR = 0x90,
    VLYNQ_RINTPENDSET = 0x94,
    VLYNQ_RINTPTR = 0x98,
    VLYNQ_RXAM = 0x9c,
    VLYNQ_RRAMS1 = 0xa0,
    VLYNQ_RRAMO1 = 0xa4,
    VLYNQ_RRAMS2 = 0xa8,
    VLYNQ_RRAMO2 = 0xac,
    VLYNQ_RRAMS3 = 0xb0,
    VLYNQ_RRAMO3 = 0xb4,
    VLYNQ_RRAMS4 = 0xb8,
    VLYNQ_RRAMO4 = 0xbc,
    VLYNQ_RCHIPVER = 0xc0,
    VLYNQ_RAUTNGO = 0xc4,
    VLYNQ_RMANNGO = 0xc8,
    VLYNQ_RNGOSTAT = 0xcc,
    VLYNQ_RINTVEC0 = 0xe0,
    VLYNQ_RINTVEC1 = 0xe4,
} vlynq_register_t;

#if 0
struct _vlynq_registers_half {

    /*--- 0x00 Revision/ID Register ---*/
    unsigned int Revision_ID;

    /*--- 0x04 Control Register ---*/
    union __vlynq_Control {
        struct _vlynq_Control {
#define VLYNQ_CTL_CTRL_SHIFT            0
            unsigned int reset:1;
#define VLYNQ_CTL_ILOOP_SHIFT           1
            unsigned int iloop:1;
#define VLYNQ_CTL_AOPT_DISABLE_SHIFT    2
            unsigned int aopt_disable:1;
            unsigned int reserved1:4;
#define VLYNQ_CTL_INT2CFG_SHIFT         7
            unsigned int int2cfg:1;
#define VLYNQ_CTL_INTVEC_SHIFT          8
            unsigned int intvec:5;
#define VLYNQ_CTL_INTEN_SHIFT           13
            unsigned int intenable:1;
#define VLYNQ_CTL_INTLOCAL_SHIFT        14
            unsigned int intlocal:1;
#define VLYNQ_CTL_CLKDIR_SHIFT          15
            unsigned int clkdir:1;
#define VLYNQ_CTL_CLKDIV_SHIFT          16
            unsigned int clkdiv:3;
            unsigned int reserved2:2;
#define VLYNQ_CTL_TXFAST_SHIFT          21
            unsigned int txfastpath:1;
#define VLYNQ_CTL_RTMEN_SHIFT           22
            unsigned int rtmenable:1;
#define VLYNQ_CTL_RTMVALID_SHIFT        23
            unsigned int rtmvalidwr:1;
#define VLYNQ_CTL_RTMSAMPLE_SHIFT       24
            unsigned int rxsampleval:3;
            unsigned int reserved3:3;
#define VLYNQ_CTL_SCLKUDIS_SHIFT        30
            unsigned int sclkpudis:1;
#define VLYNQ_CTL_PMEM_SHIFT            31
            unsigned int pmen:1;
        } Bits;
        volatile unsigned int Register;
    } Control;

    /*--- 0x08 Status Register ---*/
    union __vlynq_Status {
        struct _vlynq_Status {
            unsigned int link:1;
            unsigned int mpend:1;
            unsigned int spend:1;
            unsigned int nfempty0:1;
            unsigned int nfempty1:1;
            unsigned int nfempty2:1;
            unsigned int nfempty3:1;
            unsigned int lerror:1;
            unsigned int rerror:1;
            unsigned int oflow:1;
            unsigned int iflow:1;
            unsigned int rtm:1;
            unsigned int rxcurrent_sample:3;
            unsigned int reserved1:5;
            unsigned int swidthout:4;
            unsigned int swidthin:4;
            unsigned int reserved2:4;
        } Bits;
        volatile unsigned int Register;
    } Status;

    /*--- 0x0C Interrupt Priority Vector Status/Clear Register ---*/
    union __vlynq_Interrupt_Priority {
        struct _vlynq_Interrupt_Priority {
            unsigned int intstat:5;
            unsigned int reserved:(32 - 5 - 1);
            unsigned int nointpend:1;
        } Bits;
        volatile unsigned int Register;
    } Interrupt_Priority;

    /*--- 0x10 Interrupt Status/Clear Register ---*/
    volatile unsigned int Interrupt_Status;

    /*--- 0x14 Interrupt Pending/Set Register ---*/
    volatile unsigned int Interrupt_Pending_Set;

    /*--- 0x18 Interrupt Pointer Register ---*/
    volatile unsigned int Interrupt_Pointer;

    /*--- 0x1C Tx Address Map ---*/
    volatile unsigned int Tx_Address;

    /*--- 0x20 Rx Address Map Size 1 ---*/
    /*--- 0x24 Rx Address Map Offset 1 ---*/
    /*--- 0x28 Rx Address Map Size 2 ---*/
    /*--- 0x2c Rx Address Map Offset 2 ---*/
    /*--- 0x30 Rx Address Map Size 3 ---*/
    /*--- 0x34 Rx Address Map Offset 3 ---*/
    /*--- 0x38 Rx Address Map Size 4 ---*/
    /*--- 0x3c Rx Address Map Offset 4 ---*/
    struct ___vlynq_Rx_Address Rx_Address[4];

    /*--- 0x40 Chip Version Register ---*/
    struct ___vlynq_Chip_Version Chip_Version;

    /*--- 0x44 Auto Negotiation Register ---*/
    union __Auto_Negotiation {
        struct _Auto_Negotiation {
            unsigned int reserved1:16;
            unsigned int _2_x:1;
            unsigned int reserved2:15;
        } Bits;
        volatile unsigned int Register;
    } Auto_Negotiation;

    /*--- 0x48 Manual Negotiation Register ---*/
    volatile unsigned int Manual_Negotiation;

    /*--- 0x4C Negotiation Status Register ---*/
    union __Negotiation_Status {
        struct _Negotiation_Status {
            unsigned int status:1;
            unsigned int reserved:31;
        } Bits;
        volatile unsigned int Register;
    } Negotiation_Status;

    /*--- 0x50-0x5C Reserved ---*/
    unsigned char reserved1[0x5C - 0x4C];

    /*--- 0x60 Interrupt Vector 3-0 ---*/
    union __vlynq_Interrupt_Vector Interrupt_Vector_1;

    /*--- 0x64 Interrupt Vector 7-4 ---*/
    union __vlynq_Interrupt_Vector Interrupt_Vector_2;

    /*--- 0x68-0x7C Reserved for Interrupt Vectors 8-31 ---*/
    unsigned char reserved2[0x7C - 0x64];
};
#endif

static uint32_t ar7_vlynq_read(uint8_t * vlynq, uint32_t offset)
{
    uint32_t val = reg_read(vlynq, offset);
    TRACE(VLYNQ, logout("vlynq%u[0x%02x (%s)] = 0x%08lx\n",
                        (vlynq == av.vlynq1), offset,
                        (offset < 0xe8) ? vlynq_names[offset / 4] : "unknown",
                        (unsigned long)val));
    if (offset == VLYNQ_REVID) {
        val = cpu_to_le32(0x00010206);
    } else {
    }
    return val;
}

static void ar7_vlynq_write(uint8_t * vlynq, uint32_t offset, uint32_t val)
{
    TRACE(VLYNQ, logout("vlynq%u[0x%02x (%s)] = 0x%08lx\n",
                        (vlynq == av.vlynq1), offset,
                        (offset < 0xe8) ? vlynq_names[offset / 4] : "unknown",
                        (unsigned long)val));
    if (offset == VLYNQ_REVID) {
    } else if (offset == VLYNQ_CTRL) {
        /* control */
        if (!(val & BIT(0))) {
            /* Normal operation. Emulation sets link bit in status register. */
            reg_set(vlynq, VLYNQ_STAT, BIT(0));
        } else {
            /* Reset. */
            reg_clear(vlynq, VLYNQ_STAT, BIT(0));
        }
    } else {
    }
    reg_write(vlynq, offset, val);
}

/*****************************************************************************
 *
 * Watchdog timer emulation.
 *
 * This watchdog timer module has prescalar and counter which divide the input
 * reference frequency and upon expiration, the system is reset.
 * 
 *                        ref_freq 
 * Reset freq = ---------------------
 *                  (prescalar * counter)
 * 
 * This watchdog timer supports timer values in mSecs. Thus
 * 
 *           prescalar * counter * 1 KHZ
 * mSecs =   --------------------------
 *                  ref_freq
 *
 ****************************************************************************/

#define KHZ                         1000
#define KICK_VALUE                  1

#define KICK_LOCK_1ST_STAGE         0x5555
#define KICK_LOCK_2ND_STAGE         0xAAAA
#define PRESCALE_LOCK_1ST_STAGE     0x5A5A
#define PRESCALE_LOCK_2ND_STAGE     0xA5A5
#define CHANGE_LOCK_1ST_STAGE       0x6666
#define CHANGE_LOCK_2ND_STAGE       0xBBBB
#define DISABLE_LOCK_1ST_STAGE      0x7777
#define DISABLE_LOCK_2ND_STAGE      0xCCCC
#define DISABLE_LOCK_3RD_STAGE      0xDDDD

typedef struct {
    uint32_t kick_lock;         /* 0x00 */
    uint32_t kick;              /* 0x04 */
    uint32_t change_lock;       /* 0x08 */
    uint32_t change;            /* 0x0c */
    uint32_t disable_lock;      /* 0x10 */
    uint32_t disable;           /* 0x14 */
    uint32_t prescale_lock;     /* 0x18 */
    uint32_t prescale;          /* 0x1c */
} wdtimer_t;

static uint16_t wd_val(uint16_t val, uint16_t bits)
{
    return ((val & ~0x3) | bits);
}

static void ar7_wdt_write(unsigned offset, uint32_t val)
{
    wdtimer_t *wdt = (wdtimer_t *) & av.watchdog;
    if (offset == offsetof(wdtimer_t, kick_lock)) {
        if (val == KICK_LOCK_1ST_STAGE) {
            TRACE(WDOG, logout("kick lock 1st stage\n"));
            wdt->kick_lock = wd_val(val, 1);
        } else if (val == KICK_LOCK_2ND_STAGE) {
            TRACE(WDOG, logout("kick lock 2nd stage\n"));
            wdt->kick_lock = wd_val(val, 3);
        } else {
            TRACE(WDOG,
                  logout("kick lock unexpected value 0x%08x, %s\n", val,
                         backtrace()));
        }
    } else if (offset == offsetof(wdtimer_t, kick)) {
        if (wdt->kick_lock != wd_val(KICK_LOCK_2ND_STAGE, 3)) {
            TRACE(WDOG, logout("kick still locked!\n"));
            UNEXPECTED();
        } else if (val == KICK_VALUE) {
            TRACE(WDOG, logout("kick (restart) watchdog\n"));
        }
        MISSING();
    } else if (offset == offsetof(wdtimer_t, change_lock)) {
        if (val == CHANGE_LOCK_1ST_STAGE) {
            TRACE(WDOG, logout("change lock 1st stage\n"));
            wdt->change_lock = wd_val(val, 1);
        } else if (val == CHANGE_LOCK_2ND_STAGE) {
            TRACE(WDOG, logout("change lock 2nd stage\n"));
            wdt->change_lock = wd_val(val, 3);
        } else {
            TRACE(WDOG,
                  logout("change lock unexpected value 0x%08x, %s\n", val,
                         backtrace()));
        }
    } else if (offset == offsetof(wdtimer_t, change)) {
        if (wdt->change_lock != wd_val(CHANGE_LOCK_2ND_STAGE, 3)) {
            TRACE(WDOG, logout("change still locked!\n"));
            UNEXPECTED();
        } else {
            TRACE(WDOG, logout("change watchdog, val=0x%08x\n", val));  // val = 0xdf5c
        }
        MISSING();
    } else if (offset == offsetof(wdtimer_t, disable_lock)) {
        if (val == DISABLE_LOCK_1ST_STAGE) {
            TRACE(WDOG, logout("disable lock 1st stage\n"));
            wdt->disable_lock = wd_val(val, 1);
        } else if (val == DISABLE_LOCK_2ND_STAGE) {
            TRACE(WDOG, logout("disable lock 2nd stage\n"));
            wdt->disable_lock = wd_val(val, 2);
        } else if (val == DISABLE_LOCK_3RD_STAGE) {
            TRACE(WDOG, logout("disable lock 3rd stage\n"));
            wdt->disable_lock = wd_val(val, 3);
        } else {
            TRACE(WDOG,
                  logout("disable lock unexpected value 0x%08x, %s\n", val,
                         backtrace()));
        }
    } else if (offset == offsetof(wdtimer_t, disable)) {
        if (wdt->disable_lock != wd_val(DISABLE_LOCK_3RD_STAGE, 3)) {
            TRACE(WDOG, logout("disable still locked, val=0x%08x!\n", val));
            UNEXPECTED();
        } else {
            TRACE(WDOG, logout("disable watchdog, val=0x%08x\n", val)); // val = 0
        }
        MISSING();
    } else if (offset == offsetof(wdtimer_t, prescale_lock)) {
        if (val == PRESCALE_LOCK_1ST_STAGE) {
            TRACE(WDOG, logout("prescale lock 1st stage\n"));
            wdt->prescale_lock = wd_val(val, 1);
        } else if (val == PRESCALE_LOCK_2ND_STAGE) {
            TRACE(WDOG, logout("prescale lock 2nd stage\n"));
            wdt->prescale_lock = wd_val(val, 3);
        } else {
            TRACE(WDOG,
                  logout("prescale lock unexpected value 0x%08x, %s\n", val,
                         backtrace()));
        }
    } else if (offset == offsetof(wdtimer_t, prescale)) {
        if (wdt->prescale_lock != wd_val(PRESCALE_LOCK_2ND_STAGE, 3)) {
            TRACE(WDOG, logout("prescale still locked, val=0x%08x!\n", val));
            UNEXPECTED();
        } else {
            TRACE(WDOG, logout("set watchdog prescale, val=0x%08x\n", val));    // val = 0xffff
        }
        MISSING();
    } else {
        TRACE(WDOG,
              logout("??? offset 0x%02x = 0x%08x, %s\n", offset, val,
                     backtrace()));
    }
}

/*****************************************************************************
 *
 * Generic AR7 hardware emulation.
 *
 ****************************************************************************/

#define INRANGE(base, var) \
        (((addr) >= (base)) && ((addr) < ((base) + (sizeof(var)) - 1)))

#define VALUE(base, var) var[(addr - (base)) / 4]

static uint32_t ar7_io_memread(void *opaque, uint32_t addr)
{
    unsigned index;
    uint32_t val = 0xffffffff;
    const char *name = 0;
    int logflag = OTHER;

    assert(!(addr & 3));

    if (INRANGE(AVALANCHE_ADSLSSYS_MEM_BASE, av.adsl)) {
        name = "adsl";
        val = VALUE(AVALANCHE_ADSLSSYS_MEM_BASE, av.adsl);
    } else if (INRANGE(AVALANCHE_BBIF_BASE, av.bbif)) {
        name = "bbif";
        val = VALUE(AVALANCHE_BBIF_BASE, av.bbif);
    } else if (INRANGE(AVALANCHE_ATM_SAR_BASE, av.atmsar)) {
        name = "atm sar";
        val = VALUE(AVALANCHE_ATM_SAR_BASE, av.atmsar);
    } else if (INRANGE(AVALANCHE_USB_MEM_BASE, av.usbslave)) {
        name = "usb memory";
        val = VALUE(AVALANCHE_USB_MEM_BASE, av.usbslave);
    } else if (INRANGE(AVALANCHE_VLYNQ0_MEM_MAP_BASE, av.vlynq0mem)) {
        name = "vlynq0 memory";
        logflag = VLYNQ;
        val = VALUE(AVALANCHE_VLYNQ0_MEM_MAP_BASE, av.vlynq0mem);
        if (addr == 0x04041000) {
            /* Write PCI device id for TI TNETW1130 (ACX111) */
            val = 0x9066104c;
        }
    } else if (INRANGE(AVALANCHE_CPMAC0_BASE, av.cpmac0)) {
        //~ name = "cpmac0";
        logflag = 0;
        val = ar7_cpmac_read(av.cpmac0, addr - AVALANCHE_CPMAC0_BASE);
    } else if (INRANGE(AVALANCHE_EMIF_BASE, av.emif)) {
        name = "emif";
        logflag = EMIF;
        val = VALUE(AVALANCHE_EMIF_BASE, av.emif);
    } else if (INRANGE(AVALANCHE_GPIO_BASE, av.gpio)) {
        name = "gpio";
        logflag = GPIO;
        val = VALUE(AVALANCHE_GPIO_BASE, av.gpio);
        if (addr == 0x08610900 && val == 0x00000800) {
            /* Do not log polling of reset button. */
            logflag = 0;
        }
    } else if (INRANGE(AVALANCHE_CLOCK_BASE, av.clock_control)) {
        name = "clock";
        logflag = CLOCK;
        index = (addr - AVALANCHE_CLOCK_BASE) / 4;
        val = av.clock_control[index];
        if (index == 0x0c || index == 0x14 || index == 0x1c || index == 0x24) {
            /* Reset PLL status bit. */
            if (val == 4) {
                val &= ~1;
            } else {
                val |= 1;
            }
        }
    } else if (INRANGE(AVALANCHE_WATCHDOG_BASE, av.watchdog)) {
        name = "watchdog";
        logflag = WDOG;
        val = VALUE(AVALANCHE_WATCHDOG_BASE, av.watchdog);
    } else if (INRANGE(AVALANCHE_TIMER0_BASE, av.timer0)) {
        name = "timer0";
        val = VALUE(AVALANCHE_TIMER0_BASE, av.timer0);
    } else if (INRANGE(AVALANCHE_UART0_BASE, av.uart0)) {
        name = "uart0";
        logflag = UART0;
        val = cpu_inb(av.cpu_env, UART_MEM_TO_IO(addr));
        //~ val = VALUE(AVALANCHE_UART0_BASE, av.uart0);
    } else if (INRANGE(AVALANCHE_UART1_BASE, av.uart1)) {
        name = "uart1";
        logflag = UART1;
        val = cpu_inb(av.cpu_env, UART_MEM_TO_IO(addr));
    } else if (INRANGE(AVALANCHE_USB_SLAVE_BASE, av.usb)) {
        name = "usb slave";
        val = VALUE(AVALANCHE_USB_SLAVE_BASE, av.usb);
    } else if (INRANGE(AVALANCHE_RESET_BASE, av.reset_control)) {
        name = "reset control";
        logflag = RESET;
        val = VALUE(AVALANCHE_RESET_BASE, av.reset_control);
    } else if (INRANGE(AVALANCHE_DCL_BASE, av.device_config_latch)) {
        name = "device config latch";
        val = VALUE(AVALANCHE_DCL_BASE, av.device_config_latch);
    } else if (INRANGE(AVALANCHE_VLYNQ0_BASE, av.vlynq0)) {
        //~ name = "vlynq0";
        logflag = 0;
        val = ar7_vlynq_read(av.vlynq0, addr - AVALANCHE_VLYNQ0_BASE);
    } else if (INRANGE(AVALANCHE_VLYNQ1_BASE, av.vlynq1)) {
        //~ name = "vlynq1";
        logflag = 0;
        val = ar7_vlynq_read(av.vlynq1, addr - AVALANCHE_VLYNQ1_BASE);
    } else if (INRANGE(AVALANCHE_MDIO_BASE, av.mdio)) {
        name = "mdio";
        logflag = MDIO;
        index = (addr - AVALANCHE_MDIO_BASE) / 4;
        val = ar7_mdio_read(av.mdio, index);
    } else if (INRANGE(OHIO_WDT_BASE, av.wdt)) {
        name = "ohio wdt";
        val = VALUE(OHIO_WDT_BASE, av.wdt);
    } else if (INRANGE(AVALANCHE_INTC_BASE, av.intc)) {
        //~ name = "intc";
        logflag = 0;
        index = (addr - AVALANCHE_INTC_BASE) / 4;
        val = ar7_intc_read(av.intc, index);
    } else if (INRANGE(AVALANCHE_CPMAC1_BASE, av.cpmac1)) {
        //~ name = "cpmac1";
        logflag = 0;
        val = ar7_cpmac_read(av.cpmac1, addr - AVALANCHE_CPMAC1_BASE);
    } else {
        name = "???";
        logflag = 1;
        {
            TRACE(logflag, logout("addr 0x%08lx (%s) = 0x%08x, caller %s\n",
                                  (unsigned long)addr, name, val, backtrace()));
            MISSING();
        }
    }
    if (name != 0) {
        TRACE(logflag, logout("addr 0x%08lx (%s) = 0x%08x\n",
                              (unsigned long)addr, name, val));
    }
    return val;
}

static void ar7_io_memwrite(void *opaque, uint32_t addr, uint32_t val)
{
    unsigned index;
    const char *name = 0;
    int logflag = OTHER;

    assert(!(addr & 3));

    if (INRANGE(AVALANCHE_ADSLSSYS_MEM_BASE, av.adsl)) {
        name = "adsl";
        VALUE(AVALANCHE_ADSLSSYS_MEM_BASE, av.adsl) = val;
    } else if (INRANGE(AVALANCHE_BBIF_BASE, av.bbif)) {
        name = "bbif";
        VALUE(AVALANCHE_BBIF_BASE, av.bbif) = val;
    } else if (INRANGE(AVALANCHE_ATM_SAR_BASE, av.atmsar)) {
        name = "atm sar";
        VALUE(AVALANCHE_ATM_SAR_BASE, av.atmsar) = val;
    } else if (INRANGE(AVALANCHE_USB_MEM_BASE, av.usbslave)) {
        name = "usb memory";
        //~ VALUE(AVALANCHE_USB_MEM_BASE, av.usbslave) = val;
        VALUE(AVALANCHE_USB_MEM_BASE, av.usbslave) = 0xffffffff;
    } else if (INRANGE(AVALANCHE_VLYNQ0_MEM_MAP_BASE, av.vlynq0mem)) {
        name = "vlynq0 memory";
        logflag = VLYNQ;
        VALUE(AVALANCHE_VLYNQ0_MEM_MAP_BASE, av.vlynq0mem) = val;
    } else if (INRANGE(AVALANCHE_CPMAC0_BASE, av.cpmac0)) {
        //~ name = "cpmac0";
        logflag = 0;
        ar7_cpmac_write(av.cpmac0, 0, addr - AVALANCHE_CPMAC0_BASE, val);
    } else if (INRANGE(AVALANCHE_EMIF_BASE, av.emif)) {
        name = "emif";
        logflag = EMIF;
        VALUE(AVALANCHE_EMIF_BASE, av.emif) = val;
    } else if (INRANGE(AVALANCHE_GPIO_BASE, av.gpio)) {
        name = "gpio";
        logflag = GPIO;
        VALUE(AVALANCHE_GPIO_BASE, av.gpio) = val;
    } else if (INRANGE(AVALANCHE_CLOCK_BASE, av.clock_control)) {
        name = "clock control";
        logflag = CLOCK;
        index = (addr - AVALANCHE_CLOCK_BASE) / 4;
        TRACE(CLOCK, logout("addr 0x%08lx (clock) = %04x\n",
                            (unsigned long)addr, val));
        if (index == 0) {
            uint32_t oldpowerstate =
                VALUE(AVALANCHE_CLOCK_BASE, av.clock_control) >> 30;
            uint32_t newpowerstate = val;
            if (oldpowerstate != newpowerstate) {
                TRACE(CLOCK, logout("change power state from %u to %u\n",
                                    oldpowerstate, newpowerstate));
            }
        }
        VALUE(AVALANCHE_CLOCK_BASE, av.clock_control) = val;
    } else if (INRANGE(AVALANCHE_WATCHDOG_BASE, av.watchdog)) {
        //~ name = "watchdog";
        logflag = 0;
        ar7_wdt_write(addr - AVALANCHE_WATCHDOG_BASE, val);
    } else if (INRANGE(AVALANCHE_TIMER0_BASE, av.timer0)) {
        name = "timer0";
        VALUE(AVALANCHE_TIMER0_BASE, av.timer0) = val;
    } else if (INRANGE(AVALANCHE_UART0_BASE, av.uart0)) {
        name = "uart0";
        logflag = UART0;
        cpu_outb(av.cpu_env, UART_MEM_TO_IO(addr), val);
        //~ VALUE(AVALANCHE_UART0_BASE, av.uart0) = val;
    } else if (INRANGE(AVALANCHE_UART1_BASE, av.uart1)) {
        name = "uart1";
        logflag = UART1;
        cpu_outb(av.cpu_env, UART_MEM_TO_IO(addr), val);
    } else if (INRANGE(AVALANCHE_USB_SLAVE_BASE, av.usb)) {
        name = "usb slave";
        VALUE(AVALANCHE_USB_SLAVE_BASE, av.usb) = val;
    } else if (INRANGE(AVALANCHE_RESET_BASE, av.reset_control)) {
        //~ name = "reset control";
        logflag = 0;
        VALUE(AVALANCHE_RESET_BASE, av.reset_control) = val;
        ar7_reset_write(addr - AVALANCHE_RESET_BASE, val);
    } else if (INRANGE(AVALANCHE_DCL_BASE, av.device_config_latch)) {
        name = "device config latch";
        VALUE(AVALANCHE_DCL_BASE, av.device_config_latch) = val;
    } else if (INRANGE(AVALANCHE_VLYNQ0_BASE, av.vlynq0)) {
        //~ name = "vlynq0";
        logflag = 0;
        ar7_vlynq_write(av.vlynq0, addr - AVALANCHE_VLYNQ0_BASE, val);
    } else if (INRANGE(AVALANCHE_VLYNQ1_BASE, av.vlynq1)) {
        //~ name = "vlynq1";
        logflag = 0;
        ar7_vlynq_write(av.vlynq1, addr - AVALANCHE_VLYNQ1_BASE, val);
    } else if (INRANGE(AVALANCHE_MDIO_BASE, av.mdio)) {
        name = "mdio";
        logflag = MDIO;
        index = (addr - AVALANCHE_MDIO_BASE) / 4;
        ar7_mdio_write(av.mdio, index, val);
    } else if (INRANGE(OHIO_WDT_BASE, av.wdt)) {
        name = "ohio wdt";
        VALUE(OHIO_WDT_BASE, av.wdt) = val;
    } else if (INRANGE(AVALANCHE_INTC_BASE, av.intc)) {
        //~ name = "intc";
        logflag = 0;
        index = (addr - AVALANCHE_INTC_BASE) / 4;
        ar7_intc_write(av.intc, index, val);
    } else if (INRANGE(AVALANCHE_CPMAC1_BASE, av.cpmac1)) {
        //~ name = "cpmac1";
        logflag = 0;
        ar7_cpmac_write(av.cpmac1, 1, addr - AVALANCHE_CPMAC1_BASE, val);
    } else {
        name = "???";
        logflag = 1;
    }
    if (name != 0) {
        TRACE(logflag, logout("addr 0x%08lx (%s) = 0x%08x\n",
                              (unsigned long)addr, name, val));
    }
}

static void io_writeb(void *opaque, target_phys_addr_t addr, uint32_t value)
{
    if (addr & 3) {
        ar7_io_memwrite(opaque, addr, value);
        logout("addr=0x%08x, val=0x%02x\n", addr, value);
        UNEXPECTED();
    } else if (INRANGE(AVALANCHE_UART0_BASE, av.uart0)) {
        ar7_io_memwrite(opaque, addr, value);
    } else if (INRANGE(AVALANCHE_UART1_BASE, av.uart1)) {
        ar7_io_memwrite(opaque, addr, value);
    } else {
        ar7_io_memwrite(opaque, addr, value);
        logout("addr=0x%08x, val=0x%02x\n", addr, value);
        UNEXPECTED();
    }
    //~ cpu_outb(NULL, addr & 0xffff, value);
}

static uint32_t io_readb(void *opaque, target_phys_addr_t addr)
{
    uint32_t value = (ar7_io_memread(opaque, addr & ~3) & 0xff);
    if (addr & 3) {
        logout("addr=0x%08x, val=0x%02x\n", addr, value);
        UNEXPECTED();
    } else if (INRANGE(AVALANCHE_UART0_BASE, av.uart0)) {
    } else if (INRANGE(AVALANCHE_UART1_BASE, av.uart1)) {
    } else {
        logout("addr=0x%08x, val=0x%02x\n", addr, value);
        UNEXPECTED();
    }
    return value;
}

static void io_writew(void *opaque, target_phys_addr_t addr, uint32_t value)
{
    logout("addr=0x%08x, val=0x%02x\n", addr, value);
    UNEXPECTED();
    ar7_io_memwrite(opaque, addr, value);
}

static uint32_t io_readw(void *opaque, target_phys_addr_t addr)
{
    uint32_t value = ar7_io_memread(opaque, addr & ~3);
    switch (addr & 3) {
    case 0:
        value >>= 16;
        break;
    case 2:
        value &= 0xffff;
        break;
    default:
        assert(0);
    }
    logout("addr=0x%08x, val=0x%04x\n", addr, value);
    return value;
}

static void io_writel(void *opaque, target_phys_addr_t addr, uint32_t value)
{
    ar7_io_memwrite(opaque, addr, value);
}

static uint32_t io_readl(void *opaque, target_phys_addr_t addr)
{
    return ar7_io_memread(opaque, addr);
}

static CPUWriteMemoryFunc *const io_write[] = {
    io_writeb,
    io_writew,
    io_writel,
};

static CPUReadMemoryFunc *const io_read[] = {
    io_readb,
    io_readw,
    io_readl,
};

static void ar7_serial_init(CPUState * env)
{
    /* By default, QEMU only opens one serial console.
     * In this case we open a second console here because
     * we need it for full hardware emulation.
     */
    av.cpu_env = env;
    if (serial_hds[1] == 0) {
        serial_hds[1] = qemu_chr_open("vc");
    }
    serial_16450_init(ar7_irq, IRQ_OPAQUE,
                      UART_MEM_TO_IO(AVALANCHE_UART0_BASE), 15, serial_hds[0]);
    serial_16450_init(ar7_irq, IRQ_OPAQUE,
                      UART_MEM_TO_IO(AVALANCHE_UART1_BASE), 16, serial_hds[1]);
}

static int ar7_nic_can_receive(void *opaque)
{
    unsigned index = (unsigned)opaque;
    uint8_t *cpmac = av.cpmac0;
    if (index != 0) {
        cpmac = av.cpmac1;
    }

    TRACE(CPMAC, logout("CPMAC %u\n", index));

    return reg_read(cpmac, CPMAC_RX0_HDP) != 0;
}

static void ar7_nic_receive(void *opaque, const uint8_t * buf, int size)
{
    unsigned index = (unsigned)opaque;
    uint8_t *cpmac = av.cpmac0;
    if (index != 0) {
        cpmac = av.cpmac1;
    }

    TRACE(RXTX,
          logout("CPMAC %u received %u byte: %s\n", index, size,
                 dump(buf, size)));

    /* Received a packet. */
    static const uint8_t broadcast_macaddr[6] =
        { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

    if (!memcmp(buf, broadcast_macaddr, 6)) {
        TRACE(CPMAC, logout("broadcast\n"));
        reg_inc(cpmac, CPMAC_RXBROADCASTFRAMES);
    } else if (buf[0] & 0x01) {
        TRACE(CPMAC, logout("multicast\n"));
        reg_inc(cpmac, CPMAC_RXMULTICASTFRAMES);
    } else if (!memcmp(buf, av.nic[index].phys, 6)) {
        TRACE(CPMAC, logout("my address\n"));
    } else {
        TRACE(CPMAC, logout("unknown address\n"));
    }

    /* !!! check handling of short and long frames */
    if (size < 64) {
        reg_inc(cpmac, CPMAC_RXUNDERSIZEDFRAMES);
    } else if (size > MAX_ETH_FRAME_SIZE) {
        reg_inc(cpmac, CPMAC_RXOVERSIZEDFRAMES);
    }

    reg_inc(cpmac, CPMAC_RXGOODFRAMES);

    uint32_t val = reg_read(cpmac, CPMAC_RX0_HDP);
    if (val == 0) {
        TRACE(RXTX, logout("no buffer available, frame ignored\n"));
    } else {
        cpphy_rcb_t rcb;
        cpu_physical_memory_read(val, (uint8_t *) & rcb, sizeof(rcb));
        uint32_t addr = le32_to_cpu(rcb.buff);
        uint32_t length = le32_to_cpu(rcb.length);
        uint32_t mode = le32_to_cpu(rcb.mode);
        TRACE(CPMAC,
              logout
              ("buffer 0x%08x, next 0x%08x, buff 0x%08x, params 0x%08x, len 0x%08x\n",
               val, (unsigned)rcb.next, addr, mode, length));
        if (mode & CB_OWNERSHIP_BIT) {
            //~ assert(length > size);
            mode &= ~(CB_OWNERSHIP_BIT);
            mode |= (size & CB_SIZE_MASK);
            mode |= CB_SOF_BIT | CB_EOF_BIT /*| CB_OWNERSHIP_BIT */ ;
            if (rcb.next == 0) {
                TRACE(CPMAC, logout("last buffer\n"));
                mode |= CB_EOQ_BIT;
            }
            rcb.length = cpu_to_le32(size);
            rcb.mode = cpu_to_le32(mode);
            cpu_physical_memory_write(addr, buf, size);
            cpu_physical_memory_write(val, (uint8_t *) & rcb, sizeof(rcb));
            reg_write(cpmac, CPMAC_RX0_HDP, rcb.next);

            reg_set(cpmac, CPMAC_MAC_IN_VECTOR, MAC_IN_VECTOR_RX_INT_OR + 0);
            ar7_irq(0, cpmac_interrupt[index], 1);      // !!! fix
        } else {
            logout("buffer not free, frame ignored\n");
        }
    }
}

static void ar7_nic_init(void)
{
    unsigned i;
    unsigned n = 0;
    TRACE(CPMAC, logout("\n"));
    for (i = 0; i < nb_nics; i++) {
        NICInfo *nd = &nd_table[i];
        if (nd->vlan) {
            if (n < 2 && (nd->model == NULL || strcmp(nd->model, "ar7") == 0)) {
                TRACE(CPMAC, logout("starting AR7 nic CPMAC%u\n", n));
                av.nic[n++].vc = qemu_new_vlan_client(nd->vlan, ar7_nic_receive,
                                                      ar7_nic_can_receive,
                                                      (void *)n);
            } else {
                fprintf(stderr, "qemu: Unsupported NIC: %s\n",
                        nd_table[n].model);
                exit(1);
            }
        }
    }
}

static int ar7_load(QEMUFile * f, void *opaque, int version_id)
{
    int result = 0;
    if (version_id == 0) {
        qemu_get_buffer(f, (uint8_t *) & av, sizeof(av));
    } else {
        result = -EINVAL;
    }
    return result;
}

static void ar7_save(QEMUFile * f, void *opaque)
{
    /* TODO: fix */
    qemu_put_buffer(f, (uint8_t *) & av, sizeof(av));
}

static void ar7_reset(void *opaque)
{
    CPUState *env = opaque;
    logout("%s:%u\n", __FILE__, __LINE__);
    env->exception_index = EXCP_RESET;
    do_interrupt(env);
    //~ env->CP0_Cause |= 0x00000400;
    //~ cpu_interrupt(env, CPU_INTERRUPT_RESET);
}

void ar7_init(CPUState * env)
{
    //~ target_phys_addr_t addr = (0x08610000 & 0xffff);
    //~ unsigned offset;
    int io_memory = cpu_register_io_memory(0, io_read, io_write, env);
    //~ cpu_register_physical_memory(0x08610000, 0x00002800, io_memory);
    //~ cpu_register_physical_memory(0x00001000, 0x0860f000, io_memory);
    cpu_register_physical_memory(0x00001000, 0x0ffff000, io_memory);
    cpu_register_physical_memory(0x1e000000, 0x01c00000, io_memory);
    assert(bigendian == 0);
    bigendian = env->bigendian;
    assert(bigendian == 0);
    logout("setting endianness %d\n", bigendian);
    ar7_serial_init(env);
    ar7_nic_init();
    //~ for (offset = 0; offset < 0x2800; offset += 0x100) {
    //~ if (offset == 0xe00) continue;
    //~ if (offset == 0xf00) continue;
    //~ register_ioport_read(addr + offset, 0x100, 1, ar7_io_memread, 0);
    //~ register_ioport_read(addr + offset, 0x100, 2, ar7_io_memread, 0);
    //~ register_ioport_read(addr + offset, 0x100, 4, ar7_io_memread, 0);
    //~ register_ioport_write(addr + offset, 0x100, 1, ar7_io_memwrite, 0);
    //~ register_ioport_write(addr + offset, 0x100, 2, ar7_io_memwrite, 0);
    //~ register_ioport_write(addr + offset, 0x100, 4, ar7_io_memwrite, 0);
    //~ }
    //~ {
    //~ struct SerialState state = {
    //~ base: 0,
    //~ it_shift: 0
    //~ };
    //~ s_io_memory = cpu_register_io_memory(&state, mips_mm_read, mips_mm_write, 0);
    //~ cpu_register_physical_memory(0x08610000, 0x2000, s_io_memory);
    //~ }
#define ar7_instance 0
#define ar7_version 0
    qemu_register_reset(ar7_reset, env);
    register_savevm("ar7", ar7_instance, ar7_version, ar7_save, ar7_load, 0);
}

#if 0
static void ar7_machine_power_off(void)
{
    volatile uint32_t *power_reg = (void *)(KSEG1ADDR(0x08610A00));
    uint32_t power_state = *power_reg;

    /* add something to turn LEDs off? */

    power_state &= ~(3 << 30);
    power_state |= (3 << 30);   /* power down */
    *power_reg = power_state;

    printk("after power down?\n");
}

. / arch / mips / ar7 / tnetd73xx_misc.c:
#define CLKC_CLKCR(x)          (TNETD73XX_CLOCK_CTRL_BASE + 0x20 + (0x20 * (x)))
. / arch / mips / ar7 / tnetd73xx_misc.c:
#define CLKC_CLKPLLCR(x)       (TNETD73XX_CLOCK_CTRL_BASE + 0x30 + (0x20 * (x)))
. / include / asm - mips / ar7 / tnetd73xx.h:
#define TNETD73XX_CLOCK_CTRL_BASE           PHYS_TO_K1(0x08610A00)      /* Clock Control */
. / include / asm - mips / ar7 / tnetd73xx.h:
#define TNETD73XX_POWER_CTRL_PDCR           (TNETD73XX_CLOCK_CTRL_BASE + 0x0)
. / include / asm - mips / ar7 / tnetd73xx.h:
#define TNETD73XX_POWER_CTRL_PCLKCR         (TNETD73XX_CLOCK_CTRL_BASE + 0x4)
. / include / asm - mips / ar7 / tnetd73xx.h:
#define TNETD73XX_POWER_CTRL_PDUCR          (TNETD73XX_CLOCK_CTRL_BASE + 0x8)
. / include / asm - mips / ar7 / tnetd73xx.h:
#define TNETD73XX_POWER_CTRL_WKCR           (TNETD73XX_CLOCK_CTRL_BASE + 0xC)
. / include / asm - mips / ar7 / tnetd73xx.h:
#define TNETD73XX_CLK_CTRL_SCLKCR           (TNETD73XX_CLOCK_CTRL_BASE + 0x20)
. / include / asm - mips / ar7 / tnetd73xx.h:
#define TNETD73XX_CLK_CTRL_SCLKPLLCR        (TNETD73XX_CLOCK_CTRL_BASE + 0x30)
. / include / asm - mips / ar7 / tnetd73xx.h:
#define TNETD73XX_CLK_CTRL_MCLKCR           (TNETD73XX_CLOCK_CTRL_BASE + 0x40)
. / include / asm - mips / ar7 / tnetd73xx.h:
#define TNETD73XX_CLK_CTRL_MCLKPLLCR        (TNETD73XX_CLOCK_CTRL_BASE + 0x50)
. / include / asm - mips / ar7 / tnetd73xx.h:
#define TNETD73XX_CLK_CTRL_UCLKCR           (TNETD73XX_CLOCK_CTRL_BASE + 0x60)
. / include / asm - mips / ar7 / tnetd73xx.h:
#define TNETD73XX_CLK_CTRL_UCLKPLLCR        (TNETD73XX_CLOCK_CTRL_BASE + 0x70)
. / include / asm - mips / ar7 / tnetd73xx.h:
#define TNETD73XX_CLK_CTRL_ACLKCR0          (TNETD73XX_CLOCK_CTRL_BASE + 0x80)
. / include / asm - mips / ar7 / tnetd73xx.h:
#define TNETD73XX_CLK_CTRL_ACLKPLLCR0       (TNETD73XX_CLOCK_CTRL_BASE + 0x90)
. / include / asm - mips / ar7 / tnetd73xx.h:
#define TNETD73XX_CLK_CTRL_ACLKCR1          (TNETD73XX_CLOCK_CTRL_BASE + 0xA0)
. / include / asm - mips / ar7 / tnetd73xx.h:
#define TNETD73XX_CLK_CTRL_ACLKPLLCR1       (TNETD73XX_CLOCK_CTRL_BASE + 0xB0)

#endif
