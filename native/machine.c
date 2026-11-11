#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "embedded_assets.h"
#include "presentation.h"
#include "save_dialog.h"
#include "storage.h"
#include "audio.h"
#include "sound_manager.h"
#include "music.h"
#include "high_scores.h"
#include "room_atlas.h"
#include "system_dialog_icon.h"

enum { CY=1, NF=2, PV=4, XF=8, HF=16, YF=32, ZF=64, SF=128, XY=XF|YF, SZP=SF|ZF|PV };
static uint8_t ram[65536], a,f,b,c,d,e,h,l,ixh,ixl,iyh,iyl, alt[8];
static uint8_t ir_i,ir_r,iff,iff2,im,halted,irq_delay;
static uint16_t pc,sp,flyback;
static uint64_t cycles,next_irq=13312,frame,steps,host_frames;
static uint64_t next_frame=79872;
static bool flyback_pending, stopped, headless, trace;
static unsigned faults;
static uint16_t last[32];
static unsigned last_index;

static uint8_t inks[16], firmware_inks[16], top_inks[16], bottom_inks[16];
static int top_mode, bottom_mode, screen_mode, split_line=200;
static uint8_t completed_game_pixels[0x4000];
static unsigned completed_game_base;
static bool completed_game_frame;
static uint8_t crtc[32]={[1]=40,[6]=25,[9]=7,[12]=0x30}, crtc_select;
static uint8_t gate_pen, ppi_a, ppi_c, ay_register;
static uint8_t txt_pen=1, txt_paper, txt_x, txt_y, txt_control, txt_arg_count, txt_arg[2];
static uint8_t headless_keys[10];
static unsigned injected_start=0, injected_end=0, move_begin=0, move_end=0, move_mask=0;
static wchar_t base_path[1024];
static wchar_t profile_name[33]=L"default";
static uint8_t room_sectors[79*512], scores[512], sector_c8[512], boot_sector[512];
static bool score_write_failed;
static uint8_t file_buffer[16384];
static size_t file_size;
static bool file_opened;
static bool audio_on;
static uint64_t sound_commands;
static bool screenshot;
static unsigned frame_limit;
static uint64_t screenshot_errors;

