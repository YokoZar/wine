/*
 * GDI bit-blit operations
 *
 * Copyright 1993, 1994, 2011 Alexandre Julliard
 * Copyright 2006 Damjan Jovanovic
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winternl.h"
#include "x11drv.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(bitblt);


#define DST 0   /* Destination drawable */
#define SRC 1   /* Source drawable */
#define TMP 2   /* Temporary drawable */
#define PAT 3   /* Pattern (brush) in destination DC */

#define OP(src,dst,rop)   (OP_ARGS(src,dst) << 4 | (rop))
#define OP_ARGS(src,dst)  (((src) << 2) | (dst))

#define OP_SRC(opcode)    ((opcode) >> 6)
#define OP_DST(opcode)    (((opcode) >> 4) & 3)
#define OP_SRCDST(opcode) ((opcode) >> 4)
#define OP_ROP(opcode)    ((opcode) & 0x0f)

#define MAX_OP_LEN  6  /* Longest opcode + 1 for the terminating 0 */

#define SWAP_INT32(i1,i2) \
    do { INT __t = *(i1); *(i1) = *(i2); *(i2) = __t; } while(0)

static const unsigned char BITBLT_Opcodes[256][MAX_OP_LEN] =
{
    { OP(PAT,DST,GXclear) },                         /* 0x00  0              */
    { OP(PAT,SRC,GXor), OP(SRC,DST,GXnor) },         /* 0x01  ~(D|(P|S))     */
    { OP(PAT,SRC,GXnor), OP(SRC,DST,GXand) },        /* 0x02  D&~(P|S)       */
    { OP(PAT,SRC,GXnor) },                           /* 0x03  ~(P|S)         */
    { OP(PAT,DST,GXnor), OP(SRC,DST,GXand) },        /* 0x04  S&~(D|P)       */
    { OP(PAT,DST,GXnor) },                           /* 0x05  ~(D|P)         */
    { OP(SRC,DST,GXequiv), OP(PAT,DST,GXnor), },     /* 0x06  ~(P|~(D^S))    */
    { OP(SRC,DST,GXand), OP(PAT,DST,GXnor) },        /* 0x07  ~(P|(D&S))     */
    { OP(PAT,DST,GXandInverted), OP(SRC,DST,GXand) },/* 0x08  S&D&~P         */
    { OP(SRC,DST,GXxor), OP(PAT,DST,GXnor) },        /* 0x09  ~(P|(D^S))     */
    { OP(PAT,DST,GXandInverted) },                   /* 0x0a  D&~P           */
    { OP(SRC,DST,GXandReverse), OP(PAT,DST,GXnor) }, /* 0x0b  ~(P|(S&~D))    */
    { OP(PAT,SRC,GXandInverted) },                   /* 0x0c  S&~P           */
    { OP(SRC,DST,GXandInverted), OP(PAT,DST,GXnor) },/* 0x0d  ~(P|(D&~S))    */
    { OP(SRC,DST,GXnor), OP(PAT,DST,GXnor) },        /* 0x0e  ~(P|~(D|S))    */
    { OP(PAT,DST,GXcopyInverted) },                  /* 0x0f  ~P             */
    { OP(SRC,DST,GXnor), OP(PAT,DST,GXand) },        /* 0x10  P&~(S|D)       */
    { OP(SRC,DST,GXnor) },                           /* 0x11  ~(D|S)         */
    { OP(PAT,DST,GXequiv), OP(SRC,DST,GXnor) },      /* 0x12  ~(S|~(D^P))    */
    { OP(PAT,DST,GXand), OP(SRC,DST,GXnor) },        /* 0x13  ~(S|(D&P))     */
    { OP(PAT,SRC,GXequiv), OP(SRC,DST,GXnor) },      /* 0x14  ~(D|~(P^S))    */
    { OP(PAT,SRC,GXand), OP(SRC,DST,GXnor) },        /* 0x15  ~(D|(P&S))     */
    { OP(SRC,TMP,GXcopy), OP(PAT,SRC,GXnand),
      OP(SRC,DST,GXand), OP(TMP,DST,GXxor),
      OP(PAT,DST,GXxor) },                           /* 0x16  P^S^(D&~(P&S)  */
    { OP(SRC,TMP,GXcopy), OP(SRC,DST,GXxor),
      OP(PAT,SRC,GXxor), OP(SRC,DST,GXand),
      OP(TMP,DST,GXequiv) },                         /* 0x17 ~S^((S^P)&(S^D))*/
    { OP(PAT,SRC,GXxor), OP(PAT,DST,GXxor),
        OP(SRC,DST,GXand) },                         /* 0x18  (S^P)&(D^P)    */
    { OP(SRC,TMP,GXcopy), OP(PAT,SRC,GXnand),
      OP(SRC,DST,GXand), OP(TMP,DST,GXequiv) },      /* 0x19  ~S^(D&~(P&S))  */
    { OP(PAT,SRC,GXand), OP(SRC,DST,GXor),
      OP(PAT,DST,GXxor) },                           /* 0x1a  P^(D|(S&P))    */
    { OP(SRC,TMP,GXcopy), OP(PAT,SRC,GXxor),
      OP(SRC,DST,GXand), OP(TMP,DST,GXequiv) },      /* 0x1b  ~S^(D&(P^S))   */
    { OP(PAT,DST,GXand), OP(SRC,DST,GXor),
      OP(PAT,DST,GXxor) },                           /* 0x1c  P^(S|(D&P))    */
    { OP(DST,TMP,GXcopy), OP(PAT,DST,GXxor),
      OP(SRC,DST,GXand), OP(TMP,DST,GXequiv) },      /* 0x1d  ~D^(S&(D^P))   */
    { OP(SRC,DST,GXor), OP(PAT,DST,GXxor) },         /* 0x1e  P^(D|S)        */
    { OP(SRC,DST,GXor), OP(PAT,DST,GXnand) },        /* 0x1f  ~(P&(D|S))     */
    { OP(PAT,SRC,GXandReverse), OP(SRC,DST,GXand) }, /* 0x20  D&(P&~S)       */
    { OP(PAT,DST,GXxor), OP(SRC,DST,GXnor) },        /* 0x21  ~(S|(D^P))     */
    { OP(SRC,DST,GXandInverted) },                   /* 0x22  ~S&D           */
    { OP(PAT,DST,GXandReverse), OP(SRC,DST,GXnor) }, /* 0x23  ~(S|(P&~D))    */
    { OP(SRC,DST,GXxor), OP(PAT,SRC,GXxor),
      OP(SRC,DST,GXand) },                           /* 0x24   (S^P)&(S^D)   */
    { OP(PAT,SRC,GXnand), OP(SRC,DST,GXand),
      OP(PAT,DST,GXequiv) },                         /* 0x25  ~P^(D&~(S&P))  */
    { OP(SRC,TMP,GXcopy), OP(PAT,SRC,GXand),
      OP(SRC,DST,GXor), OP(TMP,DST,GXxor) },         /* 0x26  S^(D|(S&P))    */
    { OP(SRC,TMP,GXcopy), OP(PAT,SRC,GXequiv),
      OP(SRC,DST,GXor), OP(TMP,DST,GXxor) },         /* 0x27  S^(D|~(P^S))   */
    { OP(PAT,SRC,GXxor), OP(SRC,DST,GXand) },        /* 0x28  D&(P^S)        */
    { OP(SRC,TMP,GXcopy), OP(PAT,SRC,GXand),
      OP(SRC,DST,GXor), OP(TMP,DST,GXxor),
      OP(PAT,DST,GXequiv) },                         /* 0x29  ~P^S^(D|(P&S)) */
    { OP(PAT,SRC,GXnand), OP(SRC,DST,GXand) },       /* 0x2a  D&~(P&S)       */
    { OP(SRC,TMP,GXcopy), OP(PAT,SRC,GXxor),
      OP(PAT,DST,GXxor), OP(SRC,DST,GXand),
      OP(TMP,DST,GXequiv) },                         /* 0x2b ~S^((P^S)&(P^D))*/
    { OP(SRC,DST,GXor), OP(PAT,DST,GXand),
      OP(SRC,DST,GXxor) },                           /* 0x2c  S^(P&(S|D))    */
    { OP(SRC,DST,GXorReverse), OP(PAT,DST,GXxor) },  /* 0x2d  P^(S|~D)       */
    { OP(PAT,DST,GXxor), OP(SRC,DST,GXor),
      OP(PAT,DST,GXxor) },                           /* 0x2e  P^(S|(D^P))    */
    { OP(SRC,DST,GXorReverse), OP(PAT,DST,GXnand) }, /* 0x2f  ~(P&(S|~D))    */
    { OP(PAT,SRC,GXandReverse) },                    /* 0x30  P&~S           */
    { OP(PAT,DST,GXandInverted), OP(SRC,DST,GXnor) },/* 0x31  ~(S|(D&~P))    */
    { OP(SRC,DST,GXor), OP(PAT,DST,GXor),
      OP(SRC,DST,GXxor) },                           /* 0x32  S^(D|P|S)      */
    { OP(SRC,DST,GXcopyInverted) },                  /* 0x33  ~S             */
    { OP(SRC,DST,GXand), OP(PAT,DST,GXor),
      OP(SRC,DST,GXxor) },                           /* 0x34  S^(P|(D&S))    */
    { OP(SRC,DST,GXequiv), OP(PAT,DST,GXor),
      OP(SRC,DST,GXxor) },                           /* 0x35  S^(P|~(D^S))   */
    { OP(PAT,DST,GXor), OP(SRC,DST,GXxor) },         /* 0x36  S^(D|P)        */
    { OP(PAT,DST,GXor), OP(SRC,DST,GXnand) },        /* 0x37  ~(S&(D|P))     */
    { OP(PAT,DST,GXor), OP(SRC,DST,GXand),
      OP(PAT,DST,GXxor) },                           /* 0x38  P^(S&(D|P))    */
    { OP(PAT,DST,GXorReverse), OP(SRC,DST,GXxor) },  /* 0x39  S^(P|~D)       */
    { OP(SRC,DST,GXxor), OP(PAT,DST,GXor),
      OP(SRC,DST,GXxor) },                           /* 0x3a  S^(P|(D^S))    */
    { OP(PAT,DST,GXorReverse), OP(SRC,DST,GXnand) }, /* 0x3b  ~(S&(P|~D))    */
    { OP(PAT,SRC,GXxor) },                           /* 0x3c  P^S            */
    { OP(SRC,DST,GXnor), OP(PAT,DST,GXor),
      OP(SRC,DST,GXxor) },                           /* 0x3d  S^(P|~(D|S))   */
    { OP(SRC,DST,GXandInverted), OP(PAT,DST,GXor),
      OP(SRC,DST,GXxor) },                           /* 0x3e  S^(P|(D&~S))   */
    { OP(PAT,SRC,GXnand) },                          /* 0x3f  ~(P&S)         */
    { OP(SRC,DST,GXandReverse), OP(PAT,DST,GXand) }, /* 0x40  P&S&~D         */
    { OP(PAT,SRC,GXxor), OP(SRC,DST,GXnor) },        /* 0x41  ~(D|(P^S))     */
    { OP(DST,SRC,GXxor), OP(PAT,DST,GXxor),
      OP(SRC,DST,GXand) },                           /* 0x42  (S^D)&(P^D)    */
    { OP(SRC,DST,GXnand), OP(PAT,DST,GXand),
      OP(SRC,DST,GXequiv) },                         /* 0x43  ~S^(P&~(D&S))  */
    { OP(SRC,DST,GXandReverse) },                    /* 0x44  S&~D           */
    { OP(PAT,SRC,GXandReverse), OP(SRC,DST,GXnor) }, /* 0x45  ~(D|(P&~S))    */
    { OP(DST,TMP,GXcopy), OP(PAT,DST,GXand),
      OP(SRC,DST,GXor), OP(TMP,DST,GXxor) },         /* 0x46  D^(S|(P&D))    */
    { OP(PAT,DST,GXxor), OP(SRC,DST,GXand),
      OP(PAT,DST,GXequiv) },                         /* 0x47  ~P^(S&(D^P))   */
    { OP(PAT,DST,GXxor), OP(SRC,DST,GXand) },        /* 0x48  S&(P^D)        */
    { OP(DST,TMP,GXcopy), OP(PAT,DST,GXand),
      OP(SRC,DST,GXor), OP(TMP,DST,GXxor),
      OP(PAT,DST,GXequiv) },                         /* 0x49  ~P^D^(S|(P&D)) */
    { OP(DST,SRC,GXor), OP(PAT,SRC,GXand),
      OP(SRC,DST,GXxor) },                           /* 0x4a  D^(P&(S|D))    */
    { OP(SRC,DST,GXorInverted), OP(PAT,DST,GXxor) }, /* 0x4b  P^(D|~S)       */
    { OP(PAT,DST,GXnand), OP(SRC,DST,GXand) },       /* 0x4c  S&~(D&P)       */
    { OP(SRC,TMP,GXcopy), OP(SRC,DST,GXxor),
      OP(PAT,SRC,GXxor), OP(SRC,DST,GXor),
      OP(TMP,DST,GXequiv) },                         /* 0x4d ~S^((S^P)|(S^D))*/
    { OP(PAT,SRC,GXxor), OP(SRC,DST,GXor),
      OP(PAT,DST,GXxor) },                           /* 0x4e  P^(D|(S^P))    */
    { OP(SRC,DST,GXorInverted), OP(PAT,DST,GXnand) },/* 0x4f  ~(P&(D|~S))    */
    { OP(PAT,DST,GXandReverse) },                    /* 0x50  P&~D           */
    { OP(PAT,SRC,GXandInverted), OP(SRC,DST,GXnor) },/* 0x51  ~(D|(S&~P))    */
    { OP(DST,SRC,GXand), OP(PAT,SRC,GXor),
      OP(SRC,DST,GXxor) },                           /* 0x52  D^(P|(S&D))    */
    { OP(SRC,DST,GXxor), OP(PAT,DST,GXand),
      OP(SRC,DST,GXequiv) },                         /* 0x53  ~S^(P&(D^S))   */
    { OP(PAT,SRC,GXnor), OP(SRC,DST,GXnor) },        /* 0x54  ~(D|~(P|S))    */
    { OP(PAT,DST,GXinvert) },                        /* 0x55  ~D             */
    { OP(PAT,SRC,GXor), OP(SRC,DST,GXxor) },         /* 0x56  D^(P|S)        */
    { OP(PAT,SRC,GXor), OP(SRC,DST,GXnand) },        /* 0x57  ~(D&(P|S))     */
    { OP(PAT,SRC,GXor), OP(SRC,DST,GXand),
      OP(PAT,DST,GXxor) },                           /* 0x58  P^(D&(P|S))    */
    { OP(PAT,SRC,GXorReverse), OP(SRC,DST,GXxor) },  /* 0x59  D^(P|~S)       */
    { OP(PAT,DST,GXxor) },                           /* 0x5a  D^P            */
    { OP(DST,SRC,GXnor), OP(PAT,SRC,GXor),
      OP(SRC,DST,GXxor) },                           /* 0x5b  D^(P|~(S|D))   */
    { OP(DST,SRC,GXxor), OP(PAT,SRC,GXor),
      OP(SRC,DST,GXxor) },                           /* 0x5c  D^(P|(S^D))    */
    { OP(PAT,SRC,GXorReverse), OP(SRC,DST,GXnand) }, /* 0x5d  ~(D&(P|~S))    */
    { OP(DST,SRC,GXandInverted), OP(PAT,SRC,GXor),
      OP(SRC,DST,GXxor) },                           /* 0x5e  D^(P|(S&~D))   */
    { OP(PAT,DST,GXnand) },                          /* 0x5f  ~(D&P)         */
    { OP(SRC,DST,GXxor), OP(PAT,DST,GXand) },        /* 0x60  P&(D^S)        */
    { OP(DST,TMP,GXcopy), OP(SRC,DST,GXand),
      OP(PAT,DST,GXor), OP(SRC,DST,GXxor),
      OP(TMP,DST,GXequiv) },                         /* 0x61  ~D^S^(P|(D&S)) */
    { OP(DST,TMP,GXcopy), OP(PAT,DST,GXor),
      OP(SRC,DST,GXand), OP(TMP,DST,GXxor) },        /* 0x62  D^(S&(P|D))    */
    { OP(PAT,DST,GXorInverted), OP(SRC,DST,GXxor) }, /* 0x63  S^(D|~P)       */
    { OP(SRC,TMP,GXcopy), OP(PAT,SRC,GXor),
      OP(SRC,DST,GXand), OP(TMP,DST,GXxor) },        /* 0x64  S^(D&(P|S))    */
    { OP(PAT,SRC,GXorInverted), OP(SRC,DST,GXxor) }, /* 0x65  D^(S|~P)       */
    { OP(SRC,DST,GXxor) },                           /* 0x66  S^D            */
    { OP(SRC,TMP,GXcopy), OP(PAT,SRC,GXnor),
      OP(SRC,DST,GXor), OP(TMP,DST,GXxor) },         /* 0x67  S^(D|~(S|P)    */
    { OP(DST,TMP,GXcopy), OP(SRC,DST,GXnor),
      OP(PAT,DST,GXor), OP(SRC,DST,GXxor),
      OP(TMP,DST,GXequiv) },                         /* 0x68  ~D^S^(P|~(D|S))*/
    { OP(SRC,DST,GXxor), OP(PAT,DST,GXequiv) },      /* 0x69  ~P^(D^S)       */
    { OP(PAT,SRC,GXand), OP(SRC,DST,GXxor) },        /* 0x6a  D^(P&S)        */
    { OP(SRC,TMP,GXcopy), OP(PAT,SRC,GXor),
      OP(SRC,DST,GXand), OP(TMP,DST,GXxor),
      OP(PAT,DST,GXequiv) },                         /* 0x6b  ~P^S^(D&(P|S)) */
    { OP(PAT,DST,GXand), OP(SRC,DST,GXxor) },        /* 0x6c  S^(D&P)        */
    { OP(DST,TMP,GXcopy), OP(PAT,DST,GXor),
      OP(SRC,DST,GXand), OP(TMP,DST,GXxor),
      OP(PAT,DST,GXequiv) },                         /* 0x6d  ~P^D^(S&(P|D)) */
    { OP(SRC,TMP,GXcopy), OP(PAT,SRC,GXorReverse),
      OP(SRC,DST,GXand), OP(TMP,DST,GXxor) },        /* 0x6e  S^(D&(P|~S))   */
    { OP(SRC,DST,GXequiv), OP(PAT,DST,GXnand) },     /* 0x6f  ~(P&~(S^D))    */
    { OP(SRC,DST,GXnand), OP(PAT,DST,GXand) },       /* 0x70  P&~(D&S)       */
    { OP(SRC,TMP,GXcopy), OP(DST,SRC,GXxor),
      OP(PAT,DST,GXxor), OP(SRC,DST,GXand),
      OP(TMP,DST,GXequiv) },                         /* 0x71 ~S^((S^D)&(P^D))*/
    { OP(SRC,TMP,GXcopy), OP(PAT,SRC,GXxor),
      OP(SRC,DST,GXor), OP(TMP,DST,GXxor) },         /* 0x72  S^(D|(P^S))    */
    { OP(PAT,DST,GXorInverted), OP(SRC,DST,GXnand) },/* 0x73  ~(S&(D|~P))    */
    { OP(DST,TMP,GXcopy), OP(PAT,DST,GXxor),
      OP(SRC,DST,GXor), OP(TMP,DST,GXxor) },         /* 0x74   D^(S|(P^D))   */
    { OP(PAT,SRC,GXorInverted), OP(SRC,DST,GXnand) },/* 0x75  ~(D&(S|~P))    */
    { OP(SRC,TMP,GXcopy), OP(PAT,SRC,GXandReverse),
      OP(SRC,DST,GXor), OP(TMP,DST,GXxor) },         /* 0x76  S^(D|(P&~S))   */
    { OP(SRC,DST,GXnand) },                          /* 0x77  ~(S&D)         */
    { OP(SRC,DST,GXand), OP(PAT,DST,GXxor) },        /* 0x78  P^(D&S)        */
    { OP(DST,TMP,GXcopy), OP(SRC,DST,GXor),
      OP(PAT,DST,GXand), OP(SRC,DST,GXxor),
      OP(TMP,DST,GXequiv) },                         /* 0x79  ~D^S^(P&(D|S)) */
    { OP(DST,SRC,GXorInverted), OP(PAT,SRC,GXand),
      OP(SRC,DST,GXxor) },                           /* 0x7a  D^(P&(S|~D))   */
    { OP(PAT,DST,GXequiv), OP(SRC,DST,GXnand) },     /* 0x7b  ~(S&~(D^P))    */
    { OP(SRC,DST,GXorInverted), OP(PAT,DST,GXand),
      OP(SRC,DST,GXxor) },                           /* 0x7c  S^(P&(D|~S))   */
    { OP(PAT,SRC,GXequiv), OP(SRC,DST,GXnand) },     /* 0x7d  ~(D&~(P^S))    */
    { OP(SRC,DST,GXxor), OP(PAT,SRC,GXxor),
      OP(SRC,DST,GXor) },                            /* 0x7e  (S^P)|(S^D)    */
    { OP(PAT,SRC,GXand), OP(SRC,DST,GXnand) },       /* 0x7f  ~(D&P&S)       */
    { OP(PAT,SRC,GXand), OP(SRC,DST,GXand) },        /* 0x80  D&P&S          */
    { OP(SRC,DST,GXxor), OP(PAT,SRC,GXxor),
      OP(SRC,DST,GXnor) },                           /* 0x81  ~((S^P)|(S^D)) */
    { OP(PAT,SRC,GXequiv), OP(SRC,DST,GXand) },      /* 0x82  D&~(P^S)       */
    { OP(SRC,DST,GXorInverted), OP(PAT,DST,GXand),
      OP(SRC,DST,GXequiv) },                         /* 0x83  ~S^(P&(D|~S))  */
    { OP(PAT,DST,GXequiv), OP(SRC,DST,GXand) },      /* 0x84  S&~(D^P)       */
    { OP(PAT,SRC,GXorInverted), OP(SRC,DST,GXand),
      OP(PAT,DST,GXequiv) },                         /* 0x85  ~P^(D&(S|~P))  */
    { OP(DST,TMP,GXcopy), OP(SRC,DST,GXor),
      OP(PAT,DST,GXand), OP(SRC,DST,GXxor),
      OP(TMP,DST,GXxor) },                           /* 0x86  D^S^(P&(D|S))  */
    { OP(SRC,DST,GXand), OP(PAT,DST,GXequiv) },      /* 0x87  ~P^(D&S)       */
    { OP(SRC,DST,GXand) },                           /* 0x88  S&D            */
    { OP(SRC,TMP,GXcopy), OP(PAT,SRC,GXandReverse),
      OP(SRC,DST,GXor), OP(TMP,DST,GXequiv) },       /* 0x89  ~S^(D|(P&~S))  */
    { OP(PAT,SRC,GXorInverted), OP(SRC,DST,GXand) }, /* 0x8a  D&(S|~P)       */
    { OP(DST,TMP,GXcopy), OP(PAT,DST,GXxor),
      OP(SRC,DST,GXor), OP(TMP,DST,GXequiv) },       /* 0x8b  ~D^(S|(P^D))   */
    { OP(PAT,DST,GXorInverted), OP(SRC,DST,GXand) }, /* 0x8c  S&(D|~P)       */
    { OP(SRC,TMP,GXcopy), OP(PAT,SRC,GXxor),
      OP(SRC,DST,GXor), OP(TMP,DST,GXequiv) },       /* 0x8d  ~S^(D|(P^S))   */
    { OP(SRC,TMP,GXcopy), OP(DST,SRC,GXxor),
      OP(PAT,DST,GXxor), OP(SRC,DST,GXand),
      OP(TMP,DST,GXxor) },                           /* 0x8e  S^((S^D)&(P^D))*/
    { OP(SRC,DST,GXnand), OP(PAT,DST,GXnand) },      /* 0x8f  ~(P&~(D&S))    */
    { OP(SRC,DST,GXequiv), OP(PAT,DST,GXand) },      /* 0x90  P&~(D^S)       */
    { OP(SRC,TMP,GXcopy), OP(PAT,SRC,GXorReverse),
      OP(SRC,DST,GXand), OP(TMP,DST,GXequiv) },      /* 0x91  ~S^(D&(P|~S))  */
    { OP(DST,TMP,GXcopy), OP(PAT,DST,GXor),
      OP(SRC,DST,GXand), OP(PAT,DST,GXxor),
      OP(TMP,DST,GXxor) },                           /* 0x92  D^P^(S&(D|P))  */
    { OP(PAT,DST,GXand), OP(SRC,DST,GXequiv) },      /* 0x93  ~S^(P&D)       */
    { OP(SRC,TMP,GXcopy), OP(PAT,SRC,GXor),
      OP(SRC,DST,GXand), OP(PAT,DST,GXxor),
      OP(TMP,DST,GXxor) },                           /* 0x94  S^P^(D&(P|S))  */
    { OP(PAT,SRC,GXand), OP(SRC,DST,GXequiv) },      /* 0x95  ~D^(P&S)       */
    { OP(PAT,SRC,GXxor), OP(SRC,DST,GXxor) },        /* 0x96  D^P^S          */
    { OP(SRC,TMP,GXcopy), OP(PAT,SRC,GXnor),
      OP(SRC,DST,GXor), OP(PAT,DST,GXxor),
      OP(TMP,DST,GXxor) },                           /* 0x97  S^P^(D|~(P|S)) */
    { OP(SRC,TMP,GXcopy), OP(PAT,SRC,GXnor),
      OP(SRC,DST,GXor), OP(TMP,DST,GXequiv) },       /* 0x98  ~S^(D|~(P|S))  */
    { OP(SRC,DST,GXequiv) },                         /* 0x99  ~S^D           */
    { OP(PAT,SRC,GXandReverse), OP(SRC,DST,GXxor) }, /* 0x9a  D^(P&~S)       */
    { OP(SRC,TMP,GXcopy), OP(PAT,SRC,GXor),
      OP(SRC,DST,GXand), OP(TMP,DST,GXequiv) },      /* 0x9b  ~S^(D&(P|S))   */
    { OP(PAT,DST,GXandReverse), OP(SRC,DST,GXxor) }, /* 0x9c  S^(P&~D)       */
    { OP(DST,TMP,GXcopy), OP(PAT,DST,GXor),
      OP(SRC,DST,GXand), OP(TMP,DST,GXequiv) },      /* 0x9d  ~D^(S&(P|D))   */
    { OP(DST,TMP,GXcopy), OP(SRC,DST,GXand),
      OP(PAT,DST,GXor), OP(SRC,DST,GXxor),
      OP(TMP,DST,GXxor) },                           /* 0x9e  D^S^(P|(D&S))  */
    { OP(SRC,DST,GXxor), OP(PAT,DST,GXnand) },       /* 0x9f  ~(P&(D^S))     */
    { OP(PAT,DST,GXand) },                           /* 0xa0  D&P            */
    { OP(PAT,SRC,GXandInverted), OP(SRC,DST,GXor),
      OP(PAT,DST,GXequiv) },                         /* 0xa1  ~P^(D|(S&~P))  */
    { OP(PAT,SRC,GXorReverse), OP(SRC,DST,GXand) },  /* 0xa2  D&(P|~S)       */
    { OP(DST,SRC,GXxor), OP(PAT,SRC,GXor),
      OP(SRC,DST,GXequiv) },                         /* 0xa3  ~D^(P|(S^D))   */
    { OP(PAT,SRC,GXnor), OP(SRC,DST,GXor),
      OP(PAT,DST,GXequiv) },                         /* 0xa4  ~P^(D|~(S|P))  */
    { OP(PAT,DST,GXequiv) },                         /* 0xa5  ~P^D           */
    { OP(PAT,SRC,GXandInverted), OP(SRC,DST,GXxor) },/* 0xa6  D^(S&~P)       */
    { OP(PAT,SRC,GXor), OP(SRC,DST,GXand),
      OP(PAT,DST,GXequiv) },                         /* 0xa7  ~P^(D&(S|P))   */
    { OP(PAT,SRC,GXor), OP(SRC,DST,GXand) },         /* 0xa8  D&(P|S)        */
    { OP(PAT,SRC,GXor), OP(SRC,DST,GXequiv) },       /* 0xa9  ~D^(P|S)       */
    { OP(PAT,DST,GXnoop) },                          /* 0xaa  D              */
    { OP(PAT,SRC,GXnor), OP(SRC,DST,GXor) },         /* 0xab  D|~(P|S)       */
    { OP(SRC,DST,GXxor), OP(PAT,DST,GXand),
      OP(SRC,DST,GXxor) },                           /* 0xac  S^(P&(D^S))    */
    { OP(DST,SRC,GXand), OP(PAT,SRC,GXor),
      OP(SRC,DST,GXequiv) },                         /* 0xad  ~D^(P|(S&D))   */
    { OP(PAT,SRC,GXandInverted), OP(SRC,DST,GXor) }, /* 0xae  D|(S&~P)       */
    { OP(PAT,DST,GXorInverted) },                    /* 0xaf  D|~P           */
    { OP(SRC,DST,GXorInverted), OP(PAT,DST,GXand) }, /* 0xb0  P&(D|~S)       */
    { OP(PAT,SRC,GXxor), OP(SRC,DST,GXor),
      OP(PAT,DST,GXequiv) },                         /* 0xb1  ~P^(D|(S^P))   */
    { OP(SRC,TMP,GXcopy), OP(SRC,DST,GXxor),
      OP(PAT,SRC,GXxor), OP(SRC,DST,GXor),
      OP(TMP,DST,GXxor) },                           /* 0xb2  S^((S^P)|(S^D))*/
    { OP(PAT,DST,GXnand), OP(SRC,DST,GXnand) },      /* 0xb3  ~(S&~(D&P))    */
    { OP(SRC,DST,GXandReverse), OP(PAT,DST,GXxor) }, /* 0xb4  P^(S&~D)       */
    { OP(DST,SRC,GXor), OP(PAT,SRC,GXand),
      OP(SRC,DST,GXequiv) },                         /* 0xb5  ~D^(P&(S|D))   */
    { OP(DST,TMP,GXcopy), OP(PAT,DST,GXand),
      OP(SRC,DST,GXor), OP(PAT,DST,GXxor),
      OP(TMP,DST,GXxor) },                           /* 0xb6  D^P^(S|(D&P))  */
    { OP(PAT,DST,GXxor), OP(SRC,DST,GXnand) },       /* 0xb7  ~(S&(D^P))     */
    { OP(PAT,DST,GXxor), OP(SRC,DST,GXand),
      OP(PAT,DST,GXxor) },                           /* 0xb8  P^(S&(D^P))    */
    { OP(DST,TMP,GXcopy), OP(PAT,DST,GXand),
      OP(SRC,DST,GXor), OP(TMP,DST,GXequiv) },       /* 0xb9  ~D^(S|(P&D))   */
    { OP(PAT,SRC,GXandReverse), OP(SRC,DST,GXor) },  /* 0xba  D|(P&~S)       */
    { OP(SRC,DST,GXorInverted) },                    /* 0xbb  ~S|D           */
    { OP(SRC,DST,GXnand), OP(PAT,DST,GXand),
      OP(SRC,DST,GXxor) },                           /* 0xbc  S^(P&~(D&S))   */
    { OP(DST,SRC,GXxor), OP(PAT,DST,GXxor),
      OP(SRC,DST,GXnand) },                          /* 0xbd  ~((S^D)&(P^D)) */
    { OP(PAT,SRC,GXxor), OP(SRC,DST,GXor) },         /* 0xbe  D|(P^S)        */
    { OP(PAT,SRC,GXnand), OP(SRC,DST,GXor) },        /* 0xbf  D|~(P&S)       */
    { OP(PAT,SRC,GXand) },                           /* 0xc0  P&S            */
    { OP(SRC,DST,GXandInverted), OP(PAT,DST,GXor),
      OP(SRC,DST,GXequiv) },                         /* 0xc1  ~S^(P|(D&~S))  */
    { OP(SRC,DST,GXnor), OP(PAT,DST,GXor),
      OP(SRC,DST,GXequiv) },                         /* 0xc2  ~S^(P|~(D|S))  */
    { OP(PAT,SRC,GXequiv) },                         /* 0xc3  ~P^S           */
    { OP(PAT,DST,GXorReverse), OP(SRC,DST,GXand) },  /* 0xc4  S&(P|~D)       */
    { OP(SRC,DST,GXxor), OP(PAT,DST,GXor),
      OP(SRC,DST,GXequiv) },                         /* 0xc5  ~S^(P|(D^S))   */
    { OP(PAT,DST,GXandInverted), OP(SRC,DST,GXxor) },/* 0xc6  S^(D&~P)       */
    { OP(PAT,DST,GXor), OP(SRC,DST,GXand),
      OP(PAT,DST,GXequiv) },                         /* 0xc7  ~P^(S&(D|P))   */
    { OP(PAT,DST,GXor), OP(SRC,DST,GXand) },         /* 0xc8  S&(D|P)        */
    { OP(PAT,DST,GXor), OP(SRC,DST,GXequiv) },       /* 0xc9  ~S^(P|D)       */
    { OP(DST,SRC,GXxor), OP(PAT,SRC,GXand),
      OP(SRC,DST,GXxor) },                           /* 0xca  D^(P&(S^D))    */
    { OP(SRC,DST,GXand), OP(PAT,DST,GXor),
      OP(SRC,DST,GXequiv) },                         /* 0xcb  ~S^(P|(D&S))   */
    { OP(SRC,DST,GXcopy) },                          /* 0xcc  S              */
    { OP(PAT,DST,GXnor), OP(SRC,DST,GXor) },         /* 0xcd  S|~(D|P)       */
    { OP(PAT,DST,GXandInverted), OP(SRC,DST,GXor) }, /* 0xce  S|(D&~P)       */
    { OP(PAT,SRC,GXorInverted) },                    /* 0xcf  S|~P           */
    { OP(SRC,DST,GXorReverse), OP(PAT,DST,GXand) },  /* 0xd0  P&(S|~D)       */
    { OP(PAT,DST,GXxor), OP(SRC,DST,GXor),
      OP(PAT,DST,GXequiv) },                         /* 0xd1  ~P^(S|(D^P))   */
    { OP(SRC,DST,GXandInverted), OP(PAT,DST,GXxor) },/* 0xd2  P^(D&~S)       */
    { OP(SRC,DST,GXor), OP(PAT,DST,GXand),
      OP(SRC,DST,GXequiv) },                         /* 0xd3  ~S^(P&(D|S))   */
    { OP(SRC,TMP,GXcopy), OP(PAT,SRC,GXxor),
      OP(PAT,DST,GXxor), OP(SRC,DST,GXand),
      OP(TMP,DST,GXxor) },                           /* 0xd4  S^((S^P)&(D^P))*/
    { OP(PAT,SRC,GXnand), OP(SRC,DST,GXnand) },      /* 0xd5  ~(D&~(P&S))    */
    { OP(SRC,TMP,GXcopy), OP(PAT,SRC,GXand),
      OP(SRC,DST,GXor), OP(PAT,DST,GXxor),
      OP(TMP,DST,GXxor) },                           /* 0xd6  S^P^(D|(P&S))  */
    { OP(PAT,SRC,GXxor), OP(SRC,DST,GXnand) },       /* 0xd7  ~(D&(P^S))     */
    { OP(PAT,SRC,GXxor), OP(SRC,DST,GXand),
      OP(PAT,DST,GXxor) },                           /* 0xd8  P^(D&(S^P))    */
    { OP(SRC,TMP,GXcopy), OP(PAT,SRC,GXand),
      OP(SRC,DST,GXor), OP(TMP,DST,GXequiv) },       /* 0xd9  ~S^(D|(P&S))   */
    { OP(DST,SRC,GXnand), OP(PAT,SRC,GXand),
      OP(SRC,DST,GXxor) },                           /* 0xda  D^(P&~(S&D))   */
    { OP(SRC,DST,GXxor), OP(PAT,SRC,GXxor),
      OP(SRC,DST,GXnand) },                          /* 0xdb  ~((S^P)&(S^D)) */
    { OP(PAT,DST,GXandReverse), OP(SRC,DST,GXor) },  /* 0xdc  S|(P&~D)       */
    { OP(SRC,DST,GXorReverse) },                     /* 0xdd  S|~D           */
    { OP(PAT,DST,GXxor), OP(SRC,DST,GXor) },         /* 0xde  S|(D^P)        */
    { OP(PAT,DST,GXnand), OP(SRC,DST,GXor) },        /* 0xdf  S|~(D&P)       */
    { OP(SRC,DST,GXor), OP(PAT,DST,GXand) },         /* 0xe0  P&(D|S)        */
    { OP(SRC,DST,GXor), OP(PAT,DST,GXequiv) },       /* 0xe1  ~P^(D|S)       */
    { OP(DST,TMP,GXcopy), OP(PAT,DST,GXxor),
      OP(SRC,DST,GXand), OP(TMP,DST,GXxor) },        /* 0xe2  D^(S&(P^D))    */
    { OP(PAT,DST,GXand), OP(SRC,DST,GXor),
      OP(PAT,DST,GXequiv) },                         /* 0xe3  ~P^(S|(D&P))   */
    { OP(SRC,TMP,GXcopy), OP(PAT,SRC,GXxor),
      OP(SRC,DST,GXand), OP(TMP,DST,GXxor) },        /* 0xe4  S^(D&(P^S))    */
    { OP(PAT,SRC,GXand), OP(SRC,DST,GXor),
      OP(PAT,DST,GXequiv) },                         /* 0xe5  ~P^(D|(S&P))   */
    { OP(SRC,TMP,GXcopy), OP(PAT,SRC,GXnand),
      OP(SRC,DST,GXand), OP(TMP,DST,GXxor) },        /* 0xe6  S^(D&~(P&S))   */
    { OP(PAT,SRC,GXxor), OP(PAT,DST,GXxor),
      OP(SRC,DST,GXnand) },                          /* 0xe7  ~((S^P)&(D^P)) */
    { OP(SRC,TMP,GXcopy), OP(SRC,DST,GXxor),
      OP(PAT,SRC,GXxor), OP(SRC,DST,GXand),
      OP(TMP,DST,GXxor) },                           /* 0xe8  S^((S^P)&(S^D))*/
    { OP(DST,TMP,GXcopy), OP(SRC,DST,GXnand),
      OP(PAT,DST,GXand), OP(SRC,DST,GXxor),
      OP(TMP,DST,GXequiv) },                         /* 0xe9  ~D^S^(P&~(S&D))*/
    { OP(PAT,SRC,GXand), OP(SRC,DST,GXor) },         /* 0xea  D|(P&S)        */
    { OP(PAT,SRC,GXequiv), OP(SRC,DST,GXor) },       /* 0xeb  D|~(P^S)       */
    { OP(PAT,DST,GXand), OP(SRC,DST,GXor) },         /* 0xec  S|(D&P)        */
    { OP(PAT,DST,GXequiv), OP(SRC,DST,GXor) },       /* 0xed  S|~(D^P)       */
    { OP(SRC,DST,GXor) },                            /* 0xee  S|D            */
    { OP(PAT,DST,GXorInverted), OP(SRC,DST,GXor) },  /* 0xef  S|D|~P         */
    { OP(PAT,DST,GXcopy) },                          /* 0xf0  P              */
    { OP(SRC,DST,GXnor), OP(PAT,DST,GXor) },         /* 0xf1  P|~(D|S)       */
    { OP(SRC,DST,GXandInverted), OP(PAT,DST,GXor) }, /* 0xf2  P|(D&~S)       */
    { OP(PAT,SRC,GXorReverse) },                     /* 0xf3  P|~S           */
    { OP(SRC,DST,GXandReverse), OP(PAT,DST,GXor) },  /* 0xf4  P|(S&~D)       */
    { OP(PAT,DST,GXorReverse) },                     /* 0xf5  P|~D           */
    { OP(SRC,DST,GXxor), OP(PAT,DST,GXor) },         /* 0xf6  P|(D^S)        */
    { OP(SRC,DST,GXnand), OP(PAT,DST,GXor) },        /* 0xf7  P|~(S&D)       */
    { OP(SRC,DST,GXand), OP(PAT,DST,GXor) },         /* 0xf8  P|(D&S)        */
    { OP(SRC,DST,GXequiv), OP(PAT,DST,GXor) },       /* 0xf9  P|~(D^S)       */
    { OP(PAT,DST,GXor) },                            /* 0xfa  D|P            */
    { OP(PAT,SRC,GXorReverse), OP(SRC,DST,GXor) },   /* 0xfb  D|P|~S         */
    { OP(PAT,SRC,GXor) },                            /* 0xfc  P|S            */
    { OP(SRC,DST,GXorReverse), OP(PAT,DST,GXor) },   /* 0xfd  P|S|~D         */
    { OP(SRC,DST,GXor), OP(PAT,DST,GXor) },          /* 0xfe  P|D|S          */
    { OP(PAT,DST,GXset) }                            /* 0xff  1              */
};

