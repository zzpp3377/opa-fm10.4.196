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

#include "topology.h"
#include "topology_internal.h"
#include <stl_convertfuncs.h>
#include "stl_helper.h"
#include <limits.h>
#include <math.h>
#include <time.h>
#include <oib_utils_sa.h>
#include <oib_utils_pa.h>

#ifdef DBGPRINT
#undef DBGPRINT
#endif
#define DBGPRINT(format, args...) if (g_verbose_file) {fflush(stdout); fprintf(stderr, format, ##args); }


static int	g_skipswitchinfo= 0;	// workaround for open SM
static int	g_paclient_state = PACLIENT_UNKNOWN;	// PaClient/PaServer communications
static FILE *g_verbose_file = NULL;	// file for verbose output
static struct oib_port *g_portHandle = NULL;


/* get path from our portGuid to destination portp
 * cache path in portp, if called again report from cached value
 */
FSTATUS GetPathToPort(struct oib_port *port, EUI64 portGuid, PortData *portp)
{

	QUERY				query;
	FSTATUS status;
	PQUERY_RESULT_VALUES pQueryResults = NULL;
	IB_PATH_RECORD *pPR = &query.InputValue.PathRecordValue.PathRecord;

	if (portp->pathp)
		return FSUCCESS;	// already have path record

	if (! portp->PortGUID)
		return FINVALID_PARAMETER;	// not a directly accessible port


	memset(&query, 0, sizeof(query));	// initialize reserved fields
	query.InputType = InputTypePathRecord;
	query.InputValue.PathRecordValue.ComponentMask =
				IB_PATH_RECORD_COMP_DGID | IB_PATH_RECORD_COMP_SGID |
				IB_PATH_RECORD_COMP_PKEY |
				IB_PATH_RECORD_COMP_REVERSIBLE | IB_PATH_RECORD_COMP_NUMBPATH; 
	pPR->SGID.Type.Global.SubnetPrefix = oib_get_port_prefix(port);
	pPR->DGID.Type.Global.SubnetPrefix = oib_get_port_prefix(port);
	pPR->SGID.Type.Global.InterfaceID  = portGuid;
	pPR->DGID.Type.Global.InterfaceID  = portp->PortGUID;
	pPR->Reversible                    = 1;
	pPR->NumbPath                      = 1;
	pPR->P_Key = 0x7fff;

	query.OutputType 	= OutputTypePathRecord;

	DBGPRINT("Query: Input=%s, Output=%s\n",
				   		iba_sd_query_input_type_msg(query.InputType),
					   	iba_sd_query_result_type_msg(query.OutputType));

	// this call is synchronous
	status = oib_query_sa(port, &query, &pQueryResults);
	if (! pQueryResults)
	{
		fprintf(stderr, "%*sSA PathRecord query Failed: %s\n", 0, "", iba_fstatus_msg(status));
		goto fail;
	} else if (pQueryResults->Status != FSUCCESS) {
		fprintf(stderr, "%*sSA PathRecord query Failed: %s MadStatus 0x%x: %s\n", 0, "",
				iba_fstatus_msg(pQueryResults->Status),
			   	pQueryResults->MadStatus, iba_sd_mad_status_msg(pQueryResults->MadStatus));
		goto fail;
	} else if (pQueryResults->ResultDataSize == 0) {
		fprintf(stderr, "%*sNo Path Records Returned\n", 0, "");
		status = FNOT_FOUND;
	} else {
		PATH_RESULTS *p = (PATH_RESULTS*)pQueryResults->QueryResult;

		DBGPRINT("MadStatus 0x%x: %s\n", pQueryResults->MadStatus,
					   				iba_sd_mad_status_msg(pQueryResults->MadStatus));
		DBGPRINT("%d Bytes Returned\n", pQueryResults->ResultDataSize);
		if (p->NumPathRecords == 0) {
			fprintf(stderr, "%*sNo Path Records Returned\n", 0, "");
			status = FNOT_FOUND;
		}

        //DisplayPathRecord(&(p->PathRecords[0]), 0);
		/* we save just the 1st path record */
		portp->pathp = (IB_PATH_RECORD*)MemoryAllocate2AndClear(sizeof(IB_PATH_RECORD), IBA_MEM_FLAG_PREMPTABLE, MYTAG);
		if (! portp->pathp) {
			fprintf(stderr, "%s: Unable to allocate memory\n", g_Top_cmdname);
			goto fail;
		}
		*(portp->pathp) = p->PathRecords[0];
		status = FSUCCESS;
	}

done:
	// oib_query_sa will have allocated a result buffer
	// we must free the buffer when we are done with it
	if (pQueryResults)
		oib_free_query_result_buffer(pQueryResults);
	return status;

fail:
	// TBD verify this error always results in bad exit status g_exitstatus = 1;
	status = FERROR;
	goto done;

}




/* get path records between 2 ports
 * caller must oib_free_query_result_buffer(*ppQueryResults);
 */
FSTATUS GetPaths(struct oib_port *port,
				 PortData *portp1, 
				 PortData *portp2,
				 PQUERY_RESULT_VALUES *ppQueryResults)
{
	QUERY				query;
	FSTATUS status;

	*ppQueryResults = NULL;

	if (! portp1->PortGUID || ! portp2->PortGUID)
		return FINVALID_PARAMETER;	// not directly accessible ports

	memset(&query, 0, sizeof(query));	// initialize reserved fields
	query.InputType = InputTypePortGuidPair;
	query.InputValue.PortGuidPair.SourcePortGuid = portp1->PortGUID;
	query.InputValue.PortGuidPair.DestPortGuid = portp2->PortGUID;
	query.OutputType 	= OutputTypePathRecord;

	DBGPRINT("Query: Input=%s, Output=%s\n",
				   		iba_sd_query_input_type_msg(query.InputType),
					   	iba_sd_query_result_type_msg(query.OutputType));

	// this call is synchronous
	status = oib_query_sa(port, &query, ppQueryResults);

	if (! *ppQueryResults)
	{
		fprintf(stderr, "%*sSA PathRecord query Failed: %s\n", 0, "", iba_fstatus_msg(status));
		goto fail;
	} else if ((*ppQueryResults)->Status != FSUCCESS) {
		fprintf(stderr, "%*sSA PathRecord query Failed: %s MadStatus 0x%x: %s\n", 0, "",
				iba_fstatus_msg((*ppQueryResults)->Status),
			   	(*ppQueryResults)->MadStatus, iba_sd_mad_status_msg((*ppQueryResults)->MadStatus));
		goto fail;
	} else if ((*ppQueryResults)->ResultDataSize == 0) {
		fprintf(stderr, "%*sNo Path Records Returned\n", 0, "");
		status = FNOT_FOUND;
		goto fail;
	} else {
		PATH_RESULTS *p = (PATH_RESULTS*)(*ppQueryResults)->QueryResult;

		DBGPRINT("MadStatus 0x%x: %s\n", (*ppQueryResults)->MadStatus,
					   				iba_sd_mad_status_msg((*ppQueryResults)->MadStatus));
		DBGPRINT("%d Bytes Returned\n", (*ppQueryResults)->ResultDataSize);
		if (p->NumPathRecords == 0) {
			fprintf(stderr, "%*sNo Path Records Returned\n", 0, "");
			status = FNOT_FOUND;
			goto fail;
		}
        //DisplayPathRecord(&(p->PathRecords[0]), 0);

		/* caller can process *ppQueryResults */
		status = FSUCCESS;
	}

done:
	return status;

fail:
	// TBD verify this error always results in bad exit status g_exitstatus = 1;
	status = FERROR;
	// oib_query_sa will have allocated a result buffer
	// we must free the buffer when we are done with it
	if (*ppQueryResults) {
		oib_free_query_result_buffer(*ppQueryResults);
		*ppQueryResults = NULL;
	}
	goto done;
}

static void DisplayTraceRecord(STL_TRACE_RECORD *pTraceRecord, int indent)
{
    fprintf(g_verbose_file, "%*sIDGeneration: 0x%04x\n",
           indent, "", pTraceRecord->IDGeneration);
    fprintf(g_verbose_file, "%*sNodeType: 0x%02x\n",
           indent, "", pTraceRecord->NodeType);
    fprintf(g_verbose_file, "%*sNodeID: 0x%016"PRIx64" ChassisID: %016"PRIx64"\n",
           indent, "", pTraceRecord->NodeID, pTraceRecord->ChassisID);
    fprintf(g_verbose_file, "%*sEntryPortID: 0x%016"PRIx64" ExitPortID: %016"PRIx64"\n",
           indent, "", pTraceRecord->EntryPortID, pTraceRecord->ExitPortID);
    fprintf(g_verbose_file, "%*sEntryPort: 0x%02x ExitPort: 0x%02x\n",
           indent, "", pTraceRecord->EntryPort, pTraceRecord->ExitPort);
}


FSTATUS GetTraceRoute(struct oib_port *port,
					  IB_PATH_RECORD *pathp,
					  PQUERY_RESULT_VALUES *ppQueryResults)
{
	QUERY				query;
	FSTATUS status;

	*ppQueryResults = NULL;

	memset(&query, 0, sizeof(query));	// initialize reserved fields
	query.InputType = InputTypePathRecord;
	query.InputValue.PathRecordValue.PathRecord = *pathp;
	query.InputValue.PathRecordValue.PathRecord.NumbPath = 1;
	query.InputValue.PathRecordValue.ComponentMask = IB_PATH_RECORD_COMP_SERVICEID 
		| IB_PATH_RECORD_COMP_DGID | IB_PATH_RECORD_COMP_SGID 
		| IB_PATH_RECORD_COMP_DLID | IB_PATH_RECORD_COMP_SLID 
		| IB_PATH_RECORD_COMP_REVERSIBLE | IB_PATH_RECORD_COMP_NUMBPATH;
	query.OutputType 	= OutputTypeStlTraceRecord;

	DBGPRINT("Query: Input=%s, Output=%s\n",
				   		iba_sd_query_input_type_msg(query.InputType),
					   	iba_sd_query_result_type_msg(query.OutputType));

	// this call is synchronous
	status = oib_query_sa(port, &query, ppQueryResults);

	if (! *ppQueryResults)
	{
		fprintf(stderr, "%*sSA TraceRecord query Failed: %s\n", 0, "", iba_fstatus_msg(status));
		goto fail;
	} else if ((*ppQueryResults)->Status != FSUCCESS) {
		fprintf(stderr, "%*sSA TraceRecord query Failed: %s MadStatus 0x%x: %s\n", 0, "",
				iba_fstatus_msg((*ppQueryResults)->Status),
			   	(*ppQueryResults)->MadStatus, iba_sd_mad_status_msg((*ppQueryResults)->MadStatus));
		goto fail;
	} else if ((*ppQueryResults)->ResultDataSize == 0) {
		fprintf(stderr, "%*sNo Trace Records Data Returned\n", 0, "");
		status = FNOT_FOUND;
		goto fail;
	} else {
		STL_TRACE_RECORD_RESULTS *p = (STL_TRACE_RECORD_RESULTS*)(*ppQueryResults)->QueryResult;

		DBGPRINT("MadStatus 0x%x: %s\n", (*ppQueryResults)->MadStatus,
					   				iba_sd_mad_status_msg((*ppQueryResults)->MadStatus));
		DBGPRINT("%d Bytes Returned\n", (*ppQueryResults)->ResultDataSize);
		if (p->NumTraceRecords == 0) {
			fprintf(stderr, "%*sNo Trace Records Found\n", 0, "");
			status = FNOT_FOUND;
			goto fail;
		}

        //DisplayTraceRecord(&p->TraceRecords[0],0);

		/* caller can process *ppQueryResults */
		status = FSUCCESS;
	}

done:
	return status;

fail:
	status = FERROR;
	// oib_query_sa will have allocated a result buffer
	// we must free the buffer when we are done with it
	if (*ppQueryResults) {
		oib_free_query_result_buffer(*ppQueryResults);
		*ppQueryResults = NULL;
	}
	goto done;
}

/*
 * There are 6 cases for routes:
 * 1. CA - CA
 * 2. CA to self
 * 3. SW Port 0 to CA
 * 4. CA to SW Port 0
 * 5. SW Port 0 to SW Port 0
 * 6. SW Port 0 to self
 *
 * Two self consistent Perspectives of these cases:
 *
 * Perspective 1:  Show all "Links" along the route
 *   - every Link is a connection between 2 devices
 *   - every Link involves 2 Ports on different devices
 *   - never show SW Port 0 in a route
 *   - never show any ports for a "talk to self" route
 *   - similarly -F route:... would only select ports which -o route would show
 *
 * Perspective 2: Show all "Ports" along the route
 *   - route is a list of Ports (not Links)
 *   - show every port, including port 0 at start and/or end
 *   - for "talk to self" routes, show just the 1 port involved
 *   - similarly -F route:... would select all ports involved in the route
 *
 * The code below implements Perspective 1.  Some code in #if 0 and some
 * comments discuss possible approaches to implement perspective 2.
 * If the future, perspective 2 could become runtime if flags based on a
 * new parameter to this function.
 */

/* obtain and append to pPoint the trace route information for the given path
 * between the given pair of ports.
 * The ports are provided to aid in tranversing
 * the PortData and NodeData records and as an easy way to verify the
 * concistency of the trace route query results against our previous
 * port, node and link record queries.
 */
FSTATUS FindTraceRoute(struct oib_port *port,
					   EUI64 portGuid, 
					   FabricData_t *fabricp, 
					   PortData *portp1, 
					   PortData *portp2,
					   IB_PATH_RECORD *pathp, 
					   Point *pPoint)
{
	FSTATUS status;

	PQUERY_RESULT_VALUES pQueryResults = NULL;
	STL_TRACE_RECORD	*pTraceRecords = NULL;
	uint32 NumTraceRecords;
	int i = -1;
	PortData *p = portp1;
	int p_shown = 0;

	if (portp1 == portp2) {
		/* special case, internal loopback */
#if 0	// enable for perspective 2
		status = PointListAppendUniquePort(pPoint, portp1);
#else
		status = FSUCCESS;
#endif
		goto done;
	}
	if (portp1->neighbor == portp2) {
		/* special case, single link traversed */
		// Since portp1 has a neighbor, neither port is SW Port 0
		// same behavior for perspective 1 and 2
		status = PointListAppendUniquePort(pPoint, portp1);
		if (FSUCCESS == status)
			status = PointListAppendUniquePort(pPoint, portp2);
		goto done;
	}

	if (portGuid) {
		status = GetTraceRoute(port, pathp, &pQueryResults);
		if (FSUCCESS != status) {
			// this error results in bad exit status g_exitstatus = 1;
			goto done;
		}
		NumTraceRecords = ((STL_TRACE_RECORD_RESULTS*)pQueryResults->QueryResult)->NumTraceRecords;
		pTraceRecords = ((STL_TRACE_RECORD_RESULTS*)pQueryResults->QueryResult)->TraceRecords;
	} else {
		status = GenTraceRoutePath(fabricp, pathp, &pTraceRecords, &NumTraceRecords);
		if (FSUCCESS != status) {
			if (status == FUNAVAILABLE) {
				fprintf(stderr, "%s: Routing Tables not available\n",
							   	g_Top_cmdname);
				// this error results in bad exit status g_exitstatus = 1;
			} else if (status == FNOT_DONE) {
				DBGPRINT("Route Incomplete\n");
				// fprintf(stderr, "%s: Route Incomplete\n", g_Top_cmdname);
				// don't fail just because some routes are incomplete
				status = FSUCCESS;
			} else {
				DBGPRINT("Unable to determine route: (status=0x%x): %s\n", status, iba_fstatus_msg(status));
				// fprintf(stderr, "opareport: Unable to determine route: (status=0x%x): %s\n", status, iba_fstatus_msg(status));
				// don't fail just because some routes are unavailable
				// caller will fail if we match no devices for any of the routes
				// tried
				status = FSUCCESS;
			}
			goto done;
		}
	}

	//printf("%*s%d Hops\n", indent, "", pTrace->NumTraceRecords-1);

	ASSERT(NumTraceRecords > 0);

	/* the first Trace record should be the exit from portp1, however
	 * not all versions of the SM report this record
	 */
	if (pTraceRecords[0].NodeType != portp1->nodep->NodeInfo.NodeType) {
		/* workaround SM bug, did not report initial exit port */
		// assume portp1 is not a Switch Port 0
		p = portp1->neighbor;
		if (! p) {
			DBGPRINT("incorrect 1st trace record\n");
			goto badroute;
		}
		// same behavior for perspective 1 and 2
		status = PointListAppendUniquePort(pPoint, portp1);
		if (FSUCCESS != status)
			goto done;
	}
	for (i=0; i< NumTraceRecords; i++) {
		if (g_verbose_file)
			DisplayTraceRecord(&pTraceRecords[i], 0);
		if (p != portp1) {
			// same behavior for perspective 1 and 2
			status = PointListAppendUniquePort(pPoint, p);
			if (FSUCCESS != status)
				goto done;
			p_shown = 1;
		}
		if (pTraceRecords[i].NodeType != STL_NODE_FI) {
#if 0	// enable for perspective 2
			if (i == 0 && p == portp1) { // must be starting at switch Port 0
				status = PointListAppendUniquePort(pPoint, portp1);
				if (FSUCCESS != status)
					goto done;
			}
#endif
			p = FindNodePort(p->nodep, pTraceRecords[i].ExitPort);
			if (! p) {
				DBGPRINT("SW port not found\n");
				goto badroute;
			}
			if (0 == p->PortNum) {
				/* Switch Port 0 thus must be final port */
				if (i+1 != NumTraceRecords) {
					DBGPRINT("final switch port 0 error\n");
					goto badroute;
				}
#if 0	// enable for perspective 2
				status = PointListAppendUniquePort(pPoint, portp1);
				if (FSUCCESS != status)
					goto done;
#endif
				break;
			}
			// same behavior for perspective 1 and 2
			status = PointListAppendUniquePort(pPoint, p);
			if (FSUCCESS != status)
				goto done;
			if (p == portp2) {
				// this should not happen.  If we reach portp2 as the exit
				// port of a switch, that implies portp2 must be port 0 of
				// the switch which the test above should have caught
				// but it doesn't hurt to have this redundant test here to be
				// safe.
				/* final port must be Switch Port 0 */
				if (i+1 != NumTraceRecords) {
					DBGPRINT("final switch port 0 error\n");
					goto badroute;
				}
			} else {
				p = p->neighbor;
				if (! p) {
					DBGPRINT("incorrect neighbor port\n");
					goto badroute;
				}
				p_shown = 0;
			}
		} else if (i == 0) {
			/* since we caught CA to CA case above, SM must have given us
			 * initial Node in path
			 */
			// same behavior for perspective 1 and 2
			status = PointListAppendUniquePort(pPoint, portp1);
			if (FSUCCESS != status)
				goto done;
			/* unfortunately spec says Exit and Entry Port are 0 for CA, so
			 * can't verify consistency with portp1
			 */
			p = portp1->neighbor;
			if (! p) {
				DBGPRINT("1st port with no neighbor\n");
				goto badroute;
			}
			p_shown = 0;
		} else if (i+1 != NumTraceRecords) {
			DBGPRINT("extra unexpected trace records\n");
			goto badroute;
		}
	}
	if (! p_shown) {
		/* workaround SM bug, did not report final hop in route */
		// same behavior for perspective 1 and 2
		status = PointListAppendUniquePort(pPoint, p);
		if (FSUCCESS != status)
			goto done;
	}
	if (p != portp2) {
		DBGPRINT("ended at wrong port\n");
		goto badroute;
	}

done:

	if (pQueryResults)
		oib_free_query_result_buffer(pQueryResults);
	if (! portGuid && pTraceRecords)
		MemoryDeallocate(pTraceRecords);

	return status;

badroute:
	status = FSUCCESS;	// might as well process what we can
	fprintf(stderr, "%*sRoute reported by SM inconsistent with Trace Route\n", 0, "");
	if (g_verbose_file && i+1 < NumTraceRecords) {
    	fprintf(g_verbose_file, "%*sRemainder of Route:\n", 0, "");
		// Don't repeat records we already output above
		for (i=i+1; i< NumTraceRecords; i++)
			DisplayTraceRecord(&pTraceRecords[i], 4);
	}
	goto done;
}

/* find trace routes for all paths between 2 given ports */
FSTATUS FindPortsTraceRoutes(struct oib_port *port,
							 EUI64 portGuid, 
							 FabricData_t *fabricp, 
							 PortData *portp1, 
							 PortData *portp2, 
							 Point *pPoint)
{

	PQUERY_RESULT_VALUES pQueryResults = NULL;
	uint32 NumPathRecords;
	IB_PATH_RECORD *pPathRecords = NULL;
	FSTATUS status;
	int i;

	if (portGuid) {
		status = GetPaths(port, portp1, portp2, &pQueryResults);
		if (FSUCCESS != status)
			goto done;
		NumPathRecords = ((PATH_RESULTS*)pQueryResults->QueryResult)->NumPathRecords;
		pPathRecords = ((PATH_RESULTS*)pQueryResults->QueryResult)->PathRecords;
	} else {
		status = GenPaths(fabricp, portp1, portp2, &pPathRecords, &NumPathRecords);
		if (FSUCCESS != status)
			goto done;
	}


	for (i=0; i< NumPathRecords; i++) {
		status = FindTraceRoute(port, portGuid, fabricp, portp1, portp2, &pPathRecords[i], pPoint);
		if (FSUCCESS != status)
			return status;
	}

done:
	if (pQueryResults)
		oib_free_query_result_buffer(pQueryResults);
	if (! portGuid && pPathRecords)
		MemoryDeallocate(pPathRecords);

	return status;
}

/* find trace routes for all paths between given node and point */
FSTATUS FindPortNodeTraceRoutes(struct oib_port *port,
								EUI64 portGuid, 
								FabricData_t *fabricp, 
								PortData *portp1, 
								NodeData *nodep2, 
								Point *pPoint)
{
	cl_map_item_t *p;
	FSTATUS status;

	for (p=cl_qmap_head(&nodep2->Ports); p != cl_qmap_end(&nodep2->Ports); p = cl_qmap_next(p)) {
		PortData *portp2= PARENT_STRUCT(p, PortData, NodePortsEntry);
		status = FindPortsTraceRoutes(port, portGuid, fabricp, portp1, portp2, pPoint);
		if (FSUCCESS != status)
			return status;
	}
	return FSUCCESS;
}

