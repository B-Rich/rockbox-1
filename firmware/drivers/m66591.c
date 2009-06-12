/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id: usb-mr500.c 18487 2008-09-10 20:14:22Z bertrik $
 *
 * Copyright (C) 2009 by Karl Kurbjun
 * Portions Copyright (C) 2007 by Catalin Patulea
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
#define LOGF_ENABLE

#include "system.h"
#include "config.h"
#include "string.h"
#include "usb_ch9.h"
#include "usb_core.h"
#include "kernel.h"
#include "panic.h"
#include "usb_drv.h"
#include "logf.h"

#include "config.h"
#include "cpu.h"
#include "ata.h"
#include "usb.h"
#include "usb-target.h"
#include "m66591.h"

/*******************************************************************************
 *  These are the driver specific defines.
 ******************************************************************************/
#define HISPEED

/* Right now sending blocks till the full transfer has completed, this needs to
 *  be fixed so that it does not require a block. (USB_TRAN_LOCK ideally would
 *  not be set).
 */
#define USB_TRAN_BLOCK

/*******************************************************************************
 * The following functions are all helpers which should not be called directly
 *  from the USB stack.  They should only be called by eachother, or the USB
 *  stack visible functions.
 ******************************************************************************/

static volatile unsigned short * pipe_ctrl_addr(int pipe);
static void pipe_handshake(int pipe, int handshake);
static void pipe_c_select (int pipe, bool dir);
#if !defined(USB_TRAN_BLOCK)
static int pipe_buffer_size (int pipe);
#endif
static int pipe_maxpack_size (int pipe);
static void control_received(void);
static void transfer_complete(int endpoint);
static int mxx_transmit_receive(int endpoint);
static int mxx_queue(int endpoint, void * ptr, int length, bool send);

struct M66591_epstat {
    unsigned char dir;      /* endpoint direction */
    char *buf;              /* user buffer to store data */
    int length;             /* how match data will fit */
    volatile int count;     /* actual data count */
    bool waiting;           /* is there data to transfer? */
    bool busy;              /* has the pipe been requested for use? */
} ;

static struct M66591_epstat M66591_eps[USB_NUM_ENDPOINTS];

/* This function is used to return the control address for each pipe */
static volatile unsigned short * pipe_ctrl_addr(int pipe) {
    if(pipe==0) {
        return &M66591_DCPCTRL;
    } else {
        return &M66591_PIPECTRL1 + (pipe-1);
    }
}

/* This function sets the pipe/endpoint handshake */
static void pipe_handshake(int pipe, int handshake) {
    handshake&=0x03;
    
    if(handshake == PIPE_SHAKE_STALL) {
        if( *(pipe_ctrl_addr(pipe)) & 0x03 ) {
            *(pipe_ctrl_addr(pipe)) = 0x03;
        } else {
            *(pipe_ctrl_addr(pipe)) = 0x02;
        }
    } else {
        *(pipe_ctrl_addr(pipe)) = handshake;
    }
}

/* This function chooses the pipe desired and waits the required time before
 *  warites/reads are valid */
static void pipe_c_select (int pipe, bool dir) {
    M66591_CPORT_CTRL0 = pipe | (1<<10) | (dir<<5);
    
    // Wait for the Pipe to be valid;
    udelay(2); 
}

#if !defined(USB_TRAN_BLOCK)
/* This returns the maximum buffer size of each pipe.  On this device the size
 *  is fixed.
 */
static int pipe_buffer_size (int pipe) {
    switch(pipe) {
    case 0:
        return 256;
    case 1:
    case 2:
        return 1024;
    case 3:
    case 4:
        return 512;
    case 5:
    case 6:
        return 64;
    default:
        return 0;
    }
}
#endif

/* This function returns the maximum packet size for each endpoint/pipe.  It is
 *  Currently only setup to support Highspeed mode. 
 */
static int pipe_maxpack_size (int pipe) {
    switch(pipe) {
    case 0:
        /* DCP max packet size is configurable */
        return M66591_DCP_MXPKSZ;
    case 1:
    case 2:
    case 3:
    case 4:
        return 512;
    case 5:
    case 6:
        return 64;
    default:
        return 0;
    }
}

