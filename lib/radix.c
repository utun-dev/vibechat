/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1988, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Routines to build and maintain radix trees for routing lookups.
 */
#include <string.h>
#include <assert.h>
#include "mem.h"
#include "debug_config.h"
#include "radix.h"

#undef min
#define min(a, b) (((a) < (b)) ? (a) : (b))

static struct radix_node
	 *rn_insert(void *, struct radix_head *, int *,
	     struct radix_node [2]),
	 *rn_newpair(void *, int, struct radix_node[2]),
	 *rn_search(const void *, struct radix_node *),
	 *rn_search_m(const void *, struct radix_node *, void *);
static struct radix_node *rn_addmask(const void *, struct radix_mask_head *, int,int);

static void rn_detachhead_internal(struct radix_head *);

#define	RADIX_MAX_KEY_LEN	32

static uint8_t rn_zeros[RADIX_MAX_KEY_LEN];
static uint8_t rn_ones[RADIX_MAX_KEY_LEN] = {
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

static int	rn_lexobetter(const void *m_arg, const void *n_arg);
static struct radix_mask *
		rn_new_radix_mask(struct radix_node *tt,
		    struct radix_mask *next);
static int	rn_satisfies_leaf(const uint8_t *trial, struct radix_node *leaf,
		    int skip);

/*
 * The data structure for the keys is a radix tree with one way
 * branching removed.  The index rn_bit at an internal node n represents a bit
 * position to be tested.  The tree is arranged so that all descendants
 * of a node n have keys whose bits all agree up to position rn_bit - 1.
 * (We say the index of n is rn_bit.)
 *
 * There is at least one descendant which has a one bit at position rn_bit,
 * and at least one with a zero there.
 *
 * A route is determined by a pair of key and mask.  We require that the
 * bit-wise logical and of the key and mask to be the key.
 * We define the index of a route to associated with the mask to be
 * the first bit number in the mask where 0 occurs (with bit number 0
 * representing the highest order bit).
 *
 * We say a mask is normal if every bit is 0, past the index of the mask.
 * If a node n has a descendant (k, m) with index(m) == index(n) == rn_bit,
 * and m is a normal mask, then the route applies to every descendant of n.
 * If the index(m) < rn_bit, this implies the trailing last few bits of k
 * before bit b are all 0, (and hence consequently true of every descendant
 * of n), so the route applies to all descendants of the node as well.
 *
 * Similar logic shows that a non-normal mask m such that
 * index(m) <= index(n) could potentially apply to many children of n.
 * Thus, for each non-host route, we attach its mask to a list at an internal
 * node as high in the tree as we can go.
 *
 * The present version of the code makes use of normal routes in short-
 * circuiting an explict mask and compare operation when testing whether
 * a key satisfies a normal route, and also in remembering the unique leaf
 * that governs a subtree.
 */

/*
 * Most of the functions in this code assume that the key/mask arguments
 * are sockaddr-like structures, where the first byte is a uint8_t
 * indicating the size of the entire structure.
 *
 * To make the assumption more explicit, we use the LEN() macro to access
 * this field. It is safe to pass an expression with side effects
 * to LEN() as the argument is evaluated only once.
 */
#define LEN(x) ( (int) (*(const uint8_t *)(x)) )

static struct radix_node *
rn_search(const void *v_arg, struct radix_node *head)
{
	struct radix_node *x;
	const uint8_t *v;

	for (x = head, v = v_arg; x->rn_bit >= 0;) {
		if (x->rn_bmask & v[x->rn_offset])
			x = x->rn_right;
		else
			x = x->rn_left;
	}
	return (x);
}

static struct radix_node *
rn_search_m(const void *v_arg, struct radix_node *head, void *m_arg)
{
	struct radix_node *x;
	const uint8_t *v = v_arg, *m = m_arg;

	for (x = head; x->rn_bit >= 0;) {
		if ((x->rn_bmask & m[x->rn_offset]) &&
		    (x->rn_bmask & v[x->rn_offset]))
			x = x->rn_right;
		else
			x = x->rn_left;
	}
	return (x);
}

int
rn_refines(const void *m_arg, const void *n_arg)
{
	const uint8_t *m = m_arg, *n = n_arg;
	const uint8_t *lim, *lim2 = lim = n + LEN(n);
	int longer = LEN(n++) - LEN(m++);
	int masks_are_equal = 1;

	if (longer > 0)
		lim -= longer;
	while (n < lim) {
		if (*n & ~(*m))
			return (0);
		if (*n++ != *m++)
			masks_are_equal = 0;
	}
	while (n < lim2)
		if (*n++)
			return (0);
	if (masks_are_equal && (longer < 0))
		for (lim2 = m - longer; m < lim2; )
			if (*m++)
				return (1);
	return (!masks_are_equal);
}

struct radix_node *
rn_lookup(const void *v_arg, const void *m_arg, struct radix_head *head)
{
	struct radix_node *x;
	uint8_t *netmask;

	if (m_arg != NULL) {
		x = rn_addmask(m_arg, head->rnh_masks, 1,
		    head->rnh_treetop->rn_offset);
		if (x == NULL)
			return (NULL);
		netmask = x->rn_key;

		x = rn_match(v_arg, head);

		while (x != NULL && x->rn_mask != netmask)
			x = x->rn_dupedkey;

		return (x);
	}

	if ((x = rn_match(v_arg, head)) == NULL)
		return (NULL);

	if (LEN(x->rn_key) != LEN(v_arg) || memcmp(x->rn_key, v_arg, LEN(v_arg)))
		return (NULL);

	if (x->rn_mask != NULL)
		return (NULL);

	return (x);
}

static int
rn_satisfies_leaf(const uint8_t *trial, struct radix_node *leaf, int skip)
{
	const uint8_t *cp = trial, *cp2 = leaf->rn_key, *cp3 = leaf->rn_mask;
	const uint8_t *cplim;
	int length = min(LEN(cp), LEN(cp2));

	if (cp3 == NULL)
		cp3 = rn_ones;
	else
		length = min(length, LEN(cp3));
	cplim = cp + length; cp3 += skip; cp2 += skip;
	for (cp += skip; cp < cplim; cp++, cp2++, cp3++)
		if ((*cp ^ *cp2) & *cp3)
			return (0);
	return (1);
}

struct radix_node *
rn_match(const void *v_arg, struct radix_head *head)
{
	const uint8_t *v = v_arg;
	struct radix_node *t = head->rnh_treetop, *x;
	const uint8_t *cp = v, *cp2;
	const uint8_t *cplim;
	struct radix_node *saved_t, *top = t;
	int off = t->rn_offset, vlen = LEN(cp), matched_off;
	int test, b, rn_bit;

	for (; t->rn_bit >= 0; ) {
		if (t->rn_bmask & cp[t->rn_offset])
			t = t->rn_right;
		else
			t = t->rn_left;
	}
	if (t->rn_mask)
		vlen = *t->rn_mask;
	cp += off; cp2 = t->rn_key + off; cplim = v + vlen;
	for (; cp < cplim; cp++, cp2++)
		if (*cp != *cp2)
			goto on1;
	if (t->rn_flags & RNF_ROOT)
		t = t->rn_dupedkey;
	return (t);
on1:
	test = (*cp ^ *cp2) & 0xff;
	for (b = 7; (test >>= 1) > 0;)
		b--;
	matched_off = cp - v;
	b += matched_off << 3;
	rn_bit = -1 - b;
	if ((saved_t = t)->rn_mask == 0)
		t = t->rn_dupedkey;
	for (; t; t = t->rn_dupedkey)
		if (t->rn_flags & RNF_NORMAL) {
			if (rn_bit <= t->rn_bit)
				return (t);
		} else if (rn_satisfies_leaf(v, t, matched_off))
				return (t);
	t = saved_t;
	do {
		struct radix_mask *m;
		t = t->rn_parent;
		m = t->rn_mklist;
		while (m) {
			if (m->rm_flags & RNF_NORMAL) {
				if (rn_bit <= m->rm_bit)
					return (m->rm_leaf);
			} else {
				off = min(t->rn_offset, matched_off);
				x = rn_search_m(v, t, m->rm_mask);
				while (x && x->rn_mask != m->rm_mask)
					x = x->rn_dupedkey;
				if (x && rn_satisfies_leaf(v, x, off))
					return (x);
			}
			m = m->rm_mklist;
		}
	} while (t != top);
	return (0);
}

struct radix_node *
rn_nextprefix(struct radix_node *rn)
{
	for (rn = rn->rn_dupedkey; rn != NULL; rn = rn->rn_dupedkey) {
		if (!(rn->rn_flags & RNF_ROOT))
			return (rn);
	}
	return (NULL);
}

static struct radix_node *
rn_newpair(void *v, int b, struct radix_node nodes[2])
{
	struct radix_node *tt = nodes, *t = tt + 1;
	t->rn_bit = b;
	t->rn_bmask = 0x80 >> (b & 7);
	t->rn_left = tt;
	t->rn_offset = b >> 3;
	t->rn_parent = t->rn_right = NULL;

	tt->rn_bit = -1;
	tt->rn_mask = NULL;
	tt->rn_dupedkey = NULL;
	tt->rn_bmask = 0;
	tt->rn_key = (uint8_t *)v;
	tt->rn_parent = t;
	tt->rn_flags = t->rn_flags = RNF_ACTIVE;
	tt->rn_mklist = t->rn_mklist = 0;
	return (t);
}

static struct radix_node *
rn_insert(void *v_arg, struct radix_head *head, int *dupentry,
    struct radix_node nodes[2])
{
	uint8_t *v = v_arg;
	struct radix_node *top = head->rnh_treetop;
	int head_off = top->rn_offset, vlen = LEN(v);
	struct radix_node *t = rn_search(v_arg, top);
	uint8_t *cp = v + head_off;
	unsigned b;
	struct radix_node *p, *tt, *x;
	uint8_t *cp2 = t->rn_key + head_off;
	int cmp_res;
	uint8_t *cplim = v + vlen;

	while (cp < cplim)
		if (*cp2++ != *cp++)
			goto on1;
	*dupentry = 1;
	return (t);
on1:
	*dupentry = 0;
	cmp_res = (cp[-1] ^ cp2[-1]) & 0xff;
	for (b = (cp - v) << 3; cmp_res; b--)
		cmp_res >>= 1;

	x = top;
	cp = v;
	do {
		p = x;
		if (cp[x->rn_offset] & x->rn_bmask)
			x = x->rn_right;
		else
			x = x->rn_left;
	} while (b > (unsigned) x->rn_bit);

	t = rn_newpair(v_arg, b, nodes);
	tt = t->rn_left;
	if ((cp[p->rn_offset] & p->rn_bmask) == 0)
		p->rn_left = t;
	else
		p->rn_right = t;
	x->rn_parent = t;
	t->rn_parent = p;
	if ((cp[t->rn_offset] & t->rn_bmask) == 0) {
		t->rn_right = x;
	} else {
		t->rn_right = tt;
		t->rn_left = x;
	}
	return (tt);
}

static struct radix_node *
rn_addmask(const void *n_arg, struct radix_mask_head *maskhead, int search, int skip)
{
	const uint8_t *netmask = n_arg;
	const uint8_t *c, *clim;
	uint8_t *cp;
	struct radix_node *x;
	int b = 0, mlen, j;
	int maskduplicated, isnormal;
	struct radix_node *saved_x;
	uint8_t addmask_key[RADIX_MAX_KEY_LEN];

	if ((mlen = LEN(netmask)) > RADIX_MAX_KEY_LEN)
		mlen = RADIX_MAX_KEY_LEN;
	if (skip == 0)
		skip = 1;
	if (mlen <= skip)
		return (maskhead->mask_nodes);

	memset(addmask_key, 0, RADIX_MAX_KEY_LEN);
	if (skip > 1)
		memcpy(addmask_key + 1, rn_ones + 1, skip - 1);
	memcpy(addmask_key + skip, netmask + skip, mlen - skip);
	for (cp = addmask_key + mlen; (cp > addmask_key) && cp[-1] == 0;)
		cp--;
	mlen = cp - addmask_key;
	if (mlen <= skip)
		return (maskhead->mask_nodes);
	*addmask_key = mlen;
	x = rn_search(addmask_key, maskhead->head.rnh_treetop);
	if (memcmp(addmask_key, x->rn_key, mlen) != 0)
		x = NULL;
	if (x || search)
		return (x);
	R_Zalloc(x, struct radix_node *, RADIX_MAX_KEY_LEN + 2 * sizeof (*x));
	if ((saved_x = x) == NULL)
		return (0);
	netmask = cp = (uint8_t *)(x + 2);
	memcpy(cp, addmask_key, mlen);
	x = rn_insert(cp, &maskhead->head, &maskduplicated, x);
	if (maskduplicated) {
		DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "rn_addmask: mask impossibly already in tree");
		R_Free(saved_x);
		return (x);
	}
#define	CONTIG(_c)	(((~(_c) + 1) & (_c)) == (uint8_t)(~(_c) + 1))
	clim = netmask + mlen;
	isnormal = 1;
	for (c = netmask + skip; c < clim && *c == 0xff; c++);
	if (c != clim) {
		for (j = 0x80; (j & *c) != 0; j >>= 1)
			b++;
		if (!CONTIG(*c) || c != (clim - 1))
			isnormal = 0;
	}
	b += (c - netmask) << 3;
	x->rn_bit = -1 - b;
	if (isnormal)
		x->rn_flags |= RNF_NORMAL;
	return (x);
}

