/* BEGIN_ICS_COPYRIGHT5 ****************************************

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

** END_ICS_COPYRIGHT5   ****************************************/

/* [ICS VERSION STRING: unknown] */

//===========================================================================//
//									     //
// FILE NAME								     //
//    sa_LinkRecord.c							     //
//									     //
// DESCRIPTION								     //
//    This file contains the routines to process the SA requests for 	     //
//    records of the LinkRecord type.					     //
//									     //
// DATA STRUCTURES							     //
//    None								     //
//									     //
// FUNCTIONS								     //
//    sa_LinkRecord							     //
//									     //
// DEPENDENCIES								     //
//    ib_mad.h								     //
//    ib_status.h							     //
//									     //
//									     //
//===========================================================================//


#include "os_g.h"
#include "ib_mad.h"
#include "ib_sa.h"
#include "ib_status.h"
#include "cs_g.h"
#include "mai_g.h"
#include "sm_counters.h"
#include "sm_l.h"
#include "sa_l.h"

Status_t	sa_LinkRecord_Get(Mai_t *, uint32_t *);
Status_t	sa_LinkRecord_GetTable(Mai_t *, uint32_t *);

Status_t
sa_LinkRecord(Mai_t *maip, sa_cntxt_t* sa_cntxt ) {
	uint32_t	records;
	uint32_t	attribOffset;

	IB_ENTER("sa_LinkRecord", maip, 0, 0, 0);

//
//	Assume failure.
//
	records = 0;

//
//	Check the method.  If this is a template lookup, then call the regular
//	GetTable(*) template lookup routine.
//
	switch (maip->base.method) {
	case SA_CM_GET:
		INCREMENT_COUNTER(smCounterSaRxGetLinkRecord);
		(void)sa_LinkRecord_GetTable(maip, &records);
		break;
	case SA_CM_GETTABLE:
		INCREMENT_COUNTER(smCounterSaRxGetTblLinkRecord);
		(void)sa_LinkRecord_GetTable(maip, &records);
		break;
        default:                                                                     
                maip->base.status = MAD_STATUS_BAD_METHOD;                           
                (void)sa_send_reply(maip, sa_cntxt);                                 
                IB_LOG_WARN("sa_LinkRecord: invalid METHOD:", maip->base.method);
                IB_EXIT("sa_LinkRecord", VSTATUS_OK);                            
                return VSTATUS_OK;                                                   
                break;                                                               
	}

//
//	Determine reply status
//
	if (maip->base.status != MAD_STATUS_OK) {
		records = 0;
	} else if (records == 0) {
		maip->base.status = MAD_STATUS_SA_NO_RECORDS;
	} else if ((maip->base.method == SA_CM_GET) && (records != 1)) {
		IB_LOG_WARN("sa_LinkRecord: too many records for SA_CM_GET:", records);
		records = 0;
		maip->base.status = MAD_STATUS_SA_TOO_MANY_RECS;
	} else {
		maip->base.status = MAD_STATUS_OK;
	}

	attribOffset =  sizeof(STL_LINK_RECORD) + Calculate_Padding(sizeof(STL_LINK_RECORD));
	sa_cntxt->attribLen = attribOffset;

	sa_cntxt_data( sa_cntxt, sa_data, records * attribOffset);
	(void)sa_send_reply(maip, sa_cntxt);

	IB_EXIT("sa_LinkRecord", VSTATUS_OK);
	return(VSTATUS_OK);
}

