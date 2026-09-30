/***********************************************
/gen auto by zuitools
***********************************************/
#include "mainActivity.h"

/*TAG:GlobalVariable全局变量*/
static ZKButton* mBtnCastStopPtr;
static ZKTextView* mBarCastBottomPtr;
static ZKTextView* mTextCastMsgPtr;
static ZKVideoView* mCasterPtr;
static ZKWindow* mWinCastPtr;
static ZKTextView* mTextVolHintPtr;
static ZKSeekBar* mBarVolPtr;
static ZKTextView* mTextVolPctPtr;
static ZKTextView* mTextVolTitlePtr;
static ZKTextView* mImageView1Ptr;
static ZKWindow* mWinVolumePtr;
static ZKTextView* mTextWifiKeyBarPtr;
static ZKTextView* mTextWifiHintPtr;
static ZKButton* mBtnW2Ptr;
static ZKButton* mBtnW1Ptr;
static ZKButton* mBtnW0Ptr;
static ZKTextView* mTextWifiRow2ValuePtr;
static ZKTextView* mTextWifiRow2LabelPtr;
static ZKTextView* mTextWifiRow1ValuePtr;
static ZKTextView* mTextWifiRow1LabelPtr;
static ZKTextView* mTextWifiRow0ValuePtr;
static ZKTextView* mTextWifiRow0LabelPtr;
static ZKTextView* mTextWifiPhasePtr;
static ZKTextView* mTextWifiMainPtr;
static ZKTextView* mTextWifiTitlePtr;
static ZKTextView* mBarWifiPtr;
static ZKWindow* mWinWifiPtr;
static ZKTextView* mTextCalcKeyBarPtr;
static ZKTextView* mTextCalcHintPtr;
static ZKButton* mBtnK19Ptr;
static ZKButton* mBtnK18Ptr;
static ZKButton* mBtnK17Ptr;
static ZKButton* mBtnK16Ptr;
static ZKButton* mBtnK15Ptr;
static ZKButton* mBtnK14Ptr;
static ZKButton* mBtnK13Ptr;
static ZKButton* mBtnK12Ptr;
static ZKButton* mBtnK11Ptr;
static ZKButton* mBtnK10Ptr;
static ZKButton* mBtnK9Ptr;
static ZKButton* mBtnK8Ptr;
static ZKButton* mBtnK7Ptr;
static ZKButton* mBtnK6Ptr;
static ZKButton* mBtnK5Ptr;
static ZKButton* mBtnK4Ptr;
static ZKButton* mBtnK3Ptr;
static ZKButton* mBtnK2Ptr;
static ZKButton* mBtnK1Ptr;
static ZKButton* mBtnK0Ptr;
static ZKTextView* mTextCalcMainPtr;
static ZKTextView* mTextCalcStatusPtr;
static ZKTextView* mTextCalcTitlePtr;
static ZKTextView* mBarCalcPtr;
static ZKWindow* mWinCalcPtr;
static ZKTextView* mTextClockKeyBarPtr;
static ZKTextView* mTextClockHintPtr;
static ZKButton* mBtnU7Ptr;
static ZKButton* mBtnU6Ptr;
static ZKButton* mBtnU5Ptr;
static ZKButton* mBtnU4Ptr;
static ZKButton* mBtnU3Ptr;
static ZKButton* mBtnU2Ptr;
static ZKButton* mBtnU1Ptr;
static ZKButton* mBtnU0Ptr;
static ZKTextView* mTextClockSubPtr;
static ZKTextView* mTextClockMainPtr;
static ZKTextView* mTextClockPhasePtr;
static ZKTextView* mTextClockTitlePtr;
static ZKTextView* mBarClockPtr;
static ZKWindow* mWinClockPtr;
static ZKButton* mBtnBackListPtr;
static ZKButton* mBtnRestartPtr;
static ZKButton* mBtnResumePtr;
static ZKTextView* mTextPauseInfoPtr;
static ZKTextView* mTextPauseTitlePtr;
static ZKWindow* mWinPausePtr;
static ZKTextView* mTextGameKeyBarPtr;
static ZKTextView* mTextGameHintPtr;
static ZKTextView* mGameCanvasPtr;
static ZKTextView* mTextInfo2ValuePtr;
static ZKTextView* mTextInfo2LabelPtr;
static ZKTextView* mTextInfo1ValuePtr;
static ZKTextView* mTextInfo1LabelPtr;
static ZKTextView* mTextBestValuePtr;
static ZKTextView* mTextBestLabelPtr;
static ZKTextView* mTextScoreValuePtr;
static ZKTextView* mTextScoreLabelPtr;
static ZKTextView* mTextGameTitlePtr;
static ZKWindow* mWinGamePtr;
static ZKTextView* mTextVersionPtr;
static ZKTextView* mTextTipPtr;
static ZKTextView* mTextKeyHintPtr;
static ZKListView* mListGamesPtr;
static ZKButton* mBtnSoundPtr;
static ZKButton* mBtnTab2Ptr;
static ZKButton* mBtnTab1Ptr;
static ZKButton* mBtnTab0Ptr;
static ZKTextView* mTextTitlePtr;
static mainActivity* mActivityPtr;

