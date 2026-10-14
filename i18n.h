#ifndef YAGP_I18N_H
#define YAGP_I18N_H

/* Every user-visible string, as a string-literal macro.
 *
 * - TXT_*    wide literals (L"...") for the Win32 UI.
 * - TXT_U8_* UTF-8 literals for messages raised inside Lua (script.c).
 *
 * The language is chosen at build time: cmake -DYAGP_LANG=en (default zh_CN).
 * To add a language, copy one block, translate it and add a YAGP_LANG_xx
 * branch here plus the value in CMakeLists.txt. Every block must define every
 * macro (a missing one is a compile error in that build), and printf
 * specifiers (%s, %d, %06X, %%) must keep the same order and types.
 * Strings that contain "\0" are file-dialog filters: keep the pairs and the
 * final "\0\0". Dialog layouts are fixed sizes, so keep dialog texts short. */

#if defined(YAGP_LANG_EN)

/* ---- tray ---------------------------------------------------------------- */
#define TXT_TRAY_TIP               L"YAGP GIF player"
#define TXT_MENU_ADD_GIF           L"Add GIF…"
#define TXT_MENU_PAUSE_ALL         L"Pause all"
#define TXT_MENU_RESUME_ALL        L"Resume all"
#define TXT_MENU_AUTOSTART         L"Start with Windows"
#define TXT_MENU_CLEAR_CONFIG      L"Delete settings data…"
#define TXT_MENU_EXIT              L"Exit"
#define TXT_TAG_PAUSED             L"  (paused)"
#define TXT_TAG_MOVING             L"  (moving)"
#define TXT_TAG_CLICKTHROUGH       L"  (click-through)"
#define TXT_TAG_HIDDEN             L"  (hidden)"

/* ---- per-GIF menu --------------------------------------------------------- */
#define TXT_MENU_PAUSE             L"Pause"
#define TXT_MENU_RESUME            L"Resume"
#define TXT_MENU_REPLACE           L"Replace GIF…"
#define TXT_MENU_SCALE             L"Scale (%d%%)…"
#define TXT_MENU_COLOR_KEY         L"Colour key"
#define TXT_MENU_KEY_CURRENT       L"Current: #%06X, tolerance %d"
#define TXT_MENU_KEY_HEX           L"Choose colour (hex)…"
#define TXT_MENU_KEY_CORNER        L"Use top-left pixel colour"
#define TXT_MENU_KEY_OFF           L"Turn colour key off"
#define TXT_MENU_SCRIPT            L"Motion script"
#define TXT_MENU_SCRIPT_FAILED     L"Script error, stopped"
#define TXT_MENU_SCRIPT_NONE       L"None"
#define TXT_MENU_SCRIPT_FILE       L"Choose script file…"
#define TXT_MENU_SCRIPT_NEW        L"New script from preset"
#define TXT_MENU_SCRIPT_RELOAD     L"Reload script"
#define TXT_MENU_TOPMOST           L"Always on top"
#define TXT_MENU_LOCK              L"Lock position"
#define TXT_MENU_CLICKTHROUGH      L"Click-through"
#define TXT_MENU_RECENTER          L"Move to screen centre"
#define TXT_MENU_REMOVE            L"Remove"
#define TXT_MENU_HIDE              L"Hide"
#define TXT_MENU_SHOW              L"Show"

/* ---- built-in motion presets ---------------------------------------------- */
#define TXT_PRESET_PATROL          L"Patrol left/right"
#define TXT_PRESET_WANDER          L"Wander"
#define TXT_PRESET_BOUNCE          L"Bounce off edges"
#define TXT_PRESET_FOLLOW          L"Follow mouse"
#define TXT_PRESET_FLEE            L"Avoid mouse"

