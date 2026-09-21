#!/usr/bin/env python3
"""Remapad 连接控制台：pc/remapadctl.py 会话的图形界面入口。

窗口只负责四件事：选串口连上设备、把会话输出显示出来、把按钮与输入框里的命令送进
同一个会话队列、把结构化事件（截图落盘、升级进度、链路断开）反映到界面上。
转发、截图、升级与命令处理的实现都在 remapadctl.py 与 link.py 里，界面不复制任何
链路或协议逻辑；串口仍然只有一个持有者，因此界面与命令行不要同时连同一个口。

页面按用途分四页：**会话**放转发开关、输入手柄与链路动作，**设置**把设备屏幕上的可改项
（亮度与息屏、手柄配色、DS4/DS5 行为、电源、设备信息）搬到 PC，没有屏幕也能改；
**命令**是调试口，常用命令按分组列成填词按钮（点了只填进输入框，回车才发），
**升级**推固件镜像。设置页的控件值一律来自固件回读行（remapadctl.parse_device_reply），
设备是唯一事实源，界面不自己记状态。

用法（在 pc/ 目录执行，仅 Windows）：
    uv run python remapadgui.py

键盘：命令输入框回车发送，↑ / ↓ 取历史；其余操作都在按钮与下拉里。
连接由用户手动发起：界面不做自动连接，也不写配置文件。
"""

from __future__ import annotations

import os
import queue
import sys
import threading
import time
from pathlib import Path

import customtkinter as ctk
import tkinter
from tkinter import filedialog, messagebox

import link
import remapadctl

# 仓库统一 UTF-8：界面里的中文与导出的日志都按这个编码走。
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8")

#: 界面字体：CustomTkinter 自带的 Roboto 没有中文字形，统一指定系统中文字体。
FONT_FAMILY = "Microsoft YaHei UI"
#: 日志区保留的最大行数，超出后从最旧的整行开始丢。
LOG_MAX_LINES = 4000
#: 队列排空与状态刷新的节拍（毫秒）；转发的 4 ms 节奏在工作线程里，界面按这个节拍画。
PUMP_INTERVAL_MS = 40
STATUS_INTERVAL_MS = 500
#: 滚动区域复查的节拍（毫秒）：Configure 事件里量到的尺寸可能是重排前那一轮的，
#: 用固定节拍兜住收敛（一次复查只读一次 bbox）。
SCROLL_INTERVAL_MS = 250
#: 断开与关窗时等工作线程收尾的上限（秒）。
JOIN_TIMEOUT_S = 2.0
#: 工具条里手柄摘要的显示上限：完整描述留给「会话」页的下拉（长名字会顶掉工具条）。
PAD_SUMMARY_CHARS = 26
#: 升级镜像默认路径：相对本文件解析，避免受启动目录影响。
DEFAULT_IMAGE = str((Path(__file__).resolve().parent / remapadctl.DEFAULT_IMAGE).resolve())

#: 状态灯文案与颜色：未连接 / 正在开关 / 已连接 / 链路断开。
STATE_STYLE = {
    "disconnected": ("● 未连接", "#8a8a8a"),
    "connecting": ("● 连接中", "#d7a63b"),
    "connected": ("● 已连接", "#3fa66a"),
    "broken": ("● 链路断开", "#e06c75"),
}

#: 快捷操作按钮：(按钮文字, 发到设备的命令)。
QUICK_ACTIONS = (
    ("配新主机", "pairing start"),
    ("停止广播", "pairing stop"),
    ("唤醒主机", "wake"),
    ("断开主机", "drop"),
    ("实机截图", ":shot"),
)

#: 命令页的分组：(分组标题, （命令…))。点击只填进输入框，回车才发送——
#: 调试动作都走这条路，界面上不再给它们单独开按钮。
COMMAND_GROUPS = (
    ("输入注入", ("key a 200", "key release", "stick reset", "rumble off", "lamp 0xF",
                  "haptic 0x10")),
    ("屏幕与连接", ("connect", "ui on", "ui off", "backlight 60", "screen off", "beep")),
    ("诊断与状态", ("status", ":all", "pad", "link", "mode host", "rollback", ":log 15",
                    ":help")),
)

#: 设置页的配色预设：(名称, 机身, 按键, 高光, 握把)，与 UI 手柄设置页的四款一致。
COLORWAYS = (
    ("标准黑", 0x232323, 0xA0A0A0, 0xE6E6E6, 0x323232),
    ("枪灰黑", 0x3A4045, 0x9AA3AB, 0xC8CDD2, 0x2B2F33),
    ("银灰", 0xB9BEC4, 0x6E757C, 0xE6E6E6, 0x8A9096),
    ("墨绿金", 0x1E3B2A, 0xC8A24A, 0xC8A24A, 0x16301F),
)

#: 四段配色的字段顺序（机身 / 按键 / 高光 / 握把，与固件 ctrl 命令一致）。
COLOR_FIELDS = ("机身", "按键", "高光", "握把")

#: 出厂占位配色：没读过设备前填在自定义输入框里（与固件默认值一致）。
DEFAULT_COLORS = (0x232323, 0xA0A0A0, 0xE6E6E6, 0x323232)

#: 读设置的回读命令：连上设备与点「读取当前设置」时各发一遍，回读行喂给设置控件。
SETTINGS_READ_COMMANDS = ("status", "version", "ctrl", "ds")

#: 亮度滑条范围：固件接受 0-100，0 只在息屏时出现，滑条下限留到 5。
BRIGHTNESS_MIN = 5
BRIGHTNESS_MAX = 100

#: 设备信息行的折行宽度（像素）：内容随回读在变，用固定宽度换掉随窗口重排。
INFO_WRAPLENGTH = 420

#: 页签区与日志区的最小高度（像素）：两边都留下限，窗口压到最小时谁都不会被挤没。
#: 页签区装不下的内容由页面自己的滚动容器接管，不再靠调大窗口回避。
TAB_MIN_HEIGHT = 300
LOG_MIN_HEIGHT = 150

#: 两个可滚页面的内容高度（像素）：设置页两列内容约 400，命令页三组按钮约 290。
#: 拿它当滚动容器的请求高度，窗口默认尺寸下两页都完整显示，窗口拉小才需要滚。
SETTINGS_BODY_HEIGHT = 400
COMMAND_BODY_HEIGHT = 290

#: 回读值的短标：固件给的是枚举串，界面换成中文。
PAIRING_TEXT = {
    "idle": "未配对",
    "scanning": "扫描中",
    "advertising": "连接中",
    "pairing": "配对中",
    "paired": "已配对",
    "connected": "已连接",
    "error": "配对出错",
}
IMAGE_TEXT = {"confirmed": "已确认", "pending-verify": "待验证"}
ROLE_TEXT = {"device": "串口", "host": "USB 主机"}


class QueueReporter(remapadctl.Reporter):
    """会话输出 → 队列：工作线程只放记录，控件一律由 Tk 主线程碰。"""

    def __init__(self, sink: "queue.Queue[dict]") -> None:
        self.sink = sink

    def line(self, text: str) -> None:
        self.sink.put({"kind": "line", "text": text})

    def error(self, text: str) -> None:
        self.sink.put({"kind": "error", "text": text})

    def event(self, name: str, **fields) -> None:
        record = {"kind": "event", "name": name}
        record.update(fields)
        self.sink.put(record)