/*register activity*/
REGISTER_ACTIVITY(mainActivity);

typedef struct {
	int id; // 定时器ID ， 不能重复
	int time; // 定时器  时间间隔  单位 毫秒
}S_ACTIVITY_TIMEER;

#include "logic/mainLogic.cc"

/***********/
typedef struct {
    int id;
    const char *pApp;
} SAppInfo;

/**
 *点击跳转window
 */
static SAppInfo sAppInfoTab[] = {
//  { ID_MAIN_TEXT, "TextViewActivity" },
};

/***************/
typedef bool (*ButtonCallback)(ZKButton *pButton);
/**
 * button onclick表
 */
typedef struct {
    int id;
    ButtonCallback callback;
}S_ButtonCallback;

/*TAG:ButtonCallbackTab按键映射表*/
static S_ButtonCallback sButtonCallbackTab[] = {
    ID_MAIN_BtnCastStop, onButtonClick_BtnCastStop,
    ID_MAIN_BtnW2, onButtonClick_BtnW2,
    ID_MAIN_BtnW1, onButtonClick_BtnW1,
    ID_MAIN_BtnW0, onButtonClick_BtnW0,
    ID_MAIN_BtnK19, onButtonClick_BtnK19,
    ID_MAIN_BtnK18, onButtonClick_BtnK18,
    ID_MAIN_BtnK17, onButtonClick_BtnK17,
    ID_MAIN_BtnK16, onButtonClick_BtnK16,
    ID_MAIN_BtnK15, onButtonClick_BtnK15,
    ID_MAIN_BtnK14, onButtonClick_BtnK14,
    ID_MAIN_BtnK13, onButtonClick_BtnK13,
    ID_MAIN_BtnK12, onButtonClick_BtnK12,
    ID_MAIN_BtnK11, onButtonClick_BtnK11,
    ID_MAIN_BtnK10, onButtonClick_BtnK10,
    ID_MAIN_BtnK9, onButtonClick_BtnK9,
    ID_MAIN_BtnK8, onButtonClick_BtnK8,
    ID_MAIN_BtnK7, onButtonClick_BtnK7,
    ID_MAIN_BtnK6, onButtonClick_BtnK6,
    ID_MAIN_BtnK5, onButtonClick_BtnK5,
    ID_MAIN_BtnK4, onButtonClick_BtnK4,
    ID_MAIN_BtnK3, onButtonClick_BtnK3,
    ID_MAIN_BtnK2, onButtonClick_BtnK2,
    ID_MAIN_BtnK1, onButtonClick_BtnK1,
    ID_MAIN_BtnK0, onButtonClick_BtnK0,
    ID_MAIN_BtnU7, onButtonClick_BtnU7,
    ID_MAIN_BtnU6, onButtonClick_BtnU6,
    ID_MAIN_BtnU5, onButtonClick_BtnU5,
    ID_MAIN_BtnU4, onButtonClick_BtnU4,
    ID_MAIN_BtnU3, onButtonClick_BtnU3,
    ID_MAIN_BtnU2, onButtonClick_BtnU2,
    ID_MAIN_BtnU1, onButtonClick_BtnU1,
    ID_MAIN_BtnU0, onButtonClick_BtnU0,
    ID_MAIN_BtnBackList, onButtonClick_BtnBackList,
    ID_MAIN_BtnRestart, onButtonClick_BtnRestart,
    ID_MAIN_BtnResume, onButtonClick_BtnResume,
    ID_MAIN_BtnSound, onButtonClick_BtnSound,
    ID_MAIN_BtnTab2, onButtonClick_BtnTab2,
    ID_MAIN_BtnTab1, onButtonClick_BtnTab1,
    ID_MAIN_BtnTab0, onButtonClick_BtnTab0,
};
/***************/