/* find trace routes for all paths between given port and point */
FSTATUS FindPortPointTraceRoutes(struct oib_port *port,
								 EUI64 portGuid, 
								 FabricData_t *fabricp, 
								 PortData *portp1, 
								 Point *point2, 
								 Point *pPoint)
{
	FSTATUS status;

	switch (point2->Type) {
	case POINT_TYPE_PORT:
		return FindPortsTraceRoutes(port, portGuid, fabricp, portp1, point2->u.portp, pPoint);
	case POINT_TYPE_PORT_LIST:
		{
		LIST_ITERATOR i;
		DLIST *pList = &point2->u.portList;

		for (i=ListHead(pList); i != NULL; i = ListNext(pList, i)) {
			PortData *portp = (PortData*)ListObj(i);
			status = FindPortsTraceRoutes(port, portGuid, fabricp, portp1, portp, pPoint);
			if (FSUCCESS != status)
				return status;
		}
		}
		return FSUCCESS;
	case POINT_TYPE_NODE:
		return FindPortNodeTraceRoutes(port, portGuid, fabricp, portp1, point2->u.nodep, pPoint);
	case POINT_TYPE_NODE_LIST:
		{
		LIST_ITERATOR i;
		DLIST *pList = &point2->u.nodeList;

		for (i=ListHead(pList); i != NULL; i = ListNext(pList, i)) {
			NodeData *nodep = (NodeData*)ListObj(i);
			status = FindPortNodeTraceRoutes(port, portGuid, fabricp, portp1, nodep, pPoint);
			if (FSUCCESS != status)
				return status;
		}
		}
		return FSUCCESS;
#if !defined(VXWORKS) || defined(BUILD_DMC)
	case POINT_TYPE_IOC:
		return FindPortNodeTraceRoutes(port, portGuid, fabricp, portp1, point2->u.iocp->ioup->nodep, pPoint);
	case POINT_TYPE_IOC_LIST:
		{
		LIST_ITERATOR i;
		DLIST *pList = &point2->u.nodeList;

		for (i=ListHead(pList); i != NULL; i = ListNext(pList, i)) {
			IocData *iocp = (IocData*)ListObj(i);
			status = FindPortNodeTraceRoutes(port, portGuid, fabricp, portp1, iocp->ioup->nodep, pPoint);
			if (FSUCCESS != status)
				return status;
		}
		}
		return FSUCCESS;
#endif
	case POINT_TYPE_SYSTEM:
		{
		cl_map_item_t *p;
		SystemData *systemp = point2->u.systemp;

		for (p=cl_qmap_head(&systemp->Nodes); p != cl_qmap_end(&systemp->Nodes); p = cl_qmap_next(p)) {
			NodeData *nodep = PARENT_STRUCT(p, NodeData, SystemNodesEntry);
			status = FindPortNodeTraceRoutes(port, portGuid, fabricp, portp1, nodep, pPoint);
			if (FSUCCESS != status)
				return status;
		}
		return FSUCCESS;
		}
	default:
		return FINVALID_PARAMETER;
	}
}

/* find trace routes for all paths between given node and point */
FSTATUS FindNodePointTraceRoutes(struct oib_port *port,
								 EUI64 portGuid, 
								 FabricData_t *fabricp, 
								 NodeData *nodep1, 
								 Point *point2, 
								 Point *pPoint)
{
	cl_map_item_t *p;
	FSTATUS status;

	for (p=cl_qmap_head(&nodep1->Ports); p != cl_qmap_end(&nodep1->Ports); p = cl_qmap_next(p)) {
		PortData *portp1 = PARENT_STRUCT(p, PortData, NodePortsEntry);
		status = FindPortPointTraceRoutes(port, portGuid, fabricp, portp1, point2, pPoint);
		if (FSUCCESS != status)
			return status;
	}
	return FSUCCESS;
}

/* find all ports in trace routes for all paths between 2 given points */
FSTATUS FindPointsTraceRoutes(struct oib_port *port,
							  EUI64 portGuid, 
							  FabricData_t *fabricp, 
							  Point *point1, 
							  Point *point2, 
							  Point *pPoint)
{
	FSTATUS status;

	switch (point1->Type) {
	case POINT_TYPE_PORT:
		return FindPortPointTraceRoutes(port, portGuid, fabricp, point1->u.portp, point2, pPoint);
	case POINT_TYPE_PORT_LIST:
		{
		LIST_ITERATOR i;
		DLIST *pList = &point1->u.portList;

		for (i=ListHead(pList); i != NULL; i = ListNext(pList, i)) {
			PortData *portp = (PortData*)ListObj(i);
			status = FindPortPointTraceRoutes(port, portGuid, fabricp, portp, point2, pPoint);
			if (FSUCCESS != status)
				return status;
		}
		}
		return FSUCCESS;
	case POINT_TYPE_NODE:
		return FindNodePointTraceRoutes(port, portGuid, fabricp, point1->u.nodep, point2, pPoint);
	case POINT_TYPE_NODE_LIST:
		{
		LIST_ITERATOR i;
		DLIST *pList = &point1->u.nodeList;

		for (i=ListHead(pList); i != NULL; i = ListNext(pList, i)) {
			NodeData *nodep = (NodeData*)ListObj(i);
			status = FindNodePointTraceRoutes(port, portGuid, fabricp, nodep, point2, pPoint);
			if (FSUCCESS != status)
				return status;
		}
		}
		return FSUCCESS;
#if !defined(VXWORKS) || defined(BUILD_DMC)
	case POINT_TYPE_IOC:
		return FindNodePointTraceRoutes(port, portGuid, fabricp, point1->u.iocp->ioup->nodep, point2, pPoint);
	case POINT_TYPE_IOC_LIST:
		{
		LIST_ITERATOR i;
		DLIST *pList = &point1->u.iocList;

		for (i=ListHead(pList); i != NULL; i = ListNext(pList, i)) {
			IocData *iocp = (IocData*)ListObj(i);
			status = FindNodePointTraceRoutes(port, portGuid, fabricp, iocp->ioup->nodep, point2, pPoint);
			if (FSUCCESS != status)
				return status;
		}
		}
		return FSUCCESS;
#endif
	case POINT_TYPE_SYSTEM:
		{
		cl_map_item_t *p;
		SystemData *systemp = point1->u.systemp;

		for (p=cl_qmap_head(&systemp->Nodes); p != cl_qmap_end(&systemp->Nodes); p = cl_qmap_next(p)) {
			NodeData *nodep = PARENT_STRUCT(p, NodeData, SystemNodesEntry);
			status = FindNodePointTraceRoutes(port, portGuid, fabricp, nodep, point2, pPoint);
			if (FSUCCESS != status)
				return status;
		}
		return FSUCCESS;
		}
	default:
		return FINVALID_PARAMETER;
	}
}

static FSTATUS ParseRoutePoint(struct oib_port *port,
							   EUI64 portGuid, 
							   FabricData_t *fabricp, 
							   char* arg, 
							   Point* pPoint, 
							   char **pp)
{
	Point SrcPoint;
	Point DestPoint;
	FSTATUS status;

	ASSERT(! PointValid(pPoint));
	PointInit(&SrcPoint);
	PointInit(&DestPoint);

	if (arg == *pp) {
		fprintf(stderr, "%s: Invalid route format: '%s'\n", g_Top_cmdname, arg);
		return FINVALID_PARAMETER;
	}
	status = ParsePoint(fabricp, arg, &SrcPoint, FIND_FLAG_FABRIC, pp);
	if (FSUCCESS != status)
		return status;
	if (**pp != ':') {
		fprintf(stderr, "%s: Invalid route format: '%s'\n", g_Top_cmdname, arg);
		return FINVALID_PARAMETER;
	}
	(*pp)++;
	status = ParsePoint(fabricp, *pp, &DestPoint, FIND_FLAG_FABRIC, pp);
	if (FSUCCESS != status)
		return status;

	// now we have 2 valid points, add to pPoint all the Ports in all routes
	// between those points
	/* TBD - cleanup use of global */
	status = FindPointsTraceRoutes(port, portGuid, fabricp, &SrcPoint, &DestPoint, pPoint);
	PointDestroy(&SrcPoint);
	PointDestroy(&DestPoint);
	if (FSUCCESS != status)
		return status;
	if (! PointValid(pPoint)) {
		fprintf(stderr, "%s: Unable to resolve route: '%s'\n",
					   	g_Top_cmdname, arg);
		return FNOT_FOUND;
	}
	PointCompress(pPoint);
	return FSUCCESS;
}

// focus point syntax also allows route: format
FSTATUS ParseFocusPoint(EUI64 portGuid, 
						FabricData_t *fabricp, 
						char* arg, 
						Point* pPoint, 
						uint8 find_flag,
						char **pp, 
						boolean allow_route)
{
	char* param;
	struct oib_port *oib_port_session = NULL;
	FSTATUS fstatus = FSUCCESS;

	*pp = arg;
	PointInit(pPoint);
	if (NULL != (param = ComparePrefix(arg, "route:"))) {
		if (! allow_route || ! (find_flag & FIND_FLAG_FABRIC)) {
			fprintf(stderr, "%s: Format Not Allowed: '%s'\n", g_Top_cmdname, arg);
			fstatus = FINVALID_PARAMETER;
		} else {
			fstatus = oib_open_port_by_guid(&oib_port_session, portGuid);
			if (fstatus != FSUCCESS) {
				fprintf(stderr, "%s: Unable to open fabric interface.\n",
						g_Top_cmdname);
			} else {
				fstatus = ParseRoutePoint(oib_port_session, portGuid, fabricp, 
										  param, pPoint, pp);
				oib_close_port(oib_port_session);
			}
		}
	} else {
		fstatus = ParsePoint(fabricp, arg, pPoint, find_flag, pp);
	}

	return fstatus;
}

/* get master SM data from SM service record (if available) */
FSTATUS GetMasterSMData(struct oib_port *port, 
						EUI64 portGuid, 
						FabricData_t *fabricp, 
						SweepFlags_t flags, 
						int quiet)
{
	int					ix;
	FSTATUS				status;
	QUERY				query;
	PQUERY_RESULT_VALUES pQueryResults = NULL;
	uint32 NumServiceRecords;
	IB_SERVICE_RECORD *pServiceRecords;

	memset(&fabricp->MasterSMData, 0, sizeof(MasterSMData_t));	// clear master SM data
	memset(&query, 0, sizeof(query));	// initialize reserved fields
	query.InputType 	= InputTypeNoInput;
	query.OutputType 	= OutputTypeServiceRecord;

	DBGPRINT("Query: Input=%s, Output=%s\n",
				   		iba_sd_query_input_type_msg(query.InputType),
					   	iba_sd_query_result_type_msg(query.OutputType));

	// this call is synchronous
	status = oib_query_sa(port, &query, &pQueryResults);

	if (! pQueryResults)
	{
		fprintf(stderr, "%*sSA ServiceRecord query Failed: %s\n", 0, "", iba_fstatus_msg(status));
		goto fail;
	} else if (pQueryResults->Status != FSUCCESS) {
		fprintf(stderr, "%*sSA ServiceRecord query Failed: %s MadStatus 0x%x: %s\n", 0, "",
				iba_fstatus_msg(pQueryResults->Status),
			   	pQueryResults->MadStatus, iba_sd_mad_status_msg(pQueryResults->MadStatus));
		goto fail;
	} else if (pQueryResults->ResultDataSize == 0) {
		fprintf(stderr, "%*sNo Service Records Returned\n", 0, "");
		status = FUNAVAILABLE;
	} else {
		DBGPRINT("MadStatus 0x%x: %s\n", pQueryResults->MadStatus,
					   				iba_sd_mad_status_msg(pQueryResults->MadStatus));
		DBGPRINT("%d Bytes Returned\n", pQueryResults->ResultDataSize);
		NumServiceRecords = ((SERVICE_RECORD_RESULTS*)pQueryResults->QueryResult)->NumServiceRecords;
		pServiceRecords = ((SERVICE_RECORD_RESULTS*)pQueryResults->QueryResult)->ServiceRecords;

		for (ix = 0; ix < NumServiceRecords; ++ix)
		{
			if (pServiceRecords[ix].RID.ServiceID == SM_SERVICE_ID)
			{
				fabricp->MasterSMData.serviceID = pServiceRecords[ix].RID.ServiceID;
				fabricp->MasterSMData.version = pServiceRecords[ix].ServiceData8[0];
				fabricp->MasterSMData.capabilityMask = pServiceRecords[ix].ServiceData32[3];
				status = FSUCCESS;
				break;
			}
		}
		status = FUNAVAILABLE;
	}

done:
	// oib_query_sa will have allocated a result buffer
	// we must free the buffer when we are done with it
	if (pQueryResults)
		oib_free_query_result_buffer(pQueryResults);

	return status;

fail:
	status = FERROR;
	goto done;

}	// End of GetMasterSMData()

/* query SMA directly for Node Records for given LID
 * on fabric connected to
 * given HFI port and put results into pPorts
 */
static FSTATUS GetNodeRecordDirect(struct oib_port *port, 
								   EUI64 portGuid, 
								   FabricData_t *fabricp, 
								   NodeData *nodep, 
								   uint32 lid)
{
	FSTATUS status;
	STL_NODE_DESCRIPTION NodeDesc;
	STL_NODE_INFO NodeInfo;

	status= SmaGetNodeDesc(port, nodep, lid, &NodeDesc);
	if (status != FSUCCESS)
	{
		fprintf(stderr, "%*sSMA Get(NodeDesc) Failed to LID 0x%x Node 0x%016"PRIx64" Name: %.*s: %s\n", 0, "", lid,
			nodep->NodeInfo.NodeGUID,
			STL_NODE_DESCRIPTION_ARRAY_SIZE,
			(char*)nodep->NodeDesc.NodeString, iba_fstatus_msg(status));
		goto fail;
	}

	status= SmaGetNodeInfo(port, nodep, lid, &NodeInfo);
	if (status != FSUCCESS)
	{
		fprintf(stderr, "%*sSMA Get(NodeInfo) Failed to LID 0x%x Node 0x%016"PRIx64" Name: %.*s: %s\n", 0, "", lid,
			nodep->NodeInfo.NodeGUID,
			STL_NODE_DESCRIPTION_ARRAY_SIZE,
			(char*)nodep->NodeDesc.NodeString, iba_fstatus_msg(status));
		goto fail;
	}

	nodep->NodeDesc = NodeDesc;
	nodep->NodeInfo = NodeInfo;
	return FSUCCESS;

fail:
	return FERROR;
}

static FSTATUS GetAllBCTDirect(struct oib_port *port,
									  FabricData_t *fabricp,
									  Point *focus,
									  int quiet)
{
	cl_map_item_t *p;
	FSTATUS status = FSUCCESS;
	int ix_node;
	int numNodes = cl_qmap_count(&fabricp->AllNodes);

	if (! quiet) ProgressPrint(TRUE, "Getting All Buffer Control Tables...");

	for ( p = cl_qmap_head(&fabricp->AllNodes), ix_node = 0; p != cl_qmap_end(&fabricp->AllNodes);
			p = cl_qmap_next(p), ix_node++ )
	{
		NodeData *nodep = PARENT_STRUCT(p, NodeData, AllNodesEntry);
		uint8_t numPorts = nodep->NodeInfo.NumPorts;

		if (ix_node%PROGRESS_FREQ == 0)
			if (! quiet) ProgressPrint(FALSE, "Processed %6d of %6d Nodes...", ix_node, numNodes);
		if (focus && ! CompareNodePoint(nodep, focus))
			continue;

		if (nodep->NodeInfo.NodeType == STL_NODE_SW)
		{
				// skip port 0
			uint8_t p;
			STL_BUFFER_CONTROL_TABLE *pBCT = malloc((numPorts) * sizeof(*pBCT));
			PortData *portp = FindNodePort(nodep, 0);

			if (!portp) {
				fprintf(stderr, "%*sSMA Get(BufferControlTable %u %u) Failed to Find Port Data"
								"for Node 0x%016"PRIx64" Name: %.*s: %s\n", 0, "",
								0, numPorts, nodep->NodeInfo.NodeGUID,
								STL_NODE_DESCRIPTION_ARRAY_SIZE,
								(char*)nodep->NodeDesc.NodeString, iba_fstatus_msg(status));
				free(pBCT);
				goto done;
			} else {
				status = SmaGetBufferControlTable(port, nodep, portp->EndPortLID, 1, numPorts, pBCT);
			} 
			if (status != FSUCCESS)
			{
				fprintf(stderr, "%*sSMA Get(BufferControlTable %u %u) Failed to LID 0x%x "
								"Node 0x%016"PRIx64" Name: %.*s: %s\n", 0, "",
								0, numPorts, portp->EndPortLID, nodep->NodeInfo.NodeGUID,
								STL_NODE_DESCRIPTION_ARRAY_SIZE,
								(char*)nodep->NodeDesc.NodeString, iba_fstatus_msg(status));
				free(pBCT);
				goto done;
			} else {
				for (p = 1; p <= numPorts; p++) {
					portp = FindNodePort(nodep, p);
					if (!portp)
						continue;
					// data is undefined for down ports
					if (portp->PortInfo.PortStates.s.PortState == IB_PORT_DOWN)
						continue;
					if (! portp->pBufCtrlTable) {
						if ((status = PortDataAllocateBufCtrlTable(fabricp, portp)) != FSUCCESS)
							continue;
					}

					memcpy(portp->pBufCtrlTable, &pBCT[p-1], sizeof(*portp->pBufCtrlTable));
				}
			}
			free(pBCT);
		} else {
			uint8_t p;
			STL_BUFFER_CONTROL_TABLE bct;
			for (p = 1; p <= numPorts; p++) {
				PortData *portp = FindNodePort(nodep, p);
				if (!portp)
					continue;

				status = SmaGetBufferControlTable(port, nodep, portp->EndPortLID, p, p, &bct);
				if (status != FSUCCESS)
				{
					fprintf(stderr, "%*sSMA Get(BufferControlTable %u) Failed to LID 0x%x "
									"Node 0x%016"PRIx64" Name: %.*s: %s\n", 0, "",
									p, portp->EndPortLID, nodep->NodeInfo.NodeGUID,
									STL_NODE_DESCRIPTION_ARRAY_SIZE,
									(char*)nodep->NodeDesc.NodeString, iba_fstatus_msg(status));
					goto done;
				} else {
					if (! portp->pBufCtrlTable) {
						if ((status = PortDataAllocateBufCtrlTable(fabricp, portp)) != FSUCCESS)
							continue;
					}
					memcpy(portp->pBufCtrlTable, &bct, sizeof(*portp->pBufCtrlTable));
				}
			}
		}
	}

done:
	if (! quiet) ProgressPrint(TRUE, "Done Getting Buffer Control Tables");

	return status;
}

static FSTATUS GetAllBCTSA(struct oib_port *port,
								  FabricData_t *fabricp,
								  Point *focus,
								  int quiet)
{
	QUERY				query;
	PQUERY_RESULT_VALUES pQueryResults = NULL;
	FSTATUS status = FSUCCESS;

	if (! quiet) ProgressPrint(FALSE, "Getting All Buffer Control Tables...");

	/* Query all BCT records... */
	memset(&query, 0, sizeof(query));
	query.InputType = InputTypeNoInput;
	query.OutputType 	= OutputTypeStlBufCtrlTabRecord;

	DBGPRINT("Query: Input=%s, Output=%s\n",
				iba_sd_query_input_type_msg(query.InputType),
				iba_sd_query_result_type_msg(query.OutputType));

	status = oib_query_sa(port, &query, &pQueryResults);
	if (! pQueryResults)
	{
		fprintf(stderr, "%*sSA BufferControlTableRecord query Failed: %s\n", 0, "",
					iba_fstatus_msg(status));
		status = FERROR;
	} else if (pQueryResults->Status != FSUCCESS) {
		fprintf(stderr, "%*sSA BufferControlTableRecord query Failed: %s MadStatus 0x%x: %s\n", 0, "",
				iba_fstatus_msg(pQueryResults->Status),
				pQueryResults->MadStatus, iba_sd_mad_status_msg(pQueryResults->MadStatus));
		status = FERROR;
	} else if (pQueryResults->ResultDataSize == 0) {
		fprintf(stderr, "%*sNo BufferControlTableRecord Records Returned\n", 0, "");
	} else {
		int i;
		STL_BUFFER_CONTROL_TABLE_RECORD_RESULTS *result =
			((STL_BUFFER_CONTROL_TABLE_RECORD_RESULTS*)pQueryResults->QueryResult);
		STL_BUFFER_CONTROL_TABLE_RECORD *pBCTRecords;
		uint32_t numRecords;

		DBGPRINT("MadStatus 0x%x: %s\n", pQueryResults->MadStatus,
					iba_sd_mad_status_msg(pQueryResults->MadStatus));
		DBGPRINT("%d Bytes Returned\n", pQueryResults->ResultDataSize);

		numRecords = result->NumBufferControlRecords;
		pBCTRecords = result->BufferControlRecords;

		/* ... and place them within the fabric structure */
		for (i=0; i<numRecords; i++)
		{
			PortData *port = FindLid(fabricp, pBCTRecords[i].RID.LID);

			if (!port)
				continue;

			if (port->nodep->NodeInfo.NodeType == STL_NODE_SW)
				port = FindNodePort(port->nodep, pBCTRecords[i].RID.Port);

			if (!port)
				continue;

			if (focus && !ComparePortPoint(port, focus))
				continue;
			if (! port->pBufCtrlTable) {
				if ((status = PortDataAllocateBufCtrlTable(fabricp, port)) != FSUCCESS)
					continue;
			}

			memcpy(port->pBufCtrlTable, &pBCTRecords[i].BufferControlTable,
				sizeof(*port->pBufCtrlTable));
		}
	}

	// oib_query_port_fabric will have allocated a result buffer
	// we must free the buffer when we are done with it
	if (pQueryResults)
		oib_free_query_result_buffer(pQueryResults);

	if (! quiet) ProgressPrint(TRUE, "Done Getting Buffer Control Tables");
	return status;
}

FSTATUS GetAllBCTs(EUI64 portGuid, FabricData_t *fabricp, Point *focus, int quiet)
{
	struct oib_port *oib_port_session = NULL;
	FSTATUS fstatus = FSUCCESS;

	fstatus = oib_open_port_by_guid(&oib_port_session, portGuid);
	if (fstatus != FSUCCESS) {
		fprintf(stderr, "%s: Unable to open fabric interface.\n",
				g_Top_cmdname);
	} else {
		if (fabricp->flags & FF_SMADIRECT) {
			fstatus = GetAllBCTDirect(oib_port_session, fabricp, focus, quiet);
		} else {
			fstatus = GetAllBCTSA(oib_port_session, fabricp, focus, quiet);
		}
		oib_close_port(oib_port_session);
	}

	if (fstatus == FSUCCESS)
		fabricp->flags |= FF_BUFCTRLTABLE;

	return fstatus;
}

/* query all PortInfo Records for given LID on fabric connected to
 * given HFI port and put results into pPorts
 */
