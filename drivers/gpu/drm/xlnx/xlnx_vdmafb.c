/***************************************************************
 Copyright © ALIENTEK Co., Ltd. 1998-2029. All rights reserved.
 文件名    : xlnx_vdmafb.c
 作者      : 邓涛
 版本      : V1.0
 描述      : Xilinx VDMA LCD FrameBuffer驱动程序
 其他      : 无
 论坛      : www.openedv.com
 日志      : 初版V1.0 2020/7/23 邓涛创建
 ***************************************************************/

#include <drm/drm_fourcc.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/fb.h>
#include <linux/clk.h>
#include <linux/of_dma.h>
#include <video/videomode.h>
#include <linux/delay.h>
#include <linux/of_gpio.h>
#include <video/of_videomode.h>
#include <linux/pwm.h>
#include <linux/dma-mapping.h>

#include <linux/videodev2.h>
#include <linux/dma/xilinx_frmbuf.h>
#include "xlnx_bridge.h"

#define XLNX_VFMT_SIZE		4

/* 正点原子LCD屏硬件ID */
#define ATK4342		0			// 4.3寸480*272
#define ATK4384		4			// 4.3寸800*480
#define ATK7084		1			// 7寸800*480
#define ATK7016		2			// 7寸1024*600
#define ATK1018		5			// 10寸1280*800

/* 自定义结构体用于描述我们的LCD设备 */
struct xilinx_vdmafb_dev {
	struct fb_info *info;			// FrameBuffer设备信息
	struct platform_device *pdev;	// platform平台设备
	struct clk *pclk;				// LCD像素时钟
	struct xlnx_bridge *bridge;		// 时序控制器
	struct dma_chan *vdma;			// VDMA通道
	struct gpio_desc *rst_gpio;		// LCD Reset
	u32 fmt;						// video format, drm format
	int bl_gpio;					// LCD背光引脚
	bool hdmi;						// 是否为HDMI设备
};

static const struct fb_var_screeninfo xilinx_fb_var = {
	.bits_per_pixel = 24,

	.red =		{ 16, 8, 0 },
	.green =	{ 8, 8, 0 },
	.blue =		{ 0, 8, 0 },
	.transp =	{ 0, 0, 0 },

	.activate =	FB_ACTIVATE_NOW,
	.grayscale   = 0,
	// 彩色
	.nonstd      = 0,
	// 标准像素格式
	.accel_flags = FB_ACCEL_NONE,
	// 像素深度（bit位）
	.bits_per_pixel = 24,	
};

static int vdmafb_setcolreg(unsigned regno, unsigned red,
			unsigned green, unsigned blue,
			unsigned transp, struct fb_info *fbi)
{
	u32 *palette = fbi->pseudo_palette;

	if (regno >= 16)
		return -EINVAL;

	if (fbi->var.grayscale) {
		/* Convert color to grayscale.
			* grayscale = 0.30*R + 0.59*G + 0.11*B
			*/
		blue = (red * 77 + green * 151 + blue * 28 + 127) >> 8;
		green = blue;
		red = green;
	}
	
	red >>= 8; green >>= 8; blue >>= 8;
	palette[regno] = (red << 16) | (green << 8) | blue;;

	return 0;
}

static int vdmafb_check_var(struct fb_var_screeninfo *var,
			struct fb_info *fb_info)
{
	struct fb_var_screeninfo *fb_var = &fb_info->var;
	memcpy(var, fb_var, sizeof(struct fb_var_screeninfo));
	return 0;
}

/* Frame Buffer操作函数集 */
static struct fb_ops vdmafb_ops = {
	.owner 			= THIS_MODULE,
	.fb_setcolreg	= vdmafb_setcolreg,
	.fb_check_var	= vdmafb_check_var,
	FB_DEFAULT_IOMEM_OPS,
	.fb_copyarea	= sys_copyarea,
	.fb_imageblit	= sys_imageblit,
};

static int vdmafb_init_fbinfo_dt(struct xilinx_vdmafb_dev *fbdev,
			struct videomode *vmode)
{
	struct device *dev = &fbdev->pdev->dev;
	int display_timing;
	u32 lcd_id = 0;
	int ret, i;
	struct gpio_desc *lcd_id_gpios[3];
	int temp;

