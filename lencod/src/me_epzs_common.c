
/*!
*************************************************************************************
* \file me_epzs_common.c
*
* \brief
*    Common functions for EPZS scheme 
*
* \author
*    Main contributors (see contributors.h for copyright, address and affiliation details)
*      - Alexis Michael Tourapis <alexismt@ieee.org>
*
*************************************************************************************
*/

#include <limits.h>

#include "contributors.h"
#include "global.h"
#include "image.h"
#include "memalloc.h"
#include "mb_access.h"
#include "refbuf.h"
#include "macroblock.h"
#include "me_distortion.h"
#include "me_epzs.h"
#include "me_epzs_common.h"
#include "mv_search.h"

static const short BLOCK_PARENT[8] = { 1, 1, 1, 1, 2, 4, 4, 5 };  //!< {skip, 16x16, 16x8, 8x16, 8x8, 8x4, 4x8, 4x4}
static const int MIN_THRES_BASE[8] = { 0, 64, 32, 32, 16, 8, 8, 4 };
static const int MED_THRES_BASE[8] = { 0, 256, 128, 128, 64, 32, 32, 16 };
static const int MAX_THRES_BASE[8] = { 0, 768, 384, 384, 192, 96, 96, 48 };

// Other definitions
static const char EPZS_PATTERN[6][20] = { "Diamond", "Square", "Extended Diamond", "Large Diamond", "SBP Large Diamond", "PMVFAST" };
static const char EPZS_DUAL_PATTERN[7][20] =
{ "Disabled", "Diamond", "Square", "Extended Diamond", "Large Diamond", "SBP Large Diamond", "PMVFAST" };
static const char EPZS_FIXED_PREDICTORS[3][20] = { "Disabled", "All P", "All P + B" };
static const char EPZS_OTHER_PREDICTORS[2][20] = { "Disabled", "Enabled" };

//! Define EPZS Refinement patterns
//! Define EPZS Refinement patterns
static const short pattern_data[5][12][4] =
{
  { // Small Diamond pattern
    {  0,  4,  3, 3 }, {  4,  0,  0, 3 }, {  0, -4,  1, 3 }, { -4,  0, 2, 3 }
  },
  { // Square pattern
    {  0,  4,  7, 3 }, {  4,  4,  7, 5 }, {  4,  0,  1, 3 }, {  4, -4, 1, 5 },
    {  0, -4,  3, 3 }, { -4, -4,  3, 5 }, { -4,  0,  5, 3 }, { -4,  4, 5, 5 }
  },
  { // Enhanced Diamond pattern
    { -4,  4, 10, 5 }, {  0,  8, 10, 8 }, {  0,  4, 10, 7 }, {  4,  4, 1, 5 },
    {  8,  0, 1,  8 }, {  4,  0,  1, 7 }, {  4, -4,  4, 5 }, {  0, -8, 4, 8 },
    {  0, -4, 4,  7 }, { -4, -4,  7, 5 }, { -8,  0,  7, 8 }, { -4,  0, 7, 7 }

  },
  { // Large Diamond pattern
    {  0,  8, 6,  5 }, {  4,  4, 0,  3 }, {  8,  0, 0,  5 }, {  4, -4, 2, 3 },
    {  0, -8, 2,  5 }, { -4, -4, 4,  3 }, { -8,  0, 4,  5 }, { -4,  4, 6, 3 }
  },
  { // Extended Subpixel pattern
    {  0,  8, 6, 12 }, {  4,  4, 0, 12 }, {  8,  0, 0, 12 }, {  4, -4, 2, 12 },
    {  0, -8, 2, 12 }, { -4, -4, 4, 12 }, { -8,  0, 4, 12 }, { -4,  4, 6, 12 },
    {  0,  2, 6, 12 }, {  2,  0, 0, 12 }, {  0, -2, 2, 12 }, { -2,  0, 4, 12 }
  }
};


/*!
************************************************************************
* \brief
*    calculate RoundLog2(uiVal)
************************************************************************
*/
static int
RoundLog2 (int iValue)
{
  int iRet = 0;
  int iValue_square = iValue * iValue;
  while ((1 << (iRet + 1)) <= iValue_square)
    ++iRet;

  iRet = (iRet + 1) >> 1;
  return iRet;
}

/*!
************************************************************************
* \brief
*    Allocate EPZS pattern memory
*
* \param searchpoints
*    number of searchpoints to allocate
*
* \return
*    the allocated EPZSStructure structure
************************************************************************
*/
static EPZSStructure *
allocEPZSpattern (int searchpoints)
{
  EPZSStructure *s;
  s = calloc (1, sizeof (EPZSStructure));

  if (NULL == s)
    no_mem_exit ("alloc_EPZSpattern: s");

  s->searchPoints = searchpoints;
  s->point = (SPoint *) calloc (searchpoints, sizeof (SPoint));

  return s;
}

/*!
************************************************************************
* \brief
*    Free EPZS pattern memory.
*
* \param p
*    structure to be freed
*
************************************************************************
*/
static void
freeEPZSpattern (EPZSStructure * p)
{
  if (p)
  {
    free ((SPoint *) p->point);
    free (p);
    p = NULL;
  }
}

/*!
************************************************************************
* \brief
*    Assign EPZS pattern 
*
*
************************************************************************
*/
static void
assignEPZSpattern (EPZSStructure * pattern, int type, int stopSearch, int nextLast, EPZSStructure * nextpattern)
{
  int i;

  for (i = 0; i < pattern->searchPoints; ++i)
  {
    pattern->point[i].motion.mv_x = pattern_data[type][i][0];
    pattern->point[i].motion.mv_y = pattern_data[type][i][1];
    pattern->point[i].start_nmbr = pattern_data[type][i][2];
    pattern->point[i].next_points = pattern_data[type][i][3];
  }
  pattern->stopSearch = stopSearch;
  pattern->nextLast = nextLast;
  pattern->nextpattern = nextpattern;
}

/*!
************************************************************************
* \brief
*    EPZS Global Initialization
************************************************************************
*/
int
EPZSInit (ImageParameters * p_Img)
{

  int memory_size = 0;

  //! Definition of pottential EPZS patterns.
  //! It is possible to also define other patterns, or even use
  //! resizing patterns (such as the PMVFAST scheme. These patterns
  //! are only shown here as reference, while the same also holds
  //! for this implementation (i.e. new conditions could be added
  //! on adapting predictors, or thresholds etc. Note that search
  //! could also be performed on subpel positions directly while
  //! pattern needs not be restricted on integer positions only.

  //! Allocate memory and assign search patterns
  p_Img->sdiamond = allocEPZSpattern (4);
  assignEPZSpattern (p_Img->sdiamond, SDIAMOND, TRUE, TRUE, p_Img->sdiamond);
  p_Img->square = allocEPZSpattern (8);
  assignEPZSpattern (p_Img->square, SQUARE, TRUE, TRUE, p_Img->square);
  p_Img->ediamond = allocEPZSpattern (12);
  assignEPZSpattern (p_Img->ediamond, EDIAMOND, TRUE, TRUE, p_Img->ediamond);
  p_Img->ldiamond = allocEPZSpattern (8);
  assignEPZSpattern (p_Img->ldiamond, LDIAMOND, TRUE, TRUE, p_Img->ldiamond);
  p_Img->sbdiamond = allocEPZSpattern (12);
  assignEPZSpattern (p_Img->sbdiamond, SBDIAMOND, FALSE, TRUE, p_Img->sdiamond);
  p_Img->pmvfast = allocEPZSpattern (8);
  assignEPZSpattern (p_Img->pmvfast, LDIAMOND, FALSE, TRUE, p_Img->sdiamond);

  return memory_size;
}

/*!
************************************************************************
* \brief
*    Delete EPZS Alocated memory
************************************************************************
*/
void
EPZSDelete (ImageParameters * p_Img)
{
  // Free search patterns
  freeEPZSpattern (p_Img->pmvfast);
  freeEPZSpattern (p_Img->sbdiamond);
  freeEPZSpattern (p_Img->ldiamond);
  freeEPZSpattern (p_Img->ediamond);
  freeEPZSpattern (p_Img->sdiamond);
  freeEPZSpattern (p_Img->square);
}