typedef void (*SeekBarCallback)(ZKSeekBar *pSeekBar, int progress);
typedef struct {
    int id;
    SeekBarCallback callback;
}S_ZKSeekBarCallback;
/*TAG:SeekBarCallbackTab*/
static S_ZKSeekBarCallback SZKSeekBarCallbackTab[] = {
    ID_MAIN_BarVol, onProgressChanged_BarVol,
};


typedef int (*ListViewGetItemCountCallback)(const ZKListView *pListView);
typedef void (*ListViewobtainListItemDataCallback)(ZKListView *pListView,ZKListView::ZKListItem *pListItem, int index);
typedef void (*ListViewonItemClickCallback)(ZKListView *pListView, int index, int id);
typedef struct {
    int id;
    ListViewGetItemCountCallback getListItemCountCallback;
    ListViewobtainListItemDataCallback obtainListItemDataCallback;
    ListViewonItemClickCallback onItemClickCallback;
}S_ListViewFunctionsCallback;
/*TAG:ListViewFunctionsCallback*/
static S_ListViewFunctionsCallback SListViewFunctionsCallbackTab[] = {
    ID_MAIN_ListGames, getListItemCount_ListGames, obtainListItemData_ListGames, onListItemClick_ListGames,
};


typedef void (*SlideWindowItemClickCallback)(ZKSlideWindow *pSlideWindow, int index);
typedef struct {
    int id;
    SlideWindowItemClickCallback onSlideItemClickCallback;
}S_SlideWindowItemClickCallback;
/*TAG:SlideWindowFunctionsCallbackTab*/
static S_SlideWindowItemClickCallback SSlideWindowItemClickCallbackTab[] = {
};


typedef void (*EditTextInputCallback)(const std::string &text);
typedef struct {
    int id;
    EditTextInputCallback onEditTextChangedCallback;
}S_EditTextInputCallback;
/*TAG:EditTextInputCallback*/
static S_EditTextInputCallback SEditTextInputCallbackTab[] = {
};

typedef void (*VideoViewCallback)(ZKVideoView *pVideoView, int msg);
typedef struct {
    int id; //VideoView ID
    bool loop; // 是否是轮播类型
    int defaultvolume;//轮播类型时,默认视频音量
    VideoViewCallback onVideoViewCallback;
}S_VideoViewCallback;
/*TAG:VideoViewCallback*/
static S_VideoViewCallback SVideoViewCallbackTab[] = {
    ID_MAIN_Caster, false, 8, onVideoViewPlayerMessageListener_Caster,
};


mainActivity::mainActivity() {
	//todo add init code here
	mVideoLoopIndex = -1;
	mVideoLoopErrorCount = 0;
}

