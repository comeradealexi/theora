/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE OggTheora SOFTWARE CODEC SOURCE CODE.   *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE Theora SOURCE CODE IS COPYRIGHT (C) 2002-2007                *
 * by the Xiph.Org Foundation http://www.xiph.org/                  *
 *                                                                  *
 ********************************************************************

  function:
  last mod: $Id$

 ********************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include "toplevel_lookup.h"
#include "../internal.h"
#include "dsp.h"
#include "codec_internal.h"

static int _ilog(unsigned int v){
  int ret=0;
  while(v){
    ret++;
    v>>=1;
  }
  return(ret);
}

static int oc_log2frac(ogg_uint32_t _val,int _frac_bits){
  int l;
  l=_ilog(_val);
  if(l>16)_val>>=l-16;
  else _val<<=16-l;
  l--;
  while(_frac_bits-->0){
    int b;
    _val=_val*_val>>15;
    b=(int)(_val>>16);
    l=l<<1|b;
    _val>>=b;
  }
  return l;
}

static ogg_uint16_t oc_exp2(int _log){
  int          ipart;
  ogg_uint32_t fpart;
  ipart=_log>>12;
  if(ipart>15)return 0xFFFF;
  else if(ipart<0)return 0;
  fpart=_log-(ipart<<12)<<3;
  /*3rd order polynomial approximation in Q15:
     (((3*log(2)-2)*f+3-4*log(2))*f+log(2))*f+1*/
  fpart=(fpart*((fpart*((fpart*2603>>15)+7452)>>15)+22713)>>15)+32768;
  if(ipart<15)fpart+=1<<14-ipart;
  return fpart>>15-ipart;
}

static void oc_enc_calc_lambda(CP_INSTANCE *cpi){
  int l;
  /*For now, lambda is fixed depending on the qi value and frame type:
      lambda=1.125*(qavg[qti][qi]**1.5)
    A more adaptive scheme might perform better, but Theora's behavior does not
     seem to conform to existing models in the literature.*/
  l=oc_log2frac(cpi->qavg[cpi->FrameType!=KEY_FRAME][cpi->BaseQ],12)-(3<<12);
  l=oc_exp2(l+(l>>1));
  cpi->lambda=l+(l>>3);
}



static void oc_rc_state_init(oc_rc_state *_rc,const theora_info *_info){
  unsigned long npixels;
  unsigned long ibpp;
  /*TODO: These parameters should be exposed in a th_enc_ctl() API.*/
  _rc->bits_per_frame=(_info->target_bitrate*
   (ogg_int64_t)_info->fps_denominator+(_info->fps_numerator>>1))/
   _info->fps_numerator;
  /*Insane framerates or frame sizes mean insane bitrates.
    Let's not get carried away.*/
  if(_rc->bits_per_frame>0x40000000000000LL){
    _rc->bits_per_frame=(ogg_int64_t)0x40000000000000LL;
  }
  else if(_rc->bits_per_frame<32)_rc->bits_per_frame=32;
  /*The buffer size is set equal to the keyframe interval, clamped to the range
     [8,256] frames.
    The 8 frame minimum gives us some chance to distribute bit estimation
     errors.
    The 256 frame maximum means we'll require 8-10 seconds of pre-buffering at
     24-30 fps, which is not unreasonable.*/
  _rc->buf_delay=_info->keyframe_frequency_force>256?
   256:_info->keyframe_frequency_force;
  _rc->buf_delay=OC_MAXI(_rc->buf_delay,12);
  _rc->max=_rc->bits_per_frame*_rc->buf_delay;
  /*Start with a buffer fullness of 75%.
    We can require fully half the buffer for a keyframe, and so this initial
     level gives us maximum flexibility for over/under-shooting in subsequent
     frames.*/
  _rc->target=_rc->fullness=(_rc->max+1>>1)+(_rc->max+2>>2);
  /*Pick exponents and initial scales for quantizer selection.
    TODO: These still need to be tuned.*/
  npixels=_info->width*(unsigned long)_info->height;
  ibpp=(npixels+(_rc->bits_per_frame>>1))/_rc->bits_per_frame;
  if(ibpp<10){
    _rc->exp[0]=48;
    _rc->scale[0]=2199;
    _rc->exp[1]=77;
    _rc->scale[1]=2500;
  }
  else if(ibpp<20){
    _rc->exp[0]=51;
    _rc->scale[0]=1781;
    _rc->exp[1]=90;
    _rc->scale[1]=1700;
  }
  else{
    _rc->exp[0]=54;
    _rc->scale[0]=870;
    _rc->exp[1]=102;
    _rc->scale[1]=1300;
  }
}