/*!
************************************************************************
* \brief
*    Allocate co-located memory
*
* \param size_x
*    horizontal luma size
* \param size_y
*    vertical luma size
* \param mb_adaptive_frame_field_flag
*    flag that indicates macroblock adaptive frame/field coding
*
* \return
*    the allocated EPZSColocParams structure
************************************************************************
*/
static EPZSColocParams *
allocEPZScolocated (int size_x, int size_y, int mb_adaptive_frame_field_flag)
{
  EPZSColocParams *s;
  s = calloc (1, sizeof (EPZSColocParams));
  if (NULL == s)
    no_mem_exit ("alloc_EPZScolocated: s");

  s->size_x = size_x;
  s->size_y = size_y;
  get_mem3Dmv (&(s->frame), 2, size_y / BLOCK_SIZE, size_x / BLOCK_SIZE);

  if (mb_adaptive_frame_field_flag)
  {
    get_mem3Dmv (&(s->top), 2, size_y / (BLOCK_SIZE * 2), size_x / BLOCK_SIZE);
    get_mem3Dmv (&(s->bot), 2, size_y / (BLOCK_SIZE * 2), size_x / BLOCK_SIZE);
  }

  s->mb_adaptive_frame_field_flag = mb_adaptive_frame_field_flag;

  return s;
}

/*!
************************************************************************
* \brief
*    Free co-located memory.
*
* \param p
*    structure to be freed
*
************************************************************************
*/
static void
freeEPZScolocated (EPZSColocParams * p)
{

  if (p)
  {
    free_mem3Dmv (p->frame);
    if (p->mb_adaptive_frame_field_flag)
    {
      free_mem3Dmv (p->top);
      free_mem3Dmv (p->bot);
    }

    free (p);
    p = NULL;
  }
}

/*!
************************************************************************
* \brief
*    EPZS Search Window Predictor Initialization
************************************************************************
*/
static void
EPZSWindowPredictorInit (short search_range, EPZSStructure * predictor, short mode)
{
  short pos;
  short searchpos, fieldsearchpos;
  int prednum = -1;
  short i;
  short search_range_qpel = 2;
  SPoint *point = predictor->point;

  if (mode == 0)
  {
    for (pos = (short) (RoundLog2 (search_range) - 2); pos > -1; pos--)
    {
      searchpos = ((search_range << search_range_qpel) >> pos);

      for (i = 1; i >= -1; i -= 2)
      {
        point[++prednum].motion.mv_x = i * searchpos;
        point[prednum].motion.mv_y = 0;
        point[++prednum].motion.mv_x = i * searchpos;
        point[prednum].motion.mv_y = i * searchpos;
        point[++prednum].motion.mv_x = 0;
        point[prednum].motion.mv_y = i * searchpos;
        point[++prednum].motion.mv_x = -i * searchpos;
        point[prednum].motion.mv_y = i * searchpos;
      }
    }
  }
  else                          // if (mode == 0)
  {
    for (pos = (short) (RoundLog2 (search_range) - 2); pos > -1; pos--)
    {
      searchpos = ((search_range << search_range_qpel) >> pos);

      fieldsearchpos = ((3 * searchpos + 1) << search_range_qpel) >> 1;
      for (i = 1; i >= -1; i -= 2)
      {
        point[++prednum].motion.mv_x = i * searchpos;
        point[prednum].motion.mv_y = 0;
        point[++prednum].motion.mv_x = i * searchpos;
        point[prednum].motion.mv_y = i * searchpos;
        point[++prednum].motion.mv_x = 0;
        point[prednum].motion.mv_y = i * searchpos;
        point[++prednum].motion.mv_x = -i * searchpos;
        point[prednum].motion.mv_y = i * searchpos;
      }

      for (i = 1; i >= -1; i -= 2)
      {
        point[++prednum].motion.mv_x = i * fieldsearchpos;
        point[prednum].motion.mv_y = -i * searchpos;
        point[++prednum].motion.mv_x = i * fieldsearchpos;
        point[prednum].motion.mv_y = 0;
        point[++prednum].motion.mv_x = i * fieldsearchpos;
        point[prednum].motion.mv_y = i * searchpos;
        point[++prednum].motion.mv_x = i * searchpos;
        point[prednum].motion.mv_y = i * fieldsearchpos;
        point[++prednum].motion.mv_x = 0;
        point[prednum].motion.mv_y = i * fieldsearchpos;
        point[++prednum].motion.mv_x = -i * searchpos;
        point[prednum].motion.mv_y = i * fieldsearchpos;
      }
    }
  }

  predictor->searchPoints = prednum;
}

/*!
************************************************************************
* \brief
*    EPZS Global Initialization
************************************************************************
*/
int
EPZSStructInit (Slice * currSlice)
{
  ImageParameters *p_Img = currSlice->p_Img;
  InputParameters *p_Inp = currSlice->p_Inp;
  EPZSParameters *p_EPZS = currSlice->p_EPZS;
  int max_list_number = p_Img->MbaffFrameFlag ? 6 : 2;
  int pel_error_me = 1 << (p_Img->bitdepth_luma - 8);
  int pel_error_me_cr = 1 << (p_Img->bitdepth_chroma - 8);
  int i, memory_size = 0;
  double chroma_weight =
    p_Inp->ChromaMEEnable ? pel_error_me_cr * p_Inp->ChromaMEWeight * (double) (p_Img->width_cr * p_Img->height_cr) /
    (double) (p_Img->width * p_Img->height) : 0;
  int searchlevels = RoundLog2 (p_Inp->search_range) - 1;
  int searcharray =
    p_Inp->BiPredMotionEstimation ? (2 * imax (p_Inp->search_range, p_Inp->BiPredMESearchRange) +
    1) << 2 : (2 * p_Inp->search_range + 1) << 2;
  p_EPZS->p_Img = p_Img;
  p_EPZS->BlkCount = 1;

  //! In this implementation we keep threshold limits fixed.
  //! However one could adapt these limits based on lagrangian
  //! optimization considerations (i.e. qp), while also allow
  //! adaptation of the limits themselves based on content or complexity.

  for (i = 0; i < 8; ++i)
  {
    p_EPZS->medthres[i] = p_Inp->EPZSMedThresScale * (MED_THRES_BASE[i] * pel_error_me + (int) (MED_THRES_BASE[i] * chroma_weight + 0.5));
    p_EPZS->maxthres[i] = p_Inp->EPZSMaxThresScale * (MAX_THRES_BASE[i] * pel_error_me + (int) (MAX_THRES_BASE[i] * chroma_weight + 0.5));
    p_EPZS->minthres[i] = p_Inp->EPZSMinThresScale * (MIN_THRES_BASE[i] * pel_error_me + (int) (MIN_THRES_BASE[i] * chroma_weight + 0.5));
    p_EPZS->subthres[i] = p_Inp->EPZSSubPelThresScale * (MED_THRES_BASE[i] * pel_error_me + (int) (MED_THRES_BASE[i] * chroma_weight + 0.5));
  }
  //! Allocate and assign window based predictors.
  //! Other window types could also be used, while method could be
  //! made a bit more adaptive (i.e. patterns could be assigned
  //! based on neighborhood
  p_EPZS->window_predictor = allocEPZSpattern (searchlevels * 8);
  p_EPZS->window_predictor_ext = allocEPZSpattern (searchlevels * 20);
  EPZSWindowPredictorInit ((short) p_Inp->search_range, p_EPZS->window_predictor, 0);
  EPZSWindowPredictorInit ((short) p_Inp->search_range, p_EPZS->window_predictor_ext, 1);

  //! Also assing search predictor memory
  // maxwindow + spatial + blocktype + temporal + memspatial
  p_EPZS->predictor = allocEPZSpattern (searchlevels * 20 + 5 + 5 + 9 * (p_Inp->EPZSTemporal) + 3 * (p_Inp->EPZSSpatialMem));

  //! Finally assign memory for all other elements
  //! (distortion, EPZSMap, and temporal predictors)

  //memory_size += get_offset_mem2Dshort(&EPZSMap, searcharray, searcharray, (searcharray>>1), (searcharray>>1));  
  memory_size += get_mem3Dint (&(p_EPZS->distortion), max_list_number, 7, (p_Img->width + MB_BLOCK_SIZE)/ BLOCK_SIZE);

  if (p_Inp->BiPredMotionEstimation)
    memory_size += get_mem3Dint (&(p_EPZS->bi_distortion), max_list_number, 7, (p_Img->width + MB_BLOCK_SIZE) / BLOCK_SIZE);
  memory_size += get_mem2Dshort ((short ***) &(p_EPZS->EPZSMap), searcharray, searcharray);

  if (p_Inp->EPZSSpatialMem)
  {
#if EPZSREF
    memory_size += get_mem5Dmv (&(p_EPZS->p_motion), 6, p_Img->max_num_references, 7, 4, p_Img->width / BLOCK_SIZE);
#else 
    memory_size += get_mem4Dmv (&(p_EPZS->p_motion), 6, 7, 4, p_Img->width / BLOCK_SIZE);
#endif
  }

  if (p_Inp->EPZSTemporal)
    p_EPZS->p_colocated = allocEPZScolocated (p_Img->width, p_Img->height, p_Img->active_sps->mb_adaptive_frame_field_flag);

  switch (p_Inp->EPZSPattern)
  {
  case 5:
    p_EPZS->searchPattern = p_Img->pmvfast;
    break;
  case 4:
    p_EPZS->searchPattern = p_Img->sbdiamond;
    break;
  case 3:
    p_EPZS->searchPattern = p_Img->ldiamond;
    break;
  case 2:
    p_EPZS->searchPattern = p_Img->ediamond;
    break;
  case 1:
    p_EPZS->searchPattern = p_Img->square;
    break;
  case 0:
  default:
    p_EPZS->searchPattern = p_Img->sdiamond;
    break;
  }

  switch (p_Inp->EPZSDual)
  {
  case 6:
    p_EPZS->searchPatternD = p_Img->pmvfast;
    break;
  case 5:
    p_EPZS->searchPatternD = p_Img->sbdiamond;
    break;
  case 4:
    p_EPZS->searchPatternD = p_Img->ldiamond;
    break;
  case 3:
    p_EPZS->searchPatternD = p_Img->ediamond;
    break;
  case 2:
    p_EPZS->searchPatternD = p_Img->square;
    break;
  case 1:
  default:
    p_EPZS->searchPatternD = p_Img->sdiamond;
    break;
  }

  return memory_size;
}

