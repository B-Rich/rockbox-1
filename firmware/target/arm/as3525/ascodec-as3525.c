/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2008 by Bertrik Sikken
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

/*
    Provides access to the codec/charger/rtc/adc part of the as3525.
    This part is on address 0x46 of the internal i2c bus in the as3525.
    Registers in the codec part seem to be nearly identical to the registers
    in the AS3514 (used in the "v1" versions of the sansa c200 and e200).
    
    I2C register description:
    * I2C2_CNTRL needs to be set to 0x51 for transfers to work at all.
      bit 0: ? possibly related to using ACKs during transfers
      bit 1: direction of transfer (0 = write, 1 = read)
      bit 2: use 2-byte slave address
    * I2C2_IMR, I2C2_RIS, I2C2_MIS, I2C2_INT_CLR interrupt bits:
      bit 2: byte read interrupt
      bit 3: byte write interrupt
      bit 4: ? possibly some kind of error status
      bit 7: ACK error
    * I2C2_SR (status register) indicates in bit 0 if a transfer is busy.
    * I2C2_SLAD0 contains the i2c slave address to read from / write to.
    * I2C2_CPSR0/1 is the divider from the peripheral clock to the i2c clock.
    * I2C2_DACNT sets the number of bytes to transfer and actually starts it.
    
    When a transfer is attempted to a non-existing i2c slave address,
    interrupt bit 7 is raised and DACNT is not decremented after the transfer.
 */

#include "ascodec-target.h"
#include "clock-target.h"
#include "kernel.h"
#include "as3525.h"
#include "i2c.h"

#define I2C2_DATA       *((volatile unsigned int *)(I2C_AUDIO_BASE + 0x00))
#define I2C2_SLAD0      *((volatile unsigned int *)(I2C_AUDIO_BASE + 0x04))
#define I2C2_CNTRL      *((volatile unsigned int *)(I2C_AUDIO_BASE + 0x0C))
#define I2C2_DACNT      *((volatile unsigned int *)(I2C_AUDIO_BASE + 0x10))
#define I2C2_CPSR0      *((volatile unsigned int *)(I2C_AUDIO_BASE + 0x1C))
#define I2C2_CPSR1      *((volatile unsigned int *)(I2C_AUDIO_BASE + 0x20))
#define I2C2_IMR        *((volatile unsigned int *)(I2C_AUDIO_BASE + 0x24))
#define I2C2_RIS        *((volatile unsigned int *)(I2C_AUDIO_BASE + 0x28))
#define I2C2_MIS        *((volatile unsigned int *)(I2C_AUDIO_BASE + 0x2C))
#define I2C2_SR         *((volatile unsigned int *)(I2C_AUDIO_BASE + 0x30))
#define I2C2_INT_CLR    *((volatile unsigned int *)(I2C_AUDIO_BASE + 0x40))
#define I2C2_SADDR      *((volatile unsigned int *)(I2C_AUDIO_BASE + 0x44))

static struct mutex as_mtx;

void i2c_init(void)
{
}

/* initialises the internal i2c bus and prepares for transfers to the codec */
void ascodec_init(void)
{
    int prescaler;

    /* enable clock */
    CGU_PERI |= CGU_I2C_AUDIO_MASTER_CLOCK_ENABLE;

    /* prescaler for i2c clock */
    prescaler = AS3525_I2C_PRESCALER;
    I2C2_CPSR0 = prescaler & 0xFF;          /* 8 lsb */
    I2C2_CPSR1 = (prescaler >> 8) & 0x3;    /* 2 msb */
    
    /* set i2c slave address of codec part */
    I2C2_SLAD0 = AS3514_I2C_ADDR << 1;

    I2C2_CNTRL = 0x51;
    
    mutex_init(&as_mtx);
}


/* returns != 0 when busy */
static int i2c_busy(void)
{
    return (I2C2_SR & 1);
}

/* returns 0 on success, <0 otherwise */
int ascodec_write(unsigned int index, unsigned int value)
{
    ascodec_lock();

    /* wait if still busy */
    while (i2c_busy());

    if (index == AS3514_CVDD_DCDC3) {
        /* prevent setting of the LREG_CP_not bit */
        value &= ~(1 << 5);
    }
    
    /* start transfer */
    I2C2_SADDR = index;
    I2C2_CNTRL &= ~(1 << 1);
    I2C2_DATA = value;
    I2C2_DACNT = 1;
    
    /* wait for transfer */
    while (I2C2_DACNT != 0);

    ascodec_unlock();

    return 0;
}


/* returns value read on success, <0 otherwise */
int ascodec_read(unsigned int index)
{
    int data;

    ascodec_lock();

    /* wait if still busy */
    while (i2c_busy());

    /* start transfer */
    I2C2_SADDR = index;
    I2C2_CNTRL |= (1 << 1);
    I2C2_DACNT = 1;
    
    /* wait for transfer*/
    while (I2C2_DACNT != 0);
    data = I2C2_DATA;
    
    ascodec_unlock();
    
    return data;
}

int ascodec_readbytes(int index, int len, unsigned char *data)
{
    int i;

    ascodec_lock();

    for(i=0; i<len; i++)
    {
        int temp = ascodec_read(index+i);
        if(temp == -1)
            break;
        else
            data[i] = temp;
    }

    ascodec_unlock();

    return i;
}

void ascodec_lock(void)
{
    mutex_lock(&as_mtx);
}

void ascodec_unlock(void)
{
    mutex_unlock(&as_mtx);
}