/* This is a helper function that is only called from the interupt handler.  It
 *  copies the control packet information from the PHY and notifies the stack.
 */
static void control_received(void) {
    /* copy setup data from packet */
    static struct usb_ctrlrequest temp;
    
    memcpy(&temp, (unsigned char*)&M66591_USB_REQ0, 8);
    
    logf("mxx: bReqType=0x%02x bReq=0x%02x wVal=0x%04x"
        " wIdx=0x%04x wLen=0x%04x", 
        temp.bRequestType, temp.bRequest, temp.wValue,
        temp.wIndex, temp.wLength);

    /* acknowledge packet recieved (clear valid) */
    M66591_INTSTAT_MAIN &= ~(1<<3);

    usb_core_control_request(&temp);
}

/* This is a helper function, it is used to notife the stack that a transfer is
 *  done.
 */
static void transfer_complete(int endpoint) {
    M66591_INTCFG_EMP &= ~(1 << endpoint);
    logf("mxx: ep %d transfer complete", endpoint);
    int temp=M66591_eps[endpoint].dir ? USB_DIR_IN : USB_DIR_OUT;
    usb_core_transfer_complete(endpoint, temp, 0, 
        M66591_eps[endpoint].length);
}

/* This is the main transmit routine that is typically called from the interrupt
 *  handler (the queue function calls it in some situations)
 */
