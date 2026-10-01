#!/usr/bin/env python3
"""Remapad 按键注入 MCP 服务：自持按键状态的 CLI 注入引擎，供自动化程序与 agent 编排按键。

stdio MCP server 启动后不碰设备，由 remapad_connect 显式建链（串口 / netlog UDP 二选一）、
remapad_disconnect 断链；链路上内嵌 ctrl.Session 长会话。按键经固件调试注入 CLI（key/stick）
下发，MCP 自己维护按下状态与续期。设备控制类动作（状态/配对/截图）走固件 CLI 的只读与动作命令。
"""

from __future__ import annotations

import argparse
import atexit
import sys
import threading
import time
from pathlib import Path

import link
import ctrl
from link import (
    TYPE_PING,
    SerialLink,
    UdpLink,
    encode,
    open_hint,
    parse_endpoint,
)

from mcp.server.mcpserver import MCPServer

STICK_MAX = 4095
STICK_CENTER = 2048

#: 每次调用允许的按键数上限与 CLI 回复静默窗（镜像 ctrl.handle_line 的取值）。
MAX_KEYS_PER_CALL = 8
REPLY_SHORT_QUIET = 0.15
REPLY_LONG_QUIET = 0.25

#: 键名即固件调试注入的按键位名（pad_state.h 内部值，dp_source s_debug_keys）。
KEY_NAMES = (
    "circle", "cross", "triangle", "square",
    "l1", "r1", "l4", "r4", "l3", "r3",
    "up", "down", "left", "right",
    "opt", "touchpad", "home", "share", "mute",
)

#: 脚本时间线的规模上限：事件数与默认总时长（超限拒绝执行）。
MAX_SCRIPT_ACTIONS = 512
MAX_SCRIPT_EVENTS = 4096
DEFAULT_SCRIPT_MAX_MS = 60000
DEFAULT_TAP_MS = 200
MAX_HOLD_MS = 60000

#: 固件注入的单次保持上限（dp_source 钳制 60000ms）；无期限按住用更短的滚动期限，
#: 到期前由 tick 续期重发（掩码不变、倒计时拉满，不产生松开毛刺）。
HOLD_SLICE_MS = 55000
HOLD_REFRESH_MS = 5000


class McpError(RuntimeError):
    """工具面可预期失败：键名校验、参数越界、链路不可用等，直接透给 agent。"""


def validate_keys(keys, allow_empty: bool = False) -> list[str]:
    """键名列表校验：小写归一、去重保序、限制数量，返回规范键名。"""
    if not isinstance(keys, (list, tuple)) or (not keys and not allow_empty):
        raise McpError(f"keys 必须是非空键名列表，可用键名：{' '.join(KEY_NAMES)}")
    names: list[str] = []
    for key in keys:
        if not isinstance(key, str):
            raise McpError(f"键名必须是字符串，收到 {key!r}")
        name = key.strip().lower()
        if name not in KEY_NAMES:
            raise McpError(f"未知键名 {key!r}，可用键名：{' '.join(KEY_NAMES)}")
        if name not in names:
            names.append(name)
    if len(names) > MAX_KEYS_PER_CALL:
        raise McpError(f"一次最多 {MAX_KEYS_PER_CALL} 个键，收到 {len(names)} 个")
    return names


def validate_stick_xy(x, y) -> tuple[int, int]:
    """摇杆电平校验：0-4095 整数（2048 中位），越界或非整数报错。"""
    values = []
    for name, raw in (("x", x), ("y", y)):
        if isinstance(raw, bool) or not isinstance(raw, (int, float)) or raw < 0 or raw > STICK_MAX:
            raise McpError(f"摇杆 {name} 必须是 0-{STICK_MAX} 的数值（{STICK_CENTER} 中位）")
        values.append(int(round(raw)))
    return values[0], values[1]


