/*
 * Realtek rtl8363 DSA Switch driver
 * Copyright (C) 2017 Sean Wang <sean.wang@mediatek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/etherdevice.h>
#include <linux/if_bridge.h>
#include <linux/iopoll.h>
#include <linux/mdio.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of_gpio.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/of_platform.h>
#include <linux/phy.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/gpio/consumer.h>
#include <net/dsa.h>

#define REALTEK_8363NB_ID	25447

/* basic register operations */
#define MDC_MDIO_CTRL0_REG	31
#define MDC_MDIO_START_REG	29
#define MDC_MDIO_CTRL1_REG	21
#define MDC_MDIO_ADDRESS_REG	23
#define MDC_MDIO_DATA_WRITE_REG	24
#define MDC_MDIO_DATA_READ_REG	25

#define MDC_MDIO_PREAMBLE_LEN	32 /* NOOOO !!! */

#define MDC_MDIO_START_OP	0xFFFF
#define MDC_MDIO_ADDR_OP	0x000E
#define MDC_MDIO_READ_OP	0x0001
#define MDC_MDIO_WRITE_OP	0x0003

/* virtual registers */
#define RTL8367B_RTL_MAGIC_ID_REG	0x13c2
#define RTL8367B_RTL_MAGIC_ID_VAL	0x0249

#define RTL8367B_CHIP_NUMBER_REG	0x1300
#define RTL8367B_CHIP_VER_REG		0x1301
#define RTL8367B_CHIP_MODE_REG		0x1302

/* rgmi */

#define RTL8367C_REG_MAC0_FORCE_SELECT 0x1312

#define RTL8367C_REG_BYPASS_LINE_RATE    0x03f7
#define RTL8367C_REG_DIGITAL_INTERFACE0_FORCE    0x1310
#define RTL8367C_REG_DIGITAL_INTERFACE_SELECT_1    0x13c3
#define RTL8367C_SELECT_GMII_0_MASK    0xF
#define RTL8367C_SELECT_GMII_1_OFFSET    4
#define RTL8367C_REG_SDS_MISC    0x1d11
#define RTL8367C_REG_REG_TO_ECO4    0x1d41

enum LINKMODE {
MAC_NORMAL = 0,
MAC_FORCE,
};

enum PORTMODE {
    MODE_EXT_DISABLE = 0,
    MODE_EXT_RGMII,
    MODE_EXT_MII_MAC,
    MODE_EXT_MII_PHY,
    MODE_EXT_TMII_MAC,
    MODE_EXT_TMII_PHY,
    MODE_EXT_GMII,
    MODE_EXT_RMII_MAC,
    MODE_EXT_RMII_PHY,
    MODE_EXT_SGMII,
    MODE_EXT_HSGMII,
    MODE_EXT_1000X_100FX,
    MODE_EXT_1000X,
    MODE_EXT_100FX,
    MODE_EXT_RGMII_2,
    MODE_EXT_MII_MAC_2,
    MODE_EXT_MII_PHY_2,
    MODE_EXT_TMII_MAC_2,
    MODE_EXT_TMII_PHY_2,
    MODE_EXT_RMII_MAC_2,
    MODE_EXT_RMII_PHY_2,
    MODE_EXT_END
};

enum SPEEDMODE
{
    SPD_10M = 0,
    SPD_100M,
    SPD_1000M,
    SPD_2500M
};

enum DUPLEXMODE
{
    HALF_DUPLEX = 0,
    FULL_DUPLEX
};

enum PORTLINK
{
    PORT_LINKDOWN = 0,
    PORT_LINKUP,
    PORT_LINKSTATUS_END
};


struct rtl8363_priv {
	struct dsa_switch *ds;
	struct mii_bus *bus;
	struct gpio_desc *reset;
	struct device *dev;
};

/*
struct regval {
	u16 action;
	u16 reg;
	u16 val;
};

static const struct regval rtl8363nb_initdata[] = {
	{MDC_MDIO_READ_OP, },
	{MDC_MDIO_WRITE_OP, },
};

*/

/* String, offset, and register size in bytes if different from 4 bytes */
//static const struct rtl8363_mib_desc rtl8363_mib[] = {
//	MIB_DESC(1, 0x00, "TxDrop"),
//};