#define PAIR(hi,lo) ((uint16_t)(((uint16_t)(hi)<<8)|(lo)))
#define AF PAIR(a,f)
#define BC PAIR(b,c)
#define DE PAIR(d,e)
#define HL PAIR(h,l)
#define IX PAIR(ixh,ixl)
#define IY PAIR(iyh,iyl)
#define SET_AF(v) do { uint16_t pair_value=(v);a=pair_value>>8;f=pair_value; } while(0)
#define SET_BC(v) do { uint16_t pair_value=(v);b=pair_value>>8;c=pair_value; } while(0)
#define SET_DE(v) do { uint16_t pair_value=(v);d=pair_value>>8;e=pair_value; } while(0)
#define SET_HL(v) do { uint16_t pair_value=(v);h=pair_value>>8;l=pair_value; } while(0)
#define SET_IX(v) do { uint16_t pair_value=(v);ixh=pair_value>>8;ixl=pair_value; } while(0)
#define SET_IY(v) do { uint16_t pair_value=(v);iyh=pair_value>>8;iyl=pair_value; } while(0)
#define SET(pair,v) SET_##pair(v)
#define RAM(addr) ram[(uint16_t)(addr)]
static uint8_t rd(uint16_t addr) { return ram[addr]; }
static uint16_t word(uint16_t addr) { return PAIR(rd(addr+1),rd(addr)); }
static void put_word(uint16_t addr,uint16_t value) { RAM(addr)=value; RAM(addr+1)=value>>8; }
static void push(uint16_t value) { sp-=2; put_word(sp,value); }
static uint16_t pop(void) { uint16_t value=word(sp); sp+=2; return value; }
static uint8_t parity(uint8_t v) {
    v^=v>>4; return ((0x9669u>>(v&15))&1)?PV:0;
}
static uint8_t sz(uint8_t v) { return (v&(SF|XY))|(v?0:ZF); }
static uint8_t szp(uint8_t v) { return sz(v)|parity(v); }
static uint8_t add8(uint8_t lhs,uint8_t rhs,unsigned carry) {
    unsigned sum=lhs+rhs+carry; uint8_t v=sum;
    f=sz(v)|((sum>>8)&CY)|((lhs^rhs^v)&HF)|(((~(lhs^rhs)&(lhs^v))&128)?PV:0);
    return v;
}
static uint8_t sub8(uint8_t lhs,uint8_t rhs,unsigned carry) {
    unsigned diff=(unsigned)lhs-rhs-carry; uint8_t v=diff;
    f=sz(v)|NF|((diff>>8)&CY)|((lhs^rhs^v)&HF)|((((lhs^rhs)&(lhs^v))&128)?PV:0);
    return v;
}
static uint8_t incdec(uint8_t value,bool dec) {
    unsigned carry=f&CY;
    uint8_t result=dec?sub8(value,1,0):add8(value,1,0);
    f=(f&~CY)|carry;
    return result;
}
static uint16_t add16(uint16_t lhs,uint16_t rhs) {
    unsigned sum=lhs+rhs;
    f=(f&SZP)|((sum>>8)&XY)|((lhs^rhs^sum)&0x1000?HF:0)|((sum>>16)&CY);
    return sum;
}
static uint16_t adc16(uint16_t lhs,uint16_t rhs,bool dec) {
    unsigned carry=f&CY, wide=dec?(unsigned)lhs-rhs-carry:(unsigned)lhs+rhs+carry;
    uint16_t result=wide;
    unsigned overflow=(dec?(lhs^rhs):~(lhs^rhs))&(lhs^result)&0x8000;
    f=((result>>8)&(SF|XY))|(result?0:ZF)|(dec?NF:0)|
        (((lhs^rhs^result)&0x1000)?HF:0)|(overflow?PV:0)|((wide>>16)&CY);
    return result;
}
static void alu(unsigned op,uint8_t rhs) {
    switch(op) {
    case 0:a=add8(a,rhs,0);break;
    case 1:a=add8(a,rhs,f&CY);break;
    case 2:a=sub8(a,rhs,0);break;
    case 3:a=sub8(a,rhs,f&CY);break;
    case 4:a&=rhs;f=szp(a)|HF;break;
    case 5:a^=rhs;f=szp(a);break;
    case 6:a|=rhs;f=szp(a);break;
    case 7:sub8(a,rhs,0);f=(f&~XY)|(rhs&XY);break;
    }
}
static uint8_t rotate(unsigned op,uint8_t v) {
    unsigned carry;
    if(op&1) {
        carry=v&1;
        v=(v>>1)|((op==1?carry:op==3?(f&CY):op==5?((v>>7)&1):0)<<7);
    } else {
        carry=v>>7;
        v=(v<<1)|(op==0?carry:op==2?(f&CY):op==6);
    }
    f=szp(v)|carry;return v;
}
static void acc_rotate(unsigned op) {
    uint8_t flags=f&SZP; a=rotate(op,a); f=flags|(a&XY)|(f&CY);
}
static void bit_test(unsigned bit,uint8_t v) {
    f=(f&CY)|HF|(v&XY)|((v&(1u<<bit))?(bit==7?SF:0):ZF|PV);
}
static void decimal_adjust(void) {
    uint8_t before=a, flags=f;
    unsigned correction=0,carry=f&CY;
    if((f&HF)||(!(f&NF)&&(a&15)>9)) correction|=6;
    if(carry||(!(f&NF)&&a>0x99)){ correction|=0x60; carry=CY; }
    a=(f&NF)?a-correction:a+correction;
    f=(flags&NF)|szp(a)|((a^before)&HF)|carry;
}
static void rld(bool left) {
    uint8_t v=rd(HL), old_a=a;
    if(left) { a=(a&0xf0)|(v>>4); RAM(HL)=(v<<4)|(old_a&15); }
    else { a=(a&0xf0)|(v&15); RAM(HL)=(v>>4)|(old_a<<4); }
    f=(f&CY)|szp(a);
}
static bool condition(unsigned code) {
    static const uint8_t masks[]={ZF,ZF,CY,CY,PV,PV,SF,SF};
    return !!(f&masks[code])==!!(code&1);
}
static void exchange_af(void) { uint8_t va=a,vf=f;a=alt[0];f=alt[1];alt[0]=va;alt[1]=vf; }
static void exchange_all(void) {
    uint8_t *regs[]={&b,&c,&d,&e,&h,&l};
    for(unsigned j=0;j<6;++j){uint8_t v=*regs[j];*regs[j]=alt[j+2];alt[j+2]=v;}
}
static uint8_t key_row(unsigned row) {
    if(row>=10) return 0xff;
    return headless?headless_keys[row]:display_key_row(row)&headless_keys[row];
}
static uint8_t in_port(uint16_t port) {
    switch(port>>8) {
    case 0xf4:return ay_register==14?key_row(ppi_c&15):ppi_a;
    case 0xf5:return cycles%79872<3072?0x1f:0x1e;
    case 0xf6:return ppi_c;
    default:return 0xff;
    }
}
static void change_mode(unsigned mode) {
    screen_mode=mode&3;
    if ((frame==0 || cycles%79872<12000) && split_line==200) top_mode=screen_mode;
}
static const uint8_t hardware_ink[32]={
    13,13,19,25,1,7,10,16,7,25,24,26,6,8,15,17,
    1,19,18,20,0,2,9,11,4,22,21,23,3,5,12,14
};
static void out_port(uint16_t port,uint8_t value) {
    if((port&0xc000)==0x4000) {
        switch(value>>6) {
        case 0:gate_pen=value&31;break;
        case 1:if(gate_pen<16) inks[gate_pen]=hardware_ink[value&31];break;
        case 2:change_mode(value);break;
        }
    }
    switch(port>>8) {
    case 0xbc:crtc_select=value&31;break;
    case 0xbd:crtc[crtc_select]=value;break;
    case 0xf4:ppi_a=value;break;
    case 0xf6:
        ppi_c=value;
        if((value&0xc0)==0xc0) ay_register=ppi_a&15;
        if((value&0xc0)==0x80&&audio_on) audio_register(ay_register,ppi_a);
        break;
    case 0xf7:
        if(!(value&128)) {
            unsigned bit=(value>>1)&7;
            ppi_c=(ppi_c&~(1u<<bit))|((value&1u)<<bit);
        }
        break;
    }
}
static bool block_op(unsigned op,int direction) {
    uint8_t value=rd(HL);
    unsigned carry=f&CY;
    if(op==0) {
        RAM(DE)=value;SET(DE,DE+direction);SET(HL,HL+direction);SET(BC,BC-1);
        uint8_t sum=a+value;
        f=(f&(SF|ZF|CY))|(BC?PV:0)|(sum&XF)|((sum&2)<<4);
        return BC!=0;
    }
    if(op==1) {
        SET(HL,HL+direction);SET(BC,BC-1);
        uint8_t result=sub8(a,value,0), adjust=result-!!(f&HF);
        f=(f&(SF|ZF|HF|NF))|carry|(BC?PV:0)|(adjust&XF)|((adjust&2)<<4);
        return BC!=0&&result!=0;
    }
    if(op==2) RAM(HL)=in_port(BC);
    else out_port(BC,value);
    SET(HL,HL+direction);--b;f=sz(b)|NF;
    return b!=0;
}
static void fault(const char *reason) {
    if(stopped) return;
    fprintf(stderr,"%s at $%04X, opcode %02X, frame %llu, AF=%04X BC=%04X DE=%04X HL=%04X IX=%04X IY=%04X SP=%04X\n",
        reason,pc,rd(pc),(unsigned long long)frame,AF,BC,DE,HL,IX,IY,sp);
    fprintf(stderr,"Previous addresses:");
    for(unsigned j=0;j<32;++j) fprintf(stderr," %04X",last[(last_index+j)&31]);
    fprintf(stderr,"\nStack:");
    for(unsigned j=0;j<12;++j) fprintf(stderr," %04X",word(sp+2*j));
    fprintf(stderr,"\n");
    stopped=true; ++faults;
}
static int unknown_address(void) { fault("Untranslated address");return 0; }
static int changed_opcode(void) { fault("Opcode changed");return 0; }
static bool read_exact(const char *name,void *bytes,size_t capacity) {
    size_t size=0;
    if(embedded_asset_read(name,bytes,capacity,&size)&&size==capacity) return true;
    fprintf(stderr,"Missing or invalid asset %s (%zu bytes)\n",name,size);
    return false;
}
static void record_high_scores(const uint8_t *incoming);

