

/******************************************************************************
 * INCLUDES
 *****************************************************************************/
#include "sptensor.h"
#include "matrix.h"
#include "sort.h"
#include "io.h"
#include "timer.h"
#include "util.h"

#include <math.h>


/******************************************************************************
 * PRIVATE FUNCTONS
 *****************************************************************************/
static inline int p_same_coord(
  sptensor_t const * const tt,
  idx_t const i,
  idx_t const j)
{
  idx_t const nmodes = tt->nmodes;
  if(nmodes == 3) {
    return (tt->ind[0][i] == tt->ind[0][j]) &&
           (tt->ind[1][i] == tt->ind[1][j]) &&
           (tt->ind[2][i] == tt->ind[2][j]);
  } else {
    for(idx_t m=0; m < nmodes; ++m) {
      if(tt->ind[m][i] != tt->ind[m][j]) {
        return 0;
      }
    }
    return 1;
  }
}




/******************************************************************************
 * PUBLIC FUNCTONS
 *****************************************************************************/

val_t tt_normsq(sptensor_t const * const tt)
{
  val_t norm = 0.0;
  val_t const * const restrict tv = tt->vals;
  for(idx_t n=0; n < tt->nnz; ++n) {
    norm += tv[n] * tv[n];
  }
  return norm;
}


double tt_density(
  sptensor_t const * const tt)
{
  double root = pow((double)tt->nnz, 1./(double)tt->nmodes);
  double density = 1.0;
  for(idx_t m=0; m < tt->nmodes; ++m) {
    density *= root / (double)tt->dims[m];
  }

  return density;
}


idx_t * tt_get_slices(
  sptensor_t const * const tt,
  idx_t const m,
  idx_t * nunique)
{
  /* get maximum number of unique slices */
  idx_t minidx = tt->dims[m];
  idx_t maxidx = 0;

  idx_t const nnz = tt->nnz;
  idx_t const * const inds = tt->ind[m];

  /* find maximum number of uniques */
  for(idx_t n=0; n < nnz; ++n) {
    minidx = SS_MIN(minidx, inds[n]);
    maxidx = SS_MAX(maxidx, inds[n]);
  }
  /* +1 because maxidx is inclusive, not exclusive */
  idx_t const maxrange = 1 + maxidx - minidx;

  /* mark slices which are present and count uniques */
  idx_t * slice_mkrs = (idx_t *) calloc(maxrange, sizeof(idx_t));
  idx_t found = 0;
  for(idx_t n=0; n < nnz; ++n) {
    assert(inds[n] >= minidx);
    idx_t const idx = inds[n] - minidx;
    if(slice_mkrs[idx] == 0) {
      slice_mkrs[idx] = 1;
      ++found;
    }
  }
  *nunique = found;

  /* now copy unique slices */
  idx_t * slices = (idx_t *) splatt_malloc(found * sizeof(idx_t));
  idx_t ptr = 0;
  for(idx_t i=0; i < maxrange; ++i) {
    if(slice_mkrs[i] == 1) {
      slices[ptr++] = i + minidx;
    }
  }

  free(slice_mkrs);

  return slices;
}


idx_t * tt_get_hist(
  sptensor_t const * const tt,
  idx_t const mode)
{
  idx_t * restrict hist = splatt_malloc(tt->dims[mode] * sizeof(*hist));
  memset(hist, 0, tt->dims[mode] * sizeof(*hist));

  idx_t const * const restrict inds = tt->ind[mode];
  #pragma omp parallel for schedule(static)
  for(idx_t x=0; x < tt->nnz; ++x) {
    #pragma omp atomic
    ++hist[inds[x]];
  }

  return hist;
}