static unsigned OC_RATE_SMOOTHING[2]={0x80,0x80};

/*TODO: Convert the following entirely to fixed point.*/

static void oc_enc_update_rc_state(CP_INSTANCE *cpi,
 long _bits,int _qti,int _qi,int _trial){
  unsigned      scale;
  unsigned long npixels;
  /*Compute the estimated scale factor for this frame type.*/
  npixels=cpi->info.width*(unsigned long)cpi->info.height;
  scale=(int)(256.0*_bits/(npixels*
   pow(cpi->qavg[_qti][_qi]/32.0,cpi->rc.exp[_qti]/-64.0))+0.5);
  /*Use it to set that factor directly if this was a trial.*/
  if(_trial)cpi->rc.scale[_qti]=scale;
  /*Otherwise update an exponential moving average.*/
  else{
    cpi->rc.scale[_qti]=(scale<<16)
     +(cpi->rc.scale[_qti]-scale)*OC_RATE_SMOOTHING[_qti]>>16;
  }
  /*Update the buffer fullness level.*/
  if(!_trial)cpi->rc.fullness+=cpi->rc.bits_per_frame-_bits;
}

static int oc_enc_select_qi(CP_INSTANCE *cpi,int _qti,int _trial){
  ogg_int64_t  rate_total;
  ogg_uint32_t next_key_frame;
  int          nframes[2];
  int          buf_delay;
  /*Figure out how to re-distribute bits so that we hit our fullness target
     before the last keyframe in our current buffer window (after the current
     frame), or the end of the buffer window, whichever comes first.*/
  next_key_frame=_qti?cpi->info.keyframe_frequency_force-cpi->LastKeyFrame:0;
  nframes[0]=(cpi->rc.buf_delay-OC_MINI(next_key_frame,cpi->rc.buf_delay)
   +cpi->info.keyframe_frequency_force-1)/cpi->info.keyframe_frequency_force;
  if(nframes[0]+_qti>1){
    buf_delay=next_key_frame+(nframes[0]-1)*cpi->info.keyframe_frequency_force;
    nframes[0]--;
  }
  else buf_delay=cpi->rc.buf_delay;
  nframes[1]=buf_delay-nframes[0];
  rate_total=cpi->rc.fullness-cpi->rc.target
   +buf_delay*cpi->rc.bits_per_frame;
  /*If there aren't enough bits to achieve our desired fullness level, use the
     minimum quality permitted.*/
  if(rate_total<=0)return cpi->info.quality;
  else{
    static const double KEY_RATIO[2]={0.53125,1.0};
    unsigned long npixels;
    double        prevr;
    double        curr;
    int           qtarget;
    int           best_qi;
    int           best_qdiff;
    int           qi;
    int           i;
    npixels=cpi->info.width*(unsigned long)cpi->info.height;
    curr=rate_total/(double)buf_delay;
    for(i=0;i<10;i++){
      double rdiff;
      double rderiv;
      double exp;
      double rpow;
      prevr=curr;
      exp=cpi->rc.exp[1-_qti]/(double)cpi->rc.exp[_qti];
      rpow=pow(prevr*256.0/(npixels*(double)cpi->rc.scale[_qti]),exp);
      rdiff=(nframes[_qti]*KEY_RATIO[_qti])*prevr
       +nframes[1-_qti]*KEY_RATIO[1-_qti]*cpi->rc.scale[1-_qti]/256.0*npixels*
       rpow-rate_total;
      rderiv=nframes[_qti]*KEY_RATIO[_qti]+
       (nframes[1-_qti]*KEY_RATIO[1-_qti]*cpi->rc.scale[1-_qti]/256.0*npixels*
       rpow)*(exp/prevr);
      curr=prevr-rdiff/rderiv;
      if(curr<=0||KEY_RATIO[_qti]*curr>rate_total||fabs(prevr-curr)<1)break;
    }
    qtarget=(int)(32*pow(KEY_RATIO[_qti]*curr*256/
     (npixels*(double)cpi->rc.scale[_qti]),-64.0/cpi->rc.exp[_qti])+0.5);
    /*If this was not one of the initial frames, limit a change in quality.*/
    if(!_trial){
      int qmin;
      int qmax;
      qmin=cpi->qavg[_qti][cpi->BaseQ]*13>>4;
      qmax=cpi->qavg[_qti][cpi->BaseQ]*5>>2;
      qtarget=OC_CLAMPI(qmin,qtarget,qmax);
    }
    /*Search for the quantizer that matches the target most closely.
      We don't assume a linear ordering, but when there are ties we do pick the
       quantizer closest to the current one.*/
    best_qi=cpi->info.quality;
    best_qdiff=abs(cpi->qavg[_qti][best_qi]-qtarget);
    for(qi=cpi->info.quality+1;qi<64;qi++){
      int qdiff;
      qdiff=abs(cpi->qavg[_qti][qi]-qtarget);
      if(qdiff<best_qdiff||
       qdiff==best_qdiff&&abs(qi-cpi->BaseQ)<abs(best_qi-cpi->BaseQ)){
        best_qi=qi;
        best_qdiff=qdiff;
      }
    }
    return best_qi;
  }
}



