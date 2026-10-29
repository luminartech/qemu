/*
 * TC4x GETH Ethernet controller
 *
 * TC4x GETH0 is based on the Synopsys DWC Ethernet QoS core and has two MAC
 * ports.  No PHY is attached by default, so MDIO reads of a port return all
 * ones; board code attaches PHYs with tc4x_geth_attach_phy().
 * The PTP system time stamps frames in TX status and RX context descriptors.
 * The on-chip HSPHY reports its calibration and DLL-lock status as done.
 * The bridge forwards between the ports: in two-port mode with TxQ/RxC
 * mapping (FORWARD_CONTROL.Q_CH_MAPPING_EN), a frame that lands in RxC q of
 * one port (its receive queue: untagged queue, VLAN priority queues, or
 * the multicast/broadcast queue of MAC_RXQ_CTRL1) is sent by the other port
 * instead of the host's DMA when one of that port's TxQs is enabled
 * (TXQ_EN), a forwarding path (FWD_EN) and mapped to RxC q (TXQ_MAP_PORT_n),
 * as IfxGeth_Bridge_setPortMode(), _enablePortTxQueuesAndRxChannels() and
 * _mapTxQueue() program it.  The forwarding packet length limit, queue
 * flushes and same-port loopback are not modelled.
 *
 * Sources, all public: the GETH and HSPHY registers of the iLLD SFR headers
 * and the iLLD Ethernet driver (IfxGeth) bundled with the TC4D7 projects of
 * https://github.com/Infineon/AURIX_code_examples; the DWC Ethernet QoS
 * descriptor and DMA behaviour as Linux's stmmac driver programs it.  The
 * HSPHY status completed at once is what the iLLD waits for: INITDONE in
 * IfxHsphy.c, the DLL LOCKSTAT in IfxHsphy_Dll.c and the common calibration
 * status read through PHYREG in IfxPcie.c.
 *
 * Copyright (c) 2026 MicroVision, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/net/tc4x_geth.h"
#include "net/net.h"
#include "net/eth.h"
#include "net/checksum.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "qom/object.h"

OBJECT_DECLARE_SIMPLE_TYPE(Tc4xGethState, TC4X_GETH)

#define R_CLC                 0x0000u
#define R_RST_CTRLA           0x000Cu
#define R_RST_CTRLB           0x0010u
#define R_RST_STAT            0x0014u
#define CLC_DISR              BIT(0)
#define CLC_DISS              BIT(1)
#define RST_KRST              BIT(0)
#define RST_CTRLB_STATCLR     BIT(31)
#define CORE_BASE             0x10000u   /* everything a kernel reset clears */

#define PORT_BASE(p)          (0x10000u + (p) * 0x2000u)
#define PORT_OF(off)          (((off) - 0x10000u) / 0x2000u)
#define MAC_TX_CONFIGURATION  0x000u
#define MAC_RX_CONFIGURATION  0x004u
#define MAC_PACKET_FILTER     0x008u
#define MAC_VLAN_TAG_CTRL     0x050u
#define MAC_RXQ_CTRL0         0x0A0u
#define MAC_RXQ_CTRL1         0x0A4u
#define MAC_RXQ_CTRL2         0x0A8u
#define MAC_RXQ_CTRL3         0x0ACu
#define MAC_INTERRUPT_STATUS  0x0B0u
#define MAC_INTERRUPT_ENABLE  0x0B4u
#define MAC_VERSION           0x110u
#define MAC_HW_FEATURE0       0x11Cu
#define MAC_HW_FEATURE1       0x120u
#define MAC_HW_FEATURE2       0x124u
#define MDIO_ADDRESS          0x200u
#define MDIO_DATA             0x204u
#define MDIO_CLAUSE_22_PORT   0x220u
#define MAC_ADDRESS_HIGH(n)   (0x300u + (n) * 8u)
#define MAC_ADDRESS_LOW(n)    (0x304u + (n) * 8u)
#define MAC_ADDRESS_COUNT     32
#define MMC_TX_OCTETS_GB      0x814u
#define MMC_TX_PACKETS_GB     0x81Cu
#define MMC_TX_PACKETS_G      0x88Cu
#define MMC_TX_OCTETS_G       0x884u
#define MMC_RX_PACKETS_GB     0x900u
#define MMC_RX_OCTETS_GB      0x908u
#define MMC_RX_OCTETS_G       0x910u
#define MMC_RX_BROADCAST      0x918u
#define MMC_RX_MULTICAST      0x920u
#define MMC_RX_UNICAST        0x970u
#define MAC_TIMESTAMP_CONTROL 0xD00u
#define MAC_SYSTIME_SEC       0xD08u
#define MAC_SYSTIME_NSEC      0xD0Cu
#define MAC_SYSTIME_SEC_UPD   0xD10u
#define MAC_SYSTIME_NSEC_UPD  0xD14u
#define MAC_SYSTIME_HI_SEC    0xD1Cu
#define MAC_TIMESTAMP_STATUS  0xD20u
#define MAC_TXTS_NSEC         0xD30u
#define MAC_TXTS_SEC          0xD34u
#define MAC_TXTS_PKTID        0xD38u
#define MTL_RXQ_DMA_MAP0      0x1030u
#define MTL_RXQ_DMA_MAP1      0x1034u
#define MTL_TCQ_BASE          0x1100u
#define MTL_TCQ_END           0x1500u
#define MTL_TCQ_STRIDE        0x80u

#define MAC_TE                BIT(0)
#define MAC_RE                BIT(0)
#define RXQ_MCBCQEN           BIT(15)
#define RXQ_OMCBCQ            BIT(20)
#define PF_PR                 BIT(0)
#define PF_PM                 BIT(4)
#define PF_DBF                BIT(5)
#define PF_RA                 BIT(31)
#define MAC_ADDR_AE           BIT(31)
#define MDIO_SBUSY            BIT(22)
#define MDIO_CMD_SHIFT        16
#define MDIO_CMD_WRITE        1u
#define MDIO_CMD_READ         3u
#define MDIO_CMD_POSTINC_READ 2u
#define TS_TSENA              BIT(0)
#define TS_TSINIT             BIT(2)
#define TS_TSUPDT             BIT(3)
#define TS_TSADDREG           BIT(5)
#define TS_TSENALL            BIT(8)
#define TS_STATUS_TTSNS_SHIFT 10
#define TS_STATUS_TXTSC       BIT(15)
#define MAC_INT_TSIS          BIT(12)
#define NSEC_UPD_ADDSUB       BIT(31)

