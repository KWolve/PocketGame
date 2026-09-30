/***********************************************
/gen auto by zuitools
***********************************************/
#ifndef __MAINACTIVITY_H__
#define __MAINACTIVITY_H__


#include "app/Activity.h"
#include "entry/EasyUIContext.h"

#include "uart/ProtocolData.h"
#include "uart/ProtocolParser.h"

#include "utils/Log.h"
#include "control/ZKDigitalClock.h"
#include "control/ZKButton.h"
#include "control/ZKCircleBar.h"
#include "control/ZKDiagram.h"
#include "control/ZKListView.h"
#include "control/ZKPointer.h"
#include "control/ZKQRCode.h"
#include "control/ZKTextView.h"
#include "control/ZKSeekBar.h"
#include "control/ZKEditText.h"
#include "control/ZKVideoView.h"
#include "window/ZKSlideWindow.h"

/*TAG:Macro宏ID*/
#define ID_MAIN_BtnCastStop    20039
#define ID_MAIN_BarCastBottom    50049
#define ID_MAIN_TextCastMsg    50048
#define ID_MAIN_Caster    95001
#define ID_MAIN_WinCast    110007
#define ID_MAIN_TextVolHint    50047
#define ID_MAIN_BarVol    91001
#define ID_MAIN_TextVolPct    50046
#define ID_MAIN_TextVolTitle    50045
#define ID_MAIN_ImageView1    50044
#define ID_MAIN_WinVolume    110006
#define ID_MAIN_TextWifiKeyBar    50043
#define ID_MAIN_TextWifiHint    50042
#define ID_MAIN_BtnW2    20038
#define ID_MAIN_BtnW1    20037
#define ID_MAIN_BtnW0    20036
#define ID_MAIN_TextWifiRow2Value    50041
#define ID_MAIN_TextWifiRow2Label    50040
#define ID_MAIN_TextWifiRow1Value    50039
#define ID_MAIN_TextWifiRow1Label    50038
#define ID_MAIN_TextWifiRow0Value    50037
#define ID_MAIN_TextWifiRow0Label    50036
#define ID_MAIN_TextWifiPhase    50035
#define ID_MAIN_TextWifiMain    50034
#define ID_MAIN_TextWifiTitle    50033
#define ID_MAIN_BarWifi    50032
#define ID_MAIN_WinWifi    110005
#define ID_MAIN_TextCalcKeyBar    50031
#define ID_MAIN_TextCalcHint    50030
#define ID_MAIN_BtnK19    20035
#define ID_MAIN_BtnK18    20034
#define ID_MAIN_BtnK17    20033
#define ID_MAIN_BtnK16    20032
#define ID_MAIN_BtnK15    20031
#define ID_MAIN_BtnK14    20030
#define ID_MAIN_BtnK13    20029
#define ID_MAIN_BtnK12    20028
#define ID_MAIN_BtnK11    20027
#define ID_MAIN_BtnK10    20026
#define ID_MAIN_BtnK9    20025
#define ID_MAIN_BtnK8    20024
#define ID_MAIN_BtnK7    20023
#define ID_MAIN_BtnK6    20022
#define ID_MAIN_BtnK5    20021
#define ID_MAIN_BtnK4    20020
#define ID_MAIN_BtnK3    20019
#define ID_MAIN_BtnK2    20018
#define ID_MAIN_BtnK1    20017
#define ID_MAIN_BtnK0    20016
#define ID_MAIN_TextCalcMain    50029
#define ID_MAIN_TextCalcStatus    50028
#define ID_MAIN_TextCalcTitle    50027
#define ID_MAIN_BarCalc    50026
#define ID_MAIN_WinCalc    110004
#define ID_MAIN_TextClockKeyBar    50025
#define ID_MAIN_TextClockHint    50024
#define ID_MAIN_BtnU7    20015
#define ID_MAIN_BtnU6    20014
#define ID_MAIN_BtnU5    20013
#define ID_MAIN_BtnU4    20012
#define ID_MAIN_BtnU3    20011
#define ID_MAIN_BtnU2    20010
#define ID_MAIN_BtnU1    20009
#define ID_MAIN_BtnU0    20008
#define ID_MAIN_TextClockSub    50023
#define ID_MAIN_TextClockMain    50022
#define ID_MAIN_TextClockPhase    50021
#define ID_MAIN_TextClockTitle    50020
#define ID_MAIN_BarClock    50019
#define ID_MAIN_WinClock    110003
#define ID_MAIN_BtnBackList    20007
#define ID_MAIN_BtnRestart    20006
#define ID_MAIN_BtnResume    20005
#define ID_MAIN_TextPauseInfo    50018
#define ID_MAIN_TextPauseTitle    50017
#define ID_MAIN_WinPause    110002
#define ID_MAIN_TextGameKeyBar    50016
#define ID_MAIN_TextGameHint    50015
#define ID_MAIN_GameCanvas    50014
#define ID_MAIN_TextInfo2Value    50013
#define ID_MAIN_TextInfo2Label    50012
#define ID_MAIN_TextInfo1Value    50011
#define ID_MAIN_TextInfo1Label    50010
#define ID_MAIN_TextBestValue    50009
#define ID_MAIN_TextBestLabel    50008
#define ID_MAIN_TextScoreValue    50007
#define ID_MAIN_TextScoreLabel    50006
#define ID_MAIN_TextGameTitle    50005
#define ID_MAIN_WinGame    110001
#define ID_MAIN_TextVersion    50004
#define ID_MAIN_TextTip    50003
#define ID_MAIN_TextKeyHint    50002
#define ID_MAIN_SubGameBestTag    24005
#define ID_MAIN_SubGameBest    24004
#define ID_MAIN_SubGameDesc    24003
#define ID_MAIN_SubGameName    24002
#define ID_MAIN_SubGameIcon    24001
#define ID_MAIN_ListGames    80001
#define ID_MAIN_BtnSound    20004
#define ID_MAIN_BtnTab2    20003
#define ID_MAIN_BtnTab1    20002
#define ID_MAIN_BtnTab0    20001
#define ID_MAIN_TextTitle    50001
/*TAG:Macro宏ID END*/