static bool raw_sector(unsigned track,unsigned sector,uint8_t *bytes,bool write) {
    if(write) {
        if(track!=39||sector!=0xc9) return false;
        record_high_scores(bytes);
        return true;
    }
    const uint8_t *data=NULL;
    if(track>=31&&track<=39&&sector>=0xc1&&sector<=0xc9) {
        unsigned room=(track-31)*9+sector-0xc1;
        if(room<79) data=room_sectors+512*room;
    }
    if(track==39&&sector==0xc8) data=sector_c8;
    if(track==39&&sector==0xc9) data=scores;
    if(track==3&&sector==0x7b) data=boot_sector;
    if(!data) return false;
    memcpy(bytes,data,512);
    return true;
}
static bool amsdos(uint16_t entry) {
    bool success=true;
    switch(entry) {
    case 0xf081:a=0;break;
    case 0xf083:case 0xf087:case 0xf089:break;
    case 0xf084:case 0xf085:
        success=HL<=0xfe00&&e==0&&raw_sector(d,c,&ram[HL],entry==0xf085);
        a=success?0:0x40;break;
    default:return false;
    }
    f=(f&~CY)|(success?CY:0);
    if(trace) fprintf(stderr,"AMSDOS %04X %u/%02X $%04X => %u\n",entry,d,c,HL,success);
    return true;
}
static void far_call(void) {
    uint16_t target=word(word(pc));
    pc+=2;
    if(!amsdos(target)) { push(pc); pc=target; }
}
#include "translated.inc"
static uint8_t default_ink(unsigned pen) {
    static const uint8_t defaults[]={1,24,20,6,26,0,2,8,10,12,14,16,18,22,4,3};
    return defaults[pen];
}
static void set_mode(unsigned mode) {
    if(mode>2) return;
    screen_mode=top_mode=bottom_mode=mode;
    split_line=200;
    txt_x=txt_y=txt_control=0;
    memset(ram+0xc000,0,16384);
}
static unsigned screen_start(void) {
    return ((crtc[12]&0x30)<<10)|((crtc[12]&3)<<9)|(crtc[13]<<1);
}
static void plot_text(unsigned x,unsigned y,unsigned pen) {
    static const uint8_t bits0[]={0x80,0x08,0x20,0x02},bits1[]={0x80,0x08};
    unsigned mode=(unsigned)screen_mode,pixels_per_byte=2u<<mode;
    if(x>=80*pixels_per_byte||y>=200) return;
    unsigned addr=(screen_start()&0xc000)|((screen_start()+80*(y/8)+x/pixels_per_byte+(y%8)*2048)&0x3fff);
    unsigned shift=x%pixels_per_byte,mask=0,packed=0;
    const uint8_t *bits=mode==0?bits0:bits1;
    for(unsigned j=0;j<(4u>>mode);++j) {
        unsigned bit=bits[j]>>shift; mask|=bit;
        if(pen&(1u<<j)) packed|=bit;
    }
    ram[addr]=(ram[addr]&~mask)|packed;
}
static uint8_t glyphs[128][8];
static void make_glyphs(void) {
    HDC dc=CreateCompatibleDC(NULL);
    BITMAPINFO info={0};
    info.bmiHeader.biSize=sizeof(info.bmiHeader);
    info.bmiHeader.biWidth=8; info.bmiHeader.biHeight=-8;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32;
    uint32_t *pixels=NULL;
    HBITMAP bmp=CreateDIBSection(dc,&info,DIB_RGB_COLORS,(void **)&pixels,NULL,0);
    HFONT font=CreateFontA(-8,5,0,0,FW_BOLD,FALSE,FALSE,FALSE,ANSI_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,NONANTIALIASED_QUALITY,FIXED_PITCH,"Consolas");
    if(dc&&bmp&&font&&pixels) {
        HGDIOBJ old_bitmap=SelectObject(dc,bmp),old_font=SelectObject(dc,font);
        SetTextColor(dc,RGB(255,255,255));SetBkColor(dc,0);
        for(unsigned ch=32;ch<127;++ch) {
            char character=(char)ch;
            memset(pixels,0,256);
            TextOutA(dc,1,-1,&character,1);
            GdiFlush();
            for(unsigned y=0;y<8;++y)
                for(unsigned x=0;x<8;++x)
                    if(pixels[y*8+x]&0xffffff) glyphs[ch][y]|=0x80>>x;
        }
        SelectObject(dc,old_font);SelectObject(dc,old_bitmap);
    }
    if(font) DeleteObject(font);
    if(bmp) DeleteObject(bmp);
    if(dc) DeleteDC(dc);
}
static void put_text(uint8_t ch) {
    if(txt_control) {
        txt_arg[txt_arg_count++]=ch;
        if(txt_control==31&&txt_arg_count<2) return;
        if(txt_control==31){txt_x=txt_arg[0]?txt_arg[0]-1:0;txt_y=ch?ch-1:0;}
        if(txt_control==14) txt_paper=ch&15;
        if(txt_control==15) txt_pen=ch&15;
        if(txt_control==4) set_mode(ch);
        txt_control=0;
        return;
    }
    unsigned max_columns=20u<<screen_mode;
    switch(ch) {
    case 4:case 14:case 15:case 31:txt_control=ch;txt_arg_count=0;return;
    case 8:if(txt_x) --txt_x;return;
    case 9:++txt_x;return;
    case 10:++txt_y;return;
    case 11:if(txt_y) --txt_y;return;
    case 12:memset(ram+0xc000,0,16384);txt_x=txt_y=0;return;
    case 13:txt_x=0;return;
    case 30:txt_x=txt_y=0;return;
    }
    if(ch<32||ch>=128) return;
    if(txt_x>=max_columns){txt_x=0;++txt_y;}
    if(txt_y>=25) txt_y=0;
    for(unsigned y=0;y<8;++y)
        for(unsigned x=0;x<8;++x)
            plot_text(txt_x*8+x,txt_y*8+y,(glyphs[ch][y]&(0x80>>x))?txt_pen:txt_paper);
    ++txt_x;
}
static uint16_t ticker_routine, ticker_divisor, ticker_count;
static bool event_active;
static void save_registers(void) {
    push(AF);push(BC);push(DE);push(HL);push(IX);push(IY);
}
static void restore_registers(void) {
    SET(IY,pop());SET(IX,pop());SET(HL,pop());SET(DE,pop());SET(BC,pop());SET(AF,pop());
}
static int finish_interrupt(void) { iff=iff2=1;pc=pop();return 17; }
static int kernel_irq(void) {
    if(flyback_pending&&flyback) {
        flyback_pending=false;save_registers();push(0xb93c);pc=flyback;return 18;
    }
    return finish_interrupt();
}
static int call_firmware(void) {
    bool carry=true,set_carry=false;
    switch(pc) {
    case 0x0000:case 0x0008:fault("Original game requested firmware reset");return 0;
    case 0x001e:pc=HL;return 4;
    case 0x0038:case 0xb939:return kernel_irq();
    case 0xb93c:restore_registers();return finish_interrupt();
    case 0xb93f:restore_registers();pc=pop();event_active=false;return 8;
    case 0x07ba:case 0xbd19:
        if(cycles%79872>=3072) return 100;
        break;
    case 0xb906:case 0xb909:iff=iff2=1;break;
    case 0xbcce:break;
    case 0xbb1e: {
        bool pressed=a<80&&!(key_row(a>>3)&(1u<<(a&7)));
        f=pressed?0:ZF;
        break;
    }
    case 0xbb24:a=(~key_row(9))&63;h=a;l=(~key_row(6))&63;break;
    case 0xbb5a:put_text(a);break;
    case 0xbb75:txt_x=h?h-1:0;txt_y=l?l-1:0;break;
    case 0xbc0e:set_mode(a);break;
    case 0xbc11:a=screen_mode;break;
    case 0xbc32:inks[a&15]=firmware_inks[a&15]=b%27;break;
    case 0xbc35:b=c=firmware_inks[a&15];break;
    case 0xbc38:case 0xbc3e:break;
    case 0xbc77: {
        char filename[24]; unsigned len=b;
        if(len>16) len=16;
        bool extension=false, valid=len>0;
        for(unsigned j=0;j<len;++j) {
            uint8_t ch=rd(HL+j)&127;
            if(ch>='a'&&ch<='z') ch-=32;
            if(ch=='.') extension=true;
            if(!(ch>='A'&&ch<='Z')&&!(ch>='0'&&ch<='9')&&ch!='.') valid=false;
            filename[j]=ch;
        }
        filename[len]='\0';
        if(!extension) strcat(filename,".BIN");
        file_opened=valid&&embedded_asset_read(filename,file_buffer,sizeof(file_buffer),&file_size);
        if(trace||!file_opened) fprintf(stderr,"Open %s: %s\n",filename,file_opened?"OK":"missing");
        if(file_opened) {
            SET(BC,file_size);SET(HL,0xa700);
        }
        a=file_opened?0:0x40;carry=file_opened;set_carry=true;
        break;
    }
    case 0xbc7a:carry=file_opened;set_carry=true;file_opened=false;break;
    case 0xbc83:
        carry=file_opened&&file_size<=65536u-HL;set_carry=true;
        if(carry) {memcpy(ram+HL,file_buffer,file_size);SET(HL,0);}
        break;
    case 0xbca7:sound_reset();break;
    case 0xbcb0:break;
    case 0xbcad:a=sound_status(a);break;
    case 0xbcaa:
        carry=HL<=65536-9&&sound_queue(ram+HL);
        if(carry) ++sound_commands;
        set_carry=true;
        break;
    case 0xbcbc:case 0xbcbf:
        carry=a>=1&&a<=15&&HL<=65536-16;
        if(carry){sound_envelope(pc==0xbcbf,a,ram+HL);SET(HL,HL+16);}
        set_carry=true;
        break;
    case 0xbcd4:
        carry=rd(HL)>=0x81&&rd(HL)<=0x89;set_carry=true;
        if(carry){SET(HL,0xf000|rd(HL));c=7;}
        break;
    case 0xbcd7:case 0xbcda:flyback=DE;break;
    case 0xbcdd:flyback=0;break;
    case 0xbcef:put_word(HL,DE);break;
    case 0xbce9:
        // KL ADD TICKER receives its initial countdown in DE and its reload in BC.
        ticker_routine=DE?word(HL+6):0;
        ticker_count=DE;
        ticker_divisor=BC;
        break;
    case 0xbcec:case 0xbcf5:ticker_routine=0;break;
    case 0xbd1c:
        change_mode(a);
        if(split_line<200) bottom_mode=screen_mode;
        else top_mode=screen_mode;
        break;
    case 0xbd25:
        if(word(sp)==0x8dd2) {
            memcpy(top_inks,firmware_inks,sizeof(top_inks));top_mode=screen_mode;split_line=144;
        }
        for(unsigned j=0;j<16;++j) inks[j]=hardware_ink[rd(DE+1+j)&31];
        if(split_line<200) memcpy(bottom_inks,inks,sizeof(inks));
        a=16;f=ZF|NF;
        break;
    case 0xbd34:if(audio_on) audio_register(a,c);break;
    default:
        if(!amsdos(pc)) return unknown_address();
        break;
    }
    if(set_carry) f=(f&~(CY|ZF))|(carry?CY:0);
    pc=pop();return 17;
}
static void interrupt(void) {
    if(!iff||irq_delay) return;
    halted=0;
    iff=iff2=0;
    push(pc);
    if(im==2) {
        pc=word(PAIR(ir_i,0xff));
        if(pc==0x5555&&rd(pc)==0xc3) pc=word(pc+1);
    } else pc=0x0038;
    cycles+=im==2?19:13;
}