static int mxx_transmit_receive(int endpoint) {
    logf("mxx: do start");
    
    /* Only the lower 15 bits of the endpoint correlate to the pipe number.
     *  For example pipe 2 will corelate to endpoint 0x82, so the upper bits
     * need to be masked out.
     */
    endpoint &= 0x7F;

    int i;      /* Used as a loop counter */
    int length; /* Used in transfers to determine the amount to send/receive */
    
    bool send=M66591_eps[endpoint].dir;
    
    /* This is used as the internal buffer pointer */
    unsigned short *ptrs;

    /* Choose the pipe that data is being transfered on */
    pipe_c_select(endpoint, send);

    /* Check to see if the endpoint is ready and give it some time to become
     *  ready.  If it runs out of time exit out as an error.
     */
    i = 0;
    while (!(M66591_CPORT_CTRL1&(1<<13))) {
        if (i++ > 100000) {
            logf("mxx: FIFO %d not ready", endpoint);
            return -1;
        }
    }

    /* Write to FIFO */
    if(send) {
        int maxpack=pipe_maxpack_size(endpoint);
#if defined(USB_TRAN_BLOCK)
        length = M66591_eps[endpoint].length;
#else
        int bufsize=pipe_buffer_size(endpoint);
        length=MIN(M66591_eps[endpoint].length, bufsize);
#endif

        /* Calculate the position in the buffer, all transfers should be 2-byte
         *  aligned till the last packet or short packet.
         */
        ptrs = (unsigned short *)(M66591_eps[endpoint].buf
            + M66591_eps[endpoint].count);
        
        /* Start sending data in 16-bit words */
        for (i = 0; i < (length>>1); i++) { 
            /* This wait is dangerous in the event htat something happens to 
             *  the PHY pipe where it never becomes ready again, should probably
             *  add a timeout, and ideally completely remove.
             */
            while(!(M66591_CPORT_CTRL1&(1<<13))){};

            M66591_CPORT = *ptrs++;
            M66591_eps[endpoint].count+=2;
        }
        
        /* If the length is odd, send the last byte after setting the byte width
         *  of the FIFO.
         */
        if(length & 0x01) {
            /* Unset MBW (8-bit transfer) */
            M66591_CPORT_CTRL0 &= ~(1<<10);
            M66591_CPORT = *((unsigned char *)ptrs - 1);
            M66591_eps[endpoint].count++;
        }
        
        /* Set BVAL if length is not a multiple of the maximum packet size */
        if( (length == 0) || (length % maxpack != 0) ) {
            logf("mxx: do set BVAL");
            M66591_CPORT_CTRL1 |= (1<<15);
        }
        
        /* If the transfer is complete set up interrupts to notify when FIFO is
         *  EMPTY, disable READY and let the handler know that there is nothing
         *  left to transfer on this pipe.
         */
        if(M66591_eps[endpoint].count == M66591_eps[endpoint].length) {
            /* Enable Empty flag */
            M66591_INTCFG_EMP |= 1 << endpoint;
            /* Disable ready flag */
            M66591_INTCFG_RDY &= ~(1 << endpoint);
            /* Nothing left to transfer */
            M66591_eps[endpoint].waiting=false;
        } else {
            /* There is still data to transfer, make sure READY is enabled */
            M66591_INTCFG_RDY |= 1 << endpoint;
        }
    } else {
        /* Read data from FIFO */
        
        /* Read the number of bytes that the PHY received */
        int receive_length=M66591_CPORT_CTRL1 & 0x03FF;
        
        /* The number of bytes to actually read is either what's left of the
         *  amount requested, or the amount that the PHY received.  Choose the
         *  smaller of the two.
         */
        length = MIN(M66591_eps[endpoint].length - M66591_eps[endpoint].count, 
            receive_length);

        /* If the length is zero, just clear the buffer as specified in the
         *  datasheet.  Otherwise read in the data (in 16-bit pieces */
        if(length==0) {
            /* Set the BCLR bit */
            M66591_CPORT_CTRL1 |= 1<<14; 
        } else {
            /* Set the position in the buffer */
            ptrs = (unsigned short *)(M66591_eps[endpoint].buf 
                + M66591_eps[endpoint].count);
        
            /* Read in the data (buffer size should be even).  The PHY cannot
             *  switch from 16-bit mode to 8-bit mode on an OUT buffer.
             */
            for (i = 0; i < ((length+1)>>1); i++) {
                *ptrs++ = M66591_CPORT;
                M66591_eps[endpoint].count+=2;
            }
        }
        
        /* If the length was odd subtract 1 from the count */
        M66591_eps[endpoint].count -= (length&0x01);
        
        /* If the requested size of data was received, or the data received was
         *  less than the maximum packet size end the transfer.
         */
        if( (M66591_eps[endpoint].count == M66591_eps[endpoint].length) 
            || (length % pipe_maxpack_size(endpoint)) ) {
            
            /* If the host tries to send anything else the FIFO is not ready/
             *  enabled yet (NAK).
             */
            pipe_handshake(endpoint, PIPE_SHAKE_NAK);
            /* Tell the interrupt handler that transfer is complete. */
            M66591_eps[endpoint].waiting=false;
            /* Disable ready */
            M66591_INTCFG_RDY &= ~(1 << endpoint);
            
            /* Let the stack know that the transfer is complete */
            if(endpoint!=0)
                transfer_complete(endpoint);
        }
    }
    
    logf("mxx: do done ep %d %s len: %d cnt: %d", endpoint, 
        send ? "out" : "in", length, M66591_eps[endpoint].count);

    return 0;
}

/* This function is used to start transfers.  It is a helper function for the 
 *  usb_drv_send_nonblocking, usb_drv_send, and usb_drv_receive functions.
 */
