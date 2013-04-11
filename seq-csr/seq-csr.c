/* -*- mode: C; mode: folding; fill-column: 70; -*- */
/* Copyright 2010-2011,  Georgia Institute of Technology, USA. */
/* See COPYING for license. */
#define _FILE_OFFSET_BITS 64
#define _THREAD_SAFE
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <assert.h>

#include "../compat.h"
#include "../graph500.h"
#include "../xalloc.h"
#include "../packed_edge.h"
#include "../generator.h"
#include "../sorts.h"

char IMPLEMENTATION[] = "Reference sequential";
#define ALPHA 14
#define BETA  24

#define WORD_OFFSET(n) (n/64)
#define BIT_OFFSET(n) (n & 0x3f)

static int64_t nv;
static int64_t * restrict xoff; /* Length 2*nv+2 */
static int64_t * restrict xadj;

static void
free_graph (void)
{
  if (xadj) xfree_large (xadj);
  if (xoff) xfree_large (xoff);
}

static inline void
canonical_order_edge (int64_t * restrict ip, int64_t * restrict jp)
{
  int64_t i = *ip, j = *jp;
#if !defined(ORDERED_TRIL)
  if (((i ^ j) & 0x01) == (i < j)) {
    *ip = i;
    *jp = j;
  } else {
    *ip = j;
    *jp = i;
  }
#else
  if (i < j) {
    *ip = j;
    *jp = i;
  }
#endif
}

