/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Host test of app/orkest_ui.c: the splash and the main screen drawn into a framebuffer (build/host/screen-*.rgb565,
 * tests/ui_png.py makes PNGs), under ASan: every strip inside its buffer, the bank intact afterwards. */
#include <stdio.h>
#include <string.h>
#include "orkest.h"
#include "fm1_device.h"
#include "orkest_bank_file.h"
#include "orkest_lfo.inc"
static uint16_t fb[240*240];
static uint8_t flash[536576];
uint32_t fm1_millis(void){static uint32_t t; return t+=50;}
uint32_t fm1_flash_size(void){return sizeof flash;}
int fm1_flash_read(uint32_t o,void*d,size_t n){memcpy(d,flash+o,n);return 0;}
int fm1_flash_erase(uint32_t o){memset(flash+o,0xff,4096);return 0;}
int fm1_flash_write(uint32_t o,const void*d,size_t n){memcpy(flash+o,d,n);return 0;}
const uint8_t*fm1_flash_map(uint32_t o,size_t n){(void)n;return flash+o;}
int fm1_flash_invalidate(uint32_t o,size_t n){return 0;}
int fm1_midi_send_port(unsigned p,const uint8_t*m,size_t n){return 0;}
int fm1_midi_send(const uint8_t*m,size_t n){return 0;}
unsigned fm1_midi_input_source(void){return 1;}
int fm1_device_usb_connected(void){return 1;}
volatile uint16_t orkest_load_q8=31;
static struct fm1_controls ctl;
void fm1_controls_init(void){} void fm1_controls_read(struct fm1_controls*c){*c=ctl;memset(ctl.encoders,0,sizeof ctl.encoders);}
void fm1_led_set(unsigned k,int on){} void fm1_screen_init(void){}
void fm1_screen_fill(unsigned x,unsigned y,unsigned w,unsigned h,uint16_t c){for(unsigned r=y;r<y+h;r++)for(unsigned q=x;q<x+w;q++)fb[r*240+q]=c;}
int fm1_screen_blit(unsigned x,unsigned y,unsigned w,unsigned h,const uint16_t*p,size_t n){for(unsigned r=0;r<h;r++)memcpy(fb+(y+r)*240+x,p+r*w,w*2);return 0;}
void groove_screen_init(void); void groove_screen_control(void);
static void save(const char*f){FILE*o=fopen(f,"wb");fwrite(fb,2,240*240,o);fclose(o);}
int main(void){
  memset(flash,0xff,sizeof flash); memcpy(flash,ORK_BANK_FILE,sizeof ORK_BANK_FILE); ((struct ork_bank_header*)flash)->commit=ORK_COMMIT;
  ctl.knob=900; groove_screen_init(); save("build/host/screen-splash.rgb565");
  for(int i=0;i<40;i++) groove_screen_control();
  ork_stat.last_note=67; ork_stat.voices=6; ctl.encoders[2]=0; groove_screen_control(); for(int i=0;i<3;i++)groove_screen_control(); save("build/host/screen-main.rgb565");
  ctl.encoders[2]=10; groove_screen_control(); for(int i=0;i<3;i++)groove_screen_control(); save("build/host/screen-harp.rgb565");
  ctl.buttons = 1u << 5; groove_screen_control(); ctl.buttons = 0;   /* LFO pressed: its menu; K2 turned */
  ctl.encoders[3] = 45; groove_screen_control(); ctl.encoders[3] = 0;
  for(int i=0;i<3;i++)groove_screen_control(); save("build/host/screen-lfo.rgb565");
  if (!ork_set.lfo_on || ork_set.lfo_vib != 85) { printf("screen test FAIL: LFO menu, K2: vibrato 40 + 45, on\n"); return 1; }
  if (!ork_set.lfo_sync) { printf("screen test FAIL: the LFO starts on the tempo\n"); return 1; }
  ctl.encoders[0] = -1; groove_screen_control(); ctl.encoders[0] = 0;   /* SELECT: FREE */
  ctl.encoders[2] = 3; groove_screen_control(); ctl.encoders[2] = 0;    /* K1 in the menu: the rate, not the sound */
  if (ork_set.patch != 10 || ork_set.lfo_rate != 95) { printf("screen test FAIL: K1 in the LFO menu\n"); return 1; }
  ctl.encoders[0] = 1; groove_screen_control(); ctl.encoders[0] = 0;    /* SELECT: TEMPO */
  ctl.encoders[1] = 8; groove_screen_control(); ctl.encoders[1] = 0;    /* ALGORITHM: 128 BPM */
  ctl.encoders[2] = -1; groove_screen_control(); ctl.encoders[2] = 0;   /* K1: 1/8T -> 1/8 */
  for(int i=0;i<3;i++)groove_screen_control(); save("build/host/screen-lfo-tempo.rgb565");
  if (!ork_set.lfo_sync || ork_set.lfo_ticks != 12 || ork_set.lfo_rate != 95 ||
      ork_set.lfo_inc != (uint32_t)((((uint64_t)1 << 32) * 128u * 128u * 24u) / (44100ull * 60u * 12u))) {
    printf("screen test FAIL: TEMPO 128 BPM, 1/8\n"); return 1; }
  ctl.encoders[0] = -1; groove_screen_control(); ctl.encoders[0] = 0;   /* SELECT: FREE again */
  if (ork_set.lfo_sync || ork_set.lfo_inc != LFO_INC[95]) { printf("screen test FAIL: FREE again\n"); return 1; }
  ctl.buttons = 1u << 8; groove_screen_control(); ctl.buttons = 0; groove_screen_control();   /* HOME: back */
  ctl.encoders[2] = 1; groove_screen_control(); ctl.encoders[2] = 0;
  if (ork_set.patch != 11) { printf("screen test FAIL: HOME closes the LFO menu\n"); return 1; }
  ctl.encoders[2] = -1; groove_screen_control(); ctl.encoders[2] = 0;
  ctl.buttons = 1u << 5; groove_screen_control(); ctl.buttons = 0; groove_screen_control();   /* menu: vibrato to 0 */
  ctl.encoders[3] = -127; ctl.encoders[4] = -127; groove_screen_control(); ctl.encoders[3] = ctl.encoders[4] = 0;
  ctl.buttons = 1u << 5; groove_screen_control(); ctl.buttons = 0; groove_screen_control();   /* LFO again: back */
  if (ork_set.lfo_on) { printf("screen test FAIL: vibrato and tremolo at 0: LFO off\n"); return 1; }
  if (ork_bank.np != 24 || ork_set.patch != 10 || strcmp(ork_bank.patch[10].name, "HARP")) { printf("screen test FAIL: the bank changed under the screen\n"); return 1; }
  ctl.buttons = 1u << 4; groove_screen_control(); ctl.buttons = 0;   /* ENV: its menu; K3 transient +20, K4 tail -64 */
  ctl.encoders[4] = 20; ctl.encoders[5] = -64; groove_screen_control(); ctl.encoders[4] = ctl.encoders[5] = 0;
  for(int i=0;i<3;i++)groove_screen_control(); save("build/host/screen-env.rgb565");
  if (ork_set.transient != 84 || ork_set.tail != 0 || ork_set.patch != 10) { printf("screen test FAIL: ENV menu, K3 / K4\n"); return 1; }
  ctl.buttons = 1u << 4; groove_screen_control(); ctl.buttons = 0; groove_screen_control();   /* ENV again: back */
  ctl.encoders[4] = 5; groove_screen_control(); ctl.encoders[4] = 0;
  if (ork_set.transient != 84) { printf("screen test FAIL: ENV closes its menu\n"); return 1; }
  ctl.buttons = 1u << 10; groove_screen_control(); ctl.buttons = 0;   /* ARP: part 1 (on); K1 sound +1, K2 phrase +12, PRESETS: LATCH */
  ctl.encoders[2] = 1; ctl.encoders[3] = 12; ctl.encoders[6] = 1; groove_screen_control();
  ctl.encoders[2] = ctl.encoders[3] = 0; ctl.encoders[6] = 1; groove_screen_control(); ctl.encoders[6] = 0;
  for(int i=0;i<3;i++)groove_screen_control(); save("build/host/screen-arp.rgb565");
  if (!ork_set.arp_on || ork_set.part[0].phrase != 12 || ork_set.part[0].patch != 1 || ork_set.arp_mode != 2 || ork_set.patch != 10) {
    printf("screen test FAIL: ARP part 1 (%d %d %d %d)\n", ork_set.arp_on, ork_set.part[0].phrase, ork_set.arp_mode, ork_set.patch); return 1; }
  ctl.buttons = 1u << 10; groove_screen_control(); ctl.buttons = 0;   /* ARP: part 1's velocity: K1 pattern +3, K4 gate -10 */
  ctl.encoders[2] = 3; ctl.encoders[5] = -10; groove_screen_control(); ctl.encoders[2] = ctl.encoders[5] = 0;
  for(int i=0;i<3;i++)groove_screen_control(); save("build/host/screen-arp-velo.rgb565");
  ctl.buttons = 1u << 10; groove_screen_control(); ctl.buttons = 0;   /* ARP: part 2 (off): K1 +4 -> the 4th sound */
  ctl.encoders[2] = 4; groove_screen_control(); ctl.encoders[2] = 0;
  for(int i=0;i<3;i++)groove_screen_control(); save("build/host/screen-arp-part2.rgb565");
  if (ork_set.part[0].velo != 3 || ork_set.part[0].gate != 80 || ork_set.part[1].patch != 3) {
    printf("screen test FAIL: ARP pages (%d %d %d)\n", ork_set.part[0].velo, ork_set.part[0].gate, ork_set.part[1].patch); return 1; }
  ctl.buttons = 1u << 8; groove_screen_control(); ctl.buttons = 0; groove_screen_control();   /* HOME: back, ARP stays on */
  ork_stat.last_note = 69;
  for(int i=0;i<3;i++)groove_screen_control(); save("build/host/screen-main-arp.rgb565");
  if (!ork_set.arp_on) { printf("screen test FAIL: ARP off after HOME\n"); return 1; }
  ctl.buttons = 1u << 3; groove_screen_control(); ctl.buttons = 0;    /* SCL: K1 root A (-3), K2 MINOR, no SAVE */
  ctl.encoders[2] = -3; ctl.encoders[3] = 2; groove_screen_control(); ctl.encoders[2] = ctl.encoders[3] = 0;
  for(int i=0;i<3;i++)groove_screen_control(); save("build/host/screen-scale.rgb565");
  ctl.buttons = 1u << 3; groove_screen_control(); ctl.buttons = 0; groove_screen_control();    /* SCL again: closed, the scale kept */
  ork_set.scale = 0; ork_set.scale_root = 0;
  groove_screen_init();                                                 /* a power cycle */
  if (ork_set.scale != 2 || ork_set.scale_root != 9) { printf("screen test FAIL: scale kept (%d %d)\n", ork_set.scale, ork_set.scale_root); return 1; }
ctl.buttons = 1u << 7; groove_screen_control(); ctl.buttons = 0;    /* GLO: panning; K1 -6 (spread 70 %) */
  ctl.encoders[2] = -6; groove_screen_control(); ctl.encoders[2] = 0;
  for(int i=0;i<3;i++)groove_screen_control(); save("build/host/screen-pan.rgb565");
  if (ork_set.pan_spread != 70 || ork_set.pan_mode) { printf("screen test FAIL: GLO (%d)\n", ork_set.pan_spread); return 1; }
  printf("screen test passed (build/host/screen-*.rgb565)\n");
  return 0;}
