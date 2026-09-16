#!/usr/bin/env python3
"""Remapad 连接控制台：pc/remapadctl.py 会话的图形界面入口。

窗口只负责四件事：选串口连上设备、把会话输出显示出来、把按钮与输入框里的命令送进
同一个会话队列、把结构化事件（截图落盘、升级进度、链路断开）反映到界面上。
转发、截图、升级与命令处理的实现都在 remapadctl.py 与 link.py 里，界面不复制任何
链路或协议逻辑；串口仍然只有一个持有者，因此界面与命令行不要同时连同一个口。

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
from tkinter import filedialog

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
    ("连接键（开连接窗口）", "connect"),
    ("配新主机", "pairing start"),
    ("停止广播", "pairing stop"),
    ("唤醒主机", "wake"),
    ("断开主机", "drop"),
    ("进入屏幕操控", "ui on"),
    ("退出屏幕操控", "ui off"),
    ("拉取状态", "status"),
    ("拉取全部数据", ":all"),
    ("实机截图", ":shot"),
    ("软重启", "reboot"),
)

#: 常用命令：点击只填进输入框，回车才发送。
COMMAND_SNIPPETS = (
    "key a 200",
    "key release",
    "stick reset",
    "backlight 60",
    "screen off",
    "beep",
    "rumble off",
    "lamp 0xF",
    "haptic 0x10",
    "mode host",
    "relay 0",
    "rollback",
    "poweroff",
    ":help",
    ":log 15",
    ":log off",
)


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


class ConsoleWindow(ctk.CTk):
    """连接控制台：一个串口会话 + 一块实时日志 + 一组控制按钮。"""

    def __init__(self) -> None:
        super().__init__()
        self.title("Remapad 连接控制台")
        self.geometry("980x740")
        # 最小高度按「会话」页的内容量取：4 列按钮的最下面一行必须完整可见。
        self.minsize(880, 700)

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

        self.grid_columnconfigure(0, weight=1)
        self.grid_rowconfigure(1, weight=3)
        self.grid_rowconfigure(2, weight=2)
        self._build_toolbar()
        self._build_tabs()
        self._build_log()
        self._build_status_bar()

        self.protocol("WM_DELETE_WINDOW", self.on_close)
        self._apply_state()
        self.after(PUMP_INTERVAL_MS, self._pump_records)
        self.after(STATUS_INTERVAL_MS, self._pump_status)

    # --- 界面搭建 --------------------------------------------------

    def _build_toolbar(self) -> None:
        bar = ctk.CTkFrame(self, corner_radius=0)
        bar.grid(row=0, column=0, sticky="ew")
        bar.grid_columnconfigure(5, weight=1)

        ctk.CTkLabel(bar, text="串口", font=self.font_ui).grid(
            row=0, column=0, padx=(12, 4), pady=10)
        self.port_box = ctk.CTkComboBox(bar, width=110, values=["COM3"], font=self.font_ui)
        self.port_box.set("COM3")
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
        self._build_command_tab(self.tabs.add("命令"))
        self._build_upgrade_tab(self.tabs.add("升级"))

    def _build_session_tab(self, parent) -> None:
        parent.grid_columnconfigure(0, weight=1)
        parent.grid_rowconfigure(2, weight=1)

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
        actions.grid(row=2, column=0, sticky="nsew", padx=8, pady=(2, 8))
        for column in range(4):
            actions.grid_columnconfigure(column, weight=1)
        # 计数与标题同一行：纵向空间留给按钮，最小窗口下也不会裁掉最后一行。
        header = ctk.CTkFrame(actions, fg_color="transparent")
        header.grid(row=0, column=0, columnspan=4, sticky="ew", padx=12, pady=(8, 2))
        header.grid_columnconfigure(0, weight=1)
        ctk.CTkLabel(header, text="快捷操作", font=self.font_bold).grid(row=0, column=0, sticky="w")
        self.counters_label = ctk.CTkLabel(header, text="转发 0 ｜ 设备帧 0 ｜ 写回 0", font=self.font_ui)
        self.counters_label.grid(row=0, column=1, sticky="e")
        self.action_buttons: list[ctk.CTkButton] = []
        for index, (label, command) in enumerate(QUICK_ACTIONS):
            button = ctk.CTkButton(actions, text=label, height=30, font=self.font_ui,
                                   command=lambda cmd=command: self.send_command(cmd))
            button.grid(row=1 + index // 4, column=index % 4, sticky="ew", padx=6, pady=3)
            self.action_buttons.append(button)

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
                           "回复与固件日志一起落在下面的日志区，不受当前在哪一页影响")).grid(
            row=1, column=0, sticky="w", padx=12, pady=(4, 8))

        snippets = ctk.CTkFrame(parent)
        snippets.grid(row=2, column=0, sticky="nsew", padx=12, pady=(0, 12))
        for column in range(4):
            snippets.grid_columnconfigure(column, weight=1)
        for index, command in enumerate(COMMAND_SNIPPETS):
            ctk.CTkButton(snippets, text=command, height=28, font=self.font_ui,
                          command=lambda cmd=command: self.fill_snippet(cmd)).grid(
                row=index // 4, column=index % 4, sticky="ew", padx=6, pady=4)

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

    def refresh_ports(self) -> None:
        """列串口：读注册表，读不到就保留输入框里的值。"""
        ports = link.list_serial_ports()
        current = self.port_box.get().strip() or self.args.port
        self.port_box.configure(values=ports or [current])
        self._append_text(f"串口列表：{'  '.join(ports) if ports else '没有检测到串口'}")
        self._append_text(f"提示：当前选中的是 {current}，下拉里可以改（设备挂的是 USB-Serial/JTAG）")

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
        self.args.pad_path = self.selected_pad_path()
        session = remapadctl.Session(self.args, self.hid, ser, reporter=self.reporter)
        session.commands.put("status")  # 连上先看一眼设备活着，结果落在日志区
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
