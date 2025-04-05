/***************************************************************
 Copyright © ALIENTEK Co., Ltd. 1998-2029. All rights reserved.
 文件名    : pwm-dglnt.c
 作者      : 邓涛
 版本      : V1.0
 描述      : Digilent AXI_PWM驱动程序
 其他      : 无
 论坛      : www.openedv.com
 日志      : 初版V1.0 2020/7/9 邓涛创建
 ***************************************************************/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/io.h>
#include <linux/clk.h>

/*
 * AXI_PWM寄存器定义
 */
#define PWM_AXI_CTRL_REG_OFFSET		0
#define PWM_AXI_STATUS_REG_OFFSET	4
#define PWM_AXI_PERIOD_REG_OFFSET	8
#define PWM_AXI_DUTY_REG_OFFSET		64

/* 自定义axi_pwm设备结构体 */
struct dglnt_pwm_dev {
	struct device	*dev;			// device对象指针
	struct pwm_chip	chip;			// 内置pwm_chip成员
	struct clk		*pwm_clk;		// axi_pwm IP输入时钟
	void __iomem	*base;			// axi_pwm IP寄存器基地址
	unsigned int	period_min_ns;	// pwm输出最小周期
};

#define S_TO_NS  1000000000U		// 秒换算成纳秒的量级单位

/*
 * @description				: axi_pwm写寄存器操作函数（inline内敛函数）
 * @param – dglnt_pwm		: struct dglnt_pwm_dev对象指针
 * @param - reg				: 寄存器地址偏移量
 * @param - val				: 要写入的数据
 * @return					: 无
 */
static inline void dglnt_pwm_writel(struct dglnt_pwm_dev *dglnt_pwm,
			u32 reg, u32 val)
{
	writel(val, dglnt_pwm->base + reg);
}

/*
 * @description				: axi_pwm读寄存器操作函数（inline内敛函数）
 * @param – dglnt_pwm		: struct dglnt_pwm_dev对象指针
 * @param - reg				: 寄存器地址偏移量
 * @param - val				: 要读取的数据
 * @return					: 无
 */
static inline void dglnt_pwm_readl(struct dglnt_pwm_dev *dglnt_pwm,
	u32 reg, u32* val)
{
	*val = readl(dglnt_pwm->base + reg);
}

static inline struct dglnt_pwm_dev *to_dglnt_pwm_dev(struct pwm_chip *chip)
{
	return container_of(chip, struct dglnt_pwm_dev, chip);
}

static int dglnt_pwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
	struct pwm_state *state)
{
	u32 period, duty_cycle;
	u32 enabled = 0;
	int err=0;
	struct dglnt_pwm_dev *dglnt_pwm = to_dglnt_pwm_dev(chip);
	// struct device *dev = dglnt_pwm->chip.dev;

	dglnt_pwm_readl(dglnt_pwm, PWM_AXI_PERIOD_REG_OFFSET, &(period));
	state->period = period;
	dglnt_pwm_readl(dglnt_pwm, PWM_AXI_DUTY_REG_OFFSET +
		(4 * pwm->hwpwm), &(duty_cycle));
	state->duty_cycle = duty_cycle;
	dglnt_pwm_readl(dglnt_pwm, PWM_AXI_CTRL_REG_OFFSET, &enabled);
	state->enabled = (enabled & 0x01) == 0x01? true : false;
	state->polarity = PWM_POLARITY_NORMAL;
	return err;
}

/*
 * @description				: axi_pwm周期以及占空比配置操作函数
 * @param – chip			: struct pwm_chip对象指针
 * @param - pwm				: struct pwm_device对象指针
 * @param - state			: pwm state 对象
 * @return					: 成功返回0，失败返回负数
 */
static int dglnt_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			const struct pwm_state* state)
{
	u32 duty = state->duty_cycle;
	u32 period = state->period;
	u32 reg=0;

	struct dglnt_pwm_dev *dglnt_pwm = to_dglnt_pwm_dev(chip);
	struct device *dev = dglnt_pwm->chip.dev;

	dev_info(dev,"duty=%u period=%u\n", duty, period);
	if(pwm_get_period(pwm) != period)
	{
		if(period < dglnt_pwm->period_min_ns)
		{
			dev_err(dev, "disable pwm failed");
			return EINVAL;
		}
		dglnt_pwm_writel(dglnt_pwm, PWM_AXI_PERIOD_REG_OFFSET, period);
	}
	
	if(pwm_get_duty_cycle(pwm) != duty)
	{
		dglnt_pwm_writel(dglnt_pwm, PWM_AXI_DUTY_REG_OFFSET +
			(4 * pwm->hwpwm), duty);
	}

	if (pwm_is_enabled(pwm) && !state->enabled)
	{
		dglnt_pwm_writel(dglnt_pwm, PWM_AXI_CTRL_REG_OFFSET, 0);
		dglnt_pwm_readl(dglnt_pwm, PWM_AXI_CTRL_REG_OFFSET, &reg);
		if ((reg & 0x01) != 0) {
			dev_err(dev, "disable pwm failed");
			return EBUSY;
		}
	}
	else if(!pwm_is_enabled(pwm) && state->enabled)
	{
		dglnt_pwm_writel(dglnt_pwm, PWM_AXI_CTRL_REG_OFFSET, 1);
		dglnt_pwm_readl(dglnt_pwm, PWM_AXI_CTRL_REG_OFFSET, &reg);
		if ((reg & 0x01) != 1) {
			dev_err(dev, "enable pwm failed");
			return EBUSY;
		}
	}

	return 0;
}