static void CompressKeyFrame(CP_INSTANCE *cpi, int recode){
  oggpackB_reset(cpi->oggbuffer);
  cpi->FrameType = KEY_FRAME;
  if(cpi->info.target_bitrate>0){
    cpi->BaseQ=oc_enc_select_qi(cpi,0,cpi->CurrentFrame==1);
  }
  oc_enc_calc_lambda(cpi);
  cpi->LastKeyFrame = 0;

  /* mark as video frame */
  oggpackB_write(cpi->oggbuffer,0,1);

  WriteFrameHeader(cpi);
  PickModes(cpi,recode);
  EncodeData(cpi);

  cpi->LastKeyFrame = 1;
}

static int CompressFrame( CP_INSTANCE *cpi, int recode ) {
  oggpackB_reset(cpi->oggbuffer);
  cpi->FrameType = DELTA_FRAME;
  if(cpi->info.target_bitrate>0){
    cpi->BaseQ=oc_enc_select_qi(cpi,1,0);
  }
  oc_enc_calc_lambda(cpi);

  /* mark as video frame */
  oggpackB_write(cpi->oggbuffer,0,1);

  WriteFrameHeader(cpi);
  if(PickModes(cpi,recode)){
    /* mode analysis thinks this should have been a keyframe; start over and code as a keyframe instead */

    oggpackB_reset(cpi->oggbuffer);
    cpi->FrameType = KEY_FRAME;
    if(cpi->info.target_bitrate>0)cpi->BaseQ=oc_enc_select_qi(cpi,0,0);
    oc_enc_calc_lambda(cpi);
    cpi->LastKeyFrame = 0;

    /* mark as video frame */
    oggpackB_write(cpi->oggbuffer,0,1);

    WriteFrameHeader(cpi);

    PickModes(cpi,1);
    EncodeData(cpi);

    cpi->LastKeyFrame = 1;

    return 0;
  }

  if(cpi->first_inter_frame == 0){
    cpi->first_inter_frame = 1;
    EncodeData(cpi);
    oc_enc_update_rc_state(cpi,oggpackB_bytes(cpi->oggbuffer)<<3,
     1,cpi->BaseQ,1);
    CompressFrame(cpi,1);
    return 0;
  }

  cpi->LastKeyFrame++;
  EncodeData(cpi);

  return 0;
}

/********************** The toplevel: encode ***********************/

static void theora_encode_dispatch_init(CP_INSTANCE *cpi);