	for (i = 0; i < 3; i++) {
		lcd_id_gpios[i] = devm_gpiod_get_index(dev, "lcdID", i, GPIOD_IN);
		if (IS_ERR(lcd_id_gpios[i])) {
			dev_err(dev, "Failed to get GPIO %d: %ld\n", i, PTR_ERR(lcd_id_gpios[i]));
			return -1;
		}
	}

	/* 获取LCD硬件ID */
	ret = of_property_read_u32(dev->of_node, "lcd-id", &lcd_id);
	if (ret)
	{
		for (i = 0; i < 3; i++) {
			lcd_id |= gpiod_get_value(lcd_id_gpios[i]) << i;
		}
	}
	for (i = 0; i < 3; i++) {
		ret = gpiod_direction_output(lcd_id_gpios[i], 0);  // 设为输出模式，初始低电平
		if (ret < 0) {
			dev_err(dev, "Failed to set GPIO%d as output\n", i);
			return ret;
		}
		dev_info(dev, "GPIO %d is now output, value: %d\n", i, gpiod_get_value(lcd_id_gpios[i]));
	}

	dev_info(dev, "Alientek LCD ID: %d\n", lcd_id);

	/* 根据LCD ID匹配对应的时序参数 */
	switch (lcd_id) {
	case ATK4342: display_timing = 0; break;
	case ATK4384: display_timing = 1; break;
	case ATK7084: display_timing = 2; break;
	case ATK7016: display_timing = 3; break;
	case ATK1018: display_timing = 4; break;
	default: display_timing = 2; break;
	}

	/* 判断是不是HDMI */
	fbdev->hdmi = false;
	if (of_property_read_bool(dev->of_node, "hdmi")) {
		display_timing = 0;
		fbdev->hdmi = true;
		goto out;
	}

out:
	/* 从设备树中获取时序参数信息 */
	ret = of_get_videomode(dev->of_node, vmode, display_timing);
	if (ret < 0) {
		dev_err(dev, "Failed to get videomode from DT\n");
		return ret;
	}

	return 0;
}

static int vdmafb_init_fbinfo(struct xilinx_vdmafb_dev *fbdev,
			struct videomode *vmode)
{
	struct device *dev = &fbdev->pdev->dev;
	struct fb_info *fb_info = fbdev->info;
	struct fb_videomode mode = {0};
	dma_addr_t fb_phys;		// 显存物理地址
	void *fb_virt;			// 显存虚拟地址
	unsigned fb_size;		// 显存大小
	int ret;

	/* 解析设备树获取LCD时序参数 */
	ret = vdmafb_init_fbinfo_dt(fbdev, vmode);
	if (ret < 0)
		return ret;

	if (vmode->hactive == 0 || vmode->vactive == 0 ||
		vmode->hactive > 4096 || vmode->vactive > 4096) {
		dev_err(dev, "Invalid resolution: %dx%d\n", vmode->hactive, vmode->vactive);
		return -EINVAL;
	}

	/* 申请LCD显存 */
	fb_size = PAGE_ALIGN(vmode->hactive * vmode->vactive * 3);
	fb_virt = dma_alloc_wc(dev, fb_size, &fb_phys, GFP_KERNEL);
	if (!fb_virt)
		return -ENOMEM;

	memset(fb_virt, 0, fb_size);	// 显存清零

	/* 初始化fb_info */
	fb_info->fbops = &vdmafb_ops;
	fb_info->flags = FBINFO_VIRTFB | FBINFO_HWACCEL_DISABLED;
	fb_info->screen_base = fb_virt;
	fb_info->screen_size = fb_size;

	strcpy(fb_info->fix.id, "xlnx");
	fb_info->fix.type = FB_TYPE_PACKED_PIXELS;
	fb_info->fix.visual = FB_VISUAL_TRUECOLOR,
	fb_info->fix.accel = FB_ACCEL_NONE;
	fb_info->fix.line_length = vmode->hactive * 3;
	fb_info->fix.smem_start = fb_phys;
	fb_info->fix.smem_len = fb_size;

