#include <cblas.h>
#ifdef LAPACKE_FOLDER
#include <lapacke/lapacke.h>
#else
#include <lapacke.h>
#endif

#include "math.h"
#include "minimal_opencv.h"
#include "stdbool.h"
#include "stdio.h"
#include "string.h"

#include <lapacke_utils.h>
#include <limits.h>
#include <stdarg.h>

#include "linmath.h"

#ifdef _WIN32
#define SURVIVE_LOCAL_ONLY
#include <malloc.h>
#define alloca _alloca
#else
#define SURVIVE_LOCAL_ONLY __attribute__((visibility("hidden")))
#endif

SURVIVE_LOCAL_ONLY int cvRound(float f) { return roundf(f); }
#define CV_Error(code, msg) assert(0 && msg); // cv::error( code, msg, CV_Func, __FILE__, __LINE__ )

const int DECOMP_SVD = 1;
const int DECOMP_LU = 2;

void print_mat(const CvMat *M);

static size_t mat_size_bytes(const CvMat *mat) { return (size_t)CV_ELEM_SIZE(mat->type) * mat->cols * mat->rows; }

#ifdef USE_FLOAT
#define cblas_gemm cblas_sgemm
#define cblas_symm cblas_ssymm
#define LAPACKE_getrs LAPACKE_sgetrs
#define LAPACKE_getrf LAPACKE_sgetrf
#define LAPACKE_getri LAPACKE_sgetri
#define LAPACKE_gelss LAPACKE_sgelss
#define LAPACKE_gesvd LAPACKE_sgesvd
#define LAPACKE_getri_work LAPACKE_sgetri_work
#define LAPACKE_ge_trans LAPACKE_sge_trans
#else
#define cblas_gemm cblas_dgemm
#define cblas_symm cblas_dsymm
#define LAPACKE_getrs LAPACKE_dgetrs
#define LAPACKE_getrf LAPACKE_dgetrf
#define LAPACKE_getri LAPACKE_dgetri
#define LAPACKE_gelss LAPACKE_dgelss
#define LAPACKE_gesvd LAPACKE_dgesvd
#define LAPACKE_getri_work LAPACKE_dgetri_work
#define LAPACKE_ge_trans LAPACKE_dge_trans
#endif

// dst = alpha * src1 * src2 + beta * src3 or dst = alpha * src2 * src1 + beta * src3 where src1 is symm
SURVIVE_LOCAL_ONLY void cvSYMM(const CvMat *src1, const CvMat *src2, double alpha, const CvMat *src3, double beta,
							   CvMat *dst, bool src1First) {

	int rows1 = src1->rows;
	int cols1 = src1->cols;

	int rows2 = src2->rows;
	int cols2 = src2->cols;

	if (src3) {
		int rows3 = src3->rows;
		int cols3 = src3->cols;
		assert(rows3 == dst->rows);
		assert(cols3 == dst->cols);
	}

	// assert(src3 == 0 || beta != 0);
	assert(cols1 == rows2);
	assert(rows1 == dst->rows);
	assert(cols2 == dst->cols);

	lapack_int lda = src1->cols;
	lapack_int ldb = src2->cols;

	if (src3)
		cvCopy(src3, dst, 0);
	else
		beta = 0;

	assert(CV_RAW_PTR(dst) != CV_RAW_PTR(src1));
	assert(CV_RAW_PTR(dst) != CV_RAW_PTR(src2));
	/*
		void cblas_dsymm(OPENBLAS_CONST enum CBLAS_ORDER Order,
						 OPENBLAS_CONST enum CBLAS_SIDE Side,
						 OPENBLAS_CONST enum CBLAS_UPLO Uplo,
						 OPENBLAS_CONST blasint M,
						 OPENBLAS_CONST blasint N,
						 OPENBLAS_CONST double alpha,
						 OPENBLAS_CONST double *A,
						 OPENBLAS_CONST blasint lda,
						 OPENBLAS_CONST double *B,
						 OPENBLAS_CONST blasint ldb,
						 OPENBLAS_CONST double beta,
						 double *C,
						 OPENBLAS_CONST blasint ldc);
	*/
	cblas_symm(CblasRowMajor, src1First ? CblasLeft : CblasRight, CblasUpper, dst->rows, dst->cols, alpha,
			   CV_RAW_PTR(src1), lda, CV_RAW_PTR(src2), ldb, beta, CV_RAW_PTR(dst), dst->cols);
}