#define BRIDGE_FORWARD_CONTROL 0x1E000u
#define BRIDGE_PORT_CONTROL(p) (0x1E00Cu + (p) * 0xCu)
#define BRIDGE_TXQ_MAP(p)      (0x1E010u + (p) * 0xCu)
#define BRIDGE_FWD_EN_SHIFT    0
#define BRIDGE_TXQ_EN_SHIFT    24
#define FWD_Q_CH_MAPPING_EN    BIT(31)

#define DMA_MODE              0x1F000u
#define DMA_INTERRUPT_STATUS  0x1F008u
#define DMA_MODE_SWR          BIT(0)
#define DMA_CH_BASE           0x1F100u
#define DMA_CH_STRIDE         0x80u
#define DMA_CH_END            (DMA_CH_BASE + TC4X_GETH_NUM_DMA * DMA_CH_STRIDE)
#define CH_CONTROL            0x00u
#define CH_TX_CONTROL         0x04u
#define CH_RX_CONTROL         0x08u
#define CH_TXDESC_LIST        0x14u
#define CH_RXDESC_LIST        0x1Cu
#define CH_TXDESC_TAIL        0x24u
#define CH_RXDESC_TAIL        0x2Cu
#define CH_TX_CONTROL2        0x30u
#define CH_RX_CONTROL2        0x34u
#define CH_INTERRUPT_ENABLE   0x38u
#define CH_CUR_TXDESC         0x44u
#define CH_CUR_RXDESC         0x4Cu
#define CH_CUR_TXBUF          0x54u
#define CH_CUR_RXBUF          0x5Cu
#define CH_STATUS             0x60u
#define CH_MISS_PACKET_CNT    0x6Cu
#define CH_CONTROL_DSL(v)     extract32(v, 18, 3)
#define CH_TX_ST              BIT(0)
#define CH_RX_SR              BIT(0)
#define CH_RX_RBSZ(v)         extract32(v, 1, 14)
#define CH_RING_LEN(v)        (extract32(v, 0, 16) + 1)
#define ST_TI                 BIT(0)
#define ST_TBU                BIT(2)
#define ST_RI                 BIT(6)
#define ST_RBU                BIT(7)
#define ST_AIS                BIT(14)
#define ST_NIS                BIT(15)
#define ST_TX_MASK            (ST_TI | ST_TBU)
#define ST_RX_MASK            (ST_RI | ST_RBU)
#define IE_AIE                BIT(14)
#define IE_NIE                BIT(15)

#define DESC_SIZE             16
#define DES3_OWN              BIT(31)
#define DES3_CTXT             BIT(30)
#define DES3_FD               BIT(29)
#define DES3_LD               BIT(28)
#define TDES2_IOC             BIT(31)
#define TDES3_CIC(v)          extract32(v, 16, 2)
#define RDES3_IOC             BIT(30)
#define TDES2_TTSE            BIT(30)
#define TDES3_PIDV            BIT(25)
#define TDES3_TTSS            BIT(17)
#define RDES3_RS1V            BIT(26)
#define RDES1_TSA             BIT(14)
/* TSA as it appears in RDES3 of the RX context descriptor */
#define RDES3_CTX_TSA         BIT(4)

#define HS_PHY_CTRL1(n)       (0x214u + (n) * 8u)
#define HS_PHYREG_CTRL(n)     (0x230u + (n) * 8u)
#define HS_PHYREG_DATA(n)     (0x234u + (n) * 8u)
#define HS_DLL_CFG            0x628u
#define HS_XPCS_FIRST         0x800000u
#define HS_XPCS_STRIDE        0x800000u
#define HS_PMA_TX_GENCTRL2    0x600C8u
#define HS_PMA_RX_GENCTRL2    0x60148u
#define HS_PCS_SR_XS_CTRL1    0xC0000u
#define HS_PCS_VR_DIG_CTRL1   0xE0000u
#define HS_PCS_VR_DIG_STS     0xE0040u
#define HS_PHY_INITDONE       BIT(3)
#define HS_PHYREG_ACT         BIT(19)
#define HS_PHYREG_READ        BIT(17)
#define HS_CMNCAL_DONE        BIT(1)
#define HS_DLL_LOCKSTAT       BIT(30)
#define HS_GENCTRL2_REQ       BIT(0)
#define HS_SR_LPM             BIT(11)
#define HS_SR_RST             BIT(15)
#define HS_DIG_CTRL1_SELFCLR  (BIT(8) | BIT(10) | BIT(15))
#define HS_PSEQ_SHIFT         2
#define HS_PSEQ_NORMAL        4u
#define HS_PSEQ_LPM           6u

#define MAX_FRAME             16384
#define MIN_FRAME             60
#define VLAN_HLEN             4
#define TXTS_FIFO_DEPTH       16

typedef struct Tc4xGethTxTs {
    uint32_t pktid, nsec, sec;
} Tc4xGethTxTs;

typedef struct Tc4xGethPort {
    struct Tc4xGethState *s;
    int index;
    NICConf conf;
    NICState *nic;
    uint8_t phy_addr;
    const Tc4xGethPhyOps *phy;  /* NULL: nothing answers on MDIO */
    void *phy_opaque;
    int64_t ptp_offset_ns;
    Tc4xGethTxTs txts[TXTS_FIFO_DEPTH];
    unsigned txts_count;
} Tc4xGethPort;

typedef struct Tc4xGethDma {
    uint32_t tx_cur, rx_cur;
    uint32_t tx_pktid;          /* packet id from the last TX context desc */
    bool tx_pktid_valid;
    uint8_t txbuf[MAX_FRAME];
    unsigned txlen;
} Tc4xGethDma;

struct Tc4xGethState {
    SysBusDevice parent_obj;

    MemoryRegion geth_mr;
    uint32_t regs[TC4X_GETH_SIZE / 4];
    MemoryRegion hsphy_mr;
    GHashTable *hsphy;
    Tc4xGethPort port[TC4X_GETH_NUM_PORTS];
    Tc4xGethDma dma[TC4X_GETH_NUM_DMA];
    qemu_irq irq[TC4X_GETH_NUM_IRQ];
    bool irq_level[TC4X_GETH_NUM_IRQ];
};

/* Atomic: geth_read() serves plain registers without the BQL */
static inline uint32_t greg(Tc4xGethState *s, uint32_t off)
{
    return qatomic_read(&s->regs[off / 4]);
}

static inline void sreg(Tc4xGethState *s, uint32_t off, uint32_t v)
{
    qatomic_set(&s->regs[off / 4], v);
}

static inline uint32_t preg(Tc4xGethState *s, int p, uint32_t off)
{
    return greg(s, PORT_BASE(p) + off);
}

static inline uint32_t chreg(Tc4xGethState *s, int ch, uint32_t off)
{
    return greg(s, DMA_CH_BASE + ch * DMA_CH_STRIDE + off);
}

