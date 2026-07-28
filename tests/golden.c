/* SPDX-License-Identifier: MIT */
/* Generates docs/expected-gudprobe-output.txt. See gud_dump.c. */
#include "gud_dump.h"
#define BLITSCRT_GUD_H
#include "device.h"
#include <string.h>
static struct blitscrt_dev dev;
static int ci(void *c,uint8_t r,uint16_t v,void *b,size_t l){(void)c;int n=blitscrt_handle_ctrl(&dev,r,v,0,NULL,0,b,l);return n<0?GUD_E_IO:n;}
static int co(void *c,uint8_t r,uint16_t v,const void *b,size_t l){(void)c;unsigned char s[512];blitscrt_handle_ctrl(&dev,r,v,0,b,(uint16_t)l,s,sizeof s);return (int)l;}
static int bo(void *c,const void *b,size_t l){(void)c;(void)b;return (int)l;}
int main(void){
  static struct gud_device h; struct gud_transport t={ci,co,bo,NULL};
  blitscrt_dev_init(&dev,NULL);
  if(gud_probe(&h,&t)){puts("probe failed");return 1;}
  gud_dump(stdout,&h);
  return 0;
}