#define SNAPSHOT_FIELDS(FIELD,ARRAY) \
    FIELD(uint64_t,cycles) FIELD(uint64_t,next_irq) FIELD(uint64_t,next_frame) FIELD(uint64_t,frame) \
    FIELD(uint16_t,pc) FIELD(uint16_t,sp) FIELD(uint16_t,flyback) \
    FIELD(uint16_t,ticker_routine) FIELD(uint16_t,ticker_divisor) FIELD(uint16_t,ticker_count) \
    FIELD(uint8_t,a) FIELD(uint8_t,f) FIELD(uint8_t,b) FIELD(uint8_t,c) FIELD(uint8_t,d) FIELD(uint8_t,e) \
    FIELD(uint8_t,h) FIELD(uint8_t,l) FIELD(uint8_t,ixh) FIELD(uint8_t,ixl) FIELD(uint8_t,iyh) FIELD(uint8_t,iyl) \
    FIELD(uint8_t,ir_i) FIELD(uint8_t,ir_r) FIELD(uint8_t,iff) FIELD(uint8_t,iff2) FIELD(uint8_t,im) \
    FIELD(uint8_t,halted) FIELD(uint8_t,irq_delay) FIELD(uint8_t,flyback_pending) FIELD(uint8_t,event_active) \
    FIELD(uint8_t,gate_pen) FIELD(uint8_t,ppi_a) FIELD(uint8_t,ppi_c) FIELD(uint8_t,ay_register) \
    FIELD(uint8_t,txt_pen) FIELD(uint8_t,txt_paper) FIELD(uint8_t,txt_x) FIELD(uint8_t,txt_y) \
    FIELD(uint8_t,txt_control) FIELD(uint8_t,txt_arg_count) FIELD(uint8_t,crtc_select) \
    FIELD(int32_t,top_mode) FIELD(int32_t,bottom_mode) FIELD(int32_t,screen_mode) FIELD(int32_t,split_line) \
    FIELD(uint32_t,file_size) FIELD(uint8_t,file_opened) \
    ARRAY(ram) ARRAY(alt) ARRAY(crtc) ARRAY(inks) ARRAY(firmware_inks) ARRAY(top_inks) ARRAY(bottom_inks) ARRAY(txt_arg) \
    ARRAY(scores) ARRAY(file_buffer)