static FSTATUS GetNodePorts(struct oib_port *port,
							FabricData_t *fabricp, 
							NodeData *nodep, 
							cl_qmap_t *pPorts, 
							EUI64 guid, 
							uint32 lid)
{
	QUERY				query;
	PQUERY_RESULT_VALUES pQueryResults = NULL;
	uint32 NumPortInfoRecords;
	STL_PORTINFO_RECORD *pPortInfoRecords;
	FSTATUS status;
	int i;

	memset(&query, 0, sizeof(query));	// initialize reserved fields
	query.InputType = InputTypeLid;
    query.InputValue.Lid = lid;
	query.OutputType 	= OutputTypeStlPortInfoRecord;

	DBGPRINT("Query: Input=%s, Output=%s\n",
				   		iba_sd_query_input_type_msg(query.InputType),
					   	iba_sd_query_result_type_msg(query.OutputType));

	// this call is synchronous
	status = oib_query_sa(port, &query, &pQueryResults);
	if (! pQueryResults)
	{
		fprintf(stderr, "%*sSA PortInfo query Failed: %s\n", 0, "", iba_fstatus_msg(status));
		goto fail;
	} else if (pQueryResults->Status != FSUCCESS) {
		fprintf(stderr, "%*sSA PortInfo query Failed: %s MadStatus 0x%x: %s\n", 0, "",
				iba_fstatus_msg(pQueryResults->Status),
			   	pQueryResults->MadStatus, iba_sd_mad_status_msg(pQueryResults->MadStatus));
		goto fail;
	} else if (pQueryResults->ResultDataSize == 0) {
		fprintf(stderr, "%*sNo PortInfo Records Returned\n", 0, "");
	} else {
		DBGPRINT("MadStatus 0x%x: %s\n", pQueryResults->MadStatus,
					   				iba_sd_mad_status_msg(pQueryResults->MadStatus));
		DBGPRINT("%d Bytes Returned\n", pQueryResults->ResultDataSize);
		NumPortInfoRecords = ((STL_PORTINFO_RECORD_RESULTS*)pQueryResults->QueryResult)->NumPortInfoRecords;
		pPortInfoRecords = ((STL_PORTINFO_RECORD_RESULTS*)pQueryResults->QueryResult)->PortInfoRecords;

		for (i=0; i<NumPortInfoRecords; ++i)
		{
			if (pPortInfoRecords[i].PortInfo.PortStates.s.PortState == IB_PORT_DOWN)
			{
				DBGPRINT("skip down port\n");
				continue;
			}
			if (NULL == NodeDataAddPort(fabricp, nodep, guid, &pPortInfoRecords[i]))
				goto fail;
			//DisplayPortInfoRecord(&pPortInfoRecords[i], 0);
		}
	}
	status = FSUCCESS;

done:
	// oib_query_sa will have allocated a result buffer
	// we must free the buffer when we are done with it
	if (pQueryResults)
		oib_free_query_result_buffer(pQueryResults);

	return status;

fail:
	status = FERROR;
	goto done;
}

/* query SMA directly for all PortInfo Records for given LID
 * on fabric connected to
 * given HFI port and put results into pPorts
 */
static FSTATUS GetNodePortsDirect(struct oib_port *port, 
								  FabricData_t *fabricp, 
								  NodeData *nodep, 
								  cl_qmap_t *pPorts, 
								  EUI64 guid, uint32 lid)
{
	STL_PORTINFO_RECORD PortInfoRecord = {{0}};
	FSTATUS status;

	if (nodep->NodeInfo.NodeType == STL_NODE_SW)
   	{
		unsigned i;
		for (i=0; i<= nodep->NodeInfo.NumPorts; i++)
	   	{
			status= SmaGetPortInfo(port, nodep, lid, i, &PortInfoRecord.PortInfo);
			if (status != FSUCCESS)
			{
				fprintf(stderr, "%*sSMA Get(PortInfo %u) Failed to LID 0x%x Node 0x%016"PRIx64" Name: %.*s: %s\n", 0, "", i, lid,
					nodep->NodeInfo.NodeGUID,
					STL_NODE_DESCRIPTION_ARRAY_SIZE,
					(char*)nodep->NodeDesc.NodeString, iba_fstatus_msg(status));
				goto fail;
			}
			if (! (fabricp->flags & FF_DOWNPORTINFO)
				&& PortInfoRecord.PortInfo.PortStates.s.PortState == IB_PORT_DOWN)
			{
				DBGPRINT("skip down port\n");
				continue;
			}
			PortInfoRecord.RID.EndPortLID = lid;
			PortInfoRecord.RID.PortNum = i;
			if (NULL == NodeDataAddPort(fabricp, nodep, guid, &PortInfoRecord))
				goto fail;
			//DisplayPortInfoRecord(&PortInfo, 0);
		}
	} else {
		/* router or channel adapter */
		status= SmaGetPortInfo(port, nodep, lid, 0, &PortInfoRecord.PortInfo);
		if (status != FSUCCESS)
		{
			fprintf(stderr, "%*sSMA Get(PortInfo %u) Failed to LID 0x%x Node 0x%016"PRIx64" Name: %.*s: %s\n", 0, "", 0, lid,
				nodep->NodeInfo.NodeGUID,
				STL_NODE_DESCRIPTION_ARRAY_SIZE,
				(char*)nodep->NodeDesc.NodeString, iba_fstatus_msg(status));
			goto fail;
		}
		PortInfoRecord.RID.EndPortLID = lid;
		PortInfoRecord.RID.PortNum = PortInfoRecord.PortInfo.LocalPortNum;
		if (NULL == NodeDataAddPort(fabricp, nodep, guid, &PortInfoRecord))
			goto fail;
		//DisplayPortInfoRecord(&PortInfo, 0);
	}
	return FSUCCESS;

fail:
	return FERROR;
}

/* query all down ports on switch nodes in fabric directly from SMA
 * Note: It would have been wonderful if we could have used focus to limit the
 * scope of this scan.  However many of the focus formats have options to select
 * individual ports and that is performed once after Sweep and before reports.
 * The focus selection occurs by using the FabricData and searching it for
 * matching points.  As such there is a catch 22 so when we are asked to report
 * all down ports, we must scan them all, even if a focus was specified.
 */
static FSTATUS GetAllDownPortsDirect(struct oib_port *port,
									 FabricData_t *fabricp,
									 int quiet)
{
	FSTATUS	status = FSUCCESS;
	int ix_node;

	cl_map_item_t *p;

	int num_nodes = cl_qmap_count(&fabricp->AllNodes);

	if (! quiet) ProgressPrint(TRUE, "Getting All Down Switch Ports...");
	for ( p=cl_qmap_head(&fabricp->AllNodes), ix_node = 0; p != cl_qmap_end(&fabricp->AllNodes);
			p = cl_qmap_next(p), ix_node++ )
	{
		NodeData *nodep = PARENT_STRUCT(p, NodeData, AllNodesEntry);

		if (ix_node%PROGRESS_FREQ == 0)
			if (! quiet) ProgressPrint(FALSE, "Processed %6d of %6d Nodes...", ix_node, num_nodes);

		// Process switch nodes
		if (nodep->NodeInfo.NodeType == STL_NODE_SW) {
			uint32 lid = nodep->pSwitchInfo->RID.LID;
			uint64 guid = nodep->NodeInfo.PortGUID;	// SW only used on port 0
			STL_PORTINFO_RECORD PortInfoRecord = {{0}};
			unsigned i;
			cl_map_item_t *q;

			// Switch Port 0 should always have been found so start at 1
			for (i=1, q=cl_qmap_head(&nodep->Ports); i<= nodep->NodeInfo.NumPorts; ) {
				PortData *portp;
 				if (q != cl_qmap_end(&nodep->Ports)) {
					portp = PARENT_STRUCT(q, PortData, NodePortsEntry);
					if (portp->PortNum <= i)
					{
						q = cl_qmap_next(q);
						if (portp->PortNum == i)
							i++;
						continue;	/* skip already found switch ports */
					}
				}
				// port i not in DB
				status= SmaGetPortInfo(port, nodep, lid, i, &PortInfoRecord.PortInfo);
				if (status != FSUCCESS)
				{
					fprintf(stderr, "%*sSMA Get(PortInfo %u) Failed to LID 0x%x Node 0x%016"PRIx64" Name: %.*s: %s\n", 0, "", i, lid,
						nodep->NodeInfo.NodeGUID,
						STL_NODE_DESCRIPTION_ARRAY_SIZE,
						(char*)nodep->NodeDesc.NodeString, iba_fstatus_msg(status));
					goto fail;
				}
				PortInfoRecord.RID.EndPortLID = lid;
				PortInfoRecord.RID.PortNum = i;
				// only switch port 0 have a guid
				portp = NodeDataAddPort(fabricp, nodep, guid, &PortInfoRecord);
				if (NULL == portp)
					goto fail;
				//DisplayPortInfoRecord(&PortInfo, 0);
fail:
				i++;
			}
		}	// End of if (nodep->NodeInfo.NodeType == STL_NODE_SW

	}	// End of for ( p=cl_qmap_head(&fabricp->AllNodes)
	status = FSUCCESS;	// don't let failure to get some devices stop everything

	if (! quiet) ProgressPrint(TRUE, "Done Getting All Down Switch Ports");

	return (status);

}	// End of GetAllDownPorts()

/* query all down ports on switch nodes in fabric
 */
static FSTATUS GetAllDownPorts(struct oib_port *port,
								 EUI64 portGuid,
								 FabricData_t *fabricp,
								 int quiet)
{
	// We must get direct from SMA, SA only tracks Active ports
	return GetAllDownPortsDirect(port, fabricp, quiet);
}

/* if applicable, get the Switch information for the given node */
static FSTATUS GetNodeSwitchInfo(struct oib_port *port,
								 NodeData *nodep, 
								 IB_LID lid)
{
	QUERY				query;
	FSTATUS status;
	PQUERY_RESULT_VALUES pQueryResults = NULL;

	if (nodep->NodeInfo.NodeType != STL_NODE_SW
		|| g_skipswitchinfo)
		return FSUCCESS;

	memset(&query, 0, sizeof(query));	// initialize reserved fields
	query.InputType 	= InputTypeLid;
	query.InputValue.Lid = lid;
	query.OutputType 	= OutputTypeStlSwitchInfoRecord;

	DBGPRINT("Query: Input=%s, Output=%s\n",
				   		iba_sd_query_input_type_msg(query.InputType),
					   	iba_sd_query_result_type_msg(query.OutputType));

	// this call is synchronous
	status = oib_query_sa(port, &query, &pQueryResults);
	if (! pQueryResults)
	{
		fprintf(stderr, "%*sSA SwitchInfo query Failed: %s\n", 0, "", iba_fstatus_msg(status));
		goto fail;
	} else if (pQueryResults->Status != FSUCCESS) {
		fprintf(stderr, "%*sSA SwitchInfo query Failed: %s MadStatus 0x%x: %s\n", 0, "",
				iba_fstatus_msg(pQueryResults->Status),
			   	pQueryResults->MadStatus, iba_sd_mad_status_msg(pQueryResults->MadStatus));
		goto fail;
	} else if (pQueryResults->ResultDataSize == 0) {
		fprintf(stderr, "%*sNo SwitchInfo Records Returned\n", 0, "");
		status = FNOT_FOUND;
	} else {
		STL_SWITCHINFO_RECORD_RESULTS *p = (STL_SWITCHINFO_RECORD_RESULTS*)pQueryResults->QueryResult;

		DBGPRINT("MadStatus 0x%x: %s\n", pQueryResults->MadStatus,
					   				iba_sd_mad_status_msg(pQueryResults->MadStatus));
		DBGPRINT("%d Bytes Returned\n", pQueryResults->ResultDataSize);
		if (p->NumSwitchInfoRecords != 1) {
			status = FNOT_FOUND;
			goto fail;
		}
		status = NodeDataSetSwitchInfo(nodep, &p->SwitchInfoRecords[0]);
	}

done:
	// oib_query_sa will have allocated a result buffer
	// we must free the buffer when we are done with it
	if (pQueryResults)
		oib_free_query_result_buffer(pQueryResults);
	return status;

fail:
	g_skipswitchinfo= 1;	// workaround for open SM
	status = FERROR;
	goto done;

}

/* if applicable, get the Switch information for the given node */
/* query SMA directly for Switch Info for given LID
 * on fabric connected to
 * given HFI port and put results into pPorts
 */
static FSTATUS GetNodeSwitchInfoDirect(struct oib_port *port, NodeData *nodep, uint32 lid)
{
	FSTATUS status;
	STL_SWITCHINFO_RECORD SwitchInfoRecord;

	if (nodep->NodeInfo.NodeType != STL_NODE_SW)
		return FSUCCESS;

	status= SmaGetSwitchInfo(port, nodep, lid, &SwitchInfoRecord.SwitchInfoData);
	if (status != FSUCCESS)
	{
		fprintf(stderr, "%*sSMA Get(SwitchInfo) Failed to LID 0x%x Node 0x%016"PRIx64" Name: %.*s: %s\n", 0, "", lid,
			nodep->NodeInfo.NodeGUID,
			STL_NODE_DESCRIPTION_ARRAY_SIZE,
			(char*)nodep->NodeDesc.NodeString, iba_fstatus_msg(status));
		goto fail;
	}
	SwitchInfoRecord.RID.LID = lid;

	status = NodeDataSetSwitchInfo(nodep, &SwitchInfoRecord);
	return FSUCCESS;

fail:
	return FERROR;
}

#if !defined(VXWORKS) || defined(BUILD_DMC)
static FSTATUS GetIocServices(struct oib_port *port, IocData *iocp, PortData *portp)
{
	FSTATUS status = FSUCCESS;
	uint32 first;

	if (! iocp->IocProfile.ServiceEntries)
		goto done;

	iocp->Services = (IOC_SERVICE*)MemoryAllocate2AndClear(sizeof(IOC_SERVICE)*iocp->IocProfile.ServiceEntries, IBA_MEM_FLAG_PREMPTABLE, MYTAG);
	if (! iocp->Services) {
		status = FINSUFFICIENT_MEMORY;
		goto done;
	}
	for (first=0; first < iocp->IocProfile.ServiceEntries; first+=4) {
		uint8 last = MIN(first+3, iocp->IocProfile.ServiceEntries-1);

		/* ignore errors */
		(void)DmGetServiceEntries(port, portp->pathp,
								  iocp->IocSlot, first, last, &iocp->Services[first]);
	}

done:
	return status;
}

/* get the IOC information for the given IOU */
static FSTATUS GetIouIocs(struct oib_port *port, FabricData_t *fabricp, IouData *ioup, PortData *portp)
{
	uint8 slot;
	FSTATUS status;

	for (slot=1; slot <= ioup->IouInfo.MaxControllers; slot++) {
		IocData *iocp;
		uint8 ioc_status = IOC_AT_SLOT(&ioup->IouInfo, slot);

		if (ioc_status != IOC_INSTALLED)
			continue;

		iocp = (IocData*)MemoryAllocate2AndClear(sizeof(IocData), IBA_MEM_FLAG_PREMPTABLE, MYTAG);
		if (! iocp) {
			status = FINSUFFICIENT_MEMORY;
			goto done;
		}

		iocp->IocSlot = slot;
		iocp->ioup = ioup;
		ListItemInitState(&iocp->IouIocsEntry);
		QListSetObj(&iocp->IouIocsEntry, iocp);

		status = DmGetIocProfile(port, portp->pathp, slot, &iocp->IocProfile);
		if (FSUCCESS != status) {
			/* skip that IOC */
			MemoryDeallocate(iocp);
			continue;
		}
		if (cl_qmap_insert(&fabricp->AllIOCs, iocp->IocProfile.IocGUID, &iocp->AllIOCsEntry) != &iocp->AllIOCsEntry)
		{
			fprintf(stderr, "%s: Duplicate IOC Guids found in IocProfiles: 0x%016"PRIx64", skipping\n",
						   	g_Top_cmdname, iocp->IocProfile.IocGUID);
			MemoryDeallocate(iocp);
			continue;
		}
		(void)GetIocServices(port, iocp, portp);
		QListInsertTail(&ioup->Iocs, &iocp->IouIocsEntry);
	}
done:
	return FSUCCESS;
}

/* if applicable, get the IOU and IOC information for the given node */
static FSTATUS GetNodeIous(struct oib_port *port, 
						   EUI64 portGuid, 
						   FabricData_t *fabricp, 
						   NodeData *nodep)
{
	FSTATUS status = FSUCCESS;
	PortData *portp;
	IouData *ioup;

	/* all ports should report same IOU and IOC info, just use 1st port */
	if (cl_qmap_head(&nodep->Ports) == cl_qmap_end(&nodep->Ports))
		goto done;	/* no ports */

	portp = PARENT_STRUCT(cl_qmap_head(&nodep->Ports), PortData, NodePortsEntry);
	if (! portp->PortInfo.CapabilityMask.s.IsDeviceManagementSupported)
		goto done;	/* no Device Mgmt Agent */

	status = GetPathToPort(port, portGuid, portp);
	if (FSUCCESS != status)
		goto done;

	ioup = (IouData*)MemoryAllocate2AndClear(sizeof(IouData), IBA_MEM_FLAG_PREMPTABLE, MYTAG);
	if (! ioup) {
		status = FINSUFFICIENT_MEMORY;
		goto done;
	}

	status = DmGetIouInfo(port, portp->pathp, &ioup->IouInfo);
	if (FSUCCESS != status) {
		MemoryDeallocate(ioup);
		goto done;
	}
	ListItemInitState(&ioup->AllIOUsEntry);
	QListSetObj(&ioup->AllIOUsEntry, ioup);
	ioup->nodep = nodep;
	QListInitState(&ioup->Iocs);
	if (! QListInit(&ioup->Iocs))
	{
		MemoryDeallocate(ioup);
		status = FINSUFFICIENT_RESOURCES;
		goto done;
	}
	nodep->ioup = ioup;
	status = GetIouIocs(port, fabricp, ioup, portp);
done:
	return status;
}
#endif

static FSTATUS GetPortCableInfoDirect(struct oib_port *port,
									  FabricData_t *fabricp,
									  PortData *portp,
									  int quiet)
{
	FSTATUS status = FSUCCESS;
	uint8_t cableInfo[STL_CIB_STD_LEN];
	uint16_t addr;
	uint8_t *data;

	if (! IsCableInfoAvailable(&portp->PortInfo))
		return FSUCCESS;

	for (addr = STL_CIB_STD_HIGH_PAGE_ADDR, data=cableInfo;
		 addr + STL_CABLE_INFO_MAXLEN <= STL_CIB_STD_END_ADDR; addr += STL_CABLE_INFO_DATA_SIZE, data += STL_CABLE_INFO_DATA_SIZE)
	{
		status = SmaGetCableInfo(port, portp->nodep, portp->EndPortLID, portp->PortNum, addr, STL_CABLE_INFO_MAXLEN, data);
		if (status != FSUCCESS) {
			fprintf(stderr, "%s: SMA Get(CableInfo) Failed to LID 0x%x Node 0x%016"PRIx64" for port %u. Name: %.*s: %s\n",
					g_Top_cmdname, portp->EndPortLID, portp->nodep->NodeInfo.NodeGUID, portp->PortNum,
					STL_NODE_DESCRIPTION_ARRAY_SIZE, (char*)portp->nodep->NodeDesc.NodeString,
					iba_fstatus_msg(status));
			break;
		}
	}
	if (status != FSUCCESS)
		return status;
	if (! portp->pCableInfoData) {
		if ((status = PortDataAllocateCableInfoData(fabricp, portp)) != FSUCCESS)
			return status;
	}

	memcpy(portp->pCableInfoData, cableInfo, sizeof(cableInfo));
	return FSUCCESS;
}

static FSTATUS GetAllCablesDirect(struct oib_port *port,
									  FabricData_t *fabricp,
									  int skip_init_ports,
									  int quiet)
{
	cl_map_item_t *p;
	FSTATUS status = FSUCCESS;
	int ix_node;
	int numNodes = cl_qmap_count(&fabricp->AllNodes);

	if (! quiet) ProgressPrint(TRUE, "Getting All Cable Info...");

	for ( p = cl_qmap_head(&fabricp->AllNodes), ix_node = 0; p != cl_qmap_end(&fabricp->AllNodes);
			p = cl_qmap_next(p), ix_node++ )
	{
		NodeData *nodep = PARENT_STRUCT(p, NodeData, AllNodesEntry);
		uint8_t numPorts = nodep->NodeInfo.NumPorts;
		uint8_t portNum;

		if (ix_node%PROGRESS_FREQ == 0)
			if (! quiet) ProgressPrint(FALSE, "Processed %6d of %6d Nodes...", ix_node, numNodes);

		// switch port 0 has no cable, so just do external ports on switch
		// or all ports on HFI
		for (portNum = 1; portNum <= numPorts; portNum++) {
			PortData *portp = FindNodePort(nodep, portNum);
			if (!portp)
				continue;

			if (skip_init_ports && IsPortInitialized(portp->PortInfo.PortStates))
				continue;
			(void)GetPortCableInfoDirect(port, fabricp, portp, quiet);
		}
	}
	status = FSUCCESS;

	if (! quiet) ProgressPrint(TRUE, "Done Getting Cable Info");

	return status;
}

/* query all CableInfo Records on fabric connected to given HFI port
 * and put results into PortData's CableInfo.
 */
static FSTATUS GetAllCablesSA(struct oib_port *port,
							FabricData_t *fabricp,
							int quiet)
{
	FSTATUS				 status;
	QUERY				 query = {0};
	PQUERY_RESULT_VALUES pQueryResults = NULL;

	query.InputType = InputTypeNoInput;
	query.OutputType = OutputTypeStlCableInfoRecord;
	
	if (!quiet) ProgressPrint(FALSE, "Getting All Cable Info Records...");
	
	DBGPRINT("Query: Input=%s, Output=%s\n",
				   		iba_sd_query_input_type_msg(query.InputType),
					   	iba_sd_query_result_type_msg(query.OutputType));

	// this call is synchronous
	status = oib_query_sa(port, &query, &pQueryResults);

	if (!pQueryResults) {
		fprintf(stderr, "%*sSA CableInfo Record query Faile: %s\n", 0, "", iba_fstatus_msg(status));
		goto fail;
	} else if (pQueryResults->Status != FSUCCESS) {
		fprintf(stderr, "%*sSA CableInfo Record query Failed: %s MadStatus 0x%x: %s\n", 0, "",
				iba_fstatus_msg(pQueryResults->Status),
			   	pQueryResults->MadStatus, iba_sd_mad_status_msg(pQueryResults->MadStatus));
		goto fail;
	} else if (pQueryResults->ResultDataSize == 0) {
		fprintf(stderr, "%*sNo Cable Info Records Returned\n", 0, "");
	} else {
		STL_CABLE_INFO_RECORD_RESULTS *p = (STL_CABLE_INFO_RECORD_RESULTS*)pQueryResults->QueryResult;
		unsigned int i;
		
		DBGPRINT("MadStatus 0x%x: %s\n", pQueryResults->MadStatus,
					   				iba_sd_mad_status_msg(pQueryResults->MadStatus));
		DBGPRINT("%d Bytes Returned\n", pQueryResults->ResultDataSize);

		for (i=0; i < p->NumCableInfoRecords; ++i) {
			STL_CABLE_INFO_RECORD *pCableInfoRecord = &p->CableInfoRecords[i];
			PortData *portp;

			portp = FindLidPort(fabricp, pCableInfoRecord->LID, pCableInfoRecord->Port);
			if (!portp) {
				fprintf(stderr, "%s: Can't find Lid 0x%x Port %u: Skipping\n",
						g_Top_cmdname, pCableInfoRecord->LID, pCableInfoRecord->Port);
				continue;
			}
			
			if (pCableInfoRecord->u1.s.Address < STL_CIB_STD_HIGH_PAGE_ADDR
				|| pCableInfoRecord->u1.s.Address > STL_CIB_STD_END_ADDR) {
				fprintf(stderr, "%s: Cable Info Data Address 0x%x is outside of utilities range on node with"
						" Lid 0x%x Port %u: Ignoring\n", 
						g_Top_cmdname, pCableInfoRecord->u1.s.Address, pCableInfoRecord->LID,
						pCableInfoRecord->Port);
				continue;
			}

			if (! portp->pCableInfoData) {
				if ((status = PortDataAllocateCableInfoData(fabricp, portp)) != FSUCCESS)
					continue;
			}

			memcpy(portp->pCableInfoData
						+ pCableInfoRecord->u1.s.Address-STL_CIB_STD_HIGH_PAGE_ADDR,
					pCableInfoRecord->Data, 
					MIN(sizeof(pCableInfoRecord->Data),
						MIN(pCableInfoRecord->Length + 1,
							 STL_CIB_STD_LEN - (pCableInfoRecord->u1.s.Address-STL_CIB_STD_HIGH_PAGE_ADDR))));
		}
	}
	status = FSUCCESS;
	if (!quiet) ProgressPrint(TRUE, "Done Getting All Cable Info Records");
	
done:
	if (pQueryResults)
		oib_free_query_result_buffer(pQueryResults);
	return status;

fail:
	status = FERROR;
	goto done;
}

