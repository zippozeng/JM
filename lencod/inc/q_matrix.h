
/*!
 ***************************************************************************
 * \file
 *    q_matrix.h
 *
 * \brief
 *    Headerfile for q_matrix array
 *
 * \date
 *    07. Apr 2004
 ***************************************************************************
 */

#ifndef _Q_MATRIX_H_
#define _Q_MATRIX_H_

struct scaling_list {
  short ScalingList4x4input[6][16];
  short ScalingList8x8input[6][64];
  short ScalingList4x4[6][16];
  short ScalingList8x8[6][64];

  short UseDefaultScalingMatrix4x4Flag[6];
  short UseDefaultScalingMatrix8x8Flag[6];
};

extern void Init_QMatrix (ImageParameters *p_Img, InputParameters *p_Inp);
extern void CalculateQuant4x4Param (ImageParameters *p_Img);
extern void CalculateQuant8x8Param (ImageParameters *p_Img);
extern void free_QMatrix(QuantParameters *p_Quant);

#endif