sptensor_t * tt_copy(
  sptensor_t const * const tt)
{
  idx_t const nnz = tt->nnz;
  idx_t const nmodes = tt->nmodes;

  sptensor_t * ret = tt_alloc(nnz, nmodes);
  ret->tiled = tt->tiled;
  ret->type = tt->type;
  memcpy(ret->dims, tt->dims, nmodes * sizeof(*(tt->dims)));

  /* copy vals */
  par_memcpy(ret->vals, tt->vals, nnz * sizeof(*(tt->vals)));

  /* copy inds */
  for(idx_t m=0; m < nmodes; ++m) {
    par_memcpy(ret->ind[m], tt->ind[m], nnz * sizeof(**(tt->ind)));

    if(tt->indmap[m] != NULL) {
      ret->indmap[m] = splatt_malloc(tt->dims[m] * sizeof(**(ret->indmap)));
      par_memcpy(ret->indmap[m], tt->indmap[m],
          tt->dims[m] * sizeof(**(ret->indmap)));
    } else {
      ret->indmap[m] = NULL;
    }
  }

  return ret;
}



sptensor_t * tt_union(
  sptensor_t * const tt_a,
  sptensor_t * const tt_b)
{
  assert(tt_a->nmodes == tt_b->nmodes);
  idx_t const nmodes = tt_a->nmodes;

  tt_sort(tt_a, 0, NULL);
  tt_sort(tt_b, 0, NULL);

  /* count nnz in the union */
  idx_t uniq = 0;
  idx_t ptra = 0;
  idx_t ptrb = 0;

  while(ptra < tt_a->nnz && ptrb < tt_b->nnz) {
    /* if nnz are the same */
    bool same = true;
    /* if -1 if tt_a smaller, 0 for same sparsity, 1 for tt_b larger */
    int order = 0;

    if(tt_a->vals[ptra] != tt_b->vals[ptrb]) {
      same = false;
    }
    for(idx_t m=0; m < nmodes; ++m) {
      if(tt_a->ind[m][ptra] != tt_b->ind[m][ptrb]) {
        same = false;
        if(tt_a->ind[m][ptra] < tt_b->ind[m][ptrb]) {
          order = -1;
        } else {
          order = 1;
        }
        break;
      }
    }

    if(same) {
      printf("A: (%lu %lu %lu %0.1f) B: (%lu %lu %lu %0.1f)\n",
          tt_a->ind[0][ptra], tt_a->ind[1][ptra], tt_a->ind[2][ptra], tt_a->vals[ptra],
          tt_b->ind[0][ptrb], tt_b->ind[1][ptrb], tt_b->ind[2][ptrb], tt_b->vals[ptrb]);
      /* just copy one */
      ++ptra;
      ++ptrb;
    } else {
      /* if tt_a and tt_b have the same idx but different values */
      if(order == 0) {
        ++ptra;
        ++ptrb;
        ++uniq; /* account for both */
      } else if(order == -1) {
        ++ptra;
      } else {
        ++ptrb;
      }
    }
    ++uniq;
  }
  /* grab leftovers */
  uniq += (tt_a->nnz - ptra) + (tt_b->nnz - ptrb);

  /* allocate */
  sptensor_t * ret = tt_alloc(uniq, nmodes);

  /* now copy every thing over */
  uniq = 0;
  ptra = 0;
  ptrb = 0;
  while(ptra < tt_a->nnz && ptrb < tt_b->nnz) {
    /* if nnz are the same */
    bool same = true;
    /* if -1 if tt_a smaller, 0 for same sparsity, 1 for tt_b larger */
    int order = 0;

    if(tt_a->vals[ptra] != tt_b->vals[ptrb]) {
      same = false;
    }
    for(idx_t m=0; m < nmodes; ++m) {
      if(tt_a->ind[m][ptra] != tt_b->ind[m][ptrb]) {
        same = false;
        if(tt_a->ind[m][ptra] < tt_b->ind[m][ptrb]) {
          order = -1;
        } else {
          order = 1;
        }
        break;
      }
    }


    if(same) {
      /* just copy one */
      ret->vals[uniq] = tt_a->vals[ptra];
      for(idx_t m=0; m < nmodes; ++m) {
        ret->ind[m][uniq] = tt_a->ind[m][ptra];
      }

      ++ptra;
      ++ptrb;
    } else {
      if(order == 0) {
        /* just grab both */
        ret->vals[uniq] = tt_a->vals[ptra];
        for(idx_t m=0; m < nmodes; ++m) {
          ret->ind[m][uniq] = tt_a->ind[m][ptra];
        }
        ++uniq;

        ret->vals[uniq] = tt_b->vals[ptrb];
        for(idx_t m=0; m < nmodes; ++m) {
          ret->ind[m][uniq] = tt_b->ind[m][ptrb];
        }
        ++ptra;
        ++ptrb;
      } else if(order == -1) {
        ret->vals[uniq] = tt_a->vals[ptra];
        for(idx_t m=0; m < nmodes; ++m) {
          ret->ind[m][uniq] = tt_a->ind[m][ptra];
        }
        ++ptra;
      } else {
        ret->vals[uniq] = tt_b->vals[ptrb];
        for(idx_t m=0; m < nmodes; ++m) {
          ret->ind[m][uniq] = tt_b->ind[m][ptrb];
        }
        ++ptrb;
      }
    }
    ++uniq;
  }

  /* grab leftovers */
  for(; ptra < tt_a->nnz; ++ptra) {
    ret->vals[uniq] = tt_a->vals[ptra];
    for(idx_t m=0; m < nmodes; ++m) {
      ret->ind[m][uniq] = tt_a->ind[m][ptra];
    }
    ++uniq;
  }

  for(; ptrb < tt_b->nnz; ++ptrb) {
    ret->vals[uniq] = tt_b->vals[ptrb];
    for(idx_t m=0; m < nmodes; ++m) {
      ret->ind[m][uniq] = tt_b->ind[m][ptrb];
    }
    ++uniq;
  }

  /* grab new dims */
  tt_fill_dims(ret);

  return ret;
}