FSTATUS GetAllCables(struct oib_port *port,
						EUI64 portGuid, 
 						FabricData_t *fabricp,
						int quiet)
{
	FSTATUS fstatus = FSUCCESS;

	if (fabricp->flags & FF_SMADIRECT) {
		fstatus = GetAllCablesDirect(port, fabricp, FALSE, quiet);
	} else {
		fstatus = GetAllCablesSA(port, fabricp, quiet);
		if (fabricp->flags & FF_DOWNPORTINFO) {
			fstatus = GetAllCablesDirect(port, fabricp, TRUE, quiet);
		}
	}

	return fstatus;
}

/* query all multicast groups Records on fabric connected to given HFI port
 * for a given MGID and put results into AllMcGroupMember.
 */

FSTATUS GetAllMCGroupMember(FabricData_t *fabricp, McGroupData *mcgroupp, struct oib_port *portp,
				int quiet, FILE *g_verbose_file)
{

	QUERY query;
	FSTATUS status;
	PQUERY_RESULT_VALUES pQueryResults = NULL;
	LIST_ITEM *p;

	memset(&query, 0, sizeof(query));	// initialize reserved fields
	query.InputType = InputTypeMcGid;
	query.InputValue.Gid.AsReg64s.H= mcgroupp->MGID.AsReg64s.H;
	query.InputValue.Gid.AsReg64s.L= mcgroupp->MGID.AsReg64s.L;
	query.OutputType =  OutputTypeMcMemberRecord;

	DBGPRINT("Query: Input=%s, Output=%s\n",
			iba_sd_query_input_type_msg(query.InputType),
			iba_sd_query_result_type_msg(query.OutputType));

	// this call is synchronous
	status = oib_query_sa(portp, &query, &pQueryResults);

	if (! pQueryResults) {
		fprintf(stderr, "%*sSA McmemberRecord query Failed: %s\n", 0, "", iba_fstatus_msg(status));
		status = FERROR;
		// oib_query_sa will have allocated a result buffer
		// we must free the buffer when we are done with it
		return status;
	} else if (pQueryResults->Status != FSUCCESS) {
		fprintf(stderr, "%*sSA McMemberRecord query Failed: %s MadStatus 0x%x: %s\n", 0, "",
			iba_fstatus_msg(pQueryResults->Status),
			pQueryResults->MadStatus, iba_sd_mad_status_msg(pQueryResults->MadStatus));
		status = FERROR;
		// oib_query_sa will have allocated a result buffer
		// we must free the buffer when we are done with it
		if (pQueryResults)
			oib_free_query_result_buffer(pQueryResults);
		return status;
	} else if (pQueryResults->ResultDataSize == 0) {
		fprintf(stderr, "%*sNo multicast group Records Returned\n", 0, "");
		//release result buffer
		if (pQueryResults)
			oib_free_query_result_buffer(pQueryResults);
		status = FUNAVAILABLE;
		return status;

	} else {

		MCMEMBER_RECORD_RESULTS  *pIbMCRR = (MCMEMBER_RECORD_RESULTS*)pQueryResults->QueryResult;

		DBGPRINT("MadStatus 0x%x: %s\n", pQueryResults->MadStatus,
									iba_sd_mad_status_msg(pQueryResults->MadStatus));
		DBGPRINT("%d Bytes Returned\n", pQueryResults->ResultDataSize);

		mcgroupp->NumOfMembers=pIbMCRR->NumMcMemberRecords;

		int i;

		for (i=0; i<pIbMCRR->NumMcMemberRecords; ++i) {

			McMemberData *mcmemberp = (McMemberData*)MemoryAllocate2AndClear(sizeof(McMemberData), IBA_MEM_FLAG_PREMPTABLE, MYTAG);
			if (! mcmemberp) {
				status = FINSUFFICIENT_MEMORY;
				return status;
			}

			mcmemberp->MemberInfo = pIbMCRR->McMemberRecords[i];
			mcmemberp->MemberInfo.MLID = pIbMCRR->McMemberRecords[i].MLID;
			mcmemberp->MemberInfo.RID.MGID = pIbMCRR->McMemberRecords[i].RID.MGID;
			mcmemberp->MemberInfo.RID.PortGID = pIbMCRR->McMemberRecords[i].RID.PortGID;
			mcmemberp->pPort = FindPortGuid(fabricp, pIbMCRR->McMemberRecords[i].RID.PortGID.AsReg64s.L );

			if (mcmemberp->pPort && mcmemberp->pPort->neighbor) {
				if (mcmemberp->pPort->neighbor->nodep->NodeInfo.NodeType == STL_NODE_SW) {
					NodeData *groupswitch = mcmemberp->pPort->neighbor->nodep;
					uint16 switchentryport = mcmemberp->pPort->neighbor->PortNum ;
					AddEdgeSwitchToGroup(fabricp, mcgroupp, groupswitch, switchentryport );
				}
			}
			if ((mcmemberp->MemberInfo.RID.PortGID.AsReg64s.H == 0) && (mcmemberp->MemberInfo.RID.PortGID.AsReg64s.L ==0 ))
				mcgroupp->NumOfMembers--; // do count as valid member if PortGID is zero
			QListSetObj(&mcmemberp->McMembersEntry, mcmemberp);

// this linear insertion needs to be optimized
			boolean found = FALSE;
			p=QListHead(&mcgroupp->AllMcGroupMembers);
			// insert everything in the fabric structure ordered by PortGID
			while (!found && (p != NULL)) {
				McMemberData *pMGM = (McMemberData *)QListObj(p);
				if ( pMGM->MemberInfo.RID.PortGID.AsReg64s.H > mcmemberp->MemberInfo.RID.PortGID.AsReg64s.H)
					p = QListNext(&mcgroupp->AllMcGroupMembers, p);
				else if (pMGM->MemberInfo.RID.PortGID.AsReg64s.H == mcmemberp->MemberInfo.RID.PortGID.AsReg64s.H) {
					if (pMGM->MemberInfo.RID.PortGID.AsReg64s.L > mcmemberp->MemberInfo.RID.PortGID.AsReg64s.L)
						p = QListNext(&mcgroupp->AllMcGroupMembers, p);
					else {
						// insert mc-group-member element
						QListInsertNext(&mcgroupp->AllMcGroupMembers,p, &mcmemberp->McMembersEntry);
						found = TRUE;
					}
				}
				else {
					QListInsertNext(&mcgroupp->AllMcGroupMembers,p, &mcmemberp->McMembersEntry);
					found = TRUE;
				}
			} // end while
			if (!found)
				QListInsertTail(&mcgroupp->AllMcGroupMembers, &mcmemberp->McMembersEntry);
		} // for end

		//set group properties
		p=QListHead(&mcgroupp->AllMcGroupMembers);

		McMemberData *pmcmem = (McMemberData *)QListObj(p);
		mcgroupp->GroupInfo = pmcmem->MemberInfo;

	} // end else
	status = FSUCCESS;
	return status;
}

FSTATUS GetMCGroups(struct oib_port *port,
			EUI64 portGuid,
			FabricData_t *fabricp,
			int quiet, FILE *m_verbose_file)
{

	QUERY				 query;
	FSTATUS 			 status;
	PQUERY_RESULT_VALUES pQueryResults = NULL;

	memset(&query, 0, sizeof(query));	// initialize reserved fields
	query.InputType 	= InputTypeNoInput;
	query.OutputType 	=  OutputTypeMcMemberRecord;


	if (! quiet) ProgressPrint(TRUE, "Getting All MC Records...");

	DBGPRINT("Query: Input=%s, Output=%s\n",
						iba_sd_query_input_type_msg(query.InputType),
						iba_sd_query_result_type_msg(query.OutputType));

	// this call is synchronous
	status = oib_query_sa(port, &query, &pQueryResults);

	if (! pQueryResults) {
		fprintf(stderr, "%*sSA McmemberRecord query Failed: %s\n", 0, "", iba_fstatus_msg(status));
		status = FERROR;
		// oib_query_sa will have allocated a result buffer
		// we must free the buffer when we are done with it
		if (pQueryResults)
			oib_free_query_result_buffer(pQueryResults);
		return status;
	} else if (pQueryResults->Status != FSUCCESS) {
		fprintf(stderr, "%*sSA McMemberRecord query Failed: %s MadStatus 0x%x: %s\n", 0, "",
				iba_fstatus_msg(pQueryResults->Status),
				pQueryResults->MadStatus, iba_sd_mad_status_msg(pQueryResults->MadStatus));
		status = FERROR;
		// oib_query_sa will have allocated a result buffer
		// we must free the buffer when we are done with it
		if (pQueryResults)
			oib_free_query_result_buffer(pQueryResults);
		return status;
	} else if (pQueryResults->ResultDataSize == 0) {
		fprintf(stderr, "%*sNo multicast group Records Returned\n", 0, "");
		//release result buffer
		if (pQueryResults)
			oib_free_query_result_buffer(pQueryResults);
		status = FUNAVAILABLE;
		return status;
	} else { //// add different mcmember record whether is stl or the older version
		MCMEMBER_RECORD_RESULTS *pIbMCRR = (MCMEMBER_RECORD_RESULTS*)pQueryResults->QueryResult;
		DBGPRINT("MadStatus 0x%x: %s\n", pQueryResults->MadStatus,
									iba_sd_mad_status_msg(pQueryResults->MadStatus));
		DBGPRINT("%d Bytes Returned\n", pQueryResults->ResultDataSize);

		fabricp->NumOfMcGroups = pIbMCRR->NumMcMemberRecords;

		int i;
		for (i=0; i< pIbMCRR->NumMcMemberRecords; ++i)
		{
			McGroupData	*mcgroupp;
			boolean 	new_node;

			if (i%PROGRESS_FREQ == 0)
				if (! quiet) ProgressPrint(FALSE, "Processed %6d of %6d MC Records...", i, pIbMCRR->NumMcMemberRecords);

			//collect member information for each McRecord, create the corresponding group and add it to the fabric structure
			mcgroupp = FabricDataAddMCGroup(fabricp, port, quiet, &pIbMCRR->McMemberRecords[i], &new_node, m_verbose_file);
			if (!mcgroupp) {
				// oib_query_sa will have allocated a result buffer
				// we must free the buffer when we are done with it
				if (pQueryResults)
					oib_free_query_result_buffer(pQueryResults);
				return FERROR;
			}

			// do not count as groups in fabric those with no members
			McMemberData *pMCGH = (McMemberData *)QListObj(QListHead(&mcgroupp->AllMcGroupMembers));
			if ((pMCGH->MemberInfo.RID.PortGID.AsReg64s.H ==0) && (pMCGH->MemberInfo.RID.PortGID.AsReg64s.L==0))
				fabricp->NumOfMcGroups--;
		} // for end

	} // end else
	if (! quiet) ProgressPrint(TRUE, "Done Getting All MC Records");

	return FSUCCESS;

}

FSTATUS GetAllMCGroups(EUI64 portGuid, FabricData_t *fabricp, Point *focus, int quiet)
{
	struct oib_port *oib_port_session = NULL;
	FSTATUS fstatus = FSUCCESS;

	fstatus = oib_open_port_by_guid(&oib_port_session, portGuid);
	if (fstatus != FSUCCESS)
		fprintf(stderr, "%s: Unable to open fabric interface.\n", g_Top_cmdname);
	else  {
		fstatus = GetMCGroups(oib_port_session, portGuid, fabricp, quiet, g_verbose_file);
		oib_close_port(oib_port_session);
	}

	return fstatus;
}

/* query all NodeInfo Records on fabric connected to given HFI port
 * and put results into fabricp->AllNodes
 */
static FSTATUS GetAllNodes(struct oib_port *port, 
						   EUI64 portGuid, 
						   FabricData_t *fabricp, 
						   SweepFlags_t flags, 
						   int quiet)
{
	QUERY				query;
	FSTATUS status;
	PQUERY_RESULT_VALUES pQueryResults = NULL;

	memset(&query, 0, sizeof(query));	// initialize reserved fields
	query.InputType 	= InputTypeNoInput;
	query.OutputType 	= OutputTypeStlNodeRecord;

	if (! quiet) ProgressPrint(TRUE, "Getting All Node Records...");
	DBGPRINT("Query: Input=%s, Output=%s\n",
				   		iba_sd_query_input_type_msg(query.InputType),
					   	iba_sd_query_result_type_msg(query.OutputType));

	// this call is synchronous
	status = oib_query_sa(port, &query, &pQueryResults);

	if (! pQueryResults)
	{
		fprintf(stderr, "%*sSA NodeRecord query Failed: %s\n", 0, "", iba_fstatus_msg(status));
		goto fail;
	} else if (pQueryResults->Status != FSUCCESS) {
		fprintf(stderr, "%*sSA NodeRecord query Failed: %s MadStatus 0x%x: %s\n", 0, "",
				iba_fstatus_msg(pQueryResults->Status),
			   	pQueryResults->MadStatus, iba_sd_mad_status_msg(pQueryResults->MadStatus));
		goto fail;
	} else if (pQueryResults->ResultDataSize == 0) {
		fprintf(stderr, "%*sNo Node Records Returned\n", 0, "");
	} else {
		STL_NODE_RECORD_RESULTS *p = (STL_NODE_RECORD_RESULTS*)pQueryResults->QueryResult;
		int i;

		DBGPRINT("MadStatus 0x%x: %s\n", pQueryResults->MadStatus,
					   				iba_sd_mad_status_msg(pQueryResults->MadStatus));
		DBGPRINT("%d Bytes Returned\n", pQueryResults->ResultDataSize);
		for (i=0; i<p->NumNodeRecords; ++i)
		{
			NodeData *nodep;
			boolean new_node;

			if (i%PROGRESS_FREQ == 0)
				if (! quiet) ProgressPrint(FALSE, "Processed %6d of %6d Nodes...", i, p->NumNodeRecords);
			nodep = FabricDataAddNode(fabricp, &p->NodeRecords[i], &new_node);
			if (!nodep) {
				goto fail;
			}
			if (new_node && fabricp->flags & FF_SMADIRECT) {
				// replace node record data with actual SMA data
				(void)GetNodeRecordDirect(port, portGuid, fabricp, nodep, p->NodeRecords[i].RID.LID);
			}
			//printf("process NodeRecord LID: 0x%x\n", p->NodeRecords[i].RID.LID);
			//DisplayNodeRecord(&p->NodeRecords[i], 0);

			// we get 1 NodeRecord per port on a node, AddNode will only save
			// 1 NodeData structure per node and discard the duplicates
			// (but we need to process their corresponding ports)
			if (fabricp->flags & FF_SMADIRECT)
				status = GetNodePortsDirect(port, fabricp, nodep, &nodep->Ports, p->NodeRecords[i].NodeInfo.PortGUID, p->NodeRecords[i].RID.LID);
			else
				status = GetNodePorts(port, fabricp, nodep, &nodep->Ports, p->NodeRecords[i].NodeInfo.PortGUID, p->NodeRecords[i].RID.LID);
			if (status != FSUCCESS)
			{
				// TBD - better handling cleanup of all previous Ports for node
				cl_qmap_remove_item(&fabricp->AllNodes, &nodep->AllNodesEntry);
				MemoryDeallocate(nodep);
				goto fail;
			}

			// if this was the 1st time we saw the node
			if (new_node) {
				UpdateNodePmaCapabilities(nodep, TRUE);
#if !defined(VXWORKS) || defined(BUILD_DMC)
				if (flags & SWEEP_IOUS)
					(void)GetNodeIous(port, portGuid, fabricp, nodep);
#endif
				if (flags & SWEEP_SWITCHINFO) {
					if (fabricp->flags & FF_SMADIRECT) {
						(void)GetNodeSwitchInfoDirect(port, nodep, p->NodeRecords[i].RID.LID);
					} else {
						(void)GetNodeSwitchInfo(port, nodep, p->NodeRecords[i].RID.LID);
					}
				}
			}
		}
	}
	if (! quiet) ProgressPrint(TRUE, "Done Getting All Node Records");
	if (fabricp->flags & FF_DOWNPORTINFO) {
		if (FSUCCESS != (status = GetAllDownPorts(port, portGuid, fabricp, quiet)))
			goto done;
	}
	BuildFabricDataLists(fabricp);
	status = FSUCCESS;

done:
	// oib_query_sa will have allocated a result buffer
	// we must free the buffer when we are done with it
	if (pQueryResults)
		oib_free_query_result_buffer(pQueryResults);
	return status;

fail:
	status = FERROR;
	goto done;
}

/* query all Link Records on fabric connected to given HFI port
 * and put results into PortData entries
 */
static FSTATUS GetAllLinks(struct oib_port *port,
						   EUI64 portGuid, 
						   FabricData_t *fabricp, 
						   int quiet)
{
	QUERY				query;
	FSTATUS status;
	PQUERY_RESULT_VALUES pQueryResults = NULL;

	memset(&query, 0, sizeof(query));	// initialize reserved fields
	query.InputType 	= InputTypeNoInput;
	query.OutputType 	= OutputTypeStlLinkRecord;

	if (! quiet) ProgressPrint(FALSE, "Getting All Link Records...");
	DBGPRINT("Query: Input=%s, Output=%s\n",
				   		iba_sd_query_input_type_msg(query.InputType),
					   	iba_sd_query_result_type_msg(query.OutputType));

	// this call is synchronous
	status = oib_query_sa(port, &query, &pQueryResults);

	if (! pQueryResults)
	{
		fprintf(stderr, "%*sSA LinkRecord query Failed: %s\n", 0, "", iba_fstatus_msg(status));
		goto fail;
	} else if (pQueryResults->Status != FSUCCESS) {
		fprintf(stderr, "%*sSA LinkRecord query Failed: %s MadStatus 0x%x: %s\n", 0, "",
				iba_fstatus_msg(pQueryResults->Status),
			   	pQueryResults->MadStatus, iba_sd_mad_status_msg(pQueryResults->MadStatus));
		goto fail;
	} else if (pQueryResults->ResultDataSize == 0) {
		fprintf(stderr, "%*sNo Link Records Returned\n", 0, "");
	} else {
		STL_LINK_RECORD_RESULTS *p = (STL_LINK_RECORD_RESULTS*)pQueryResults->QueryResult;
		int i;

		DBGPRINT("MadStatus 0x%x: %s\n", pQueryResults->MadStatus,
					   				iba_sd_mad_status_msg(pQueryResults->MadStatus));
		DBGPRINT("%d Bytes Returned\n", pQueryResults->ResultDataSize);
		for (i=0; i<p->NumLinkRecords; ++i)
		{
			STL_LINK_RECORD *pLinkRecord = &p->LinkRecords[i];

			// ignore errors
			(void)FabricDataAddLinkRecord(fabricp, pLinkRecord);

			//printf("process LinkRecord LID: 0x%x\n", p->LinkRecords[i].RID.LID);
			//DisplayLinkRecord(&p->LinkRecords[i], 0);
		}
	}
	status = FSUCCESS;
	if (! quiet) ProgressPrint(TRUE, "Done Getting All Link Records");

done:
	// oib_query_sa will have allocated a result buffer
	// we must free the buffer when we are done with it
	if (pQueryResults)
		oib_free_query_result_buffer(pQueryResults);
	return status;

fail:
	status = FERROR;
	goto done;
}

/* query all SMInfo Records on fabric connected to given HFI port
 * and put results into fabricp->AllSMs
 * We always perform this via an SA query.  Note that an SMA SMInfo query
 * can trigger an SM to resweep in order to find the potentially new
 * SM in the fabric which is querying it.
 */
static FSTATUS GetAllSMs(struct oib_port *port,
						 EUI64 portGuid, 
						 FabricData_t *fabricp, 
						 int quiet)
{
	QUERY				query;
	FSTATUS status;
	PQUERY_RESULT_VALUES pQueryResults = NULL;

	memset(&query, 0, sizeof(query));	// initialize reserved fields
	query.InputType 	= InputTypeNoInput;
	query.OutputType 	= OutputTypeStlSMInfoRecord;

	if (! quiet) ProgressPrint(FALSE, "Getting All SM Info Records...");
	DBGPRINT("Query: Input=%s, Output=%s\n",
				   		iba_sd_query_input_type_msg(query.InputType),
					   	iba_sd_query_result_type_msg(query.OutputType));

	// this call is synchronous
	status = oib_query_sa(port, &query, &pQueryResults);

	if (! pQueryResults)
	{
		fprintf(stderr, "%*sSA SmInfoRecord query Failed: %s\n", 0, "", iba_fstatus_msg(status));
		goto fail;
	} else if (pQueryResults->Status != FSUCCESS) {
		fprintf(stderr, "%*sSA SmInfoRecord query Failed: %s MadStatus 0x%x: %s\n", 0, "",
				iba_fstatus_msg(pQueryResults->Status),
			   	pQueryResults->MadStatus, iba_sd_mad_status_msg(pQueryResults->MadStatus));
		goto fail;
	} else if (pQueryResults->ResultDataSize == 0) {
		fprintf(stderr, "%*sNo SmInfo Records Returned\n", 0, "");
	} else {
		STL_SMINFO_RECORD_RESULTS *p = (STL_SMINFO_RECORD_RESULTS*)pQueryResults->QueryResult;
		int i;

		DBGPRINT("MadStatus 0x%x: %s\n", pQueryResults->MadStatus,
					   				iba_sd_mad_status_msg(pQueryResults->MadStatus));
		DBGPRINT("%d Bytes Returned\n", pQueryResults->ResultDataSize);
		for (i=0; i<p->NumSMInfoRecords; ++i)
		{
			SMData *smp = (SMData*)MemoryAllocate2AndClear(sizeof(SMData), IBA_MEM_FLAG_PREMPTABLE, MYTAG);
			if (! smp) {
				fprintf(stderr, "%s: Unable to allocate memory\n", g_Top_cmdname);
				goto fail;
			}
			//printf("process SMInfoRecord LID: 0x%x\n", p->SMInfoRecords[i].RID.LID);
			//DisplaySMInfoRecord(&p->SMInfoRecords[i], 0);

			smp->SMInfoRecord = p->SMInfoRecords[i];
			smp->portp = FindLidPort(fabricp, smp->SMInfoRecord.RID.LID, 0);
			if (! smp->portp) {
				fprintf(stderr, "%s: SM LID not found: 0x%x\n",
							   	g_Top_cmdname, p->SMInfoRecords[i].RID.LID);
				MemoryDeallocate(smp);
				goto fail;
			}
			if (&smp->AllSMsEntry != cl_qmap_insert(&fabricp->AllSMs, smp->SMInfoRecord.SMInfo.PortGUID, &smp->AllSMsEntry)) {
				fprintf(stderr, "%s: Duplicate SM Port Guids: 0x%016"PRIx64"\n",
							   	g_Top_cmdname,
							   	smp->SMInfoRecord.SMInfo.PortGUID);
				MemoryDeallocate(smp);
				goto fail;
			}
		}
	}
	status = FSUCCESS;
	if (! quiet) ProgressPrint(TRUE, "Done Getting All SM Info Records");

done:
	// oib_query_sa will have allocated a result buffer
	// we must free the buffer when we are done with it
	if (pQueryResults)
		oib_free_query_result_buffer(pQueryResults);
	return status;

fail:
	status = FERROR;
	goto done;
}