/* ---- notifications -------------------------------------------------------- */
#define TXT_CLICKTHROUGH_ON_TITLE  L"Click-through enabled"
#define TXT_CLICKTHROUGH_ON_TEXT   L"This GIF no longer reacts to the mouse. Turn it off in the GIF's submenu in the tray menu."
#define TXT_NO_KEY_TITLE           L"No colour key needed"
#define TXT_NO_KEY_TEXT            L"The top-left pixel is already transparent."
#define TXT_HIDDEN_TITLE           L"GIF hidden"
#define TXT_HIDDEN_TEXT            L"It stays in the tray menu. Show it again from its submenu there."
#define TXT_CONFIG_DELETED         L"Settings deleted"
#define TXT_SCRIPT_STOPPED_TITLE   L"Motion script stopped: %s"
#define TXT_UNKNOWN_ERROR          L"Unknown error"

/* ---- errors (one %s argument unless noted) -------------------------------- */
#define TXT_ERR_INIT_WINDOW        L"Failed to initialise windows."
#define TXT_ERR_INIT_D3D           L"Failed to initialise D3D11 / DirectComposition."
#define TXT_ERR_OPEN_GIF_ADD       L"Cannot open GIF (invalid file, or larger than the 2048×2048 pixel budget):\n%s"
#define TXT_ERR_OPEN_GIF           L"Cannot open GIF:\n%s"
#define TXT_ERR_SWAPCHAIN          L"Cannot create a swap chain for this GIF:\n%s"
#define TXT_ERR_PICKER             L"Cannot start the file picker:\n%s"
#define TXT_ERR_TOO_MANY_FILES     L"Too many files selected at once. Please add them in smaller batches."
#define TXT_ERR_AUTOSTART          L"Cannot change the startup setting (%s)."
#define TXT_ERR_DELETE_CONFIG      L"Cannot delete the settings folder:\n%s"
#define TXT_ERR_DELETE_AUTOSTART   L"Cannot delete the startup entry:\n%s"
#define TXT_ERR_WRITE_SCRIPT       L"Cannot write the script file"     /* Message box title, no argument. */
#define TXT_ERR_BUILTIN_MISSING    L"Built-in script missing"           /* No argument. */
#define TXT_ERR_READ_SCRIPT        L"Cannot read the script file (missing or larger than 1 MB):\n%s"
/* Arguments: settings folder, cache folder, startup registry value. */
#define TXT_CLEAR_CONFIRM          L"This deletes:\nSettings folder %s\nCache folder %s\nStartup entry %s\n\n" \
                                   L"Until the next start nothing will be saved (the GIFs on screen stay). " \
                                   L"Saving resumes the next time the program starts.\n\nDelete?"

/* ---- file dialogs --------------------------------------------------------- */
#define TXT_TITLE_ADD_GIF          L"Add GIF"
#define TXT_TITLE_REPLACE_GIF      L"Replace GIF"
#define TXT_TITLE_PICK_SCRIPT      L"Choose motion script"
#define TXT_TITLE_NEW_SCRIPT       L"New motion script from preset"
#define TXT_FILTER_GIF             L"GIF animations (*.gif)\0*.gif\0All files\0*.*\0\0"
#define TXT_FILTER_LUA             L"Lua scripts (*.lua)\0*.lua\0All files\0*.*\0\0"
#define TXT_FILTER_LUA_ONLY        L"Lua scripts (*.lua)\0*.lua\0\0"

/* ---- dialogs -------------------------------------------------------------- */
#define TXT_OK                     L"OK"
#define TXT_CANCEL                 L"Cancel"
#define TXT_DLG_KEY_TITLE          L"Colour key"
#define TXT_DLG_KEY_COLOR          L"Colour to remove (hex, e.g. #00FF00 or #0F0):"
#define TXT_DLG_KEY_TOLERANCE      L"Tolerance (per channel 0-255):"
#define TXT_DLG_SCALE_TITLE        L"Scale"
#define TXT_DLG_SCALE_HINT         L"Drag to scale (live preview):"