static int
rn_lexobetter(const void *m_arg, const void *n_arg)
{
	const uint8_t *mp = m_arg, *np = n_arg, *lim;

	if (LEN(mp) > LEN(np))
		return (1);
	if (LEN(mp) == LEN(np))
		for (lim = mp + LEN(mp); mp < lim;)
			if (*mp++ > *np++)
				return (1);
	return (0);
}

static struct radix_mask *
rn_new_radix_mask(struct radix_node *tt, struct radix_mask *next)
{
	struct radix_mask *m;

	R_Malloc(m, struct radix_mask *, sizeof (struct radix_mask));
	if (m == NULL) {
		DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "Failed to allocate route mask");
		return (0);
	}
	memset(m, 0, sizeof(*m));
	m->rm_bit = tt->rn_bit;
	m->rm_flags = tt->rn_flags;
	if (tt->rn_flags & RNF_NORMAL)
		m->rm_leaf = tt;
	else
		m->rm_mask = tt->rn_mask;
	m->rm_mklist = next;
	tt->rn_mklist = m;
	return (m);
}

struct radix_node *
rn_addroute(void *v_arg, const void *n_arg, struct radix_head *head,
    struct radix_node treenodes[2])
{
	uint8_t *v = (uint8_t *)v_arg, *netmask = NULL;
	struct radix_node *t, *x = NULL, *tt;
	struct radix_node *saved_tt, *top = head->rnh_treetop;
	short b = 0, b_leaf = 0;
	int keyduplicated;
	uint8_t *mmask;
	struct radix_mask *m, **mp;