static const unsigned char bit_swap[256] =
{
    0x00, 0x80, 0x40, 0xc0, 0x20, 0xa0, 0x60, 0xe0,
    0x10, 0x90, 0x50, 0xd0, 0x30, 0xb0, 0x70, 0xf0,
    0x08, 0x88, 0x48, 0xc8, 0x28, 0xa8, 0x68, 0xe8,
    0x18, 0x98, 0x58, 0xd8, 0x38, 0xb8, 0x78, 0xf8,
    0x04, 0x84, 0x44, 0xc4, 0x24, 0xa4, 0x64, 0xe4,
    0x14, 0x94, 0x54, 0xd4, 0x34, 0xb4, 0x74, 0xf4,
    0x0c, 0x8c, 0x4c, 0xcc, 0x2c, 0xac, 0x6c, 0xec,
    0x1c, 0x9c, 0x5c, 0xdc, 0x3c, 0xbc, 0x7c, 0xfc,
    0x02, 0x82, 0x42, 0xc2, 0x22, 0xa2, 0x62, 0xe2,
    0x12, 0x92, 0x52, 0xd2, 0x32, 0xb2, 0x72, 0xf2,
    0x0a, 0x8a, 0x4a, 0xca, 0x2a, 0xaa, 0x6a, 0xea,
    0x1a, 0x9a, 0x5a, 0xda, 0x3a, 0xba, 0x7a, 0xfa,
    0x06, 0x86, 0x46, 0xc6, 0x26, 0xa6, 0x66, 0xe6,
    0x16, 0x96, 0x56, 0xd6, 0x36, 0xb6, 0x76, 0xf6,
    0x0e, 0x8e, 0x4e, 0xce, 0x2e, 0xae, 0x6e, 0xee,
    0x1e, 0x9e, 0x5e, 0xde, 0x3e, 0xbe, 0x7e, 0xfe,
    0x01, 0x81, 0x41, 0xc1, 0x21, 0xa1, 0x61, 0xe1,
    0x11, 0x91, 0x51, 0xd1, 0x31, 0xb1, 0x71, 0xf1,
    0x09, 0x89, 0x49, 0xc9, 0x29, 0xa9, 0x69, 0xe9,
    0x19, 0x99, 0x59, 0xd9, 0x39, 0xb9, 0x79, 0xf9,
    0x05, 0x85, 0x45, 0xc5, 0x25, 0xa5, 0x65, 0xe5,
    0x15, 0x95, 0x55, 0xd5, 0x35, 0xb5, 0x75, 0xf5,
    0x0d, 0x8d, 0x4d, 0xcd, 0x2d, 0xad, 0x6d, 0xed,
    0x1d, 0x9d, 0x5d, 0xdd, 0x3d, 0xbd, 0x7d, 0xfd,
    0x03, 0x83, 0x43, 0xc3, 0x23, 0xa3, 0x63, 0xe3,
    0x13, 0x93, 0x53, 0xd3, 0x33, 0xb3, 0x73, 0xf3,
    0x0b, 0x8b, 0x4b, 0xcb, 0x2b, 0xab, 0x6b, 0xeb,
    0x1b, 0x9b, 0x5b, 0xdb, 0x3b, 0xbb, 0x7b, 0xfb,
    0x07, 0x87, 0x47, 0xc7, 0x27, 0xa7, 0x67, 0xe7,
    0x17, 0x97, 0x57, 0xd7, 0x37, 0xb7, 0x77, 0xf7,
    0x0f, 0x8f, 0x4f, 0xcf, 0x2f, 0xaf, 0x6f, 0xef,
    0x1f, 0x9f, 0x5f, 0xdf, 0x3f, 0xbf, 0x7f, 0xff
};

