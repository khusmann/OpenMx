/*
 *  Copyright 2007-2013 The OpenMx Project
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
 */

#include <stdio.h>
#include <sys/types.h>
#include <errno.h>

#include <R.h>
#include <Rinternals.h>
#include <Rdefines.h>
#include <R_ext/Rdynload.h>
#include <R_ext/BLAS.h>
#include <R_ext/Lapack.h>

#include "omxDefines.h"
#include "types.h"
#include "npsolWrap.h"
#include "omxOpenmpWrap.h"
#include "omxState.h"
#include "omxGlobalState.h"
#include "omxMatrix.h"
#include "omxAlgebra.h"
#include "omxFitFunction.h"
#include "omxExpectation.h"
#include "omxNPSOLSpecific.h"
#include "omxImportFrontendState.h"
#include "omxExportBackendState.h"
#include "omxOptimizer.h"
#include "omxHessianCalculation.h"
#include "Compute.h"

omp_lock_t GlobalRLock;

static R_CallMethodDef callMethods[] = {
	{"omxBackend", (DL_FUNC) omxBackend, 12},
	{"omxCallAlgebra", (DL_FUNC) omxCallAlgebra, 3},
	{"findIdenticalRowsData", (DL_FUNC) findIdenticalRowsData, 5},
	{NULL, NULL, 0}
};

#ifdef  __cplusplus
extern "C" {
#endif

void R_init_OpenMx(DllInfo *info) {
	R_registerRoutines(info, NULL, callMethods, NULL, NULL);

	omx_omp_init_lock(&GlobalRLock);

	// There is no code that will change behavior whether openmp
	// is set for nested or not. I'm just keeping this in case it
	// makes a difference with older versions of openmp. 2012-12-24 JNP
#if defined(_OPENMP) && _OPENMP <= 200505
	omp_set_nested(0);
#endif
}

void R_unload_OpenMx(DllInfo *info) {
	omx_omp_destroy_lock(&GlobalRLock);
}

#ifdef  __cplusplus
}
#endif

void string_to_try_error( const std::string& str )
{
	error("%s", str.c_str());
}

void exception_to_try_error( const std::exception& ex )
{
	string_to_try_error(ex.what());
}

SEXP asR(MxRList *out)
{
       SEXP names, ans;
       int len = out->size();
       PROTECT(names = allocVector(STRSXP, len));
       PROTECT(ans = allocVector(VECSXP, len));
       for (int lx=0; lx < len; ++lx) {
               SET_STRING_ELT(names, lx, (*out)[lx].first);
               SET_VECTOR_ELT(ans,   lx, (*out)[lx].second);
       }
       namesgets(ans, names);
       return ans;
}

/* Main functions */
SEXP omxCallAlgebra2(SEXP matList, SEXP algNum, SEXP options) {

	omxManageProtectInsanity protectManager;

	if(OMX_DEBUG) { Rprintf("-----------------------------------------------------------------------\n");}
	if(OMX_DEBUG) { Rprintf("Explicit call to algebra %d.\n", INTEGER(algNum));}

	int j,k,l;
	omxMatrix* algebra;
	int algebraNum = INTEGER(algNum)[0];
	SEXP ans, nextMat;
	char output[MAX_STRING_LEN];

	/* Create new omxState for current state storage and initialize it. */
	
	globalState = new omxState;
	omxInitState(globalState, NULL);
	if(OMX_DEBUG) { Rprintf("Created state object at 0x%x.\n", globalState);}

	/* Retrieve All Matrices From the MatList */

	if(OMX_DEBUG) { Rprintf("Processing %d matrix(ces).\n", length(matList));}

	omxMatrix *args[length(matList)];
	for(k = 0; k < length(matList); k++) {
		PROTECT(nextMat = VECTOR_ELT(matList, k));	// This is the matrix + populations
		args[k] = omxNewMatrixFromRPrimitive(nextMat, globalState, 1, - k - 1);
		globalState->matrixList.push_back(args[k]);
		if(OMX_DEBUG) {
			Rprintf("Matrix initialized at 0x%0xd = (%d x %d).\n",
				globalState->matrixList[k], globalState->matrixList[k]->rows, globalState->matrixList[k]->cols);
		}
	}

	algebra = omxNewAlgebraFromOperatorAndArgs(algebraNum, args, length(matList), globalState);

	if(algebra==NULL) {
		error(globalState->statusMsg);
	}

	if(OMX_DEBUG) {Rprintf("Completed Algebras and Matrices.  Beginning Initial Compute.\n");}
	omxStateNextEvaluation(globalState);

	omxRecompute(algebra);

	PROTECT(ans = allocMatrix(REALSXP, algebra->rows, algebra->cols));
	for(l = 0; l < algebra->rows; l++)
		for(j = 0; j < algebra->cols; j++)
			REAL(ans)[j * algebra->rows + l] =
				omxMatrixElement(algebra, l, j);

	if(OMX_DEBUG) { Rprintf("All Algebras complete.\n"); }

	output[0] = 0;
	if (isErrorRaised(globalState)) {
		strncpy(output, globalState->statusMsg, MAX_STRING_LEN);
	}

	omxFreeAllMatrixData(algebra);
	omxFreeState(globalState);

	if(output[0]) error(output);

	return ans;
}

