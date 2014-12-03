#ifndef SPLATT_IO_H
#define SPLATT_IO_H


/******************************************************************************
 * INCLUDES
 *****************************************************************************/
#include "sptensor.h"
#include "matrix.h"
#include "graph.h"


/******************************************************************************
 * PUBLIC FUNCTIONS
 *****************************************************************************/
sptensor_t * tt_read_file(
  char const * const fname);

void mat_write(
  matrix_t const * const mat,
  char const * const fname);

void tt_write_file(
  sptensor_t const * const tt,
  FILE * fout);
void tt_write(
  sptensor_t const * const tt,
  char const * const fname);

void hgraph_write_file(
  hgraph_t const * const hg,
  FILE * fout);
void hgraph_write(
  hgraph_t const * const hg,
  char const * const fname);

void spmat_write(
  spmatrix_t const * const mat,
  char const * const fname);
void spmat_write_file(
  spmatrix_t const * const mat,
  FILE * fout);

#endif