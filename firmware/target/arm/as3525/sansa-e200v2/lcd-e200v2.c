/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2004 by Linus Nielsen Feltzing
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "config.h"

#include "cpu.h"
#include "lcd.h"
#include "kernel.h"
#include "thread.h"
#include <string.h>
#include <stdlib.h>
#include "file.h"
#include "debug.h"
#include "system.h"
#include "font.h"
#include "bidi.h"
#include "clock-target.h"

static bool display_on = false; /* is the display turned on? */
static bool display_flipped = false;
/* we need to write a red pixel for correct button reads
 * (see lcd_button_support()), but that must not happen while the lcd is
 * updating so block lcd_button_support the during updates */
static volatile bool lcd_busy = false;

/* register defines */
#define R_START_OSC             0x00
#define R_DRV_OUTPUT_CONTROL    0x01
#define R_DRV_WAVEFORM_CONTROL  0x02
#define R_ENTRY_MODE            0x03
#define R_COMPARE_REG1          0x04
#define R_COMPARE_REG2          0x05

#define R_DISP_CONTROL1     0x07
#define R_DISP_CONTROL2     0x08
#define R_DISP_CONTROL3     0x09

#define R_FRAME_CYCLE_CONTROL 0x0b
#define R_EXT_DISP_IF_CONTROL 0x0c

#define R_POWER_CONTROL1    0x10
#define R_POWER_CONTROL2    0x11
#define R_POWER_CONTROL3    0x12
#define R_POWER_CONTROL4    0x13

#define R_RAM_ADDR_SET  0x21
#define R_WRITE_DATA_2_GRAM 0x22

#define R_GAMMA_FINE_ADJ_POS1   0x30
#define R_GAMMA_FINE_ADJ_POS2   0x31
#define R_GAMMA_FINE_ADJ_POS3   0x32
#define R_GAMMA_GRAD_ADJ_POS    0x33

#define R_GAMMA_FINE_ADJ_NEG1   0x34
#define R_GAMMA_FINE_ADJ_NEG2   0x35
#define R_GAMMA_FINE_ADJ_NEG3   0x36
#define R_GAMMA_GRAD_ADJ_NEG    0x37

#define R_GAMMA_AMP_ADJ_RES_POS     0x38
#define R_GAMMA_AMP_AVG_ADJ_RES_NEG 0x39

#define R_GATE_SCAN_POS         0x40
#define R_VERT_SCROLL_CONTROL   0x41
#define R_1ST_SCR_DRV_POS       0x42
#define R_2ND_SCR_DRV_POS       0x43
#define R_HORIZ_RAM_ADDR_POS    0x44
#define R_VERT_RAM_ADDR_POS     0x45

/* Flip Flag */
#define R_ENTRY_MODE_HORZ_NORMAL 0x7030
#define R_ENTRY_MODE_HORZ_FLIPPED 0x7000
static unsigned short r_entry_mode = R_ENTRY_MODE_HORZ_NORMAL;
#define R_ENTRY_MODE_VERT 0x7038
#define R_ENTRY_MODE_SOLID_VERT  0x1038
#define R_ENTRY_MODE_VIDEO_NORMAL 0x7020
#define R_ENTRY_MODE_VIDEO_FLIPPED 0x7010


/* Reverse Flag */
#define R_DISP_CONTROL_NORMAL 0x0004
#define R_DISP_CONTROL_REV    0x0000
static unsigned short r_disp_control_rev = R_DISP_CONTROL_NORMAL;

static inline void lcd_delay(int x)
{
    do {
        asm volatile ("nop\n");
    } while (x--);
}