void tt_fill_dims(
  sptensor_t * const tt)
{
  for(idx_t m=0; m < tt->nmodes; ++m) {
    idx_t dim = 0;
    #pragma omp parallel for reduction(max:dim)
    for(idx_t n=0; n < tt->nnz; ++n) {
      dim = SS_MAX(dim, tt->ind[m][n] + 1);
    }
    tt->dims[m] = dim;
  }
}


idx_t tt_remove_dups(
  sptensor_t * const tt)
{
  tt_sort(tt, 0, NULL);

  idx_t const nmodes = tt->nmodes;

  idx_t newnnz = 0;
  for(idx_t nnz = 1; nnz < tt->nnz; ++nnz) {
    /* if the two nnz are the same, average them */
    if(p_same_coord(tt, newnnz, nnz)) {
      tt->vals[newnnz] += tt->vals[nnz];
    } else {
      /* new another nnz */
      ++newnnz;
      for(idx_t m=0; m < nmodes; ++m) {
        tt->ind[m][newnnz] = tt->ind[m][nnz];
      }
      tt->vals[newnnz] = tt->vals[nnz];
    }
  }
  ++newnnz;

  idx_t const diff = tt->nnz - newnnz;
  tt->nnz = newnnz;
  return diff;
}


idx_t tt_remove_empty(
  sptensor_t * const tt)
{
  idx_t dim_sizes[MAX_NMODES];

  idx_t nremoved = 0;

  /* Allocate indmap */
  idx_t const nmodes = tt->nmodes;
  idx_t const nnz = tt->nnz;

  idx_t maxdim = 0;
  for(idx_t m=0; m < tt->nmodes; ++m) {
    maxdim = tt->dims[m] > maxdim ? tt->dims[m] : maxdim;
  }
  /* slice counts */
  idx_t * scounts = (idx_t *) splatt_malloc(maxdim * sizeof(idx_t));

  for(idx_t m=0; m < nmodes; ++m) {
    dim_sizes[m] = 0;
    memset(scounts, 0, maxdim * sizeof(idx_t));

    /* Fill in indmap */
    for(idx_t n=0; n < tt->nnz; ++n) {
      /* keep track of #unique slices */
      if(scounts[tt->ind[m][n]] == 0) {
        scounts[tt->ind[m][n]] = 1;
        ++dim_sizes[m];
      }
    }

    /* move on if no remapping is necessary */
    if(dim_sizes[m] == tt->dims[m]) {
      tt->indmap[m] = NULL;
      continue;
    }

    nremoved += tt->dims[m] - dim_sizes[m];

    /* Now scan to remove empty slices */
    idx_t ptr = 0;
    for(idx_t i=0; i < tt->dims[m]; ++i) {
      if(scounts[i] == 1) {
        scounts[i] = ptr++;
      }
    }

    tt->indmap[m] = (idx_t *) splatt_malloc(dim_sizes[m] * sizeof(idx_t));

    /* relabel all indices in mode m */
    tt->dims[m] = dim_sizes[m];
    for(idx_t n=0; n < tt->nnz; ++n) {
      idx_t const global = tt->ind[m][n];
      idx_t const local = scounts[global];
      assert(local < dim_sizes[m]);
      tt->indmap[m][local] = global; /* store local -> global mapping */
      tt->ind[m][n] = local;
    }
  }

  free(scounts);
  return nremoved;
}