static inline void chset(Tc4xGethState *s, int ch, uint32_t off, uint32_t v)
{
    sreg(s, DMA_CH_BASE + ch * DMA_CH_STRIDE + off, v);
}

static void mmc_inc(Tc4xGethState *s, int p, uint32_t off, uint32_t n)
{
    sreg(s, PORT_BASE(p) + off, preg(s, p, off) + n);
}

static void geth_set_irq(Tc4xGethState *s, int n, bool level)
{
    if (level != s->irq_level[n]) {
        s->irq_level[n] = level;
        qemu_set_irq(s->irq[n], level);
    }
}

static void geth_update_irq(Tc4xGethState *s)
{
    uint32_t dma_is = 0;

    for (int ch = 0; ch < TC4X_GETH_NUM_DMA; ch++) {
        uint32_t st = chreg(s, ch, CH_STATUS);
        uint32_t ie = chreg(s, ch, CH_INTERRUPT_ENABLE);
        uint32_t act = st & ie & ~(IE_AIE | IE_NIE);

        if (st & ie & 0x3FFFu) {
            dma_is |= BIT(ch);
        }
        geth_set_irq(s, TC4X_GETH_IRQ_TX(ch), act & ST_TX_MASK);
        geth_set_irq(s, TC4X_GETH_IRQ_RX(ch), act & ST_RX_MASK);
    }
    for (int p = 0; p < TC4X_GETH_NUM_PORTS; p++) {
        if (preg(s, p, MAC_INTERRUPT_STATUS) &
            preg(s, p, MAC_INTERRUPT_ENABLE)) {
            dma_is |= BIT(17);
        }
    }
    sreg(s, DMA_INTERRUPT_STATUS, dma_is);
    geth_set_irq(s, TC4X_GETH_IRQ_MAC, dma_is & BIT(17));
}

static void ch_status(Tc4xGethState *s, int ch, uint32_t bits)
{
    uint32_t st = chreg(s, ch, CH_STATUS) | bits;

    if (st & (ST_TI | ST_RI)) {
        st |= ST_NIS;
    }
    if (st & (ST_TBU | ST_RBU)) {
        st |= ST_AIS;
    }
    chset(s, ch, CH_STATUS, st);
    geth_update_irq(s);
}

static int64_t ptp_now(Tc4xGethPort *port)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + port->ptp_offset_ns;
}

static void ptp_split(int64_t t, uint32_t *sec, uint32_t *nsec)
{
    if (t < 0) {
        t = 0;
    }
    *sec = t / NANOSECONDS_PER_SECOND;
    *nsec = t % NANOSECONDS_PER_SECOND;
}

static void ptp_control_write(Tc4xGethState *s, int p, uint32_t val)
{
    Tc4xGethPort *port = &s->port[p];
    int64_t upd = (int64_t)preg(s, p, MAC_SYSTIME_SEC_UPD) *
                  NANOSECONDS_PER_SECOND +
                  (preg(s, p, MAC_SYSTIME_NSEC_UPD) & ~NSEC_UPD_ADDSUB);

    if (val & TS_TSINIT) {
        port->ptp_offset_ns = upd - qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    } else if (val & TS_TSUPDT) {
        port->ptp_offset_ns += (preg(s, p, MAC_SYSTIME_NSEC_UPD) &
                                NSEC_UPD_ADDSUB) ? -upd : upd;
    }
    /* TSINIT, TSUPDT, TSADDREG complete immediately */
    sreg(s, PORT_BASE(p) + MAC_TIMESTAMP_CONTROL,
         val & ~(TS_TSINIT | TS_TSUPDT | TS_TSADDREG));
}

static void txts_push(Tc4xGethState *s, int p, uint32_t pktid,
                      uint32_t sec, uint32_t nsec)
{
    Tc4xGethPort *port = &s->port[p];

    if (port->txts_count == TXTS_FIFO_DEPTH) {
        return;
    }
    port->txts[port->txts_count] = (Tc4xGethTxTs){ pktid, nsec, sec };
    qatomic_set(&port->txts_count, port->txts_count + 1);
    sreg(s, PORT_BASE(p) + MAC_INTERRUPT_STATUS,
         preg(s, p, MAC_INTERRUPT_STATUS) | MAC_INT_TSIS);
    geth_update_irq(s);
}

static uint32_t txts_read(Tc4xGethState *s, int p, uint32_t reg)
{
    Tc4xGethPort *port = &s->port[p];
    Tc4xGethTxTs head;

    if (port->txts_count == 0) {
        return 0;
    }
    head = port->txts[0];
    if (reg == MAC_TXTS_PKTID) {
        return head.pktid;
    }
    if (reg == MAC_TXTS_NSEC) {
        return head.nsec;
    }
    /* reading the seconds word completes the FIFO entry */
    memmove(&port->txts[0], &port->txts[1],
            (port->txts_count - 1) * sizeof(port->txts[0]));
    qatomic_set(&port->txts_count, port->txts_count - 1);
    return head.sec;
}

static void mdio_command(Tc4xGethState *s, int p, uint32_t ctrl)
{
    Tc4xGethPort *port = &s->port[p];
    uint32_t addr = preg(s, p, MDIO_ADDRESS);
    uint32_t pa = extract32(addr, 16, 5);
    uint32_t ra = extract32(addr, 0, 16);
    uint32_t da = extract32(addr, 21, 5);
    uint32_t cmd = extract32(ctrl, MDIO_CMD_SHIFT, 2);
    bool cl22 = preg(s, p, MDIO_CLAUSE_22_PORT) & BIT(pa);
    bool write = cmd == MDIO_CMD_WRITE;
    uint16_t data = 0xFFFF;

    if (port->phy && pa == port->phy_addr) {
        if (cl22 && write) {
            port->phy->write22(port->phy_opaque, ra & 0x1F, ctrl & 0xFFFF);
        } else if (cl22) {
            data = port->phy->read22(port->phy_opaque, ra & 0x1F);
        } else if (write) {
            port->phy->write45(port->phy_opaque, da, ra, ctrl & 0xFFFF);
        } else {
            data = port->phy->read45(port->phy_opaque, da, ra);
            if (cmd == MDIO_CMD_POSTINC_READ) {
                sreg(s, PORT_BASE(p) + MDIO_ADDRESS,
                     deposit32(addr, 0, 16, ra + 1));
            }
        }
    }
    ctrl &= ~MDIO_SBUSY;
    if (!write) {
        ctrl = deposit32(ctrl, 0, 16, data);
    }
    sreg(s, PORT_BASE(p) + MDIO_DATA, ctrl);
}