	fb_info->var = xilinx_fb_var;
	
	//fb_info->var.width  = xxx;	LCD屏的物理宽度（单位毫米）
	//fb_info->var.height = yyy;	LCD屏的物理高度（单位毫米）

	fb_info->var.xres = fb_info->var.xres_virtual = vmode->hactive;
	fb_info->var.yres = fb_info->var.yres_virtual = vmode->vactive;
	fb_info->var.xoffset = fb_info->var.yoffset = 0;

	dev_info(dev, "Resolution: %dx%d, fb_size=%u\n", 
		vmode->hactive, vmode->vactive, fb_size);

	fb_videomode_from_videomode(vmode, &mode);
	fb_videomode_to_var(&fb_info->var, &mode);

	return 0;
}

static int vdmafb_init_vdma(struct xilinx_vdmafb_dev *fbdev)
{
	struct device *dev = &fbdev->pdev->dev;
	struct fb_info *fbi = fbdev->info;
	struct dma_interleaved_template dma_template = {0};
	struct dma_async_tx_descriptor *tx_desc;
	// struct xilinx_vdma_config vdma_config = {0};

	if (!fbi->fix.smem_start) {
		dev_err(dev, "Invalid DMA buffer physical address\n");
		return -EINVAL;
	}
	
	/* 申请VDMA通道 */
	fbdev->vdma = dma_request_chan(dev, "lcd_vdma");
	if (IS_ERR(fbdev->vdma)) {
		dev_err(dev, "Failed to request vdma channel\n");
		return PTR_ERR(fbdev->vdma);
	}
	/* 终止VDMA通道数据传输 */
	dmaengine_terminate_all(fbdev->vdma);
	// /* 设置传输模式 */
	// xilinx_xdma_set_mode(fbdev->vdma, AUTO_RESTART);
	/* 设置图像格式 */
	xilinx_xdma_drm_config(fbdev->vdma, fbdev->fmt);
	/* 初始化VDMA通道 */
	dma_template.dir         = DMA_MEM_TO_DEV;
	dma_template.numf        = fbi->var.yres;
	dma_template.sgl[0].size = fbi->fix.line_length;
	dma_template.frame_size  = 1;
	dma_template.sgl[0].icg  = 0;
	dma_template.src_start   = fbi->fix.smem_start;	// 物理地址
	dma_template.src_sgl     = 1;
	dma_template.src_inc     = 1;
	dma_template.dst_inc     = 0;
	dma_template.dst_sgl     = 0;

	tx_desc = dmaengine_prep_interleaved_dma(fbdev->vdma, &dma_template,
			DMA_CTRL_ACK | DMA_PREP_INTERRUPT);
	if (!tx_desc) {
		dev_err(dev, "Failed to prepare DMA descriptor\n");
		dma_release_channel(fbdev->vdma);
		return -1;
	}

	// vdma_config.park = 1;
	// if(xilinx_vdma_channel_set_config(fbdev->vdma, &vdma_config))
	// {
	// 	dev_err(dev, "Failed to configure dma channel\n");
	// 	dma_release_channel(fbdev->vdma);
	// 	return -1;
	// }
	
	/* 启动VDMA通道数据传输 */
	dmaengine_submit(tx_desc);
	dma_async_issue_pending(fbdev->vdma);
	return 0;
}

static int vdmafb_init_bridge(struct xilinx_vdmafb_dev *fbdev,
			struct videomode *vmode)
{
	struct device *dev = &fbdev->pdev->dev;
	struct device_node *bridge_node;
	int ret;

	/* VTC Bridge support */
	bridge_node = of_parse_phandle(dev->of_node, "xlnx,bridge", 0);
	if (bridge_node) {
		fbdev->bridge = of_xlnx_bridge_get(bridge_node);
		if (!fbdev->bridge) {
			dev_info(dev, "Didn't get xilinx bridge instance\n");
			return -EPROBE_DEFER;
		}
	} else {
		dev_info(dev, "xilinx bridge property not present\n");
	}