class mainActivity : public Activity, 
                     public ZKSeekBar::ISeekBarChangeListener, 
                     public ZKListView::IItemClickListener,
                     public ZKListView::AbsListAdapter,
                     public ZKSlideWindow::ISlideItemClickListener,
                     public EasyUIContext::ITouchListener,
                     public ZKEditText::ITextChangeListener,
                     public ZKVideoView::IVideoPlayerMessageListener
{
public:
    mainActivity();
    virtual ~mainActivity();

    /**
     * 注册定时器
     */
	void registerUserTimer(int id, int time);
	/**
	 * 取消定时器
	 */
	void unregisterUserTimer(int id);
	/**
	 * 重置定时器
	 */
	void resetUserTimer(int id, int time);

protected:
    /*TAG:PROTECTED_FUNCTION*/
    virtual const char* getAppName() const;
    virtual void onCreate();
    virtual void onClick(ZKBase *pBase);
    virtual void onResume();
    virtual void onPause();
    virtual void onIntent(const Intent *intentPtr);
    virtual bool onTimer(int id);

    virtual void onProgressChanged(ZKSeekBar *pSeekBar, int progress);

    virtual int getListItemCount(const ZKListView *pListView) const;
    virtual void obtainListItemData(ZKListView *pListView, ZKListView::ZKListItem *pListItem, int index);
    virtual void onItemClick(ZKListView *pListView, int index, int subItemIndex);

    virtual void onSlideItemClick(ZKSlideWindow *pSlideWindow, int index);

    virtual bool onTouchEvent(const MotionEvent &ev);

    virtual void onTextChanged(ZKTextView *pTextView, const string &text);

    void rigesterActivityTimer();

    virtual void onVideoPlayerMessage(ZKVideoView *pVideoView, int msg);
    void videoLoopPlayback(ZKVideoView *pVideoView, int msg, size_t callbackTabIndex);
    void startVideoLoopPlayback();
    void stopVideoLoopPlayback();
    bool parseVideoFileList(const char *pFileListPath, std::vector<string>& mediaFileList);
    int removeCharFromString(string& nString, char c);


private:
    /*TAG:PRIVATE_VARIABLE*/
    int mVideoLoopIndex;
    int mVideoLoopErrorCount;

};

#endif