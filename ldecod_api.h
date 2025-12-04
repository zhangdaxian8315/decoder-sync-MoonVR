/*!
 ************************************************************************
 *  \file
 *     h264decoder.h
 *  \brief
 *     interface for H.264 decoder.
 *  \author
 *     Copyright (C) 2009 Dolby
 *  Yuwen He (yhe@dolby.com)
 *  
 ************************************************************************
 */
#ifndef _LDECODE_API_H_
#define _LDECODE_API_H_

#ifdef __cplusplus
extern "C" {
#endif


#define  WRITE_TO_FILE  0
#define  INPUT_REF_LEN  12
#define  PKT_DELAY       5


typedef enum
{
  DEC_GEN_NOERR = 0,
  DEC_OPEN_NOERR = 0,
  DEC_CLOSE_NOERR = 0,  
  DEC_SUCCEED = 0,
  DEC_EOS =1,
  DEC_NEED_DATA = 2,
  DEC_INVALID_PARAM = 3,
  DEC_MORE = 4, //add by lxb
  DEC_ERRMASK = 0x8000
  
//  DEC_ERRMASK = 0x80000000
}DecErrCode;

typedef struct dec_set_t
{
  int iPostprocLevel; // valid interval are [0..100]
  int bDBEnable;
  int bAllLayers;
  int time_incr;
  int bDecCompAdapt;
} DecSet_t;

typedef struct DecPacket
{
  unsigned char *data;
  int   size;
  long long  pts;
  long long  dts;
} DecPacket;


typedef struct RefMv
{
  short mv_x;
  short mv_y;
} RefMv;





typedef struct InputRefFrame
{
  unsigned char *data[3];
  long long   pts;
  long long  dts;
  int inuse;   //0, can be overid; 1, 
  RefMv  *pmv;
}InputRefFrame;

typedef struct DecFrame
{
  unsigned char *data[3];
  long long   pts;
  long long  dts;
  InputRefFrame  inputRef[INPUT_REF_LEN];
} DecFrame;








static int selectOneRefBuf(DecFrame *decframe)
{
    int i;
    //printf("-----------------selectOneRefBuf  start-----------------\n");
    for(i = 0; i< INPUT_REF_LEN; i++)
    {
      if(decframe->inputRef[i].inuse == 0)
      {
        decframe->inputRef[i].inuse = 1;
        break;
      }
    }
    if (i == INPUT_REF_LEN) //failed not find
       i = -1;
    
    printf("-----------------selectOneRefBuf  end i=%d-----------------\n",i);
    return i;
}

static int selectOneRefBufInUse(DecFrame *decframe)
{
    int i;
    int index = -1;
    long long  mindts = 1024*1024*256;
    int firstpos = 0;

    for(i = 0; i< INPUT_REF_LEN; i++)
    {
      if(decframe->inputRef[i].inuse == 1)
      {  
        mindts = decframe->inputRef[i].dts; //set for sure;
        firstpos = i;
        i = INPUT_REF_LEN;
      }
    }
    //printf("-----------------selectOneRefBufInUse  start mindts = %ld, 1spos=%d-----------------\n",mindts,firstpos);
    for(i = 0; i< INPUT_REF_LEN; i++)
    {
      //printf("----selectOneRefBufInUse yyyyyy i=%d, dts=%lld, mindts=%lld\n",i,decframe->inputRef[i].dts, mindts);
      if(decframe->inputRef[i].inuse == 1)
      {
        //break;
        //printf("----selectOneRefBufInUse xxx i=%d, dts=%lld\n",i,decframe->inputRef[i].dts);
        if(decframe->inputRef[i].dts <= mindts)
        {
           mindts = decframe->inputRef[i].dts;
           index = i;
           //printf("      ----selectOneRefBufInUse zzzzz i=%d, dts=%lld\n",i,decframe->inputRef[i].dts);
        }
      }
    }

    if(index!=-1)
    {
      //decframe->inputRef[index].inuse = 0;
    }

    printf("-----------------selectOneRefBufInUse  end  index=%d-----------------\n\n",index);
   
    return index;
}



// #ifdef __cplusplus
// extern "C" {
// #endif
//int FinitDecoder(DecodedPicList **ppDecPicList);
//int DecodeOneFrameApi(DecodedPicList **ppDecPic, void *decHandle);
void* xavc3d_decode_init(int *yuvsize );
int xavc3d_decode_frame(void *decHandle, DecFrame *data, int *got_frame, DecPacket *pkt);
int xavc3d_decode_pkt(void *decHandle, DecFrame *data, int *got_frame, DecPacket *pkt);
int xavc3d_decode_flush( void *decHandle);
int xavc3d_decode_close(void **decHandle);
//int SetOptsDecoder(DecSet_t *pDecOpts);
#ifdef __cplusplus
}
#endif
#endif
