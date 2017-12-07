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

//===========================================================================//
//									     //
// FILE NAME								     //
//    sm_multicast.c							     //
//									     //
// DESCRIPTION								     //
//    This file contains the functions necessary to implement the multi-     //
//    cast functions for SM/SA.						     //
//									     //
// DATA STRUCTURES							     //
//    NONE								     //
//									     //
// FUNCTIONS								     //
//    NONE								     //
//									     //
// DEPENDENCIES								     //
//    ib_types.h							     //
//    ib_mad.h								     //
//    ib_status.h							     //
//									     //
//									     //
//===========================================================================//

#include "os_g.h"
#include "ib_types.h"
#include "ib_mad.h"
#include "ib_status.h"
#include "ib_macros.h"
#include "ib_sm.h"
#include "ib_sa.h"
#include "cs_g.h"
#include "mai_g.h"
#include "sm_l.h"
#include "sa_l.h"
#include "iba/ib_helper.h"



 __inline__
int find_mlid_offset(Lid_t mLid) {
	return(mLid & STL_LID_MCAST_OFFSET_MASK);
}

McSpanningTrees_t spanningTrees[STL_MTU_MAX+1][IB_STATIC_RATE_MAX+1];

McSpanningTree_t **uniqueSpanningTrees;
int	uniqueSpanningTreeCount = 0;

Node_t *sm_mcSpanningTreeRootSwitch = NULL;
uint64_t sm_mcSpanningTreeRootGuid = 0;
Lock_t sm_mcSpanningTreeRootGuidLock;
/* Number of classes that actually exist */
uint16_t numMcGroupClasses = 0;

McGroupClass_t mcGroupClasses[MAX_SUPPORTED_MCAST_GRP_CLASSES];
McGroupClass_t defaultMcGroupClass = {
	.maximumLids = 0,
	.currentLids = 0,
	.mask.Raw = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	.value.Raw = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};

void sm_pruneMcastSpanningTree(McSpanningTree_t *mcST);

static __inline__
void Dump_SpanningTrees(uint8_t MTU, uint8_t RATE) {
	int		nodeNum;

	printf("ST[%d][%d] ->\n", MTU, RATE);
	if (spanningTrees[MTU][RATE].spanningTree) {
		for (nodeNum = 0; nodeNum < spanningTrees[MTU][RATE].spanningTree->num_nodes; ++nodeNum) {
			printf("\t%3d->%3d,%3d\n",
				(int)spanningTrees[MTU][RATE].spanningTree->nodes[nodeNum].index,
				(int)spanningTrees[MTU][RATE].spanningTree->nodes[nodeNum].nodeno,
				(int)spanningTrees[MTU][RATE].spanningTree->nodes[nodeNum].portno);
		}
	} else {
		printf("Spanning Tree is empty\n\n");
	}

	printf("\n");
}

void dumpSpanningTrees(void)
{
	int mtu, rate;
	for (mtu = IB_MTU_256; mtu <= STL_MTU_MAX; mtu = getNextMTU(mtu)) {
		for (rate = IB_STATIC_RATE_MIN; rate <= IB_STATIC_RATE_MAX; ++rate) {
			Dump_SpanningTrees(mtu, rate);
			printf("\n");
		}
	}
}

void sm_spanning_tree_resetGlobals(void) {
	memset(spanningTrees,0,sizeof(spanningTrees));
}

/* If filter_mtu_rate is set, the SM will:
         a) construct spanning tree by selecting only those ISLs whose mtu and rate are greater than
            equal to specified mtu and rate.
         b) Only consider links that are ACTIVE.

   If filter_mtu_rate is not set, the SM does not consider mtu and rate of ISLs. It will consider
   any ISL that is atleast in INIT state.

*/

Status_t sm_ideal_spanning_tree(McSpanningTree_t *mcST, int filter_mtu_rate, int32_t mtu, int32_t rate, int num_nodes, int *complete)
{
	int			i, j, k;
	int			num_links = 0;
	Node_t			*tmp_nodep;
	Node_t			*nodep;
	Port_t			*portp;
	McNode_t		*mcNodes;
	SwitchList_t	*swlist_head, *prev_sw, *peer_sw, *tmp;

	mcNodes = mcST->nodes;

	/* For constructing the spanning tree, start at the mcSpanningTreeRoot switch and do
	 * a BFS from there by walking through its connections to other switches
	 */
	swlist_head = NULL;
	prev_sw = NULL; 
	peer_sw = NULL;
	nodep = sm_mcSpanningTreeRootSwitch;

	while (nodep != NULL) {
		for_all_physical_ports(nodep, portp) {
			if (!sm_valid_port(portp)) {
				continue;
			}
			if (filter_mtu_rate) {
				if ( portp->state < IB_PORT_ACTIVE) {
					continue;
				}
			} else {
				if ( portp->state <= IB_PORT_DOWN) {
					continue;
				}
			}

			if (portp->nodeno == (int32_t)-1 || portp->portno == (int32_t)-1) {	// switch to switch link broken
				continue;
			}

			if (portp->nodeno == sm_mcSpanningTreeRootSwitch->index) continue;

			if (portp->nodeno == nodep->index) continue;

			tmp_nodep = sm_find_node(sm_topop, portp->nodeno);
			if (!tmp_nodep || tmp_nodep->nodeInfo.NodeType != NI_TYPE_SWITCH) {
				continue;
			}

			/* check if this switch needs to be added to the list of switches
			 * whose links we have to check as part of the BFS
			 */
			if (!tmp_nodep->mcSpanningChkDone && !is_switch_on_list(swlist_head, tmp_nodep)) {
				/* add peer switch to list to analyze its links to other switches later on*/
				peer_sw = (SwitchList_t *) malloc(sizeof(SwitchList_t));
				if (peer_sw == NULL) {
					IB_LOG_ERROR0("can't malloc node!");
					return VSTATUS_NOMEM;
				}

				if (swlist_head == NULL)
					swlist_head = peer_sw;

				peer_sw->switchp = tmp_nodep;
				peer_sw->next = NULL;
				if (prev_sw)
					prev_sw->next = peer_sw;
				prev_sw = peer_sw;
			}

			if (filter_mtu_rate) {
				if (portp->portData->maxVlMtu < mtu) {	// MTU too small, skip
					continue;
				}

				if (linkrate_lt(linkWidthToRate(portp->portData), rate)) {	// link speed to low, skip
					continue;
				}
			}

			j = -1;
			k = -1;
			for (i = 0; i < num_nodes; i++) {
				if (mcNodes[i].index == portp->nodeno) {
					j = i;
				}
				if (mcNodes[i].index == nodep->index) {
					k = i;
				}

				if (j >= 0 && k >= 0)
					break;
			}
			if (mcNodes[j].nodeno < 0) {
				mcNodes[j].nodeno = nodep->index;
				mcNodes[j].portno = portp->index;
                mcNodes[j].parent = &mcNodes[k];
				++num_links;
			}
		}

		nodep->mcSpanningChkDone = 1;

		if (swlist_head) {	
			nodep = swlist_head->switchp;
			tmp = swlist_head;
			swlist_head	= swlist_head->next;
			if (prev_sw == tmp)
				prev_sw = swlist_head;
			free(tmp);
		} else {
			nodep = NULL;
		}
	}

	if (num_links == (num_nodes - 1))
		*complete = 1;
	else
		*complete = 0;

	return VSTATUS_OK;
}