class KeyStateEngine:
    """自持按键状态的注入引擎：下发固件调试 CLI 的 key/stick 命令。

    不变式：固件注入掩码 = 活跃键全集，倒计时 = 最小剩余期限。因此任何增删都把
    活跃键按剩余时长降序逐条重发（最后一条的倒计时即最早期限）；tap 在无按住键时
    直发（固件到点自动松开，零状态零流量），与按住键并存时作为有限期限入表。
    固件没有单键松开，子集松开只能 `key release` 全松后重发存活键——存活键在边界
    上有至多一个 dp 拍（5ms）的瞬断。状态变更来自工具线程；命令发送只允许会话
    线程（串口唯一写者），由 tick 驱动。
    """

    def __init__(self, ping_period_s: float = 1.0) -> None:
        self._lock = threading.Lock()
        self._held: dict[str, list] = {}  # 键名 → [期限 monotonic 秒, 是否滚动续期]
        self._stick = {
            "l": [STICK_CENTER, STICK_CENTER],
            "r": [STICK_CENTER, STICK_CENTER],
        }
        self._now: float | None = None  # 最近一次 tick 的时钟：工具线程据此打期限戳
        self._sent: frozenset[str] = frozenset()  # 最近一次下发的固件掩码
        self._stick_sent = {"l": [STICK_CENTER, STICK_CENTER], "r": [STICK_CENTER, STICK_CENTER]}
        self._keys_dirty = False
        self._stick_dirty = False
        self._release_pending = False  # 用户侧松空了按键表：固件掩码还在，需要显式 key release
        self._last_ping = 0.0
        self.ping_period_s = ping_period_s

    def _stamp(self) -> float:
        return self._now if self._now is not None else time.monotonic()

    # --- 状态变更（工具线程） ---------------------------------------

    def tap(self, names: list[str], hold_ms: int) -> None:
        with self._lock:
            deadline = self._stamp() + hold_ms / 1000.0
            for name in names:
                self._held[name] = [deadline, False]
            self._keys_dirty = True

    def press(self, names: list[str]) -> list[str]:
        with self._lock:
            added = [name for name in names if name not in self._held]
            for name in added:
                self._held[name] = [self._stamp() + HOLD_SLICE_MS / 1000.0, True]
            if added:
                self._keys_dirty = True
        return added

    def release(self, names: list[str]) -> list[str]:
        with self._lock:
            removed = [name for name in names if name in self._held]
            for name in removed:
                del self._held[name]
            if removed:
                self._keys_dirty = True
                if not self._held:
                    self._release_pending = True
        return removed

    def set_stick(self, side: str, x: int, y: int) -> None:
        with self._lock:
            self._stick[side] = [x, y]
            self._stick_dirty = True

    def reset_stick(self) -> None:
        with self._lock:
            self._stick = {
                "l": [STICK_CENTER, STICK_CENTER],
                "r": [STICK_CENTER, STICK_CENTER],
            }
            self._stick_dirty = True

    def clear(self) -> list[str]:
        """全部松开并回中：逃生口与脚本收尾共用（也提前终止在按的 tap）。"""
        with self._lock:
            held = list(self._held)
            self._held.clear()
            self._stick = {
                "l": [STICK_CENTER, STICK_CENTER],
                "r": [STICK_CENTER, STICK_CENTER],
            }
            if held:
                self._keys_dirty = True
                self._release_pending = True
            self._stick_dirty = True
        return held

    def state(self) -> dict:
        with self._lock:
            return {
                "held": list(self._held),
                "stick": {"l": list(self._stick["l"]), "r": list(self._stick["r"])},
            }

    # --- 命令发送（仅会话线程） -------------------------------------

    def tick(self, now: float, session) -> None:
        """会话主循环驱动：UDP 保活、到期剪枝、滚动续期与脏状态下发。"""
        self._now = now
        if isinstance(session.link, UdpLink) and now - self._last_ping >= self.ping_period_s:
            session.link.write(encode(TYPE_PING, 0, session.seq, b""))
            session.seq = (session.seq + 1) & 0xFF
            self._last_ping = now
        with self._lock:
            expired = [name for name, entry in self._held.items()
                       if not entry[1] and now >= entry[0]]
            for name in expired:
                del self._held[name]
            if expired:
                # 固件倒计时归零时清掉整个掩码，存活键要重发，已发集合随之清空。
                self._sent = frozenset()
                self._keys_dirty = True
            refreshed = [name for name, entry in self._held.items()
                         if entry[1] and entry[0] - now < HOLD_REFRESH_MS / 1000.0]
            for name in refreshed:
                self._held[name] = [now + HOLD_SLICE_MS / 1000.0, True]
            if refreshed:
                self._keys_dirty = True
        if self._keys_dirty:
            self._flush_keys(session, now)
        if self._stick_dirty:
            self._flush_stick(session)

    def _flush_keys(self, session, now: float) -> None:
        with self._lock:
            entries = sorted(((deadline, name) for name, (deadline, _rolling) in self._held.items()),
                             reverse=True)
            release_pending = self._release_pending
            stale = self._sent - {name for _deadline, name in entries}
            self._keys_dirty = False
            self._release_pending = False
        if not entries:
            # 自然到期（tap 到点）固件已自行清掉掩码，不必多发；用户松空才显式全松。
            if release_pending:
                session.send_cli("key release")
                self._sent = frozenset()
            return
        # 掩码按位或、单键清不掉：下发集合有缩小时先 key release 再重发，
        # 随后按剩余降序逐条发（最后一条的倒计时 = 最早期限）。
        if stale or release_pending:
            session.send_cli("key release")
        for deadline, name in entries:
            remaining = max(1, round((deadline - now) * 1000))
            session.send_cli(f"key {name} {min(remaining, MAX_HOLD_MS)}")
        self._sent = frozenset(name for _deadline, name in entries)

    def _flush_stick(self, session) -> None:
        with self._lock:
            stick = {side: list(levels) for side, levels in self._stick.items()}
            sent = self._stick_sent
            self._stick_dirty = False
        if all(levels == [STICK_CENTER, STICK_CENTER] for levels in stick.values()):
            if sent is None or any(sent.get(side) != stick[side] for side in stick):
                session.send_cli("stick reset")
                self._stick_sent = {side: list(levels) for side, levels in stick.items()}
            return
        for side in ("l", "r"):
            if sent is None or sent.get(side) != stick[side]:
                x, y = stick[side]
                session.send_cli(f"stick {side} {x} {y}")
        self._stick_sent = {side: list(levels) for side, levels in stick.items()}