/* ---- Lua host (UTF-8) ----------------------------------------------------- */
#define TXT_U8_SCRIPT_TIMEOUT      "script timed out (one call ran more than about %d instructions)"
#define TXT_U8_NON_STRING_ERROR    "(error object is not a string)"
#define TXT_U8_COORD_RANGE         "coordinate out of range"
#define TXT_U8_OUT_OF_MEMORY       "out of memory"
#define TXT_U8_LUA_STATE           "cannot create a Lua state"
#define TXT_U8_NO_GC               "__gc metamethods are not allowed (finalizers cannot be interrupted)"

#else /* zh_CN (default) */

/* ---- tray ---------------------------------------------------------------- */
#define TXT_TRAY_TIP               L"YAGP GIF 播放器"
#define TXT_MENU_ADD_GIF           L"添加 GIF…"
#define TXT_MENU_PAUSE_ALL         L"全部暂停"
#define TXT_MENU_RESUME_ALL        L"全部继续"
#define TXT_MENU_AUTOSTART         L"开机自启动"
#define TXT_MENU_CLEAR_CONFIG      L"删除配置数据…"
#define TXT_MENU_EXIT              L"退出"
#define TXT_TAG_PAUSED             L"  (已暂停)"
#define TXT_TAG_MOVING             L"  (运动)"
#define TXT_TAG_CLICKTHROUGH       L"  (穿透)"
#define TXT_TAG_HIDDEN             L"  (已隐藏)"

/* ---- per-GIF menu --------------------------------------------------------- */
#define TXT_MENU_PAUSE             L"暂停"
#define TXT_MENU_RESUME            L"继续播放"
#define TXT_MENU_REPLACE           L"更换 GIF…"
#define TXT_MENU_SCALE             L"缩放 (%d%%)…"
#define TXT_MENU_COLOR_KEY         L"色键抠图"
#define TXT_MENU_KEY_CURRENT       L"当前：#%06X，容差 %d"
#define TXT_MENU_KEY_HEX           L"指定颜色 (hex)…"
#define TXT_MENU_KEY_CORNER        L"使用左上角像素颜色"
#define TXT_MENU_KEY_OFF           L"关闭色键"
#define TXT_MENU_SCRIPT            L"运动脚本"
#define TXT_MENU_SCRIPT_FAILED     L"脚本出错，已停止"
#define TXT_MENU_SCRIPT_NONE       L"无"
#define TXT_MENU_SCRIPT_FILE       L"选择脚本文件…"
#define TXT_MENU_SCRIPT_NEW        L"基于预设新建脚本"
#define TXT_MENU_SCRIPT_RELOAD     L"重新加载脚本"
#define TXT_MENU_TOPMOST           L"置顶"
#define TXT_MENU_LOCK              L"锁定位置"
#define TXT_MENU_CLICKTHROUGH      L"鼠标穿透"
#define TXT_MENU_RECENTER          L"移到屏幕中央"
#define TXT_MENU_REMOVE            L"删除"
#define TXT_MENU_HIDE              L"隐藏"
#define TXT_MENU_SHOW              L"显示"

/* ---- built-in motion presets ---------------------------------------------- */
#define TXT_PRESET_PATROL          L"左右巡逻"
#define TXT_PRESET_WANDER          L"随机游走"
#define TXT_PRESET_BOUNCE          L"边缘反弹"
#define TXT_PRESET_FOLLOW          L"跟随鼠标"
#define TXT_PRESET_FLEE            L"躲避鼠标"

/* ---- notifications -------------------------------------------------------- */
#define TXT_CLICKTHROUGH_ON_TITLE  L"已开启鼠标穿透"
#define TXT_CLICKTHROUGH_ON_TEXT   L"该 GIF 不再响应鼠标。可在托盘菜单中对应 GIF 的子菜单里关闭穿透。"
#define TXT_NO_KEY_TITLE           L"无需色键"
#define TXT_NO_KEY_TEXT            L"左上角像素已经是透明的。"
#define TXT_HIDDEN_TITLE           L"已隐藏 GIF"
#define TXT_HIDDEN_TEXT            L"它仍在托盘菜单里，可在对应 GIF 的子菜单中重新显示。"
#define TXT_CONFIG_DELETED         L"配置已删除"
#define TXT_SCRIPT_STOPPED_TITLE   L"运动脚本已停止：%s"
#define TXT_UNKNOWN_ERROR          L"未知错误"

