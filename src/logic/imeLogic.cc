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
 static bool onimeActivityTouchEvent(const MotionEvent &ev) {
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
static bool onButtonClick_BtnProbeK1(ZKButton* pButton) {
  LOGD_TRACE("BtnProbeK1 click");
  return false;
}

static bool onButtonClick_BtnKbClear(ZKButton* pButton) {
  LOGD_TRACE("BtnKbClear click");
  return false;
}

static bool onButtonClick_BtnKbHide(ZKButton* pButton) {
  LOGD_TRACE("BtnKbHide click");
  return false;
}

static bool onButtonClick_BtnK01(ZKButton* pButton) {
  LOGD_TRACE("BtnK01 click");
  return false;
}

static bool onButtonClick_BtnK02(ZKButton* pButton) {
  LOGD_TRACE("BtnK02 click");
  return false;
}

static bool onButtonClick_BtnK03(ZKButton* pButton) {
  LOGD_TRACE("BtnK03 click");
  return false;
}

static bool onButtonClick_BtnK04(ZKButton* pButton) {
  LOGD_TRACE("BtnK04 click");
  return false;
}

static bool onButtonClick_BtnK05(ZKButton* pButton) {
  LOGD_TRACE("BtnK05 click");
  return false;
}

static bool onButtonClick_BtnK06(ZKButton* pButton) {
  LOGD_TRACE("BtnK06 click");
  return false;
}

static bool onButtonClick_BtnK07(ZKButton* pButton) {
  LOGD_TRACE("BtnK07 click");
  return false;
}

static bool onButtonClick_BtnK08(ZKButton* pButton) {
  LOGD_TRACE("BtnK08 click");
  return false;
}

static bool onButtonClick_BtnK09(ZKButton* pButton) {
  LOGD_TRACE("BtnK09 click");
  return false;
}

static bool onButtonClick_BtnK10(ZKButton* pButton) {
  LOGD_TRACE("BtnK10 click");
  return false;
}

static bool onButtonClick_BtnK11(ZKButton* pButton) {
  LOGD_TRACE("BtnK11 click");
  return false;
}

static bool onButtonClick_BtnK12(ZKButton* pButton) {
  LOGD_TRACE("BtnK12 click");
  return false;
}

static bool onButtonClick_BtnK13(ZKButton* pButton) {
  LOGD_TRACE("BtnK13 click");
  return false;
}

static bool onButtonClick_BtnK14(ZKButton* pButton) {
  LOGD_TRACE("BtnK14 click");
  return false;
}

static bool onButtonClick_BtnK15(ZKButton* pButton) {
  LOGD_TRACE("BtnK15 click");
  return false;
}

static bool onButtonClick_BtnK16(ZKButton* pButton) {
  LOGD_TRACE("BtnK16 click");
  return false;
}

static bool onButtonClick_BtnK17(ZKButton* pButton) {
  LOGD_TRACE("BtnK17 click");
  return false;
}

static bool onButtonClick_BtnK18(ZKButton* pButton) {
  LOGD_TRACE("BtnK18 click");
  return false;
}

static bool onButtonClick_BtnK19(ZKButton* pButton) {
  LOGD_TRACE("BtnK19 click");
  return false;
}

static bool onButtonClick_BtnK20(ZKButton* pButton) {
  LOGD_TRACE("BtnK20 click");
  return false;
}

static bool onButtonClick_BtnK21(ZKButton* pButton) {
  LOGD_TRACE("BtnK21 click");
  return false;
}

static bool onButtonClick_BtnK22(ZKButton* pButton) {
  LOGD_TRACE("BtnK22 click");
  return false;
}

static bool onButtonClick_BtnK23(ZKButton* pButton) {
  LOGD_TRACE("BtnK23 click");
  return false;
}

static bool onButtonClick_BtnK24(ZKButton* pButton) {
  LOGD_TRACE("BtnK24 click");
  return false;
}

static bool onButtonClick_BtnK25(ZKButton* pButton) {
  LOGD_TRACE("BtnK25 click");
  return false;
}

static bool onButtonClick_BtnK26(ZKButton* pButton) {
  LOGD_TRACE("BtnK26 click");
  return false;
}

static bool onButtonClick_BtnK27(ZKButton* pButton) {
  LOGD_TRACE("BtnK27 click");
  return false;
}

static bool onButtonClick_BtnK28(ZKButton* pButton) {
  LOGD_TRACE("BtnK28 click");
  return false;
}

static bool onButtonClick_BtnK29(ZKButton* pButton) {
  LOGD_TRACE("BtnK29 click");
  return false;
}

static bool onButtonClick_BtnK30(ZKButton* pButton) {
  LOGD_TRACE("BtnK30 click");
  return false;
}

static bool onButtonClick_BtnK31(ZKButton* pButton) {
  LOGD_TRACE("BtnK31 click");
  return false;
}

static bool onButtonClick_BtnK32(ZKButton* pButton) {
  LOGD_TRACE("BtnK32 click");
  return false;
}

static bool onButtonClick_BtnK33(ZKButton* pButton) {
  LOGD_TRACE("BtnK33 click");
  return false;
}

static bool onButtonClick_BtnK34(ZKButton* pButton) {
  LOGD_TRACE("BtnK34 click");
  return false;
}

static bool onButtonClick_BtnK35(ZKButton* pButton) {
  LOGD_TRACE("BtnK35 click");
  return false;
}

static bool onButtonClick_BtnK36(ZKButton* pButton) {
  LOGD_TRACE("BtnK36 click");
  return false;
}

static bool onButtonClick_BtnK37(ZKButton* pButton) {
  LOGD_TRACE("BtnK37 click");
  return false;
}

static bool onButtonClick_BtnK38(ZKButton* pButton) {
  LOGD_TRACE("BtnK38 click");
  return false;
}

static bool onButtonClick_BtnK39(ZKButton* pButton) {
  LOGD_TRACE("BtnK39 click");
  return false;
}

static bool onButtonClick_BtnK40(ZKButton* pButton) {
  LOGD_TRACE("BtnK40 click");
  return false;
}

static bool onButtonClick_BtnK41(ZKButton* pButton) {
  LOGD_TRACE("BtnK41 click");
  return false;
}

static bool onButtonClick_BtnK42(ZKButton* pButton) {
  LOGD_TRACE("BtnK42 click");
  return false;
}

static bool onButtonClick_BtnK43(ZKButton* pButton) {
  LOGD_TRACE("BtnK43 click");
  return false;
}

static bool onButtonClick_BtnK44(ZKButton* pButton) {
  LOGD_TRACE("BtnK44 click");
  return false;
}

static bool onButtonClick_BtnK45(ZKButton* pButton) {
  LOGD_TRACE("BtnK45 click");
  return false;
}

static bool onButtonClick_BtnCand0(ZKButton* pButton) {
  LOGD_TRACE("BtnCand0 click");
  return false;
}

static bool onButtonClick_BtnCand1(ZKButton* pButton) {
  LOGD_TRACE("BtnCand1 click");
  return false;
}

static bool onButtonClick_BtnCand2(ZKButton* pButton) {
  LOGD_TRACE("BtnCand2 click");
  return false;
}

static bool onButtonClick_BtnCand3(ZKButton* pButton) {
  LOGD_TRACE("BtnCand3 click");
  return false;
}

static bool onButtonClick_BtnCand4(ZKButton* pButton) {
  LOGD_TRACE("BtnCand4 click");
  return false;
}

static bool onButtonClick_BtnCand5(ZKButton* pButton) {
  LOGD_TRACE("BtnCand5 click");
  return false;
}

static bool onButtonClick_BtnKbMode(ZKButton* pButton) {
  LOGD_TRACE("BtnKbMode click");
  return false;
}

static bool onButtonClick_BtnKbPageUp(ZKButton* pButton) {
  LOGD_TRACE("BtnKbPageUp click");
  return false;
}

static bool onButtonClick_BtnKbPageDown(ZKButton* pButton) {
  LOGD_TRACE("BtnKbPageDown click");
  return false;
}