/* query all PortCounters on all ports in fabric;
   use PaClient if available, else issue direct PMA query
 */
FSTATUS GetAllPortCounters(EUI64 portGuid, IB_GID localGid, FabricData_t *fabricp,
				Point *focus, boolean limitstats, boolean quiet, uint32 begin, uint32 end)
{
	FSTATUS status;
	cl_map_item_t *p;
#ifdef PRODUCT_OPENIB_FF
	uint16 lid = 0;
#endif
	int i=0;
	int num_nodes = cl_qmap_count(&fabricp->AllNodes);
	uint32 node_count = 0;
	uint32 fail_node_count = 0;
	uint32 fail_port_count = 0;
	STL_PortStatusData_t PortStatusData = { 0 };

	if (! quiet) ProgressPrint(TRUE, "Getting All Port Counters...");
#ifdef PRODUCT_OPENIB_FF
	if ((g_paclient_state == PACLIENT_UNKNOWN) && !(fabricp->flags & FF_PMADIRECT)){
		g_paclient_state = oib_pa_client_init_by_guid(&g_portHandle, portGuid, g_verbose_file);
		if (g_paclient_state < 0) {
			return FERROR;
		}
	} else {
		status = oib_open_port_by_guid(&g_portHandle, portGuid);
		if (status != FSUCCESS) {
			return status;
		}
	}
#else
	status = oib_open_port_by_guid(&g_portHandle, portGuid);
	if (status != FSUCCESS) {
		return status;
	}
#endif
	for (p=cl_qmap_head(&fabricp->AllNodes); p != cl_qmap_end(&fabricp->AllNodes); p = cl_qmap_next(p),i++) {
		NodeData *nodep = PARENT_STRUCT(p, NodeData, AllNodesEntry);
		PortData *first_portp;
		cl_map_item_t *q;
		boolean got = FALSE;
		boolean fail = FALSE;

		if (i%PROGRESS_FREQ == 0 || node_count == 1)
			if (! quiet) ProgressPrint(FALSE, "Processed %6d of %6d Nodes...", node_count, num_nodes);
		if (limitstats && focus && ! CompareNodePoint(nodep, focus))
			continue;
		if (i%PROGRESS_FREQ == 0)
			if (! quiet) ProgressPrint(FALSE, "Processed %6d of %6d Nodes...", i, num_nodes);
		if (cl_qmap_head(&nodep->Ports) == cl_qmap_end(&nodep->Ports))
			continue; /* no ports */
		/* issue all switch PMA requests to port 0, its only one with a LID */
		if (nodep->NodeInfo.NodeType == STL_NODE_SW) {
			first_portp = PARENT_STRUCT(cl_qmap_head(&nodep->Ports), PortData, NodePortsEntry);
#ifdef PRODUCT_OPENIB_FF
			lid = first_portp->PortInfo.LID;
#endif
			if (g_paclient_state != PACLIENT_OPERATIONAL) {
				status = GetPathToPort(g_portHandle, portGuid, first_portp);
				if (FSUCCESS != status) {
					DBGPRINT("Unable to get Path to Port %d LID 0x%04x Node 0x%016"PRIx64"\n",
						first_portp->PortNum,
						first_portp->EndPortLID,
						first_portp->nodep->NodeInfo.NodeGUID);
					DBGPRINT("    Name: %.*s\n",
						STL_NODE_DESCRIPTION_ARRAY_SIZE,
						(char*)nodep->NodeDesc.NodeString);
					//fail_port_count+= nodep->NodeInfo.NumPorts; // wrong
					fail_port_count+= cl_qmap_count(&nodep->Ports); // better
					fail_node_count++;
					continue;
				}
			}
		} else {
			first_portp = NULL;
		}

		for (q=cl_qmap_head(&nodep->Ports); q != cl_qmap_end(&nodep->Ports); q = cl_qmap_next(q)) {
			PortData *portp = PARENT_STRUCT(q, PortData, NodePortsEntry);

			if (focus && ! ComparePortPoint(portp, focus)
				&& (limitstats || ! portp->neighbor || ! ComparePortPoint(portp->neighbor, focus)))
				continue;

#ifdef PRODUCT_OPENIB_FF

			/* use PaClient if available */
			if (g_paclient_state == PACLIENT_OPERATIONAL)
			{
				if (!first_portp)
					lid = portp->PortInfo.LID;

				status = FERROR;
				//verify pa has necessary capabilities
				STL_CLASS_PORT_INFO * portInfo;
				if ((portInfo = iba_pa_classportinfo_response_query(g_portHandle))!= NULL){
					STL_PA_CLASS_PORT_INFO_CAPABILITY_MASK paCap;
					memcpy(&paCap, &portInfo->CapMask, sizeof(STL_PA_CLASS_PORT_INFO_CAPABILITY_MASK));
					//if trying to query by time, check if feature available
					if (begin || end){
						if (!(paCap.s.IsAbsTimeQuerySupported)){
							DBGPRINT("PA does not support time queries\n");
							status = FERROR;
						}else{
							status = FSUCCESS;
						}
					}else {
						status = FSUCCESS;
					}
					MemoryDeallocate(portInfo);
				}else {
						DBGPRINT("failed to determine PA capabilities\n");
						status = FERROR;
				}

				if (status == FSUCCESS){
					STL_PORT_COUNTERS_DATA portCounters1 = {0};
					STL_PA_IMAGE_ID_DATA imageIdQuery1 = {0};

					status = pa_client_get_port_stats(g_portHandle, imageIdQuery1, lid, portp->PortNum,
							NULL, &portCounters1, NULL, 0, !(end || begin)); //last param is user_counters flag,
					//if begin or end set we want raw
					//counters
					if (FSUCCESS == status){
						if (begin && end){// need to perform another query
							STL_PA_IMAGE_ID_DATA imageIdQuery2 = {0};

							imageIdQuery2.imageNumber = PACLIENT_IMAGE_TIMED;
							imageIdQuery2.imageTime.absoluteTime = begin; //we got counters for end first

							STL_PORT_COUNTERS_DATA portCounters2 = {0};

							status = pa_client_get_port_stats(g_portHandle, imageIdQuery2, lid, portp->PortNum,
									NULL, &portCounters2, NULL, 0, 0);

							if (FSUCCESS == status){
								CounterSelectMask_t clearedCounters = DiffPACounters(&portCounters1, &portCounters2, &portCounters1);
								if (clearedCounters.AsReg32){
									char counterBuf[128];
									FormatStlCounterSelectMask(counterBuf, clearedCounters);
									fprintf(stderr, "Counters reset, reporting latest count: %s\n", counterBuf);
								}
								StlPortCountersToPortStatus(&portCounters1, &PortStatusData);
							}
						}else{
							StlPortCountersToPortStatus(&portCounters1, &PortStatusData);
						}
					}
				}
			}
#endif
			/* issue direct PMA query */
			else {
				if (begin || end){
					continue;
				}
				STL_PORT_STATUS_RSP PortStatus;

				if (! PortHasPma(portp))
					continue;
				if (first_portp) {
					/* switch, issue query to port 0 */
					if (! nodep->PmaAvoidClassPortInfo)
						(void)STLPmGetClassPortInfo(g_portHandle, first_portp);
					status = STLPmGetPortStatus(g_portHandle, first_portp, portp->PortNum, &PortStatus);
				} else {
					/* CA and router, issue query to specific port */
					status = GetPathToPort(g_portHandle, portGuid, portp);
					if (FSUCCESS == status) {
						if (! nodep->PmaAvoidClassPortInfo)
							(void)STLPmGetClassPortInfo(g_portHandle, portp);
						status = STLPmGetPortStatus(g_portHandle, portp, portp->PortNum, &PortStatus);
					} else {
						DBGPRINT("Unable to get Path to Port %d LID 0x%04x Node 0x%016"PRIx64"\n",
							portp->PortNum, portp->EndPortLID,
							portp->nodep->NodeInfo.NodeGUID);
						DBGPRINT("    Name: %.*s\n",
							STL_NODE_DESCRIPTION_ARRAY_SIZE,
							(char*)portp->nodep->NodeDesc.NodeString);
					}
				}
				if (FSUCCESS == status) {
					PortStatusData.PortXmitData = PortStatus.PortXmitData;
					PortStatusData.PortRcvData = PortStatus.PortRcvData;
					PortStatusData.PortXmitPkts = PortStatus.PortXmitPkts;
					PortStatusData.PortRcvPkts = PortStatus.PortRcvPkts;
					PortStatusData.PortMulticastXmitPkts = PortStatus.PortMulticastXmitPkts;
					PortStatusData.PortMulticastRcvPkts = PortStatus.PortMulticastRcvPkts;
					PortStatusData.LocalLinkIntegrityErrors = PortStatus.LocalLinkIntegrityErrors;
					PortStatusData.FMConfigErrors = PortStatus.FMConfigErrors;
					PortStatusData.PortRcvErrors = PortStatus.PortRcvErrors;
					PortStatusData.ExcessiveBufferOverruns = PortStatus.ExcessiveBufferOverruns;
					PortStatusData.PortRcvConstraintErrors = PortStatus.PortRcvConstraintErrors;
					PortStatusData.PortRcvSwitchRelayErrors = PortStatus.PortRcvSwitchRelayErrors;
					PortStatusData.PortXmitDiscards = PortStatus.PortXmitDiscards;
					PortStatusData.PortXmitConstraintErrors = PortStatus.PortXmitConstraintErrors;
					PortStatusData.PortRcvRemotePhysicalErrors = PortStatus.PortRcvRemotePhysicalErrors;
					PortStatusData.SwPortCongestion = PortStatus.SwPortCongestion;
					PortStatusData.PortXmitWait = PortStatus.PortXmitWait;
					PortStatusData.PortRcvFECN = PortStatus.PortRcvFECN;
					PortStatusData.PortRcvBECN = PortStatus.PortRcvBECN;
					PortStatusData.PortXmitTimeCong = PortStatus.PortXmitTimeCong;
					PortStatusData.PortXmitWastedBW = PortStatus.PortXmitWastedBW;
					PortStatusData.PortXmitWaitData = PortStatus.PortXmitWaitData;
					PortStatusData.PortRcvBubble = PortStatus.PortRcvBubble;
					PortStatusData.PortMarkFECN = PortStatus.PortMarkFECN;
					PortStatusData.LinkErrorRecovery  = PortStatus.LinkErrorRecovery;
					PortStatusData.LinkDowned  = PortStatus.LinkDowned;
					PortStatusData.UncorrectableErrors = PortStatus.UncorrectableErrors;
					PortStatusData.lq = PortStatus.lq;
					PortStatusData.lq.AsReg8 |= ((portp->PortInfo.LinkWidthDowngrade.RxActive < portp->PortInfo.LinkWidth.Active ?
						StlLinkWidthToInt(portp->PortInfo.LinkWidth.Active) -
						StlLinkWidthToInt(portp->PortInfo.LinkWidthDowngrade.RxActive) : 0) << 4);
				}
			}

			if (FSUCCESS != status) {
				DBGPRINT("Unable to get Port Counters for Port %d LID 0x%04x Node 0x%016"PRIx64"\n",
					portp->PortNum, portp->EndPortLID,
					portp->nodep->NodeInfo.NodeGUID);
				DBGPRINT("    Name: %.*s\n",
					STL_NODE_DESCRIPTION_ARRAY_SIZE,
					(char*)portp->nodep->NodeDesc.NodeString);
				fail_port_count++;
				fail = TRUE;
				continue;
			}

			portp->pPortStatus = (STL_PortStatusData_t*)MemoryAllocate2AndClear(sizeof(STL_PortStatusData_t), IBA_MEM_FLAG_PREMPTABLE, MYTAG);
			if (! portp->pPortStatus) {
				DBGPRINT("Unable to allocate memory for Port Counters for Port %d LID 0x%04x Node 0x%016"PRIx64"\n",
					portp->PortNum, portp->EndPortLID,
					portp->nodep->NodeInfo.NodeGUID);
				DBGPRINT("    Name: %.*s\n",
					STL_NODE_DESCRIPTION_ARRAY_SIZE,
					(char*)portp->nodep->NodeDesc.NodeString);
				fail_port_count++;
				fail = TRUE;
				continue;
			}

			*(portp->pPortStatus) = PortStatusData;
			got = TRUE;
		}
		if (got)
			node_count++;
		if (fail)
			fail_node_count++;
	}

	//Close the oib port handle
	if (g_portHandle) {
		oib_close_port(g_portHandle);
		g_portHandle = NULL;
#ifdef PRODUCT_OPENIB_FF
		g_paclient_state = PACLIENT_UNKNOWN;
#endif
	}

	if (! quiet) ProgressPrint(TRUE, "Done Getting All Port Counters");
	if (fail_port_count)
		if (! quiet) ProgressPrint(TRUE, "Unable to get %u Ports on %u Nodes", fail_port_count, fail_node_count);
	fabricp->flags |= FF_STATS;
	return FSUCCESS;	// TBD
}

static FSTATUS GetAllVFs(struct oib_port *port, EUI64 portGuid, FabricData_t *fabricp, int quiet)
{
	FSTATUS status = FERROR;
	QUERY query;
	PQUERY_RESULT_VALUES pQueryResults;
	STL_VFINFO_RECORD_RESULTS * pinfos;
	STL_VFINFO_RECORD * pinfo;
	int i;

	if (! quiet) ProgressPrint(FALSE, "Getting vFabric Records...");

	memset(&query, 0, sizeof(query));	// initialize reserved fields
	query.InputType = InputTypeNoInput;
	query.OutputType = OutputTypeStlVfInfoRecord;
	pQueryResults = NULL;

	DBGPRINT("Query: Input=%s, Output=%s\n",
		iba_sd_query_input_type_msg(query.InputType),
		iba_sd_query_result_type_msg(query.OutputType));

	// this call is synchronous
	status = oib_query_sa(port, &query, &pQueryResults);
	if (!pQueryResults)
	{
		fprintf( stderr, "%*sSA VFInfo query Failed: %s\n", 0, "",
			iba_fstatus_msg(status) );
		return FERROR;
	}
	
	if (pQueryResults->Status != FSUCCESS) {
		fprintf( stderr,
			"%*sSA VFInfo query Failed: %s MadStatus 0x%x: %s\n", 0, "",
			iba_fstatus_msg(pQueryResults->Status), pQueryResults->MadStatus,
			iba_sd_mad_status_msg(pQueryResults->MadStatus) );
		goto free;
	}

	if (!pQueryResults->ResultDataSize) {
		fprintf(stderr, "%*sSA VFInfo query Returned Invalid Data Size:%u\n",
			0, "", pQueryResults->ResultDataSize );
		goto free;
	}

	DBGPRINT("MadStatus 0x%x: %s\n", pQueryResults->MadStatus,
		iba_sd_mad_status_msg(pQueryResults->MadStatus));
	DBGPRINT("%d Bytes Returned\n", pQueryResults->ResultDataSize);

	pinfos = (STL_VFINFO_RECORD_RESULTS *)pQueryResults->QueryResult;
	pinfo = pinfos->VfInfoRecords;

	for (i = 0; i < pinfos->NumVfInfoRecords; ++i, ++pinfo) {
		VFData_t *vf = (VFData_t *)MemoryAllocate2AndClear(sizeof(VFData_t), IBA_MEM_FLAG_PREMPTABLE, MYTAG);
		if (!vf) {
			fprintf(stderr, "%s: Unable to allocate memory\n", g_Top_cmdname);
			goto free;
		}
		vf->record = *pinfo;
		QListSetObj(&vf->AllVFsEntry, vf);
		QListInsertTail(&fabricp->AllVFs, &vf->AllVFsEntry);
	}

	status = FSUCCESS;
	if (! quiet) ProgressPrint(TRUE, "Done Getting vFabric Records");

free:
	oib_free_query_result_buffer(pQueryResults);
	return status;
}

void copySCSCTable(int *ix_rec_scsc, STL_SC_MAPPING_TABLE_RECORD_RESULTS *pSCSCRR, STL_SC_MAPPING_TABLE_RECORD **pSCSCR_2, PortData *portp)
{
	for ( ; ((*ix_rec_scsc) < pSCSCRR->NumSCSCTableRecords) &&
	    ((*pSCSCR_2)->RID.LID == portp->EndPortLID) &&
		((*pSCSCR_2)->RID.InputPort == portp->PortNum);
		(*ix_rec_scsc)++, (*pSCSCR_2)++ )
	{
		uint8 outport;
		outport = (*pSCSCR_2)->RID.OutputPort;
		QOSDataAddSCSCMap(portp, outport, (STL_SCSCMAP *)&((*pSCSCR_2)->Map));
	}
}

void copyVLArbTable(int *ix_rec_vla, STL_VLARB_TABLE *pQOSVLARB, STL_VLARBTABLE_RECORD_RESULTS *pVLATRR, STL_VLARBTABLE_RECORD **pVLATR_2, PortData *portp)
{
	int ix, ix_2;
	for ( ix = 0; ((*ix_rec_vla) < pVLATRR->NumVLArbTableRecords) &&
	    (ix < STL_VLARB_NUM_SECTIONS) &&
	    ((*pVLATR_2)->RID.LID == portp->EndPortLID) &&
	    ((*pVLATR_2)->RID.OutputPortNum == portp->PortNum);
	    (*ix_rec_vla)++, ix++, (*pVLATR_2)++ )
	{
		for (ix_2 = 0; ix_2 < VLARB_TABLE_LENGTH; ix_2++)
		{
			pQOSVLARB[ix].Elements[ix_2] = (*pVLATR_2)->VLArbTable.Elements[ix_2];
		}
	}
}

void copyPKeyTable(int *ix_rec_pk, STL_PKEY_ELEMENT **pPKEY, STL_PKEYTABLE_RECORD_RESULTS *pPKTRR, STL_P_KEY_TABLE_RECORD **pPKTR_2, PortData *portp, uint16 pkey_cap)
{
	int ix;
	for ( ; ((*ix_rec_pk) < pPKTRR->NumPKeyTableRecords) &&
	    ((*pPKTR_2)->RID.LID == portp->EndPortLID) &&
	    ((*pPKTR_2)->RID.PortNum == portp->PortNum);
	    (*pPKTR_2)++, (*ix_rec_pk)++)
	{
		uint32 ix_base = (*pPKTR_2)->RID.Blocknum *NUM_PKEY_ELEMENTS_BLOCK;
		for (ix = 0; (ix < NUM_PKEY_ELEMENTS_BLOCK) &&
		    ( (ix_base + ix) < pkey_cap); ix++, (*pPKEY)++ )
		{
			(*pPKEY)->AsReg16 = (*pPKTR_2)->PKeyTblData.PartitionTableBlock[ix].AsReg16;
		}
	}
}

// TBD - should we do whole fabric queries (which will be large responses)
// or per port queries
/* query all Port VL info from SA
 */