	if (n_arg)  {
		x = rn_addmask(n_arg, head->rnh_masks, 0, top->rn_offset);
		if (x == NULL)
			return (0);
		b_leaf = x->rn_bit;
		b = -1 - x->rn_bit;
		netmask = x->rn_key;
	}
	saved_tt = tt = rn_insert(v, head, &keyduplicated, treenodes);
	if (keyduplicated) {
		for (t = tt; tt; t = tt, tt = tt->rn_dupedkey) {
			if (tt->rn_mask == netmask)
				return (0);
			if (netmask == 0 ||
			    (tt->rn_mask &&
			     ((b_leaf < tt->rn_bit)
			      || rn_refines(netmask, tt->rn_mask)
			      || rn_lexobetter(netmask, tt->rn_mask))))
				break;
		}
		if (tt == saved_tt) {
			struct	radix_node *xx = x;
			(tt = treenodes)->rn_dupedkey = t;
			tt->rn_flags = t->rn_flags;
			tt->rn_parent = x = t->rn_parent;
			t->rn_parent = tt;
			if (x->rn_left == t)
				x->rn_left = tt;
			else
				x->rn_right = tt;
			saved_tt = tt; x = xx;
		} else {
			(tt = treenodes)->rn_dupedkey = t->rn_dupedkey;
			t->rn_dupedkey = tt;
			tt->rn_parent = t;
			if (tt->rn_dupedkey)
				tt->rn_dupedkey->rn_parent = tt;
		}
		tt->rn_key = (uint8_t *) v;
		tt->rn_bit = -1;
		tt->rn_flags = RNF_ACTIVE;
	}
	if (netmask) {
		tt->rn_mask = netmask;
		tt->rn_bit = x->rn_bit;
		tt->rn_flags |= x->rn_flags & RNF_NORMAL;
	}
	t = saved_tt->rn_parent;
	if (keyduplicated)
		goto on2;
	b_leaf = -1 - t->rn_bit;
	if (t->rn_right == saved_tt)
		x = t->rn_left;
	else
		x = t->rn_right;
	if (x->rn_bit < 0) {
	    for (mp = &t->rn_mklist; x; x = x->rn_dupedkey)
		if (x->rn_mask && (x->rn_bit >= b_leaf) && x->rn_mklist == 0) {
			*mp = m = rn_new_radix_mask(x, 0);
			if (m)
				mp = &m->rm_mklist;
		}
	} else if (x->rn_mklist) {
		for (mp = &x->rn_mklist; (m = *mp); mp = &m->rm_mklist)
			if (m->rm_bit >= b_leaf)
				break;
		t->rn_mklist = m; *mp = NULL;
	}
on2:
	if ((netmask == 0) || (b > t->rn_bit ))
		return (tt);
	b_leaf = tt->rn_bit;
	do {
		x = t;
		t = t->rn_parent;
	} while (b <= t->rn_bit && x != top);
	for (mp = &x->rn_mklist; (m = *mp); mp = &m->rm_mklist) {
		if (m->rm_bit < b_leaf)
			continue;
		if (m->rm_bit > b_leaf)
			break;
		if (m->rm_flags & RNF_NORMAL) {
			mmask = m->rm_leaf->rn_mask;
			if (tt->rn_flags & RNF_NORMAL) {
			    DEBUG_ERROR(DEBUG_CATEGORY_ROUTING,
			        "Non-unique normal route, mask not entered");
				return (tt);
			}
		} else
			mmask = m->rm_mask;
		if (mmask == netmask) {
			m->rm_refs++;
			tt->rn_mklist = m;
			return (tt);
		}
		if (rn_refines(netmask, mmask)
		    || rn_lexobetter(netmask, mmask))
			break;
	}
	*mp = rn_new_radix_mask(tt, *mp);
	return (tt);
}

