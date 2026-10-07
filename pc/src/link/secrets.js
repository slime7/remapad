// 数据与核对状态见 docs/ABSTRACTIONS.md。

export const SECRET_MASK = '***';

// 凭据回读行（ok netlog cred ssid=x pass=y）里的密码段；pass=- 表示未配置，保持原样。
const CRED_PASS_RE = /(netlog cred ssid=\S+ pass=)(\S+)/g;
// 直连命令的子命令白名单：命中这些词的不是 SSID，不脱敏。
const NETLOG_SUBS = 'save\\b|cred\\b|scan\\b|scanlist\\b|phyreset\\b|power\\b|reconnect\\b|off\\b|state=';
// 发送回显（GUI 给发出的命令统一加「> 」前缀，设备行不会有）：保存与直连两种
// 命令形态都在前缀之后匹配，避免把 netlog 开头的固件日志词当 SSID 误伤。
const SAVE_PASS_RE = /(^\s*>\s*netlog save \S+ )(\S+)/gm;
const CONNECT_PASS_RE = new RegExp(`(^\\s*>\\s*netlog (?!${NETLOG_SUBS})(?![[<])\\S+ )(\\S+)`, 'gm');

/** 一行日志展示前的脱敏：netlog 命令与凭据回读里的 WiFi 密码换成 ***，其余原样。 */
export function maskSecrets(text) {
  const cred = text.replace(CRED_PASS_RE, (_match, head, pass) => head + (pass === '-' ? pass : SECRET_MASK));
  const saved = cred.replace(SAVE_PASS_RE, `$1${SECRET_MASK}`);
  return saved.replace(CONNECT_PASS_RE, `$1${SECRET_MASK}`);
}