// Special case dst = alpha * src2 * src1 * src2' + beta * src3
void mulBABt(const CvMat *src1, const CvMat *src2, double alpha, const CvMat *src3, double beta, CvMat *dst) {
	size_t dims = src2->rows;
	assert(src2->cols == src2->rows);
	CREATE_STACK_MAT(tmp, dims, dims);

	// This has been profiled; and weirdly enough the SYMM version is slower for a 19x19 matrix. Guessing access order
	// or some other cache thing matters more than the additional 2x multiplications.
//#define USE_SYM
#ifdef USE_SYM
	cvSYMM(src1, src2, 1, 0, 0, &tmp, false);
	cvGEMM(&tmp, src2, alpha, src3, beta, dst, CV_GEMM_B_T);
#else
	cvGEMM(src1, src2, 1, 0, 0, &tmp, CV_GEMM_B_T);
	cvGEMM(src2, &tmp, alpha, src3, beta, dst, 0);
#endif
}
// dst = alpha * src1 * src2 + beta * src3
SURVIVE_LOCAL_ONLY void cvGEMM(const CvMat *src1, const CvMat *src2, double alpha, const CvMat *src3, double beta,
							   CvMat *dst, int tABC) {

	int rows1 = (tABC & CV_GEMM_A_T) ? src1->cols : src1->rows;
	int cols1 = (tABC & CV_GEMM_A_T) ? src1->rows : src1->cols;

	int rows2 = (tABC & CV_GEMM_B_T) ? src2->cols : src2->rows;
	int cols2 = (tABC & CV_GEMM_B_T) ? src2->rows : src2->cols;

	if (src3) {
		int rows3 = (tABC & CV_GEMM_C_T) ? src3->cols : src3->rows;
		int cols3 = (tABC & CV_GEMM_C_T) ? src3->rows : src3->cols;
		assert(rows3 == dst->rows);
		assert(cols3 == dst->cols);
	}

	// assert(src3 == 0 || beta != 0);
	assert(cols1 == rows2);
	assert(rows1 == dst->rows);
	assert(cols2 == dst->cols);

	lapack_int lda = src1->cols;
	lapack_int ldb = src2->cols;

	if (src3)
		cvCopy(src3, dst, 0);
	else
		beta = 0;

	assert(CV_RAW_PTR(dst) != CV_RAW_PTR(src1));
	assert(CV_RAW_PTR(dst) != CV_RAW_PTR(src2));

	cblas_gemm(CblasRowMajor, (tABC & CV_GEMM_A_T) ? CblasTrans : CblasNoTrans,
			   (tABC & CV_GEMM_B_T) ? CblasTrans : CblasNoTrans, dst->rows, dst->cols, cols1, alpha, CV_RAW_PTR(src1),
			   lda, CV_RAW_PTR(src2), ldb, beta, CV_RAW_PTR(dst), dst->cols);
}

// dst = scale * src ^ t * src     iff order == 1
// dst = scale *     src * src ^ t iff order == 0
SURVIVE_LOCAL_ONLY void cvMulTransposed(const CvMat *src, CvMat *dst, int order, const CvMat *delta, double scale) {
	lapack_int rows = src->rows;
	lapack_int cols = src->cols;

	lapack_int drows = order == 0 ? dst->rows : dst->cols;
	assert(drows == dst->cols);
	assert(order == 1 ? (dst->cols == src->cols) : (dst->cols == src->rows));
	assert(delta == 0 && "This isn't implemented yet");
	double beta = 0;

	bool isAT = order == 1;
	bool isBT = !isAT;

	lapack_int dstCols = dst->cols;

	cblas_gemm(CblasRowMajor, isAT ? CblasTrans : CblasNoTrans, isBT ? CblasTrans : CblasNoTrans, dst->rows, dst->cols,
			   order == 1 ? src->rows : src->cols, scale, CV_RAW_PTR(src), src->cols, CV_RAW_PTR(src), src->cols, beta,
			   CV_RAW_PTR(dst), dstCols);
}

