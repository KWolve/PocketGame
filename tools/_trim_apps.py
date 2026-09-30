# -*- coding: utf-8 -*-
"""一次性脚本：删除 pet/elf/fairy/kitten 四个应用（slot 33~36）的代码接线。
★ 每个锚点都 assert 出现次数，找不到就当场报错 —— 不做静默跳过。
★ 图标控件与 gen_icons 的 ICONS 表**保留**（主界面图标按 slot 取第 N 个控件，
  删控件会让 slot 37 的「智能家居」找不到图标）。
"""
import io
import os
import re
import shutil

ROOT = r"E:\AICODE\trae\V851s\PocketGame"
BAK = os.path.join(ROOT, ".bak_trim20260923")


def backup(rel):
    src = os.path.join(ROOT, rel)
    dst = os.path.join(BAK, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
    print("  bak %s" % rel)


def read(rel):
    return io.open(os.path.join(ROOT, rel), encoding="utf-8").read()


def write(rel, s):
    io.open(os.path.join(ROOT, rel), "w", encoding="utf-8", newline="").write(s)


def cut(s, start_mark, end_mark, label, keep_end=True):
    """删掉 [start_mark, end_mark) 之间的内容（start 必须唯一；end 取 start 之后的第一次出现）。"""
    i = s.find(start_mark)
    assert i >= 0, "找不到起点: %s / %s" % (label, start_mark[:40])
    j = s.find(end_mark, i + len(start_mark))
    assert j > i, "找不到终点: %s / %s" % (label, end_mark[:40])
    out = s[:i] + (end_mark if keep_end else "") + s[j + len(end_mark):]
    print("  cut %-28s 去掉 %d 字符" % (label, j - i))
    return out


def must_once(s, mark, label):
    n = s.count(mark)
    assert n == 1, "%s: 期望锚点唯一，实际 %d 次 -> %r" % (label, n, mark[:50])
    return True


print("=== 1) 备份 ===")
for rel in ["src/core/PgGames.h", "src/core/PgGames.cpp", "src/logic/mainLogic.cc",
            "src/logic/navibar.cc", "tools/gen_ui.py", "tools/check_overlay.py",
            "ui/main.html", "ui/ha.html"]:
    backup(rel)

print("=== 2) PgGames.h：删 GamePet / GameElf 两个类 ===")
p = "src/core/PgGames.h"
s = read(p)
must_once(s, '#include "core/PgPetArt.h"', "PgPetArt include")
s = s.replace('#include "core/PgPetArt.h"   // GamePet 的 poseNow 用到 gameart::Def   '
              '// pg::music::Note / NOTE_MAX（节奏钢琴、打鼓共用内核）\n', "")
# 两个类连注释一起删：从"电子宠物（slot 33"的注释块起，到 seedRandom 注释前
s = cut(s, "/* ---------------- 电子宠物（slot 33，2026-09-17） ----------------",
        "// 初始化随机种子（进程内只生效一次），logic 层启动时调一次", "GamePet+GameElf")
write(p, s)

print("=== 3) PgGames.cpp：include + create + 表行 ===")
p = "src/core/PgGames.cpp"
s = read(p)
must_once(s, '#include "PgMovie.h"', "PgMovie include")
s = s.replace('#include "PgMovie.h"\n', "")
s = cut(s, "/* 2026-09-17 新增：电子宠物（slot 33）",
        "/* 2026-09-23 新增：智能家居（slot 37；界面/逻辑全在独立 ftu，本行只给卡片元信息）。 */",
        "createPet..createKitten")
s = cut(s, '    {"pet", createPet, APP_GAME, 33},',
        "    /* 2026-09-23 新增：智能家居（Home Assistant 遥控器）。",
        "kAppTable 四行")
write(p, s)

print("=== 4) mainLogic.cc：kPageApps + autostart ===")
p = "src/logic/mainLogic.cc"
s = read(p)
for pat in [r'\s*\{"pet", "petActivity"\}[^\n]*\n', r'\s*\{"elf", "elfActivity"\}[^\n]*\n',
            r'\s*\{"fairy", "fairyActivity"\}[^\n]*\n', r'\s*\{"kitten", "kittenActivity"\}[^\n]*\n']:
    s2 = re.sub(pat, "\n", s, count=1)
    assert s2 != s, "kPageApps 没删到: %s" % pat
    s = s2
# autostart 里 fairy/kitten/pet/elf 的分支
for key in ["fairy", "kitten", "pet", "elf"]:
    pat = r'\n  if \(strncmp\(buf, "%s", \d+\) == 0\) \{.*?\n  \}\n' % key
    m = re.search(pat, s, re.S)
    if m:
        s = s[:m.start()] + "\n" + s[m.end():]
        print("  cut autostart 分支 %s（%d 字符）" % (key, m.end() - m.start()))
write(p, s)

print("=== 5) navibar.cc：kTitleMap 四行 ===")
p = "src/logic/navibar.cc"
s = read(p)
for key in ["petActivity", "elfActivity", "fairyActivity", "kittenActivity"]:
    pat = r'[^\n]*"%s"[^\n]*\n' % key
    s2 = re.sub(pat, "", s, count=1)
    if s2 != s:
        s = s2
        print("  cut kTitleMap %s" % key)
    else:
        print("  !! kTitleMap 里没找到 %s（可能本来就没有）" % key)
write(p, s)

print("=== 6) gen_ui.py：UI_SOURCES + ImgMovieCover ===")
p = "tools/gen_ui.py"
s = read(p)
s = cut(s, "              # 2026-09-20 新增：两部\"动画短片\"应用",
        "              # 2026-09-23 新增：智能家居（Home Assistant 遥控器）。",
        "UI_SOURCES fairy/kitten")
must_once(s, '"ImgIptvCover", "ImgCastCover", "ImgCamCover", "ImgMovieCover"', "ImgMovieCover")
s = s.replace('"ImgIptvCover", "ImgCastCover", "ImgCamCover", "ImgMovieCover"',
              '"ImgIptvCover", "ImgCastCover", "ImgCamCover"')
write(p, s)
print("  cut HIDDEN_CONTROLS 的 ImgMovieCover")

print("=== 7) check_overlay.py：VideoMain（fairy/kitten 播放区）===")
p = "tools/check_overlay.py"
s = read(p)
n = s.count("VideoMain")
print("  VideoMain 出现 %d 次" % n)
s = re.sub(r'[^\n]*VideoMain[^\n]*\n', "", s)
write(p, s)
print("  cut 所有 VideoMain 行")

print("\n完成。请接着手工核对（grep）残留引用。")
