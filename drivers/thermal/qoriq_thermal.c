/*
 * Copyright 2015 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

/*
 * Based on Freescale QorIQ Thermal Monitoring Unit (TMU)
 */
#include <linux/cpufreq.h>
#include <linux/cpu_cooling.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/thermal.h>

#define SITES_MAX		16

#define TMU_TEMP_PASSIVE	85000
#define TMU_TEMP_CRITICAL	95000

#define TMU_PASSIVE_DELAY	1000	/* Milliseconds */
#define TMU_POLLING_DELAY	5000

/* The driver supports 1 passive trip point and 1 critical trip point */
enum tmu_thermal_trip {
	TMU_TRIP_PASSIVE,
	TMU_TRIP_CRITICAL,
	TMU_TRIP_NUM,
};

/*
 * QorIQ TMU Registers
 */
struct qoriq_tmu_site_regs {
	__be32 tritsr;		/* Immediate Temperature Site Register */
	__be32 tratsr;		/* Average Temperature Site Register */
	u8 res0[0x8];
} __packed;

struct qoriq_tmu_regs {
	__be32 tmr;		/* Mode Register */
#define TMR_DISABLE	0x0
#define TMR_ME		0x80000000
#define TMR_ALPF	0x0c000000
#define TMR_MSITE	0x00008000
#define TMR_ALL		(TMR_ME | TMR_ALPF | TMR_MSITE)
	__be32 tsr;		/* Status Register */
	__be32 tmtmir;		/* Temperature measurement interval Register */
#define TMTMIR_DEFAULT	0x00000007
	u8 res0[0x14];
	__be32 tier;		/* Interrupt Enable Register */
#define TIER_DISABLE	0x0
	__be32 tidr;		/* Interrupt Detect Register */
	__be32 tiscr;		/* Interrupt Site Capture Register */
	__be32 ticscr;		/* Interrupt Critical Site Capture Register */
	u8 res1[0x10];
	__be32 tmhtcrh;		/* High Temperature Capture Register */
	__be32 tmhtcrl;		/* Low Temperature Capture Register */
	u8 res2[0x8];
	__be32 tmhtitr;		/* High Temperature Immediate Threshold */
	__be32 tmhtatr;		/* High Temperature Average Threshold */
	__be32 tmhtactr;	/* High Temperature Average Crit Threshold */
	u8 res3[0x24];
	__be32 ttcfgr;		/* Temperature Configuration Register */
	__be32 tscfgr;		/* Sensor Configuration Register */
	u8 res4[0x78];
	struct qoriq_tmu_site_regs site[SITES_MAX];
	u8 res5[0x9f8];
	__be32 ipbrr0;		/* IP Block Revision Register 0 */
	__be32 ipbrr1;		/* IP Block Revision Register 1 */
	u8 res6[0x310];
	__be32 ttr0cr;		/* Temperature Range 0 Control Register */
	__be32 ttr1cr;		/* Temperature Range 1 Control Register */
	__be32 ttr2cr;		/* Temperature Range 2 Control Register */
	__be32 ttr3cr;		/* Temperature Range 3 Control Register */
};

/*
 * Thermal zone data
 */
struct qoriq_tmu_data {
	struct thermal_zone_device *tz;
	struct thermal_cooling_device *cdev;
	enum thermal_device_mode mode;
	unsigned long temp_passive;
	unsigned long temp_critical;
	struct qoriq_tmu_regs __iomem *regs;
};

static int tmu_get_mode(struct thermal_zone_device *tz,
			enum thermal_device_mode *mode)
{
	struct qoriq_tmu_data *data = tz->devdata;

	*mode = data->mode;

	return 0;
}

static int tmu_set_mode(struct thermal_zone_device *tz,
			enum thermal_device_mode mode)
{
	struct qoriq_tmu_data *data = tz->devdata;

	if (mode == THERMAL_DEVICE_ENABLED) {
		tz->polling_delay = TMU_POLLING_DELAY;
		tz->passive_delay = TMU_PASSIVE_DELAY;
		thermal_zone_device_update(tz);
	} else {
		tz->polling_delay = 0;
		tz->passive_delay = 0;
	}

	data->mode = mode;

	return 0;
}