static void dma_rd(uint32_t addr, void *buf, unsigned len)
{
    if (address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED,
                           buf, len) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "tc4x-geth: DMA read fault 0x%08x\n",
                      addr);
    }
}

static void dma_wr(uint32_t addr, const void *buf, unsigned len)
{
    if (address_space_write(&address_space_memory, addr,
                            MEMTXATTRS_UNSPECIFIED, buf, len) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "tc4x-geth: DMA write fault 0x%08x\n",
                      addr);
    }
}

static void desc_load(uint32_t addr, uint32_t d[4])
{
    dma_rd(addr, d, DESC_SIZE);
    for (int i = 0; i < 4; i++) {
        d[i] = le32_to_cpu(d[i]);
    }
}

static void desc_store(uint32_t addr, const uint32_t d[4])
{
    uint32_t le[4];

    for (int i = 0; i < 4; i++) {
        le[i] = cpu_to_le32(d[i]);
    }
    dma_wr(addr, le, DESC_SIZE);
}

static uint32_t desc_next(Tc4xGethState *s, int ch, uint32_t cur, bool tx)
{
    uint32_t base = chreg(s, ch, tx ? CH_TXDESC_LIST : CH_RXDESC_LIST);
    uint32_t len = CH_RING_LEN(chreg(s, ch, tx ? CH_TX_CONTROL2 :
                                            CH_RX_CONTROL2));
    uint32_t stride = DESC_SIZE + CH_CONTROL_DSL(chreg(s, ch, CH_CONTROL)) * 8;
    uint32_t idx = (cur - base) / stride + 1;

    return idx >= len ? base : base + idx * stride;
}

/* TxQs of port @p that are enabled and fed by its Tx DMA channels */
static uint32_t txq_dma_mask(Tc4xGethState *s, int p)
{
    uint32_t ctrl = greg(s, BRIDGE_PORT_CONTROL(p));

    return extract32(ctrl, BRIDGE_TXQ_EN_SHIFT, 8) &
           ~extract32(ctrl, BRIDGE_FWD_EN_SHIFT, 8);
}

static int tx_port_of_channel(Tc4xGethState *s, int ch)
{
    for (int p = 0; p < TC4X_GETH_NUM_PORTS; p++) {
        uint32_t en = txq_dma_mask(s, p);
        uint32_t map = greg(s, BRIDGE_TXQ_MAP(p));

        for (int q = 0; q < 8; q++) {
            if ((en & BIT(q)) && extract32(map, q * 4, 4) == ch) {
                return p;
            }
        }
    }
    return 0;
}

static void port_send(Tc4xGethState *s, int p, const uint8_t *buf,
                      unsigned len)
{
    Tc4xGethPort *port = &s->port[p];

    if (!port->nic || !(preg(s, p, MAC_TX_CONFIGURATION) & MAC_TE)) {
        return;
    }
    mmc_inc(s, p, MMC_TX_PACKETS_GB, 1);
    mmc_inc(s, p, MMC_TX_PACKETS_G, 1);
    mmc_inc(s, p, MMC_TX_OCTETS_GB, len);
    mmc_inc(s, p, MMC_TX_OCTETS_G, len);
    qemu_send_packet(qemu_get_queue(port->nic), buf, len);
}

static void tx_checksum(uint8_t *buf, unsigned len, uint32_t cic)
{
    unsigned l2 = ETH_HLEN;
    uint8_t *ip;
    unsigned ihl, tot;

    net_checksum_calculate(buf, len, cic >= 2 ? CSUM_ALL : CSUM_IP);
    if (cic < 2) {
        return;
    }
    /* net_checksum_calculate() leaves ICMP alone; the MAC does it */
    if (len >= ETH_HLEN + VLAN_HLEN && lduw_be_p(buf + 12) == ETH_P_VLAN) {
        l2 += VLAN_HLEN;
    }
    if (len < l2 + 20 || lduw_be_p(buf + l2 - 2) != ETH_P_IP) {
        return;
    }
    ip = buf + l2;
    ihl = (ip[0] & 0xF) * 4;
    tot = lduw_be_p(ip + 2);
    if (ip[9] != 1 /* ICMP */ || l2 + tot > len || tot < ihl + 4 ||
        (lduw_be_p(ip + 6) & 0x3FFF)) {
        return;
    }
    stw_be_p(ip + ihl + 2, 0);
    stw_be_p(ip + ihl + 2, net_raw_checksum(ip + ihl, tot - ihl));
}

static void tx_process(Tc4xGethState *s, int ch)
{
    Tc4xGethDma *dma = &s->dma[ch];
    uint32_t tail = chreg(s, ch, CH_TXDESC_TAIL);
    uint32_t ring = CH_RING_LEN(chreg(s, ch, CH_TX_CONTROL2));
    uint32_t status = 0;
    uint32_t cic = 0;

    if (!(chreg(s, ch, CH_TX_CONTROL) & CH_TX_ST)) {
        return;
    }
    for (uint32_t n = 0; n < ring && dma->tx_cur != tail; n++) {
        uint32_t d[4];

        desc_load(dma->tx_cur, d);
        if (!(d[3] & DES3_OWN)) {
            status |= ST_TBU;
            break;
        }
        if (d[3] & DES3_CTXT) {
            dma->tx_pktid_valid = d[3] & TDES3_PIDV;
            dma->tx_pktid = d[0] & 0x3FF;
            d[3] = DES3_CTXT;
            desc_store(dma->tx_cur, d);
        } else {
            uint32_t l1 = extract32(d[2], 0, 14);
            uint32_t l2 = extract32(d[2], 16, 14);

            if (d[3] & DES3_FD) {
                dma->txlen = 0;
                cic = TDES3_CIC(d[3]);
            }
            l1 = MIN(l1, MAX_FRAME - dma->txlen);
            dma_rd(d[0], dma->txbuf + dma->txlen, l1);
            dma->txlen += l1;
            l2 = MIN(l2, MAX_FRAME - dma->txlen);
            dma_rd(d[1], dma->txbuf + dma->txlen, l2);
            dma->txlen += l2;
            chset(s, ch, CH_CUR_TXBUF, d[0]);

            if (d[3] & DES3_LD) {
                int p = tx_port_of_channel(s, ch);

                if (cic) {
                    tx_checksum(dma->txbuf, dma->txlen, cic);
                }
                if (d[2] & TDES2_TTSE) {
                    uint32_t sec, nsec;

                    ptp_split(ptp_now(&s->port[p]), &sec, &nsec);
                    if (dma->tx_pktid_valid) {
                        txts_push(s, p, dma->tx_pktid | (ch << 5), sec, nsec);
                    }
                    d[0] = nsec;
                    d[1] = sec;
                    d[3] |= TDES3_TTSS;
                }
                dma->tx_pktid_valid = false;
                port_send(s, p, dma->txbuf, dma->txlen);
                dma->txlen = 0;
            }
            if (d[2] & TDES2_IOC) {
                status |= ST_TI;
            }
            d[3] &= ~DES3_OWN;
            desc_store(dma->tx_cur, d);
        }
        dma->tx_cur = desc_next(s, ch, dma->tx_cur, true);
    }
    chset(s, ch, CH_CUR_TXDESC, dma->tx_cur);
    if (status) {
        ch_status(s, ch, status);
    }
}

