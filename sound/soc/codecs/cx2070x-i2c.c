// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <sound/soc.h>
#include "cx2070x.h"

static int cx2070x_i2c_probe(struct i2c_client *i2c,
			     const struct i2c_device_id *id)
{
	struct regmap *regmap = devm_regmap_init_i2c(i2c, &cx2070x_regmap);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	return cx2070x_probe(&i2c->dev, regmap);
}

static const struct i2c_device_id cx2070x_i2c_id[] = {
	{ "cnxt,cx20707", },
	{ }
};
MODULE_DEVICE_TABLE(i2c, cx2070x_i2c_id);

static const struct of_device_id cx2070x_of_match[] = {
	{ .compatible = "cnxt,cx20707", },
	{ }
};
MODULE_DEVICE_TABLE(of, cx2070x_of_match);

static struct i2c_driver cx2070x_i2c_driver = {
	.probe		= cx2070x_i2c_probe,
	.id_table	= cx2070x_i2c_id,
	.driver		= {
		.name	= "cx2070x",
		.of_match_table = cx2070x_of_match,
	},
};
module_i2c_driver(cx2070x_i2c_driver);

MODULE_DESCRIPTION("Conexant cx2070x i2c codec driver");
MODULE_AUTHOR("Aleksandrov Stanislav <lightofmysoul@gmail.com>");
MODULE_LICENSE("GPL v2");