// sm_spanning_tree
//
// For a given rate and mtu, this function computes a minimum
// spanning tree between all switches in the fabric.
//
// If Multicast RootSelectionAlgorithm is set to LeastTotalCost
// or LeastWorstCaseCost  we compute the spanning tree by selecting
// the root of the spanning tree to be a switch that has a low cost
// associated with it based on the floyd cost computations (sm_routing_calc_floyds).
// This is done in sm_ideal_spanning_tree().
//
// If Multicast RootSelectionAlgorithm is set to SMNeighbor, we use
// the below (legacy) approach:
//
// This is black magic of the highest order... For a given
// rate and mtu, it computes a minimum spanning tree between all
// switches in the fabric. This tree is then used in the programming
// of the multicast forwarding tables of the switches. The really
// spooky black-magic part of the algorithm used in computing the
// spanning tree is that the algorithm assumes that the order in
// which switches are returned from the for_all_switch_nodes macro
// is the same order in which they were discovered (breadth first
// search from the SM node).
// 
// Needless to say, if discovery ever modifies the order in which
// switches are discovered during a sweep, this algorithm will break.
//
// There will be MTU's and rates that the fabric does not support,
// and this algorith cannot generate a spanning tree for these values.
// We detect these values by maintaining a count of the number of
// edges/links calculated in the MST... For N nodes, an MST must have
// N - 1 edges. At the end of our calculations, we mark any incomplete
// spanning tree as invalid by freeing the memory associated with it
// and setting the pointer in the 'spanningTrees' array to NULL.
//
// TODO: There are several enhancements that can be made to this
// function:
//
// - First, we do not calculate the optimum minimum spanning tree. The
// optimum spanning tree would be one that minimizes the depth of the
// tree. The algorithm here could quite possibly come up with a spanning
// tree that looks more like a linked list than a tree.
//
// - Second, we could conceivably preserve the partial spanning trees
// calculated for higher MTU's or rates if we ever decide to allow nodes
// to create mcast groups with higher MTU's/rates than the entire fabric
// supports.
//
Status_t 
sm_spanning_tree(int32_t mtu, int32_t rate, int *complete) {
	int			i, j;
	int			num_nodes = 0, num_links = 0;
	void			*address;
	uint32_t		bytes;
	Node_t			*tmp_nodep;
	Node_t			*nodep;
	Port_t			*portp;
	McNode_t		*mcNodes;
	Topology_t		*tp;
	McSpanningTree_t	*mcST;

	IB_ENTER(__func__, 0, 0, 0, 0);

	tp = (Topology_t *)&sm_newTopology;

    /*
     * Count the nodes.
     * MWHEINZ FIXME: Why aren't we using the data in the topology structure?
     */
	num_nodes = 0;
	for_all_switch_nodes(sm_topop, nodep) {
		num_nodes++;
	}

    /*
     * Allocate space for the nodes.
     * MWHEINZ FIXME: What's up with the 32 bytes of padding?
     */
	bytes = 32 + sizeof(McSpanningTree_t) + (num_nodes * sizeof(McNode_t));
	if (vs_pool_alloc(&sm_pool, bytes, (void **)&address) != VSTATUS_OK) {
		IB_EXIT(__func__, VSTATUS_BAD);
		return(VSTATUS_BAD);
	}

    /*
     * Free the old spanning tree in this space.
     */
	if (!spanningTrees[mtu][rate].copy && (spanningTrees[mtu][rate].spanningTree != NULL)) {
		vs_pool_free(&sm_pool, (void *)spanningTrees[mtu][rate].spanningTree);
	}

	memset((void *)address, 0, bytes);
	spanningTrees[mtu][rate].spanningTree = (McSpanningTree_t *)address;
	spanningTrees[mtu][rate].copy = 0;

	uniqueSpanningTrees[uniqueSpanningTreeCount] = spanningTrees[mtu][rate].spanningTree;
	uniqueSpanningTreeCount++;

	mcST = spanningTrees[mtu][rate].spanningTree;
	mcST->num_nodes = num_nodes;
	mcST->nodes = (McNode_t *)(mcST+1);

    /*
     * Initialize the nodes.
     */
	/* PR 123715: make sure we don't use an old nodep... */
	sm_mcSpanningTreeRootSwitch = NULL; 
	i = 0;
	mcNodes = mcST->nodes;
	for_all_switch_nodes(sm_topop, nodep) {
		if (sm_useIdealMcSpanningTreeRoot &&
			 (sm_mcSpanningTreeRootGuid == nodep->nodeInfo.NodeGUID)) {
			sm_mcSpanningTreeRootSwitch = nodep;
		}
		mcNodes[i].index = nodep->index;
		mcNodes[i].nodeno = -1;
		mcNodes[i].portno = -1;
        mcNodes[i].parent = NULL;
		mcNodes[i].mft_mlid_init = 0;
		nodep->mcSpanningChkDone = 0;
		i++;
	}


	if (sm_useIdealMcSpanningTreeRoot) {
		if (sm_mcSpanningTreeRootSwitch) {
			Status_t status;
			/* Compute the ideal spanning tree */
			status = sm_ideal_spanning_tree(mcST, 1, mtu, rate, num_nodes, complete);
			if (status == VSTATUS_OK)
				return status;
		} else {
			if (tp->num_sws != 0)
				IB_LOG_ERROR_FMT(__func__, "Did not find ideal multicast spanning tree root."
						 "Falling back to default for constructing spanning tree for MTU %s Rate %s", Decode_MTU(mtu), StlStaticRateToText(rate));
		}
		/* Fall back to old approach if there are errors in building
		 * ideal spanning tree.
		 */
	}
    /*
     * Compute the spanning tree.
     */
	for (i = 0; i < num_nodes-1; i++) {
		if ((nodep = sm_find_node(sm_topop, mcNodes[i].index)) == NULL) continue;
		for_all_physical_ports(nodep, portp) {
			if (!sm_valid_port(portp) || portp->state < IB_PORT_ACTIVE) {	// not ACTIVE, skip
				continue;
			}

			if (portp->nodeno == (int32_t)-1 || portp->portno == (int32_t)-1) {	// switch to switch link broken
				continue;
			}

			if (portp->portData->maxVlMtu < mtu) {	// MTU too small, skip
				continue;
			}

			if (linkrate_lt(linkWidthToRate(portp->portData), rate)) {	// link speed to low, skip
				continue;
			}

			tmp_nodep = sm_find_node(sm_topop, portp->nodeno);
			if (!tmp_nodep || tmp_nodep->nodeInfo.NodeType != NI_TYPE_SWITCH) {
				continue;
			}

			for (j = i+1; j < num_nodes; j++) {
				if (mcNodes[j].index == portp->nodeno) {
					if (mcNodes[j].nodeno < 0) {
						mcNodes[j].nodeno = nodep->index;
						mcNodes[j].portno = portp->index;
                        mcNodes[j].parent = &mcNodes[i];
						++num_links;
					}
					break;
				}
			}
		}
	}

	if (num_links == (num_nodes - 1))
		*complete = 1;
	else
		*complete = 0;

	// Dump_SpanningTrees(mtu, rate);

	IB_EXIT(__func__, VSTATUS_OK);
	return(VSTATUS_OK);
}

void sm_build_spanning_trees(void)
{
	int i = 0, j = 0, complete = 0;
	int mtu = 0, rate = 0, max_mtu = 0, max_rate = 0;

	uniqueSpanningTreeCount = 0;

	if (sm_mc_config.disable_mcast_check != McGroupBehaviorRelaxed) {
		/* maxMcastMtu, maxMcastRate are the smallest ISL Mtu and Rate seen in the fabric
		 * and when McGroup behavior is not relaxed, only Multicast groups with mtu and rate
		 * less than or equal to these values are allowed.
		 * Hence we need to build a spanning tree only for this MTU and Rate as that spanning
		 * tree can support all multicast groups. Also such a spanning tree will be complete
		 * because it is based on the smallest ISL Mtu and Rate seen in the fabric.
		 * MTU/Rate greater than the max MTU and Rate can be skipped as they will not be used
		 * anyway, because of the strict MC group checking.
		 */
		mtu = sm_newTopology.maxMcastMtu;
		rate = sm_newTopology.maxMcastRate;
		if (mtu < 0 || mtu > STL_MTU_MAX)
			mtu = STL_MTU_MAX;
		if (rate < 0 || rate > IB_STATIC_RATE_MAX)
			rate = IB_STATIC_RATE_MAX;

		sm_spanning_tree(mtu, rate, &complete);

		for (i = IB_MTU_256; i <= STL_MTU_MAX; i = getNextMTU(i)) {
			for (j = IB_STATIC_RATE_MIN; j <= IB_STATIC_RATE_MAX; j++) {
				if ((i == mtu) && (j == rate))
					continue;

				if ((i > mtu) || (linkrate_gt(j, rate))) {
					if (!spanningTrees[i][j].copy && (spanningTrees[i][j].spanningTree != NULL)) {
							vs_pool_free(&sm_pool, (void *)spanningTrees[i][j].spanningTree);
							spanningTrees[i][j].spanningTree = NULL;
					}
					continue;
				}

				if (!spanningTrees[i][j].copy && (spanningTrees[i][j].spanningTree != NULL)) {
					vs_pool_free(&sm_pool, (void *)spanningTrees[i][j].spanningTree);
				}
				spanningTrees[i][j].spanningTree = spanningTrees[mtu][rate].spanningTree;
				spanningTrees[i][j].copy = 1;			
			}
		}
		return;
	}


	/* First build the spanning tree for maximum ISL MTU/Rate discovered in the fabric.
	 * If its complete then that spanning tree is sufficient. If not, start building
	 * spanning trees for other MTU/Rates. Whenever we get a complete
	 * spanning tree, it can be copied for MTU and Rates smaller than that.
	 * MTU/Rate greater than the max MTU and Rate can be skipped as they will anyway result
	 * in an empty spanning tree.
	 */
	max_mtu = sm_newTopology.maxISLMtu;
	max_rate = sm_newTopology.maxISLRate;

	if (max_mtu < 0 || max_mtu > STL_MTU_MAX)
		max_mtu = STL_MTU_MAX;
	if (max_rate < 0 || max_rate > IB_STATIC_RATE_MAX)
		max_rate = IB_STATIC_RATE_MAX;

	sm_spanning_tree(max_mtu, max_rate, &complete);

	if (complete) {
		mtu = max_mtu;
		rate = max_rate;
	}

	for (i = STL_MTU_MAX; i >= IB_MTU_256; i = getPrevMTU(i)) {
		for (j = IB_STATIC_RATE_MAX; j >= IB_STATIC_RATE_MIN; j--) {
			if ((i == max_mtu) && (j == max_rate))
				continue;

			if ((i > max_mtu) || (linkrate_gt(j, max_rate))) {
				if (!spanningTrees[i][j].copy && (spanningTrees[i][j].spanningTree != NULL)) {
						vs_pool_free(&sm_pool, (void *)spanningTrees[i][j].spanningTree);
						spanningTrees[i][j].spanningTree = NULL;
				}
				continue;
			}

			if (complete && (mtu >= i) && linkrate_ge(rate, j)) {
				if (!spanningTrees[i][j].copy && (spanningTrees[i][j].spanningTree != NULL)) {
					vs_pool_free(&sm_pool, (void *)spanningTrees[i][j].spanningTree);
				}
				spanningTrees[i][j].spanningTree = spanningTrees[mtu][rate].spanningTree;
				spanningTrees[i][j].copy = 1;
			} else {
				sm_spanning_tree(i, j, &complete);
				if (complete) {
					mtu = i;
					rate = j;
				}
			}
		}
	}
}