static int rtl8363_write_reg(struct rtl8363_priv *priv, u32 reg, u32 value) {
        struct mii_bus *bus = priv->bus;
	int ret;

	ret = bus->write(bus, 0, MDC_MDIO_CTRL0_REG, MDC_MDIO_ADDR_OP);
	if(ret)
		return ret;

	ret = bus->write(bus, 0, MDC_MDIO_ADDRESS_REG, reg);
	if(ret)
		return ret;

	ret = bus->write(bus, 0, MDC_MDIO_DATA_WRITE_REG, value);
	if(ret)
		return ret;

	return bus->write(bus, 0, MDC_MDIO_CTRL1_REG, MDC_MDIO_WRITE_OP);
}

static int rtl8363_read_reg(struct rtl8363_priv *priv, u32 reg, u32 *value)
{
        struct mii_bus *bus = priv->bus;
        int ret;

	ret = bus->write(bus, 0, MDC_MDIO_CTRL0_REG, MDC_MDIO_ADDR_OP);
	if(ret)
		return ret;

	ret = bus->write(bus, 0, MDC_MDIO_ADDRESS_REG, reg);
	if(ret)
		return ret;

	ret = bus->write(bus, 0, MDC_MDIO_CTRL1_REG, MDC_MDIO_READ_OP);
	if(ret)
		return ret;

	*value = bus->read(bus, 0, MDC_MDIO_DATA_READ_REG);
	return ret;
}

static int rtl8363_setbit_reg(struct rtl8363_priv *priv, u32 reg, u32 bit, u32 on) {
	int ret;
	int temp;

	ret = rtl8363_read_reg(priv, reg, &temp);
	if(ret)
		return ret;

	if(on)
		temp = temp | (1 << bit);
	else
		temp = temp & (~(1 << bit));

	ret = rtl8363_write_reg(priv, reg, temp);
	return ret;
}

static int rtl8363_setbits_reg(struct rtl8363_priv *priv, u32 reg, u32 mask, u32 regbits) {
	int ret;
	u32 temp;
	u32 valshift;
	u32 maskshift = 0;

	ret = rtl8363_read_reg(priv, reg, &temp);
	if(ret)
		return ret;

	/* Why shift? */
	while(!( mask & (1 << maskshift ) ) ) {
		maskshift++;
//		dev_err(priv->dev, "Maskshift !\n");
		if (maskshift >= 16) {
			dev_err(priv->dev, "Maskshift more than 16\n");
			return -EINVAL;
		}
        }
	valshift = regbits << maskshift;

	temp = temp & (~mask);
	temp = temp | (valshift & mask);

	return rtl8363_write_reg(priv, reg, temp);
}

#define	RTL8367C_REG_GPHY_OCP_MSB_0    0x1d15
#define	RTL8367C_CFG_CPU_OCPADR_MSB_MASK    0xFC0
#define RTL8367C_PHY_BASE 0x2000
#define RTL8367C_PHY_OFFSET 5

static int rtl8363_phy_read_reg(struct rtl8363_priv *priv, u32 port, u32 reg, u32 *value) {
	int ret;
	u32 phy_reg;
	u32 ocpaddr1;
	u32 ocpaddr2;

	ret = rtl8363_setbits_reg(priv, RTL8367C_REG_GPHY_OCP_MSB_0, RTL8367C_CFG_CPU_OCPADR_MSB_MASK, ((reg & 0xFC00) >> 10));
	if(ret)
		return ret;

	ocpaddr2 = ((reg >> 6) & 0x000F);
	ocpaddr1 = ((reg >> 1) & 0x001F);

	phy_reg =  RTL8367C_PHY_BASE | (ocpaddr2 << 8) | (port << RTL8367C_PHY_OFFSET) | ocpaddr1;
	return rtl8363_read_reg(priv, phy_reg, value);
}


static int rtl8363_phy_write_reg(struct rtl8363_priv *priv, u32 port, u32 reg, u32 value) {
	int ret;
	u32 phy_reg;
	u32 ocpaddr1;
	u32 ocpaddr2;

	ret = rtl8363_setbits_reg(priv, RTL8367C_REG_GPHY_OCP_MSB_0, RTL8367C_CFG_CPU_OCPADR_MSB_MASK, ((reg & 0xFC00) >> 10));
	if(ret)
		return ret;

	ocpaddr2 = ((reg >> 6) & 0x000F);
	ocpaddr1 = ((reg >> 1) & 0x001F);

	phy_reg =  RTL8367C_PHY_BASE | (ocpaddr2 << 8) | (port << RTL8367C_PHY_OFFSET) | ocpaddr1;
	return rtl8363_write_reg(priv, phy_reg, value);
}