	xlnx_bridge_disable(fbdev->bridge);						// 禁止vtc
	ret = xlnx_bridge_set_timing(fbdev->bridge, vmode);		// 配置vtc时序参数
	if (ret) {
		dev_err(dev, "set timing failed\n");
		return ret;
	}
	ret = xlnx_bridge_enable(fbdev->bridge);				// 使能vtc
	if (ret) {
		dev_err(dev, "enable bridge failed\n");
		return ret;
	}
	return 0;
}

static int vdmafb_probe(struct platform_device *pdev)
{
	struct xilinx_vdmafb_dev *fbdev;
	struct fb_info *info;
	struct videomode* vmode;
	struct pwm_device *pwm;
	int ret;
	u32 clk_rate = 0;
	const struct drm_format_info *fmt_info;
	const char* vformat;

	vmode = kmalloc(sizeof(struct videomode), GFP_KERNEL);
	if(!vmode)
	{
		return -ENOMEM;
	}

	/* 实例化一个fb_info结构体对象 */
	info = framebuffer_alloc(sizeof(struct xilinx_vdmafb_dev), &pdev->dev);
	if (!info) {
		dev_err(&pdev->dev, "Failed to allocate memory\n");
		return -ENOMEM;
	}

	fbdev = info->par;
	fbdev->info = info;
	fbdev->pdev = pdev;

	/* 获取LCD所需的像素时钟 */
	fbdev->pclk = devm_clk_get(&pdev->dev, "lcd_pclk");
	if (IS_ERR(fbdev->pclk)) {
		dev_err(&pdev->dev, "Failed to get pixel clock\n");
		ret = PTR_ERR(fbdev->pclk);
		goto out1;
	}

	clk_disable_unprepare(fbdev->pclk);		// 先禁止时钟输出

	/* 初始化info变量 */
	ret = vdmafb_init_fbinfo(fbdev, vmode);
	if (ret)
		goto out1;

	ret = fb_alloc_cmap(&info->cmap, 256, 0);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to allocate color map\n");
		goto out2;
	}

	info->pseudo_palette = devm_kzalloc(&pdev->dev, sizeof(u32) * 16, GFP_KERNEL);
	if (!info->pseudo_palette) {
		ret = -ENOMEM;
		goto out3;
	}

	/* 设置LCD像素时钟、使能时钟 */
	clk_rate = PICOS2KHZ(info->var.pixclock) * 1000;
	dev_info(&pdev->dev, "try to set lcd pixel clk %u Hz\n", clk_rate);
	clk_set_rate(fbdev->pclk, clk_rate);
	clk_prepare_enable(fbdev->pclk);
	msleep(5);  // delay

	ret = of_property_read_string(pdev->dev.of_node, "xlnx,vformat", &vformat);
	if (ret) {
		dev_err(&pdev->dev, "No xlnx,vformat value in dts\n");
		return ret;
	}

	strncpy((char *)&fbdev->fmt, vformat, XLNX_VFMT_SIZE);

	fmt_info = drm_format_info(fbdev->fmt);
	if (!fmt_info) {
		dev_err(&pdev->dev, "Invalid video format in dts\n");
		goto out4;
	}
	dev_info(&pdev->dev, "video format: fmt: %4s, planes: %d, depth: %d, hsub: %d, vsub: %d", 
		(const char*)&(fmt_info->format), 
		fmt_info->num_planes, 
		fmt_info->depth,
		fmt_info->hsub,
		fmt_info->vsub
	);
	/* 初始化LCD时序控制器vtc */
	ret = vdmafb_init_bridge(fbdev, vmode);
	if (ret)
		goto out4;

	/* 初始化LCD VDMA */
	ret = vdmafb_init_vdma(fbdev);
	if (ret)
		goto out5;

	/* 注册FrameBuffer设备 */
	ret = register_framebuffer(info);
	if (ret < 0) {
		dev_err(&pdev->dev,"Failed to register framebuffer device\n");
		goto out6;
	}

	if (fbdev->hdmi)
		goto out;

	/* 打开LCD背光 */
	pwm = devm_pwm_get(&pdev->dev, NULL);
	if (IS_ERR(pwm)) {
		dev_err(&pdev->dev, "failed to get pwm, try use bl-gpio\n");
		fbdev->bl_gpio = of_get_named_gpio(pdev->dev.of_node, "bl-gpio", 0);
		if (!gpio_is_valid(fbdev->bl_gpio)) {
			ret = fbdev->bl_gpio;
			goto out7;
		}

		ret = devm_gpio_request_one(&pdev->dev, fbdev->bl_gpio,
					GPIOF_OUT_INIT_HIGH, "lcd backlight");	//输出高电平打开背光
		if (ret < 0)
			goto out7;
	}
	else {
		dev_info(&pdev->dev, "get pwm device success\n");
		pwm_config(pwm, 5000000, 5000000);	// 配置PWM
		pwm_enable(pwm);		// 使能PWM打开背光
	}

	fbdev->rst_gpio = devm_gpiod_get(&pdev->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(fbdev->rst_gpio)) {
		ret = PTR_ERR(fbdev->rst_gpio);
		if (ret == -EPROBE_DEFER)
			dev_info(&pdev->dev,
				"Probe deferred due to GPIO reset defer\n");
		else
			dev_err(&pdev->dev,
				"Unable to locate reset property in dt\n");
		return ret;
	}

	gpiod_set_value_cansleep(fbdev->rst_gpio, 0x0);

	dev_info(&pdev->dev, "Xilinx VDMAFB Driver Probed!!! \n");
	memset(fbdev->info->screen_base, 0xFF, fbdev->info->screen_size); // 全屏白色
	dev_info(&pdev->dev, "Forced screen fill with white\n");