class ReplySink(ctrl.Reporter):
    """设备文本行收集器：供工具线程按静默窗收取 CLI 回复；噪音行不入收集。"""

    NOISE_PREFIXES = ("设备在线", "已转发", "反馈", "（已合并", "会话已连接",
                      "ok key injected", "ok keys released", "ok stick set", "ok sticks centered")

    def __init__(self) -> None:
        super().__init__()
        self._lock = threading.Lock()
        self._lines: list[tuple[float, str]] = []

    def line(self, text: str) -> None:
        self._append(text)

    def error(self, text: str) -> None:
        self._append(text)

    def event(self, name: str, **fields) -> None:
        pass

    def _append(self, text: str) -> None:
        if text.startswith(self.NOISE_PREFIXES):
            return
        with self._lock:
            self._lines.append((time.monotonic(), link.mask_secrets(text)))

    def mark(self) -> int:
        with self._lock:
            return len(self._lines)

    def collect(self, mark: int, wait_s: float) -> list[str]:
        """收取 mark 之后的行：任何行推静默窗，ok/err/pong 短窗，硬截止兜底。"""
        hard = time.monotonic() + wait_s
        quiet_deadline = 0.0
        while True:
            now = time.monotonic()
            with self._lock:
                pending = self._lines[mark:]
            if pending:
                window = REPLY_SHORT_QUIET if pending[-1][1].startswith(("ok", "err", "pong")) else REPLY_LONG_QUIET
                quiet_deadline = now + window
            if (pending and now >= quiet_deadline) or now >= hard:
                break
            time.sleep(0.005)
        with self._lock:
            lines = [text for _, text in self._lines[mark:]]
            del self._lines[mark:]
        return lines


class EngineSession(ctrl.Session):
    """会话子类：在主循环里挂引擎的命令下发，串口唯一写者仍只在会话线程。"""

    def __init__(self, engine: KeyStateEngine, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)
        self._engine = engine

    def pump(self, now: float) -> None:
        self._engine.tick(now, self)
        super().pump(now)


