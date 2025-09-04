#include <linux/module.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/of_reserved_mem.h>
#include <linux/pm_runtime.h>
#include <linux/debugfs.h>
#include <linux/devfreq.h>
#include <linux/err.h>
#include <linux/sysfs.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_module.h>
#include <drm/drm_of.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "malidp_drv.h"
#include "malidp_mw.h"
#include "malidp_regs.h"
#include "malidp_hw.h"

#define MALIDP_CONF_VALID_TIMEOUT    250
#define AFBC_HEADER_SIZE             16
#define AFBC_SUPERBLK_ALIGNMENT      128

/* --- GPU Devfreq Userspace --- */
static struct devfreq *gpu_devfreq;
static struct devfreq_dev_profile gpu_profile;

/* Example GPU frequencies (you can edit to match your SoC) */
static unsigned long gpu_freq_table[] = {
    754000000,
    1000000000,
    1500000000,
    2000000000,
    2500000000,
    3000000000,
    3500000000,
    4212000000,
};

/* Function to set GPU frequency from userspace */
int malidp_set_gpu_freq(unsigned long freq)
{
    int ret;

    if (!gpu_devfreq)
        return -ENODEV;

    ret = devfreq_update_user_policy(gpu_devfreq, freq, 0);
    if (ret)
        DRM_ERROR("Failed to set GPU freq to %lu Hz\n", freq);
    else
        DRM_INFO("GPU freq set to %lu Hz\n", freq);

    return ret;
}
EXPORT_SYMBOL(malidp_set_gpu_freq);

/* --- Sysfs Interface for GPU Freq Control --- */
static ssize_t cur_freq_show(struct device *dev,
                             struct device_attribute *attr, char *buf)
{
    if (!gpu_devfreq)
        return -ENODEV;
    return sysfs_emit(buf, "%lu\n", gpu_devfreq->previous_freq);
}

static ssize_t min_freq_show(struct device *dev,
                             struct device_attribute *attr, char *buf)
{
    return sysfs_emit(buf, "%lu\n", gpu_profile.min_freq);
}

static ssize_t max_freq_show(struct device *dev,
                             struct device_attribute *attr, char *buf)
{
    return sysfs_emit(buf, "%lu\n", gpu_profile.max_freq);
}

static ssize_t available_frequencies_show(struct device *dev,
                                          struct device_attribute *attr,
                                          char *buf)
{
    int i, len = 0;
    for (i = 0; i < ARRAY_SIZE(gpu_freq_table); i++)
        len += sysfs_emit_at(buf, len, "%lu ", gpu_freq_table[i]);
    len += sysfs_emit_at(buf, len, "\n");
    return len;
}

static ssize_t set_freq_store(struct device *dev,
                              struct device_attribute *attr,
                              const char *buf, size_t count)
{
    unsigned long freq;
    int ret;

    if (!gpu_devfreq)
        return -ENODEV;

    ret = kstrtoul(buf, 0, &freq);
    if (ret)
        return ret;

    ret = malidp_set_gpu_freq(freq);
    if (ret)
        return ret;

    return count;
}

static ssize_t scaling_governor_show(struct device *dev,
                                     struct device_attribute *attr, char *buf)
{
    if (!gpu_devfreq || !gpu_devfreq->governor)
        return sysfs_emit(buf, "none\n");
    return sysfs_emit(buf, "%s\n", gpu_devfreq->governor->name);
}

static DEVICE_ATTR_RO(cur_freq);
static DEVICE_ATTR_RO(min_freq);
static DEVICE_ATTR_RO(max_freq);
static DEVICE_ATTR_RO(available_frequencies);
static DEVICE_ATTR_WO(set_freq);
static DEVICE_ATTR_RO(scaling_governor);

static struct attribute *gpu_devfreq_attrs[] = {
    &dev_attr_cur_freq.attr,
    &dev_attr_min_freq.attr,
    &dev_attr_max_freq.attr,
    &dev_attr_available_frequencies.attr,
    &dev_attr_set_freq.attr,
    &dev_attr_scaling_governor.attr,
    NULL,
};

static const struct attribute_group gpu_devfreq_attr_group = {
    .attrs = gpu_devfreq_attrs,
};