// -------------------------------------------------------------------------- //
static int
findFirstPortInMftBlock(STL_PORTMASK *mftBlock, int blockPos) {
    int i, j;
    for (i = 0, j = 1; i < STL_PORT_MASK_WIDTH; i++, j *= 2) {
        if (*mftBlock & j)
            return blockPos + i;
    }
    return -1;
}

#define	Mft_Position(X)			((X)/STL_PORT_MASK_WIDTH)
#define	Mft_PortmaskBit(X)		((uint64)1 << (X)%STL_PORT_MASK_WIDTH)


/* sm_check_mc_groups_realizable - checks multicast groups to see if they have become unrealizable
 * due to topology/mtu/rate changes, or if for some other reason we did not compute a valid spanning
 * tree and marks such groups  as unrealizable.
 * It also stores the mlid of the first mc group that matches the spanning tree's mtu/rate.
 * Later on, that mlid offset is used in switch->mft[] while computing the port mask for the spanning tree.
 */

void sm_check_mc_groups_realizable(void)
{
	McSpanningTree_t	*tree;	
	McGroup_t	*mcGroup;
	Topology_t *topo;
	int mlid_offset;

	topo = &sm_newTopology;

	for_all_multicast_groups(mcGroup) {
		tree = spanningTrees[mcGroup->mtu][mcGroup->rate].spanningTree;
		if (sm_mc_config.disable_mcast_check != McGroupBehaviorRelaxed)
		{
			if (  (tree == NULL)
			   || (mcGroup->mtu > topo->maxMcastMtu)
			   || linkrate_gt(mcGroup->rate, topo->maxMcastRate) ) {
	   
				if ((mcGroup->flags & McGroupUnrealizable) == 0) {
					mcGroup->flags |= McGroupUnrealizable;
					// Only emit this message once
					smCsmLogMessage(CSM_SEV_NOTICE, CSM_COND_OTHER_ERROR, getMyCsmNodeId(), NULL,
				                "Multicast group "FMT_GID" with mtu of %s and rate of %s has become "
				                "unrealizable due to fabric changes; current fabric mtu: %s, current "
				                "fabric rate: %s", 
								mcGroup->mGid.Type.Global.SubnetPrefix,
								mcGroup->mGid.Type.Global.InterfaceID, 
								Decode_MTU(mcGroup->mtu),
				                StlStaticRateToText(mcGroup->rate), Decode_MTU(topo->maxMcastMtu),
				                StlStaticRateToText(topo->maxMcastRate));
				}
				continue;
			} else if (mcGroup->flags & McGroupUnrealizable) {
				// Group had become unrealizable and now is back to being realizable
				mcGroup->flags &= ~McGroupUnrealizable;

				smCsmLogMessage(CSM_SEV_NOTICE, CSM_COND_OTHER_ERROR, getMyCsmNodeId(), NULL,
				                "Return to normal: Multicast group "FMT_GID" has been restored "
				                "by fabric changes; current fabric mtu: %s, current "
				                "fabric rate: %s", mcGroup->mGid.Type.Global.SubnetPrefix,
								mcGroup->mGid.Type.Global.InterfaceID,
								Decode_MTU(topo->maxMcastMtu), StlStaticRateToText(topo->maxMcastRate));
			}
		} else {
			if (  (tree == NULL)
			   || (mcGroup->mtu > topo->maxISLMtu)
			   || linkrate_gt(mcGroup->rate, topo->maxISLRate) ) {
				/* the group's MTU, rate are higher than the maximum MTU, ISL we found in the fabric 
			 	 * which means, the spanning tree for that mtu/rate would be NULL anyway.
				 */
				if ((mcGroup->flags & McGroupUnrealizable) == 0) {
					mcGroup->flags |= McGroupUnrealizable;
					// Only emit this message once
					smCsmLogMessage(CSM_SEV_NOTICE, CSM_COND_OTHER_ERROR, getMyCsmNodeId(), NULL,
					                "Multicast group "FMT_GID" with mtu of %s and rate of %s has become "
					                "unrealizable due to fabric changes; current fabric maximum ISL mtu: %s, "
					                "rate: %s", 
									mcGroup->mGid.Type.Global.SubnetPrefix,
									mcGroup->mGid.Type.Global.InterfaceID, 
									Decode_MTU(mcGroup->mtu),
					                StlStaticRateToText(mcGroup->rate), Decode_MTU(topo->maxISLMtu),
				    	            StlStaticRateToText(topo->maxMcastRate));
				}
				continue;
			} else if (mcGroup->flags & McGroupUnrealizable) {
				// Group had become unrealizable and now is back to being realizable
				mcGroup->flags &= ~McGroupUnrealizable;

				smCsmLogMessage(CSM_SEV_NOTICE, CSM_COND_OTHER_ERROR, getMyCsmNodeId(), NULL,
			    	            "Return to normal: Multicast group "FMT_GID" has been restored "
			        	        "by fabric changes; current maximum ISL mtu: %s, rate: %s",
			            	    mcGroup->mGid.Type.Global.SubnetPrefix,
								mcGroup->mGid.Type.Global.InterfaceID,
								Decode_MTU(topo->maxISLMtu), StlStaticRateToText(topo->maxISLRate));
			}
		}

		mlid_offset = find_mlid_offset(mcGroup->mLid);
		if (mlid_offset >= sm_mcast_mlid_table_cap) {
			IB_LOG_ERROR_FMT(__func__,
			"sm_setup_spanning_tree_port_masks - Multicast group "FMT_GID" MLID 0x%x exceeds MLIDTableCap %u\n",
			mcGroup->mGid.Type.Global.SubnetPrefix,	mcGroup->mGid.Type.Global.InterfaceID, 
			mcGroup->mLid, sm_mcast_mlid_table_cap);

			mcGroup->flags |= McGroupUnrealizable;
			continue;
		}

		if (tree && !tree->first_mlid) {
			tree->first_mlid = mcGroup->mLid;
		}
	}
}

/* sm_calculate_spanning_tree_port_mask - Computes the port masks that correspond to each
 * spanning tree. It does this by tracing the spanning trees, and setting the corresponding
 * port bits in switch->mft[]. It stores the port masks in the tree->first_mlid offset
 * of switch->mft[].  Note that the portion of the port mask corresponding to the spanning
 * tree will be the same for all MLIDs that share the same spanning tree.
 */

void sm_calculate_spanning_tree_port_mask(void)
{
	McSpanningTree_t	*tree;	
	McNode_t	*mcNode;
	Port_t 	*parent_portp = NULL, *portp = NULL;
	Node_t  *parent_nodep = NULL, *nodep = NULL;
	int		i, j, mlid_offset;
	Topology_t *topo;

	topo = &sm_newTopology;

	for (i=0; i < uniqueSpanningTreeCount; i++) {
		tree = uniqueSpanningTrees[i];
		if (!tree->first_mlid)
			continue;
		mlid_offset = find_mlid_offset(tree->first_mlid);
		for (j=0; j < tree->num_nodes; j++) {
			mcNode = &tree->nodes[j];
			if (mcNode && mcNode->nodeno >= 0) {
				parent_portp = sm_find_port(topo, mcNode->nodeno, mcNode->portno);
				parent_nodep = sm_find_node(topo, mcNode->nodeno);
				if (sm_valid_port(parent_portp)) {
					portp = sm_find_port(topo, parent_portp->nodeno, parent_portp->portno);
					nodep = sm_find_node(topo, parent_portp->nodeno);
				} else {
					IB_LOG_INFINI_INFO_FMT(__func__,
						"can't find port %d for nodeno %d", mcNode->portno, mcNode->nodeno);
					continue;
				}

				/* Set bits in the port mask in the switch corresponding to mcNode*/
				if (sm_valid_port(portp) && nodep) {
					if (mcNode->mft_mlid_init != tree->first_mlid) {
						nodep->mft[mlid_offset][Mft_Position(portp->index)] = Mft_PortmaskBit(portp->index);
						mcNode->mft_mlid_init = tree->first_mlid;
					} else {
						nodep->mft[mlid_offset][Mft_Position(portp->index)] |= Mft_PortmaskBit(portp->index);
					}
				} else {
					IB_LOG_INFINI_INFO_FMT(__func__,
						   "can't find port %d for nodeno %d",
						   parent_portp->portno, parent_portp->nodeno);
				}

				/* Set bits in the port mask in the switch corresponding to mcNode's parent */
				if (sm_valid_port(parent_portp) && parent_nodep) {
					if (mcNode->parent->mft_mlid_init != tree->first_mlid) {
						parent_nodep->mft[mlid_offset][Mft_Position(parent_portp->index)] = Mft_PortmaskBit(parent_portp->index);
						mcNode->parent->mft_mlid_init = tree->first_mlid;
					} else {
						parent_nodep->mft[mlid_offset][Mft_Position(parent_portp->index)] |= Mft_PortmaskBit(parent_portp->index);
					}
				}
			} else if (!mcNode) {
				IB_LOG_ERROR0("sm_calculate_spanning_tree_port_mask - spanning tree entry is null");
			}
		}
	}
}