class PadBridge:
    """MCP 工具面与会话线程之间的桥：链路写全部经由会话线程，工具调用串行化。

    连接是显式动作：服务启动不碰设备，remapad_connect 建链后其余工具才可用，
    remapad_disconnect 断链（松键、key release、关链路）。
    """

    def __init__(self, ctrl_args: argparse.Namespace, script_max_ms: int, ping_period_s: float) -> None:
        self.ctrl_args = ctrl_args
        self.script_max_ms = script_max_ms
        self.engine = KeyStateEngine(ping_period_s=ping_period_s)
        self.sink = ReplySink()
        self.lock = threading.RLock()
        self.session: EngineSession | None = None
        self.worker: threading.Thread | None = None

    def connect(self, port: str | None, net: str | None) -> dict:
        """建立设备链路：串口 port 或 netlog UDP 地址 net 二选一，无参回落启动默认。"""
        with self.lock:
            if self.session is not None and self.worker is not None and self.worker.is_alive():
                raise McpError("设备已连接，先 remapad_disconnect 再换链路")
            port, net = port or self.ctrl_args.port, net or self.ctrl_args.net
            if bool(port) == bool(net):
                raise McpError("需要 port（串口号，如 COM12）或 net（设备IP[:端口]）恰好一个")
            try:
                if net:
                    host, port_num = parse_endpoint(net)
                    conn = UdpLink(host, port_num)
                    link_desc = f"{host}:{port_num}（WiFi netlog）"
                else:
                    # 直接构造 SerialLink：open_port 失败会以 SystemExit 结束进程，
                    # 在工具线程里只会无声卡死请求，这里改为转换成 McpError。
                    conn = SerialLink(port, self.ctrl_args.baud)
                    link_desc = f"{port}（串口）"
            except OSError as exc:
                raise McpError(f"打开设备链路失败：{exc}\n{open_hint(exc)}")
            session = EngineSession(self.engine, self.ctrl_args, None, conn, reporter=self.sink)
            self.session = session
            self.worker = threading.Thread(
                target=self._run_session, args=(session, conn), name="remapad-mcp-session", daemon=True)
            self.worker.start()
        return {"ok": True, "link": link_desc}

    def disconnect(self) -> dict:
        """断开设备链路：会话收尾会 key release 全松并关链路。"""
        with self.lock:
            session, worker = self.session, self.worker
            self.session, self.worker = None, None
        if session is None:
            return {"ok": True, "link": "未连接"}
        self.engine.clear()
        session.stop = True
        if worker is not None:
            worker.join(timeout=3)
        return {"ok": True, "link": "已断开", "engine": self.engine.state()}

    def stop(self) -> None:
        """进程退出钩子：静默断开。"""
        try:
            self.disconnect()
        except Exception:
            pass

    def _session(self) -> EngineSession:
        """当前活跃会话；未连接或会话线程已死一律报错，不隐式重连。"""
        session, worker = self.session, self.worker
        if session is None or worker is None or not worker.is_alive():
            raise McpError("设备未连接：先调用 remapad_connect（port=串口号 或 net=IP:端口）")
        return session

    def query(self, command: str, wait_s: float | None = None) -> list[str]:
        """发一条固件 CLI 并收回复行：命令经会话队列落线程，回复按静默窗收集。"""
        with self.lock:
            session = self._session()
            mark = self.sink.mark()
            session.commands.put(command)
            return self.sink.collect(mark, wait_s if wait_s is not None else self.ctrl_args.reply_wait)

    def screenshot(self, path: str | None) -> dict:
        with self.lock:
            session = self._session()
            if isinstance(session.link, UdpLink):
                raise McpError("截图只支持串口会话（netlog UDP 通道不回传图像帧）")
            target = Path(path).expanduser() if path else ctrl.default_shot_path()
            session.shot_path = target
            session.shot_ready = False
            session.shot.begin(time.monotonic(), session.args.shot_timeout)
            session.commands.put("shot")
            deadline = time.monotonic() + session.args.shot_timeout
            while not session.shot_ready and time.monotonic() < deadline:
                time.sleep(0.02)
            if not session.shot_ready:
                raise McpError(f"{session.args.shot_timeout:.0f} 秒内没有收到完整截图")
            return {"path": str(session.shot_path)}

    def _run_session(self, session: EngineSession, conn) -> None:
        try:
            session.run_interactive(read_stdin=False)
        except Exception as exc:
            self.sink.error(f"会话异常结束：{exc!r}")
        finally:
            # 泵循环可能因 stop 直接退出，收尾不依赖脏标记：直接下发全松与回中。
            try:
                session.send_cli("key release")
                session.send_cli("stick reset")
            except OSError:
                pass
            try:
                conn.close()
            except OSError:
                pass


