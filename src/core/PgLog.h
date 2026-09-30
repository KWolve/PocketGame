/*
 * PgLog.h - core 层的最小日志钩子
 *
 * 为什么要有它：core/*（画布 / 游戏 / 贴图仓库）刻意**不依赖 easyui**（可在 PC 上编译），
 * 所以不能用框架的 LOGD。但"素材尺寸与清单不符""贴图加载失败"这类问题**必须留下痕迹**
 * —— 它们画出来只是差几个像素或缺个元素，肉眼很容易当成"风格如此"。
 *
 * 用法：platform 侧（mainLogic）启动时 `pg::setLogSink(...)` 把日志接到框架的 LOGD；
 * 没设 sink 时静默丢弃（PC/离屏验收不需要日志）。
 */
#ifndef PG_LOG_H_
#define PG_LOG_H_

namespace pg {

/* 日志接收器：收到的是**已经格式化好**的一行文本（不带换行）。 */
typedef void (*LogSink)(const char *msg);

void setLogSink(LogSink sink);

/* printf 风格。没装 sink 时不做任何事（连格式化都省了）。 */
void logInfo(const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

}  // namespace pg

#endif  // PG_LOG_H_
