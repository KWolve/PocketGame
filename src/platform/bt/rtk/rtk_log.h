/* rtk_log.h —— 给移植过来的 Realtek hciattach/rtb_fwc 代码提供 LOGx 宏
 * （原工程从 config.h / 平台日志头拿；探针工程里用 printf 顶上）
 * 注意：这些宏是 GNU C 变参写法，RS_xxx(fmt, arg...) 会展开成 LOGD(fmt, ##arg)。 */
#ifndef RTK_LOG_H
#define RTK_LOG_H
#include <stdio.h>
#define LOGD(fmt, ...) do { printf("[rtk] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } while (0)
#define LOGI(fmt, ...) do { printf("[rtk] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } while (0)
#define LOGW(fmt, ...) do { printf("[rtk] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } while (0)
#define LOGE(fmt, ...) do { printf("[rtk!] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } while (0)
#endif