static int mxx_queue(int endpoint, void * ptr, int length, bool send) {
    /* Disable IRQs */
    int flags = disable_irq_save();

    /* Only the lower 15 bits of the endpoint correlate to the pipe number.
     *  For example pipe 2 will corelate to endpoint 0x82, so the upper bits
     *  need to be masked out.
     */
    endpoint &= 0x7F;
    
    /* Initialize the enpoint status registers used for the transfer */
    M66591_eps[endpoint].buf=ptr;
    M66591_eps[endpoint].length=length;
    M66591_eps[endpoint].count=0;
    M66591_eps[endpoint].dir=send;
    M66591_eps[endpoint].waiting=true;

    logf("mxx: queue ep %d %s, len: %d", endpoint, send ? "out" : "in", length);
    
    /* Pick the pipe that communications are happening on */
    pipe_c_select(endpoint, send);

    /* All transfers start with a BUF handshake */
    pipe_handshake(endpoint, PIPE_SHAKE_BUF);
    
    /* This USB PHY takes care of control completion packets by setting the
     *  CCPL bit in EP0 (endpoint 0, or DCP).  If the control state is "write no
     *  data tranfer" then we just need to set the CCPL bit (hopefully) 
     *  regardless of what the stack said to send.
     */
    int control_state = (M66591_INTSTAT_MAIN & 0x07);
    if(endpoint==0 && control_state==CTRL_WTND) {
        logf("mxx: queue ep 0 ctls: 5, set ccpl");
        
        /* Set CCPL */
        M66591_DCPCTRL |= 1<<2; 
    } else {
        /* This is the standard case for transmitting data */
        if(send) {
            /* If the pipe is not ready don't try and send right away; instead 
             *  just set the READY interrupt so that the handler can initiate
             *  the transfer.
             */
            if((M66591_CPORT_CTRL1&(1<<13))) {
                mxx_transmit_receive(endpoint);
            } else {
                M66591_INTCFG_RDY |= 1 << endpoint;
            }
            
            if(length==0) {
                transfer_complete(endpoint);
            }
        } else {
            /* When receiving data, just enable the ready interrupt, the PHY
             *  will trigger it and then the reads can start.
             */
            M66591_INTCFG_RDY |= 1 << endpoint;
        }
    }

    /* Re-enable IRQs */
    restore_irq(flags);
    return 0;
}

/*******************************************************************************
 * This is the interrupt handler for this driver.  It should be called from the
 *  target interrupt handler routine (eg. GPIO3 on M:Robe 500).
 ******************************************************************************/
void USB_DEVICE(void) {
    int pipe_restore=M66591_CPORT_CTRL0;
    logf("mxx: INT BEGIN tick: %d\n", (int) current_tick);
    
    logf("mxx: sMAIN0: 0x%04x, sRDY: 0x%04x", 
        M66591_INTSTAT_MAIN, M66591_INTSTAT_RDY);
    logf("mxx:  sNRDY: 0x%04x, sEMP: 0x%04x", 
        M66591_INTSTAT_NRDY, M66591_INTSTAT_EMP);

    /* VBUS (connected) interrupt */
	while ( M66591_INTSTAT_MAIN & (1<<15) ) {
	    M66591_INTSTAT_MAIN &= ~(1<<15);
	
	    /* If device is not clocked, interrupt flag must be set manually */
	    if ( !(M66591_TRN_CTRL & (1<<10)) ) {
		    M66591_INTSTAT_MAIN |= (1<<15);
	    }
	}
	
	/* Resume interrupt: This is not used. Extra logic needs to be added similar
	 *  to the VBUS interrupt incase the PHY clock is not running.
	 */
	if(M66591_INTSTAT_MAIN & (1<<14)) {
	    M66591_INTSTAT_MAIN &= ~(1<<14);
	    logf("mxx: RESUME");
	}
	
	/* Device state transition interrupt: Not used, but useful for debugging */
	if(M66591_INTSTAT_MAIN & (1<<12)) {
	    M66591_INTSTAT_MAIN &= ~(1<<12);
	    logf("mxx: DEV state CHANGE=%d", 
	        ((M66591_INTSTAT_MAIN & (0x07<<4)) >> 4) );
	}

    /* Control transfer stage interrupt */
    if(M66591_INTSTAT_MAIN & (1<<11)) {
        M66591_INTSTAT_MAIN &= ~(1<<11);
        int control_state = (M66591_INTSTAT_MAIN & 0x07);
    
        logf("mxx: CTRT with CTSQ=%d", control_state);
            
        switch ( control_state ) {
        case CTRL_IDLE:
            transfer_complete(0);
            break;
        case CTRL_RTDS: 
        case CTRL_WTDS:
        case CTRL_WTND:
            // If data is not valid stop
	        if(!(M66591_INTSTAT_MAIN & (1<<3)) ) {
	            logf("mxx: CTRT interrupt but VALID is false");
	            break;
            }
            control_received();
            break;
        case CTRL_RTSS:
        case CTRL_WTSS:
            pipe_handshake(0, PIPE_SHAKE_BUF);
            M66591_DCPCTRL |= 1<<2; // Set CCPL
            break;
        default:
            logf("mxx: CTRT with unknown CTSQ");
            break;
        }
    }
    
    /* FIFO EMPTY interrupt: when this happens the transfer should be complete.
     *  When the interrupt occurs notify the stack.
     */
    if(M66591_INTSTAT_MAIN & (1<<10)) {
        int i;
        logf("mxx: INT EMPTY: 0x%04x", M66591_INTSTAT_EMP);
        
        for(i=0; i<USB_NUM_ENDPOINTS; i++) {
            if(M66591_INTSTAT_EMP&(1<<i)) {
                /* Clear the empty flag */
                M66591_INTSTAT_EMP=~(1<<i);
                /* Notify the stack */
                transfer_complete(i);
            }
        }
    }
    
    /* FIFO NOT READY interrupt: This is not used, but included incase the
     *  interrupt is endabled.
     */
    if(M66591_INTSTAT_MAIN & (1<<9)) {
        logf("mxx: INT NOT READY: 0x%04x", M66591_INTSTAT_NRDY);
        M66591_INTSTAT_NRDY = 0;
    }

    /* FIFO READY interrupt: This just initiates transfers if they are needed */
    if(M66591_INTSTAT_MAIN & (1<<8)) {
        int i;
        logf("mxx: INT READY: 0x%04x", M66591_INTSTAT_RDY);
        
        for(i=0; i<USB_NUM_ENDPOINTS; i++) {
            /* Was this endpoint ready and waiting */
            if(M66591_INTSTAT_RDY&(1<<i) && M66591_eps[i].waiting) {
                /* Clear the ready flag */
                M66591_INTSTAT_RDY=~(1<<i);
                /* It was ready and waiting so start a transfer */
                mxx_transmit_receive(i);
            }
        }
    }
    
    /* Make sure that the INTStatus register is completely cleared. */
    M66591_INTSTAT_MAIN = 0;
    
    /* Restore the pipe state before the interrupt occured */
    M66591_CPORT_CTRL0=pipe_restore;
    logf("\nmxx: INT END");
}