static FSTATUS GetAllPortVLInfoSA(struct oib_port *port,
								  FabricData_t *fabricp, 
								  Point *focus, 
								  int quiet,
								  int *use_scsc)
{
	FSTATUS	status = FSUCCESS;
	int ix_node, ix_port;
	int ix_rec_scsc = 0, ix_rec_slsc = 0, ix_rec_scsl = 0, ix_rec_scvlt = 0,
		ix_rec_scvlnt = 0, ix_rec_vla = 0, ix_rec_pk = 0;

	cl_map_item_t *p, *p2;
	NodeData *nodep;
	PortData *portp;
	STL_SLSCMAP *pQOSSLSC;
	STL_SCSLMAP *pQOSSCSL;
	STL_SCVLMAP *pQOSSCVLt;
	STL_SCVLMAP *pQOSSCVLnt;
	STL_VLARB_TABLE *pQOSVLARB;
	STL_PKEY_ELEMENT *pPKEY;
	int num_nodes = cl_qmap_count(&fabricp->AllNodes);

	QUERY	query;
	PQUERY_RESULT_VALUES	pQueryResultsSLSCMap = NULL;
	PQUERY_RESULT_VALUES	pQueryResultsSCSLMap = NULL;
	PQUERY_RESULT_VALUES	pQueryResultsSCVLtMap = NULL;
	PQUERY_RESULT_VALUES	pQueryResultsSCVLntMap = NULL;
	PQUERY_RESULT_VALUES	pQueryResultsVLArb = NULL;
	PQUERY_RESULT_VALUES	pQueryResultsPKey = NULL;
	STL_SC_MAPPING_TABLE_RECORD_RESULTS	*pSCSCRR = NULL;
	STL_SC_MAPPING_TABLE_RECORD	*pSCSCR = NULL, *pSCSCR_2 = NULL;
	STL_SL2SC_MAPPING_TABLE_RECORD_RESULTS	*pSLSCRR = NULL;
	STL_SL2SC_MAPPING_TABLE_RECORD	*pSLSCR, *pSLSCR_2 = NULL;
	STL_SC2SL_MAPPING_TABLE_RECORD_RESULTS	*pSCSLRR = NULL;
	STL_SC2SL_MAPPING_TABLE_RECORD	*pSCSLR, *pSCSLR_2 = NULL;
	STL_SC2PVL_T_MAPPING_TABLE_RECORD_RESULTS	*pSCVLtRR = NULL;
	STL_SC2PVL_T_MAPPING_TABLE_RECORD	*pSCVLtR = NULL, *pSCVLtR_2 = NULL;
	STL_SC2PVL_NT_MAPPING_TABLE_RECORD_RESULTS *pSCVLntRR = NULL;
	STL_SC2PVL_NT_MAPPING_TABLE_RECORD *pSCVLntR = NULL, *pSCVLntR_2 = NULL;
	STL_VLARBTABLE_RECORD_RESULTS	*pVLATRR = NULL;
	STL_VLARBTABLE_RECORD	*pVLATR = NULL, *pVLATR_2 = NULL;
	STL_PKEYTABLE_RECORD_RESULTS	*pPKTRR = NULL;
	STL_P_KEY_TABLE_RECORD	*pPKTR = NULL, *pPKTR_2 = NULL;

	if (! quiet) ProgressPrint(TRUE, "Getting All Port VL Tables...");

	// Query all SLSC Map records
	memset(&query, 0, sizeof(query));	// initialize reserved fields
	query.InputType 	= InputTypeNoInput;
	query.OutputType 	= OutputTypeStlSLSCTableRecord;
	pQueryResultsSLSCMap = NULL;
	pSLSCRR = NULL;

	DBGPRINT("Query: Input=%s, Output=%s\n",
		iba_sd_query_input_type_msg(query.InputType),
		iba_sd_query_result_type_msg(query.OutputType));

	// this call is synchronous
	status = oib_query_sa(port, &query, &pQueryResultsSLSCMap);
	if (! pQueryResultsSLSCMap)
	{
		fprintf( stderr, "%*sSA SLSCMap query Failed: %s\n", 0, "",
			iba_fstatus_msg(status) );
		goto fail;

	} else if (pQueryResultsSLSCMap->Status != FSUCCESS) {
		fprintf( stderr,
			"%*sSA SLSCMap query Failed: %s MadStatus 0x%x: %s\n", 0, "",
			iba_fstatus_msg(pQueryResultsSLSCMap->Status),
			pQueryResultsSLSCMap->MadStatus,
			iba_sd_mad_status_msg(pQueryResultsSLSCMap->MadStatus) );
		goto fail;
	}

	pSLSCRR = (STL_SL2SC_MAPPING_TABLE_RECORD_RESULTS*)pQueryResultsSLSCMap->QueryResult;
	if (!pQueryResultsSLSCMap->ResultDataSize) {
		pSLSCR = NULL;
	} else {
		pSLSCR = pSLSCRR->SLSCRecords;
	}

	DBGPRINT("MadStatus 0x%x: %s\n", pQueryResultsSLSCMap->MadStatus,
		iba_sd_mad_status_msg(pQueryResultsSLSCMap->MadStatus));
	DBGPRINT("%d Bytes Returned\n", pQueryResultsSLSCMap->ResultDataSize);

	// Query all SCSL Map records
	memset(&query, 0, sizeof(query));	// initialize reserved fields
	query.InputType 	= InputTypeNoInput;
	query.OutputType 	= OutputTypeStlSCSLTableRecord;
	pQueryResultsSCSLMap = NULL;
	pSCSLRR = NULL;

	DBGPRINT("Query: Input=%s, Output=%s\n",
		iba_sd_query_input_type_msg(query.InputType),
		iba_sd_query_result_type_msg(query.OutputType));

	// this call is synchronous
	status = oib_query_sa(port, &query, &pQueryResultsSCSLMap);
	if (! pQueryResultsSCSLMap)
	{
		fprintf( stderr, "%*sSA SCSLMap query Failed: %s\n", 0, "",
			iba_fstatus_msg(status) );
		goto fail;

	} else if (pQueryResultsSCSLMap->Status != FSUCCESS) {
		fprintf( stderr,
			"%*sSA SCSLMap query Failed: %s MadStatus 0x%x: %s\n", 0, "",
			iba_fstatus_msg(pQueryResultsSCSLMap->Status),
			pQueryResultsSCSLMap->MadStatus,
			iba_sd_mad_status_msg(pQueryResultsSCSLMap->MadStatus) );
		goto fail;
	}

	pSCSLRR = (STL_SC2SL_MAPPING_TABLE_RECORD_RESULTS*)pQueryResultsSCSLMap->QueryResult;
	if (!pQueryResultsSCSLMap->ResultDataSize) {
		pSCSLR = NULL;
	} else {
		pSCSLR = pSCSLRR->SCSLRecords;
	}

	DBGPRINT("MadStatus 0x%x: %s\n", pQueryResultsSCSLMap->MadStatus,
		iba_sd_mad_status_msg(pQueryResultsSCSLMap->MadStatus));
	DBGPRINT("%d Bytes Returned\n", pQueryResultsSCSLMap->ResultDataSize);

	// Query all SCVLt Table records
	memset(&query, 0, sizeof(query));	// initialize reserved fields
	query.InputType		= InputTypeNoInput;
	query.OutputType	= OutputTypeStlSCVLtTableRecord;
	pQueryResultsSCVLtMap = NULL;
	pSCVLtRR = NULL;

	DBGPRINT("Query: Input=%s, Output=%s\n",
			 iba_sd_query_input_type_msg(query.InputType),
			 iba_sd_query_result_type_msg(query.OutputType));

	// this call is synchronous
	status = oib_query_sa(port, &query, &pQueryResultsSCVLtMap);
	if (! pQueryResultsSCVLtMap) 
	{
		fprintf( stderr, "%*sSA SCVLt query Failed: %s\n", 0, "", 
				 iba_fstatus_msg(status) );
		goto fail;
	} else if (pQueryResultsSCVLtMap->Status != FSUCCESS) {
		fprintf( stderr,
				 "%*sSA SCVLt query Failed: %s MadStatus 0x%x: %s\n", 0, "",
				 iba_fstatus_msg(pQueryResultsSCVLtMap->Status),
				 pQueryResultsSCVLtMap->MadStatus,
				 iba_sd_mad_status_msg(pQueryResultsSCVLtMap->MadStatus) );
		goto fail;
	}

	pSCVLtRR = (STL_SC2PVL_T_MAPPING_TABLE_RECORD_RESULTS*)pQueryResultsSCVLtMap->QueryResult;
	if (!pQueryResultsSCVLtMap->ResultDataSize) {
		pSCVLtR = NULL;
	} else {
		pSCVLtR = pSCVLtRR->SCVLtRecords;
	}

	DBGPRINT("MadStatus 0x%x: %s\n", pQueryResultsSCVLtMap->MadStatus,
		iba_sd_mad_status_msg(pQueryResultsSCVLtMap->MadStatus));
	DBGPRINT("%d Bytes Returned\n", pQueryResultsSCVLtMap->ResultDataSize);

	// Query all SCVLnt Table records
	memset(&query, 0, sizeof(query));
	query.InputType		= InputTypeNoInput;
	query.OutputType	= OutputTypeStlSCVLntTableRecord;
	pQueryResultsSCVLntMap = NULL;
	pSCVLntRR = NULL;

	DBGPRINT("Query: Input=%s, Output=%s\n",
			 iba_sd_query_input_type_msg(query.InputType),
			 iba_sd_query_result_type_msg(query.OutputType));

	status = oib_query_sa(port, &query, &pQueryResultsSCVLntMap);
	if (! pQueryResultsSCVLntMap) {
		fprintf(stderr, "%*sSA SCVLnt query Failed: %s\n", 0, "",
				iba_fstatus_msg(status));
		goto fail;
	} else if (pQueryResultsSCVLntMap->Status != FSUCCESS) {
		fprintf(stderr, "%*sSA SCVLnt query Failed: %s MadStatus 0x%x: %s\n", 0, "",
				iba_fstatus_msg(pQueryResultsSCVLntMap->Status),
				pQueryResultsSCVLntMap->MadStatus,
				iba_sd_mad_status_msg(pQueryResultsSCVLntMap->MadStatus));
		goto fail;
	}

	pSCVLntRR = (STL_SC2PVL_NT_MAPPING_TABLE_RECORD_RESULTS*)pQueryResultsSCVLntMap->QueryResult;
	if (!pQueryResultsSCVLntMap->ResultDataSize) {
		pSCVLntR = NULL;
	} else {
		pSCVLntR = pSCVLntRR->SCVLntRecords;
	}

	DBGPRINT("MadStatus 0x%x: %s\n", pQueryResultsSCVLntMap->MadStatus,
			 iba_sd_mad_status_msg(pQueryResultsSCVLntMap->MadStatus));
	DBGPRINT("%d Bytes Returned\n", pQueryResultsSCVLntMap->ResultDataSize);

	// Query all VLArb Table records
	memset(&query, 0, sizeof(query));	// initialize reserved fields
	query.InputType 	= InputTypeNoInput;
	query.OutputType 	= OutputTypeStlVLArbTableRecord;
	pQueryResultsVLArb = NULL;
	pVLATRR = NULL;

	DBGPRINT("Query: Input=%s, Output=%s\n",
		iba_sd_query_input_type_msg(query.InputType),
		iba_sd_query_result_type_msg(query.OutputType));

	// this call is synchronous
	status = oib_query_sa(port, &query, &pQueryResultsVLArb);
	if (! pQueryResultsVLArb)
	{
		fprintf( stderr, "%*sSA VLArb query Failed: %s\n", 0, "",
			iba_fstatus_msg(status) );
		goto fail;

	} else if (pQueryResultsVLArb->Status != FSUCCESS) {
		fprintf( stderr,
			"%*sSA VLArb query Failed: %s MadStatus 0x%x: %s\n", 0, "",
			iba_fstatus_msg(pQueryResultsVLArb->Status),
			pQueryResultsVLArb->MadStatus,
			iba_sd_mad_status_msg(pQueryResultsVLArb->MadStatus) );
		goto fail;
	}

	pVLATRR = (STL_VLARBTABLE_RECORD_RESULTS*)pQueryResultsVLArb->QueryResult;
	if (!pQueryResultsVLArb->ResultDataSize) {
		pVLATR = NULL;

	} else {
		pVLATR = pVLATRR->VLArbTableRecords;
	}

	DBGPRINT("MadStatus 0x%x: %s\n", pQueryResultsVLArb->MadStatus,
		iba_sd_mad_status_msg(pQueryResultsVLArb->MadStatus));
	DBGPRINT("%d Bytes Returned\n", pQueryResultsVLArb->ResultDataSize);

	// Query all PKey Table records
	memset(&query, 0, sizeof(query));	// initialize reserved fields
	query.InputType 	= InputTypeNoInput;
	query.OutputType 	= OutputTypeStlPKeyTableRecord;
	pQueryResultsPKey = NULL;
	pPKTRR = NULL;

	DBGPRINT("Query: Input=%s, Output=%s\n",
		iba_sd_query_input_type_msg(query.InputType),
		iba_sd_query_result_type_msg(query.OutputType));

	// this call is synchronous
	status = oib_query_sa(port, &query, &pQueryResultsPKey);
	if (! pQueryResultsPKey)
	{
		fprintf( stderr, "%*sSA P_Key query Failed: %s\n", 0, "",
			iba_fstatus_msg(status) );
		goto fail;

	} else if (pQueryResultsPKey->Status != FSUCCESS) {
		fprintf( stderr,
			"%*sSA P_Key query Failed: %s MadStatus 0x%x: %s\n", 0, "",
			iba_fstatus_msg(pQueryResultsPKey->Status),
			pQueryResultsPKey->MadStatus,
			iba_sd_mad_status_msg(pQueryResultsPKey->MadStatus) );
		goto fail;
	}

	pPKTRR = (STL_PKEYTABLE_RECORD_RESULTS*)pQueryResultsPKey->QueryResult;
	if (!pQueryResultsPKey->ResultDataSize) {
		pPKTR = NULL;

	} else {
		pPKTR = pPKTRR->PKeyTableRecords;
	}

	DBGPRINT("MadStatus 0x%x: %s\n", pQueryResultsPKey->MadStatus,
		iba_sd_mad_status_msg(pQueryResultsPKey->MadStatus));
	DBGPRINT("%d Bytes Returned\n", pQueryResultsPKey->ResultDataSize);

	STL_SCSCMAP basescsc;
	uint8 i;
	int found_scsc =0;
	for (i = 0; i < STL_MAX_SCS; i++) {
		basescsc.SCSCMap[i].SC = i;
		basescsc.SCSCMap[i].Reserved = 0;
	}

	for ( p = cl_qmap_head(&fabricp->AllNodes), ix_node = 0; p != cl_qmap_end(&fabricp->AllNodes);
			p = cl_qmap_next(p), ix_node++ )
	{
		nodep = PARENT_STRUCT(p, NodeData, AllNodesEntry);
		PQUERY_RESULT_VALUES	pQueryResultsSCSCMap = NULL;

		if (ix_node%PROGRESS_FREQ == 0)
			if (! quiet) ProgressPrint(FALSE, "Processed %6d of %6d Nodes...", ix_node, num_nodes);
		if (focus && ! CompareNodePoint(nodep, focus))
			continue;

		if (nodep->NodeInfo.NodeType == STL_NODE_SW) {
			 // Query all SCSC Map records
			memset(&query, 0, sizeof(query));
			query.InputType		= InputTypeLid;
			query.OutputType	= OutputTypeStlSCSCTableRecord;
			query.InputValue.Lid	= nodep->pSwitchInfo->RID.LID;
			pQueryResultsSCSCMap = NULL;
			pSCSCRR = NULL;

			DBGPRINT("Query: Input=%s, Output=%s\n",
				iba_sd_query_input_type_msg(query.InputType),
				iba_sd_query_result_type_msg(query.OutputType));

			// this call is synchronous
			status = oib_query_sa(port, &query, &pQueryResultsSCSCMap);
			if (!pQueryResultsSCSCMap)
			{
				fprintf(stderr, "%*sSA SCSC Map query for LID 0x%X Failed: %s\n", 0, "",
					query.InputValue.Lid, iba_fstatus_msg(status));
				goto fail;
			} else if (pQueryResultsSCSCMap->Status != FSUCCESS) {
				fprintf(stderr,
					"%*sSA SCSCMap query for LID 0x%X Failed: %s MadStatus 0x%x: %s\n", 0, "",
					query.InputValue.Lid,
					iba_fstatus_msg(pQueryResultsSCSCMap->Status),
					pQueryResultsSCSCMap->MadStatus,
					iba_sd_mad_status_msg(pQueryResultsSCSCMap->MadStatus));
				goto fail;
			}
			pSCSCRR = (STL_SC_MAPPING_TABLE_RECORD_RESULTS*)pQueryResultsSCSCMap->QueryResult;
			if (pQueryResultsSCSCMap->ResultDataSize == 0) {
				fprintf(stderr, "%*sNo SCSC Records returned for LID 0x%X\n", 0, "", query.InputValue.Lid);
				pSCSCR = NULL;
			} else {
				pSCSCR = pSCSCRR->SCSCRecords;
			}
			
		}

		// Process all ports on node
		for ( p2 = cl_qmap_head(&nodep->Ports), ix_port = 0;
				p2 != cl_qmap_end(&nodep->Ports);
				p2 = cl_qmap_next(p2), ix_port++ )
		{
			uint16 pkey_cap;
			portp = PARENT_STRUCT(p2, PortData, NodePortsEntry);

			// QOS and PKey data is undefined for down ports
			if (portp->PortInfo.PortStates.s.PortState == IB_PORT_DOWN)
			{
				DBGPRINT("skip down port\n");
				continue;
			}

			if ((status = PortDataAllocateQOSData(fabricp, portp)) != FSUCCESS)
				break;

			if (nodep->NodeInfo.NodeType == STL_NODE_SW && portp->PortNum) {
				// switch external ports have SC2SC tables
				if ( (pSCSCR_2) && (pSCSCR_2->RID.LID == portp->EndPortLID) && (pSCSCR_2->RID.InputPort == portp->PortNum) )
				{
					copySCSCTable(&ix_rec_scsc, pSCSCRR, &pSCSCR_2, portp);
				} else {
					for ( ix_rec_scsc = 0, pSCSCR_2 = pSCSCR;
						(ix_rec_scsc < pSCSCRR->NumSCSCTableRecords) && pSCSCR_2;
						ix_rec_scsc++, pSCSCR_2++ )
					{
						if ( (pSCSCR_2->RID.LID == portp->EndPortLID) &&
							(pSCSCR_2->RID.InputPort == portp->PortNum) )
						{
							// assume all the records for a given port are
							// contiguous
							// Add SCSC Table data to PortData
							copySCSCTable(&ix_rec_scsc, pSCSCRR, &pSCSCR_2, portp);
							// check for SCSC transitions if needed
							if ((*use_scsc) && (!found_scsc) && (memcmp(&(pSCSCR_2->Map), &(basescsc), sizeof(STL_SCSCMAP))!=0))
								found_scsc = 1;
							break;
						}
					}
				}	// End of for ( ix_rec = 0, pQOSSCSC = portp->pQOS->SC2SCMap
			} else {
				// HFIs and switch port 0 have SL2SC and SC2SL tables
				// Find first SLSC record
				pQOSSLSC = portp->pQOS->SL2SCMap;
				if ( pSLSCR_2 && pSLSCR_2->RID.LID == portp->EndPortLID)
				{
					memcpy(pQOSSLSC, &(pSLSCR_2->SLSCMap), sizeof(STL_SLSCMAP));
					pSLSCR_2++;
				} else {
					for ( ix_rec_slsc = 0, pSLSCR_2 = pSLSCR;
					  	(ix_rec_slsc < pSLSCRR->NumSLSCTableRecords) && pSLSCR_2;
					  	ix_rec_slsc++, pSLSCR_2++ ) 
					{
						if ( pSLSCR_2->RID.LID == portp->EndPortLID)
						{
							// Add SLSC Table data to PortData
							memcpy(pQOSSLSC, &(pSLSCR_2->SLSCMap), sizeof(STL_SLSCMAP));
							pSLSCR_2++;
							break;
						}
					}
				}
				// Find first SCSL record
				pQOSSCSL = portp->pQOS->SC2SLMap;
				if (pSCSLR_2 && pSCSLR_2->RID.LID == portp->EndPortLID)
				{
					memcpy(pQOSSCSL, &(pSCSLR_2->SCSLMap), sizeof(STL_SCSLMAP));
					pSCSLR_2++;
				} else {
					for ( ix_rec_scsl = 0, pSCSLR_2 = pSCSLR;
					  	(ix_rec_scsl < pSCSLRR->NumSCSLTableRecords) && pSCSLR_2;
					  	ix_rec_scsl++, pSCSLR_2++ ) 
					{
						if ( pSCSLR_2->RID.LID == portp->EndPortLID)
						{
							// Add SCSL Table data to PortData
							memcpy(pQOSSCSL, &(pSCSLR_2->SCSLMap), sizeof(STL_SCSLMAP));
							pSCSLR_2++;
							break;
						}
					}
				}
			}

			// Process SCVL Table Data
			// Find first SCVLt record
			pQOSSCVLt = portp->pQOS->SC2VLMaps;
			if ( pSCVLtR_2 && (pSCVLtR_2->RID.LID == portp->EndPortLID) &&
				(pSCVLtR_2->RID.Port == portp->PortNum) )
			{
				memcpy(&(pQOSSCVLt[Enum_SCVLt].SCVLMap), &(pSCVLtR_2->SCVLMap), sizeof(STL_SCVLMAP));
				pSCVLtR_2++;
			} else {
				for ( ix_rec_scvlt = 0, pSCVLtR_2 = pSCVLtR;
					  (ix_rec_scvlt < pSCVLtRR->NumSCVLtTableRecords) && pSCVLtR_2;
					  ix_rec_scvlt++, pSCVLtR_2++ ) 
				{
					if ( (pSCVLtR_2->RID.LID == portp->EndPortLID) &&
						 (pSCVLtR_2->RID.Port == portp->PortNum) ) 
					{
						// Add SCVLt Table data to PortData
						memcpy(&(pQOSSCVLt[Enum_SCVLt].SCVLMap), &(pSCVLtR_2->SCVLMap), sizeof(STL_SCVLMAP));
						pSCVLtR_2++;
						break;
					}
				}
			}
			// SCVLnt
			pQOSSCVLnt = portp->pQOS->SC2VLMaps;
			if ( pSCVLntR_2 && (pSCVLntR_2->RID.LID == portp->EndPortLID) &&
				(pSCVLntR_2->RID.Port == portp->PortNum) )
			{
				memcpy(&(pQOSSCVLnt[Enum_SCVLnt].SCVLMap), &(pSCVLntR_2->SCVLMap), sizeof(STL_SCVLMAP));
				pSCVLntR_2++;
			} else {
				for ( ix_rec_scvlnt = 0, pQOSSCVLnt = portp->pQOS->SC2VLMaps, pSCVLntR_2 = pSCVLntR;
					  (ix_rec_scvlnt < pSCVLntRR->NumSCVLntTableRecords) && pSCVLntR_2;
					  ix_rec_scvlnt++, pSCVLntR_2++) {
					if ( (pSCVLntR_2->RID.LID == portp->EndPortLID) &&
						 (pSCVLntR_2->RID.Port == portp->PortNum)) {
						// Add SCVLnt table data to PortData
						memcpy(&(pQOSSCVLnt[Enum_SCVLnt].SCVLMap), &(pSCVLntR_2->SCVLMap), sizeof(STL_SCVLMAP));
						pSCVLntR_2++;
						break;
					}
				}
			}

			// Process VL Arb Table data
			// Find first VL Arb record
			pQOSVLARB = portp->pQOS->VLArbTable;
			if ( pVLATR_2 && (pVLATR_2->RID.LID == portp->EndPortLID) &&
				(pVLATR_2->RID.OutputPortNum == portp->PortNum) )
			{
				copyVLArbTable(&ix_rec_vla, pQOSVLARB, pVLATRR, &pVLATR_2, portp);
			} else {
				for ( ix_rec_vla = 0, pQOSVLARB = portp->pQOS->VLArbTable, pVLATR_2 = pVLATR;
					(ix_rec_vla < pVLATRR->NumVLArbTableRecords) && pVLATR_2;
					ix_rec_vla++, pVLATR_2++ )
				{
					if ( (pVLATR_2->RID.LID == portp->EndPortLID) &&
						(pVLATR_2->RID.OutputPortNum == portp->PortNum) )
					{
						copyVLArbTable(&ix_rec_vla, pQOSVLARB, pVLATRR, &pVLATR_2, portp);
						break;
					}	// End of for ( ix = 0; (ix_rec <
				}

			}	// End of for ( ix_rec = 0, pQOSVLARB = portp->pQOS->VLArbTable

			// Process P_Key data
			if ((status = PortDataAllocatePartitionTable(fabricp, portp)) != FSUCCESS)
				break;
			pkey_cap = PortPartitionTableSize(portp);

			// Find P_Key record
			pPKEY = portp->pPartitionTable;
			if ( pPKTR_2 && (pPKTR_2->RID.LID == portp->EndPortLID) &&
				(pPKTR_2->RID.PortNum == portp->PortNum) )
			{
				copyPKeyTable(&ix_rec_pk, &pPKEY, pPKTRR, &pPKTR_2, portp, pkey_cap);
			} else {
				for ( ix_rec_pk = 0, pPKTR_2 = pPKTR;
					(ix_rec_pk < pPKTRR->NumPKeyTableRecords) && pPKTR_2;
					ix_rec_pk++, pPKTR_2++ )
				{
					if ( (pPKTR_2->RID.LID == portp->EndPortLID) &&
						(pPKTR_2->RID.PortNum == portp->PortNum) )

					{
						copyPKeyTable(&ix_rec_pk, &pPKEY, pPKTRR, &pPKTR_2, portp, pkey_cap);
						break;
					}	// End of for ( ; (ix_rec < pPKTRR->NumPKeyTableRecords
				}

			}	// End of for ( ix_rec = 0, pPKEY = portp->pPartitionTable

		}	// End of for ( p2 = cl_qmap_head(&nodep->Ports)
		if (pQueryResultsSCSCMap)
			oib_free_query_result_buffer(pQueryResultsSCSCMap);

	}	// End of for ( p=cl_qmap_head(&fabricp->AllNodes)

	fabricp->flags |= FF_QOSDATA;
	(*use_scsc) &= found_scsc;

done:
	// Free query results buffers
	if (pQueryResultsSLSCMap)
		oib_free_query_result_buffer(pQueryResultsSLSCMap);
	if (pQueryResultsSCSLMap)
		oib_free_query_result_buffer(pQueryResultsSCSLMap);
	if (pQueryResultsSCVLtMap)
		oib_free_query_result_buffer(pQueryResultsSCVLtMap);
	if (pQueryResultsSCVLntMap)
		oib_free_query_result_buffer(pQueryResultsSCVLntMap);
	if (pQueryResultsVLArb)
		oib_free_query_result_buffer(pQueryResultsVLArb);
	if (pQueryResultsPKey)
		oib_free_query_result_buffer(pQueryResultsPKey);

	if (! quiet) ProgressPrint(TRUE, "Done Getting All Port VL Tables");
	return (status);

fail:
	status = FERROR;
	goto done;
}	// End of GetAllPortVLInfoSA()

