/* BEGIN_ICS_COPYRIGHT7 ****************************************

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

** END_ICS_COPYRIGHT7   ****************************************/

/* [ICS VERSION STRING: unknown] */

#include "sm_l.h"
#include "pm_l.h"
#include "pm_topology.h"
#include <iba/ibt.h>
#include <limits.h>
#include <iba/stl_helper.h>

/* indicate if the given node supports a PMA */
static boolean PmNodeHasPma(Node_t *nodep)
{
	//This function will remain in the code, for the possibility
	// of a future device or port that does not have a PMA
	return TRUE;
}

/* indicate if the given port supports a PMA */
// assumes caller will first check if Node supports a PMA */
static boolean PmPortHasPma(Port_t *portp)
{
	//This function will remain in the code, for the possibility 
	// of a future device or port that does not have a PMA
	return TRUE;
}

/* Based on Node, determine PMA capabilities and limitations */
/* this does not issue any packets on wire */
void PmUpdateNodePmaCapabilities(PmNode_t *pmnodep, Node_t *nodep, boolean ProcessHFICounters)
{
	pmnodep->u.s.PmaAvoid = 0;
	if ((nodep->nodeInfo.NodeType == STL_NODE_FI && !ProcessHFICounters) || !PmNodeHasPma(nodep)) {
		pmnodep->u.s.PmaAvoid = 1;
	}
}

/* Based on Port, determine PMA capabilities and limitations */
/* Assumes PmUpdateNodePmaCapabilities already called for parent node */
/* this does not issue any packets on wire */
void PmUpdatePortPmaCapabilities(PmPort_t *pmportp, Port_t *portp)
{
	pmportp->u.s.PmaAvoid = 0;
	if (pmportp->pmnodep->u.s.PmaAvoid || ! PmPortHasPma(portp)) {
		pmportp->u.s.PmaAvoid = 1;
	}
}
