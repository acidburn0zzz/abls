/*

   BLIS
   An object-based framework for developing high-performance BLAS-like
   libraries.

   Copyright (C) 2022 - 2023, Advanced Micro Devices, Inc. All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:
    - Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    - Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    - Neither the name(s) of the copyright holder(s) nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include "blis.h"
#include "lpgemm_5loop_interface_apis.h"
#include "lpgemm_types.h"
#include "lpgemm_utils.h"
#include "lpgemm_thrinfo_utils.h"
#include "lpgemm_kernels.h"

// Kernel function prototypes
typedef void (*lpgemm_rowvar_f32)
     (
       const dim_t,
       const dim_t,
       const dim_t,
       const float*,
       const dim_t,
       const dim_t,
       const dim_t,
       const float*,
       const dim_t,
       const dim_t,
       float*,
       const dim_t,
       const dim_t,
       const float,
       const float,
       lpgemm_post_op*,
       lpgemm_post_op_attr
     );

void lpgemm_pack_a_f32f32f32of32
     (
       const float* input_buf_addr_a,
       float*       reorder_buf_addr_a,
       const dim_t  m,
       const dim_t  k,
       const dim_t  rs_a,
       const dim_t  cs_a,
       const dim_t  ps_p,
       const dim_t  MR,
       cntx_t*      cntx
     );

void lpgemm_pack_b_f32f32f32of32
     (
       const float* input_buf_addr_b,
       float*       reorder_buf_addr_b,
       const dim_t  n,
       const dim_t  k,
       const dim_t  rs_b,
       const dim_t  cs_b,
       const dim_t  ps_p,
       const dim_t  NR,
       cntx_t*      cntx
     );

LPGEMM_5LOOP(float,float,float,f32f32f32of32)
{
    // Query the global cntx.
    cntx_t* cntx = bli_gks_query_cntx();

    num_t dt = BLIS_FLOAT;

    // Query the context for various blocksizes.
    const dim_t NR = bli_cntx_get_l3_sup_blksz_def_dt( dt, BLIS_NR, cntx );
    const dim_t MR = bli_cntx_get_l3_sup_blksz_def_dt( dt, BLIS_MR, cntx );
    const dim_t NC = bli_cntx_get_l3_sup_blksz_def_dt( dt, BLIS_NC, cntx );
    const dim_t MC = bli_cntx_get_l3_sup_blksz_def_dt( dt, BLIS_MC, cntx );
    const dim_t KC = bli_cntx_get_l3_sup_blksz_def_dt( dt, BLIS_KC, cntx );

    /*ToDo: Based on context kernel 6x64m or 6x16m will be picked here */    
    
    // Strides are updated based on matrix packing/reordering.
    const float* a_use = NULL;
    dim_t rs_a_use = rs_a;
    dim_t cs_a_use = cs_a;

    const float* b_use = NULL;
    dim_t rs_b_use = rs_b;
    dim_t cs_b_use = cs_b;

    float* c_use_jc = NULL;
    float* c_use_ic = NULL;

    dim_t rs_c_downscale = rs_c;

    // Only supporting row major with unit column strided C for now.
    const dim_t cs_c_use = 1;

    /* Compute partitioning step values for each matrix of each loop. */
    inc_t ps_a_use;
    inc_t ps_b_use;
    auxinfo_t aux;

    // Check if packing of A is required.
    bool should_pack_A = bli_rntm_pack_a( rntm );

    // Pack buffer for A.
    float* pack_a_buffer_f32f32f32of32;
    mem_t mem_a = BLIS_MEM_INITIALIZER;
    siz_t mem_a_size_req = 0;

    // Check if packing of A is required.
    bool should_pack_B = bli_rntm_pack_b( rntm );

    // Pack buffer for B.
    float* pack_b_buffer_f32f32f32of32;
    mem_t mem_b = BLIS_MEM_INITIALIZER;
    siz_t mem_b_size_req = 0;

    float one_local = *PASTEMAC(s,1);

    // To decide whether to apply post ops or not.
    bool is_last_k = FALSE;

    // To decide whether to use original s8 C or temp buffer for beta scale.
    bool is_first_k = FALSE;

    lpgemm_post_op_attr post_ops_attr;
    if ( c_downscale == TRUE )
    {
        post_ops_attr.buf_downscale = c;
    }
    else
    {
        post_ops_attr.buf_downscale = NULL;
    }

    // Generate thrinfo objects for jc and ic loops from lpgemm_thrinfo_t.
    thrinfo_t thread_jc;
    thrinfo_t thread_ic;

    lpgemm_gen_thrinfo( thread, &thread_jc, &thread_ic );

    // Compute the JC loop thread range for the current thread.
    dim_t jc_start, jc_end;
    bli_thread_range_sub( &thread_jc, n, NR, FALSE, &jc_start, &jc_end );

    for ( dim_t jc = jc_start; jc < jc_end; jc += NC )
    {
        dim_t nc0 = bli_min( ( jc_end - jc ), NC );
        c_use_jc = c + jc;

        dim_t jc_cur_loop = jc;
        dim_t jc_cur_loop_rem = 0;
        dim_t n_sub_updated = 0;

        if ( mtag_b == REORDERED )
        {
            get_B_panel_reordered_start_offset_width
            (
              jc, n, NC, NR,
              &jc_cur_loop, &jc_cur_loop_rem,
              &nc0, &n_sub_updated
            );
        }

        for ( dim_t pc = 0; pc < k; pc += KC )
        {
            float beta0 = ( pc == 0 ) ? beta : one_local;
            dim_t kc0 = bli_min( ( k - pc ), KC );

            // No parallelization in k dim, k always starts at 0.
            is_first_k = ( pc == 0 ) ? ( TRUE ) : ( FALSE );
            post_ops_attr.is_first_k = is_first_k;

            is_last_k = ( ( pc + KC ) >= k ) ? ( TRUE ) : ( FALSE );
            post_ops_attr.is_last_k = is_last_k;

            if ( ( mtag_b == PACK ) && ( should_pack_B == TRUE ) )
            {
                // Pack B chunks are based on jc work id.
                dim_t jc_work_id = bli_thread_work_id( &thread_jc );

                // Using child thrinfo (thread_ic) tid to decide chief thread
                // per B matrix chunk (jc work id group)
                if ( bli_thread_am_ochief( &thread_ic ) )
                {
                    // nc0 needs to be a multiple of 16 since this gives maximum
                    // vectorization. Packing B always results in buffers with width
                    // which is a multiple of 16. Subsequently the nc0 offsets used
                    // for packed/reordered buffers needs to be updated.
                    dim_t nc0_updated = make_multiple_of_n( nc0, NR );
                    mem_b_size_req = sizeof( float ) * nc0_updated * kc0;

                    lpgemm_alloc_mem_panel
                    (
                      mem_b_size_req, BLIS_BUFFER_FOR_B_PANEL,
                      &mem_b, rntm
                    );

                    thread->comm[jc_work_id].sent_object = bli_mem_buffer(&mem_b);
                }

                // All threads in work group should wait till chief thread has
                // finished allocating the packing buffers.
                bli_thrcomm_barrier
                (
                  bli_thread_ocomm_id( &thread_ic ),
                  &thread->comm[jc_work_id]
                );

                pack_b_buffer_f32f32f32of32 =
                              ( float* ) thread->comm[jc_work_id].sent_object;
                // Set the strides for pack buffer.
                rs_b_use = NR;
                cs_b_use = 1;
                ps_b_use = kc0;

                // Compute the B panel per thread loop range for parallel
                // packing using ic_ways number of threads. Since atmost only
                // ic_ways threads can be used, the thread_ic attributes are
                // used to split the loop range.
                dim_t jc_packb_start, jc_packb_end;
                bli_thread_range_sub
                (
                  &thread_ic, nc0, NR, FALSE,
                  &jc_packb_start, &jc_packb_end
                );

                // Ensure thread ranges are valid, especially cases where no:
                // of threads available for parallelization are greater than
                // no: of B panel NR chunks.
                if ( ( jc_packb_end > jc_packb_start ) &&
                     ( jc_packb_start < ( jc + nc0 ) ) )
                {
                    lpgemm_pack_b_f32f32f32of32
                    (
                      ( b + ( rs_b * pc ) + ( cs_b * jc ) + ( cs_b * jc_packb_start ) ),
                      pack_b_buffer_f32f32f32of32 + ( jc_packb_start * kc0 ),
                      ( jc_packb_end - jc_packb_start ), kc0,
                      rs_b, cs_b, ( NR * ps_b_use ), NR,
                      cntx
                    );
                }

                // All threads in work group should wait till B matrix packing
                // is completed by the participating threads.
                bli_thrcomm_barrier
                (
                  bli_thread_ocomm_id( &thread_ic ),
                  &thread->comm[jc_work_id]
                );
                b_use = pack_b_buffer_f32f32f32of32;
            }
            else if ( mtag_b == REORDERED )
            {
                // In multi-threaded scenarios, an extra offset into a given
                // packed B panel is required, since the jc loop split can
                // result in per thread start offset inside the panel, instead
                // of panel boundaries.
                b_use = b + ( jc_cur_loop * k ) +
                        ( n_sub_updated * pc ) + ( jc_cur_loop_rem * kc0 );

                rs_b_use = NR;
                cs_b_use = 1;
                ps_b_use = kc0;
            }
            else
            {
                b_use = b + ( pc * rs_b ) + ( jc * cs_b );
                ps_b_use = 1;
            }

            dim_t ic_start, ic_end;
            bli_thread_range_sub( &thread_ic, m, MR, FALSE, &ic_start, &ic_end );

            for ( dim_t ic = ic_start; ic < ic_end; ic += MC )
            {
                dim_t mc0 = bli_min( ( ic_end - ic ), MC );
                c_use_ic = c_use_jc + ( rs_c * ic );

                if ( mtag_a == REORDERED )
                {
                    // Extra space since packing does width in multiples of MR.
                    const dim_t m_updated = ( ( m + MR - 1 ) / MR ) * MR;
                    a_use = a + ( pc * m_updated ) + ( kc0 * ic );

                    rs_a_use = 1;
                    cs_a_use = MR;
                    ps_a_use = MR * kc0;
                }
                else if ( should_pack_A == TRUE )
                {
                    // Extra space since packing does width in multiples of MR.
                    const dim_t mc0_updated = ( ( mc0 + MR - 1 ) / MR ) * MR;
                    mem_a_size_req = sizeof( float ) * mc0_updated * kc0;

                    lpgemm_alloc_mem_panel
                    (
                      mem_a_size_req, BLIS_BUFFER_FOR_A_BLOCK,
                      &mem_a, rntm
                    );
                    pack_a_buffer_f32f32f32of32 = ( float* )bli_mem_buffer( &mem_a );

                    rs_a_use = 1;
                    cs_a_use = MR;
                    ps_a_use = MR * kc0;

                    lpgemm_pack_a_f32f32f32of32
                    (
                      ( a + ( rs_a * ic ) + pc ),
                      pack_a_buffer_f32f32f32of32,
                      mc0, kc0,
                      rs_a, cs_a, ps_a_use, MR,
                      cntx
                    );

                    a_use = pack_a_buffer_f32f32f32of32;
                }
                else
                {
                    a_use = a + ( rs_a * ic ) + pc;
                    ps_a_use = MR * rs_a;
                }

                // Embed the panel stride of A within the auxinfo_t object. The
                // millikernel will query and use this to iterate through
                // micropanels of A (if needed).
                bli_auxinfo_set_ps_a( ps_a_use, &aux );

                for ( dim_t jr = 0; jr < nc0; jr += NR )
                {
                    dim_t nr0 = bli_min( ( nc0 - jr ), NR );

                    // Post ops meta attributes.
                    post_ops_attr.post_op_c_i = ic;
                    post_ops_attr.post_op_c_j = ( jc + jr );
                    post_ops_attr.rs_c_downscale = rs_c_downscale;

                    // Reordered/unpacked B, reordered/unpacked A.
                    ( ( lpgemm_rowvar_f32 )lcntx->kern_fun_ptr )
                    (
                      mc0, nr0, kc0,
                      ( float* )a_use, rs_a_use, cs_a_use, ps_a_use,
                      ( float* )( b_use + ( jr * ps_b_use ) ), rs_b_use, cs_b_use,
                      ( c_use_ic + jr ), rs_c, cs_c_use,
                      alpha , beta0,
                      post_op_list, post_ops_attr
                    );
                }
            }
        }
        if ( mtag_b == REORDERED )
        {
            adjust_B_panel_reordered_jc( &jc, jc_cur_loop );
        }
    }

    // Release pack buffers.
    if ( mtag_b == PACK )
    {
        // All threads in work group should wait till B matrix usage is
        // completed by the participating threads.
        bli_thrcomm_barrier
        (
          bli_thread_ocomm_id( &thread_jc ),
          &thread->comm[bli_thread_work_id( &thread_jc)]
        );

        if ( bli_thread_am_ochief( &thread_ic ) )
        {
            if ( bli_mem_is_alloc( &mem_b ) )
            {
                bli_membrk_release( rntm, &mem_b );
            }
        }
    }
    if ( should_pack_A == TRUE )
    {
        if ( bli_mem_is_alloc( &mem_a ) )
        {
            bli_membrk_release( rntm, &mem_a );
        }
    }
}

