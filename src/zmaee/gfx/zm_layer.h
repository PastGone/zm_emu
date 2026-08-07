#ifndef ZM_LAYER_H
#define ZM_LAYER_H

/* -------------------- ZMAEE 图层 / 图片子系统 --------------------
 *
 * 真机上 AEE_IDisplay 是"多图层 + 直接帧缓冲"模型：
 *   createLayer(id, rect)        申请一层
 *   setActiveLayer(id)           后续绘制指令作用于该层
 *   getLayerInfo(id, info)       把该层的像素缓冲指针交给 applet，
 *                                applet 可以绕过所有绘制 API 直接写像素
 *   updateLayer(id, x, y, w, h)  把该层刷到屏幕
 *
 * 0000050b 与 00000506 正是靠 getLayerInfo 拿到缓冲后自己软件渲染的，
 * 所以模拟器必须真的提供一块【客户机可见】的像素内存，而不能只在
 * 宿主机侧用 SDL 画。因此这里把每一层都实现成客户机堆上的
 * RGB565 缓冲，所有绘制原语都用软件光栅化写进去；
 * SDL 只负责最后把各层合成、显示与截图。
 */

#include <stdbool.h>
#include <stdint.h>
#include <unicorn/unicorn.h>

#define ZM_MAX_LAYERS 8

/* 层信息结构（applet 侧 0x34 字节）里我们会填写的字段偏移 —
 * 由 0000050b(sub@0xC64) 与 00000506(sub@0x102F4) 的用法反推得到。 */
#define ZM_LAYERINFO_W 0x0C
#define ZM_LAYERINFO_H 0x10
#define ZM_LAYERINFO_BUF 0x24
#define ZM_LAYERINFO_SIZE 0x34

/* 可绘制描述符（applet 侧 0x20 字节），00000506 用它调用 drawImage */
#define ZM_IMGDESC_W 0x00
#define ZM_IMGDESC_H 0x04
#define ZM_IMGDESC_FMT 0x08
#define ZM_IMGDESC_BUF 0x1C
#define ZM_IMGDESC_SIZE 0x20

/* 客户机 IImage 对象布局（我们自己分配在堆上） */
#define ZM_IMG_VPTR 0x00
#define ZM_IMG_W 0x04
#define ZM_IMG_H 0x08
#define ZM_IMG_BUF 0x0C   /* RGB565 像素 */
#define ZM_IMG_MASK 0x10  /* 1 字节/像素的不透明掩码，0=透明 */
#define ZM_IMG_MAGIC 0x14 /* 校验字，确认这是我们造的对象 */
#define ZM_IMG_SIZE 0x20
#define ZM_IMG_MAGIC_VAL 0x5A4D4931U /* "ZMI1" */

typedef struct {
  bool used;
  int w, h;
  uint32_t buf; /* 客户机 RGB565 缓冲地址 */
  bool opaque; /* 整层已 clear/fill 过 → 不透明画布，黑色(0)是真实颜色；
                * 否则 0 像素在合成时视为透明（overlay 层） */
} ZmLayer;

/* ---- 生命周期 ---- */
void zm_layer_reset(int screen_w, int screen_h);

/* ---- 层管理（GFX_VT） ---- */
uint32_t zm_gfx_create_layer(uc_engine *uc, uint32_t id, uint32_t rect_ptr);
uint32_t zm_gfx_free_layers(uc_engine *uc);
uint32_t zm_gfx_active_layer(uc_engine *uc, uint32_t id);
uint32_t zm_gfx_layer_info(uc_engine *uc, uint32_t id, uint32_t info_ptr);
uint32_t zm_gfx_update_layer(uc_engine *uc, uint32_t id, uint32_t x,
                             uint32_t y, uint32_t sp);
uint32_t zm_gfx_get_active_layer(uc_engine *uc);
uint32_t zm_gfx_clear_layer(uc_engine *uc, uint32_t id, uint32_t color);

/* ---- 图片（GFX_VT / IMAGE_VT） ---- */
uint32_t zm_gfx_image_new(uc_engine *uc, uint32_t out_ptr);
uint32_t zm_gfx_image_file(uc_engine *uc, uint32_t path_ptr, uint32_t out_ptr);
uint32_t zm_gfx_draw_image(uc_engine *uc, uint32_t x, uint32_t y,
                           uint32_t obj_ptr, uint32_t rect_ptr);
uint32_t zm_img_release(uc_engine *uc, uint32_t img);
uint32_t zm_img_load_file(uc_engine *uc, uint32_t img, uint32_t path_ptr,
                          uint32_t path_len);
uint32_t zm_img_get_size(uc_engine *uc, uint32_t img, uint32_t out_ptr);
uint32_t zm_img_make_desc(uc_engine *uc, uint32_t img, uint32_t out_ptr);

/* ---- 供 zm_gfx.c 内部使用的软件光栅化原语 ---- */
ZmLayer *zm_layer_active(void);
ZmLayer *zm_layer_get(uint32_t id);
void zm_layer_fill(uc_engine *uc, ZmLayer *L, int x, int y, int w, int h,
                   uint32_t argb);
void zm_layer_frame(uc_engine *uc, ZmLayer *L, int x, int y, int w, int h,
                    uint32_t argb);
void zm_layer_blit_argb(uc_engine *uc, ZmLayer *L, int dx, int dy,
                        const uint32_t *src, int sw, int sh, int pitch_px,
                        int clip_x, int clip_y, int clip_w, int clip_h);
/* 把所有已用图层按序合成到宿主机 ARGB8888 帧缓冲 */
void zm_layer_composite(uc_engine *uc, uint32_t *out, int w, int h);
/* 是否有任何一层被真正创建过（决定摘要里"画面"是否有意义） */
bool zm_layer_any(void);

#endif /* ZM_LAYER_H */
