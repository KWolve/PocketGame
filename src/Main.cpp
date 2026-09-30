#include "entry/EasyUIContext.h"
#include "uart/UartContext.h"
#include "manager/ConfigManager.h"
#include "platform/PgFlip.h"
#include "platform/PgVideoLayer.h"
#include "utils/Log.h"
#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

void onEasyUIInit(EasyUIContext *pContext) {
	/* 程序启动第一件事：释放上一轮残留的 disp 视频层。
	 * 异常重启（崩溃/被 kill）时视频层会留着 enable，屏幕就停在上一轮画面上。
	 * 幂等，只清非 UI 层（实现见 platform/PgVideoLayer.h）。 */
	int n = pg::VideoLayer::release();
	LOGD("PocketGame: 启动图层释放 -> 清掉 %d 个残留视频层", n);

	/* ⚠️ 全局状态栏（音量 OSD 的宿主）**不在这里显示** —— 实测在 onEasyUIInit 阶段
	 * 调 `showStatusBar()` 不生效（框架的 SysApp 工厂还没就绪，日志里连"状态栏就绪"
	 * 都没有）。改在主界面主循环里**收敛式**保证，见 mainLogic.cc 的 TIMER_LOOP。 */

	// 初始化时打开串口
	UARTCONTEXT->openUart(CONFIGMANAGER->getUartName().c_str(), CONFIGMANAGER->getUartBaudRate());

	/* 整屏翻转（挂绳倒挂）状态复位：读落盘的用户意愿，并记住"框架当前角度"。
	 * ⚠️ 只读状态、**不在这里转屏** —— 真正下发由各环境页显示时每拍调用的 flipTick() 完成
	 *    （开机时还没有任何环境页在显示，此刻转过去只会让菜单倒着）。见 platform/PgFlip.h。 */
	pg::flipInit();
}

void onEasyUIDeinit(EasyUIContext *pContext) {
	UARTCONTEXT->closeUart();
}

const char* onStartupApp(EasyUIContext *pContext) {
	return "mainActivity";
}


#ifdef __cplusplus

}

#endif  /* __cplusplus */