/* IEEE754 constants and macros */
#define CV_TOGGLE_FLT(x) ((x) ^ ((int)(x) < 0 ? 0x7fffffff : 0))
#define CV_TOGGLE_DBL(x) ((x) ^ ((int64)(x) < 0 ? CV_BIG_INT(0x7fffffffffffffff) : 0))

#define CV_DbgAssert assert

#define CV_CREATE_MAT_HEADER_ALLOCA(stack_mat, rows, cols, type)                                                       \
	CvMat *stack_mat = cvInitMatHeader(alloca(sizeof(CvMat)), rows, cols, type);

#define CV_CREATE_MAT_ALLOCA(stack_mat, height, width, type)                                                           \
	CV_CREATE_MAT_HEADER_ALLOCA(stack_mat, height, width, type);                                                       \
	stack_mat->data.ptr = alloca(mat_size_bytes(stack_mat));

#define CREATE_CV_STACK_MAT(name, rows, cols, type)                                                                    \
	FLT *_##name = alloca(rows * cols * sizeof(FLT));                                                                  \
	CvMat name = cvMat(rows, cols, SURVIVE_CV_F, _##name);

static inline lapack_int LAPACKE_getri_static_alloc(int matrix_layout, lapack_int n, FLT *a, lapack_int lda,
													const lapack_int *ipiv) {
	lapack_int info = 0;
	lapack_int lwork = -1;
	FLT *work = NULL;
	FLT work_query;
	if (matrix_layout != LAPACK_COL_MAJOR && matrix_layout != LAPACK_ROW_MAJOR) {
		LAPACKE_xerbla("LAPACKE_dgetri", -1);
		return -1;
	}
	/* Query optimal working array(s) size */
	info = LAPACKE_getri_work(matrix_layout, n, a, lda, ipiv, &work_query, lwork);
	if (info != 0) {
		goto exit_level_0;
	}
	lwork = (lapack_int)work_query;
	/* Allocate memory for work arrays */
	work = (FLT *)alloca(sizeof(FLT) * lwork);
	if (work == NULL) {
		info = LAPACK_WORK_MEMORY_ERROR;
		goto exit_level_0;
	}
	/* Call middle-level interface */
	info = LAPACKE_getri_work(matrix_layout, n, a, lda, ipiv, work, lwork);
	/* Release memory and exit */

exit_level_0:
	if (info == LAPACK_WORK_MEMORY_ERROR) {
		LAPACKE_xerbla("LAPACKE_dgetri", info);
	}
	return info;
}

SURVIVE_LOCAL_ONLY double cvInvert(const CvMat *srcarr, CvMat *dstarr, int method) {
	lapack_int inf;
	lapack_int rows = srcarr->rows;
	lapack_int cols = srcarr->cols;
	lapack_int lda = srcarr->cols;

	cvCopy(srcarr, dstarr, 0);
	FLT *a = CV_RAW_PTR(dstarr);

#ifdef DEBUG_PRINT
	printf("a: \n");
	print_mat(srcarr);
#endif
	if (method == DECOMP_LU) {
		lapack_int *ipiv = alloca(sizeof(lapack_int) * MIN(srcarr->rows, srcarr->cols));

		lapack_int lda_t = MAX(1, rows);

		FLT *a_t = (FLT *)alloca(sizeof(FLT) * lda_t * MAX(1, cols));
		LAPACKE_ge_trans(LAPACK_ROW_MAJOR, rows, cols, a, lda, a_t, lda_t);

		inf = LAPACKE_getrf(LAPACK_COL_MAJOR, rows, cols, a_t, lda, ipiv);
		assert(inf == 0);

		inf = LAPACKE_getri_static_alloc(LAPACK_COL_MAJOR, rows, a_t, lda, ipiv);
		assert(inf >= 0);

		LAPACKE_ge_trans(LAPACK_COL_MAJOR, rows, cols, a_t, lda, a, lda_t);

		if (inf > 0) {
			printf("Warning: Singular matrix: \n");
			// print_mat(srcarr);
		}

		// free(ipiv);

	} else if (method == DECOMP_SVD) {
		// TODO: There is no way this needs this many allocations,
		// but in my defense I was very tired when I wrote this code
		CREATE_CV_STACK_MAT(w, 1, MIN(dstarr->rows, dstarr->cols), dstarr->type);
		CREATE_CV_STACK_MAT(u, dstarr->cols, dstarr->cols, dstarr->type);
		CREATE_CV_STACK_MAT(v, dstarr->rows, dstarr->rows, dstarr->type);
		CREATE_CV_STACK_MAT(um, w.cols, w.cols, w.type);

		cvSVD(dstarr, &w, &u, &v, 0);

		cvSetZero(&um);
		for (int i = 0; i < w.cols; i++) {
			cvmSet(&um, i, i, 1. / (_w)[i]);
		}

		CvMat *tmp = cvCreateMat(dstarr->cols, dstarr->rows, dstarr->type);
		cvGEMM(&v, &um, 1, 0, 0, tmp, CV_GEMM_A_T);
		cvGEMM(tmp, &u, 1, 0, 0, dstarr, CV_GEMM_B_T);

		cvReleaseMat(&tmp);
	} else {
		assert(0 && "Bad argument");
		return -1;
	}

	return 0;
}

#define CV_CLONE_MAT_ALLOCA(stack_mat, mat)                                                                            \
	CV_CREATE_MAT_ALLOCA(stack_mat, mat->rows, mat->cols, mat->type)                                                   \
	cvCopy(mat, stack_mat, 0);

SURVIVE_LOCAL_ONLY int cvSolve(const CvMat *Aarr, const CvMat *xarr, CvMat *Barr, int method) {
	lapack_int inf;
	lapack_int arows = Aarr->rows;
	lapack_int acols = Aarr->cols;
	lapack_int xcols = xarr->cols;
	lapack_int xrows = xarr->rows;
	lapack_int lda = acols; // Aarr->step / sizeof(double);
	lapack_int type = CV_MAT_TYPE(Aarr->type);

	if (method == DECOMP_LU) {
		assert(Aarr->cols == Barr->rows);
		assert(xarr->rows == Aarr->rows);
		assert(Barr->cols == xarr->cols);
		assert(type == CV_MAT_TYPE(Barr->type) && (type == CV_32F || type == CV_64F));

		cvCopy(xarr, Barr, 0);
		FLT *a_ws = alloca(mat_size_bytes(Aarr));
		memcpy(a_ws, CV_RAW_PTR(Aarr), mat_size_bytes(Aarr));

		lapack_int brows = Barr->rows;
		lapack_int bcols = Barr->cols;
		lapack_int ldb = bcols; // Barr->step / sizeof(double);

		lapack_int *ipiv = alloca(sizeof(lapack_int) * MIN(Aarr->rows, Aarr->cols));

		inf = LAPACKE_getrf(LAPACK_ROW_MAJOR, arows, acols, (a_ws), lda, ipiv);
		assert(inf >= 0);
		if (inf > 0) {
			printf("Warning: Singular matrix: \n");
			// print_mat(a_ws);
		}

#ifdef DEBUG_PRINT
		printf("Solve A * x = B:\n");
		// print_mat(a_ws);
		print_mat(Barr);
#endif

		inf = LAPACKE_getrs(LAPACK_ROW_MAJOR, CblasNoTrans, arows, bcols, (a_ws), lda, ipiv, CV_RAW_PTR(Barr), ldb);
		assert(inf == 0);

		// free(ipiv);
		// cvReleaseMat(&a_ws);
	} else if (method == DECOMP_SVD) {

#ifdef DEBUG_PRINT
		printf("Solve |b - A * x|:\n");
		print_mat(Aarr);
		print_mat(xarr);
#endif
		bool xLargerThanB = xarr->rows > acols;
		CvMat *xCpy = 0;
		if (xLargerThanB) {
			CV_CLONE_MAT_ALLOCA(xCpyStack, xarr);
			xCpy = xCpyStack;
		} else {
			xCpy = Barr;
			memcpy(CV_RAW_PTR(Barr), CV_RAW_PTR(xarr), mat_size_bytes(xarr));
		}

		// CvMat *aCpy = cvCloneMat(Aarr);
		FLT *aCpy = alloca(mat_size_bytes(Aarr));
		memcpy(aCpy, CV_RAW_PTR(Aarr), mat_size_bytes(Aarr));

		FLT *S = alloca(sizeof(FLT) * MIN(arows, acols));
		// FLT *S = malloc(sizeof(FLT) * MIN(arows, acols));
		FLT rcond = -1;
		lapack_int *rank = alloca(sizeof(lapack_int) * MIN(arows, acols));
		// lapack_int *rank = malloc(sizeof(lapack_int) * MIN(arows, acols));
		lapack_int inf = LAPACKE_gelss(LAPACK_ROW_MAJOR, arows, acols, xcols, (aCpy), acols, CV_RAW_PTR(xCpy), xcols, S,
									   rcond, rank);
		assert(Barr->rows == acols);
		assert(Barr->cols == xCpy->cols);

		if (xLargerThanB) {
			xCpy->rows = acols;
			cvCopy(xCpy, Barr, 0);
			// cvReleaseMat(&xCpy);
		}

		// cvReleaseMat(&aCpy);
#ifdef DEBUG_PRINT
		print_mat(Barr);
#endif
		assert(inf == 0);
	}
	return 0;
}

SURVIVE_LOCAL_ONLY void cvTranspose(const CvMat *M, CvMat *dst) {
	bool inPlace = M == dst || CV_RAW_PTR(M) == CV_RAW_PTR(dst);
	FLT *src = CV_RAW_PTR(M);

	if (inPlace) {
		src = alloca(mat_size_bytes(M));
		memcpy(src, CV_RAW_PTR(M), mat_size_bytes(M));
	} else {
		assert(M->rows == dst->cols);
		assert(M->cols == dst->rows);
	}

	for (unsigned i = 0; i < M->rows; i++) {
		for (unsigned j = 0; j < M->cols; j++) {
			CV_RAW_PTR(dst)[j * M->rows + i] = src[i * M->cols + j];
		}
	}
}

SURVIVE_LOCAL_ONLY void cvSVD(CvMat *aarr, CvMat *warr, CvMat *uarr, CvMat *varr, int flags) {
	char jobu = 'A';
	char jobvt = 'A';

	lapack_int inf;

	if ((flags & CV_SVD_MODIFY_A) == 0) {
		aarr = cvCloneMat(aarr);
	}

	if (uarr == 0)
		jobu = 'N';
	if (varr == 0)
		jobvt = 'N';

	FLT *pw, *pu, *pv;
	lapack_int arows = aarr->rows, acols = aarr->cols;

	pw = warr ? CV_RAW_PTR(warr) : (FLT *)alloca(sizeof(FLT) * arows * acols);
	pu = uarr ? CV_RAW_PTR(uarr) : (FLT *)alloca(sizeof(FLT) * arows * arows);
	pv = varr ? CV_RAW_PTR(varr) : (FLT *)alloca(sizeof(FLT) * acols * acols);

	lapack_int ulda = uarr ? uarr->cols : acols;
	lapack_int plda = varr ? varr->cols : acols;

	FLT *superb = alloca(sizeof(FLT) * MIN(arows, acols));
	inf = LAPACKE_gesvd(LAPACK_ROW_MAJOR, jobu, jobvt, arows, acols, CV_RAW_PTR(aarr), acols, pw, pu, ulda, pv, plda,
						superb);

	// free(superb);

	switch (inf) {
	case -6:
		assert(false && "matrix has NaNs");
		break;
	case 0:
		break;
	default:
		assert(inf == 0);
	}

	if (uarr && (flags & CV_SVD_U_T)) {
		cvTranspose(uarr, uarr);
	}

	if (varr && (flags & CV_SVD_V_T) == 0) {
		cvTranspose(varr, varr);
	}

	if ((flags & CV_SVD_MODIFY_A) == 0) {
		cvReleaseMat(&aarr);
	}
}

SURVIVE_LOCAL_ONLY double cvDet(const CvMat *M) {
	assert(M->rows == M->cols);
	assert(M->rows <= 3 && "cvDet unimplemented for matrices >3");

	FLT *m = CV_RAW_PTR(M);

	switch (M->rows) {
	case 1:
		return m[0];
	case 2: {
		return m[0] * m[3] - m[1] * m[2];
	}
	case 3: {
		FLT m00 = m[0], m01 = m[1], m02 = m[2], m10 = m[3], m11 = m[4], m12 = m[5], m20 = m[6], m21 = m[7], m22 = m[8];

		return m00 * (m11 * m22 - m12 * m21) - m01 * (m10 * m22 - m12 * m20) + m02 * (m10 * m21 - m11 * m20);
	}
	default:
		abort();
	}
}