#ifdef WORDS_BIGENDIAN
static const unsigned int zeropad_masks[32] =
{
    0xffffffff, 0x80000000, 0xc0000000, 0xe0000000, 0xf0000000, 0xf8000000, 0xfc000000, 0xfe000000,
    0xff000000, 0xff800000, 0xffc00000, 0xffe00000, 0xfff00000, 0xfff80000, 0xfffc0000, 0xfffe0000,
    0xffff0000, 0xffff8000, 0xffffc000, 0xffffe000, 0xfffff000, 0xfffff800, 0xfffffc00, 0xfffffe00,
    0xffffff00, 0xffffff80, 0xffffffc0, 0xffffffe0, 0xfffffff0, 0xfffffff8, 0xfffffffc, 0xfffffffe
};
#else
static const unsigned int zeropad_masks[32] =
{
    0xffffffff, 0x00000080, 0x000000c0, 0x000000e0, 0x000000f0, 0x000000f8, 0x000000fc, 0x000000fe,
    0x000000ff, 0x000080ff, 0x0000c0ff, 0x0000e0ff, 0x0000f0ff, 0x0000f8ff, 0x0000fcff, 0x0000feff,
    0x0000ffff, 0x0080ffff, 0x00c0ffff, 0x00e0ffff, 0x00f0ffff, 0x00f8ffff, 0x00fcffff, 0x00feffff,
    0x00ffffff, 0x80ffffff, 0xc0ffffff, 0xe0ffffff, 0xf0ffffff, 0xf8ffffff, 0xfcffffff, 0xfeffffff
};
#endif