/*!
************************************************************************
* \brief
*    Delete EPZS Alocated memory
************************************************************************
*/
void
EPZSStructDelete (Slice * currSlice)
{
  InputParameters *p_Inp = currSlice->p_Inp;
  EPZSParameters *p_EPZS = currSlice->p_EPZS;
  if (p_Inp->EPZSTemporal)
    freeEPZScolocated (p_EPZS->p_colocated);

  //free_offset_mem2Dshort(EPZSMap, searcharray, (searcharray>>1), (searcharray>>1));
  free_mem2Dshort ((short **) p_EPZS->EPZSMap);
  free_mem3Dint (p_EPZS->distortion);

  if (p_Inp->BiPredMotionEstimation)
    free_mem3Dint (p_EPZS->bi_distortion);

  freeEPZSpattern (p_EPZS->window_predictor_ext);
  freeEPZSpattern (p_EPZS->window_predictor);
  freeEPZSpattern (p_EPZS->predictor);

  if (p_Inp->EPZSSpatialMem)
  {
#if EPZSREF
    free_mem5Dmv (p_EPZS->p_motion);
#else
    free_mem4Dmv (p_EPZS->p_motion);
#endif
  }

  free (currSlice->p_EPZS);
  currSlice->p_EPZS = NULL;
}