int theora_encode_init(theora_state *th, theora_info *c){
  CP_INSTANCE *cpi;

  memset(th, 0, sizeof(*th));
  /*Currently only the 4:2:0 format is supported.*/
  if(c->pixelformat!=OC_PF_420)return OC_IMPL;
  th->internal_encode=cpi=_ogg_calloc(1,sizeof(*cpi));
  theora_encode_dispatch_init(cpi);
  oc_mode_scheme_chooser_init(&cpi->chooser);

  dsp_static_init (&cpi->dsp);

  c->version_major=TH_VERSION_MAJOR;
  c->version_minor=TH_VERSION_MINOR;
  c->version_subminor=TH_VERSION_SUB;

  if(c->quality>63)c->quality=63;
  if(c->quality<0)c->quality=32;
  if(c->target_bitrate<0)c->target_bitrate=0;
  cpi->BaseQ = c->quality;

  /* Set encoder flags. */
  /* if not AutoKeyframing cpi->ForceKeyFrameEvery = is frequency */
  if(!c->keyframe_auto_p)
    c->keyframe_frequency_force = c->keyframe_frequency;

  /* Set the frame rate variables. */
  if ( c->fps_numerator < 1 )
    c->fps_numerator = 1;
  if ( c->fps_denominator < 1 )
    c->fps_denominator = 1;

  /* don't go too nuts on keyframe spacing; impose a high limit to
     make certain the granulepos encoding strategy works */
  if(c->keyframe_frequency_force>32768)c->keyframe_frequency_force=32768;
  if(c->keyframe_mindistance>32768)c->keyframe_mindistance=32768;
  if(c->keyframe_mindistance>c->keyframe_frequency_force)
    c->keyframe_mindistance=c->keyframe_frequency_force;
  cpi->keyframe_granule_shift=_ilog(c->keyframe_frequency_force-1);

  /* clamp the target_bitrate to a maximum of 24 bits so we get a
     more meaningful value when we write this out in the header. */
  if(c->target_bitrate>(1<<24)-1)c->target_bitrate=(1<<24)-1;

  /* copy in config */
  memcpy(&cpi->info,c,sizeof(*c));
  th->i=&cpi->info;
  th->granulepos=-1;

  /* Set up an encode buffer */
  cpi->oggbuffer = _ogg_malloc(sizeof(oggpack_buffer));
  oggpackB_writeinit(cpi->oggbuffer);

  InitFrameInfo(cpi);

  /* Initialise the compression process. */
  /* We always start at frame 1 */
  cpi->CurrentFrame = 1;

  InitHuffmanSet(cpi);

  /* This makes sure encoder version specific tables are initialised */
  memcpy(&cpi->quant_info, &TH_VP31_QUANT_INFO, sizeof(th_quant_info));
  InitQTables(cpi);
  if(cpi->info.target_bitrate>0)oc_rc_state_init(&cpi->rc,&cpi->info);

  /* Indicate that the next frame to be compressed is the first in the
     current clip. */
  cpi->LastKeyFrame = -1;
  cpi->readyflag = 1;

  cpi->HeadersWritten = 0;
  /*We overload this flag to track header output.*/
  cpi->doneflag=-3;

  return 0;
}

