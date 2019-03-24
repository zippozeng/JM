/*
***********************************************************************
* COPYRIGHT AND WARRANTY INFORMATION
*
* Copyright 2001, International Telecommunications Union, Geneva
*
* DISCLAIMER OF WARRANTY
*
* These software programs are available to the user without any
* license fee or royalty on an "as is" basis. The ITU disclaims
* any and all warranties, whether express, implied, or
* statutory, including any implied warranties of merchantability
* or of fitness for a particular purpose.  In no event shall the
* contributor or the ITU be liable for any incidental, punitive, or
* consequential damages of any kind whatsoever arising from the
* use of these programs.
*
* This disclaimer of warranty extends to the user of these programs
* and user's customers, employees, agents, transferees, successors,
* and assigns.
*
* The ITU does not represent or warrant that the programs furnished
* hereunder are free of infringement of any third-party patents.
* Commercial implementations of ITU-T Recommendations, including
* shareware, may be subject to royalty fees to patent holders.
* Information regarding the ITU-T patent policy is available from
* the ITU Web site at http://www.itu.int.
*
* THIS IS NOT A GRANT OF PATENT RIGHTS - SEE THE ITU-T PATENT POLICY.
************************************************************************
*/

/*!
 ***********************************************************************
 * \file macroblock.c
 *
 * \brief
 *     Decode a Macroblock
 *
 * \author
 *    Main contributors (see contributors.h for copyright, address and affiliation details)
 *    - Inge Lille-Lang�y               <inge.lille-langoy@telenor.com>
 *    - Rickard Sjoberg                 <rickard.sjoberg@era.ericsson.se>
 *    - Jani Lainema                    <jani.lainema@nokia.com>
 *    - Sebastian Purreiter             <sebastian.purreiter@mch.siemens.de>
 *    - Thomas Wedi                     <wedi@tnt.uni-hannover.de>
 *    - Detlev Marpe                    <marpe@hhi.de>
 *    - Gabi Blaettermann               <blaetter@hhi.de>
 *    - Ye-Kui Wang                     <wangy@cs.tut.fi>
 ***********************************************************************
*/

#include "contributors.h"

#include <math.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "global.h"
#include "mbuffer.h"
#include "elements.h"
#include "errorconcealment.h"
#include "macroblock.h"
#include "decodeiff.h"
#include "fmo.h"
#include "abt.h"


void SetMotionVectorPredictor (struct img_par  *img,
                          int             *pmv_x,
                          int             *pmv_y,
                          int             ref_frame,
                          int             **refFrArr,
                          int             ***tmp_mv,
                          int             block_x,
                          int             block_y,
                          int             blockshape_x,
                          int             blockshape_y); // declare to avoid warnings. mwi

/*!
 ************************************************************************
 * \brief
 *    Checks the availability of neighboring macroblocks of
 *    the current macroblock for prediction and context determination;
 *    marks the unavailable MBs for intra prediction in the
 *    ipredmode-array by -1. Only neighboring MBs in the causal
 *    past of the current MB are checked.
 ************************************************************************
 */
void CheckAvailabilityOfNeighbors(struct img_par *img)
{
  int i,j;
  const int mb_width = img->width/MB_BLOCK_SIZE;
  const int mb_nr = img->current_mb_nr;
  Macroblock *currMB = &img->mb_data[mb_nr];

  // mark all neighbors as unavailable
  for (i=0; i<3; i++)
    for (j=0; j<3; j++)
      img->mb_data[mb_nr].mb_available[i][j]=NULL;
  img->mb_data[mb_nr].mb_available[1][1]=currMB; // current MB

  // Check MB to the left
  if(img->pix_x >= MB_BLOCK_SIZE)
  {
    int remove_prediction = currMB->slice_nr != img->mb_data[mb_nr-1].slice_nr;
    // upper blocks
    if (remove_prediction || (img->UseConstrainedIntraPred && img->intra_block[mb_nr-1][1]==0))
    {
      img->ipredmode[img->block_x][img->block_y+1] = -1;
      img->ipredmode[img->block_x][img->block_y+2] = -1;
    }
    // lower blocks
    if (remove_prediction || (img->UseConstrainedIntraPred && img->intra_block[mb_nr-1][3]==0))
    {
      img->ipredmode[img->block_x][img->block_y+3] = -1;
      img->ipredmode[img->block_x][img->block_y+4] = -1;
    }
    if (!remove_prediction)
    {
      currMB->mb_available[1][0]=&(img->mb_data[mb_nr-1]);
    }
  }


  // Check MB above
  if(img->pix_y >= MB_BLOCK_SIZE)
  {
    int remove_prediction = currMB->slice_nr != img->mb_data[mb_nr-mb_width].slice_nr;
    // upper blocks
    if (remove_prediction || (img->UseConstrainedIntraPred && img->intra_block[mb_nr-mb_width][2]==0))
    {
      img->ipredmode[img->block_x+1][img->block_y] = -1;
      img->ipredmode[img->block_x+2][img->block_y] = -1;
    }
    // lower blocks
    if (remove_prediction || (img->UseConstrainedIntraPred && img->intra_block[mb_nr-mb_width][3]==0))
    {
      img->ipredmode[img->block_x+3][img->block_y] = -1;
      img->ipredmode[img->block_x+4][img->block_y] = -1;
    }
    if (!remove_prediction)
    {
      currMB->mb_available[0][1]=&(img->mb_data[mb_nr-mb_width]);
    }
  }

  // Check MB left above
  if(img->pix_x >= MB_BLOCK_SIZE && img->pix_y  >= MB_BLOCK_SIZE )
  {
    if(currMB->slice_nr == img->mb_data[mb_nr-mb_width-1].slice_nr)
      img->mb_data[mb_nr].mb_available[0][0]=&(img->mb_data[mb_nr-mb_width-1]);
  }

  // Check MB right above
  if(img->pix_y >= MB_BLOCK_SIZE && img->pix_x < (img->width-MB_BLOCK_SIZE ))
  {
    if(currMB->slice_nr == img->mb_data[mb_nr-mb_width+1].slice_nr)
      currMB->mb_available[0][2]=&(img->mb_data[mb_nr-mb_width+1]);
  }
}

/*!
 ************************************************************************
 * \brief
 *    initializes the current macroblock
 ************************************************************************
 */
void start_macroblock(struct img_par *img,struct inp_par *inp, int CurrentMBInScanOrder)
{
  int i,j,k,l;
  Macroblock *currMB;   // intialization code deleted, see below, StW

  assert (img->current_mb_nr >=0 && img->current_mb_nr < img->max_mb_nr);
  img->current_mb_nr = CurrentMBInScanOrder;
  currMB = &img->mb_data[CurrentMBInScanOrder];

  /* Update coordinates of the current macroblock */
  if (img->mb_frame_field_flag)
  {
    img->mb_x = (img->current_mb_nr)%((2*img->width)/MB_BLOCK_SIZE);
    img->mb_y = 2*((img->current_mb_nr)/((2*img->width)/MB_BLOCK_SIZE));

    if (img->mb_x % 2)
    {
      img->mb_y++;
    }

    img->mb_x /= 2;
  }
  else
  {
    img->mb_x = (img->current_mb_nr)%(img->width/MB_BLOCK_SIZE);
    img->mb_y = (img->current_mb_nr)/(img->width/MB_BLOCK_SIZE);
  }
  
  /* Define vertical positions */
  img->block_y = img->mb_y * BLOCK_SIZE;      /* luma block position */
  img->pix_y   = img->mb_y * MB_BLOCK_SIZE;   /* luma macroblock position */
  img->pix_c_y = img->mb_y * MB_BLOCK_SIZE/2; /* chroma macroblock position */
  
  /* Define horizontal positions */
  img->block_x = img->mb_x * BLOCK_SIZE;      /* luma block position */
  img->pix_x   = img->mb_x * MB_BLOCK_SIZE;   /* luma pixel position */
  img->pix_c_x = img->mb_x * MB_BLOCK_SIZE/2; /* chroma pixel position */

  // Save the slice number of this macroblock. When the macroblock below
  // is coded it will use this to decide if prediction for above is possible
  currMB->slice_nr = img->current_slice_nr;
  
  // If MB is next to a slice boundary, mark neighboring blocks unavailable for prediction
  if (img->mb_frame_field_flag==0)
    CheckAvailabilityOfNeighbors(img);      // support only slice mode 0 in MBINTLC1 at this time

  // Reset syntax element entries in MB struct
  currMB->qp          = img->qp ;
  currMB->mb_type     = 0;
  currMB->delta_quant = 0;
  currMB->cbp         = 0;
  currMB->cbp_blk     = 0;
  for (i=0; i<4; i++)
  {
    currMB->useABT[i]        = 0;       // ABT
    currMB->abt_mode[i]      = ABT_OFF; // ABT
    currMB->abt_pred_mode[i] = B4x4;    // ABT. initialization to 4x4 mode.
  }

  for (l=0; l < 2; l++)
    for (j=0; j < BLOCK_MULTIPLE; j++)
      for (i=0; i < BLOCK_MULTIPLE; i++)
        for (k=0; k < 2; k++)
          currMB->mvd[l][j][i][k] = 0;

  for (i=0; i < (BLOCK_MULTIPLE*BLOCK_MULTIPLE); i++)
    currMB->intra_pred_modes[i] = 0;

  currMB->cbp_bits   = 0;

  // initialize img->m7 for ABT
  for (j=0; j<MB_BLOCK_SIZE; j++)
    for (i=0; i<MB_BLOCK_SIZE; i++)
      img->m7[i][j] = 0;
}

/*!
 ************************************************************************
 * \brief
 *    set coordinates of the next macroblock
 *    check end_of_slice condition 
 ************************************************************************
 */
int exit_macroblock(struct img_par *img,struct inp_par *inp)
{
  Slice *currSlice = img->currentSlice;

  //! The if() statement below resembles the original code, which tested 
  //! img->current_mb_nr == img->max_mb_nr.  Both is, of course, nonsense
  //! In an error prone environment, one can only be sure to have a new
  //! picture by checking the tr of the next slice header!

// printf ("exit_macroblock: FmoGetLastMBOfPicture %d, img->current_mb_nr %d\n", FmoGetLastMBOfPicture(), img->current_mb_nr);
  if (img->current_mb_nr == FmoGetLastMBOfPicture(currSlice->structure))
  {
    if (currSlice->next_header != EOS)
      currSlice->next_header = SOP;
    return TRUE;
  }
  // ask for last mb in the slice  UVLC
  else
  {
// printf ("exit_macroblock: Slice %d old MB %d, now using MB %d\n", img->current_slice_nr, img->current_mb_nr, FmoGetNextMBNr (img->current_mb_nr));

    img->current_mb_nr = FmoGetNextMBNr (img->current_mb_nr, currSlice->structure);
 
    if (img->current_mb_nr == -1)     // End of Slice group, MUST be end of slice
    {
      assert (nal_startcode_follows (img, inp) == TRUE);
      return TRUE;
    }

    if(nal_startcode_follows(img, inp) == FALSE) 
      return FALSE;

    if(img->type == INTRA_IMG  || img->type == SI_IMG || inp->symbol_mode == CABAC)
      return TRUE;
    if(img->cod_counter<=0)
      return TRUE;
    return FALSE;
  }
}

/*!
 ************************************************************************
 * \brief
 *    Interpret the mb mode for P-Frames
 ************************************************************************
 */
void interpret_mb_mode_P(struct img_par *img)
{
  int i;
  const int ICBPTAB[6] = {0,16,32,15,31,47};
  Macroblock *currMB = &img->mb_data[img->current_mb_nr];
  int         mbmode = currMB->mb_type;

#define ZERO_P8x8     (mbmode==5)
#define MODE_IS_P8x8  (mbmode==4 || mbmode==5)
#define MODE_IS_I4x4  (mbmode==6)
#define I16OFFSET     (mbmode-7)

  if(mbmode <4)
  {
    currMB->mb_type = mbmode;
    for (i=0;i<4;i++)
    {
      currMB->b8mode[i]   = mbmode;
      currMB->b8pdir[i]   = 0;
      currMB->useABT[i]   = (USEABT!=NO_ABT);
      currMB->abt_mode[i] = USEABT? B8x8 : ABT_OFF; // mbmode? B8x8 : ABT_OFF;
    }
  }
  else if(MODE_IS_P8x8)
  {
    currMB->mb_type = P8x8;
    img->allrefzero = ZERO_P8x8;
    // b8mode and pdir are read and set later
    // abt mode and useABT are set later.
  }
  else if(MODE_IS_I4x4)
  {
    currMB->mb_type = I4MB;
    for (i=0;i<4;i++)
    {
      currMB->b8mode[i] = IBLOCK;
      currMB->b8pdir[i] = -1;
      currMB->useABT[i] = (USEABT == INTER_INTRA_ABT);
    }
  }
  else
  {
    currMB->mb_type = I16MB;
    for (i=0;i<4;i++) {currMB->b8mode[i]=0; currMB->b8pdir[i]=-1; currMB->useABT[i] = NO_ABT;} // I16MB not with ABT
    currMB->cbp= ICBPTAB[(I16OFFSET)>>2];
    currMB->i16mode = (I16OFFSET) & 0x03;
  }
}

/*!
 ************************************************************************
 * \brief
 *    Interpret the mb mode for I-Frames
 ************************************************************************
 */
void interpret_mb_mode_I(struct img_par *img)
{
  int i;
  const int ICBPTAB[6] = {0,16,32,15,31,47};
  Macroblock *currMB   = &img->mb_data[img->current_mb_nr];
  int         mbmode   = currMB->mb_type;

  if (mbmode==0)
  {
    currMB->mb_type = I4MB;
    for (i=0;i<4;i++) {currMB->b8mode[i]=IBLOCK; currMB->b8pdir[i]=-1; currMB->useABT[i] = (USEABT == INTER_INTRA_ABT);}
  }
  else
  {
    currMB->mb_type = I16MB;
    for (i=0;i<4;i++) {currMB->b8mode[i]=0; currMB->b8pdir[i]=-1; currMB->useABT[i] = NO_ABT;} // I16MB not with ABT
    currMB->cbp= ICBPTAB[(mbmode-1)>>2];
    currMB->i16mode = (mbmode-1) & 0x03;
  }
}

/*!
 ************************************************************************
 * \brief
 *    Interpret the mb mode for B-Frames
 ************************************************************************
 */
