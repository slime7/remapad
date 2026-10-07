// Remapad 按键注入 MCP 服务：工具注册面与 stdio 传输。
// 键名即固件内部值（PS 位置语义）；注入走固件调试 CLI，语义见 pc/README.md。
// 数据与核对状态见 docs/ABSTRACTIONS.md。

import { Server } from '@modelcontextprotocol/sdk/server/index.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import { CallToolRequestSchema, ListToolsRequestSchema } from '@modelcontextprotocol/sdk/types.js';

import { sleep } from '../util.js';
import {
  KEY_NAMES, MAX_HOLD_MS, STICK_CENTER, STICK_MAX, DEFAULT_TAP_MS, McpError, validateKeys, validateStickXy,
} from './engine.js';
import { compileScript, runScript } from './timeline.js';

/** 工具注册表：声明面（名字/描述/JSON Schema）与执行器同一处维护。 */
export const TOOLS = [];

function defineTool(name, description, inputSchema, handler) {
  TOOLS.push({ name, description, inputSchema, handler });
  void [name];
}

const ARRAY_OF_STRINGS = { type: 'array', items: { type: 'string' } };

defineTool(
  'remapad_connect',
  '连接设备：port=串口号（如 COM12）或 net=设备IP[:端口]（WiFi netlog 通道）二选一；'
    + '无参时用服务启动参数的默认值。连接成功后其余工具才可用',
  { type: 'object', properties: { port: { type: 'string' }, net: { type: 'string' } } },
  async ({ port, net }) => wrap(() => BRIDGE().connect(port ?? null, net ?? null)),
);

defineTool(
  'remapad_disconnect',
  '断开设备链路：打断进行中的回放，key release 全松、摇杆回中并关闭串口/网络会话',
  { type: 'object', properties: {} },
  async () => wrap(() => BRIDGE().disconnect()),
);

defineTool(
  'remapad_status',
  '读取链路状态、按键引擎状态与回放进度：未连接时只报链路、引擎与回放；'
    + '已连接时附带设备回执（status/pad/link 原始行）',
  { type: 'object', properties: {} },
  async () => wrap(async () => {
    const bridge = BRIDGE();
    const engine = bridge.engine.state();
    const replay = bridge.replaySnapshot();
    try {
      bridge._session();
    } catch (exc) {
      if (exc instanceof McpError) {
        return { ok: true, link: '未连接', engine, replay };
      }
      throw exc;
    }
    const device = [
      ...(await bridge.query('status')),
      ...(await bridge.query('pad')),
      ...(await bridge.query('link')),
    ];
    return { ok: true, link: '已连接', device, engine, replay };
  }),
);

defineTool(
  'remapad_pair',
  '按下设备上的连接键（配对键）：打开连接窗口等 NS2 主机连上来（未配对身份进配对流程）',
  { type: 'object', properties: {} },
  async () => wrap(async () => ({ ok: true, device: await BRIDGE().query('connect') })),
);

defineTool(
  'remapad_drop',
  '断开设备与当前 NS2 主机的 BLE 连接（不动 PC 与设备之间的链路）',
  { type: 'object', properties: {} },
  async () => wrap(async () => ({ ok: true, device: await BRIDGE().query('drop') })),
);

defineTool(
  'remapad_tap',
  '按下并在 hold_ms 毫秒后松开一组按键（同起同落的组合键），阻塞到松开才返回。'
        + `键名：${KEY_NAMES.join(' ')}；hold_ms 取 1-${MAX_HOLD_MS}`,
  { type: 'object', properties: { keys: ARRAY_OF_STRINGS, hold_ms: { type: 'number' } }, required: ['keys'] },
  async ({ keys, hold_ms = DEFAULT_TAP_MS }) => wrap(async () => {
    const names = validateKeys(keys);
    if (typeof hold_ms !== 'number' || !Number.isFinite(hold_ms) || hold_ms < 1 || hold_ms > MAX_HOLD_MS) {
      throw new McpError(`hold_ms 必须是 1-${MAX_HOLD_MS} 的毫秒数`);
    }
    const bridge = BRIDGE();
    bridge._session();
    bridge.checkNoReplay();
    bridge.engine.tap(names, Math.trunc(hold_ms));
    await sleep(hold_ms / 1000.0);
    return { ok: true, keys: names, hold_ms };
  }),
);