int theora_encode_YUVin(theora_state *t,
                         yuv_buffer *yuv){
  int dropped = 0;
  ogg_int32_t i;
  unsigned char *LocalDataPtr;
  unsigned char *InputDataPtr;
  CP_INSTANCE *cpi=(CP_INSTANCE *)(t->internal_encode);

  if(!cpi->readyflag)return OC_EINVAL;
  if(cpi->doneflag>0)return OC_EINVAL;

  /* If frame size has changed, abort out for now */
  if (yuv->y_height != (int)cpi->info.height ||
      yuv->y_width != (int)cpi->info.width )
    return(-1);

  /* Copy over input YUV to internal YUV buffers. */
  /* we invert the image for backward compatibility with VP3 */
  /* First copy over the Y data */
  LocalDataPtr = cpi->frame + cpi->offset[0] + cpi->stride[0]*(yuv->y_height - 1);
  InputDataPtr = yuv->y;
  for ( i = 0; i < yuv->y_height; i++ ){
    memcpy( LocalDataPtr, InputDataPtr, yuv->y_width );
    LocalDataPtr -= cpi->stride[0];
    InputDataPtr += yuv->y_stride;
  }

  /* Now copy over the U data */
  LocalDataPtr = cpi->frame + cpi->offset[1] + cpi->stride[1]*(yuv->uv_height - 1);
  InputDataPtr = yuv->u;
  for ( i = 0; i < yuv->uv_height; i++ ){
    memcpy( LocalDataPtr, InputDataPtr, yuv->uv_width );
    LocalDataPtr -= cpi->stride[1];
    InputDataPtr += yuv->uv_stride;
  }

  /* Now copy over the V data */
  LocalDataPtr = cpi->frame + cpi->offset[2] + cpi->stride[2]*(yuv->uv_height - 1);
  InputDataPtr = yuv->v;
  for ( i = 0; i < yuv->uv_height; i++ ){
    memcpy( LocalDataPtr, InputDataPtr, yuv->uv_width );
    LocalDataPtr -= cpi->stride[2];
    InputDataPtr += yuv->uv_stride;
  }

  /* don't allow generating invalid files that overflow the p-frame
     shift, even if keyframe_auto_p is turned off */
  if(cpi->LastKeyFrame==-1 || cpi->LastKeyFrame >= (ogg_uint32_t)
     cpi->info.keyframe_frequency_force){

    CompressKeyFrame(cpi,0);
    oc_enc_update_rc_state(cpi,oggpackB_bytes(cpi->oggbuffer)<<3,
     0,cpi->BaseQ,1);

    /* On first frame, the previous was a initial dry-run to prime
       feed-forward statistics */
    if(cpi->CurrentFrame==1)CompressKeyFrame(cpi,1);

  }
  else{
    /*Compress the frame.*/
    dropped=CompressFrame(cpi,0);
  }

  /* Update stats variables. */
  {
    /* swap */
    unsigned char *temp;
    temp=cpi->lastrecon;
    cpi->lastrecon=cpi->recon;
    cpi->recon=temp;
  }
  if(cpi->FrameType==KEY_FRAME){
    memcpy(cpi->golden,cpi->lastrecon,sizeof(*cpi->lastrecon)*cpi->frame_size);
  }
  cpi->CurrentFrame++;
  cpi->packetflag=1;
  if(cpi->info.target_bitrate>0){
    oc_enc_update_rc_state(cpi,oggpackB_bytes(cpi->oggbuffer)<<3,
     cpi->FrameType!=KEY_FRAME,cpi->BaseQ,0);
  }

  t->granulepos=
    ((cpi->CurrentFrame - cpi->LastKeyFrame)<<cpi->keyframe_granule_shift)+
    cpi->LastKeyFrame - 1;

  return 0;
}

int theora_encode_packetout( theora_state *t, int last_p, ogg_packet *op){
  CP_INSTANCE *cpi=(CP_INSTANCE *)(t->internal_encode);
  long bytes=oggpackB_bytes(cpi->oggbuffer);

  if(!bytes)return(0);
  if(!cpi->packetflag)return(0);
  if(cpi->doneflag>0)return(-1);

  op->packet=oggpackB_get_buffer(cpi->oggbuffer);
  op->bytes=bytes;
  op->b_o_s=0;
  op->e_o_s=last_p;

  op->packetno=cpi->CurrentFrame;
  op->granulepos=t->granulepos;

  cpi->packetflag=0;
  if(last_p){
    cpi->doneflag=1;
#ifdef COLLECT_METRICS
    DumpMetrics(cpi);
#endif
  }
  return 1;
}

static void _tp_writebuffer(oggpack_buffer *opb, const char *buf, const long len)
{
  long i;

  for (i = 0; i < len; i++)
    oggpackB_write(opb, *buf++, 8);
}

static void _tp_writelsbint(oggpack_buffer *opb, long value)
{
  oggpackB_write(opb, value&0xFF, 8);
  oggpackB_write(opb, value>>8&0xFF, 8);
  oggpackB_write(opb, value>>16&0xFF, 8);
  oggpackB_write(opb, value>>24&0xFF, 8);
}