struct radix_node *
rn_delete(const void *v_arg, const void *netmask_arg, struct radix_head *head)
{
	struct radix_node *t, *p, *x, *tt;
	struct radix_mask *m, *saved_m, **mp;
	struct radix_node *dupedkey, *saved_tt, *top;
	const uint8_t *v;
	const uint8_t *netmask;
	int b, head_off, vlen;

	v = v_arg;
	netmask = netmask_arg;
	x = head->rnh_treetop;
	tt = rn_search(v, x);
	head_off = x->rn_offset;
	vlen =  LEN(v);
	saved_tt = tt;
	top = x;
	if (tt == NULL ||
	    memcmp(v + head_off, tt->rn_key + head_off, vlen - head_off))
		return (0);
	if (netmask) {
		x = rn_addmask(netmask, head->rnh_masks, 1, head_off);
		if (x == NULL)
			return (0);
		netmask = x->rn_key;
		while (tt->rn_mask != netmask)
			if ((tt = tt->rn_dupedkey) == NULL)
				return (0);
	}
	if (tt->rn_mask == 0 || (saved_m = m = tt->rn_mklist) == NULL)
		goto on1;
	if (tt->rn_flags & RNF_NORMAL) {
		if (m->rm_leaf != tt || m->rm_refs > 0) {
			DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "rn_delete: inconsistent annotation");
			return (0);
		}
	} else {
		if (m->rm_mask != tt->rn_mask) {
			DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "rn_delete: inconsistent annotation");
			goto on1;
		}
		if (--m->rm_refs >= 0)
			goto on1;
	}
	b = -1 - tt->rn_bit;
	t = saved_tt->rn_parent;
	if (b > t->rn_bit)
		goto on1;
	do {
		x = t;
		t = t->rn_parent;
	} while (b <= t->rn_bit && x != top);
	for (mp = &x->rn_mklist; (m = *mp); mp = &m->rm_mklist)
		if (m == saved_m) {
			*mp = m->rm_mklist;
			R_Free(m);
			break;
		}
	if (m == NULL) {
		DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "rn_delete: couldn't find our annotation");
		if (tt->rn_flags & RNF_NORMAL)
			return (0);
	}