mainActivity::~mainActivity() {
  //todo add init file here
  // 退出应用时需要反注册
    EASYUICONTEXT->unregisterGlobalTouchListener(this);
    unregisterProtocolDataUpdateListener(onProtocolDataUpdate);
    onUI_quit();
    mActivityPtr = NULL;
    mBtnCastStopPtr = NULL;
    mBarCastBottomPtr = NULL;
    mTextCastMsgPtr = NULL;
    mCasterPtr = NULL;
    mWinCastPtr = NULL;
    mTextVolHintPtr = NULL;
    mBarVolPtr = NULL;
    mTextVolPctPtr = NULL;
    mTextVolTitlePtr = NULL;
    mImageView1Ptr = NULL;
    mWinVolumePtr = NULL;
    mTextWifiKeyBarPtr = NULL;
    mTextWifiHintPtr = NULL;
    mBtnW2Ptr = NULL;
    mBtnW1Ptr = NULL;
    mBtnW0Ptr = NULL;
    mTextWifiRow2ValuePtr = NULL;
    mTextWifiRow2LabelPtr = NULL;
    mTextWifiRow1ValuePtr = NULL;
    mTextWifiRow1LabelPtr = NULL;
    mTextWifiRow0ValuePtr = NULL;
    mTextWifiRow0LabelPtr = NULL;
    mTextWifiPhasePtr = NULL;
    mTextWifiMainPtr = NULL;
    mTextWifiTitlePtr = NULL;
    mBarWifiPtr = NULL;
    mWinWifiPtr = NULL;
    mTextCalcKeyBarPtr = NULL;
    mTextCalcHintPtr = NULL;
    mBtnK19Ptr = NULL;
    mBtnK18Ptr = NULL;
    mBtnK17Ptr = NULL;
    mBtnK16Ptr = NULL;
    mBtnK15Ptr = NULL;
    mBtnK14Ptr = NULL;
    mBtnK13Ptr = NULL;
    mBtnK12Ptr = NULL;
    mBtnK11Ptr = NULL;
    mBtnK10Ptr = NULL;
    mBtnK9Ptr = NULL;
    mBtnK8Ptr = NULL;
    mBtnK7Ptr = NULL;
    mBtnK6Ptr = NULL;
    mBtnK5Ptr = NULL;
    mBtnK4Ptr = NULL;
    mBtnK3Ptr = NULL;
    mBtnK2Ptr = NULL;
    mBtnK1Ptr = NULL;
    mBtnK0Ptr = NULL;
    mTextCalcMainPtr = NULL;
    mTextCalcStatusPtr = NULL;
    mTextCalcTitlePtr = NULL;
    mBarCalcPtr = NULL;
    mWinCalcPtr = NULL;
    mTextClockKeyBarPtr = NULL;
    mTextClockHintPtr = NULL;
    mBtnU7Ptr = NULL;
    mBtnU6Ptr = NULL;
    mBtnU5Ptr = NULL;
    mBtnU4Ptr = NULL;
    mBtnU3Ptr = NULL;
    mBtnU2Ptr = NULL;
    mBtnU1Ptr = NULL;
    mBtnU0Ptr = NULL;
    mTextClockSubPtr = NULL;
    mTextClockMainPtr = NULL;
    mTextClockPhasePtr = NULL;
    mTextClockTitlePtr = NULL;
    mBarClockPtr = NULL;
    mWinClockPtr = NULL;
    mBtnBackListPtr = NULL;
    mBtnRestartPtr = NULL;
    mBtnResumePtr = NULL;
    mTextPauseInfoPtr = NULL;
    mTextPauseTitlePtr = NULL;
    mWinPausePtr = NULL;
    mTextGameKeyBarPtr = NULL;
    mTextGameHintPtr = NULL;
    mGameCanvasPtr = NULL;
    mTextInfo2ValuePtr = NULL;
    mTextInfo2LabelPtr = NULL;
    mTextInfo1ValuePtr = NULL;
    mTextInfo1LabelPtr = NULL;
    mTextBestValuePtr = NULL;
    mTextBestLabelPtr = NULL;
    mTextScoreValuePtr = NULL;
    mTextScoreLabelPtr = NULL;
    mTextGameTitlePtr = NULL;
    mWinGamePtr = NULL;
    mTextVersionPtr = NULL;
    mTextTipPtr = NULL;
    mTextKeyHintPtr = NULL;
    mListGamesPtr = NULL;
    mBtnSoundPtr = NULL;
    mBtnTab2Ptr = NULL;
    mBtnTab1Ptr = NULL;
    mBtnTab0Ptr = NULL;
    mTextTitlePtr = NULL;
}

const char* mainActivity::getAppName() const{
	return "main.ftu";
}