//! For ME purposes restricting the co-located partition is not necessary.
/*!
************************************************************************
* \brief
*    EPZS Slice Level Initialization
************************************************************************
*/
void
EPZSSliceInit (Slice * currSlice)
{
  ImageParameters *p_Img = currSlice->p_Img;
  InputParameters *p_Inp = currSlice->p_Inp;
  StorablePicture *p_Pic = p_Img->enc_picture;
  EPZSColocParams *p = currSlice->p_EPZS->p_colocated;
  StorablePicture ***listX = p_Img->listX;
  StorablePicture *fs, *fs_top, *fs_bottom;
  StorablePicture *fs1, *fs_top1, *fs_bottom1, *fsx;
  EPZSParameters *p_EPZS = currSlice->p_EPZS;
  PicMotionParams *p_motion = NULL;
  int i, j, k, jj, jdiv, loffset;
  int prescale, iTRb, iTRp;
  int list = (currSlice->slice_type == B_SLICE) ? LIST_1 : LIST_0;
  int tempmv_scale[2];
  int epzs_scale[2][6][MAX_LIST_SIZE];
  int iref;
  int invmv_precision = 8;

  // Lets compute scaling factoes between all references in lists.
  // Needed to scale spatial predictors.
  for (j = LIST_0; j < 2 + (currSlice->MbaffFrameFlag << 2); ++j)
  {
    for (k = 0; k < p_Img->listXsize[j]; ++k)
    {
      for (i = 0; i < p_Img->listXsize[j]; ++i)
      {
        if ((j >> 1) == 0)
        {
          iTRb = iClip3 (-128, 127, p_Pic->poc - listX[j][i]->poc);
          iTRp = iClip3 (-128, 127, p_Pic->poc - listX[j][k]->poc);
        }
        else if ((j >> 1) == 1)
        {
          iTRb = iClip3 (-128, 127, p_Pic->top_poc - listX[j][i]->poc);
          iTRp = iClip3 (-128, 127, p_Pic->top_poc - listX[j][k]->poc);
        }
        else
        {
          iTRb = iClip3 (-128, 127, p_Pic->bottom_poc - listX[j][i]->poc);
          iTRp = iClip3 (-128, 127, p_Pic->bottom_poc - listX[j][k]->poc);
        }

        if (iTRp != 0)
        {
          prescale = (16384 + iabs (iTRp / 2)) / iTRp;
          p_EPZS->mv_scale[j][i][k] = iClip3 (-2048, 2047, rshift_rnd_sf ((iTRb * prescale), 6));
        }
        else
          p_EPZS->mv_scale[j][i][k] = 256;
      }
    }
  }

  if (p_Inp->EPZSTemporal)
  {
    MotionVector **MotionVector0 = p->frame[LIST_0];
    MotionVector **MotionVector1 = p->frame[LIST_1];

    fs_top = fs_bottom = fs = listX[list][0];
    if (p_Img->listXsize[list] > 1)
      fs_top1 = fs_bottom1 = fs1 = listX[list][1];
    else
      fs_top1 = fs_bottom1 = fs1 = listX[list][0];
    for (j = 0; j < 6; ++j)
    {
      for (i = 0; i < 6; ++i)
      {
        epzs_scale[0][j][i] = 256;
        epzs_scale[1][j][i] = 256;
      }
    }

    for (j = 0; j < 2 + (currSlice->MbaffFrameFlag << 2); j += 2)
    {
      for (i = 0; i < p_Img->listXsize[j]; ++i)
      {
        if (j == 0)
          iTRb = iClip3 (-128, 127, p_Pic->poc - listX[LIST_0 + j][i]->poc);
        else if (j == 2)
          iTRb = iClip3 (-128, 127, p_Pic->top_poc - listX[LIST_0 + j][i]->poc);
        else
          iTRb = iClip3 (-128, 127, p_Pic->bottom_poc - listX[LIST_0 + j][i]->poc);
        iTRp = iClip3 (-128, 127, listX[list + j][0]->poc - listX[LIST_0 + j][i]->poc);

        if (iTRp != 0)
        {
          prescale = (16384 + iabs (iTRp / 2)) / iTRp;
          prescale = iClip3 (-2048, 2047, rshift_rnd_sf ((iTRb * prescale), 6));

          //prescale = (iTRb * prescale + 32) >> 6;
        }
        else                    // This could not happen but lets use it in case that reference is removed.
          prescale = 256;

        epzs_scale[0][j][i] = rshift_rnd_sf ((p_EPZS->mv_scale[j][0][i] * prescale), 8);
        epzs_scale[0][j + 1][i] = prescale - 256;

        if (p_Img->listXsize[list + j] > 1)
        {
          iTRp = iClip3 (-128, 127, listX[list + j][1]->poc - listX[LIST_0 + j][i]->poc);
          if (iTRp != 0)
          {
            prescale = (16384 + iabs (iTRp / 2)) / iTRp;
            prescale = iClip3 (-2048, 2047, rshift_rnd_sf ((iTRb * prescale), 6));
            //prescale = (iTRb * prescale + 32) >> 6;
          }
          else                  // This could not happen but lets use it for case that reference is removed.
            prescale = 256;

          epzs_scale[1][j][i] = rshift_rnd_sf ((p_EPZS->mv_scale[j][1][i] * prescale), 8);
          epzs_scale[1][j + 1][i] = prescale - 256;
        }
        else
        {
          epzs_scale[1][j][i] = epzs_scale[0][j][i];
          epzs_scale[1][j + 1][i] = epzs_scale[0][j + 1][i];
        }
      }
    }

    if (currSlice->MbaffFrameFlag)
    {
      fs_top = listX[list + 2][0];
      fs_bottom = listX[list + 4][0];

      if (p_Img->listXsize[0] > 1)
      {
        fs_top1 = listX[list + 2][1];
        fs_bottom1 = listX[list + 4][1];
      }
    }
    else
    {
      if (currSlice->structure != FRAME)
      {
        if ((currSlice->structure != fs->structure) && (fs->coded_frame))
        {
          if (currSlice->structure == TOP_FIELD)
          {
            fs_top = fs_bottom = fs = listX[list][0]->top_field;
            fs_top1 = fs_bottom1 = fs1 = listX[list][0]->bottom_field;
          }
          else
          {
            fs_top = fs_bottom = fs = listX[list][0]->bottom_field;
            fs_top1 = fs_bottom1 = fs1 = listX[list][0]->top_field;
          }
        }
      }
    }

    p_motion = &fs->motion;

    if (!currSlice->active_sps->frame_mbs_only_flag)
    {
      if (currSlice->MbaffFrameFlag)
      {
        for (j = 0; j < fs->size_y >> 2; ++j)
        {
          jj = j >> 1;
          jdiv = jj + 4 * (j >> 3);
          for (i = 0; i < fs->size_x >> 2; ++i)
          {
            if (p_motion->field_frame[j][i])
            {
              //! Assign frame buffers for field MBs
              //! Check whether we should use top or bottom field mvs.
              //! Depending on the assigned poc values.
              if (iabs (p_Pic->poc - fs_bottom->poc) > iabs (p_Pic->poc - fs_top->poc))
              {
                tempmv_scale[LIST_0] = 256;
                tempmv_scale[LIST_1] = 0;

                if (p_motion->ref_id[LIST_0][jdiv][i] < 0 && p_Img->listXsize[LIST_0] > 1)
                {
                  fsx = fs_top1;
                  loffset = 1;
                }
                else
                {
                  fsx = fs_top;
                  loffset = 0;
                }

                if (p_motion->ref_id[LIST_0][jdiv][i] != -1)
                {
                  for (iref = 0; iref < imin (currSlice->num_ref_idx_active[LIST_0], p_Img->listXsize[LIST_0]); ++iref)
                  {
                    if (p_Pic->ref_pic_num[LIST_0][iref] == p_motion->ref_id[LIST_0][jdiv][i])
                    {
                      tempmv_scale[LIST_0] = epzs_scale[loffset][LIST_0][iref];
                      tempmv_scale[LIST_1] = epzs_scale[loffset][LIST_1][iref];
                      break;
                    }
                  }

                  compute_scaled (&MotionVector0[j][i], &MotionVector1[j][i], tempmv_scale, fsx->motion.mv[LIST_0][jj][i], invmv_precision);
                }
                else
                {
                  MotionVector0[j][i].mv_x = 0;
                  MotionVector0[j][i].mv_y = 0;
                  MotionVector1[j][i].mv_x = 0;
                  MotionVector1[j][i].mv_y = 0;
                }
              }
              else
              {
                tempmv_scale[LIST_0] = 256;
                tempmv_scale[LIST_1] = 0;
                if (p_motion->ref_id[LIST_0][jdiv + 4][i] < 0 && p_Img->listXsize[LIST_0] > 1)
                {
                  fsx = fs_bottom1;
                  loffset = 1;
                }
                else
                {
                  fsx = fs_bottom;
                  loffset = 0;
                }

                if (p_motion->ref_id[LIST_0][jdiv + 4][i] != -1)
                {
                  for (iref = 0; iref < imin (currSlice->num_ref_idx_active[LIST_0], p_Img->listXsize[LIST_0]); ++iref)
                  {
                    if (p_Pic->ref_pic_num[LIST_0][iref] == p_motion->ref_id[LIST_0][jdiv + 4][i])
                    {
                      tempmv_scale[LIST_0] = epzs_scale[loffset][LIST_0][iref];
                      tempmv_scale[LIST_1] = epzs_scale[loffset][LIST_1][iref];
                      break;
                    }
                  }

                  compute_scaled (&MotionVector0[j][i], &MotionVector1[j][i], tempmv_scale, fsx->motion.mv[LIST_0][jj][i], invmv_precision);
                }
                else
                {
                  MotionVector0[j][i].mv_x = 0;
                  MotionVector0[j][i].mv_y = 0;
                  MotionVector1[j][i].mv_x = 0;
                  MotionVector1[j][i].mv_y = 0;
                }
              }
            }
            else
            {
              tempmv_scale[LIST_0] = 256;
              tempmv_scale[LIST_1] = 0;

              if (p_motion->ref_id[LIST_0][j][i] < 0 && p_Img->listXsize[LIST_0] > 1)
              {
                fsx = fs1;
                loffset = 1;
              }
              else
              {
                fsx = fs;
                loffset = 0;
              }

              if (fsx->motion.ref_id[LIST_0][j][i] != -1)
              {
                for (iref = 0; iref < imin (currSlice->num_ref_idx_active[LIST_0], p_Img->listXsize[LIST_0]); ++iref)
                {
                  if (p_Pic->ref_pic_num[LIST_0][iref] == fsx->motion.ref_id[LIST_0][j][i])
                  {
                    tempmv_scale[LIST_0] = epzs_scale[loffset][LIST_0][iref];
                    tempmv_scale[LIST_1] = epzs_scale[loffset][LIST_1][iref];
                    break;
                  }
                }
                compute_scaled (&MotionVector0[j][i], &MotionVector1[j][i], tempmv_scale, fsx->motion.mv[LIST_0][j][i], invmv_precision);
              }
              else
              {
                MotionVector0[j][i].mv_x = 0;
                MotionVector0[j][i].mv_y = 0;
                MotionVector1[j][i].mv_x = 0;
                MotionVector1[j][i].mv_y = 0;
              }
            }
          }
        }
      }
      else
      {
        for (j = 0; j < fs->size_y >> 2; ++j)
        {
          jj = j >> 1;
          jdiv = jj + 4 * (j >> 3);
          for (i = 0; i < fs->size_x >> 2; ++i)
          {
            tempmv_scale[LIST_0] = 256;
            tempmv_scale[LIST_1] = 0;
            if (p_motion->ref_id[LIST_0][j][i] < 0 && p_Img->listXsize[LIST_0] > 1)
            {
              fsx = fs1;
              loffset = 1;
            }
            else
            {
              fsx = fs;
              loffset = 0;
            }

            if (fsx->motion.ref_id[LIST_0][j][i] != -1)
            {
              for (iref = 0; iref < imin (currSlice->num_ref_idx_active[LIST_0], p_Img->listXsize[LIST_0]); ++iref)
              {
                if (p_Pic->ref_pic_num[LIST_0][iref] == fsx->motion.ref_id[LIST_0][j][i])
                {
                  tempmv_scale[LIST_0] = epzs_scale[loffset][LIST_0][iref];
                  tempmv_scale[LIST_1] = epzs_scale[loffset][LIST_1][iref];

                  break;
                }
              }

              compute_scaled (&MotionVector0[j][i], &MotionVector1[j][i], tempmv_scale, fsx->motion.mv[LIST_0][j][i], invmv_precision);
            }
            else
            {
              MotionVector0[j][i].mv_x = 0;
              MotionVector0[j][i].mv_y = 0;
              MotionVector1[j][i].mv_x = 0;
              MotionVector1[j][i].mv_y = 0;
            }
          }
        }
      }

      //! Generate field MVs from Frame MVs
      if (currSlice->structure || currSlice->MbaffFrameFlag)
      {
        for (j = 0; j < fs->size_y >> 3; ++j)
        {
          for (i = 0; i < fs->size_x >> 2; ++i)
          {
            if (!currSlice->MbaffFrameFlag)
            {
              tempmv_scale[LIST_0] = 256;
              tempmv_scale[LIST_1] = 0;
              if (p_motion->ref_id[LIST_0][j][i] < 0 && p_Img->listXsize[LIST_0] > 1)
              {
                fsx = fs1;
                loffset = 1;
              }
              else
              {
                fsx = fs;
                loffset = 0;
              }

              if (fsx->motion.ref_id[LIST_0][j][i] != -1)
              {
                for (iref = 0; iref < imin (currSlice->num_ref_idx_active[LIST_0], p_Img->listXsize[LIST_0]); ++iref)
                {
                  if (p_Pic->ref_pic_num[LIST_0][iref] == fsx->motion.ref_id[LIST_0][j][i])
                  {
                    tempmv_scale[LIST_0] = epzs_scale[loffset][LIST_0][iref];
                    tempmv_scale[LIST_1] = epzs_scale[loffset][LIST_1][iref];
                    break;
                  }
                }
                compute_scaled (&MotionVector0[j][i], &MotionVector1[j][i], tempmv_scale, fsx->motion.mv[LIST_0][j][i], invmv_precision);
              }
              else
              {
                MotionVector0[j][i].mv_x = 0;
                MotionVector0[j][i].mv_y = 0;
                MotionVector1[j][i].mv_x = 0;
                MotionVector1[j][i].mv_y = 0;
              }
            }
            else
            {
              tempmv_scale[LIST_0] = 256;
              tempmv_scale[LIST_1] = 0;
              if (fs_bottom->motion.ref_id[LIST_0][j][i] < 0 && p_Img->listXsize[LIST_0] > 1)
              {
                fsx = fs_bottom1;
                loffset = 1;
              }
              else
              {
                fsx = fs_bottom;
                loffset = 0;
              }

              if (fsx->motion.ref_id[LIST_0][j][i] != -1)
              {
                for (iref = 0; iref < imin (2 * currSlice->num_ref_idx_active[LIST_0], p_Img->listXsize[LIST_0 + 4]); ++iref)
                {
                  if (p_Pic->ref_pic_num[LIST_0 + 4][iref] == fsx->motion.ref_id[LIST_0][j][i])
                  {
                    tempmv_scale[LIST_0] = epzs_scale[loffset][LIST_0 + 4][iref];
                    tempmv_scale[LIST_1] = epzs_scale[loffset][LIST_1 + 4][iref];
                    break;
                  }
                }

                compute_scaled (&p->bot[LIST_0][j][i], &p->bot[LIST_1][j][i], tempmv_scale, fsx->motion.mv[LIST_0][j][i], invmv_precision);
              }
              else
              {
                p->bot[LIST_0][j][i].mv_x = 0;
                p->bot[LIST_0][j][i].mv_y = 0;
                p->bot[LIST_1][j][i].mv_x = 0;
                p->bot[LIST_1][j][i].mv_y = 0;
              }

              if (!p_motion->field_frame[2 * j][i])
              {
                p->bot[LIST_0][j][i].mv_y = (p->bot[LIST_0][j][i].mv_y + 1) >> 1;
                p->bot[LIST_1][j][i].mv_y = (p->bot[LIST_1][j][i].mv_y + 1) >> 1;
              }

              tempmv_scale[LIST_0] = 256;
              tempmv_scale[LIST_1] = 0;
              if (fs_top->motion.ref_id[LIST_0][j][i] < 0 && p_Img->listXsize[LIST_0] > 1)
              {
                fsx = fs_top1;
                loffset = 1;
              }
              else
              {
                fsx = fs_top;
                loffset = 0;
              }
              if (fsx->motion.ref_id[LIST_0][j][i] != -1)
              {
                for (iref = 0; iref < imin (2 * currSlice->num_ref_idx_active[LIST_0], p_Img->listXsize[LIST_0 + 2]); ++iref)
                {
                  if (p_Pic->ref_pic_num[LIST_0 + 2][iref] == fsx->motion.ref_id[LIST_0][j][i])
                  {
                    tempmv_scale[LIST_0] = epzs_scale[loffset][LIST_0 + 2][iref];
                    tempmv_scale[LIST_1] = epzs_scale[loffset][LIST_1 + 2][iref];
                    break;
                  }
                }

                compute_scaled (&p->top[LIST_0][j][i], &p->top[LIST_1][j][i], tempmv_scale, fsx->motion.mv[LIST_0][j][i], invmv_precision);
              }
              else
              {
                p->top[LIST_0][j][i].mv_x = 0;
                p->top[LIST_0][j][i].mv_y = 0;
                p->top[LIST_1][j][i].mv_x = 0;
                p->top[LIST_1][j][i].mv_y = 0;
              }

              if (!p_motion->field_frame[2 * j][i])
              {
                p->top[LIST_0][j][i].mv_y = (p->top[LIST_0][j][i].mv_y + 1) >> 1;
                p->top[LIST_1][j][i].mv_y = (p->top[LIST_1][j][i].mv_y + 1) >> 1;
              }
            }
          }
        }
      }

      //! Use inference flag to remap mvs/references
      //! Frame with field co-located
      if (!currSlice->structure)
      {
        for (j = 0; j < fs->size_y >> 2; ++j)
        {
          jj = j >> 1;
          jdiv = (j >> 1) + ((j >> 3) << 2);
          for (i = 0; i < fs->size_x >> 2; ++i)
          {
            if (p_motion->field_frame[j][i])
            {
              tempmv_scale[LIST_0] = 256;
              tempmv_scale[LIST_1] = 0;
              if (p_motion->ref_id[LIST_0][jdiv][i] < 0 && p_Img->listXsize[LIST_0] > 1)
              {
                fsx = fs1;
                loffset = 1;
              }
              else
              {
                fsx = fs;
                loffset = 0;
              }

              if (fsx->motion.ref_id[LIST_0][jdiv][i] != -1)
              {
                for (iref = 0; iref < imin (currSlice->num_ref_idx_active[LIST_0], p_Img->listXsize[LIST_0]); ++iref)
                {
                  if (p_Pic->ref_pic_num[LIST_0][iref] == fsx->motion.ref_id[LIST_0][jdiv][i])
                  {
                    tempmv_scale[LIST_0] = epzs_scale[loffset][LIST_0][iref];
                    tempmv_scale[LIST_1] = epzs_scale[loffset][LIST_1][iref];
                    break;
                  }
                }
                if (iabs (p_Pic->poc - fsx->bottom_field->poc) > iabs (p_Pic->poc - fsx->top_field->poc))
                {
                  compute_scaled (&MotionVector0[j][i], &MotionVector1[j][i], tempmv_scale,
                    fsx->top_field->motion.mv[LIST_0][j][i], invmv_precision);
                }
                else
                {
                  compute_scaled (&MotionVector0[j][i], &MotionVector1[j][i], tempmv_scale,
                    fsx->bottom_field->motion.mv[LIST_0][j][i], invmv_precision);
                }
              }
              else
              {
                MotionVector0[j][i].mv_x = 0;
                MotionVector0[j][i].mv_y = 0;
                MotionVector1[j][i].mv_x = 0;
                MotionVector1[j][i].mv_y = 0;
              }
            }
          }
        }
      }
    }
    else
    {
      for (j = 0; j < fs->size_y >> 2; ++j)
      {
        for (i = 0; i < fs->size_x >> 2; ++i)
        {
          tempmv_scale[LIST_0] = 256;
          tempmv_scale[LIST_1] = 0;

          if (p_motion->ref_id[LIST_0][j][i] < 0 && p_Img->listXsize[LIST_0] > 1)
          {
            fsx = fs1;
            loffset = 1;
          }
          else
          {
            fsx = fs;
            loffset = 0;
          }
          if (fsx->motion.ref_id[LIST_0][j][i] != -1)
          {
            for (iref = 0; iref < imin (currSlice->num_ref_idx_active[LIST_0], p_Img->listXsize[LIST_0]); ++iref)
            {
              if (p_Pic->ref_pic_num[LIST_0][iref] == fsx->motion.ref_id[LIST_0][j][i])
              {
                tempmv_scale[LIST_0] = epzs_scale[loffset][LIST_0][iref];
                tempmv_scale[LIST_1] = epzs_scale[loffset][LIST_1][iref];
                break;
              }
            }

            compute_scaled (&MotionVector0[j][i], &MotionVector1[j][i], tempmv_scale, fsx->motion.mv[LIST_0][j][i], invmv_precision);
          }
          else
          {
            MotionVector0[j][i].mv_x = 0;
            MotionVector0[j][i].mv_y = 0;
            MotionVector1[j][i].mv_x = 0;
            MotionVector1[j][i].mv_y = 0;
          }
        }
      }
    }

    if (!currSlice->active_sps->frame_mbs_only_flag)
    {
      for (j = 0; j < fs->size_y >> 2; ++j)
      {
        for (i = 0; i < fs->size_x >> 2; ++i)
        {
          if ((!currSlice->MbaffFrameFlag && !currSlice->structure && p_motion->field_frame[j][i])
            || (currSlice->MbaffFrameFlag && p_motion->field_frame[j][i]))
          {
            MotionVector0[j][i].mv_y *= 2;
            MotionVector1[j][i].mv_y *= 2;
          }
          else if (currSlice->structure && !p_motion->field_frame[j][i])
          {
            MotionVector0[j][i].mv_y = (short) rshift_rnd_sf (MotionVector0[j][i].mv_y, 1);
            MotionVector1[j][i].mv_y = (short) rshift_rnd_sf (MotionVector1[j][i].mv_y, 1);
          }
        }
      }
    }
  }
}


