
/*!
 *************************************************************************************
 * \file context_ini.h
 *
 * \brief
 *    CABAC context initializations
 *
 * \author
 *    Main contributors (see contributors.h for copyright, address and affiliation details)
 *    - Detlev Marpe                    <marpe@hhi.de>
 *    - Heiko Schwarz                   <hschwarz@hhi.de>
 **************************************************************************************
 */

#ifndef _CONTEXT_INI_
#define _CONTEXT_INI_

extern void  create_context_memory       (ImageParameters *p_Img, InputParameters *p_Inp);
extern void  free_context_memory         (ImageParameters *p_Img);
extern void  update_field_frame_contexts (ImageParameters *p_Img, int);
extern void  SetCtxModelNumber           (Slice *currSlice);
extern void  init_contexts               (Slice *currSlice);
extern void  store_contexts              (Slice *currSlice);

#endif