static int tmu_get_temp(struct thermal_zone_device *tz, unsigned long *temp)
{
	u8 val;
	struct qoriq_tmu_data *data = tz->devdata;

	val = ioread32be(&data->regs->site[0].tritsr);
	*temp = (unsigned long)val * 1000;

	return 0;
}

static int tmu_get_trip_type(struct thermal_zone_device *tz, int trip,
			     enum thermal_trip_type *type)
{
	*type = (trip == TMU_TRIP_PASSIVE) ? THERMAL_TRIP_PASSIVE :
					     THERMAL_TRIP_CRITICAL;

	return 0;
}

static int tmu_get_trip_temp(struct thermal_zone_device *tz, int trip,
			     unsigned long *temp)
{
	struct qoriq_tmu_data *data = tz->devdata;

	*temp = (trip == TMU_TRIP_PASSIVE) ? data->temp_passive :
					     data->temp_critical;

	return 0;
}

static int tmu_get_crit_temp(struct thermal_zone_device *tz,
			     unsigned long *temp)
{
	struct qoriq_tmu_data *data = tz->devdata;

	*temp = data->temp_critical;

	return 0;
}

static int tmu_bind(struct thermal_zone_device *tz,
		    struct thermal_cooling_device *cdev)
{
	int ret;

	ret = thermal_zone_bind_cooling_device(tz, TMU_TRIP_PASSIVE, cdev,
					       THERMAL_NO_LIMIT,
					       THERMAL_NO_LIMIT);
	if (ret) {
		dev_err(&tz->device,
			"Binding zone %s with cdev %s failed:%d\n",
			tz->type, cdev->type, ret);
	}

	return ret;
}

static int tmu_unbind(struct thermal_zone_device *tz,
		      struct thermal_cooling_device *cdev)
{
	int ret;

	ret = thermal_zone_unbind_cooling_device(tz, TMU_TRIP_PASSIVE, cdev);
	if (ret) {
		dev_err(&tz->device,
			"Unbinding zone %s with cdev %s failed:%d\n",
			tz->type, cdev->type, ret);
	}

	return ret;
}

static int qoriq_tmu_calibration(struct platform_device *pdev)
{
	int i, val, len;
	u32 range[4];
	const __be32 *calibration;
	struct device_node *node = pdev->dev.of_node;
	struct qoriq_tmu_data *data = dev_get_drvdata(&pdev->dev);

	/* Disable monitoring before calibration */
	iowrite32be(TMR_DISABLE, &data->regs->tmr);

	if (of_property_read_u32_array(node, "fsl,tmu-range", range, 4))
		return -1;

	/* Init temperature range registers */
	iowrite32be(range[0], &data->regs->ttr0cr);
	iowrite32be(range[1], &data->regs->ttr1cr);
	iowrite32be(range[2], &data->regs->ttr2cr);
	iowrite32be(range[3], &data->regs->ttr3cr);

	calibration = of_get_property(node, "fsl,tmu-calibration", &len);
	if (calibration == NULL)
		return -1;

	for (i = 0; i < len; i += 8, calibration += 2) {
		val = (int)of_read_number(calibration, 1);
		iowrite32be(val, &data->regs->ttcfgr);
		val = (int)of_read_number(calibration + 1, 1);
		iowrite32be(val, &data->regs->tscfgr);
	}

	return 0;
}

static void qoriq_tmu_init_device(struct qoriq_tmu_data *data)
{
	/* Disable interrupt, using polling instead */
	iowrite32be(TIER_DISABLE, &data->regs->tier);

	/* Set update_interval */
	iowrite32be(TMTMIR_DEFAULT, &data->regs->tmtmir);

	/* Enable monitoring */
	iowrite32be(TMR_ALL, &data->regs->tmr);
}

static struct thermal_zone_device_ops tmu_tz_ops = {
	.bind = tmu_bind,
	.unbind = tmu_unbind,
	.get_temp = tmu_get_temp,
	.get_mode = tmu_get_mode,
	.set_mode = tmu_set_mode,
	.get_trip_type = tmu_get_trip_type,
	.get_trip_temp = tmu_get_trip_temp,
	.get_crit_temp = tmu_get_crit_temp,
};

