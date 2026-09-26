#include "usb_audio_parse.h"

#include <string.h>

/* 标准描述符类型与音频类常量（USB 1.1 / UAC 1.0）。 */
#define USB_DESC_INTERFACE 0x04u
#define USB_DESC_ENDPOINT 0x05u
#define USB_DESC_CS_INTERFACE 0x24u
#define USB_AUDIO_CLASS 0x01u
#define USB_AUDIO_SUBCLASS_STREAMING 0x02u
#define USB_AUDIO_AS_GENERAL 0x01u
#define USB_AUDIO_FORMAT_TYPE 0x02u
#define USB_AUDIO_FORMAT_TYPE_I 0x01u
#define USB_AUDIO_FORMAT_PCM 0x0001u
#define USB_EP_ATTR_XFER_MASK 0x03u
#define USB_EP_ATTR_XFER_ISOC 0x01u
#define USB_EP_DIR_IN 0x80u

/** 一个备用设置组在扫描过程中攒下的信息：INTERFACE 开组，类特定描述符与
 *  端点描述符随到随记，下一个 INTERFACE（或描述符走完）时验收。 */
typedef struct {
  uint8_t iface;
  uint8_t alt;
  bool is_as;
  bool pcm;
  bool format_ok;
  uint8_t channels;
  uint8_t subframe;
  uint8_t bits;
  uint32_t rate;
  uint8_t ep_addr;
  uint16_t ep_mps;
} as_group_t;

static bool group_complete(const as_group_t *g)
{
  return g->is_as && g->alt != 0 && g->pcm && g->format_ok && g->channels > 0 && g->subframe == 2 && g->bits == 16 &&
         g->rate != 0 && g->ep_addr != 0;
}

static void parse_cs_interface(as_group_t *g, const uint8_t *p, uint8_t len)
{
  const uint8_t subtype = p[2];
  if (subtype == USB_AUDIO_AS_GENERAL && len >= 7) {
    const uint16_t format_tag = (uint16_t)(p[5] | (p[6] << 8));
    g->pcm = format_tag == USB_AUDIO_FORMAT_PCM;
  } else if (subtype == USB_AUDIO_FORMAT_TYPE && len >= 11) {
    /* bFormatType 之后是 bNrChannels/bSubframeSize/bBitResolution 与采样率
         * 表（bSamFreqType 为 0 表示连续频率，没有可对账的定值，按不识别跳过）。 */
    g->format_ok = p[3] == USB_AUDIO_FORMAT_TYPE_I && p[7] >= 1;
    g->channels = p[4];
    g->subframe = p[5];
    g->bits = p[6];
    g->rate = (uint32_t)p[8] | ((uint32_t)p[9] << 8) | ((uint32_t)p[10] << 16);
  }
}

bool usb_audio_find_as_out(const uint8_t *config_desc, size_t len, usb_audio_as_out_t *out)
{
  if (config_desc == NULL || len < 2 || out == NULL) {
    return false;
  }
  as_group_t g;
  memset(&g, 0, sizeof(g));
  const uint8_t *p = config_desc;
  const uint8_t *end = config_desc + len;
  while (p + 2 <= end) {
    const uint8_t desc_len = p[0];
    const uint8_t desc_type = p[1];
    if (desc_len < 2 || p + desc_len > end) {
      break;
    }
    if (desc_type == USB_DESC_INTERFACE && desc_len >= 9) {
      if (group_complete(&g)) {
        break;
      }
      g.iface = p[2];
      g.alt = p[3];
      g.is_as = p[5] == USB_AUDIO_CLASS && p[6] == USB_AUDIO_SUBCLASS_STREAMING;
      g.pcm = false;
      g.format_ok = false;
      g.channels = 0;
      g.subframe = 0;
      g.bits = 0;
      g.rate = 0;
      g.ep_addr = 0;
      g.ep_mps = 0;
    } else if (desc_type == USB_DESC_CS_INTERFACE && g.is_as && g.alt != 0 && desc_len >= 3) {
      parse_cs_interface(&g, p, desc_len);
    } else if (desc_type == USB_DESC_ENDPOINT && g.is_as && g.alt != 0 && desc_len >= 7) {
      const uint8_t attrs = p[3] & USB_EP_ATTR_XFER_MASK;
      if (attrs == USB_EP_ATTR_XFER_ISOC && (p[2] & USB_EP_DIR_IN) == 0) {
        g.ep_addr = p[2];
        g.ep_mps = (uint16_t)((p[4] | (p[5] << 8)) & 0x07FFu);
      }
    }
    p += desc_len;
  }
  if (!group_complete(&g)) {
    return false;
  }
  out->iface = g.iface;
  out->alt = g.alt;
  out->ep_addr = g.ep_addr;
  out->ep_mps = g.ep_mps;
  out->channels = g.channels;
  out->subframe_size = g.subframe;
  out->bit_resolution = g.bits;
  out->sample_rate_hz = g.rate;
  return true;
}