static int rtl8363_detect(struct rtl8363_priv *priv) {
	int ret, val1, val2;
	struct device *dev = priv->dev;

	ret = rtl8363_write_reg(priv, RTL8367B_RTL_MAGIC_ID_REG, RTL8367B_RTL_MAGIC_ID_VAL);
	if(ret)
		dev_err(dev, "Fail 1: %d\n", ret);

	ret = rtl8363_read_reg(priv, RTL8367B_CHIP_NUMBER_REG, &val1);
	if(ret)
		dev_err(dev, "Fail 2: %d\n", ret);

	ret = rtl8363_read_reg(priv, RTL8367B_CHIP_VER_REG, &val2);
	if(ret)
		dev_err(dev, "Fail 3: %d\n", ret);

	ret = rtl8363_write_reg(priv, RTL8367B_RTL_MAGIC_ID_REG, 0);
	if(ret)
		dev_err(dev, "Fail 4: %d\n", ret);


	if(val1 == REALTEK_8363NB_ID) {
		dev_err(dev, "RTL8363NB switch detected. chip number: %d(0x%x), chip version: %d(0x%x)\n", val1, val1, val2, val2);
		return 0;
	}
	return -ENOENT;
}

static void rtl8363_reset(struct rtl8363_priv *priv) {
	gpiod_direction_output(priv->reset, 1);
	usleep_range(2000, 3000);
	gpiod_set_value(priv->reset, 0);
	msleep(1000);
}

static int rtl8363_real_phy_write(struct rtl8363_priv *priv, int phy, u32 reg, u32 value) {
	return rtl8363_phy_write_reg(priv, phy, (0xa400 + reg *2), value);
}

static int rtl8363_real_phy_read(struct rtl8363_priv *priv, int phy, u32 reg, u32 *value) {
	return rtl8363_phy_read_reg(priv, phy, (0xa400 + reg *2), value);
}

static int rtl8363_phy_read(struct dsa_switch *ds, int port, int regnum) {
	u32 value = 0;
	rtl8363_real_phy_read(ds->priv, port, regnum, &value);
//	dev_err(ds->dev, "%s, phy_read, port %d, reg: 0x%x, returned value: 0x%x\n", __func__ , port, regnum, value);
	return value;
}

static int rtl8363_phy_write(struct dsa_switch *ds, int port, int regnum, u16 val) {
//	dev_err(ds->dev, "%s, phy_write, port %d, reg: 0x%x val: 0x%x\n", __func__ , port, regnum, val);
	return rtl8363_real_phy_write(ds->priv, port, regnum, val);
}


static void rtl8363_dump(struct rtl8363_priv *priv, int port) {
	int ret;
	u32 val;
	int i;
	for(i = 0; i < 15; i++) {
		rtl8363_real_phy_read(priv, port, i, &val);
		dev_err(priv->dev, "dump port:%d reg:%d val 0x%x\n",port, i, val); 
	}
}
#define    RTL8367C_CFG_SGMII_FDUP_OFFSET    10
#define    RTL8367C_CFG_SGMII_SPD_MASK    0x180
#define    RTL8367C_CFG_SGMII_LINK_OFFSET    9
#define    RTL8367C_CFG_SGMII_TXFC_OFFSET    13
#define    RTL8367C_CFG_SGMII_RXFC_OFFSET    14

