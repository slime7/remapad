#include "netlog.h"

/* 网络调试会话裁掉（REMAPAD_NETLOG=OFF）时的空实现：全部入口只在 cli.c 的
 * netlog 命令里被引用，而那条命令由同名编译定义裁剪。 */