/* query all Port VL info directly from SMA
 */
static FSTATUS GetAllPortVLInfoDirect(struct oib_port *port,
									  FabricData_t *fabricp, 
									  Point *focus, 
									  int quiet,
									  int *use_scsc)
{
	FSTATUS	status = FSUCCESS;
	int ix_node, ix_port, block;

	cl_map_item_t *p, *p2;
	NodeData *nodep;
	PortData *portp;
	STL_VLARB_TABLE *pQOSVLARB;
	STL_PKEY_ELEMENT *pPKEY;
	int num_nodes = cl_qmap_count(&fabricp->AllNodes);
	uint8 in_port;
	uint8 out_port;

	if (! quiet) ProgressPrint(TRUE, "Getting All Port VL Tables...");

	STL_SCSCMAP basescsc;
	int found_scsc = 0;
	uint8 i;
	for (i = 0; i < STL_MAX_SCS; i++) {
		basescsc.SCSCMap[i].SC = i;
		basescsc.SCSCMap[i].Reserved = 0;
	}

	for ( p = cl_qmap_head(&fabricp->AllNodes), ix_node = 0; p != cl_qmap_end(&fabricp->AllNodes);
			p = cl_qmap_next(p), ix_node++ )
	{
		nodep = PARENT_STRUCT(p, NodeData, AllNodesEntry);
		//boolean enhancedp0 = ( nodep->pSwitchInfo
		//							&& nodep->pSwitchInfo->SwitchInfoData.u2.s.EnhancedPort0);

		if (ix_node%PROGRESS_FREQ == 0)
			if (! quiet) ProgressPrint(FALSE, "Processed %6d of %6d Nodes...", ix_node, num_nodes);
		if (focus && ! CompareNodePoint(nodep, focus))
			continue;

		// Process all ports on node
		for ( p2 = cl_qmap_head(&nodep->Ports), ix_port = 0;
				p2 != cl_qmap_end(&nodep->Ports);
				p2 = cl_qmap_next(p2), ix_port++ )
		{
			int pkey_cap;
			portp = PARENT_STRUCT(p2, PortData, NodePortsEntry);

			if (portp->PortInfo.PortStates.s.PortState == IB_PORT_DOWN)
			{
				DBGPRINT("skip down port\n");
				continue;
			}
			if ((status = PortDataAllocateQOSData(fabricp, portp)) != FSUCCESS)
				break;

			if (nodep->NodeInfo.NodeType == STL_NODE_SW && portp->PortNum) {
				// switch external ports have SC2SC tables
				cl_map_item_t *p3;

				in_port = portp->PortNum;
				// just visit active ports
				for ( p3 = cl_qmap_head(&nodep->Ports);
							p3 != cl_qmap_end(&nodep->Ports);
							p3 = cl_qmap_next(p3) )
				{
					PortData *outportp = PARENT_STRUCT(p3, PortData, NodePortsEntry);
					STL_SCSCMAP SCSCMap;

					out_port = outportp->PortNum;
					if (out_port == 0)
						continue;
					status = SmaGetSCSCMappingTable(port, nodep, portp->EndPortLID, in_port, out_port, &SCSCMap);
					if (status != FSUCCESS)
					{
						fprintf(stderr, "%*sSMA Get(SCSCMap %u %u) Failed to LID 0x%x Node 0x%016"PRIx64" Name: %.*s: %s\n", 0, "", in_port, out_port, portp->EndPortLID,
							nodep->NodeInfo.NodeGUID,
							STL_NODE_DESCRIPTION_ARRAY_SIZE,
							(char*)nodep->NodeDesc.NodeString, iba_fstatus_msg(status));
					} else {
						// Copy the SCSC map to the QOS data
						QOSDataAddSCSCMap(portp, out_port, &SCSCMap);
						// check for SCSC transitions if needed
						if((*use_scsc) && (!found_scsc) && (memcmp(&SCSCMap, &(basescsc), sizeof(STL_SCSCMAP))!=0))
							found_scsc = 1;
					}
				}
			} else {
				// HFIs and switch port 0 have SC2SC and SC2SL tables

				STL_SLSCMAP SLSCMap;
				STL_SCSLMAP SCSLMap;

				// Process SLSC Mapping Table data
				status = SmaGetSLSCMappingTable(port, nodep, portp->EndPortLID, &SLSCMap);
				if (status != FSUCCESS)
				{
					fprintf(stderr, "%*sSMA Get(SLSCMap) Failed to LID 0x%x Node 0x%016"PRIx64" Name: %.*s: %s\n", 0, "", portp->EndPortLID,
						nodep->NodeInfo.NodeGUID,
						STL_NODE_DESCRIPTION_ARRAY_SIZE,
						(char*)nodep->NodeDesc.NodeString, iba_fstatus_msg(status));
				} else {
					// Copy the SLSCMap to the pQOS data
					memcpy(portp->pQOS->SL2SCMap, &SLSCMap, sizeof(STL_SLSCMAP));
				}
				// Process SCSL Mapping Table data
				status = SmaGetSCSLMappingTable(port, nodep, portp->EndPortLID, &SCSLMap);
				if (status != FSUCCESS)
				{
					fprintf(stderr, "%*sSMA Get(SCSLMap) Failed to LID 0x%x Node 0x%016"PRIx64" Name: %.*s: %s\n", 0, "", portp->EndPortLID,
						nodep->NodeInfo.NodeGUID,
						STL_NODE_DESCRIPTION_ARRAY_SIZE,
						(char*)nodep->NodeDesc.NodeString, iba_fstatus_msg(status));
				} else {
					// Copy the SCSLMap to the pQOS data
					memcpy(portp->pQOS->SC2SLMap, &SCSLMap, sizeof(STL_SCSLMAP));
				}
			}

			// process scvl_t table data
			{
				STL_SCVLMAP SCVLtMap;

				status = SmaGetSCVLMappingTable(port, nodep, portp->EndPortLID, portp->PortNum, &SCVLtMap, STL_MCLASS_ATTRIB_ID_SC_VLT_MAPPING_TABLE);
				if (status != FSUCCESS) 
				{
					fprintf(stderr, "%*sSMA Get(SCVLtMap: %u) Failed to LID 0x%x Node 0x%016"PRIx64" Name: %.*s: %s\n", 0, "", portp->PortNum, portp->EndPortLID,
							nodep->NodeInfo.NodeGUID,
							STL_NODE_DESCRIPTION_ARRAY_SIZE,
							(char *)nodep->NodeDesc.NodeString, iba_fstatus_msg(status));
				} else {
					memcpy(&(portp->pQOS->SC2VLMaps[Enum_SCVLt]), &SCVLtMap, sizeof(STL_SCVLMAP));
				}
			}

			// process scvl_nt table data (not valid on switch port 0)
			if (nodep->NodeInfo.NodeType != STL_NODE_SW || portp->PortNum)
			{
				STL_SCVLMAP SCVLntMap;

				status = SmaGetSCVLMappingTable(port, nodep, portp->EndPortLID, portp->PortNum, &SCVLntMap, STL_MCLASS_ATTRIB_ID_SC_VLNT_MAPPING_TABLE);
				if (status != FSUCCESS) 
				{
					fprintf(stderr, "%*sSMA Get(SCVLntMap: %u) Failed to LID 0x%x Node 0x%016"PRIx64" Name: %.*s: %s\n", 0, "", portp->PortNum, portp->EndPortLID,
							nodep->NodeInfo.NodeGUID,
							STL_NODE_DESCRIPTION_ARRAY_SIZE,
							(char *)nodep->NodeDesc.NodeString, iba_fstatus_msg(status));
				} else {
					memcpy(&(portp->pQOS->SC2VLMaps[Enum_SCVLnt]), &SCVLntMap, sizeof(STL_SCVLMAP));
				}
			}

			// Process VL Arb Table data, only valid on ports which
			// support > 1 VL.  Not valid on non-enhanced port 0
			// SA does not report this port any switch port 0, so skip
			if (portp->PortInfo.VL.s2.Cap != 1
				&& (nodep->NodeInfo.NodeType != STL_NODE_SW
						|| portp->PortNum != 0 /*|| enhancedp0*/)) {
				out_port = portp->PortNum;

				for ( block = 0, pQOSVLARB = portp->pQOS->VLArbTable;
						block < STL_VLARB_NUM_SECTIONS;
						block++ )
				{
					STL_VLARB_TABLE VLArbTable;
					status = SmaGetVLArbTable(port, nodep, portp->EndPortLID, out_port, block, &VLArbTable);
					if (status != FSUCCESS) {
						fprintf(stderr, "%*sSMA Get(VLArbTable %u %u) Failed to LID 0x%x Node 0x%016"PRIx64" Name: %.*s: %s\n", 0, "", block, out_port, portp->EndPortLID,
							nodep->NodeInfo.NodeGUID,
							STL_NODE_DESCRIPTION_ARRAY_SIZE,
							(char*)nodep->NodeDesc.NodeString, iba_fstatus_msg(status));
					} else {
						// Add VLArb data to PortData
						pQOSVLARB[block] = VLArbTable;
					}
				}
			}

			// Get P_Key data
			if ((status = PortDataAllocatePartitionTable(fabricp, portp)) != FSUCCESS)
				break;
			pkey_cap = (int)(unsigned)PortPartitionTableSize(portp);
			if (nodep->NodeInfo.NodeType == STL_NODE_SW)
				out_port = portp->PortNum;
			else
				out_port = 0;

			for ( block = 0, pPKEY = portp->pPartitionTable;
					pkey_cap > 0;
				   	block++, pPKEY += NUM_PKEY_ELEMENTS_BLOCK, pkey_cap -= NUM_PKEY_ELEMENTS_BLOCK)
			{
				STL_PARTITION_TABLE PartTable;
				status = SmaGetPartTable(port, nodep, portp->EndPortLID, out_port, block, &PartTable);
				if (status != FSUCCESS)
				{
					fprintf(stderr, "%*sSMA Get(P_KeyTable %u %u) Failed to LID 0x%x Node 0x%016"PRIx64" Name: %.*s: %s\n", 0, "", out_port, block, portp->EndPortLID,
						nodep->NodeInfo.NodeGUID,
						STL_NODE_DESCRIPTION_ARRAY_SIZE,
						(char*)nodep->NodeDesc.NodeString, iba_fstatus_msg(status));
				} else {
					// Add P_Key data to PortData
					memcpy(pPKEY, &PartTable.PartitionTableBlock[0], sizeof(STL_PKEY_ELEMENT)*MIN(pkey_cap, NUM_PKEY_ELEMENTS_BLOCK));
				}

			}	// End of for ( block = 0, pPKEY = portp->pPartitionTable

		}	// End of for ( p2 = cl_qmap_head(&nodep->Ports)

	}	// End of for ( p=cl_qmap_head(&fabricp->AllNodes)

	fabricp->flags |= FF_QOSDATA;
	(*use_scsc) &= found_scsc;

	if (! quiet) ProgressPrint(TRUE, "Done Getting All Port VL Tables");
	return (status);

}	// End of GetAllPortVLInfoDirect()

/* query all Port VL info
 */
FSTATUS GetAllPortVLInfo(EUI64 portGuid, FabricData_t *fabricp, Point *focus, int quiet, int *use_scsc)
{
	struct oib_port *oib_port_session = NULL;
	FSTATUS fstatus = FSUCCESS;

	fstatus = oib_open_port_by_guid(&oib_port_session, portGuid);
	if (fstatus != FSUCCESS) {
		fprintf(stderr, "%s: Unable to open fabric interface.\n",
				g_Top_cmdname);
	} else {
		if (fabricp->flags & FF_SMADIRECT) {
			fstatus = GetAllPortVLInfoDirect(oib_port_session, fabricp, focus, quiet, use_scsc);
		} else {
			fstatus = GetAllPortVLInfoSA(oib_port_session, fabricp, focus, quiet, use_scsc);
		}
		oib_close_port(oib_port_session);
	}

	return fstatus;
}

/* copy linear FDB block
 */
static FSTATUS CopyLinearFDBBlock(STL_LINEAR_FORWARDING_TABLE *pDestFwdTbl, PORT *pSrcFDBData, uint16 blockSize)
{
	if (!pDestFwdTbl || !pSrcFDBData || !blockSize)
		return (FINVALID_PARAMETER);

	memcpy(pDestFwdTbl, pSrcFDBData, blockSize);
	return (FSUCCESS);
}

/* copy multicast FDB block
 */
static FSTATUS CopyMulticastFDBBlock( NodeData *pNode, STL_PORTMASK *pDestFwdTbl,
	STL_PORTMASK *pSrcFDBData, uint16 blockSize, unsigned int position )
{
	int ix;

	if ( !pNode || !pNode->switchp || !pDestFwdTbl || !pSrcFDBData ||
			!blockSize || (position >= 256/STL_PORT_MASK_WIDTH) )
		return (FINVALID_PARAMETER);

	for ( ix = 0, pDestFwdTbl += position; ix < blockSize;
			pDestFwdTbl += pNode->switchp->MulticastFDBEntrySize, ix++ )
		*pDestFwdTbl = pSrcFDBData[ix];

	return (FSUCCESS);
}

/* query all forwarding DBs on switch nodes in fabric from SA
 */
static FSTATUS GetAllFDBsSA(struct oib_port *port, FabricData_t *fabricp, Point *focus, int quiet)
{
	FSTATUS	status = FSUCCESS;
	int ix, ix_node;

	cl_map_item_t *p;

	QUERY	query;
	PQUERY_RESULT_VALUES	pQueryResultsLinearFDB = NULL;
	PQUERY_RESULT_VALUES	pQueryResultsMulticastFDB = NULL;
	PQUERY_RESULT_VALUES	pQueryResultsPGT = NULL;
	PQUERY_RESULT_VALUES	pQueryResultsPGFT = NULL;
	STL_MULTICAST_FORWARDING_TABLE_RECORD	*pMFR;
	STL_LINEAR_FDB_RECORD_RESULTS		*pLFRR;
	STL_MCAST_FDB_RECORD_RESULTS	*pMFRR;
	STL_PORT_GROUP_TABLE_RECORD_RESULTS *pPGTRR = NULL;
	STL_PORT_GROUP_FORWARDING_TABLE_RECORD_RESULTS *pPGFTRR = NULL;
	uint32	linearFDBSize = 0;
	uint32	multicastFDBSize = 0;
	uint16	portGroupSize = 0;
	uint32	pgftSize = 0;

	int num_nodes = cl_qmap_count(&fabricp->AllNodes);

	if (! quiet) ProgressPrint(TRUE, "Getting All FDB Tables...");
	for ( p=cl_qmap_head(&fabricp->AllNodes), ix_node = 0; p != cl_qmap_end(&fabricp->AllNodes);
			p = cl_qmap_next(p), ix_node++ )
	{
		NodeData *nodep = PARENT_STRUCT(p, NodeData, AllNodesEntry);

		if (ix_node%PROGRESS_FREQ == 0)
			if (! quiet) ProgressPrint(FALSE, "Processed %6d of %6d Nodes...", ix_node, num_nodes);
		if (focus && ! CompareNodePoint(nodep, focus))
			continue;

		// Process switch nodes
		if (nodep->NodeInfo.NodeType == STL_NODE_SW) {

			// Query LinearFDB records
			memset(&query, 0, sizeof(query));	// initialize reserved fields
			query.InputType 	= InputTypeLid;
			query.InputValue.Lid = nodep->pSwitchInfo->RID.LID;
			query.OutputType 	= OutputTypeStlLinearFDBRecord;
			pQueryResultsLinearFDB = NULL;
			pLFRR = NULL;
			linearFDBSize = 0;

			DBGPRINT("Query: Input=%s, Output=%s\n",
				iba_sd_query_input_type_msg(query.InputType),
				iba_sd_query_result_type_msg(query.OutputType));

			// this call is synchronous
			status = oib_query_sa(port, &query, &pQueryResultsLinearFDB);
			if (! pQueryResultsLinearFDB)
			{
				fprintf( stderr, "%*sSA LinearFDB query for LID 0x%X Failed: %s\n", 0, "",
					query.InputValue.Lid, iba_fstatus_msg(status) );
			} else if (pQueryResultsLinearFDB->Status != FSUCCESS) {
				fprintf( stderr,
					"%*sSA LinearFDB query for LID 0x%X Failed: %s MadStatus 0x%x: %s\n",
					0, "", query.InputValue.Lid,
					iba_fstatus_msg(pQueryResultsLinearFDB->Status),
					pQueryResultsLinearFDB->MadStatus,
					iba_sd_mad_status_msg(pQueryResultsLinearFDB->MadStatus) );
			} else if (pQueryResultsLinearFDB->ResultDataSize == 0) {
				fprintf(stderr, "%*sNo LinearFDB Records Returned\n", 0, "");
			} else {
				pLFRR = (STL_LINEAR_FDB_RECORD_RESULTS*)pQueryResultsLinearFDB->QueryResult;
				linearFDBSize = pLFRR->NumLinearFDBRecords * MAX_LFT_ELEMENTS_BLOCK;

				if ( linearFDBSize >
						nodep->pSwitchInfo->SwitchInfoData.LinearFDBTop + 1 )
					linearFDBSize =
						nodep->pSwitchInfo->SwitchInfoData.LinearFDBTop + 1;

				DBGPRINT("MadStatus 0x%x: %s\n", pQueryResultsLinearFDB->MadStatus,
					iba_sd_mad_status_msg(pQueryResultsLinearFDB->MadStatus));
				DBGPRINT("%d Bytes Returned\n", pQueryResultsLinearFDB->ResultDataSize);

			}	// End of else

			if(nodep->pSwitchInfo->SwitchInfoData.AdaptiveRouting.s.Enable) {
				// Query Port Group records
				memset(&query, 0, sizeof(query));	// initialize reserved fields
				query.InputType 	= InputTypeLid;
				query.InputValue.Lid = nodep->pSwitchInfo->RID.LID;
				query.OutputType 	= OutputTypeStlPortGroupRecord;
				pQueryResultsPGT = NULL;
				pPGTRR = NULL;
				portGroupSize = 0;

				DBGPRINT("Query: Input=%s, Output=%s\n",
					iba_sd_query_input_type_msg(query.InputType),
					iba_sd_query_result_type_msg(query.OutputType));

				// this call is synchronous
				status = oib_query_sa(port, &query, &pQueryResultsPGT);
				if (! pQueryResultsPGT)
				{
					fprintf( stderr, "%*sSA PortGroup query for LID 0x%X Failed: %s\n", 0, "",
						query.InputValue.Lid, iba_fstatus_msg(status) );
				} else if (pQueryResultsPGT->Status != FSUCCESS) {
					fprintf( stderr,
						"%*sSA PortGroup query for LID 0x%X Failed: %s MadStatus 0x%x: %s\n",
						0, "", query.InputValue.Lid,
						iba_fstatus_msg(pQueryResultsPGT->Status),
						pQueryResultsPGT->MadStatus,
						iba_sd_mad_status_msg(pQueryResultsPGT->MadStatus) );
				} else if (pQueryResultsPGT->ResultDataSize == 0) {
					fprintf(stderr, "%*sNo Port Group Records Returned\n", 0, "");
				} else {
					pPGTRR = (STL_PORT_GROUP_TABLE_RECORD_RESULTS*)pQueryResultsPGT->QueryResult;
					portGroupSize = pPGTRR->NumRecords * NUM_PGT_ELEMENTS_BLOCK;

					DBGPRINT("MadStatus 0x%x: %s\n", pQueryResultsPGT->MadStatus,
						iba_sd_mad_status_msg(pQueryResultsPGT->MadStatus));
					DBGPRINT("%d Bytes Returned\n", pQueryResultsPGT->ResultDataSize);
				}

				// Query Port Group FDB records
				memset(&query, 0, sizeof(query));	// initialize reserved fields
				query.InputType 	= InputTypeLid;
				query.InputValue.Lid = nodep->pSwitchInfo->RID.LID;
				query.OutputType 	= OutputTypeStlPortGroupFwdRecord;
				pQueryResultsPGFT = NULL;
				pPGFTRR = NULL;
				pgftSize = 0;

				DBGPRINT("Query: Input=%s, Output=%s\n",
					iba_sd_query_input_type_msg(query.InputType),
					iba_sd_query_result_type_msg(query.OutputType));

				// this call is synchronous
				status = oib_query_sa(port, &query, &pQueryResultsPGFT);
				if (! pQueryResultsPGFT)
				{
					fprintf( stderr, "%*sSA PGFT query for LID 0x%X Failed: %s\n", 0, "",
						query.InputValue.Lid, iba_fstatus_msg(status) );
				} else if (pQueryResultsPGFT->Status != FSUCCESS) {
					fprintf( stderr,
						"%*sSA PGFT query for LID 0x%X Failed: %s MadStatus 0x%x: %s\n",
						0, "", query.InputValue.Lid,
						iba_fstatus_msg(pQueryResultsPGFT->Status),
						pQueryResultsPGFT->MadStatus,
						iba_sd_mad_status_msg(pQueryResultsPGFT->MadStatus) );
				} else if (pQueryResultsPGFT->ResultDataSize == 0) {
					fprintf(stderr, "%*sNo PGFT Records Returned\n", 0, "");
				} else {
					pPGFTRR = (STL_PORT_GROUP_FORWARDING_TABLE_RECORD_RESULTS*)pQueryResultsPGFT->QueryResult;
					DBGPRINT("MadStatus 0x%x: %s\n", pQueryResultsPGFT->MadStatus,
						iba_sd_mad_status_msg(pQueryResultsPGFT->MadStatus));
					DBGPRINT("%d Bytes Returned\n", pQueryResultsPGFT->ResultDataSize);
					pgftSize = pPGFTRR->NumRecords * MAX_LFT_ELEMENTS_BLOCK;
					uint32 pgftCap = nodep->pSwitchInfo->SwitchInfoData.PortGroupFDBCap ?
										nodep->pSwitchInfo->SwitchInfoData.PortGroupFDBCap :
										DEFAULT_MAX_PGFT_LID + 1;
					if (pgftSize < MIN(linearFDBSize, pgftCap) && pgftSize != 0) {
						fprintf(stderr, "%*sIncorrect # of PGFT Records Returned "
							"(LFT(%d) versus PGFT(%d)\n",
							0, "", linearFDBSize, pgftSize);
					}
				}
			}

			// Query MulticastFDB records
			memset(&query, 0, sizeof(query));	// initialize reserved fields
			query.InputType 	= InputTypeLid;
			query.InputValue.Lid = nodep->pSwitchInfo->RID.LID;
			query.OutputType 	= OutputTypeStlMCastFDBRecord;
			pQueryResultsMulticastFDB = NULL;
			pMFRR = NULL;
			multicastFDBSize = 0;

			DBGPRINT("Query: Input=%s, Output=%s\n",
				iba_sd_query_input_type_msg(query.InputType),
				iba_sd_query_result_type_msg(query.OutputType));

			// this call is synchronous
			status = oib_query_sa(port, &query, &pQueryResultsMulticastFDB);
			if (! pQueryResultsMulticastFDB)
			{
				fprintf( stderr, "%*sSA MulticastFDB query for LID 0x%X Failed: %s\n", 0, "",
					query.InputValue.Lid, iba_fstatus_msg(status) );
			} else if (pQueryResultsMulticastFDB->Status != FSUCCESS) {
				fprintf( stderr,
					"%*sSA MulticastFDB query for LID 0x%X Failed: %s MadStatus 0x%x: %s\n",
					0, "", query.InputValue.Lid,
					iba_fstatus_msg(pQueryResultsMulticastFDB->Status),
					pQueryResultsMulticastFDB->MadStatus,
					iba_sd_mad_status_msg(pQueryResultsMulticastFDB->MadStatus) );
			} else if (pQueryResultsMulticastFDB->ResultDataSize == 0) {
				fprintf(stderr, "%*sNo MulticastFDB Records Returned\n", 0, "");
			} else {

				pMFRR =
				(STL_MCAST_FDB_RECORD_RESULTS*)pQueryResultsMulticastFDB->QueryResult;
				multicastFDBSize = (pMFRR->NumMCastFDBRecords * STL_NUM_MFT_ELEMENTS_BLOCK)
						/ ComputeMulticastFDBEntrySize(nodep->NodeInfo.NumPorts);

				DBGPRINT("MadStatus 0x%x: %s\n", pQueryResultsMulticastFDB->MadStatus,
					iba_sd_mad_status_msg(pQueryResultsMulticastFDB->MadStatus));
				DBGPRINT("%d Bytes Returned\n", pQueryResultsMulticastFDB->ResultDataSize);

			}	// End of else

			//
			// Add forwarding tables to SwitchData
			// 
			status = NodeDataAllocateSwitchData( fabricp, nodep, linearFDBSize,
				multicastFDBSize);

			if ((status == FSUCCESS) && linearFDBSize) {
				uint32_t limit = ROUNDUP(linearFDBSize,MAX_LFT_ELEMENTS_BLOCK)/MAX_LFT_ELEMENTS_BLOCK;
				STL_LINEAR_FORWARDING_TABLE_RECORD 	*pLFR;

				for ( ix = 0, pLFR = pLFRR->LinearFDBRecords;
					ix < limit; ix++, pLFR++ ) {
					CopyLinearFDBBlock( &nodep->switchp->LinearFDB[ix],
						pLFR->LinearFdbData,
						MIN(linearFDBSize - ix, MAX_LFT_ELEMENTS_BLOCK));
				}	// End of for ( ix = 0, pLFR = pLFRR->LinearFDBRecords
			}

			if ((status == FSUCCESS) && portGroupSize) {
				STL_PORT_GROUP_TABLE_RECORD *pPGT = pPGTRR->Records;
				for ( ix = 0; ix < pPGTRR->NumRecords; ix++) {
					memcpy(&nodep->switchp->PortGroupElements[ix*NUM_PGT_ELEMENTS_BLOCK],
						pPGT[ix].GroupBlock,
						NUM_PGT_ELEMENTS_BLOCK*sizeof(STL_PORTMASK));
				}
			}

			if ((status == FSUCCESS) && pgftSize) {
				STL_PORT_GROUP_FORWARDING_TABLE_RECORD *pPGFTR;
				unsigned int pgfdbcap = nodep->pSwitchInfo->SwitchInfoData.PortGroupFDBCap ?
											nodep->pSwitchInfo->SwitchInfoData.PortGroupFDBCap :
											DEFAULT_MAX_PGFT_LID+1;
				unsigned int pgfdbsize = ROUNDUP(MIN(nodep->switchp->LinearFDBSize, pgfdbcap),
								NUM_PGFT_ELEMENTS_BLOCK)/NUM_PGFT_ELEMENTS_BLOCK;

				// Don't core dump if you received more data than you expected.
				pgfdbsize=MIN(pgfdbsize, pPGFTRR->NumRecords);
				memset(nodep->switchp->PortGroupFDB,0xff,pgfdbsize);

				for ( ix = 0, 
 					pPGFTR = &pPGFTRR->Records[ix];
					ix < pgfdbsize; ix++, pPGFTR++) {
					memcpy(&nodep->switchp->PortGroupFDB[ix],
						pPGFTR->PGFdbData,
						NUM_PGFT_ELEMENTS_BLOCK*sizeof(PORT));
				}
			}

			if ((status == FSUCCESS) && multicastFDBSize) {
				uint32	blockNum = 0xFFFFFFFF;
				unsigned int	position = 0;
				// multicastFDBSize is always a multiple of MFT_BLOCK_SIZE
				for ( ix = 0, pMFR = pMFRR->MCastFDBRecords;
						ix < pMFRR->NumMCastFDBRecords; ix ++, pMFR++ ) {
					if (pMFR->RID.u1.s.BlockNum != blockNum) {
						blockNum = pMFR->RID.u1.s.BlockNum;
						position = 0;
					}
					else
						position += 1;

					CopyMulticastFDBBlock( nodep,
						GetMulticastFDBEntry(nodep, blockNum * STL_NUM_MFT_ELEMENTS_BLOCK),
						pMFR->MftTable.MftBlock, STL_NUM_MFT_ELEMENTS_BLOCK, position );

				}	// End of for ( ix = 0, pMFR = pMFRR->MCastFDBRecords
			}

			// Free query results buffers
			if (pQueryResultsMulticastFDB)
				oib_free_query_result_buffer(pQueryResultsMulticastFDB);

			if (pQueryResultsLinearFDB)
				oib_free_query_result_buffer(pQueryResultsLinearFDB);

			if (pQueryResultsPGT)
				oib_free_query_result_buffer(pQueryResultsPGT);

			if (pQueryResultsPGFT) 
				oib_free_query_result_buffer(pQueryResultsPGFT);

		}	// End of if (nodep->NodeInfo.NodeType == STL_NODE_SW

	}	// End of for ( p=cl_qmap_head(&fabricp->AllNodes)

	fabricp->flags |= FF_ROUTES;

	if (! quiet) ProgressPrint(TRUE, "Done Getting All FDB Tables");

	return (status);

}	// End of GetAllFDBs()