static void
is_block_available (Macroblock * currMB, StorablePicture * ref_picture, MEBlock * mv_block, int block_available[4])
{
  if ((mv_block->block_y << 2) > 0)
  {
    if ((mv_block->block_x << 2) < 8) // first column of 8x8 blocks
    {
      if ((mv_block->block_y << 2) == 8)
      {
        block_available[0] = (mv_block->blocksize_x != MB_BLOCK_SIZE) || (currMB->mb_x < (ref_picture->size_x >> 4) - 1);
      }
      else
      {
        block_available[0] = ((mv_block->block_x << 2) + mv_block->blocksize_x != 8)
          || (currMB->mb_x < (ref_picture->size_x >> 4) - 1);
      }
    }
    else
    {
      block_available[0] = ((mv_block->block_x << 2) + mv_block->blocksize_x != MB_BLOCK_SIZE) || (currMB->mb_x < (ref_picture->size_x >> 4) - 1);
    }
  }
  else
  {
    block_available[0] = ((mv_block->block_x << 2) + mv_block->blocksize_x != MB_BLOCK_SIZE) || (currMB->mb_x < (ref_picture->size_x >> 4) - 1);
  }

  block_available[1] = ((mv_block->block_y << 2) + mv_block->blocksize_y != MB_BLOCK_SIZE) || ((currMB->mb_y < (ref_picture->size_y >> 4) - 1));
  block_available[2] = mv_block->block[0].available;
  block_available[3] = mv_block->block[1].available;
}