int 
create_graph_from_edgelist (struct packed_edge *IJ, int64_t nedge, int64_t nv_in)
{
  int64_t * restrict xoff_half = NULL;
  int64_t * restrict xadj_half = NULL;
  size_t sz;
  int64_t accum;

  nv = nv_in;
  xoff = NULL;
  xadj = NULL;

  /* Allocate offset arrays.  */
  sz = (nv+1) * sizeof (*xoff);
  xoff = xmalloc_large_ext (sz);
  xoff_half = xmalloc_large_ext (sz);
  if (!xoff || !xoff_half) goto err;

  /* Set up xoff_half to hold a single copy of the edges. */
  for (int64_t k = 0; k <= nv; ++k)
    xoff_half[k] = 0;

  int64_t nself_edges = 0;

  for (int64_t k = 0; k < nedge; ++k) {
    int64_t i, j;
#if !defined(STORED_EDGELIST)
    uint8_t w;
    make_edge (k, &i, &j, &w);
#else
    i = get_v0_from_edge(&IJ[k]);
    j = get_v1_from_edge(&IJ[k]);
#endif
    assert (i >= 0);
    assert (j >= 0);
    if (i != j) { /* Skip self-edges. */
      canonical_order_edge (&i, &j);
      ++xoff_half[i+1];
    } else
      ++nself_edges;
  }

  /* Prefix sum to convert counts to offsets. */
  accum = 0;
  for (int64_t k = 1; k <= nv; ++k) {
    int64_t tmp = xoff_half[k];
    xoff_half[k] = accum;
    accum += tmp;
  }

  assert (accum + nself_edges == nedge);
  /* All either counted or ignored. */

  /* Allocate endpoint storage.  Do not yet know final number of edges. */
  xadj_half = xmalloc_large_ext (accum * sizeof (*xadj_half));
  if (!xadj_half) goto err;

  /* Copy endpoints. */
  for (int64_t k = 0; k < nedge; ++k) {
    int64_t i, j;
#if !defined(STORED_EDGELIST)
    uint8_t w;
    make_edge (k, &i, &j, &w);
#else
    i = get_v0_from_edge(&IJ[k]);
    j = get_v1_from_edge(&IJ[k]);
#endif
    assert (i >= 0);
    assert (j >= 0);
    if (i != j) { /* Skip self-edges. */
      int64_t where;
      canonical_order_edge (&i, &j);
      where = xoff_half[i+1]++;
      xadj_half[where] = j;
    }
  }

  int64_t ndup = 0;

  for (int64_t i = 0; i <= nv; ++i)
    xoff[i] = 0;

  /* Collapse duplicates and count final numbers. */
  for (int64_t i = 0; i < nv; ++i) {
    int64_t kcur = xoff_half[i];
    const int64_t kend = xoff_half[i+1];

    if (kcur == kend) continue; /* Empty. */

    introsort_i64 (&xadj_half[kcur], kend-kcur);

    for (int64_t k = kcur+1; k < kend; ++k)
      if (xadj_half[k] != xadj_half[kcur]) {
	xadj_half[++kcur] = xadj_half[k];
      }
    ++kcur;
    if (kcur != kend)
      xadj_half[kcur] = -1;
    ndup += kend - kcur;

    /* Current vertex i count. */
    xoff[i+1] += kcur - xoff_half[i];

    /* Scatter adjacent vertex j counts. */
    for (int64_t k = xoff_half[i]; k < kcur; ++k) {
      const int64_t j = xadj_half[k];
      ++xoff[j+1];
    }
  }

  assert (xoff[0] == 0);

  /* Another prefix sum to convert counts to offsets. */
  accum = 0;
  for (int64_t k = 1; k <= nv; ++k) {
    int64_t tmp = xoff[k];
    xoff[k] = accum;
    accum += tmp;
  }

  assert (xoff[0] == 0);

  assert (accum % 2 == 0);
  assert (accum/2 + ndup + nself_edges == nedge);

  /* Allocate final endpoint storage. */
  xadj = xmalloc_large_ext (accum * sizeof (*xadj));
  if (!xadj) goto err;

  /* Copy to final locations. */
  for (int64_t i = 0; i < nv; ++i) {
    const int64_t khalfstart = xoff_half[i];
    const int64_t khalfend = xoff_half[i+1];
    int64_t khalflen = khalfend - khalfstart;
    const int64_t kstart = xoff[i+1];
    int64_t k;

    if (!khalflen) continue; /* Empty. */

    /* Copy the contiguous portion. */
    for (k = 0; k < khalflen; ++k) {
      const int64_t j = xadj_half[khalfstart + k];
      if (j < 0) break;
      xadj[kstart + k] = j;
    }
    xoff[i+1] += k;

    /* Scatter the other side. */
    for (int64_t k2 = 0; k2 < k; ++k2) {
      const int64_t j = xadj_half[khalfstart + k2];
      const int64_t where = xoff[j+1]++;
      xadj[where] = i;
    }
  }

  assert (accum == xoff[nv]);

  xfree_large (xoff_half);
  xfree_large (xadj_half);
  return 0;

 err:
  if (xoff_half) xfree_large (xoff_half);
  if (xadj_half) xfree_large (xadj_half);
  if (xoff) xfree_large (xoff);
  if (xadj) xfree_large (xadj);
  return -1;
}

#define SET_BIT(bm, i) (bm)[WORD_OFFSET((i))] |= (((uint64_t) 1)<<BIT_OFFSET((i)))
#define GET_BIT(bm, i) (bm)[WORD_OFFSET((i))] & (((uint64_t) 1)<<BIT_OFFSET((i)))

static void
fill_bitmap_from_queue (uint64_t * restrict bm, int64_t * restrict vlist,
			int64_t out, int64_t in)
{
  for (int64_t q_index = out; q_index < in; q_index++)
    SET_BIT(bm, vlist[q_index]);
}

static void
fill_queue_from_bitmap (uint64_t * restrict bm,
			int64_t * restrict vlist,
			int64_t * restrict out, int64_t * restrict in)
{
  int64_t len = 0;

  *out = 0;

  assert (8 == CHAR_BIT);
  assert (8 == sizeof (uint64_t));

  for (int64_t k = 0; k < nv; k += 64) {
    const uint64_t m = bm[k/64];
    if (!m) continue;
    for (int kk = 0; kk < 64; ++kk)
      if (m & (((uint64_t)1) << kk))
	vlist[len++] = k + kk;
  }

  *in = len;
}