//TAG:onCreate
void mainActivity::onCreate() {
	Activity::onCreate();
    mBtnCastStopPtr = (ZKButton*)findControlByID(ID_MAIN_BtnCastStop);
    mBarCastBottomPtr = (ZKTextView*)findControlByID(ID_MAIN_BarCastBottom);
    mTextCastMsgPtr = (ZKTextView*)findControlByID(ID_MAIN_TextCastMsg);
    mCasterPtr = (ZKVideoView*)findControlByID(ID_MAIN_Caster);if(mCasterPtr!= NULL){mCasterPtr->setVideoPlayerMessageListener(this);}
    mWinCastPtr = (ZKWindow*)findControlByID(ID_MAIN_WinCast);
    mTextVolHintPtr = (ZKTextView*)findControlByID(ID_MAIN_TextVolHint);
    mBarVolPtr = (ZKSeekBar*)findControlByID(ID_MAIN_BarVol);if(mBarVolPtr!= NULL){mBarVolPtr->setSeekBarChangeListener(this);}
    mTextVolPctPtr = (ZKTextView*)findControlByID(ID_MAIN_TextVolPct);
    mTextVolTitlePtr = (ZKTextView*)findControlByID(ID_MAIN_TextVolTitle);
    mImageView1Ptr = (ZKTextView*)findControlByID(ID_MAIN_ImageView1);
    mWinVolumePtr = (ZKWindow*)findControlByID(ID_MAIN_WinVolume);
    mTextWifiKeyBarPtr = (ZKTextView*)findControlByID(ID_MAIN_TextWifiKeyBar);
    mTextWifiHintPtr = (ZKTextView*)findControlByID(ID_MAIN_TextWifiHint);
    mBtnW2Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnW2);
    mBtnW1Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnW1);
    mBtnW0Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnW0);
    mTextWifiRow2ValuePtr = (ZKTextView*)findControlByID(ID_MAIN_TextWifiRow2Value);
    mTextWifiRow2LabelPtr = (ZKTextView*)findControlByID(ID_MAIN_TextWifiRow2Label);
    mTextWifiRow1ValuePtr = (ZKTextView*)findControlByID(ID_MAIN_TextWifiRow1Value);
    mTextWifiRow1LabelPtr = (ZKTextView*)findControlByID(ID_MAIN_TextWifiRow1Label);
    mTextWifiRow0ValuePtr = (ZKTextView*)findControlByID(ID_MAIN_TextWifiRow0Value);
    mTextWifiRow0LabelPtr = (ZKTextView*)findControlByID(ID_MAIN_TextWifiRow0Label);
    mTextWifiPhasePtr = (ZKTextView*)findControlByID(ID_MAIN_TextWifiPhase);
    mTextWifiMainPtr = (ZKTextView*)findControlByID(ID_MAIN_TextWifiMain);
    mTextWifiTitlePtr = (ZKTextView*)findControlByID(ID_MAIN_TextWifiTitle);
    mBarWifiPtr = (ZKTextView*)findControlByID(ID_MAIN_BarWifi);
    mWinWifiPtr = (ZKWindow*)findControlByID(ID_MAIN_WinWifi);
    mTextCalcKeyBarPtr = (ZKTextView*)findControlByID(ID_MAIN_TextCalcKeyBar);
    mTextCalcHintPtr = (ZKTextView*)findControlByID(ID_MAIN_TextCalcHint);
    mBtnK19Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnK19);
    mBtnK18Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnK18);
    mBtnK17Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnK17);
    mBtnK16Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnK16);
    mBtnK15Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnK15);
    mBtnK14Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnK14);
    mBtnK13Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnK13);
    mBtnK12Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnK12);
    mBtnK11Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnK11);
    mBtnK10Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnK10);
    mBtnK9Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnK9);
    mBtnK8Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnK8);
    mBtnK7Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnK7);
    mBtnK6Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnK6);
    mBtnK5Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnK5);
    mBtnK4Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnK4);
    mBtnK3Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnK3);
    mBtnK2Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnK2);
    mBtnK1Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnK1);
    mBtnK0Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnK0);
    mTextCalcMainPtr = (ZKTextView*)findControlByID(ID_MAIN_TextCalcMain);
    mTextCalcStatusPtr = (ZKTextView*)findControlByID(ID_MAIN_TextCalcStatus);
    mTextCalcTitlePtr = (ZKTextView*)findControlByID(ID_MAIN_TextCalcTitle);
    mBarCalcPtr = (ZKTextView*)findControlByID(ID_MAIN_BarCalc);
    mWinCalcPtr = (ZKWindow*)findControlByID(ID_MAIN_WinCalc);
    mTextClockKeyBarPtr = (ZKTextView*)findControlByID(ID_MAIN_TextClockKeyBar);
    mTextClockHintPtr = (ZKTextView*)findControlByID(ID_MAIN_TextClockHint);
    mBtnU7Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnU7);
    mBtnU6Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnU6);
    mBtnU5Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnU5);
    mBtnU4Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnU4);
    mBtnU3Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnU3);
    mBtnU2Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnU2);
    mBtnU1Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnU1);
    mBtnU0Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnU0);
    mTextClockSubPtr = (ZKTextView*)findControlByID(ID_MAIN_TextClockSub);
    mTextClockMainPtr = (ZKTextView*)findControlByID(ID_MAIN_TextClockMain);
    mTextClockPhasePtr = (ZKTextView*)findControlByID(ID_MAIN_TextClockPhase);
    mTextClockTitlePtr = (ZKTextView*)findControlByID(ID_MAIN_TextClockTitle);
    mBarClockPtr = (ZKTextView*)findControlByID(ID_MAIN_BarClock);
    mWinClockPtr = (ZKWindow*)findControlByID(ID_MAIN_WinClock);
    mBtnBackListPtr = (ZKButton*)findControlByID(ID_MAIN_BtnBackList);
    mBtnRestartPtr = (ZKButton*)findControlByID(ID_MAIN_BtnRestart);
    mBtnResumePtr = (ZKButton*)findControlByID(ID_MAIN_BtnResume);
    mTextPauseInfoPtr = (ZKTextView*)findControlByID(ID_MAIN_TextPauseInfo);
    mTextPauseTitlePtr = (ZKTextView*)findControlByID(ID_MAIN_TextPauseTitle);
    mWinPausePtr = (ZKWindow*)findControlByID(ID_MAIN_WinPause);
    mTextGameKeyBarPtr = (ZKTextView*)findControlByID(ID_MAIN_TextGameKeyBar);
    mTextGameHintPtr = (ZKTextView*)findControlByID(ID_MAIN_TextGameHint);
    mGameCanvasPtr = (ZKTextView*)findControlByID(ID_MAIN_GameCanvas);
    mTextInfo2ValuePtr = (ZKTextView*)findControlByID(ID_MAIN_TextInfo2Value);
    mTextInfo2LabelPtr = (ZKTextView*)findControlByID(ID_MAIN_TextInfo2Label);
    mTextInfo1ValuePtr = (ZKTextView*)findControlByID(ID_MAIN_TextInfo1Value);
    mTextInfo1LabelPtr = (ZKTextView*)findControlByID(ID_MAIN_TextInfo1Label);
    mTextBestValuePtr = (ZKTextView*)findControlByID(ID_MAIN_TextBestValue);
    mTextBestLabelPtr = (ZKTextView*)findControlByID(ID_MAIN_TextBestLabel);
    mTextScoreValuePtr = (ZKTextView*)findControlByID(ID_MAIN_TextScoreValue);
    mTextScoreLabelPtr = (ZKTextView*)findControlByID(ID_MAIN_TextScoreLabel);
    mTextGameTitlePtr = (ZKTextView*)findControlByID(ID_MAIN_TextGameTitle);
    mWinGamePtr = (ZKWindow*)findControlByID(ID_MAIN_WinGame);
    mTextVersionPtr = (ZKTextView*)findControlByID(ID_MAIN_TextVersion);
    mTextTipPtr = (ZKTextView*)findControlByID(ID_MAIN_TextTip);
    mTextKeyHintPtr = (ZKTextView*)findControlByID(ID_MAIN_TextKeyHint);
    mListGamesPtr = (ZKListView*)findControlByID(ID_MAIN_ListGames);if(mListGamesPtr!= NULL){mListGamesPtr->setListAdapter(this);mListGamesPtr->setItemClickListener(this);}
    mBtnSoundPtr = (ZKButton*)findControlByID(ID_MAIN_BtnSound);
    mBtnTab2Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnTab2);
    mBtnTab1Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnTab1);
    mBtnTab0Ptr = (ZKButton*)findControlByID(ID_MAIN_BtnTab0);
    mTextTitlePtr = (ZKTextView*)findControlByID(ID_MAIN_TextTitle);
	mActivityPtr = this;
	onUI_init();
  registerProtocolDataUpdateListener(onProtocolDataUpdate);
  rigesterActivityTimer();
}