void sm_add_mcmember_port_masks(void)
{
	McGroup_t	*mcGroup;
	McMember_t	*mcMember;
	Port_t 	*portp = NULL;
	Node_t  *nodep = NULL;
	Topology_t *topo;
	int mlid_offset;

	topo = &sm_newTopology;

	for_all_multicast_groups(mcGroup) {
		if (mcGroup->flags & McGroupUnrealizable)
			continue;
		if (!mcGroup->mcMembers)
			continue;
		mlid_offset = find_mlid_offset(mcGroup->mLid);
		for_all_multicast_members(mcGroup, mcMember) {
			if (mcMember->portGuid == SA_FAKE_MULTICAST_GROUP_MEMBER) continue;
			portp = sm_find_active_port_guid(topo, mcMember->portGuid);
			if (!sm_valid_port(portp)) {
				if (smDebugPerf) {
					/* this is ok; host(s) gone and we haven't cleaned up the mcmember tables yet */
					IB_LOG_VERBOSE_FMT(__func__, 
						   "Could not find active port for mcMember->portGuid "FMT_U64" in new topology", 
						   mcMember->portGuid);
				}
				continue;
			}
			nodep = sm_find_node(topo, portp->nodeno);
			if (!nodep) {
				IB_LOG_ERROR_FMT(__func__,
						   "Could not find the switch for mcMember->portGuid "FMT_U64" in new topology", 
						   mcMember->portGuid);
				continue;
			}
			if (nodep->nodeInfo.NodeType != NI_TYPE_SWITCH) {
				// hca to hca
				continue;
			}
			if ((mcMember->record.JoinNonMember) || 
				(mcMember->record.JoinFullMember)) {
				nodep->mft[mlid_offset][Mft_Position(portp->portno)] |= Mft_PortmaskBit(portp->portno);
			} else if (mcMember->record.JoinSendOnlyMember) {
				// mark this switch as having a send only broadcast group member so we do not prune
				nodep->hasSendOnlyMember = 1;
			}
		}
   	}
}

Status_t sm_calculate_mfts()
{
	McSpanningTree_t	*tree;	
	McGroup_t	*mcGroup;
	Port_t 	 *portp = NULL;
	Node_t   *nodep = NULL, *oldswp = NULL;
	int		i, mlid_offset, first_mlid_offset, mlid_count;
	Lid_t	maxmcLid;
	Topology_t *topo;
	Status_t status;

	topo = &sm_newTopology;

	for (i=0; i < uniqueSpanningTreeCount; i++) {
		uniqueSpanningTrees[i]->first_mlid = 0;
	}

	if ((status = vs_lock(&sm_McGroups_lock)) != VSTATUS_OK) {
		IB_LOG_ERRORRC("Failed to get sm_McGroups_lock rc:", status);
		return status;
	}

	sm_check_mc_groups_realizable();


	sm_calculate_spanning_tree_port_mask();

	/* Copy the spanning tree port mask to all the MLIDs from tree->first_mlid where
	 * it is stored by sm_calculate_spanning_tree_port_masks().
	 */

	maxmcLid = sm_multicast_get_max_lid();

	for_all_switch_nodes(topo, nodep) {
		mlid_count = find_mlid_offset(maxmcLid);
		for (mlid_offset = 0 ; mlid_offset <= mlid_count; mlid_offset++) {
			tree = NULL;
			if (uniqueSpanningTreeCount == 1) {
				tree = uniqueSpanningTrees[0];
			} else {
				for_all_multicast_groups(mcGroup) {
					if (mcGroup->flags & McGroupUnrealizable)
						continue;
					if (mcGroup->mLid == (maxmcLid-mlid_offset)) {
						tree = spanningTrees[mcGroup->mtu][mcGroup->rate].spanningTree;
						break;
					}
				}
			}
			if (!tree)
				continue;
			if (!tree->first_mlid)
				continue;
			if (mlid_offset != find_mlid_offset(tree->first_mlid)) {
                		first_mlid_offset = find_mlid_offset(tree->first_mlid);
				memcpy((void *)nodep->mft[mlid_offset], (void *)nodep->mft[first_mlid_offset],
						 sizeof(STL_PORTMASK) * STL_NUM_MFT_POSITIONS_MASK);
			}
		}
        /* see if switch mft needs full programming */
        if (!nodep->mftChange && topology_passcount && sm_valid_port((portp = sm_get_port(nodep, 0)))) {
            if ((oldswp = lidmap[portp->portData->lid].oldNodep)) {
                if (  oldswp->switchInfo.u1.s.PortStateChange
                   || nodep->switchInfo.u1.s.PortStateChange
                   || nodep->portsInInit) {
                    if (!nodep->mftChange) {
                        nodep->mftChange = 1;
                    }
                }
			} else {
                /* switch just came in fabric, mark it */
                if (!nodep->mftChange) {
                    nodep->mftChange = 1;
                }
            }
        }
	}

	/* Set the bits in port masks corresponding to ports for McMembers */
	sm_add_mcmember_port_masks();

    if (sm_mc_config.enable_pruning) {
		for (i = 0; i < uniqueSpanningTreeCount; i++) {
			sm_pruneMcastSpanningTree(uniqueSpanningTrees[i]);
		}
	}

	/* check if port masks have changed since last sweep */
	for_all_switch_nodes(topo, nodep) {
		mlid_count = find_mlid_offset(maxmcLid);
		for (mlid_offset = 0 ; mlid_offset <= mlid_count; mlid_offset++) {
	        if (!nodep->mftPortMaskChange && topology_passcount && sm_valid_port((portp = sm_get_port(nodep, 0)))) {
	   	        if ((oldswp = lidmap[portp->portData->lid].oldNodep)) {
					if (memcmp((void *)nodep->mft[mlid_offset], (void *)oldswp->mft[mlid_offset], sizeof(STL_PORTMASK) * STL_NUM_MFT_POSITIONS_MASK))
						nodep->mftPortMaskChange = 1;
				}
        	}		
		}
	}

   (void)vs_unlock(&sm_McGroups_lock);

	return VSTATUS_OK;
}

void sm_pruneMcastSpanningTree(McSpanningTree_t *mcST)
{
	int			i, j;
    int         loneEntryPos=0;
    int         portNum;
    int         count=0;
	Port_t			*portp;
	McNode_t		*mcNode;
	Node_t			*switchp;
	Node_t			*neighborSw;
    STL_PORTMASK	*swMftEntry=NULL;
	Topology_t		*topo;
	int				mlid_offset, mlid_count;
	Lid_t			maxmcLid;

	topo = &sm_newTopology;

	maxmcLid = sm_multicast_get_max_lid();

     // prune out switches with only one entry
	mlid_count = find_mlid_offset(maxmcLid);
	for (mlid_offset = 0 ; mlid_offset <= mlid_count; mlid_offset++) {
        	for (i = 0; i < mcST->num_nodes; i++) {
            		mcNode = &mcST->nodes[i];
            		if (!mcNode) {
                		IB_LOG_ERROR0("sm_pruneMcastSpanningTree - spanning tree entry is null");
                		continue;
            		}
            		// from this node, walk up the branch from the leaf pruning
            		// as many nodes as possible
            		while (mcNode) {
                		switchp = sm_find_node(topo, mcNode->index);
                		if (!switchp) {
                    		IB_LOG_ERROR_FMT(__func__,
                           		"can't find switch node for nodeno %d",
                           		mcNode->index);
                    			break;
                		}
                		// FIXME: this flag should be group specific
                		if (switchp->hasSendOnlyMember) break;
                		// don't try to get an accurate count... just 0, 1, or 2+
                		// is enough to go on
                		count = 0;
                		for (j = 0; j < STL_NUM_MFT_POSITIONS_MASK && j * STL_PORT_MASK_WIDTH <= switchp->nodeInfo.NumPorts; j++) {
                    			if (switchp->mft[mlid_offset][j]) {
                        			if (switchp->mft[mlid_offset][j] & (switchp->mft[mlid_offset][j] - 1)) {
                            			// more than one port in this block
                            			count = 2;
                            			break;
                        			}
                        			if (count++) break;
                        			// found exactly 1 so far... save it
                        			swMftEntry = &switchp->mft[mlid_offset][j];
                        			loneEntryPos = j;
                    			}
                		}
                		//IB_LOG_INFINI_INFO_FMT(__func__,
                		//       "NodeGUID "FMT_U64" [%s]: mlid 0x%04x, count %d", 
                		//       switchp->nodeInfo.NodeGUID, sm_nodeDescString(switchp), mcGroup->mLid, count);
                		// clear it if there is only one port set
                		if (  count == 1
                   		&& (portNum = findFirstPortInMftBlock(swMftEntry, loneEntryPos)) >= 0) {
                    			//IB_LOG_INFINI_INFO_FMT(__func__, "clearing mft entry for node[%d], port[%d], MCGLid 0x%4X", 
                    			//       mcNode->index, portNum, mcGroup->mLid);
                    			// clear this switch's mft and the corresponding entry of it's neighbor
                    			*swMftEntry ^= Mft_PortmaskBit(portNum);
					if (mcNode->nodeno < 0) {
						mcNode = mcNode->parent;
						continue;
					}
                    			// clear the neighbor
                    			if ((neighborSw = sm_find_node(topo, mcNode->nodeno)) != NULL
                       			&& (portp = sm_find_node_port(topo, neighborSw, mcNode->portno)) != NULL) {
                        			//IB_LOG_INFINI_INFO_FMT(__func__, "clearing neighborSw mft entry for node[%d], port[%d], MCGLid 0x%4X", 
                        			//       mcNode->nodeno, mcNode->portno, mcGroup->mLid);
                        			neighborSw->mft[mlid_offset][Mft_Position(portp->index)] ^= Mft_PortmaskBit(portp->index);
                    			} else {
                        			IB_LOG_WARN_FMT(__func__, 
                               			"Could not find neighbor for NodeGUID "FMT_U64" [%s] (neighbor idx %d, port %d) in new topology; spanning tree not up to date",
                               			switchp->nodeInfo.NodeGUID, sm_nodeDescString(switchp),
                               				mcNode->nodeno, mcNode->portno);
                    			}
                    			mcNode = mcNode->parent;
				} else {
                    			break;
				}
			}
		}
	}	
}