/*******************************************************************************
 *  The following functions are all called by and visible to the USB stack.
 ******************************************************************************/

/* The M55691 handles this automatically, nothing to do */
void usb_drv_set_address(int address) {
    (void) address;
}

/* This function sets the standard test modes, it is not required, but might as
 *  well implement it since the hardware supports it
 */
void usb_drv_set_test_mode(int mode) {
    /* This sets the test bits and assumes that mode is from 0 to 0x04 */
    M66591_TESTMODE &= 0x0007;
    M66591_TESTMODE |= mode;
}

/* Request an unused endpoint, support for interrupt endpoints needs addition */
int usb_drv_request_endpoint(int type, int dir) {
    int ep;
    int pipecfg = 0;
    
    if (type != USB_ENDPOINT_XFER_BULK)
        return -1;

    /* The endpoint/pipes are hard coded: This could be more flexible */
    if (dir == USB_DIR_IN) {
        pipecfg |= (1<<4);
        ep = 2;
    } else {
        ep = 1;
    }
    
    if (!M66591_eps[ep].busy) {
        M66591_eps[ep].busy = true;
        M66591_eps[ep].dir = dir;
    } else {
        logf("mxx: ep %d busy", ep);
        return -1;
    }
    
    M66591_PIPE_CFGSEL=ep;
    
    pipecfg |= 1<<15 | 1<<9 | 1<<8;
    
    pipe_handshake(ep, PIPE_SHAKE_NAK);

    // Setup the flags
    M66591_PIPE_CFGWND=pipecfg;
    
    logf("mxx: ep req ep#: %d config: 0x%04x", ep, M66591_PIPE_CFGWND);

    return ep | dir;
}

