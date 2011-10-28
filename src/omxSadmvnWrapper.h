/*
 *  Copyright 2007-2011 The OpenMx Project
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */
 
#ifndef _OMXSADMVNWRAPPER_H
#define _OMXSADMVNWRAPPER_H

#ifdef _OPENMP

#include <omp.h>
extern omp_lock_t sadmvn_lock;

#else  // (!defined _OPENMP)

extern void* sadmvn_lock;

#endif // _OPENMP



void omxSadmvnWrapper(omxObjective *oo, omxMatrix *cov, omxMatrix *ordCov, 
	double *corList, double *lThresh, double *uThresh, int *Infin, double *likelihood, int *inform);


#endif 
