# 网络收音机电台源实测报告

> 工具 `tools/radio_probe.py`，判据 = 真拉一段字节验同步字（不是看清单声明）。

| 判定 | 名称 | 分组 | 实测 | TTFB | 地址 |
|---|---|---|---|---|---|
| OK | Classic Vinyl HD | 古典 | MP3(+526) ctype=audio/mpeg 16384b | 1031ms | `https://icecast.walmradio.com:8443/classic` |
| OK | Radio Swiss Classic | 古典 | MP3(+0) ctype=audio/mpeg 16384b | 1460ms | `http://stream.srg-ssr.ch/m/rsc_de/mp3_128` |
| OK | YourClassical 放松 | 古典 | MP3(+0) ctype=audio/mpeg 16384b | 829ms | `http://relax.stream.publicradio.org/relax.mp3` |
| OK | 德国拜仁古典 | 古典 | MP3(+0) ctype=audio/mpeg 16384b | 1236ms | `http://mp3channels.webradio.antenne.de/classic-rock-live` |
| OK | 经典调频 | 古典 | MP3(+0) ctype=audio/mpeg 16384b | 595ms | `http://media-ice.musicradio.com/ClassicFMMP3` |
| OK | 世界华声 | 国家台 | HLS 分片=MPEG-TS ctype=video/MP2T | 553ms | `https://sk.cri.cn/hxfh.m3u8` |
| OK | 南海之声 | 国家台 | HLS 分片=MPEG-TS ctype=video/MP2T | 420ms | `https://sk.cri.cn/nhzs.m3u8` |
| OK | 环球资讯广播 | 国家台 | HLS 分片=MPEG-TS ctype=video/MP2T | 386ms | `https://sk.cri.cn/hyhq.m3u8` |
| OK | 英语资讯广播 | 国家台 | HLS 分片=MPEG-TS ctype=video/MP2T | 417ms | `https://sk.cri.cn/am846.m3u8` |
| OK | 中国之声 | 央广 | HLS 分片=MPEG-TS ctype=video/mp2t | 415ms | `https://ngcdn001.cnr.cn/live/zgzs/index.m3u8` |
| OK | 中国之声(MP3直连) | 央广 | MP3(+41) ctype=audio/mpeg 16384b | 1202ms | `https://lhttp.qtfm.cn/live/15318317/64k.mp3` |
| OK | 中国交通广播 | 央广 | HLS 分片=MPEG-TS ctype=video/mp2t | 520ms | `https://ngcdn002.cnr.cn/live/gsgljtgb/index.m3u8` |
| OK | 经济之声 | 央广 | HLS 分片=MPEG-TS ctype=video/mp2t | 384ms | `https://ngcdn002.cnr.cn/live/jjzs/index.m3u8` |
| OK | 香港之声 | 央广 | HLS 分片=MPEG-TS ctype=video/mp2t | 338ms | `https://ngcdn002.cnr.cn/live/xgzs/index.m3u8` |
| OK | 1.FM Chillout Lounge | 氛围 | MP3(+0) ctype=audio/mpeg 16384b | 498ms | `http://strm112.1.fm/chilloutlounge_mobile_mp3` |
| OK | Antenne Bayern Chillout | 氛围 | MP3(+0) ctype=audio/mpeg 16384b | 1736ms | `http://mp3channels.webradio.antenne.de/chillout` |
| OK | Chilltrax | 氛围 | MP3(+191) ctype=audio/mpeg 16384b | 739ms | `http://server1.chilltrax.com:9000/` |
| OK | Radio Paradise 主流 | 氛围 | MP3(+0) ctype=audio/mpeg 16384b | 824ms | `http://stream.radioparadise.com/mp3-128` |
| OK | Radio Paradise 摇滚 | 氛围 | AAC/ADTS(+0) ctype=audio/aac 16384b | 1805ms | `http://stream.radioparadise.com/rock-128` |
| OK | Radio Paradise 舒缓 | 氛围 | AAC/ADTS(+0) ctype=audio/aac 16384b | 819ms | `http://stream.radioparadise.com/mellow-128` |
| OK | Smooth Chill | 氛围 | MP3(+0) ctype=audio/mpeg 16384b | 1891ms | `https://media-ssl.musicradio.com/ChillMP3` |
| OK | SomaFM Beat Blender | 氛围 | MP3(+0) ctype=audio/mpeg 16384b | 891ms | `https://ice2.somafm.com/beatblender-128-mp3` |
| OK | SomaFM DEF CON Radio | 氛围 | MP3(+0) ctype=audio/mpeg 16384b | 1396ms | `https://ice2.somafm.com/defcon-128-mp3` |
| OK | SomaFM Deep Space One | 氛围 | MP3(+0) ctype=audio/mpeg 16384b | 641ms | `https://ice2.somafm.com/deepspaceone-128-mp3` |
| OK | SomaFM Drone Zone | 氛围 | MP3(+0) ctype=audio/mpeg 16384b | 1863ms | `https://ice6.somafm.com/dronezone-128-mp3` |
| OK | SomaFM Groove Salad | 氛围 | MP3(+0) ctype=audio/mpeg 16384b | 769ms | `https://ice2.somafm.com/groovesalad-128-mp3` |
| OK | SomaFM Indie Pop Rocks | 氛围 | MP3(+0) ctype=audio/mpeg 16384b | 1025ms | `https://ice2.somafm.com/indiepop-128-mp3` |
| OK | SomaFM Lush | 氛围 | MP3(+0) ctype=audio/mpeg 16384b | 774ms | `https://ice5.somafm.com/lush-128-mp3` |
| OK | SomaFM Metal Detector | 氛围 | MP3(+0) ctype=audio/mpeg 16384b | 997ms | `https://ice6.somafm.com/metal-128-mp3` |
| OK | SomaFM Secret Agent | 氛围 | MP3(+0) ctype=audio/mpeg 16384b | 773ms | `https://ice5.somafm.com/secretagent-128-mp3` |
| OK | SomaFM Suburbs of Goa | 氛围 | MP3(+0) ctype=audio/mpeg 16384b | 923ms | `https://ice2.somafm.com/suburbsofgoa-128-mp3` |
| OK | SomaFM The Trip | 氛围 | MP3(+0) ctype=audio/mpeg 16384b | 844ms | `https://ice2.somafm.com/thetrip-128-mp3` |
| OK | SomaFM Underground 80s | 氛围 | MP3(+0) ctype=audio/mpeg 16384b | 699ms | `https://ice5.somafm.com/u80s-128-mp3` |
| OK | Adroit Jazz Underground | 爵士 | MP3(+243) ctype=audio/mpeg 16384b | 4927ms | `https://icecast.walmradio.com:8443/jazz` |
| OK | Jazz Radio 爵士 | 爵士 | MP3(+0) ctype=audio/mpeg 16384b | 780ms | `http://jazzradio.ice.infomaniak.ch/jazzradio-high.mp3` |
| OK | Jazz Radio 蓝调 | 爵士 | MP3(+0) ctype=audio/mpeg 16384b | 699ms | `http://jazzblues.ice.infomaniak.ch/jazzblues-high.mp3` |
| OK | Smooth Jazz 101 | 爵士 | MP3(+0) ctype=audio/mpeg 16384b | 614ms | `http://jking.cdnstream1.com/b22139_128mp3` |
| OK | 瑞士古典爵士 | 爵士 | MP3(+0) ctype=audio/mpeg 16384b | 3512ms | `http://stream.srg-ssr.ch/m/rsj/mp3_128` |
| OK | 上海新闻广播 | 省市台 | MP3(+200) ctype=audio/mpeg 16384b | 976ms | `https://lhttp.qtfm.cn/live/270/64k.mp3` |
| OK | 云南新闻广播 | 省市台 | MP3(+25) ctype=audio/mpeg 16384b | 648ms | `https://lhttp.qtfm.cn/live/1926/64k.mp3` |
| OK | 云南音乐广播 | 省市台 | MP3(+81) ctype=audio/mpeg 16384b | 805ms | `https://lhttp.qtfm.cn/live/1929/64k.mp3` |
| OK | 北京交通广播 | 省市台 | MP3(+174) ctype=audio/mpeg 16384b | 1096ms | `https://lhttp.qtfm.cn/live/336/64k.mp3` |
| OK | 北京音乐广播 | 省市台 | MP3(+103) ctype=audio/mpeg 16384b | 983ms | `https://lhttp.qtfm.cn/live/332/64k.mp3` |
| OK | 四川交通广播 | 省市台 | MP3(+93) ctype=audio/mpeg 16384b | 728ms | `https://lhttp.qtfm.cn/live/4886/64k.mp3` |
| OK | 四川新闻广播 | 省市台 | MP3(+0) ctype=audio/mpeg 16384b | 930ms | `https://lhttp.qtfm.cn/live/4906/64k.mp3` |
| OK | 大连音乐广播 | 省市台 | MP3(+9) ctype=audio/mpeg 16384b | 642ms | `https://lhttp.qtfm.cn/live/1084/64k.mp3` |
| OK | 山东文艺广播 | 省市台 | MP3(+118) ctype=audio/mpeg 16384b | 745ms | `https://lhttp.qtfm.cn/live/20238/64k.mp3` |
| OK | 山东经济广播 | 省市台 | MP3(+148) ctype=audio/mpeg 16384b | 641ms | `https://lhttp.qtfm.cn/live/20236/64k.mp3` |
| OK | 广东交通之声 | 省市台 | MP3(+6) ctype=audio/mpeg 16384b | 877ms | `https://lhttp.qtfm.cn/live/1262/64k.mp3` |
| OK | 广东珠江经济台 | 省市台 | MP3(+2) ctype=audio/mpeg 16384b | 948ms | `https://lhttp.qtfm.cn/live/1259/64k.mp3` |
| OK | 广东音乐之声 | 省市台 | MP3(+53) ctype=audio/mpeg 16384b | 882ms | `https://lhttp.qtfm.cn/live/1260/64k.mp3` |
| OK | 深圳交通广播 | 省市台 | MP3(+45) ctype=audio/mpeg 16384b | 708ms | `https://lhttp.qtfm.cn/live/1272/64k.mp3` |
| OK | 深圳新闻广播 | 省市台 | MP3(+74) ctype=audio/mpeg 16384b | 959ms | `https://lhttp.qtfm.cn/live/1270/64k.mp3` |
| OK | 深圳音乐广播 | 省市台 | MP3(+22) ctype=audio/mpeg 16384b | 643ms | `https://lhttp.qtfm.cn/live/1271/64k.mp3` |
| OK | 温州音乐之声 | 省市台 | MP3(+181) ctype=audio/mpeg 16384b | 663ms | `https://lhttp.qtfm.cn/live/1149/64k.mp3` |
| OK | 湖北楚天交通广播 | 省市台 | MP3(+120) ctype=audio/mpeg 16384b | 827ms | `https://lhttp.qtfm.cn/live/1291/64k.mp3` |
| OK | 湖北经典音乐广播 | 省市台 | MP3(+93) ctype=audio/mpeg 16384b | 925ms | `https://lhttp.qtfm.cn/live/1296/64k.mp3` |
| OK | 福建交通广播 | 省市台 | MP3(+53) ctype=audio/mpeg 16384b | 534ms | `https://lhttp.qtfm.cn/live/1733/64k.mp3` |
| OK | 福建新闻综合广播 | 省市台 | MP3(+53) ctype=audio/mpeg 16384b | 490ms | `https://lhttp.qtfm.cn/live/1731/64k.mp3` |
| OK | 第一财经广播 | 省市台 | MP3(+93) ctype=audio/mpeg 16384b | 727ms | `https://lhttp.qtfm.cn/live/276/64k.mp3` |
| OK | 贵州音乐广播 | 省市台 | MP3(+7) ctype=audio/mpeg 16384b | 1488ms | `https://lhttp.qtfm.cn/live/20067/64k.mp3` |
| OK | 辽宁交通广播 | 省市台 | MP3(+62) ctype=audio/mpeg 16384b | 597ms | `https://lhttp.qtfm.cn/live/20025/64k.mp3` |
| OK | 陕西交通广播 | 省市台 | MP3(+10) ctype=audio/mpeg 16384b | 633ms | `https://lhttp.qtfm.cn/live/1601/64k.mp3` |
| OK | 陕西新闻广播 | 省市台 | MP3(+22) ctype=audio/mpeg 16384b | 504ms | `https://lhttp.qtfm.cn/live/1600/64k.mp3` |
| OK | 青岛新闻广播 | 省市台 | MP3(+83) ctype=audio/mpeg 16384b | 727ms | `https://lhttp.qtfm.cn/live/1673/64k.mp3` |
| OK | 两广之声音乐台 | 音乐 | MP3(+80) ctype=audio/mpeg 16384b | 629ms | `https://lhttp.qtfm.cn/live/20500149/64k.mp3` |
| OK | 云南音乐广播HLS | 音乐 | HLS 分片=MP3/ID3 ctype=audio/x-aac | 454ms | `https://gbw.ynradio.cn/radio/yygb.stream/playlist.m3u8` |
| OK | 亚洲粤语台 | 音乐 | MP3(+131) ctype=audio/mpeg 16384b | 246ms | `https://lhttp.qtfm.cn/live/15318569/64k.mp3` |
| OK | 华语金曲台 | 音乐 | MP3(+18) ctype=audio/mpeg 16384b | 567ms | `https://lhttp.qtfm.cn/live/5022308/64k.mp3` |
| OK | 吉林音乐广播 | 音乐 | HLS 分片=MPEG-TS ctype=video/mp2t | 412ms | `https://live-jlr.jlntv.cn/live/fm927.m3u8` |
| OK | 山东音乐广播 | 音乐 | HLS 分片=MPEG-TS ctype=video/MP2T | 75ms | `http://audiolive302.iqilu.com/sdradioYinyue/sdradio07/playlist.m3u8` |
| OK | 广州金曲音乐广播 | 音乐 | MP3(+100) ctype=audio/mpeg 16384b | 527ms | `https://lhttp.qtfm.cn/live/20192/64k.mp3` |
| OK | 怀集音乐之声 | 音乐 | MP3(+144) ctype=audio/mpeg 16384b | 715ms | `https://lhttp.qtfm.cn/live/4804/64k.mp3` |
| OK | 河南星河音乐广播 | 音乐 | MP3(+149) ctype=audio/mpeg 16384b | 741ms | `https://lhttp.qtfm.cn/live/20210755/64k.mp3` |
| OK | 河南音乐广播 | 音乐 | HLS 分片=MP3/ID3 ctype=audio/x-aac | 411ms | `https://stream.hndt.com/live/yinyue/playlist.m3u8` |
| OK | 清晨音乐台 | 音乐 | MP3(+25) ctype=audio/mpeg 16384b | 400ms | `https://lhttp.qtfm.cn/live/4915/64k.mp3` |
| FAIL | 香港电台第1台 | 中国香港 | 连不上：URLError: <urlopen error timed out> | 10138ms | `https://stm.rthk.hk/radio1` |
| FAIL | 香港电台第2台 | 中国香港 | 连不上：URLError: <urlopen error timed out> | 10134ms | `https://stm.rthk.hk/radio2` |
| FAIL | 香港电台第3台 | 中国香港 | 连不上：URLError: <urlopen error timed out> | 10047ms | `https://stm.rthk.hk/radio3` |
| FAIL | 香港电台第4台 | 中国香港 | 连不上：URLError: <urlopen error timed out> | 10095ms | `https://stm.rthk.hk/radio4` |
| FAIL | 法国音乐台 | 古典 | 连不上：URLError: <urlopen error timed out> | 10714ms | `http://direct.francemusique.fr/live/francemusique-midfi.mp3` |
| FAIL | 文艺之声 | 央广 | HTTP 404 | 1102ms | `https://lhttp.qtfm.cn/live/15318303/64k.mp3` |
| FAIL | 经典音乐广播 | 央广 | HTTP 404 | 959ms | `https://lhttp.qtfm.cn/live/15318302/64k.mp3` |
| FAIL | 音乐之声 | 央广 | 清单拉不到：HTTP 403 | 0ms | `https://ngcdn003.cnr.cn/live/yyzs/index.m3u8` |
| FAIL | Café del Mar | 氛围 | 连不上：URLError: <urlopen error timed out> | 10064ms | `https://streams.radio.co/se1a320b47/listen` |
| FAIL | 法国FIP爵士 | 爵士 | 连不上：URLError: <urlopen error timed out> | 10095ms | `http://icecast.radiofrance.fr/fipjazz-hifi.aac` |
| FAIL | 北京新闻广播 | 省市台 | HTTP 404 | 982ms | `https://lhttp.qtfm.cn/live/334/64k.mp3` |
| FAIL | 南京新闻广播 | 省市台 | HTTP 404 | 483ms | `https://lhttp.qtfm.cn/live/1050/64k.mp3` |
| FAIL | 天津相声广播 | 省市台 | HTTP 404 | 478ms | `https://lhttp.qtfm.cn/live/1063/64k.mp3` |
| FAIL | 浙江之声 | 省市台 | HTTP 404 | 433ms | `https://lhttp.qtfm.cn/live/1180/64k.mp3` |
| FAIL | 哈尔滨音乐广播 | 音乐 | 清单拉不到：URLError: <urlopen error [SSL: CERTIFICATE_VERIFY_FAILED] certificate verify failed: unable to get local issuer certificate (_ssl.c:1032)> | 0ms | `https://stream.hrbtv.net/yypl/playlist.m3u8` |

**OK 76 / WEAK 0 / 共 91**