/* DBOP initialisation, do what OF does */
static void ams3525_dbop_init(void)
{
    CGU_DBOP = (1<<3) | AS3525_DBOP_DIV;

    DBOP_TIMPOL_01 = 0xe167e167;
    DBOP_TIMPOL_23 = 0xe167006e;
    
    /* short count, 16bit write, read-timing =8 */
    DBOP_CTRL = (1<<18)|(1<<12)|(8<<0);

    GPIOB_AFSEL = 0xfc;
    GPIOC_AFSEL = 0xff;

    DBOP_TIMPOL_23 = 0x6000e;
    
    /* short count,write enable, 16bit write, read-timing =8 */
    DBOP_CTRL = (1<<18)|(1<<16)|(1<<12)|(8<<0);
    DBOP_TIMPOL_01 = 0x6e167;
    DBOP_TIMPOL_23 = 0xa167e06f;

    /* TODO: The OF calls some other functions here, but maybe not important */
}

static void lcd_write_single_data16(unsigned short value)
{
    DBOP_CTRL &= ~(1<<14|1<<13);
    lcd_delay(10);
    DBOP_DOUT16 = value;
    while ((DBOP_STAT & (1<<10)) == 0);
}

static void lcd_write_cmd(int cmd)
{
    /* Write register */
    DBOP_TIMPOL_23 = 0xa167006e;
    lcd_write_single_data16(cmd);

    /* Wait for fifo to empty */
    while ((DBOP_STAT & (1<<10)) == 0);

    /* Fuze OF has this loop and it seems to help us now also */
    int delay=8;
    while(delay--);

    DBOP_TIMPOL_23 = 0xa167e06f;
}

void lcd_write_data(const fb_data* p_bytes, int count)
{
    const long *data;
    if ((int)p_bytes & 0x3)
    {   /* need to do a single 16bit write beforehand if the address is */
        /* not word aligned*/
        lcd_write_single_data16(*p_bytes);
        count--;p_bytes++;
    }
    /* from here, 32bit transfers are save */
    /* set it to transfer 4*(outputwidth) units at a time, */
    /* if bit 12 is set it only does 2 halfwords though */
    DBOP_CTRL |= (1<<13|1<<14);
    data = (long*)p_bytes;
    while (count > 1)
    {
        DBOP_DOUT32 = *data++;
        count -= 2;

        /* Wait if push fifo is full */
        while ((DBOP_STAT & (1<<6)) != 0);
    }
    /* While push fifo is not empty */
    while ((DBOP_STAT & (1<<10)) == 0);

    /* due to the 32bit alignment requirement or uneven count,
     * we possibly need to do a 16bit transfer at the end also */
    if (count > 0)
        lcd_write_single_data16(*(fb_data*)data);
}

static void lcd_write_reg(int reg, int value)
{
    fb_data data = value;

    lcd_write_cmd(reg);
    lcd_write_single_data16(data);
}

/*** hardware configuration ***/

void lcd_set_contrast(int val)
{
    (void)val;
}

void lcd_set_invert_display(bool yesno)
{
    r_disp_control_rev = yesno ? R_DISP_CONTROL_REV :
                                 R_DISP_CONTROL_NORMAL;

    if (display_on)
    {
        lcd_write_reg(R_DISP_CONTROL1, 0x0033 | r_disp_control_rev);
    }

}

/* turn the display upside down  */
void lcd_set_flip(bool yesno)
{
    display_flipped = yesno;

    r_entry_mode = yesno ? R_ENTRY_MODE_HORZ_FLIPPED :
                           R_ENTRY_MODE_HORZ_NORMAL;
}

static void lcd_window(int xmin, int ymin, int xmax, int ymax)
{
    if (!display_flipped)
    {
        lcd_write_reg(R_HORIZ_RAM_ADDR_POS, (xmax << 8) | xmin);
        lcd_write_reg(R_VERT_RAM_ADDR_POS,  (ymax << 8) | ymin);
        lcd_write_reg(R_RAM_ADDR_SET,       (ymin << 8) | xmin);
    }
    else
    {
        lcd_write_reg(R_HORIZ_RAM_ADDR_POS, 
            ((LCD_WIDTH-1 - xmin) << 8) | (LCD_WIDTH-1 - xmax));
        lcd_write_reg(R_VERT_RAM_ADDR_POS, 
            ((LCD_HEIGHT-1 - ymin) << 8) | (LCD_HEIGHT-1 - ymax));
        lcd_write_reg(R_RAM_ADDR_SET, 
            ((LCD_HEIGHT-1 - ymin) << 8) | (LCD_WIDTH-1 - xmin));
    }
}