/*!
************************************************************************
* \brief
*    EPZS Block Type Predictors
************************************************************************
*/
void
EPZSBlockTypePredictorsMB (Slice * currSlice, MEBlock * mv_block, SPoint * point, int *prednum)
{
  int blocktype = mv_block->blocktype;
  int block_x   = mv_block->block_x;
  int block_y   = mv_block->block_y;
  int list      = mv_block->list;
  int ref       = mv_block->ref_idx;
  EPZSParameters *p_EPZS = currSlice->p_EPZS;
  short *****all_mv = currSlice->all_mv[list];
  MotionVector *cur_mv = &point[*prednum].motion;

  if (blocktype != 1)
  {
    short *mv = all_mv[ref][BLOCK_PARENT[blocktype]][block_y][block_x];
    cur_mv->mv_x = mv[0];
    cur_mv->mv_y = mv[1];
    //*prednum += ((cur_mv->mv_x | cur_mv->mv_y) != 0);
    *prednum += (*((int *) cur_mv) != 0);

    cur_mv = &point[*prednum].motion;
    mv = all_mv[ref][1][block_y][block_x];
    cur_mv->mv_x = mv[0];
    cur_mv->mv_y = mv[1];
    //*prednum += ((cur_mv->mv_x | cur_mv->mv_y) != 0);
    *prednum += (*((int *) cur_mv) != 0);
  }

  if (ref > 0)
  {
    cur_mv = &point[*prednum].motion;
    scale_mv (cur_mv, p_EPZS->mv_scale[list][ref][ref - 1], all_mv[ref - 1][blocktype][block_y][block_x], 8);
    //*prednum += ((cur_mv->mv_x | cur_mv->mv_y) != 0);
    *prednum += (*((int *) cur_mv) != 0);

    if (ref > 1)
    {
      cur_mv = &point[*prednum].motion;
      scale_mv (cur_mv, p_EPZS->mv_scale[list][ref][0], all_mv[0][blocktype][block_y][block_x], 8);
      //*prednum += ((cur_mv->mv_x | cur_mv->mv_y) != 0);
      *prednum += (*((int *) cur_mv) != 0);
    }
  }
}

/*!
***********************************************************************
* \brief
*    Spatial Predictors
*    AMT/HYC
***********************************************************************
*/
short
EPZSSpatialPredictors (EPZSParameters * p_EPZS, PixelPos * block, int list, int list_offset, short ref, char **refPic, short ***tmp_mv)
{
  short refA = 0, refB = 0, refC = 0, refD = 0;
  ImageParameters *p_Img = p_EPZS->p_Img;
  int *mot_scale = p_EPZS->mv_scale[list + list_offset][ref];
  SPoint *point = p_EPZS->predictor->point;

  // zero predictor
  (point)->motion.mv_x = 0;
  (point++)->motion.mv_y = 0;

  // Non MB-AFF mode
  if (!p_Img->MbaffFrameFlag)
  {
    refA = block[0].available ? (short) refPic[block[0].pos_y][block[0].pos_x] : -1;
    refB = block[1].available ? (short) refPic[block[1].pos_y][block[1].pos_x] : -1;
    refC = block[2].available ? (short) refPic[block[2].pos_y][block[2].pos_x] : -1;
    refD = block[3].available ? (short) refPic[block[3].pos_y][block[3].pos_x] : -1;

    // Left Predictor
    if (block[0].available)
    {
      scale_mv (&point->motion, mot_scale[refA], tmp_mv[block[0].pos_y][block[0].pos_x], 8);
      /*
      if (*((int*) &point->motion) == 0)
      {
      point->motion.mv_x = 12;
      point->motion.mv_y = 0;
      }
      */
      ++point;
    }
    else
    {
      (point)->motion.mv_x = 12;
      (point++)->motion.mv_y = 0;
    }
    // Up predictor
    if (block[1].available)
    {
      scale_mv (&point->motion, mot_scale[refB], tmp_mv[block[1].pos_y][block[1].pos_x], 8);
      /*
      if (*((int*) &point->motion) == 0)
      {
      point->motion.mv_x = 0;
      point->motion.mv_y = 12;
      }
      */
      ++point;
    }
    else
    {
      (point)->motion.mv_x = 0;
      (point++)->motion.mv_y = 12;
    }

    // Up-Right predictor
    if (block[2].available)
    {
      scale_mv (&point->motion, mot_scale[refC], tmp_mv[block[2].pos_y][block[2].pos_x], 8);
      /*
      if (*((int*) &point->motion) == 0)
      {
      point->motion.mv_x = -12;
      point->motion.mv_y = 0;
      }
      */
      ++point;
    }
    else
    {
      (point)->motion.mv_x = -12;
      (point++)->motion.mv_y = 0;
    }

    //Up-Left predictor
    if (block[3].available)
    {
      scale_mv (&point->motion, mot_scale[refD], tmp_mv[block[3].pos_y][block[3].pos_x], 8);
      /*
      if (*((int*) &point->motion) == 0)
      {
      point->motion.mv_x = 0;
      point->motion.mv_y = -12;
      }
      */
      ++point;
    }
    else
    {
      (point)->motion.mv_x = 0;
      (point++)->motion.mv_y = -12;
    }
  }
  else                          // MB-AFF mode
  {
    // Field Macroblock
    if (list_offset)
    {
      refA = block[0].available ? p_Img->mb_data[block[0].mb_addr].mb_field ? (short) refPic[block[0].pos_y][block[0].pos_x]
      : (short) refPic[block[0].pos_y][block[0].pos_x] * 2 : -1;
      refB = block[1].available ? p_Img->mb_data[block[1].mb_addr].mb_field ? (short) refPic[block[1].pos_y][block[1].pos_x]
      : (short) refPic[block[1].pos_y][block[1].pos_x] * 2 : -1;
      refC = block[2].available ? p_Img->mb_data[block[2].mb_addr].mb_field ? (short) refPic[block[2].pos_y][block[2].pos_x]
      : (short) refPic[block[2].pos_y][block[2].pos_x] * 2 : -1;
      refD = block[3].available ? p_Img->mb_data[block[3].mb_addr].mb_field ? (short) refPic[block[3].pos_y][block[3].pos_x]
      : (short) refPic[block[3].pos_y][block[3].pos_x] * 2 : -1;

      // Left Predictor
      if (block[0].available)
      {
        scale_mv (&point->motion, mot_scale[refA], tmp_mv[block[0].pos_y][block[0].pos_x], 8);
        if (!p_Img->mb_data[block[0].mb_addr].mb_field)
          //point->motion.mv_y = (short) rshift_rnd_sf(point->motion.mv_y, 1);
          point->motion.mv_y <<= 1;
        ++point;
      }
      else
      {
        (point)->motion.mv_x = 12;
        (point++)->motion.mv_y = 0;
      }

      // Up predictor
      if (block[1].available)
      {
        scale_mv (&point->motion, mot_scale[refB], tmp_mv[block[1].pos_y][block[1].pos_x], 8);
        if (!p_Img->mb_data[block[1].mb_addr].mb_field)
          //point->motion.mv_y = (short) rshift_rnd_sf(point->motion.mv_y, 1);
          point->motion.mv_y <<= 1;
        ++point;
      }
      else
      {
        (point)->motion.mv_x = 0;
        (point++)->motion.mv_y = 12;
      }

      // Up-Right predictor
      if (block[2].available)
      {
        scale_mv (&point->motion, mot_scale[refC], tmp_mv[block[2].pos_y][block[2].pos_x], 8);
        if (!p_Img->mb_data[block[2].mb_addr].mb_field)
          //point->motion.mv_y = (short) rshift_rnd_sf(point->motion.mv_y, 1);
          point->motion.mv_y <<= 1;
        ++point;
      }
      else
      {
        (point)->motion.mv_x = -12;
        (point++)->motion.mv_y = 0;
      }

      //Up-Left predictor
      if (block[3].available)
      {
        scale_mv (&point->motion, mot_scale[refD], tmp_mv[block[3].pos_y][block[3].pos_x], 8);
        if (!p_Img->mb_data[block[3].mb_addr].mb_field)
          //point->motion.mv_y = (short) rshift_rnd_sf(point->motion.mv_y, 1);
          point->motion.mv_y <<= 1;
        ++point;
      }
      else
      {
        (point)->motion.mv_x = 0;
        (point++)->motion.mv_y = -12;
      }
    }
    else                        // Frame macroblock
    {
      refA = block[0].available
        ? p_Img->mb_data[block[0].mb_addr].mb_field
        ? (short) refPic[block[0].pos_y][block[0].pos_x] >> 1 : (short) refPic[block[0].pos_y][block[0].pos_x] : -1;
      refB = block[1].available
        ? p_Img->mb_data[block[1].mb_addr].mb_field
        ? (short) refPic[block[1].pos_y][block[1].pos_x] >> 1 : (short) refPic[block[1].pos_y][block[1].pos_x] : -1;
      refC = block[2].available
        ? p_Img->mb_data[block[2].mb_addr].mb_field
        ? (short) refPic[block[2].pos_y][block[2].pos_x] >> 1 : (short) refPic[block[2].pos_y][block[2].pos_x] : -1;
      refD = block[3].available
        ? p_Img->mb_data[block[3].mb_addr].mb_field
        ? (short) refPic[block[3].pos_y][block[3].pos_x] >> 1 : (short) refPic[block[3].pos_y][block[3].pos_x] : -1;

      // Left Predictor
      if (block[0].available)
      {
        scale_mv (&point->motion, mot_scale[refA], tmp_mv[block[0].pos_y][block[0].pos_x], 8);
        if (p_Img->mb_data[block[0].mb_addr].mb_field)
          point->motion.mv_y = (short) rshift_rnd_sf (point->motion.mv_y, 1);
        ++point;
      }
      else
      {
        (point)->motion.mv_x = 12;
        (point++)->motion.mv_y = 0;
      }

      // Up predictor
      if (block[1].available)
      {
        scale_mv (&point->motion, mot_scale[refB], tmp_mv[block[1].pos_y][block[1].pos_x], 8);
        if (p_Img->mb_data[block[1].mb_addr].mb_field)
          point->motion.mv_y = (short) rshift_rnd_sf (point->motion.mv_y, 1);
        ++point;
      }
      else
      {
        (point)->motion.mv_x = 0;
        (point++)->motion.mv_y = 12;
      }

      // Up-Right predictor
      if (block[2].available)
      {
        scale_mv (&point->motion, mot_scale[refC], tmp_mv[block[2].pos_y][block[2].pos_x], 8);
        if (p_Img->mb_data[block[2].mb_addr].mb_field)
          point->motion.mv_y = (short) rshift_rnd_sf (point->motion.mv_y, 1);
        ++point;
      }
      else
      {
        (point)->motion.mv_x = -12;
        (point++)->motion.mv_y = 0;
      }

      //Up-Left predictor
      if (block[3].available)
      {
        scale_mv (&point->motion, mot_scale[refD], tmp_mv[block[3].pos_y][block[3].pos_x], 8);
        if (p_Img->mb_data[block[3].mb_addr].mb_field)
          point->motion.mv_y = (short) rshift_rnd_sf (point->motion.mv_y, 1);
        ++point;
      }
      else
      {
        (point)->motion.mv_x = 12;
        (point++)->motion.mv_y = 0;
      }
    }
  }

  return ((refA == -1) + (refB == -1) + ((refC == -1) && (refD == -1)));
}