defineTool(
  'remapad_hold',
  '按住（pressed=true）或松开（pressed=false）一组按键，不限时长；'
    + '配合其它调用可实现长按期间做别的动作。键名同 remapad_tap',
  { type: 'object', properties: { keys: ARRAY_OF_STRINGS, pressed: { type: 'boolean' } }, required: ['keys'] },
  async ({ keys, pressed = true }) => wrap(async () => {
    const names = validateKeys(keys);
    const bridge = BRIDGE();
    bridge._session();
    bridge.checkNoReplay();
    const changed = pressed ? bridge.engine.press(names) : bridge.engine.release(names);
    return { ok: true, keys: names, pressed, changed, engine: bridge.engine.state() };
  }),
);

defineTool(
  'remapad_stick',
  `设定摇杆电平并持续保持：side 为 l 或 r；x/y 取 0-${STICK_MAX}`
        + `（${STICK_CENTER} 中位，x 向右为正、y 向上为正）`,
  {
    type: 'object',
    properties: { side: { type: 'string' }, x: { type: 'number' }, y: { type: 'number' } },
    required: ['side', 'x', 'y'],
  },
  async ({ side, x, y }) => wrap(async () => {
    if (side !== 'l' && side !== 'r') {
      throw new McpError('side 必须是 l 或 r');
    }
    const [sx, sy] = validateStickXy(x, y);
    const bridge = BRIDGE();
    bridge._session();
    bridge.checkNoReplay();
    bridge.engine.setStick(side, sx, sy);
    return { ok: true, side, x: sx, y: sy };
  }),
);

defineTool(
  'remapad_stick_reset',
  '两侧摇杆回中；回放进行中会被拒绝',
  { type: 'object', properties: {} },
  async () => wrap(async () => {
    const bridge = BRIDGE();
    bridge._session();
    bridge.checkNoReplay();
    bridge.engine.resetStick();
    return { ok: true };
  }),
);

defineTool(
  'remapad_script',
  '可编程脚本模式：时间线编排任意多组按键与摇杆，各组按下/松开时刻独立，'
    + '阻塞执行到结束，结束时松开全部按键并回中摇杆；回放进行中会被拒绝。'
    + 'actions 每项字段可任选：'
    + '{"t": 起始毫秒, "down": [键名], "up": [键名], "tap": [键名], '
    + '"hold_ms": tap 保持毫秒（默认 200）, "stick": {"side": "l|r", "x": 0-4095, "y": 0-4095}, '
    + '"stick_reset": true}；loop 为整条时间线的循环次数',
  {
    type: 'object',
    properties: { actions: { type: 'array', items: { type: 'object' } }, loop: { type: 'number' } },
    required: ['actions'],
  },
  async ({ actions, loop = 1 }) => wrap(async () => {
    const bridge = BRIDGE();
    bridge._session();
    bridge.claimScript();
    try {
      const [events] = compileScript(actions, loop, bridge.scriptMaxMs);
      const durationMs = await runScript(bridge.engine, events, loop);
      return { ok: true, events: events.length, loop, duration_ms: Math.round(durationMs) };
    } finally {
      bridge.releaseScript();
    }
  }),
);

defineTool(
  'remapad_replay',
  '回放按键记录文件（TAS 式逐帧输入表）：后台执行、立即返回，期间其它按键工具会被拒绝；'
    + 'loop 为整条记录的循环次数（1-1000），进度看 remapad_status，打断用 remapad_replay_stop。'
    + '格式：# 开头是注释；头部 key = value（frame_ms=每帧毫秒 或 fps=帧率，缺省 15ms）；'
    + '帧行 |帧号|按键+按键|左摇杆x,y|右摇杆x,y|（. 或空 = 保持不变），'
    + '帧行表示从该帧起保持到下一帧行，循环边界自动松键回中',
  { type: 'object', properties: { path: { type: 'string' }, loop: { type: 'number' } }, required: ['path'] },
  async ({ path, loop = 1 }) => wrap(async () => {
    if (typeof path !== 'string' || !path.trim()) {
      throw new McpError('path 必须是回放记录文件的路径');
    }
    return BRIDGE().startReplay(path.trim(), loop);
  }),
);