static bool rx_mac_accept(Tc4xGethState *s, int p, const uint8_t *buf)
{
    uint32_t pf = preg(s, p, MAC_PACKET_FILTER);

    if (pf & (PF_PR | PF_RA)) {
        return true;
    }
    if (is_broadcast_ether_addr(buf)) {
        return !(pf & PF_DBF);
    }
    if (is_multicast_ether_addr(buf)) {
        return true;                    /* hash filter not modelled */
    }
    for (int n = 0; n < MAC_ADDRESS_COUNT; n++) {
        uint32_t hi = preg(s, p, MAC_ADDRESS_HIGH(n));
        uint32_t lo = preg(s, p, MAC_ADDRESS_LOW(n));

        if ((n == 0 || (hi & MAC_ADDR_AE)) &&
            ldl_le_p(buf) == lo && lduw_le_p(buf + 4) == (hi & 0xFFFF)) {
            return true;
        }
    }
    return false;
}

/* Receive queue (RxC) of a frame on port @p */
static int rx_queue(Tc4xGethState *s, int p, const uint8_t *buf,
                    unsigned len)
{
    uint32_t en = preg(s, p, MAC_RXQ_CTRL0);
    uint32_t ctrl1 = preg(s, p, MAC_RXQ_CTRL1);
    int q = extract32(ctrl1, 0, 4);                       /* UPQ */
    bool mcbc = (ctrl1 & RXQ_MCBCQEN) && is_multicast_ether_addr(buf);

    if (mcbc && (ctrl1 & RXQ_OMCBCQ)) {
        return extract32(ctrl1, 8, 4);                    /* MCBCQ */
    }
    if (len >= ETH_HLEN + VLAN_HLEN && lduw_be_p(buf + 12) == ETH_P_VLAN) {
        uint32_t pcp = buf[14] >> 5;

        for (int i = 0; i < 8; i++) {
            uint32_t psrq = extract32(preg(s, p, i < 4 ? MAC_RXQ_CTRL2 :
                                                       MAC_RXQ_CTRL3),
                                      (i % 4) * 8, 8);

            if (extract32(en, i * 2, 2) && (psrq & BIT(pcp))) {
                return i;
            }
        }
    }
    return mcbc ? extract32(ctrl1, 8, 4) : q;
}

static int rx_channel(Tc4xGethState *s, int p, int q)
{
    return extract32(preg(s, p, q < 4 ? MTL_RXQ_DMA_MAP0 : MTL_RXQ_DMA_MAP1),
                     (q % 4) * 8, 3);
}

/*
 * Port the bridge forwards RxC @q of port @p to: a TxQ of the other port
 * that is enabled, marked a forwarding path and mapped to that RxC, in
 * two-port mode with TxQ/RxC mapping; -1 for the host's DMA
 */
static int rx_forward_port(Tc4xGethState *s, int p, int q)
{
    int e = !p;
    uint32_t ctrl = greg(s, BRIDGE_PORT_CONTROL(e));
    uint32_t fwd = extract32(ctrl, BRIDGE_TXQ_EN_SHIFT, 8) &
                   extract32(ctrl, BRIDGE_FWD_EN_SHIFT, 8);
    uint32_t map = greg(s, BRIDGE_TXQ_MAP(e));

    if (!(greg(s, BRIDGE_FORWARD_CONTROL) & FWD_Q_CH_MAPPING_EN)) {
        return -1;
    }
    for (int t = 0; t < 8; t++) {
        if ((fwd & BIT(t)) && extract32(map, t * 4, 4) == q) {
            return e;
        }
    }
    return -1;
}

/* Returns false when the frame must wait for descriptors */
static bool rx_deliver(Tc4xGethState *s, int p, int ch, const uint8_t *buf,
                       unsigned len)
{
    Tc4xGethDma *dma = &s->dma[ch];
    uint32_t bufsz = CH_RX_RBSZ(chreg(s, ch, CH_RX_CONTROL)) & ~3u;
    uint32_t tsctl = preg(s, p, MAC_TIMESTAMP_CONTROL);
    bool ts = (tsctl & TS_TSENA) && (tsctl & TS_TSENALL);
    unsigned need, off = 0;
    uint32_t cur, d[4], status = 0;

    if (!(chreg(s, ch, CH_RX_CONTROL) & CH_RX_SR) || bufsz == 0 ||
        chreg(s, ch, CH_RXDESC_LIST) == 0) {
        return true;                    /* DMA stopped: frame is lost */
    }
    /* All descriptors of the frame must be available before writing any */
    need = DIV_ROUND_UP(len, bufsz) + (ts ? 1 : 0);
    cur = dma->rx_cur;
    for (unsigned i = 0; i < need; i++) {
        desc_load(cur, d);
        if (!(d[3] & DES3_OWN)) {
            ch_status(s, ch, ST_RBU);
            return false;
        }
        cur = desc_next(s, ch, cur, false);
    }

    while (off < len) {
        unsigned chunk = MIN(bufsz, len - off);
        uint32_t wb3;

        desc_load(dma->rx_cur, d);
        dma_wr(d[0], buf + off, chunk);
        chset(s, ch, CH_CUR_RXBUF, d[0]);
        if (d[3] & RDES3_IOC) {
            status |= ST_RI;
        }
        wb3 = (off == 0 ? DES3_FD : 0);
        off += chunk;
        if (off == len) {
            wb3 |= DES3_LD | (len & 0x7FFF);
            if (ts) {
                wb3 |= RDES3_RS1V;
            }
        } else {
            wb3 |= chunk & 0x7FFF;
        }
        d[0] = 0;
        d[1] = (off == len && ts) ? RDES1_TSA : 0;
        d[2] = 0;
        d[3] = wb3;
        desc_store(dma->rx_cur, d);
        dma->rx_cur = desc_next(s, ch, dma->rx_cur, false);
    }
    if (ts) {
        uint32_t sec, nsec;

        ptp_split(ptp_now(&s->port[p]), &sec, &nsec);
        d[0] = nsec;
        d[1] = sec;
        d[2] = 0;
        d[3] = DES3_CTXT | RDES3_CTX_TSA;
        desc_store(dma->rx_cur, d);
        dma->rx_cur = desc_next(s, ch, dma->rx_cur, false);
    }
    chset(s, ch, CH_CUR_RXDESC, dma->rx_cur);
    if (status) {
        ch_status(s, ch, status);
    }
    return true;
}