#ifdef BITBLT_TEST  /* Opcodes test */

static int do_bitop( int s, int d, int rop )
{
    int res;
    switch(rop)
    {
    case GXclear:        res = 0; break;
    case GXand:          res = s & d; break;
    case GXandReverse:   res = s & ~d; break;
    case GXcopy:         res = s; break;
    case GXandInverted:  res = ~s & d; break;
    case GXnoop:         res = d; break;
    case GXxor:          res = s ^ d; break;
    case GXor:           res = s | d; break;
    case GXnor:          res = ~(s | d); break;
    case GXequiv:        res = ~s ^ d; break;
    case GXinvert:       res = ~d; break;
    case GXorReverse:    res = s | ~d; break;
    case GXcopyInverted: res = ~s; break;
    case GXorInverted:   res = ~s | d; break;
    case GXnand:         res = ~(s & d); break;
    case GXset:          res = 1; break;
    }
    return res & 1;
}

int main()
{
    int rop, i, res, src, dst, pat, tmp, dstUsed;
    const unsigned char *opcode;

    for (rop = 0; rop < 256; rop++)
    {
        res = dstUsed = 0;
        for (i = 0; i < 8; i++)
        {
            pat = (i >> 2) & 1;
            src = (i >> 1) & 1;
            dst = i & 1;
            for (opcode = BITBLT_Opcodes[rop]; *opcode; opcode++)
            {
                switch(*opcode >> 4)
                {
                case OP_ARGS(DST,TMP):
                    tmp = do_bitop( dst, tmp, *opcode & 0xf );
                    break;
                case OP_ARGS(DST,SRC):
                    src = do_bitop( dst, src, *opcode & 0xf );
                    break;
                case OP_ARGS(SRC,TMP):
                    tmp = do_bitop( src, tmp, *opcode & 0xf );
                    break;
                case OP_ARGS(SRC,DST):
                    dst = do_bitop( src, dst, *opcode & 0xf );
                    dstUsed = 1;
                    break;
                case OP_ARGS(PAT,DST):
                    dst = do_bitop( pat, dst, *opcode & 0xf );
                    dstUsed = 1;
                    break;
                case OP_ARGS(PAT,SRC):
                    src = do_bitop( pat, src, *opcode & 0xf );
                    break;
                case OP_ARGS(TMP,DST):
                    dst = do_bitop( tmp, dst, *opcode & 0xf );
                    dstUsed = 1;
                    break;
                case OP_ARGS(TMP,SRC):
                    src = do_bitop( tmp, src, *opcode & 0xf );
                    break;
                default:
                    printf( "Invalid opcode %x\n", *opcode );
                }
            }
            if (!dstUsed) dst = src;
            if (dst) res |= 1 << i;
        }
        if (res != rop) printf( "%02x: ERROR, res=%02x\n", rop, res );
    }

    return 0;
}

#endif  /* BITBLT_TEST */


static void get_colors(X11DRV_PDEVICE *physDevDst, X11DRV_PDEVICE *physDevSrc,
		       int *fg, int *bg)
{
    RGBQUAD rgb[2];

    *fg = physDevDst->textPixel;
    *bg = physDevDst->backgroundPixel;
    if(physDevSrc->depth == 1) {
        if(GetDIBColorTable(physDevSrc->dev.hdc, 0, 2, rgb) == 2) {
            DWORD logcolor;
            logcolor = RGB(rgb[0].rgbRed, rgb[0].rgbGreen, rgb[0].rgbBlue);
            *fg = X11DRV_PALETTE_ToPhysical( physDevDst, logcolor );
            logcolor = RGB(rgb[1].rgbRed, rgb[1].rgbGreen,rgb[1].rgbBlue);
            *bg = X11DRV_PALETTE_ToPhysical( physDevDst, logcolor );
        }
    }
}

/* return a mask for meaningful bits when doing an XGetPixel on an image */
static unsigned long image_pixel_mask( X11DRV_PDEVICE *physDev )
{
    unsigned long ret;
    ColorShifts *shifts = physDev->color_shifts;

    if (!shifts) shifts = &X11DRV_PALETTE_default_shifts;
    ret = (shifts->physicalRed.max << shifts->physicalRed.shift) |
        (shifts->physicalGreen.max << shifts->physicalGreen.shift) |
        (shifts->physicalBlue.max << shifts->physicalBlue.shift);
    if (!ret) ret = (1 << physDev->depth) - 1;
    return ret;
}


/***********************************************************************
 *           BITBLT_StretchRow
 *
 * Stretch a row of pixels. Helper function for BITBLT_StretchImage.
 */
static void BITBLT_StretchRow( int *rowSrc, int *rowDst,
                               INT startDst, INT widthDst,
                               INT xinc, INT xoff, WORD mode )
{
    register INT xsrc = xinc * startDst + xoff;
    rowDst += startDst;
    switch(mode)
    {
    case STRETCH_ANDSCANS:
        for(; widthDst > 0; widthDst--, xsrc += xinc)
            *rowDst++ &= rowSrc[xsrc >> 16];
        break;
    case STRETCH_ORSCANS:
        for(; widthDst > 0; widthDst--, xsrc += xinc)
            *rowDst++ |= rowSrc[xsrc >> 16];
        break;
    case STRETCH_DELETESCANS:
        for(; widthDst > 0; widthDst--, xsrc += xinc)
            *rowDst++ = rowSrc[xsrc >> 16];
        break;
    }
}


/***********************************************************************
 *           BITBLT_ShrinkRow
 *
 * Shrink a row of pixels. Helper function for BITBLT_StretchImage.
 */
static void BITBLT_ShrinkRow( int *rowSrc, int *rowDst,
                              INT startSrc, INT widthSrc,
                              INT xinc, INT xoff, WORD mode )
{
    register INT xdst = xinc * startSrc + xoff;
    rowSrc += startSrc;
    switch(mode)
    {
    case STRETCH_ORSCANS:
        for(; widthSrc > 0; widthSrc--, xdst += xinc)
            rowDst[xdst >> 16] |= *rowSrc++;
        break;
    case STRETCH_ANDSCANS:
        for(; widthSrc > 0; widthSrc--, xdst += xinc)
            rowDst[xdst >> 16] &= *rowSrc++;
        break;
    case STRETCH_DELETESCANS:
        for(; widthSrc > 0; widthSrc--, xdst += xinc)
            rowDst[xdst >> 16] = *rowSrc++;
        break;
    }
}


/***********************************************************************
 *           BITBLT_GetRow
 *
 * Retrieve a row from an image. Helper function for BITBLT_StretchImage.
 */