def compile_script(actions, loop, max_ms: int) -> tuple[list[tuple], float]:
    """把 actions 编译成事件时间线：tap 展开成 down/up 对，按 (t, 种类) 稳定排序。

    返回 (events, span_ms)；同刻事件的执行顺序为 up → down → stick。
    """
    if not isinstance(actions, list) or not actions:
        raise McpError("actions 必须是非空数组")
    if len(actions) > MAX_SCRIPT_ACTIONS:
        raise McpError(f"actions 最多 {MAX_SCRIPT_ACTIONS} 项，收到 {len(actions)} 项")
    if isinstance(loop, bool) or not isinstance(loop, int) or loop < 1 or loop > 1000:
        raise McpError("loop 必须是 1-1000 的整数")
    events: list[tuple[float, int, str, object]] = []
    for index, action in enumerate(actions):
        if not isinstance(action, dict):
            raise McpError(f"actions[{index}] 必须是对象")
        at_ms = action.get("t", 0)
        if isinstance(at_ms, bool) or not isinstance(at_ms, (int, float)) or at_ms < 0:
            raise McpError(f"actions[{index}].t 必须是不小于 0 的毫秒数")
        for name in validate_keys(action.get("up", []), allow_empty=True):
            events.append((at_ms, 0, "up", name))
        for name in validate_keys(action.get("down", []), allow_empty=True):
            events.append((at_ms, 1, "down", name))
        tap_keys = validate_keys(action.get("tap", []), allow_empty=True)
        hold_ms = action.get("hold_ms", DEFAULT_TAP_MS)
        if isinstance(hold_ms, bool) or not isinstance(hold_ms, (int, float)) or hold_ms < 1 or hold_ms > MAX_HOLD_MS:
            raise McpError(f"actions[{index}].hold_ms 必须是 1-{MAX_HOLD_MS} 的毫秒数")
        for name in tap_keys:
            events.append((at_ms, 1, "down", name))
            events.append((at_ms + hold_ms, 0, "up", name))
        stick = action.get("stick")
        if stick is not None:
            side = stick.get("side")
            if side not in ("l", "r"):
                raise McpError(f"actions[{index}].stick.side 必须是 l 或 r")
            x, y = validate_stick_xy(stick.get("x"), stick.get("y"))
            events.append((at_ms, 2, "stick", (side, x, y)))
        if action.get("stick_reset"):
            events.append((at_ms, 2, "stick_reset", None))
        if len(events) > MAX_SCRIPT_EVENTS:
            raise McpError(f"展开后的事件数超过 {MAX_SCRIPT_EVENTS}")
    if not events:
        raise McpError("actions 里没有可执行的动作字段（down/up/tap/stick/stick_reset）")
    events.sort(key=lambda item: (item[0], item[1]))
    span_ms = max(item[0] for item in events)
    total_ms = span_ms * loop
    if total_ms > max_ms:
        raise McpError(f"脚本总时长 {total_ms:.0f}ms 超过上限 {max_ms}ms（可调 --script-max-ms）")
    return events, span_ms


def run_script(engine: KeyStateEngine, events: list[tuple], loop: int) -> float:
    """按时间线驱动引擎：阻塞执行，结束（含异常路径）松开全部并回中。"""
    started = time.monotonic()
    try:
        for _ in range(loop):
            rep_started = time.monotonic()
            for at_ms, _order, kind, payload in events:
                delay = rep_started + at_ms / 1000.0 - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
                if kind == "down":
                    engine.press([payload])
                elif kind == "up":
                    engine.release([payload])
                elif kind == "stick":
                    engine.set_stick(payload[0], payload[1], payload[2])
                else:
                    engine.reset_stick()
    finally:
        engine.clear()
    return (time.monotonic() - started) * 1000.0