/* ---- errors (one %s argument unless noted) -------------------------------- */
#define TXT_ERR_INIT_WINDOW        L"窗口初始化失败。"
#define TXT_ERR_INIT_D3D           L"D3D11 / DirectComposition 初始化失败。"
#define TXT_ERR_OPEN_GIF_ADD       L"无法打开 GIF（文件无效，或尺寸超过 2048×2048 像素预算）：\n%s"
#define TXT_ERR_OPEN_GIF           L"无法打开 GIF：\n%s"
#define TXT_ERR_SWAPCHAIN          L"无法为该 GIF 创建交换链：\n%s"
#define TXT_ERR_PICKER             L"无法启动文件选择器：\n%s"
#define TXT_ERR_TOO_MANY_FILES     L"一次选择的文件过多，请分批添加。"
#define TXT_ERR_AUTOSTART          L"无法修改开机自启动设置（%s）。"
#define TXT_ERR_DELETE_CONFIG      L"无法删除配置目录：\n%s"
#define TXT_ERR_DELETE_AUTOSTART   L"无法删除开机自启动项：\n%s"
#define TXT_ERR_WRITE_SCRIPT       L"无法写入脚本文件"                 /* Message box title, no argument. */
#define TXT_ERR_BUILTIN_MISSING    L"内置脚本缺失"                     /* No argument. */
#define TXT_ERR_READ_SCRIPT        L"无法读取脚本文件（不存在或超过 1 MB）：\n%s"
/* Arguments: settings folder, cache folder, startup registry value. */
#define TXT_CLEAR_CONFIRM          L"将删除：\n配置目录 %s\n缓存目录 %s\n开机自启动项 %s\n\n" \
                                   L"删除后本次运行期间不再保存配置和缓存（当前显示的 GIF 不受影响）。" \
                                   L"下次启动程序时会重新开始保存。\n\n确定删除吗？"

/* ---- file dialogs --------------------------------------------------------- */
#define TXT_TITLE_ADD_GIF          L"添加 GIF"
#define TXT_TITLE_REPLACE_GIF      L"更换 GIF"
#define TXT_TITLE_PICK_SCRIPT      L"选择运动脚本"
#define TXT_TITLE_NEW_SCRIPT       L"基于预设新建运动脚本"
#define TXT_FILTER_GIF             L"GIF 动画 (*.gif)\0*.gif\0所有文件\0*.*\0\0"
#define TXT_FILTER_LUA             L"Lua 脚本 (*.lua)\0*.lua\0所有文件\0*.*\0\0"
#define TXT_FILTER_LUA_ONLY        L"Lua 脚本 (*.lua)\0*.lua\0\0"

/* ---- dialogs -------------------------------------------------------------- */
#define TXT_OK                     L"确定"
#define TXT_CANCEL                 L"取消"
#define TXT_DLG_KEY_TITLE          L"色键抠图"
#define TXT_DLG_KEY_COLOR          L"要扣除的颜色 (hex，如 #00FF00 或 #0F0)："
#define TXT_DLG_KEY_TOLERANCE      L"容差 (每通道 0-255)："
#define TXT_DLG_SCALE_TITLE        L"缩放"
#define TXT_DLG_SCALE_HINT         L"拖动滑块调整缩放比例（实时预览）："

/* ---- Lua host (UTF-8) ----------------------------------------------------- */
#define TXT_U8_SCRIPT_TIMEOUT      "脚本执行超时（单次调用超过约 %d 条指令）"
#define TXT_U8_NON_STRING_ERROR    "(非字符串错误对象)"
#define TXT_U8_COORD_RANGE         "坐标超出范围"
#define TXT_U8_OUT_OF_MEMORY       "内存不足"
#define TXT_U8_LUA_STATE           "无法创建 Lua 状态"
#define TXT_U8_NO_GC               "不允许使用 __gc 元方法（终结器无法被中断）"

#endif

#endif