/*
 * PWM操作函数集，struct pwm_ops结构体对象
 */
static const struct pwm_ops dglnt_pwm_ops = {
	.apply = dglnt_pwm_apply,				// PWM配置方法
	.get_state = dglnt_pwm_get_state,		// PWM获取参数
	.owner = THIS_MODULE,
};

static int dglnt_pwm_probe(struct platform_device *pdev)
{
	struct dglnt_pwm_dev *dglnt_pwm;
	unsigned long clk_rate;
	struct resource *res;
	int ret;

	/* 实例化一个dglnt_pwm_dev对象 */
	dglnt_pwm = devm_kzalloc(&pdev->dev, sizeof(*dglnt_pwm), GFP_KERNEL);
	if (!dglnt_pwm)
		return -ENOMEM;

	dglnt_pwm->dev = &pdev->dev;

	/* 获取平台资源、得到寄存器基地址 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	dglnt_pwm->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(dglnt_pwm->base))
		return PTR_ERR(dglnt_pwm->base);

	/* 获取时钟、得到时钟大小 */
	dglnt_pwm->pwm_clk = devm_clk_get(&pdev->dev, "pwm");
	if (IS_ERR(dglnt_pwm->pwm_clk)) {
		dev_err(&pdev->dev, "failed to get pwm clock\n");
		return PTR_ERR(dglnt_pwm->pwm_clk);
	}

	clk_rate = clk_get_rate(dglnt_pwm->pwm_clk);

	/* 计算PWM的最小周期 */
	dglnt_pwm->period_min_ns = S_TO_NS / clk_rate;
	dev_info(&pdev->dev, "pwm_clk=%ld  period_min_ns=%d\n",
				clk_rate, dglnt_pwm->period_min_ns);

	/* 注册PWM设备 */
	dglnt_pwm->chip.dev = &pdev->dev;
	dglnt_pwm->chip.ops = &dglnt_pwm_ops;
	dglnt_pwm->chip.base = 0;
	ret = of_property_read_u32(pdev->dev.of_node, "npwm", &dglnt_pwm->chip.npwm);
	if (ret) {
		dev_err(&pdev->dev, "failed to read npwm\n");
		return ret;
	}

	dglnt_pwm_writel(dglnt_pwm, PWM_AXI_CTRL_REG_OFFSET, 0);//先禁止PWM输出
	ret = pwmchip_add(&dglnt_pwm->chip);
	if (0 > ret) {
		dev_err(&pdev->dev, "pwmchip_add failed: %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, dglnt_pwm);
	return 0;
}

static void dglnt_pwm_remove(struct platform_device *pdev)
{
	struct dglnt_pwm_dev *dglnt_pwm = platform_get_drvdata(pdev);
	unsigned int i;

	/* 禁止PWM输出 */
	for (i = 0; i < dglnt_pwm->chip.npwm; i++)
		dglnt_pwm_writel(dglnt_pwm, PWM_AXI_CTRL_REG_OFFSET, 0);

	/* 卸载PWM设备 */
	pwmchip_remove(&dglnt_pwm->chip);
}

static const struct of_device_id dglnt_pwm_of_match[] = {
	{ .compatible = "digilent,axi-pwm", },
	{ },
};
MODULE_DEVICE_TABLE(of, dglnt_pwm_of_match);

static struct platform_driver dglnt_pwm_driver = {
	.driver = {
		.name = "dglnt-pwm",
		.of_match_table = dglnt_pwm_of_match,
	},
	.probe = dglnt_pwm_probe,
	.remove_new = dglnt_pwm_remove,
};

module_platform_driver(dglnt_pwm_driver);

MODULE_AUTHOR("Deng Tao <773904075@qq.com>");
MODULE_DESCRIPTION("Simple Driver for Digilent AXI_PWM IP Core");
MODULE_LICENSE("GPL v2");