SERVER = MCPServer(
    name="remapad-pad",
    title="Remapad 按键注入",
    instructions=(
        "向 NS2 主机注入按键供自动化与 agent 编排，注入走固件调试 CLI。"
        "先 remapad_connect 连接设备（port=串口号 或 net=IP:端口 二选一），"
        "之后其余工具才可用；结束用 remapad_disconnect 断开。"
        "单键/组合键用 remapad_tap，不限时长按住用 remapad_hold，摇杆用 remapad_stick，"
        "重叠时值编排用 remapad_script；键名即固件内部值（PS 位置语义）。"
        "让 NS2 主机连上来用 remapad_pair（连接键动作）；remapad_status 可看链路与引擎状态。"
    ),
)

BRIDGE: PadBridge | None = None


def _bridge() -> PadBridge:
    if BRIDGE is None:
        raise McpError("服务未初始化")
    return BRIDGE


@SERVER.tool(description="连接设备：port=串口号（如 COM12）或 net=设备IP[:端口]（WiFi netlog 通道）二选一；"
                         "无参时用服务启动参数的默认值。连接成功后其余工具才可用")
def remapad_connect(port: str | None = None, net: str | None = None) -> dict:
    try:
        return _bridge().connect(port, net)
    except McpError as exc:
        return {"ok": False, "error": str(exc)}


@SERVER.tool(description="断开设备链路：key release 全松、摇杆回中并关闭串口/网络会话")
def remapad_disconnect() -> dict:
    try:
        return _bridge().disconnect()
    except McpError as exc:
        return {"ok": False, "error": str(exc)}


@SERVER.tool(description="读取链路状态与按键引擎状态：未连接时只报链路与引擎；"
                         "已连接时附带设备回执（status/pad/link 原始行）")
def remapad_status() -> dict:
    try:
        bridge = _bridge()
        engine = bridge.engine.state()
        try:
            bridge._session()
        except McpError:
            return {"ok": True, "link": "未连接", "engine": engine}
        device = bridge.query("status") + bridge.query("pad") + bridge.query("link")
        return {"ok": True, "link": "已连接", "device": device, "engine": engine}
    except McpError as exc:
        return {"ok": False, "error": str(exc)}


@SERVER.tool(description="按下设备上的连接键（配对键）：打开连接窗口等 NS2 主机连上来（未配对身份进配对流程）")
def remapad_pair() -> dict:
    try:
        return {"ok": True, "device": _bridge().query("connect")}
    except McpError as exc:
        return {"ok": False, "error": str(exc)}


@SERVER.tool(description="断开设备与当前 NS2 主机的 BLE 连接（不动 PC 与设备之间的链路）")
def remapad_drop() -> dict:
    try:
        return {"ok": True, "device": _bridge().query("drop")}
    except McpError as exc:
        return {"ok": False, "error": str(exc)}


@SERVER.tool(
    description="按下并在 hold_ms 毫秒后松开一组按键（同起同落的组合键），阻塞到松开才返回。"
                f"键名：{' '.join(KEY_NAMES)}；hold_ms 取 1-{MAX_HOLD_MS}")
def remapad_tap(keys: list[str], hold_ms: int = DEFAULT_TAP_MS) -> dict:
    try:
        names = validate_keys(keys)
        if isinstance(hold_ms, bool) or not isinstance(hold_ms, (int, float)) or hold_ms < 1 or hold_ms > MAX_HOLD_MS:
            raise McpError(f"hold_ms 必须是 1-{MAX_HOLD_MS} 的毫秒数")
        bridge = _bridge()
        bridge._session()
        bridge.engine.tap(names, int(hold_ms))
        time.sleep(hold_ms / 1000.0)
        return {"ok": True, "keys": names, "hold_ms": hold_ms}
    except McpError as exc:
        return {"ok": False, "error": str(exc)}


@SERVER.tool(
    description="按住（pressed=true）或松开（pressed=false）一组按键，不限时长；"
                "配合其它调用可实现长按期间做别的动作。键名同 remapad_tap")
def remapad_hold(keys: list[str], pressed: bool = True) -> dict:
    try:
        names = validate_keys(keys)
        bridge = _bridge()
        bridge._session()
        changed = bridge.engine.press(names) if pressed else bridge.engine.release(names)
        return {"ok": True, "keys": names, "pressed": pressed, "changed": changed, "engine": bridge.engine.state()}
    except McpError as exc:
        return {"ok": False, "error": str(exc)}


@SERVER.tool(
    description="设定摇杆电平并持续保持：side 为 l 或 r；x/y 取 0-4095"
                f"（{STICK_CENTER} 中位，x 向右为正、y 向上为正）")