/* build the initial short header for stream recognition and format */
int theora_encode_header(theora_state *t, ogg_packet *op){
  CP_INSTANCE *cpi=(CP_INSTANCE *)(t->internal_encode);
  int offset_y;

  oggpackB_reset(cpi->oggbuffer);
  oggpackB_write(cpi->oggbuffer,0x80,8);
  _tp_writebuffer(cpi->oggbuffer, "theora", 6);

  oggpackB_write(cpi->oggbuffer,TH_VERSION_MAJOR,8);
  oggpackB_write(cpi->oggbuffer,TH_VERSION_MINOR,8);
  oggpackB_write(cpi->oggbuffer,TH_VERSION_SUB,8);

  oggpackB_write(cpi->oggbuffer,cpi->info.width>>4,16);
  oggpackB_write(cpi->oggbuffer,cpi->info.height>>4,16);
  oggpackB_write(cpi->oggbuffer,cpi->info.frame_width,24);
  oggpackB_write(cpi->oggbuffer,cpi->info.frame_height,24);
  oggpackB_write(cpi->oggbuffer,cpi->info.offset_x,8);
  /* Applications use offset_y to mean offset from the top of the image; the
   * meaning in the bitstream is the opposite (from the bottom). Transform.
   */
  offset_y = cpi->info.height - cpi->info.frame_height -
    cpi->info.offset_y;
  oggpackB_write(cpi->oggbuffer,offset_y,8);

  oggpackB_write(cpi->oggbuffer,cpi->info.fps_numerator,32);
  oggpackB_write(cpi->oggbuffer,cpi->info.fps_denominator,32);
  oggpackB_write(cpi->oggbuffer,cpi->info.aspect_numerator,24);
  oggpackB_write(cpi->oggbuffer,cpi->info.aspect_denominator,24);

  oggpackB_write(cpi->oggbuffer,cpi->info.colorspace,8);
  oggpackB_write(cpi->oggbuffer,cpi->info.target_bitrate,24);
  oggpackB_write(cpi->oggbuffer,cpi->info.quality,6);

  oggpackB_write(cpi->oggbuffer,cpi->keyframe_granule_shift,5);

  oggpackB_write(cpi->oggbuffer,cpi->info.pixelformat,2);

  oggpackB_write(cpi->oggbuffer,0,3); /* spare config bits */

  op->packet=oggpackB_get_buffer(cpi->oggbuffer);
  op->bytes=oggpackB_bytes(cpi->oggbuffer);

  op->b_o_s=1;
  op->e_o_s=0;

  op->packetno=0;

  op->granulepos=0;
  cpi->packetflag=0;

  return(0);
}

/* build the comment header packet from the passed metadata */
int theora_encode_comment(theora_comment *tc, ogg_packet *op)
{
  const char *vendor = theora_version_string();
  const int vendor_length = strlen(vendor);
  oggpack_buffer *opb;

  opb = _ogg_malloc(sizeof(oggpack_buffer));
  oggpackB_writeinit(opb);
  oggpackB_write(opb, 0x81, 8);
  _tp_writebuffer(opb, "theora", 6);

  _tp_writelsbint(opb, vendor_length);
  _tp_writebuffer(opb, vendor, vendor_length);

  _tp_writelsbint(opb, tc->comments);
  if(tc->comments){
    int i;
    for(i=0;i<tc->comments;i++){
      if(tc->user_comments[i]){
        _tp_writelsbint(opb,tc->comment_lengths[i]);
        _tp_writebuffer(opb,tc->user_comments[i],tc->comment_lengths[i]);
      }else{
        oggpackB_write(opb,0,32);
      }
    }
  }
  op->bytes=oggpack_bytes(opb);

  /* So we're expecting the application will free this? */
  op->packet=_ogg_malloc(oggpack_bytes(opb));
  memcpy(op->packet, oggpack_get_buffer(opb), oggpack_bytes(opb));
  oggpack_writeclear(opb);

  _ogg_free(opb);

  op->b_o_s=0;
  op->e_o_s=0;

  op->packetno=0;
  op->granulepos=0;

  return (0);
}

/* build the final header packet with the tables required
   for decode */
int theora_encode_tables(theora_state *t, ogg_packet *op){
  CP_INSTANCE *cpi=(CP_INSTANCE *)(t->internal_encode);

  oggpackB_reset(cpi->oggbuffer);
  oggpackB_write(cpi->oggbuffer,0x82,8);
  _tp_writebuffer(cpi->oggbuffer,"theora",6);

  oc_quant_params_pack(cpi->oggbuffer,&cpi->quant_info);
  WriteHuffmanTrees(cpi->HuffRoot_VP3x,cpi->oggbuffer);

  op->packet=oggpackB_get_buffer(cpi->oggbuffer);
  op->bytes=oggpackB_bytes(cpi->oggbuffer);

  op->b_o_s=0;
  op->e_o_s=0;

  op->packetno=0;

  op->granulepos=0;
  cpi->packetflag=0;

  cpi->HeadersWritten = 1;

  return(0);
}