static void malidp_write_gamma_table(struct malidp_hw_device *hwdev,
                                     struct drm_property_blob *blob,
                                     struct malidp_gamma *gamma_lut)
{
    int i;
    u32 __iomem *gamma_lut_regs = hwdev->map.base + MALIDP_GAMMA_TABLE;

    if (!blob)
        return;

    for (i = 0; i < gamma_lut->num_lut_entries; i++) {
        u32 r = (gamma_lut->lut[i].red >> 8) & 0xFF;
        u32 g = (gamma_lut->lut[i].green >> 8) & 0xFF;
        u32 b = (gamma_lut->lut[i].blue >> 8) & 0xFF;
        writel((r << 16) | (g << 8) | b, &gamma_lut_regs[i]);
    }
}

static void malidp_commit(struct drm_atomic_state *state)
{
    struct drm_device *drm = state->dev;
    struct malidp_drm *malidp = drm->dev_private;
    struct drm_crtc *crtc;
    struct drm_crtc_state *crtc_state;
    struct malidp_crtc_state *mcrtc_st;

    for_each_new_crtc_in_state(state, crtc, crtc_state, i) {
        if (!crtc_state->active)
            continue;

        mcrtc_st = to_malidp_crtc_state(crtc_state);
        malidp_write_gamma_table(malidp->dev, mcrtc_st->gamma_blob,
                                 &mcrtc_st->gamma);
    }
}

static irqreturn_t malidp_irq_handler(int irq, void *arg)
{
    struct drm_device *drm = arg;
    struct malidp_drm *malidp = drm->dev_private;
    u32 irq_status = readl(malidp->dev->map.base + MALIDP_INTERRUPT_STATUS);

    if (!irq_status)
        return IRQ_NONE;

    writel(irq_status, malidp->dev->map.base + MALIDP_INTERRUPT_CLEAR);

    if (irq_status & MALIDP_VSYNC_IRQ)
        drm_handle_vblank(drm, 0);

    return IRQ_HANDLED;
}

static int malidp_enable_vblank(struct drm_device *drm, unsigned int pipe)
{
    struct malidp_drm *malidp = drm->dev_private;

    writel(MALIDP_VSYNC_IRQ,
           malidp->dev->map.base + MALIDP_INTERRUPT_ENABLE);
    return 0;
}

static void malidp_disable_vblank(struct drm_device *drm, unsigned int pipe)
{
    struct malidp_drm *malidp = drm->dev_private;

    writel(0, malidp->dev->map.base + MALIDP_INTERRUPT_ENABLE);
}

DEFINE_DRM_GEM_DMA_FOPS(malidp_drm_fops);

static const struct drm_driver malidp_drm_driver = {
    .driver_features    = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
    .fops               = &malidp_drm_fops,
    .name               = "malidp",
    .desc               = "ARM Mali DP DRM driver",
    .date               = "20250101",
    .major              = 1,
    .minor              = 0,
};

static int malidp_bind(struct device *dev, struct device *master, void *data)
{
    struct drm_device *drm = data;
    struct malidp_drm *malidp;
    int ret;

    malidp = devm_kzalloc(dev, sizeof(*malidp), GFP_KERNEL);
    if (!malidp)
        return -ENOMEM;

    drm->dev_private = malidp;
    malidp->dev = dev_get_drvdata(dev);

    drm_mode_config_init(drm);
    drm->irq_enabled = true;

    ret = drm_vblank_init(drm, 1);
    if (ret)
        return ret;

    drm->mode_config.min_width  = 0;
    drm->mode_config.min_height = 0;
    drm->mode_config.max_width  = 4096;
    drm->mode_config.max_height = 4096;
    drm->mode_config.funcs      = &malidp_mode_config_funcs;

    drm_mode_config_reset(drm);

    drm_kms_helper_poll_init(drm);

    return 0;
}

static void malidp_unbind(struct device *dev, struct device *master, void *data)
{
    struct drm_device *drm = data;

    drm_kms_helper_poll_fini(drm);
    drm_mode_config_cleanup(drm);
}

static const struct component_ops malidp_component_ops = {
    .bind   = malidp_bind,
    .unbind = malidp_unbind,
};

