// FlowCast Windows client - UI string tables.
#include "i18n.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace fc {
namespace {

// --- Simplified Chinese -----------------------------------------------------
const Strings kZhHans = {
    /* tagline       */ L"把 Windows 屏幕和声音投到电视",
    /* targetLabel   */ L"投屏目标",
    /* scanning      */ L"正在搜索设备…",
    /* noReceiver    */ L"未发现设备",
    /* autoReceiver  */ L"自动选择",
    /* refresh       */ L"重新搜索",
    /* quality       */ L"画质",
    /* screen        */ L"捕获屏幕",
    /* screenMain    */ L" · 主屏",
    /* sound         */ L"声音",
    /* language      */ L"语言",
    /* start         */ L"开始投屏",
    /* stop          */ L"停止投屏",
    /* connecting    */ L"正在连接…",
    /* stopping      */ L"正在断开…",
    /* ready         */ L"就绪",
    /* readyHint     */ L"选好电视后开始投屏",
    /* mirroring     */ L"正在投屏",
    /* failed        */ L"连接失败",
    /* langSystem    */ L"跟随系统",
    /* q720          */ L"720p · 30 帧",
    /* q1080         */ L"1080p · 30 帧",
    /* q1080_60      */ L"1080p · 60 帧",
    /* q1440         */ L"1440p · 30 帧",
    /* qNative       */ L"原始分辨率",
    /* soundOn       */ L"系统声音",
    /* soundOff      */ L"仅画面",
    /* statTime      */ L"已投屏",
    /* statFps       */ L"帧率",
    /* statData      */ L"流量",
    /* manualHint    */ L"输入电视的 IP 地址，例如 192.168.0.201",
    /* manualEntry   */ L"手动输入地址…",
    /* deviceSuffix  */ L" 个设备",
    /* noLan         */ L"检查电视与电脑是否在同一 Wi-Fi",
    /* manualEscape  */ L"没看到电视？点上面的卡片手动输入地址",
    /* errRefused    */ L"确认目标设备已打开无线投屏、停在等待连接页",
    /* errNet        */ L"连不上这个地址，检查 IP 是否正确",
    /* errNoCapture  */ L"抓不到画面：屏幕可能已锁定，或正在远程桌面里运行",
    /* errDisconnected */ L"电视端已断开，投屏结束",
    /* errEncoder    */ L"画面编码器起不来：更新显卡驱动，或安装媒体功能包",
    /* logLink       */ L"查看日志 ›",
    /* trayShow      */ L"显示窗口",
    /* trayStop      */ L"停止投屏",
    /* trayQuit      */ L"退出",
    /* trayHint      */ L"FlowCast 已在后台运行，点击托盘图标可重新显示窗口",
    /* errHangup     */ L"接收端中途断开了连接：它可能只接受苹果设备的投屏",
};

// --- Traditional Chinese ----------------------------------------------------
const Strings kZhHant = {
    L"將 Windows 螢幕和聲音投到電視",
    L"投放目標",
    L"正在搜尋裝置…",
    L"未發現裝置",
    L"自動選擇",
    L"重新搜尋",
    L"畫質",
    L"擷取螢幕",
    L" · 主螢幕",
    L"聲音",
    L"語言",
    L"開始投放",
    L"停止投放",
    L"正在連線…",
    L"正在中斷…",
    L"就緒",
    L"選好電視後開始投放",
    L"正在投放",
    L"連線失敗",
    L"跟隨系統",
    L"720p · 30 格",
    L"1080p · 30 格",
    L"1080p · 60 格",
    L"1440p · 30 格",
    L"原始解析度",
    L"系統聲音",
    L"僅畫面",
    L"已投放",
    L"影格率",
    L"流量",
    L"輸入電視的 IP 位址，例如 192.168.0.201",
    L"手動輸入位址…",
    L" 個裝置",
    L"檢查電視與電腦是否在同一個 Wi-Fi",
    L"沒看到電視？點上面的卡片手動輸入位址",
    L"確認目標裝置已開啟無線投放、停在等待連線頁",
    L"連不上這個位址，檢查 IP 是否正確",
    L"擷取不到畫面：螢幕可能已鎖定，或正在遠端桌面中執行",
    L"電視端已中斷，投放結束",
    L"畫面編碼器起不來：更新顯示卡驅動程式，或安裝媒體功能包",
    L"檢視記錄 ›",
    L"顯示視窗",
    L"停止投放",
    L"結束",
    L"FlowCast 已在背景執行，點擊匣圖示可重新顯示視窗",
    L"接收端中途斷開了連線：它可能只接受蘋果裝置的投放",
};

// --- English ----------------------------------------------------------------
const Strings kEn = {
    L"Mirror this PC's screen and sound to your TV",
    L"Mirror to",
    L"Looking for receivers…",
    L"No receiver found",
    L"Automatic",
    L"Search again",
    L"Quality",
    L"Capture screen",
    L" · Primary",
    L"Sound",
    L"Language",
    L"Start mirroring",
    L"Stop mirroring",
    L"Connecting…",
    L"Disconnecting…",
    L"Ready",
    L"Pick a TV, then start mirroring",
    L"Mirroring",
    L"Connection failed",
    L"Follow system",
    L"720p · 30 fps",
    L"1080p · 30 fps",
    L"1080p · 60 fps",
    L"1440p · 30 fps",
    L"Native resolution",
    L"System audio",
    L"Picture only",
    L"Duration",
    L"Frame rate",
    L"Data",
    L"Enter the TV's IP address, e.g. 192.168.0.201",
    L"Enter an address…",
    L" receivers",
    L"Check that the TV and this PC are on the same Wi-Fi",
    L"No TV listed? Tap the card above to enter an address",
    L"Check the device has wireless display open and waiting",
    L"Cannot reach that address. Check the IP.",
    L"Cannot capture the screen - locked or remote session",
    L"The TV disconnected. Mirroring has stopped.",
    L"No H.264 encoder available - update the graphics driver",
    L"Open log ›",
    L"Show window",
    L"Stop mirroring",
    L"Quit",
    L"FlowCast is still running. Click the tray icon to show the window again.",
    L"The receiver hung up mid-handshake. It may only accept Apple senders.",
};

// Order must match the Lang enum indices.
const Strings* const kTables[kLangCount] = { &kZhHans, &kZhHans, &kZhHant, &kEn };

} // namespace