void mainActivity::onClick(ZKBase *pBase) {
	//TODO: add widget onClik code 
    int buttonTablen = sizeof(sButtonCallbackTab) / sizeof(S_ButtonCallback);
    for (int i = 0; i < buttonTablen; ++i) {
        if (sButtonCallbackTab[i].id == pBase->getID()) {
            if (sButtonCallbackTab[i].callback((ZKButton*)pBase)) {
            	return;
            }
            break;
        }
    }


    int len = sizeof(sAppInfoTab) / sizeof(sAppInfoTab[0]);
    for (int i = 0; i < len; ++i) {
        if (sAppInfoTab[i].id == pBase->getID()) {
            EASYUICONTEXT->openActivity(sAppInfoTab[i].pApp);
            return;
        }
    }

	Activity::onClick(pBase);
}

void mainActivity::onResume() {
	Activity::onResume();
	EASYUICONTEXT->registerGlobalTouchListener(this);
	startVideoLoopPlayback();
	onUI_show();
}

void mainActivity::onPause() {
	Activity::onPause();
	EASYUICONTEXT->unregisterGlobalTouchListener(this);
	stopVideoLoopPlayback();
	onUI_hide();
}

void mainActivity::onIntent(const Intent *intentPtr) {
	Activity::onIntent(intentPtr);
	onUI_intent(intentPtr);
}

bool mainActivity::onTimer(int id) {
	return onUI_Timer(id);
}

