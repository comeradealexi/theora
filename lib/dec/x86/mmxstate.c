/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE OggTheora SOFTWARE CODEC SOURCE CODE.   *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE Theora SOURCE CODE IS COPYRIGHT (C) 2002-2007                *
 * by the Xiph.Org Foundation and contributors http://www.xiph.org/ *
 *                                                                  *
 ********************************************************************

  function:
    last mod: $Id$

 ********************************************************************/

/*MMX acceleration of complete fragment reconstruction algorithm.
  Originally written by Rudolf Marek.*/
#include <string.h>
#include "x86int.h"
#include "mmxfrag.h"
#include "mmxloop.h"

#if defined(OC_X86_ASM)

void oc_state_frag_recon_mmx(const oc_theora_state *_state,ptrdiff_t _fragi,
 int _pli,ogg_int16_t _dct_coeffs[64],int _last_zzi,int _ncoefs,
 ogg_uint16_t _dc_quant,const ogg_uint16_t _ac_quant[64]){
  ogg_int16_t  OC_ALIGN8  res_buf[64];
  unsigned char          *dst;
  ptrdiff_t               frag_buf_off;
  int                     ystride;
  int                     mb_mode;
  /*Dequantize and apply the inverse transform.*/
  oc_dequant_idct8x8_mmx(res_buf,_dct_coeffs,
   _last_zzi,_ncoefs,_dc_quant,_ac_quant);
  /*Fill in the target buffer.*/
  frag_buf_off=_state->frag_buf_offs[_fragi];
  mb_mode=_state->frags[_fragi].mb_mode;
  ystride=_state->ref_ystride[_pli];
  dst=_state->ref_frame_data[_state->ref_frame_idx[OC_FRAME_SELF]]+frag_buf_off;
  if(mb_mode==OC_MODE_INTRA)oc_frag_recon_intra_mmx(dst,ystride,res_buf);
  else{
    const unsigned char *ref;
    int                  mvoffsets[2];
    ref=
     _state->ref_frame_data[_state->ref_frame_idx[OC_FRAME_FOR_MODE[mb_mode]]]
     +frag_buf_off;
    if(oc_state_get_mv_offsets(_state,mvoffsets,_pli,
     _state->frag_mvs[_fragi][0],_state->frag_mvs[_fragi][1])>1){
      oc_frag_recon_inter2_mmx(dst,ref+mvoffsets[0],ref+mvoffsets[1],ystride,
       res_buf);
    }
    else oc_frag_recon_inter_mmx(dst,ref+mvoffsets[0],ystride,res_buf);
  }
}

/*We copy these entire function to inline the actual MMX routines so that we
   use only a single indirect call.*/

/*Copies the fragments specified by the lists of fragment indices from one
   frame to another.
  _fragis:    A pointer to a list of fragment indices.
  _nfragis:   The number of fragment indices to copy.
  _dst_frame: The reference frame to copy to.
  _src_frame: The reference frame to copy from.
  _pli:       The color plane the fragments lie in.*/
void oc_state_frag_copy_list_mmx(const oc_theora_state *_state,
 const ptrdiff_t *_fragis,ptrdiff_t _nfragis,
 int _dst_frame,int _src_frame,int _pli){
  const ptrdiff_t     *frag_buf_offs;
  const unsigned char *src_frame_data;
  unsigned char       *dst_frame_data;
  ptrdiff_t            fragii;
  int                  ystride;
  dst_frame_data=_state->ref_frame_data[_state->ref_frame_idx[_dst_frame]];
  src_frame_data=_state->ref_frame_data[_state->ref_frame_idx[_src_frame]];
  ystride=_state->ref_frame_bufs[0][_pli].stride;
  frag_buf_offs=_state->frag_buf_offs;
  for(fragii=0;fragii<_nfragis;fragii++){
    ptrdiff_t frag_buf_off;
    frag_buf_off=frag_buf_offs[_fragis[fragii]];
    OC_FRAG_COPY_MMX(dst_frame_data+frag_buf_off,
     src_frame_data+frag_buf_off,ystride);
  }
}

/*Apply the loop filter to a given set of fragment rows in the given plane.
  The filter may be run on the bottom edge, affecting pixels in the next row of
   fragments, so this row also needs to be available.
  _bv:        The bounding values array.
  _refi:      The index of the frame buffer to filter.
  _pli:       The color plane to filter.
  _fragy0:    The Y coordinate of the first fragment row to filter.
  _fragy_end: The Y coordinate of the fragment row to stop filtering at.*/
void oc_state_loop_filter_frag_rows_mmx(const oc_theora_state *_state,
 int _bv[256],int _refi,int _pli,int _fragy0,int _fragy_end){
  unsigned char OC_ALIGN8  ll[8];
  const oc_fragment_plane *fplane;
  const oc_fragment       *frags;
  const ptrdiff_t         *frag_buf_offs;
  unsigned char           *ref_frame_data;
  ptrdiff_t                fragi_top;
  ptrdiff_t                fragi_bot;
  ptrdiff_t                fragi0;
  ptrdiff_t                fragi0_end;
  int                      ystride;
  int                      nhfrags;
  memset(ll,_state->loop_filter_limits[_state->qis[0]],sizeof(ll));
  fplane=_state->fplanes+_pli;
  nhfrags=fplane->nhfrags;
  fragi_top=fplane->froffset;
  fragi_bot=fragi_top+fplane->nfrags;
  fragi0=fragi_top+_fragy0*(ptrdiff_t)nhfrags;
  fragi0_end=fragi0+(_fragy_end-_fragy0)*(ptrdiff_t)nhfrags;
  ystride=_state->ref_ystride[_pli];
  frags=_state->frags;
  frag_buf_offs=_state->frag_buf_offs;
  ref_frame_data=_state->ref_frame_data[_refi];
  /*The following loops are constructed somewhat non-intuitively on purpose.
    The main idea is: if a block boundary has at least one coded fragment on
     it, the filter is applied to it.
    However, the order that the filters are applied in matters, and VP3 chose
     the somewhat strange ordering used below.*/
  while(fragi0<fragi0_end){
    ptrdiff_t fragi;
    ptrdiff_t fragi_end;
    fragi=fragi0;
    fragi_end=fragi+nhfrags;
    while(fragi<fragi_end){
      if(frags[fragi].coded){
        unsigned char *ref;
        ref=ref_frame_data+frag_buf_offs[fragi];
        if(fragi>fragi0)OC_LOOP_FILTER_H_MMX(ref,ystride,ll);
        if(fragi0>fragi_top)OC_LOOP_FILTER_V_MMX(ref,ystride,ll);
        if(fragi+1<fragi_end&&!frags[fragi+1].coded){
          OC_LOOP_FILTER_H_MMX(ref+8,ystride,ll);
        }
        if(fragi+nhfrags<fragi_bot&&!frags[fragi+nhfrags].coded){
          OC_LOOP_FILTER_V_MMX(ref+(ystride<<3),ystride,ll);
        }
      }
      fragi++;
    }
    fragi0+=nhfrags;
  }
}

#endif