Lang system_language() {
    // GetUserDefaultUILanguage, not GetSystemDefaultUILanguage: a user who set
    // their own display language expects that one, not the install language.
    const LANGID id = ::GetUserDefaultUILanguage();
    if (PRIMARYLANGID(id) == LANG_CHINESE) {
        switch (SUBLANGID(id)) {
        case SUBLANG_CHINESE_TRADITIONAL: // zh-TW
        case SUBLANG_CHINESE_HONGKONG:    // zh-HK
        case SUBLANG_CHINESE_MACAU:       // zh-MO
            return Lang::ZhHant;
        default:
            return Lang::ZhHans;
        }
    }
    return Lang::En;
}

Lang resolve_lang(Lang setting) {
    switch (setting) {
    case Lang::ZhHans: return Lang::ZhHans;
    case Lang::ZhHant: return Lang::ZhHant;
    case Lang::En: return Lang::En;
    case Lang::System:
    default: return system_language();
    }
}

const Strings& strings(Lang resolved) {
    const int i = (int)resolved;
    if (i < 0 || i >= kLangCount) return *kTables[(int)Lang::En];
    return *kTables[i];
}

const wchar_t* font_family(Lang resolved) {
    switch (resolved) {
    case Lang::ZhHans: return L"Microsoft YaHei UI";
    case Lang::ZhHant: return L"Microsoft JhengHei UI";
    default: return L"Segoe UI";
    }
}

const wchar_t* language_label(Lang lang) {
    switch (lang) {
    case Lang::ZhHans: return L"简体中文";
    case Lang::ZhHant: return L"繁體中文";
    case Lang::En: return L"English";
    case Lang::System:
    default: return L"";
    }
}

} // namespace fc
