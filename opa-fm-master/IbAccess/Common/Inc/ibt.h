/* BEGIN_ICS_COPYRIGHT1 ****************************************

Copyright (c) 2015, Intel Corporation

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
     documentation and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

** END_ICS_COPYRIGHT1   ****************************************/

/* [ICS VERSION STRING: unknown] */

/* Suppress duplicate loading of this file */
#ifndef _IBA_IBT_INTF_H_
#define _IBA_IBT_INTF_H_

/* This is the primary include file for kernel mode access to
 * the IbAccess APIs.  It in turn includes all the files required to
 * define the datatypes and interface functions which are exported.
 */

#if defined(VXWORKS)
#include "iba/vpi_export.h"
#include "iba/public/statustext.h"
#include "iba/ib_ibt.h"
#include "iba/ib_gsi.h"
#include "iba/ib_smi.h"
#ifdef BUILD_CM
#include "iba/ib_cm.h"
#endif
#include "iba/stl_sd.h"
#include "iba/stl_pa.h"
#include "iba/ib_avtracker.h"
#include "iba/umadt.h"
#ifdef BUILD_DMC
#include "iba/ib_dma.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef BUILD_DMC
extern FSTATUS TtsInitialize(EUI64 SystemImageGUID, IOU_CALLBACK IouCallback, void* Context);
#else
extern FSTATUS TtsInitialize(EUI64 SystemImageGUID);
#endif

#ifdef __cplusplus
};
#endif

#endif /* defined(VXWORKS) */
#endif /* _IBA_IBT_INTF_H_ */
