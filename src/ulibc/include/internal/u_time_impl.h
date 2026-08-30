#ifndef U_TIME_IMPL_H
#define U_TIME_IMPL_H
/**
 * @file u_time_impl.h
 * @brief 时间模块内部共享的 struct tm 读写
 *
 * 客户机 struct tm 布局与宿主不同，必须逐字段搬运。
 * 供 localtime / gmtime / mktime / asctime / strftime 共用。
 */
#include <stdint.h>
#include <time.h>
#include <unicorn/unicorn.h>

/** 宿主 struct tm → 客户机 tm_ptr（9 个 int32） */
void u_tm_store(uc_engine *uc, uint32_t tm_ptr, const struct tm *t);

/** 客户机 tm_ptr → 宿主 struct tm */
void u_tm_load(uc_engine *uc, uint32_t tm_ptr, struct tm *t);

#endif /* U_TIME_IMPL_H */
