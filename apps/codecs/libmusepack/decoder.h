/*
  Copyright (c) 2005, The Musepack Development Team
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

  * Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the following
  disclaimer in the documentation and/or other materials provided
  with the distribution.

  * Neither the name of the The Musepack Development Team nor the
  names of its contributors may be used to endorse or promote
  products derived from this software without specific prior
  written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/// \file decoder.h

#ifndef _mpcdec_decoder_h_
#define _mpcdec_decoder_h_

#include "huffman.h"
#include "math.h"
#include "musepack.h"
#include "reader.h"
#include "streaminfo.h"

// define this to enable/disable support for streamversion SV4-6
//#define MPC_SUPPORT_SV456

// SCF_HACK is used to avoid possible distortion after seeking with mpc files
// background: scf's are coded differential in time domain. if you seek to the
// desired postion it might happen that the baseline is missed and the resulting
// scf is much too high (hissing noise). this hack uses the lowest scaling until
// a non-differential scf could be decoded after seek. through this hack subbands
// are faded out until there was at least a single non-differential scf found.
#define SCF_HACK

enum {
    MPC_V_MEM = 2304,
    MPC_DECODER_MEMSIZE = 16384,  // overall buffer size (words)
    MPC_SEEK_BUFFER_SIZE = 8192, // seek buffer size (words)
};

typedef struct {
    mpc_int16_t  L [36];
    mpc_int16_t  R [36];
} QuantTyp;

typedef struct mpc_decoder_t {
    mpc_reader *r;

    /// @name internal state variables
    //@{

    mpc_uint32_t  next;
    mpc_uint32_t  dword; /// currently decoded 32bit-word
    mpc_uint32_t  pos;   /// bit-position within dword
    mpc_uint32_t  *Speicher; /// read-buffer
    mpc_uint32_t  Zaehler; /// actual index within read-buffer
    mpc_uint32_t  Ring;

    mpc_uint32_t  samples_to_skip;
    mpc_uint32_t  last_block_samples;
    mpc_uint32_t  FwdJumpInfo;
    mpc_uint32_t  ActDecodePos;
    

    mpc_uint32_t  DecodedFrames;
    mpc_uint32_t  OverallFrames;
    mpc_int32_t   SampleRate;                 // Sample frequency

    mpc_uint32_t  StreamVersion;              // version of bitstream
    mpc_int32_t   Max_Band;
    mpc_uint32_t  MPCHeaderPos;               // AB: needed to support ID3v2

    mpc_uint32_t  FrameWasValid;
    mpc_uint32_t  MS_used;                    // MS-coding used ?
    mpc_uint32_t  TrueGaplessPresent;

    mpc_uint32_t  WordsRead;                  // counts amount of decoded dwords

    // randomizer state variables
    mpc_uint32_t  __r1; 
    mpc_uint32_t  __r2; 

    mpc_int8_t    SCF_Index_L [32] [3];
    mpc_int8_t    SCF_Index_R [32] [3];       // holds scalefactor-indices
    QuantTyp      Q [32];                     // holds quantized samples
    mpc_int8_t    Res_L [32];
    mpc_int8_t    Res_R [32];                 // holds the chosen quantizer for each subband
#ifdef MPC_SUPPORT_SV456
    mpc_bool_t    DSCF_Flag_L [32];
    mpc_bool_t    DSCF_Flag_R [32];           // differential SCF used?
#endif
    mpc_int8_t    SCFI_L [32];
    mpc_int8_t    SCFI_R [32];                // describes order of transmitted SCF
    mpc_bool_t    MS_Flag[32];                // MS used?

    mpc_uint32_t  SeekTableCounter;           // used to sum up skip info, if SeekTable_Step != 1
    mpc_uint32_t  MaxDecodedFrames;           // Maximum frames decoded (indicates usable seek table entries)
    mpc_uint32_t* SeekTable;                  // seek table itself
    mpc_uint8_t   SeekTable_Step;             // 1<<SeekTable_Step = frames per table index
    mpc_uint32_t  SeekTable_Mask;             // used to avoid modulo-operation in seek

#ifdef MPC_FIXED_POINT
    mpc_uint8_t   SCF_shift[256];
#endif

    MPC_SAMPLE_FORMAT V_L[MPC_V_MEM + 960];
    MPC_SAMPLE_FORMAT V_R[MPC_V_MEM + 960];
    MPC_SAMPLE_FORMAT (*Y_L)[32];
    MPC_SAMPLE_FORMAT (*Y_R)[32];
    MPC_SAMPLE_FORMAT SCF[256]; ///< holds adapted scalefactors (for clipping prevention)
    //@}

} mpc_decoder;

#endif // _mpc_decoder_h
