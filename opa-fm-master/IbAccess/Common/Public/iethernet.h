/* BEGIN_ICS_COPYRIGHT3 ****************************************

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

** END_ICS_COPYRIGHT3   ****************************************/

/* [ICS VERSION STRING: unknown] */

#ifndef _IBA_PUBLIC_IETHERNET_H_
#define _IBA_PUBLIC_IETHERNET_H_

#include "iba/public/datatypes.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "iba/public/ipackon.h"

#define ETHERNET_ADDR_LEN	6
#define ETHERNET_MIN_FRAME	60		/* not incl 4 byte CRC trailer */
#define ETHERNET_MAX_FRAME	1514	/* not incl 4 byte CRC trailer */
#define ETHERNET_MAX_DATA	1500
#define ETHERNET_MAX_OVERHEAD	18	/* 14 frame header + 4 FCS bytes */

#define ETHERNET_PROTO_IP	0x0800
#define ETHERNET_PROTO_IPV6	0x86dd
#define ETHERNET_PROTO_ARP	0x0806
#define ETHERNET_PROTO_RARP	0x8035

typedef struct _ETHERNET_HEADER
{
	uint8	DestMac[ETHERNET_ADDR_LEN];
	uint8	SrcMac[ETHERNET_ADDR_LEN];
	uint16	Protocol;
} PACK_SUFFIX ETHERNET_HEADER;

#include "iba/public/ipackoff.h"

#ifdef __cplusplus
}	/* extern "C" */
#endif

#endif /* _IBA_PUBLIC_IETHERNET_H_ */