on1:
	if (tt->rn_flags & RNF_ROOT)
		return (0);
	t = tt->rn_parent;
	dupedkey = saved_tt->rn_dupedkey;
	if (dupedkey) {
		if (tt == saved_tt) {
			x = dupedkey; x->rn_parent = t;
			if (t->rn_left == tt)
				t->rn_left = x;
			else
				t->rn_right = x;
		} else {
			for (x = p = saved_tt; p && p->rn_dupedkey != tt;)
				p = p->rn_dupedkey;
			if (p) {
				p->rn_dupedkey = tt->rn_dupedkey;
				if (tt->rn_dupedkey)
					tt->rn_dupedkey->rn_parent = p;
			} else DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "rn_delete: couldn't find us");
		}
		t = tt + 1;
		if  (t->rn_flags & RNF_ACTIVE) {
			*++x = *t;
			p = t->rn_parent;
			if (p->rn_left == t)
				p->rn_left = x;
			else
				p->rn_right = x;
			x->rn_left->rn_parent = x;
			x->rn_right->rn_parent = x;
		}
		goto out;
	}
	if (t->rn_left == tt)
		x = t->rn_right;
	else
		x = t->rn_left;
	p = t->rn_parent;
	if (p->rn_right == t)
		p->rn_right = x;
	else
		p->rn_left = x;
	x->rn_parent = p;
	if (t->rn_mklist) {
		if (x->rn_bit >= 0) {
			for (mp = &x->rn_mklist; (m = *mp);)
				mp = &m->rm_mklist;
			*mp = t->rn_mklist;
		} else {
			for (m = t->rn_mklist; m && x; x = x->rn_dupedkey)
				if (m == x->rn_mklist) {
					struct radix_mask *mm = m->rm_mklist;
					x->rn_mklist = 0;
					if (--(m->rm_refs) < 0)
						R_Free(m);
					m = mm;
				}
			if (m)
				DEBUG_ERROR(DEBUG_CATEGORY_ROUTING,
				    "rn_delete: Orphaned Mask %p at %p", m, x);
		}
	}
	x = tt + 1;
	if (t != x) {
		*t = *x;
		t->rn_left->rn_parent = t;
		t->rn_right->rn_parent = t;
		p = x->rn_parent;
		if (p->rn_left == x)
			p->rn_left = t;
		else
			p->rn_right = t;
	}
