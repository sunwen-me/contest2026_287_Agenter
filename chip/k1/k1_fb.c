/****************************************************************************
 * vendor/spacemit/chips/k1/k1_fb.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#if defined(CONFIG_K1_FB) && defined(CONFIG_VIDEO_FB)

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/kmalloc.h>
#include <nuttx/video/fb.h>

#include "k1_cache.h"
#include "k1_fb.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#if CONFIG_K1_FB_BPP == 16
#  define K1_FB_FORMAT FB_FMT_RGB16_565
#elif CONFIG_K1_FB_BPP == 32
#  define K1_FB_FORMAT FB_FMT_RGB32
#else
#  error "K1 framebuffer supports only 16 or 32 bits per pixel"
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct k1_fb_s
{
  struct fb_vtable_s      vtable;
  struct fb_planeinfo_s   planeinfo;
  struct fb_videoinfo_s   videoinfo;
  int                     power;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int k1_fb_getvideoinfo(FAR struct fb_vtable_s *vtable,
                              FAR struct fb_videoinfo_s *vinfo)
{
  FAR struct k1_fb_s *fb = (FAR struct k1_fb_s *)vtable;

  if (vinfo == NULL)
    {
      return -EINVAL;
    }

  memcpy(vinfo, &fb->videoinfo, sizeof(*vinfo));
  return OK;
}

static int k1_fb_getplaneinfo(FAR struct fb_vtable_s *vtable, int planeno,
                              FAR struct fb_planeinfo_s *pinfo)
{
  FAR struct k1_fb_s *fb = (FAR struct k1_fb_s *)vtable;

  if (planeno != 0 || pinfo == NULL)
    {
      return -EINVAL;
    }

  memcpy(pinfo, &fb->planeinfo, sizeof(*pinfo));
  return OK;
}

static int k1_fb_pandisplay(FAR struct fb_vtable_s *vtable,
                            FAR struct fb_planeinfo_s *pinfo)
{
  FAR struct k1_fb_s *fb = (FAR struct k1_fb_s *)vtable;
  uintptr_t start;

  if (pinfo == NULL || pinfo->xoffset != 0 || pinfo->yoffset != 0)
    {
      return -EINVAL;
    }

  /* The U-Boot framebuffer is a single buffer.  Clean the CPU cache so the
   * display engine sees writes made through /dev/fb0.  No DPU register is
   * touched here; the display pipeline is intentionally inherited from
   * U-Boot in this first bring-up profile.
   */

  start = (uintptr_t)fb->planeinfo.fbmem;
  k1_dcache_clean(start, fb->planeinfo.fblen);
  return OK;
}

#ifdef CONFIG_FB_UPDATE
static int k1_fb_updatearea(FAR struct fb_vtable_s *vtable,
                             FAR const struct fb_area_s *area)
{
  FAR struct k1_fb_s *fb = (FAR struct k1_fb_s *)vtable;
  uintptr_t start;
  size_t length;
  size_t bytes_per_pixel;

  if (area == NULL || area->w == 0 || area->h == 0 ||
      area->x >= fb->videoinfo.xres || area->y >= fb->videoinfo.yres ||
      area->w > fb->videoinfo.xres - area->x ||
      area->h > fb->videoinfo.yres - area->y)
    {
      return -EINVAL;
    }

  bytes_per_pixel = fb->planeinfo.bpp / 8;
  start = (uintptr_t)fb->planeinfo.fbmem +
          (size_t)area->y * fb->planeinfo.stride +
          (size_t)area->x * bytes_per_pixel;
  length = ((size_t)area->h - 1) * fb->planeinfo.stride +
           (size_t)area->w * bytes_per_pixel;

  /* The display engine reads from memory independently of the CPU cache.
   * Clean the changed rectangle, including each row's intervening bytes.
   */

  k1_dcache_clean(start, length);
  return OK;
}
#endif

static int k1_fb_getpower(FAR struct fb_vtable_s *vtable)
{
  FAR struct k1_fb_s *fb = (FAR struct k1_fb_s *)vtable;

  return fb->power;
}

static int k1_fb_setpower(FAR struct fb_vtable_s *vtable, int power)
{
  FAR struct k1_fb_s *fb = (FAR struct k1_fb_s *)vtable;

  if (power < 0)
    {
      return -EINVAL;
    }

  /* There is no panel/backlight GPIO description in the NuttX profile yet.
   * Retain the requested logical state without changing the inherited DPU
   * pipeline.
   */

  fb->power = power;
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int k1_fb_initialize(int display)
{
  FAR struct k1_fb_s *fb;
  int ret;

  fb = kmm_zalloc(sizeof(*fb));
  if (fb == NULL)
    {
      return -ENOMEM;
    }

  fb->videoinfo.fmt     = K1_FB_FORMAT;
  fb->videoinfo.xres   = CONFIG_K1_FB_XRES;
  fb->videoinfo.yres   = CONFIG_K1_FB_YRES;
  fb->videoinfo.nplanes = 1;

  fb->planeinfo.fbmem       = (FAR void *)(uintptr_t)CONFIG_K1_FB_BASE;
  fb->planeinfo.bpp         = CONFIG_K1_FB_BPP;
  fb->planeinfo.stride      = CONFIG_K1_FB_XRES * (CONFIG_K1_FB_BPP / 8);
  fb->planeinfo.fblen       = (size_t)fb->planeinfo.stride *
                              CONFIG_K1_FB_YRES;
  fb->planeinfo.display     = display;
  fb->planeinfo.xres_virtual = CONFIG_K1_FB_XRES;
  fb->planeinfo.yres_virtual = CONFIG_K1_FB_YRES;

  fb->vtable.getvideoinfo = k1_fb_getvideoinfo;
  fb->vtable.getplaneinfo = k1_fb_getplaneinfo;
  fb->vtable.pandisplay   = k1_fb_pandisplay;
#ifdef CONFIG_FB_UPDATE
  fb->vtable.updatearea   = k1_fb_updatearea;
#endif
  fb->vtable.getpower     = k1_fb_getpower;
  fb->vtable.setpower     = k1_fb_setpower;
  fb->power               = 1;

  ret = fb_register_device(display, 0, &fb->vtable);
  if (ret < 0)
    {
      kmm_free(fb);
    }
  else
    {
      /* fb_register_device() clears the inherited buffer as it creates
       * /dev/fb0.  Make that initial state visible to the inherited DPU.
       */

      k1_dcache_clean((uintptr_t)fb->planeinfo.fbmem,
                      fb->planeinfo.fblen);
    }

  return ret;
}

#endif /* CONFIG_K1_FB && CONFIG_VIDEO_FB */