static bool geth_can_receive(NetClientState *nc)
{
    return true;
}

static ssize_t geth_receive(NetClientState *nc, const uint8_t *data,
                            size_t size)
{
    Tc4xGethPort *port = qemu_get_nic_opaque(nc);
    Tc4xGethState *s = port->s;
    int p = port->index;
    uint8_t buf[MAX_FRAME];
    unsigned len = size;
    int q, fwd;

    if ((greg(s, R_CLC) & CLC_DISR) ||
        !(preg(s, p, MAC_RX_CONFIGURATION) & MAC_RE) ||
        size < ETH_HLEN || size > MAX_FRAME) {
        return size;
    }
    memcpy(buf, data, size);
    if (len < MIN_FRAME) {
        memset(buf + len, 0, MIN_FRAME - len);
        len = MIN_FRAME;
    }
    if (!rx_mac_accept(s, p, buf)) {
        return size;
    }
    q = rx_queue(s, p, buf, len);
    fwd = rx_forward_port(s, p, q);
    if (fwd >= 0) {
        port_send(s, fwd, buf, len);
    } else if (!rx_deliver(s, p, rx_channel(s, p, q), buf, len)) {
        return 0;                       /* queued; flushed on tail write */
    }
    mmc_inc(s, p, MMC_RX_PACKETS_GB, 1);
    mmc_inc(s, p, MMC_RX_OCTETS_GB, len);
    mmc_inc(s, p, MMC_RX_OCTETS_G, len);
    mmc_inc(s, p, is_broadcast_ether_addr(buf) ? MMC_RX_BROADCAST :
                  is_multicast_ether_addr(buf) ? MMC_RX_MULTICAST :
                  MMC_RX_UNICAST, 1);
    return size;
}

static void rx_kick(Tc4xGethState *s)
{
    for (int p = 0; p < TC4X_GETH_NUM_PORTS; p++) {
        if (s->port[p].nic) {
            qemu_flush_queued_packets(qemu_get_queue(s->port[p].nic));
        }
    }
}

static void geth_core_reset(Tc4xGethState *s)
{
    memset(&s->regs[CORE_BASE / 4], 0, TC4X_GETH_SIZE - CORE_BASE);
    for (int p = 0; p < TC4X_GETH_NUM_PORTS; p++) {
        uint32_t base = PORT_BASE(p);

        sreg(s, base + MAC_VERSION, 0x00003052u);
        /* GMII/RGMII, TS, TX/RX COE, 32 extra MAC addresses */
        sreg(s, base + MAC_HW_FEATURE0, 0x087C5003u | (31u << 18));
        sreg(s, base + MAC_HW_FEATURE1, 0x00000000u | (2u << 14));
        sreg(s, base + MAC_HW_FEATURE2, 0x07070707u);
        sreg(s, base + MAC_ADDRESS_HIGH(0), 0x8000FFFFu);
        sreg(s, base + MAC_ADDRESS_LOW(0), 0xFFFFFFFFu);
        qatomic_set(&s->port[p].txts_count, 0);
    }
    for (int ch = 0; ch < TC4X_GETH_NUM_DMA; ch++) {
        s->dma[ch].tx_cur = s->dma[ch].rx_cur = 0;
        s->dma[ch].txlen = 0;
        s->dma[ch].tx_pktid_valid = false;
    }
    geth_update_irq(s);
}

static void geth_reset_hold(Object *obj, ResetType type)
{
    Tc4xGethState *s = TC4X_GETH(obj);

    memset(s->regs, 0, sizeof(s->regs));
    sreg(s, R_CLC, CLC_DISR | CLC_DISS);
    g_hash_table_remove_all(s->hsphy);
    for (int p = 0; p < TC4X_GETH_NUM_PORTS; p++) {
        Tc4xGethPort *port = &s->port[p];

        if (port->phy && port->phy->reset) {
            port->phy->reset(port->phy_opaque);
        }
        port->ptp_offset_ns = 0;
    }
    geth_core_reset(s);
}

/* TX timestamp FIFO fill level */
static uint32_t timestamp_status(Tc4xGethState *s, int p)
{
    unsigned count = qatomic_read(&s->port[p].txts_count);

    return deposit32(preg(s, p, MAC_TIMESTAMP_STATUS), TS_STATUS_TTSNS_SHIFT,
                     5, count) | (count ? TS_STATUS_TXTSC : 0);
}

static uint32_t geth_read_word(Tc4xGethState *s, uint32_t off)
{
    uint32_t v = greg(s, off);

    if (off == R_CLC) {
        return (v & ~CLC_DISS) | ((v & CLC_DISR) ? CLC_DISS : 0);
    }
    if (off >= PORT_BASE(0) && off < PORT_BASE(TC4X_GETH_NUM_PORTS)) {
        int p = PORT_OF(off);
        uint32_t r = off - PORT_BASE(p);
        uint32_t sec, nsec;

        switch (r) {
        case MAC_SYSTIME_SEC:
        case MAC_SYSTIME_NSEC:
            ptp_split(ptp_now(&s->port[p]), &sec, &nsec);
            return r == MAC_SYSTIME_SEC ? sec : nsec;
        case MAC_SYSTIME_HI_SEC:
            return 0;
        case MAC_TIMESTAMP_STATUS:
            return timestamp_status(s, p);
        case MAC_TXTS_NSEC:
        case MAC_TXTS_SEC:
        case MAC_TXTS_PKTID:
            return txts_read(s, p, r);
        case MAC_INTERRUPT_STATUS:
            /* TSIS reflects a non-empty TX timestamp FIFO */
            v = s->port[p].txts_count ? (v | MAC_INT_TSIS) :
                                        (v & ~MAC_INT_TSIS);
            sreg(s, off, v);
            return v;
        }
    }
    return v;
}