static void rtl8363_rgmii_enable(struct rtl8363_priv *priv) {
	int ret;
	int port_id = 1;

	int forcemode = 1;
	int speed = 2;
	int duplex = 1;
	int link = 1;
	int nway = 0;
	int tx_pause = 1;
	int rx_pause = 1;

	int val;

//    v24 = rtl8367c_setAsicReg(0x13C0u, 0x249u);
	ret = rtl8363_write_reg(priv, 0x13c0, 0x249);
//    v25 = rtl8367c_getAsicReg(0x13C1u, &v163);
	ret = rtl8363_read_reg(priv, 0x13c1, &val);
//    v26 = rtl8367c_setAsicReg(0x13C0u, 0);
	ret = rtl8363_write_reg(priv, 0x13c0, 0);

//    v28 = rtl8367c_setAsicRegBit(0x3F7u, v2, 1);
	ret = rtl8363_setbit_reg(priv, 0x3f7, port_id, 0);
	
//    v62 = rtl8367c_setAsicRegBit(0x1D11u, 6u, 0);
	ret = rtl8363_setbit_reg(priv, 0x1D11, 6, 0);
//    v63 = rtl8367c_setAsicRegBit(0x1D11u, 0xBu, 0);
	ret = rtl8363_setbit_reg(priv, 0x1D11, 0xb, 0);
//    v64 = rtl8367c_setAsicRegBits(0x1305u, 15 << 4 * v2, v3, v16);
	ret = rtl8363_setbits_reg(priv, 0x1305, 15 << 4 * port_id, 1);
//    v13 = rtl8367c_setAsicReg(0x6602u, 0x7106u);
	ret = rtl8363_write_reg(priv, 0x6602, 0x7106);
//    v13 = rtl8367c_setAsicReg(0x6601u, 3u);
	ret = rtl8363_write_reg(priv, 0x6601, 3);
//    v13 = rtl8367c_setAsicReg(0x6600u, 0xC0u);
	ret = rtl8363_write_reg(priv, 0x6600, 0xC0);

//	ret = rtl8363_setbit_reg(priv, RTL8367C_REG_BYPASS_LINE_RATE, port_id, 0);
//	ret = rtl8363_setbits_reg(priv, RTL8367C_REG_DIGITAL_INTERFACE_SELECT_1, RTL8367C_SELECT_GMII_0_MASK << (port_id * RTL8367C_SELECT_GMII_1_OFFSET), MODE_EXT_RGMII);

	// asic port force link ext

	ret = rtl8363_read_reg(priv, RTL8367C_REG_REG_TO_ECO4, &val);
	if( (val &(0x0001 << 5)) && (val & (0x0001 << 7)) ) {
		dev_err(priv->dev, "SHITTY BEAR\n");
	}

	ret = rtl8363_setbit_reg(priv, RTL8367C_REG_SDS_MISC, RTL8367C_CFG_SGMII_FDUP_OFFSET, duplex); //Full Duplex
	ret = rtl8363_setbits_reg(priv, RTL8367C_REG_SDS_MISC, RTL8367C_CFG_SGMII_SPD_MASK, speed); // 1G Speed
	ret = rtl8363_setbit_reg(priv, RTL8367C_REG_SDS_MISC, RTL8367C_CFG_SGMII_LINK_OFFSET, link); // PORT LINK UP!
	ret = rtl8363_setbit_reg(priv, RTL8367C_REG_SDS_MISC, RTL8367C_CFG_SGMII_TXFC_OFFSET, tx_pause); //Tx Pause
	ret = rtl8363_setbit_reg(priv, RTL8367C_REG_SDS_MISC, RTL8367C_CFG_SGMII_RXFC_OFFSET, rx_pause); //Rx Pause

	val = 0;
	val |= forcemode << 12; //forcemode - mac_force
	val |= 0 << 9; //mstfault
	val |= 0 << 8; //mstmode
	val |= nway << 7;
	val |= tx_pause << 6;
	val |= rx_pause << 5;
	val |= link << 4;
	val |= duplex << 2;
	val |= speed;

	ret = rtl8363_write_reg(priv, RTL8367C_REG_DIGITAL_INTERFACE0_FORCE + port_id, val);
	

//	ret = rtl8363_write_reg(priv, RTL8367C_REG_MAC0_FORCE_SELECT+6, val);

}

