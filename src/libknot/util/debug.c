/*  Copyright (C) 2011 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <stdlib.h>

#include "util/utils.h"
#include "util/debug.h"
#include "libknot/rrset.h"
#include "common/descriptor_new.h"
#include "common/print.h"

void knot_rdata_dump(const knot_rrset_t *rrset, size_t rdata_pos)
{
#if defined(KNOT_ZONE_DEBUG) || defined(KNOT_RDATA_DEBUG)
#endif
}

void knot_rrset_dump(const knot_rrset_t *rrset)
{
#if defined(KNOT_ZONE_DEBUG) || defined(KNOT_RRSET_DEBUG)
	fprintf(stderr, "  ------- RRSET -------\n");
	fprintf(stderr, "  %p\n", rrset);
	if (!rrset) {
		return;
	}
        char *name = knot_dname_to_str(rrset->owner);
        fprintf(stderr, "  owner: %s\n", name);
        free(name);
	fprintf(stderr, "  type: %u\n", rrset->type);
	fprintf(stderr, "  class: %d\n", rrset->rclass);
	fprintf(stderr, "  ttl: %d\n", rrset->ttl);

        fprintf(stderr, "  RRSIGs:\n");
        if (rrset->rrsigs != NULL) {
                knot_rrset_dump(rrset->rrsigs);
        } else {
                fprintf(stderr, "  none\n");
        }

	if (rrset->rdata == NULL) {
		fprintf(stderr, "  NO RDATA!\n");
		fprintf(stderr, "  ------- RRSET -------\n");
		return;
	}
	
	// TODO dump rdata

	fprintf(stderr, "  ------- RRSET -------\n");
#endif
}

void knot_node_dump(knot_node_t *node)
{
#if defined(KNOT_ZONE_DEBUG) || defined(KNOT_NODE_DEBUG)
	//char loaded_zone = *((char*) data);
	char *name;

	fprintf(stderr, "------- NODE --------\n");
	name = knot_dname_to_str(node->owner);
	fprintf(stderr, "owner: %s\n", name);
	free(name);
	fprintf(stderr, "labels: ");
	hex_print((char *)node->owner->labels, node->owner->label_count);
	fprintf(stderr, "node: %p\n", node);
	fprintf(stderr, "node (in node's owner): %p\n", node->owner->node);

	if (knot_node_is_deleg_point(node)) {
		fprintf(stderr, "delegation point\n");
	}

	if (knot_node_is_non_auth(node)) {
		fprintf(stderr, "non-authoritative node\n");
	}

	if (node->parent != NULL) {
		/*! \todo This causes segfault when parent was free'd,
		 *        e.g. when applying changesets.
		 */
		name = knot_dname_to_str(node->parent->owner);
		fprintf(stderr, "parent: %s\n", name);
		free(name);
	} else {
		fprintf(stderr, "no parent\n");
	}

	if (node->prev != NULL) {
		fprintf(stderr, "previous node: %p\n", node->prev);
		/*! \todo This causes segfault when prev was free'd,
		 *        e.g. when applying changesets.
		 */
		name = knot_dname_to_str(node->prev->owner);
		fprintf(stderr, "previous node: %s\n", name);
		free(name);
	} else {
		fprintf(stderr, "previous node: none\n");
	}

	knot_rrset_t **rrsets = knot_node_get_rrsets(node);

	fprintf(stderr, "Wildcard child: ");

	if (node->wildcard_child != NULL) {
		/*! \todo This causes segfault when wildcard child was free'd,
		 *        e.g. when applying changesets.
		 */
		name = knot_dname_to_str(node->wildcard_child->owner);
		fprintf(stderr, "%s\n", name);
		free(name);
	} else {
		fprintf(stderr, "none\n");
	}

	fprintf(stderr, "NSEC3 node: ");

	if (node->nsec3_node != NULL) {
		/*! \todo This causes segfault when nsec3_node was free'd,
		 *        e.g. when applying changesets.
		 */
		name = knot_dname_to_str(node->nsec3_node->owner);
		fprintf(stderr, "%s\n", name);
		free(name);
	} else {
		fprintf(stderr, "none\n");
	}
	
	fprintf(stderr, "Zone: %p\n", node->zone);

	fprintf(stderr, "RRSet count: %d\n", node->rrset_count);

	for (int i = 0; i < node->rrset_count; i++) {
		knot_rrset_dump(rrsets[i]);
	}
	free(rrsets);
	//assert(node->owner->node == node);
	fprintf(stderr, "------- NODE --------\n");
#endif
}

void knot_zone_contents_dump(knot_zone_contents_t *zone)
{
#if defined(KNOT_ZONE_DEBUG)
	if (!zone) {
		fprintf(stderr, "------- STUB ZONE --------\n");
		return;
	}

	fprintf(stderr, "------- ZONE --------\n");

	knot_zone_contents_tree_apply_inorder(zone, knot_node_dump, NULL);

	fprintf(stderr, "------- ZONE --------\n");
	
	fprintf(stderr, "------- NSEC 3 tree -\n");

	knot_zone_contents_nsec3_apply_inorder(zone, knot_node_dump, NULL);

	fprintf(stderr, "------- NSEC 3 tree -\n");
#endif
}