static void geth_dma_ch_write(Tc4xGethState *s, int ch, uint32_t r,
                              uint32_t val)
{
    Tc4xGethDma *dma = &s->dma[ch];

    switch (r) {
    case CH_STATUS:
        val = chreg(s, ch, CH_STATUS) & ~val;
        if (!(val & (ST_TI | ST_RI))) {
            val &= ~ST_NIS;
        }
        if (!(val & (ST_TBU | ST_RBU))) {
            val &= ~ST_AIS;
        }
        chset(s, ch, r, val);
        geth_update_irq(s);
        return;
    case CH_CUR_TXDESC:
    case CH_CUR_RXDESC:
    case CH_CUR_TXBUF:
    case CH_CUR_RXBUF:
    case CH_MISS_PACKET_CNT:
        return;                         /* read-only */
    }
    chset(s, ch, r, val);
    switch (r) {
    case CH_TXDESC_LIST:
        dma->tx_cur = val;
        chset(s, ch, CH_CUR_TXDESC, val);
        break;
    case CH_RXDESC_LIST:
        dma->rx_cur = val;
        chset(s, ch, CH_CUR_RXDESC, val);
        break;
    case CH_TX_CONTROL:
    case CH_TXDESC_TAIL:
        tx_process(s, ch);
        break;
    case CH_RXDESC_TAIL:
        /*
         * A tail pointer write resumes a suspended RX DMA and clears RBU.
         * RBU is set again if the frame waiting in the queue still finds
         * no free descriptor.
         */
        val = chreg(s, ch, CH_STATUS) & ~ST_RBU;
        chset(s, ch, CH_STATUS,
              (val & (ST_TBU | ST_RBU)) ? val : (val & ~ST_AIS));
        geth_update_irq(s);
        rx_kick(s);
        break;
    case CH_RX_CONTROL:
        rx_kick(s);
        break;
    case CH_INTERRUPT_ENABLE:
        geth_update_irq(s);
        break;
    }
}

static void geth_write_word(Tc4xGethState *s, uint32_t off, uint32_t val)
{
    if (off == R_RST_CTRLA || off == R_RST_CTRLB) {
        if (off == R_RST_CTRLB && (val & RST_CTRLB_STATCLR)) {
            sreg(s, R_RST_STAT, 0);
        }
        sreg(s, off, val & RST_KRST);
        if ((greg(s, R_RST_CTRLA) & RST_KRST) &&
            (greg(s, R_RST_CTRLB) & RST_KRST)) {
            geth_core_reset(s);
            sreg(s, R_RST_CTRLA, 0);
            sreg(s, R_RST_CTRLB, 0);
            sreg(s, R_RST_STAT, RST_KRST);
        }
        return;
    }
    if (off == R_RST_STAT || off == DMA_INTERRUPT_STATUS) {
        return;
    }
    if (off == DMA_MODE) {
        if (val & DMA_MODE_SWR) {
            geth_core_reset(s);
        }
        sreg(s, off, val & ~DMA_MODE_SWR);
        return;
    }
    if (off >= DMA_CH_BASE && off < DMA_CH_END) {
        geth_dma_ch_write(s, (off - DMA_CH_BASE) / DMA_CH_STRIDE,
                          (off - DMA_CH_BASE) % DMA_CH_STRIDE, val);
        return;
    }
    if (off >= PORT_BASE(0) && off < PORT_BASE(TC4X_GETH_NUM_PORTS)) {
        int p = PORT_OF(off);
        uint32_t r = off - PORT_BASE(p);

        switch (r) {
        case MDIO_DATA:
            sreg(s, off, val);
            if (val & MDIO_SBUSY) {
                mdio_command(s, p, val);
            }
            return;
        case MAC_VLAN_TAG_CTRL:
            sreg(s, off, val & ~BIT(0));        /* OB: busy self-clears */
            return;
        case MAC_TIMESTAMP_CONTROL:
            ptp_control_write(s, p, val);
            return;
        case MAC_TIMESTAMP_STATUS:
        case MAC_TXTS_NSEC:
        case MAC_TXTS_SEC:
        case MAC_TXTS_PKTID:
        case MAC_VERSION:
        case MAC_HW_FEATURE0:
        case MAC_HW_FEATURE1:
        case MAC_HW_FEATURE2:
        case MAC_INTERRUPT_STATUS:
            return;
        case MAC_INTERRUPT_ENABLE:
            sreg(s, off, val);
            geth_update_irq(s);
            return;
        case MAC_RX_CONFIGURATION:
            sreg(s, off, val);
            rx_kick(s);
            return;
        }
        if (r >= MTL_TCQ_BASE && r < MTL_TCQ_END &&
            (r - MTL_TCQ_BASE) % MTL_TCQ_STRIDE == 0) {
            val &= ~BIT(0);                     /* FTQ: flush completes */
        }
    }
    sreg(s, off, val);
}

/*
 * Reads of registers without side effects (all but CLC and the MAC port
 * registers) are served without the BQL, so busy-polling the DMA channel
 * status does not contend for it.
 */
static bool geth_read_is_plain(uint32_t off)
{
    return off != R_CLC &&
           (off < PORT_BASE(0) || off >= PORT_BASE(TC4X_GETH_NUM_PORTS));
}

/* TX timestamp status reads are served without the BQL too */
static bool geth_read_is_ts_status(uint32_t off, int *p)
{
    if (off < PORT_BASE(0) || off >= PORT_BASE(TC4X_GETH_NUM_PORTS) ||
        off - PORT_BASE(PORT_OF(off)) != MAC_TIMESTAMP_STATUS) {
        return false;
    }
    *p = PORT_OF(off);
    return true;
}

static uint64_t geth_read(void *opaque, hwaddr addr, unsigned size)
{
    Tc4xGethState *s = opaque;
    uint32_t off = addr & ~3u;
    uint32_t word;
    int p;

    if (geth_read_is_plain(off)) {
        word = greg(s, off);
    } else if (geth_read_is_ts_status(off, &p)) {
        word = timestamp_status(s, p);
    } else {
        BQL_LOCK_GUARD();
        word = geth_read_word(s, off);
    }
    return extract32(word, (addr & 3) * 8, size * 8);
}

static void geth_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Tc4xGethState *s = opaque;
    uint32_t off = addr & ~3u;
    uint32_t word = val;

    BQL_LOCK_GUARD();
    if (size < 4) {
        /* the bytes not written clear no channel status bits */
        bool w1c = off >= DMA_CH_BASE && off < DMA_CH_END &&
                   (off - DMA_CH_BASE) % DMA_CH_STRIDE == CH_STATUS;

        word = deposit32(w1c ? 0 : greg(s, off), (addr & 3) * 8, size * 8,
                         val);
    }
    geth_write_word(s, off, word);
}

