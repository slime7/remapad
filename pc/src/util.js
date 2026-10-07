// 小工具：单调时钟与可等待事件。全仓库的时值都按「秒」走。
export function now() {
  return performance.now() / 1000;
}

export function sleep(seconds) {
  return new Promise((resolve) => setTimeout(resolve, Math.max(0, seconds * 1000)));
}

/** 可等待的布尔事件：set/clear/isSet 与带超时的等待（等待期被 set 则为 true）。 */
export class SyncEvent {
  constructor() {
    this._flag = false;
    this._waiters = [];
  }

  set() {
    this._flag = true;
    for (const waiter of this._waiters.splice(0)) {
      waiter();
    }
  }

  clear() {
    this._flag = false;
  }

  isSet() {
    return this._flag;
  }

  wait(timeoutS) {
    if (this._flag) {
      return Promise.resolve(true);
    }
    if (timeoutS <= 0) {
      return Promise.resolve(false);
    }
    return new Promise((resolve) => {
      const waiter = () => {
        clearTimeout(timer);
        resolve(true);
      };
      const timer = setTimeout(() => {
        const index = this._waiters.indexOf(waiter);
        if (index >= 0) {
          this._waiters.splice(index, 1);
        }
        resolve(false);
      }, timeoutS * 1000);
      this._waiters.push(waiter);
    });
  }
}