void lpgemm_pack_a_f32f32f32of32
     (
       const float* input_buf_addr_a,
       float*       reorder_buf_addr_a,
       const dim_t  m,
       const dim_t  k,
       const dim_t  rs_a,
       const dim_t  cs_a,
       const dim_t  ps_p,
       const dim_t  MR,
       cntx_t*      cntx
     )
{
    float one_local  = *PASTEMAC(s,1);
    float* restrict kappa_cast = &one_local;

    // Set the schema to "column stored row panels" to indicate packing to conventional
    // column-stored row panels.
    pack_t schema = BLIS_PACKED_ROW_PANELS;
    trans_t transc = BLIS_NO_TRANSPOSE;
    conj_t conjc = bli_extract_conj( transc );
    // Compute the total number of iterations we'll need.
    dim_t m_iter = ( m + MR - 1 ) / MR;

    inc_t cs_p = MR;

    float* p_temp = reorder_buf_addr_a;

    dim_t ir, it;
    // Iterate over every logical micropanel in the source matrix.
    for ( ir = 0, it = 0; it < m_iter; ir += MR, it += 1 )
    {
        dim_t panel_dim_i = bli_min( MR, m - ir );

        const float* a_use = input_buf_addr_a + ( ir * rs_a );
        float* p_use = p_temp;

        PASTEMAC(s,packm_cxk)
        (
          conjc,
          schema,
          panel_dim_i,
          MR,
          k,
          k,
          kappa_cast,
          ( float* )a_use, rs_a, cs_a,
          p_use, cs_p,
          cntx
        );

        p_temp += ps_p;
    }
}

