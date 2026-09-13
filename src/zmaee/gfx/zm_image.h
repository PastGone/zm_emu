#ifndef ZM_IMAGE_H
#define ZM_IMAGE_H

#include <stdint.h>
#include <unicorn/unicorn.h>

/* =========================================================================
 * ZMAEE IImage（图像解码）与解码产物 IBitmap 对象池
 *
 * 逆向依据（00000506 资源加载链，theme/其它 applet 同型）：
 *   IDisplay::CreateImage(alloc, free, &out)   → 新建 IImage（用给定分配器）
 *   IImage::SetData(this, 0, name_ptr, len)    → 按文件名装入图像数据
 *                                                （返回 0 成功，非 0 失败）
 *   IImage::Decode(this, alloc, free, &out, 0) → 解码成 IBitmap（返回 0 成功）
 *   IImage::Release(this)                      → 释放
 * 解码产物常被当作 sprite 直接交给 IDisplay::DrawImage / DrawBitmap 绘制。
 *
 * 模拟器实现策略：
 *   - IImage / IBitmap 都是"不透明句柄"，applet 只通过虚表调用，不直接摸字段；
 *     因此这里用固定地址的对象池（IMAGE_POOL / BITMAP_POOL），
 *     对象地址 → 宿主侧记录（像素、尺寸）用下标映射。
 *   - SetData 收到的是**文件名**（applet 用 sprintf 拼出的 "res\xxx.png"），
 *     所以这里直接读文件 → 按魔数/扩展名选 PNG / JPEG 解码器 → RGBA8888。
 *   - 解码结果同时服务于 IImage 本身（DrawImage）与 Decode 出的 IBitmap
 *     （DrawBitmap/GetInfo）。
 * ========================================================================= */

/* 按 IDisplay::CreateImage 语义创建 IImage：写入 out_ptr，返回 0。
 * r1=alloc 回调、r2=free 回调（当前实现用宿主内存，仅记录）。 */
uint32_t zm_image_CreateImage(uc_engine *uc, uint32_t r0, uint32_t r1,
                              uint32_t r2, uint32_t r3);

/* IImage 各槽（r0 = this） */
uint32_t zm_image_AddRef(uc_engine *uc, uint32_t r0);
uint32_t zm_image_Release(uc_engine *uc, uint32_t r0);
uint32_t zm_image_SetData(uc_engine *uc, uint32_t r0, uint32_t r1, uint32_t r2,
                          uint32_t r3);
uint32_t zm_image_GetFrameCount(uc_engine *uc, uint32_t r0);
uint32_t zm_image_Width(uc_engine *uc, uint32_t r0);
uint32_t zm_image_Height(uc_engine *uc, uint32_t r0);
uint32_t zm_image_GetType(uc_engine *uc, uint32_t r0);
uint32_t zm_image_DecodeToBitmap(uc_engine *uc, uint32_t r0, uint32_t r1,
                                 uint32_t r2, uint32_t r3, uint32_t sp);
uint32_t zm_image_stub(uc_engine *uc, uint32_t off, uint32_t r0, uint32_t r1,
                       uint32_t r2, uint32_t r3);

/* ---- surface 门面（IImage::Decode 的 out 对象 / entry+8）----
 * applet 对 surface 调 vt[4] 取宽、vt[8] 取高（逆向 00000506 sub_37B4）。 */
uint32_t zm_surf_addRef(uc_engine *uc, uint32_t r0);
uint32_t zm_surf_release(uc_engine *uc, uint32_t r0);
uint32_t zm_surf_wh(uc_engine *uc, uint32_t r0, uint32_t which);
uint32_t zm_surf_encode(uc_engine *uc, uint32_t r0, uint32_t r1, uint32_t r2,
                        uint32_t r3);
/* SURF_VT_ADDR+0x10 GetRect(this, out)：写 int16 矩形 {left,top,right,bottom} */
uint32_t zm_surf_getrect(uc_engine *uc, uint32_t r0, uint32_t r1);
/* 无副作用的空实现（未细究语义的槽位统一走它） */
uint32_t zm_surf_nop(uc_engine *uc, uint32_t off);

/* =========================================================================
 * ZMAEE_GDI_Surface（BitBlt 的源对象）
 *
 * 逆向（ZMAEE_Mask16To16 / Mask16To32 / Mask8To16 + GDI_BitBlt_Ext）：
 *   a4      = surface
 *   a4+8    = **像素数据指针**，其值同时作为 byte_5B658[?] 的索引，
 *             用来选出"每像素字节数"（1/2/3/4）
 *   a4+12   = 格式描述对象 v4：
 *               v4+12 = 透明色（16bpp 路径下就是 RGB565 值，如 0xF81F）
 *               v4+20 = 调色板基址（仅 8bpp 索引色路径用）
 *   Mask16To16：源像素 == 透明色则跳过（不写目标），否则直接拷贝
 *   Mask16To32：565 → ARGB8888 展开
 *   Mask8To16 ：索引值 == 透明索引则跳过，否则查调色板得 RGB565
 *
 * 运行期 dump（00000506）里的 a4 结构与此完全吻合：
 *   [0]=宽  [1]=高  [2]=**像素步进 1/2/3/4**（不是格式枚举！）
 *   [3]=透明色（实测 0xF81F）
 * 见 zm_image_get_gdi_surface / zm_image_blit_gdi_surface。
 * ========================================================================= */

/* GDI_Surface 头部长度：像素数据紧跟其后（逆向 Mask16To16 的数据布局）；
 * 头部内 +0x8 是数据指针、+0xC 是格式对象（+12 透明色、+20 调色板基址）。 */
#define GDI_SURFACE_HDR 0x1CU

/* 读 GDI_Surface 元数据；成功返回 1，*w/*h/*step(每像素字节数)/*colorkey 有效 */
int zm_image_get_gdi_surface(uc_engine *uc, uint32_t surf, int *w, int *h,
                             int *step, uint32_t *colorkey);

/* 把 GDI_Surface 贴到帧缓冲 (dx,dy)。rect_ptr = {left,top,right,bottom} 或 0
 * （整块）；mode = applet 的类型字节（镜像/翻转）。已处理返回 1。 */
int zm_image_blit_gdi_surface(uc_engine *uc, uint32_t surf, int dx, int dy,
                              uint32_t rect_ptr, int mode);

/* 取对象（entry / surface / IBitmap）对应的像素数据，供 IDisplay 绘制：
 * 成功返回 1 并填 *w/*h/*rgba（RGBA8888，w*h*4 字节）；失败返回 0。 */
int zm_image_get_pixels(uint32_t obj, int *w, int *h, const uint8_t **rgba);

/* 解码/加载像素池（PIX_POOL）分配，循环复用。0 = 失败。
 * 供 IImage::Decode 与 IDisplay::LoadBitmap 共用。 */
uint32_t zm_pix_pool_alloc(uint32_t bytes);

/* 清空所有图像对象（重新加载 applet 时调用） */
void zm_image_reset(void);

#endif /* ZM_IMAGE_H */