SEXP omxCallAlgebra(SEXP matList, SEXP algNum, SEXP options)
{
	try {
		return omxCallAlgebra2(matList, algNum, options);
	} catch( std::exception& __ex__ ) {
		exception_to_try_error( __ex__ );
	} catch(...) {
		string_to_try_error( "c++ exception (unknown reason)" );
	}
}

SEXP omxBackend2(SEXP fitfunction, SEXP startVals, SEXP constraints,
	SEXP matList, SEXP varList, SEXP algList, SEXP expectList,
	SEXP data, SEXP intervalList, SEXP checkpointList, SEXP options, SEXP state) {

	/* Helpful variables */

	SEXP nextLoc;

	int disableOptimizer = 0;
	int analyticGradients = 0;

	/* Sanity Check and Parse Inputs */
	/* TODO: Need to find a way to account for nullness in these.  For now, all checking is done on the front-end. */
//	if(!isVector(startVals)) error ("startVals must be a vector");
//	if(!isVector(matList)) error ("matList must be a list");
//	if(!isVector(algList)) error ("algList must be a list");

	omxManageProtectInsanity protectManager;

	/* Create new omxState for current state storage and initialize it. */
	globalState = new omxState;
	omxInitState(globalState, NULL);

	/* 	Set NPSOL options */
	omxSetNPSOLOpts(options, &globalState->numHessians, &globalState->calculateStdErrors, 
		&globalState->ciMaxIterations, &disableOptimizer, &globalState->numThreads, 
		&analyticGradients, length(startVals));

	globalState->numFreeParams = length(startVals);
	globalState->analyticGradients = analyticGradients;
	if(OMX_DEBUG) { Rprintf("Created state object at 0x%x.\n", globalState);}

	/* Retrieve Data Objects */
	omxProcessMxDataEntities(data);
	if (globalState->statusMsg[0]) error(globalState->statusMsg);
    
	/* Retrieve All Matrices From the MatList */
	omxProcessMxMatrixEntities(matList);
	if (globalState->statusMsg[0]) error(globalState->statusMsg);

	if (length(startVals) != length(varList)) error("varList and startVals must be the same length");

	/* Process Free Var List */
	omxProcessFreeVarList(varList);
	if (globalState->statusMsg[0]) error(globalState->statusMsg);

	omxProcessMxExpectationEntities(expectList);
	if (globalState->statusMsg[0]) error(globalState->statusMsg);

	omxProcessMxAlgebraEntities(algList);
	if (globalState->statusMsg[0]) error(globalState->statusMsg);

	omxCompleteMxExpectationEntities();
	if (globalState->statusMsg[0]) error(globalState->statusMsg);

	omxProcessMxFitFunction(algList);
	if (globalState->statusMsg[0]) error(globalState->statusMsg);

	// This is the chance to check for matrix
	// conformability, etc.  Any errors encountered should
	// be reported using R's error() function, not
	// omxRaiseErrorf.

	omxInitialMatrixAlgebraCompute();
	omxResetStatus(globalState);

	// maybe require a Compute object? TODO
	omxComputeGD *topCompute = NULL;
	omxMatrix *fitMatrix = NULL;
	if(!isNull(fitfunction)) {
		if(OMX_DEBUG) { Rprintf("Processing fit function.\n"); }
		fitMatrix = omxMatrixLookupFromState1(fitfunction, globalState);
		topCompute = new omxComputeGD(fitMatrix);
	}
	if (globalState->statusMsg[0]) error(globalState->statusMsg);
	
	/* Process Matrix and Algebra Population Function */
	/*
	  Each matrix is a list containing a matrix and the other matrices/algebras that are
	  populated into it at each iteration.  The first element is already processed, above.
	  The rest of the list will be processed here.
	*/
	for(int j = 0; j < length(matList); j++) {
		PROTECT(nextLoc = VECTOR_ELT(matList, j));		// This is the matrix + populations
		omxProcessMatrixPopulationList(globalState->matrixList[j], nextLoc);
	}

	/* Processing Constraints */
	omxProcessConstraints(constraints);

	/* Process Confidence Interval List */
	omxProcessConfidenceIntervals(intervalList);

	/* Process Checkpoint List */
	omxProcessCheckpointOptions(checkpointList);

	// Probably, this is always the same for all children and
	// doesn't need to be copied to child states.
	cacheFreeVarDependencies(globalState);

	int n = globalState->numFreeParams;

	if (topCompute) topCompute->setStartValues(startVals);
	
	if (topCompute) topCompute->compute(disableOptimizer);

	SEXP evaluations, algebras, matrices, expectations;

	PROTECT(evaluations = NEW_NUMERIC(2));
	PROTECT(matrices = NEW_LIST(globalState->matrixList.size()));
	PROTECT(algebras = NEW_LIST(globalState->algebraList.size()));
	PROTECT(expectations = NEW_LIST(globalState->numExpects));

	REAL(evaluations)[0] = globalState->computeCount;

	MxRList result;

	if (!isErrorRaised(globalState) && globalState->numHessians && fitMatrix != NULL &&
	    globalState->numConstraints == 0) {
		omxComputeEstimateHessian *eh =
			new omxComputeEstimateHessian(fitMatrix, topCompute->getEstimate());
		eh->compute(FALSE);
		eh->reportResults(&result);
		delete eh;
	}

	// What if fitfunction has its own repopulateFun? TODO
	if (topCompute) handleFreeVarListHelper(globalState, topCompute->getEstimate(), n);

	omxFinalAlgebraCalculation(globalState, matrices, algebras, expectations); 

	REAL(evaluations)[1] = globalState->computeCount;

	if (topCompute) {
		topCompute->reportResults(&result);
		delete topCompute;
	}

	MxRList backwardCompatStatus;
	backwardCompatStatus.push_back(std::make_pair(mkChar("code"), NA_STRING));
	backwardCompatStatus.push_back(std::make_pair(mkChar("status"),
						      ScalarInteger(-isErrorRaised(globalState))));

	if (isErrorRaised(globalState)) {
		SEXP msg;
		PROTECT(msg = allocVector(STRSXP, 1));
		SET_STRING_ELT(msg, 0, mkChar(globalState->statusMsg));
		result.push_back(std::make_pair(mkChar("error"), msg));
		backwardCompatStatus.push_back(std::make_pair(mkChar("statusMsg"), msg));
	}

	result.push_back(std::make_pair(mkChar("status"), asR(&backwardCompatStatus)));
	result.push_back(std::make_pair(mkChar("evaluations"), evaluations));
	result.push_back(std::make_pair(mkChar("matrices"), matrices));
	result.push_back(std::make_pair(mkChar("algebras"), algebras));
	result.push_back(std::make_pair(mkChar("expectations"), expectations));

	/* Free data memory */
	omxFreeState(globalState);

	return asR(&result);

}

SEXP omxBackend(SEXP fitfunction, SEXP startVals, SEXP constraints,
	SEXP matList, SEXP varList, SEXP algList, SEXP expectList,
	SEXP data, SEXP intervalList, SEXP checkpointList, SEXP options, SEXP state)
{
	try {
		return omxBackend2(fitfunction, startVals, constraints,
				   matList, varList, algList, expectList,
				   data, intervalList, checkpointList, options, state);
	} catch( std::exception& __ex__ ) {
		exception_to_try_error( __ex__ );
	} catch(...) {
		string_to_try_error( "c++ exception (unknown reason)" );
	}
}