static int qoriq_tmu_probe(struct platform_device *pdev)
{
	int ret;
	struct cpumask clip_cpus;
	struct qoriq_tmu_data *data;

	if (!cpufreq_get_current_driver()) {
		dev_dbg(&pdev->dev, "No cpufreq driver yet\n");
		return -EPROBE_DEFER;
	}

	if (!pdev->dev.of_node) {
		dev_err(&pdev->dev, "Device OF-Node is NULL");
		return -EFAULT;
	}

	data = devm_kzalloc(&pdev->dev, sizeof(struct qoriq_tmu_data),
			    GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	dev_set_drvdata(&pdev->dev, data);
	data->regs = of_iomap(pdev->dev.of_node, 0);

	if (!data->regs) {
		dev_err(&pdev->dev, "Failed to get memory region\n");
		ret = -ENODEV;
		goto err_iomap;
	}

	ret = qoriq_tmu_calibration(pdev);	/* TMU calibration */
	if (ret < 0) {
		dev_err(&pdev->dev, "TMU calibration failed.\n");
		ret = -ENODEV;
		goto err_iomap;
	}

	qoriq_tmu_init_device(data);	/* TMU initialization */

	cpumask_setall(&clip_cpus);
	data->cdev = cpufreq_cooling_register(&clip_cpus);
	if (IS_ERR(data->cdev)) {
		ret = PTR_ERR(data->cdev);
		dev_err(&data->cdev->device,
			"Failed to register cpufreq cooling device: %d\n", ret);
		goto err_cooling;
	}

	data->temp_passive = TMU_TEMP_PASSIVE;
	data->temp_critical = TMU_TEMP_CRITICAL;
	data->tz = thermal_zone_device_register("tmu_thermal_zone",
						TMU_TRIP_NUM,
						0, data,
						&tmu_tz_ops, NULL,
						TMU_PASSIVE_DELAY,
						TMU_POLLING_DELAY);

	if (IS_ERR(data->tz)) {
		ret = PTR_ERR(data->tz);
		dev_err(&pdev->dev,
			"Failed to register thermal zone device %d\n", ret);
		goto err_thermal;
	}

	data->mode = THERMAL_DEVICE_ENABLED;

	return 0;

err_thermal:
	cpufreq_cooling_unregister(data->cdev);

err_cooling:
	iounmap(data->regs);

err_iomap:
	dev_set_drvdata(&pdev->dev, NULL);
	devm_kfree(&pdev->dev, data);

	return ret;
}

static int qoriq_tmu_remove(struct platform_device *pdev)
{
	struct qoriq_tmu_data *data = dev_get_drvdata(&pdev->dev);

	/* Disable monitoring */
	iowrite32be(TMR_DISABLE, &data->regs->tmr);

	thermal_zone_device_unregister(data->tz);
	cpufreq_cooling_unregister(data->cdev);
	iounmap(data->regs);

	dev_set_drvdata(&pdev->dev, NULL);
	devm_kfree(&pdev->dev, data);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int qoriq_tmu_suspend(struct device *dev)
{
	struct qoriq_tmu_data *data = dev_get_drvdata(dev);

	/* Disable monitoring */
	iowrite32be(TMR_DISABLE, &data->regs->tmr);
	data->mode = THERMAL_DEVICE_DISABLED;

	return 0;
}

static int qoriq_tmu_resume(struct device *dev)
{
	struct qoriq_tmu_data *data = dev_get_drvdata(dev);

	/* Enable monitoring */
	iowrite32be(TMR_ALL, &data->regs->tmr);
	data->mode = THERMAL_DEVICE_ENABLED;

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(qoriq_tmu_pm_ops,
			 qoriq_tmu_suspend, qoriq_tmu_resume);

static const struct of_device_id qoriq_tmu_match[] = {
	{ .compatible = "fsl,qoriq-tmu", },
	{},
};

static struct platform_driver qoriq_tmu = {
	.driver	= {
		.owner		= THIS_MODULE,
		.name		= "qoriq_thermal",
		.pm = &qoriq_tmu_pm_ops,
		.of_match_table	= qoriq_tmu_match,
	},
	.probe	= qoriq_tmu_probe,
	.remove	= qoriq_tmu_remove,
};
module_platform_driver(qoriq_tmu);

MODULE_AUTHOR("Jia Hongtao <hongtao.jia@freescale.com>");
MODULE_DESCRIPTION("Freescale QorIQ Thermal Monitoring Unit driver");
MODULE_LICENSE("GPL v2");