out:
	platform_set_drvdata(pdev, fbdev);
	return 0;

out7:
	unregister_framebuffer(info);	// 卸载FrameBuffer设备

out6:
	dmaengine_terminate_all(fbdev->vdma);	// 终止VDMA通道所有数据传输
	dma_release_channel(fbdev->vdma);		// 释放VDMA通道

out5:
	xlnx_bridge_disable(fbdev->bridge);			// 禁止vtc时序控制器

out4:
	clk_disable_unprepare(fbdev->pclk);		// 禁止时钟输出

out3:
	fb_dealloc_cmap(&info->cmap);			// 销毁colormap

out2:
	dma_free_wc(&pdev->dev, info->screen_size,	// 释放DMA内存
				info->screen_base, info->fix.smem_start);

out1:
	framebuffer_release(info);				// 释放fb_info对象
	kfree(vmode);
	return ret;
}

static int vdmafb_remove(struct platform_device *pdev)
{
	struct xilinx_vdmafb_dev *fbdev = platform_get_drvdata(pdev);
	struct fb_info *info = fbdev->info;

	unregister_framebuffer(info);
	dmaengine_terminate_all(fbdev->vdma);
	dma_release_channel(fbdev->vdma);
	xlnx_bridge_disable(fbdev->bridge);
	clk_disable_unprepare(fbdev->pclk);
	fb_dealloc_cmap(&info->cmap);
	dma_free_wc(&pdev->dev, info->screen_size,
				info->screen_base, info->fix.smem_start);
	framebuffer_release(info);
	return 0;
}

static void vdmafb_shutdown(struct platform_device *pdev)
{
	struct xilinx_vdmafb_dev *fbdev = platform_get_drvdata(pdev);
	xlnx_bridge_disable(fbdev->bridge);
	clk_disable_unprepare(fbdev->pclk);
}

static const struct of_device_id vdmafb_of_match_table[] = {
	{ .compatible = "xlnx,vdmafb", },
	{ /* end of table */ },
};
MODULE_DEVICE_TABLE(of, vdmafb_of_match_table);

static struct platform_driver xilinx_vdmafb_driver = {
	.probe    = vdmafb_probe,
	.remove   = vdmafb_remove,
	.shutdown = vdmafb_shutdown,
	.driver = {
		.name           = "xilinx-vdmafb",
		.of_match_table = vdmafb_of_match_table,
	},
};

module_platform_driver(xilinx_vdmafb_driver);

MODULE_DESCRIPTION("Framebuffer driver based on Xilinx VDMA IP Core.");
MODULE_AUTHOR("Deng Tao <773904075@qq.com>, ALIENTEK, Inc.");
MODULE_LICENSE("GPL v2");