static void _display_on(void)
{
    /* Initialisation the display the same way as the original firmware */

    lcd_write_reg(R_START_OSC, 0x0001); /* Start Oscilation */

    lcd_write_reg(R_DRV_OUTPUT_CONTROL, 0x011b); /* 220 lines, GS=0, SS=1 */

    /* B/C = 1: n-line inversion form
     * EOR = 1: polarity inversion occurs by applying an EOR to odd/even
     *          frame select signal and an n-line inversion signal.
     * FLD = 01b: 1 field interlaced scan, external display iface */
    lcd_write_reg(R_DRV_WAVEFORM_CONTROL, 0x0700);

    /* Address counter updated in horizontal direction; left to right;
     * vertical increment horizontal increment.
     * data format for 8bit transfer or spi = 65k (5,6,5) */
    lcd_write_reg(R_ENTRY_MODE, r_entry_mode);

    /* Replace data on writing to GRAM */
    lcd_write_reg(R_COMPARE_REG1, 0);
    lcd_write_reg(R_COMPARE_REG2, 0);

    /* GON = 0, DTE = 0, D1-0 = 00b */
    lcd_write_reg(R_DISP_CONTROL1, 0x0000 | r_disp_control_rev);

    /* Front porch lines: 2; Back porch lines: 2; */
    lcd_write_reg(R_DISP_CONTROL2, 0x0203);

    /* Scan cycle = 0 frames */
    lcd_write_reg(R_DISP_CONTROL3, 0x0000);

    /* 16 clocks */
    lcd_write_reg(R_FRAME_CYCLE_CONTROL, 0x0000);

    /* 18-bit RGB interface (one transfer/pixel)
     * internal clock operation;
     * System interface/VSYNC interface */
    lcd_write_reg(R_EXT_DISP_IF_CONTROL, 0x0000);


    /* zero everything*/
    lcd_write_reg(R_POWER_CONTROL1, 0x0000); /* STB = 0, SLP = 0 */

    lcd_delay(10);

    /* initialise power supply */

    /* DC12-10 = 000b: Step-up1 = clock/8,
     * DC02-00 = 000b: Step-up2 = clock/16,
     * VC2-0   = 010b: VciOUT = 0.87 * VciLVL */
    lcd_write_reg(R_POWER_CONTROL2, 0x0002);

    /* VRH3-0 = 1000b: Vreg1OUT = REGP * 1.90 */
    lcd_write_reg(R_POWER_CONTROL3, 0x0008);

    lcd_delay(40);

    lcd_write_reg(R_POWER_CONTROL4, 0x0000); /* VCOMG = 0 */

    /* This register is unknown */
    lcd_write_reg(0x56, 0x80f);


    lcd_write_reg(R_POWER_CONTROL1, 0x4140);

    lcd_delay(10);

    lcd_write_reg(R_POWER_CONTROL2, 0x0000);
    lcd_write_reg(R_POWER_CONTROL3, 0x0013);

    lcd_delay(20);

    lcd_write_reg(R_POWER_CONTROL4, 0x6d0e);

    lcd_delay(20);

    lcd_write_reg(R_POWER_CONTROL4, 0x6d0e);

    lcd_write_reg(R_GAMMA_FINE_ADJ_POS1, 0x0002);
    lcd_write_reg(R_GAMMA_FINE_ADJ_POS2, 0x0707);
    lcd_write_reg(R_GAMMA_FINE_ADJ_POS3, 0x0182);
    lcd_write_reg(R_GAMMA_GRAD_ADJ_POS, 0x0203);
    lcd_write_reg(R_GAMMA_FINE_ADJ_NEG1, 0x0706);
    lcd_write_reg(R_GAMMA_FINE_ADJ_NEG2, 0x0006);
    lcd_write_reg(R_GAMMA_FINE_ADJ_NEG3, 0x0706);
    lcd_write_reg(R_GAMMA_GRAD_ADJ_NEG, 0x0000);
    lcd_write_reg(R_GAMMA_AMP_ADJ_RES_POS, 0x030f);
    lcd_write_reg(R_GAMMA_AMP_AVG_ADJ_RES_NEG, 0x0f08);


    lcd_write_reg(R_GATE_SCAN_POS, 0);
    lcd_write_reg(R_VERT_SCROLL_CONTROL, 0);

    lcd_window(0, 0, LCD_WIDTH-1, LCD_HEIGHT-1);
    lcd_write_reg(R_1ST_SCR_DRV_POS, (LCD_HEIGHT-1) << 8);
    lcd_write_reg(R_2ND_SCR_DRV_POS, (LCD_HEIGHT-1) << 8);

    lcd_write_reg(R_DISP_CONTROL1, 0x0033 | r_disp_control_rev);

    display_on=true;  /* must be done before calling lcd_update() */
    lcd_update();
}