/* Used by stack to tell the helper functions that the pipe is not in use */
void usb_drv_release_endpoint(int ep) {
    int flags;
    ep &= 0x7f;

    if (ep < 1 || ep > USB_NUM_ENDPOINTS || M66591_eps[ep].busy == false)
        return ;

    flags = disable_irq_save();
    
    logf("mxx: ep %d release", ep);

    M66591_eps[ep].busy = false;
    M66591_eps[ep].dir = -1;

    restore_irq(flags);
}

/* Periodically called to check if a cable was plugged into the device */
inline int usb_detect(void)
{
    if(M66591_INTSTAT_MAIN&(1<<7))
        return USB_INSERTED;
    else
        return USB_EXTRACTED;
}

void usb_enable(bool on) {
    logf("mxx: %s: %s", __FUNCTION__, on ? "true" : "false");
    if (on)
        usb_core_init();
    else
        usb_core_exit();
}

/* This is where the driver stuff starts */
void usb_drv_init(void) {
    logf("mxx: Device Init");
    
    /* State left behind by m:robe 500i original firmware */
    M66591_TRN_CTRL         = 0x8001; /* External 48 MHz clock */
    M66591_TRN_LNSTAT       = 0x0040; /* "Reserved. Set it to '1'." */
    
    M66591_PIN_CFG0         = 0x0000;
    M66591_PIN_CFG1         = 0x8000; /* Drive Current: 3.3V setting */
    M66591_PIN_CFG2         = 0x0000;
    
    M66591_INTCFG_MAIN      = 0x0000; /* All Interrupts Disable for now */
    M66591_INTCFG_OUT       = 0x0000; /* Sense is edge, polarity is low */
    M66591_INTCFG_RDY       = 0x0000;
    M66591_INTCFG_NRDY      = 0x0000;
    M66591_INTCFG_EMP       = 0x0000;
    
    M66591_INTSTAT_MAIN     = 0;
    M66591_INTSTAT_RDY      = 0;
    M66591_INTSTAT_NRDY     = 0;
    M66591_INTSTAT_EMP      = 0;
}

/* fully enable driver */
void usb_attach(void) {
    int i;
    
    /* Reset Endpoint states */
    for(i=0; i<USB_NUM_ENDPOINTS; i++) {
        M66591_eps[i].dir = -1;
        M66591_eps[i].buf = (char *) 0;
        M66591_eps[i].length = 0;
        M66591_eps[i].count = 0;
        M66591_eps[i].waiting = false;
        M66591_eps[i].busy = false;
    }

    /* Issue a h/w reset */
    usb_init_device();
    usb_drv_init();
    
    /* USB Attach Process: This follows the flow diagram in the M66591GP 
     *  Reference Manual Rev 1.00, p. 77 */

#if defined(HISPEED)
    /* Run Hi-Speed */
    M66591_TRN_CTRL |= 1<<7;
#else
    /* Run Full-Speed */
    M66591_TRN_CTRL &= ~(1<<7);
#endif

    /* Enable oscillation buffer */
	M66591_TRN_CTRL |= (1<<13);

    udelay(1500);

    /* Enable reference clock, PLL */
    M66591_TRN_CTRL |= (3<<11);

    udelay(9);

    /* Enable internal clock supply */
    M66591_TRN_CTRL |= (1<<10);

    /* Disable PIPE ready interrupts */
    M66591_INTCFG_RDY = 0;
    
    /* Disable PIPE not-ready interrupts */
    M66591_INTCFG_NRDY = 0;
    
    /* Disable PIPE empyt/size error interrupts */
    M66591_INTCFG_EMP = 0;

    /* Enable all interrupts except NOT READY, RESUME, and VBUS */
    M66591_INTCFG_MAIN = 0x1DFF;

    pipe_c_select(0, false);
    
    /* Enable continuous transfer mode on the DCP */
    M66591_DCP_CNTMD |= (1<<8);
    
    /* Set the threshold that the PHY will automatically transmit from EP0 */
    M66591_DCP_CTRLEN = 128;
    
    pipe_handshake(0, PIPE_SHAKE_NAK);
    
    /* Set the Max packet size to 64 */
    M66591_DCP_MXPKSZ = 64;

    /* Attach notification to PC (D+ pull-up) */
    M66591_TRN_CTRL |= (1<<4);

    logf("mxx: attached");
}

