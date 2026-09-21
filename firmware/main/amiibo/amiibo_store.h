#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "amiibo_proto.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * amiibo 镜像存储：storage 分区（SPIFFS，挂载 /amiibo）里的固定槽位文件表——槽位镜像 "a<i>"、
 * 名称 "n<i>"、选中号 "sel"，容量按 AMIIBO_SLOTS_MAX 封顶。选中即预置进 NFC 模拟层并持久化，
 * 重启自动恢复；主机写卡的存档经 ns2_nfc 的写回回调写回选中槽位。
 */

/** storage 分区里最多保留的 amiibo 数量。 */
#define AMIIBO_SLOTS_MAX 200u

/** 槽位记录长度（= NS2_NFC_IMAGE_MAX：镜像 + 厂商签名）。 */
#define AMIIBO_RECORD_SIZE NS2_NFC_IMAGE_MAX

/** 挂载 storage 分区、扫描槽位并恢复选中（选中槽位的记录预置进 NFC 模拟
 *  层）。挂载/扫描耗秒级，由 amiibo_store_init_task 在低优先级任务里执行，
 *  不挡开机画面；初始化完成前对槽位的操作按「未挂载」拒绝。 */
esp_err_t amiibo_store_init(void);

/** 后台初始化任务体（main 直接 xTaskCreate 用）：跑完 amiibo_store_init 后
 *  自删。 */
void amiibo_store_init_task(void *unused);

/** 已占用的槽位数。 */
size_t amiibo_store_count(void);

/** 槽位容量上限（AMIIBO_SLOTS_MAX）。 */
size_t amiibo_store_capacity(void);

/** 第 index 个槽位的名称（不存在返回 false）。 */
bool amiibo_store_name(size_t index, char *out, size_t cap);

/** 读第 index 个槽位的标签镜像段（out 容量至少 NS2_NFC_TAG_SIZE）。 */
bool amiibo_store_read(size_t index, uint8_t *out, size_t cap);

/** 读第 index 个槽位的整份记录（镜像 + 签名，out 容量至少
 *  AMIIBO_RECORD_SIZE；返回实际读到的长度，0 表示槽位不存在）。 */
size_t amiibo_store_read_record(size_t index, uint8_t *out, size_t cap);

/** 落一份新镜像（amiibo_session 的 END 回调）：长度 540（签名补零）或 572，
 *  占用第一个空槽位，成功返回槽位号，满 / 写文件失败返回负值。 */
int amiibo_store_add(const char *name, const uint8_t *data, size_t len, void *user);

/** 删除槽位；删的是当前选中时同时取消选中。 */
esp_err_t amiibo_store_remove(size_t index);

/** 选中槽位并预置进 NFC 模拟层（持久化，重启恢复）。 */
esp_err_t amiibo_store_select(size_t index);

/** 取消选中（NFC 模拟层回到无卡）。 */
void amiibo_store_deselect(void);

/** 当前选中的槽位号；无选中返回 -1。 */
int amiibo_store_selected(void);

/** 主机写卡提交（ns2_nfc 写回回调）：把标签镜像写回当前选中槽位记录的
 *  镜像段（签名段不动）。没有选中槽位返回 false，镜像只活在内存。 */
bool amiibo_store_write_selected(const uint8_t *data, size_t len, void *user);

#ifdef __cplusplus
}
#endif