/* LCD init */
void lcd_init_device(void)
{
    ams3525_dbop_init();

    /* Init GPIOs the same as the OF */

    GPIOA_DIR |= (1<<5);
    GPIOA_PIN(5) = 0;

    GPIOA_PIN(4) = 0;  /*c80b0040 := 0;*/

    lcd_delay(1);

    GPIOA_PIN(5) = (1<<5);

    lcd_delay(1);

    _display_on();
}

#if defined(HAVE_LCD_ENABLE)
void lcd_enable(bool on)
{
    if(display_on!=on)
    {
        if(on)
        {
            _display_on();
            send_event(LCD_EVENT_ACTIVATION, NULL);
        }
        else
        {
            display_on=false;
            lcd_write_reg(R_POWER_CONTROL1, 0x0001);
        }
    }
}
#endif

#if defined(HAVE_LCD_ENABLE) || defined(HAVE_LCD_SLEEP)
bool lcd_active(void)
{
    return display_on;
}

#endif

/*** update functions ***/

static unsigned lcd_yuv_options = 0;

/* Line write helper function for lcd_yuv_blit. Write two lines of yuv420. */
extern void lcd_write_yuv420_lines(unsigned char const * const src[3],
                                   int width,
                                   int stride);
extern void lcd_write_yuv420_lines_odither(unsigned char const * const src[3],
                                   int width,
                                   int stride,
                                   int x_screen, /* To align dither pattern */
                                   int y_screen);

void lcd_yuv_set_options(unsigned options)
{
    lcd_yuv_options = options;
}

static void lcd_window_blit(int xmin, int ymin, int xmax, int ymax)
{
    if (!display_flipped)
    {
        lcd_write_reg(R_HORIZ_RAM_ADDR_POS, 
            ((LCD_WIDTH-1 - xmin) << 8) | (LCD_WIDTH-1 - xmax));
        lcd_write_reg(R_VERT_RAM_ADDR_POS,  (ymax << 8) | ymin);
        lcd_write_reg(R_RAM_ADDR_SET, 
            (ymin << 8) | (LCD_WIDTH-1 - xmin));
    }
    else
    {
        lcd_write_reg(R_HORIZ_RAM_ADDR_POS, (xmax << 8) | xmin);
        lcd_write_reg(R_VERT_RAM_ADDR_POS,  (ymax << 8) | ymin);
        lcd_write_reg(R_RAM_ADDR_SET,       (ymax << 8) | xmin);
    }
}

/* Performance function to blit a YUV bitmap directly to the LCD
 * src_x, src_y, width and height should be even
 * x, y, width and height have to be within LCD bounds
 */