/*
 *  copy the old mfts to new topology
 */
void sm_multicast_switch_mft_copy() {
	Topology_t		*newtopop;
	Node_t			*oldswp, *newswp;
	Port_t			*portp;
    int             mlid_offset, mlid_count;
	Lid_t			maxmcLid;

    newtopop = (Topology_t *)&sm_newTopology;

	(void)vs_lock(&sm_McGroups_lock);
	maxmcLid = sm_multicast_get_max_lid();
	(void)vs_unlock(&sm_McGroups_lock);
	ASSERT(find_mlid_offset(maxmcLid) < sm_mcast_mlid_table_cap);

    for_all_switch_nodes(newtopop, newswp) {
        if (sm_valid_port((portp = sm_get_port(newswp,0)))) {
            if ((oldswp = lidmap[portp->portData->lid].oldNodep)) {
		mlid_count = find_mlid_offset(maxmcLid);
		for (mlid_offset = 0 ; mlid_offset <= mlid_count; mlid_offset++) {
                    /* copy the mft over into new topology */
                    memcpy(newswp->mft[mlid_offset], oldswp->mft[mlid_offset],
                           sizeof(STL_PORTMASK) * STL_NUM_MFT_POSITIONS_MASK);
                }
            }
        }
    }
}


Status_t sm_set_all_mft(int force, Topology_t *prev_tp)
{
	static Lid_t	oldMaxmcLid 	= 0;
	Status_t		status;
	Status_t		worstStatus = VSTATUS_OK;
	Node_t			*switchp, *oldswp = NULL;
    Port_t          *portp;
	Lid_t			lid;
	Lid_t			newMaxmcLid;
	Lid_t			maxmcLid;
    uint16_t		numBlocks = 1;
	int				i, j;
	STL_MULTICAST_FORWARDING_TABLE mft;
	uint32_t		amod;
	Topology_t		*topo;
    uint64_t        sTime, eTime;
	int             dispatched = 0, mftBlockChange = 0;
	uint16_t		old_portMask = 0;
	int mlid_offset, mlid_count;

	if ((status = vs_lock(&sm_McGroups_lock)) != VSTATUS_OK) {
		IB_LOG_ERRORRC("Failed to get sa lock rc:", status);
		return status;
	}

    	if (smDebugPerf) {
 	       vs_time_get(&sTime);
        IB_LOG_INFINI_INFO0("START MFT set for switches");
        if (force)
            IB_LOG_INFINI_INFO0("Forcing complete MFT reprogramming");
    	}

	newMaxmcLid = sm_multicast_get_max_lid();
	if ((status = vs_unlock(&sm_McGroups_lock)) != VSTATUS_OK) {
		IB_LOG_ERRORRC("Failed to give sa lock rc:", status);
	}

	if (oldMaxmcLid > newMaxmcLid) {
		maxmcLid = oldMaxmcLid;
	} else {
		maxmcLid = newMaxmcLid;
		ASSERT(find_mlid_offset(maxmcLid) < sm_mcast_mlid_table_cap);
	}
	topo = (Topology_t *)&old_topology;
	for_all_switch_nodes(topo, switchp)
	{
		if (sm_state != SM_STATE_MASTER)
		{
			IB_LOG_INFO_FMT(__func__,
			       "Breaking out of MFT programming early since I am no longer the master SM");
			worstStatus = VSTATUS_NOT_MASTER;
			break;
		}

		/* PR 106923 - SM Tries to program MFT's for switches that belong to another
		 * SM during first topology sweep after 2 fabrics have been merged... We need to
		 * hold off doing so until one SM relinquished control
		 */
		if (!sm_valid_port((portp = sm_get_port(switchp,0))) || portp->state < IB_PORT_ACTIVE)
			continue;

        /* skip switches whose mft's did not change */
        if (topology_passcount > 1 && !switchp->mftChange && !switchp->mftPortMaskChange && !force) {
            continue;
        } 
        //else if (switchp->mftChange) {
        // 	IB_LOG_INFINI_INFO_FMT(__func__,
        //            "Updating Switch[%d] %s's MFT", switchp->index, sm_nodeDescString(switchp));
        //}

		if (topology_passcount > 1 && prev_tp)
			oldswp = sm_find_guid(prev_tp, switchp->nodeInfo.NodeGUID);

		memset(&mft, 0, sizeof(mft));
		status = VSTATUS_OK;
		mlid_count = find_mlid_offset(maxmcLid);
		for (mlid_offset = 0 ; mlid_offset <= mlid_count && status == VSTATUS_OK; mlid_offset+=STL_NUM_MFT_ELEMENTS_BLOCK) {
			lid = (maxmcLid-mlid_count+mlid_offset);
			for (i = 0; i < STL_NUM_MFT_POSITIONS_MASK && i * STL_PORT_MASK_WIDTH <= switchp->nodeInfo.NumPorts; ++i) {
				mftBlockChange = 0;
				for (j = 0; j < STL_NUM_MFT_ELEMENTS_BLOCK; j++) {
					if (mlid_offset+j < sm_mcast_mlid_table_cap)
						mft.MftBlock[j] = switchp->mft[mlid_offset + j][i];
					else
						mft.MftBlock[j] = 0;

					if (mftBlockChange)
						continue;

					if (j > (mlid_count - mlid_offset)) 
						break;

					if ((lid + j) > oldMaxmcLid) {
						/* new multicast lid */
						if (mft.MftBlock[j] != 0)
							mftBlockChange = 1;
					} else if (!force && !switchp->mftChange && oldswp) {
						if (mlid_offset+j < sm_mcast_mlid_table_cap)
							old_portMask = oldswp->mft[mlid_offset + j][i];
						else
							old_portMask = 0;
						if (mft.MftBlock[j] != old_portMask)
							mftBlockChange = 1;
					}
				}

				if (!force && !switchp->mftChange && oldswp && !mftBlockChange)
					continue;

				amod = (numBlocks << 24) | (i << 22) | (mlid_offset / STL_NUM_MFT_ELEMENTS_BLOCK);
                		status = SM_Set_MFT_DispatchLR(fd_topology, amod, sm_lid, portp->portData->lid,
					                               &mft, sm_config.mkey, switchp, &sm_asyncDispatch);
				if (status != VSTATUS_OK) {
					worstStatus = status;
					IB_LOG_ERROR_FMT(__func__, "can't set MFT on switch %s, amod 0x%.8X "
						   "with status 0x%.8X",
						   sm_nodeDescString(switchp), amod, status);
					break;
				}
				dispatched = 1;
			}
		}
	}
	
	if (dispatched) {
		status = sm_dispatch_wait(&sm_asyncDispatch);
		if (status != VSTATUS_OK) {
			sm_dispatch_clear(&sm_asyncDispatch);
			IB_LOG_ERRORRC("failed to service the dispatch queue rc:", status);
			return status;
		}
	}

	oldMaxmcLid = newMaxmcLid;
    if (smDebugPerf) {
        vs_time_get(&eTime);
        IB_LOG_INFINI_INFO("END SET MFT of switches, elapsed time(usecs)=", 
                           (int)(eTime-sTime));
    }
	return worstStatus;
}

// -------------------------------------------------------------------------- //

//
//
//
static __inline__
uint64_t lidClassKey(PKey_t pKey, uint8_t mtu, uint8_t rate)
{
	union {
		uint64_t key;
		struct {
			uint8_t mtu:6;
			uint8_t rate:6;
			uint16_t pKey;
		} s;
	} k;

	k.key = 0;

	k.s.pKey = PKEY_VALUE(pKey); // always ignore the membership bit in the pkey
	k.s.mtu = mtu;
	k.s.rate = rate;

	return k.key;
}

//
//
//
static __inline__
LidClass_t * getLidClass(McGroupClass_t * grpClass, PKey_t pKey, uint8_t mtu, uint8_t rate)
{
	cl_map_item_t * mi = NULL;
	LidClass_t * lidClass = NULL;

	mi = cl_qmap_get(&grpClass->usageMap, lidClassKey(pKey, mtu, rate));

	if (mi != cl_qmap_end(&grpClass->usageMap))
		lidClass = PARENT_STRUCT(mi, LidClass_t, mapItem);

	return lidClass;
}