#define DECLARE_FIELD(type,name) type name;
#define DECLARE_ARRAY(name) uint8_t name[sizeof(name)];
typedef struct {
    uint32_t format;
    SNAPSHOT_FIELDS(DECLARE_FIELD,DECLARE_ARRAY)
} Snapshot;
#undef DECLARE_FIELD
#undef DECLARE_ARRAY

static Snapshot title_start;

static void capture_snapshot(Snapshot *snapshot) {
    memset(snapshot,0,sizeof(*snapshot));
    snapshot->format=2;
#define CAPTURE_FIELD(type,name) snapshot->name=name;
#define CAPTURE_ARRAY(name) memcpy(snapshot->name,name,sizeof(name));
    SNAPSHOT_FIELDS(CAPTURE_FIELD,CAPTURE_ARRAY)
#undef CAPTURE_FIELD
#undef CAPTURE_ARRAY
}

static void restore_snapshot(const Snapshot *snapshot) {
#define RESTORE_FIELD(type,name) name=snapshot->name;
#define RESTORE_ARRAY(name) memcpy(name,snapshot->name,sizeof(name));
    SNAPSHOT_FIELDS(RESTORE_FIELD,RESTORE_ARRAY)
#undef RESTORE_FIELD
#undef RESTORE_ARRAY
    completed_game_frame=false;
    sound_reset();
    music_stop();
}

static bool gameplay_active(void) { return word(0x92f9)==0x92e1; }
static bool player_game_active(void) { return gameplay_active()&&!(rd(0x6851)&0x80); }
static void update_live_atlas(bool force);

static void return_to_title(void) {
    // The clean start has the original code and overlays, before any quest changes.
    uint8_t current_scores[sizeof(scores)];
    memcpy(current_scores,scores,sizeof(scores));
    restore_snapshot(&title_start);
    memcpy(scores,current_scores,sizeof(scores));
    memset(headless_keys,0xff,sizeof(headless_keys));
}

static void finish_game_frame(void) {
    completed_game_base=screen_start()&0xc000;
    memcpy(completed_game_pixels,ram+completed_game_base,sizeof(completed_game_pixels));
    completed_game_frame=true;
}

static bool door_frame_finished(uint16_t address) {
    // Each address follows a complete door or player redraw in the original
    // transition. This sequence runs outside the regular gameplay frame commit.
    switch(address) {
    case 0x7985: // Player drawn before opening.
    case 0x7998: // Door opening.
    case 0x79b9: // Player moving into the door.
    case 0x7a94: // Destination room drawn.
    case 0x7b15: // Destination door drawn open.
    case 0x7b2d: // Player moving out of the door.
    case 0x7b54: // Door closing.
    case 0x7beb: // Player and object redraw shared by both sides.
        return true;
    default:
        return false;
    }
}

static void draw_frame(void) {
    unsigned start=screen_start();
    if(gameplay_active()&&completed_game_frame&&(start&0xc000)==completed_game_base) {
        // Hold completed sprite pixels while preserving live raster palette changes.
        uint8_t hud_inks[16];
        for(unsigned pen=0;pen<16;++pen) hud_inks[pen]=hardware_ink[rd(0x9374+pen)&31];
        display_frame_split(completed_game_pixels,split_line<200?top_inks:firmware_inks,
                            split_line<200?bottom_inks:hud_inks,start&0x3fff,144,0,1);
    } else if(split_line<200) {
        display_frame_split(ram,top_inks,bottom_inks,start,split_line,top_mode,bottom_mode);
    } else {
        display_frame(ram,inks,start,200,screen_mode,screen_mode);
    }
}
static bool ensure_directory(const wchar_t *path) {
    if(CreateDirectoryW(path,NULL)) return true;
    DWORD attributes=GetFileAttributesW(path);
    return attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY);
}
enum { PROFILE_PATH_CAPACITY=1120, SAVE_DIALOG_PATH_CAPACITY=32768 };
static const wchar_t high_scores_filename[]=L"highscores.dat";

static bool profile_directory(wchar_t *path,size_t capacity,bool create) {
    if(swprintf(path,capacity,L"%lsprofiles",base_path)<0) return false;
    if(create&&!ensure_directory(path)) return false;
    if(swprintf(path,capacity,L"%lsprofiles\\%ls",base_path,profile_name)<0) return false;
    return !create||ensure_directory(path);
}
static bool profile_data_path(wchar_t *path,const wchar_t *filename,bool create) {
    return (!create||profile_directory(path,PROFILE_PATH_CAPACITY,true))&&
        swprintf(path,PROFILE_PATH_CAPACITY,L"%lsprofiles\\%ls\\%ls",base_path,profile_name,filename)>=0;
}