static void BITBLT_GetRow( XImage *image, int *pdata, INT row,
                           INT start, INT width, INT depthDst,
                           int fg, int bg, unsigned long pixel_mask, BOOL swap)
{
    register INT i;

    assert( (row >= 0) && (row < image->height) );
    assert( (start >= 0) && (width <= image->width) );

    pdata += swap ? start+width-1 : start;
    if (image->depth == depthDst)  /* color -> color */
    {
        if (X11DRV_PALETTE_XPixelToPalette && (depthDst != 1))
            if (swap) for (i = 0; i < width; i++)
                *pdata-- = X11DRV_PALETTE_XPixelToPalette[XGetPixel( image, i, row )];
            else for (i = 0; i < width; i++)
                *pdata++ = X11DRV_PALETTE_XPixelToPalette[XGetPixel( image, i, row )];
        else
            if (swap) for (i = 0; i < width; i++)
                *pdata-- = XGetPixel( image, i, row );
            else for (i = 0; i < width; i++)
                *pdata++ = XGetPixel( image, i, row );
    }
    else
    {
        if (image->depth == 1)  /* monochrome -> color */
        {
            if (X11DRV_PALETTE_XPixelToPalette)
            {
                fg = X11DRV_PALETTE_XPixelToPalette[fg];
                bg = X11DRV_PALETTE_XPixelToPalette[bg];
            }
            if (swap) for (i = 0; i < width; i++)
                *pdata-- = XGetPixel( image, i, row ) ? bg : fg;
            else for (i = 0; i < width; i++)
                *pdata++ = XGetPixel( image, i, row ) ? bg : fg;
        }
        else  /* color -> monochrome */
        {
            if (swap) for (i = 0; i < width; i++)
                *pdata-- = ((XGetPixel( image, i, row ) & pixel_mask) == bg) ? 1 : 0;
            else for (i = 0; i < width; i++)
                *pdata++ = ((XGetPixel( image, i, row ) & pixel_mask) == bg) ? 1 : 0;
        }
    }
}


/***********************************************************************
 *           BITBLT_StretchImage
 *
 * Stretch an X image.
 * FIXME: does not work for full 32-bit coordinates.
 */
static void BITBLT_StretchImage( XImage *srcImage, XImage *dstImage,
                                 INT widthSrc, INT heightSrc,
                                 INT widthDst, INT heightDst,
                                 RECT *visRectSrc, RECT *visRectDst,
                                 int foreground, int background,
                                 unsigned long pixel_mask, WORD mode )
{
    int *rowSrc, *rowDst, *pixel;
    char *pdata;
    INT xinc, xoff, yinc, ysrc, ydst;
    register INT x, y;
    BOOL hstretch, vstretch, hswap, vswap;

    hswap = widthSrc * widthDst < 0;
    vswap = heightSrc * heightDst < 0;
    widthSrc  = abs(widthSrc);
    heightSrc = abs(heightSrc);
    widthDst  = abs(widthDst);
    heightDst = abs(heightDst);

    if (!(rowSrc = HeapAlloc( GetProcessHeap(), 0,
                              (widthSrc+widthDst)*sizeof(int) ))) return;
    rowDst = rowSrc + widthSrc;

      /* When stretching, all modes are the same, and DELETESCANS is faster */
    if ((widthSrc < widthDst) && (heightSrc < heightDst))
        mode = STRETCH_DELETESCANS;

    if (mode == STRETCH_HALFTONE) /* FIXME */
        mode = STRETCH_DELETESCANS;

    if (mode != STRETCH_DELETESCANS)
        memset( rowDst, (mode == STRETCH_ANDSCANS) ? 0xff : 0x00,
                widthDst*sizeof(int) );

    hstretch = (widthSrc < widthDst);
    vstretch = (heightSrc < heightDst);

    if (hstretch)
    {
        xinc = (widthSrc << 16) / widthDst;
        xoff = ((widthSrc << 16) - (xinc * widthDst)) / 2;
    }
    else
    {
        xinc = ((int)widthDst << 16) / widthSrc;
        xoff = ((widthDst << 16) - (xinc * widthSrc)) / 2;
    }

    wine_tsx11_lock();
    if (vstretch)
    {
        yinc = (heightSrc << 16) / heightDst;
        ydst = visRectDst->top;
        if (vswap)
        {
            ysrc = yinc * (heightDst - ydst - 1);
            yinc = -yinc;
        }
        else
            ysrc = yinc * ydst;

        for ( ; (ydst < visRectDst->bottom); ysrc += yinc, ydst++)
        {
            if (((ysrc >> 16) < visRectSrc->top) ||
                ((ysrc >> 16) >= visRectSrc->bottom)) continue;

            /* Retrieve a source row */
            BITBLT_GetRow( srcImage, rowSrc, (ysrc >> 16) - visRectSrc->top,
                           visRectSrc->left, visRectSrc->right - visRectSrc->left,
                           dstImage->depth, foreground, background, pixel_mask, hswap );

            /* Stretch or shrink it */
            if (hstretch)
                BITBLT_StretchRow( rowSrc, rowDst, visRectDst->left,
                                   visRectDst->right - visRectDst->left,
                                   xinc, xoff, mode );
            else BITBLT_ShrinkRow( rowSrc, rowDst, visRectSrc->left,
                                   visRectSrc->right - visRectSrc->left,
                                   xinc, xoff, mode );

            /* Store the destination row */
            pixel = rowDst + visRectDst->right - 1;
            y = ydst - visRectDst->top;
            for (x = visRectDst->right-visRectDst->left-1; x >= 0; x--)
                XPutPixel( dstImage, x, y, *pixel-- );
            if (mode != STRETCH_DELETESCANS)
                memset( rowDst, (mode == STRETCH_ANDSCANS) ? 0xff : 0x00,
                        widthDst*sizeof(int) );

            /* Make copies of the destination row */

            pdata = dstImage->data + dstImage->bytes_per_line * y;
            while (((ysrc + yinc) >> 16 == ysrc >> 16) &&
                   (ydst < visRectDst->bottom-1))
            {
                memcpy( pdata + dstImage->bytes_per_line, pdata,
                        dstImage->bytes_per_line );
                pdata += dstImage->bytes_per_line;
                ysrc += yinc;
                ydst++;
            }
        }
    }
    else  /* Shrinking */
    {
        yinc = (heightDst << 16) / heightSrc;
        ysrc = visRectSrc->top;
        ydst = ((heightDst << 16) - (yinc * heightSrc)) / 2;
        if (vswap)
        {
            ydst += yinc * (heightSrc - ysrc - 1);
            yinc = -yinc;
        }
        else
            ydst += yinc * ysrc;

        for( ; (ysrc < visRectSrc->bottom); ydst += yinc, ysrc++)
        {
            if (((ydst >> 16) < visRectDst->top) ||
                ((ydst >> 16) >= visRectDst->bottom)) continue;

            /* Retrieve a source row */
            BITBLT_GetRow( srcImage, rowSrc, ysrc - visRectSrc->top,
                           visRectSrc->left, visRectSrc->right - visRectSrc->left,
                           dstImage->depth, foreground, background, pixel_mask, hswap );

            /* Stretch or shrink it */
            if (hstretch)
                BITBLT_StretchRow( rowSrc, rowDst, visRectDst->left,
                                   visRectDst->right - visRectDst->left,
                                   xinc, xoff, mode );
            else BITBLT_ShrinkRow( rowSrc, rowDst, visRectSrc->left,
                                   visRectSrc->right - visRectSrc->left,
                                   xinc, xoff, mode );

            /* Merge several source rows into the destination */
            if (mode == STRETCH_DELETESCANS)
            {
                /* Simply skip the overlapping rows */
                while (((ydst + yinc) >> 16 == ydst >> 16) &&
                       (ysrc < visRectSrc->bottom-1))
                {
                    ydst += yinc;
                    ysrc++;
                }
            }
            else if (((ydst + yinc) >> 16 == ydst >> 16) &&
                     (ysrc < visRectSrc->bottom-1))
                continue;  /* Restart loop for next overlapping row */

            /* Store the destination row */
            pixel = rowDst + visRectDst->right - 1;
            y = (ydst >> 16) - visRectDst->top;
            for (x = visRectDst->right-visRectDst->left-1; x >= 0; x--)
                XPutPixel( dstImage, x, y, *pixel-- );
            if (mode != STRETCH_DELETESCANS)
                memset( rowDst, (mode == STRETCH_ANDSCANS) ? 0xff : 0x00,
                        widthDst*sizeof(int) );
        }
    }
    wine_tsx11_unlock();
    HeapFree( GetProcessHeap(), 0, rowSrc );
}


/***********************************************************************
 *           BITBLT_GetSrcAreaStretch
 *
 * Retrieve an area from the source DC, stretching and mapping all the
 * pixels to Windows colors.
 */
static int BITBLT_GetSrcAreaStretch( X11DRV_PDEVICE *physDevSrc, X11DRV_PDEVICE *physDevDst,
                                     Pixmap pixmap, GC gc,
                                     const struct bitblt_coords *src, const struct bitblt_coords *dst )
{
    XImage *imageSrc, *imageDst;
    RECT rectSrc = src->visrect;
    RECT rectDst = dst->visrect;
    int fg, bg;

    OffsetRect( &rectSrc, -src->x, -src->y );
    OffsetRect( &rectDst, -dst->x, -dst->y );

    if (src->width < 0)  OffsetRect( &rectSrc, -src->width, 0 );
    if (dst->width < 0)  OffsetRect( &rectDst, -dst->width, 0 );
    if (src->height < 0) OffsetRect( &rectSrc, 0, -src->height );
    if (dst->height < 0) OffsetRect( &rectDst, 0, -dst->height );

    get_colors(physDevDst, physDevSrc, &fg, &bg);
    wine_tsx11_lock();
    /* FIXME: avoid BadMatch errors */
    imageSrc = XGetImage( gdi_display, physDevSrc->drawable,
                          physDevSrc->dc_rect.left + src->visrect.left,
                          physDevSrc->dc_rect.top + src->visrect.top,
                          src->visrect.right - src->visrect.left,
                          src->visrect.bottom - src->visrect.top,
                          AllPlanes, ZPixmap );
    wine_tsx11_unlock();

    imageDst = X11DRV_DIB_CreateXImage( rectDst.right - rectDst.left,
                                        rectDst.bottom - rectDst.top, physDevDst->depth );
    BITBLT_StretchImage( imageSrc, imageDst, src->width, src->height,
                         dst->width, dst->height, &rectSrc, &rectDst,
                         fg, physDevDst->depth != 1 ? bg : physDevSrc->backgroundPixel,
                         image_pixel_mask( physDevSrc ), GetStretchBltMode(physDevDst->dev.hdc) );
    wine_tsx11_lock();
    XPutImage( gdi_display, pixmap, gc, imageDst, 0, 0, 0, 0,
               rectDst.right - rectDst.left, rectDst.bottom - rectDst.top );
    XDestroyImage( imageSrc );
    X11DRV_DIB_DestroyXImage( imageDst );
    wine_tsx11_unlock();
    return 0;  /* no exposure events generated */
}


/***********************************************************************
 *           BITBLT_GetSrcArea
 *
 * Retrieve an area from the source DC, mapping all the
 * pixels to Windows colors.
 */