void mainActivity::onProgressChanged(ZKSeekBar *pSeekBar, int progress){

    int seekBarTablen = sizeof(SZKSeekBarCallbackTab) / sizeof(S_ZKSeekBarCallback);
    for (int i = 0; i < seekBarTablen; ++i) {
        if (SZKSeekBarCallbackTab[i].id == pSeekBar->getID()) {
            SZKSeekBarCallbackTab[i].callback(pSeekBar, progress);
            break;
        }
    }
}

int mainActivity::getListItemCount(const ZKListView *pListView) const{
    int tablen = sizeof(SListViewFunctionsCallbackTab) / sizeof(S_ListViewFunctionsCallback);
    for (int i = 0; i < tablen; ++i) {
        if (SListViewFunctionsCallbackTab[i].id == pListView->getID()) {
            return SListViewFunctionsCallbackTab[i].getListItemCountCallback(pListView);
            break;
        }
    }
    return 0;
}

void mainActivity::obtainListItemData(ZKListView *pListView,ZKListView::ZKListItem *pListItem, int index){
    int tablen = sizeof(SListViewFunctionsCallbackTab) / sizeof(S_ListViewFunctionsCallback);
    for (int i = 0; i < tablen; ++i) {
        if (SListViewFunctionsCallbackTab[i].id == pListView->getID()) {
            SListViewFunctionsCallbackTab[i].obtainListItemDataCallback(pListView, pListItem, index);
            break;
        }
    }
}

void mainActivity::onItemClick(ZKListView *pListView, int index, int id){
    int tablen = sizeof(SListViewFunctionsCallbackTab) / sizeof(S_ListViewFunctionsCallback);
    for (int i = 0; i < tablen; ++i) {
        if (SListViewFunctionsCallbackTab[i].id == pListView->getID()) {
            SListViewFunctionsCallbackTab[i].onItemClickCallback(pListView, index, id);
            break;
        }
    }
}

void mainActivity::onSlideItemClick(ZKSlideWindow *pSlideWindow, int index) {
    int tablen = sizeof(SSlideWindowItemClickCallbackTab) / sizeof(S_SlideWindowItemClickCallback);
    for (int i = 0; i < tablen; ++i) {
        if (SSlideWindowItemClickCallbackTab[i].id == pSlideWindow->getID()) {
            SSlideWindowItemClickCallbackTab[i].onSlideItemClickCallback(pSlideWindow, index);
            break;
        }
    }
}

bool mainActivity::onTouchEvent(const MotionEvent &ev) {
    /* ★ 2026-09-15 实验：先让框架把触摸照常分发（不做任何拦截），
     *   验证"控件收不到触摸"是否由这里造成。 */
    (void)ev;
    return false;
}

void mainActivity::onTextChanged(ZKTextView *pTextView, const std::string &text) {
    int tablen = sizeof(SEditTextInputCallbackTab) / sizeof(S_EditTextInputCallback);
    for (int i = 0; i < tablen; ++i) {
        if (SEditTextInputCallbackTab[i].id == pTextView->getID()) {
            SEditTextInputCallbackTab[i].onEditTextChangedCallback(text);
            break;
        }
    }
}

void mainActivity::rigesterActivityTimer() {
    int tablen = sizeof(REGISTER_ACTIVITY_TIMER_TAB) / sizeof(S_ACTIVITY_TIMEER);
    for (int i = 0; i < tablen; ++i) {
        S_ACTIVITY_TIMEER temp = REGISTER_ACTIVITY_TIMER_TAB[i];
        registerTimer(temp.id, temp.time);
    }
}


void mainActivity::onVideoPlayerMessage(ZKVideoView *pVideoView, int msg) {
    int tablen = sizeof(SVideoViewCallbackTab) / sizeof(S_VideoViewCallback);
    for (int i = 0; i < tablen; ++i) {
        if (SVideoViewCallbackTab[i].id == pVideoView->getID()) {
        	if (SVideoViewCallbackTab[i].loop) {
                //循环播放
        		videoLoopPlayback(pVideoView, msg, i);
        	} else if (SVideoViewCallbackTab[i].onVideoViewCallback != NULL){
        	    SVideoViewCallbackTab[i].onVideoViewCallback(pVideoView, msg);
        	}
            break;
        }
    }
}