static void update_live_atlas(bool force) {
    static bool active;
    static uint64_t next_update;
    static wchar_t path[PROFILE_PATH_CAPACITY];
    if(headless) return;
    if(!player_game_active()) {
        if(active) room_atlas_clear();
        active=false;
        return;
    }
    if(!active||force) {
        if(!profile_data_path(path,L"Atlas-Live.json",true)) path[0]=L'\0';
        active=true;
        next_update=0;
    }
    if(host_frames<next_update) return;
    room_atlas_update(ram,path[0]?path:NULL);
    next_update=host_frames+12;
}
static bool progress_path(wchar_t *path,bool create) {
    return profile_data_path(path,L"progress.sav",create);
}
static void refresh_high_scores(void) {
    wchar_t path[PROFILE_PATH_CAPACITY];
    if(profile_data_path(path,high_scores_filename,false)&&high_scores_load(path,scores)) {
        memcpy(ram+0x9900,scores,HIGH_SCORE_TABLE_SIZE);
    }
}
static void record_high_scores(const uint8_t *incoming) {
    wchar_t path[PROFILE_PATH_CAPACITY];
    if(!profile_data_path(path,high_scores_filename,true)||!high_scores_submit(path,scores,incoming)) {
        high_scores_merge(scores,incoming);
        fprintf(stderr,"Could not write the high-score table.\n");
        score_write_failed=true;
    }
    // The original qualification and display routines use this live ledger.
    memcpy(ram+0x9900,scores,HIGH_SCORE_TABLE_SIZE);
}
static bool valid_profile(const char *name) {
    size_t len=strlen(name);
    if(!len||len>=sizeof(profile_name)/sizeof(*profile_name)) return false;
    for(size_t n=0;n<len;++n) {
        unsigned ch=(unsigned char)name[n];
        if(!(ch>='0'&&ch<='9')&&!(ch>='a'&&ch<='z')&&!(ch>='A'&&ch<='Z')&&ch!='-'&&ch!='_') return false;
        profile_name[n]=ch;
    }
    profile_name[len]=0;
    return true;
}
static void report_progress(const char *action) {
    printf("%s frame=%llu room=%02X x=%02X y=%02X energy=%02X\n",action,
        (unsigned long long)frame,rd(0x9314),rd(0x6842),rd(0x6841),rd(0x684d));
}
static bool save_progress_to(const wchar_t *path) {
    Snapshot snapshot;
    capture_snapshot(&snapshot);
    if(!storage_save(path,&snapshot,sizeof(snapshot))) return false;
    report_progress("Saved");
    return true;
}
enum { SAVE_ERROR, SAVE_OK, SAVE_CANCELED };
static int save_progress(bool overwrite,bool cancel) {
    wchar_t path[PROFILE_PATH_CAPACITY];
    if(!player_game_active()||!progress_path(path,true)) return SAVE_ERROR;
    if(GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES&&!overwrite) {
        if(cancel||headless||!display_confirm(L"Replace the saved game for this profile?")) return SAVE_CANCELED;
    }
    return save_progress_to(path)?SAVE_OK:SAVE_ERROR;
}
static bool load_progress_from(const wchar_t *path) {
    Snapshot snapshot;
    if(!storage_load(path,&snapshot,sizeof(snapshot))) return false;
    if(snapshot.format!=2||snapshot.file_size>sizeof(file_buffer)||
       snapshot.ram[0x9314]>=79||snapshot.im>2||snapshot.txt_arg_count>2||
       snapshot.screen_mode<0||snapshot.screen_mode>2||snapshot.split_line<0||snapshot.split_line>200||
       snapshot.top_mode<0||snapshot.top_mode>2||snapshot.bottom_mode<0||snapshot.bottom_mode>2||
       snapshot.cycles-snapshot.frame*79872>80000) return false;
    uint8_t current_scores[sizeof(scores)];
    memcpy(current_scores,scores,sizeof(scores));
    restore_snapshot(&snapshot);
    memcpy(scores,current_scores,sizeof(scores));
    wchar_t score_path[PROFILE_PATH_CAPACITY];
    if(profile_data_path(score_path,high_scores_filename,true)&&
       !high_scores_import(score_path,scores,snapshot.scores)) score_write_failed=true;
    memcpy(ram+0x9900,scores,HIGH_SCORE_TABLE_SIZE);
    for(unsigned index=1;index<=15;++index) {
        sound_envelope(false,index,ram+0xa600+16*index);
        sound_envelope(true,index,ram+0xa700+16*index);
    }
    report_progress("Loaded");
    update_live_atlas(true);
    return true;
}
static bool load_progress(void) {
    wchar_t path[PROFILE_PATH_CAPACITY];
    return progress_path(path,false)&&load_progress_from(path);
}
static bool show_progress_dialog(bool saving) {
    bool in_game=player_game_active();
    if(saving&&!in_game) {
        display_notice(L"Start a game before saving progress.",false);
        return false;
    }

    wchar_t profile[PROFILE_PATH_CAPACITY];
    const wchar_t *folder=profile_directory(profile,ARRAYSIZE(profile),true)?profile:base_path;
    wchar_t path[SAVE_DIALOG_PATH_CAPACITY];
    SaveDialogResult result=save_dialog_choose(display_owner(),folder,saving,path,ARRAYSIZE(path));
    if(result==SAVE_DIALOG_CANCELED) return false;
    if(result!=SAVE_DIALOG_SELECTED) {
        display_notice(L"The saved game file dialog could not complete.",true);
        return false;
    }
    if(saving) {
        if(save_progress_to(path)) return true;
        display_notice(L"Could not write the selected save file.",true);
    } else if(!in_game||display_confirm(L"Load the selected save and replace current progress?")) {
        if(load_progress_from(path)) return true;
        display_notice(L"The selected file is not a compatible Sorcery+ save.",true);
    }
    return false;
}

static void close_game_window(void) {
    if(!player_game_active()) {
        stopped=true;
        return;
    }
    int answer=app_message_box(display_owner(),L"Save your progress before closing Sorcery+?",
                               L"Sorcery+",MB_YESNOCANCEL|MB_ICONQUESTION|MB_DEFBUTTON3);
    if(answer==IDNO||(answer==IDYES&&show_progress_dialog(true))) stopped=true;
}

static void apply_audio_settings(void) {
    AudioSettings settings;
    display_get_audio_settings(&settings);
    audio_set_levels(settings.sfx_enabled?settings.sfx_volume:0, settings.music_volume, settings.music_enabled);
}