//
//
//
static __inline__
LidClass_t * addLidClass(McGroupClass_t * grpClass, PKey_t pKey, uint8_t mtu, uint8_t rate)
{
	Status_t status;
	LidClass_t * lidClass = NULL;
	cl_map_item_t * mi = NULL;

	
	// We allocate the LidClass_t structure and the array of lid array with one allocation,
	// then set the 'lids' pointer in the lidClass to the proper memory address
	status = vs_pool_alloc(&sm_pool, sizeof(LidClass_t) +
	                                 (sizeof(LidUsage_t) * grpClass->maximumLids),
	                       (void*) &lidClass);

	if (status == VSTATUS_OK)
	{
		memset(lidClass, 0, sizeof(LidClass_t) + sizeof(LidUsage_t) * grpClass->maximumLids);

		lidClass->lidsUsed = 0;
		lidClass->pKey = pKey;
		lidClass->mtu = mtu;
		lidClass->rate = rate;
		lidClass->lids = (LidUsage_t *) (lidClass + 1);

		mi = cl_qmap_insert(&grpClass->usageMap, lidClassKey(pKey, mtu, rate),
		                    &lidClass->mapItem);

		// We initialize every byte in the table to 0xFF, becase 0xFFFF is an
		// invalid lid for our purposes, and 0xFFFF will also be an effective
		// infinite value for usageCount
		memset(lidClass->lids, 0xFF, sizeof(LidUsage_t) * grpClass->maximumLids);

		assert(mi == &lidClass->mapItem);
	} else
		lidClass = NULL;

	return lidClass;
}

//
// gids are in host byte order
//
static Status_t
sm_multicast_create_group_class(McGroupClass_t * class, IB_GID mask, IB_GID value,
                                uint16_t maxLids)
{
	Status_t status = VSTATUS_OK;

	IB_ENTER(__func__, class, &value, maxLids, 0);

	/* initialize our data type */
	memset(class, 0, sizeof(McGroupClass_t));
	memcpy(&(class->mask), &(mask), sizeof(IB_GID));
	memcpy(&(class->value), &(value), sizeof(IB_GID));
	class->maximumLids = maxLids;

	cl_qmap_init(&class->usageMap, NULL);

	IB_EXIT(__func__, status);
	return status;
}

//
// gid is in host byte order
//
Status_t
sm_multicast_add_group_class(IB_GID mask, IB_GID value, uint16_t maxLids)
{
	Status_t status = VSTATUS_OK;
	int i = 0;

	IB_ENTER(__func__, &mask, &value, maxLids, 0);

	/* Zero out the Class array before the first group creation */
	if (numMcGroupClasses == 0)
		memset(mcGroupClasses, 0,
		       MAX_SUPPORTED_MCAST_GRP_CLASSES * sizeof(McGroupClass_t));
	else if (numMcGroupClasses >= MAX_SUPPORTED_MCAST_GRP_CLASSES)
		status = VSTATUS_NOMEM;
	else
	{
		for (i = 0; i < numMcGroupClasses; ++i)
		{
			/* Check to see if we've already added a matching entry */
			if (  (memcmp(&(mcGroupClasses[i].mask), &mask, sizeof(IB_GID)) == 0)
			   && (memcmp(&(mcGroupClasses[i].value), &value, sizeof(IB_GID)) == 0) )
			{
				status = VSTATUS_KNOWN;
			}
		}
	}

	if (status == VSTATUS_OK)
		status = sm_multicast_create_group_class((mcGroupClasses + numMcGroupClasses), 
					mask, value, maxLids);

	if (status == VSTATUS_OK)
	{
		++numMcGroupClasses;

		IB_LOG_VERBOSE_FMT(__func__, "Added multicast group to match table with "
		       "Mask: " FMT_GID " Value: " FMT_GID " and limit of %d",
		       mask.Type.Global.SubnetPrefix, mask.Type.Global.InterfaceID, 
			   value.Type.Global.SubnetPrefix, value.Type.Global.InterfaceID, maxLids);
	}

	IB_EXIT(__func__, status);
	return status;
}

//
//
//
Status_t
sm_multicast_set_default_group_class(uint16_t maxLids)
{
	Status_t status = VSTATUS_OK;
	IB_ENTER(__func__, maxLids, 0, 0, 0);

	status = sm_multicast_create_group_class(&defaultMcGroupClass,
	                                         nullGid, nullGid, maxLids);
	if (status == VSTATUS_OK)
		IB_LOG_VERBOSE_FMT(__func__, "Default mcast table cap set to %d", maxLids);

	IB_EXIT(__func__, status);
	return status;
}

//
// Function matches a McGroupClass based on Gid
// Returns ptr to the Group Class, or null if there are no matches
//
// gid is in host byte order.
//
static McGroupClass_t *
find_mc_group_class(IB_GID gid)
{
	McGroupClass_t * retVal = NULL;
	int i = 0, j = 0;

	IB_ENTER(__func__, &gid, 0, 0, 0);

	for (i = 0; i < numMcGroupClasses; ++i)
	{
		/* assume the group matches */
		retVal = (mcGroupClasses + i);

		/* if (group.mask & gid != group.value), set retVal to NULL */
		for (j = 0; j < 16; ++j)
		{
			if ((mcGroupClasses[i].mask.Raw[j] & gid.Raw[j])
			       != mcGroupClasses[i].value.Raw[j])
			{
				retVal = NULL;
				break;
			}
		}

		/* break out if we found one */
		if (retVal != NULL)
			break;
	}

	if (retVal == NULL)
		retVal = &defaultMcGroupClass;

	IB_EXIT(__func__, retVal);
	return retVal;
}

//
//
//
static int
sm_multicast_is_lid_in_use(Lid_t lid) {
	int retVal = 0;
	McGroup_t	*mcGroup;

	IB_ENTER(__func__, lid, 0, 0, 0);

	/* Loop over all of the groups looking for this MC lid. */
	for_all_multicast_groups(mcGroup) {
		if (mcGroup->mLid == lid) {
			++retVal;
		}
	}

	IB_EXIT(__func__, retVal);
	return retVal;
}

//
//
//
static Status_t
sm_mc_find_unused_lid(Lid_t *lid) {
	Lid_t		i;

	IB_ENTER(__func__, lid, 0, 0, 0);

	// JSY - we need a topology variable which tells us the Min(switchp->MLIDTableCap), trust user config for now

	/* Find a lid we can use for this Multicast Group. */
	for (i = MULTICAST_LID_MIN; i < MULTICAST_LID_MIN+sm_mcast_mlid_table_cap; i++) {
		if (sm_multicast_is_lid_in_use(i) == 0) {
			*lid = i;
			IB_EXIT(__func__, i);
			return(VSTATUS_OK);
		}
	}

	IB_EXIT(__func__, VSTATUS_BAD);
	return(VSTATUS_BAD);
}

//
//
//
Lid_t
sm_multicast_get_max_lid() {
	Lid_t maxLid = 0;
	McGroup_t *localGroup;

	IB_ENTER(__func__, 0, 0, 0, 0);

	for_all_multicast_groups(localGroup) {
		if (localGroup->mLid > maxLid) {
			maxLid = localGroup->mLid;
		}
	}

	IB_EXIT(__func__, maxLid);
	return maxLid;
}

// -------------------------------------------------------------------------- //

/*
 * Basic macros for calulating indexes of family members in a heap
 */
#define PARENT_NODE(x) ((x - 1) >> 1)
/* Left child is at index (2n + 1) relative to parent */
#define CHILD_LEFT(x) ((x << 1) + 1)
/* Right child is at index (2n + 2) relative to parent */
#define CHILD_RIGHT(x) ((x + 1) << 1)

#define TEST_HEAP 1

#if defined(TEST_HEAP)
static void
checkHeap(LidClass_t * lidClass)
{   
    int i = 0;
    
    for (i = 1; i < lidClass->lidsUsed; ++i)
    {   
        int parent = PARENT_NODE(i);
        
        assert(lidClass->lids[i].usageCount >= lidClass->lids[parent].usageCount);
    }

	IB_EXIT(__func__, 0);
}

#endif // TEST_HEAP

static void
heapTrickleDown(LidClass_t * lidClass, int index)
{
    int i = 0, child = 0;
    LidUsage_t temp;
	
    memcpy(&temp, lidClass->lids + index, sizeof(LidUsage_t));

    for (i = index; (child = CHILD_LEFT(i)) < lidClass->lidsUsed; i = child)
    {
        if (  ((child + 1) < lidClass->lidsUsed)
           && (lidClass->lids[child].usageCount > lidClass->lids[child + 1].usageCount))
            ++child;

        if (temp.usageCount <= lidClass->lids[child].usageCount)
            break;

        memcpy(lidClass->lids + i, lidClass->lids + child, sizeof(LidUsage_t));
    }

    memcpy(lidClass->lids + i, &temp, sizeof(LidUsage_t));

#if defined(TEST_HEAP)
    checkHeap(lidClass);
#endif
	IB_EXIT(__func__, 0);
}

static void
heapTrickleUp(LidClass_t * lidClass, int index)
{
    int i = 0, parent = 0;
    LidUsage_t temp;

    for (i = index, parent = PARENT_NODE(i); i > 0;
         i = parent, parent = PARENT_NODE(i))
    {
		assert(parent >= 0);

        if (lidClass->lids[i].usageCount < lidClass->lids[parent].usageCount)
        {
            memcpy(&temp, lidClass->lids + parent, sizeof(LidUsage_t));
            memcpy(lidClass->lids + parent, lidClass->lids + i, sizeof(LidUsage_t));
            memcpy(lidClass->lids + i, &temp, sizeof(LidUsage_t));
        } else
            break;
    }

#if defined(TEST_HEAP)
    checkHeap(lidClass);
#endif
	IB_EXIT(__func__, 0);
}


