// 会话输出接收器：命令行写标准流，图形界面把同一批记录送进队列。
// 数据与核对状态见 docs/ABSTRACTIONS.md。

import { maskSecrets } from '../link/secrets.js';

/** 默认实现：行与错误分别写标准输出与标准错误，事件丢弃。 */
export class ConsoleReporter {
  line(text) {
    console.log(maskSecrets(text));
  }

  error(text) {
    console.error(text);
  }

  event(_name, _fields) {}
}

/** 会话输出 → 队列：图形界面后端把记录转给界面线程，队列只放记录。 */
export class QueueReporter {
  constructor(sink) {
    this.sink = sink;
  }

  line(text) {
    this.sink.push({ kind: 'line', text });
  }

  error(text) {
    this.sink.push({ kind: 'error', text });
  }

  event(name, fields = {}) {
    this.sink.push({ kind: 'event', name, ...fields });
  }
}