void lpgemm_pack_b_f32f32f32of32
     (
       const float* input_buf_addr_b,
       float*       reorder_buf_addr_b,
       const dim_t  n,
       const dim_t  k,
       const dim_t  rs_b,
       const dim_t  cs_b,
       const dim_t  ps_p,
       const dim_t  NR,
       cntx_t*      cntx
     )
{
    float one_local  = *PASTEMAC(s,1);
    float* restrict kappa_cast = &one_local;

    // Set the schema to "row stored column panels" to indicate packing to
    // conventional row-stored column panels.
    pack_t schema = BLIS_PACKED_COL_PANELS;
    trans_t transc = BLIS_NO_TRANSPOSE;
    conj_t conjc = bli_extract_conj( transc );
    // Compute the total number of iterations we'll need.
    dim_t n_iter = ( n + NR - 1 ) / NR;

    inc_t rs_p = NR;

    float* p_temp = reorder_buf_addr_b;

    dim_t jr, it;
    // Iterate over every logical micropanel in the source matrix.
    for ( jr = 0, it = 0; it < n_iter; jr += NR, it += 1 )
    {
        dim_t panel_dim_i = bli_min( NR, n - jr );

        const float* b_use = input_buf_addr_b + ( jr * cs_b );
        float* p_use = p_temp;

        PASTEMAC(s,packm_cxk)
        (
          conjc,
          schema,
          panel_dim_i,
          NR,
          k,
          k,
          kappa_cast,
          ( float* )b_use, cs_b, rs_b,
          p_use, rs_p,
          cntx
        );

        p_temp += ps_p;
    }
}