static int64_t
bfs_bottom_up_step (int64_t * restrict bfs_tree,
		    const uint64_t * restrict past, uint64_t * restrict next)
{
  int64_t awake_count = 0;

  for (int64_t i = 0; i < nv; i++) {
    if (bfs_tree[i] == -1) {
      for (int64_t vo = xoff[i]; vo < xoff[1+i]; vo++) {
	const int64_t j = xadj[vo];
	if (GET_BIT(past, j)) {
	  bfs_tree[i] = j;
	  SET_BIT(next, i);
	  awake_count++;
	  break;
	}
      }
    }
  }

  return awake_count;
}

static void
bfs_top_down_step (int64_t * restrict bfs_tree,
		   int64_t * restrict vlist,
		   int64_t * head_in, int64_t * tail_in)
{
  const int64_t tail = *tail_in;
  int64_t new_tail = tail;

  for (int64_t k = *head_in; k < tail; ++k) {
    const int64_t v = vlist[k];
    const int64_t veo = xoff[1+v];
    int64_t vo;
    for (vo = xoff[v]; vo < veo; ++vo) {
      const int64_t j = xadj[vo];
      if (bfs_tree[j] == -1) {
	assert (new_tail < nv);
	vlist[new_tail++] = j;
	bfs_tree[j] = v;
      }
    }
  }

  *head_in = tail;
  *tail_in = new_tail;

  return;
}

int
make_bfs_tree (int64_t *bfs_tree_out, int64_t srcvtx)
{
  int64_t * restrict bfs_tree = bfs_tree_out;
  uint64_t * bm_storage = NULL;
  const size_t bmlen = (nv + 63) & ~63;
  uint64_t * restrict past = NULL;
  uint64_t * restrict next = NULL;
  int err = 0;

  int64_t * restrict vlist = NULL;
  int64_t k1, k2;

  int64_t down_cutoff = nv / BETA;
  int64_t scout_count = xoff[1+srcvtx] - xoff[srcvtx];
  int64_t awake_count = 1;
  int64_t edges_to_check = xoff[nv];

  vlist = xmalloc_large (nv * sizeof (*vlist));
  if (!vlist) return -1;

  bm_storage = xmalloc_large (2 * bmlen * sizeof (*bm_storage));
  if (!bm_storage) {
    free (vlist);
    return -2;
  }
  past = bm_storage;
  next = &bm_storage[bmlen];

  vlist[0] = srcvtx;
  k1 = 0; k2 = 1;

  for (int64_t k = 0; k < nv; ++k)
    bfs_tree[k] = -1;
  bfs_tree[srcvtx] = srcvtx;

  while (awake_count != 0) {
    if (scout_count < ((edges_to_check - scout_count)/ALPHA)) {
      // Top-down
      bfs_top_down_step (bfs_tree, vlist, &k1, &k2);
      edges_to_check -= scout_count;
      awake_count = k2-k1;
    } else {
      // Bottom-up
      for (int64_t k = 0; k < bmlen; ++k)
	next[k] = 0;
      fill_bitmap_from_queue (next, vlist, k1, k2);
      do {
	/* Swap past & next */
	uint64_t * restrict t = past;
	past = next;
	next = t;
	for (int64_t k = 0; k < bmlen; ++k)
	  next[k] = 0;
	awake_count = bfs_bottom_up_step (bfs_tree, past, next);
      } while ((awake_count > down_cutoff));
      fill_queue_from_bitmap (next, vlist, &k1, &k2);
    }
    // Count the number of edges in the frontier
    scout_count = 0;
    for (int64_t i=k1; i<k2; i++) {
      int64_t v = vlist[i];
      scout_count += xoff[1+v] - xoff[v];
    }
  }

  xfree_large (bm_storage);
  xfree_large (vlist);

  return err;
}

void
destroy_graph (void)
{
  free_graph ();
}
