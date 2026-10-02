/*
 * This file is part of the BSGS distribution (https://github.com/JeanLucPons/BSGS).
 * Copyright (c) 2020 Jean Luc PONS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "IntGroup.h"

using namespace std;

IntGroup::IntGroup(int size) {
  this->size = size;
  subp = (Int *)malloc(size * sizeof(Int));
}

IntGroup::~IntGroup() {
  free(subp);
}

void IntGroup::Set(Int *pts) {
  ints = pts;
}

// Compute modular inversion of the whole group
/* Montgomery batch inversion. The prefix product pass and the back
   substitution pass are chains of dependent multiplications, so the group is
   split into CHAINS segments whose chains are interleaved: the CPU overlaps
   them. The CHAINS segment totals are then inverted together with one modular
   inversion (product tree) instead of one inversion per segment. */
#define CHAINS 4

void IntGroup::ModInv() {

  if (size < 2 * CHAINS) {
    // small group: plain single chain
    Int newValue;
    Int inverse;
    subp[0].Set(&ints[0]);
    for (int i = 1; i < size; i++) {
      subp[i].ModMulK1(&subp[i - 1], &ints[i]);
    }
    inverse.Set(&subp[size - 1]);
    inverse.ModInv();
    for (int i = size - 1; i > 0; i--) {
      newValue.ModMulK1(&subp[i - 1], &inverse);
      inverse.ModMulK1(&ints[i]);
      ints[i].Set(&newValue);
    }
    ints[0].Set(&inverse);
    return;
  }

  int start[CHAINS], end[CHAINS], len = 0;
  for (int k = 0; k < CHAINS; k++) {
    start[k] = (int)((int64_t)size * k / CHAINS);
    end[k] = (int)((int64_t)size * (k + 1) / CHAINS);
    if (end[k] - start[k] > len) len = end[k] - start[k];
  }

  // prefix products, CHAINS independent chains interleaved
  for (int k = 0; k < CHAINS; k++) subp[start[k]].Set(&ints[start[k]]);
  for (int j = 1; j < len; j++) {
    for (int k = 0; k < CHAINS; k++) {
      int i = start[k] + j;
      if (i < end[k]) subp[i].ModMulK1(&subp[i - 1], &ints[i]);
    }
  }

  // one inversion of the product of the chain totals, then the inverse of
  // each total: inv(T_k) = inv(T_0 ... T_3) * prod of the other totals
  Int t01, t23, all, inv, inv01, inv23;
  Int inverse[CHAINS];
  t01.ModMulK1(&subp[end[0] - 1], &subp[end[1] - 1]);
  t23.ModMulK1(&subp[end[2] - 1], &subp[end[3] - 1]);
  all.ModMulK1(&t01, &t23);
  inv.Set(&all);
  inv.ModInv();
  inv01.ModMulK1(&inv, &t23);
  inv23.ModMulK1(&inv, &t01);
  inverse[0].ModMulK1(&inv01, &subp[end[1] - 1]);
  inverse[1].ModMulK1(&inv01, &subp[end[0] - 1]);
  inverse[2].ModMulK1(&inv23, &subp[end[3] - 1]);
  inverse[3].ModMulK1(&inv23, &subp[end[2] - 1]);

  // back substitution, interleaved
  Int newValue;
  for (int j = len - 1; j > 0; j--) {
    for (int k = 0; k < CHAINS; k++) {
      int i = start[k] + j;
      if (i < end[k]) {
        newValue.ModMulK1(&subp[i - 1], &inverse[k]);
        inverse[k].ModMulK1(&ints[i]);
        ints[i].Set(&newValue);
      }
    }
  }
  for (int k = 0; k < CHAINS; k++) ints[start[k]].Set(&inverse[k]);

}
