#include "blis.h"
#ifdef BLIS_ENABLE_CBLAS
/*
 * cblas_dscal.c
 *
 * The program is a C interface to dscal.
 *
 * Written by Keita Teranishi.  2/11/1998
 *
 *
 * Copyright (C) 2021, Advanced Micro Devices, Inc. All rights reserved.
 */
#include "cblas.h"
#include "cblas_f77.h"
void cblas_dscal( f77_int N, double alpha, double *X,
                       f77_int incX)
{
#ifdef F77_INT
   F77_INT F77_N=N, F77_incX=incX;
#else
   #define F77_N N
   #define F77_incX incX
   #define F77_incY incY
#endif

   F77_dscal( &F77_N, &alpha, X, &F77_incX);
}
#endif

