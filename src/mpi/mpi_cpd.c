
/******************************************************************************
 * INCLUDES
 *****************************************************************************/
#include "../splatt_mpi.h"
#include "../mttkrp.h"
#include "../timer.h"
#include "../thd_info.h"
#include "../tile.h"

#include <math.h>
#include <omp.h>


/******************************************************************************
 * PRIVATE FUNCTIONS
 *****************************************************************************/

/**
* @brief Flush the updated values in globalmat to our local representation.
*
* @param localmat The local matrix to update.
* @param globalmat The recently updated global matrix.
* @param rinfo MPI rank information.
* @param nfactors The number of columns in the factor matrices.
* @param mode The mode we are operating on.
*/
static void __flush_glob_to_local(
  idx_t const * const indmap,
  matrix_t * const localmat,
  matrix_t const * const globalmat,
  rank_info const * const rinfo,
  idx_t const nfactors,
  idx_t const mode)
{
  idx_t const m = mode;

  idx_t const mat_start = rinfo->mat_start[m];
  idx_t const mat_end = rinfo->mat_end[m];
  idx_t const start = rinfo->ownstart[m];
  idx_t const nowned = rinfo->nowned[m];

  assert(start + nowned <= localmat->I);

  memcpy(localmat->vals + (start*nfactors),
         globalmat->vals,
         nowned * nfactors * sizeof(val_t));
}



/******************************************************************************
 * PUBLIC FUNCTIONS
 *****************************************************************************/

void mpi_send_rows(
  int const pdest,
  val_t * const nbr2globs_buf,
  matrix_t * const globalmat,
  rank_info * const rinfo,
  idx_t const nfactors,
  idx_t const mode)
{
  idx_t const m = mode;

  /* The number of rows to send to pdest */
  int const nsends = rinfo->nbr2globs_ptr[m][pdest] / nfactors;
  int const disp = rinfo->nbr2globs_disp[m][pdest] / nfactors;

  if(nsends == 0) {
    return;
  }

  idx_t const mat_start = rinfo->mat_start[m];
  idx_t const * const nbr2globs_inds = rinfo->nbr2globs_inds[m];
  val_t const * const gmatv = globalmat->vals;

  /* first prepare all rows that I own and need to send */
  #pragma omp parallel for
  for(int s=disp; s < disp+nsends; ++s) {
    idx_t const row = nbr2globs_inds[s] - mat_start;
    for(idx_t f=0; f < nfactors; ++f) {
      nbr2globs_buf[f+(s*nfactors)] = gmatv[f+(row*nfactors)];
    }
  }

  MPI_Isend(&(nbr2globs_buf[disp*nfactors]), nsends*nfactors, SS_MPI_VAL,
      pdest, 0, rinfo->layer_comm[m], &(rinfo->req));
}


void mpi_recv_rows(
  int const porig,
  idx_t const * const indmap,
  val_t * const nbr2local_buf,
  matrix_t * const localmat,
  matrix_t * const globalmat,
  rank_info * const rinfo,
  idx_t const nfactors,
  idx_t const mode)
{
  idx_t const m = mode;

  /* The number of rows to recv from porig */
  int const nrecvs = rinfo->local2nbr_ptr[m][porig] / nfactors;
  int const disp = rinfo->local2nbr_disp[m][porig] / nfactors;

  if(nrecvs == 0) {
    return;
  }

  MPI_Recv(&(nbr2local_buf[disp*nfactors]), nrecvs*nfactors, SS_MPI_VAL, porig,
      0, rinfo->layer_comm[m], &(rinfo->status));

  /* now write incoming nbr2locals to my local matrix */
  idx_t const * const local2nbr_inds = rinfo->local2nbr_inds[m];
  val_t * const matv = localmat->vals;
  #pragma omp for
  for(int r=disp; r < disp + nrecvs; ++r) {
    idx_t const row = local2nbr_inds[r];
    for(idx_t f=0; f < nfactors; ++f) {
      matv[f+(row*nfactors)] = nbr2local_buf[f+(r*nfactors)];
    }
  }

  /* ensure the local matrix is up to date too */
  __flush_glob_to_local(indmap, localmat, globalmat, rinfo, nfactors, m);
}