out:
	tt->rn_flags &= ~RNF_ACTIVE;
	tt[1].rn_flags &= ~RNF_ACTIVE;
	return (tt);
}

int
rn_walktree_from(struct radix_head *h, void *a, void *m,
    walktree_f_t *f, void *w)
{
	int error;
	struct radix_node *base, *next;
	uint8_t *xa = (uint8_t *)a;
	uint8_t *xm = (uint8_t *)m;
	struct radix_node *rn, *last = NULL;
	int stopping = 0;
	int lastb;

	assert(m != NULL);

	for (rn = h->rnh_treetop; rn->rn_bit >= 0; ) {
		last = rn;
		if (!(rn->rn_bmask & xm[rn->rn_offset]))
			break;
		if (rn->rn_bmask & xa[rn->rn_offset])
			rn = rn->rn_right;
		else
			rn = rn->rn_left;
	}

	if (rn->rn_bit >= 0)
		rn = last;
	lastb = last->rn_bit;

	while (rn->rn_bit >= 0)
		rn = rn->rn_left;

	while (!stopping) {
		base = rn;
		while (rn->rn_parent->rn_right == rn
		       && !(rn->rn_flags & RNF_ROOT)) {
			rn = rn->rn_parent;
			if (rn->rn_bit <= lastb)
				stopping = 1;
		}

		if (rn->rn_parent->rn_flags & RNF_ROOT)
			stopping = 1;

		for (rn = rn->rn_parent->rn_right; rn->rn_bit >= 0;)
			rn = rn->rn_left;
		next = rn;
		while ((rn = base) != NULL) {
			base = rn->rn_dupedkey;
			if (!(rn->rn_flags & RNF_ROOT)
			    && (error = (*f)(rn, w)))
				return (error);
		}
		rn = next;

		if (rn->rn_flags & RNF_ROOT)
			stopping = 1;
	}
	return (0);
}