def remapad_stick(side: str, x: int, y: int) -> dict:
    try:
        if side not in ("l", "r"):
            raise McpError("side 必须是 l 或 r")
        sx, sy = validate_stick_xy(x, y)
        bridge = _bridge()
        bridge._session()
        bridge.engine.set_stick(side, sx, sy)
        return {"ok": True, "side": side, "x": sx, "y": sy}
    except McpError as exc:
        return {"ok": False, "error": str(exc)}


@SERVER.tool(description="两侧摇杆回中")
def remapad_stick_reset() -> dict:
    try:
        bridge = _bridge()
        bridge._session()
        bridge.engine.reset_stick()
        return {"ok": True}
    except McpError as exc:
        return {"ok": False, "error": str(exc)}


@SERVER.tool(
    description="可编程脚本模式：时间线编排任意多组按键与摇杆，各组按下/松开时刻独立，"
                "阻塞执行到结束，结束时松开全部按键并回中摇杆。actions 每项字段可任选："
                '{"t": 起始毫秒, "down": [键名], "up": [键名], "tap": [键名], '
                '"hold_ms": tap 保持毫秒（默认 200）, "stick": {"side": "l|r", "x": 0-4095, "y": 0-4095}, '
                '"stick_reset": true}；loop 为整条时间线的循环次数')
def remapad_script(actions: list[dict], loop: int = 1) -> dict:
    try:
        bridge = _bridge()
        bridge._session()
        events, _span_ms = compile_script(actions, loop, bridge.script_max_ms)
        duration_ms = run_script(bridge.engine, events, loop)
        return {"ok": True, "events": len(events), "loop": loop, "duration_ms": round(duration_ms)}
    except McpError as exc:
        return {"ok": False, "error": str(exc)}


@SERVER.tool(description="松开全部按键并把摇杆回中（随时可调用的逃生口）")
def remapad_release_all() -> dict:
    try:
        bridge = _bridge()
        bridge._session()
        released = bridge.engine.clear()
        return {"ok": True, "released": released}
    except McpError as exc:
        return {"ok": False, "error": str(exc)}


@SERVER.tool(description="截取设备屏幕当前画面存为 PNG（仅串口会话可用；不传 path 落默认截图目录）")
def remapad_screenshot(path: str | None = None) -> dict:
    try:
        result = _bridge().screenshot(path)
        return {"ok": True, **result}
    except McpError as exc:
        return {"ok": False, "error": str(exc)}


def parse_mcp_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Remapad 按键注入 MCP 服务（stdio）")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("-p", "--port", default=None,
                       help="默认串口名（remapad_connect 无参时使用；连接动作本身由工具完成）")
    group.add_argument("-n", "--net", metavar="HOST[:PORT]", default=None,
                       help="默认走设备 netlog 的 UDP 通道（WiFi）；设备侧 netlog 会话需开启")
    parser.add_argument("--baud", type=int, default=115200, help="波特率（USJ 忽略）")
    parser.add_argument("--reply-wait", type=float, default=1.2, help="CLI 回复窗秒数（默认 1.2）")
    parser.add_argument("--ping-ms", type=int, default=1000,
                        help="WiFi 通道的 PING 保活间隔毫秒（维持设备侧 2 秒桥接窗口，默认 1000）")
    parser.add_argument("--script-max-ms", type=int, default=DEFAULT_SCRIPT_MAX_MS,
                        help=f"脚本总时长上限毫秒（默认 {DEFAULT_SCRIPT_MAX_MS}）")
    return parser.parse_args(argv)


def main(argv=None) -> int:
    global BRIDGE
    args = parse_mcp_args(argv)
    ctrl_args = ctrl.parse_args(["--no-pad"])
    ctrl_args.port = args.port
    ctrl_args.net = args.net
    ctrl_args.baud = args.baud
    ctrl_args.reply_wait = args.reply_wait
    BRIDGE = PadBridge(ctrl_args, script_max_ms=args.script_max_ms, ping_period_s=args.ping_ms / 1000.0)
    atexit.register(BRIDGE.stop)
    SERVER.run("stdio")
    return 0


if __name__ == "__main__":
    sys.exit(main())