void mpi_update_rows(
  idx_t const * const indmap,
  val_t * const nbr2globs_buf,
  val_t * const nbr2local_buf,
  matrix_t * const localmat,
  matrix_t * const globalmat,
  rank_info const * const rinfo,
  idx_t const nfactors,
  idx_t const mode)
{
  timer_start(&timers[TIMER_MPI_UPDATE]);
  idx_t const m = mode;
  idx_t const mat_start = rinfo->mat_start[m];
  idx_t const * const nbr2globs_inds = rinfo->nbr2globs_inds[m];
  val_t const * const gmatv = globalmat->vals;

  #pragma omp parallel
  {

    /* first prepare all rows that I own and need to send */
    #pragma omp for
    for(idx_t s=0; s < rinfo->nnbr2globs[m]; ++s) {
      idx_t const row = nbr2globs_inds[s] - mat_start;
      for(idx_t f=0; f < nfactors; ++f) {
        nbr2globs_buf[f+(s*nfactors)] = gmatv[f+(row*nfactors)];
      }
    }

    #pragma omp master
    {
      /* grab ptr/disp from rinfo. nbr2local and local2nbr will have the same
       * structure so we just reuse those */
      int const * const restrict nbr2globs_ptr = rinfo->nbr2globs_ptr[m];
      int const * const restrict nbr2local_ptr = rinfo->local2nbr_ptr[m];
      int const * const restrict nbr2globs_disp = rinfo->nbr2globs_disp[m];
      int const * const restrict nbr2local_disp = rinfo->local2nbr_disp[m];

      timer_start(&timers[TIMER_MPI_IDLE]);
      MPI_Barrier(rinfo->layer_comm[m]);
      timer_stop(&timers[TIMER_MPI_IDLE]);

      /* exchange rows */
      timer_start(&timers[TIMER_MPI_COMM]);
      MPI_Alltoallv(nbr2globs_buf, nbr2globs_ptr, nbr2globs_disp, SS_MPI_VAL,
                    nbr2local_buf, nbr2local_ptr, nbr2local_disp, SS_MPI_VAL,
                    rinfo->layer_comm[m]);
      timer_stop(&timers[TIMER_MPI_COMM]);
    }

    #pragma omp barrier

    /* now write incoming nbr2locals to my local matrix */
    idx_t const * const local2nbr_inds = rinfo->local2nbr_inds[m];
    val_t * const matv = localmat->vals;
    #pragma omp for
    for(idx_t r=0; r < rinfo->nlocal2nbr[m]; ++r) {
      idx_t const row = local2nbr_inds[r];
      for(idx_t f=0; f < nfactors; ++f) {
        matv[f+(row*nfactors)] = nbr2local_buf[f+(r*nfactors)];
      }
    }

    /* ensure the local matrix is up to date too */
    #pragma omp master
    __flush_glob_to_local(indmap, localmat, globalmat, rinfo, nfactors, m);
  } /* end omp parallel */

  timer_stop(&timers[TIMER_MPI_UPDATE]);
}


void mpi_reduce_rows(
  val_t * const restrict local2nbr_buf,
  val_t * const restrict nbr2globs_buf,
  matrix_t const * const localmat,
  matrix_t * const globalmat,
  rank_info const * const rinfo,
  idx_t const nfactors,
  idx_t const mode)
{
  timer_start(&timers[TIMER_MPI_REDUCE]);
  idx_t const m = mode;

  val_t const * const restrict matv = localmat->vals;
  idx_t const * const restrict local2nbr_inds = rinfo->local2nbr_inds[m];

  /* copy my partial products into the sendbuf */
  #pragma omp parallel for
  for(idx_t s=0; s < rinfo->nlocal2nbr[m]; ++s) {
    idx_t const row = local2nbr_inds[s];
    for(idx_t f=0; f < nfactors; ++f) {
      local2nbr_buf[f + (s*nfactors)] = matv[f + (row*nfactors)];
    }
  }

  /* grab ptr/disp from rinfo. nbr2local and local2nbr will have the same
   * structure so we just reuse those */
  int const * const restrict nbr2globs_ptr = rinfo->nbr2globs_ptr[m];
  int const * const restrict nbr2local_ptr = rinfo->local2nbr_ptr[m];
  int const * const restrict nbr2globs_disp = rinfo->nbr2globs_disp[m];
  int const * const restrict nbr2local_disp = rinfo->local2nbr_disp[m];

  timer_start(&timers[TIMER_MPI_IDLE]);
  MPI_Barrier(rinfo->layer_comm[m]);
  timer_stop(&timers[TIMER_MPI_IDLE]);

  timer_start(&timers[TIMER_MPI_COMM]);
  /* exchange rows */
  MPI_Alltoallv(local2nbr_buf, nbr2local_ptr, nbr2local_disp, SS_MPI_VAL,
                nbr2globs_buf, nbr2globs_ptr, nbr2globs_disp, SS_MPI_VAL,
                rinfo->layer_comm[m]);
  timer_stop(&timers[TIMER_MPI_COMM]);


  /* now add received rows to globmats */
  idx_t const mat_start = rinfo->mat_start[m];
  idx_t const * const restrict nbr2globs_inds = rinfo->nbr2globs_inds[m];
  val_t * const restrict gmatv = globalmat->vals;
  for(idx_t r=0; r < rinfo->nnbr2globs[m]; ++r) {
    idx_t const row = nbr2globs_inds[r] - mat_start;
    for(idx_t f=0; f < nfactors; ++f) {
      gmatv[f+(row*nfactors)] += nbr2globs_buf[f+(r*nfactors)];
    }
  }
  timer_stop(&timers[TIMER_MPI_REDUCE]);
}