static void reset_high_scores(void) {
    if(!display_confirm(L"Restore the original high-score table for this profile? This replaces its saved all-time scores.")) return;
    wchar_t path[PROFILE_PATH_CAPACITY];
    uint8_t original[HIGH_SCORES_SECTOR_SIZE];
    if(!read_exact("track-39-sector-c9-high-scores.bin",original,sizeof(original))||
       !profile_data_path(path,high_scores_filename,true)||!high_scores_reset(path,original)) {
        display_notice(L"Could not reset the high-score table for this profile.",true);
        return;
    }
    memcpy(scores,original,sizeof(scores));
    memcpy(ram+0x9900,scores,HIGH_SCORE_TABLE_SIZE);
    score_write_failed=false;
    if(!player_game_active()) return_to_title();
}

static void handle_game_actions(unsigned action) {
    if(action&DISPLAY_ACTION_AUDIO_SETTINGS) apply_audio_settings();
    if(action&DISPLAY_ACTION_CLOSE) {
        close_game_window();
    } else if(action&DISPLAY_ACTION_EXIT_TO_TITLE) {
        if(gameplay_active()) return_to_title();
    } else {
        if(action&DISPLAY_ACTION_RESET_HIGH_SCORES) reset_high_scores();
        if(action&DISPLAY_ACTION_SAVE) show_progress_dialog(true);
        if(action&DISPLAY_ACTION_LOAD) show_progress_dialog(false);
    }
}

