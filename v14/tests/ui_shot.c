/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Host test of app/orkest_ui.c: the splash and the main screen drawn into a framebuffer (build/host/screen-*.rgb565,
 * tests/ui_png.py makes PNGs), under ASan: every strip inside its buffer, the bank intact afterwards. */
#include <stdio.h>
#include <string.h>
#include "orkest.h"
#include "fm1_device.h"
#include "orkest_bank_file.h"
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
  if (ork_bank.np != 24 || ork_set.patch != 10 || strcmp(ork_bank.patch[10].name, "HARP")) { printf("screen test FAIL: the bank changed under the screen\n"); return 1; }
  printf("screen test passed (build/host/screen-*.rgb565)\n");
  return 0;}