static int rtl8363_setup(struct dsa_switch *ds)
{
	struct rtl8363_priv *priv = ds->priv;
//	struct mii_bus *bus = priv->bus;
	struct device *dev = priv->dev;
	int ret = 0;
	int magic_reg = 0x18;
	u32 val;
	u32 regData;
	dev_err(dev, "Setup started\n");

	rtl8363_reset(priv);

	ret = rtl8363_detect(priv);
	if(ret)
		return ret;
//	return 0;
//// RTK SWITCH INIT
//	while(1);
	int port = 0;


	for(port = 0; port < 32; port++) {
		if(port != 1 && port != 3)
			continue;

		dev_err(dev, "Setting up port %d\n", port);
		ret = rtl8363_setbit_reg(priv, magic_reg+(port*0x20), 0xB, 1);
		ret = rtl8363_setbit_reg(priv, magic_reg+(port*0x20), 0xA, 1);
		ret = rtl8363_setbit_reg(priv, magic_reg+(port*0x20), 0x9, 1);
		ret = rtl8363_setbit_reg(priv, magic_reg+(port*0x20), 0x8, 1);

		u32 phy0;
		ret = rtl8363_phy_read_reg(priv, port, 0xA428, &phy0);
		phy0 &= 0xFFFFFDFF;
		ret = rtl8363_phy_write_reg(priv, port, 0xA428, phy0);

		ret = rtl8363_phy_read_reg(priv, port, 0xA5D0, &phy0);
		phy0 |= 0x6;
		ret = rtl8363_phy_write_reg(priv, port, 0xA5D0, phy0);
	}

//	return -EINVAL;

//endwhile

	// base config ?

	ret = rtl8363_write_reg(priv, 0x13EB, 0x15BB);
	ret = rtl8363_write_reg(priv, 0x1303, 0x6d6);
	ret = rtl8363_write_reg(priv, 0x1304, 0x700);
	ret = rtl8363_write_reg(priv, 0x13E2, 0x3F);
	ret = rtl8363_write_reg(priv, 0x13F9, 0x90);
	ret = rtl8363_write_reg(priv, 0x121E, 0x3CA);
	ret = rtl8363_write_reg(priv, 0x1233, 0x352);
	ret = rtl8363_write_reg(priv, 0x1237, 0xA0);
	ret = rtl8363_write_reg(priv, 0x123A, 0x30);
	ret = rtl8363_write_reg(priv, 0x1239, 0x84);
	ret = rtl8363_write_reg(priv, 0x301, 0x1000);
	ret = rtl8363_write_reg(priv, 0x1349, 0x1F);

	ret = rtl8363_setbit_reg(priv, 0x18E0, 0, 0);
	ret = rtl8363_setbit_reg(priv, 0x122B, 0xE, 1);
	ret = rtl8363_setbits_reg(priv, 0x1305, 0xC00, 3);

	//label7
	ret = rtl8363_write_reg(priv, 0x3FA, 0x7);
	ret = rtl8363_setbit_reg(priv, 0x1D32, 1, 1);

#define RTL8367C_REG_LUT_CFG 0x0a30
#define RTL8367C_LUT_IPMC_LOOKUP_OP_OFFSET 3
	//?
	ret = rtl8363_setbit_reg(priv, RTL8367C_REG_LUT_CFG, RTL8367C_LUT_IPMC_LOOKUP_OP_OFFSET, 1);

	//Enable RGMII on 6 port for host connection
	rtl8363_rgmii_enable(priv);

//rgmi delay tx 1, rx 7
#define RTL8367C_REG_EXT1_RGMXF    0x1307
	ret = rtl8363_read_reg(priv, RTL8367C_REG_EXT1_RGMXF, &val);
	dev_err(dev, "Rgmi delay read: 0x%x\n", val);
				//tx			//rx
	val = (val & 0xFFF0) | ((1 << 3) & 0x0008) | (7 & 0x0007);
	dev_err(dev, "Rgmi delay write: 0x%x\n", val);
	ret = rtl8363_write_reg(priv, RTL8367C_REG_EXT1_RGMXF, val);

	ret = rtl8363_read_reg(priv, RTL8367C_REG_EXT1_RGMXF, &val);
	dev_err(dev, "Rgmi delay read: 0x%x\n", val);


//PHY ENABLE ALL
	ret = rtl8363_setbit_reg(priv, 0x130F, 5, 1);

//MOAR MAGIC 
//	ret = rtl8363_write_reg(priv, 0x1d53, 0x1);
//	ret = rtl8363_write_reg(priv, 0x1d55, 0xf);
//	ret = rtl8363_write_reg(priv, 0x1d54, 0x5fa);
//	ret = rtl8363_write_reg(priv, 0x1d52, 0x2473);
//	ret = rtl8363_write_reg(priv, 0x1d70, 0);

//	rtl8363_dump(priv, 1);
//	rtl8363_dump(priv, 3);
//	rtl8363_dump(priv, 6);

//	msleep(10000);

//	end_while

//	return -EINVAL;
	return 0;
}

static enum dsa_tag_protocol rtl8363_get_tag_protocol(struct dsa_switch *ds, int port, enum dsa_tag_protocol proto)
{
	dev_err(ds->dev, "%s, Get tag proto\n", __func__);

        return DSA_TAG_PROTO_NONE;
}


static int rtl8363_port_enable(struct dsa_switch *ds, int port, struct phy_device *phy) {
//static int rtl8363_port_enable(struct dsa_switch *ds, int port) {
	dev_err(ds->dev, "%s, port %d enable\n", __func__, port);
	return 0;
}

