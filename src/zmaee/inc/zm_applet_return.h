#ifndef ZM_APPLET_RETURN_H
#define ZM_APPLET_RETURN_H

/**
 * @file zm_applet_return.h
 * @brief ZM 框架事件处理返回值定义
 *
 * 来源：各 .app.lst 及 zmsys001.dll.lst 反汇编分析
 * 这些返回值由 applet 入口函数返回给框架，指示事件是否被处理以及后续行为。
 *
 * 注意：不同模块可能使用额外的负值（如 -2、-3）作为内部错误码，
 *       但以下定义为框架层面通用的返回值。
 */

/* 或使用枚举（推荐） */
typedef enum {
  ZMAEE_RET_OK = 0,         /**< 处理成功，结束 */
  ZMAEE_RET_CONTINUE = 1,   /**< 处理成功，但继续框架默认流程 */
  ZMAEE_RET_UNHANDLED = -1, /**< 未处理，交由框架 */
  ZMAEE_RET_ERR_INV = -4    /**< 错误：无效上下文 */
} zmaee_ret_t;

#endif /* _ZM_APPLET_RETURN_H_ */