/*!
***********************************************************************
* \brief
*    Temporal Predictors
*    AMT/HYC
***********************************************************************
*/
void
EPZSTemporalPredictors (Macroblock *currMB,                 //!< Currrent Macroblock
                        StorablePicture *ref_picture,       //!< Current reference picture
                        EPZSParameters * p_EPZS,            //!< EPZS structure
                        MEBlock *mv_block,                  //!< motion estimation information block
                        int *prednum,                       //!< Pointer to first empty position in EPZS predictor list
                        int stopCriterion,                  //!< EPZS thresholding criterion for temporal predictor considerations
                        int min_mcost)                      //!< Current minimum distortion for block
{
  int list_offset  = currMB->list_offset;
  int blockshape_x = (mv_block->blocksize_x >> 2);  // horizontal block size in 4-pel units
  int blockshape_y = (mv_block->blocksize_y >> 2);  // vertical block size in 4-pel units
  int o_block_x = mv_block->pos_x2;
  int o_block_y = mv_block->pos_y2;
  int list      = mv_block->list;
  int ref       = mv_block->ref_idx;

  EPZSColocParams *p_Coloc = p_EPZS->p_colocated;
  SPoint * point = p_EPZS->predictor->point;
  int mvScale = p_EPZS->mv_scale[list + list_offset][ref][0];
  MotionVector **col_mv = (list_offset == 0) ? p_Coloc->frame[list] : (list_offset == 2) ? p_Coloc->top[list] : p_Coloc->bot[list];
  MotionVector *cur_mv = &point[*prednum].motion;

  *prednum += add_predictor (cur_mv, col_mv[o_block_y][o_block_x], mvScale, 8);

  if (min_mcost > stopCriterion && ref < 2)
  {
    int block_available[4];
    is_block_available (currMB, ref_picture, mv_block, block_available);

    if (block_available[2])
    {
      *prednum += add_predictor (&point[*prednum].motion, col_mv[o_block_y][o_block_x - 1], mvScale, 8);

      //Up_Left
      if (block_available[3])
      {
        *prednum += add_predictor (&point[*prednum].motion, col_mv[o_block_y - 1][o_block_x - 1], mvScale, 8);
      }

      //Down_Left
      if (block_available[1])
      {
        *prednum += add_predictor (&point[*prednum].motion, col_mv[o_block_y + blockshape_y][o_block_x - 1], mvScale, 8);
      }
    }

    // Up
    if (block_available[3])
    {
      *prednum += add_predictor (&point[*prednum].motion, col_mv[o_block_y - 1][o_block_x], mvScale, 8);
    }

    // Up - Right
    if (block_available[0])
    {
      *prednum += add_predictor (&point[*prednum].motion, col_mv[o_block_y][o_block_x + blockshape_x], mvScale, 8);
      if (block_available[3])
      {
        *prednum += add_predictor (&point[*prednum].motion, col_mv[o_block_y - 1][o_block_x + blockshape_x], mvScale, 8);
      }

      if (block_available[1])
      {
        *prednum += add_predictor (&point[*prednum].motion, col_mv[o_block_y + blockshape_y][o_block_x + blockshape_x], mvScale, 8);
      }
    }

    if (block_available[1])
    {
      *prednum += add_predictor (&point[*prednum].motion, col_mv[o_block_y + blockshape_y][o_block_x], mvScale, 8);
    }
  }
}

/*!
************************************************************************
* \brief
*    EPZS Block Type Predictors
************************************************************************
*/
void
EPZSBlockTypePredictors (Slice * currSlice, MEBlock *mv_block, SPoint * point, int *prednum)
{
  int blocktype = mv_block->blocktype;
  int block_x   = mv_block->block_x;
  int block_y   = mv_block->block_y;
  int list      = mv_block->list;
  int ref       = mv_block->ref_idx; 
  short *****all_mv = currSlice->all_mv[list];
  short *mv = all_mv[ref][BLOCK_PARENT[blocktype]][block_y][block_x];
  MotionVector *cur_mv = &point[*prednum].motion;

  cur_mv->mv_x = mv[0];
  cur_mv->mv_y = mv[1];

  //*prednum += ((cur_mv->mv_x | cur_mv->mv_y) != 0);
  *prednum += (*((int *) cur_mv) != 0);

  if ((ref > 0) && (currSlice->structure != FRAME))
  {
    EPZSParameters *p_EPZS = currSlice->p_EPZS;
    cur_mv = &point[*prednum].motion;
    scale_mv (cur_mv, p_EPZS->mv_scale[list][ref][ref - 1], all_mv[ref - 1][blocktype][block_y][block_x], 8);

    //*prednum += ((cur_mv->mv_x | cur_mv->mv_y) != 0);
    *prednum += (*((int *) cur_mv) != 0);
    if (ref > 1)
    {
      cur_mv = &point[*prednum].motion;
      scale_mv (cur_mv, p_EPZS->mv_scale[list][ref][0], all_mv[0][blocktype][block_y][block_x], 8);
      //*prednum += ((cur_mv->mv_x | cur_mv->mv_y) != 0);
      *prednum += (*((int *) cur_mv) != 0);
    }
  }

  cur_mv = &point[*prednum].motion;
  mv = all_mv[ref][1][block_y][block_x];
  cur_mv->mv_x = mv[0];
  cur_mv->mv_y = mv[1];
  //*prednum += ((cur_mv->mv_x | cur_mv->mv_y) != 0);
  *prednum += (*((int *) cur_mv) != 0);
}