//static void rtl8363_port_disable(struct dsa_switch *ds, int port, struct phy_device *phy) {
static void rtl8363_port_disable(struct dsa_switch *ds, int port) {
	dev_err(ds->dev, "%s, port %d disable\n", __func__, port);
}


static void rtl8363_adjust_link(struct dsa_switch *ds, int port,
                                struct phy_device *phydev) {

	if(phy_is_pseudo_fixed_link(phydev)) {
		dev_err(ds->dev, "Fixed device at port %d\n", port);
	} else {
		dev_err(ds->dev, "Not Fixed device at port %d\n", port);
	}

//	dev_err(ds->dev, "Phydev: %d\n", phydev->loopback_enabled, phydev->speed, phydev->pause, phydev->duplex, phydev->pause, phydev->asym_pause);
}

static const struct dsa_switch_ops rtl8363_switch_ops = {
	.get_tag_protocol	= rtl8363_get_tag_protocol,
	.setup			= rtl8363_setup,
//	.get_strings		= rtl8363_get_strings,

	.phy_read		= rtl8363_phy_read,
	.phy_write		= rtl8363_phy_write,
//	.get_ethtool_stats	= rtl8363_get_ethtool_stats,
//	.get_sset_count		= rtl8363_get_sset_count,
	.adjust_link		= rtl8363_adjust_link,
	.port_enable		= rtl8363_port_enable,
	.port_disable		= rtl8363_port_disable,
//	.port_stp_state_set	= rtl8363_stp_state_set,
//	.port_bridge_join	= rtl8363_port_bridge_join,
//	.port_bridge_leave	= rtl8363_port_bridge_leave,
//	.port_fdb_add		= rtl8363_port_fdb_add,
//	.port_fdb_del		= rtl8363_port_fdb_del,
//	.port_fdb_dump		= rtl8363_port_fdb_dump,
};

static int rtl8363_probe(struct mdio_device *mdiodev)
{
	struct rtl8363_priv *priv;
	struct mii_bus *bus = mdiodev->bus;
	int val = 0;

	dev_err(&mdiodev->dev, "MDIO INIT, SUKA! bus: %p \n", mdiodev->bus);

//	val = bus->read(bus, 0x1, MII_MMD_DATA);
//	dev_err(&mdiodev->dev, "VAL: %d\n", val);

	priv = devm_kzalloc(&mdiodev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

//	priv->ds = dsa_switch_alloc(&mdiodev->dev, DSA_MAX_PORTS);
	priv->ds = devm_kzalloc(&mdiodev->dev, sizeof(*priv->ds), GFP_KERNEL);
	if (!priv->ds)
		return -ENOMEM;

	priv->ds->num_ports = DSA_MAX_PORTS;
	priv->ds->dev = &mdiodev->dev;

	priv->reset = devm_gpiod_get(&mdiodev->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(priv->reset)) {
		dev_err(&mdiodev->dev, "Couldn't get our reset line\n");
			return PTR_ERR(priv->reset);
	}

	priv->bus = mdiodev->bus;
	priv->dev = &mdiodev->dev;
	priv->ds->priv = priv;
	priv->ds->ops = &rtl8363_switch_ops;

	dev_set_drvdata(&mdiodev->dev, priv);

	return dsa_register_switch(priv->ds);

}

static void rtl8363_remove(struct mdio_device *mdiodev)
{
	struct rtl8363_priv *priv = dev_get_drvdata(&mdiodev->dev);
	dev_err(&mdiodev->dev, "SUKA REMOVED !\n");
	dsa_unregister_switch(priv->ds);
}

static const struct of_device_id rtl8363_of_match[] = {
	{ .compatible = "realtek,rtl8363nb" },
	{ /* sentinel */ },
};

static struct mdio_driver rtl8363_mdio_driver = {
	.probe  = rtl8363_probe,
	.remove = rtl8363_remove,
	.mdiodrv.driver = {
		.name = "rtl8363",
		.of_match_table = rtl8363_of_match,
	},
};

mdio_module_driver(rtl8363_mdio_driver);

MODULE_AUTHOR("Sean Wang <sean.wang@mediatek.com>");
MODULE_DESCRIPTION("Driver for Realtek 8363 Switch");
MODULE_LICENSE("GPL");
//MODULE_ALIAS("platform:mediatek-rtl8363");