static void new_frame(void) {
    ++frame;
    ++host_frames;
    audio_set_title_music(ticker_routine==0x4f51);
    if(player_game_active()) music_start();
    else music_stop();
    sound_frame();
    if(!gameplay_active()) completed_game_frame=false;
    if(!player_game_active()) update_live_atlas(false);
    flyback_pending=true;
    if(headless||injected_start||move_begin) {
        memset(headless_keys,0xff,sizeof(headless_keys));
        if(frame>=injected_start&&frame<injected_end) headless_keys[9]&=~16u;
        if(frame>=move_begin&&frame<move_end) headless_keys[9]&=~move_mask;
    }
    if(trace&&(frame<=5||!(frame%25))) fprintf(stderr,
        "frame %llu PC=%04X IM=%u room=%02X xy=%02X,%02X flags=%02X phase=%u IY=%04X, mode=%d\n",
        (unsigned long long)frame,pc,im,rd(0x9314),rd(0x6842),rd(0x6841),rd(0x684a),rd(0x92fb),IY,screen_mode);
    if(ticker_count) --ticker_count;
    if(ticker_routine&&!ticker_count&&!event_active&&!halted) {
        ticker_count=ticker_divisor;event_active=true;push(pc);save_registers();push(0xb93f);pc=ticker_routine;
        if(!ticker_divisor) ticker_routine=0;
    }
    bool capture=screenshot&&host_frames==frame_limit;
    if(!headless||capture) {
        draw_frame();
        if(capture&&!display_snapshot(L"snapshot.bmp")) ++screenshot_errors;
    }
    if(!headless&&display_pump()) {
        if(score_write_failed) {
            score_write_failed=false;
            display_notice(L"Could not write the high-score table for this profile.",true);
        }
        handle_game_actions(display_take_action());
    } else if(!headless) stopped=true;
    if(split_line<200) memcpy(inks,firmware_inks,sizeof(inks));
    split_line=200;
}
static void game_step(void) {
    while(!stopped&&cycles>=next_frame) { next_frame+=79872; new_frame(); }
    if(stopped) return;
    if(cycles>=next_irq) { next_irq+=13312; interrupt(); }
    if(irq_delay) --irq_delay;
    if(halted) { cycles=next_irq<next_frame?next_irq:next_frame;return; }
    // This disk lacks STRANGE.SCN. Continue the attract sequence after its optional splash.
    if(pc==0x7167) pc=0x71a5;
    // The original routine is about to compare the new score against the all-time table.
    if(pc==0x882b) refresh_high_scores();
    // $74B9 follows the gameplay update's sprite and HUD writes.
    const bool room_ready = pc==0x74b9||(pc==0x7467&&!completed_game_frame)||pc==0x7a94;
    if(room_ready||door_frame_finished(pc)) finish_game_frame();
    if(room_ready) update_live_atlas(false);
    last[last_index++&31]=pc;
    if(trace&&steps<70) fprintf(stderr,"$%04X AF=%04X BC=%04X DE=%04X HL=%04X\n",pc,AF,BC,DE,HL);
    ++steps;
    ++ir_r;
    if((pc>=0x7000&&pc<0xa157)||(pc>=0x4f00&&pc<0x531b)) cycles+=translated_step();
    else cycles+=call_firmware();
}
static bool boot(void) {
    DWORD len=GetModuleFileNameW(NULL,base_path,1024);
    if(!len||len>=1024) return false;
    wchar_t *end=wcsrchr(base_path,L'\\');
    if(!end) return false;
    end[1]=0;
    if(!read_exact("CODE-decoded-7000.bin",ram+0x7000,12631)||
       !read_exact("CONALP-decoded-0100.bin",ram+0x0100,2304)||
       !read_exact("TISCRN-decoded-69c0.bin",ram+0x69c0,720)||
       !read_exact("SOUND-decoded-a600.bin",ram+0xa600,647)||
       !read_exact("track-03-sector-7b-4000.bin",boot_sector,sizeof(boot_sector))||
       !read_exact("room-sectors-00-4e.bin",room_sectors,sizeof(room_sectors))||
       !read_exact("track-39-sector-c9-high-scores.bin",scores,sizeof(scores))||
       !read_exact("track-39-sector-c8.bin",sector_c8,sizeof(sector_c8))) return false;
    refresh_high_scores();
    uint8_t score_code[1051];
    if(!read_exact("MUCODE.BIN",score_code,sizeof(score_code))) return false;
    // The supplied disk stores its music code and note data with this byte transform.
    for(size_t index=0;index<sizeof(score_code);++index) {
        uint8_t byte=(uint8_t)-score_code[index];
        score_code[index]=(uint8_t)(((byte>>1)|(byte<<7))-0x25);
    }
    if(!music_load(score_code,sizeof(score_code))) return false;
    for(unsigned j=0;j<16;++j) inks[j]=default_ink(j);
    memset(headless_keys,0xff,sizeof(headless_keys));
    make_glyphs();
    uint8_t title[4816];
    if(!read_exact("TITLEP.BIN",title,sizeof(title))) return false;
    set_mode(0);
    memcpy(inks,boot_sector+0x119,16);
    memcpy(firmware_inks,inks,sizeof(inks));
    for(unsigned tile=0;tile<720;++tile) {
        unsigned id=title[4096+tile];
        for(unsigned row=0;row<8;++row) {
            unsigned target=0xc000+2*tile+row*2048;
            ram[target]=title[row*512+id];
            ram[target+1]=title[row*512+256+id];
        }
    }
    for(unsigned offset=0x129;offset<sizeof(boot_sector);++offset) {
        uint8_t value=boot_sector[offset];put_text(value&127);
        if(value&128) break;
    }
    put_word(0x39,0xb939);put_word(0xbd1a,0x87ba);
    pc=0x7000;sp=0xbff0;iff=iff2=1;im=1;
    return true;
}
static uint64_t frame_hash(void) {
    uint64_t hash=14695981039346656037ull;
    for(unsigned j=0;j<0x4000;++j) {hash^=rd(screen_start()+j);hash*=1099511628211ull;}
    return hash;
}
static void dump_probe(void) {
    FILE *file=fopen("probe-ram.bin","wb");
    if(file){fwrite(ram,1,sizeof(ram),file);fclose(file);}
    file=fopen("probe-display.bin","wb");
    if(file) {
        fwrite(inks,1,sizeof(inks),file);
        fputc(top_mode,file);fputc(bottom_mode,file);fputc(split_line,file);
        fwrite(top_inks,1,sizeof(top_inks),file);fwrite(bottom_inks,1,sizeof(bottom_inks),file);
        fclose(file);
    }
}
int main(int argc,char **argv) {
    unsigned save_at=0;
    bool dump=false,load=false,overwrite=false,cancel=false,disable_audio=false;
    for(int k=1;k<argc;++k) {
        if(!strcmp(argv[k],"--trace")) trace=true;
        else if(!strcmp(argv[k],"--dump")) dump=true;
        else if(!strcmp(argv[k],"--screenshot")) screenshot=true;
        else if(!strcmp(argv[k],"--audio-hash")) audio_measure();
        else if(!strcmp(argv[k],"--probe")&&k+1<argc) {
            headless=true;frame_limit=strtoul(argv[++k],NULL,10);
            if(!frame_limit) return 2;
        } else if(!strcmp(argv[k],"--show-probe")&&k+1<argc) {
            frame_limit=strtoul(argv[++k],NULL,10);
            if(!frame_limit) return 2;
        } else if(!strcmp(argv[k],"--profile")&&k+1<argc) {
            if(!valid_profile(argv[++k])) {fprintf(stderr,"Invalid profile name.\n");return 2;}
        } else if(!strcmp(argv[k],"--load")) {
            load=true;
        } else if(!strcmp(argv[k],"--save-at")&&k+1<argc) {
            save_at=strtoul(argv[++k],NULL,10);
        } else if(!strcmp(argv[k],"--overwrite")) {
            overwrite=true;
        } else if(!strcmp(argv[k],"--cancel-overwrite")) {
            cancel=true;
        } else if(!strcmp(argv[k],"--mute")) {
            disable_audio=true;
        } else if(!strcmp(argv[k],"--fire")&&k+2<argc) {
            injected_start=strtoul(argv[++k],NULL,10);injected_end=strtoul(argv[++k],NULL,10);
        } else if(!strcmp(argv[k],"--move")&&k+3<argc) {
            move_begin=strtoul(argv[++k],NULL,10);move_end=strtoul(argv[++k],NULL,10);
            move_mask=strtoul(argv[++k],NULL,0);
        } else {fprintf(stderr,"Invalid argument: %s\n",argv[k]);return 2;}
    }
    if(!boot()) {
        if(!headless) app_message_box(NULL,L"Could not start Sorcery+. Try a fresh copy of SorceryPlus.exe.",
                                      L"Sorcery+",MB_OK|MB_ICONERROR);
        return 2;
    }
    capture_snapshot(&title_start);
    if(!headless&&!display_open()) return 2;
    apply_audio_settings();
    if(!headless&&!disable_audio) audio_on=audio_open();
    if(load&&!load_progress()) {fprintf(stderr,"No compatible save for this profile.\n");return 2;}
    LARGE_INTEGER frequency, start, now;
    QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);
    uint64_t shown=0,target=start.QuadPart;
    bool save_failed=false;
    while(!stopped&&(!frame_limit||host_frames<frame_limit)) {
        game_step();
        if(save_at&&host_frames>=save_at) {
            int result=save_progress(overwrite,cancel);
            if(result==SAVE_CANCELED) puts("Canceled save overwrite");
            if(result==SAVE_ERROR) {fprintf(stderr,"Save failed.\n");save_failed=true;}
            save_at=0;
        }
        if(!headless&&host_frames!=shown) {
            shown=host_frames;
            uint64_t interval=frequency.QuadPart/50;
            target+=interval;
            QueryPerformanceCounter(&now);
            if((uint64_t)now.QuadPart>target+interval) target=now.QuadPart;
            do {
                QueryPerformanceCounter(&now);
                if((uint64_t)now.QuadPart+frequency.QuadPart/400<target) Sleep(1);
            } while((uint64_t)now.QuadPart<target);
        }
    }
    QueryPerformanceCounter(&now);
    if(dump) dump_probe();
    printf("Frames=%llu instructions=%llu elapsed_ms=%.2f room=%02X mode=%d vram=%016llx sound_commands=%llu audio_device=%u audio_buffers=%u pcm=%016llx faults=%u\n",
        (unsigned long long)host_frames,(unsigned long long)steps,
        (double)(now.QuadPart-start.QuadPart)*1000.0/frequency.QuadPart,
        rd(0x9314),screen_mode,(unsigned long long)frame_hash(),
        (unsigned long long)sound_commands,audio_on,audio_submissions(),(unsigned long long)audio_pcm_hash(),faults);
    report_progress("Ended");
    if(screenshot_errors) fprintf(stderr,"Could not create snapshot.bmp. Use a folder without a previous snapshot.\n");
    fflush(stdout);
    fflush(stderr);
    if(audio_on) audio_close();
    if(!headless) display_close();
    return faults||save_failed||screenshot_errors?1:0;
}