/* query all forwarding DBs on switch nodes in fabric directly from SMA
 */
static FSTATUS GetAllFDBsDirect(struct oib_port *port, FabricData_t *fabricp, Point *focus, int quiet)
{
	FSTATUS	status = FSUCCESS;
	int ix, ix_node;
	unsigned block, position;

	cl_map_item_t *p;

	STL_LINEAR_FORWARDING_TABLE linearFDB;
	STL_MULTICAST_FORWARDING_TABLE multicastFDB;
	uint32	linearFDBSize; // Size increased in STL
	uint32	multicastFDBSize;

	int num_nodes = cl_qmap_count(&fabricp->AllNodes);

	if (! quiet) ProgressPrint(TRUE, "Getting All FDB Tables...");
	for ( p=cl_qmap_head(&fabricp->AllNodes), ix_node = 0; p != cl_qmap_end(&fabricp->AllNodes);
			p = cl_qmap_next(p), ix_node++ )
	{
		NodeData *nodep = PARENT_STRUCT(p, NodeData, AllNodesEntry);

		if (ix_node%PROGRESS_FREQ == 0)
			if (! quiet) ProgressPrint(FALSE, "Processed %6d of %6d Nodes...", ix_node, num_nodes);
		if (focus && ! CompareNodePoint(nodep, focus))
			continue;

		// Process switch nodes
		if (nodep->NodeInfo.NodeType == STL_NODE_SW) {
			uint32 lid = nodep->pSwitchInfo->RID.LID;
			uint32_t limit;

			linearFDBSize = nodep->pSwitchInfo->SwitchInfoData.LinearFDBTop+1;
			multicastFDBSize = ComputeMulticastFDBSize(&nodep->pSwitchInfo->SwitchInfoData);
 			limit = ROUNDUP(linearFDBSize,MAX_LFT_ELEMENTS_BLOCK)/MAX_LFT_ELEMENTS_BLOCK;
			// Add LinearFDB and MulticastFDB data to SwitchData
			status = NodeDataAllocateSwitchData( fabricp, nodep, linearFDBSize,
				multicastFDBSize);
			if (status != FSUCCESS)
				break;

			for (ix = 0; ix < limit; ix++) {
				status = SmaGetLinearFDBTable(port, nodep, lid, ix, &linearFDB);
				if (status != FSUCCESS)
				{
					fprintf(stderr, "%*sSMA Get(LFT %u) Failed to LID 0x%x Node 0x%016"PRIx64" Name: %.*s: %s\n", 0, "", ix, lid,
						nodep->NodeInfo.NodeGUID,
						STL_NODE_DESCRIPTION_ARRAY_SIZE,
						(char*)nodep->NodeDesc.NodeString, iba_fstatus_msg(status));
				} else {
					CopyLinearFDBBlock( &nodep->switchp->LinearFDB[ix],
						linearFDB.LftBlock,
						MIN(linearFDBSize - ix, (int)MAX_LFT_ELEMENTS_BLOCK));
				}
			}

			for (ix = 0, block=0; ix < multicastFDBSize; ix += STL_NUM_MFT_ELEMENTS_BLOCK, block++) {
				for (position=0; position < nodep->switchp->MulticastFDBEntrySize; position++) {
					status = SmaGetMulticastFDBTable(port, nodep, lid, block, position, &multicastFDB);
					if (status != FSUCCESS)
					{
						fprintf(stderr, "%*sSMA Get(MFT %u %u) Failed to LID 0x%04x Node 0x%016"PRIx64" Name: %.*s: %s\n", 0, "", block, position, lid,
							nodep->NodeInfo.NodeGUID,
							STL_NODE_DESCRIPTION_ARRAY_SIZE,
							(char*)nodep->NodeDesc.NodeString, iba_fstatus_msg(status));
					} else {
						CopyMulticastFDBBlock( nodep,
							GetMulticastFDBEntry(nodep, block * STL_NUM_MFT_ELEMENTS_BLOCK),
							multicastFDB.MftBlock, MIN(multicastFDBSize-ix,STL_NUM_MFT_ELEMENTS_BLOCK), position );
					}
				}
			}
		}	// End of if (nodep->NodeInfo.NodeType == STL_NODE_SW

	}	// End of for ( p=cl_qmap_head(&fabricp->AllNodes)

	fabricp->flags |= FF_ROUTES;

	if (! quiet) ProgressPrint(TRUE, "Done Getting All FDB Tables");

	return (status);

}	// End of GetAllFDBs()

/* query all forwarding DBs on switch nodes in fabric
 */
FSTATUS GetAllFDBs(EUI64 portGuid, FabricData_t *fabricp, Point *focus, int quiet)
{
	struct oib_port *oib_port_session = NULL;
	FSTATUS fstatus = FSUCCESS;

	fstatus = oib_open_port_by_guid(&oib_port_session, portGuid);
	if (fstatus != FSUCCESS) {
		fprintf(stderr, "%s: Unable to open fabric interface.\n",
				g_Top_cmdname);
	} else {
		if (fabricp->flags & FF_SMADIRECT) {
			fstatus = GetAllFDBsDirect(oib_port_session, fabricp, focus, quiet);
		} else {
			fstatus = GetAllFDBsSA(oib_port_session, fabricp, focus, quiet);
		}
		oib_close_port(oib_port_session);
	}
	return fstatus;
}

/* clear all PortCounters on all ports in fabric
 */
FSTATUS ClearAllPortCounters(EUI64 portGuid, IB_GID localGid, FabricData_t *fabricp,
			   	Point *focus, uint32 counterselect,
			   	boolean limitstats, boolean quiet,
			   	uint32 *node_countp, uint32 *port_countp,
			   	uint32 *fail_node_countp, uint32 *fail_port_countp)
{
	FSTATUS status;
	cl_map_item_t *p;
	int i;
	int num_nodes = cl_qmap_count(&fabricp->AllNodes);

	*node_countp=0;
	*port_countp=0;
	*fail_node_countp=0;
	*fail_port_countp=0;
	if (! quiet) ProgressPrint(TRUE, "Clearing Port Counters...");
#ifdef PRODUCT_OPENIB_FF
	if ((g_paclient_state == PACLIENT_UNKNOWN) && !(fabricp->flags & FF_PMADIRECT)) {
		g_paclient_state = oib_pa_client_init_by_guid(&g_portHandle, portGuid, g_verbose_file);
		if (g_paclient_state < 0) {
			return FERROR;
		}
	} else {
		status = oib_open_port_by_guid(&g_portHandle, portGuid);
		if (status != FSUCCESS) {
			return status;
		}
	}
#else
	status = oib_open_port_by_guid(&g_portHandle, portGuid);
	if (status != FSUCCESS) {
		return status;
	}
#endif
	for (i=0, p=cl_qmap_head(&fabricp->AllNodes); p != cl_qmap_end(&fabricp->AllNodes);
			p = cl_qmap_next(p),i++)
	{
		NodeData *nodep = PARENT_STRUCT(p, NodeData, AllNodesEntry);
		PortData *first_portp;
		cl_map_item_t *q;
#ifdef PRODUCT_OPENIB_FF
		uint16 lid = 0;
#endif
		boolean cleared = FALSE;
		boolean fail = FALSE;

		if (i%PROGRESS_FREQ == 0 || *node_countp == 1)
			if (! quiet) ProgressPrint(FALSE, "Processed %6d of %6d Nodes...", *node_countp, num_nodes);
		if (limitstats && focus && ! CompareNodePoint(nodep, focus))
			continue;
		if (cl_qmap_head(&nodep->Ports) == cl_qmap_end(&nodep->Ports))
			continue; /* no ports */
		/* issue all switch PMA requests to port 0, its only one with a LID */
		if (nodep->NodeInfo.NodeType == STL_NODE_SW) {
			first_portp = PARENT_STRUCT(cl_qmap_head(&nodep->Ports), PortData, NodePortsEntry);
#ifdef PRODUCT_OPENIB_FF
			lid = first_portp->PortInfo.LID;
#endif
			if (g_paclient_state != PACLIENT_OPERATIONAL) {
				status = GetPathToPort(g_portHandle, portGuid, first_portp);
				if (FSUCCESS != status) {
					DBGPRINT("Unable to get Path to Port %d LID 0x%04x Node 0x%016"PRIx64"\n",
						first_portp->PortNum,
						first_portp->EndPortLID,
						first_portp->nodep->NodeInfo.NodeGUID);
					DBGPRINT("    Name: %.*s\n",
						STL_NODE_DESCRIPTION_ARRAY_SIZE,
						(char*)nodep->NodeDesc.NodeString);
					//(*fail_port_countp)+= nodep->NodeInfo.NumPorts; // wrong
					(*fail_port_countp)+= cl_qmap_count(&nodep->Ports); // better
					(*fail_node_countp)++;
					continue;
				}
			}
		} else {
			first_portp = NULL;
		}

		/* to be safe and keep it simple, we issue a clear per port.
		 * ALL_PORT_SELECT is an optional capability not worth the effort to
		 * fetch and check
		 */
		for (q=cl_qmap_head(&nodep->Ports); q != cl_qmap_end(&nodep->Ports); q = cl_qmap_next(q)) {
			PortData *portp = PARENT_STRUCT(q, PortData, NodePortsEntry);
			uint8 ports = 1;	// how many we are doing at a time

			if (focus && ! ComparePortPoint(portp, focus)
				&& (limitstats || ! portp->neighbor || ! ComparePortPoint(portp->neighbor, focus)))
				continue;

#ifdef PRODUCT_OPENIB_FF
			/* use PaClient if available */
			if (g_paclient_state == PACLIENT_OPERATIONAL)
			{
				STL_PA_IMAGE_ID_DATA imageIdQuery = {PACLIENT_IMAGE_CURRENT, 0};

				if (!first_portp)
					lid = portp->PortInfo.LID;
				status = pa_client_clr_port_counters( g_portHandle, imageIdQuery, lid,
					portp->PortNum, counterselect );
			}
#endif
			/* issue direct PMA query */
			else {
				if (! PortHasPma(portp))
					continue;
				if (first_portp) {
					/* switch, issue clear to port 0 */
					// AllPortSelect availability can help out, so ask
					if (! focus && ! nodep->PmaAvoidClassPortInfo)
						(void)STLPmGetClassPortInfo(g_portHandle, first_portp);

					ports = cl_qmap_count(&nodep->Ports);
					status = STLPmClearPortCounters(g_portHandle, first_portp, nodep->NodeInfo.NumPorts, counterselect);
				} else {
					/* CA and router, issue clear to specific port */
					status = GetPathToPort(g_portHandle, portGuid, portp);
					if (FSUCCESS == status) {
						status = STLPmClearPortCounters(g_portHandle, portp, 0, counterselect);
					} else {
						DBGPRINT("Unable to get Path to Port %d LID 0x%04x Node 0x%016"PRIx64"\n",
							portp->PortNum, portp->EndPortLID,
							portp->nodep->NodeInfo.NodeGUID);
						DBGPRINT("    Name: %.*s\n",
							STL_NODE_DESCRIPTION_ARRAY_SIZE,
							(char*)portp->nodep->NodeDesc.NodeString);
					}
				}
			}

			if (FSUCCESS != status) {
				DBGPRINT("Unable to clear Port Counters for Port %d LID 0x%04x Node 0x%016"PRIx64"\n",
					portp->PortNum, portp->EndPortLID,
					portp->nodep->NodeInfo.NodeGUID);
				DBGPRINT("    Name: %.*s\n",
					STL_NODE_DESCRIPTION_ARRAY_SIZE,
					(char*)portp->nodep->NodeDesc.NodeString);
				(*fail_port_countp)+=ports;
				fail = TRUE;
				if (ports > 1)
					break;
				continue;
			}
			(*port_countp)+=ports;
			cleared = TRUE;
			if (ports > 1)
				break;
		}
		if (cleared)
			(*node_countp)++;
		if (fail)
			(*fail_node_countp)++;
	}

	//Close the oib port handle
	if (g_portHandle) {
		oib_close_port(g_portHandle);
		g_portHandle = NULL;
#ifdef PRODUCT_OPENIB_FF
		g_paclient_state = PACLIENT_UNKNOWN;
#endif
	}

	if (! quiet) ProgressPrint(TRUE, "Done Clearing Port Counters");
	return FSUCCESS;	// TBD
}

FSTATUS InitSweepVerbose(FILE *verbose_file)
{
		g_verbose_file = verbose_file;
		return FSUCCESS;
}

// only FF_LIDARRAY fflag is used, others ignored
FSTATUS Sweep(EUI64 portGuid, FabricData_t *fabricp, FabricFlags_t fflags,  SweepFlags_t flags, int quiet)
{
	FSTATUS fstatus;
	struct oib_port *oib_port_session = NULL;

	if (FSUCCESS != InitFabricData(fabricp, fflags)) {
		fprintf(stderr, "%s: Unable to initialize fabric storage area\n",
					   	g_Top_cmdname);
		return FERROR;
	}

	fstatus = oib_open_port_by_guid(&oib_port_session, portGuid);
	if (fstatus != FSUCCESS) {
		fprintf(stderr, "%s: Unable to open fabric interface.\n",
					   	g_Top_cmdname);
		return fstatus;
	}

	time(&fabricp->time);
#ifdef IB_STACK_OPENIB
//	oib_mad_refresh_pkey_glob();
#endif
	// get QLogic master SM data if available
	if ( (FSUCCESS != (fstatus = GetMasterSMData(oib_port_session, portGuid, fabricp, flags, quiet))) &&
			(FUNAVAILABLE != fstatus) )
		goto done;

	// get the data from the SA
	if (FSUCCESS != (fstatus = GetAllNodes(oib_port_session, portGuid, fabricp, flags, quiet)))
		goto done;
	if (FSUCCESS != (fstatus = GetAllLinks(oib_port_session, portGuid, fabricp, quiet)))
		goto done;
	if (FSUCCESS != (fstatus = GetAllCables(oib_port_session, portGuid, fabricp, quiet)))
		goto done;
	if (flags & SWEEP_SM) {
		if (FSUCCESS != (fstatus = GetAllSMs(oib_port_session, portGuid, fabricp, quiet)))
			goto done;
	}
	if (FSUCCESS != (fstatus = GetAllVFs(oib_port_session, portGuid, fabricp, quiet)))
		goto done;
done:
	oib_close_port(oib_port_session);
	return fstatus;
}

/* Get all quarantined node records.
 * Note that caller must free QueryResults.
 */
PQUERY_RESULT_VALUES GetAllQuarantinedNodes(struct oib_port *port, 
											FabricData_t *fabricp, 
											Point *focus, 
											int quiet)
{
	QUERY				query;
	FSTATUS status;
	PQUERY_RESULT_VALUES pQueryResults = NULL;

	memset(&query, 0, sizeof(query));	// initialize reserved fields
	query.InputType 	= InputTypeNoInput;
	query.OutputType 	= OutputTypeStlQuarantinedNodeRecord;

	if (! quiet) ProgressPrint(FALSE, "Getting All Quarantined Node Records...");
	DBGPRINT("Query: Input=%s, Output=%s\n",
				   		iba_sd_query_input_type_msg(query.InputType),
					   	iba_sd_query_result_type_msg(query.OutputType));

	// this call is synchronous
	status = oib_query_sa(port, &query, &pQueryResults);

	if (! pQueryResults)
	{
		fprintf(stderr, "%*sSA QuarantineNodeRecord query Failed: %s\n", 0, "", iba_fstatus_msg(status));
		return (NULL);
	} else if (pQueryResults->Status != FSUCCESS) {
		fprintf(stderr, "%*sSA QuarantineNodeRecord query Failed: %s MadStatus 0x%x: %s\n", 0, "",
				iba_fstatus_msg(pQueryResults->Status),
			   	pQueryResults->MadStatus, iba_sd_mad_status_msg(pQueryResults->MadStatus));
		return (NULL);
	} else if (pQueryResults->ResultDataSize == 0) {
		fprintf(stderr, "%*sNo Quarantine Node Records Returned\n", 0, "");
	} else {
		DBGPRINT("MadStatus 0x%x: %s\n", pQueryResults->MadStatus,
					   				iba_sd_mad_status_msg(pQueryResults->MadStatus));
		DBGPRINT("%d Bytes Returned\n", pQueryResults->ResultDataSize);
	}
	if (! quiet) ProgressPrint(TRUE, "Done Getting All Quarantined Node Records");

	// Note that caller must free QueryResults
	return (pQueryResults);
}