static int BITBLT_GetSrcArea( X11DRV_PDEVICE *physDevSrc, X11DRV_PDEVICE *physDevDst,
                              Pixmap pixmap, GC gc, RECT *visRectSrc )
{
    XImage *imageSrc, *imageDst;
    register INT x, y;
    int exposures = 0;
    INT width  = visRectSrc->right - visRectSrc->left;
    INT height = visRectSrc->bottom - visRectSrc->top;
    int fg, bg;
    BOOL memdc = (GetObjectType(physDevSrc->dev.hdc) == OBJ_MEMDC);

    if (physDevSrc->depth == physDevDst->depth)
    {
        wine_tsx11_lock();
        if (!X11DRV_PALETTE_XPixelToPalette ||
            (physDevDst->depth == 1))  /* monochrome -> monochrome */
        {
            if (physDevDst->depth == 1)
            {
                /* MSDN says if StretchBlt must convert a bitmap from monochrome
                   to color or vice versa, the foreground and background color of
                   the device context are used.  In fact, it also applies to the
                   case when it is converted from mono to mono. */
                XSetBackground( gdi_display, gc, physDevDst->textPixel );
                XSetForeground( gdi_display, gc, physDevDst->backgroundPixel );
                XCopyPlane( gdi_display, physDevSrc->drawable, pixmap, gc,
                            physDevSrc->dc_rect.left + visRectSrc->left,
                            physDevSrc->dc_rect.top + visRectSrc->top,
                            width, height, 0, 0, 1);
            }
            else
                XCopyArea( gdi_display, physDevSrc->drawable, pixmap, gc,
                           physDevSrc->dc_rect.left + visRectSrc->left,
                           physDevSrc->dc_rect.top + visRectSrc->top,
                           width, height, 0, 0);
            exposures++;
        }
        else  /* color -> color */
        {
            if (memdc)
                imageSrc = XGetImage( gdi_display, physDevSrc->drawable,
                                      physDevSrc->dc_rect.left + visRectSrc->left,
                                      physDevSrc->dc_rect.top + visRectSrc->top,
                                      width, height, AllPlanes, ZPixmap );
            else
            {
                /* Make sure we don't get a BadMatch error */
                XCopyArea( gdi_display, physDevSrc->drawable, pixmap, gc,
                           physDevSrc->dc_rect.left + visRectSrc->left,
                           physDevSrc->dc_rect.top + visRectSrc->top,
                           width, height, 0, 0);
                exposures++;
                imageSrc = XGetImage( gdi_display, pixmap, 0, 0, width, height,
                                      AllPlanes, ZPixmap );
            }
            for (y = 0; y < height; y++)
                for (x = 0; x < width; x++)
                    XPutPixel(imageSrc, x, y,
                              X11DRV_PALETTE_XPixelToPalette[XGetPixel(imageSrc, x, y)]);
            XPutImage( gdi_display, pixmap, gc, imageSrc,
                       0, 0, 0, 0, width, height );
            XDestroyImage( imageSrc );
        }
        wine_tsx11_unlock();
    }
    else
    {
        if (physDevSrc->depth == 1)  /* monochrome -> color */
        {
	    get_colors(physDevDst, physDevSrc, &fg, &bg);

            wine_tsx11_lock();
            if (X11DRV_PALETTE_XPixelToPalette)
            {
                XSetBackground( gdi_display, gc,
                             X11DRV_PALETTE_XPixelToPalette[fg] );
                XSetForeground( gdi_display, gc,
                             X11DRV_PALETTE_XPixelToPalette[bg]);
            }
            else
            {
                XSetBackground( gdi_display, gc, fg );
                XSetForeground( gdi_display, gc, bg );
            }
            XCopyPlane( gdi_display, physDevSrc->drawable, pixmap, gc,
                        physDevSrc->dc_rect.left + visRectSrc->left,
                        physDevSrc->dc_rect.top + visRectSrc->top,
                        width, height, 0, 0, 1 );
            exposures++;
            wine_tsx11_unlock();
        }
        else  /* color -> monochrome */
        {
            unsigned long pixel_mask;
            wine_tsx11_lock();
            /* FIXME: avoid BadMatch error */
            imageSrc = XGetImage( gdi_display, physDevSrc->drawable,
                                  physDevSrc->dc_rect.left + visRectSrc->left,
                                  physDevSrc->dc_rect.top + visRectSrc->top,
                                  width, height, AllPlanes, ZPixmap );
            if (!imageSrc)
            {
                wine_tsx11_unlock();
                return exposures;
            }
            imageDst = X11DRV_DIB_CreateXImage( width, height, physDevDst->depth );
            if (!imageDst)
            {
                XDestroyImage(imageSrc);
                wine_tsx11_unlock();
                return exposures;
            }
            pixel_mask = image_pixel_mask( physDevSrc );
            for (y = 0; y < height; y++)
                for (x = 0; x < width; x++)
                    XPutPixel(imageDst, x, y,
                              !((XGetPixel(imageSrc,x,y) ^ physDevSrc->backgroundPixel) & pixel_mask));
            XPutImage( gdi_display, pixmap, gc, imageDst,
                       0, 0, 0, 0, width, height );
            XDestroyImage( imageSrc );
            X11DRV_DIB_DestroyXImage( imageDst );
            wine_tsx11_unlock();
        }
    }
    return exposures;
}


/***********************************************************************
 *           BITBLT_GetDstArea
 *
 * Retrieve an area from the destination DC, mapping all the
 * pixels to Windows colors.
 */
static int BITBLT_GetDstArea(X11DRV_PDEVICE *physDev, Pixmap pixmap, GC gc, const RECT *visRectDst)
{
    int exposures = 0;
    INT width  = visRectDst->right - visRectDst->left;
    INT height = visRectDst->bottom - visRectDst->top;
    BOOL memdc = (GetObjectType( physDev->dev.hdc ) == OBJ_MEMDC);

    wine_tsx11_lock();

    if (!X11DRV_PALETTE_XPixelToPalette || (physDev->depth == 1) ||
	(X11DRV_PALETTE_PaletteFlags & X11DRV_PALETTE_VIRTUAL) )
    {
        XCopyArea( gdi_display, physDev->drawable, pixmap, gc,
                   physDev->dc_rect.left + visRectDst->left, physDev->dc_rect.top + visRectDst->top,
                   width, height, 0, 0 );
        exposures++;
    }
    else
    {
        register INT x, y;
        XImage *image;

        if (memdc)
            image = XGetImage( gdi_display, physDev->drawable,
                               physDev->dc_rect.left + visRectDst->left,
                               physDev->dc_rect.top + visRectDst->top,
                               width, height, AllPlanes, ZPixmap );
        else
        {
            /* Make sure we don't get a BadMatch error */
            XCopyArea( gdi_display, physDev->drawable, pixmap, gc,
                       physDev->dc_rect.left + visRectDst->left,
                       physDev->dc_rect.top + visRectDst->top,
                       width, height, 0, 0);
            exposures++;
            image = XGetImage( gdi_display, pixmap, 0, 0, width, height,
                               AllPlanes, ZPixmap );
        }
        if (image)
        {
            for (y = 0; y < height; y++)
                for (x = 0; x < width; x++)
                    XPutPixel( image, x, y,
                               X11DRV_PALETTE_XPixelToPalette[XGetPixel( image, x, y )]);
            XPutImage( gdi_display, pixmap, gc, image, 0, 0, 0, 0, width, height );
            XDestroyImage( image );
        }
    }

    wine_tsx11_unlock();
    return exposures;
}


/***********************************************************************
 *           BITBLT_PutDstArea
 *
 * Put an area back into the destination DC, mapping the pixel
 * colors to X pixels.
 */
static int BITBLT_PutDstArea(X11DRV_PDEVICE *physDev, Pixmap pixmap, const RECT *visRectDst)
{
    int exposures = 0;
    INT width  = visRectDst->right - visRectDst->left;
    INT height = visRectDst->bottom - visRectDst->top;

    /* !X11DRV_PALETTE_PaletteToXPixel is _NOT_ enough */

    if (!X11DRV_PALETTE_PaletteToXPixel || (physDev->depth == 1) ||
        (X11DRV_PALETTE_PaletteFlags & X11DRV_PALETTE_VIRTUAL) )
    {
        XCopyArea( gdi_display, pixmap, physDev->drawable, physDev->gc, 0, 0, width, height,
                   physDev->dc_rect.left + visRectDst->left,
                   physDev->dc_rect.top + visRectDst->top );
        exposures++;
    }
    else
    {
        register INT x, y;
        XImage *image = XGetImage( gdi_display, pixmap, 0, 0, width, height,
                                   AllPlanes, ZPixmap );
        for (y = 0; y < height; y++)
            for (x = 0; x < width; x++)
            {
                XPutPixel( image, x, y,
                           X11DRV_PALETTE_PaletteToXPixel[XGetPixel( image, x, y )]);
            }
        XPutImage( gdi_display, physDev->drawable, physDev->gc, image, 0, 0,
                   physDev->dc_rect.left + visRectDst->left,
                   physDev->dc_rect.top + visRectDst->top, width, height );
        XDestroyImage( image );
    }
    return exposures;
}

static BOOL same_format(X11DRV_PDEVICE *physDevSrc, X11DRV_PDEVICE *physDevDst)
{
    if (physDevSrc->depth != physDevDst->depth) return FALSE;
    if (!physDevSrc->color_shifts && !physDevDst->color_shifts) return TRUE;
    if (physDevSrc->color_shifts && physDevDst->color_shifts)
        return !memcmp(physDevSrc->color_shifts, physDevDst->color_shifts, sizeof(ColorShifts));
    return FALSE;
}

void execute_rop( X11DRV_PDEVICE *physdev, Pixmap src_pixmap, GC gc, const RECT *visrect, DWORD rop )
{
    Pixmap pixmaps[3];
    Pixmap result = src_pixmap;
    BOOL null_brush;
    const BYTE *opcode = BITBLT_Opcodes[(rop >> 16) & 0xff];
    BOOL use_pat = (((rop >> 4) & 0x0f0000) != (rop & 0x0f0000));
    BOOL use_dst = (((rop >> 1) & 0x550000) != (rop & 0x550000));
    int width  = visrect->right - visrect->left;
    int height = visrect->bottom - visrect->top;

    pixmaps[SRC] = src_pixmap;
    pixmaps[TMP] = 0;
    wine_tsx11_lock();
    pixmaps[DST] = XCreatePixmap( gdi_display, root_window, width, height, physdev->depth );
    wine_tsx11_unlock();

    if (use_dst) BITBLT_GetDstArea( physdev, pixmaps[DST], gc, visrect );
    null_brush = use_pat && !X11DRV_SetupGCForPatBlt( physdev, gc, TRUE );

    wine_tsx11_lock();
    for ( ; *opcode; opcode++)
    {
        if (OP_DST(*opcode) == DST) result = pixmaps[DST];
        XSetFunction( gdi_display, gc, OP_ROP(*opcode) );
        switch(OP_SRCDST(*opcode))
        {
        case OP_ARGS(DST,TMP):
        case OP_ARGS(SRC,TMP):
            if (!pixmaps[TMP])
                pixmaps[TMP] = XCreatePixmap( gdi_display, root_window, width, height, physdev->depth );
            /* fall through */
        case OP_ARGS(DST,SRC):
        case OP_ARGS(SRC,DST):
        case OP_ARGS(TMP,SRC):
        case OP_ARGS(TMP,DST):
            XCopyArea( gdi_display, pixmaps[OP_SRC(*opcode)], pixmaps[OP_DST(*opcode)], gc,
                       0, 0, width, height, 0, 0 );
            break;
        case OP_ARGS(PAT,DST):
        case OP_ARGS(PAT,SRC):
            if (!null_brush)
                XFillRectangle( gdi_display, pixmaps[OP_DST(*opcode)], gc, 0, 0, width, height );
            break;
        }
    }
    XSetFunction( gdi_display, physdev->gc, GXcopy );
    physdev->exposures += BITBLT_PutDstArea( physdev, result, visrect );
    XFreePixmap( gdi_display, pixmaps[DST] );
    if (pixmaps[TMP]) XFreePixmap( gdi_display, pixmaps[TMP] );
    wine_tsx11_unlock();
}

/***********************************************************************
 *           X11DRV_PatBlt
 */
BOOL X11DRV_PatBlt( PHYSDEV dev, struct bitblt_coords *dst, DWORD rop )
{
    X11DRV_PDEVICE *physDev = get_x11drv_dev( dev );
    BOOL usePat = (((rop >> 4) & 0x0f0000) != (rop & 0x0f0000));
    const BYTE *opcode = BITBLT_Opcodes[(rop >> 16) & 0xff];

    if (usePat && !X11DRV_SetupGCForBrush( physDev )) return TRUE;

    X11DRV_LockDIBSection( physDev, DIB_Status_GdiMod );

    wine_tsx11_lock();
    XSetFunction( gdi_display, physDev->gc, OP_ROP(*opcode) );

    switch(rop)  /* a few special cases */
    {
    case BLACKNESS:  /* 0x00 */
    case WHITENESS:  /* 0xff */
        if ((physDev->depth != 1) && X11DRV_PALETTE_PaletteToXPixel)
        {
            XSetFunction( gdi_display, physDev->gc, GXcopy );
            if (rop == BLACKNESS)
                XSetForeground( gdi_display, physDev->gc, X11DRV_PALETTE_PaletteToXPixel[0] );
            else
                XSetForeground( gdi_display, physDev->gc,
                                WhitePixel( gdi_display, DefaultScreen(gdi_display) ));
            XSetFillStyle( gdi_display, physDev->gc, FillSolid );
        }
        break;
    case DSTINVERT:  /* 0x55 */
        if (!(X11DRV_PALETTE_PaletteFlags & (X11DRV_PALETTE_PRIVATE | X11DRV_PALETTE_VIRTUAL)))
        {
            /* Xor is much better when we do not have full colormap.   */
            /* Using white^black ensures that we invert at least black */
            /* and white. */
            unsigned long xor_pix = (WhitePixel( gdi_display, DefaultScreen(gdi_display) ) ^
                                     BlackPixel( gdi_display, DefaultScreen(gdi_display) ));
            XSetFunction( gdi_display, physDev->gc, GXxor );
            XSetForeground( gdi_display, physDev->gc, xor_pix);
            XSetFillStyle( gdi_display, physDev->gc, FillSolid );
        }
        break;
    }
    XFillRectangle( gdi_display, physDev->drawable, physDev->gc,
                    physDev->dc_rect.left + dst->visrect.left,
                    physDev->dc_rect.top + dst->visrect.top,
                    dst->visrect.right - dst->visrect.left,
                    dst->visrect.bottom - dst->visrect.top );
    wine_tsx11_unlock();

    X11DRV_UnlockDIBSection( physDev, TRUE );
    return TRUE;
}


/***********************************************************************
 *           X11DRV_StretchBlt
 */