/******************************************************************************
 * PUBLIC FUNCTONS
 *****************************************************************************/
sptensor_t * tt_read(
  char const * const ifname)
{
  return tt_read_file(ifname);
}


sptensor_t * tt_alloc(
  idx_t const nnz,
  idx_t const nmodes)
{
  sptensor_t * tt = splatt_malloc(sizeof(*tt));
  tt->tiled = SPLATT_NOTILE;

  tt->nnz = nnz;
  tt->vals = splatt_malloc(nnz * sizeof(*(tt->vals)));

  tt->nmodes = nmodes;
  tt->type = (nmodes == 3) ? SPLATT_3MODE : SPLATT_NMODE;

  tt->dims = splatt_malloc(nmodes * sizeof(*(tt->dims)));
  tt->ind = splatt_malloc(nmodes * sizeof(*(tt->ind)));
  for(idx_t m=0; m < nmodes; ++m) {
    tt->ind[m] = splatt_malloc(nnz * sizeof(**(tt->ind)));
    tt->indmap[m] = NULL;
  }

  return tt;
}


void tt_fill(
  sptensor_t * const tt,
  idx_t const nnz,
  idx_t const nmodes,
  idx_t ** const inds,
  val_t * const vals)
{
  tt->tiled = SPLATT_NOTILE;
  tt->nnz = nnz;
  tt->vals = vals;
  tt->ind = inds;

  tt->nmodes = nmodes;
  tt->type = (nmodes == 3) ? SPLATT_3MODE : SPLATT_NMODE;

  tt->dims = (idx_t*) splatt_malloc(nmodes * sizeof(idx_t));
  for(idx_t m=0; m < nmodes; ++m) {
    tt->indmap[m] = NULL;
  }
  tt_fill_dims(tt);
}



void tt_free(
  sptensor_t * tt)
{
  tt->nnz = 0;
  for(idx_t m=0; m < tt->nmodes; ++m) {
    free(tt->ind[m]);
    free(tt->indmap[m]);
  }
  tt->nmodes = 0;
  free(tt->dims);
  free(tt->ind);
  free(tt->vals);
  free(tt);
}

spmatrix_t * tt_unfold(
  sptensor_t * const tt,
  idx_t const mode)
{
  idx_t nrows = tt->dims[mode];
  idx_t ncols = 1;

  for(idx_t m=1; m < tt->nmodes; ++m) {
    ncols *= tt->dims[(mode + m) % tt->nmodes];
  }

  /* sort tt */
  tt_sort(tt, mode, NULL);

  /* allocate and fill matrix */
  spmatrix_t * mat = spmat_alloc(nrows, ncols, tt->nnz);
  idx_t * const rowptr = mat->rowptr;
  idx_t * const colind = mat->colind;
  val_t * const mvals  = mat->vals;

  /* make sure to skip ahead to the first non-empty slice */
  idx_t row = 0;
  for(idx_t n=0; n < tt->nnz; ++n) {
    /* increment row and account for possibly empty ones */
    while(row <= tt->ind[mode][n]) {
      rowptr[row++] = n;
    }
    mvals[n] = tt->vals[n];

    idx_t col = 0;
    idx_t mult = 1;
    for(idx_t m = 0; m < tt->nmodes; ++m) {
      idx_t const off = tt->nmodes - 1 - m;
      if(off == mode) {
        continue;
      }
      col += tt->ind[off][n] * mult;
      mult *= tt->dims[off];
    }

    colind[n] = col;
  }
  /* account for any empty rows at end, too */
  for(idx_t r=row; r <= nrows; ++r) {
    rowptr[r] = tt->nnz;
  }

  return mat;
}