Status_t
sa_LinkRecord_Set(uint8_t *lrp, Node_t *nodep, Port_t *portp) {
	uint32_t	    portno;
	uint32		    lid;
	uint32		    newlid;
	Node_t		    *newnodep;
    Port_t          *lnkPortp;
	STL_LINK_RECORD linkRecord = {{0}};

	IB_ENTER("sa_LinkRecord_Set", lrp, nodep, portp, 0);

    //
    //	Find the neighbor node.
    //
	newnodep = sm_find_node(&old_topology, portp->nodeno);
	if (newnodep==NULL) {
		return (VSTATUS_BAD);
	}
	portno = (newnodep->nodeInfo.NodeType == NI_TYPE_SWITCH) ? 0 : portp->portno;

	if (!sm_valid_port((lnkPortp = sm_get_port(newnodep,portno)))) {
		IB_LOG_ERROR0("sa_LinkRecord_Set: failed to get port of neighbor node");
		IB_EXIT("sa_LinkRecord_Set", VSTATUS_BAD);
		return(VSTATUS_BAD);
	}

	newlid = lnkPortp->portData->lid;

    //
    //	Load the actual LinkRecord.
    //
	portno = (nodep->nodeInfo.NodeType == NI_TYPE_SWITCH) ? 0 : portp->index;

	if (!sm_valid_port((lnkPortp = sm_get_port(nodep,portno)))) {
		IB_LOG_ERROR0("sa_LinkRecord_Set: failed to get port");
		IB_EXIT("sa_LinkRecord_Set", VSTATUS_BAD);
		return(VSTATUS_BAD);
	}

	lid = lnkPortp->portData->lid;

	linkRecord.RID.FromLID = lid;
	linkRecord.RID.FromPort = portp->index;
	linkRecord.ToLID = newlid;
	linkRecord.ToPort = portp->portno;
	BSWAPCOPY_STL_LINK_RECORD(&linkRecord, (STL_LINK_RECORD*)lrp);

	IB_EXIT("sa_LinkRecord_Set", VSTATUS_OK);
	return(VSTATUS_OK);
}

Status_t
sa_LinkRecord_GetTable(Mai_t *maip, uint32_t *records) {
	uint8_t		*data;
	uint32_t	bytes;
	Node_t		*nodep;
	Port_t		*portp;
	STL_SA_MAD		samad;
	Status_t	status;

	IB_ENTER("sa_LinkRecord_GetTable", maip, *records, 0, 0);

	*records = 0;
	data = sa_data;
	bytes = Calculate_Padding(sizeof(STL_LINK_RECORD));

//
//  Verify the size of the data received for the request
//
	if ( maip->datasize-sizeof(STL_SA_MAD_HEADER) < sizeof(STL_LINK_RECORD) ) {
		IB_LOG_ERROR_FMT(__func__,
			"invalid MAD length; size of STL_LINK_RECORD[%"PRISZT"], datasize[%d]",
			sizeof(STL_LINK_RECORD), (int)(maip->datasize-sizeof(STL_SA_MAD_HEADER)));
		maip->base.status = MAD_STATUS_SA_REQ_INVALID;
		IB_EXIT("sa_LinkRecord_GetTable", MAD_STATUS_SA_REQ_INVALID);
		return (MAD_STATUS_SA_REQ_INVALID);
	}

	BSWAPCOPY_STL_SA_MAD((STL_SA_MAD*)maip->data, &samad, sizeof(STL_LINK_RECORD));

//
//	Create the template mask for the lookup.
//
	status = sa_create_template_mask(maip->base.aid, samad.header.mask);
	if (status != VSTATUS_OK) {
		IB_EXIT("sa_LinkRecord_GetTable", VSTATUS_OK);
		return(VSTATUS_OK);
	}

//
//	Find the LinkRecords in the SADB
//
	(void)vs_rdlock(&old_topology_lock);

	for_all_nodes(&old_topology, nodep) {
		for_all_physical_ports(nodep, portp) {
			if (!sm_valid_port(portp) || portp->state <= IB_PORT_DOWN) {
				continue;
			}

			if ((status = sa_check_len(data, sizeof(STL_LINK_RECORD), bytes)) != VSTATUS_OK) {
				maip->base.status = MAD_STATUS_SA_NO_RESOURCES;
				IB_LOG_ERROR_FMT( "sa_LinkRecord_GetTable",
					   "Reached size limit at %d records", *records);
				goto done;
			}

			if ((status = sa_LinkRecord_Set(data, nodep, portp)) != VSTATUS_OK) {
				maip->base.status = MAD_STATUS_SA_NO_RESOURCES;
				goto done;
			}

			(void)sa_template_test_mask(samad.header.mask, samad.data, &data, sizeof(STL_LINK_RECORD), bytes, records);
		}
	}

done:
	(void)vs_rwunlock(&old_topology_lock);

	IB_EXIT("sa_LinkRecord_GetTable", VSTATUS_OK);
	return(VSTATUS_OK);
}