static void theora_encode_clear (theora_state  *th){
  CP_INSTANCE *cpi;
  cpi=(CP_INSTANCE *)th->internal_encode;
  if(cpi){

    ClearHuffmanSet(cpi);
    ClearFrameInfo(cpi);

    oggpackB_writeclear(cpi->oggbuffer);
    _ogg_free(cpi->oggbuffer);

    memset(cpi,0,sizeof(cpi));
    _ogg_free(cpi);
  }

  memset(th,0,sizeof(*th));
}


/* returns, in seconds, absolute time of current packet in given
   logical stream */
static double theora_encode_granule_time(theora_state *th,
 ogg_int64_t granulepos){
#ifndef THEORA_DISABLE_FLOAT
  CP_INSTANCE *cpi=(CP_INSTANCE *)(th->internal_encode);

  if(granulepos>=0){
    ogg_int64_t iframe=granulepos>>cpi->keyframe_granule_shift;
    ogg_int64_t pframe=granulepos-(iframe<<cpi->keyframe_granule_shift);

    return (iframe+pframe)*
      ((double)cpi->info.fps_denominator/cpi->info.fps_numerator);

  }
#endif

  return(-1); /* negative granulepos or float calculations disabled */
}

/* returns frame number of current packet in given logical stream */
static ogg_int64_t theora_encode_granule_frame(theora_state *th,
 ogg_int64_t granulepos){
  CP_INSTANCE *cpi=(CP_INSTANCE *)(th->internal_encode);

  if(granulepos>=0){
    ogg_int64_t iframe=granulepos>>cpi->keyframe_granule_shift;
    ogg_int64_t pframe=granulepos-(iframe<<cpi->keyframe_granule_shift);

    return (iframe+pframe);
  }

  return(-1);
}


static int theora_encode_control(theora_state *th,int req,
 void *buf,size_t buf_sz) {
  CP_INSTANCE *cpi;
  int value;

  if(th == NULL)
    return TH_EFAULT;

  cpi = th->internal_encode;

  switch(req) {
    case TH_ENCCTL_SET_QUANT_PARAMS:
      if( ( buf==NULL&&buf_sz!=0 )
  	   || ( buf!=NULL&&buf_sz!=sizeof(th_quant_info) )
  	   || cpi->HeadersWritten ){
        return TH_EINVAL;
      }

      memcpy(&cpi->quant_info, buf, sizeof(th_quant_info));
      InitQTables(cpi);

      return 0;
    case TH_ENCCTL_SET_VP3_COMPATIBLE:
      if(cpi->HeadersWritten)
        return TH_EINVAL;

      memcpy(&cpi->quant_info, &TH_VP31_QUANT_INFO, sizeof(th_quant_info));
      InitQTables(cpi);

      return 0;
    case TH_ENCCTL_SET_SPLEVEL:
      if(buf == NULL || buf_sz != sizeof(int))
        return TH_EINVAL;

      memcpy(&value, buf, sizeof(int));

      switch(value) {
        case 0:
          cpi->MotionCompensation = 1;
          cpi->info.quick_p = 0;
        break;

        case 1:
          cpi->MotionCompensation = 1;
          cpi->info.quick_p = 1;
        break;

        case 2:
          cpi->MotionCompensation = 0;
          cpi->info.quick_p = 1;
        break;

        default:
          return TH_EINVAL;
      }

      return 0;
    case TH_ENCCTL_GET_SPLEVEL_MAX:
      value = 2;
      memcpy(buf, &value, sizeof(int));
      return 0;
    default:
      return TH_EIMPL;
  }
}

static void theora_encode_dispatch_init(CP_INSTANCE *cpi){
  cpi->dispatch_vtbl.clear=theora_encode_clear;
  cpi->dispatch_vtbl.control=theora_encode_control;
  cpi->dispatch_vtbl.granule_frame=theora_encode_granule_frame;
  cpi->dispatch_vtbl.granule_time=theora_encode_granule_time;
}