void lcd_blit_yuv(unsigned char * const src[3],
                  int src_x, int src_y, int stride,
                  int x, int y, int width, int height)
{
    unsigned char const * yuv_src[3];
    off_t z;

    lcd_busy = true;

    /* Sorry, but width and height must be >= 2 or else */
    width &= ~1;
    height >>= 1;

    z = stride*src_y;
    yuv_src[0] = src[0] + z + src_x;
    yuv_src[1] = src[1] + (z >> 2) + (src_x >> 1);
    yuv_src[2] = src[2] + (yuv_src[1] - src[1]);

    if (!display_flipped)
    {
        lcd_write_reg(R_ENTRY_MODE, R_ENTRY_MODE_VIDEO_NORMAL);
    }
    else
    {
        lcd_write_reg(R_ENTRY_MODE, R_ENTRY_MODE_VIDEO_FLIPPED);
    }

    if (lcd_yuv_options & LCD_YUV_DITHER)
    {
        do
        {
            lcd_window_blit(y, x, y+1, x+width-1);

            /* Start write to GRAM */
            lcd_write_cmd(R_WRITE_DATA_2_GRAM);

            lcd_write_yuv420_lines_odither(yuv_src, width, stride, x, y);
            yuv_src[0] += stride << 1; /* Skip down two luma lines */
            yuv_src[1] += stride >> 1; /* Skip down one chroma line */
            yuv_src[2] += stride >> 1;
            y+=2;
        }
        while (--height > 0);
    }
    else
    {
        do
        {
            lcd_window_blit(y, x, y+1, x+width-1);

            /* Start write to GRAM */
            lcd_write_cmd(R_WRITE_DATA_2_GRAM);

            lcd_write_yuv420_lines(yuv_src, width, stride);
            yuv_src[0] += stride << 1; /* Skip down two luma lines */
            yuv_src[1] += stride >> 1; /* Skip down one chroma line */
            yuv_src[2] += stride >> 1;
            y+=2;
        }
        while (--height > 0);
    }

    lcd_busy = false;
}

/* Update the display.
   This must be called after all other LCD functions that change the display. */
void lcd_update(void)
{
    if (!display_on)
        return;

    lcd_busy = true;

    lcd_write_reg(R_ENTRY_MODE, r_entry_mode);

    /* Set start position and window */
    lcd_window(0, 0, LCD_WIDTH-1, LCD_HEIGHT-1);

    lcd_write_cmd(R_WRITE_DATA_2_GRAM);

    lcd_write_data((fb_data*)lcd_framebuffer, LCD_WIDTH*LCD_HEIGHT);

    lcd_busy = false;
} /* lcd_update */


/* Update a fraction of the display. */
void lcd_update_rect(int x, int y, int width, int height)
{
    const fb_data *ptr;
    int ymax, xmax;


    if (!display_on)
        return;

    xmax = x + width;
    if (xmax >= LCD_WIDTH)
        xmax = LCD_WIDTH - 1; /* Clip right */
    if (x < 0)
        x = 0;                /* Clip left */
    if (x >= xmax)
        return; /* nothing left to do */

    width = xmax - x + 1;     /* Fix width */

    ymax = y + height;
    if (ymax >= LCD_HEIGHT)
        ymax = LCD_HEIGHT - 1; /* Clip bottom */
    if (y < 0)
        y = 0; /* Clip top */
    if (y >= ymax)
        return; /* nothing left to do */

    lcd_busy = true;

    lcd_write_reg(R_ENTRY_MODE, r_entry_mode);
    
    lcd_window(x, y, xmax, ymax);
    lcd_write_cmd(R_WRITE_DATA_2_GRAM);

    ptr = (fb_data*)&lcd_framebuffer[y][x];

    
    height = ymax - y; /* fix height */

    do
    {
        lcd_write_data(ptr, width);
        ptr += LCD_WIDTH;
    }
    while (--height >= 0);

    lcd_busy = false;
} /* lcd_update_rect */

/* writes one red pixel outside the visible area, needed for correct
 * dbop reads */
bool lcd_button_support(void)
{
    fb_data data = (0xf<<12);

    if (lcd_busy)
        return false;

    lcd_write_reg(R_ENTRY_MODE, r_entry_mode);
    /* Set start position and window */
    lcd_window(LCD_WIDTH+1, LCD_HEIGHT+1, LCD_WIDTH+2, LCD_HEIGHT+2);

    lcd_write_cmd(R_WRITE_DATA_2_GRAM);

    lcd_write_single_data16(data);
    return true;
}