def short_pad_name(description: str, limit: int = PAD_SUMMARY_CHARS) -> str:
    """工具条里的手柄摘要：太长就按词截断，完整描述留在「会话」页的下拉里。"""
    if len(description) <= limit:
        return description
    head, _, _tail = description[:limit].rpartition(" ")
    return (head or description[:limit]) + "…"


def port_summary(ports: list[str], current: str) -> str:
    """串口列表的一行摘要：标出当前选中的那个，选中项不在列表里也直说。"""
    if not ports:
        return "没有检测到串口（设备挂的是 USB-Serial/JTAG，插上后点「刷新」）"
    listed = "  ".join(f"{port}（已选）" if port == current else port for port in ports)
    if current and current not in ports:
        return f"串口 {len(ports)} 个：{listed}；当前选的 {current} 不在列表里"
    return f"串口 {len(ports)} 个：{listed}"


def port_selection(ports: list[str], current: str, fallback: str, chosen: bool) -> str:
    """刷新后该选中哪个串口：用户自己选过就守着他的选择，否则落在本机第一个口上。

    本机一个口都没有时保留手里的值（可能是马上要插上的板子，也可能是手动敲的）；
    用户清空过输入框就留空，让他自己填。
    """
    if chosen:
        return current
    return ports[0] if ports else (current or fallback)


def css_color(rgb: int) -> str:
    """0xRRGGBB → #rrggbb（配色按钮的底色用）。"""
    return f"#{rgb & 0xFFFFFF:06x}"


