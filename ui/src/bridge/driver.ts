import type { DeviceCmd, DeviceMsg } from './protocol';
import { mockHandleCmd } from './mock';

export type MessageCallback = (msg: DeviceMsg) => void;
export type Unsubscribe = () => void;

/**
 * 产品控制面桥接预留。
 *
 * 该类型描述未来 USB 输入、NS2 状态和 BLE 配对控制的 UI 管理面；
 * 它不是 PocketJS 的 ESP-IDF UI binding，当前应用也没有调用它。
 */
export class HardwareDriver {
  private seqId = 1;
  private pending = new Map<number, MessageCallback>();
  private eventListeners = new Set<MessageCallback>();
  private initialized = false;

  constructor() {
    this.initNativeListener();
  }

  /** 检查当前是否运行在未来的产品原生控制面环境中。 */
  public isNative(): boolean {
    return typeof (globalThis as any).__nativeBridge !== 'undefined';
  }

  /** 挂载未来原生控制面的主动回调。 */
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
   * 发送未来产品控制面命令；不参与当前 PocketJS UI turn。
   */
  public send(cmdWithoutId: Omit<DeviceCmd, 'id'>, onReply?: MessageCallback): number {
    const id = this.seqId++;
    const cmd = { ...cmdWithoutId, id } as DeviceCmd;

    if (onReply) {
      this.pending.set(id, onReply);
    }

    if (this.isNative()) {
      (globalThis as any).__nativeBridge.postMessage(JSON.stringify(cmd));
    } else {
      mockHandleCmd(cmd, (msg) => {
        this.routeMessage(msg);
      });
    }

    return id;
  }

  /** 监听未来产品控制面主动事件。 */
  public onEvent(listener: MessageCallback): Unsubscribe {
    this.eventListeners.add(listener);
    return () => {
      this.eventListeners.delete(listener);
    };
  }

  /** 路由未来产品控制面应答和广播事件。 */
  public routeMessage(msg: DeviceMsg): void {
    if ('id' in msg && typeof msg.id === 'number' && this.pending.has(msg.id)) {
      const callback = this.pending.get(msg.id);
      this.pending.delete(msg.id);
      callback?.(msg);
    } else {
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

/** 产品控制面预留单例。 */
export const hardware = new HardwareDriver();