static const MemoryRegionOps geth_ops = {
    .read = geth_read,
    .write = geth_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static uint32_t hs_get(Tc4xGethState *s, uint32_t off)
{
    return GPOINTER_TO_UINT(g_hash_table_lookup(s->hsphy,
                                                GUINT_TO_POINTER(off)));
}

static void hs_set(Tc4xGethState *s, uint32_t off, uint32_t v)
{
    g_hash_table_insert(s->hsphy, GUINT_TO_POINTER(off), GUINT_TO_POINTER(v));
}

static uint32_t hsphy_read_word(Tc4xGethState *s, uint32_t off)
{
    uint32_t v = hs_get(s, off);

    if (off == HS_PHY_CTRL1(0) || off == HS_PHY_CTRL1(1) ||
        off == HS_PHY_CTRL1(2)) {
        return v | HS_PHY_INITDONE;
    }
    if (off == HS_DLL_CFG) {
        return v | HS_DLL_LOCKSTAT;
    }
    if (off >= HS_XPCS_FIRST && off < HS_XPCS_FIRST + 3 * HS_XPCS_STRIDE) {
        uint32_t r = (off - HS_XPCS_FIRST) % HS_XPCS_STRIDE;
        uint32_t base = off - r;

        if (r == HS_PCS_VR_DIG_STS) {
            bool lpm = hs_get(s, base + HS_PCS_SR_XS_CTRL1) & HS_SR_LPM;

            return deposit32(v, HS_PSEQ_SHIFT, 3,
                             lpm ? HS_PSEQ_LPM : HS_PSEQ_NORMAL);
        }
    }
    return v;
}

static void hsphy_write_word(Tc4xGethState *s, uint32_t off, uint32_t val)
{
    if (off == HS_PHYREG_CTRL(0) || off == HS_PHYREG_CTRL(1) ||
        off == HS_PHYREG_CTRL(2)) {
        if ((val & HS_PHYREG_ACT) && (val & HS_PHYREG_READ)) {
            /* on-chip HSPHY register read: report calibration done */
            hs_set(s, off + 4, HS_CMNCAL_DONE);
        }
        val &= ~HS_PHYREG_ACT;
    } else if (off >= HS_XPCS_FIRST &&
               off < HS_XPCS_FIRST + 3 * HS_XPCS_STRIDE) {
        uint32_t r = (off - HS_XPCS_FIRST) % HS_XPCS_STRIDE;

        if (r == HS_PMA_TX_GENCTRL2 || r == HS_PMA_RX_GENCTRL2) {
            val &= ~HS_GENCTRL2_REQ;
        } else if (r == HS_PCS_VR_DIG_CTRL1) {
            val &= ~HS_DIG_CTRL1_SELFCLR;
        } else if (r == HS_PCS_SR_XS_CTRL1) {
            val &= ~HS_SR_RST;
        }
    }
    hs_set(s, off, val);
}

static uint64_t hsphy_read(void *opaque, hwaddr addr, unsigned size)
{
    uint32_t word = hsphy_read_word(opaque, addr & ~3u);

    return extract32(word, (addr & 3) * 8, size * 8);
}

static void hsphy_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned size)
{
    Tc4xGethState *s = opaque;
    uint32_t off = addr & ~3u;
    uint32_t word = val;

    if (size < 4) {
        word = deposit32(hs_get(s, off), (addr & 3) * 8, size * 8, val);
    }
    hsphy_write_word(s, off, word);
}

static const MemoryRegionOps hsphy_ops = {
    .read = hsphy_read,
    .write = hsphy_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static NetClientInfo geth_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = geth_can_receive,
    .receive = geth_receive,
};

static void geth_realize(DeviceState *dev, Error **errp)
{
    Tc4xGethState *s = TC4X_GETH(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->geth_mr, OBJECT(s), &geth_ops, s, "geth0",
                          TC4X_GETH_SIZE);
    memory_region_enable_lockless_io(&s->geth_mr);
    sysbus_init_mmio(sbd, &s->geth_mr);
    memory_region_init_io(&s->hsphy_mr, OBJECT(s), &hsphy_ops, s, "hsphy",
                          TC4X_HSPHY_SIZE);
    sysbus_init_mmio(sbd, &s->hsphy_mr);
    for (int i = 0; i < TC4X_GETH_NUM_IRQ; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }

    s->hsphy = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (int p = 0; p < TC4X_GETH_NUM_PORTS; p++) {
        Tc4xGethPort *port = &s->port[p];
        g_autofree char *id = g_strdup_printf("%s.port%d",
                                              dev->id ? dev->id : "geth0", p);

        port->s = s;
        port->index = p;
        if (p > 0 && !port->conf.peers.ncs[0]) {
            continue;                   /* unused extra port: no NIC */
        }
        qemu_macaddr_default_if_unset(&port->conf.macaddr);
        port->nic = qemu_new_nic(&geth_net_info, &port->conf,
                                 object_get_typename(OBJECT(dev)), id,
                                 &dev->mem_reentrancy_guard, port);
        qemu_format_nic_info_str(qemu_get_queue(port->nic),
                                 port->conf.macaddr.a);
    }
}

static const Property geth_properties[] = {
    DEFINE_NIC_PROPERTIES(Tc4xGethState, port[0].conf),
    DEFINE_PROP_MACADDR("mac1", Tc4xGethState, port[1].conf.macaddr),
    DEFINE_PROP_NETDEV("netdev1", Tc4xGethState, port[1].conf.peers),
};

static void geth_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "TC4x GETH Ethernet controller";
    dc->realize = geth_realize;
    rc->phases.hold = geth_reset_hold;
    device_class_set_props(dc, geth_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo geth_types[] = {
    {
        .name = TYPE_TC4X_GETH,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Tc4xGethState),
        .class_init = geth_class_init,
    },
};

DEFINE_TYPES(geth_types)

void tc4x_geth_attach_phy(DeviceState *dev, unsigned port, uint8_t addr,
                          const Tc4xGethPhyOps *ops, void *opaque)
{
    Tc4xGethState *s = TC4X_GETH(dev);
    Tc4xGethPort *p;

    assert(port < TC4X_GETH_NUM_PORTS);
    p = &s->port[port];
    p->phy_addr = addr;
    p->phy = ops;
    p->phy_opaque = opaque;
}

DeviceState *tc4x_geth_create(MemoryRegion *sysmem)
{
    DeviceState *dev = qdev_new(TYPE_TC4X_GETH);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    NICInfo *nd;

    qemu_configure_nic_device(dev, true, NULL);
    nd = qemu_find_nic_info(TC4X_GETH_PORT1_MODEL, false, NULL);
    if (nd) {
        qdev_prop_set_macaddr(dev, "mac1", nd->macaddr.a);
        if (nd->netdev) {
            qdev_prop_set_netdev(dev, "netdev1", nd->netdev);
        }
        nd->instantiated = 1;
    }
    sysbus_realize_and_unref(sbd, &error_fatal);
    memory_region_add_subregion_overlap(sysmem, TC4X_GETH_BASE,
                                        sysbus_mmio_get_region(sbd, 0), 1);
    memory_region_add_subregion_overlap(sysmem, TC4X_HSPHY_BASE,
                                        sysbus_mmio_get_region(sbd, 1), 1);
    return dev;
}
