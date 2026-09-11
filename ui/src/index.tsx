import { mount } from '@pocketjs/framework/vue-vapor';
import Hero from './Hero';
import App from './App';

// quickjs-ng 的 js_std_add_helpers 只给全局 console 装了 log，框架 prelude 的
// 守卫用 `typeof console !== 'object'` 判断，看到这个半成品对象就跳过了 warn
// 与 error 的补齐。渲染器把所有捕获到的异常都交给 console.error 上报，方法缺失
// 时真正的失败会被 "TypeError: not a function" 顶替，原始错误无法定位。
// 这里把缺失的方法补到 native console.log 上，输出经由 QuickJS js_print 进串口。
type LogFn = (...args: unknown[]) => void;

interface PocketConsole {
  log?: LogFn;
  warn?: LogFn;
  error?: LogFn;
}

const pocketConsole = (globalThis as { console?: PocketConsole }).console;
const nativeLog = pocketConsole?.log;

function describe(value: unknown): string {
  if (value instanceof Error) {
    const stack = (value as { stack?: string }).stack;
    const header = `${value.name}: ${value.message}`;
    // QuickJS 的 stack 只包含栈帧，错误名与消息需要自己拼回去。
    return stack === undefined || stack.length === 0
      ? header
      : `${header}\n${stack}`;
  }
  return String(value);
}

if (pocketConsole !== undefined && typeof nativeLog === 'function') {
  const write = nativeLog;
  const forward = (level: string): LogFn =>
    (...args) => {
      write(`[${level}]`, ...args.map(describe));
    };
  if (typeof pocketConsole.error !== 'function') {
    pocketConsole.error = forward('error');
  }
  if (typeof pocketConsole.warn !== 'function') {
    pocketConsole.warn = forward('warn');
  }
}

mount(() => <App />);