void mpi_add_my_partials(
  idx_t const * const indmap,
  matrix_t const * const localmat,
  matrix_t * const globmat,
  rank_info const * const rinfo,
  idx_t const nfactors,
  idx_t const mode)
{
  timer_start(&timers[TIMER_MPI_PARTIALS]);
  idx_t const m = mode;

  idx_t const mat_start = rinfo->mat_start[m];
  idx_t const mat_end = rinfo->mat_end[m];
  idx_t const start = rinfo->ownstart[m];
  idx_t const nowned = rinfo->nowned[m];

  memset(globmat->vals, 0, globmat->I * nfactors * sizeof(val_t));

  idx_t const goffset = (indmap == NULL) ?
      start - mat_start : indmap[start] - mat_start;

  memcpy(globmat->vals + (goffset * nfactors),
         localmat->vals + (start * nfactors),
         nowned * nfactors * sizeof(val_t));
  timer_stop(&timers[TIMER_MPI_PARTIALS]);
}


void mpi_time_stats(
  rank_info const * const rinfo)
{
  double max_mttkrp, avg_mttkrp;
  double max_mpi, avg_mpi;
  double max_idle, avg_idle;
  double max_com, avg_com;

  timers[TIMER_MPI].seconds =
      timers[TIMER_MPI_ATA].seconds
      + timers[TIMER_MPI_REDUCE].seconds
      + timers[TIMER_MPI_PARTIALS].seconds
      + timers[TIMER_MPI_NORM].seconds
      + timers[TIMER_MPI_UPDATE].seconds
      + timers[TIMER_MPI_FIT].seconds;

  /* get avg times */
  MPI_Reduce(&timers[TIMER_MTTKRP].seconds, &avg_mttkrp, 1, MPI_DOUBLE,
      MPI_SUM, 0, rinfo->comm_3d);
  MPI_Reduce(&timers[TIMER_MPI].seconds, &avg_mpi, 1, MPI_DOUBLE,
      MPI_SUM, 0, rinfo->comm_3d);
  MPI_Reduce(&timers[TIMER_MPI_IDLE].seconds, &avg_idle, 1, MPI_DOUBLE,
      MPI_SUM, 0, rinfo->comm_3d);
  MPI_Reduce(&timers[TIMER_MPI_COMM].seconds, &avg_com, 1, MPI_DOUBLE,
      MPI_SUM, 0, rinfo->comm_3d);

  /* get max times */
  MPI_Reduce(&timers[TIMER_MTTKRP].seconds, &max_mttkrp, 1, MPI_DOUBLE,
      MPI_MAX, 0, rinfo->comm_3d);
  MPI_Reduce(&timers[TIMER_MPI].seconds, &max_mpi, 1, MPI_DOUBLE,
      MPI_MAX, 0, rinfo->comm_3d);
  MPI_Reduce(&timers[TIMER_MPI_IDLE].seconds, &max_idle, 1, MPI_DOUBLE,
      MPI_MAX, 0, rinfo->comm_3d);
  MPI_Reduce(&timers[TIMER_MPI_COMM].seconds, &max_com, 1, MPI_DOUBLE,
      MPI_MAX, 0, rinfo->comm_3d);

  /* set avg times */
  timers[TIMER_MTTKRP].seconds   = avg_mttkrp / rinfo->npes;
  timers[TIMER_MPI].seconds      = avg_mpi    / rinfo->npes;
  timers[TIMER_MPI_IDLE].seconds = avg_idle   / rinfo->npes;
  timers[TIMER_MPI_COMM].seconds = avg_com    / rinfo->npes;
  /* set max times */
  timers[TIMER_MTTKRP_MAX].seconds   = max_mttkrp;
  timers[TIMER_MPI_MAX].seconds      = max_mpi;
  timers[TIMER_MPI_IDLE_MAX].seconds = max_idle;
  timers[TIMER_MPI_COMM_MAX].seconds = max_com;
}