static Status_t
sm_mc_get_group_class_lid(McGroupClass_t * groupClass, PKey_t pKey, uint8_t mtu, uint8_t rate,
                          Lid_t requestedLid, Lid_t * lid)
{
	Status_t status = VSTATUS_OK;
	Lid_t newLid = 0;
	int index = 0;
	LidClass_t * lidClass = getLidClass(groupClass, pKey, mtu, rate);

	if (groupClass->currentLids < groupClass->maximumLids)
	{
		if (lidClass == NULL)
		{
			// If lidClass doesn't yet exist, create it
			if ((lidClass = addLidClass(groupClass, pKey, mtu, rate)) == NULL)
				status = VSTATUS_NOMEM;
		}

		if (status == VSTATUS_OK)
		{
			if (requestedLid == 0)
			{
				// Simple case... assign a new MLid and put it at the end of
				// the heap, then trickle it up towards the front.
				if ((status = sm_mc_find_unused_lid(&newLid)) == VSTATUS_OK)
				{
					assert(lidClass->lids[lidClass->lidsUsed].lid == 0xFFFFFFFF);

					index = lidClass->lidsUsed;
					++lidClass->lidsUsed;

					lidClass->lids[index].lid = newLid;
					lidClass->lids[index].usageCount = 1;

					heapTrickleUp(lidClass, index);

					++groupClass->currentLids;
					*lid = newLid;
				}
			} else
			{
				// More complicated... cycle through the heap looking for the lid
				for (index = 0; index < lidClass->lidsUsed; ++index)
				{
					if (lidClass->lids[index].lid == requestedLid)
						break;
				}

				if (index == lidClass->lidsUsed)
				{
					assert(lidClass->lids[lidClass->lidsUsed].lid == 0xFFFFFFFF);

					// didn't find the LID, add to end of heap & trickle it up
					lidClass->lids[index].lid = requestedLid;
					lidClass->lids[index].usageCount = 1;

					heapTrickleUp(lidClass, index);

					++lidClass->lidsUsed;
					++groupClass->currentLids;
					*lid = requestedLid;
				} else
				{
					assert(lidClass->lids[index].lid == requestedLid);

					// found the lid... Increment usage count & trickle it down
					++lidClass->lids[index].usageCount;
					heapTrickleDown(lidClass, index);
					*lid = requestedLid;
				}
			}
		}
	} else
	{
		if (lidClass == NULL)
		{
			// error - we don't have anymore lids that can be assigned out of
			// our pool... Can we do anything better than throw an error?
			status = VSTATUS_NORESOURCE;
		} else if (requestedLid == 0)
		{
			// This MC group class has run out of lids, pick the one from the top of
			// the heap (the one with the smallest usage count) and use it, trickling
			// it down the heap after adjusting usage count
			assert(lidClass->lids[0].lid != 0xFFFFFFFF);

			*lid = newLid = lidClass->lids[0].lid;
			++lidClass->lids[0].usageCount;
			heapTrickleDown(lidClass, 0);

		} else
		{
			// This branch deals with the replication of the lid tables during a standby
			// sm's db sync when all of the lids for a given group class are already in
			// use... Search through the lidClass for the specified lid, if found increment
			// the usageCount and trickle the entry down the heap
			for (index = 0; index < lidClass->lidsUsed; ++index)
			{
				if (lidClass->lids[index].lid == requestedLid)
					break;
			}

			if (index < lidClass->lidsUsed)
			{
				// we found it
				++lidClass->lids[index].usageCount;
				heapTrickleDown(lidClass, index);
				*lid = requestedLid;
			} else
			{
				// We're out of lids and can't honor the request... This should *only*
				// happen in situations where standby & master SM configurations differ,
				// and should only occur on the standby SM.
				status = VSTATUS_NORESOURCE;
			}
			
		}
	}

	IB_EXIT(__func__, status);
	return status;
}

Status_t
sm_multicast_decommision_group(McGroup_t * group)
{
	McGroupClass_t * groupClass = NULL;
	LidClass_t * lidClass = NULL;
	Status_t status = VSTATUS_OK;
	int i = 0;

	// Find the group class
	groupClass = find_mc_group_class(group->mGid);

#if defined(TEST_HEAP)
	assert(groupClass != NULL);
#endif

	if (groupClass->maximumLids != 0)
	{

		lidClass = getLidClass(groupClass, group->pKey, group->mtu, group->rate);
#if defined(TEST_HEAP)
		assert(lidClass != NULL);
#endif

        for (i = 0; i < lidClass->lidsUsed; ++i) {
            if (lidClass->lids[i].lid == group->mLid) {
#if (TEST_HEAP)
				assert(lidClass->lids[i].usageCount > 0);
#endif
				--lidClass->lids[i].usageCount;

				if (lidClass->lids[i].usageCount == 0)
				{
					if (i != (lidClass->lidsUsed - 1)) 
					{
						// Swap the last entry in the heap with this entry... We just do a memset
						// on the last entry instead of a real swap.
						memcpy(&lidClass->lids[i], &lidClass->lids[lidClass->lidsUsed - 1],
						       sizeof(LidUsage_t));
						memset(&lidClass->lids[lidClass->lidsUsed - 1], 0xFF, sizeof(LidUsage_t));

						// lidClass->lids[i] now contains an entry that needs to be moved
						// somewhere in the heap... If its not the root element of the heap, and if
						// its usage count is less than it's parent, we trickle up, otherwise we
						// trickle down
						if (  i != 0
						   && lidClass->lids[i].usageCount < lidClass->lids[PARENT_NODE(i)].usageCount)
							heapTrickleUp(lidClass, i);
						else
							heapTrickleDown(lidClass, i);
					} else
					{
						// If this element was the last one in the heap, just set it to 0xFF... No
						// need to do anything else
						memset(&lidClass->lids[lidClass->lidsUsed - 1], 0xFF, sizeof(LidUsage_t));
					}
						
					// This mlid no longer has any MC groups - decrement the
					// lid count for the entire group class & mark the mlid
					// entry as unused
					--lidClass->lidsUsed;
					--groupClass->currentLids;

				} else
				{
					heapTrickleUp(lidClass, i);
				}

				break;
			}
		}
	}

	IB_EXIT(__func__, status);
	return status;
}

//
// This function checks to make sure that an mLid hasn't been assigned to a
// different MC group class than the DB sync wants
//
Status_t
sm_multicast_check_sync_consistancy(McGroupClass_t * groupClass, PKey_t pKey,
                                    uint8_t mtu, uint8_t rate, Lid_t lid)
{
	Status_t status = VSTATUS_OK;
	int i, j;
	cl_map_item_t * mi = NULL;
	LidClass_t * lidClass = NULL;

	// Check to make sure that the lid is not being used by a different one
	// than returned by find_mc_group_class()
	for (i = 0; i < numMcGroupClasses; ++i) {
		for (mi = cl_qmap_head(&mcGroupClasses[i].usageMap);
		     mi != cl_qmap_end(&mcGroupClasses[i].usageMap);
		     mi = cl_qmap_next(mi))
		{
			lidClass = PARENT_STRUCT(mi, LidClass_t, mapItem);

			// We ignore the grpClass/pkey/mtu/rate lidClass that we think
			// the lid should be a part of
			if (  (groupClass == &mcGroupClasses[i]) && (lidClass->pKey == pKey)
			   && (lidClass->mtu == mtu) && (lidClass->rate == rate))
			{
				continue;
			}

			for (j = 0; j < lidClass->lidsUsed; ++j) {
				if (lidClass->lids[j].lid == lid) {
					status = VSTATUS_REJECT;
					break;
				}
			}

			if (status != VSTATUS_OK) break;
		}

		if (status != VSTATUS_OK) break;
	}

	// similar to the statement above, we make sure that the lid isn't already part of the
	// default group class
	for (mi = cl_qmap_head(&defaultMcGroupClass.usageMap);
	     mi != cl_qmap_end(&defaultMcGroupClass.usageMap);
	     mi = cl_qmap_next(mi))
	{
		lidClass = PARENT_STRUCT(mi, LidClass_t, mapItem);

		// We ignore the grpClass/pkey/mtu/rate lidClass that we think
		// the lid should be a part of
		if ((lidClass->pKey == pKey) && (lidClass->mtu == mtu) && (lidClass->rate == rate))
			continue;

		for (j = 0; j < lidClass->lidsUsed; ++j) {
			if (lidClass->lids[j].lid == lid) {
				status = VSTATUS_REJECT;
				break;
			}
		}

		if (status != VSTATUS_OK) break;
	}

	IB_EXIT(__func__, status);
	return status;
}

Status_t
sm_multicast_sync_lid(IB_GID mGid, PKey_t pKey, uint8_t mtu, uint8_t rate, Lid_t lid)
{
	Status_t status = VSTATUS_OK;
	McGroupClass_t * groupClass = NULL;
	Lid_t newLid = 0;

	IB_ENTER(__func__, lid, mtu, rate, 0);

	groupClass = find_mc_group_class(mGid);

#if defined(TEST_HEAP)
	assert(groupClass != NULL);
#endif

	status = sm_multicast_check_sync_consistancy(groupClass, pKey, mtu, rate, lid);

	if (status == VSTATUS_OK)
	{
		if (groupClass->maximumLids == 0)
		{
			/* value was explicitly set to zero - just increment the lid count */
			++groupClass->currentLids;
		} else
		{
			status = sm_mc_get_group_class_lid(groupClass, pKey, mtu, rate, lid, &newLid);

			if (status != VSTATUS_OK)
				IB_LOG_ERROR_FMT(__func__, "Could not assign multicast lid %04X to group "
				       FMT_GID, lid, mGid.Type.Global.SubnetPrefix, mGid.Type.Global.InterfaceID);
		}
	} else
	{
		IB_LOG_ERROR_FMT(__func__, 
		       "%s Consistency check for sync of group " FMT_GID
		       " failed. (lid = 0x%04X, mtu = %d, rate = %d, pkey = %04X)",
		       __func__, mGid.Type.Global.SubnetPrefix, mGid.Type.Global.InterfaceID, lid, mtu, rate, pKey);
		       
	}

	IB_EXIT(__func__, status);
	return status;
}