/*!
************************************************************************
* \brief
*    EPZS Window Based Predictors
************************************************************************
*/
void
EPZSWindowPredictors (MotionVector * mv, EPZSStructure * predictor, int *prednum, EPZSStructure * windowPred)
{
  int pos;
  SPoint *wPoint = &windowPred->point[0];
  SPoint *pPoint = &predictor->point[(*prednum)];

  for (pos = 0; pos < windowPred->searchPoints; ++pos)
  {
    (pPoint++)->motion = add_MVs ((wPoint++)->motion, mv);
  }
  *prednum += windowPred->searchPoints;
}

/*!
***********************************************************************
* \brief
*    Spatial Predictors
*    AMT/HYC
***********************************************************************
*/
void
EPZSSpatialMemPredictors (EPZSParameters * p_EPZS,  //!< EPZS Parameters
                          MEBlock * mv_block, //!< Motion estimation structure
                          int list, //!< Current list
                          int *prednum, //!< predictor position
                          int img_width)  //!< image width
{

  SPoint *point = p_EPZS->predictor->point;

  int blocktype = mv_block->blocktype - 1;

  int by = mv_block->block_y;

  int bs_x = (mv_block->blocksize_x >> 2);  // horizontal block size in 4-pel units
  int bs_y = (mv_block->blocksize_y >> 2);  // vertical block size in 4-pel units
  int pic_x = mv_block->pos_x2;

  int ref = mv_block->ref_idx;

#if EPZSREF
  MotionVector **prd_mv = p_EPZS->p_motion[list][ref][blocktype];
  MotionVector *cur_mv = &point[*prednum].motion;

  // Left Predictor
  if (pic_x > 0)
  {
    *cur_mv = prd_mv[by][pic_x - bs_x];
    *prednum += (*((int *) cur_mv) != 0);
    cur_mv = &point[*prednum].motion;
  }

  by = (by > 0) ? by - bs_y : 4 - bs_y;

  // Up predictor
  *cur_mv = prd_mv[by][pic_x];
  *prednum += (*((int *) cur_mv) != 0);

  // Up-Right predictor
  if (pic_x + bs_x < img_width)
  {
    cur_mv = &point[*prednum].motion;
    *cur_mv = prd_mv[by][pic_x + bs_x];
    *prednum += (*((int *) cur_mv) != 0);
  }
#else
  int mot_scale = p_EPZS->mv_scale[list][ref][0];

  MotionVector **prd_mv = p_EPZS->p_motion[list][blocktype];

  // Left Predictor
  point[*prednum].motion.mv_x = (pic_x > 0)
    ? (short) rshift_rnd_sf ((mot_scale * prd_mv[by][pic_x - bs_x].mv_x), 8)
    : 0;
  point[*prednum].motion.mv_y = (pic_x > 0)
    ? (short) rshift_rnd_sf ((mot_scale * prd_mv[by][pic_x - bs_x].mv_y), 8)
    : 0;
  *prednum += ((point[*prednum].motion.mv_x != 0) || (point[*prednum].motion.mv_y != 0));

  // Up predictor
  point[*prednum].motion.mv_x = (by > 0)
    ? (short) rshift_rnd_sf ((mot_scale * prd_mv[by - bs_y][pic_x].mv_x), 8)
    : (short) rshift_rnd_sf ((mot_scale * prd_mv[4 - bs_y][pic_x].mv_x), 8);
  point[*prednum].motion.mv_y = (by > 0)
    ? (short) rshift_rnd_sf ((mot_scale * prd_mv[by - bs_y][pic_x].mv_y), 8)
    : (short) rshift_rnd_sf ((mot_scale * prd_mv[4 - bs_y][pic_x].mv_y), 8);
  *prednum += ((point[*prednum].motion.mv_x != 0) || (point[*prednum].motion.mv_y != 0));

  // Up-Right predictor
  point[*prednum].motion.mv_x = (pic_x + bs_x < img_width)
    ? (by > 0)
    ? (short) rshift_rnd_sf ((mot_scale * prd_mv[by - bs_y][pic_x + bs_x].mv_x), 8)
    : (short) rshift_rnd_sf ((mot_scale * prd_mv[4 - bs_y][pic_x + bs_x].mv_x), 8)
    : 0;
  point[*prednum].motion.mv_y = (pic_x + bs_x < img_width)
    ? (by > 0)
    ? (short) rshift_rnd_sf ((mot_scale * prd_mv[by - bs_y][pic_x + bs_x].mv_y), 8)
    : (short) rshift_rnd_sf ((mot_scale * prd_mv[4 - bs_y][pic_x + bs_x].mv_y), 8)
    : 0;
  *prednum += ((point[*prednum].motion.mv_x != 0) || (point[*prednum].motion.mv_y != 0));
#endif
}

/*!
*************************************************************************************
* \brief
*    Determine stop criterion for EPZS
*************************************************************************************
*/
int
EPZSDetermineStopCriterion (EPZSParameters * p_EPZS, int *prevSad, MEBlock * mv_block, int lambda_dist)
{
  int blocktype = mv_block->blocktype;
  int blockshape_x = (mv_block->blocksize_x >> 2);
  PixelPos *block = mv_block->block;
  int sadA, sadB, sadC, stopCriterion;
  sadA = block[0].available ? prevSad[-blockshape_x] : INT_MAX;
  sadB = block[1].available ? prevSad[0] : INT_MAX;
  sadC = block[2].available ? prevSad[blockshape_x] : INT_MAX;

  stopCriterion = imin (sadA, imin (sadB, sadC));
  stopCriterion = imax (stopCriterion, p_EPZS->minthres[blocktype]);
  stopCriterion = imin (stopCriterion, p_EPZS->maxthres[blocktype] + lambda_dist);
  stopCriterion = (9 * imax (p_EPZS->medthres[blocktype] + lambda_dist, stopCriterion) + 2 * p_EPZS->medthres[blocktype]) >> 3;

  return stopCriterion + lambda_dist;
}

/*!
***********************************************************************
* \brief
*    Report function for EPZS Fast ME
*    AMT/HYC
***********************************************************************
*/
void
EPZSOutputStats (InputParameters * p_Inp, FILE * stat, short stats_file)
{
  if (stats_file == 1)
  {
    fprintf (stat, " EPZS Pattern                 : %s\n", EPZS_PATTERN[p_Inp->EPZSPattern]);
    fprintf (stat, " EPZS Dual Pattern            : %s\n", EPZS_DUAL_PATTERN[p_Inp->EPZSDual]);
    fprintf (stat, " EPZS Fixed Predictors        : %s\n", EPZS_FIXED_PREDICTORS[p_Inp->EPZSFixed]);
    fprintf (stat, " EPZS Temporal Predictors     : %s\n", EPZS_OTHER_PREDICTORS[p_Inp->EPZSTemporal]);
    fprintf (stat, " EPZS Spatial Predictors      : %s\n", EPZS_OTHER_PREDICTORS[p_Inp->EPZSSpatialMem]);
    fprintf (stat, " EPZS Threshold Multipliers   : (%d %d %d)\n", p_Inp->EPZSMedThresScale, p_Inp->EPZSMinThresScale, p_Inp->EPZSMaxThresScale);
    fprintf (stat, " EPZS Subpel ME               : %s\n", EPZS_OTHER_PREDICTORS[p_Inp->EPZSSubPelME]);
    fprintf (stat, " EPZS Subpel ME BiPred        : %s\n", EPZS_OTHER_PREDICTORS[p_Inp->EPZSSubPelMEBiPred]);
  }
  else
  {
    fprintf (stat, " EPZS Pattern                      : %s\n", EPZS_PATTERN[p_Inp->EPZSPattern]);
    fprintf (stat, " EPZS Dual Pattern                 : %s\n", EPZS_DUAL_PATTERN[p_Inp->EPZSDual]);
    fprintf (stat, " EPZS Fixed Predictors             : %s\n", EPZS_FIXED_PREDICTORS[p_Inp->EPZSFixed]);
    fprintf (stat, " EPZS Temporal Predictors          : %s\n", EPZS_OTHER_PREDICTORS[p_Inp->EPZSTemporal]);
    fprintf (stat, " EPZS Spatial Predictors           : %s\n", EPZS_OTHER_PREDICTORS[p_Inp->EPZSSpatialMem]);
    fprintf (stat, " EPZS Threshold Multipliers        : (%d %d %d)\n", p_Inp->EPZSMedThresScale, p_Inp->EPZSMinThresScale, p_Inp->EPZSMaxThresScale);
    fprintf (stat, " EPZS Subpel ME                    : %s\n", EPZS_OTHER_PREDICTORS[p_Inp->EPZSSubPelME]);
    fprintf (stat, " EPZS Subpel ME BiPred             : %s\n", EPZS_OTHER_PREDICTORS[p_Inp->EPZSSubPelMEBiPred]);
  }
}