BOOL X11DRV_StretchBlt( PHYSDEV dst_dev, struct bitblt_coords *dst,
                        PHYSDEV src_dev, struct bitblt_coords *src, DWORD rop )
{
    X11DRV_PDEVICE *physDevDst = get_x11drv_dev( dst_dev );
    X11DRV_PDEVICE *physDevSrc = get_x11drv_dev( src_dev );
    BOOL fStretch;
    INT width, height;
    INT sDst, sSrc = DIB_Status_None;
    const BYTE *opcode;
    Pixmap src_pixmap;
    GC tmpGC;

    if (src_dev->funcs != dst_dev->funcs)
    {
        dst_dev = GET_NEXT_PHYSDEV( dst_dev, pStretchBlt );
        return dst_dev->funcs->pStretchBlt( dst_dev, dst, src_dev, src, rop );
    }

    fStretch = (src->width != dst->width) || (src->height != dst->height);

    if (physDevDst != physDevSrc)
        sSrc = X11DRV_LockDIBSection( physDevSrc, DIB_Status_None );

    width  = dst->visrect.right - dst->visrect.left;
    height = dst->visrect.bottom - dst->visrect.top;

    sDst = X11DRV_LockDIBSection( physDevDst, DIB_Status_None );
    if (physDevDst == physDevSrc) sSrc = sDst;

    /* try client-side DIB copy */
    if (!fStretch && sSrc == DIB_Status_AppMod)
    {
        if (physDevDst != physDevSrc) X11DRV_UnlockDIBSection( physDevSrc, FALSE );
        X11DRV_UnlockDIBSection( physDevDst, TRUE );
        dst_dev = GET_NEXT_PHYSDEV( dst_dev, pStretchBlt );
        return dst_dev->funcs->pStretchBlt( dst_dev, dst, src_dev, src, rop );
    }

    X11DRV_CoerceDIBSection( physDevDst, DIB_Status_GdiMod );

    opcode = BITBLT_Opcodes[(rop >> 16) & 0xff];

    /* a few optimizations for single-op ROPs */
    if (!fStretch && !opcode[1] && OP_SRCDST(opcode[0]) == OP_ARGS(SRC,DST))
    {
        if (same_format(physDevSrc, physDevDst))
        {
            wine_tsx11_lock();
            XSetFunction( gdi_display, physDevDst->gc, OP_ROP(*opcode) );
            wine_tsx11_unlock();

            if (physDevSrc != physDevDst) X11DRV_CoerceDIBSection( physDevSrc, DIB_Status_GdiMod );
            wine_tsx11_lock();
            XCopyArea( gdi_display, physDevSrc->drawable,
                       physDevDst->drawable, physDevDst->gc,
                       physDevSrc->dc_rect.left + src->visrect.left,
                       physDevSrc->dc_rect.top + src->visrect.top,
                       width, height,
                       physDevDst->dc_rect.left + dst->visrect.left,
                       physDevDst->dc_rect.top + dst->visrect.top );
            physDevDst->exposures++;
            wine_tsx11_unlock();
            goto done;
        }
        if (physDevSrc->depth == 1)
        {
            int fg, bg;

            X11DRV_CoerceDIBSection( physDevSrc, DIB_Status_GdiMod );
            get_colors(physDevDst, physDevSrc, &fg, &bg);
            wine_tsx11_lock();
            XSetBackground( gdi_display, physDevDst->gc, fg );
            XSetForeground( gdi_display, physDevDst->gc, bg );
            XSetFunction( gdi_display, physDevDst->gc, OP_ROP(*opcode) );
            XCopyPlane( gdi_display, physDevSrc->drawable,
                        physDevDst->drawable, physDevDst->gc,
                        physDevSrc->dc_rect.left + src->visrect.left,
                        physDevSrc->dc_rect.top + src->visrect.top,
                        width, height,
                        physDevDst->dc_rect.left + dst->visrect.left,
                        physDevDst->dc_rect.top + dst->visrect.top, 1 );
            physDevDst->exposures++;
            wine_tsx11_unlock();
            goto done;
        }
    }

    wine_tsx11_lock();
    tmpGC = XCreateGC( gdi_display, physDevDst->drawable, 0, NULL );
    XSetSubwindowMode( gdi_display, tmpGC, IncludeInferiors );
    XSetGraphicsExposures( gdi_display, tmpGC, False );
    src_pixmap = XCreatePixmap( gdi_display, root_window, width, height, physDevDst->depth );
    wine_tsx11_unlock();

    if (physDevDst != physDevSrc) X11DRV_CoerceDIBSection( physDevSrc, DIB_Status_GdiMod );

    if (fStretch)
        BITBLT_GetSrcAreaStretch( physDevSrc, physDevDst, src_pixmap, tmpGC, src, dst );
    else
        BITBLT_GetSrcArea( physDevSrc, physDevDst, src_pixmap, tmpGC, &src->visrect );

    execute_rop( physDevDst, src_pixmap, tmpGC, &dst->visrect, rop );

    wine_tsx11_lock();
    XFreePixmap( gdi_display, src_pixmap );
    XFreeGC( gdi_display, tmpGC );
    wine_tsx11_unlock();

done:
    if (physDevDst != physDevSrc) X11DRV_UnlockDIBSection( physDevSrc, FALSE );
    X11DRV_UnlockDIBSection( physDevDst, TRUE );
    return TRUE;
}


static void free_heap_bits( struct gdi_image_bits *bits )
{
    HeapFree( GetProcessHeap(), 0, bits->ptr );
}

static void free_ximage_bits( struct gdi_image_bits *bits )
{
    wine_tsx11_lock();
    XFree( bits->ptr );
    wine_tsx11_unlock();
}

/* store the palette or color mask data in the bitmap info structure */
static void set_color_info( PHYSDEV dev, const ColorShifts *color_shifts, BITMAPINFO *info )
{
    DWORD *colors = (DWORD *)((char *)info + info->bmiHeader.biSize);

    info->bmiHeader.biCompression = BI_RGB;
    info->bmiHeader.biClrUsed = 0;

    switch (info->bmiHeader.biBitCount)
    {
    case 4:
    case 8:
    {
        RGBQUAD *rgb = (RGBQUAD *)colors;
        PALETTEENTRY palette[256];
        UINT i, count;

        info->bmiHeader.biClrUsed = 1 << info->bmiHeader.biBitCount;
        count = X11DRV_GetSystemPaletteEntries( dev, 0, info->bmiHeader.biClrUsed, palette );
        for (i = 0; i < count; i++)
        {
            rgb[i].rgbRed   = palette[i].peRed;
            rgb[i].rgbGreen = palette[i].peGreen;
            rgb[i].rgbBlue  = palette[i].peBlue;
            rgb[i].rgbReserved = 0;
        }
        memset( &rgb[count], 0, (info->bmiHeader.biClrUsed - count) * sizeof(*rgb) );
        break;
    }
    case 16:
        colors[0] = color_shifts->logicalRed.max << color_shifts->logicalRed.shift;
        colors[1] = color_shifts->logicalGreen.max << color_shifts->logicalGreen.shift;
        colors[2] = color_shifts->logicalBlue.max << color_shifts->logicalBlue.shift;
        info->bmiHeader.biCompression = BI_BITFIELDS;
        break;
    case 32:
        colors[0] = color_shifts->logicalRed.max << color_shifts->logicalRed.shift;
        colors[1] = color_shifts->logicalGreen.max << color_shifts->logicalGreen.shift;
        colors[2] = color_shifts->logicalBlue.max << color_shifts->logicalBlue.shift;
        if (colors[0] != 0xff0000 || colors[1] != 0x00ff00 || colors[2] != 0x0000ff)
            info->bmiHeader.biCompression = BI_BITFIELDS;
        break;
    }
}

/* check if the specified color info is suitable for PutImage */
static BOOL matching_color_info( PHYSDEV dev, const ColorShifts *color_shifts, const BITMAPINFO *info )
{
    DWORD *colors = (DWORD *)((char *)info + info->bmiHeader.biSize);

    switch (info->bmiHeader.biBitCount)
    {
    case 1:
        if (info->bmiHeader.biCompression != BI_RGB) return FALSE;
        return !info->bmiHeader.biClrUsed;  /* color map not allowed */
    case 4:
    case 8:
    {
        RGBQUAD *rgb = (RGBQUAD *)colors;
        PALETTEENTRY palette[256];
        UINT i, count;

        if (info->bmiHeader.biCompression != BI_RGB) return FALSE;
        count = X11DRV_GetSystemPaletteEntries( dev, 0, 1 << info->bmiHeader.biBitCount, palette );
        if (count != info->bmiHeader.biClrUsed) return FALSE;
        for (i = 0; i < count; i++)
        {
            if (rgb[i].rgbRed   != palette[i].peRed ||
                rgb[i].rgbGreen != palette[i].peGreen ||
                rgb[i].rgbBlue  != palette[i].peBlue) return FALSE;
        }
        return TRUE;
    }
    case 16:
        if (info->bmiHeader.biCompression == BI_BITFIELDS)
            return (color_shifts->logicalRed.max << color_shifts->logicalRed.shift == colors[0] &&
                    color_shifts->logicalGreen.max << color_shifts->logicalGreen.shift == colors[1] &&
                    color_shifts->logicalBlue.max << color_shifts->logicalBlue.shift == colors[2]);
        if (info->bmiHeader.biCompression == BI_RGB)
            return (color_shifts->logicalRed.max << color_shifts->logicalRed.shift     == 0x7c00 &&
                    color_shifts->logicalGreen.max << color_shifts->logicalGreen.shift == 0x03e0 &&
                    color_shifts->logicalBlue.max << color_shifts->logicalBlue.shift   == 0x001f);
        break;
    case 32:
        if (info->bmiHeader.biCompression == BI_BITFIELDS)
            return (color_shifts->logicalRed.max << color_shifts->logicalRed.shift == colors[0] &&
                    color_shifts->logicalGreen.max << color_shifts->logicalGreen.shift == colors[1] &&
                    color_shifts->logicalBlue.max << color_shifts->logicalBlue.shift == colors[2]);
        /* fall through */
    case 24:
        if (info->bmiHeader.biCompression == BI_RGB)
            return (color_shifts->logicalRed.max << color_shifts->logicalRed.shift     == 0xff0000 &&
                    color_shifts->logicalGreen.max << color_shifts->logicalGreen.shift == 0x00ff00 &&
                    color_shifts->logicalBlue.max << color_shifts->logicalBlue.shift   == 0x0000ff);
        break;
    }
    return FALSE;
}

static inline BOOL is_r8g8b8( int depth, const ColorShifts *color_shifts )
{
    return depth == 24 && color_shifts->logicalBlue.shift == 0 && color_shifts->logicalRed.shift == 16;
}

/* copy the image bits, fixing up alignment and byte swapping as necessary */
DWORD copy_image_bits( BITMAPINFO *info, BOOL is_r8g8b8, XImage *image,
                       const struct gdi_image_bits *src_bits, struct gdi_image_bits *dst_bits,
                       struct bitblt_coords *coords, const int *mapping, unsigned int zeropad_mask )
{
#ifdef WORDS_BIGENDIAN
    static const int client_byte_order = MSBFirst;
#else
    static const int client_byte_order = LSBFirst;
#endif
    BOOL need_byteswap;
    int x, y, height = coords->visrect.bottom - coords->visrect.top;
    int width_bytes = image->bytes_per_line;
    int padding_pos;
    unsigned char *src, *dst;

    switch (info->bmiHeader.biBitCount)
    {
    case 1:
        need_byteswap = (image->bitmap_bit_order != MSBFirst);
        break;
    case 4:
        need_byteswap = (image->byte_order != MSBFirst);
        break;
    case 16:
    case 32:
        need_byteswap = (image->byte_order != client_byte_order);
        break;
    case 24:
        need_byteswap = (image->byte_order == MSBFirst) ^ !is_r8g8b8;
        break;
    default:
        need_byteswap = FALSE;
        break;
    }

    src = src_bits->ptr;
    if (info->bmiHeader.biHeight > 0)
        src += (info->bmiHeader.biHeight - coords->visrect.bottom) * width_bytes;
    else
        src += coords->visrect.top * width_bytes;

    if ((need_byteswap && !src_bits->is_copy) ||  /* need to swap bytes */
        (zeropad_mask != ~0u && !src_bits->is_copy) ||  /* need to clear padding bytes */
        (mapping && !src_bits->is_copy) ||  /* need to remap pixels */
        (width_bytes & 3) ||  /* need to fixup line alignment */
        (info->bmiHeader.biHeight > 0))  /* need to flip vertically */
    {
        width_bytes = (width_bytes + 3) & ~3;
        info->bmiHeader.biSizeImage = height * width_bytes;
        if (!(dst_bits->ptr = HeapAlloc( GetProcessHeap(), 0, info->bmiHeader.biSizeImage )))
            return ERROR_OUTOFMEMORY;
        dst_bits->is_copy = TRUE;
        dst_bits->free = free_heap_bits;
    }
    else
    {
        /* swap bits in place */
        dst_bits->ptr = src;
        dst_bits->is_copy = src_bits->is_copy;
        dst_bits->free = NULL;
        if (!need_byteswap && zeropad_mask == ~0u && !mapping) return ERROR_SUCCESS;  /* nothing to do */
    }

    dst = dst_bits->ptr;
    padding_pos = width_bytes/sizeof(unsigned int) - 1;

    if (info->bmiHeader.biHeight > 0)
    {
        dst += (height - 1) * width_bytes;
        width_bytes = -width_bytes;
    }