void interpret_mb_mode_B(struct img_par *img)
{
  static const int offset2pdir16x16[12]   = {0, 0, 1, 2, 0,0,0,0,0,0,0,0};
  static const int offset2pdir16x8[22][2] = {{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{1,1},{0,0},{0,1},{0,0},{1,0},
                                             {0,0},{0,2},{0,0},{1,2},{0,0},{2,0},{0,0},{2,1},{0,0},{2,2},{0,0}};
  static const int offset2pdir8x16[22][2] = {{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{1,1},{0,0},{0,1},{0,0},
                                             {1,0},{0,0},{0,2},{0,0},{1,2},{0,0},{2,0},{0,0},{2,1},{0,0},{2,2}};

  const int ICBPTAB[6] = {0,16,32,15,31,47};
  Macroblock *currMB = &img->mb_data[img->current_mb_nr];

  int i, mbmode;
  int mbtype  = currMB->mb_type;
  int *b8mode = currMB->b8mode;
  int *b8pdir = currMB->b8pdir;
  int *abt    = currMB->abt_mode;

  for (i=0;i<4;i++) { currMB->useABT[i] = USEABT;}
  for (i=0;i<4;i++) { currMB ->bipred_weighting_type[i]=0;}

  //--- set mbtype, b8type, and b8pdir ---
  if (mbtype==0)       // direct
  {
      mbmode=0;       for(i=0;i<4;i++) {b8mode[i]=0;          b8pdir[i]=2;          if (USEABT) getDirectModeABT(i,img);} //*KS*
  }
  else if (mbtype==23) // intra4x4
  {
    mbmode=I4MB;    for(i=0;i<4;i++) {b8mode[i]=IBLOCK;     b8pdir[i]=-1;         currMB->useABT[i] = (USEABT == INTER_INTRA_ABT);}
  }
  else if (mbtype>23) // intra16x16
  {
    mbmode=I16MB;   for(i=0;i<4;i++) {b8mode[i]=0;          b8pdir[i]=-1;         currMB->useABT[i] = NO_ABT;} // I16MB not with ABT
    currMB->cbp     = ICBPTAB[(mbtype-24)>>2];
    currMB->i16mode = (mbtype-24) & 0x03;
  }
  else if (mbtype==22) // 8x8(+split)
  {
    mbmode=P8x8;       // b8mode and pdir is transmitted in additional codewords
  }
  else if (mbtype<4)   // 16x16
  {
    mbmode=1;       for(i=0;i<4;i++) {b8mode[i]=1;          b8pdir[i]=offset2pdir16x16[mbtype];         abt[i]=B8x8; }
  }
  else if (mbtype%2==0) // 16x8
  {
    mbmode=2;       for(i=0;i<4;i++) {b8mode[i]=2;          b8pdir[i]=offset2pdir16x8 [mbtype][i/2];    abt[i]=B8x8; }
  }
  else
  {
    mbmode=3;       for(i=0;i<4;i++) {b8mode[i]=3;          b8pdir[i]=offset2pdir8x16 [mbtype][i%2];    abt[i]=B8x8; }
  }
  currMB->mb_type = mbmode;
}
/*!
 ************************************************************************
 * \brief
 *    Interpret the mb mode for SI-Frames
 ************************************************************************
 */
void interpret_mb_mode_SI(struct img_par *img)
{
  int i;
  const int ICBPTAB[6] = {0,16,32,15,31,47};
  Macroblock *currMB   = &img->mb_data[img->current_mb_nr];
  int         mbmode   = currMB->mb_type;

  if (mbmode==0)
  {
    currMB->mb_type = SI4MB;
    for (i=0;i<4;i++) {currMB->b8mode[i]=IBLOCK; currMB->b8pdir[i]=-1; currMB->useABT[i] = (USEABT == INTER_INTRA_ABT);}
    img->siblock[img->mb_x][img->mb_y]=1;
  }
  else if (mbmode==1)
  {
    currMB->mb_type = I4MB;
    for (i=0;i<4;i++) {currMB->b8mode[i]=IBLOCK; currMB->b8pdir[i]=-1; currMB->useABT[i] = (USEABT == INTER_INTRA_ABT);}
  }
  else
  {
    currMB->mb_type = I16MB;
    for (i=0;i<4;i++) {currMB->b8mode[i]=0; currMB->b8pdir[i]=-1; currMB->useABT[i] = NO_ABT;} // I16MB not with ABT
    currMB->cbp= ICBPTAB[(mbmode-1)>>2];
    currMB->i16mode = (mbmode-2) & 0x03;
  }
}
/*!
 ************************************************************************
 * \brief
 *    init macroblock I and P frames
 ************************************************************************
 */
void init_macroblock(struct img_par *img)
{
  int i,j;
  int predframe_no;
  Macroblock *currMB = &img->mb_data[img->current_mb_nr];
  int img_block_y;
  int j2 = img->block_y/2 - 2*(img->current_mb_nr%2);

  if (img->mb_frame_field_flag)
  {
    if (img->current_mb_nr%2==0)
    {
      img_block_y   = img->block_y/2;

      img->mv_frm[img->block_x+4][img->block_y][2]=img->number;
      img->mv_top[img->block_x+4][img_block_y][2]=img->number*2;
    }
    else
    {
      img_block_y   = (img->block_y-4)/2;

      img->mv_frm[img->block_x+4][img->block_y][2]=img->number;
      img->mv_bot[img->block_x+4][img_block_y][2]=img->number*2;
    }
  }
  else
    img->mv[img->block_x+4][img->block_y][2]=img->number;

  for (i=0;i<BLOCK_SIZE;i++)
  {                           // reset vectors and pred. modes
    for(j=0;j<BLOCK_SIZE;j++)
    {
      if (img->mb_frame_field_flag)
      {
        if (img->current_mb_nr%2==0)
        {
          img_block_y   = img->block_y/2;

          img->mv_frm[img->block_x+i+4][img->block_y+j][0] = 0;
          img->mv_frm[img->block_x+i+4][img->block_y+j][1] = 0;

          img->mv_top[img->block_x+i+4][img_block_y+j][0] = 0;
          img->mv_top[img->block_x+i+4][img_block_y+j][1] = 0;
        }
        else
        {
          img_block_y   = (img->block_y-4)/2;

          img->mv_frm[img->block_x+i+4][img->block_y+j][0] = 0;
          img->mv_frm[img->block_x+i+4][img->block_y+j][1] = 0;

          img->mv_bot[img->block_x+i+4][img_block_y+j][0] = 0;
          img->mv_bot[img->block_x+i+4][img_block_y+j][1] = 0;
        }
      }
      else
      {
        img->mv[img->block_x+i+4][img->block_y+j][0] = 0;
        img->mv[img->block_x+i+4][img->block_y+j][1] = 0;
      }

      img->ipredmode[img->block_x+i+1][img->block_y+j+1] = 0;
      if (img->current_mb_nr%2==0)
        img->ipredmode_top[img->block_x+i+1][j2+j+1] = 0;
      else
        img->ipredmode_bot[img->block_x+i+1][j2+j+1] = 0;
    }
  }

  predframe_no = 0;
  if (img->structure != FRAME)  // Initialize for field mode (use for copy)
    if (img->number>1)          // use the top field for the bottom field of the first picture
      predframe_no = 1;         // g.b.1;

 
  // Set the reference frame information for motion vector prediction
  if (IS_INTRA (currMB))
  {
    if (img->structure != FRAME)
    {
        for (j=0; j<4; j++)
        for (i=0; i<4; i++)
        {
            refFrArr[img->block_y+j][img->block_x+i] = -1;
        }
    }
    else
    {
        if (img->mb_frame_field_flag)
        {
          if (img->current_mb_nr%2==0)
          {
            img_block_y   = img->block_y/2;
            for (j=0; j<4; j++)
              for (i=0; i<4; i++)
              {
                refFrArr_top[img_block_y+j][img->block_x+i] = -1;
                refFrArr_frm[img->block_y+j][img->block_x+i] = -1;
              }
          }
          else
          {
            img_block_y   = (img->block_y-4)/2;
            for (j=0; j<4; j++)
              for (i=0; i<4; i++)
              {
                refFrArr_bot[img_block_y+j][img->block_x+i] = -1;
                refFrArr_frm[img->block_y+j][img->block_x+i] = -1;
              }
          }
        }
        else
        {
          for (j=0; j<4; j++)
            for (i=0; i<4; i++)
            {
              refFrArr[img->block_y+j][img->block_x+i] = -1;
            }
        }
    }
  }
  else if (!IS_P8x8 (currMB))
  {
    if (img->structure != FRAME)
    {
        for (j=0; j<4; j++)
          for (i=0; i<4; i++)
          {
            refFrArr[img->block_y+j][img->block_x+i] = 0;
          }
    }
    else
    {
        if (img->mb_frame_field_flag)
        {
          if (img->current_mb_nr%2==0)
          {
              img_block_y   = img->block_y/2;
              for (j=0; j<4; j++)
                for (i=0; i<4; i++)
                {
                  refFrArr_top[img_block_y+j][img->block_x+i] = 0;
                  refFrArr_frm[img->block_y+j][img->block_x+i] = 0;
                }
          }
          else
          {
              img_block_y   = (img->block_y-4)/2;
              for (j=0; j<4; j++)
                for (i=0; i<4; i++)
                {
                  refFrArr_bot[img_block_y+j][img->block_x+i] = 0;
                  refFrArr_frm[img->block_y+j][img->block_x+i] = 0;
                }
          }
        }
        else
        {
          for (j=0; j<4; j++)
            for (i=0; i<4; i++)
            {
              refFrArr[img->block_y+j][img->block_x+i] = 0;
            }
        }
    }
  }
  else
  {
        if (img->mb_frame_field_flag)
        {
          if (img->current_mb_nr%2==0)
          {
              img_block_y   = img->block_y/2;
              for (j=0; j<4; j++)
                for (i=0; i<4; i++)
                {
                  if(img->structure != FRAME) 
                    refFrArr[img->block_y+j][img->block_x+i] = (currMB->b8mode[2*(j/2)+(i/2)]==IBLOCK ? -1 : (img->number>1)?1:0);
                  else
                  {
                    refFrArr_top[img_block_y+j][img->block_x+i] = (currMB->b8mode[2*(j/2)+(i/2)]==IBLOCK ? -1 : 0);
                    refFrArr_frm[img->block_y+j][img->block_x+i] = (currMB->b8mode[2*(j/2)+(i/2)]==IBLOCK ? -1 : 0);
                  }
                }
          }
          else
          {
              img_block_y   = (img->block_y-4)/2;
              for (j=0; j<4; j++)
                for (i=0; i<4; i++)
                {
                  if(img->structure != FRAME) 
                    refFrArr[img->block_y+j][img->block_x+i] = (currMB->b8mode[2*(j/2)+(i/2)]==IBLOCK ? -1 : (img->number>1)?1:0);
                  else
                  {
                    refFrArr_bot[img_block_y+j][img->block_x+i] = (currMB->b8mode[2*(j/2)+(i/2)]==IBLOCK ? -1 : 0);
                    refFrArr_frm[img->block_y+j][img->block_x+i] = (currMB->b8mode[2*(j/2)+(i/2)]==IBLOCK ? -1 : 0);
                  }
                }
          }
        }
        else
        {
          for (j=0; j<4; j++)
            for (i=0; i<4; i++)
            {
              if(img->structure != FRAME) 
                refFrArr[img->block_y+j][img->block_x+i] = (currMB->b8mode[2*(j/2)+(i/2)]==IBLOCK ? -1 : (img->number>1)?1:0);
              else
                refFrArr[img->block_y+j][img->block_x+i] = (currMB->b8mode[2*(j/2)+(i/2)]==IBLOCK ? -1 : 0);
            }
        }
  }
}


/*!
 ************************************************************************
 * \brief
 *    Sets mode for 8x8 block
 ************************************************************************
 */
void
SetB8Mode (struct img_par* img, Macroblock* currMB, int value, int i)
{
  static const int p_v2b8 [ 5] = {4, 5, 6, 7, IBLOCK};
  static const int p_v2pd [ 5] = {0, 0, 0, 0, -1};
  static const int p_v2abt[ 5] = {0, 1, 2, 3, ABT_OFF}; // could use enum values here
  static const int b_v2b8 [14] = {0, 4, 4, 4, 5, 6, 5, 6, 5, 6, 7, 7, 7, IBLOCK};
  static const int b_v2pd [14] = {2, 0, 1, 2, 0, 0, 1, 1, 2, 2, 0, 1, 2, -1};
  static const int b_v2abt[14] = {0, 0, 0, 0, 1, 2, 1, 2, 1, 2, 3, 3, 3, ABT_OFF};  // REVISIT FOR INTRA! could use enum values here

  currMB->useABT[i] = (USEABT != NO_ABT);

  if (img->type==B_IMG_1 || img->type==B_IMG_MULT)
  {
    currMB->b8mode[i]   = b_v2b8[value];
    currMB->b8pdir[i]   = b_v2pd[value];
    if (USEABT) //*KS*
        currMB->abt_mode[i] = value? b_v2abt[value] : getDirectModeABT(i,img);

  }
  else
  {
    currMB->b8mode[i]   = p_v2b8[value];
    currMB->b8pdir[i]   = p_v2pd[value];
    currMB->abt_mode[i] = p_v2abt[value];
  }

  if (currMB->b8mode[i]==IBLOCK)
    currMB->useABT[i] = ( USEABT == INTER_INTRA_ABT );
}


/*!
 ************************************************************************
 * \brief
 *    Get the syntax elements from the NAL
 ************************************************************************
 */
int read_one_macroblock(struct img_par *img,struct inp_par *inp)
{
  int i;

  SyntaxElement currSE;
  Macroblock *currMB = &img->mb_data[img->current_mb_nr];

  Slice *currSlice = img->currentSlice;
  DataPartition *dP;
  int *partMap = assignSE2partition[currSlice->dp_mode];

  Macroblock *topMB = &img->mb_data[img->current_mb_nr - 1];    // only work for bottom field
  int  skip;// = (img->current_mb_nr%2 && topMB->mb_type == 0); // && (img->type != INTER_IMG_1 || img->type != INTER_IMG_MULT);
  int  img_block_y;


  if(!(img->type == B_IMG_1 || img->type == B_IMG_MULT))
      skip = (img->current_mb_nr%2 && topMB->mb_type == 0);
  else 
      skip = (img->current_mb_nr%2 && topMB->mb_type == 0 && topMB->cbp == 0);
  
  if (img->current_mb_nr%2 == 0)
    img->mb_field = 0;

  currMB->qp = img->qp ;

  currSE.type = SE_MBTYPE;
  // if( img->type!=INTRA_IMG || img->type!= SI_IMG || USEABT!=INTER_INTRA_ABT )    //in intra frames, there can only be one.
  if( (img->type!=INTRA_IMG && img->type!= SI_IMG) || USEABT!=INTER_INTRA_ABT ) //in intra frames, there can only be one. // bug fix mwi: I&&SI. 
  {
    //  read MB mode *****************************************************************
    if(img->type == B_IMG_1 || img->type == B_IMG_MULT) dP = &(currSlice->partArr[partMap[SE_BFRAME]]);
    else                                                dP = &(currSlice->partArr[partMap[currSE.type]]);

    if (inp->symbol_mode == UVLC || dP->bitstream->ei_flag)   currSE.mapping = linfo;
    else                                                      currSE.reading = readMB_typeInfoFromBuffer_CABAC;

    if(inp->symbol_mode == CABAC || (img->type != SP_IMG_1 && img->type != SP_IMG_MULT && img->type != INTER_IMG_1 && img->type != INTER_IMG_MULT && img->type != B_IMG_1 && img->type != B_IMG_MULT))
    {
      //  read MB mode
#if TRACE
      strncpy(currSE.tracestring, "MB Type", TRACESTRING_SIZE);
#endif
      dP->readSyntaxElement(&currSE,img,inp,dP);
      currMB->mb_type = currSE.value1;
      if(!dP->bitstream->ei_flag)
        currMB->ei_flag = 0;

      // read super MB type here
//          if (img->structure==FRAME && img->mb_frame_field_flag)
      if ((img->structure==FRAME && img->mb_frame_field_flag) && ((img->current_mb_nr%2==0) || (img->current_mb_nr && skip)))
      {
#if TRACE
        strncpy(currSE.tracestring, "Field mode", TRACESTRING_SIZE);
#endif
        dP->readSyntaxElement(&currSE,img,inp,dP);
        img->mb_field = currSE.value1;
      }

      if ((img->type==B_IMG_1 || img->type==B_IMG_MULT) && currSE.value1==0 && currSE.value2==0)
      {
        img->cod_counter=0;
      }
    } 
    else
    {
      if(img->cod_counter == -1)
      {
#if TRACE
        strncpy(currSE.tracestring, "MB runlength", TRACESTRING_SIZE);
#endif
        dP->readSyntaxElement(&currSE,img,inp,dP);
        img->cod_counter = currSE.value1;
      }
      if (img->cod_counter==0)
      {
#if TRACE
        strncpy(currSE.tracestring, "MB Type", TRACESTRING_SIZE);
#endif
        dP->readSyntaxElement(&currSE,img,inp,dP);
        if(img->type == INTER_IMG_1 || img->type == INTER_IMG_MULT || img->type == SP_IMG_1 || img->type == SP_IMG_MULT)
          currSE.value1++;
        currMB->mb_type = currSE.value1;
        if(!dP->bitstream->ei_flag)
          currMB->ei_flag = 0;
        img->cod_counter--;
        // read super MB type here
//          if (img->structure==FRAME && img->mb_frame_field_flag)
        if ((img->structure==FRAME && img->mb_frame_field_flag) && ((img->current_mb_nr%2==0) || (img->current_mb_nr && skip)))
        {
#if TRACE
          strncpy(currSE.tracestring, "Field mode", TRACESTRING_SIZE);
#endif
          dP->readSyntaxElement(&currSE,img,inp,dP);
          img->mb_field = currSE.value1;
        }
      } 
      else
      {
        img->cod_counter--;
        currMB->mb_type = 0;
        currMB->ei_flag = 0;
        if(img->mb_frame_field_flag)
        {
          if(img->cod_counter == 0 && (img->current_mb_nr%2 == 0))
          {
            peekSyntaxElement_UVLC(&currSE,img,inp,dP);
            img->mb_field = currSE.value1;
          }
          else if(img->cod_counter > 0 && (img->current_mb_nr%2 == 0))
            img->mb_field = 0;
        }
      }
    }
  }else{//end if code MBmode (due to intra and ABT)
    currMB->mb_type=0;
  }
  img->siblock[img->mb_x][img->mb_y]=0;

  field_mb[img->mb_y][img->mb_x] = img->mb_field;


  if ((img->type==INTER_IMG_1) || (img->type==INTER_IMG_MULT))    // inter frame
    interpret_mb_mode_P(img);
  else if (img->type==INTRA_IMG)                                  // intra frame
    interpret_mb_mode_I(img);
  else if ((img->type==B_IMG_1) || (img->type==B_IMG_MULT))       // B frame
    interpret_mb_mode_B(img);
  else if ((img->type==SP_IMG_1) || (img->type==SP_IMG_MULT))     // SP frame
    interpret_mb_mode_P(img);
  else if (img->type==SI_IMG)     // SI frame
    interpret_mb_mode_SI(img);

  if(img->mb_frame_field_flag)
  {
    if(img->mb_field)
    {
      img->buf_cycle = 2*(inp->buf_cycle+1);
      img->num_ref_pic_active_fwd <<=1;
    }
    else
      img->buf_cycle = inp->buf_cycle+1;
  }

  //====== READ 8x8 SUB-PARTITION MODES (modes of 8x8 blocks) and Intra VBST block modes ======
  if (IS_P8x8 (currMB))
  {
    currSE.type    = SE_MBTYPE;
    if (img->type==B_IMG_1 || img->type==B_IMG_MULT)      dP = &(currSlice->partArr[partMap[SE_BFRAME]]);
    else                                                  dP = &(currSlice->partArr[partMap[SE_MBTYPE]]);

    for (i=0; i<4; i++)
    {
      if (inp->symbol_mode==UVLC || dP->bitstream->ei_flag) currSE.mapping = linfo;
      else                                                  currSE.reading = readB8_typeInfoFromBuffer_CABAC;

#if TRACE
      strncpy(currSE.tracestring, "8x8 mode", TRACESTRING_SIZE);
#endif
      dP->readSyntaxElement (&currSE, img, inp, dP);
      SetB8Mode (img, currMB, currSE.value1, i);
      if( currMB->useABT[i] && currMB->b8mode[i]==IBLOCK )
      {
        if (inp->symbol_mode==UVLC || dP->bitstream->ei_flag) currSE.mapping = linfo;
        else                                                  currSE.reading = readABTIntraBlkModeInfo2Buffer_CABAC;
        dP->readSyntaxElement (&currSE, img, inp, dP);

        currMB->abt_mode[i]=3-currSE.value1;
        assert(!(currMB->abt_mode[i]&~3));//must be from 0 to 3.
      }
    }
  }

  if( USEABT==INTER_INTRA_ABT && currMB->mb_type==I4MB )
  {
    // for Intra ABT, we need to decode the blocking mode.
    // Note: for ABT Intra

    currSE.type    = SE_MBTYPE;
    if (img->type==B_IMG_1 || img->type==B_IMG_MULT)      dP = &(currSlice->partArr[partMap[SE_BFRAME]]);
    else                                                  dP = &(currSlice->partArr[partMap[SE_MBTYPE]]);
    if (inp->symbol_mode==UVLC || dP->bitstream->ei_flag) currSE.mapping = linfo;
    else                                                  currSE.reading = readABTIntraBlkModeInfo2Buffer_CABAC;

    dP->readSyntaxElement (&currSE, img, inp, dP);
    for(i=0; i<4; i++)
      currMB->abt_mode[i]=3-currSE.value1;
    if(!dP->bitstream->ei_flag)
      currMB->ei_flag = 0;

    assert( currMB->abt_mode[0]>=0 && currMB->abt_mode[0]<4 );
  }

  if(img->UseConstrainedIntraPred && (img->type==INTER_IMG_1 || img->type==INTER_IMG_MULT))        // inter frame
  {
    if (!IS_NEWINTRA (currMB) && currMB->b8mode[0]!=IBLOCK) img->intra_block[img->current_mb_nr][0] = 0;
    if (!IS_NEWINTRA (currMB) && currMB->b8mode[1]!=IBLOCK) img->intra_block[img->current_mb_nr][1] = 0;
    if (!IS_NEWINTRA (currMB) && currMB->b8mode[2]!=IBLOCK) img->intra_block[img->current_mb_nr][2] = 0;
    if (!IS_NEWINTRA (currMB) && currMB->b8mode[3]!=IBLOCK) img->intra_block[img->current_mb_nr][3] = 0;
  }

  if(!(img->type == B_IMG_1 || img->type == B_IMG_MULT))
    for (i=0; i<4; i++)
      setDirectModeABT(i,img);

  //! TO for Error Concelament
  //! If we have an INTRA Macroblock and we lost the partition
  //! which contains the intra coefficients Copy MB would be better 
  //! than just a grey block.
  //! Seems to be a bit at the wrong place to do this right here, but for this case 
  //! up to now there is no other way.
  dP = &(currSlice->partArr[partMap[SE_CBP_INTRA]]);
  if(IS_INTRA (currMB) && dP->bitstream->ei_flag && img->number)
  {
    currMB->mb_type = 0;
    currMB->ei_flag = 1;
    for (i=0;i<4;i++) {currMB->b8mode[i]=currMB->b8pdir[i]=0; }
  }
  if(img->type == B_IMG_1 || img->type == B_IMG_MULT)  dP = &(currSlice->partArr[partMap[SE_BFRAME]]);
  else                                                 dP = &(currSlice->partArr[partMap[currSE.type]]);
  //! End TO


  //--- init macroblock data ---
  if ((img->type==B_IMG_1) || (img->type==B_IMG_MULT))  init_macroblock_Bframe(img);
  else                                                  init_macroblock       (img);

  if (IS_DIRECT (currMB) && img->cod_counter >= 0)
  {
    int i, j, iii, jjj;
    currMB->cbp = 0;
    for (i=0;i<BLOCK_SIZE;i++)
    { // reset luma coeffs
      for (j=0;j<BLOCK_SIZE;j++)
        for(iii=0;iii<BLOCK_SIZE;iii++)
          for(jjj=0;jjj<BLOCK_SIZE;jjj++)
            img->cof[i][j][iii][jjj]=0;
    }
    for (j=4;j<6;j++)
    { // reset chroma coeffs
      for (i=0;i<4;i++)
        for (iii=0;iii<4;iii++)
          for (jjj=0;jjj<4;jjj++)
            img->cof[i][j][iii][jjj]=0;
    }
    if (inp->symbol_mode==CABAC)
      img->cod_counter=-1;

    for (i=0; i < 4; i++)
      for (j=0; j < 6; j++)
        img->nz_coeff[img->mb_x ][img->mb_y][i][j]=0;  // CAVLC

    return DECODE_MB;
  }

  if (IS_COPY (currMB)) //keep last macroblock
  {
    int i, j, iii, jjj, pmv[2];
    int ***tmp_mv         = img->mv;
    int mb_available_up   = (img->mb_y == 0)  ? 0 : (currMB->slice_nr == img->mb_data[img->current_mb_nr-img->width/16].slice_nr);
    int mb_available_left = (img->mb_x == 0)  ? 0 : (currMB->slice_nr == img->mb_data[img->current_mb_nr-1].slice_nr);
    int zeroMotionAbove   = !mb_available_up  ? 1 : refFrArr[img->block_y-1][img->block_x]  == 0 && tmp_mv[4+img->block_x  ][img->block_y-1][0] == 0 && tmp_mv[4+img->block_x  ][img->block_y-1][1] == 0 ? 1 : 0;
    int zeroMotionLeft    = !mb_available_left? 1 : refFrArr[img->block_y][img->block_x-1]  == 0 && tmp_mv[4+img->block_x-1][img->block_y  ][0] == 0 && tmp_mv[4+img->block_x-1][img->block_y  ][1] == 0 ? 1 : 0;

    mb_available_up   = (img->mb_y == 0)  ? 0 : 1;
    mb_available_left   = (img->mb_x == 0)  ? 0 : 1;
    zeroMotionAbove   = !mb_available_up  ? 1 : refFrArr[img->block_y-1][img->block_x]  == 0 && tmp_mv[4+img->block_x  ][img->block_y-1][0] == 0 && tmp_mv[4+img->block_x  ][img->block_y-1][1] == 0 ? 1 : 0;;
    zeroMotionLeft   = !mb_available_left  ? 1 : refFrArr[img->block_y][img->block_x-1]  == 0 && tmp_mv[4+img->block_x-1][img->block_y  ][0] == 0 && tmp_mv[4+img->block_x-1][img->block_y  ][1] == 0 ? 1 : 0;


    currMB->cbp = 0;

    for (i=0;i<BLOCK_SIZE;i++)
    { // reset luma coeffs
      for (j=0;j<BLOCK_SIZE;j++)
        for(iii=0;iii<BLOCK_SIZE;iii++)
          for(jjj=0;jjj<BLOCK_SIZE;jjj++)
            img->cof[i][j][iii][jjj]=0;
    }
    for (j=4;j<6;j++)
    { // reset chroma coeffs
      for (i=0;i<4;i++)
        for (iii=0;iii<4;iii++)
          for (jjj=0;jjj<4;jjj++)
            img->cof[i][j][iii][jjj]=0;
    }

    for (i=0; i < 4; i++)
      for (j=0; j < 6; j++)
        img->nz_coeff[img->mb_x ][img->mb_y][i][j]=0;  // CAVLC

    img_block_y   = img->block_y;
  
    if (img->mb_field && img->mb_frame_field_flag)
    {
      refFrArr = refFrArr_top;
      img->mv = img->mv_top;
      img_block_y   = img->block_y/2;
      mb_available_left = (img->mb_x == 0) ? 0 : 1;
      mb_available_up = (img->mb_y/2 == 0) ? 0 : 1;
      
      if (img->current_mb_nr%2) // bottom field
      {
        mb_available_up = ((img->mb_y-1)/2 == 0) ? 0 : 1;
        img_block_y   = (img->block_y-4)/2;
        img->mv = img->mv_bot;
        refFrArr = refFrArr_bot;
      }
      
      zeroMotionAbove   = !mb_available_up  ? 1 : refFrArr[img_block_y-1][img->block_x]  == 0 && img->mv[4+img->block_x  ][img_block_y-1][0] == 0 && img->mv[4+img->block_x  ][img_block_y-1][1] == 0 ? 1 : 0;
      zeroMotionLeft    = !mb_available_left? 1 : refFrArr[img_block_y][img->block_x-1]  == 0 && img->mv[4+img->block_x-1][img_block_y  ][0] == 0 && img->mv[4+img->block_x-1][img_block_y  ][1] == 0 ? 1 : 0;
      
      if(mb_available_up)
        zeroMotionAbove = field_mb[img->mb_y-1][img->mb_x] ? zeroMotionAbove:1; // if top MB Pair is field then motion MV ok, else set it to 1
      
      if(mb_available_left)
        zeroMotionLeft = field_mb[img->mb_y][img->mb_x-1] ? zeroMotionLeft:1;     // if left MB Pair is field then motion MV ok, else set it to 1
    }
    else if (img->mb_frame_field_flag)
    {
      if(mb_available_up)
        zeroMotionAbove = field_mb[img->mb_y-1][img->mb_x] ? 1:zeroMotionAbove; // if top MB Pair is frame then motion MV ok, else set it to 1
      
      if(mb_available_left)
        zeroMotionLeft = field_mb[img->mb_y][img->mb_x-1] ? 1:zeroMotionLeft;     // if left MB Pair is frame then motion MV ok, else set it to 1
    }
    
    if (zeroMotionAbove || zeroMotionLeft)
    {
      if(img->mb_frame_field_flag)
      {
        for(i=0;i<BLOCK_SIZE;i++)
          for(j=0;j<BLOCK_SIZE;j++)
          {
            img->mv_frm[img->block_x+i+BLOCK_SIZE][img->block_y+j][0] = 0;
            img->mv_frm[img->block_x+i+BLOCK_SIZE][img->block_y+j][1] = 0;
            
            if (img->current_mb_nr%2==0)
            {
              img_block_y   = img->block_y/2;
              img->mv_top[img->block_x+i+BLOCK_SIZE][img_block_y+j][0] = 0;
              img->mv_top[img->block_x+i+BLOCK_SIZE][img_block_y+j][1] = 0;
            }
            else
            {
              img_block_y   = (img->block_y-4)/2;
              img->mv_bot[img->block_x+i+BLOCK_SIZE][img_block_y+j][0] = 0;
              img->mv_bot[img->block_x+i+BLOCK_SIZE][img_block_y+j][1] = 0;
            }
          }
      }
      else
      {
        for(i=0;i<BLOCK_SIZE;i++)
          for(j=0;j<BLOCK_SIZE;j++)
          {
            img->mv[img->block_x+i+BLOCK_SIZE][img->block_y+j][0] = 0;
            img->mv[img->block_x+i+BLOCK_SIZE][img->block_y+j][1] = 0;
          }
      }
    }
    else
    {
      SetMotionVectorPredictor (img, pmv, pmv+1, 0, refFrArr, img->mv, 0, 0, 16, 16);

      for(i=0;i<BLOCK_SIZE;i++)
        for(j=0;j<BLOCK_SIZE;j++)
        {
          img->mv[img->block_x+i+BLOCK_SIZE][img_block_y+j][0] = pmv[0];
          img->mv[img->block_x+i+BLOCK_SIZE][img_block_y+j][1] = pmv[1];
        }

      for(i=0;i<BLOCK_SIZE;i++)
        for(j=0;j<BLOCK_SIZE;j++)
        {
          if (img->mb_field && img->mb_frame_field_flag)
          { 
            img->mv_frm[img->block_x+i+BLOCK_SIZE][img->block_y+j][0] = pmv[0];
            img->mv_frm[img->block_x+i+BLOCK_SIZE][img->block_y+j][1] = pmv[1]*2;
          }
          else if (img->mb_frame_field_flag && img->current_mb_nr%2==0)
          {
            img_block_y   = img->block_y/2;
            img->mv_top[img->block_x+i+BLOCK_SIZE][img_block_y+j][0] = pmv[0];
            img->mv_top[img->block_x+i+BLOCK_SIZE][img_block_y+j][1] = pmv[1]/2;
          }
          else if (img->mb_frame_field_flag && img->current_mb_nr%2)
          {
            img_block_y   = (img->block_y-4)/2;
            img->mv_bot[img->block_x+i+BLOCK_SIZE][img_block_y+j][0] = pmv[0];
            img->mv_bot[img->block_x+i+BLOCK_SIZE][img_block_y+j][1] = pmv[1]/2;
          }
        }
    }

    if(img->structure == FRAME)
    {
      for (j=0; j<BLOCK_SIZE;j++)
        for (i=0; i<BLOCK_SIZE;i++)
        {
          refFrArr_frm[img->block_y+j][img->block_x+i] = 0;
          if (img->current_mb_nr%2==0 && img->mb_frame_field_flag)
          {
            img_block_y   = img->block_y/2;
            refFrArr_top[img_block_y+j][img->block_x+i] = 0;
          }
          else if(img->mb_frame_field_flag)
          {
            img_block_y   = (img->block_y-4)/2;
            refFrArr_bot[img_block_y+j][img->block_x+i] = 0;
          }
        }
    }
    else
    {
      for (j=0; j<BLOCK_SIZE;j++)
        for (i=0; i<BLOCK_SIZE;i++)
          refFrArr[img->block_y+j][img->block_x+i] = 0;
    }

    return DECODE_MB;
  }


  // intra prediction modes for a macroblock 4x4 **********************************************
  read_ipred_modes(img,inp);    //changed this completely and moved it in the function below.


  /* read inter frame vector data *********************************************************/
  if (IS_INTERMV (currMB))
  {
    readMotionInfoFromNAL (img, inp);
  }


  // read CBP and Coeffs  ***************************************************************
  readCBPandCoeffsFromNAL (img,inp);

  return DECODE_MB;
}

void read_ipred_modes(struct img_par *img,struct inp_par *inp)
{
  int b8,bs_x,bs_y,bbs_x,bbs_y,i,j,ii,jj,bi,bj,dec;
  SyntaxElement currSE;
  Slice *currSlice;
  DataPartition *dP;
  int *partMap;
  Macroblock *currMB;
  int ts, ls;
  int j2, dec1;
  int mostProbableIntraPredMode;
  int upIntraPredMode;
  int leftIntraPredMode;
  int mapTab[9] = {2, 0, 1, 4, 3, 5, 7, 8, 6};
  int revMapTab[9] = {1, 2, 0, 4, 3, 5, 8, 6, 7};
  
  currMB=img->mb_data+img->current_mb_nr;

  currSlice = img->currentSlice;
  partMap = assignSE2partition[currSlice->dp_mode];

  currSE.type = SE_INTRAPREDMODE;
#if TRACE
  strncpy(currSE.tracestring, "Ipred Mode", TRACESTRING_SIZE);
#endif

  if(img->type == B_IMG_1 || img->type == B_IMG_MULT)     dP = &(currSlice->partArr[partMap[SE_BFRAME]]);
  else                                                    dP = &(currSlice->partArr[partMap[currSE.type]]);

  if (!(inp->symbol_mode == UVLC || dP->bitstream->ei_flag)) currSE.reading = readIntraPredModeFromBuffer_CABAC;

  for(b8=0;b8<4;b8++)  //loop 8x8 blocks
  {
    if( currMB->b8mode[b8]==IBLOCK )
    {
      bs_x=bs_y=4;
      if(currMB->useABT[b8])        // need support for MBINTLC1
      {
        bs_x=ABT_TRSIZE[currMB->abt_mode[b8]][0];
        bs_y=ABT_TRSIZE[currMB->abt_mode[b8]][1];
      }
      bbs_x = (bs_x>>2);    // bug fix for solaris. mwi
      bbs_y = (bs_y>>2);    // bug fix for solaris. mwi
      for(j=0;j<2;j+=bbs_y)  //loop subblocks
        for(i=0;i<2;i+=bbs_x)
        {
          //get from stream
          if (inp->symbol_mode == UVLC || dP->bitstream->ei_flag)
            readSyntaxElement_Intra4x4PredictionMode(&currSE,img,inp,dP);
          else {
            currSE.context=(b8<<2)+(j<<1)+i;
            dP->readSyntaxElement(&currSE,img,inp,dP);
          }

          //get from array and decode
          bi = img->block_x + ((b8&1)<<1) + i ;
          bj = img->block_y +    (b8&2)   + j ;

          ts=ls=0;   // Check to see if the neighboring block is SI
          if (IS_OLDINTRA(currMB) && img->type == SI_IMG)           // need support for MBINTLC1
          {
            if (bi==img->block_x && img->mb_x>0)
              if (img->siblock [img->mb_x-1][img->mb_y])
                ls=1;

            if (bj==img->block_y && img->mb_y>0)
              if (img->siblock [img->mb_x][img->mb_y-1])
                ts=1;
          }

          upIntraPredMode            = ts == 0 ? img->ipredmode[bi+1][bj] : DC_PRED;
          leftIntraPredMode          = ls == 0 ? img->ipredmode[bi][bj+1] : DC_PRED;
          mostProbableIntraPredMode  = (upIntraPredMode < 0 || leftIntraPredMode < 0) ? DC_PRED : mapTab[upIntraPredMode] < mapTab[leftIntraPredMode] ? upIntraPredMode : leftIntraPredMode;

          dec = (currSE.value1 == -1) ? mostProbableIntraPredMode : revMapTab[currSE.value1 + (currSE.value1 >= mapTab[mostProbableIntraPredMode])];

          //set
          for(jj=0;jj<(bs_y>>2);jj++)   //loop 4x4s in the subblock
            for(ii=0;ii<(bs_x>>2);ii++)
            {
              img->ipredmode[1+bi+ii][1+bj+jj]=dec;
//              if(jj||ii)currMB->intra_pred_modes[(b8<<2)|((j+jj)<<1)|(i+ii)]=dec;
            }
          if (img->mb_frame_field_flag)
            j2 = img->block_y / 2 +    (b8&2)   + j - 2*(img->current_mb_nr%2);
          else
            j2 = bj;

          if (img->mb_field && img->mb_frame_field_flag)
          {
            if (img->current_mb_nr %2 ==0)
            {
              upIntraPredMode            = ts == 0 ? img->ipredmode_top[bi+1][j2] : DC_PRED;
              leftIntraPredMode          = ls == 0 ? img->ipredmode_top[bi][j2+1] : DC_PRED;
              mostProbableIntraPredMode  = (upIntraPredMode < 0 || leftIntraPredMode < 0) ? DC_PRED : mapTab[upIntraPredMode] < mapTab[leftIntraPredMode] ? upIntraPredMode : leftIntraPredMode;

              dec1 = (currSE.value1 == -1) ? mostProbableIntraPredMode : revMapTab[currSE.value1 + (currSE.value1 >= mapTab[mostProbableIntraPredMode])];

              for(jj=0;jj<(bs_y>>2);jj++)   //loop 4x4s in the subblock
                for(ii=0;ii<(bs_x>>2);ii++)
                {
                  img->ipredmode_top[1+bi+ii][1+j2+jj]=dec1;
                  img->ipredmode[1+bi+ii][1+bj+jj] = img->ipredmode_top[1+bi+ii][1+j2+jj];
                }
            }
            else
            {
              upIntraPredMode            = ts == 0 ? img->ipredmode_bot[bi+1][j2] : DC_PRED;
              leftIntraPredMode          = ls == 0 ? img->ipredmode_bot[bi][j2+1] : DC_PRED;
              mostProbableIntraPredMode  = (upIntraPredMode < 0 || leftIntraPredMode < 0) ? DC_PRED : mapTab[upIntraPredMode] < mapTab[leftIntraPredMode] ? upIntraPredMode : leftIntraPredMode;

              dec1 = (currSE.value1 == -1) ? mostProbableIntraPredMode : revMapTab[currSE.value1 + (currSE.value1 >= mapTab[mostProbableIntraPredMode])];

              for(jj=0;jj<(bs_y>>2);jj++)   //loop 4x4s in the subblock
                for(ii=0;ii<(bs_x>>2);ii++)
                {
                  img->ipredmode_bot[1+bi+ii][1+j2+jj]=dec1;
                  img->ipredmode[1+bi+ii][1+bj+jj] = img->ipredmode_bot[1+bi+ii][1+j2+jj];
                }
            }
          }
          else if (img->mb_frame_field_flag)
          {
            if (img->mb_y < 2)      // to match encoder, address frame/field mismatch
            {
              for(jj=0;jj<(bs_y>>2);jj++)   //loop 4x4s in the subblock
                for(ii=0;ii<(bs_x>>2);ii++)
                {
                    img->ipredmode_top[1+bi+ii][1+j2+jj] = 0;
                    img->ipredmode_bot[1+bi+ii][1+j2+jj] = 0;
                }
            }
            else
            {
              if (img->current_mb_nr%2==0)
              {
                for(jj=0;jj<(bs_y>>2);jj++) //loop 4x4s in the subblock
                  for(ii=0;ii<(bs_x>>2);ii++)
                    img->ipredmode_top[1+bi+ii][1+j2+jj] = img->ipredmode[1+bi+ii][1+bj+jj];                
              }
              else
              {
                for(jj=0;jj<(bs_y>>2);jj++) //loop 4x4s in the subblock
                  for(ii=0;ii<(bs_x>>2);ii++)
                    img->ipredmode_bot[1+bi+ii][1+j2+jj] = img->ipredmode[1+bi+ii][1+bj+jj];    
              }
            }
          }
        }
    }
  }
}



/*!
 ************************************************************************
 * \brief
 *    Set motion vector predictor
 ************************************************************************
 */
void
SetMotionVectorPredictor (struct img_par  *img,
                          int             *pmv_x,
                          int             *pmv_y,
                          int             ref_frame,
                          int             **refFrArr,
                          int             ***tmp_mv,
                          int             block_x,
                          int             block_y,
                          int             blockshape_x,
                          int             blockshape_y)
{
  int mb_x                 = 4*block_x;
  int mb_y                 = 4*block_y;
  int pic_block_x          = img->block_x + block_x;
  int pic_block_y          = img->block_y + block_y;
  int mb_width             = img->width/16;
  int mb_available_up =      (img->mb_y == 0        ) ? 0 : 1;
  int mb_available_left =    (img->mb_x == 0        ) ? 0 : 1;
  int mb_available_upleft  = (img->mb_x == 0          ||
                              img->mb_y == 0        ) ? 0 : 1;
  int mb_available_upright = (img->mb_x >= mb_width-1 ||
                              img->mb_y == 0        ) ? 0 : 1;

  int block_available_up, block_available_left, block_available_upright, block_available_upleft;
  int mv_a, mv_b, mv_c, mv_d, pred_vec=0;
  int mvPredType, rFrameL, rFrameU, rFrameUR;
  int hv;


  if (img->structure==FRAME && img->mb_field)
  {
    if (img->current_mb_nr%2==0)    // top field
    {
      if (!(img->type==B_IMG_1 || img->type==B_IMG_MULT))
        tmp_mv             = img->mv_top;
      pic_block_x          = img->block_x + (mb_x>>2);
      pic_block_y          = img->block_y/2 + (mb_y>>2);
      mb_available_up =      (img->mb_y == 0        ) ? 0 : 1;
      mb_available_left =    (img->mb_x == 0        ) ? 0 : 1;
      mb_available_upleft  = (img->mb_x == 0          ||
                              img->mb_y == 0        ) ? 0 : 1;
      mb_available_upright = (img->mb_x >= mb_width-1 ||
                              img->mb_y == 0        ) ? 0 : 1;
    }
    else
    {
      if (!(img->type==B_IMG_1 || img->type==B_IMG_MULT))
        tmp_mv             = img->mv_bot;
      pic_block_x          = img->block_x + (mb_x>>2);
      pic_block_y          = (img->block_y-4)/2 + (mb_y>>2);
      mb_available_up =      (img->mb_y == 1        ) ? 0 : 1;
      mb_available_left =    (img->mb_x == 0        ) ? 0 : 1;
      mb_available_upleft  = (img->mb_x == 0          ||
                              img->mb_y == 1        ) ? 0 : 1;
      mb_available_upright = 0;
    }
  }
  else
  {
    mb_available_up =      (img->mb_y == 0        ) ? 0 : 1;
    mb_available_left =    (img->mb_x == 0        ) ? 0 : 1;
    mb_available_upleft  = (img->mb_x == 0          ||
                              img->mb_y == 0        ) ? 0 : 1;
    mb_available_upright = (img->mb_x >= mb_width-1 ||
                              img->mb_y == 0        ) ? 0 : 1;
    if(img->mb_frame_field_flag)
      mb_available_upright = (img->mb_y%2) ? 0:mb_available_upright;
  }

  /* D B C */
  /* A X   */

  /* 1 A, B, D are set to 0 if unavailable       */
  /* 2 If C is not available it is replaced by D */
  block_available_up   = mb_available_up   || (mb_y > 0);
  block_available_left = mb_available_left || (mb_x > 0);

  if (mb_y > 0)
  {
    if (mb_x < 8)  // first column of 8x8 blocks
    {
      if (mb_y==8)
      {
        if (blockshape_x == 16)      block_available_upright = 0;
        else                         block_available_upright = 1;
      }
      else
      {
        if (mb_x+blockshape_x != 8)  block_available_upright = 1;
        else                         block_available_upright = 0;
      }
    }
    else
    {
      if (mb_x+blockshape_x != 16)   block_available_upright = 1;
      else                           block_available_upright = 0;
    }
  }
  else if (mb_x+blockshape_x != MB_BLOCK_SIZE)
  {
    block_available_upright = block_available_up;
  }
  else
  {
    block_available_upright = mb_available_upright;
  }

  if (mb_x > 0)
  {
    block_available_upleft = (mb_y > 0 ? 1 : mb_available_up);
  }
  else if (mb_y > 0)
  {
    block_available_upleft = mb_available_left;
  }
  else
  {
    block_available_upleft = mb_available_upleft;
  }


  mvPredType = MVPRED_MEDIAN;
  rFrameL    = block_available_left    ? refFrArr[pic_block_y]  [pic_block_x-1             ] : -1;
  rFrameU    = block_available_up      ? refFrArr[pic_block_y-1][pic_block_x               ]   : -1;
  rFrameUR   = block_available_upright ? refFrArr[pic_block_y-1][pic_block_x+blockshape_x/4] :
               block_available_upleft  ? refFrArr[pic_block_y-1][pic_block_x-1             ] : -1;


  /* Prediction if only one of the neighbors uses the reference frame
   * we are checking
   */
  if(rFrameL == ref_frame && rFrameU != ref_frame && rFrameUR != ref_frame)       mvPredType = MVPRED_L;
  else if(rFrameL != ref_frame && rFrameU == ref_frame && rFrameUR != ref_frame)  mvPredType = MVPRED_U;
  else if(rFrameL != ref_frame && rFrameU != ref_frame && rFrameUR == ref_frame)  mvPredType = MVPRED_UR;
  // Directional predictions 
  else if(blockshape_x == 8 && blockshape_y == 16)
  {
    if(mb_x == 0)
    {
      if(rFrameL == ref_frame)
        mvPredType = MVPRED_L;
    }
    else
    {
      if(rFrameUR == ref_frame)
        mvPredType = MVPRED_UR;
    }
  }
  else if(blockshape_x == 16 && blockshape_y == 8)
  {
    if(mb_y == 0)
    {
      if(rFrameU == ref_frame)
        mvPredType = MVPRED_U;
    }
    else
    {
      if(rFrameL == ref_frame)
        mvPredType = MVPRED_L;
    }
  }

  for (hv=0; hv < 2; hv++)
  {
    mv_a = block_available_left    ? tmp_mv[4+pic_block_x-1             ][pic_block_y  ][hv] : 0;
    mv_b = block_available_up      ? tmp_mv[4+pic_block_x               ][pic_block_y-1][hv] : 0;
    mv_d = block_available_upleft  ? tmp_mv[4+pic_block_x-1             ][pic_block_y-1][hv] : 0;
    mv_c = block_available_upright ? tmp_mv[4+pic_block_x+blockshape_x/4][pic_block_y-1][hv] : mv_d;


    switch (mvPredType)
    {
    case MVPRED_MEDIAN:
      if(!(block_available_upleft || block_available_up || block_available_upright))
        pred_vec = mv_a;
      else
        pred_vec = mv_a+mv_b+mv_c-min(mv_a,min(mv_b,mv_c))-max(mv_a,max(mv_b,mv_c));
      break;
    case MVPRED_L:
      pred_vec = mv_a;
      break;
    case MVPRED_U:
      pred_vec = mv_b;
      break;
    case MVPRED_UR:
      pred_vec = mv_c;
      break;
    default:
      break;
    }

    if (hv==0)  *pmv_x = pred_vec;
    else        *pmv_y = pred_vec;

  }
  if (img->structure==FRAME && img->mb_field)
    tmp_mv = img->mv_frm;
}


/*!
 ************************************************************************
 * \brief
 *    Set context for reference frames
 ************************************************************************
 */
int
BType2CtxRef (int btype)
{
  if (btype<4)  return 0;
  else          return 1;
}

/*!
 ************************************************************************
 * \brief
 *    Read motion info
 ************************************************************************
 */
void readMotionInfoFromNAL (struct img_par *img, struct inp_par *inp)
{
  int i,j,k,l,m;
  int step_h,step_v;
  int curr_mvd;
  int mb_nr           = img->current_mb_nr;
  Macroblock *currMB  = &img->mb_data[mb_nr];
  SyntaxElement currSE;
  Slice *currSlice    = img->currentSlice;
  DataPartition *dP;
  int *partMap        = assignSE2partition[inp->partition_mode];
  int bframe          = (img->type==B_IMG_1 || img->type==B_IMG_MULT);
  int partmode        = (IS_P8x8(currMB)?4:currMB->mb_type);
  int step_h0         = BLOCK_STEP [partmode][0];
  int step_v0         = BLOCK_STEP [partmode][1];

  int mv_mode, i0, j0, refframe;
  int pmv[2];
  int j4, i4, ii,jj;
  int vec;

  int iTRb,iTRp;
  int mv_scale;
  int frame_no_next_P, frame_no_B, delta_P;
  int ref;
  int img_block_y;
  int use_scaled_mv;
  int fw_refframe,current_tr;

  int **fwRefFrArr = img->fw_refFrArr;
  int **bwRefFrArr = img->bw_refFrArr;
  int  ***fw_mv = img->fw_mv;
  int  ***bw_mv = img->bw_mv;
  int  **moving_block_dir = moving_block; 
  int  ***fw_mv_array, ***bw_mv_array;
  int j6;  

  if (bframe && IS_P8x8 (currMB))
  {
    if (img->direct_type && img->mb_frame_field_flag )
    {
      if (!img->mb_field)
      {
        fwRefFrArr= img->fw_refFrArr_frm;
        bwRefFrArr= img->bw_refFrArr_frm;
        fw_mv=img->fw_mv_frm;
        bw_mv=img->bw_mv_frm;
        fw_mv_array = img->dfMV;
        bw_mv_array = img->dbMV;
      }
      else if (img->current_mb_nr%2 )
      {
        fwRefFrArr= img->fw_refFrArr_bot;
        bwRefFrArr= img->bw_refFrArr_bot;
        fw_mv=img->fw_mv_bot;
        bw_mv=img->bw_mv_bot;
        moving_block_dir = moving_block_bot; 
        fw_mv_array = img->dfMV_bot;
        bw_mv_array = img->dbMV_bot;
      }
      else
      {
        fwRefFrArr=img->fw_refFrArr_top ;
        bwRefFrArr=img->bw_refFrArr_top ;
        fw_mv=img->fw_mv_top;
        bw_mv=img->bw_mv_top;
        moving_block_dir = moving_block_top; 
        fw_mv_array = img->dfMV_top;
        bw_mv_array = img->dbMV_top;
      }
    }
    if (img->direct_type)
    {
      int pic_blockx          = img->block_x;
      int pic_blocky          = (img->mb_frame_field_flag && img->mb_field)? ((img->current_mb_nr%2)?img->block_y/2-BLOCK_SIZE / 2:img->block_y/2):img->block_y;
      int mb_nr                = img->current_mb_nr;
      int mb_width             = img->width/16;
      int mb_available_up =      (img->mb_y == 0 || pic_blocky == 0  ) ? 0 : (img->mb_frame_field_flag? 1 :(currMB->slice_nr == img->mb_data[mb_nr-mb_width].slice_nr));
      int mb_available_left =    (img->mb_x == 0                      ) ? 0 : (currMB->slice_nr == img->mb_data[mb_nr-1].slice_nr);
      int mb_available_upleft  = (img->mb_x == 0 || img->mb_y == 0 || pic_blocky == 0) ? 0 : (img->mb_frame_field_flag)? 1 :(currMB->slice_nr == img->mb_data[mb_nr-mb_width-1].slice_nr);
      int mb_available_upright = (img->mb_frame_field_flag && img->current_mb_nr%2)?0:(img->mb_x >= mb_width-1 ||
        img->mb_y == 0 || pic_blocky == 0 ) ? 0 : (img->mb_frame_field_flag)? 1 :(currMB->slice_nr == img->mb_data[mb_nr-mb_width+1].slice_nr);
      
      
      int fw_rFrameL              = mb_available_left    ? fwRefFrArr[pic_blocky]  [pic_blockx-1]   : -1;
      int fw_rFrameU              = mb_available_up      ? fwRefFrArr[pic_blocky-1][pic_blockx]     : -1;
      int fw_rFrameUL             = mb_available_upleft  ? fwRefFrArr[pic_blocky-1][pic_blockx-1]   : -1;
      int fw_rFrameUR             = mb_available_upright ? fwRefFrArr[pic_blocky-1][pic_blockx+4]   : fw_rFrameUL;  
      
      int bw_rFrameL              = mb_available_left    ? bwRefFrArr[pic_blocky]  [pic_blockx-1]   : -1;
      int bw_rFrameU              = mb_available_up      ? bwRefFrArr[pic_blocky-1][pic_blockx]     : -1;
      int bw_rFrameUL             = mb_available_upleft  ? bwRefFrArr[pic_blocky-1][pic_blockx-1]   : -1;
      int bw_rFrameUR             = mb_available_upright ? bwRefFrArr[pic_blocky-1][pic_blockx+4]   : bw_rFrameUL;
      
      
      int fw_rFrame,bw_rFrame;
      int pmvfw[2]={0,0},pmvbw[2]={0,0};
      int j5=0;
      
      if (!fw_rFrameL || !fw_rFrameU || !fw_rFrameUR)
        fw_rFrame=0;
      else
        fw_rFrame=min(fw_rFrameL&15,min(fw_rFrameU&15,fw_rFrameUR&15));
      
      if(img->num_ref_pic_active_bwd>1 && (bw_rFrameL==1 || bw_rFrameU==1 || bw_rFrameUR==1))
        bw_rFrame=1;
      else if (!bw_rFrameL || !bw_rFrameU || !bw_rFrameUR)
        bw_rFrame=0;
      else
        bw_rFrame=min(bw_rFrameL&15,min(bw_rFrameU&15,bw_rFrameUR&15));
      
      if (fw_rFrame !=15)
        SetMotionVectorPredictor (img, pmvfw, pmvfw+1, fw_rFrame, fwRefFrArr, fw_mv, 0, 0, 16, 16);  
      if (bw_rFrame !=15)
        SetMotionVectorPredictor (img, pmvbw, pmvbw+1, bw_rFrame, bwRefFrArr, bw_mv, 0, 0, 16, 16);  
      
      for (i=0;i<4;i++)
      {
        if (currMB->b8mode[i] == 0)
          for(j=2*(i/2);j<2*(i/2)+2;j++)
            for(k=2*(i%2);k<2*(i%2)+2;k++)
            {
              j4 = img->block_y+j;
              j6 = pic_blocky+j;
              i4 = img->block_x+k;
              if (img->mb_frame_field_flag) 
              {
                j5 = img->block_y / 2 + j;
                if (img->current_mb_nr%2)
                  j5 -= BLOCK_SIZE / 2;
              }
              if (!(img->mb_frame_field_flag && img->mb_field))
              {
                if (fw_rFrame !=15)
                {
                  if  (!fw_rFrame  && !moving_block_dir[j6][i4])       
                  {                    
                    img->fw_mv[i4+BLOCK_SIZE][j4][0]=img->dfMV[i4+BLOCK_SIZE][j4][0] = 0;
                    img->fw_mv[i4+BLOCK_SIZE][j4][1]=img->dfMV[i4+BLOCK_SIZE][j4][1] = 0;
                    if (img->mb_frame_field_flag)
                    {
                      if (img->current_mb_nr%2 == 0)
                      {
                        img->dfMV_top[i4+BLOCK_SIZE][j5][0]=img->fw_mv_top[i4+BLOCK_SIZE][j5][0]=0;
                        img->dfMV_top[i4+BLOCK_SIZE][j5][1]=img->fw_mv_top[i4+BLOCK_SIZE][j5][1]=0;
                        img->fw_refFrArr_top[j5][i4]=0;                        
                      }
                      else
                      {
                        img->dfMV_bot[i4+BLOCK_SIZE][j5][0]=img->fw_mv_bot[i4+BLOCK_SIZE][j5][0]=0;
                        img->dfMV_bot[i4+BLOCK_SIZE][j5][1]=img->fw_mv_bot[i4+BLOCK_SIZE][j5][1]=0;
                        img->fw_refFrArr_bot[j5][i4]=0;
                      }
                    }                    
                    if (img->structure == TOP_FIELD)  //! Note that this seems to be unecessary for different img->structure.
                      fwRefFrArr[j4][i4] = 0;         //! copied to be consistent with temporal direct, just in case there is 
                    else                              //! change in the software.
                      fwRefFrArr[j4][i4] = 0;                                      
                  }
                  else
                  {
                    img->fw_mv[i4+BLOCK_SIZE][j4][0]=img->dfMV[i4+BLOCK_SIZE][j4][0] = pmvfw[0];
                    img->fw_mv[i4+BLOCK_SIZE][j4][1]=img->dfMV[i4+BLOCK_SIZE][j4][1] = pmvfw[1];
                    if (img->structure == TOP_FIELD)
                      fwRefFrArr[j4][i4] = fw_rFrame ;
                    else                
                      fwRefFrArr[j4][i4] = fw_rFrame;                
                    if (img->mb_frame_field_flag)
                    {
                      if (img->current_mb_nr%2 == 0)
                      {
                        img->dfMV_top[i4+BLOCK_SIZE][j5][0]=img->fw_mv_top[i4+BLOCK_SIZE][j5][0]=pmvfw[0];
                        img->dfMV_top[i4+BLOCK_SIZE][j5][1]=img->fw_mv_top[i4+BLOCK_SIZE][j5][1]=pmvfw[1]/2;
                        img->fw_refFrArr_top[j5][i4]=2*fw_rFrame;
                      }
                      else
                      {
                        img->dfMV_bot[i4+BLOCK_SIZE][j5][0]=img->fw_mv_bot[i4+BLOCK_SIZE][j5][0]=pmvfw[0];
                        img->dfMV_bot[i4+BLOCK_SIZE][j5][1]=img->fw_mv_bot[i4+BLOCK_SIZE][j5][1]=pmvfw[1]/2;
                        img->fw_refFrArr_bot[j5][i4]=2*fw_rFrame;
                      }
                    }
                  }
                }
                else 
                {
                  img->fw_refFrArr[j4][i4]=-1;                                   
                  img->fw_mv[i4+BLOCK_SIZE][j4][0]=img->dfMV[i4+BLOCK_SIZE][j4][0] = 0;
                  img->fw_mv[i4+BLOCK_SIZE][j4][1]=img->dfMV[i4+BLOCK_SIZE][j4][1] = 0;
                  if (img->mb_frame_field_flag)
                  {
                    if (img->current_mb_nr%2 == 0)
                    {
                      img->dfMV_top[i4+BLOCK_SIZE][j5][0]=img->fw_mv_top[i4+BLOCK_SIZE][j5][0]=0;
                      img->dfMV_top[i4+BLOCK_SIZE][j5][1]=img->fw_mv_top[i4+BLOCK_SIZE][j5][1]=0;
                      img->fw_refFrArr_top[j5][i4]=-1;
                    }
                    else
                    {
                      img->dfMV_bot[i4+BLOCK_SIZE][j5][0]=img->fw_mv_bot[i4+BLOCK_SIZE][j5][0]=0;
                      img->dfMV_bot[i4+BLOCK_SIZE][j5][1]=img->fw_mv_bot[i4+BLOCK_SIZE][j5][1]=0;
                      img->fw_refFrArr_bot[j5][i4]=-1;
                    }
                  }
                }
                if (bw_rFrame !=15)
                {
                  if  (bw_rFrame==((img->num_ref_pic_active_bwd>1)?1:0) && !moving_block_dir[j6][i4])                         
                  {                  
                    img->bw_mv[i4+BLOCK_SIZE][j4][0]=img->dbMV[i4+BLOCK_SIZE][j4][0] = 0;
                    img->bw_mv[i4+BLOCK_SIZE][j4][1]=img->dbMV[i4+BLOCK_SIZE][j4][1] = 0;
                    bwRefFrArr[j4][i4]=bw_rFrame;
                    if (img->mb_frame_field_flag)
                    {
                      if (img->current_mb_nr%2 == 0)
                      {
                        img->dbMV_top[i4+BLOCK_SIZE][j5][0]=img->bw_mv_top[i4+BLOCK_SIZE][j5][0]=0;
                        img->dbMV_top[i4+BLOCK_SIZE][j5][1]=img->bw_mv_top[i4+BLOCK_SIZE][j5][1]=0;
                        img->bw_refFrArr_top[j5][i4]=2*bw_rFrame;
                      }
                      else
                      {
                        img->dbMV_bot[i4+BLOCK_SIZE][j5][0]=img->bw_mv_bot[i4+BLOCK_SIZE][j5][0]=0;
                        img->dbMV_bot[i4+BLOCK_SIZE][j5][1]=img->bw_mv_bot[i4+BLOCK_SIZE][j5][1]=0;
                        img->bw_refFrArr_bot[j5][i4]=2*bw_rFrame;
                      }
                    }
                  }
                  else
                  {                    
                    img->bw_mv[i4+BLOCK_SIZE][j4][0]=img->dbMV[i4+BLOCK_SIZE][j4][0] = pmvbw[0];
                    img->bw_mv[i4+BLOCK_SIZE][j4][1]=img->dbMV[i4+BLOCK_SIZE][j4][1] = pmvbw[1];
                    bwRefFrArr[j4][i4]=bw_rFrame;
                    if (img->mb_frame_field_flag)
                    {
                      if (img->current_mb_nr%2 == 0)
                      {
                        img->dbMV_top[i4+BLOCK_SIZE][j5][0]=img->bw_mv_top[i4+BLOCK_SIZE][j5][0]=pmvbw[0];
                        img->dbMV_top[i4+BLOCK_SIZE][j5][1]=img->bw_mv_top[i4+BLOCK_SIZE][j5][1]=pmvbw[1]/2;
                        img->bw_refFrArr_top[j5][i4]=2*bw_rFrame;
                      }
                      else
                      {
                        img->dbMV_bot[i4+BLOCK_SIZE][j5][0]=img->bw_mv_bot[i4+BLOCK_SIZE][j5][0]=pmvbw[0];
                        img->dbMV_bot[i4+BLOCK_SIZE][j5][1]=img->bw_mv_bot[i4+BLOCK_SIZE][j5][1]=pmvbw[1]/2;
                        img->bw_refFrArr_bot[j5][i4]=2*bw_rFrame;
                      }
                    }
                  }
                }
                else
                {                                                            
                  img->bw_mv[i4+BLOCK_SIZE][j4][0]=img->dbMV[i4+BLOCK_SIZE][j4][0] = 0;
                  img->bw_mv[i4+BLOCK_SIZE][j4][1]=img->dbMV[i4+BLOCK_SIZE][j4][1] = 0;
                  bwRefFrArr[j4][i4]=-1;                  
                  
                  if (img->mb_frame_field_flag)
                  {
                    if (img->current_mb_nr%2 == 0)
                    {
                      img->dbMV_top[i4+BLOCK_SIZE][j5][0]=img->bw_mv_top[i4+BLOCK_SIZE][j5][0]=0;
                      img->dbMV_top[i4+BLOCK_SIZE][j5][1]=img->bw_mv_top[i4+BLOCK_SIZE][j5][1]=0;
                      img->bw_refFrArr_top[j5][i4]=-1;
                    }
                    else
                    {
                      img->dbMV_bot[i4+BLOCK_SIZE][j5][0]=img->bw_mv_bot[i4+BLOCK_SIZE][j5][0]=0;
                      img->dbMV_bot[i4+BLOCK_SIZE][j5][1]=img->bw_mv_bot[i4+BLOCK_SIZE][j5][1]=0;
                      img->bw_refFrArr_bot[j5][i4]=-1;
                    }
                  }
                }
                if (fw_rFrame ==15 && bw_rFrame ==15)
                {
                  if (img->structure == TOP_FIELD)
                    fwRefFrArr[j4][i4] =  0;
                  else                
                    fwRefFrArr[j4][i4]=0;                
                  bwRefFrArr[j4][i4] =(img->num_ref_pic_active_bwd>1)?1:0;
                  
                  if (img->mb_frame_field_flag)
                  {
                    if (img->current_mb_nr%2 == 0)
                    {
                      img->fw_refFrArr_top[j5][i4]=0;
                      img->bw_refFrArr_top[j5][i4]=2*bwRefFrArr[j4][i4];
                    }
                    else
                    {
                      img->fw_refFrArr_bot[j5][i4]=0;                        
                      img->bw_refFrArr_bot[j5][i4]=2*bwRefFrArr[j4][i4];
                    }
                  }
                }
                
                if (img->mb_frame_field_flag)
                {
                  
                  img->fw_mv_frm[i4+BLOCK_SIZE][j4][0]=img->fw_mv[i4+BLOCK_SIZE][j4][0];
                  img->fw_mv_frm[i4+BLOCK_SIZE][j4][1]=img->fw_mv[i4+BLOCK_SIZE][j4][1];
                  img->bw_mv_frm[i4+BLOCK_SIZE][j4][0]=img->bw_mv[i4+BLOCK_SIZE][j4][0];
                  img->bw_mv_frm[i4+BLOCK_SIZE][j4][1]=img->bw_mv[i4+BLOCK_SIZE][j4][1];
                  if (img->mb_field)
                  {
                    img->fw_refFrArr_frm[img->block_y+j][img->block_x+i] = (fwRefFrArr[j4][i4]==-1) ? -1 : (fwRefFrArr[j4][i4] + 1)/2;
                    img->bw_refFrArr_frm[img->block_y+j][img->block_x+i] = (bwRefFrArr[j4][i4]==-1) ? -1 : (bwRefFrArr[j4][i4] + 1)/2;
                    // Revisit Alexis   img->bw_refFrArr_frm[img->block_y + j][img->block_x + k] = -1;
                  } 
                }
              }
              else //! (img->mb_frame_field_flag && img->mb_field)
              {
                if (fw_rFrame !=15)
                {
                  if  (!fw_rFrame  && !moving_block_dir[j5][i4])       
                  {                    
                    fwRefFrArr[j5][i4] = 0;
                    fw_mv_array[i4+BLOCK_SIZE][j5][0]=fw_mv[i4+BLOCK_SIZE][j5][0]=0;
                    fw_mv_array[i4+BLOCK_SIZE][j5][1]=fw_mv[i4+BLOCK_SIZE][j5][1]=0;
                    
                  }
                  else
                  {
                    fwRefFrArr[j5][i4] = fw_rFrame ;                    
                    fw_mv_array[i4+BLOCK_SIZE][j5][0]=fw_mv[i4+BLOCK_SIZE][j5][0]=pmvfw[0];
                    fw_mv_array[i4+BLOCK_SIZE][j5][1]=fw_mv[i4+BLOCK_SIZE][j5][1]=pmvfw[1];                    
                  }
                }
                else
                {
                  fwRefFrArr[j5][i4]=-1;
                  fw_mv_array[i4+BLOCK_SIZE][j5][0]=fw_mv[i4+BLOCK_SIZE][j5][0]=0;
                  fw_mv_array[i4+BLOCK_SIZE][j5][1]=fw_mv[i4+BLOCK_SIZE][j5][1]=0;
                }
                if (bw_rFrame !=15)
                {
                  
                  if  (bw_rFrame==((img->num_ref_pic_active_bwd>1)?1:0) && !moving_block_dir[j5][i4])                         
                  {
                    bwRefFrArr[j5][i4]=bw_rFrame;
                    bw_mv_array[i4+BLOCK_SIZE][j5][0]=bw_mv[i4+BLOCK_SIZE][j5][0]=0;
                    bw_mv_array[i4+BLOCK_SIZE][j5][1]=bw_mv[i4+BLOCK_SIZE][j5][1]=0;
                  }
                  else
                  {
                    bwRefFrArr[j5][i4]=bw_rFrame;
                    bw_mv_array[i4+BLOCK_SIZE][j5][0]=bw_mv[i4+BLOCK_SIZE][j5][0]=pmvbw[0];
                    bw_mv_array[i4+BLOCK_SIZE][j5][1]=bw_mv[i4+BLOCK_SIZE][j5][1]=pmvbw[1];
                  }
                }
                else
                {
                  bwRefFrArr[j5][i4]=-1;
                  bw_mv_array[i4+BLOCK_SIZE][j5][0]=bw_mv[i4+BLOCK_SIZE][j5][0]=0;
                  bw_mv_array[i4+BLOCK_SIZE][j5][1]=bw_mv[i4+BLOCK_SIZE][j5][1]=0;
                }
                if (fw_rFrame ==15 && bw_rFrame ==15)
                {
                  fwRefFrArr[j5][i4]=  0;
                  bwRefFrArr[j5][i4] =(img->num_ref_pic_active_bwd>1)?1:0;
                }
                if (fwRefFrArr[j5][i4]!=-1)
                  img->fw_refFrArr_frm[j4][i4]=fwRefFrArr[j5][i4]/2;
                else
                  img->fw_refFrArr_frm[j4][i4]=-1;
                
                if (bwRefFrArr[j5][i4]!=-1)
                  img->bw_refFrArr_frm[j4][i4]=bwRefFrArr[j5][i4]/2;
                else
                  img->bw_refFrArr_frm[j4][i4]=-1;

                img->fw_mv_frm[i4+BLOCK_SIZE][j4][0]=fw_mv[i4+BLOCK_SIZE][j5][0];
                img->fw_mv_frm[i4+BLOCK_SIZE][j4][1]=fw_mv[i4+BLOCK_SIZE][j5][1]*2;
                img->bw_mv_frm[i4+BLOCK_SIZE][j4][0]=bw_mv[i4+BLOCK_SIZE][j5][0];
                img->bw_mv_frm[i4+BLOCK_SIZE][j4][1]=bw_mv[i4+BLOCK_SIZE][j5][1]*2;
              }
            }
        }
    }
    else
    {
      for (i=0;i<4;i++)
      {
        if (currMB->b8mode[i] == 0)
        {
          for(j=2*(i/2);j<2*(i/2)+2;j++)
          {
            for(k=2*(i%2);k<2*(i%2)+2;k++)
            {
              if (img->mb_frame_field_flag && img->structure==FRAME)
              {
                if (img->mb_field==0)
                {
                  if (refFrArr_frm[img->block_y+j][img->block_x+k] == -1)
                  {
                    img->fw_refFrArr_frm[img->block_y + j][img->block_x + k] = -1;
                    img->bw_refFrArr_frm[img->block_y + j][img->block_x + k] = -1;
                  }
                  else
                  {
                    img->fw_refFrArr_frm[img->block_y + j][img->block_x + k] = refFrArr_frm[img->block_y+j][img->block_x+k];
                    img->bw_refFrArr_frm[img->block_y + j][img->block_x + k] = 0;
                  }
                }
                else if (img->current_mb_nr%2==0)
                {
                  img_block_y   = img->block_y/2;
                  if (refFrArr_top[img_block_y+j][img->block_x+k] == -1)
                  {
                    img->fw_refFrArr_top[img_block_y + j][img->block_x + k] = -1;
                    img->bw_refFrArr_top[img_block_y + j][img->block_x + k] = -1;
                  }
                  else
                  {
                    img->fw_refFrArr_top[img_block_y + j][img->block_x + k] = refFrArr_top[img_block_y+j][img->block_x+k];
                    img->bw_refFrArr_top[img_block_y + j][img->block_x + k] = 0;
                  }
                }
                else
                {
                  img_block_y   = (img->block_y-4)/2;
                  if (refFrArr_bot[img_block_y+j][img->block_x+k] == -1)
                  {
                    img->fw_refFrArr_bot[img_block_y + j][img->block_x + k] = -1;
                    img->bw_refFrArr_bot[img_block_y + j][img->block_x + k] = -1;
                  }
                  else
                  {
                    img->fw_refFrArr_bot[img_block_y + j][img->block_x + k] = refFrArr_bot[img_block_y+j][img->block_x+k];
                    img->bw_refFrArr_bot[img_block_y + j][img->block_x + k] = 0;
                  }
                }
              }
              else
              {
                if (refFrArr[img->block_y+j][img->block_x+k] == -1)
                {
                  img->fw_refFrArr[img->block_y + j][img->block_x + k] = -1;
                  img->bw_refFrArr[img->block_y + j][img->block_x + k] = -1;
                }
                else
                {
                  if (img->structure == TOP_FIELD)
                    img->fw_refFrArr[img->block_y + j][img->block_x + k] = refFrArr[img->block_y+j][img->block_x+k] + 0;    // PLUS2, Krit, 7/06 (used to be + 1)
                  else
                    img->fw_refFrArr[img->block_y + j][img->block_x + k] = refFrArr[img->block_y+j][img->block_x+k];
                  img->bw_refFrArr[img->block_y + j][img->block_x + k] = 0;
                }
              }
            }
          }
        }
      }
    }
  } 
  //  If multiple ref. frames, read reference frame for the MB *********************************
  if(img->num_ref_pic_active_fwd>1) 
  {
    currSE.type = SE_REFFRAME;
    if (bframe)                                               dP = &(currSlice->partArr[partMap[SE_BFRAME]]);
    else                                                      dP = &(currSlice->partArr[partMap[SE_REFFRAME]]);
    if (inp->symbol_mode == UVLC || dP->bitstream->ei_flag)   currSE.mapping = linfo;
    else                                                      currSE.reading = readRefFrameFromBuffer_CABAC;
    
    for (j0=0; j0<4; j0+=step_v0)
      for (i0=0; i0<4; i0+=step_h0)
      {
        k=2*(j0/2)+(i0/2);
        if ((currMB->b8pdir[k]==0 || currMB->b8pdir[k]==2) && currMB->b8mode[k]!=0)
        {
#if TRACE
          strncpy(currSE.tracestring,  "Reference frame no ", TRACESTRING_SIZE);
#endif
          img->subblock_x = i0;
          img->subblock_y = j0;
          
          if (!IS_P8x8 (currMB) || bframe || (!bframe && !img->allrefzero))
          {
            currSE.context = BType2CtxRef (currMB->b8mode[k]);
            dP->readSyntaxElement (&currSE,img,inp,dP);
            refframe = currSE.value1;
            
            if(img->structure!=FRAME) //favoring the same field
            {
              if(refframe % 2)
                refframe -= 1;
              else
                refframe += 1;
            }   //end of favoring the same field
          }
          else
          {
            refframe = 0;
          }
          
          if (bframe && refframe>img->buf_cycle)    // img->buf_cycle should be correct for field MBs now
          {
            set_ec_flag(SE_REFFRAME);
            refframe = 1;
            
          }
          
          if ((!bframe) && (img->structure==FRAME))
          {
            if (img->current_mb_nr%2==0 && img->mb_frame_field_flag)
            {
              if (img->mb_field==0)
              {
                img_block_y   = img->block_y/2;
                for (j=j0; j<j0+step_v0;j++)
                  for (i=i0; i<i0+step_h0;i++)
                  {
                    refFrArr_frm[img->block_y+j][img->block_x+i] = refframe;
                    refFrArr_top[img_block_y+j][img->block_x+i] = refframe==-1 ? -1 : 2*refframe;
                  }
              }
              else
              {
                img_block_y   = img->block_y/2;
                for (j=j0; j<j0+step_v0;j++)
                  for (i=i0; i<i0+step_h0;i++)
                  {
                    refFrArr_top[img_block_y+j][img->block_x+i] = refframe;
                    refFrArr_frm[img->block_y+j][img->block_x+i] = refframe==-1 ? -1 : refframe/2;
                  }
              }
            }
            else if (img->mb_frame_field_flag)
            {
              if (img->mb_field==0)
              {
                img_block_y   = (img->block_y-4)/2;
                for (j=j0; j<j0+step_v0;j++)
                  for (i=i0; i<i0+step_h0;i++)
                  {
                    refFrArr_frm[img->block_y+j][img->block_x+i] = refframe;
                    refFrArr_bot[img_block_y+j][img->block_x+i] = refframe==-1 ? -1 : 2*refframe;
                  }
              }
              else
              {
                img_block_y   = (img->block_y-4)/2;
                for (j=j0; j<j0+step_v0;j++)
                  for (i=i0; i<i0+step_h0;i++)
                  {
                    refFrArr_bot[img_block_y+j][img->block_x+i] = refframe;
                    refFrArr_frm[img->block_y+j][img->block_x+i] = refframe==-1 ? -1 : refframe/2;
                  }
              }
            }
            else
            {
              if (!bframe)
              {
                for (j=j0; j<j0+step_v0;j++)
                  for (i=i0; i<i0+step_h0;i++)
                    refFrArr[img->block_y+j][img->block_x+i] = refframe;
              }
            }
          }
          else if ((bframe) && (img->structure==FRAME))
          {
            if (img->current_mb_nr%2==0 && img->mb_frame_field_flag)
            {
              if (img->mb_field==0)
              {
                img_block_y   = img->block_y/2;
                for (j=j0; j<j0+step_v0;j++)
                  for (i=i0; i<i0+step_h0;i++)
                  {
                    img->fw_refFrArr_frm[img->block_y+j][img->block_x+i] = refframe;
                    img->fw_refFrArr_top[img_block_y+j][img->block_x+i] = refframe==-1 ? -1 : 2*refframe;
                  }
              }
              else
              {
                img_block_y   = img->block_y/2;
                for (j=j0; j<j0+step_v0;j++)
                  for (i=i0; i<i0+step_h0;i++)
                  {
                    img->fw_refFrArr_top[img_block_y+j][img->block_x+i] = refframe;
                    img->fw_refFrArr_frm[img->block_y+j][img->block_x+i] = refframe==-1 ? -1 : refframe/2;
                  }
              }
            }
            else if (img->mb_frame_field_flag)
            {
              if (img->mb_field==0)
              {
                img_block_y   = (img->block_y-4)/2;
                for (j=j0; j<j0+step_v0;j++)
                  for (i=i0; i<i0+step_h0;i++)
                  {
                    img->fw_refFrArr_frm[img->block_y+j][img->block_x+i] = refframe;
                    img->fw_refFrArr_bot[img_block_y+j][img->block_x+i] = refframe==-1 ? -1 : 2*refframe;
                  }
              }
              else
              {
                img_block_y   = (img->block_y-4)/2;
                for (j=j0; j<j0+step_v0;j++)
                  for (i=i0; i<i0+step_h0;i++)
                  {
                    img->fw_refFrArr_bot[img_block_y+j][img->block_x+i] = refframe;
                    img->fw_refFrArr_frm[img->block_y+j][img->block_x+i] = refframe==-1 ? -1 : refframe/2;
                  }
              }
            }
            else
            {
              for (j=j0; j<j0+step_v0;j++)
                for (i=i0; i<i0+step_h0;i++)
                  img->fw_refFrArr[img->block_y+j][img->block_x+i] = refframe;
            }
          }
          else if ((!bframe) && (img->structure!=FRAME))
          {
            for (j=j0; j<j0+step_v0;j++)
              for (i=i0; i<i0+step_h0;i++)
                refFrArr[img->block_y+j][img->block_x+i] = refframe;
          }
          else if ((bframe) && (img->structure!=FRAME))
          {
            for (j=j0; j<j0+step_v0;j++)
              for (i=i0; i<i0+step_h0;i++)
                img->fw_refFrArr[img->block_y+j][img->block_x+i] = refframe;
          }
      }
    }
  }
  else if(img->mb_frame_field_flag && !img->mb_field)
    SetOneRefMV(img);
  
  //  If backward multiple ref. frames, read backward reference frame for the MB *********************************
  if(img->num_ref_pic_active_bwd>1)
  {
    currSE.type = SE_BFRAME;
    dP = &(currSlice->partArr[partMap[SE_BFRAME]]);
    if (inp->symbol_mode == UVLC || dP->bitstream->ei_flag)   currSE.mapping = linfo;
    else                                                      currSE.reading = readBwdRefFrameFromBuffer_CABAC;
    
    for (j0=0; j0<4; j0+=step_v0)
      for (i0=0; i0<4; i0+=step_h0)
      {
        k=2*(j0/2)+(i0/2);
        if ((currMB->b8pdir[k]==1 || currMB->b8pdir[k]==2) && currMB->b8mode[k]!=0)
        {
#if TRACE
          strncpy(currSE.tracestring,  "Bwd Reference frame no ", TRACESTRING_SIZE);
#endif
          img->subblock_x = i0;
          img->subblock_y = j0;
          
          {
            currSE.context = BType2CtxRef (currMB->b8mode[k]);
            dP->readSyntaxElement (&currSE,img,inp,dP);
            refframe = currSE.value1;
            
            if(refframe<2) refframe = 1-refframe; // switch default index order
            
            if(img->structure!=FRAME) //favoring the same field
            {
              if(refframe % 2)
                refframe -= 1;
              else
                refframe += 1;
            }   //end of favoring the same field
          }
          
          if (img->structure==FRAME && img->mb_frame_field_flag)
          {
            if (img->current_mb_nr%2==0)
            {
              if (img->mb_field==0)
              {
                img_block_y   = img->block_y/2;
                for (j=j0; j<j0+step_v0;j++)
                  for (i=i0; i<i0+step_h0;i++)
                  {
                    img->bw_refFrArr_frm[img->block_y+j][img->block_x+i] = refframe;
                    img->bw_refFrArr_top[img_block_y+j][img->block_x+i] = refframe==-1 ? -1 : 2*refframe;
                  }
              }
              else
              {
                img_block_y   = img->block_y/2;
                for (j=j0; j<j0+step_v0;j++)
                  for (i=i0; i<i0+step_h0;i++)
                  {
                    img->bw_refFrArr_top[img_block_y+j][img->block_x+i] = refframe;
                    img->bw_refFrArr_frm[img->block_y+j][img->block_x+i] = refframe==-1 ? -1 : refframe/2;
                  }
              }
            }
            else
            {
              if (img->mb_field==0)
              {
                img_block_y   = (img->block_y-4)/2;
                for (j=j0; j<j0+step_v0;j++)
                  for (i=i0; i<i0+step_h0;i++)
                  {
                    img->bw_refFrArr_frm[img->block_y+j][img->block_x+i] = refframe;
                    img->bw_refFrArr_bot[img_block_y+j][img->block_x+i] = refframe==-1 ? -1 : 2*refframe;
                  }
              }
              else
              {
                img_block_y   = (img->block_y-4)/2;
                for (j=j0; j<j0+step_v0;j++)
                  for (i=i0; i<i0+step_h0;i++)
                  {
                    img->bw_refFrArr_bot[img_block_y+j][img->block_x+i] = refframe;
                    img->bw_refFrArr_frm[img->block_y+j][img->block_x+i] = refframe==-1 ? -1 : refframe/2;
                  }
              }
            }
          }
          else
          {
            for (j=j0; j<j0+step_v0;j++)
              for (i=i0; i<i0+step_h0;i++)
                img->bw_refFrArr[img->block_y+j][img->block_x+i] = refframe;
          }
        }
      }
  }

  //=====  READ FORWARD MOTION VECTORS =====
  currSE.type = SE_MVD;
  if (bframe)   dP = &(currSlice->partArr[partMap[SE_BFRAME]]);
  else          dP = &(currSlice->partArr[partMap[SE_MVD]]);

  if (inp->symbol_mode == UVLC || dP->bitstream->ei_flag) currSE.mapping = linfo_mvd;
  else if (bframe)                                        currSE.reading = readBiMVD2Buffer_CABAC;
  else                                                    currSE.reading = readMVDFromBuffer_CABAC;

  for (j0=0; j0<4; j0+=step_v0)
    for (i0=0; i0<4; i0+=step_h0)
    {
      k=2*(j0/2)+(i0/2);
      if ((currMB->b8pdir[k]==0 || currMB->b8pdir[k]==2) && (currMB->b8mode[k] !=0))//has forward vector
      {
        mv_mode  = currMB->b8mode[k];
        step_h   = BLOCK_STEP [mv_mode][0];
        step_v   = BLOCK_STEP [mv_mode][1];
        
        if (img->structure==FRAME && img->mb_field)
        {
          if (img->current_mb_nr%2==0)
          {
            refFrArr = refFrArr_top;
            if (!bframe)  refframe = refFrArr        [img->block_y/2+j0][img->block_x+i0];
            else          refframe = img->fw_refFrArr_top[img->block_y/2+j0][img->block_x+i0];
          }
          else
          {
            refFrArr = refFrArr_bot;
            if (!bframe)  refframe = refFrArr        [(img->block_y-4)/2+j0][img->block_x+i0];
            else          refframe = img->fw_refFrArr_bot[(img->block_y-4)/2+j0][img->block_x+i0];
          }
        }
        else
        {
          if (!bframe)  refframe = refFrArr        [img->block_y+j0][img->block_x+i0];
          else if(img->mb_frame_field_flag)          refframe = img->fw_refFrArr_frm[img->block_y+j0][img->block_x+i0];
          else                                       refframe = img->fw_refFrArr[img->block_y+j0][img->block_x+i0];
        }
        
        for (j=j0; j<j0+step_v0; j+=step_v)
          for (i=i0; i<i0+step_h0; i+=step_h)
          {
            j4 = img->block_y+j;
            i4 = img->block_x+i;
            
            // first make mv-prediction
            if (!bframe)  
              SetMotionVectorPredictor (img, pmv, pmv+1, refframe, refFrArr,         img->mv,    i, j, 4*step_h, 4*step_v);
            else if (img->mb_field && img->mb_frame_field_flag)
            {          
              if (img->current_mb_nr%2 == 0)
                SetMotionVectorPredictor (img, pmv, pmv+1, refframe, img->fw_refFrArr_top, img->fw_mv_top, i, j, 4*step_h, 4*step_v);
              else
                SetMotionVectorPredictor (img, pmv, pmv+1, refframe, img->fw_refFrArr_bot, img->fw_mv_bot, i, j, 4*step_h, 4*step_v);
            }
            else if (img->mb_frame_field_flag)
            {
              if (img->structure==FRAME)
                SetMotionVectorPredictor (img, pmv, pmv+1, refframe, img->fw_refFrArr_frm, img->fw_mv_frm, i, j, 4*step_h, 4*step_v);
              else
                SetMotionVectorPredictor (img, pmv, pmv+1, refframe, img->fw_refFrArr, img->fw_mv, i, j, 4*step_h, 4*step_v);
            }
            else               
              SetMotionVectorPredictor (img, pmv, pmv+1, refframe, img->fw_refFrArr, img->fw_mv, i, j, 4*step_h, 4*step_v);
            for (k=0; k < 2; k++) 
            {
#if TRACE
              snprintf(currSE.tracestring, TRACESTRING_SIZE, " MVD");
#endif
              img->subblock_x = i; // position used for context determination
              img->subblock_y = j; // position used for context determination
              currSE.value2 = (!bframe ? k : 2*k); // identifies the component; only used for context determination
              dP->readSyntaxElement(&currSE,img,inp,dP);
              curr_mvd = currSE.value1; 
              
              vec=curr_mvd+pmv[k];           /* find motion vector */
              
              // need B support
              if (!bframe)
              {
                if (img->current_mb_nr%2==0 && img->mb_frame_field_flag)
                { 
                  if (img->mb_field==0)
                  {
                    j4 = img->block_y/2 + j;
                    for(ii=0;ii<step_h;ii++)
                      for(jj=0;jj<step_v;jj++)
                      {
                        img->mv_frm[i4+ii+BLOCK_SIZE][img->block_y+j+jj][k]=vec;
                        img->mv_top[i4+ii+BLOCK_SIZE][j4+jj][k]=(k==1 ? vec/2 : vec);
                      }
                  }
                  else
                  {
                    j4 = img->block_y/2 + j;
                    for(ii=0;ii<step_h;ii++)
                      for(jj=0;jj<step_v;jj++)
                      {
                        img->mv_top[i4+ii+BLOCK_SIZE][j4+jj][k]=vec;
                        img->mv_frm[i4+ii+BLOCK_SIZE][img->block_y+j+jj][k]=(k==1 ? vec*2 : vec);
                      }
                  }
                }
                else if (img->mb_frame_field_flag)
                {
                  if (img->mb_field==0)
                  {
                    j4 = (img->block_y-4)/2 + j;
                    for(ii=0;ii<step_h;ii++)
                      for(jj=0;jj<step_v;jj++)
                      {
                        img->mv_frm[i4+ii+BLOCK_SIZE][img->block_y+j+jj][k]=vec;
                        img->mv_bot[i4+ii+BLOCK_SIZE][j4+jj][k]=(k==1 ? vec/2 : vec);
                      }
                  }
                  else
                  {
                    j4 = (img->block_y-4)/2 + j;
                    for(ii=0;ii<step_h;ii++)
                      for(jj=0;jj<step_v;jj++)
                      {
                        img->mv_bot[i4+ii+BLOCK_SIZE][j4+jj][k]=vec;
                        img->mv_frm[i4+ii+BLOCK_SIZE][img->block_y+j+jj][k]=(k==1 ? vec*2 : vec);
                      }
                  }
                }
                else
                {
                  for(ii=0;ii<step_h;ii++)
                    for(jj=0;jj<step_v;jj++)
                      img->mv[i4+ii+BLOCK_SIZE][j4+jj][k]=vec;
                }
              }
              else      // B frame
              {
                if (img->current_mb_nr%2==0 && img->mb_frame_field_flag)
                { 
                  if (img->mb_field==0)
                  {
                    j4 = img->block_y/2 + j;
                    for(ii=0;ii<step_h;ii++)
                      for(jj=0;jj<step_v;jj++)
                      {
                        img->fw_mv_frm[i4+ii+BLOCK_SIZE][img->block_y+j+jj][k]=vec;
                        img->fw_mv_top[i4+ii+BLOCK_SIZE][j4+jj][k]=(k==1 ? vec/2 : vec);
                      }
                  }
                  else
                  {
                    j4 = img->block_y/2 + j;
                    for(ii=0;ii<step_h;ii++)
                      for(jj=0;jj<step_v;jj++)
                      {
                        img->fw_mv_top[i4+ii+BLOCK_SIZE][j4+jj][k]=vec;
                        img->fw_mv_frm[i4+ii+BLOCK_SIZE][img->block_y+j+jj][k]=(k==1 ? vec*2 : vec);
                      }
                  }
                }
                else if (img->mb_frame_field_flag)
                {
                  if (img->mb_field==0)
                  {
                    j4 = (img->block_y-4)/2 + j;
                    for(ii=0;ii<step_h;ii++)
                      for(jj=0;jj<step_v;jj++)
                      {
                        img->fw_mv_frm[i4+ii+BLOCK_SIZE][img->block_y+j+jj][k]=vec;
                        img->fw_mv_bot[i4+ii+BLOCK_SIZE][j4+jj][k]=(k==1 ? vec/2 : vec);
                      }
                  }
                  else
                  {
                    j4 = (img->block_y-4)/2 + j;
                    for(ii=0;ii<step_h;ii++)
                      for(jj=0;jj<step_v;jj++)
                      {
                        img->fw_mv_bot[i4+ii+BLOCK_SIZE][j4+jj][k]=vec;
                        img->fw_mv_frm[i4+ii+BLOCK_SIZE][img->block_y+j+jj][k]=(k==1 ? vec*2 : vec);
                      }
                  }
                }
                else
                {
                  for(ii=0;ii<step_h;ii++)
                    for(jj=0;jj<step_v;jj++)
                      img->fw_mv[i4+ii+BLOCK_SIZE][j4+jj][k]=vec;
                }
              }
              
              /* store (oversampled) mvd */
              for (l=0; l < step_v; l++) 
                for (m=0; m < step_h; m++)  
                  currMB->mvd[0][j+l][i+m][k] =  curr_mvd;
        }
      }
    }
    else if (currMB->b8mode[k=2*(j0/2)+(i0/2)]==0)      
    {  
      if (!img->direct_type)
      {
        
        ref=img->mb_field ? (img->mb_y%2 ? refFrArr_bot[(img->block_y-4)/2+j0][img->block_x+i0]
          :refFrArr_top[img->block_y/2+j0][img->block_x+i0] ):refFrArr[img->block_y+j0][img->block_x+i0];          
        
        if (ref==-1)
        {
          img_block_y = (img->current_mb_nr%2) ? (img->block_y-4)/2:img->block_y/2;
          for (j=j0; j<j0+step_v0; j++)
            for (i=i0; i<i0+step_h0; i++)
            {            
              if(img->mb_frame_field_flag&&img->mb_field && (img->current_mb_nr%2 == 0))
              {
                img->fw_refFrArr_top[img_block_y+j][img->block_x+i]=-1;
                img->bw_refFrArr_top[img_block_y+j][img->block_x+i]=-1;
              }
              else if(img->mb_frame_field_flag&&img->mb_field && (img->current_mb_nr%2 != 0))
              {
                img->fw_refFrArr_bot[img_block_y+j][img->block_x+i]=-1;
                img->bw_refFrArr_bot[img_block_y+j][img->block_x+i]=-1;
              }
              else if(img->mb_frame_field_flag)
              {
                img->fw_refFrArr_frm[img->block_y+j][img->block_x+i]=-1;
                img->bw_refFrArr_frm[img->block_y+j][img->block_x+i]=-1;
              }
              else
              {
                img->fw_refFrArr[img->block_y+j][img->block_x+i]=-1; 
                img->bw_refFrArr[img->block_y+j][img->block_x+i]=-1;
              }            
              j4 = img->block_y+j;
              i4 = img->block_x+i;            
              for (ii=0; ii < 2; ii++) 
              {
                img->fw_mv[i4+BLOCK_SIZE][j4][ii]=0;
                img->bw_mv[i4+BLOCK_SIZE][j4][ii]=0;
                if (img->mb_frame_field_flag && img->mb_field)
                {
                  if (img->current_mb_nr%2 == 0)
                  {
                    j4 = img->block_y/2 + j;
                    img->fw_mv_top[i4+BLOCK_SIZE][j4][ii]=0;
                    img->bw_mv_top[i4+BLOCK_SIZE][j4][ii]=0;
                    img->fw_refFrArr_top[j4][i4]=-1;
                    img->bw_refFrArr_top[j4][i4]=-1;
                  }
                  else
                  {
                    j4 = (img->block_y-4)/2 + j;
                    img->fw_mv_bot[i4+BLOCK_SIZE][j4][ii]=0;
                    img->bw_mv_bot[i4+BLOCK_SIZE][j4][ii]=0;
                    img->fw_refFrArr_bot[j4][i4]=-1;
                    img->bw_refFrArr_bot[j4][i4]=-1;
                  }
                }
                if (img->mb_frame_field_flag && img->mb_field == 0)
                {
                  img->fw_mv_frm[i4+BLOCK_SIZE][j4][ii]=0;
                  img->bw_mv_frm[i4+BLOCK_SIZE][j4][ii]=0;
                }
              }
            }
        }
        else 
        {        
          for (j=j0; j<j0+step_v0; j++)
            for (i=i0; i<i0+step_h0; i++)
            { 
              ref=img->mb_field ? (img->mb_y%2 ? refFrArr_bot[(img->block_y-4)/2+j][img->block_x+i]
                :refFrArr_top[img->block_y/2+j][img->block_x+i] ):refFrArr[img->block_y+j][img->block_x+i];
              img_block_y = (img->current_mb_nr%2) ? (img->block_y-4)/2:img->block_y/2;
              {
                frame_no_next_P =img->imgtr_next_P+((mref==mref_fld)&&(img->structure==BOTTOM_FIELD));
                frame_no_B = (img->structure==TOP_FIELD || img->structure==BOTTOM_FIELD) ? img->tr_fld : 2*img->tr_frm;
                
                delta_P = (img->imgtr_next_P - img->imgtr_last_P);
                if((mref==mref_fld) && (img->structure==TOP_FIELD)) // top field
                {
                  iTRp = delta_P*(ref/2+1)-(ref+1)%2;
                }
                else if((mref==mref_fld) && (img->structure==BOTTOM_FIELD)) // bot field
                {
                  iTRp = 1+delta_P*(ref+1)/2-ref%2;
                }
                else  // frame
                {
                  iTRp = (ref+1)*delta_P;
                  if(img->mb_frame_field_flag && img->mb_field)
                  {
                    if(img->mb_y%2)
                      iTRp = 1+delta_P*(ref+1)/2-ref%2;
                    else
                      iTRp = delta_P*(ref/2+1)-(ref+1)%2;
                  }
                  
                }
                
                iTRb = iTRp - (frame_no_next_P - frame_no_B);
                mv_scale = (iTRb * 256)/ iTRp;  //! Note that this could be precomputed at the frame/slice level. 
                //! In such case, an array is needed for each different reference.
                if (img->structure == TOP_FIELD)
                {
                  img->fw_refFrArr[img->block_y+j][img->block_x+i]=ref + 0; // PLUS2, Krit, 7/06 (used to be + 1)
                  img->bw_refFrArr[img->block_y+j][img->block_x+i]=0;
                }
                else   
                {
                  if(img->mb_frame_field_flag&&img->mb_field && (img->current_mb_nr%2 == 0))
                  {
                    img->fw_refFrArr_top[img_block_y+j][img->block_x+i]=ref;
                    img->bw_refFrArr_top[img_block_y+j][img->block_x+i]=0;
                  }
                  else if(img->mb_frame_field_flag&&img->mb_field && (img->current_mb_nr%2 != 0))
                  {
                    img->fw_refFrArr_bot[img_block_y+j][img->block_x+i]=ref;
                    img->bw_refFrArr_bot[img_block_y+j][img->block_x+i]=0;
                  }
                  else if(img->mb_frame_field_flag)
                  {
                    img->fw_refFrArr_frm[img->block_y+j][img->block_x+i]=ref;
                    img->bw_refFrArr_frm[img->block_y+j][img->block_x+i]=0;
                  }
                  else
                  {
                    img->fw_refFrArr[img->block_y+j][img->block_x+i]=ref + 0; // PLUS2, Krit, 7/06 (used to be + 1)
                    img->bw_refFrArr[img->block_y+j][img->block_x+i]=0;
                  }
                }
                j4 = img->block_y+j;
                i4 = img->block_x+i;
                for (ii=0; ii < 2; ii++) 
                {              
                  img->fw_mv[i4+BLOCK_SIZE][j4][ii]= (mv_scale * img->mv[i4+BLOCK_SIZE][j4][ii] + 128)>>8;
                  img->bw_mv[i4+BLOCK_SIZE][j4][ii]= ((mv_scale - 256) * img->mv[i4+BLOCK_SIZE][j4][ii] + 128)>>8;
                  //                img->fw_mv[i4+BLOCK_SIZE][j4][ii]= iTRb*img->mv[i4+BLOCK_SIZE][j4][ii]/iTRp;
                  //                img->bw_mv[i4+BLOCK_SIZE][j4][ii]=(iTRb-iTRp)*img->mv[i4+BLOCK_SIZE][j4][ii]/iTRp;
                  if (img->mb_frame_field_flag && img->mb_field)
                  {
                    if (img->current_mb_nr%2 == 0)
                    {
                      j4 = img->block_y/2 + j;
                      //                    img->fw_mv_top[i4+BLOCK_SIZE][j4][ii]= iTRb*img->mv_top[i4+BLOCK_SIZE][j4][ii]/iTRp;
                      //                    img->bw_mv_top[i4+BLOCK_SIZE][j4][ii]=(iTRb-iTRp)*img->mv_top[i4+BLOCK_SIZE][j4][ii]/iTRp;
                      img->fw_mv_top[i4+BLOCK_SIZE][j4][ii]= (mv_scale * img->mv_top[i4+BLOCK_SIZE][j4][ii] + 128)>>8;
                      img->bw_mv_top[i4+BLOCK_SIZE][j4][ii]=((mv_scale - 256) * img->mv_top[i4+BLOCK_SIZE][j4][ii] + 128)>>8;
                    }
                    else
                    {
                      j4 = (img->block_y-4)/2 + j;
                      img->fw_mv_bot[i4+BLOCK_SIZE][j4][ii]= (mv_scale * img->mv_bot[i4+BLOCK_SIZE][j4][ii] + 128)>>8;  // TBD -- fix it to use bot mv 
                      img->bw_mv_bot[i4+BLOCK_SIZE][j4][ii]=((mv_scale - 256) * img->mv_bot[i4+BLOCK_SIZE][j4][ii] + 128)>>8; // TBD -- fix it to use bot mv 
                      //                    img->fw_mv_bot[i4+BLOCK_SIZE][j4][ii]= iTRb*img->mv_bot[i4+BLOCK_SIZE][j4][ii]/iTRp;
                      //                    img->bw_mv_bot[i4+BLOCK_SIZE][j4][ii]=(iTRb-iTRp)*img->mv_bot[i4+BLOCK_SIZE][j4][ii]/iTRp;
                    }
                  }
                  if (img->mb_frame_field_flag && img->mb_field == 0)
                  {
                    img->fw_mv_frm[i4+BLOCK_SIZE][j4][ii]= (mv_scale * img->mv[i4+BLOCK_SIZE][j4][ii] + 128)>>8;
                    img->bw_mv_frm[i4+BLOCK_SIZE][j4][ii]=((mv_scale - 256) * img->mv[i4+BLOCK_SIZE][j4][ii] + 128)>>8;
                    
                    //                  img->fw_mv_frm[i4+BLOCK_SIZE][j4][ii]= iTRb*img->mv[i4+BLOCK_SIZE][j4][ii]/iTRp;
                    //                  img->bw_mv_frm[i4+BLOCK_SIZE][j4][ii]=(iTRb-iTRp)*img->mv[i4+BLOCK_SIZE][j4][ii]/iTRp;
                  }
                }
              } 
            }
         }  
      } 
    }
  }
  

  //=====  READ BACKWARD MOTION VECTORS =====
  currSE.type = SE_MVD;
  dP          = &(currSlice->partArr[partMap[SE_BFRAME]]);

  if (inp->symbol_mode == UVLC || dP->bitstream->ei_flag) currSE.mapping = linfo_mvd;
  else                                                    currSE.reading = readBiMVD2Buffer_CABAC;

  img_block_y = img->block_y;
  if (img->structure==FRAME && img->mb_field)
  {
    if (img->current_mb_nr % 2 ==0)
      img_block_y = img->block_y / 2;
    else
      img_block_y = (img->block_y-4) / 2;
  }
  
  for (j0=0; j0<4; j0+=step_v0)
  for (i0=0; i0<4; i0+=step_h0)
  {
    k=2*(j0/2)+(i0/2);
    if ((currMB->b8pdir[k]==1 || currMB->b8pdir[k]==2) && (currMB->b8mode[k]!=0))//has backward vector
    {
      mv_mode  = currMB->b8mode[k];
      step_h   = BLOCK_STEP [mv_mode][0];
      step_v   = BLOCK_STEP [mv_mode][1];

      if (img->mb_frame_field_flag)
      {
        if (img->structure==FRAME && img->mb_field)
        {
          if (img->current_mb_nr%2==0)
            refframe = img->bw_refFrArr_top[img_block_y+j0][img->block_x+i0]; // always 0
          else
            refframe = img->bw_refFrArr_bot[img_block_y+j0][img->block_x+i0]; // always 0
        }
        else
        {
          if (img->structure!=FRAME)
            refframe = img->bw_refFrArr[img_block_y+j0][img->block_x+i0]; // always 0
          else
            refframe = img->bw_refFrArr_frm[img_block_y+j0][img->block_x+i0]; // always 0
        }
      }
      else
        refframe = img->bw_refFrArr[img->block_y+j0][img->block_x+i0]; // always 0

      use_scaled_mv = 0;
      if(currMB->b8pdir[k]==2)
      {
        fw_refframe = img->fw_refFrArr[img->block_y+j0][img->block_x+i0];
        current_tr  = (img->structure==TOP_FIELD || img->structure==BOTTOM_FIELD)?img->tr_fld:2*img->tr_frm;
        if(img->explicit_B_prediction==1)
        {
          for (j=j0; j<j0+step_v0; j++)
          for (i=i0; i<i0+step_h0; i++)
            currMB->bipred_weighting_type[2*(j/2)+(i/2)] = (refframe > fw_refframe);
        }
        if((current_tr >= img->imgtr_next_P) && (current_tr >= img->imgtr_last_P))
        {
          use_scaled_mv = 1;
          mv_scale = ((refframe+1)*256)/(fw_refframe+1);
        }
      }

      for (j=j0; j<j0+step_v0; j+=step_v)
      for (i=i0; i<i0+step_h0; i+=step_h)
      {
        j4 = img_block_y+j;
        i4 = img->block_x+i;

        // first make mv-prediction
        if(use_scaled_mv)
        {
          pmv[0] = (mv_scale*img->fw_mv[i4+BLOCK_SIZE][j4][0]+128)>>8;
          pmv[1] = (mv_scale*img->fw_mv[i4+BLOCK_SIZE][j4][1]+128)>>8;
        }
        else if (img->mb_frame_field_flag)
        {
          if (img->structure==FRAME && img->mb_field)
          {
            if (img->current_mb_nr % 2 == 0)
              SetMotionVectorPredictor (img, pmv, pmv+1, refframe, img->bw_refFrArr_top, img->bw_mv_top, i, j, 4*step_h, 4*step_v);
            else
              SetMotionVectorPredictor (img, pmv, pmv+1, refframe, img->bw_refFrArr_bot, img->bw_mv_bot, i, j, 4*step_h, 4*step_v);
          }
          else if (img->structure==FRAME)
            SetMotionVectorPredictor (img, pmv, pmv+1, refframe, img->bw_refFrArr_frm, img->bw_mv_frm, i, j, 4*step_h, 4*step_v);
          else
            SetMotionVectorPredictor (img, pmv, pmv+1, refframe, img->bw_refFrArr, img->bw_mv, i, j, 4*step_h, 4*step_v);
        }
        else
          SetMotionVectorPredictor (img, pmv, pmv+1, refframe, img->bw_refFrArr, img->bw_mv, i, j, 4*step_h, 4*step_v);

        for (k=0; k < 2; k++) 
        {
#if TRACE
          snprintf(currSE.tracestring, TRACESTRING_SIZE, " MVD");
#endif
          img->subblock_x = i; // position used for context determination
          img->subblock_y = j; // position used for context determination
          currSE.value2   = 2*k+1; // identifies the component; only used for context determination
          dP->readSyntaxElement(&currSE,img,inp,dP);
          curr_mvd = currSE.value1; 
  
          vec=curr_mvd+pmv[k];           /* find motion vector */
          if (img->current_mb_nr%2==0 && img->mb_frame_field_flag)
          { 
              if (img->mb_field==0)
              {
                j4 = img->block_y/2 + j;
                for(ii=0;ii<step_h;ii++)
                  for(jj=0;jj<step_v;jj++)
                  {
                    img->bw_mv_frm[i4+ii+BLOCK_SIZE][img->block_y+j+jj][k]=vec;
                    img->bw_mv_top[i4+ii+BLOCK_SIZE][j4+jj][k]=(k==1 ? vec/2 : vec);
                  }
              }
              else
              {
                j4 = img->block_y/2 + j;
                for(ii=0;ii<step_h;ii++)
                  for(jj=0;jj<step_v;jj++)
                  {
                    img->bw_mv_top[i4+ii+BLOCK_SIZE][j4+jj][k]=vec;
                    img->bw_mv_frm[i4+ii+BLOCK_SIZE][img->block_y+j+jj][k]=(k==1 ? vec*2 : vec);
                  }
              }
            }
          else if (img->mb_frame_field_flag)
          {
              if (img->mb_field==0)
              {
                j4 = (img->block_y-4)/2 + j;
                for(ii=0;ii<step_h;ii++)
                  for(jj=0;jj<step_v;jj++)
                  {
                    img->bw_mv_frm[i4+ii+BLOCK_SIZE][img->block_y+j+jj][k]=vec;
                    img->bw_mv_bot[i4+ii+BLOCK_SIZE][j4+jj][k]=(k==1 ? vec/2 : vec);
                  }
              }
              else
              {
                j4 = (img->block_y-4)/2 + j;
                for(ii=0;ii<step_h;ii++)
                  for(jj=0;jj<step_v;jj++)
                  {
                    img->bw_mv_bot[i4+ii+BLOCK_SIZE][j4+jj][k]=vec;
                    img->bw_mv_frm[i4+ii+BLOCK_SIZE][img->block_y+j+jj][k]=(k==1 ? vec*2 : vec);
                  }
              }
          }
          else
            for(ii=0;ii<step_h;ii++)
              for(jj=0;jj<step_v;jj++)
                img->bw_mv[i4+ii+BLOCK_SIZE][j4+jj][k]=vec;

          /* store (oversampled) mvd */
          for (l=0; l < step_v; l++) 
            for (m=0; m < step_h; m++)  
              currMB->mvd[1][j+l][i+m][k] =  curr_mvd;
        }
      }
    }
  }
  if (img->mb_frame_field_flag)
    refFrArr = refFrArr_frm;
}



/*!
 ************************************************************************
 * \brief
 *    Get the Prediction from the Neighboring BLocks for Number of Nonzero Coefficients 
 *    
 *    Luma Blocks
 ************************************************************************
 */
int predict_nnz(struct img_par *img, int i,int j)
{
    int Left_block,Top_block, pred_nnz;
  int cnt=0;

  if (i)
    Left_block= img->nz_coeff [img->mb_x ][img->mb_y ][i-1][j];
  else
      Left_block= img->mb_x > 0 ? img->nz_coeff [img->mb_x-1 ][img->mb_y ][3][j] : -1;
  if (j)
    Top_block=  img->nz_coeff [img->mb_x ][img->mb_y ][i][j-1];
  else
      Top_block=  img->mb_y > 0 ? img->nz_coeff [img->mb_x ][img->mb_y-1 ][i][3] : -1;
  
//  if (img->mb_frame_field_flag)
//    Top_block=0;

  pred_nnz=0;
  if (Left_block>-1)
  {
    pred_nnz=Left_block;
    cnt++;
  }
  if (Top_block>-1)
  {
    pred_nnz+=Top_block;
    cnt++;
  }
  if (cnt)
    pred_nnz/=cnt; 
    return pred_nnz;
}
/*!
 ************************************************************************
 * \brief
 *    Get the Prediction from the Neighboring BLocks for Number of Nonzero Coefficients 
 *    
 *    Chroma Blocks   
 ************************************************************************
 */
int predict_nnz_chroma(struct img_par *img, int i,int j)
{
    int Left_block,Top_block, pred_nnz;
  int cnt=0;

  if (i==1 || i==3)
    Left_block= img->nz_coeff [img->mb_x ][img->mb_y ][i-1][j];
  else
      Left_block= img->mb_x > 0 ? img->nz_coeff [img->mb_x-1 ][img->mb_y ][i+1][j] : -1;
  if (j==5)
    Top_block=  img->nz_coeff [img->mb_x ][img->mb_y ][i][j-1];
  else
    Top_block=  img->mb_y > 0 ? img->nz_coeff [img->mb_x ][img->mb_y-1 ][i][5] : -1;

//  if (img->mb_frame_field_flag)
//   Top_block=0;
  
  pred_nnz=0;
  if (Left_block>-1)
  {
    pred_nnz=Left_block;
    cnt++;
  }
  if (Top_block>-1)
  {
    pred_nnz+=Top_block;
    cnt++;
  }
  if (cnt)
    pred_nnz/=cnt; 
    return pred_nnz;
}


/*!
 ************************************************************************
 * \brief
 *    Reads coeff of an 4x4 block (CAVLC)
 *
 * \author
 *    Karl Lillevold <karll@real.com>
 *    contributions by James Au <james@ubvideo.com>
 ************************************************************************
 */


void readCoeff4x4_CAVLC (struct img_par *img,struct inp_par *inp,
                        int block_type, 
                        int i, int j, int levarr[16], int runarr[16],
                        int *number_coefficients)
{
  int mb_nr = img->current_mb_nr;
  Macroblock *currMB = &img->mb_data[mb_nr];
  SyntaxElement currSE;
  Slice *currSlice = img->currentSlice;
  DataPartition *dP;
  int *partMap = assignSE2partition[currSlice->dp_mode];


  int k, code, vlcnum;
  int numcoeff, numtrailingones, numcoeff_vlc;
  int level_two_or_higher;
  int numones, totzeros, level, cdc=0, cac=0;
  int zerosleft, ntr, dptype = 0;
  int max_coeff_num = 0, nnz;
  char type[15];
  int incVlc[] = {0,3,6,12,24,48,32768};    // maximum vlc = 6

  numcoeff = 0;

  switch (block_type)
  {
  case LUMA:
    max_coeff_num = 16;
    sprintf(type, "%s", "Luma");
    if (IS_INTRA (currMB))
    {
      dptype = SE_LUM_AC_INTRA;
    }
    else
    {
      dptype = SE_LUM_AC_INTER;
    }
    break;
  case LUMA_INTRA16x16DC:
    max_coeff_num = 16;
    sprintf(type, "%s", "Lum16DC");
    dptype = SE_LUM_DC_INTRA;
    break;
  case LUMA_INTRA16x16AC:
    max_coeff_num = 15;
    sprintf(type, "%s", "Lum16AC");
    dptype = SE_LUM_AC_INTRA;
    break;

  case CHROMA_DC:
    max_coeff_num = 4;
    cdc = 1;

    sprintf(type, "%s", "ChrDC");
    if (IS_INTRA (currMB))
    {
      dptype = SE_CHR_DC_INTRA;
    }
    else
    {
      dptype = SE_CHR_DC_INTER;
    }
    break;
  case CHROMA_AC:
    max_coeff_num = 15;
    cac = 1;
    sprintf(type, "%s", "ChrAC");
    if (IS_INTRA (currMB))
    {
      dptype = SE_CHR_AC_INTRA;
    }
    else
    {
      dptype = SE_CHR_AC_INTER;
    }
    break;
  default:
    error ("readCoeff4x4_CAVLC: invalid block type", 600);
    break;
  }

  if(img->type == B_IMG_1 || img->type == B_IMG_MULT)
  {
    dptype = SE_BFRAME;
  }

  currSE.type = dptype;
  dP = &(currSlice->partArr[partMap[dptype]]);

  img->nz_coeff[img->mb_x ][img->mb_y][i][j] = 0;


  if (!cdc)
  {
    // luma or chroma AC
    if (!cac)
    {
      nnz = predict_nnz(img, i, j);
    }
    else
    {
      nnz = predict_nnz_chroma(img, i, j);
    }

    if (nnz < 2)
    {
      numcoeff_vlc = 0;
    }
    else if (nnz < 4)
    {
      numcoeff_vlc = 1;
    }
    else if (nnz < 8)
    {
      numcoeff_vlc = 2;
    }
    else //
    {
      numcoeff_vlc = 3;
    }

    currSE.value1 = numcoeff_vlc;

    readSyntaxElement_NumCoeffTrailingOnes(&currSE, dP, type);

    numcoeff =  currSE.value1;
    numtrailingones =  currSE.value2;

    img->nz_coeff[img->mb_x ][img->mb_y][i][j] = numcoeff;
  }
  else
  {
    // chroma DC
    readSyntaxElement_NumCoeffTrailingOnesChromaDC(&currSE, dP);

    numcoeff =  currSE.value1;
    numtrailingones =  currSE.value2;
  }


  for (k = 0; k < max_coeff_num; k++)
  {
    levarr[k] = 0;
    runarr[k] = 0;
  }

  numones = numtrailingones;
  *number_coefficients = numcoeff;

  if (numcoeff)
  {
    if (numtrailingones)
    {

      currSE.len = numtrailingones;

#if TRACE
      snprintf(currSE.tracestring, 
        TRACESTRING_SIZE, "%s trailing ones sign (%d,%d)", type, i, j);
#endif

      readSyntaxElement_FLC (&currSE, dP);

      code = currSE.inf;
      ntr = numtrailingones;
      for (k = numcoeff-1; k > numcoeff-1-numtrailingones; k--)
      {
        ntr --;
        if ((code>>ntr)&1)
          levarr[k] = -1;
        else
          levarr[k] = 1;
      }
    }

    // decode levels
    level_two_or_higher = 1;
    if (numcoeff > 3 && numtrailingones == 3)
      level_two_or_higher = 0;

      if (numcoeff > 10 && numtrailingones < 3)
          vlcnum = 1;
      else
          vlcnum = 0;

    for (k = numcoeff - 1 - numtrailingones; k >= 0; k--)
    {

#if TRACE
      snprintf(currSE.tracestring, 
        TRACESTRING_SIZE, "%s lev (%d,%d) k=%d vlc=%d ", type,
          i, j, k, vlcnum);
#endif

      if (vlcnum == 0)
          readSyntaxElement_Level_VLC0(&currSE, dP);
      else
          readSyntaxElement_Level_VLCN(&currSE, vlcnum, dP);

      if (level_two_or_higher)
      {
          if (currSE.inf > 0)
          currSE.inf ++;
          else
          currSE.inf --;
          level_two_or_higher = 0;
      }

      level = levarr[k] = currSE.inf;
      if (abs(level) == 1)
        numones ++;

      // update VLC table
      if (abs(level)>incVlc[vlcnum])
        vlcnum++;

      if (k == numcoeff - 1 - numtrailingones && abs(level)>3)
        vlcnum = 2;

    }
    
    if (numcoeff < max_coeff_num)
    {
      // decode total run
      vlcnum = numcoeff-1;
      currSE.value1 = vlcnum;

#if TRACE
      snprintf(currSE.tracestring, 
        TRACESTRING_SIZE, "%s totalrun (%d,%d) vlc=%d ", type, i,j, vlcnum);
#endif
      if (cdc)
        readSyntaxElement_TotalZerosChromaDC(&currSE, dP);
      else
        readSyntaxElement_TotalZeros(&currSE, dP);

      totzeros = currSE.value1;
    }
    else
    {
      totzeros = 0;
    }

    // decode run before each coefficient
    zerosleft = totzeros;
    i = numcoeff-1;
    if (zerosleft > 0 && i > 0)
    {
      do 
      {
        // select VLC for runbefore
        vlcnum = zerosleft - 1;
        if (vlcnum > RUNBEFORE_NUM-1)
          vlcnum = RUNBEFORE_NUM-1;

        currSE.value1 = vlcnum;
#if TRACE
        snprintf(currSE.tracestring, 
          TRACESTRING_SIZE, "%s run (%d,%d) k=%d vlc=%d ",
            type, i, j, i, vlcnum);
#endif

        readSyntaxElement_Run(&currSE, dP);
        runarr[i] = currSE.value1;

        zerosleft -= runarr[i];
        i --;
      } while (zerosleft != 0 && i != 0);
    }
    runarr[i] = zerosleft;

  } // if numcoeff
}



/*!
 ************************************************************************
 * \brief
 *    Get coded block pattern and coefficients (run/level)
 *    from the NAL
 ************************************************************************
 */
void readCBPandCoeffsFromNAL(struct img_par *img,struct inp_par *inp)
{
  int i,j,k;
  int level;
  int mb_nr = img->current_mb_nr;
  int ii,jj;
  int i1,j1, m2,jg2;
  Macroblock *currMB = &img->mb_data[mb_nr];
  int cbp;
  SyntaxElement currSE;
  Slice *currSlice = img->currentSlice;
  DataPartition *dP;
  int *partMap = assignSE2partition[currSlice->dp_mode];
  int iii,jjj;
  int coef_ctr, i0, j0, b8;
  int ll;
  int block_x,block_y;
  int start_scan;
  int uv;


  int run, len;
  int levarr[16], runarr[16], numcoeff;

  int qp_per    = (img->qp-MIN_QP)/6;
  int qp_rem    = (img->qp-MIN_QP)%6;
  int qp_per_uv = QP_SCALE_CR[img->qp-MIN_QP]/6;
  int qp_rem_uv = QP_SCALE_CR[img->qp-MIN_QP]%6;
  int smb       = ((img->type==SP_IMG_1 || img->type==SP_IMG_MULT) && IS_INTER (currMB)) || (img->type == SI_IMG && currMB->mb_type == SI4MB);

  // read CBP if not new intra mode
  if (!IS_NEWINTRA (currMB))
  {
    if (IS_OLDINTRA (currMB) || currMB->mb_type == SI4MB )   currSE.type = SE_CBP_INTRA;
    else                        currSE.type = SE_CBP_INTER;

    if(img->type == B_IMG_1 || img->type == B_IMG_MULT)  dP = &(currSlice->partArr[partMap[SE_BFRAME]]);
    else                                                 dP = &(currSlice->partArr[partMap[currSE.type]]);
    
    if (inp->symbol_mode == UVLC || dP->bitstream->ei_flag)
    {
      if (IS_OLDINTRA (currMB) || currMB->mb_type == SI4MB)  currSE.mapping = linfo_cbp_intra;
      else                       currSE.mapping = linfo_cbp_inter;
    }
    else
    {
      currSE.reading = readCBPFromBuffer_CABAC;
    }

#if TRACE
    snprintf(currSE.tracestring, TRACESTRING_SIZE, " CBP ");
#endif
    dP->readSyntaxElement(&currSE,img,inp,dP);
    currMB->cbp = cbp = currSE.value1;
    // Delta quant only if nonzero coeffs
    if (cbp !=0)
    {
      if (IS_INTER (currMB))  currSE.type = SE_DELTA_QUANT_INTER;
      else                    currSE.type = SE_DELTA_QUANT_INTRA;

      if(img->type == B_IMG_1 || img->type == B_IMG_MULT)  dP = &(currSlice->partArr[partMap[SE_BFRAME]]);
      else                                                 dP = &(currSlice->partArr[partMap[currSE.type]]);
      
      if (inp->symbol_mode == UVLC || dP->bitstream->ei_flag)
      {
        currSE.mapping = linfo_dquant;
      }
      else
                currSE.reading= readDquant_FromBuffer_CABAC; //gabi

#if TRACE
      snprintf(currSE.tracestring, TRACESTRING_SIZE, " Delta quant ");
#endif
      dP->readSyntaxElement(&currSE,img,inp,dP);
      currMB->delta_quant = currSE.value1;
      img->qp= (img->qp-MIN_QP+currMB->delta_quant+(MAX_QP-MIN_QP+1))%(MAX_QP-MIN_QP+1)+MIN_QP;
    }
  }
  else
  {
    cbp = currMB->cbp;
  }

  for (i=0;i<BLOCK_SIZE;i++)
    for (j=0;j<BLOCK_SIZE;j++)
      for(iii=0;iii<BLOCK_SIZE;iii++)
        for(jjj=0;jjj<BLOCK_SIZE;jjj++)
          img->cof[i][j][iii][jjj]=0;// reset luma coeffs


  if (IS_NEWINTRA (currMB)) // read DC coeffs for new intra modes
  {
    currSE.type = SE_DELTA_QUANT_INTRA;

    if(img->type == B_IMG_1 || img->type == B_IMG_MULT)  dP = &(currSlice->partArr[partMap[SE_BFRAME]]);
    else                                                 dP = &(currSlice->partArr[partMap[currSE.type]]);
    
    if (inp->symbol_mode == UVLC || dP->bitstream->ei_flag)
    {
      currSE.mapping = linfo_dquant;
    }
    else
    {
            currSE.reading= readDquant_FromBuffer_CABAC;
      //currSE.reading= readDquant_intra_FromBuffer_CABAC;
    }
#if TRACE
    snprintf(currSE.tracestring, TRACESTRING_SIZE, " Delta quant ");
#endif
    dP->readSyntaxElement(&currSE,img,inp,dP);
    currMB->delta_quant = currSE.value1;
    img->qp= (img->qp-MIN_QP+currMB->delta_quant+(MAX_QP-MIN_QP+1))%(MAX_QP-MIN_QP+1)+MIN_QP;

    for (i=0;i<BLOCK_SIZE;i++)
      for (j=0;j<BLOCK_SIZE;j++)
        img->ipredmode[img->block_x+i+1][img->block_y+j+1]=0;


    if (inp->symbol_mode == UVLC)
    {
      readCoeff4x4_CAVLC(img, inp, LUMA_INTRA16x16DC, 0, 0,
                          levarr, runarr, &numcoeff);

      coef_ctr=-1;
      level = 1;                            // just to get inside the loop
      for(k = 0; k < numcoeff; k++)
      {
        if (levarr[k] != 0)                     // leave if len=1
        {
          coef_ctr=coef_ctr+runarr[k]+1;

          i0=SNGL_SCAN[coef_ctr][0];
          j0=SNGL_SCAN[coef_ctr][1];

          img->cof[i0][j0][0][0]=levarr[k];// add new intra DC coeff
        }
      }
    }
    else
    {

      currSE.type = SE_LUM_DC_INTRA;
      if(img->type == B_IMG_1 || img->type == B_IMG_MULT)  dP = &(currSlice->partArr[partMap[SE_BFRAME]]);
      else                                                 dP = &(currSlice->partArr[partMap[currSE.type]]);

      currSE.golomb_maxlevels=0;  //do not use generic golomb

      currSE.context      = LUMA_16DC;
      currSE.type         = SE_LUM_DC_INTRA;
      img->is_intra_block = 1;

      if (inp->symbol_mode == UVLC || dP->bitstream->ei_flag)
      {
        currSE.mapping = linfo_levrun_inter;
      }
      else
      {
        currSE.reading = readRunLevelFromBuffer_CABAC;
      }



      coef_ctr=-1;
      level = 1;                            // just to get inside the loop
      for(k=0;(k<17) && (level!=0);k++)
      {
#if TRACE
        snprintf(currSE.tracestring, TRACESTRING_SIZE, "DC luma 16x16 ");
#endif
        dP->readSyntaxElement(&currSE,img,inp,dP);
        level = currSE.value1;
        run   = currSE.value2;
        len   = currSE.len;

        if (level != 0)                     // leave if len=1
        {
          coef_ctr=coef_ctr+run+1;

          i0=SNGL_SCAN[coef_ctr][0];
          j0=SNGL_SCAN[coef_ctr][1];

          img->cof[i0][j0][0][0]=level;// add new intra DC coeff
        }
      }
    }
    itrans_2(img);// transform new intra DC
  }

  qp_per    = (img->qp-MIN_QP)/6;
  qp_rem    = (img->qp-MIN_QP)%6;
  qp_per_uv = QP_SCALE_CR[img->qp-MIN_QP]/6;
  qp_rem_uv = QP_SCALE_CR[img->qp-MIN_QP]%6;
  currMB->qp = img->qp;

  // luma coefficients
  for (block_y=0; block_y < 4; block_y += 2) /* all modes */
  {
    for (block_x=0; block_x < 4; block_x += 2)
    {

      b8 = 2*(block_y/2) + block_x/2;
      if (inp->symbol_mode == UVLC && !currMB->useABT[b8])
      {
        for (j=block_y; j < block_y+2; j++)
        {
          for (i=block_x; i < block_x+2; i++)
          {
            ii = block_x/2; jj = block_y/2;
            b8 = 2*jj+ii;

            if (cbp & (1<<b8))  /* are there any coeff in current block at all */
            {
              if (IS_NEWINTRA(currMB))
              {
                readCoeff4x4_CAVLC(img, inp, LUMA_INTRA16x16AC, i, j,
                                    levarr, runarr, &numcoeff);

                start_scan = 1;
              }
              else
              {
                readCoeff4x4_CAVLC(img, inp, LUMA, i, j,
                                    levarr, runarr, &numcoeff);
                start_scan = 0;
              }

              coef_ctr = start_scan-1;
              for (k = 0; k < numcoeff; k++)
              {
                if (levarr[k] != 0)
                {
                  coef_ctr             += runarr[k]+1;
                  i0=SNGL_SCAN[coef_ctr][0];
                  j0=SNGL_SCAN[coef_ctr][1];
                  currMB->cbp_blk      |= 1 << ((j<<2) + i) ;
                  img->cof[i][j][i0][j0]= levarr[k]*dequant_coef[qp_rem][i0][j0]<<qp_per;
                }
              }
            }
            else
            {
              img->nz_coeff[img->mb_x][img->mb_y][i][j] = 0;
            }
          }
        }
      } // VLC && !ABT
      else
      {
        b8 = 2*(block_y/2) + block_x/2;

        if (currMB->useABT[b8])
        {
          for (j=block_y; j < block_y+2; j++)
          {
            for (i=block_x; i < block_x+2; i++)
            {
              img->nz_coeff[img->mb_x][img->mb_y][i][j] = 0;  // CAVLC
            }
          }
          readLumaCoeffABT_B8(b8, inp, img);

        } // ABT
        else
        { 
          // CABAC && ! ABT
          for (j=block_y; j < block_y+2; j++)
          {
            //jj=j/2;
            for (i=block_x; i < block_x+2; i++)
            {
              //ii = i/2;
              //b8 = 2*jj+ii;

              if (IS_NEWINTRA (currMB))   start_scan = 1; // skip DC coeff
              else                        start_scan = 0; // take all coeffs

              img->subblock_x = i; // position for coeff_count ctx
              img->subblock_y = j; // position for coeff_count ctx
              if (cbp & (1<<b8))  // are there any coeff in current block at all
              {
                coef_ctr = start_scan-1;
                level    = 1;
                for(k=start_scan;(k<17) && (level!=0);k++)
                {
                 /*
                  * make distinction between INTRA and INTER coded
                  * luminance coefficients
                  */
                  currSE.context      = (IS_NEWINTRA(currMB) ? LUMA_16AC : LUMA_4x4);
                  currSE.type         = (IS_NEWINTRA(currMB) || currMB->b8mode[b8]==IBLOCK ?
                                        (k==0 ? SE_LUM_DC_INTRA : SE_LUM_AC_INTRA) :
                                        (k==0 ? SE_LUM_DC_INTER : SE_LUM_AC_INTER));
                  img->is_intra_block = (IS_NEWINTRA(currMB) || currMB->b8mode[b8]==IBLOCK);
                  
#if TRACE
                  sprintf(currSE.tracestring, " Luma sng ");
#endif
                  if(img->type == B_IMG_1 || img->type == B_IMG_MULT)  dP = &(currSlice->partArr[partMap[SE_BFRAME]]);
                  else                                                 dP = &(currSlice->partArr[partMap[currSE.type]]);
                  
                  if (inp->symbol_mode == UVLC || dP->bitstream->ei_flag)  currSE.mapping = linfo_levrun_inter;
                  else                                                     currSE.reading = readRunLevelFromBuffer_CABAC;
                  
                  currSE.golomb_maxlevels=0;  //do not use generic golomb
                  
                  dP->readSyntaxElement(&currSE,img,inp,dP);
                  level = currSE.value1;
                  run   = currSE.value2;
                  len   = currSE.len;
                  
                  if (level != 0)    /* leave if len=1 */
                  {
                    coef_ctr             += run+1;
                    i0=SNGL_SCAN[coef_ctr][0];
                    j0=SNGL_SCAN[coef_ctr][1];
                    currMB->cbp_blk      |= 1 << ((j<<2) + i) ;
                    img->cof[i][j][i0][j0]= level*dequant_coef[qp_rem][i0][j0]<<qp_per;
                  }
                }
              }
            }
          }
        } // else ABT
      } 
    }
  }

  for (j=4;j<6;j++) // reset all chroma coeffs before read
    for (i=0;i<4;i++)
      for (iii=0;iii<4;iii++)
        for (jjj=0;jjj<4;jjj++)
          img->cof[i][j][iii][jjj]=0;

  m2 =img->mb_x*2;
  jg2=img->mb_y*2;

  // chroma 2x2 DC coeff
  if(cbp>15)
  {
    for (ll=0;ll<3;ll+=2)
    {
      for (i=0;i<4;i++)
        img->cofu[i]=0;


      if (inp->symbol_mode == UVLC)
      {

        readCoeff4x4_CAVLC(img, inp, CHROMA_DC, 0, 0,
                            levarr, runarr, &numcoeff);
        coef_ctr=-1;
        level=1;
        for(k = 0; k < numcoeff; k++)
        {
          if (levarr[k] != 0)
          {
            currMB->cbp_blk |= 0xf0000 << (ll<<1) ;
            coef_ctr=coef_ctr+runarr[k]+1;
            img->cofu[coef_ctr]=levarr[k];
          }
        }
      }
      else
      {
        coef_ctr=-1;
        level=1;
        for(k=0;(k<5)&&(level!=0);k++)
        {
          currSE.context      = CHROMA_DC;
          currSE.type         = (IS_INTRA(currMB) ? SE_CHR_DC_INTRA : SE_CHR_DC_INTER);
          img->is_intra_block =  IS_INTRA(currMB);
          img->is_v_block     = ll;

#if TRACE
          snprintf(currSE.tracestring, TRACESTRING_SIZE, " 2x2 DC Chroma ");
#endif
          if(img->type == B_IMG_1 || img->type == B_IMG_MULT)
            dP = &(currSlice->partArr[partMap[SE_BFRAME]]);
          else
            dP = &(currSlice->partArr[partMap[currSE.type]]);
        
          if (inp->symbol_mode == UVLC || dP->bitstream->ei_flag)
            currSE.mapping = linfo_levrun_c2x2;
          else
            currSE.reading = readRunLevelFromBuffer_CABAC;

          dP->readSyntaxElement(&currSE,img,inp,dP);
          level = currSE.value1;
          run = currSE.value2;
          len = currSE.len;
          if (level != 0)
          {
            currMB->cbp_blk |= 0xf0000 << (ll<<1) ;
            coef_ctr=coef_ctr+run+1;
            // Bug: img->cofu has only 4 entries, hence coef_ctr MUST be <4 (which is
            // caught by the assert().  If it is bigger than 4, it starts patching the
            // img->predmode pointer, which leads to bugs later on.
            //
            // This assert() should be left in the code, because it captures a very likely
            // bug early when testing in error prone environments (or when testing NAL
            // functionality).
            assert (coef_ctr < 4);
            img->cofu[coef_ctr]=level;
          }
        }
      }

      if (smb) // check to see if MB type is SPred or SIntra4x4 
      {
         img->cof[0+ll][4][0][0]=img->cofu[0];   img->cof[1+ll][4][0][0]=img->cofu[1];
         img->cof[0+ll][5][0][0]=img->cofu[2];   img->cof[1+ll][5][0][0]=img->cofu[3];
      }
      else
      { 
        for (i=0;i<4;i++)
          img->cofu[i]*=dequant_coef[qp_rem_uv][0][0]<<qp_per_uv;
        img->cof[0+ll][4][0][0]=(img->cofu[0]+img->cofu[1]+img->cofu[2]+img->cofu[3])>>1;
        img->cof[1+ll][4][0][0]=(img->cofu[0]-img->cofu[1]+img->cofu[2]-img->cofu[3])>>1;
        img->cof[0+ll][5][0][0]=(img->cofu[0]+img->cofu[1]-img->cofu[2]-img->cofu[3])>>1;
        img->cof[1+ll][5][0][0]=(img->cofu[0]-img->cofu[1]-img->cofu[2]+img->cofu[3])>>1;
      }
    }
  }

  // chroma AC coeff, all zero fram start_scan
  if (cbp<=31)
    for (j=4; j < 6; j++)
      for (i=0; i < 4; i++)
        img->nz_coeff [img->mb_x ][img->mb_y ][i][j]=0;


  // chroma AC coeff, all zero fram start_scan
  uv=-1;
  if (cbp>31)
  {
    block_y=4;
    for (block_x=0; block_x < 4; block_x += 2)
    {
      for (j=block_y; j < block_y+2; j++)
      {
        jj=j/2;
        j1=j-4;
        for (i=block_x; i < block_x+2; i++)
        {

          ii=i/2;
          i1=i%2;

          if (inp->symbol_mode == UVLC)
          {
            readCoeff4x4_CAVLC(img, inp, CHROMA_AC, i, j,
                                levarr, runarr, &numcoeff);
            coef_ctr=0;
            level=1;
            uv++;
            for(k = 0; k < numcoeff;k++)
            {
              if (levarr[k] != 0)
              {
                currMB->cbp_blk |= 1 << (16 + (j1<<1) + i1 + (block_x<<1) ) ;
                coef_ctr=coef_ctr+runarr[k]+1;
                i0=SNGL_SCAN[coef_ctr][0];
                j0=SNGL_SCAN[coef_ctr][1];
                img->cof[i][j][i0][j0]=levarr[k]*dequant_coef[qp_rem_uv][i0][j0]<<qp_per_uv;
              }
            }
          }

          else
          {
            coef_ctr=0;
            level=1;
            uv++;

            img->subblock_y = j/5;
            img->subblock_x = i%2;

            for(k=0;(k<16)&&(level!=0);k++)
            {
              currSE.context      = CHROMA_AC;
              currSE.type         = (IS_INTRA(currMB) ? SE_CHR_AC_INTRA : SE_CHR_AC_INTER);
              img->is_intra_block =  IS_INTRA(currMB);
              img->is_v_block     = (uv>=4);

#if TRACE
              snprintf(currSE.tracestring, TRACESTRING_SIZE, " AC Chroma ");
#endif
              if(img->type == B_IMG_1 || img->type == B_IMG_MULT)
                dP = &(currSlice->partArr[partMap[SE_BFRAME]]);
              else
                dP = &(currSlice->partArr[partMap[currSE.type]]);
            
              if (inp->symbol_mode == UVLC || dP->bitstream->ei_flag)
                currSE.mapping = linfo_levrun_inter;
              else
                currSE.reading = readRunLevelFromBuffer_CABAC;
                       
              dP->readSyntaxElement(&currSE,img,inp,dP);
              level = currSE.value1;
              run = currSE.value2;
              len = currSE.len;

              if (level != 0)
              {
                currMB->cbp_blk |= 1 << (16 + (j1<<1) + i1 + (block_x<<1) ) ;
                coef_ctr=coef_ctr+run+1;
                i0=SNGL_SCAN[coef_ctr][0];
                j0=SNGL_SCAN[coef_ctr][1];
                img->cof[i][j][i0][j0]=level*dequant_coef[qp_rem_uv][i0][j0]<<qp_per_uv;
              }
            }
          }
        }
      }
    }
  }
}



/*!
 ************************************************************************
 * \brief
 *    decode one macroblock
 ************************************************************************
 */

int decode_one_macroblock(struct img_par *img,struct inp_par *inp)
{
  int tmp_block[BLOCK_SIZE][BLOCK_SIZE];
  int tmp_blockbw[BLOCK_SIZE][BLOCK_SIZE];
  int js[2][2];
  int i=0,j=0,k,ii=0,jj=0,i1=0,j1=0,j4=0,i4=0;
  int js0=0,js1=0,js2=0,js3=0,jf=0;
  int uv, hv;
  int vec1_x=0,vec1_y=0,vec2_x=0,vec2_y=0;
  int ioff,joff;
  int block8x8;   // needed for ABT
  int curr_blk[B8_SIZE][B8_SIZE];
  int tmp;

  int bw_pred, fw_pred, ifx;
  int ii0,jj0,ii1,jj1,if1,jf1,if0,jf0;
  int mv_mul,f1,f2,f3,f4;

  const byte decode_block_scan[16] = {0,1,4,5,2,3,6,7,8,9,12,13,10,11,14,15};

  Macroblock *currMB   = &img->mb_data[img->current_mb_nr];
  int refframe, fw_refframe, bw_refframe, mv_mode, pred_dir, intra_prediction; // = currMB->ref_frame;
  int*** mv_array, ***fw_mv_array, ***bw_mv_array;
  int bframe = (img->type==B_IMG_1 || img->type==B_IMG_MULT);
//  byte refP_tr;

  int **fwRefFrArr = img->fw_refFrArr;
  int **bwRefFrArr = img->bw_refFrArr;
  int  ***fw_mv = img->fw_mv;
  int  ***bw_mv = img->bw_mv;
  int  **moving_block_dir = moving_block; 


  int frame_no_next_P, frame_no_B, delta_P;

  int ref;

  int iTRb, iTRp;
  int mv_scale;

  int mb_nr             = img->current_mb_nr;
  int mb_width          = img->width/16;
  int mb_available_up;
  int mb_available_left;
  int smb       = ((img->type==SP_IMG_1 || img->type==SP_IMG_MULT) && IS_INTER (currMB)) || (img->type == SI_IMG && currMB->mb_type == SI4MB);

  int j6,j5 = 0;

  int fwd_refframe_offset,bwd_refframe_offset;
  int direct_pdir;

  if (img->mb_frame_field_flag)
  {
    mb_available_up   = (img->mb_y == 0) ? 0 : 1;
    mb_available_left = (img->mb_x == 0) ? 0 : 1;
  }
  else
  {
    mb_available_up   = (img->mb_y == 0) ? 0 : (img->mb_data[mb_nr].slice_nr == img->mb_data[mb_nr-mb_width].slice_nr);
    mb_available_left = (img->mb_x == 0) ? 0 : (img->mb_data[mb_nr].slice_nr == img->mb_data[mb_nr-1].slice_nr);
  }

  if(img->UseConstrainedIntraPred)
  {
    if (mb_available_up   && (img->intra_block[mb_nr-mb_width][2]==0 || img->intra_block[mb_nr-mb_width][3]==0))
      mb_available_up   = 0;
    if (mb_available_left && (img->intra_block[mb_nr-       1][1]==0 || img->intra_block[mb_nr       -1][3]==0))
      mb_available_left = 0;
  }

  if(bframe)
  {
    int current_tr  = (img->structure==TOP_FIELD || img->structure==BOTTOM_FIELD)?img->tr_fld:2*img->tr_frm;
    if(img->imgtr_next_P <= current_tr)
      fwd_refframe_offset = 0;
    else if (img->structure==FRAME)
      fwd_refframe_offset = 1;
    else
      fwd_refframe_offset = 2;
  }
  else
  {
    fwd_refframe_offset = 0;
  }

  if (bframe && img->disposable_flag)
  {
    if(img->structure == TOP_FIELD)
      bwd_refframe_offset = 1;
    else
      bwd_refframe_offset = 0;
  }
  else
  {
    bwd_refframe_offset = 0;
  }

#define fwd_ref_idx_to_refframe(idx) ((idx)+fwd_refframe_offset)
#define bwd_ref_idx_to_refframe(idx) ((idx)+bwd_refframe_offset)


  // set variables depending on mv_res
  if(img->mv_res)
  {
    mv_mul=8;
    f1=16;
    f2=15;
  }
  else
  {
    mv_mul=4;
    f1=8;
    f2=7;
  }

  f3=f1*f1;
  f4=f3/2;

  // luma decoding **************************************************

  // get prediction for INTRA_MB_16x16
  if (IS_NEWINTRA (currMB))
  {
    intrapred_luma_2(img, currMB->i16mode);
  }

  for (block8x8=0; block8x8<4; block8x8++)
  {
    if( !currMB->useABT[block8x8] || currMB->b8mode[block8x8]!=IBLOCK )  // intra ABT blocks are regarded below
    {

#ifdef MBINTLC
              // skipped MB processing for field MB  ***********************************
      if (IS_DIRECT (currMB) && img->mb_frame_field_flag && img->current_mb_nr%2 ==0)      
        decode_skip_Direct_topMB(img,inp);
#endif
      for (k = block8x8*4; k < block8x8*4+4; k ++)
      {
        i = (decode_block_scan[k] & 3);
        j = ((decode_block_scan[k] >> 2) & 3);

        ioff=i*4;
        i4=img->block_x+i;

        joff=j*4;
        j4=img->block_y+j;

        if (img->mb_frame_field_flag) 
        {
          j5 = img->block_y / 2 + j;
          if (img->current_mb_nr%2)
            j5 -= BLOCK_SIZE / 2;
        }
        mv_mode  = currMB->b8mode[2*(j/2)+(i/2)];
        pred_dir = currMB->b8pdir[2*(j/2)+(i/2)];

        // PREDICTION
        if (mv_mode==IBLOCK)
        {
          //===== INTRA PREDICTION =====
          if (intrapred(img,ioff,joff,i4,j4)==SEARCH_SYNC)  /* make 4x4 prediction block mpr from given prediction img->mb_mode */
            return SEARCH_SYNC;                   /* bit error */
        }
        else if (!IS_NEWINTRA (currMB))
        {
          if (pred_dir != 2)
          {
            //===== FORWARD/BACKWARD PREDICTION =====
            if (!bframe)
            {
              //refframe = (img->frame_cycle + img->buf_cycle - refFrArr[j4][i4]) % img->buf_cycle;
              refframe = refFrArr[j4][i4];
              mv_array = img->mv;
            }
            else if (!pred_dir)
            {
              refframe = fwd_ref_idx_to_refframe(img->fw_refFrArr[j4][i4]);

              if (img->mb_frame_field_flag)
                mv_array = img->fw_mv_frm;
              else
                mv_array = img->fw_mv;
            }
            else
            {
              //refframe = (img->frame_cycle + img->buf_cycle) % img->buf_cycle;
              refframe = bwd_ref_idx_to_refframe(img->bw_refFrArr[j4][i4]);

              if (img->mb_frame_field_flag)
                mv_array = img->bw_mv_frm;
              else
                mv_array = img->bw_mv;
            }

            vec1_x = i4*4*mv_mul + mv_array[i4+BLOCK_SIZE][j4][0];
            vec1_y = j4*4*mv_mul + mv_array[i4+BLOCK_SIZE][j4][1];

            get_block (refframe, vec1_x, vec1_y, img, tmp_block);

            for(ii=0;ii<BLOCK_SIZE;ii++)
              for(jj=0;jj<BLOCK_SIZE;jj++)  img->mpr[ii+ioff][jj+joff] = tmp_block[ii][jj];
          }
          else
          {
            if (mv_mode != 0)
            {
              //===== BI-DIRECTIONAL PREDICTION =====
              if (img->structure==FRAME && img->mb_frame_field_flag)
              {
                fw_mv_array = img->fw_mv_frm;
                bw_mv_array = img->bw_mv_frm;
              }
              else
              {
                fw_mv_array = img->fw_mv;
                bw_mv_array = img->bw_mv;
              }

              fw_refframe = fwd_ref_idx_to_refframe(img->fw_refFrArr[j4][i4]);
              bw_refframe = bwd_ref_idx_to_refframe(img->bw_refFrArr[j4][i4]);
            }
            else
            {
              //===== DIRECT PREDICTION =====
              fw_mv_array = img->dfMV;
              bw_mv_array = img->dbMV;
              bw_refframe = 0;
              if (img->structure==TOP_FIELD)  bw_refframe = 1;

              if (img->direct_type && img->mb_frame_field_flag )
              {
                if (!img->mb_field)
                {
                  fwRefFrArr= img->fw_refFrArr_frm;
                  bwRefFrArr= img->bw_refFrArr_frm;
                  fw_mv=img->fw_mv_frm;
                  bw_mv=img->bw_mv_frm;
                }
                else if (img->current_mb_nr%2 )
                {
                  fwRefFrArr= img->fw_refFrArr_bot;
                  bwRefFrArr= img->bw_refFrArr_bot;
                  fw_mv=img->fw_mv_bot;
                  bw_mv=img->bw_mv_bot;
                  moving_block_dir = moving_block_bot; 
                }
                else
                {
                  fwRefFrArr=img->fw_refFrArr_top ;
                  bwRefFrArr=img->bw_refFrArr_top ;
                  fw_mv=img->fw_mv_top;
                  bw_mv=img->bw_mv_top;
                  moving_block_dir = moving_block_top; 
                }
              }

              if (img->direct_type )
              {
                int pic_blockx           = img->block_x;
                int pic_blocky           = (img->mb_frame_field_flag && img->mb_field)? img->block_y/2:img->block_y;
                int mb_nr                = img->current_mb_nr;
                int mb_width             = img->width/16;
                int mb_available_up =      (img->mb_y == 0        ) ? 0 : (img->mb_frame_field_flag? 1 :(currMB->slice_nr == img->mb_data[mb_nr-mb_width].slice_nr));
                int mb_available_left =    (img->mb_x == 0        ) ? 0 : (currMB->slice_nr == img->mb_data[mb_nr-1].slice_nr);
                int mb_available_upleft  = (img->mb_x == 0          ||
                  img->mb_y == 0        ) ? 0 : (img->mb_frame_field_flag)? 1 :(currMB->slice_nr == img->mb_data[mb_nr-mb_width-1].slice_nr);
                int mb_available_upright = (img->mb_frame_field_flag && img->current_mb_nr%2)?0:(img->mb_x >= mb_width-1 ||
                  img->mb_y == 0        ) ? 0 : (img->mb_frame_field_flag)? 1 :(currMB->slice_nr == img->mb_data[mb_nr-mb_width+1].slice_nr);
                
                int fw_rFrameL              = mb_available_left    ? fwRefFrArr[pic_blocky]  [pic_blockx-1]   : -1;
                int fw_rFrameU              = mb_available_up      ? fwRefFrArr[pic_blocky-1][pic_blockx]     : -1;
                int fw_rFrameUL             = mb_available_upleft  ? fwRefFrArr[pic_blocky-1][pic_blockx-1]   : -1;
                int fw_rFrameUR             = mb_available_upright ? fwRefFrArr[pic_blocky-1][pic_blockx+4]   : fw_rFrameUL;  
                
                int bw_rFrameL              = mb_available_left    ? bwRefFrArr[pic_blocky]  [pic_blockx-1]   : -1;
                int bw_rFrameU              = mb_available_up      ? bwRefFrArr[pic_blocky-1][pic_blockx]     : -1;
                int bw_rFrameUL             = mb_available_upleft  ? bwRefFrArr[pic_blocky-1][pic_blockx-1]   : -1;
                int bw_rFrameUR             = mb_available_upright ? bwRefFrArr[pic_blocky-1][pic_blockx+4]   : bw_rFrameUL;
                
                int fw_rFrame,bw_rFrame;
                int pmvfw[2]={0,0},pmvbw[2]={0,0};

                                
                j6= pic_blocky+j;
                
                if (!fw_rFrameL || !fw_rFrameU || !fw_rFrameUR)
                  fw_rFrame=0;
                else
                  fw_rFrame=min(fw_rFrameL&15,min(fw_rFrameU&15,fw_rFrameUR&15));

                if(img->num_ref_pic_active_bwd>1 && (bw_rFrameL==1 || bw_rFrameU==1 || bw_rFrameUR==1))
                  bw_rFrame=1;
                else if (!bw_rFrameL || !bw_rFrameU || !bw_rFrameUR)
                  bw_rFrame=0;
                else
                  bw_rFrame=min(bw_rFrameL&15,min(bw_rFrameU&15,bw_rFrameUR&15));

                if (fw_rFrame !=15)
                  SetMotionVectorPredictor (img, pmvfw, pmvfw+1, fw_rFrame, fwRefFrArr, fw_mv, 0, 0, 16, 16);  
                if (bw_rFrame !=15)
                  SetMotionVectorPredictor (img, pmvbw, pmvbw+1, bw_rFrame, bwRefFrArr, bw_mv, 0, 0, 16, 16);  
                
                ref=refFrArr[j4][i4];
                if (fw_rFrame !=15)
//                                if (fw_rFrame !=15)
                {
                  if (!fw_rFrame  && !moving_block_dir[j6][i4]) 
                  {
                    img->fw_mv[i4+BLOCK_SIZE][j4][0]=img->dfMV[i4+BLOCK_SIZE][j4][0] = 0;
                    img->fw_mv[i4+BLOCK_SIZE][j4][1]=img->dfMV[i4+BLOCK_SIZE][j4][1] = 0;
                    if (img->mb_frame_field_flag)
                    {
                      if (img->current_mb_nr%2 == 0)
                      {
                        img->dfMV_top[i4+BLOCK_SIZE][j5][0]=img->fw_mv_top[i4+BLOCK_SIZE][j5][0]=0;
                        img->dfMV_top[i4+BLOCK_SIZE][j5][1]=img->fw_mv_top[i4+BLOCK_SIZE][j5][1]=0;
                        img->fw_refFrArr_top[j5][i4]=0;
                      }
                      else
                      {
                        img->dfMV_bot[i4+BLOCK_SIZE][j5][0]=img->fw_mv_bot[i4+BLOCK_SIZE][j5][0]=0;
                        img->dfMV_bot[i4+BLOCK_SIZE][j5][1]=img->fw_mv_bot[i4+BLOCK_SIZE][j5][1]=0;
                        img->fw_refFrArr_bot[j5][i4]=0;
                      }
                    } 
                    if (img->structure == TOP_FIELD)
                      fwRefFrArr[j6][i4] = 0;
                    else
                      fwRefFrArr[j6][i4] = 0; 
                  }
                  else
                  {
                    img->fw_mv[i4+BLOCK_SIZE][j4][0]=img->dfMV[i4+BLOCK_SIZE][j4][0] = pmvfw[0];
                    img->fw_mv[i4+BLOCK_SIZE][j4][1]=img->dfMV[i4+BLOCK_SIZE][j4][1] = pmvfw[1];
                    if (img->structure == TOP_FIELD)
                      fwRefFrArr[j6][i4] = fw_rFrame ;
                    else
                      fwRefFrArr[j6][i4] = fw_rFrame; 
                    if (img->mb_frame_field_flag)
                    {
                      if (img->current_mb_nr%2 == 0)
                      {
                        img->dfMV_top[i4+BLOCK_SIZE][j5][0]=img->fw_mv_top[i4+BLOCK_SIZE][j5][0]=pmvfw[0];
                        img->dfMV_top[i4+BLOCK_SIZE][j5][1]=img->fw_mv_top[i4+BLOCK_SIZE][j5][1]=pmvfw[1]/2;
                        img->fw_refFrArr_top[j5][i4]=2*fw_rFrame;
                      }
                      else
                      {
                        img->dfMV_bot[i4+BLOCK_SIZE][j5][0]=img->fw_mv_bot[i4+BLOCK_SIZE][j5][0]=pmvfw[0];
                        img->dfMV_bot[i4+BLOCK_SIZE][j5][1]=img->fw_mv_bot[i4+BLOCK_SIZE][j5][1]=pmvfw[1]/2;
                        img->fw_refFrArr_bot[j5][i4]=2*fw_rFrame;
                      }
                    }
                  }
                }
                else
                {
                  img->fw_refFrArr[j4][i4]=-1;
                  img->fw_mv[i4+BLOCK_SIZE][j4][0]=img->dfMV[i4+BLOCK_SIZE][j4][0] = 0;
                  img->fw_mv[i4+BLOCK_SIZE][j4][1]=img->dfMV[i4+BLOCK_SIZE][j4][1] = 0;
                  if (img->mb_frame_field_flag)
                  {
                    if (img->current_mb_nr%2 == 0)
                    {
                      img->dfMV_top[i4+BLOCK_SIZE][j5][0]=img->fw_mv_top[i4+BLOCK_SIZE][j5][0]=0;
                      img->dfMV_top[i4+BLOCK_SIZE][j5][1]=img->fw_mv_top[i4+BLOCK_SIZE][j5][1]=0;
                      img->fw_refFrArr_top[j5][i4]=-1;
                    }
                    else
                    {
                      img->dfMV_bot[i4+BLOCK_SIZE][j5][0]=img->fw_mv_bot[i4+BLOCK_SIZE][j5][0]=0;
                      img->dfMV_bot[i4+BLOCK_SIZE][j5][1]=img->fw_mv_bot[i4+BLOCK_SIZE][j5][1]=0;
                      img->fw_refFrArr_bot[j5][i4]=-1;
                    }
                  }
                }
                
                if (bw_rFrame !=15)
                {
                  if  (bw_rFrame==((img->num_ref_pic_active_bwd>1)?1:0) && !moving_block_dir[j6][i4])  
                  {                  
                    
                    img->bw_mv[i4+BLOCK_SIZE][j4][0]=img->dbMV[i4+BLOCK_SIZE][j4][0] = 0;
                    img->bw_mv[i4+BLOCK_SIZE][j4][1]=img->dbMV[i4+BLOCK_SIZE][j4][1] = 0;
                    bwRefFrArr[j4][i4]=bw_rFrame;
                    if (img->mb_frame_field_flag)
                    {
                      if (img->current_mb_nr%2 == 0)
                      {
                        img->dbMV_top[i4+BLOCK_SIZE][j5][0]=img->bw_mv_top[i4+BLOCK_SIZE][j5][0]=0;
                        img->dbMV_top[i4+BLOCK_SIZE][j5][1]=img->bw_mv_top[i4+BLOCK_SIZE][j5][1]=0;
                        img->bw_refFrArr_top[j5][i4]=2*bw_rFrame;
                      }
                      else
                      {
                        img->dbMV_bot[i4+BLOCK_SIZE][j5][0]=img->bw_mv_bot[i4+BLOCK_SIZE][j5][0]=0;
                        img->dbMV_bot[i4+BLOCK_SIZE][j5][1]=img->bw_mv_bot[i4+BLOCK_SIZE][j5][1]=0;
                        img->bw_refFrArr_bot[j5][i4]=2*bw_rFrame;
                      }
                    }                    
                  }
                  else
                  {
                    img->bw_mv[i4+BLOCK_SIZE][j4][0]=img->dbMV[i4+BLOCK_SIZE][j4][0] = pmvbw[0];
                    img->bw_mv[i4+BLOCK_SIZE][j4][1]=img->dbMV[i4+BLOCK_SIZE][j4][1] = pmvbw[1];
                    bwRefFrArr[j4][i4]=bw_rFrame;
                    if (img->mb_frame_field_flag)
                    {
                      if (img->current_mb_nr%2 == 0)
                      {
                        img->dbMV_top[i4+BLOCK_SIZE][j5][0]=img->bw_mv_top[i4+BLOCK_SIZE][j5][0]=pmvbw[0];
                        img->dbMV_top[i4+BLOCK_SIZE][j5][1]=img->bw_mv_top[i4+BLOCK_SIZE][j5][1]=pmvbw[1]/2;
                        img->bw_refFrArr_top[j5][i4]=2*bw_rFrame;
                      }
                      else
                      {
                        img->dbMV_bot[i4+BLOCK_SIZE][j5][0]=img->bw_mv_bot[i4+BLOCK_SIZE][j5][0]=pmvbw[0];
                        img->dbMV_bot[i4+BLOCK_SIZE][j5][1]=img->bw_mv_bot[i4+BLOCK_SIZE][j5][1]=pmvbw[1]/2;
                        img->bw_refFrArr_bot[j5][i4]=2*bw_rFrame;
                      }
                    }
                  }               
                }
                else
                {                  
                  img->bw_mv[i4+BLOCK_SIZE][j4][0]=img->dbMV[i4+BLOCK_SIZE][j4][0] = 0;
                  img->bw_mv[i4+BLOCK_SIZE][j4][1]=img->dbMV[i4+BLOCK_SIZE][j4][1] = 0;
                  bwRefFrArr[j6][i4]=-1;
                  
                  if (img->mb_frame_field_flag)
                  {
                    if (img->current_mb_nr%2 == 0)
                    {
                      img->dbMV_top[i4+BLOCK_SIZE][j5][0]=img->bw_mv_top[i4+BLOCK_SIZE][j5][0]=0;
                      img->dbMV_top[i4+BLOCK_SIZE][j5][1]=img->bw_mv_top[i4+BLOCK_SIZE][j5][1]=0;
                      img->bw_refFrArr_top[j5][i4]=-1;
                    }
                    else
                    {
                      img->dbMV_bot[i4+BLOCK_SIZE][j5][0]=img->bw_mv_bot[i4+BLOCK_SIZE][j5][0]=0;
                      img->dbMV_bot[i4+BLOCK_SIZE][j5][1]=img->bw_mv_bot[i4+BLOCK_SIZE][j5][1]=0;
                      img->bw_refFrArr_bot[j5][i4]=-1;
                    }
                  }
                }
                if (fw_rFrame ==15 && bw_rFrame ==15)
                {
                  if (img->structure == TOP_FIELD)
                    fwRefFrArr[j6][i4] = 0;
                  else
                    fwRefFrArr[j6][i4] = 0;
                  bwRefFrArr[j6][i4] = (img->num_ref_pic_active_bwd>1)?1:0;

                  if (img->mb_frame_field_flag)
                  {
                    if (img->current_mb_nr%2 == 0)
                    {
                      img->fw_refFrArr_top[j5][i4]=0;
                      img->bw_refFrArr_top[j5][i4]=2*bwRefFrArr[j6][i4];
                    }
                    else
                    {
                      img->fw_refFrArr_bot[j5][i4]=0; 
                      img->bw_refFrArr_bot[j5][i4]=2*bwRefFrArr[j6][i4];
                    }
                  }
                }
                if (img->mb_frame_field_flag)
                {
                  img->fw_mv_frm[i4+BLOCK_SIZE][j4][0]=img->fw_mv[i4+BLOCK_SIZE][j4][0];
                  img->fw_mv_frm[i4+BLOCK_SIZE][j4][1]=img->fw_mv[i4+BLOCK_SIZE][j4][1];
                  img->bw_mv_frm[i4+BLOCK_SIZE][j4][0]=img->bw_mv[i4+BLOCK_SIZE][j4][0];
                  img->bw_mv_frm[i4+BLOCK_SIZE][j4][1]=img->bw_mv[i4+BLOCK_SIZE][j4][1];
                }

                // Revisit Alexis
                fw_refframe = (fwRefFrArr[j6][i4]!=-1) ? fwd_ref_idx_to_refframe(fwRefFrArr[j6][i4]):0;
                bw_refframe = (bwRefFrArr[j6][i4]!=-1) ? bwd_ref_idx_to_refframe(bwRefFrArr[j6][i4]):0;

                if      (bwRefFrArr[j6][i4]==-1) direct_pdir = 0;
                else if (fwRefFrArr[j6][i4]==-1) direct_pdir = 1;
                else                             direct_pdir = 2;

                if(img->explicit_B_prediction==1 && direct_pdir==2) 
                {
                  currMB->bipred_weighting_type[2*(j/2)+(i/2)] = (bwRefFrArr[j6][i4] > fwRefFrArr[j6][i4]);
                }

              }
              else // Temporal Mode
              {
                if(refFrArr[j4][i4]==-1) // next P is intra mode
                {
                  for(hv=0; hv<2; hv++)
                  {
                    img->dfMV[i4+BLOCK_SIZE][j4][hv]=img->dbMV[i4+BLOCK_SIZE][j4][hv]=0;
                    img->fw_mv[i4+BLOCK_SIZE][j4][hv]=img->bw_mv[i4+BLOCK_SIZE][j4][hv]=0;
                    
                    if (img->mb_frame_field_flag)
                    {
                      if (img->current_mb_nr%2 == 0)
                      {
                        img->dfMV_top[i4+BLOCK_SIZE][j5][hv]=img->dbMV_top[i4+BLOCK_SIZE][j5][hv]=0;
                        img->fw_mv_top[i4+BLOCK_SIZE][j5][hv]=img->bw_mv_top[i4+BLOCK_SIZE][j5][hv]=0;
                      }
                      else
                      {
                        img->dfMV_bot[i4+BLOCK_SIZE][j5][hv]=img->dbMV_bot[i4+BLOCK_SIZE][j5][hv]=0;
                        img->fw_mv_bot[i4+BLOCK_SIZE][j5][hv]=img->bw_mv_bot[i4+BLOCK_SIZE][j5][hv]=0;
                      }
                    }
                  }
                  img->fw_refFrArr[j4][i4]=-1;
                  img->bw_refFrArr[j4][i4]=-1;
                  
                  if (img->mb_frame_field_flag)
                  {
                    if (img->current_mb_nr%2 == 0)
                    {
                      img->fw_refFrArr_top[j5][i4]=-1;
                      img->bw_refFrArr_top[j5][i4]=-1;
                    }
                    else
                    {
                      img->fw_refFrArr_bot[j5][i4]=-1;
                      img->bw_refFrArr_bot[j5][i4]=-1;
                    }
                  }
                  
                  if (img->structure == FRAME)
                    fw_refframe = 1;
                  else
                  {
                    if (img->structure == TOP_FIELD)
                      fw_refframe = 2;    // used to be 3;
                    else
                      fw_refframe = 1;    // used to be 2
                  }
                }
                else // next P is skip or inter mode
                {                  
                  refframe = refFrArr[j4][i4];
                  frame_no_next_P =img->imgtr_next_P+((mref==mref_fld)&&(img->structure==BOTTOM_FIELD));
                  //frame_no_B = (mref==mref_fld) ? img->tr : 2*img->tr;
                  frame_no_B = (img->structure==TOP_FIELD || img->structure==BOTTOM_FIELD) ? img->tr_fld : 2*img->tr_frm;
                  delta_P = (img->imgtr_next_P - img->imgtr_last_P);
                  
                  if((mref==mref_fld) && (img->structure==TOP_FIELD)) // top field
                  {
                    iTRp = delta_P*(refframe/2+1)-(refframe+1)%2;
                  }
                  else if((mref==mref_fld) && (img->structure==BOTTOM_FIELD)) // bot field
                  {
                    iTRp = 1+delta_P*(refframe+1)/2-refframe%2;
                  }
                  else  // frame
                  {
                    iTRp = (refframe+1)*delta_P;
                  }
                  
                  iTRb = iTRp - (frame_no_next_P - frame_no_B);
                  mv_scale = (iTRb * 256)/ iTRp;  //! Note that this could be precomputed at the frame/slice level. 
                  //! In such case, an array is needed for each different reference.
                  img->dfMV[i4+BLOCK_SIZE][j4][0]=(mv_scale * img->mv[i4+BLOCK_SIZE][j4][0] + 128 ) >> 8;
                  img->dfMV[i4+BLOCK_SIZE][j4][1]=(mv_scale * img->mv[i4+BLOCK_SIZE][j4][1] + 128 ) >> 8;
                  img->dbMV[i4+BLOCK_SIZE][j4][0]=((mv_scale-256) * img->mv[i4+BLOCK_SIZE][j4][0] + 128 ) >> 8;
                  img->dbMV[i4+BLOCK_SIZE][j4][1]=((mv_scale-256) * img->mv[i4+BLOCK_SIZE][j4][1] + 128 ) >> 8;

                  if (img->mb_frame_field_flag)
                  {
                    if (img->current_mb_nr%2 == 0)
                    {
                      img->dfMV_top[i4+BLOCK_SIZE][j5][0]=(mv_scale * img->mv[i4+BLOCK_SIZE][j4][0] + 128 ) >> 8;
                      img->dfMV_top[i4+BLOCK_SIZE][j5][1]=(mv_scale/2 * img->mv[i4+BLOCK_SIZE][j4][1] + 128) >> 8;
                      img->dbMV_top[i4+BLOCK_SIZE][j5][0]=((mv_scale-256)*img->mv[i4+BLOCK_SIZE][j4][0] + 128 ) >> 8;
                    }
                    else
                    {
                      img->dfMV_bot[i4+BLOCK_SIZE][j5][0]=(mv_scale * img->mv[i4+BLOCK_SIZE][j4][0] + 128) >> 8;
                      img->dfMV_bot[i4+BLOCK_SIZE][j5][1]=(mv_scale/2 * img->mv[i4+BLOCK_SIZE][j4][1] + 128) >> 8;
                      img->dbMV_bot[i4+BLOCK_SIZE][j5][0]=((mv_scale-256)*img->mv[i4+BLOCK_SIZE][j4][0] + 128) >> 8;
                      img->dbMV_bot[i4+BLOCK_SIZE][j5][1]=((mv_scale/2-256)*img->mv[i4+BLOCK_SIZE][j4][1] + 128) >> 8;
                    }
                  }
                  
                  if (img->structure == TOP_FIELD)
                    fw_refframe = refFrArr[j4][i4]+2;
                  else
                  {
                    fw_refframe = max(0,refFrArr[j4][i4]) + 1;  // DIRECT
                  }
                  img->fw_mv[i4+BLOCK_SIZE][j4][0]=img->dfMV[i4+BLOCK_SIZE][j4][0];
                  img->fw_mv[i4+BLOCK_SIZE][j4][1]=img->dfMV[i4+BLOCK_SIZE][j4][1];
                  img->bw_mv[i4+BLOCK_SIZE][j4][0]=img->dbMV[i4+BLOCK_SIZE][j4][0];
                  img->bw_mv[i4+BLOCK_SIZE][j4][1]=img->dbMV[i4+BLOCK_SIZE][j4][1];
                  
                  if (img->mb_frame_field_flag)
                  {
                    if (img->current_mb_nr%2 == 0)
                    {
                      img->fw_mv_top[i4+BLOCK_SIZE][j5][0]=img->dfMV_top[i4+BLOCK_SIZE][j5][0]=img->fw_mv[i4+BLOCK_SIZE][j4][0];
                      img->fw_mv_top[i4+BLOCK_SIZE][j5][1]=img->dfMV_top[i4+BLOCK_SIZE][j5][1]=img->fw_mv[i4+BLOCK_SIZE][j4][1]/2;
                      img->bw_mv_top[i4+BLOCK_SIZE][j5][0]=img->dbMV_top[i4+BLOCK_SIZE][j5][0]=img->bw_mv[i4+BLOCK_SIZE][j4][0];
                      img->bw_mv_top[i4+BLOCK_SIZE][j5][1]=img->dbMV_top[i4+BLOCK_SIZE][j5][1]=img->bw_mv[i4+BLOCK_SIZE][j4][1]/2;
                      
                      img->fw_mv_frm[i4+BLOCK_SIZE][j4][0]=img->fw_mv[i4+BLOCK_SIZE][j4][0];
                      img->fw_mv_frm[i4+BLOCK_SIZE][j4][1]=img->fw_mv[i4+BLOCK_SIZE][j4][1];
                      img->bw_mv_frm[i4+BLOCK_SIZE][j4][0]=img->bw_mv[i4+BLOCK_SIZE][j4][0];
                      img->bw_mv_frm[i4+BLOCK_SIZE][j4][1]=img->bw_mv[i4+BLOCK_SIZE][j4][1];
                    }
                    else
                    {
                      img->fw_mv_bot[i4+BLOCK_SIZE][j5][0]=img->dfMV_bot[i4+BLOCK_SIZE][j5][0]=img->fw_mv[i4+BLOCK_SIZE][j4][0];
                      img->fw_mv_bot[i4+BLOCK_SIZE][j5][1]=img->dfMV_bot[i4+BLOCK_SIZE][j5][1]=img->fw_mv[i4+BLOCK_SIZE][j4][1]/2;
                      img->bw_mv_bot[i4+BLOCK_SIZE][j5][0]=img->dbMV_bot[i4+BLOCK_SIZE][j5][0]=img->bw_mv[i4+BLOCK_SIZE][j4][0];
                      img->bw_mv_bot[i4+BLOCK_SIZE][j5][1]=img->dbMV_bot[i4+BLOCK_SIZE][j5][1]=img->bw_mv[i4+BLOCK_SIZE][j4][1]/2;
                      
                      img->fw_mv_frm[i4+BLOCK_SIZE][j4][0]=img->fw_mv[i4+BLOCK_SIZE][j4][0];
                      img->fw_mv_frm[i4+BLOCK_SIZE][j4][1]=img->fw_mv[i4+BLOCK_SIZE][j4][1];
                      img->bw_mv_frm[i4+BLOCK_SIZE][j4][0]=img->bw_mv[i4+BLOCK_SIZE][j4][0];
                      img->bw_mv_frm[i4+BLOCK_SIZE][j4][1]=img->bw_mv[i4+BLOCK_SIZE][j4][1];
                    }
                  }
                  
                  if (img->structure == TOP_FIELD)
                    img->fw_refFrArr[j4][i4]=refFrArr[j4][i4] + 0;    // PLUS2, Krit, 7/05 (used to be + 1)
                  else
                    img->fw_refFrArr[j4][i4]=refFrArr[j4][i4];
                  img->bw_refFrArr[j4][i4]=0;
                  
                  if (img->mb_frame_field_flag)
                  {
                    if (img->current_mb_nr%2 == 0)
                    {
                      img->fw_refFrArr_top[j5][i4] = (refFrArr[j4][i4] == -1) ? -1:2*refFrArr[j4][i4];
                      img->bw_refFrArr_top[j5][i4] = (bw_refframe == -1) ? -1:2*bw_refframe;  // will always be 0
                    }
                    else
                    {
                      img->fw_refFrArr_bot[j5][i4] = (refFrArr[j4][i4] == -1) ? -1:2*refFrArr[j4][i4];
                      img->bw_refFrArr_bot[j5][i4] = (bw_refframe == -1) ? -1:2*bw_refframe;  // will always be 0
                    }
                  }
              }
              }
            }            
            if (mv_mode==0 && img->direct_type )
            {
              if (fwRefFrArr[j6][i4] >= 0)
              {
                vec1_x = i4*4*mv_mul + fw_mv_array[i4+BLOCK_SIZE][j4][0];
                vec1_y = j4*4*mv_mul + fw_mv_array[i4+BLOCK_SIZE][j4][1];
                get_block(fw_refframe, vec1_x, vec1_y, img, tmp_block);
              }
              
              if (bwRefFrArr[j6][i4] >= 0)
              {
                vec2_x = i4*4*mv_mul + bw_mv_array[i4+BLOCK_SIZE][j4][0];
                vec2_y = j4*4*mv_mul + bw_mv_array[i4+BLOCK_SIZE][j4][1];
                get_block(bw_refframe, vec2_x, vec2_y, img, tmp_blockbw);
              }              
            }
            else
            {
              vec1_x = i4*4*mv_mul + fw_mv_array[i4+BLOCK_SIZE][j4][0];
              vec1_y = j4*4*mv_mul + fw_mv_array[i4+BLOCK_SIZE][j4][1];
              vec2_x = i4*4*mv_mul + bw_mv_array[i4+BLOCK_SIZE][j4][0];
              vec2_y = j4*4*mv_mul + bw_mv_array[i4+BLOCK_SIZE][j4][1];
              
              get_block(fw_refframe, vec1_x, vec1_y, img, tmp_block);
              get_block(bw_refframe, vec2_x, vec2_y, img, tmp_blockbw);
              
            }

            if (mv_mode==0 && img->direct_type && direct_pdir==0)
            {
              for(ii=0;ii<BLOCK_SIZE;ii++)
                for(jj=0;jj<BLOCK_SIZE;jj++)  
                  img->mpr[ii+ioff][jj+joff] = tmp_block[ii][jj];
            }
            else if (mv_mode==0 && img->direct_type && direct_pdir==1)
            {              
              for(ii=0;ii<BLOCK_SIZE;ii++)
                for(jj=0;jj<BLOCK_SIZE;jj++)  
                  img->mpr[ii+ioff][jj+joff] = tmp_blockbw[ii][jj];
            }
            else if(img->explicit_B_prediction==1)
            {
              if(currMB->bipred_weighting_type[2*(j/2)+(i/2)]==0)
                for(ii=0;ii<BLOCK_SIZE;ii++)
                  for(jj=0;jj<BLOCK_SIZE;jj++)  
                     img->mpr[ii+ioff][jj+joff] = (tmp_block[ii][jj]+tmp_blockbw[ii][jj]+1)/2;
              else 
                for(ii=0;ii<BLOCK_SIZE;ii++)
                  for(jj=0;jj<BLOCK_SIZE;jj++)  
                    img->mpr[ii+ioff][jj+joff] =  min(255,max(0,2*tmp_block[ii][jj]-tmp_blockbw[ii][jj]));
            }
            else
            {
              for(ii=0;ii<BLOCK_SIZE;ii++)
                for(jj=0;jj<BLOCK_SIZE;jj++)  
                  img->mpr[ii+ioff][jj+joff] = (tmp_block[ii][jj]+tmp_blockbw[ii][jj]+1)/2;
            }
          }
        }
        if (currMB->useABT[block8x8]==NO_ABT)
        {
          if (smb && mv_mode!=IBLOCK)
          {
            itrans_sp(img,ioff,joff,i,j);
          }
          else
          {
            itrans   (img,ioff,joff,i,j);      // use DCT transform and make 4x4 block m7 from prediction block mpr
          }
          
          for(ii=0;ii<BLOCK_SIZE;ii++)
          {
            for(jj=0;jj<BLOCK_SIZE;jj++)
            {
              imgY[j4*BLOCK_SIZE+jj][i4*BLOCK_SIZE+ii]=img->m7[ii][jj]; // contruct picture from 4x4 blocks
            }
          }
        }
      }
    }
    if (currMB->useABT[block8x8])
    {
      get_curr_blk (block8x8, img, curr_blk);

      if (currMB->b8mode[block8x8]!=IBLOCK)//((IS_INTER (currMB) && mv_mode!=IBLOCK))
      {
        // inverse transform, copy reconstructed block to img->m7 and imgY
        if ( img->type==SP_IMG_1 || img->type==SP_IMG_MULT || img->type == SI_IMG)
          idct_dequant_abt_sp (block8x8, currMB->abt_mode[block8x8], WHOLE_BLK, WHOLE_BLK, curr_blk, img);
        else
          idct_dequant_abt_B8 (block8x8, currMB->qp+QP_OFS-MIN_QP, currMB->abt_mode[block8x8], WHOLE_BLK, WHOLE_BLK, curr_blk, img);
      }
      else
      {
        ii1=ABT_TRSIZE[currMB->abt_mode[block8x8]][0]>>2;
        jj1=ABT_TRSIZE[currMB->abt_mode[block8x8]][1]>>2;
        for(jj=0;jj<2;jj+=jj1)
          for(ii=0;ii<2;ii+=ii1)
          {
            //intrapred creates the intraprediction in the array img->mpr[x][y] at the location corresponding to the position within the 16x16 MB.
            tmp = intrapred_ABT( img, (img->mb_x<<4)+((block8x8&1)<<3)+(ii<<2),
                                      (img->mb_y<<4)+((block8x8&2)<<2)+(jj<<2), ii1<<2,jj1<<2);
            if ( tmp == SEARCH_SYNC)  /* make 4x4 prediction block mpr from given prediction img->mb_mode */
              return SEARCH_SYNC;                   /* bit error */
            idct_dequant_abt_B8(block8x8,currMB->qp+QP_OFS-MIN_QP,currMB->abt_mode[block8x8],ii<<2,jj<<2,curr_blk,img);
          }
      }
    }
  }

  // skipped MB processing for field MB  ***********************************
  if (IS_COPY (currMB) && img->mb_frame_field_flag && img->current_mb_nr%2 ==0)      
    decode_one_Copy_topMB(img,inp);

  // chroma decoding *******************************************************
  for(uv=0;uv<2;uv++)
  {
    intra_prediction = (IS_NEWINTRA (currMB)        ||
                        currMB->b8mode[0] == IBLOCK ||
                        currMB->b8mode[1] == IBLOCK ||
                        currMB->b8mode[2] == IBLOCK ||
                        currMB->b8mode[3] == IBLOCK);

    if (intra_prediction)
    {
      js0=0;
      js1=0;
      js2=0;
      js3=0;
      for(i=0;i<4;i++)
      {
        if(mb_available_up)
        {
          js0=js0+imgUV[uv][img->pix_c_y-1][img->pix_c_x+i];
          js1=js1+imgUV[uv][img->pix_c_y-1][img->pix_c_x+i+4];
        }
        if(mb_available_left)
        {
          js2=js2+imgUV[uv][img->pix_c_y+i][img->pix_c_x-1];
          js3=js3+imgUV[uv][img->pix_c_y+i+4][img->pix_c_x-1];
        }
      }
      if(mb_available_up && mb_available_left)
      {
        js[0][0]=(js0+js2+4)/8;
        js[1][0]=(js1+2)/4;
        js[0][1]=(js3+2)/4;
        js[1][1]=(js1+js3+4)/8;
      }
      if(mb_available_up && !mb_available_left)
      {
        js[0][0]=(js0+2)/4;
        js[1][0]=(js1+2)/4;
        js[0][1]=(js0+2)/4;
        js[1][1]=(js1+2)/4;
      }
      if(mb_available_left && !mb_available_up)
      {
        js[0][0]=(js2+2)/4;
        js[1][0]=(js2+2)/4;
        js[0][1]=(js3+2)/4;
        js[1][1]=(js3+2)/4;
      }
      if(!mb_available_up && !mb_available_left)
      {
        js[0][0]=128;
        js[1][0]=128;
        js[0][1]=128;
        js[1][1]=128;
      }
    }

    for (j=4;j<6;j++)
    {
      joff=(j-4)*4;
      j4=img->pix_c_y+joff;
      for(i=0;i<2;i++)
      {
        ioff=i*4;
        i4=img->pix_c_x+ioff;

        mv_mode  = currMB->b8mode[2*(j-4)+i];
        pred_dir = currMB->b8pdir[2*(j-4)+i];

        // PREDICTION
        if (mv_mode==IBLOCK || IS_NEWINTRA (currMB))
        {
          //--- INTRA PREDICTION ---
          for (ii=0; ii<4; ii++)
          for (jj=0; jj<4; jj++)
          {
            img->mpr[ii+ioff][jj+joff]=js[i][j-4];
          }
        }
        else if (pred_dir != 2)
        {
          //--- FORWARD/BACKWARD PREDICTION ---
          if (!bframe)        mv_array = img->mv;
          else if (!pred_dir)
          {
              if (img->mb_frame_field_flag)
                mv_array = img->fw_mv_frm;
              else
                mv_array = img->fw_mv;
          }
          else                
          {
              if (img->mb_frame_field_flag)
                mv_array = img->bw_mv_frm;
              else
                mv_array = img->bw_mv;
          }

          for(jj=0;jj<4;jj++)
          {
            jf=(j4+jj)/2;
            for(ii=0;ii<4;ii++)
            {
              if1=(i4+ii)/2;

#if 0
              if (!bframe)        refframe =           refFrArr[jf][if1];
              else if (!pred_dir) refframe = 1+img->fw_refFrArr[jf][if1];
              else                refframe = 0;
#endif

              if (!bframe)        refframe = refFrArr[jf][if1];
              else if (!pred_dir) refframe = fwd_ref_idx_to_refframe(img->fw_refFrArr[jf][if1]);
              else                refframe = bwd_ref_idx_to_refframe(img->bw_refFrArr[jf][if1]);

              i1=(img->pix_c_x+ii+ioff)*f1+mv_array[if1+4][jf][0];
              j1=(img->pix_c_y+jj+joff)*f1+mv_array[if1+4][jf][1];

              ii0=max (0, min (i1/f1, img->width_cr-1));
              jj0=max (0, min (j1/f1, img->height_cr-1));
              ii1=max (0, min ((i1+f2)/f1, img->width_cr-1));
              jj1=max (0, min ((j1+f2)/f1, img->height_cr-1));

              if1=(i1 & f2);
              jf1=(j1 & f2);
              if0=f1-if1;
              jf0=f1-jf1;
              img->mpr[ii+ioff][jj+joff]=(if0*jf0*mcef[refframe][uv][jj0][ii0]+
                                          if1*jf0*mcef[refframe][uv][jj0][ii1]+
                                          if0*jf1*mcef[refframe][uv][jj1][ii0]+
                                          if1*jf1*mcef[refframe][uv][jj1][ii1]+f4)/f3;
            }
          }
        }
        else
        {
          if (mv_mode != 0)
          {
            //===== BI-DIRECTIONAL PREDICTION =====
            if (img->structure==FRAME && img->mb_frame_field_flag)
            {
              fw_mv_array = img->fw_mv_frm;
              bw_mv_array = img->bw_mv_frm;
            }
            else
            {
              fw_mv_array = img->fw_mv;
              bw_mv_array = img->bw_mv;
            }
          }
          else
          {
            //===== DIRECT PREDICTION =====
            fw_mv_array = img->dfMV;
            bw_mv_array = img->dbMV;
          }

          for(jj=0;jj<4;jj++)
          {
            jf=(j4+jj)/2;
            for(ii=0;ii<4;ii++)
            {
              ifx=(i4+ii)/2;

              direct_pdir = 2;

              if (mv_mode != 0)
              {
                fw_refframe = fwd_ref_idx_to_refframe(img->fw_refFrArr[jf][ifx]);
                bw_refframe = bwd_ref_idx_to_refframe(img->bw_refFrArr[jf][ifx]);
              }
              else
              {
                if (!mv_mode && img->direct_type )
                {
                  if (fwRefFrArr[2*(jf/2)][(ifx/2)*2]!=-1)
                    fw_refframe = fwd_ref_idx_to_refframe(fwRefFrArr[2*(jf/2)][(ifx/2)*2]);
                  if (bwRefFrArr[2*(jf/2)][(ifx/2)*2]!=-1)
                    bw_refframe = bwd_ref_idx_to_refframe(bwRefFrArr[2*(jf/2)][(ifx/2)*2]);

                  if      (bwRefFrArr[2*(jf/2)][(ifx/2)*2]==-1) direct_pdir = 0;
                  else if (fwRefFrArr[2*(jf/2)][(ifx/2)*2]==-1) direct_pdir = 1;
                  else                                                direct_pdir = 2;
                }                
                else
                {
                  bw_refframe = 0;
                  if (img->structure==TOP_FIELD)  bw_refframe = 1;
                
                  if (img->structure == FRAME)
                  {
                    if(refFrArr[jf][ifx]==-1)  fw_refframe = 1;
                    else                       fw_refframe = 1+refFrArr[jf][ifx];
                  }
                  else
                  {
                    if (img->structure == TOP_FIELD)
                    {
                      if(refFrArr[jf][ifx]==-1)  fw_refframe = 2;  // used to be 3; DIRECT
                      else                       fw_refframe = 2+refFrArr[jf][ifx];
                    }
                    else
                    {
                      if(refFrArr[jf][ifx]==-1)  fw_refframe = 1;  // used to be 2; DIRECT
                      else                       fw_refframe = max(0,refFrArr[jf][ifx]) + 1;  // DIRECT
                      // used to have 1+refFrArr[jf][ifx]; DIRECT
                    }
                  }
                }
              }


              if (mv_mode==0 && img->direct_type )
              {
                if (direct_pdir == 0 || direct_pdir == 2)
                {
                  i1=(img->pix_c_x+ii+ioff)*f1+fw_mv_array[ifx+4][jf][0];
                  j1=(img->pix_c_y+jj+joff)*f1+fw_mv_array[ifx+4][jf][1];
                  
                  ii0=max (0, min (i1/f1, img->width_cr-1));
                  jj0=max (0, min (j1/f1, img->height_cr-1));
                  ii1=max (0, min ((i1+f2)/f1, img->width_cr-1));
                  jj1=max (0, min ((j1+f2)/f1, img->height_cr-1));
                  
                  if1=(i1 & f2);
                  jf1=(j1 & f2);
                  if0=f1-if1;
                  jf0=f1-jf1;
                  
                  fw_pred=(if0*jf0*mcef[fw_refframe][uv][jj0][ii0]+
                    if1*jf0*mcef[fw_refframe][uv][jj0][ii1]+
                    if0*jf1*mcef[fw_refframe][uv][jj1][ii0]+
                    if1*jf1*mcef[fw_refframe][uv][jj1][ii1]+f4)/f3;
                }
                if (direct_pdir == 1 || direct_pdir == 2)
                {
                  i1=(img->pix_c_x+ii+ioff)*f1+bw_mv_array[ifx+4][jf][0];
                  j1=(img->pix_c_y+jj+joff)*f1+bw_mv_array[ifx+4][jf][1];
                
                  ii0=max (0, min (i1/f1, img->width_cr-1));
                  jj0=max (0, min (j1/f1, img->height_cr-1));
                  ii1=max (0, min ((i1+f2)/f1, img->width_cr-1));
                  jj1=max (0, min ((j1+f2)/f1, img->height_cr-1));
                
                  if1=(i1 & f2);
                  jf1=(j1 & f2);
                  if0=f1-if1;
                  jf0=f1-jf1;
                
                  bw_pred=(if0*jf0*mcef[bw_refframe][uv][jj0][ii0]+
                      if1*jf0*mcef[bw_refframe][uv][jj0][ii1]+
                      if0*jf1*mcef[bw_refframe][uv][jj1][ii0]+
                      if1*jf1*mcef[bw_refframe][uv][jj1][ii1]+f4)/f3;
                }

              }
              else
              {
                i1=(img->pix_c_x+ii+ioff)*f1+fw_mv_array[ifx+4][jf][0];
                j1=(img->pix_c_y+jj+joff)*f1+fw_mv_array[ifx+4][jf][1];
                
                ii0=max (0, min (i1/f1, img->width_cr-1));
                jj0=max (0, min (j1/f1, img->height_cr-1));
                ii1=max (0, min ((i1+f2)/f1, img->width_cr-1));
                jj1=max (0, min ((j1+f2)/f1, img->height_cr-1));
                
                if1=(i1 & f2);
                jf1=(j1 & f2);
                if0=f1-if1;
                jf0=f1-jf1;

                fw_pred=(if0*jf0*mcef[fw_refframe][uv][jj0][ii0]+
                       if1*jf0*mcef[fw_refframe][uv][jj0][ii1]+
                       if0*jf1*mcef[fw_refframe][uv][jj1][ii0]+
                       if1*jf1*mcef[fw_refframe][uv][jj1][ii1]+f4)/f3;

                i1=(img->pix_c_x+ii+ioff)*f1+bw_mv_array[ifx+4][jf][0];
                j1=(img->pix_c_y+jj+joff)*f1+bw_mv_array[ifx+4][jf][1];

                ii0=max (0, min (i1/f1, img->width_cr-1));
                jj0=max (0, min (j1/f1, img->height_cr-1));
                ii1=max (0, min ((i1+f2)/f1, img->width_cr-1));
                jj1=max (0, min ((j1+f2)/f1, img->height_cr-1));

                if1=(i1 & f2);
                jf1=(j1 & f2);
                if0=f1-if1;
                jf0=f1-jf1;

                bw_pred=(if0*jf0*mcef[bw_refframe][uv][jj0][ii0]+
                         if1*jf0*mcef[bw_refframe][uv][jj0][ii1]+
                         if0*jf1*mcef[bw_refframe][uv][jj1][ii0]+
                         if1*jf1*mcef[bw_refframe][uv][jj1][ii1]+f4)/f3;

              }

              if (img->direct_type && direct_pdir==1)
              {
                img->mpr[ii+ioff][jj+joff]=bw_pred;   //<! Replaced with integer only operations
              }
              else if (img->direct_type && direct_pdir==0)
              {
                img->mpr[ii+ioff][jj+joff]=fw_pred;   //<! Replaced with integer only operations
              }
              else if(img->explicit_B_prediction==1)
              {
                if(currMB->bipred_weighting_type[2*(j-4)+i]==0)
                  img->mpr[ii+ioff][jj+joff]=(fw_pred + bw_pred + 1 )/2;   
                else
                  img->mpr[ii+ioff][jj+joff]=(int)min(255,max(0,2*fw_pred-bw_pred));
              } 
              else
              {
                img->mpr[ii+ioff][jj+joff]=(fw_pred + bw_pred + 1 )/2;   //<! Replaced with integer only operations
              }

            }
          }
        }

        if (!smb)
        {
          itrans(img,ioff,joff,2*uv+i,j);
          for(ii=0;ii<4;ii++)
          for(jj=0;jj<4;jj++)
          {
            imgUV[uv][j4+jj][i4+ii]=img->m7[ii][jj];
          }
        }
      }
    }

    if(smb)
    {
      itrans_sp_chroma(img,2*uv);
      for (j=4;j<6;j++)
      {
        joff=(j-4)*4;
        j4=img->pix_c_y+joff;
        for(i=0;i<2;i++)
        {
          ioff=i*4;
          i4=img->pix_c_x+ioff;
          itrans(img,ioff,joff,2*uv+i,j);

          for(ii=0;ii<4;ii++)
            for(jj=0;jj<4;jj++)
            {
              imgUV[uv][j4+jj][i4+ii]=img->m7[ii][jj];
            }
        }
      }
    }
  }

  if (img->mb_frame_field_flag)
    refFrArr = refFrArr_frm;

#undef fwd_ref_idx_to_refframe
#undef bwd_ref_idx_to_refframe

  return 0;
}

/*!
 ************************************************************************
 * \brief
 *    copy current MB from last MB
 ************************************************************************
 */
void decode_one_Copy_topMB(struct img_par *img,struct inp_par *inp)
{
  int tmp_block[BLOCK_SIZE][BLOCK_SIZE];
  int i, j, ii, jj, uv;
  int ref_frame = 0; // copy from the most recent field in field buffer;
  int mv_mul;
  int field_y = img->pix_y / 2;
  int field_c_y = img->pix_c_y / 2;

  img->height /= 2;
  img->height_cr /= 2;
  mref = mref_fld;
  mcef = mcef_fld;

  if(img->mv_res)
    mv_mul=8;
  else
    mv_mul=4;

  // get luma pixel *************************************************
  for(j=0;j<MB_BLOCK_SIZE;j+=BLOCK_SIZE)
  {
    for(i=0;i<MB_BLOCK_SIZE;i+=BLOCK_SIZE)
    {
      get_block(ref_frame,(img->pix_x+i)*mv_mul,(field_y+j)*mv_mul,img,tmp_block);

      for(ii=0;ii<BLOCK_SIZE;ii++)
        for(jj=0;jj<BLOCK_SIZE;jj++)
          imgY_top[field_y+j+jj][img->pix_x+i+ii]=tmp_block[ii][jj];
    }
  }
#if 0       // not support SP picture with super MB structure
  if (img->type==SP_IMG_1 || img->type==SP_IMG_MULT)
  {
    for(j=0;j<MB_BLOCK_SIZE;j++)
    {
      jj=field_y+j;
      for(i=0;i<MB_BLOCK_SIZE;i++)
      {
        ii=img->pix_x+i;
        img->mpr[i][j]=imgY[jj][ii];
      }
    }
    for (i=0; i < MB_BLOCK_SIZE; i+=BLOCK_SIZE)
      for (j=0; j < MB_BLOCK_SIZE; j+=BLOCK_SIZE)
        copyblock_sp(img,i,j);
  }
#endif
  // get chroma pixel **********************************************
  for(uv=0;uv<2;uv++)
  {
    for(j=0;j<MB_BLOCK_SIZE/2;j++)
    {
      jj=field_c_y+j;
      for(i=0;i<MB_BLOCK_SIZE/2;i++)
      {
        ii=img->pix_c_x+i;
        imgUV_top[uv][jj][ii]=mcef[ref_frame][uv][jj][ii];
      }
    }
#if 0       // not support SP picture with super MB structure
    if(img->type==SP_IMG_1 || img->type==SP_IMG_MULT)
    {
      for(j=0;j<MB_BLOCK_SIZE/2;j++)
      {
        jj=field_c_y+j;
        for(i=0;i<MB_BLOCK_SIZE/2;i++)
        {
          ii=img->pix_c_x+i;
          img->mpr[i][j]=imgUV_top[uv][jj][ii];
        }
      }
      for (j=4;j<6;j++)
        for(i=0;i<4;i++)
          for(ii=0;ii<4;ii++)
            for(jj=0;jj<4;jj++)
              img->cof[i][j][ii][jj]=0;

      itrans_sp_chroma(img,2*uv);

      for (j=4;j<6;j++)
      {
        for(i=0;i<2;i++)
        {
          itrans(img,i*4,(j-4)*4,2*uv+i,j);

          for(ii=0;ii<4;ii++)
            for(jj=0;jj<4;jj++)
            {
              imgUV[uv][field_c_y+(j-4)*4+jj][img->pix_c_x+i*4+ii]=img->m7[ii][jj];
            }
        }
      }
    }
#endif
  }

  mref = mref_frm;
  mcef = mcef_frm;
  img->height *= 2;
  img->height_cr *= 2;
}

/*!
 ************************************************************************
 * \brief
 *    initialize one super macroblock
 ************************************************************************
 */

void init_super_macroblock(struct img_par *img,struct inp_par *inp)
{



  if (img->mb_field == 0)
  {
    mref = mref_frm;
    mcef = mcef_frm;

    imgY = imgY_frm;
    imgUV = imgUV_frm;

    img->mv = img->mv_frm;
    refFrArr = refFrArr_frm;
    img->fw_refFrArr = img->fw_refFrArr_frm;
    img->bw_refFrArr = img->bw_refFrArr_frm;
  }
  else
  {
    mref = mref_fld;
    mcef = mcef_fld;

    if (img->current_mb_nr%2)   // bottom field
    {  
      imgY = imgY_bot;
      imgUV = imgUV_bot;
      img->mv = img->mv_bot;
      refFrArr = refFrArr_bot;
      img->fw_refFrArr = img->fw_refFrArr_bot;
      img->bw_refFrArr = img->bw_refFrArr_bot;
    }
    else
    {
      imgY = imgY_top;
      imgUV = imgUV_top;
      img->mv = img->mv_top;
      refFrArr = refFrArr_top;
      img->fw_refFrArr = img->fw_refFrArr_top;
      img->bw_refFrArr = img->bw_refFrArr_top;
    }
  }
}

/*!
 ************************************************************************
 * \brief
 *    exit one super macroblock
 ************************************************************************
 */

void exit_super_macroblock(struct img_par *img,struct inp_par *inp)
{
  int i,j;
  int offset_y = img->mb_y*MB_BLOCK_SIZE;
  int offset_x = img->mb_x*MB_BLOCK_SIZE;
  int field_y = offset_y/2;
  int chroma_y = offset_y/2;
  int chroma_x = offset_x/2;
  int field_c_y = field_y/2;
  Macroblock *currMB = &img->mb_data[img->current_mb_nr];
      
  if (img->mb_field==0)
    for (i=0;i<4;i++)
      for (j=0;j<4;j++)
      {
        img->field_anchor[img->block_y+j][img->block_x+i] = 0;
      }
  else
    for (i=0;i<4;i++)
      for (j=0;j<4;j++)
      {
        img->field_anchor[img->block_y+j][img->block_x+i] = 1;
      }


  if (img->mb_field==0)
  {
    // fill field buffer
    for (i=0;i<MB_BLOCK_SIZE/2;i++)
      for (j=0;j<MB_BLOCK_SIZE;j++)
      {
        imgY_top[field_y+i][offset_x+j] = imgY[offset_y+i*2][offset_x+j];
        imgY_bot[field_y+i][offset_x+j] = imgY[offset_y+i*2+1][offset_x+j];
      }
    for (i=0;i<BLOCK_SIZE;i++)
      for (j=0;j<BLOCK_SIZE*2;j++)
      {
        imgUV_top[0][field_c_y+i][chroma_x+j] = imgUV[0][chroma_y+i*2][chroma_x+j];
        imgUV_top[1][field_c_y+i][chroma_x+j] = imgUV[1][chroma_y+i*2][chroma_x+j];
        imgUV_bot[0][field_c_y+i][chroma_x+j] = imgUV[0][chroma_y+i*2+1][chroma_x+j];
        imgUV_bot[1][field_c_y+i][chroma_x+j] = imgUV[1][chroma_y+i*2+1][chroma_x+j];
      }
  }
  else
  {
    if (img->current_mb_nr%2)
    {
      offset_y -= MB_BLOCK_SIZE - 1;
      field_y -= MB_BLOCK_SIZE/2;
      for (i=0;i<MB_BLOCK_SIZE;i++)
        for (j=0;j<MB_BLOCK_SIZE;j++)
            imgY_frm[offset_y+i*2][offset_x+j] = imgY_bot[field_y+i][offset_x+j];
      chroma_y -= BLOCK_SIZE*2 - 1;
      field_c_y -= BLOCK_SIZE;
      for (i=0;i<BLOCK_SIZE*2;i++)
        for (j=0;j<BLOCK_SIZE*2;j++)
        {
          imgUV_frm[0][chroma_y+i*2][chroma_x+j] = imgUV_bot[0][field_c_y+i][chroma_x+j];
          imgUV_frm[1][chroma_y+i*2][chroma_x+j] = imgUV_bot[1][field_c_y+i][chroma_x+j];
        }
    }
    else
    {
      for (i=0;i<MB_BLOCK_SIZE;i++)
        for (j=0;j<MB_BLOCK_SIZE;j++)
            imgY_frm[offset_y+i*2][offset_x+j] = imgY_top[field_y+i][offset_x+j];
      for (i=0;i<BLOCK_SIZE*2;i++)
        for (j=0;j<BLOCK_SIZE*2;j++)
        {
          imgUV_frm[0][chroma_y+i*2][chroma_x+j] = imgUV_top[0][field_c_y+i][chroma_x+j];
          imgUV_frm[1][chroma_y+i*2][chroma_x+j] = imgUV_top[1][field_c_y+i][chroma_x+j];
        }
    }
  }

  mref = mref_frm;
  mcef = mcef_frm;
 
  imgY = imgY_frm;
  imgUV = imgUV_frm;

  img->mv = img->mv_frm;
  refFrArr = refFrArr_frm;
}

/*!
 ************************************************************************
 * \brief
 *    decode one super macroblock
 ************************************************************************
 */

int decode_super_macroblock(struct img_par *img,struct inp_par *inp)
{
  int tmp_block[BLOCK_SIZE][BLOCK_SIZE];
  int tmp_blockbw[BLOCK_SIZE][BLOCK_SIZE];
  int js[2][2];
  int i=0,j=0,k,ii=0,jj=0,i1=0,j1=0,j4=0,i4=0;
  int js0=0,js1=0,js2=0,js3=0,jf=0;
  int uv, hv;
  int vec1_x=0,vec1_y=0,vec2_x=0,vec2_y=0;
  int ioff,joff;

  int bw_pred, fw_pred, ifx;
  int ii0,jj0,ii1,jj1,if1,jf1,if0,jf0;
  int mv_mul,f1,f2,f3,f4;

  const byte decode_block_scan[16] = {0,1,4,5,2,3,6,7,8,9,12,13,10,11,14,15};

  Macroblock *currMB   = &img->mb_data[img->current_mb_nr];
  int refframe, fw_refframe, bw_refframe, mv_mode, pred_dir, intra_prediction; // = currMB->ref_frame;
  int*** mv_array, ***fw_mv_array, ***bw_mv_array;
  int bframe = (img->type==B_IMG_1 || img->type==B_IMG_MULT);
//  byte refP_tr;

  int frame_no_next_P, frame_no_B, delta_P;

  int img_pix_c_y, img_height_cr, block_type, j5;

  int iTRb, iTRp;
  int mv_scale;

  int mb_nr             = img->current_mb_nr;
  int mb_width          = img->width/16;
  
  int mb_available_up, mb_available_left;
  int fwd_refframe_offset,bwd_refframe_offset;
  int direct_pdir;

  int **fwRefFrArr = img->fw_refFrArr;
  int **bwRefFrArr = img->bw_refFrArr;
  int  ***fw_mv = img->fw_mv;
  int  ***bw_mv = img->bw_mv;
  int  **moving_block_dir = moving_block; 


  if (img->mb_frame_field_flag)
  {
    mb_available_up   = (img->mb_y == 0) ? 0 : 1;
    mb_available_left = (img->mb_x == 0) ? 0 : 1;
  }
  else
  {
    mb_available_up   = (img->mb_y == 0) ? 0 :(img->mb_data[mb_nr].slice_nr == img->mb_data[mb_nr-mb_width].slice_nr);
    mb_available_left = (img->mb_x == 0) ? 0 :(img->mb_data[mb_nr].slice_nr == img->mb_data[mb_nr-1].slice_nr);
  }

  if (img->mb_field)
    if (img->current_mb_nr%2)
      mb_available_up   = ((img->mb_y-1)/2 == 0) ? 0 : 1;
    else
      mb_available_up   = (img->mb_y/2 == 0) ? 0 : 1;

  if(img->UseConstrainedIntraPred)
  {
    if (mb_available_up   && (img->intra_block[mb_nr-mb_width][2]==0 || img->intra_block[mb_nr-mb_width][3]==0))
      mb_available_up   = 0;
    if (mb_available_left && (img->intra_block[mb_nr-       1][1]==0 || img->intra_block[mb_nr       -1][3]==0))
      mb_available_left = 0;
  }
 
  if (img->structure == TOP_FIELD)
      block_type = TOP_FIELD;
  else if (img->structure == BOTTOM_FIELD)
      block_type = BOTTOM_FIELD;
  else
      block_type = FRAME;

  if(bframe)
  {
    int current_tr  = (img->structure==TOP_FIELD || img->structure==BOTTOM_FIELD)?img->tr_fld:2*img->tr_frm;
    if(img->imgtr_next_P <= current_tr)
      fwd_refframe_offset = 0;
    else if (block_type!=FRAME || img->mb_field)
      fwd_refframe_offset = 2;
    else
      fwd_refframe_offset = 1;
  }
  else
  {
    fwd_refframe_offset = 0;
  }

  if (bframe && img->disposable_flag)
  {
    if(block_type==TOP_FIELD || (img->mb_field && img->current_mb_nr%2==0)) // top field  
      bwd_refframe_offset = 1;
    else
      bwd_refframe_offset = 0;
  }
  else
  {
    bwd_refframe_offset = 0;
  }

#define fwd_ref_idx_to_refframe(idx) ((idx)+fwd_refframe_offset)
#define bwd_ref_idx_to_refframe(idx) ((idx)+bwd_refframe_offset)

  // set variables depending on mv_res
  if(img->mv_res)
  {
    mv_mul=8;
    f1=16;
    f2=15;
  }
  else
  {
    mv_mul=4;
    f1=8;
    f2=7;
  }

  f3=f1*f1;
  f4=f3/2;

  // luma decoding **************************************************

  // get prediction for INTRA_MB_16x16
  if (IS_NEWINTRA (currMB))
  {
    intrapred_luma_2(img, currMB->i16mode);
  }

  for (k = 0; k < (MB_BLOCK_SIZE/BLOCK_SIZE)*(MB_BLOCK_SIZE/BLOCK_SIZE); k ++)
  {
    i = (decode_block_scan[k] & 3);
    j = ((decode_block_scan[k] >> 2) & 3);

    ioff=i*4;
    i4=img->block_x+i;

    joff=j*4;
    j4=img->block_y+j;

    if (img->mb_field) 
    {
      j5 = j4;
      j4 = img->block_y / 2 + j;
      if (img->current_mb_nr%2)
        j4 -= BLOCK_SIZE / 2;
    }

    mv_mode  = currMB->b8mode[2*(j/2)+(i/2)];
    pred_dir = currMB->b8pdir[2*(j/2)+(i/2)];
          
    // PREDICTION
    if (mv_mode==IBLOCK)
    {
      //===== INTRA PREDICTION =====
      if (intrapred(img,ioff,joff,i4,j4)==SEARCH_SYNC)  /* make 4x4 prediction block mpr from given prediction img->mb_mode */
        return SEARCH_SYNC;                   /* bit error */
    }
    else if (!IS_NEWINTRA (currMB))
    {
      if (pred_dir != 2)
      {
        //===== FORWARD/BACKWARD PREDICTION =====
        if (!bframe)
        {
          //refframe = (img->frame_cycle + img->buf_cycle - refFrArr[j4][i4]) % img->buf_cycle;
          refframe = refFrArr[j4][i4];
          mv_array = img->mv;
        }
        else if (!pred_dir)
        {
          refframe = fwd_ref_idx_to_refframe(img->fw_refFrArr[j4][i4]);

          if (img->current_mb_nr%2 == 0)
            mv_array = img->fw_mv_top;
          else
            mv_array = img->fw_mv_bot;
        }
        else
        {
          //refframe = (img->frame_cycle + img->buf_cycle) % img->buf_cycle;
          refframe = bwd_ref_idx_to_refframe(img->bw_refFrArr[j4][i4]);

          if (img->current_mb_nr%2 == 0)
            mv_array = img->bw_mv_top;
          else
            mv_array = img->bw_mv_bot;
        }

        vec1_x = i4*4*mv_mul + mv_array[i4+BLOCK_SIZE][j4][0];
        vec1_y = j4*4*mv_mul + mv_array[i4+BLOCK_SIZE][j4][1];

        get_block (refframe, vec1_x, vec1_y, img, tmp_block);

        for(ii=0;ii<BLOCK_SIZE;ii++)
        for(jj=0;jj<BLOCK_SIZE;jj++)  img->mpr[ii+ioff][jj+joff] = tmp_block[ii][jj];
      }
      else
      {
   
        if (mv_mode != 0)
        {
          //===== BI-DIRECTIONAL PREDICTION =====
          if (img->current_mb_nr%2 == 0)
          {
            fw_mv_array = img->fw_mv_top;
            bw_mv_array = img->bw_mv_top;
          }
          else
          {
            fw_mv_array = img->fw_mv_bot;
            bw_mv_array = img->bw_mv_bot;
          }

          fw_refframe = fwd_ref_idx_to_refframe(img->fw_refFrArr[j4][i4]);
          bw_refframe = bwd_ref_idx_to_refframe(img->bw_refFrArr[j4][i4]);
        }   
        else
        {          
//--------------------------------------------------------------------------------------
          //===== DIRECT PREDICTION =====
          if (img->current_mb_nr%2 == 0)
          {
            fw_mv_array = img->dfMV_top;
            bw_mv_array = img->dbMV_top;
          }
          else
          {
            fw_mv_array = img->dfMV_bot;
            bw_mv_array = img->dbMV_bot;
          }
          bw_refframe = 0;

          if (img->current_mb_nr%2 == 0)
            bw_refframe = 1;
          
          if (img->direct_type && img->mb_frame_field_flag )
          {
            if (!img->mb_field)
            {
              fwRefFrArr= img->fw_refFrArr_frm;
              bwRefFrArr= img->bw_refFrArr_frm;
              fw_mv=img->fw_mv_frm;
              bw_mv=img->bw_mv_frm;
            }
            else if (img->current_mb_nr%2 )
            {
              fwRefFrArr= img->fw_refFrArr_bot;
              bwRefFrArr= img->bw_refFrArr_bot;
              fw_mv=img->fw_mv_bot;
              bw_mv=img->bw_mv_bot;
              moving_block_dir = moving_block_bot; 
            }
            else
            {
              fwRefFrArr=img->fw_refFrArr_top ;
              bwRefFrArr=img->bw_refFrArr_top ;
              fw_mv=img->fw_mv_top;
              bw_mv=img->bw_mv_top;
              moving_block_dir = moving_block_top; 
            }
          }          

          if (img->direct_type )
          {
            int pic_blockx              = img->block_x;
            int pic_blocky              = (img->current_mb_nr%2)?img->block_y/2-BLOCK_SIZE / 2:img->block_y/2;
            int mb_nr                   = img->current_mb_nr;
            int mb_width                = img->width/16;
            // Note pic_blocky == 0 && condition on img->mb_frame_field_flag is a temp fix. Should be fixed when proper support of slices within MB-AFF is added.
            int mb_available_up         = (img->mb_y == 0 || pic_blocky == 0  ) ? 0 : (img->mb_frame_field_flag? 1 :(currMB->slice_nr == img->mb_data[mb_nr-mb_width].slice_nr));
            int mb_available_left       = (img->mb_x == 0                      ) ? 0 : (currMB->slice_nr == img->mb_data[mb_nr-1].slice_nr);
            int mb_available_upleft     = (img->mb_x == 0 || img->mb_y == 0 || pic_blocky == 0) ? 0 : (img->mb_frame_field_flag)? 1 :(currMB->slice_nr == img->mb_data[mb_nr-mb_width-1].slice_nr);
            int mb_available_upright    = (img->mb_frame_field_flag && img->current_mb_nr%2)?0:(img->mb_x >= mb_width-1 ||
                                          img->mb_y == 0 || pic_blocky == 0 ) ? 0 : (img->mb_frame_field_flag)? 1 :(currMB->slice_nr == img->mb_data[mb_nr-mb_width+1].slice_nr);
            
            int fw_rFrameL              = mb_available_left    ? fwRefFrArr[pic_blocky]  [pic_blockx-1]   : -1;
            int fw_rFrameU              = mb_available_up      ? fwRefFrArr[pic_blocky-1][pic_blockx]     : -1;
            int fw_rFrameUL             = mb_available_upleft  ? fwRefFrArr[pic_blocky-1][pic_blockx-1]   : -1;
            int fw_rFrameUR             = mb_available_upright ? fwRefFrArr[pic_blocky-1][pic_blockx+4]   : fw_rFrameUL;  
            
            int bw_rFrameL              = mb_available_left    ? bwRefFrArr[pic_blocky]  [pic_blockx-1]   : -1;
            int bw_rFrameU              = mb_available_up      ? bwRefFrArr[pic_blocky-1][pic_blockx]     : -1;
            int bw_rFrameUL             = mb_available_upleft  ? bwRefFrArr[pic_blocky-1][pic_blockx-1]   : -1;
            int bw_rFrameUR             = mb_available_upright ? bwRefFrArr[pic_blocky-1][pic_blockx+4]   : bw_rFrameUL;
            
            int fw_rFrame,bw_rFrame;
            int pmvfw[2]={0,0},pmvbw[2]={0,0};

            if (!fw_rFrameL || !fw_rFrameU || !fw_rFrameUR)
              fw_rFrame=0;
            else
              fw_rFrame=min(fw_rFrameL&15,min(fw_rFrameU&15,fw_rFrameUR&15));

            if(img->num_ref_pic_active_bwd>1 && (bw_rFrameL==1 || bw_rFrameU==1 || bw_rFrameUR==1))
              bw_rFrame=1;
            else if (!bw_rFrameL || !bw_rFrameU || !bw_rFrameUR)
              bw_rFrame=0;
            else
              bw_rFrame=min(bw_rFrameL&15,min(bw_rFrameU&15,bw_rFrameUR&15));

            if (fw_rFrame !=15)
              SetMotionVectorPredictor (img, pmvfw, pmvfw+1, fw_rFrame, fwRefFrArr, fw_mv, 0, 0, 16, 16);  
            if (bw_rFrame !=15)
              SetMotionVectorPredictor (img, pmvbw, pmvbw+1, bw_rFrame, bwRefFrArr, bw_mv, 0, 0, 16, 16);  
            
            if (fw_rFrame !=15)
            {
              if  (!fw_rFrame  && !moving_block_dir[j4][i4])       
              {                    
                fwRefFrArr[j4][i4] = 0;
                fw_mv_array[i4+BLOCK_SIZE][j4][0]=fw_mv[i4+BLOCK_SIZE][j4][0]=0;
                fw_mv_array[i4+BLOCK_SIZE][j4][1]=fw_mv[i4+BLOCK_SIZE][j4][1]=0;
                
              }
              else
              {
                fwRefFrArr[j4][i4] = fw_rFrame ;                    
                fw_mv_array[i4+BLOCK_SIZE][j4][0]=fw_mv[i4+BLOCK_SIZE][j4][0]=pmvfw[0];
                fw_mv_array[i4+BLOCK_SIZE][j4][1]=fw_mv[i4+BLOCK_SIZE][j4][1]=pmvfw[1];                    
              }
            }
            else
            {
              fwRefFrArr[j4][i4]=-1;                                                       
              fw_mv_array[i4+BLOCK_SIZE][j4][0]=fw_mv[i4+BLOCK_SIZE][j4][0]=0;
              fw_mv_array[i4+BLOCK_SIZE][j4][1]=fw_mv[i4+BLOCK_SIZE][j4][1]=0;
            }
            
            if (bw_rFrame !=15)
            {
              if  (bw_rFrame==((img->num_ref_pic_active_bwd>1)?1:0) && !moving_block_dir[j4][i4])
              {                                                  
                bwRefFrArr[j4][i4]=bw_rFrame;;
                bw_mv_array[i4+BLOCK_SIZE][j4][0]=bw_mv[i4+BLOCK_SIZE][j4][0]=0;
                bw_mv_array[i4+BLOCK_SIZE][j4][1]=bw_mv[i4+BLOCK_SIZE][j4][1]=0;   
              }
              else
              {
                bwRefFrArr[j4][i4]=bw_rFrame;
                bw_mv_array[i4+BLOCK_SIZE][j4][0]=bw_mv[i4+BLOCK_SIZE][j4][0]=pmvbw[0];
                bw_mv_array[i4+BLOCK_SIZE][j4][1]=bw_mv[i4+BLOCK_SIZE][j4][1]=pmvbw[1];   
              }               
            }
            else
            {
              bwRefFrArr[j4][i4]=-1;                  
              bw_mv_array[i4+BLOCK_SIZE][j4][0]=bw_mv[i4+BLOCK_SIZE][j4][0]=0;
              bw_mv_array[i4+BLOCK_SIZE][j4][1]=bw_mv[i4+BLOCK_SIZE][j4][1]=0;                 
            }
            if (fw_rFrame ==15 && bw_rFrame ==15)
            {
              fwRefFrArr[j4][i4]= 0;
              bwRefFrArr[j4][i4] = (img->num_ref_pic_active_bwd>1)?1:0;
            }

            if (fwRefFrArr[j4][i4]!=-1)
              img->fw_refFrArr_frm[j5][i4]=fwRefFrArr[j4][i4]/2;
            else 
              img->fw_refFrArr_frm[j5][i4]=-1;

              img->bw_refFrArr_frm[j5][i4]=bwRefFrArr[j4][i4];

            img->dfMV[i4+BLOCK_SIZE][j5][0]=img->fw_mv_frm[i4+BLOCK_SIZE][j5][0]=fw_mv[i4+BLOCK_SIZE][j4][0];
            img->dfMV[i4+BLOCK_SIZE][j5][1]=img->fw_mv_frm[i4+BLOCK_SIZE][j5][1]=fw_mv[i4+BLOCK_SIZE][j4][1]*2;
            img->dbMV[i4+BLOCK_SIZE][j5][0]=img->bw_mv_frm[i4+BLOCK_SIZE][j5][0]=bw_mv[i4+BLOCK_SIZE][j4][0];
            img->dbMV[i4+BLOCK_SIZE][j5][1]=img->bw_mv_frm[i4+BLOCK_SIZE][j5][1]=bw_mv[i4+BLOCK_SIZE][j4][1]*2;
            

            fw_refframe = (fwRefFrArr[j4][i4]!=-1) ? fwd_ref_idx_to_refframe(fwRefFrArr[j4][i4]):0;
            bw_refframe = (bwRefFrArr[j4][i4]!=-1) ? bwd_ref_idx_to_refframe(bwRefFrArr[j4][i4]):0;

            if      (bwRefFrArr[j4][i4]==-1) direct_pdir = 0;
            else if (fwRefFrArr[j4][i4]==-1) direct_pdir = 1;
            else                             direct_pdir = 2;
           }
           else // Temporal Mode
           {
             if(refFrArr[j4][i4]==-1) // next P is intra mode
             {
               if (img->current_mb_nr%2 == 0)
               {
                 for(hv=0; hv<2; hv++)
                 {
                   img->dfMV_top[i4+BLOCK_SIZE][j4][hv]=img->dbMV_top[i4+BLOCK_SIZE][j4][hv]=0;
                   img->fw_mv_top[i4+BLOCK_SIZE][j4][hv]=img->bw_mv_top[i4+BLOCK_SIZE][j4][hv]=0;
                   
                   img->dfMV[i4+BLOCK_SIZE][j5][hv]=img->dbMV[i4+BLOCK_SIZE][j5][hv]=0;
                   img->fw_mv[i4+BLOCK_SIZE][j5][hv]=img->bw_mv[i4+BLOCK_SIZE][j5][hv]=0;
                 }
               }
               else
               {
                 for(hv=0; hv<2; hv++)
                 {
                   img->dfMV_bot[i4+BLOCK_SIZE][j4][hv]=img->dbMV_bot[i4+BLOCK_SIZE][j4][hv]=0;
                   img->fw_mv_bot[i4+BLOCK_SIZE][j4][hv]=img->bw_mv_bot[i4+BLOCK_SIZE][j4][hv]=0;
                   
                   img->dfMV[i4+BLOCK_SIZE][j5][hv]=img->dbMV[i4+BLOCK_SIZE][j5][hv]=0;
                   img->fw_mv[i4+BLOCK_SIZE][j5][hv]=img->bw_mv[i4+BLOCK_SIZE][j5][hv]=0;
                 }
               }
               
               img->fw_refFrArr[j4][i4]=img->fw_refFrArr_frm[j5][i4]=-1;
               img->bw_refFrArr[j4][i4]=img->bw_refFrArr_frm[j5][i4]=-1;
               
               if (block_type == FRAME)
               {
                 fw_refframe = 1;
                 if (img->mb_frame_field_flag && img->mb_field && img->current_mb_nr%2 == 0)
                   fw_refframe = 2;
               }
               else
               {
                 if (block_type == TOP_FIELD)
                   fw_refframe = 2;    // used to be 3;
                 else
                   fw_refframe = 1;    // used to be 2, Krit
               }       
             }
             else // next P is skip or inter mode
             {
               refframe = (img->mb_field) ? (img->mb_y%2 == 0 ? refFrArr_top[j4][i4]:refFrArr_bot[j4][i4]):refFrArr[j4][i4];
               frame_no_next_P =img->imgtr_next_P+((mref==mref_fld)&&(img->structure==BOTTOM_FIELD));
               //          frame_no_B = (mref==mref_fld) ? img->tr : 2*img->tr;
               frame_no_B = (img->structure != FRAME) ? img->tr_fld : 2*img->tr_frm;
               
               delta_P = (img->imgtr_next_P - img->imgtr_last_P);
               if((mref==mref_fld) && (img->structure==TOP_FIELD)) // top field
               {
                 iTRp = delta_P*(refframe/2+1)-(refframe+1)%2;
               }
               else if((mref==mref_fld) && (img->structure==BOTTOM_FIELD)) // bot field
               {
                 iTRp = 1+delta_P*(refframe+1)/2-refframe%2;
               }
               else    // frame
               {
                 iTRp    = (refframe+1)*delta_P;
                 if(img->mb_frame_field_flag && img->mb_field)
                 {
                   if(img->mb_y%2)
                     iTRp = 1+delta_P*(refframe+1)/2-refframe%2;
                   else
                     iTRp = delta_P*(refframe/2+1)-(refframe+1)%2;
                 }
                 
               }
               
               iTRb = iTRp - (frame_no_next_P - frame_no_B);
               mv_scale=(iTRb*256)/iTRp;
               
               if(!img->mb_field)
               {
                 if (img->current_mb_nr%2 == 0)
                 {
                   img->dfMV_top[i4+BLOCK_SIZE][j4][0]=img->dfMV[i4+BLOCK_SIZE][j5][0]=(mv_scale * img->mv[i4+BLOCK_SIZE][j4][0] + 128) >> 8;
                   img->dbMV_top[i4+BLOCK_SIZE][j4][0]=img->dbMV[i4+BLOCK_SIZE][j5][0]=((mv_scale-256) * img->mv[i4+BLOCK_SIZE][j4][0] + 128) >> 8;
                   img->dfMV_top[i4+BLOCK_SIZE][j4][1]=(mv_scale * img->mv[i4+BLOCK_SIZE][j4][1]  + 128) >> 8;
                   img->dfMV[i4+BLOCK_SIZE][j5][1]=(2 * mv_scale  * img->mv[i4+BLOCK_SIZE][j4][1]  + 128) >> 8;
                   img->dbMV_top[i4+BLOCK_SIZE][j4][1]=((mv_scale-256) * img->mv[i4+BLOCK_SIZE][j4][1]  + 128) >> 8;
                   img->dbMV[i4+BLOCK_SIZE][j5][1]=( 2 * (mv_scale-256) * img->mv[i4+BLOCK_SIZE][j4][1]  + 128) >> 8;
                 }
                 else
                 {
                   img->dfMV_bot[i4+BLOCK_SIZE][j4][0]=img->dfMV[i4+BLOCK_SIZE][j5][0]=(mv_scale * img->mv[i4+BLOCK_SIZE][j4][0] + 128) >> 8;
                   img->dbMV_bot[i4+BLOCK_SIZE][j4][0]=img->dbMV[i4+BLOCK_SIZE][j5][0]=((mv_scale-256) *img->mv[i4+BLOCK_SIZE][j4][0] + 128) >> 8;
                   img->dfMV_bot[i4+BLOCK_SIZE][j4][1]=(mv_scale * img->mv[i4+BLOCK_SIZE][j4][1] + 128) >> 8;
                   img->dfMV[i4+BLOCK_SIZE][j5][1]=(2 * mv_scale  * img->mv[i4+BLOCK_SIZE][j4][1] + 128) >> 8;
                   img->dbMV_bot[i4+BLOCK_SIZE][j4][1]=((mv_scale-256) *img->mv[i4+BLOCK_SIZE][j4][1] + 128) >> 8;
                   img->dbMV[i4+BLOCK_SIZE][j5][1]=( 2 * (mv_scale-256) *img->mv[i4+BLOCK_SIZE][j4][1] + 128) >> 8;
                 }
               }
               else
               {
                 if (img->current_mb_nr%2 == 0)
                 {
                   img->dfMV_top[i4+BLOCK_SIZE][j4][0]=img->dfMV[i4+BLOCK_SIZE][j5][0]=(mv_scale * img->mv_top[i4+BLOCK_SIZE][j4][0] + 128) >> 8;
                   img->dbMV_top[i4+BLOCK_SIZE][j4][0]=img->dbMV[i4+BLOCK_SIZE][j5][0]=((mv_scale-256) * img->mv_top[i4+BLOCK_SIZE][j4][0] + 128) >> 8;
                   img->dfMV_top[i4+BLOCK_SIZE][j4][1]=(mv_scale * img->mv_top[i4+BLOCK_SIZE][j4][1]  + 128) >> 8;
                   img->dfMV[i4+BLOCK_SIZE][j5][1]=(2 * mv_scale  * img->mv_top[i4+BLOCK_SIZE][j4][1]  + 128) >> 8;
                   img->dbMV_top[i4+BLOCK_SIZE][j4][1]=((mv_scale-256) * img->mv_top[i4+BLOCK_SIZE][j4][1]  + 128) >> 8;
                   img->dbMV[i4+BLOCK_SIZE][j5][1]=( 2 * (mv_scale-256) * img->mv_top[i4+BLOCK_SIZE][j4][1]  + 128) >> 8;
                 }
                 else
                 {
                   img->dfMV_bot[i4+BLOCK_SIZE][j4][0]=img->dfMV[i4+BLOCK_SIZE][j5][0]=(mv_scale * img->mv_bot[i4+BLOCK_SIZE][j4][0] + 128) >> 8;
                   img->dbMV_bot[i4+BLOCK_SIZE][j4][0]=img->dbMV[i4+BLOCK_SIZE][j5][0]=((mv_scale-256) *img->mv_bot[i4+BLOCK_SIZE][j4][0] + 128) >> 8;
                   img->dfMV_bot[i4+BLOCK_SIZE][j4][1]=(mv_scale * img->mv_bot[i4+BLOCK_SIZE][j4][1] + 128) >> 8;
                   img->dfMV[i4+BLOCK_SIZE][j5][1]=(2 * mv_scale  * img->mv_bot[i4+BLOCK_SIZE][j4][1] + 128) >> 8;
                   img->dbMV_bot[i4+BLOCK_SIZE][j4][1]=((mv_scale-256) *img->mv_bot[i4+BLOCK_SIZE][j4][1] + 128) >> 8;
                   img->dbMV[i4+BLOCK_SIZE][j5][1]=( 2 * (mv_scale-256) *img->mv_bot[i4+BLOCK_SIZE][j4][1] + 128) >> 8;
                 }
               }
               
               fw_refframe = max(0,refFrArr[j4][i4]) + 1;
               if (img->mb_frame_field_flag && img->mb_field && img->current_mb_nr%2 == 0)
                 fw_refframe = refFrArr[j4][i4]+2;
               
               if (img->current_mb_nr%2 == 0)
               {
                 img->fw_mv_top[i4+BLOCK_SIZE][j4][0]=img->dfMV_top[i4+BLOCK_SIZE][j4][0];
                 img->fw_mv_top[i4+BLOCK_SIZE][j4][1]=img->dfMV_top[i4+BLOCK_SIZE][j4][1];
                 img->bw_mv_top[i4+BLOCK_SIZE][j4][0]=img->dbMV_top[i4+BLOCK_SIZE][j4][0];
                 img->bw_mv_top[i4+BLOCK_SIZE][j4][1]=img->dbMV_top[i4+BLOCK_SIZE][j4][1];
                 
                 img->fw_mv_frm[i4+BLOCK_SIZE][j5][0]=img->dfMV[i4+BLOCK_SIZE][j5][0]=img->fw_mv_top[i4+BLOCK_SIZE][j4][0];
                 img->fw_mv_frm[i4+BLOCK_SIZE][j5][1]=img->dfMV[i4+BLOCK_SIZE][j5][1]=img->fw_mv_top[i4+BLOCK_SIZE][j4][1]*2;
                 img->bw_mv_frm[i4+BLOCK_SIZE][j5][0]=img->dbMV[i4+BLOCK_SIZE][j5][0]=img->bw_mv_top[i4+BLOCK_SIZE][j4][0];
                 img->bw_mv_frm[i4+BLOCK_SIZE][j5][1]=img->dbMV[i4+BLOCK_SIZE][j5][1]=img->bw_mv_top[i4+BLOCK_SIZE][j4][1]*2;
               }
               else
               {
                 img->fw_mv_bot[i4+BLOCK_SIZE][j4][0]=img->dfMV_bot[i4+BLOCK_SIZE][j4][0];
                 img->fw_mv_bot[i4+BLOCK_SIZE][j4][1]=img->dfMV_bot[i4+BLOCK_SIZE][j4][1];
                 img->bw_mv_bot[i4+BLOCK_SIZE][j4][0]=img->dbMV_bot[i4+BLOCK_SIZE][j4][0];
                 img->bw_mv_bot[i4+BLOCK_SIZE][j4][1]=img->dbMV_bot[i4+BLOCK_SIZE][j4][1];     
                 
                 img->fw_mv_frm[i4+BLOCK_SIZE][j5][0]=img->dfMV[i4+BLOCK_SIZE][j5][0]=img->fw_mv_bot[i4+BLOCK_SIZE][j4][0];
                 img->fw_mv_frm[i4+BLOCK_SIZE][j5][1]=img->dfMV[i4+BLOCK_SIZE][j5][1]=img->fw_mv_bot[i4+BLOCK_SIZE][j4][1]*2;
                 img->bw_mv_frm[i4+BLOCK_SIZE][j5][0]=img->dbMV[i4+BLOCK_SIZE][j5][0]=img->bw_mv_bot[i4+BLOCK_SIZE][j4][0];
                 img->bw_mv_frm[i4+BLOCK_SIZE][j5][1]=img->dbMV[i4+BLOCK_SIZE][j5][1]=img->bw_mv_bot[i4+BLOCK_SIZE][j4][1]*2;
               }
               
               img->fw_refFrArr[j4][i4]=refFrArr[j4][i4];
               img->fw_refFrArr_frm[j5][i4]=refFrArr[j4][i4]/2;
               img->bw_refFrArr[j4][i4]=img->bw_refFrArr_frm[j5][i4]=0;
            }
          }
        }
        if (mv_mode==0  && img->direct_type)
        {
          if (fwRefFrArr[j4][i4] >= 0)
          {
            vec1_x = i4*4*mv_mul + fw_mv_array[i4+BLOCK_SIZE][j4][0];
            vec1_y = j4*4*mv_mul + fw_mv_array[i4+BLOCK_SIZE][j4][1];
            get_block(fw_refframe, vec1_x, vec1_y, img, tmp_block);
          }
          
          if (bwRefFrArr[j4][i4] >= 0)
          {
            vec2_x = i4*4*mv_mul + bw_mv_array[i4+BLOCK_SIZE][j4][0];
            vec2_y = j4*4*mv_mul + bw_mv_array[i4+BLOCK_SIZE][j4][1];
            get_block(bw_refframe, vec2_x, vec2_y, img, tmp_blockbw);
          }
          
        }
        else
        {
          
          vec1_x = i4*4*mv_mul + fw_mv_array[i4+BLOCK_SIZE][j4][0];
          vec1_y = j4*4*mv_mul + fw_mv_array[i4+BLOCK_SIZE][j4][1];
          vec2_x = i4*4*mv_mul + bw_mv_array[i4+BLOCK_SIZE][j4][0];
          vec2_y = j4*4*mv_mul + bw_mv_array[i4+BLOCK_SIZE][j4][1];
          
          get_block(fw_refframe, vec1_x, vec1_y, img, tmp_block);
          get_block(bw_refframe, vec2_x, vec2_y, img, tmp_blockbw);

        }

        if (mv_mode==0 && img->direct_type && direct_pdir==0)
        {
          for(ii=0;ii<BLOCK_SIZE;ii++)
            for(jj=0;jj<BLOCK_SIZE;jj++)  
              img->mpr[ii+ioff][jj+joff] = tmp_block[ii][jj];
        }
        else if (mv_mode==0 && img->direct_type && direct_pdir==1)
        {              
          for(ii=0;ii<BLOCK_SIZE;ii++)
            for(jj=0;jj<BLOCK_SIZE;jj++)  
              img->mpr[ii+ioff][jj+joff] = tmp_blockbw[ii][jj];
        }
        else if(img->explicit_B_prediction==1)
        {
          if(currMB->bipred_weighting_type[2*(j/2)+(i/2)]==0)
            for(ii=0;ii<BLOCK_SIZE;ii++)
              for(jj=0;jj<BLOCK_SIZE;jj++)  
                 img->mpr[ii+ioff][jj+joff] =  (tmp_block[ii][jj]+tmp_blockbw[ii][jj]+1)/2;
          else
            for(ii=0;ii<BLOCK_SIZE;ii++)
              for(jj=0;jj<BLOCK_SIZE;jj++)  
                img->mpr[ii+ioff][jj+joff] =  min(255,max(0,2*tmp_block[ii][jj]-tmp_blockbw[ii][jj]));
        }
        else
        {
          for(ii=0;ii<BLOCK_SIZE;ii++)
            for(jj=0;jj<BLOCK_SIZE;jj++)  
              img->mpr[ii+ioff][jj+joff] = (tmp_block[ii][jj]+tmp_blockbw[ii][jj]+1)/2;
        }
      }
    }
//--------------------------------------------------------------------------------------

    if ((img->type==SP_IMG_1 || img->type==SP_IMG_MULT) && (IS_INTER (currMB) && mv_mode!=IBLOCK))
    {
      itrans_sp(img,ioff,joff,i,j);
    }
    else
    {
      itrans   (img,ioff,joff,i,j);      // use DCT transform and make 4x4 block m7 from prediction block mpr
    }

    if (img->mb_field)
    {
      j4 = img->block_y / 2 + j;
      if (img->current_mb_nr%2)
        j4 -= BLOCK_SIZE / 2;
      for(ii=0;ii<BLOCK_SIZE;ii++)
      {
        for(jj=0;jj<BLOCK_SIZE;jj++)
        {
          imgY[j4*BLOCK_SIZE+jj][i4*BLOCK_SIZE+ii]=img->m7[ii][jj]; // contruct picture from 4x4 blocks
        }
      }
    }
    else
    {
      for(ii=0;ii<BLOCK_SIZE;ii++)
      {
        for(jj=0;jj<BLOCK_SIZE;jj++)
        {
          imgY[j4*BLOCK_SIZE+jj][i4*BLOCK_SIZE+ii]=img->m7[ii][jj]; // contruct picture from 4x4 blocks
        }
      }
    }
  }


  // chroma decoding *******************************************************
  if (img->structure == TOP_FIELD)
      block_type = TOP_FIELD;
  else if (img->structure == BOTTOM_FIELD)
      block_type = BOTTOM_FIELD;
  else
      block_type = FRAME;
  
  img_pix_c_y = img->pix_c_y;   // initialize for MBINTLC1
  img_height_cr = img->height_cr;
  
  if (img->structure==FRAME && img->mb_field)
  {
    img_pix_c_y /= 2;
    img_height_cr /= 2;
  }

  if (img->current_mb_nr%2)
      img_pix_c_y -= BLOCK_SIZE;

  for(uv=0;uv<2;uv++)
  {
    intra_prediction = (IS_NEWINTRA (currMB)        ||
                        currMB->b8mode[0] == IBLOCK ||
                        currMB->b8mode[1] == IBLOCK ||
                        currMB->b8mode[2] == IBLOCK ||
                        currMB->b8mode[3] == IBLOCK);

    if (intra_prediction)
    {
      js0=0;
      js1=0;
      js2=0;
      js3=0;
      for(i=0;i<4;i++)
      {
        if(mb_available_up)
        {
          js0=js0+imgUV[uv][img_pix_c_y-1][img->pix_c_x+i];
          js1=js1+imgUV[uv][img_pix_c_y-1][img->pix_c_x+i+4];
        }
        if(mb_available_left)
        {
          js2=js2+imgUV[uv][img_pix_c_y+i][img->pix_c_x-1];
          js3=js3+imgUV[uv][img_pix_c_y+i+4][img->pix_c_x-1];
        }
      }
      if(mb_available_up && mb_available_left)
      {
        js[0][0]=(js0+js2+4)/8;
        js[1][0]=(js1+2)/4;
        js[0][1]=(js3+2)/4;
        js[1][1]=(js1+js3+4)/8;
      }
      if(mb_available_up && !mb_available_left)
      {
        js[0][0]=(js0+2)/4;
        js[1][0]=(js1+2)/4;
        js[0][1]=(js0+2)/4;
        js[1][1]=(js1+2)/4;
      }
      if(mb_available_left && !mb_available_up)
      {
        js[0][0]=(js2+2)/4;
        js[1][0]=(js2+2)/4;
        js[0][1]=(js3+2)/4;
        js[1][1]=(js3+2)/4;
      }
      if(!mb_available_up && !mb_available_left)
      {
        js[0][0]=128;
        js[1][0]=128;
        js[0][1]=128;
        js[1][1]=128;
      }
    }

    for (j=4;j<6;j++)
    {
      joff=(j-4)*4;
      j4=img_pix_c_y+joff;
      for(i=0;i<2;i++)
      {
        ioff=i*4;
        i4=img->pix_c_x+ioff;

        mv_mode  = currMB->b8mode[2*(j-4)+i];
        pred_dir = currMB->b8pdir[2*(j-4)+i];

        // PREDICTION
        if (mv_mode==IBLOCK || IS_NEWINTRA (currMB))
        {
          //--- INTRA PREDICTION ---
          for (ii=0; ii<4; ii++)
          for (jj=0; jj<4; jj++)
          {
            img->mpr[ii+ioff][jj+joff]=js[i][j-4];
          }
        }
        else if (pred_dir != 2)
        {   
          //--- FORWARD/BACKWARD PREDICTION ---
          if (!bframe)          mv_array = img->mv;
          else if (!pred_dir)   
          {
            if (img->current_mb_nr%2 == 0)
              mv_array = img->fw_mv_top;
            else
              mv_array = img->fw_mv_bot;            
          }
          else
          {
            if (img->current_mb_nr%2 == 0)
              mv_array = img->bw_mv_top;
            else
              mv_array = img->bw_mv_bot;            
          }

          for(jj=0;jj<4;jj++)
          {
            jf=(j4+jj)/2;
            for(ii=0;ii<4;ii++)
            {
              if1=(i4+ii)/2;
              if (!bframe)        refframe = refFrArr[jf][if1];
              else if (!pred_dir) refframe = fwd_ref_idx_to_refframe(img->fw_refFrArr[jf][if1]);
              else                refframe = bwd_ref_idx_to_refframe(img->bw_refFrArr[jf][if1]);

              i1=(img->pix_c_x+ii+ioff)*f1+mv_array[if1+4][jf][0];
              j1=(img_pix_c_y+jj+joff)*f1+mv_array[if1+4][jf][1];

              ii0=max (0, min (i1/f1, img->width_cr-1));
              jj0=max (0, min (j1/f1, img_height_cr-1));
              ii1=max (0, min ((i1+f2)/f1, img->width_cr-1));
              jj1=max (0, min ((j1+f2)/f1, img_height_cr-1));

              if1=(i1 & f2);
              jf1=(j1 & f2);
              if0=f1-if1;
              jf0=f1-jf1;
              img->mpr[ii+ioff][jj+joff]=(if0*jf0*mcef[refframe][uv][jj0][ii0]+
                                          if1*jf0*mcef[refframe][uv][jj0][ii1]+
                                          if0*jf1*mcef[refframe][uv][jj1][ii0]+
                                          if1*jf1*mcef[refframe][uv][jj1][ii1]+f4)/f3;
            }
          }
        }
        else
        {
          if (mv_mode != 0)
          {
            //===== BI-DIRECTIONAL PREDICTION =====
            if (img->current_mb_nr%2 == 0)
            {
              fw_mv_array = img->fw_mv_top;
              bw_mv_array = img->bw_mv_top;
            }
            else
            {
              fw_mv_array = img->fw_mv_bot;
              bw_mv_array = img->bw_mv_bot;
            }
          }
          else
          {
            //===== DIRECT PREDICTION =====
            if (img->current_mb_nr%2 == 0)
            {
              fw_mv_array = img->dfMV_top;
              bw_mv_array = img->dbMV_top;
            }
            else
            {
              fw_mv_array = img->dfMV_bot;
              bw_mv_array = img->dbMV_bot;
            }
          }

          for(jj=0;jj<4;jj++)
          {
            jf=(j4+jj)/2;
            for(ii=0;ii<4;ii++)
            {
              ifx=(i4+ii)/2;

              direct_pdir = 2;
              
              if (mv_mode != 0)
              {
                fw_refframe = fwd_ref_idx_to_refframe(img->fw_refFrArr[jf][ifx]);
                bw_refframe = bwd_ref_idx_to_refframe(img->bw_refFrArr[jf][ifx]);
              }
              else
              {
                if (img->direct_type)                  
                {
                  fw_refframe = fwRefFrArr[2*(jf/2)][(ifx/2)*2];
                  bw_refframe = bwRefFrArr[2*(jf/2)][(ifx/2)*2];
                  fw_refframe = (fw_refframe!=-1) ? fwd_ref_idx_to_refframe(fw_refframe):0;
                  bw_refframe = (bw_refframe!=-1) ? bwd_ref_idx_to_refframe(bw_refframe):0;
                  if      (fwRefFrArr[2*(jf/2)][(ifx/2)*2]==-1)  direct_pdir = 1;
                  else if (bwRefFrArr[2*(jf/2)][(ifx/2)*2]==-1)  direct_pdir = 0;
                  else                                           direct_pdir = 2;
                }
                else
                {
                  bw_refframe = 0;
                  if (block_type==TOP_FIELD)  bw_refframe = 1;
                  if ((img->mb_field) && (img->current_mb_nr%2==0)) bw_refframe = 1;
                  
                  if (block_type == FRAME)
                  {
                    if (img->mb_frame_field_flag==0)
                    {
                      if(refFrArr[jf][ifx]==-1)  fw_refframe = 1;
                      else                       fw_refframe = 1+refFrArr[jf][ifx];
                    }
                    else if (img->current_mb_nr%2 == 0)
                    {
                      if(refFrArr[jf][ifx]==-1)  fw_refframe = 2;  // used to be 3; DIRECT, Krit
                      else                       fw_refframe = 2+refFrArr[jf][ifx];
                    }
                    else
                    {
                      if(refFrArr[jf][ifx]==-1)  fw_refframe = 1;  // used to be 2; DIRECT, Krit
                      else                       fw_refframe = max(0,refFrArr[jf][ifx]) + 1;  // DIRECT, Krit
                    }
                  }
                  else    
                  {
                    if (block_type == TOP_FIELD)
                    {
                      if(refFrArr[jf][ifx]==-1)  fw_refframe = 2;  // used to be 3; DIRECT, Krit
                      else                       fw_refframe = 2+refFrArr[jf][ifx];
                    }
                    else
                    {
                      if(refFrArr[jf][ifx]==-1)  fw_refframe = 1;  // used to be 2; DIRECT, Krit
                      else                       fw_refframe = max(0,refFrArr[jf][ifx]) + 1;  // DIRECT, Krit
                      // used to have 1+refFrArr[jf][ifx]; DIRECT, Krit
                    }
                  }   
                }
              }
              if (mv_mode==0 && img->direct_type)
              {
                if (direct_pdir == 0 || direct_pdir == 2)
                {
                  i1=(img->pix_c_x+ii+ioff)*f1+fw_mv_array[ifx+4][jf][0];
                  j1=(img_pix_c_y+jj+joff)*f1+fw_mv_array[ifx+4][jf][1];
                  
                  ii0=max (0, min (i1/f1, img->width_cr-1));
                  jj0=max (0, min (j1/f1, img_height_cr-1));
                  ii1=max (0, min ((i1+f2)/f1, img->width_cr-1));
                  jj1=max (0, min ((j1+f2)/f1, img_height_cr-1));
                  
                  if1=(i1 & f2);
                  jf1=(j1 & f2);
                  if0=f1-if1;
                  jf0=f1-jf1;
                  
                  fw_pred=(if0*jf0*mcef[fw_refframe][uv][jj0][ii0]+
                    if1*jf0*mcef[fw_refframe][uv][jj0][ii1]+
                    if0*jf1*mcef[fw_refframe][uv][jj1][ii0]+
                    if1*jf1*mcef[fw_refframe][uv][jj1][ii1]+f4)/f3;                
                }
                if (direct_pdir == 1 || direct_pdir == 2)
                {
                  i1=(img->pix_c_x+ii+ioff)*f1+bw_mv_array[ifx+4][jf][0];
                  j1=(img_pix_c_y+jj+joff)*f1+bw_mv_array[ifx+4][jf][1];
                  
                  ii0=max (0, min (i1/f1, img->width_cr-1));
                  jj0=max (0, min (j1/f1, img_height_cr-1));
                  ii1=max (0, min ((i1+f2)/f1, img->width_cr-1));
                  jj1=max (0, min ((j1+f2)/f1, img_height_cr-1));
                  
                  if1=(i1 & f2);
                  jf1=(j1 & f2);
                  if0=f1-if1;
                  jf0=f1-jf1;
                  
                  bw_pred=(if0*jf0*mcef[bw_refframe][uv][jj0][ii0]+
                    if1*jf0*mcef[bw_refframe][uv][jj0][ii1]+
                    if0*jf1*mcef[bw_refframe][uv][jj1][ii0]+
                    if1*jf1*mcef[bw_refframe][uv][jj1][ii1]+f4)/f3;
                }

              }
              else
              {
                i1=(img->pix_c_x+ii+ioff)*f1+fw_mv_array[ifx+4][jf][0];
                j1=(img_pix_c_y+jj+joff)*f1+fw_mv_array[ifx+4][jf][1];
                
                ii0=max (0, min (i1/f1, img->width_cr-1));
                jj0=max (0, min (j1/f1, img_height_cr-1));
                ii1=max (0, min ((i1+f2)/f1, img->width_cr-1));
                jj1=max (0, min ((j1+f2)/f1, img_height_cr-1));
                
                if1=(i1 & f2);
                jf1=(j1 & f2);
                if0=f1-if1;
                jf0=f1-jf1;
                
                fw_pred=(if0*jf0*mcef[fw_refframe][uv][jj0][ii0]+
                  if1*jf0*mcef[fw_refframe][uv][jj0][ii1]+
                  if0*jf1*mcef[fw_refframe][uv][jj1][ii0]+
                  if1*jf1*mcef[fw_refframe][uv][jj1][ii1]+f4)/f3;
                
                i1=(img->pix_c_x+ii+ioff)*f1+bw_mv_array[ifx+4][jf][0];
                j1=(img_pix_c_y+jj+joff)*f1+bw_mv_array[ifx+4][jf][1];
                
                ii0=max (0, min (i1/f1, img->width_cr-1));
                jj0=max (0, min (j1/f1, img_height_cr-1));
                ii1=max (0, min ((i1+f2)/f1, img->width_cr-1));
                jj1=max (0, min ((j1+f2)/f1, img_height_cr-1));
                
                if1=(i1 & f2);
                jf1=(j1 & f2);
                if0=f1-if1;
                jf0=f1-jf1;
                
                bw_pred=(if0*jf0*mcef[bw_refframe][uv][jj0][ii0]+
                  if1*jf0*mcef[bw_refframe][uv][jj0][ii1]+
                  if0*jf1*mcef[bw_refframe][uv][jj1][ii0]+
                  if1*jf1*mcef[bw_refframe][uv][jj1][ii1]+f4)/f3;

              }

              if (img->direct_type && direct_pdir==1) 
              {
                img->mpr[ii+ioff][jj+joff]=bw_pred;   //<! Replaced with integer only operations
              }
              else if (img->direct_type && direct_pdir==0)
              {
                img->mpr[ii+ioff][jj+joff]=fw_pred;   //<! Replaced with integer only operations
              }
              else if(img->explicit_B_prediction==1)
              {
                if(currMB->bipred_weighting_type[2*(j-4)+i]==0)
                  img->mpr[ii+ioff][jj+joff]=(fw_pred + bw_pred + 1 )/2;   
                else
                  img->mpr[ii+ioff][jj+joff]=(int)min(255,max(0,2*fw_pred-bw_pred));
              } 
              else
              {
                img->mpr[ii+ioff][jj+joff]=(fw_pred + bw_pred + 1 )/2;   //<! Replaced with integer only operations
              }
            }
          }
        }

        if ((img->type!=SP_IMG_1 && img->type!=SP_IMG_MULT) || IS_INTRA (currMB))
        {
          itrans(img,ioff,joff,2*uv+i,j);
          for(ii=0;ii<4;ii++)
          for(jj=0;jj<4;jj++)
          {
            imgUV[uv][j4+jj][i4+ii]=img->m7[ii][jj];
          }
        }
      }
    }

    if((img->type==SP_IMG_1 || img->type==SP_IMG_MULT) && IS_INTER (currMB))
    {
      itrans_sp_chroma(img,2*uv);
      for (j=4;j<6;j++)
      {
        joff=(j-4)*4;
        j4=img_pix_c_y+joff;
        for(i=0;i<2;i++)
        {
          ioff=i*4;
          i4=img->pix_c_x+ioff;
          itrans(img,ioff,joff,2*uv+i,j);

          for(ii=0;ii<4;ii++)
            for(jj=0;jj<4;jj++)
            {
              imgUV[uv][j4+jj][i4+ii]=img->m7[ii][jj];
            }
        }
      }
    }
  }

  return 0;
}


void SetOneRefMV(struct img_par* img)
{
  int i0,j0,i,j,k;
  Macroblock *currMB = &img->mb_data[img->current_mb_nr];
  int bframe          = (img->type==B_IMG_1 || img->type==B_IMG_MULT);
  int partmode        = (IS_P8x8(currMB)?4:currMB->mb_type);
  int step_h0         = BLOCK_STEP [partmode][0];
  int step_v0         = BLOCK_STEP [partmode][1];
  int refframe, img_block_y;


  for (j0=0; j0<4; j0+=step_v0)
    for (i0=0; i0<4; i0+=step_h0)
    {
      k=2*(j0/2)+(i0/2);
      if ((currMB->b8pdir[k]==0 || currMB->b8pdir[k]==2) && currMB->b8mode[k]!=0) // forward MVs
      {
        img->subblock_x = i0;
        img->subblock_y = j0;

        if (!IS_P8x8 (currMB) || bframe || (!bframe && !img->allrefzero))
          refframe = 0;
        else
          refframe = 0;
          
        if ((!bframe) && (img->structure==FRAME))
        {
          if (img->current_mb_nr%2==0 && img->mb_frame_field_flag)
          {
            if (img->mb_field==0)
            {
              img_block_y = img->block_y/2;
              for (j=j0; j<j0+step_v0;j++)
                for (i=i0; i<i0+step_h0;i++)
                {
                  refFrArr_frm[img->block_y+j][img->block_x+i] = refframe;
                  refFrArr_top[img_block_y+j][img->block_x+i] = refframe==-1 ? -1 : 2*refframe;
                }
            }
            else
            {
              img_block_y   = img->block_y/2;
              for (j=j0; j<j0+step_v0;j++)
                for (i=i0; i<i0+step_h0;i++)
                {
                  refFrArr_top[img_block_y+j][img->block_x+i] = refframe;
                  refFrArr_frm[img->block_y+j][img->block_x+i] = refframe==-1 ? -1 : refframe/2;
                }
            }
          }
          else if (img->mb_frame_field_flag)
          {
            if (img->mb_field==0)
            {
              img_block_y = (img->block_y-4)/2;
              for (j=j0; j<j0+step_v0;j++)
                for (i=i0; i<i0+step_h0;i++)
                {
                  refFrArr_frm[img->block_y+j][img->block_x+i] = refframe;
                  refFrArr_bot[img_block_y+j][img->block_x+i] = refframe==-1 ? -1 : 2*refframe;
                }
            }
            else
            {
              img_block_y   = (img->block_y-4)/2;
              for (j=j0; j<j0+step_v0;j++)
                for (i=i0; i<i0+step_h0;i++)
                {
                  refFrArr_bot[img_block_y+j][img->block_x+i] = refframe;
                  refFrArr_frm[img->block_y+j][img->block_x+i] = refframe==-1 ? -1 : refframe/2;
                }
            }
          }
          else
          {
            if (!bframe)
            {
              for (j=j0; j<j0+step_v0;j++)
                for (i=i0; i<i0+step_h0;i++)
                  refFrArr[img->block_y+j][img->block_x+i] = refframe;
            }
          }
        }
        else if ((bframe) && (img->structure==FRAME))
        {
          if (img->current_mb_nr%2==0 && img->mb_frame_field_flag)
          {
            if (img->mb_field==0)
            {
              img_block_y = img->block_y/2;
              for (j=j0; j<j0+step_v0;j++)
                for (i=i0; i<i0+step_h0;i++)
                {
                  img->fw_refFrArr_frm[img->block_y+j][img->block_x+i] = refframe;
                  img->fw_refFrArr_top[img_block_y+j][img->block_x+i] = refframe==-1 ? -1 : 2*refframe;
                }
            }
            else
            {
              img_block_y   = img->block_y/2;
              for (j=j0; j<j0+step_v0;j++)
                for (i=i0; i<i0+step_h0;i++)
                {
                  img->fw_refFrArr_top[img_block_y+j][img->block_x+i] = refframe;
                  img->fw_refFrArr_frm[img->block_y+j][img->block_x+i] = refframe==-1 ? -1 : refframe/2;
                }
            }
          }
          else if (img->mb_frame_field_flag)
          {
            if (img->mb_field==0)
            {
              img_block_y = (img->block_y-4)/2;
              for (j=j0; j<j0+step_v0;j++)
                for (i=i0; i<i0+step_h0;i++)
                {
                  img->fw_refFrArr_frm[img->block_y+j][img->block_x+i] = refframe;
                  img->fw_refFrArr_bot[img_block_y+j][img->block_x+i] = refframe==-1 ? -1 : 2*refframe;
                }
            }
            else
            {
              img_block_y   = (img->block_y-4)/2;
              for (j=j0; j<j0+step_v0;j++)
                for (i=i0; i<i0+step_h0;i++)
                {
                  img->fw_refFrArr_bot[img_block_y+j][img->block_x+i] = refframe;
                  img->fw_refFrArr_frm[img->block_y+j][img->block_x+i] = refframe==-1 ? -1 : refframe/2;
                }
            }
          }
          else
          {
            for (j=j0; j<j0+step_v0;j++)
              for (i=i0; i<i0+step_h0;i++)
                img->fw_refFrArr[img->block_y+j][img->block_x+i] = refframe;
          }
        }
        
      }
    }
}