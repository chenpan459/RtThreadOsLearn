/*
 * FAL auto-init for QEMU vexpress-a9.
 */
#include <fal.h>

static int drv_fal_init(void)
{
    fal_init();
    return 0;
}
INIT_COMPONENT_EXPORT(drv_fal_init);