def readable_on(rgb: int) -> str:
    """按亮度挑前景色：亮底黑字、暗底白字（与 UI 配色块的描边判定同一条）。"""
    red, green, blue = (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF
    return "#000000" if 0.2126 * red + 0.7152 * green + 0.0722 * blue > 0.4 * 255 else "#ffffff"


def pressed_color(rgb: int, factor: float = 0.85) -> str:
    """配色按钮的悬停色：整体压暗一档，按钮不至于看起来是死的。"""
    channels = [max(0, min(255, round(((rgb >> shift) & 0xFF) * factor)))
                for shift in (16, 8, 0)]
    return "#%02x%02x%02x" % tuple(channels)


def parse_color(text: str) -> int | None:
    """输入框里的 0xRRGGBB / RRGGBB → 整数；不是一段合法配色返回 None。"""
    cleaned = text.strip().lower().removeprefix("0x")
    if not cleaned or len(cleaned) > 6:
        return None
    try:
        return int(cleaned, 16)
    except ValueError:
        return None


def format_uptime(seconds: int) -> str:
    """开机时长 → 时:分:秒；不足一小时只给 分:秒。"""
    hours, rest = divmod(max(seconds, 0), 3600)
    minutes, secs = divmod(rest, 60)
    return f"{hours}:{minutes:02d}:{secs:02d}" if hours else f"{minutes:02d}:{secs:02d}"


class ConsoleWindow(ctk.CTk):
    """连接控制台：一个串口会话 + 一块实时日志 + 一组控制按钮。"""

    def __init__(self) -> None:
        super().__init__()
        self.title("Remapad 连接控制台")
        # 默认高度按两个可滚页面的内容量取：默认尺寸下它们完整显示，窗口拉小才滚动。
        self.geometry("1040x860")
        self.minsize(900, 640)

        self.font_ui = ctk.CTkFont(family=FONT_FAMILY, size=13)
        self.font_bold = ctk.CTkFont(family=FONT_FAMILY, size=13, weight="bold")
        self.font_log = ctk.CTkFont(family=FONT_FAMILY, size=12)

        # 会话参数：默认值全部来自命令行入口，界面只覆盖串口与手柄两项。
        self.args = remapadctl.parse_args([])
        self.args.image = DEFAULT_IMAGE

        self.records: queue.Queue = queue.Queue()
        self.reporter = QueueReporter(self.records)
        self.command_var = ctk.StringVar()
        self.hid = None
        self.hid_problem = ""
        self.pad_entries: list[dict] = []
        self.session: remapadctl.Session | None = None
        self.worker: threading.Thread | None = None
        self.port_link: link.SerialLink | None = None
        # 注意：不能叫 self.state——CTk 窗口自己要用 state() 设标题栏配色，会互相遮住。
        self.session_state = "disconnected"
        self.state_text = "未连接"
        self.last_error = ""
        self.pending_ota_wait = False
        self.history: list[str] = []
        self.history_index = 0
        self.log_lines = 0
        self.closing = False
        #: 用户自己选过或敲过串口：刷新端口列表时不再替他改动选择。
        self.port_chosen = False
        #: 设备事实（版本 / 分区 / 电池 / 堆 / 配对……）：由回读行合并进来，界面只显示。
        self.device_facts: dict[str, object] = {}
        #: 需要连着设备才有意义的控件：断开时统一置灰。
        self.session_widgets: list = []
        #: 页面里的滚动容器：滚动条按内容与画布高度自己显示或收起，见 _sync_scroll。
        self.scroll_bodies: list = []

        self.grid_columnconfigure(0, weight=1)
        # 页签与日志按 2:1 分纵向空间：设置页内容比日志区更需要高度。
        self.grid_rowconfigure(1, weight=2, minsize=TAB_MIN_HEIGHT)
        self.grid_rowconfigure(2, weight=1, minsize=LOG_MIN_HEIGHT)
        self._build_toolbar()
        self._build_tabs()
        self._build_log()
        self._build_status_bar()

        self.protocol("WM_DELETE_WINDOW", self.on_close)
        self._apply_state()
        self.after(PUMP_INTERVAL_MS, self._pump_records)
        self.after(STATUS_INTERVAL_MS, self._pump_status)
        self.after(SCROLL_INTERVAL_MS, self._pump_scroll)

    # --- 界面搭建 --------------------------------------------------

    def _build_toolbar(self) -> None:
        bar = ctk.CTkFrame(self, corner_radius=0)
        bar.grid(row=0, column=0, sticky="ew")
        bar.grid_columnconfigure(5, weight=1)

        ctk.CTkLabel(bar, text="串口", font=self.font_ui).grid(
            row=0, column=0, padx=(12, 4), pady=10)
        # 值的落点由 refresh_ports 定：本机有口就落在第一个口上，用户自己选过之后不再改。
        self.port_box = ctk.CTkComboBox(bar, width=110, values=[self.args.port],
                                        font=self.font_ui, command=self.on_port_picked)
        self.port_box.set(self.args.port)
        self.port_box.bind("<KeyRelease>", self.on_port_picked)
        self.port_box.grid(row=0, column=1, padx=(0, 4), pady=10)
        ctk.CTkButton(bar, text="刷新", width=60, font=self.font_ui,
                      command=self.refresh_ports).grid(row=0, column=2, padx=(0, 12), pady=10)
        self.connect_button = ctk.CTkButton(bar, text="连接", width=90, font=self.font_bold,
                                            command=self.toggle_connection)
        self.connect_button.grid(row=0, column=3, padx=(0, 12), pady=10)
        self.state_light = ctk.CTkLabel(bar, text="● 未连接", text_color="#8a8a8a",
                                        font=self.font_bold)
        self.state_light.grid(row=0, column=4, padx=(0, 12), pady=10)
        self.pad_summary = ctk.CTkLabel(bar, text="手柄：未接入", anchor="e", font=self.font_ui)
        self.pad_summary.grid(row=0, column=5, sticky="e", padx=(0, 12), pady=10)

    def _build_tabs(self) -> None:
        self.tabs = ctk.CTkTabview(self)
        self.tabs.grid(row=1, column=0, sticky="nsew", padx=12, pady=(12, 8))
        self._build_session_tab(self.tabs.add("会话"))
        self._build_settings_tab(self.tabs.add("设置"))
        self._build_command_tab(self.tabs.add("命令"))
        self._build_upgrade_tab(self.tabs.add("升级"))

    def _build_session_tab(self, parent) -> None:
        parent.grid_columnconfigure(0, weight=1)

        forwarding = ctk.CTkFrame(parent)
        forwarding.grid(row=0, column=0, sticky="ew", padx=8, pady=(8, 2))
        forwarding.grid_columnconfigure(1, weight=1)
        self.forward_var = ctk.BooleanVar(value=True)
        self.forward_switch = ctk.CTkSwitch(forwarding, text="转发手柄到设备", variable=self.forward_var,
                                            command=self.on_forward_toggle, font=self.font_ui)
        self.forward_switch.grid(row=0, column=0, padx=(12, 16), pady=8, sticky="w")
        ctk.CTkLabel(forwarding, text="默认开启；关掉后手柄不再上行，命令与截图照常",
                     font=self.font_ui).grid(row=0, column=1, sticky="w", pady=8)

        pad_frame = ctk.CTkFrame(parent)
        pad_frame.grid(row=1, column=0, sticky="ew", padx=8, pady=2)
        pad_frame.grid_columnconfigure(1, weight=1)
        ctk.CTkLabel(pad_frame, text="输入手柄", font=self.font_ui).grid(
            row=0, column=0, padx=(12, 8), pady=8)
        self.pad_box = ctk.CTkComboBox(pad_frame, values=["未发现手柄"], state="readonly",
                                       font=self.font_ui, command=self.on_pad_selected)
        self.pad_box.grid(row=0, column=1, sticky="ew", pady=8)
        ctk.CTkButton(pad_frame, text="刷新", width=60, font=self.font_ui,
                      command=self.refresh_pads).grid(row=0, column=2, padx=(12, 4), pady=8)
        ctk.CTkButton(pad_frame, text="列出候选接口", width=110, font=self.font_ui,
                      command=self.log_candidates).grid(row=0, column=3, padx=(0, 12), pady=8)

        actions = ctk.CTkFrame(parent)
        actions.grid(row=2, column=0, sticky="new", padx=8, pady=(2, 8))
        for column in range(3):
            actions.grid_columnconfigure(column, weight=1)
        # 计数与标题同一行：纵向空间留给按钮，最小窗口下也不会裁掉最后一行。
        header = ctk.CTkFrame(actions, fg_color="transparent")
        header.grid(row=0, column=0, columnspan=3, sticky="ew", padx=12, pady=(8, 2))
        header.grid_columnconfigure(0, weight=1)
        ctk.CTkLabel(header, text="链路动作", font=self.font_bold).grid(row=0, column=0, sticky="w")
        self.counters_label = ctk.CTkLabel(header, text="转发 0 ｜ 设备帧 0 ｜ 写回 0", font=self.font_ui)
        self.counters_label.grid(row=0, column=1, sticky="e")
        self.action_buttons: list[ctk.CTkButton] = []
        for index, (label, command) in enumerate(QUICK_ACTIONS):
            button = ctk.CTkButton(actions, text=label, height=30, font=self.font_ui,
                                   command=lambda cmd=command: self.send_command(cmd))
            button.grid(row=1 + index // 3, column=index % 3, sticky="ew", padx=6, pady=3)
            self.action_buttons.append(button)
        hint_row = 1 + (len(QUICK_ACTIONS) - 1) // 3 + 1
        ctk.CTkLabel(actions, font=self.font_ui, justify="left", anchor="w",
                     text=("连接键、屏幕操控与状态回读不放按钮了：\n"
                           "在「命令」页点一下命令，再回车发送。")).grid(
            row=hint_row, column=0, columnspan=3, sticky="w", padx=12, pady=(4, 8))

    # --- 设置页 ----------------------------------------------------

    def _build_settings_tab(self, parent) -> None:
        """设置页：把设备屏幕上的可改项搬到 PC，没有屏幕的设备也能改。

        两列排布——左列屏幕与亮度、电源、设备信息，右列手柄配色与 DS4/DS5 设置。
        控件值全部来自固件回读行（parse_device_reply），设备是唯一事实源：
        写完命令等回读确认，界面不自己记状态。
        """
        body = self._scroll_body(parent, 0, SETTINGS_BODY_HEIGHT)
        body.grid_columnconfigure(0, weight=1, uniform="settings")
        body.grid_columnconfigure(1, weight=1, uniform="settings")
        left = ctk.CTkFrame(body, fg_color="transparent")
        left.grid(row=0, column=0, sticky="new", padx=(4, 6), pady=4)
        left.grid_columnconfigure(0, weight=1)
        right = ctk.CTkFrame(body, fg_color="transparent")
        right.grid(row=0, column=1, sticky="new", padx=(6, 4), pady=4)
        right.grid_columnconfigure(0, weight=1)
        self._build_screen_section(left, 0)
        self._build_power_section(left, 1)
        self._build_info_section(left, 2)
        self._build_color_section(right, 0)
        self._build_ds_section(right, 1)

    def _scroll_body(self, parent, row: int, height: int) -> ctk.CTkScrollableFrame:
        """页面里的可滚动内容容器：放不下时出现滚动条，装得下时自动收起。

        内容一律填满宽度（滚动条占右侧一条），父容器只给这一个子控件配权重。
        height 是内容装得下时希望占的高度：拿去当请求高度，窗口默认尺寸下页面就该
        完整显示，不靠用户先拉大窗口。
        """
        parent.grid_rowconfigure(row, weight=1)
        parent.grid_columnconfigure(0, weight=1)
        body = ctk.CTkScrollableFrame(parent, fg_color="transparent", corner_radius=0,
                                      height=height)
        body.grid(row=row, column=0, sticky="nsew", padx=(12, 4))
        body.grid_columnconfigure(0, weight=1)
        self.scroll_bodies.append(body)
        # 内容框架自身的尺寸变化与画布的重排都要重算滚动区域。
        for target in (body, body.master):
            target.bind("<Configure>",
                        lambda _event, frame=body: self._sync_scroll(frame), add="+")
        return body

    def _sync_scroll(self, frame: ctk.CTkScrollableFrame) -> None:
        """补上滚动区域（scrollregion）：不补的话滚动条拖不动、滚轮也不滚。

        CustomTkinter 只在内容框架的 Configure 里刷它，实测页签里的滚动框架拿不到
        那个事件，滚动区域一直是空的。内容框架挂在内部画布上，画布就是它的 master；
        拿不到画布（上游改了组成）就直接跳过，页面照旧显示，只是没了滚动。

        滚动条常驻：CustomTkinter 没有「装得下就收起」的开关，而映射状态要等下一轮
        事件循环才反映出来，按它判断会来回抖。
        """
        canvas = frame.master
        if not isinstance(canvas, tkinter.Canvas):
            return
        region = canvas.bbox("all")
        if region is None:
            return
        if canvas.cget("scrollregion") == " ".join(str(value) for value in region):
            return
        canvas.configure(scrollregion=region)

    def _pump_scroll(self) -> None:
        """按节拍复查滚动区域：Configure 里量到的尺寸可能是重排前那一轮的。"""
        if self.closing:
            return
        for body in self.scroll_bodies:
            self._sync_scroll(body)
        self.after(SCROLL_INTERVAL_MS, self._pump_scroll)

    def _section(self, parent, title: str, row: int) -> ctk.CTkFrame:
        """设置分组卡片：标题落在 row 0，内容行由调用方从 row 1 起自己排。"""
        frame = ctk.CTkFrame(parent)
        frame.grid(row=row, column=0, sticky="ew", pady=(0, 6))
        frame.grid_columnconfigure(1, weight=1)
        ctk.CTkLabel(frame, text=title, anchor="w", font=self.font_bold).grid(
            row=0, column=0, sticky="w", padx=12, pady=(8, 2))
        return frame

    def _section_hint(self, parent, row: int, text: str) -> None:
        """分组卡片里的说明行：文案自带换行，不跟着窗口宽度重排。"""
        label = ctk.CTkLabel(parent, text=text, anchor="w", justify="left", font=self.font_ui)
        label.grid(row=row, column=0, columnspan=2, sticky="ew", padx=12, pady=(2, 8))

    def _build_screen_section(self, parent, row: int) -> None:
        frame = self._section(parent, "屏幕与亮度", row)
        ctk.CTkLabel(frame, text="亮度", anchor="w", font=self.font_ui).grid(
            row=1, column=0, sticky="w", padx=(12, 8), pady=(6, 2))
        row_frame = ctk.CTkFrame(frame, fg_color="transparent")
        row_frame.grid(row=1, column=1, sticky="ew", padx=(0, 12), pady=(6, 2))
        row_frame.grid_columnconfigure(0, weight=1)
        # 拖动只更新数字，松手才发命令：每挪一格发一条会把命令队列刷满。
        self.brightness_slider = ctk.CTkSlider(row_frame, from_=BRIGHTNESS_MIN,
                                               to=BRIGHTNESS_MAX, command=self.on_brightness_drag)
        self.brightness_slider.set(60)
        self.brightness_slider.grid(row=0, column=0, sticky="ew")
        self.brightness_slider.bind("<ButtonRelease-1>", self.on_brightness_apply)
        self.brightness_label = ctk.CTkLabel(row_frame, text="60", width=36, anchor="e",
                                             font=self.font_ui)
        self.brightness_label.grid(row=0, column=1, padx=(8, 0))
        self.screen_off_var = ctk.BooleanVar(value=False)
        self.screen_switch = ctk.CTkSwitch(frame, text="息屏（只关背光）", font=self.font_ui,
                                           variable=self.screen_off_var,
                                           command=self.on_screen_toggle)
        self.screen_switch.grid(row=2, column=0, columnspan=2, sticky="w", padx=12, pady=(6, 2))
        self._section_hint(frame, 3, "亮度改完即落盘，调亮度会顺带亮屏。")
        self.session_widgets += [self.brightness_slider, self.screen_switch]

    def _build_power_section(self, parent, row: int) -> None:
        frame = self._section(parent, "电源", row)
        buttons = ctk.CTkFrame(frame, fg_color="transparent")
        buttons.grid(row=1, column=0, columnspan=2, sticky="ew", padx=12, pady=(6, 2))
        for column in range(2):
            buttons.grid_columnconfigure(column, weight=1)
        self.reboot_button = ctk.CTkButton(buttons, text="重启设备", height=30, font=self.font_ui,
                                           command=self.ask_reboot)
        self.reboot_button.grid(row=0, column=0, sticky="ew", padx=(0, 4))
        self.poweroff_button = ctk.CTkButton(buttons, text="设备关机", height=30, font=self.font_ui,
                                             fg_color="#8f3b3b", hover_color="#a24a4a",
                                             command=self.ask_poweroff)
        self.poweroff_button.grid(row=0, column=1, sticky="ew", padx=(4, 0))
        self._section_hint(frame, 2, "与屏幕电源页同一套命令：reboot 重启回 COM 模式，\n"
                                     "poweroff 释放电源锁存（USB 供电下关不掉）。")
        self.session_widgets += [self.reboot_button, self.poweroff_button]

    def _build_info_section(self, parent, row: int) -> None:
        frame = self._section(parent, "设备信息", row)
        self.read_button = ctk.CTkButton(frame, text="读取当前设置", width=110, height=28,
                                         font=self.font_ui, command=self.read_settings)
        self.read_button.grid(row=0, column=1, sticky="e", padx=12, pady=(6, 2))
        self.device_info_label = ctk.CTkLabel(
            frame, text="连接设备后自动读一次，也可以点右上角重读。", anchor="w",
            justify="left", wraplength=INFO_WRAPLENGTH, font=self.font_ui)
        self.device_info_label.grid(row=1, column=0, columnspan=2, sticky="ew", padx=12,
                                    pady=(4, 8))
        self.session_widgets.append(self.read_button)

    def _build_color_section(self, parent, row: int) -> None:
        frame = self._section(parent, "手柄配色", row)
        presets = ctk.CTkFrame(frame, fg_color="transparent")
        presets.grid(row=1, column=0, columnspan=2, sticky="ew", padx=12, pady=(6, 2))
        for column in range(len(COLORWAYS)):
            presets.grid_columnconfigure(column, weight=1)
        for index, (name, body, button, accent, grip) in enumerate(COLORWAYS):
            swatch = ctk.CTkButton(
                presets, text=name, width=96, height=30, font=self.font_ui,
                fg_color=css_color(body),
                hover_color=pressed_color(body), text_color=readable_on(body),
                command=lambda colors=(body, button, accent, grip): self.apply_colors(colors))
            swatch.grid(row=0, column=index, sticky="ew", padx=(0 if index == 0 else 4, 0))
            self.session_widgets.append(swatch)
        fields = ctk.CTkFrame(frame, fg_color="transparent")
        fields.grid(row=2, column=0, columnspan=2, sticky="ew", padx=12, pady=(8, 2))
        self.color_vars = [ctk.StringVar(value=f"0x{value:06x}") for value in DEFAULT_COLORS]
        for index, name in enumerate(COLOR_FIELDS):
            fields.grid_columnconfigure(index, weight=1)
            ctk.CTkLabel(fields, text=name, anchor="w", font=self.font_ui).grid(
                row=0, column=index, sticky="w", padx=(0 if index == 0 else 6, 0))
            entry = ctk.CTkEntry(fields, textvariable=self.color_vars[index], width=88,
                                 font=self.font_ui)
            entry.grid(row=1, column=index, sticky="ew", padx=(0 if index == 0 else 6, 0))
            self.session_widgets.append(entry)
        self.apply_colors_button = ctk.CTkButton(frame, text="应用配色", width=90, height=28,
                                                 font=self.font_ui, command=self.apply_custom_colors)
        self.apply_colors_button.grid(row=3, column=0, sticky="w", padx=12, pady=(8, 2))
        self.color_summary = ctk.CTkLabel(frame, text="当前：未读取", anchor="w", font=self.font_ui)
        self.color_summary.grid(row=3, column=1, sticky="w", padx=(0, 12), pady=(8, 2))
        self._section_hint(frame, 4, "四段依次是机身 / 按键 / 高光 / 握把，写下 0xRRGGBB；\n"
                                     "改完设备会断链并重开连接窗口，主机自己连回来。")
        self.session_widgets.append(self.apply_colors_button)

    def _build_ds_section(self, parent, row: int) -> None:
        frame = self._section(parent, "DS4、DS5 设置", row)
        self.ds_touchpad_var = ctk.BooleanVar(value=False)
        self.ds_touchpad_switch = ctk.CTkSwitch(
            frame, text="触摸板加减", variable=self.ds_touchpad_var, font=self.font_ui,
            command=lambda: self.send_ds_behavior("touchpad", self.ds_touchpad_var.get()))
        self.ds_touchpad_switch.grid(row=1, column=0, columnspan=2, sticky="w", padx=12,
                                     pady=(6, 2))
        self.ds_capture_var = ctk.BooleanVar(value=True)
        self.ds_capture_switch = ctk.CTkSwitch(
            frame, text="截图键", variable=self.ds_capture_var, font=self.font_ui,
            command=lambda: self.send_ds_behavior("capture", self.ds_capture_var.get()))
        self.ds_capture_switch.grid(row=2, column=0, columnspan=2, sticky="w", padx=12,
                                    pady=(4, 2))
        self._section_hint(frame, 3, "触摸板加减：左半区按下发减号、右半区发加号；\n"
                                     "截图键：触摸板按下发截图。两项都落盘在设备上。")
        self.session_widgets += [self.ds_touchpad_switch, self.ds_capture_switch]

    def _build_upgrade_tab(self, parent) -> None:
        parent.grid_columnconfigure(0, weight=1)

        ctk.CTkLabel(parent, text="镜像路径", anchor="w", font=self.font_ui).grid(
            row=0, column=0, sticky="w", padx=12, pady=(12, 4))
        row = ctk.CTkFrame(parent, fg_color="transparent")
        row.grid(row=1, column=0, sticky="ew", padx=12)
        row.grid_columnconfigure(0, weight=1)
        self.image_var = ctk.StringVar(value=DEFAULT_IMAGE)
        ctk.CTkEntry(row, textvariable=self.image_var, font=self.font_ui).grid(
            row=0, column=0, sticky="ew")
        ctk.CTkButton(row, text="浏览", width=60, font=self.font_ui, command=self.browse_image).grid(
            row=0, column=1, padx=(8, 0))
        ctk.CTkButton(row, text="校验镜像", width=80, font=self.font_ui,
                      command=self.validate_image).grid(row=0, column=2, padx=(8, 0))

        self.upgrade_button = ctk.CTkButton(parent, text="开始升级", font=self.font_bold,
                                            command=self.start_upgrade)
        self.upgrade_button.grid(row=2, column=0, sticky="ew", padx=12, pady=(12, 8))

        self.progress = ctk.CTkProgressBar(parent)
        self.progress.set(0.0)
        self.progress.grid(row=3, column=0, sticky="ew", padx=12, pady=(0, 4))
        self.progress_label = ctk.CTkLabel(parent, text="未开始", anchor="w", font=self.font_ui)
        self.progress_label.grid(row=4, column=0, sticky="w", padx=12)

        self.wait_var = ctk.BooleanVar(value=True)
        ctk.CTkSwitch(parent, text="升级完成后等设备回来并重新连接", variable=self.wait_var,
                      font=self.font_ui).grid(row=5, column=0, sticky="w", padx=12, pady=(12, 4))
        hint = "\n".join((
            "升级写进非运行分区，校验通过后设备自动重启，首次启动处于「待验证」状态；",
            "重启会让当前会话结束（链路断开是正常现象），勾选上面的开关会自动等设备回到串口。",
            "同一进程里转发与命令照常，升级期间不要关闭窗口。",
        ))
        ctk.CTkLabel(parent, text=hint, anchor="w", justify="left", font=self.font_ui).grid(
            row=6, column=0, sticky="w", padx=12, pady=(4, 12))

    def _build_command_tab(self, parent) -> None:
        parent.grid_columnconfigure(0, weight=1)

        row = ctk.CTkFrame(parent, fg_color="transparent")
        row.grid(row=0, column=0, sticky="ew", padx=12, pady=(12, 4))
        row.grid_columnconfigure(0, weight=1)
        self.command_entry = ctk.CTkEntry(
            row, textvariable=self.command_var, font=self.font_ui,
            placeholder_text="固件 CLI 命令，或 : 开头的工具命令（:help 看清单）")
        self.command_entry.grid(row=0, column=0, sticky="ew")
        self.send_button = ctk.CTkButton(row, text="发送", width=60, font=self.font_ui,
                                         command=self.submit_command)
        self.send_button.grid(row=0, column=1, padx=(8, 0))
        self.command_entry.bind("<Return>", self.submit_command)
        self.command_entry.bind("<Up>", lambda _event: self.recall_history(-1))
        self.command_entry.bind("<Down>", lambda _event: self.recall_history(1))

        ctk.CTkLabel(parent, anchor="w", justify="left", font=self.font_ui,
                     text=("回车发送；↑ / ↓ 取历史。下面的按钮只把命令填进输入框，确认后再回车。\n"
                           "回复与固件日志一起落在下面的日志区，不受当前在哪一页影响；"
                           "设备命令全表用 help 命令看")).grid(
            row=1, column=0, sticky="w", padx=12, pady=(4, 8))

        groups = self._scroll_body(parent, 2, COMMAND_BODY_HEIGHT)
        for group_index, (title, commands) in enumerate(COMMAND_GROUPS):
            ctk.CTkLabel(groups, text=title, anchor="w", font=self.font_bold).grid(
                row=group_index * 2, column=0, sticky="w", pady=(8 if group_index else 0, 2))
            grid = ctk.CTkFrame(groups, fg_color="transparent")
            grid.grid(row=group_index * 2 + 1, column=0, sticky="ew")
            for column in range(4):
                grid.grid_columnconfigure(column, weight=1)
            for index, command in enumerate(commands):
                ctk.CTkButton(grid, text=command, height=26, font=self.font_ui,
                              command=lambda cmd=command: self.fill_snippet(cmd)).grid(
                    row=index // 4, column=index % 4, sticky="ew", padx=4, pady=2)

    def _build_log(self) -> None:
        frame = ctk.CTkFrame(self)
        frame.grid(row=2, column=0, sticky="nsew", padx=12, pady=(0, 8))
        frame.grid_columnconfigure(0, weight=1)
        frame.grid_rowconfigure(1, weight=1)

        tools = ctk.CTkFrame(frame, fg_color="transparent")
        tools.grid(row=0, column=0, sticky="ew", padx=8, pady=(8, 0))
        self.stamp_var = ctk.BooleanVar(value=True)
        ctk.CTkSwitch(tools, text="时间戳", variable=self.stamp_var,
                      font=self.font_ui).grid(row=0, column=0, padx=(0, 12))
        self.autoscroll_var = ctk.BooleanVar(value=True)
        ctk.CTkSwitch(tools, text="自动滚动", variable=self.autoscroll_var,
                      font=self.font_ui).grid(row=0, column=1, padx=(0, 12))
        ctk.CTkButton(tools, text="清空", width=60, font=self.font_ui,
                      command=self.clear_log).grid(row=0, column=2, padx=(0, 8))
        ctk.CTkButton(tools, text="导出", width=60, font=self.font_ui,
                      command=self.export_log).grid(row=0, column=3, padx=(0, 8))
        ctk.CTkButton(tools, text=":help", width=60, font=self.font_ui,
                      command=lambda: self.send_command(":help")).grid(row=0, column=4)

        self.log_box = ctk.CTkTextbox(frame, font=self.font_log, activate_scrollbars=True)
        self.log_box.grid(row=1, column=0, sticky="nsew", padx=8, pady=(8, 8))
        self.log_box.configure(state="disabled")
        # tag_config 直接落到原生 Text：颜色选项是 foreground，不是控件的 text_color。
        self.log_box.tag_config("error", foreground="#e06c75")
        self.log_box.tag_config("send", foreground="#7aa2f7")
        self.log_box.tag_config("event", foreground="#3fa66a")

    def _build_status_bar(self) -> None:
        self.status_var = ctk.StringVar(value="未连接")
        self.status_label = ctk.CTkLabel(self, textvariable=self.status_var, anchor="w",
                                         justify="left", font=self.font_ui)
        self.status_label.grid(row=3, column=0, sticky="ew", padx=14, pady=(0, 8))
        # 状态栏会带上最近一次错误：按窗口宽度折行，别把内容挤出可视区。
        self.status_label.bind("<Configure>", self._wrap_status_label)

    def _wrap_status_label(self, event) -> None:
        self.status_label.configure(wraplength=max(event.width - 8, 200))

    # --- 启动与刷新 ------------------------------------------------

    def run(self) -> None:
        """刷新端口与手柄列表后进入事件循环。"""
        self.refresh_ports()
        self.refresh_pads()
        self.command_entry.focus_set()
        try:
            self.mainloop()
        finally:
            # 关窗按钮之外还有 Ctrl+C 之类的退出路径，这里兜底保证串口被放掉。
            self.shutdown_session()

    def on_port_picked(self, _value=None) -> None:
        """用户自己选过或敲过串口（下拉的 command 与输入框的按键共用）。"""
        self.port_chosen = True

    def refresh_ports(self) -> None:
        """列串口：本机有口就让选择落在第一个口上——只选中，不连接。

        用户自己选过或敲过之后不再改动他的选择（敲的那一个可能不在注册表列表里），
        连上以后同样不动：那时的取值已经用在了当前会话上。
        """
        ports = link.list_serial_ports()
        current = port_selection(ports, self.port_box.get().strip(), self.args.port,
                                 self.port_chosen)
        self.port_box.configure(values=ports or ([current] if current else []))
        if self.port_box.get().strip() != current:
            self.port_box.set(current)
        self._append_text(port_summary(ports, current))

    def refresh_pads(self) -> None:
        """列候选输入手柄：与命令行 --list 用的是同一份枚举。"""
        if self.hid is None:
            try:
                self.hid = remapadctl.load_hid()
                self.hid_problem = ""
            except remapadctl.HidUnavailable as exc:
                self.hid_problem = str(exc)
                self._append_text(self.hid_problem, tag="error")
                self._append_text("手柄转发相关控件不可用，串口命令与截图照常")
                self.pad_box.configure(values=["hidapi 不可用"])
                self.pad_box.set("hidapi 不可用")
                self._apply_state()
                return
        try:
            candidates = remapadctl.list_candidates(self.hid)
        except Exception as exc:  # hidapi 枚举硬件时可能抛任意 OSError
            self._append_text(f"枚举手柄失败：{exc}", tag="error")
            return
        self.pad_entries = candidates
        values = [remapadctl.describe(info) for info in candidates] or ["未发现手柄"]
        current = self.pad_box.get()
        self.pad_box.configure(values=values)
        # 先把控件状态摆好再写文本：disabled 状态下 CustomTkinter 的下拉写不进内容。
        self._apply_state()
        self.pad_box.set(current if current in values else values[0])
        self._append_text(f"候选手柄：{len(candidates)} 个（自动接入选中的那只）")

    def log_candidates(self) -> None:
        """把候选接口逐行打进日志：与命令行 --list 的输出对齐。"""
        if self.hid is None:
            self._append_text(self.hid_problem or "hidapi 不可用", tag="error")
            return
        try:
            candidates = remapadctl.list_candidates(self.hid)
        except Exception as exc:
            self._append_text(f"枚举手柄失败：{exc}", tag="error")
            return
        if not candidates:
            self._append_text("没有找到手柄接口")
            return
        for info in candidates:
            self._append_text(remapadctl.describe(info))

    # --- 连接与断开 ------------------------------------------------

    def toggle_connection(self) -> None:
        if self.session is None:
            self.connect()
        else:
            self.disconnect()

    def connect(self) -> None:
        """手动连接：打开串口、建会话、起工作线程。"""
        if self.session is not None:
            return
        port = self.port_box.get().strip()
        if not port:
            self._append_text("先选一个串口再连接", tag="error")
            return
        self._set_state("connecting", f"正在打开 {port}")
        try:
            ser = link.SerialLink(port, self.args.baud)
        except OSError as exc:
            hint = link.open_hint(exc)
            self.last_error = hint
            self._append_text(f"{port}: {hint}", tag="error")
            self._set_state("disconnected", "未连接")
            return
        self.args.port = port
        self.port_chosen = True
        self.args.pad_path = self.selected_pad_path()
        session = remapadctl.Session(self.args, self.hid, ser, reporter=self.reporter)
        # 连上先把设置读一遍：status 顺便确认设备活着，其余几条喂设置页的控件。
        for command in SETTINGS_READ_COMMANDS:
            session.commands.put(command)
        self.session = session
        self.port_link = ser
        self.worker = threading.Thread(target=self._run_session, args=(session, ser),
                                       name="remapad-gui-session", daemon=True)
        self.worker.start()
        self.forward_var.set(True)
        self._set_state("connected", f"已连接 {port}")
        self._append_text(f"会话已建立：{port}（转发手柄默认开启）", tag="event")

    def disconnect(self) -> None:
        """请求断开：工作线程在下一轮循环里自行停手、发 DETACH 并关端口。"""
        session = self.session
        if session is None:
            return
        self._set_state("connecting", "正在断开")
        session.stop = True

    def _run_session(self, session: remapadctl.Session, ser: link.SerialLink) -> None:
        """工作线程：会话主循环 + 收尾（发 DETACH、关端口、报结束）。"""
        code = 1
        try:
            code = session.run_interactive(read_stdin=False)
        except Exception as exc:  # 线程里冒出的异常不能让界面停在「已连接」
            self.reporter.error(f"会话异常结束：{exc!r}")
        finally:
            try:
                session.detach_pad()
            except OSError:
                pass
            ser.close()
            self.reporter.event("session_closed", code=code)

    def on_session_closed(self, code: int) -> None:
        self.session = None
        self.worker = None
        self.port_link = None
        self._set_state("disconnected" if code == 0 else "broken",
                        "会话已结束" if code == 0 else "链路断开")
        self._apply_state()
        if self.pending_ota_wait:
            self.pending_ota_wait = False
            self._append_text("等设备重启回来（最多 60 秒）……", tag="event")
            threading.Thread(target=self._wait_device_back, name="remapad-gui-wait",
                             daemon=True).start()
        elif code != 0:
            self._append_text("链路已断开：检查设备是否重启或拔线，然后重新连接", tag="error")

    def _wait_device_back(self) -> None:
        """升级后的等待：与命令行 --wait 同一实现，成功后请求界面重连。"""
        code = remapadctl.wait_for_version(self.args.port, self.args.baud, self.reporter)
        if code == 0:
            self.records.put({"kind": "event", "name": "device_back"})

    # --- 手柄 ------------------------------------------------------

    def selected_pad_path(self):
        """下拉里选中的手柄对应的 HID 接口路径；未选中返回 None（由固件/工具挑第一只）。"""
        text = self.pad_box.get()
        for info in self.pad_entries:
            if remapadctl.describe(info) == text:
                return info["path"]
        return None

    def on_pad_selected(self, _value: str) -> None:
        self.args.pad_path = self.selected_pad_path()
        self._append_text(f"输入手柄已选中：{self.pad_box.get()}")
        if self.session is not None and self.session.pad_info is not None:
            self._append_text("已接入的手柄会在下一轮重新接入新选中的那只")

    def on_forward_toggle(self) -> None:
        enabled = bool(self.forward_var.get())
        session = self.session
        if session is None:
            self._append_text("连接后转发开关才会生效（默认开启）")
            return
        session.forward = enabled
        self._append_text("手柄转发已开启" if enabled else "手柄转发已关闭（设备会收到 DETACH）")
        if not enabled:
            self._append_text("注意：用户自己按的组合键或串口 ui 命令不受影响")

    # --- 命令 ------------------------------------------------------

    def send_command(self, command: str) -> None:
        """把一行命令送进会话队列：不是 : 开头的按固件 CLI 原样发送。"""
        command = command.strip()
        if not command:
            return
        session = self.session
        if session is None:
            self._append_text("未连接设备，命令没有发出", tag="error")
            return
        session.commands.put(command)
        self._append_text(f"> {command}", tag="send")

    def submit_command(self, _event=None) -> str:
        text = self.command_var.get().strip()
        if not text:
            return "break"
        self.history.append(text)
        self.history_index = len(self.history)
        self.command_var.set("")
        self.send_command(text)
        return "break"

    def recall_history(self, step: int) -> str:
        if not self.history:
            return "break"
        self.history_index = max(0, min(len(self.history), self.history_index + step))
        if self.history_index >= len(self.history):
            self.command_var.set("")
        else:
            self.command_var.set(self.history[self.history_index])
        return "break"

    def fill_snippet(self, command: str) -> None:
        self.command_var.set(command)
        self.command_entry.focus_set()

    # --- 设置 ------------------------------------------------------

    def read_settings(self) -> None:
        """读一遍设备设置：控件值全部来自回读行，这里只负责把命令发出去。"""
        for command in SETTINGS_READ_COMMANDS:
            self.send_command(command)

    def on_brightness_drag(self, value: float) -> None:
        self.brightness_label.configure(text=f"{int(round(value))}")

    def on_brightness_apply(self, _event=None) -> None:
        """松手才发命令：拖动过程中的每一格都发一条会把命令队列刷满。"""
        value = int(round(self.brightness_slider.get()))
        self.brightness_label.configure(text=f"{value}")
        self.send_command(f"backlight {value}")
        # 固件把调亮度当亮屏动作（js_bridge_set_brightness）：息屏状态下也会恢复画面。
        self.screen_off_var.set(False)

    def on_screen_toggle(self) -> None:
        self.send_command("screen off" if self.screen_off_var.get() else "screen on")

    def apply_colors(self, colors: tuple[int, int, int, int]) -> None:
        """四段配色：与 UI 手柄设置页同一组取值，命令 ctrl。"""
        self.send_command("ctrl " + " ".join(f"0x{value:06x}" for value in colors))

    def apply_custom_colors(self) -> None:
        colors: list[int] = []
        for index, name in enumerate(COLOR_FIELDS):
            text = self.color_vars[index].get()
            value = parse_color(text)
            if value is None:
                self._append_text(f"{name}配色要写 0xRRGGBB，现在是 {text!r}", tag="error")
                return
            colors.append(value)
        self.apply_colors(tuple(colors))  # type: ignore[arg-type]

    def send_ds_behavior(self, key: str, on: bool) -> None:
        self.send_command(f"ds {key} {'on' if on else 'off'}")

    def ask_reboot(self) -> None:
        if messagebox.askyesno("确认重启", "重启会结束当前会话，设备回来后要重新连接。确定重启？",
                               parent=self):
            self.send_command("reboot")

    def ask_poweroff(self) -> None:
        if messagebox.askyesno("确认关机", "关机会断开链路；USB 供电下设备会重新上电。确定关机？",
                               parent=self):
            self.send_command("poweroff")

    def _absorb_reply(self, text: str) -> None:
        """设备回读行 → 设置控件：固件是唯一事实源，界面只跟着回读走。"""
        parsed = remapadctl.parse_device_reply(text)
        if parsed is None:
            return
        channel, fields = parsed
        if "light" in fields:
            light = int(fields["light"])
            self.brightness_slider.set(min(max(light, BRIGHTNESS_MIN), BRIGHTNESS_MAX))
            self.brightness_label.configure(text=f"{light}")
        if "screen_on" in fields:
            self.screen_off_var.set(not fields["screen_on"])
        if channel == "ctrl":
            colors = {key: int(fields[key]) for key in ("body", "button", "accent", "grip")}
            for var, key in zip(self.color_vars, ("body", "button", "accent", "grip")):
                var.set(f"0x{colors[key]:06x}")
            self.color_summary.configure(
                text="当前：" + " ".join(f"0x{colors[key]:06x}" for key in colors))
        elif channel == "ds":
            self.ds_touchpad_var.set(bool(fields["touchpad_plus_minus"]))
            self.ds_capture_var.set(bool(fields["capture_key"]))
        elif channel == "device":
            self.device_facts.update(fields)
            self._render_device_facts()

    def _render_device_facts(self) -> None:
        """设备事实拼成两行：没读到的项不占位。"""
        facts = self.device_facts
        image = str(facts.get("image") or "")
        first = [
            f"固件 {facts['firmware']}" if facts.get("firmware") else "",
            f"分区 {facts['partition']}" if facts.get("partition") else "",
            f"镜像 {IMAGE_TEXT.get(image, image)}" if image else "",
            f"升级 {facts['ota_state']}" if facts.get("ota_state") else "",
        ]
        second = []
        if facts.get("battery_mv") is not None:
            charge = "（充电中）" if facts.get("charging") else ""
            second.append(f"电量 {int(facts['battery_percent'])}% · "
                          f"{int(facts['battery_mv']) / 1000:.2f}V{charge}")
        if facts.get("heap") is not None:
            second.append(f"堆内存 {int(facts['heap']) // 1024} KB")
        if facts.get("uptime_s") is not None:
            second.append(f"运行 {format_uptime(int(facts['uptime_s']))}")
        pairing = str(facts.get("pairing") or "")
        if pairing:
            second.append(f"配对 {PAIRING_TEXT.get(pairing, pairing)}")
        role = str(facts.get("role") or "")
        if role:
            second.append(f"角色 {ROLE_TEXT.get(role, role)}")
        if facts.get("pad") and facts["pad"] != "none":
            second.append(f"手柄 {facts['pad']}")
        lines = [" ｜ ".join(part for part in line if part) for line in (first, second)]
        self.device_info_label.configure(
            text="\n".join(line for line in lines if line) or "设备没有回可读的状态")

    # --- 升级 ------------------------------------------------------

    def browse_image(self) -> None:
        path = filedialog.askopenfilename(title="选择固件镜像", filetypes=[
            ("应用镜像", "*.bin"), ("所有文件", "*.*")])
        if path:
            self.image_var.set(path)

    def validate_image(self) -> bool:
        """本地校验镜像：与 --dry-run 同一份检查，不接设备。"""
        path = Path(self.image_var.get().strip())
        try:
            image, version = remapadctl.load_image(path)
        except remapadctl.ImageError as exc:
            self.last_error = str(exc)
            self._append_text(str(exc), tag="error")
            self.progress_label.configure(text="镜像不合法")
            return False
        self._append_text(f"镜像校验通过：{path}（{len(image)} 字节，版本 {version}）", tag="event")
        return True

    def start_upgrade(self) -> None:
        if self.session is None:
            self._append_text("先连接设备再升级", tag="error")
            return
        if not self.validate_image():
            return
        self.pending_ota_wait = False
        self.progress.set(0.0)
        self.progress_label.configure(text="准备写入……")
        self.send_command(f":ota {self.image_var.get().strip()}")

    # --- 日志 ------------------------------------------------------

    def clear_log(self) -> None:
        self.log_box.configure(state="normal")
        self.log_box.delete("1.0", "end")
        self.log_box.configure(state="disabled")
        self.log_lines = 0

    def export_log(self) -> None:
        path = filedialog.asksaveasfilename(title="导出日志", defaultextension=".txt",
                                            filetypes=[("文本文件", "*.txt"), ("所有文件", "*.*")])
        if not path:
            return
        text = self.log_box.get("1.0", "end")
        try:
            Path(path).write_text(text, encoding="utf-8")
        except OSError as exc:
            self._append_text(f"导出失败：{exc}", tag="error")
            return
        self._append_text(f"日志已导出：{path}", tag="event")

    def _append_text(self, text: str, tag: str | None = None) -> None:
        """写入日志区（只由 Tk 主线程调用）。"""
        stamp = f"[{time.strftime('%H:%M:%S')}] " if self.stamp_var.get() else ""
        self.log_box.configure(state="normal")
        if tag is None:
            self.log_box.insert("end", f"{stamp}{text}\n")
        else:
            self.log_box.insert("end", f"{stamp}{text}\n", tag)
        self.log_lines += 1
        if self.log_lines > LOG_MAX_LINES:
            drop = self.log_lines - LOG_MAX_LINES
            self.log_box.delete("1.0", f"{drop + 1}.0")
            self.log_lines = LOG_MAX_LINES
        self.log_box.configure(state="disabled")
        if self.autoscroll_var.get():
            self.log_box.see("end")

    def _pump_records(self) -> None:
        """排空会话队列：普通行写日志，错误行标红，事件交给状态机。"""
        if self.closing:
            return
        records = []
        while True:
            try:
                records.append(self.records.get_nowait())
            except queue.Empty:
                break
        for record in records:
            if record["kind"] == "event":
                self._handle_event(record)
            elif record["kind"] == "error":
                self.last_error = record["text"]
                self._append_text(record["text"], tag="error")
            else:
                self._absorb_reply(record["text"])
                self._append_text(record["text"])
        self.after(PUMP_INTERVAL_MS, self._pump_records)

    def _handle_event(self, record: dict) -> None:
        name = record["name"]
        if name == "shot_saved":
            self._append_text(f"截图已落盘：{record['path']}", tag="event")
            self.open_screenshot(record["path"])
        elif name == "ota_progress":
            total = max(int(record["total"]), 1)
            confirmed = int(record["confirmed"])
            self.progress.set(min(confirmed / total, 1.0))
            self.progress_label.configure(
                text=f"写入 {confirmed} / {record['total']} 字节（{confirmed * 100 // total}%）")
        elif name == "ota_finished":
            self.progress.set(1.0 if record["ok"] else 0.0)
            self.progress_label.configure(text="升级完成" if record["ok"] else "升级失败")
            self.pending_ota_wait = bool(record["ok"] and self.wait_var.get())
            if record["ok"] and not self.pending_ota_wait:
                self._append_text("设备重启后点「连接」重新连上", tag="event")
        elif name == "session_closed":
            self.on_session_closed(int(record.get("code", 1)))
        elif name == "device_back":
            self._append_text("设备已回到串口，正在重新连接", tag="event")
            self.connect()
        elif name == "link_error":
            self.last_error = record["message"]
        elif name == "pad_attached":
            self._append_text(f"手柄已接入：{record['describe']}", tag="event")
        elif name == "pad_detached":
            self._append_text("手柄已断开（已向设备发 DETACH）")

    def open_screenshot(self, path: str) -> None:
        """用系统看图器打开截图：界面里不引 Pillow。"""
        try:
            os.startfile(path)
        except OSError as exc:
            self._append_text(f"打开截图失败：{exc}", tag="error")

    # --- 状态 ------------------------------------------------------

    def _set_state(self, state: str, text: str) -> None:
        self.session_state = state
        self.state_text = text
        label, color = STATE_STYLE[state]
        self.state_light.configure(text=label, text_color=color)
        self._apply_state()

    def _apply_state(self) -> None:
        connected = self.session is not None
        busy = self.session_state == "connecting"
        self.connect_button.configure(text="断开" if connected else "连接",
                                      state="disabled" if busy else "normal")
        for button in self.action_buttons:
            button.configure(state="normal" if connected else "disabled")
        for widget in self.session_widgets:
            widget.configure(state="normal" if connected else "disabled")
        self.upgrade_button.configure(state="normal" if connected else "disabled")
        self.send_button.configure(state="normal" if connected else "disabled")
        hid_ready = self.hid is not None
        self.forward_switch.configure(state="normal" if hid_ready else "disabled")
        self.pad_box.configure(state="readonly" if hid_ready else "disabled")

    def _pump_status(self) -> None:
        if self.closing:
            return
        session = self.session
        parts = [self.state_text]
        if session is not None:
            pad = remapadctl.describe(session.pad_info) if session.pad_info else "未接入"
            self.pad_summary.configure(text=f"手柄：{short_pad_name(pad)}")
            self.counters_label.configure(
                text=(f"转发 {session.reports} ｜ 设备帧 {session.frames}"
                      f" ｜ 写回 {session.outputs}"))
            parts.append(f"转发 {session.reports}")
            parts.append(f"设备帧 {session.frames}")
            parts.append(f"写回 {session.outputs}")
            if session.ota is not None:
                total = max(len(session.ota.image), 1)
                parts.append(f"升级 {session.ota.confirmed * 100 // total}%")
        else:
            self.pad_summary.configure(text="手柄：未接入")
        if self.last_error:
            parts.append(f"最近错误：{self.last_error}")
        self.status_var.set(" ｜ ".join(parts))
        self.after(STATUS_INTERVAL_MS, self._pump_status)

    # --- 关窗 ------------------------------------------------------

    def on_close(self) -> None:
        """关窗就是断开：先让会话收尾（发 DETACH、关端口），再销毁窗口。"""
        if self.closing:
            return
        self.closing = True
        self.shutdown_session()
        self.destroy()

    def shutdown_session(self) -> None:
        """同步收尾当前会话：等工作线程发完 DETACH 并关端口，超时就在主线程兜底。

        串口只有一个持有者，所以关窗必须把口放掉：正常情况由工作线程自己收尾；
        它若卡在一次长命令的等待里，就由主线程补一帧 DETACH 再关句柄，
        阻塞中的读会立刻失败，线程随后退出。本方法可以重复调用。
        """
        session, worker, ser = self.session, self.worker, self.port_link
        self.session = None
        self.worker = None
        self.port_link = None
        if session is None:
            return
        session.stop = True
        if worker is not None:
            worker.join(timeout=JOIN_TIMEOUT_S)
        if worker is not None and worker.is_alive():
            try:
                session.detach_pad()
            except OSError:
                pass
            if ser is not None:
                ser.close()
            worker.join(timeout=0.5)


def main() -> int:
    ctk.set_appearance_mode("system")
    ctk.set_default_color_theme("blue")
    window = ConsoleWindow()
    window.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
