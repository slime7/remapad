import type { DeviceCmd, DeviceMsg } from './protocol';
import { mockHandleCmd } from './mock';

export type MessageCallback = (msg: DeviceMsg) => void;
export type Unsubscribe = () => void;

/**
 * 统一硬件调用门面 (HardwareDriver)
 * 借鉴 pocket-youtube 的 driver.ts 架构：
 * 1. 自动嗅探运行宿主环境 (ESP32-S3 原生 C 固件 vs PC 浏览器 WebAssembly 仿真)
 * 2. 对外暴露统一的强类型硬件指令发送与事件订阅接口
 */
export class HardwareDriver {
  private seqId = 1;
  private pending = new Map<number, MessageCallback>();
  private eventListeners = new Set<MessageCallback>();
  private initialized = false;

  constructor() {
    this.initNativeListener();
  }

  /** 检查当前是否运行在具有原生 C 绑定的硬件环境中 */
  public isNative(): boolean {
    return typeof (globalThis as any).__nativeBridge !== 'undefined';
  }

  /** 挂载原生环境的主动回调 */
  private initNativeListener(): void {
    if (this.initialized) {
      return;
    }
    this.initialized = true;

    (globalThis as any).__onNativeBridgeMessage = (rawJson: string) => {
      try {
        const msg = JSON.parse(rawJson) as DeviceMsg;
        this.routeMessage(msg);
      } catch (e) {
        console.error('[HardwareDriver] Failed to parse native message:', e);
      }
    };
  }

  /**
   * 发送硬件控制命令 (请求-响应模型)
   * @param cmdWithoutId 命令负载 (无需手动指定 id)
   * @param onReply 可选的应答回调函数
   * @returns 分配的命令序号 ID
   */
  public send(cmdWithoutId: Omit<DeviceCmd, 'id'>, onReply?: MessageCallback): number {
    const id = this.seqId++;
    const cmd = { ...cmdWithoutId, id } as DeviceCmd;

    if (onReply) {
      this.pending.set(id, onReply);
    }

    if (this.isNative()) {
      // 真机环境：调用 C 注入的原生门面
      (globalThis as any).__nativeBridge.postMessage(JSON.stringify(cmd));
    } else {
      // 浏览器 / PC 仿真环境：走 Mock 虚拟外设
      mockHandleCmd(cmd, (msg) => {
        this.routeMessage(msg);
      });
    }

    return id;
  }

  /**
   * 监听硬件主动上报的异步事件 (发布-订阅模型)
   * @param listener 事件监听函数
   * @returns 取消订阅的清理函数
   */
  public onEvent(listener: MessageCallback): Unsubscribe {
    this.eventListeners.add(listener);
    return () => {
      this.eventListeners.delete(listener);
    };
  }

  /**
   * 统一消息路由分发
   * @param msg 硬件返回或主动推送的消息
   */
  public routeMessage(msg: DeviceMsg): void {
    if ('id' in msg && typeof msg.id === 'number' && this.pending.has(msg.id)) {
      const callback = this.pending.get(msg.id);
      this.pending.delete(msg.id);
      callback?.(msg);
    } else {
      // 无 id 的硬件主动推送事件或未匹配的广播
      this.eventListeners.forEach((listener) => {
        try {
          listener(msg);
        } catch (e) {
          console.error('[HardwareDriver] Error in event listener:', e);
        }
      });
    }
  }
}

/** 全局硬件驱动单例 */
export const hardware = new HardwareDriver();