void mainActivity::videoLoopPlayback(ZKVideoView *pVideoView, int msg, size_t callbackTabIndex) {

	switch (msg) {
	case ZKVideoView::E_MSGTYPE_VIDEO_PLAY_STARTED:
		LOGD("ZKVideoView::E_MSGTYPE_VIDEO_PLAY_STARTED\n");
    if (callbackTabIndex >= (sizeof(SVideoViewCallbackTab)/sizeof(S_VideoViewCallback))) {
      break;
    }
		pVideoView->setVolume(SVideoViewCallbackTab[callbackTabIndex].defaultvolume / 10.0);
		mVideoLoopErrorCount = 0;
		break;
	case ZKVideoView::E_MSGTYPE_VIDEO_PLAY_ERROR:
		/**错误处理 */
		++mVideoLoopErrorCount;
		if (mVideoLoopErrorCount > 100) {
			LOGD("video loop error counts > 100, quit loop playback !");
            break;
		} //不用break, 继续尝试播放下一个
	case ZKVideoView::E_MSGTYPE_VIDEO_PLAY_COMPLETED:
		LOGD("ZKVideoView::E_MSGTYPE_VIDEO_PLAY_COMPLETED\n");
        std::vector<std::string> videolist;
        std::string fileName(getAppName());
        if (fileName.size() < 4) {
             LOGD("getAppName size < 4, ignore!");
             break;
        }
        fileName = fileName.substr(0, fileName.length() - 4) + "_video_list.txt";
        fileName = "/mnt/extsd/" + fileName;
        if (!parseVideoFileList(fileName.c_str(), videolist)) {
            LOGD("parseVideoFileList failed !");
		    break;
        }
		if (pVideoView && !videolist.empty()) {
			mVideoLoopIndex = (mVideoLoopIndex + 1) % videolist.size();
			pVideoView->play(videolist[mVideoLoopIndex].c_str());
		}
		break;
	}
}

void mainActivity::startVideoLoopPlayback() {
    int tablen = sizeof(SVideoViewCallbackTab) / sizeof(S_VideoViewCallback);
    for (int i = 0; i < tablen; ++i) {
    	if (SVideoViewCallbackTab[i].loop) {
    		ZKVideoView* videoView = (ZKVideoView*)findControlByID(SVideoViewCallbackTab[i].id);
    		if (!videoView) {
    			return;
    		}
    		//循环播放
    		videoLoopPlayback(videoView, ZKVideoView::E_MSGTYPE_VIDEO_PLAY_COMPLETED, i);
    		return;
    	}
    }
}

void mainActivity::stopVideoLoopPlayback() {
    int tablen = sizeof(SVideoViewCallbackTab) / sizeof(S_VideoViewCallback);
    for (int i = 0; i < tablen; ++i) {
    	if (SVideoViewCallbackTab[i].loop) {
    		ZKVideoView* videoView = (ZKVideoView*)findControlByID(SVideoViewCallbackTab[i].id);
    		if (!videoView) {
    			return;
    		}
    		if (videoView->isPlaying()) {
    		    videoView->stop();
    		}
    		return;
    	}
    }
}

bool mainActivity::parseVideoFileList(const char *pFileListPath, std::vector<string>& mediaFileList) {
	mediaFileList.clear();
	if (NULL == pFileListPath || 0 == strlen(pFileListPath)) {
        LOGD("video file list is null!");
		return false;
	}

	ifstream is(pFileListPath, ios_base::in);
	if (!is.is_open()) {
		LOGD("cann't open file %s \n", pFileListPath);
		return false;
	}
	char tmp[1024] = {0};
	while (is.getline(tmp, sizeof(tmp))) {
		string str = tmp;
		removeCharFromString(str, '\"');
		removeCharFromString(str, '\r');
		removeCharFromString(str, '\n');
		if (str.size() > 1) {
     		mediaFileList.push_back(str.c_str());
		}
	}
  LOGD("(f:%s, l:%d) parse fileList[%s], get [%d]files", __FUNCTION__,
      __LINE__, pFileListPath, int(mediaFileList.size()));
  for (std::vector<string>::size_type i = 0; i < mediaFileList.size(); i++) {
    LOGD("file[%d]:[%s]", int(i), mediaFileList[i].c_str());
  }
	is.close();

	return true;
}

int mainActivity::removeCharFromString(string& nString, char c) {
    string::size_type   pos;
    while(1) {
        pos = nString.find(c);
        if(pos != string::npos) {
            nString.erase(pos, 1);
        } else {
            break;
        }
    }
    return (int)nString.size();
}

void mainActivity::registerUserTimer(int id, int time) {
	registerTimer(id, time);
}

void mainActivity::unregisterUserTimer(int id) {
	unregisterTimer(id);
}

void mainActivity::resetUserTimer(int id, int time) {
	resetTimer(id, time);
}