    if (need_byteswap || mapping)
    {
        switch (info->bmiHeader.biBitCount)
        {
        case 1:
            for (y = 0; y < height; y++, src += image->bytes_per_line, dst += width_bytes)
            {
                for (x = 0; x < image->bytes_per_line; x++)
                    dst[x] = bit_swap[src[x]];
                ((unsigned int *)dst)[padding_pos] &= zeropad_mask;
            }
            break;
        case 4:
            for (y = 0; y < height; y++, src += image->bytes_per_line, dst += width_bytes)
            {
                if (mapping)
                    for (x = 0; x < image->bytes_per_line; x++)
                        dst[x] = (mapping[src[x] & 0x0f] << 4) | mapping[src[x] >> 4];
                else
                    for (x = 0; x < image->bytes_per_line; x++)
                        dst[x] = (src[x] << 4) | (src[x] >> 4);
                ((unsigned int *)dst)[padding_pos] &= zeropad_mask;
            }
            break;
        case 8:
            for (y = 0; y < height; y++, src += image->bytes_per_line, dst += width_bytes)
            {
                for (x = 0; x < image->bytes_per_line; x++)
                    dst[x] = mapping[src[x]];
                ((unsigned int *)dst)[padding_pos] &= zeropad_mask;
            }
            break;
        case 16:
            for (y = 0; y < height; y++, src += image->bytes_per_line, dst += width_bytes)
            {
                for (x = 0; x < info->bmiHeader.biWidth; x++)
                    ((USHORT *)dst)[x] = RtlUshortByteSwap( ((const USHORT *)src)[x] );
                ((unsigned int *)dst)[padding_pos] &= zeropad_mask;
            }
            break;
        case 24:
            for (y = 0; y < height; y++, src += image->bytes_per_line, dst += width_bytes)
            {
                for (x = 0; x < info->bmiHeader.biWidth; x++)
                {
                    unsigned char tmp = src[3 * x];
                    dst[3 * x]     = src[3 * x + 2];
                    dst[3 * x + 1] = src[3 * x + 1];
                    dst[3 * x + 2] = tmp;
                }
                ((unsigned int *)dst)[padding_pos] &= zeropad_mask;
            }
            break;
        case 32:
            for (y = 0; y < height; y++, src += image->bytes_per_line, dst += width_bytes)
                for (x = 0; x < info->bmiHeader.biWidth; x++)
                    ((ULONG *)dst)[x] = RtlUlongByteSwap( ((const ULONG *)src)[x] );
            break;
        }
    }
    else if (src != dst)
    {
        for (y = 0; y < height; y++, src += image->bytes_per_line, dst += width_bytes)
        {
            memcpy( dst, src, image->bytes_per_line );
            ((unsigned int *)dst)[padding_pos] &= zeropad_mask;
        }
    }
    else  /* only need to clear the padding */
    {
        for (y = 0; y < height; y++, dst += width_bytes)
            ((unsigned int *)dst)[padding_pos] &= zeropad_mask;
    }
    return ERROR_SUCCESS;
}

/***********************************************************************
 *           X11DRV_PutImage
 */
DWORD X11DRV_PutImage( PHYSDEV dev, HBITMAP hbitmap, HRGN clip, BITMAPINFO *info,
                       const struct gdi_image_bits *bits, struct bitblt_coords *src,
                       struct bitblt_coords *dst, DWORD rop )
{
    X11DRV_PDEVICE *physdev;
    X_PHYSBITMAP *bitmap;
    DWORD ret;
    XImage *image;
    int depth;
    struct gdi_image_bits dst_bits;
    const XPixmapFormatValues *format;
    const ColorShifts *color_shifts;
    const BYTE *opcode = BITBLT_Opcodes[(rop >> 16) & 0xff];
    const int *mapping = NULL;

    if (hbitmap)
    {
        if (!(bitmap = X11DRV_get_phys_bitmap( hbitmap ))) return ERROR_INVALID_HANDLE;
        physdev = NULL;
        depth = bitmap->depth;
        color_shifts = &bitmap->color_shifts;
    }
    else
    {
        physdev = get_x11drv_dev( dev );
        bitmap = NULL;
        depth = physdev->depth;
        color_shifts = physdev->color_shifts;
    }
    format = pixmap_formats[depth];

    if (info->bmiHeader.biPlanes != 1) goto update_format;
    if (info->bmiHeader.biBitCount != format->bits_per_pixel) goto update_format;
    /* FIXME: could try to handle 1-bpp using XCopyPlane */
    if (!matching_color_info( dev, color_shifts, info )) goto update_format;
    if (!bits) return ERROR_SUCCESS;  /* just querying the format */
    if ((src->width != dst->width) || (src->height != dst->height)) return ERROR_TRANSFORM_NOT_SUPPORTED;

    wine_tsx11_lock();
    image = XCreateImage( gdi_display, visual, depth, ZPixmap, 0, NULL,
                          info->bmiHeader.biWidth, src->visrect.bottom - src->visrect.top, 32, 0 );
    wine_tsx11_unlock();
    if (!image) return ERROR_OUTOFMEMORY;

    if (image->bits_per_pixel == 4 || image->bits_per_pixel == 8)
    {
        if (bitmap || (!opcode[1] && OP_SRCDST(opcode[0]) == OP_ARGS(SRC,DST)))
            mapping = X11DRV_PALETTE_PaletteToXPixel;
    }

    ret = copy_image_bits( info, is_r8g8b8(depth,color_shifts), image, bits, &dst_bits, src, mapping, ~0u );

    if (!ret)
    {
        int width = dst->visrect.right - dst->visrect.left;
        int height = dst->visrect.bottom - dst->visrect.top;

        image->data = dst_bits.ptr;
        /* hack: make sure the bits are readable if we are reading from a DIB section */
        /* to be removed once we get rid of DIB access protections */
        if (!dst_bits.is_copy) IsBadReadPtr( dst_bits.ptr, height * image->bytes_per_line );

        if (bitmap)
        {
            RGNDATA *clip_data = NULL;
            GC gc;

            if (clip) clip_data = X11DRV_GetRegionData( clip, 0 );
            X11DRV_DIB_Lock( bitmap, DIB_Status_GdiMod );

            wine_tsx11_lock();
            gc = XCreateGC( gdi_display, bitmap->pixmap, 0, NULL );
            XSetGraphicsExposures( gdi_display, gc, False );
            if (clip_data) XSetClipRectangles( gdi_display, gc, 0, 0, (XRectangle *)clip_data->Buffer,
                                               clip_data->rdh.nCount, YXBanded );
            XPutImage( gdi_display, bitmap->pixmap, gc, image, src->visrect.left, 0,
                       dst->visrect.left, dst->visrect.top, width, height );
            XFreeGC( gdi_display, gc );
            wine_tsx11_unlock();

            X11DRV_DIB_Unlock( bitmap, TRUE );
            HeapFree( GetProcessHeap(), 0, clip_data );
        }
        else
        {
            RGNDATA *saved_region = NULL;

            if (clip) saved_region = add_extra_clipping_region( physdev, clip );
            X11DRV_LockDIBSection( physdev, DIB_Status_GdiMod );

            /* optimization for single-op ROPs */
            if (!opcode[1] && OP_SRCDST(opcode[0]) == OP_ARGS(SRC,DST))
            {
                wine_tsx11_lock();
                XSetFunction( gdi_display, physdev->gc, OP_ROP(*opcode) );
                XPutImage( gdi_display, physdev->drawable, physdev->gc, image, src->visrect.left, 0,
                           physdev->dc_rect.left + dst->visrect.left,
                           physdev->dc_rect.top + dst->visrect.top, width, height );
                wine_tsx11_unlock();
            }
            else
            {
                Pixmap src_pixmap;
                GC gc;

                wine_tsx11_lock();
                gc = XCreateGC( gdi_display, physdev->drawable, 0, NULL );
                XSetSubwindowMode( gdi_display, gc, IncludeInferiors );
                XSetGraphicsExposures( gdi_display, gc, False );
                src_pixmap = XCreatePixmap( gdi_display, root_window, width, height, depth );
                XPutImage( gdi_display, src_pixmap, gc, image, src->visrect.left, 0, 0, 0, width, height );
                wine_tsx11_unlock();

                execute_rop( physdev, src_pixmap, gc, &dst->visrect, rop );

                wine_tsx11_lock();
                XFreePixmap( gdi_display, src_pixmap );
                XFreeGC( gdi_display, gc );
                wine_tsx11_unlock();
            }

            X11DRV_UnlockDIBSection( physdev, !ret );
            restore_clipping_region( physdev, saved_region );
        }
        image->data = NULL;
    }

    wine_tsx11_lock();
    XDestroyImage( image );
    wine_tsx11_unlock();
    if (dst_bits.free) dst_bits.free( &dst_bits );
    return ret;

update_format:
    info->bmiHeader.biPlanes   = 1;
    info->bmiHeader.biBitCount = format->bits_per_pixel;
    if (info->bmiHeader.biHeight > 0) info->bmiHeader.biHeight = -info->bmiHeader.biHeight;
    set_color_info( dev, color_shifts, info );
    return ERROR_BAD_FORMAT;
}

/***********************************************************************
 *           X11DRV_GetImage
 */
DWORD X11DRV_GetImage( PHYSDEV dev, HBITMAP hbitmap, BITMAPINFO *info,
                       struct gdi_image_bits *bits, struct bitblt_coords *src )
{
    X11DRV_PDEVICE *physdev;
    X_PHYSBITMAP *bitmap;
    DWORD ret = ERROR_SUCCESS;
    XImage *image;
    UINT align, x, y, width, height;
    int depth;
    struct gdi_image_bits src_bits;
    const XPixmapFormatValues *format;
    const ColorShifts *color_shifts;
    const int *mapping = NULL;

    if (hbitmap)
    {
        if (!(bitmap = X11DRV_get_phys_bitmap( hbitmap ))) return ERROR_INVALID_HANDLE;
        physdev = NULL;
        depth = bitmap->depth;
        color_shifts = &bitmap->color_shifts;
    }
    else
    {
        physdev = get_x11drv_dev( dev );
        bitmap = NULL;
        depth = physdev->depth;
        color_shifts = physdev->color_shifts;
    }
    format = pixmap_formats[depth];

    /* align start and width to 32-bit boundary */
    switch (format->bits_per_pixel)
    {
    case 1:  align = 32; break;
    case 4:  align = 8;  mapping = X11DRV_PALETTE_XPixelToPalette; break;
    case 8:  align = 4;  mapping = X11DRV_PALETTE_XPixelToPalette; break;
    case 16: align = 2;  break;
    case 24: align = 4;  break;
    case 32: align = 1;  break;
    default:
        FIXME( "depth %u bpp %u not supported yet\n", depth, format->bits_per_pixel );
        return ERROR_BAD_FORMAT;
    }

    info->bmiHeader.biSize          = sizeof(info->bmiHeader);
    info->bmiHeader.biPlanes        = 1;
    info->bmiHeader.biBitCount      = format->bits_per_pixel;
    info->bmiHeader.biXPelsPerMeter = 0;
    info->bmiHeader.biYPelsPerMeter = 0;
    info->bmiHeader.biClrImportant  = 0;
    set_color_info( dev, color_shifts, info );

    if (!bits) return ERROR_SUCCESS;  /* just querying the color information */

    x = src->visrect.left & ~(align - 1);
    y = src->visrect.top;
    width = src->visrect.right - x;
    height = src->visrect.bottom - src->visrect.top;
    if (format->scanline_pad != 32) width = (width + (align - 1)) & ~(align - 1);
    /* make the source rectangle relative to the returned bits */
    src->x -= x;
    src->y -= y;
    OffsetRect( &src->visrect, -x, -y );

    if (bitmap)
    {
        BITMAP bm;
        GetObjectW( hbitmap, sizeof(bm), &bm );
        width = min( width, bm.bmWidth - x );
        height = min( height, bm.bmHeight - y );
        X11DRV_DIB_Lock( bitmap, DIB_Status_GdiMod );
        wine_tsx11_lock();
        image = XGetImage( gdi_display, bitmap->pixmap, x, y, width, height, AllPlanes, ZPixmap );
        wine_tsx11_unlock();
        X11DRV_DIB_Unlock( bitmap, TRUE );
    }
    else if (GetObjectType( dev->hdc ) == OBJ_MEMDC)
    {
        X11DRV_LockDIBSection( physdev, DIB_Status_GdiMod );
        width = min( width, physdev->dc_rect.right - physdev->dc_rect.left - x );
        height = min( height, physdev->dc_rect.bottom - physdev->dc_rect.top - y );
        wine_tsx11_lock();
        image = XGetImage( gdi_display, physdev->drawable,
                           physdev->dc_rect.left + x, physdev->dc_rect.top + y,
                           width, height, AllPlanes, ZPixmap );
        wine_tsx11_unlock();
        X11DRV_UnlockDIBSection( physdev, FALSE );
    }
    else
    {
        Pixmap pixmap;

        wine_tsx11_lock();
        /* use a temporary pixmap to avoid BadMatch errors */
        pixmap = XCreatePixmap( gdi_display, root_window, width, height, depth );
        XCopyArea( gdi_display, physdev->drawable, pixmap, get_bitmap_gc(depth),
                   physdev->dc_rect.left + x, physdev->dc_rect.top + y, width, height, 0, 0 );
        image = XGetImage( gdi_display, pixmap, 0, 0, width, height, AllPlanes, ZPixmap );
        XFreePixmap( gdi_display, pixmap );
        wine_tsx11_unlock();
    }
    if (!image) return ERROR_OUTOFMEMORY;

    info->bmiHeader.biWidth     = width;
    info->bmiHeader.biHeight    = -height;
    info->bmiHeader.biSizeImage = height * image->bytes_per_line;

    src_bits.ptr     = image->data;
    src_bits.is_copy = TRUE;
    ret = copy_image_bits( info, is_r8g8b8(depth,color_shifts), image, &src_bits, bits, src, mapping,
                           zeropad_masks[(width * image->bits_per_pixel) & 31] );

    if (!ret && bits->ptr == image->data)
    {
        bits->free = free_ximage_bits;
        image->data = NULL;
    }
    wine_tsx11_lock();
    XDestroyImage( image );
    wine_tsx11_unlock();
    return ret;
}
