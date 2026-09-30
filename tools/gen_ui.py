#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gen_ui.py - UI 生成流水线（ui/main.html -> ui/main.json -> ui/main.ftu）

为什么需要这个脚本（而不是直接调 html2json）：
  1) html2json 把 .card 当"初始可见"，把 .modal 当"初始隐藏 + 模态"。
     游戏页 WinGame 需要「初始隐藏」但**不能是模态**——模态窗口会吞掉所有外部输入
     （FlyThings 的 modal 弹窗是输入黑洞，实测点/键都不透传），而游戏页是整屏页面语义。
     所以这里在 html2json 之后做一次字段修正。
  2) 一并调用 ui/fui.exe pack，保证 .ftu 与 .json 同步（设备实际加载的是 ftu）。

用法: python tools/gen_ui.py [--no-pack]
"""
import glob
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ios_theme import TOKENS, btn_asset_for  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
UI_DIR = os.path.join(ROOT, "ui")
FUI = os.path.join(UI_DIR, "fui.exe")
# html2json.py（HTML → EasyUI json 的转换器）属于 FlyThings 工具链，**本仓库不附带**。
# 用环境变量 PG_HTML2JSON 指向它；未设置时按常见安装位置兜底，仍找不到会在用到时明确报错
# （见 docs/DEPENDENCIES.md）。
HTML2JSON = os.environ.get(
    "PG_HTML2JSON", r"D:\zkswe\flythings-mcp-open\ui_tools\html2json.py")

# 多 ftu：每个 <name>.html -> <name>.json -> <name>.ftu（一个 Activity 一个 ftu）
#   main.html   -> main.ftu   主界面/游戏/工具/音量OSD
#   wifi.html   -> wifi.ftu   WiFi 独立应用（主页+探针+密码弹窗）
#   remote.html -> remote.ftu 蓝牙遥控独立应用（遥控页 + 学习页）
#   ime.html    -> ime.ftu    自定义输入法布局（APP_TYPE_SYS_IME，见 src/logic/imeApp.cc）
# ⚠️ 约定：**一个独立功能 = 一个独立 ftu + src/logic/<name>Logic.cc**（名字与 ftu 同名，
#    fun 会自动把 <name>Logic.cc 绑到生成的 ui_<name>.h）。这样功能之间互不干扰，
#    逻辑也各管一份，好维护。
# 主界面拆分（2026-09-14）：游戏不动，其余功能界面各自一个 ftu。
# 新增一个功能页 = 这里 +1 个 html + src/logic/<页名>Logic.cc（fun 自动绑定）。
UI_SOURCES = ["main.html", "wifi.html", "remote.html", "ime.html", "screensaver.html",
              "stopwatch.html", "pomodoro.html", "timer.html", "calc.html", "react.html",
              "clocksuite.html", "iptv.html",
              # 信号探针独立应用（WiFi 探测 + 蓝牙探测 + 热点猎手）。
              # 原来它是 wifi.ftu 里的一个二级 window（WinProbe），已提升为独立应用。
              "probe.html",
              # 2026-09-16 新增两个工具应用：
              #   网络收音机（radio.ftu / radioActivity）—— 电台列表 + 播放页
              #   局域网摄像头查看（camera.ftu / cameraActivity）—— 扫描页 + 查看页
              "radio.html", "camera.html",
              # 全局状态栏（SysApp：音量 OSD）。fun 会自动注册成 APP_TYPE_SYS_STATUSBAR，
              # 见 .fun/v85x/generated/ui_statusbar.cpp —— 加这一行就够了。
              "statusbar.html",
              # 全局导航栏（SysApp：WiFi 连接状态常显）。fun 自动注册成
              # APP_TYPE_SYS_NAVIBAR（见 generated/ui_navibar.cpp）。
              # ★ 它的窗口是 **480x26**（不是整屏）—— 常显浮层只有这么小才不会
              #   吃掉全机触摸，理由写在 ui/navibar.html 的注释里。
              "navibar.html",
              # 系统设置（2026-09-16）：音效 / WiFi / 音量 / 背光 四项收纳页，
              # 独立 ftu + 独立 Activity（kAppTable slot 32，系统分类）。
              "settings.html",
              # ★ 2026-09-23：fairy.html / kitten.html **已下线**（用户要求去掉
              #   "电子宠物/小精灵/飞天仙女/可爱小猫"这 4 个 APP 省空间）。
              #   同时删掉的还有：ui/fairy.* + ui/kitten.*、src/logic/{fairy,kitten}Logic.cc、
              #   src/ui/MoviePage.{h,cpp}（只被这两页用）、resources/media/{fairy,kitten}.mp4
              #   （两个片源合计 860KB，是 /res 省下来的大头之一）。
              #   电子宠物/小精灵**本来就没有 ftu**（它们跑在宿主画布上，是 Game 子类）⇒
              #   这里没有它们的条目，删的时候改的是 kAppTable 与贴图目录。
              # 2026-09-23 新增：智能家居（Home Assistant 遥控器）。
              #   ha.html -> ha.ftu / haActivity（kAppTable slot 37，系统分类）
              #   三个整屏 window（P2 起）：
              #     · WinHaHome —— 我的设备（**2 列 x 4 行卡片**，默认显示页）
              #     · WinHaPick / WinHaName / WinHaDone —— 添加向导三步
              #     · WinHaMenu / WinHaConfirm —— 卡片操作菜单 / 移除确认（弹层）
              #     · WinHaSet —— 设置（调试动作收在这里）
              #   ⚠️ 后两个必须同时进 HIDDEN_PAGE_WINDOWS（.window 生成出来是
              #      visible:true，不列的话进页三页叠在一起）**和** OPAQUE_WINDOWS
              #      （窗口 data-bg 会被写成 -1 透明 ⇒ 会透出上一页的残留）。
              #   ⚠️ WinHaHome 是默认页，**不能**进 HIDDEN_PAGE_WINDOWS（列了进页一片空白）。
              "ha.html"]

# 需要「初始隐藏但非模态」的整屏窗口
#   WinGame  = 画布类应用（游戏）
#   WinClock = 原生控件类工具（番茄钟/定时器/秒表）
#   WinCalc  = 原生控件类工具（计算器）
#   WinProbeLan / WinProbeSniff = 信号探针的局域网页 / 嗅探页（初始隐藏）
#   WinRemoteLearn = 蓝牙遥控·学习页（独立 remote.ftu 里的整屏窗口，初始隐藏）
# 都必须是普通 window 而不是 modal —— modal 窗口是输入黑洞，会吞掉所有外部输入
# （实测点/键都不透传），整屏页面用普通 window 语义。
HIDDEN_PAGE_WINDOWS = ["WinProbeLan",   # 信号探针：局域网设备页（初始隐藏，页签切）
                       "WinProbeSniff",  # 信号探针：无线嗅探页（初始隐藏，页签切）
                       "WinGame", "WinClock", "WinCalc", "WinReact",
                       # ⚠️ WinClockSuite 已搬到 clocksuite.html 且是**默认显示的主页**，
                       #    不能列在这里（否则进页是一片空白）。WinWorld 仍要隐藏。
                       "WinWorld", "WinAlarmRing",
                       # ⚠️ WinVolume 已删（音量 OSD 收归全局状态栏 statusbar.html，见 statusbarLogic.cc）
                       "WinRemoteLearn", "WinCast",
                       # IPTV 播放页（见 docs/iptv.md）。
                       # ⚠️ WinIptv（选台页）已随 iptv.ftu 独立，而且是新页的**默认显示页**
                       #    ⇒ 不能列在这里（列了进 iptv 页就是一片空白）。
                       "WinIptvPlay",
                       # wifi.ftu 的密码弹窗：原来是 modal（小盒子），现改成
                       # **整屏 window + 半屏软键盘**（modal 是输入黑洞，键盘铺在
                       # 弹窗盒子之外会点不动）。
                       # ⚠️ 必须列在这里：html2json 对 .window 实际给的是
                       #   **visible:true**（不是它文档里写的"默认隐藏"），
                       #   不显式关掉的话**开机就把密码框显示出来**（实测踩到）。
                       "WinWifiPwd",
                       # probe.ftu 的两个二级页（默认显示的是 WinProbeWifi，不列）。
                       # ⚠️ 同样必须列：.window 生成出来是 visible:true，
                       #    不关的话进探针页三页叠在一起。见 probeLogic.cc。
                       "WinProbeBt", "WinHunt",
                       # 2026-09-16 新增两页的二级窗口（整屏 window，初始隐藏）：
                       #   WinRadioPlay —— 收音机播放页（默认显示的是列表页 WinRadio）
                       #   WinCamView   —— 摄像头查看页（默认显示的是扫描页 WinCam）
                       # ⚠️ 两页的**主窗口**（WinRadio / WinCam）都不能列在这里，
                       #    列了进页就是一片空白。
                       "WinRadioPlay", "WinCamView",
                       # camera.ftu 的**账号密码弹窗**（2026-09-17）：整屏 window，初始隐藏。
                       # ⚠️ 同 WinWifiPwd 的理由：.window 生成出来是 visible:true
                       #    （不是它文档里写的"默认隐藏"），不列在这里**开机就会把登录框
                       #    显示出来**。见 ui/camera.html 与 cameraLogic.cc。
                       "WinCamPwd",
                       # 2026-09-23 ha.ftu（智能家居）的两个二级页。
                       # ⚠️ 默认显示的是 WinHaHome，**它不能列在这里**（列了进页一片空白）。
                       # ★ 2026-09-23 交互改版：WinHaScan/WinHaEdit 已被
                       #   WinHaPick（向导①选择）/ WinHaName（向导②命名）/ WinHaDone（向导③完成）
                       #   取代，另加设置页 WinHaSet。
                       # ⚠️⚠️ **WinHaMenu / WinHaConfirm 绝不能列在这里**（2026-09-24）：
                       #   本列表会把窗口强制改成 `visible=false **modal=false**`，
                       #   而这两个弹层必须**保持 modal=true**（画在背景页之上、背景列表可见）。
                       #   它们本来就是 `class="modal"` ⇒ html2json 已给 visible=false，
                       #   不需要这里再补；写进来反而会把 modal 打掉（实测：弹层后面只剩黑）。
                       "WinHaPick", "WinHaName", "WinHaDone", "WinHaSet"]

# 初始隐藏的普通控件（不是整页窗口）。
# ⚠️ 电池相关的条目（BattBolt / BattShell / BattFill / BattNub…）**已于 2026-09-16 全部删除**：
#    电量显示搬到全局导航栏 navibar，且改成"一枚整图 = 一个状态"
#    （`images/batt_<色>_<档>.png`，由 ios_theme.gen_battery() 烘，运行时只换图不定位）。
#    所以这里**不再需要**电池的初始隐藏项 —— 别再往这张表里加回去。
# 卡片圆形图标：item 模板里"应用数"个同位置重叠的控件，运行时由 logic 的 syncRowIcon()
# 按 slot 只显示一个。**首帧必须全部隐藏**，否则会看到一堆图标叠在一起。
# ⚠️ 数量必须 == kIconCount（mainLogic.cc）== 应用总数（现在 38）—— 加应用时四处同步。
# ★ 2026-09-23：37 -> **38**（新增 slot 37 智能家居）。四处 = ui/main.html 的 Icon37 /
#   tools/gen_icons.py 的 ICONS / mainLogic.cc 的 kIconCount / 本文件这一行。
# 视频区"缓冲底图"（ImgIptvCover / ImgCastCover / ImgCamCover）：只在拉清单/等首帧期间显示，
# 出画立刻隐藏。**首帧必须隐藏**：UI 层在视频层之上，开机就露出来会一直挡着视频。
# ⚠️ 用括号包起来（不能靠反斜杠续行 + 行内注释，那是语法错误 —— 已踩）。
HIDDEN_CONTROLS = (["Icon%d" % i for i in range(38)] +
                   # 视频区"缓冲底图"：只在拉清单/等首帧期间显示，出画立刻隐藏。
                   # ⚠️ 2026-09-20：fairy/kitten 两页各有一张**不透明整屏**的 ImgMovieCover，
                   #   首帧必须隐藏（否则它压住 videoview，视频永远看不见 —— 逻辑层在
                   #   onUI_init 里立刻 showCover(true) 重新点亮，见 src/ui/MoviePage.cpp）。
                   #   ★ 2026-09-23：随 fairy/kitten 下线**已删**（那两个控件与 MoviePage 一起没了）。
                   # ha.ftu（智能家居）主页里**两组互斥的浮层**，开机都必须隐藏：
                   #   · 连接失败状态卡（只有断连时显示）
                   #   · 空态组（只有"我的设备为空"时显示）
                   # ⚠️ 2026-09-23 实测：不列这里的话它们开机就可见 —— 而逻辑层的
                   #    "变化检测"因为初值与控件实际状态一致，**第一次同步会被跳过**
                   #    ⇒ 卡片上盖着空态文字（抓屏抓到 (124,410) 是文字色就是这个原因）。
                   ["CardHaOffline", "TextHaOffL1", "TextHaOffL2", "TextHaOffL3", "BtnHaOffSet",
                    "ImgHaEmpty", "TextHaEmpT", "TextHaEmpL1", "TextHaEmpL2", "TextHaEmpL3",
                    "BtnHaEmpAdd", "BtnHaEmpOne", "TextHaEmpHint"] +
                   ["ImgIptvCover", "ImgCastCover", "ImgCamCover"] +
                   # 全局音量面板：开机隐藏，音量变化时由 statusbarLogic.cc 弹出、1.6s 自动收起
                   ["WinSbVolume"] +
                   # ★ 音量面板的两个图标按钮**叠在同一格**，运行时按静音态只显示一个
                   #   （见 statusbar.cc 的 updateMuteVisual）。两个都先隐藏：
                   #   否则开机第一帧会看到两枚图标叠在一起。
                   #   ⚠️ 为什么不"一个按钮 + setBackgroundPic 换图"：PgSkin.h 记着
                   #     运行时 setBackgroundPic **不保留 alpha**，透明底会被渲染成纯白，
                   #     线框图标会变成一整块白方块。
                   ["BtnSbVolIcon", "BtnSbVolIconMute"] +
                   # WiFi 列表的「整组卡」四段角：同一位置叠了 4 张图，运行时由
                   # obtainListItemData_ListWifiAp 按 index 只显示一张（首/中/末/独）。
                   # 首帧只留中间行那张（上下都是直角，最不显眼），另三张必须先藏起来，
                   # 否则会看到几张底图叠在一起。
                   ["RowCardTop", "RowCardBot", "RowCardSolo"] +
                   # WiFi 列表行右侧的信号强度图标：四档同样叠在同一位置，按 rssi 只显示一张。
                   # 首帧留最强的 SubApSig3（看不清是"叠图"），另三张藏起来。
                   ["SubApSig2", "SubApSig1", "SubApSig0"] +
                   # ★ 屏保砖块钟（2026-09-18）：马里奥 4 帧同位置重叠，运行时按相位只显一帧
                   #   ⇒ 首帧必须全藏（否则看到 4 个马里奥叠在一起）。
                   #   背景两套（白天/夜空）也叠在同一处，首帧留白天、藏夜空，
                   #   运行首帧由 logic 按小时改正（夜间的第一帧可能闪一下白天，可接受）。
                   ["MarioW0", "MarioW1", "MarioW2", "MarioJump", "SaverBgNight"] +
                   # ★ 收音机播放页的"视图切换"（2026-09-19）：频谱组 vs 双指针 VU 表**同一区域互相显隐**。
                   #   默认视图 = 表盘 ⇒ 首帧藏"频谱组 + 未选中的那半枚页签"：
                   #     · TabVuOff（"表盘"的暗态，只有在频谱视图下才显示）
                   #     · TabSpecOn（"频谱"的亮态，同上）
                   #     · EqB00..EqB29 + RpFloor/RpLevel/RpLevelBg（整条频谱与电平条）
                   #   ⚠️ 与之对称：表盘组（VuFaceL/R、VuNeedleL/R、TextVuDbL/R）**不能**藏，
                   #      它们就是首帧要显示的东西。
                   ["TabVuOff", "TabSpecOn"] +
                   ["EqB%02d" % i for i in range(30)] +
                   ["RpFloor", "RpLevel", "RpLevelBg"])


def patch_listviews(data):
    """清掉 listview.item 的默认占位文本。

    html2json 给 listview.item 硬编码了 'text': 'ListItem'（vendor 默认值）。
    我们的卡片内容全部由 subItem + 逻辑层 setText 提供，这个默认文本会直接画在
    卡片上（真机上能看到列表里有个 "ListItem" 字样），必须清空。
    """
    n = 0

    def walk(node):
        nonlocal n
        if isinstance(node, dict):
            it = node.get("item")
            if isinstance(it, dict) and "subItem" in it and it.get("text"):
                print("patch listview %s: item.text %r -> ''"
                      % (node.get("caption"), it["text"]))
                it["text"] = ""
                n += 1
            for v in node.values():
                walk(v)
        elif isinstance(node, list):
            for v in node:
                walk(v)

    walk(data)
    if n == 0:
        print("listview item 默认文本：无需清理")
    return n


def find_by_caption(data, caption):
    """在 json 的顶层与嵌套 window 里按 caption 找控件节点"""
    if isinstance(data, dict):
        if data.get("caption") == caption:
            return data
        for v in data.values():
            r = find_by_caption(v, caption)
            if r is not None:
                return r
    elif isinstance(data, list):
        for v in data:
            r = find_by_caption(v, caption)
            if r is not None:
                return r
    return None


# 音量进度条（ZKSeekBar）的轨道图 / 填充图
# ⚠️ ZKSeekBar 不给图就什么都不画（实测：只有卡片、图标、百分比、提示文字出得来，
#    进度条位置一片空白）。html2json 只认 data-track/data-fill 指向的图片文件，
#    不会从颜色合成，所以图必须自己产出到 resources/images/。
#
# ★★ 2026-09-16 第二版（用户：「音量弹出框效果参考手机修改，现在的显示效果太差了。
#    可以做成半透的加进度条」）——旧版观感差在哪（真机像素实测）：
#      · 条高 20px、**两个纯色方块**（竖向渐变），四角**没有圆角**、没有 alpha；
#      · 填充是"从左侧按进度裁到哪儿算哪儿"⇒ 右端是**硬切口**，不像手机的圆头；
#      · 面板底色是不透明 #1C1C1E ⇒ 跟背后页面糊成一块，没有"浮层"感。
#    新版三条改动：
#      ① **细条 10px + 胶囊圆角 + 半透明**：轨道 = 白色低 alpha，填充 = 实色；
#      ② **加圆头（thumb）**：一个小圆钮压在进度末端 ⇒ 不管填充图是被裁还是被缩放，
#         末端永远是圆的（★ 这一步顺带绕开了"裁切 vs 缩放"这个没法从文档确认的问题：
#         实测两种行为下都不难看）；
#      ③ 面板本身换成**带 alpha 的圆角 PNG**（见 gen_osd_panel），真半透明。
#    ⚠️ 尺寸必须 == 控件盒（轨道/填充 200x10 与 312x10、圆钮 10x10）—— 工程铁律，
#      `tools/check_stretch.py` 会抓；**圆钮图别加描边环**（会在屏幕上留一圈暗环，
#      MCP `pixel-analysis-ai.md` 的实测坑）。
# ★ 2026-09-17 用户需求：「进度条不需要按键，直接拖动就好了」。
#   ⇒ 设置页两条进度条改成**可拖动**、删掉 ＋/－ 按钮。两个直接后果：
#     ① **条要变宽**：原来 312 宽是给右侧两个 56 宽按钮留的位置 ⇒ 改 448（左右各 16 边距）；
#     ② **控件盒要做高**：手指最小命中区约 34px，而视觉上仍要保持 10px 细条
#        ⇒ 盒 448x34，胶囊画在盒内 y=12..21（居中），上下透明、什么都不画。
#   素材尺寸必须 == 控件盒（工程铁律，`check_stretch.py` 会抓）⇒ 两个变体各出一套图。
VOL_BAR_VARIANTS = (
    # (宽, 盒高, 胶囊在盒内的 y 偏移, 文件名后缀, 用哪套填充色)
    (164, 10, 0, "", "osd"),        # 状态栏音量 OSD：只显示 ⇒ 盒 = 条本身
                                    #   （2026-09-17 用户要求"进度条左侧放音量图标"⇒
                                    #     原来 200 宽让位给 32 宽的图标按钮，改 164）
    (448, 34, 12, "_448", "set"),   # 系统设置页：**可拖动** ⇒ 盒做高撑出命中区
)
VOL_BAR_H = 10                    # 胶囊**本身**的高度（视觉上就是这一条）
# 轨道：白色低 alpha（半透明 → 压在任意底色上都好看）
VOL_TRACK_RGBA = (255, 255, 255, 40)
# 填充色：OSD 里用白色（手机音量条就是白的）；设置页用页面 accent（青蓝 #64D2FF）
VOL_FILL_OSD_RGBA = (255, 255, 255, 235)
VOL_FILL_SET_RGBA = (100, 210, 255, 255)
SS = 4                            # 超采样倍率：圆角/圆的边缘要精确覆盖率，不能靠模糊


def _save_capsule(path, w, box_h, offset, h, rgba):
    """在 w×box_h 的**透明**画布上，把 h 高的胶囊画在 y=offset 处。

    offset > 0 的用法见 VOL_BAR_VARIANTS：控件盒做高（手指命中区）而视觉仍是细条。
    """
    from PIL import Image, ImageDraw
    W, H = w * SS, box_h * SS
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(im).rounded_rectangle(
        [0, offset * SS, W - 1, (offset + h) * SS - 1], radius=(h * SS // 2), fill=rgba)
    im.resize((w, box_h), Image.BOX).save(path)


def gen_volume_bar_assets():
    """生成音量/背光进度条的轨道 + 填充（幂等；没有 PIL 就跳过并提示）。

    变体见 VOL_BAR_VARIANTS：
      · `vol_track.png` / `vol_fill.png` —— 200x10，状态栏音量 OSD（只显示）
      · `vol_track_448.png` / `vol_fill_448.png` —— 448x34，系统设置页（**可拖动**）

    ⛔ **不生成圆钮（thumb）** —— 2026-09-16 真机实测后去掉：
      ZKSeekBar 的填充图是**按进度把整张图缩放**（实测 0/6/42/100% 四档：
      填充宽严格 = 进度宽），所以
        · 胶囊填充图缩放后两端**天然是圆的** ⇒ 观感不需要圆钮；
        · 而圆钮会被画在"进度位置"上 ⇒ **0% 也留一个 10px 白点**（错的）；
        · html2json 见到 `data-thumb` 还会**自动置 touchable=true**，
          与"进度条只负责显示"的设计冲突（OSD 那条就只显示）。
      ⇒ 两条都只用 track + fill 两张图。（旧 vol_thumb*.png 由下面 stale 清理删掉。）
    """
    try:
        from PIL import Image
    except Exception as e:  # noqa: BLE001
        print("!! 没有 PIL，跳过进度条图生成（%s）；音量 OSD 的进度条会不可见" % e)
        return False

    out_dir = os.path.join(ROOT, "resources", "images")
    os.makedirs(out_dir, exist_ok=True)
    n = 0
    for w, box_h, off, suf, kind in VOL_BAR_VARIANTS:
        track = os.path.join(out_dir, "vol_track%s.png" % suf)
        fill_p = os.path.join(out_dir, "vol_fill%s.png" % suf)
        # ① 轨道（半透明白胶囊；画在透明盒内的 y=off 处）
        _save_capsule(track, w, box_h, off, VOL_BAR_H, VOL_TRACK_RGBA)
        # ② 填充（实色胶囊；会被按进度缩放，末端天然是圆角）
        fill = VOL_FILL_OSD_RGBA if kind == "osd" else VOL_FILL_SET_RGBA
        _save_capsule(fill_p, w, box_h, off, VOL_BAR_H, fill)
        n += 2
        print("gen vol_track%s / vol_fill%s (%dx%d，胶囊在 y=%d..%d)"
              % (suf, suf, w, box_h, off, off + VOL_BAR_H - 1))

    # ---- stale 清理：改版后不再使用的素材 ----
    stale = ["vol_thumb.png", "vol_thumb_312.png", "vol_thumb_448.png",
             "vol_track_312.png", "vol_fill_312.png",
             # 2026-09-17：OSD 那条从 200 宽改成 164（图标移到条左侧），旧图要删
             "vol_track_200.png", "vol_fill_200.png"]
    for fn in stale:
        p2 = os.path.join(out_dir, fn)
        if os.path.exists(p2):
            os.remove(p2)
            print("  清掉废弃的 %s" % fn)

    # ★ 尺寸自检：图必须与控件盒 1:1（不等 ⇒ 框架缩放 ⇒ 圆角发糊/拉伸）
    from PIL import Image as _Im
    for w, box_h, off, suf, _kind in VOL_BAR_VARIANTS:
        for kind in ("vol_track%s.png" % suf, "vol_fill%s.png" % suf):
            got = _Im.open(os.path.join(out_dir, kind)).size
            if got != (w, box_h):
                raise SystemExit("进度条图尺寸错：%s = %s，应为 %dx%d"
                                 % (kind, got, w, box_h))
            # 胶囊必须落在盒内（否则被裁）
            im = _Im.open(os.path.join(out_dir, kind)).convert("RGBA")
            rows = [y for y in range(box_h)
                    if any(im.getpixel((x, y))[3] > 0 for x in range(0, w, 3))]
            if not rows:
                raise SystemExit("进度条图全透明：%s" % kind)
            if min(rows) != off or max(rows) != off + VOL_BAR_H - 1:
                raise SystemExit("胶囊位置错：%s 实际 y=%d..%d，应为 %d..%d"
                                 % (kind, min(rows), max(rows), off, off + VOL_BAR_H - 1))
    return True


# ============ 音量 OSD 面板底：**半透明圆角**（2026-09-16 用户要求"做成半透的"）============
# 为什么是"一张带 alpha 的 PNG"而不是 `data-bg="#..."`：
#   ① JSON 背景色走 `setBackgroundColor(int)`，而空明/alpha 在 JSON 解析链路上是**不确定的**
#      （SDK 头文件写的是 `setBackgroundColor(uint32_t)` + 0xAARRGGBB，但 JSON 那个十进制数
#      要 > 2^31 才带得进 A 通道，中间任何一层按 int 解析就会被截掉，风险不可控）；
#   ② MCP `pixel-analysis-ai.md` 的实测结论：**真透明装饰件用 button + picTab（alpha 混合正确）**。
#   ⇒ 所以面板底做成一个 button（`picTab` 挂这张图），作为面板的**最底层**（html 里第一个定义）。
OSD_PANEL_W, OSD_PANEL_H = 240, 112
OSD_PANEL_RADIUS = 22
OSD_PANEL_BG_RGBA = (0, 0, 0, 184)      # 黑 72% —— 既有"浮层"感，又保证字看得清


def gen_osd_panel():
    """生成音量 OSD 的半透明圆角面板底 `osd_panel.png`（240x112）。"""
    try:
        from PIL import Image, ImageDraw
    except Exception as e:  # noqa: BLE001
        print("!! 没有 PIL，跳过 OSD 面板底生成（%s）" % e)
        return False
    out_dir = os.path.join(ROOT, "resources", "images")
    os.makedirs(out_dir, exist_ok=True)
    W, H = OSD_PANEL_W * SS, OSD_PANEL_H * SS
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(im).rounded_rectangle([0, 0, W - 1, H - 1],
                                         radius=OSD_PANEL_RADIUS * SS,
                                         fill=OSD_PANEL_BG_RGBA)
    im.resize((OSD_PANEL_W, OSD_PANEL_H), Image.BOX).save(
        os.path.join(out_dir, "osd_panel.png"))
    got = Image.open(os.path.join(out_dir, "osd_panel.png")).size
    if got != (OSD_PANEL_W, OSD_PANEL_H):
        raise SystemExit("osd_panel.png 尺寸错：%s" % (got,))
    print("gen osd_panel.png (%dx%d, 半透明黑 %d%%)"
          % (OSD_PANEL_W, OSD_PANEL_H, OSD_PANEL_BG_RGBA[3] * 100 // 255))
    return True


# 视频区"缓冲底图"（IPTV 播放页 / 投屏页）
# ⚠️ 为什么必须有：videoview 是 **UI 层给下层 disp 视频层开的透明窗口**，
#    视频层还没数据（拉清单/等首帧）时那块是**透出最底层**的 —— 屏幕上就是
#    "纯黑 + 一层发虚的文字"。放一张**不透明的底图**在该区域，缓冲期就是一张正常画面，
#    出画后由 logic 立刻隐藏（隐藏必须及时：UI 层在视频层之上，留着会挡住视频）。
# ⚠️ 只生成 480x700（= 视频区尺寸），不用整屏，免得盖到下面那排按钮。
COVER_W, COVER_H = 480, 700


def gen_video_cover_asset():
    """生成视频缓冲底图 resources/images/video_cover.png（幂等，PIL 缺失则跳过）。

    风格跟工程一致：深蓝黑渐变 + 一枚主色播放标记 + 上下细描边；**不放文字**
    （PIL 默认字体只有 ASCII，中文字要另带字体文件，这里没必要）。
    ⚠️ 必须是**不透明**的 RGB 图：带 alpha 的话该区域又变成半透明，
       视频层照样透出来，等于白做。
    """
    try:
        from PIL import Image, ImageDraw
    except Exception as e:  # noqa: BLE001
        print("!! 没有 PIL，跳过视频底图生成（%s）；缓冲期视频区会是黑底" % e)
        return False

    out_dir = os.path.join(ROOT, "resources", "images")
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, "video_cover.png")

    top, bottom = (11, 15, 20), (22, 32, 44)          # #0B0F14 -> #16202C
    im = Image.new("RGB", (COVER_W, COVER_H))
    px = im.load()
    for y in range(COVER_H):
        t = y / float(COVER_H - 1)
        col = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        for x in range(COVER_W):
            px[x, y] = col

    d = ImageDraw.Draw(im)
    # ⚠️ 图形**必须避开中间那三行 loading 文字**（TextIptvLoad/Bar/Hint 在 y=290..406）：
    #    原来 cy = 700/2-20 = 330，播放标记正好压在文字上，屏幕上是一团叠着的图形+字。
    #    现在整块图形（环 128..252 + 点 ~268）落在 290 以上，文字在下方，上下分明。
    cx, cy = COVER_W // 2, 190
    # 外圈 + 内圈（双层细环，给一点"仪表"质感）
    d.ellipse([cx - 62, cy - 62, cx + 62, cy + 62], outline=(46, 58, 74), width=3)
    d.ellipse([cx - 50, cy - 50, cx + 50, cy + 50], outline=(30, 40, 52), width=1)
    # 主色播放三角（#4FC3F7）
    d.polygon([(cx - 17, cy - 24), (cx - 17, cy + 24), (cx + 24, cy)], fill=(79, 195, 247))
    # 三角下方三个小点（静态的"等待中"暗示）
    for i, dx in enumerate((-26, 0, 26)):
        r = 5 if i == 1 else 4
        col = (79, 195, 247) if i == 1 else (46, 58, 74)
        d.ellipse([cx + dx - r, cy + 78 - r, cx + dx + r, cy + 78 + r], fill=col)
    # 上下细描边（与视频区边界对齐，转场时看不出接缝）
    d.rectangle([0, 0, COVER_W - 1, 0], fill=(30, 40, 52))
    d.rectangle([0, COVER_H - 1, COVER_W - 1, COVER_H - 1], fill=(30, 40, 52))

    im.save(path)
    print("gen video_cover.png (%dx%d)" % (COVER_W, COVER_H))
    return True


def patch_flipcards(data):
    """屏保钟：只让「数字 0」与冒号亮态初始可见。

    每位数字是 10 个同位置重叠的 img（数字 0..9），全可见就是一团糊。
    初始统一显示 0，运行首帧由 logic 立刻改成真实时间（见 src/logic/screensaver.cc）。
    html2json 不支持 data-visible（只能后处理），所以在这里改。
    ⚠️ 2026-09-18 砖块钟改版：控件名从 FlipU/FlipD 改成 **Dig<位>_<数字>**
       （砖块与数字拆成两层，砖块 Brick0..3 是静态底板，见 ui/screensaver.html）。
    """
    import re as _re
    n = 0

    def walk(node):
        nonlocal n
        if isinstance(node, dict):
            cap = node.get("caption")
            want = None
            if isinstance(cap, str):
                m = _re.match(r"^Dig(\d)_(\d)$", cap)
                if m:
                    want = (int(m.group(2)) == 0)   # 只有数字 0 的图层初始可见
                elif cap == "ColonOn":
                    want = True
                elif cap == "ColonOff":
                    want = False
            # ⚠️ 可见的也要**显式**写 True：html2json 对 textview/img **根本不写 visible 字段**，
            #    而"缺字段"的行为不可靠（EditPwd 缺 touchable 就是前车之鉴）。
            if want is not None and node.get("visible") != want:
                node["visible"] = want
                n += 1
            for v in node.values():
                walk(v)
        elif isinstance(node, list):
            for v in node:
                walk(v)

    walk(data)
    if n:
        print("patch flipcards: %d 个数字图层显式设置 visible（只留数字 0）" % n)


def patch_edittexts(data):
    """给 ZKEditText 补 touchable/visible。

    ⚠️ 2026-09-13 实测发现的 html2json 缺漏：生成 `edittext` 时**不写 touchable/visible**
    （button/subItem/seekbar 等分支都显式写了），引擎缺省 touchable=false ⇒
    **点输入框完全没反应、拉不起 IME**（现象极易误判成"IME 没注册"）。
    判断依据：带 `textType` 键的节点就是 EditText（btn/text 都没有这个键）。
    """
    n = 0

    def walk(node):
        nonlocal n
        if isinstance(node, dict):
            if "textType" in node:
                node["touchable"] = True
                node["visible"] = True
                n += 1
            for v in node.values():
                walk(v)
        elif isinstance(node, list):
            for v in node:
                walk(v)

    walk(data)
    if n:
        print("  patch %d 个输入框: touchable=true visible=true（html2json 漏写）" % n)


# ==================================================================
#  圆角按钮的"背景色"= **所坐容器的底色**（★ 2026-09-24 重做，修"黑色倒角"）
# ==================================================================
# 症状（用户反复报）：弹窗里的"改名/移除"等按钮、卡片上的按钮，四角是**黑的**
#   —— 明明按钮坐在 #1C1C1E 的卡片上，圆角外却是一圈黑（像被切了个黑倒角）。
#
# 机理（两步，缺一不可）：
#   ① 圆角素材 `ios_btn_*.9.png` 走 JSON 路径渲染时，**圆角外是透明的**
#      ⇒ 那里显示的是**控件自己的 `bgColorTab`**（框架把 bgColorTab 画在图的下面）。
#   ② `inject_rounded()` 原来把 bgColorTab 清成 **-1**（= 纯黑），
#      html2json 也**故意**不给"有图的按钮"写 bgColorTab
#      （源码注释：`c.pop('bgColorTab')  # 有图不用底色（透明角会透出底色，图片叠色效果错乱）`）
#      ⇒ 不管按钮坐在什么上，圆角外永远是黑的。
#
# 为什么不能"填按钮自己的色"（那条 html2json 注释说的"效果错乱"就是这个）：
#   钮底色填进去 ⇒ 圆角外也是钮色 ⇒ 看着就是个**方块**，圆角白做了。
#
# ⇒ 正解：填 **按钮所坐容器的底色**。圆角外 = 卡片色 ⇒ 视觉上就是"圆角按钮自然坐在卡片上"。
#   容器是页面/窗口（黑）⇒ 填 -1（纯黑 = 页面色，与"方块"无关，因为背景本来就是黑的）。
#
# 这一套取代了原来的 `CARD_FILL` 白名单（手工登记 caption → 颜色）：
#   白名单必然漏（新增按钮忘了登记就复发），而且它只覆盖 #1C1C1E / #2C2C2E 两种；
#   现在从**源稿**直接推：任何写了 `data-bg` 的容器、`data-round` 面板，其子树都自动继承。
#
# 判据与工具：`tools/audit_resources.py` 的【1】节（图角行为 × 座位底色，全量普查）。
ROUND_FILL = {                      # data-round 的面板名 → 该面板**本体**的底色
    "panel":  TOKENS["SURFACE"],    # ios_panel    ≥ #1C1C1E
    "panel2": TOKENS["SURFACE2"],   # ios_panel2   ≥ #2C2C2E
    "panel2c": TOKENS["SURFACE2"],  # ios_panel2_c ≥ #2C2C2E
}


def parse_caption_fill(html_path):
    """从源稿读 `caption → 容器底色 #RRGGBB`（只收"非黑"的，黑=页面，等价于不写）。

    · `data-bg="#XXXXXX"`（且不是 #000000）→ 它就是一块**有色容器**；
    · `data-round="panel|panel2|panel2c"` → 面板图有本体色，按 ROUND_FILL 取。
    """
    out = {}
    try:
        with open(html_path, encoding="utf-8") as f:
            txt = f.read()
    except OSError:
        return out
    for m in re.finditer(r'<[^>]*data-caption="([^"]+)"[^>]*>', txt, re.S):
        tag = m.group(0)
        cap = m.group(1)
        bg = re.search(r'data-bg="(#[0-9A-Fa-f]{6})"', tag)
        rd = re.search(r'data-round="([^"]+)"', tag)
        if bg and bg.group(1).upper() != "#000000":
            out[cap] = bg.group(1).upper()
        elif rd and rd.group(1) in ROUND_FILL:
            out[cap] = ROUND_FILL[rd.group(1)]
    return out


def parse_noround_sizes(html_path):
    """扫出所有 `data-noround` 按钮的 (宽, 高) —— 给 ios_theme 生成"运行时普通 PNG"用。

    为什么要扫而不是手写清单：运行时按钮的圆角必须用**普通 PNG**（九宫格的 marker 边
    在运行时挂图的路径上会被渲染成一条白线，见 ios_theme.gen_runtime_buttons 的注释），
    而普通 PNG 必须"图尺寸 == 控件尺寸"⇒ 每种尺寸都要一张。手写清单必然漏；
    从源稿扫则"标注即生效"。
    """
    try:
        with open(html_path, encoding="utf-8") as f:
            txt = f.read()
    except OSError:
        return []
    out = []
    for m in re.finditer(r"<[^>]*>", txt):
        tag = m.group(0)
        if "data-noround" not in tag:
            continue
        w = re.search(r'data-w="(\d+)"', tag)
        h = re.search(r'data-h="(\d+)"', tag)
        if w and h:
            out.append((int(w.group(1)), int(h.group(1))))
    return out


def parse_marked(html_path, attr):
    """从**源 html** 里挑出带某个 data-* 标记的 caption。

    为什么从 html 读而不是从 json 读：html2json 会丢掉它不认识的 data-* 属性，
    json 里查不到。而这两个标记必须由"写 UI 的人"在源稿上显式声明：
      data-noround="1"    → 该按钮**运行时**会改底色，不能挂静态两态图
      data-round="panel"  → 该窗口/面板要挂圆角背景图
    比在 gen_ui.py 里硬编码一张 caption 清单抗漂移得多（caption 一改就静默失效）。
    """
    try:
        with open(html_path, encoding="utf-8") as f:
            txt = f.read()
    except OSError:
        return {}
    out = {}
    # 逐个标签扫：data-caption 与目标标记在同一标签内
    for m in re.finditer(r"<[^>]*>", txt):
        tag = m.group(0)
        if attr not in tag:
            continue
        cap = re.search(r'data-caption="([^"]+)"', tag)
        val = re.search(r'%s="([^"]*)"' % re.escape(attr), tag)
        if cap and val:
            out[cap.group(1)] = val.group(1)
    return out


def _hexstr(color):
    return "#%06X" % (color & 0xFFFFFF)


def inject_rounded(data, src, noround):
    """给按钮自动挂圆角九宫格两态图（本工程 UI 改版的核心机制）。

    为什么要自动化：全工程 285 个按钮。逐个写 data-pic0/data-pic1 既啰嗦又必然漏，
    而**一种颜色的九宫格可以拉伸复用任意尺寸** ⇒ 只要按"按钮底色"映射到资源即可，
    HTML 里一个 data-pic 都不用写。

    三条硬约束：
      ① **`bgColorTab` 要填"按钮所坐容器的底色"**（★★ 2026-09-24 修正）——
         圆角素材的圆角外是**透明**的，那里显示的就是 bgColorTab。
            · 填 -1/黑（老行为）⇒ 坐在卡片上就是**四个黑倒角**（用户反复报的那个）；
            · 填**按钮自己的色** ⇒ 圆角被填成方的，圆角白做（html2json 注释里的"效果错乱"）；
            · 填**容器色** ⇒ 正确。容器是页面/窗口（黑）时填 -1。
         源稿侧：任何写了非黑 `data-bg` 的容器、`data-round` 面板，其子树自动继承（见
         `parse_caption_fill` / `ROUND_FILL`）。**不用再手工登记白名单**。
      ② **运行时改底色的按钮不能挂图** —— `setBackgroundColor` 会把 backgroundPic 清掉
         （实机血案，见 ui/main.html 的注释）。这类按钮在源稿上标 `data-noround="1"`，
         改由图外的运行时 helper `pg::applyRoundedBg()` 负责（见 src/platform/PkSkin.h）。
      ③ 素材侧：静态九宫格套**烘透明底**（`ios_theme.STATIC_BAKE = None`）。
         运行时那套 `ios_rt_*.png` 仍**必须**烘不透明底（那条渲染路径不保留 alpha）。
    """
    n_ok, n_skip_dyn, n_noasset, n_fill, n_pic = 0, 0, 0, 0, 0
    caps = parse_caption_fill(os.path.join(UI_DIR, src))

    def rect(node):
        p = node.get("position") or {}
        return (p.get("left", 0), p.get("top", 0), p.get("width", 0), p.get("height", 0))

    def backdrop(node, siblings):
        """★ 求"本控件所坐**有色容器**的底色"（None = 页面/窗口 = 黑）。

        ⚠️⚠️ 这里是本函数第二次踩坑（2026-09-24）：**本工程的控件树是平铺的** ——
           同一个窗口里所有控件都是并列的兄弟节点（button__66 / button__67 / …），
           "卡片包着按钮"在 JSON 里**不是父子关系**。
           所以绝不能用"递归往下传 on_card"来判（老代码就是这么写的 ⇒ 从来没生效过，
           这也是 `_c`（面板底）变体全工程一个都没被选中的原因）。
           唯一可靠的判据 = **几何包含**：在所有"声明了非黑底色的控件"里，
           取矩形**完整包含**本控件且**面积最小**的那一个。
        """
        x, y, w, h = rect(node)
        best = None
        for o in siblings:
            if o is node:
                continue
            oc = o.get("caption")
            c = caps.get(oc) if isinstance(oc, str) else None
            if not c:
                continue                      # 只有"有色容器"能当底
            if (x, y, w, h) == rect(o):
                continue                      # 同格叠放的兄弟（段控两态）不算容器
            ox, oy, ow, oh = rect(o)
            if x >= ox and y >= oy and x + w <= ox + ow and y + h <= oy + oh:
                if best is None or ow * oh < best[0]:
                    best = (ow * oh, c)
        return best[1] if best else None

    def controls_of(node, out):
        """把一个窗口/容器下的**全部平铺控件**收集起来（含 listview 的 item 子项）。"""
        if isinstance(node, dict):
            if "position" in node and node.get("caption"):
                out.append(node)
            if isinstance(node.get("item"), dict):
                for sv in node["item"].get("subItem", []):
                    if "position" in sv and sv.get("caption"):
                        out.append(sv)
                return
            for v in node.values():
                if isinstance(v, (dict, list)):
                    controls_of(v, out)
        elif isinstance(node, list):
            for v in node:
                controls_of(v, out)
        return out

    windows = [v for k, v in data.items()
               if str(k).startswith("window") and isinstance(v, dict)]
    for win in windows:
        sibs = controls_of(win, [])
        for node in sibs:
            cap = node.get("caption")
            is_btn = isinstance(cap, str) and "picTab" in node and "item" not in node
            has_bgpic = bool(node.get("backgroundPic"))
            if not (is_btn or has_bgpic):
                continue
            fill = backdrop(node, sibs)
            if is_btn:
                pt = node.get("picTab") or {}
                if pt.get("pic0"):
                    pass                                   # 源稿已显式指定 → 尊重
                elif cap in noround:
                    n_skip_dyn += 1
                else:
                    bg = (node.get("bgColorTab") or {}).get("color0", -1)
                    if bg != -1:
                        h = int((node.get("position") or {}).get("height", 0))
                        a = btn_asset_for(_hexstr(bg), h)
                        if a:
                            node["picTab"] = {"pic0": "images/" + a[0],
                                              "pic1": "images/" + a[1]}
                            # ★ ①：圆角外要露的是**容器色**（不是按钮自己的色、也不是黑）
                            node["bgColorTab"] = {"color0": _hexint(fill)}
                            node["backgroundColor"] = -1
                            n_ok += 1
                            if fill is not None:
                                n_fill += 1
                        else:
                            n_noasset += 1
            elif has_bgpic and fill is not None:
                # 只有"挂在有色容器上"的 backgroundPic 才需要补（页面上的保持原样，
                # 免得动到全工程上千个控件的默认行为）。典型受益者：probe 页那些
                # 坐在彩色按钮上的 icon（圆角外原本露黑 ⇒ 按钮上四个黑角）。
                if (node.get("bgColorTab") or {}).get("color0", -1) in (-1, 0):
                    node["bgColorTab"] = {"color0": _hexint(fill)}
                    n_pic += 1

    print("  [圆角] %s: 挂图 %d 个按钮（其中 %d 个按容器色填 bgColorTab）；"
          "backgroundPic 补容器色 %d；动态底色跳过 %d；无对应资源 %d"
          % (src, n_ok, n_fill, n_pic, n_skip_dyn, n_noasset))
    return n_ok


def _hexint(h):
    """'#RRGGBB' → 0xRRGGBB；None（页面/窗口黑）→ **-1**（不是 0x000000！）。

    ⚠️ 不能填 0x000000：html2json 把"纯黑 0"当成"没设置"（血案），会退回引擎默认色。
       本工程约定 -1 = 不设底色 = 黑（实测渲染为 (0,0,0)）。
    """
    return -1 if not h else int(h.lstrip("#"), 16)


# data-round 的取值 → 九宫格资源名（在 tools/ios_theme.py 的 SURFACE_STYLES 里）
# ⚠️ 源稿上写短名（panel / panel2 / panel2c），资源名带 ios_ 前缀；这张表就是两者的桥。
#    对不上的值会**直接报错**（不静默跳过）：挂错图（或没挂上）在深色页上几乎看不出来，
#    但会让"整屏唯一一个直角盒子"这种问题留到真机上才发现。
# ⚠️ `panel2c` = "坐在面板上的次级面板"（#2C2C2E **烘 SURFACE 底**）：给"弹窗卡片里的输入框"
#    这类嵌套场合用。用普通的 panel2（烘页面黑底）会在卡片上露出一圈黑角。
ROUND_ASSETS = {"panel": "ios_panel", "panel2": "ios_panel2", "panel2c": "ios_panel2_c"}


# ==================================================================
#  整屏窗口的"不透明底"修正（★ 血案见下）
# ==================================================================
# ⚠️⚠️ html2json 会把**窗口类节点**的 `data-bg="#000000"` 写成 `backgroundColor = -1`（透明）——
#    它把合法的"纯黑 0"当成了"没设置"（根节点不受影响：root 的 #000000 正确得到 0）。
#    实测：`<div class="modal" data-bg="#000000">` → -1；改成 `#010101` → 65793。
#
#    后果：整屏窗口透明 ⇒ **下层的东西从窗口上面露出来**。
#    血案（2026-09-15 用户报"桌面的背景覆盖上来了"）：进游戏后，主界面改成图标网格后
#    上方露出一整行彩色图标 + 顶栏 + 分段控件（画布只有 540 高，窗口又是透明的）。
#
# 修法：把"**确认要盖住下层**"的整屏窗口的 backgroundColor 兜底成 0（纯黑不透明，与根节点同）。
#
# ⚠️ **故意透明的窗口绝不能动** —— 特别是 iptv 的 `WinIptvPlay`：
#    视频层是独立 disp 图层、在 UI 层**之下**，靠窗口透明才看得见画面
#    （参见 docs/iptv.md 与 "videoview 是透明窗口" 那条）。给它填黑底 = 视频全黑。
#    所以这里用**白名单**而不是"凡 -1 就填"。
OPAQUE_WINDOWS = {
    # 盖在**主界面**之上的整屏窗口（不透明黑，否则桌面会透出来）
    ("main.html", "WinGame"),
    ("main.html", "WinClock"),
    ("main.html", "WinCalc"),
    ("main.html", "WinReact"),
    ("main.html", "WinAlarmRing"),   # 响铃提醒页（到点弹）—— 透明会露出桌面
    ("main.html", "WinCast"),        # 投屏页 —— 同上
#    盖在各自**主页**之上的整屏窗口
    ("wifi.html", "WinWifiPwd"),
    ("remote.html", "WinRemoteLearn"),
    ("clocksuite.html", "WinClockSuite"),
    ("clocksuite.html", "WinWorld"),
    ("iptv.html", "WinIptv"),        # 选台页（页面主体）
    # probe.ftu 的整屏页：默认页 + 三个二级页 + 一个整屏浮层，都必须不透明
    # （否则会透出桌面）。**加页签/加页时这个集合必须跟着加**：
    # 漏了的话表现是"进这一页看到的是上一页的残留"（窗口没铺黑 = 透明）。
    ("probe.html", "WinProbeWifi"),
    ("probe.html", "WinProbeBt"),
    ("probe.html", "WinProbeLan"),      # 局域网设备（主动探测）
    ("probe.html", "WinProbeSniff"),    # 无线嗅探（monitor 顺听）
    ("probe.html", "WinHunt"),
    # ha.ftu（智能家居）：**整屏页**都必须不透明
    #   （漏了的表现就是"切过去看到的是上一页的残留"：窗口没铺黑 = 透明）。
    # ⚠️⚠️ 但 **WinHaMenu / WinHaConfirm 绝不能列在这里**（2026-09-24 用户要求
    #   "弹框的时候背景的列表让它在那里显示"）：它们是 **modal** 弹层，
    #   html2json 对 modal 给的是 backgroundColor=-1（透明），而这正是我们要的 ——
    #   半透遮罩图（ImgHaMask / ImgHaMask2）叠在**下面那层还活着的列表**上，
    #   压暗之后背景列表依然看得见。给它们兜底铺黑 = 遮罩下面还是纯黑 = 白做。
    ("ha.html", "WinHaHome"),
    ("ha.html", "WinHaPick"),
    ("ha.html", "WinHaName"),
    ("ha.html", "WinHaDone"),
    ("ha.html", "WinHaSet"),
    # ⛔ 故意不加：("iptv.html", "WinIptvPlay") —— 视频播放页，必须透明，见上面的说明
}


def fix_window_bg(data, src):
    """整屏窗口兜底不透明黑（白名单内、且当前是 -1 且没挂背景图的）。"""
    n = 0
    for k, v in list(data.items()):
        if not isinstance(v, dict) or not k.startswith("window__"):
            continue
        cap = v.get("caption")
        if (src, cap) not in OPAQUE_WINDOWS:
            continue
        if v.get("backgroundColor") == -1 and not v.get("backgroundPic"):
            v["backgroundColor"] = 0        # 0 = 纯黑（与根节点一致；-1 才是"不画"）
            n += 1
    if n:
        print("  [窗口底] %s: %d 个整屏窗口补不透明黑" % (src, n))
    return n


def inject_panels(data, src, panels):
    """给窗口/面板挂圆角背景图（源稿标 data-round="panel|panel2"）。

    ⚠️ 只给"小面板/弹窗"挂，**不是给整屏页面窗口挂**：整屏页面是页面底
       （纯色 #000000，不该有圆角，圆角会把屏幕四角切掉）。
    ⚠️ 挂了图必须清 backgroundColor：图有透明圆角，底色会把四角填成方的。
    """
    n = 0

    def walk(node):
        nonlocal n
        if isinstance(node, dict):
            cap = node.get("caption")
            if isinstance(cap, str) and cap in panels:
                key = panels[cap]
                if key not in ROUND_ASSETS:
                    raise SystemExit("gen_ui: %s 的 data-round=\"%s\" 不是已知面板名（%s）"
                                     % (cap, key, "/".join(sorted(ROUND_ASSETS))))
                node["backgroundPic"] = "images/%s.9.png" % ROUND_ASSETS[key]
                node["backgroundColor"] = -1
                n += 1
            for v in node.values():
                walk(v)
        elif isinstance(node, list):
            for v in node:
                walk(v)

    walk(data)
    if n:
        print("  [面板] %s: 挂圆角面板 %d 个" % (src, n))
    return n


def polish_generated_icons():
    """给"带透明通道的图形类资源"做「生成后像素精修」：清幽灵像素 + 清隐形 RGB。

    为什么要有这一步（2026-09-16，作者："倒角有锯齿，还有一个黑色边框。
    检讨整套 resource 目录下的图标。"）：
        icon_*.png 由 MCP 的 gen_res.glyph_icon 渲染（**我们不改 MCP**），
        ios_theme / gen_flipclock 自己画的矢量图也可能残留同类瑕疵。
        全目录体检（tools/check_assets.py）实测到两类：
          ① **幽灵像素**：重采样核（LANCZOS/BILINEAR）带负瓣/过冲，在形状边界
             **之外**漏出一圈极淡的像素（实测 batt_bolt 闪电外侧一串 α≈17）。
             它们在深色底上就是隐约的毛刺 —— 与"倒角锯齿"是同一观感来源。
             判据必须精确，否则会误伤**线末端**（那里覆盖率天然就低）：
             只有"α 不够浓 **且 8 邻域内没有任何浓像素**"（= 完全悬空）才算幽灵。
          ② **隐形像素**：α==0 却带 RGB（非预乘缩放的产物）。渲染路径一旦忽略
             alpha（或做 1-bit 量化），它们就会直接显形。
        ⚠️ 本工程自己的生成器已全部改用 Image.BOX（精确覆盖率，见 ios_theme.py），
           这里只是兜底清理 **MCP 产出** + 任何漏网的残留。

    ⚠️ 只清"悬空"的像素，**不按固定阈值一刀切** —— 阈值法会把线条边缘也啃掉。
    """
    import glob as _glob
    from PIL import Image
    GHOST_PEAK = 150      # ≥ 这个 α 才算"浓像素"（主体）
    # 带不透明烘底的图（面板/按钮/圆角底）不走这条路径 —— 它们没有 alpha 语义
    OPAQUE = ("ios_btn", "ios_panel", "ios_seg", "ios_card", "ios_row", "ios_rt_",
              "grid_cell", "batt_shell", "batt_fill", "video_cover")
    # ★★ 2026-09-16 血案：**整体半透明**的素材会被这条规则**整张清空**。
    #   判据是"α < 150 且 8 邻域内没有 α > 150 的像素 ⇒ 悬空幽灵"——这对"某个形状
    #   边界外面漏出的一圈淡像素"是对的；但对**故意做成低 alpha 的整张图**
    #   （音量条的半透明轨道 α=40）就全图都满足条件 ⇒ **整张被清零**。
    #   实测：`vol_track.png` / `vol_track_312.png` 生成后被清成 alpha 全 0，
    #   屏幕上表现为"进度条只剩填充、轨道整条消失"（而且不报错）。
    #   ⇒ 修法：先看**全图最大 α**，若它本身就 < GHOST_PEAK（= 全图没有"主体"），
    #     说明这张图是**故意半透明**的，直接跳过，不参与幽灵清理。
    n_files = n_ghost = n_hidden = 0
    n_skip_translucent = 0
    for p in sorted(_glob.glob(os.path.join(ROOT, "resources", "images", "*.png"))):
        base = os.path.basename(p)
        if base.startswith(OPAQUE):
            continue
        im = Image.open(p)
        if im.mode not in ("RGBA", "LA", "P"):
            continue
        im = im.convert("RGBA")
        if (im.getchannel("A").getextrema()[1] or 0) < GHOST_PEAK:
            n_skip_translucent += 1
            continue
        px = im.load()
        w, h = im.size
        touched = 0
        for y in range(h):
            for x in range(w):
                r, g, b, a = px[x, y]
                if a == 0:
                    if r or g or b:                 # ② 隐形像素
                        px[x, y] = (0, 0, 0, 0)
                        touched += 1
                        n_hidden += 1
                    continue
                if a > GHOST_PEAK:
                    continue
                dense = 0                           # ① 幽灵像素：8 邻域有没有主体
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        if dx == 0 and dy == 0:
                            continue
                        xx, yy = x + dx, y + dy
                        if 0 <= xx < w and 0 <= yy < h and px[xx, yy][3] > GHOST_PEAK:
                            dense += 1
                if dense == 0:
                    px[x, y] = (0, 0, 0, 0)
                    touched += 1
                    n_ghost += 1
        if touched:
            im.save(p)
            n_files += 1
    print("  [资源精修] %d 张图：清除 %d 个幽灵像素 + %d 个隐形像素"
          "（另有 %d 张整体半透明图按『故意半透明』跳过）"
          % (n_files, n_ghost, n_hidden, n_skip_translucent))
    return n_files


def main():
    no_pack = "--no-pack" in sys.argv
    py = sys.executable

    gen_volume_bar_assets()   # 进度条图（html2json 不会合成，必须先有图）
    gen_osd_panel()           # 音量 OSD 的半透明圆角面板底
    gen_video_cover_asset()   # 视频缓冲底图（同上：html2json 只认文件）
    # iOS 圆角资源包（按钮九宫格 / 面板 / 电池 / 运行时普通 PNG）。幂等，改色板后重跑即可。
    # ⚠️ 先扫一遍源稿拿到"运行时按钮的尺寸集合"——这批不能用九宫格（见 ios_theme 的注释）。
    import ios_theme
    rt_sizes = set()
    for src in UI_SOURCES:
        rt_sizes.update(parse_noround_sizes(os.path.join(UI_DIR, src)))
    if rt_sizes:
        print("运行时按钮尺寸（生成普通 PNG）：%s"
              % ", ".join("%dx%d" % s for s in sorted(rt_sizes)))
    ios_theme.gen_all(os.path.join(ROOT, "resources", "images"), rt_sizes)

    for src in UI_SOURCES:
        html = os.path.join(UI_DIR, src)
        js = os.path.join(UI_DIR, src.replace(".html", ".json"))
        r = subprocess.run([py, HTML2JSON, html, js], capture_output=True, text=True)
        print(r.stdout.strip() or r.stderr.strip())
        if r.returncode != 0:
            print("html2json 失败: %s" % src)
            return 1

        with open(js, encoding="utf-8") as f:
            data = json.load(f)

        if src == "statusbar.html":
            # 状态栏：**悬浮在所有页面之上**的整屏透明浮层。
            # ⚠️ touchable=false 是硬要求 —— 状态栏盖住整屏，不穿透会把整个系统的触摸吃掉
            #    （官方 StatusBarDemo / Game640480_Retro 的 statusbar.ftu 根节点实测都是这样）。
            # ⚠️ topmost=true 保证在最顶层（IME 之外），modal=false（模态是输入黑洞，历史坑）。
            data["topmost"] = True
            # ★★ 2026-09-16：touchable 从 false 改成 **true** —— 与"窗口收成 240x160"
            #   是配套的两步，缺一不可：
            #     · 原来整屏 + touchable=false：全机控件点不动（血案），
            #       而 false 还会让**面板自己的按钮也收不到触摸**（实测：面板上的
            #       "静音"按钮点了没反应，日志一条都没有）；
            #     · 现在窗口只有 240x160，true 只会吃这一块 ——
            #       面板显示时正好"吃自己、放别人"，按钮也就可点了。
            data["touchable"] = True
            data["modal"] = False
            data["visible"] = True
            data["hideTimeOut"] = -1
            # ★ 2026-09-16：根节点 = **面板大小**（见 ui/statusbar.html 的说明）——
            #   整屏浮层会吃掉全机控件级触摸，连它自己面板上的"静音"按钮都点不动。
            #   窗口收成面板大小之后：
            #     · 面板显示时只吃那一块的触摸（下面页面照常），
            #     · 面板上的按钮可点（= 弹框内快速静音）。
            #   position 必须显式给：html 的 root 只描述尺寸，窗口位置在这里定。
            # ★ 2026-09-16 第二轮：面板从 240x160 改成 **240x112**（手机风格紧凑版，
            #   见 ui/statusbar.html）。位置取 (120,544) ⇒ 视觉中心仍在 y≈600，
            #   与旧版一致（用户已经熟悉这个位置）。
            #   ⚠️ 改了这里必须同步 src/logic/statusbar.cc 的 kPanelWinLeft/Top
            #     （那里要用它把窗口内坐标换算成屏幕坐标判"点面板外"）。
            data["position"] = {"left": 120, "top": 544, "width": 240, "height": 112}
            # ⚠️⚠️ 面板那一层也必须是 touchable —— 根节点开了 true 还不够：
            #   父窗口（WinSbVolume）默认是 touchable=false，**父不吃触摸 ⇒ 子控件收不到**
            #   （2026-09-16 实测：按钮自身已经是 touchable=true，点击仍然零日志）。
            #   这是本项目第三次栽在同一件事上（statusbar 根 / ime 根 / 这次是中间层），
            #   判据统一是：**要能点的按钮，它的每一层祖先都得可触摸**。
            _win = find_by_caption(data, "WinSbVolume")
            if _win is not None:
                _win["touchable"] = True
                print("patch WinSbVolume: touchable=true（面板上的静音按钮要可点）")
            print("patch statusbar 根节点: topmost=true touchable=true pos=(120,544,240,112)")

        if src == "navibar.html":
            # 导航栏：**常显**的顶部状态条（WiFi 状态）。
            # ⚠️ 与 statusbar 同样的根属性（topmost + touchable=false + 非模态），
            #    但**尺寸是 480x26**（写在 html 的 data-res 里）—— 常显浮层只有这么小
            #    才不会吃掉全机触摸；改成整屏就是第二个"整机点不动"血案。
            data["topmost"] = True
            data["touchable"] = False
            data["modal"] = False
            data["visible"] = True
            data["hideTimeOut"] = -1
            print("patch navibar 根节点: topmost=true touchable=false 尺寸=%s"
                  % (data.get("resolution") or {}))

        if src == "ime.html":
            # IME 页：根节点补 touchable/modal（对齐官方 ImeDemo 的 UserIme.ftu）
            data["modal"] = False
            data["touchable"] = False
            data["visible"] = True
            data["hideTimeOut"] = -1
            print("patch ime 根节点: touchable=false modal=false（对齐 ImeDemo）")

        for cap in HIDDEN_PAGE_WINDOWS:
            node = find_by_caption(data, cap)
            if node is None:
                continue  # 该窗口不在本 ftu 里
            node["visible"] = False
            node["modal"] = False
            node["hideTimeOut"] = -1
            print("patch %s (%s): visible=false modal=false" % (cap, src))

        for cap in HIDDEN_CONTROLS:
            node = find_by_caption(data, cap)
            if node is None:
                continue
            node["visible"] = False
            print("patch %s (%s): visible=false" % (cap, src))

        # ---- iOS 圆角资源注入（颜色 → 九宫格图；见 inject_rounded 的注释）----
        # ⚠️ 顺序：先挂面板、再挂按钮 —— inject_panels 会把 `data-round` 的面板图挂上，
        #    inject_rounded 依据**源稿的容器底色**填 bgColorTab（面板色在 ROUND_FILL 里）。
        html_path = os.path.join(UI_DIR, src)
        inject_panels(data, src, parse_marked(html_path, "data-round"))
        inject_rounded(data, src, parse_marked(html_path, "data-noround"))
        fix_window_bg(data, src)     # 整屏窗口兜底不透明黑（血案见函数上方注释）
        # 纯装饰层（如分段控件的容器底）：自己不该响应触摸，否则会吃掉"分段的间隙"上的点击
        for cap in parse_marked(html_path, "data-untouchable"):
            node = find_by_caption(data, cap)
            if node is not None:
                node["touchable"] = False
                print("  [装饰] %s (%s): touchable=false" % (cap, src))

        # ★ 可拖的 seekbar（2026-09-17 加）：html2json 把 seekbar 的 touchable 与
        #   `data-thumb` **绑死**了（源码 1491 行：默认 touchable=False，只有写了
        #   data-thumb/…-pressed/…-size 才置 True）。而本工程的条**故意不加 thumb** ——
        #   实测：ZKSeekBar 把填充图按进度缩放，两端天然是圆的，加圆钮反而会在 0% 时
        #   留一个 10px 白点（见 tools/gen_ui.py 的 gen_volume_bar_assets 说明）。
        #   ⇒ 用 `data-drag` 显式打开"可拖"（源稿表达意图，这里落地），
        #     与 patch_edittexts 补 touchable 是同一类"缺字段行为不可靠"的补救。
        for cap in parse_marked(html_path, "data-drag"):
            node = find_by_caption(data, cap)
            if node is not None:
                node["touchable"] = True
                print("  [可拖] %s (%s): touchable=true" % (cap, src))

        patch_listviews(data)
        patch_edittexts(data)
        patch_flipcards(data)

        with open(js, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)

    # ★ 拉伸检查（放在**写完所有 json 之后**）：**图尺寸必须 == 控件尺寸**
    #   （不等 ⇒ 框架缩放 ⇒ 拉伸/发糊/锯齿）。用户 2026-09-15 的硬要求
    #   "所有东西不能做拉伸"，这里是执行者。有问题只告警、不中断（照旧出包），
    #   但会在输出里显式列出来。
    try:
        import check_stretch
        check_stretch.main()
    except Exception as e:                                     # noqa: BLE001
        print("  [拉伸检查] 跳过：%s" % e)

    # ★ 布局重叠检查（放在写完所有 json 之后）：**部分重叠 = 会互相遮挡**。
    #   为什么必须有（2026-09-16）：用户反馈「IP 被开关按键覆盖到了」——
    #   这类问题的麻烦在于**抓屏查不出来**（被盖住的内容在屏幕上就是"没有"，
    #   你不知道本该有什么）。只能在生成阶段用盒模型算。
    #   判据区分"完整包含"（正常的底板叠放）与"部分重叠"（一定互相遮挡）。
    try:
        import check_overlap
        check_overlap.main()
    except Exception as e:                                     # noqa: BLE001
        print("  [重叠检查] 跳过：%s" % e)

    # ★ 文案黑名单检查（2026-09-17 加）：转换器 html2json 里有一张**会静默丢弃**字符的
    #   黑名单（`_TEXT_BLACKLIST`：⌫℃■●‹－＋–…→★◆▶▷①），源稿写了这些字符，
    #   生成的 json 里 text 会**直接少掉它们**（不报错、不警告）。
    #   血案：设置页的 ＋/－ 按钮 text 变成空串 ⇒ 屏幕上只剩一个底色块
    #   （用户原话"音量条右侧两个黑块是什么"），另外 3 个页面的"…"也一直在消失。
    #   ⚠️ 黑名单在 MCP 仓库里，工程侧不能改（约定：不代改 MCP）⇒ 只能在生成阶段拦截。
    try:
        import check_textblack
        check_textblack.main()
    except Exception as e:                                     # noqa: BLE001
        print("  [文案黑名单检查] 跳过：%s" % e)

    # ★ 浮层覆盖检查（放在最后）：navibar 是 **topmost + 常显** 的 480x52 窗口，
    #   它压住屏幕顶部 y ∈ [0,52) 这一条。页面里只要还留着"顶部色条 / 大标题"，
    #   就会**露出半截或一条残带**（2026-09-16 真机实测：11 处标题在 y=52..60
    #   露出一条 8px 的 #1C1C1E 横带；用户需求「检讨有 UI 区域覆盖的情况下告警并修复」）。
    #   ⚠️ 这类问题**抓屏能看出来但容易归因错**（看着像"页面底色不对"），
    #     必须在生成阶段用盒模型算。判据与修法见 tools/check_overlay.py 的说明。
    try:
        import check_overlay
        check_overlay.main()
    except Exception as e:                                     # noqa: BLE001
        print("  [浮层覆盖检查] 跳过：%s" % e)

    polish_generated_icons()

    if no_pack:
        print("skip pack")
        return 0

    r = subprocess.run([FUI, "pack", "ui"], cwd=ROOT, capture_output=True, text=True)
    out = (r.stdout or "") + (r.stderr or "")
    print(out.strip())
    return 0 if r.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