defineTool(
  'remapad_replay_stop',
  '打断进行中的回放：停掉时间线、松开全部按键并回中摇杆；没有回放时幂等成功',
  { type: 'object', properties: {} },
  async () => wrap(() => BRIDGE().stopReplay()),
);

defineTool(
  'remapad_release_all',
  '松开全部按键并把摇杆回中（随时可调用的逃生口）；回放进行中会被拒绝，'
    + '先 remapad_replay_stop 打断',
  { type: 'object', properties: {} },
  async () => wrap(async () => {
    const bridge = BRIDGE();
    bridge._session();
    bridge.checkNoReplay();
    const released = bridge.engine.clear();
    return { ok: true, released };
  }),
);

defineTool(
  'remapad_screenshot',
  '截取设备屏幕当前画面存为 PNG（仅串口会话可用；不传 path 落默认截图目录）',
  { type: 'object', properties: { path: { type: 'string' } } },
  async ({ path }) => wrap(async () => ({ ok: true, ...(await BRIDGE().screenshot(path ?? null)) })),
);

/** 工具执行器的统一出口：可预期失败转 {ok:false, error}，其余原样抛给 SDK。 */
async function wrap(task) {
  try {
    return await task();
  } catch (exc) {
    if (exc instanceof McpError) {
      return { ok: false, error: exc.message };
    }
    throw exc;
  }
}

/** 工具面清单（测试与调试用）：与 ListTools 应答同形。 */
export function listTools() {
  return TOOLS.map(({ name, description, inputSchema }) => ({ name, description, inputSchema }));
}

let BRIDGE_INSTANCE = null;

function BRIDGE() {
  if (BRIDGE_INSTANCE === null) {
    throw new McpError('服务未初始化');
  }
  return BRIDGE_INSTANCE;
}

/** 供测试替换的全局桥。 */
export function setBridge(bridge) {
  BRIDGE_INSTANCE = bridge;
}

export function createServer() {
  const server = new Server(
    { name: 'remapad-pad', version: '0.1.0' },
    {
      capabilities: { tools: {} },
      instructions: '向 NS2 主机注入按键供自动化与 agent 编排，注入走固件调试 CLI。'
                + '先 remapad_connect 连接设备（port=串口号 或 net=IP:端口 二选一），'
                + '之后其余工具才可用；结束用 remapad_disconnect 断开。'
                + '单键/组合键用 remapad_tap，不限时长按住用 remapad_hold，摇杆用 remapad_stick，'
                + '重叠时值编排用 remapad_script；录好的按键记录文件用 remapad_replay 后台回放，'
                + '回放期间按键工具会被拒绝，打断用 remapad_replay_stop，进度看 remapad_status。'
                + '键名即固件内部值（PS 位置语义）；让 NS2 主机连上来用 remapad_pair（连接键动作）。',
    },
  );
  server.setRequestHandler(ListToolsRequestSchema, async () => ({ tools: listTools() }));
  server.setRequestHandler(CallToolRequestSchema, async (request) => {
    const { name, arguments: args } = request.params;
    const tool = TOOLS.find((item) => item.name === name);
    if (!tool) {
      return { content: [{ type: 'text', text: `未知工具：${name}` }], isError: true };
    }
    try {
      const result = await tool.handler(args ?? {});
      return { content: [{ type: 'text', text: JSON.stringify(result) }], structuredContent: result };
    } catch (exc) {
      return { content: [{ type: 'text', text: String(exc.message ?? exc) }], isError: true };
    }
  });
  return server;
}

export async function runStdio(bridge) {
  BRIDGE_INSTANCE = bridge;
  const server = createServer();
  const transport = new StdioServerTransport();
  await server.connect(transport);
  // 挂住进程直到 stdin 关闭（客户端断开）。
  await new Promise((resolve) => {
    const previous = transport.onclose;
    transport.onclose = () => {
      if (previous) previous();
      resolve();
    };
  });
}
