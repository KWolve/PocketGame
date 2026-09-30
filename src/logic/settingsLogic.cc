#ifdef FUN_BUILD
#include GENERATED_UI_DEFINITIONS
INIT_UI_EVENT_BINDINGS
#endif // FUN_BUILD

/**
 * @brief 当界面构造时触发
 */
static void onUI_init() {
  LOGD_TRACE("");
}

/**
 * @brief 当切换到该界面时触发
 */
static void onUI_intent(const Intent *intent) {
  LOGD_TRACE("");
  if (intent != NULL) {
  }
}

/**
 * @brief 当界面显示时触发
 */
static void onUI_show() {
  LOGD_TRACE("");
}

/**
 * @brief 当界面隐藏时触发
 */
static void onUI_hide() {
  LOGD_TRACE("");
}

/**
 * @brief 当界面完全退出时触发
 */
static void onUI_quit() {
  LOGD_TRACE("");
}

/**
 * @brief 串口数据回调接口
 */
static void onProtocolDataUpdate(const SProtocolData &data) {
  LOGD_TRACE("");
}

/**
 * @brief 定时器回调函数, 不要在此函数中写耗时操作, 否则将影响UI刷新
 * @param id 当前所触发定时器的id, 与注册时的id相同
 * @return true  继续运行当前定时器
 *         false 停止运行当前定时器
 */
static bool onUI_Timer(int id) {
  LOGD_TRACE("on timer %d", id);
  switch (id) {

  default:
    break;
  }
  return true;
}


/**
 * @brief 有新的触摸事件时触发
 * @param ev 触摸事件
 * @return true 表示该触摸事件在此被拦截，系统不再将此触摸事件传递到控件上
 *         false 触摸事件将继续传递到控件上
 */
 static bool onsettingsActivityTouchEvent(const MotionEvent &ev) {
  switch (ev.mActionStatus) {
  case MotionEvent::E_ACTION_DOWN: // 触摸按下
    // LOGD_TRACE("时刻 = %ld 坐标  x = %d, y = %d", ev.mEventTime, ev.mX, ev.mY);
    break;
  case MotionEvent::E_ACTION_MOVE: // 触摸滑动
    break;
  case MotionEvent::E_ACTION_UP: // 触摸抬起
    break;
  default:
    break;
  }
  return false;
}
static bool onButtonClick_BtnSetBack(ZKButton* pButton) {
  LOGD_TRACE("BtnSetBack click");
  return false;
}

static bool onButtonClick_BtnSetSound(ZKButton* pButton) {
  LOGD_TRACE("BtnSetSound click");
  return false;
}

static bool onButtonClick_BtnSetWifi(ZKButton* pButton) {
  LOGD_TRACE("BtnSetWifi click");
  return false;
}

static bool onButtonClick_BtnSetWifiPage(ZKButton* pButton) {
  LOGD_TRACE("BtnSetWifiPage click");
  return false;
}

static void onProgressChanged_BarSetVol(ZKSeekBar *pSeekBar, int progress) {
  LOGD_TRACE("BarSetVol progress changed %d", progress);
}

static bool onButtonClick_BtnSetVolDown(ZKButton* pButton) {
  LOGD_TRACE("BtnSetVolDown click");
  return false;
}

static bool onButtonClick_BtnSetVolUp(ZKButton* pButton) {
  LOGD_TRACE("BtnSetVolUp click");
  return false;
}

static void onProgressChanged_BarSetBright(ZKSeekBar *pSeekBar, int progress) {
  LOGD_TRACE("BarSetBright progress changed %d", progress);
}

static bool onButtonClick_BtnSetBrightDown(ZKButton* pButton) {
  LOGD_TRACE("BtnSetBrightDown click");
  return false;
}

static bool onButtonClick_BtnSetBrightUp(ZKButton* pButton) {
  LOGD_TRACE("BtnSetBrightUp click");
  return false;
}