int
rn_walktree(struct radix_head *h, walktree_f_t *f, void *w)
{
	int error;
	struct radix_node *base, *next;
	struct radix_node *rn = h->rnh_treetop;

	while (rn->rn_bit >= 0)
		rn = rn->rn_left;
	for (;;) {
		base = rn;
		while (rn->rn_parent->rn_right == rn
		       && (rn->rn_flags & RNF_ROOT) == 0)
			rn = rn->rn_parent;
		for (rn = rn->rn_parent->rn_right; rn->rn_bit >= 0;)
			rn = rn->rn_left;
		next = rn;
		while ((rn = base)) {
			base = rn->rn_dupedkey;
			if (!(rn->rn_flags & RNF_ROOT)
			    && (error = (*f)(rn, w)))
				return (error);
		}
		rn = next;
		if (rn->rn_flags & RNF_ROOT)
			return (0);
	}
}

void
rn_inithead_internal(struct radix_head *rh, struct radix_node *base_nodes, int off)
{
	struct radix_node *t, *tt, *ttt;

	t = rn_newpair(rn_zeros, off, base_nodes);
	ttt = base_nodes + 2;
	t->rn_right = ttt;
	t->rn_parent = t;
	tt = t->rn_left;
	tt->rn_flags = t->rn_flags = RNF_ROOT | RNF_ACTIVE;
	tt->rn_bit = -1 - off;
	*ttt = *tt;
	ttt->rn_key = rn_ones;

	rh->rnh_treetop = t;
}

static void
rn_detachhead_internal(struct radix_head *head)
{
	assert(head != NULL);
	R_Free(head);
}

int
rn_inithead(void **head, int off)
{
	struct radix_node_head *rnh;
	struct radix_mask_head *rmh;

	rnh = *head;
	rmh = NULL;

	if (*head != NULL)
		return (1);

	R_Zalloc(rnh, struct radix_node_head *, sizeof (*rnh));
	R_Zalloc(rmh, struct radix_mask_head *, sizeof (*rmh));
	if (rnh == NULL || rmh == NULL) {
		if (rnh != NULL)
			R_Free(rnh);
		if (rmh != NULL)
			R_Free(rmh);
		return (0);
	}

	rn_inithead_internal(&rnh->rh, rnh->rnh_nodes, off);
	rn_inithead_internal(&rmh->head, rmh->mask_nodes, 0);
	*head = rnh;
	rnh->rh.rnh_masks = rmh;

	rnh->rnh_addaddr = rn_addroute;
	rnh->rnh_deladdr = rn_delete;
	rnh->rnh_matchaddr = rn_match;
	rnh->rnh_lookup = rn_lookup;
	rnh->rnh_walktree = rn_walktree;
	rnh->rnh_walktree_from = rn_walktree_from;

	return (1);
}

static int
rn_freeentry(struct radix_node *rn, void *arg)
{
	struct radix_head * const rnh = arg;
	struct radix_node *x;

	x = (struct radix_node *)rn_delete(rn + 2, NULL, rnh);
	if (x != NULL)
		R_Free(x);
	return (0);
}

int
rn_detachhead(void **head)
{
	struct radix_node_head *rnh;

	assert(head != NULL && *head != NULL);
	rnh = (struct radix_node_head *)(*head);

	rn_walktree(&rnh->rh.rnh_masks->head, rn_freeentry, rnh->rh.rnh_masks);
	rn_detachhead_internal(&rnh->rh.rnh_masks->head);
	rn_detachhead_internal(&rnh->rh);

	*head = NULL;

	return (1);
}