/* --- GPU Devfreq init during probe --- */
static int malidp_devfreq_init(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    int ret;

    gpu_profile.target = NULL;
    gpu_profile.get_cur_freq = NULL;
    gpu_profile.exit = NULL;
    gpu_profile.timer = DEVFREQ_TIMER_DELAYED;
    gpu_profile.polling_ms = 1000;
    gpu_profile.initial_freq = gpu_freq_table[0];
    gpu_profile.min_freq = gpu_freq_table[0];
    gpu_profile.max_freq = gpu_freq_table[ARRAY_SIZE(gpu_freq_table) - 1];

    gpu_devfreq = devm_devfreq_add_device(dev, &gpu_profile,
                                          "userspace", NULL);
    if (IS_ERR(gpu_devfreq)) {
        DRM_ERROR("Failed to register GPU devfreq\n");
        return PTR_ERR(gpu_devfreq);
    }

    ret = sysfs_create_group(&dev->kobj, &gpu_devfreq_attr_group);
    if (ret) {
        DRM_ERROR("Failed to create GPU sysfs group\n");
        return ret;
    }

    DRM_INFO("GPU devfreq with userspace governor initialized\n");
    return 0;
}

static int malidp_probe(struct platform_device *pdev)
{
    struct drm_device *drm;
    int ret;

    drm = drm_dev_alloc(&malidp_drm_driver, &pdev->dev);
    if (IS_ERR(drm))
        return PTR_ERR(drm);

    platform_set_drvdata(pdev, drm);

    ret = component_add(&pdev->dev, &malidp_component_ops);
    if (ret) {
        drm_dev_put(drm);
        return ret;
    }

    /* Initialize GPU devfreq sysfs */
    malidp_devfreq_init(pdev);

    ret = drm_dev_register(drm, 0);
    if (ret) {
        component_del(&pdev->dev, &malidp_component_ops);
        drm_dev_put(drm);
        return ret;
    }

    drm_fbdev_generic_setup(drm, 32);

    return 0;
}

static int malidp_remove(struct platform_device *pdev)
{
    struct drm_device *drm = platform_get_drvdata(pdev);

    sysfs_remove_group(&pdev->dev.kobj, &gpu_devfreq_attr_group);
    component_del(&pdev->dev, &malidp_component_ops);
    drm_dev_unregister(drm);
    drm_dev_put(drm);

    return 0;
}

#ifdef CONFIG_PM_SLEEP
static int malidp_suspend(struct device *dev)
{
    struct drm_device *drm = dev_get_drvdata(dev);

    drm_kms_helper_poll_disable(drm);
    drm_modeset_lock_all(drm);
    drm_atomic_helper_suspend(drm);
    drm_modeset_unlock_all(drm);

    return 0;
}

static int malidp_resume(struct device *dev)
{
    struct drm_device *drm = dev_get_drvdata(dev);

    drm_modeset_lock_all(drm);
    drm_atomic_helper_resume(drm, drm->mode_config.acquire_ctx);
    drm_modeset_unlock_all(drm);
    drm_kms_helper_poll_enable(drm);

    return 0;
}
#endif

static const struct dev_pm_ops malidp_pm_ops = {
    SET_SYSTEM_SLEEP_PM_OPS(malidp_suspend, malidp_resume)
};

static const struct of_device_id malidp_of_match[] = {
    { .compatible = "arm,mali-dp500" },
    { .compatible = "arm,mali-dp550" },
    { .compatible = "arm,mali-dp650" },
    { /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, malidp_of_match);

static struct platform_driver malidp_platform_driver = {
    .probe      = malidp_probe,
    .remove     = malidp_remove,
    .driver     = {
        .name   = "malidp",
        .of_match_table = malidp_of_match,
        .pm     = &malidp_pm_ops,
    },
};

static int __init malidp_init(void)
{
    return platform_driver_register(&malidp_platform_driver);
}
module_init(malidp_init);

static void __exit malidp_exit(void)
{
    platform_driver_unregister(&malidp_platform_driver);
}
module_exit(malidp_exit);

MODULE_AUTHOR("ARM Ltd.");
MODULE_DESCRIPTION("ARM Mali Display Processor DRM driver with GPU devfreq");
MODULE_LICENSE("GPL v2");