/*
 * This function figures out what LID to assign to a multicast
 * group, based on the the group's GID. It does this
 * by figuring out what 'class' this group is in... Based on
 * the group class and the number of groups already in that
 * class, and the configured maximum number of lids reserved for
 * that class, it will either assign a new lid to the group
 * or use one of the lids that are already in use by the groups
 * in that class.
 *
 * mGid is in host byte order.
 */
Status_t
sm_multicast_assign_lid(IB_GID mGid, PKey_t pKey, uint8_t mtu, uint8_t rate,
                        Lid_t * lid)
{
	Status_t status = VSTATUS_OK;
	McGroupClass_t * groupClass = NULL;

	groupClass = find_mc_group_class(mGid);

#if defined(TEST_HEAP)
	assert(groupClass != NULL);
#endif

	if (groupClass->maximumLids == 0)
	{
		/* Non-shared MLIDs for this MGID - just generate a new mcast
		 * lid and return */
		status = sm_mc_find_unused_lid(lid);
		if (status == VSTATUS_OK)
			++groupClass->currentLids;
	} else
	{
		IB_LOG_INFO_FMT(__func__, 
		       "sm_multicast_assign_lid: got request for new group that matches "
		       "group class with mask: " FMT_GID " and value: " FMT_GID,
		       groupClass->mask.Type.Global.SubnetPrefix, groupClass->mask.Type.Global.InterfaceID,
		       groupClass->value.Type.Global.SubnetPrefix, groupClass->value.Type.Global.InterfaceID);

		status = sm_mc_get_group_class_lid(groupClass, pKey, mtu, rate, 0, lid);
	}


	//IB_LOG_INFINI_INFO_FMT(__func__, "sm_multicast_assign_lid - assigned lid %08X", *lid);

	IB_EXIT(__func__, status);
	return status;
}

// -------------------------------------------------------------------------- //

// gid is in host byte order
McGroup_t *
sm_find_multicast_gid(IB_GID gid) {
	McGroup_t	*mcGroup=NULL;

	IB_ENTER(__func__, &gid, 0, 0, 0);

//
//	Loop over all MC groups looking for this MC gid.
//
	for_all_multicast_groups(mcGroup) {
		if (memcmp(&(mcGroup->mGid), &gid, 16) == 0) {
			break;
		}
	}

	IB_EXIT(__func__, mcGroup);
	return(mcGroup);
}

// gid is in host byte order
McGroup_t *
sm_find_next_multicast_gid(IB_GID gid) {
	McGroup_t	*dGroup;
	McGroup_t	*pGroupBest = NULL;
	int			i,j;
		
	for_all_multicast_groups(dGroup) {
		for(i=0;i<16;i++){
			if(dGroup->mGid.Raw[i] < gid.Raw[i]){
				break;
			}else if(dGroup->mGid.Raw[i] > gid.Raw[i]){
				if(!pGroupBest){
					pGroupBest = dGroup;
				}else{
					for(j=0;j<16;j++){
						if(dGroup->mGid.Raw[j] < pGroupBest->mGid.Raw[j]){
							pGroupBest = dGroup;
							break;
						}else if(dGroup->mGid.Raw[j] > pGroupBest->mGid.Raw[j]){
							break;
						}
					}
				}
				break;
			}
		}
	}

	return pGroupBest;

}

// gid is in host byte order
Status_t
sm_multicast_gid_assign(uint32_t scope, IB_GID gid) {
	uint32_t	i;

	IB_ENTER(__func__, gid.Raw, 0, 0, 0);

//
//	If the scope is zero, we use the site-local scope value.
//
	if (scope == 0) {
		scope = IB_SITE_LOCAL_SCOPE;
	}

//
//	Find a gid we can use for this Multicast Group.
//
	gid.AsReg64s.H = 0ull;
	gid.AsReg64s.L = 0ull;
	gid.Type.Multicast.s.FormatPrefix = 0xff;
	gid.Type.Multicast.s.Flags = 0x1;
	gid.Type.Multicast.s.Scope = scope;
	gid.Type.Multicast.GroupId[13] = 0xa0;
	gid.Type.Multicast.GroupId[12] = 0x1b;

	*(uint64_t*)(&gid.Type.Multicast.GroupId[4]) = sm_config.subnet_prefix;
	i = 0;
	do
	{
		memcpy(gid.Type.Multicast.GroupId, &i, sizeof(i));
		if (sm_find_multicast_gid(gid) == NULL) {
			IB_EXIT(__func__, VSTATUS_OK);
			return(VSTATUS_OK);
		}
	} while (i++ < 0xFFFFFFFF);

	IB_EXIT(__func__, VSTATUS_BAD);
	return(VSTATUS_BAD);
}

// gid is in host byte order
Status_t
sm_multicast_gid_check(IB_GID gid) {

	IB_ENTER(__func__, &gid, 0, 0, 0);

//
//	Check to be sure that this gid is not being used already.
//JSY - we need to bound this check by the ranges that switches can support
//
	if (sm_find_multicast_gid(gid) != NULL) {
		IB_EXIT(__func__, VSTATUS_BAD);
		return(VSTATUS_BAD);
	}

	IB_EXIT(__func__, VSTATUS_OK);
	return(VSTATUS_OK);
}

// gid is in host byte order
Status_t
sm_multicast_gid_valid(uint8_t scope, IB_GID gid) {
	uint8 temp;

	IB_ENTER(__func__, scope, gid.Raw, 0, 0);

//
//	Check to see if this has the multicast scope bits at the front.
//
	if (gid.Type.Multicast.s.FormatPrefix != 0xff) {	// Not multicast
		IB_EXIT(__func__, VSTATUS_BAD);
		return(VSTATUS_BAD);
	}

//
//	The flags can only be '0' or '1'.
//
	if ((gid.Type.Multicast.s.Flags) >= 0x2) {		// Bad flags
		IB_EXIT(__func__, VSTATUS_BAD);
		return(VSTATUS_BAD);
	}

	temp = gid.Type.Multicast.s.Scope;
    /* check the scope bits in the gid */
	if ((temp != IB_LINK_LOCAL_SCOPE) &&
        (temp != IB_SITE_LOCAL_SCOPE) && 
        (temp != IB_ORG_LOCAL_SCOPE) && 
        (temp != IB_GLOBAL_SCOPE)) {			// Bad scope
		IB_EXIT(__func__, VSTATUS_BAD);
		return(VSTATUS_BAD);
	}

//
//	If SA-specific signature and link-local scope bits, then bad.
//
	if ((gid.Type.Multicast.GroupId[13] == 0xa0) && 
		(gid.Type.Multicast.GroupId[12] == 0x1b)) {	// SA-specific
		if (temp == 0x2) {		// Link local
			IB_EXIT(__func__, VSTATUS_BAD);
			return(VSTATUS_BAD);
		}
	}

	IB_EXIT(__func__, VSTATUS_OK);
	return(VSTATUS_OK);
}

// -------------------------------------------------------------------------- //

// gid is in host byte order
McMember_t *
sm_find_multicast_member(McGroup_t *mcGroup, IB_GID gid) {
	McMember_t		*mcMember=NULL;

	IB_ENTER(__func__, mcGroup, &gid, 0, 0);

//
//	Loop over all of the members looking for this gid.
//
	for_all_multicast_members(mcGroup, mcMember) {
		if (memcmp((void *)(&gid), (void *)mcMember->record.RID.PortGID.Raw, 16) == 0) {
			break;
		}
	}

	IB_EXIT(__func__, mcMember);
	return(mcMember);
}


McMember_t *
sm_find_multicast_member_by_index(McGroup_t *mcGroup, uint32_t index) {
	McMember_t		*mcMember=NULL;

	IB_ENTER(__func__, mcGroup, index, 0, 0);

//
//	Loop over all of the members looking for this index
//
	for_all_multicast_members(mcGroup, mcMember) {
		if (mcMember->index == index) {
			break;
		}
	}

	IB_EXIT(__func__, mcMember);
	return(mcMember);
}

McMember_t *
sm_find_next_multicast_member_by_index(McGroup_t *mcGroup, uint32_t index) {
	McMember_t		*mcMember=NULL;
	McMember_t		*mcMemberBest=NULL;

	IB_ENTER(__func__, mcGroup, index, 0, 0);

//
//	Loop over all of the members looking for this index
//
	for_all_multicast_members(mcGroup, mcMember) {
		if (mcMember->index > index) {
			if(!mcMemberBest){
				mcMemberBest = mcMember;
			}else if(mcMember->index < mcMemberBest->index){
				mcMemberBest = mcMember;
			}
		}
	}

	IB_EXIT(__func__, mcMember);
	return(mcMemberBest);
}