void usb_drv_exit(void) {
    /* USB Detach Process: This follows the flow diagram in the M66591GP 
     *  Reference Manual Rev 1.00, p. 78.
     */

	/* Detach notification to PC (disable D+ pull-up) */
	M66591_TRN_CTRL &= ~(1<<4);

	/* Software reset */
	M66591_TRN_CTRL &= ~0x01;

	/* Disable internal clock supply */
	M66591_TRN_CTRL &= ~(1<<10);
	udelay(3);

	/* Disable PLL */
	M66591_TRN_CTRL &= ~(1<<11);
	udelay(3);

	/* Disable internal reference clock */
	M66591_TRN_CTRL &= ~(1<<12);
	udelay(3);

	/* Disable oscillation buffer, reenable USB operation */
	M66591_TRN_CTRL &= ~(1<<13);
	
	M66591_TRN_CTRL |= 0x01;

	logf("mxx: detached");
}

/* This function begins a transmit (on an IN endpoint), it should not block
 *  so the actual transmit is done in the interrupt handler.
 */
int usb_drv_send_nonblocking(int endpoint, void* ptr, int length)
{
    /* The last arguement for queue specifies the dir of data (true==send) */
    return mxx_queue(endpoint, ptr, length, true);
}

/* This function begins a transmit (on an IN endpoint), it does not block
 *  so the actual transmit is done in the interrupt handler.
 */
int usb_drv_send(int endpoint, void* ptr, int length)
{
    /* The last arguement for queue specifies the dir of data (true==send) */
    return mxx_queue(endpoint, ptr, length, true);
}

/* This function begins a receive (on an OUT endpoint), it should not block
 *  so the actual receive is done in the interrupt handler.
 */
int usb_drv_recv(int endpoint, void* ptr, int length)
{
    /* Last arguement for queue specifies the dir of data (false==receive) */
    return mxx_queue(endpoint, ptr, length, false);
}

/* This function checks the reset handshake speed status 
 *  (Fullspeed or Highspeed)
 */
int usb_drv_port_speed(void)
{
    int handshake = (M66591_HSFS & 0xFF);
    
    if( handshake == 0x02) {
        return 0; /* Handshook at Full-Speed */
    } else if( handshake == 0x03) {
        return 1; /* Handshook at Hi-Speed */
    } else {
        return -1; /* Error, handshake may not be complete */
    }
}

/* This function checks if the endpoint is stalled (error).  I am not sure what
 *  the "in" variable is intended for.
 */
bool usb_drv_stalled(int endpoint,bool in)
{
    (void) in;
    
    bool stalled = (*(pipe_ctrl_addr(endpoint)) & (0x02)) ? true : false;
    
    logf("mxx: stall?: %s ep: %d", stalled ? "true" : "false", endpoint);
    
    if(stalled) {
        return true;
    } else {
        return false;
    }
}

/* This function stalls/unstalls the endpoint.  Stalls only happen on error so
 *  if the endpoint is functioning properly this should not be called.  I am
 *  not sure what the "in" variable is intended for.
 */
void usb_drv_stall(int endpoint, bool stall,bool in)
{
    (void) in;
    
    logf("mxx: stall - ep: %d", endpoint);
    
    if(stall) {
        /* Stall the pipe (host needs to intervene/error) */
        pipe_handshake(endpoint, PIPE_SHAKE_STALL);
    } else {
        /* Setting this to a NAK, not sure if it is appropriate */
        pipe_handshake(endpoint, PIPE_SHAKE_NAK);
    }
}

/* !!!!!!!!!!This function is likely incomplete!!!!!!!!!!!!!! */
void usb_drv_cancel_all_transfers(void)
{
    int endpoint;
    int flags;

    logf("mxx: %s", __func__);

    flags = disable_irq_save();
    for (endpoint = 0; endpoint < USB_NUM_ENDPOINTS; endpoint++) {
        if (M66591_eps[endpoint].buf) {
            M66591_eps[endpoint].buf = NULL;
        }
    }
    
    restore_irq(flags);
}
