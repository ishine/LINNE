#include "lpc.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <float.h>
#include <assert.h>

/* メモリアラインメント */
#define LPC_ALIGNMENT 16

/* nの倍数切り上げ */
#define LPC_ROUNDUP(val, n) ((((val) + ((n) - 1)) / (n)) * (n))

/* 円周率 */
#define LPC_PI 3.1415926535897932384626433832795029

/* 残差絶対値の最小値 */
#define LPCAF_RESIDUAL_EPSILON 1e-6

/* 内部エラー型 */
typedef enum LPCErrorTag {
    LPC_ERROR_OK = 0,
    LPC_ERROR_NG,
    LPC_ERROR_SINGULAR_MATRIX,
    LPC_ERROR_INVALID_ARGUMENT
} LPCError;

/* LPC計算ハンドル */
struct LPCCalculator {
    uint32_t max_order; /* 最大次数 */
    uint32_t max_num_buffer_samples; /* 最大バッファサンプル数 */
    /* 内部的な計算結果は精度を担保するため全てdoubleで持つ */
    /* floatだとサンプル数を増やすと標本自己相関値の誤差に起因して出力の計算結果がnanになる */
    double *a_vec; /* 計算用ベクトル1 */
    double *u_vec; /* 計算用ベクトル3 */
    double *v_vec; /* 計算用ベクトル4 */
    double **r_mat; /* 補助関数法/Burg法で使用する行列（(max_order + 1)次） */
    double *auto_corr; /* 標本自己相関 */
    double *lpc_coef; /* LPC係数ベクトル */
    double *parcor_coef; /* PARCOR係数ベクトル */
    double *buffer; /* 入力信号のバッファ領域 */
    uint8_t alloced_by_own; /* 自分で領域確保したか？ */
    void *work; /* ワーク領域先頭ポインタ */
};

/* round関数（C89で定義されていない） */
static double LPC_Round(double d)
{
    return (d >= 0.0) ? floor(d + 0.5) : -floor(-d + 0.5);
}

/* log2関数（C89で定義されていない） */
static double LPC_Log2(double d)
{
#define INV_LOGE2 (1.4426950408889634)  /* 1 / log(2) */
    return log(d) * INV_LOGE2;
#undef INV_LOGE2
}

/* LPC係数計算ハンドルのワークサイズ計算 */
int32_t LPCCalculator_CalculateWorkSize(const struct LPCCalculatorConfig *config)
{
    int32_t work_size;

    /* 引数チェック */
    if (config == NULL) {
        return -1;
    }

    work_size = sizeof(struct LPCCalculator) + LPC_ALIGNMENT;
    work_size += (int32_t)(sizeof(double) * (config->max_order + 2) * 3); /* a, u, v ベクトル分の領域 */
    work_size += (int32_t)(sizeof(double) * (config->max_order + 1)); /* 標本自己相関の領域 */
    work_size += (int32_t)(sizeof(double) * (config->max_order + 1) * 2); /* 係数ベクトルの領域 */
    /* 補助関数法で使用する行列領域 */
    work_size += (int32_t)(sizeof(double *) * (config->max_order + 1));
    work_size += (int32_t)(sizeof(double) * (config->max_order + 1) * (config->max_order + 1));
    /* 入力信号バッファ領域 */
    work_size += (int32_t)(sizeof(double) * config->max_num_samples);

    return work_size;
}

/* LPC係数計算ハンドルの作成 */
struct LPCCalculator* LPCCalculator_Create(const struct LPCCalculatorConfig *config, void *work, int32_t work_size)
{
    struct LPCCalculator *lpcc;
    uint8_t *work_ptr;
    uint8_t tmp_alloc_by_own = 0;

    /* 自前でワーク領域確保 */
    if ((work == NULL) && (work_size == 0)) {
        if ((work_size = LPCCalculator_CalculateWorkSize(config)) < 0) {
            return NULL;
        }
        work = malloc((uint32_t)work_size);
        tmp_alloc_by_own = 1;
    }

    /* 引数チェック */
    if ((config == NULL) || (work == NULL)
            || (work_size < LPCCalculator_CalculateWorkSize(config))
            || (config->max_order == 0) || (config->max_num_samples == 0)) {
        if (tmp_alloc_by_own == 1) {
            free(work);
        }
        return NULL;
    }

    /* ワーク領域取得 */
    work_ptr = (uint8_t *)work;

    /* ハンドル領域確保 */
    work_ptr = (uint8_t *)LPC_ROUNDUP((uintptr_t)work_ptr, LPC_ALIGNMENT);
    lpcc = (struct LPCCalculator *)work_ptr;
    work_ptr += sizeof(struct LPCCalculator);

    /* ハンドルメンバの設定 */
    lpcc->max_order = config->max_order;
    lpcc->max_num_buffer_samples = config->max_num_samples;
    lpcc->work = work;
    lpcc->alloced_by_own = tmp_alloc_by_own;

    /* 計算用ベクトルの領域割当 */
    lpcc->a_vec = (double *)work_ptr;
    work_ptr += sizeof(double) * (config->max_order + 2); /* a_0, a_k+1を含めるとmax_order+2 */
    lpcc->u_vec = (double *)work_ptr;
    work_ptr += sizeof(double) * (config->max_order + 2);
    lpcc->v_vec = (double *)work_ptr;
    work_ptr += sizeof(double) * (config->max_order + 2);

    /* 標本自己相関の領域割当 */
    lpcc->auto_corr = (double *)work_ptr;
    work_ptr += sizeof(double) * (config->max_order + 1);

    /* 係数ベクトルの領域割当 */
    lpcc->lpc_coef = (double *)work_ptr;
    work_ptr += sizeof(double) * (config->max_order + 1);
    lpcc->parcor_coef = (double *)work_ptr;
    work_ptr += sizeof(double) * (config->max_order + 1);

    /* 補助関数法/Burg法で使用する行列領域 */
    {
        uint32_t ord;
        lpcc->r_mat = (double **)work_ptr;
        work_ptr += sizeof(double *) * (config->max_order + 1);
        for (ord = 0; ord < config->max_order + 1; ord++) {
            lpcc->r_mat[ord] = (double *)work_ptr;
            work_ptr += sizeof(double) * (config->max_order + 1);
        }
    }

    /* 入力信号バッファの領域 */
    lpcc->buffer = (double *)work_ptr;
    work_ptr += sizeof(double) * config->max_num_samples;

    /* バッファオーバーフローチェック */
    assert((work_ptr - (uint8_t *)work) <= work_size);

    return lpcc;
}

/* LPC係数計算ハンドルの破棄 */
void LPCCalculator_Destroy(struct LPCCalculator *lpcc)
{
    if (lpcc != NULL) {
        /* ワーク領域を時前確保していたときは開放 */
        if (lpcc->alloced_by_own == 1) {
            free(lpcc->work);
        }
    }
}

/* 窓関数の適用 */
static LPCError LPC_ApplyWindow(
    LPCWindowType window_type, const double *input, uint32_t num_samples, double *output)
{
    /* 引数チェック */
    if (input == NULL || output == NULL) {
        return LPC_ERROR_INVALID_ARGUMENT;
    }

    switch (window_type) {
    case LPC_WINDOWTYPE_RECTANGULAR:
        memcpy(output, input, sizeof(double) * num_samples);
        break;
    case LPC_WINDOWTYPE_SIN:
        {
            uint32_t smpl;
            for (smpl = 0; smpl < num_samples; smpl++) {
                output[smpl] = input[smpl] * sin((LPC_PI * smpl) / (num_samples - 1));
            }
        }
        break;
    case LPC_WINDOWTYPE_WELCH:
        {
            uint32_t smpl;
            const double divisor = 4.0 * pow(num_samples - 1, -2.0);
            for (smpl = 0; smpl < (num_samples >> 1); smpl++) {
                const double weight = divisor * smpl * (num_samples - 1 - smpl);
                output[smpl] = input[smpl] * weight;
                output[num_samples - smpl - 1] = input[num_samples - smpl - 1] * weight;
            }
        }
        break;
    default:
        return LPC_ERROR_NG;
    }

    return LPC_ERROR_OK;
}

/*（標本）自己相関の計算 */
static LPCError LPC_CalculateAutoCorrelation(
    const double *data, uint32_t num_samples, double *auto_corr, uint32_t order)
{
    uint32_t i, lag;

    assert(num_samples >= order);

    /* 引数チェック */
    if (data == NULL || auto_corr == NULL) {
        return LPC_ERROR_INVALID_ARGUMENT;
    }

    /* 自己相関初期化 */
    for (i = 0; i < order; i++) {
        auto_corr[i] = 0.0;
    }

    /* 0次は係数は単純計算 */
    for (i = 0; i < num_samples; i++) {
        auto_corr[0] += data[i] * data[i];
    }

    /* 1次以降の係数 */
    for (lag = 1; lag < order; lag++) {
        uint32_t l, L;
        uint32_t Llag2;
        const uint32_t lag2 = lag << 1;

        /* 被乗数が重複している連続した項の集まりの数 */
        if ((3 * lag) < num_samples) {
            L = 1 + (num_samples - (3 * lag)) / lag2;
        } else {
            L = 0;
        }
        Llag2 = L * lag2;

        /* 被乗数が重複している分を積和 */
        for (i = 0; i < lag; i++) {
            for (l = 0; l < Llag2; l += lag2) {
                /* 一般的に lag < L なので、ループはこの順 */
                auto_corr[lag] += data[l + lag + i] * (data[l + i] + data[l + lag2 + i]);
            }
        }

        /* 残りの項を単純に積和 */
        for (i = 0; i < (num_samples - Llag2 - lag); i++) {
            auto_corr[lag] += data[Llag2 + lag + i] * data[Llag2 + i];
        }

    }

    return LPC_ERROR_OK;
}

/* Levinson-Durbin再帰計算 */
static LPCError LPC_LevinsonDurbinRecursion(struct LPCCalculator *lpcc, uint32_t coef_order)
{
    uint32_t k, i;
    double gamma; /* 反射係数 */
    double ek; /* 誤差分散 */

    /* オート変数にポインタをコピー */
    double *a_vec = lpcc->a_vec;
    double *u_vec = lpcc->u_vec;
    double *v_vec = lpcc->v_vec;
    double *coef = lpcc->lpc_coef;
    double *parcor_coef = lpcc->parcor_coef;
    const double *auto_corr = lpcc->auto_corr;

    /* 引数チェック */
    if ((lpcc == NULL) || (coef == NULL) || (auto_corr == NULL)) {
        return LPC_ERROR_INVALID_ARGUMENT;
    }

    /* 0次自己相関（信号の二乗和）が小さい場合
    * => 係数は全て0として無音出力システムを予測 */
    if (fabs(auto_corr[0]) < FLT_EPSILON) {
        for (i = 0; i < coef_order + 1; i++) {
            coef[i] = parcor_coef[i] = 0.0;
        }
        return LPC_ERROR_OK;
    }

    /* 初期化 */
    for (i = 0; i < coef_order + 2; i++) {
        a_vec[i] = u_vec[i] = v_vec[i] = 0.0;
    }

    /* 最初のステップの係数をセット */
    a_vec[0] = 1.0;
    ek = auto_corr[0];
    a_vec[1] = - auto_corr[1] / auto_corr[0];
    parcor_coef[0] = 0.0;
    parcor_coef[1] = auto_corr[1] / ek;
    ek += auto_corr[1] * a_vec[1];
    u_vec[0] = 1.0; u_vec[1] = 0.0;
    v_vec[0] = 0.0; v_vec[1] = 1.0;

    /* 再帰処理 */
    for (k = 1; k < coef_order; k++) {
        gamma = 0.0;
        for (i = 0; i < k + 1; i++) {
            gamma += a_vec[i] * auto_corr[k + 1 - i];
        }
        gamma /= -ek;
        ek *= (1.0 - gamma * gamma);
        /* 誤差分散（パワー）は非負 */
        assert(ek >= 0.0);

        /* u_vec, v_vecの更新 */
        for (i = 0; i < k; i++) {
            u_vec[i + 1] = v_vec[k - i] = a_vec[i + 1];
        }
        u_vec[0] = 1.0; u_vec[k + 1] = 0.0;
        v_vec[0] = 0.0; v_vec[k + 1] = 1.0;

        /* 係数の更新 */
        for (i = 0; i < k + 2; i++) {
            a_vec[i] = u_vec[i] + gamma * v_vec[i];
        }
        /* PARCOR係数は反射係数の符号反転 */
        parcor_coef[k + 1] = -gamma;
        /* PARCOR係数の絶対値は1未満（収束条件） */
        assert(fabs(gamma) < 1.0);
    }

    /* 結果を取得 */
    memcpy(coef, a_vec, sizeof(double) * (coef_order + 1));

    return LPC_ERROR_OK;
}

/* 係数計算の共通関数 */
static LPCError LPC_CalculateCoef(
    struct LPCCalculator *lpcc, const double *data, uint32_t num_samples, uint32_t coef_order,
    LPCWindowType window_type)
{
    /* 引数チェック */
    if (lpcc == NULL) {
        return LPC_ERROR_INVALID_ARGUMENT;
    }

    /* 窓関数を適用 */
    if (LPC_ApplyWindow(window_type, data, num_samples, lpcc->buffer) != LPC_ERROR_OK) {
        return LPC_ERROR_NG;
    }

    /* 自己相関を計算 */
    if (LPC_CalculateAutoCorrelation(
            lpcc->buffer, num_samples, lpcc->auto_corr, coef_order + 1) != LPC_ERROR_OK) {
        return LPC_ERROR_NG;
    }

    /* 入力サンプル数が少ないときは、係数が発散することが多数
    * => 無音データとして扱い、係数はすべて0とする */
    if (num_samples < coef_order) {
        uint32_t ord;
        for (ord = 0; ord < coef_order + 1; ord++) {
            lpcc->lpc_coef[ord] = lpcc->parcor_coef[ord] = 0.0;
        }
        return LPC_ERROR_OK;
    }

    /* 再帰計算を実行 */
    if (LPC_LevinsonDurbinRecursion(lpcc, coef_order) != LPC_ERROR_OK) {
        return LPC_ERROR_NG;
    }

    return LPC_ERROR_OK;
}

/* Levinson-Durbin再帰計算によりLPC係数を求める（倍精度） */
LPCApiResult LPCCalculator_CalculateLPCCoefficients(
    struct LPCCalculator *lpcc,
    const double *data, uint32_t num_samples, double *coef, uint32_t coef_order,
    LPCWindowType window_type)
{
    /* 引数チェック */
    if ((data == NULL) || (coef == NULL)) {
        return LPC_APIRESULT_INVALID_ARGUMENT;
    }

    /* 次数チェック */
    if (coef_order > lpcc->max_order) {
        return LPC_APIRESULT_EXCEED_MAX_ORDER;
    }

    /* 入力サンプル数チェック */
    if (num_samples > lpcc->max_num_buffer_samples) {
        return LPC_APIRESULT_EXCEED_MAX_NUM_SAMPLES;
    }

    /* 係数計算 */
    if (LPC_CalculateCoef(lpcc, data, num_samples, coef_order, window_type) != LPC_ERROR_OK) {
        return LPC_APIRESULT_FAILED_TO_CALCULATION;
    }

    /* 計算成功時は結果をコピー */
    /* 先頭要素は必ず1.0なので2要素目からコピー */
    memmove(coef, &lpcc->lpc_coef[1], sizeof(double) * coef_order);

    return LPC_APIRESULT_OK;
}

/* コレスキー分解により Amat * xvec = bvec を解く */
static LPCError LPC_CholeskyDecomposition(
        double **Amat, int32_t dim, double *xvec, double *bvec, double *inv_diag)
{
    int32_t i, j, k;
    double sum;

    /* 引数チェック */
    assert((Amat != NULL) && (inv_diag != NULL) && (bvec != NULL) && (xvec != NULL));

    /* コレスキー分解 */
    for (i = 0; i < dim; i++) {
        sum = Amat[i][i];
        for (k = i - 1; k >= 0; k--) {
            sum -= Amat[i][k] * Amat[i][k];
        }
        if (sum <= 0.0) {
            return LPC_ERROR_SINGULAR_MATRIX;
        }
        /* 1.0 / sqrt(sum) は除算により桁落ちするためpowを使用 */
        inv_diag[i] = pow(sum, -0.5);
        for (j = i + 1; j < dim; j++) {
            sum = Amat[i][j];
            for (k = i - 1; k >= 0; k--) {
                sum -= Amat[i][k] * Amat[j][k];
            }
            Amat[j][i] = sum * inv_diag[i];
        }
    }

    /* 分解を用いて線形一次方程式を解く */
    for (i = 0; i < dim; i++) {
        sum = bvec[i];
        for (j = i - 1; j >= 0; j--) {
            sum -= Amat[i][j] * xvec[j];
        }
        xvec[i] = sum * inv_diag[i];
    }
    for (i = dim - 1; i >= 0; i--) {
        sum = xvec[i];
        for (j = i + 1; j < dim; j++) {
            sum -= Amat[j][i] * xvec[j];
        }
        xvec[i] = sum * inv_diag[i];
    }

    return LPC_ERROR_OK;
}

#if 1
/* 補助関数法（前向き残差）による係数行列計算 */
static LPCError LPCAF_CalculateCoefMatrixAndVector(
        const double *data, uint32_t num_samples,
        const double *a_vec, double **r_mat, double *r_vec,
        uint32_t coef_order, double *pobj_value)
{
    double obj_value;
    uint32_t smpl, i, j;

    assert(data != NULL);
    assert(a_vec != NULL);
    assert(r_mat != NULL);
    assert(r_vec != NULL);
    assert(pobj_value != NULL);
    assert(num_samples > coef_order);

    /* 行列を0初期化 */
    for (i = 0; i < coef_order; i++) {
        r_vec[i] = 0.0;
        for (j = 0; j < coef_order; j++) {
            r_mat[i][j] = 0.0;
        }
    }

    obj_value = 0.0;

    for (smpl = coef_order; smpl < num_samples; smpl++) {
        /* 残差計算 */
        double residual = data[smpl];
        double inv_residual;
        for (i = 0; i < coef_order; i++) {
            residual += a_vec[i] * data[smpl - i - 1];
        }
        residual = fabs(residual);
        obj_value += residual;
        /* 小さすぎる残差は丸め込む（ゼERO割回避、正則化） */
        residual = (residual < LPCAF_RESIDUAL_EPSILON) ? LPCAF_RESIDUAL_EPSILON : residual;
        inv_residual = 1.0 / residual;
        /* 係数行列に蓄積 */
        for (i = 0; i < coef_order; i++) {
            r_vec[i] -= data[smpl] * data[smpl - i - 1] * inv_residual;
            for (j = i; j < coef_order; j++) {
                r_mat[i][j] += data[smpl - i - 1] * data[smpl - j - 1] * inv_residual;
            }
        }
    }

    /* 対称要素に拡張 */
    for (i = 0; i < coef_order; i++) {
        for (j = i + 1; j < coef_order; j++) {
            r_mat[j][i] = r_mat[i][j];
        }
    }

    /* 目的関数値のセット */
    (*pobj_value) = obj_value / (num_samples - coef_order);

    return LPC_ERROR_OK;
}
#else
/* 補助関数法（前向き後ろ向き残差）による係数行列計算 */
static LPCError LPCAF_CalculateCoefMatrixAndVector(
        const double *data, uint32_t num_samples,
        const double *a_vec, double **r_mat, double *r_vec,
        uint32_t coef_order, double *pobj_value)
{
    double obj_value;
    uint32_t smpl, i, j;

    assert(data != NULL);
    assert(a_vec != NULL);
    assert(r_mat != NULL);
    assert(r_vec != NULL);
    assert(pobj_value != NULL);
    assert(num_samples > coef_order);

    /* 行列を0初期化 */
    for (i = 0; i < coef_order; i++) {
        r_vec[i] = 0.0;
        for (j = 0; j < coef_order; j++) {
            r_mat[i][j] = 0.0;
        }
    }

    obj_value = 0.0;

    for (smpl = coef_order; smpl < num_samples - coef_order; smpl++) {
        /* 残差計算 */
        double forward = data[smpl], backward = data[smpl];
        double inv_forward, inv_backward;
        for (i = 0; i < coef_order; i++) {
            forward += a_vec[i] * data[smpl - i - 1];
            backward += a_vec[i] * data[smpl + i + 1];
        }
        forward = fabs(forward);
        backward = fabs(backward);
        obj_value += (forward + backward);
        /* 小さすぎる残差は丸め込む（ゼERO割回避、正則化） */
        forward = (forward < LPCAF_RESIDUAL_EPSILON) ? LPCAF_RESIDUAL_EPSILON : forward;
        backward = (backward < LPCAF_RESIDUAL_EPSILON) ? LPCAF_RESIDUAL_EPSILON : backward;
        inv_forward = 1.0 / forward;
        inv_backward = 1.0 / backward;
        /* 係数行列に足し込み */
        for (i = 0; i < coef_order; i++) {
            r_vec[i] -= data[smpl] * data[smpl - i - 1] * inv_forward;
            r_vec[i] -= data[smpl] * data[smpl + i + 1] * inv_backward;
            for (j = i; j < coef_order; j++) {
                r_mat[i][j] += data[smpl - i - 1] * data[smpl - j - 1] * inv_forward;
                r_mat[i][j] += data[smpl + i + 1] * data[smpl + j + 1] * inv_backward;
            }
        }
    }

    /* 対称要素に拡張 */
    for (i = 0; i < coef_order; i++) {
        for (j = i + 1; j < coef_order; j++) {
            r_mat[j][i] = r_mat[i][j];
        }
    }

    (*pobj_value) = obj_value / (2.0 * (num_samples - (2.0 * coef_order)));

    return LPC_ERROR_OK;
}
#endif

/* 補助関数法による係数計算 */
static LPCError LPC_CalculateCoefAF(
        struct LPCCalculator *lpcc, const double *data, uint32_t num_samples, uint32_t coef_order,
        const uint32_t max_num_iteration, const double obj_epsilon, LPCWindowType window_type)
{
    uint32_t itr, i;
    double *a_vec = lpcc->a_vec;
    double *r_vec = lpcc->u_vec;
    double **r_mat = lpcc->r_mat;
    double obj_value, prev_obj_value;
    LPCError err;

    /* 係数をLebinson-Durbin法で初期化 */
    if ((err = LPC_CalculateCoef(lpcc, data, num_samples, coef_order, window_type)) != LPC_ERROR_OK) {
        return err;
    }
    memcpy(a_vec, &lpcc->lpc_coef[1], sizeof(double) * coef_order);

    /* 0次自己相関（信号の二乗和）が小さい場合
    * => 係数は全て0として無音出力システムを予測 */
    if (fabs(lpcc->auto_corr[0]) < FLT_EPSILON) {
        for (i = 0; i < coef_order + 1; i++) {
            lpcc->lpc_coef[i] = 0.0;
        }
        return LPC_ERROR_OK;
    }

    prev_obj_value = FLT_MAX;
    for (itr = 0; itr < max_num_iteration; itr++) {
        /* 係数行列要素の計算 */
        if ((err = LPCAF_CalculateCoefMatrixAndVector(
                data, num_samples, a_vec, r_mat, r_vec, coef_order, &obj_value)) != LPC_ERROR_OK) {
            return err;
        }
        /* コレスキー分解で r_mat @ avec = r_vec を解く */
        if ((err = LPC_CholeskyDecomposition(
                        r_mat, (int32_t)coef_order,
                        a_vec, r_vec, lpcc->v_vec)) == LPC_ERROR_SINGULAR_MATRIX) {
            /* 特異行列になるのは理論上入力が全部0のとき。係数を0クリアして終わる */
            for (i = 0; i < coef_order; i++) {
                lpcc->lpc_coef[i] = 0.0;
            }
            return LPC_ERROR_OK;
        }
        assert(err == LPC_ERROR_OK);
        /* 収束判定 */
        if (fabs(prev_obj_value - obj_value) < obj_epsilon) {
            break;
        }
        prev_obj_value = obj_value;
    }

    /* 解を設定 */
    memmove(lpcc->lpc_coef, a_vec, sizeof(double) * coef_order);

    return LPC_ERROR_OK;
}

/* 補助関数法よりLPC係数を求める（倍精度） */
LPCApiResult LPCCalculator_CalculateLPCCoefficientsAF(
    struct LPCCalculator *lpcc,
    const double *data, uint32_t num_samples, double *coef, uint32_t coef_order,
    uint32_t max_num_iteration, LPCWindowType window_type)
{
    /* 引数チェック */
    if ((lpcc == NULL) || (data == NULL) || (coef == NULL)) {
        return LPC_APIRESULT_INVALID_ARGUMENT;
    }

    /* 次数チェック */
    if (coef_order > lpcc->max_order) {
        return LPC_APIRESULT_EXCEED_MAX_ORDER;
    }

    /* 係数計算 */
    if (LPC_CalculateCoefAF(lpcc, data, num_samples, coef_order, max_num_iteration, 1e-8, window_type) != LPC_ERROR_OK) {
        return LPC_APIRESULT_FAILED_TO_CALCULATION;
    }

    /* 計算成功時は結果をコピー */
    memmove(coef, lpcc->lpc_coef, sizeof(double) * coef_order);

    return LPC_APIRESULT_OK;
}

/* Burg法による係数計算 */
static LPCError LPC_CalculateCoefBurg(
        struct LPCCalculator *lpcc, const double *data, uint32_t num_samples, uint32_t coef_order)
{
#if 1
    uint32_t i, j, k;
    double *a_vec = lpcc->a_vec;
    double **cov = lpcc->r_mat;
    LPCError err;

    /* 自己共分散行列計算 */
    for (i = 0; i <= coef_order; i++) {
        if ((err = LPC_CalculateAutoCorrelation(
                        data, num_samples - i, &cov[i][i], coef_order + 1 - i)) != LPC_ERROR_OK) {
            return err;
        }
        for (j = i + 1; j <= coef_order; j++) {
            cov[j][i] = cov[i][j];
        }
    }

    /* 係数初期化 */
    for (i = 0; i <= coef_order; i++) {
        a_vec[i] = 0.0;
    }
    a_vec[0] = 1.0;

    /* 次数ごとに計算 */
    for (k = 0; k < coef_order; k++) {
        double mu;
        double FkpBk = 0.0, sum = 0.0, Ck = 0.0;
        /* Fk + Bk */
        for (i = 0; i <= k; i++) {
            FkpBk += a_vec[i] * a_vec[i] * (cov[i][i] + cov[k + 1 - i][k + 1 - i]);
            /* 対角成分以外は対称性を使って半分だけ計算 */
            for (j = i + 1; j <= k; j++) {
                sum += a_vec[i] * a_vec[j] * (cov[i][j] + cov[k + 1 - i][k + 1 - j]);
            }
        }
        FkpBk += 2.0 * sum;
        /* Ck */
        for (i = 0; i <= k; i++) {
            for (j = 0; j <= k; j++) {
                Ck += a_vec[i] * a_vec[j] * cov[i][k + 1 - j];
            }
        }
        /* 反射係数の負号 */
        mu = - 2.0 * Ck / FkpBk;
        assert(fabs(mu) <= 1.0);
        /* 係数更新 */
        for (i = 0; i <= (k + 1) / 2; i++) {
            double tmp1, tmp2;
            tmp1 = a_vec[i]; tmp2 = a_vec[k + 1 - i];
            a_vec[i]         = tmp1 + mu * tmp2;
            a_vec[k + 1 - i] = mu * tmp1 + tmp2;
        }
    }

    /* 解を設定 */
    memmove(lpcc->lpc_coef, &lpcc->a_vec[1], sizeof(double) * coef_order);
#else
    uint32_t i, k;
    double *a_vec = lpcc->a_vec;
    double *f_vec, *b_vec;
    double Dk, mu;
    double tmp1, tmp2;

    /* ベクトル領域割り当て */
    f_vec = malloc(sizeof(double) * num_samples);
    b_vec = malloc(sizeof(double) * num_samples);

    /* 各ベクトル初期化 */
    for (k = 0; k < coef_order + 1; k++) {
        a_vec[k] = 0.0;
    }
    a_vec[0] = 1.0;
    memcpy(f_vec, data, sizeof(double) * num_samples);
    memcpy(b_vec, data, sizeof(double) * num_samples);

    /* Dkの初期化 */
    Dk = 0.0;
    for (i = 0; i < num_samples; i++) {
        Dk += 2.0 * f_vec[i] * f_vec[i];
    }
    Dk -= f_vec[0] * f_vec[0] + f_vec[num_samples - 1] * f_vec[num_samples - 1];

    /* Burg 再帰アルゴリズム */
    for (k = 0; k < coef_order; k++) {
        /* 反射（PARCOR）係数の計算 */
        mu = 0.0;
        for (i = 0; i < num_samples - k - 1; i++) {
            mu += f_vec[i + k + 1] * b_vec[i];
        }
        mu *= -2.0 / Dk;
        assert(fabs(mu) < 1.0);
        /* a_vecの更新 */
        for (i = 0; i <= (k + 1) / 2; i++) {
            tmp1 = a_vec[i]; tmp2 = a_vec[k + 1 - i];
            a_vec[i]         = tmp1 + mu * tmp2;
            a_vec[k + 1 - i] = mu * tmp1 + tmp2;
        }
        /* f_vec, b_vecの更新 */
        for (i = 0; i < num_samples - k - 1; i++) {
            tmp1 = f_vec[i + k + 1]; tmp2 = b_vec[i];
            f_vec[i + k + 1] = tmp1 + mu * tmp2;
            b_vec[i]         = mu * tmp1 + tmp2;
        }
        /* Dkの更新 */
        Dk = (1.0 - mu * mu) * Dk - f_vec[k + 1] * f_vec[k + 1] - b_vec[num_samples - k - 2] * b_vec[num_samples - k - 2];
    }

    /* 係数コピー */
    memcpy(lpcc->lpc_coef, &a_vec[1], sizeof(double) * coef_order);

    free(b_vec);
    free(f_vec);
#endif
    return LPC_ERROR_OK;
}

/* Burg法によりLPC係数を求める（倍精度） */
LPCApiResult LPCCalculator_CalculateLPCCoefficientsBurg(
    struct LPCCalculator *lpcc,
    const double *data, uint32_t num_samples, double *coef, uint32_t coef_order)
{
    /* 引数チェック */
    if ((lpcc == NULL) || (data == NULL) || (coef == NULL)) {
        return LPC_APIRESULT_INVALID_ARGUMENT;
    }

    /* 次数チェック */
    if (coef_order > lpcc->max_order) {
        return LPC_APIRESULT_EXCEED_MAX_ORDER;
    }

    /* 係数計算 */
    if (LPC_CalculateCoefBurg(lpcc, data, num_samples, coef_order) != LPC_ERROR_OK) {
        return LPC_APIRESULT_FAILED_TO_CALCULATION;
    }

    /* 計算成功時は結果をコピー */
    memmove(coef, lpcc->lpc_coef, sizeof(double) * coef_order);

    return LPC_APIRESULT_OK;
}

/* 入力データからサンプルあたりの推定符号長を求める */
LPCApiResult LPCCalculator_EstimateCodeLength(
        struct LPCCalculator *lpcc,
        const double *data, uint32_t num_samples, uint32_t bits_per_sample,
        uint32_t coef_order, double *length_per_sample_bits, LPCWindowType window_type)
{
    uint32_t ord;
    double log2_mean_res_power, log2_var_ratio;

    /* 定数値 */
#define BETA_CONST_FOR_LAPLACE_DIST   (1.9426950408889634)  /* sqrt(2 * E * E) */
#define BETA_CONST_FOR_GAUSS_DIST     (2.047095585180641)   /* sqrt(2 * E * PI) */

    /* 引数チェック */
    if ((lpcc == NULL) || (data == NULL) || (length_per_sample_bits == NULL)) {
        return LPC_APIRESULT_INVALID_ARGUMENT;
    }

    /* 係数計算 */
    if (LPC_CalculateCoef(lpcc, data, num_samples, coef_order, window_type) != LPC_ERROR_OK) {
        return LPC_APIRESULT_FAILED_TO_CALCULATION;
    }

    /* log2(パワー平均)の計算 */
    log2_mean_res_power = lpcc->auto_corr[0]; /* 0次標本自己相関はパワー */
    /* 整数PCMの振幅に変換（doubleの密度保障） */
    log2_mean_res_power *= pow(2, (double)(2.0 * (bits_per_sample - 1)));
    if (fabs(log2_mean_res_power) <= FLT_MIN) {
        /* ほぼ無音だった場合は符号長を0とする */
        (*length_per_sample_bits) = 0.0;
        return LPC_APIRESULT_OK;
    }
    log2_mean_res_power = LPC_Log2((double)log2_mean_res_power) - LPC_Log2((double)num_samples);

    /* sum(log2(1 - (parcor * parcor)))の計算 */
    /* 1次の係数は0で確定だから飛ばす */
    log2_var_ratio = 0.0;
    for (ord = 1; ord <= coef_order; ord++) {
        log2_var_ratio += LPC_Log2(1.0 - lpcc->parcor_coef[ord] * lpcc->parcor_coef[ord]);
    }

    /* エントロピー計算 */
    /* →サンプルあたりの最小のビット数が得られる */
    (*length_per_sample_bits) = BETA_CONST_FOR_LAPLACE_DIST + 0.5f * (log2_mean_res_power + log2_var_ratio);

    /* 推定ビット数が負値の場合は、1サンプルあたり1ビットで符号化できることを期待する */
    /* 補足）このケースは入力音声パワーが非常に低い */
    if ((*length_per_sample_bits) <= 0) {
        (*length_per_sample_bits) = 1.0;
        return LPC_APIRESULT_OK;
    }

#undef BETA_CONST_FOR_LAPLACE_DIST
#undef BETA_CONST_FOR_GAUSS_DIST

    return LPC_APIRESULT_OK;
}

/* MDL（最小記述長）を計算 */
LPCApiResult LPCCalculator_CalculateMDL(
    struct LPCCalculator *lpcc,
    const double *data, uint32_t num_samples, uint32_t coef_order, double *mdl,
    LPCWindowType window_type)
{
    uint32_t k;
    double tmp;

    /* 引数チェック */
    if ((lpcc == NULL) || (data == NULL) || (mdl == NULL)) {
        return LPC_APIRESULT_INVALID_ARGUMENT;
    }

    /* 係数計算 */
    if (LPC_CalculateCoef(lpcc, data, num_samples, coef_order, window_type) != LPC_ERROR_OK) {
        return LPC_APIRESULT_FAILED_TO_CALCULATION;
    }

    /* 第一項の計算 */
    /* 1次の係数は0で確定だから飛ばす */
    tmp = 0.0;
    for (k = 1; k <= coef_order; k++) {
        tmp += log(1.0 - lpcc->parcor_coef[k] * lpcc->parcor_coef[k]);
    }
    tmp *= num_samples;

    /* 第二項の計算 */
    tmp += coef_order * log(num_samples);

    (*mdl) = tmp;

    return LPC_APIRESULT_OK;
}

/* LPC係数の整数量子化 */
LPCApiResult LPC_QuantizeCoefficients(
    const double *double_coef, uint32_t coef_order, uint32_t nbits_precision,
    int32_t *int_coef, uint32_t *coef_rshift)
{
    uint32_t rshift;
    int32_t ord, ndigit, qtmp;
    double max, qerror;
    const int32_t qmax = (1 << (nbits_precision - 1));

    /* 引数チェック */
    if ((double_coef == NULL) || (int_coef == NULL)
            || (coef_rshift == NULL) || (nbits_precision == 0)) {
        return LPC_APIRESULT_INVALID_ARGUMENT;
    }

    /* 係数絶対値の計算 */
    max = 0.0;
    for (ord = 0; ord < coef_order; ord++) {
        if (max < fabs(double_coef[ord])) {
            max = fabs(double_coef[ord]);
        }
    }

    /* 与えられたビット数で表現できないほど小さいときは0とみなす */
    if (max <= pow(2.0, -(int32_t)(nbits_precision - 1))) {
        (*coef_rshift) = nbits_precision;
        memset(int_coef, 0, sizeof(int32_t) * coef_order);
        return LPC_APIRESULT_OK;
    }

    /* 最大値を[1/2, 1)に収めるための右シフト量の計算 */
    /* max = x * 2^ndigit, |x| in [1/2, 1)を計算 */
    (void)frexp(max, &ndigit);
    /* 符号ビットを落とす */
    nbits_precision--;
    /* nbits_precisionで表現可能にするためのシフト量計算 */
    assert((int32_t)nbits_precision > ndigit);
    rshift = (uint32_t)((int32_t)nbits_precision - ndigit);

    /* 量子化 */
    qerror = 0.0;
    for (ord = (int32_t)coef_order - 1; ord >= 0; ord--) {
        /* 前の係数の誤差を取り込んで量子化 */
        /* インパルスの先頭部分には誤差を入れたくないため、末尾から処理 */
        qerror += double_coef[ord] * pow(2.0, rshift);
        qtmp = (int32_t)LPC_Round(qerror);
        /* 正負境界の丸め込み */
        if (qtmp >= qmax) {
            qtmp = qmax - 1;
        } else if (qtmp < -qmax) {
            qtmp = -qmax;
        }
        /* 引いた分が量子化誤差として残る */
        qerror -= qtmp;
        int_coef[ord] = qtmp;
    }
    (*coef_rshift) = rshift;

    return LPC_APIRESULT_OK;
}

/* LPC係数により予測/誤差出力 */
LPCApiResult LPC_Predict(
    const int32_t *data, uint32_t num_samples,
    const int32_t *coef, uint32_t coef_order, int32_t *residual, uint32_t coef_rshift)
{
    uint32_t smpl, ord;

    /* 引数チェック */
    if ((data == NULL) || (coef == NULL)
            || (residual == NULL) || (coef_rshift == 0)) {
        return LPC_APIRESULT_INVALID_ARGUMENT;
    }

    memcpy(residual, data, sizeof(int32_t) * num_samples);

    /* LPC係数による予測 */
    for (smpl = 1; smpl < coef_order; smpl++) {
        int32_t predict = (1 << (coef_rshift - 1));
        for (ord = 0; ord < smpl; ord++) {
            predict += (coef[ord] * data[smpl - ord - 1]);
        }
        residual[smpl] += (predict >> coef_rshift);
    }
    for (smpl = coef_order; smpl < num_samples; smpl++) {
        int32_t predict = (1 << (coef_rshift - 1));
        for (ord = 0; ord < coef_order; ord++) {
            predict += (coef[ord] * data[smpl - ord - 1]);
        }
        residual[smpl] += (predict >> coef_rshift);
    }

    return LPC_APIRESULT_OK;
}

/* LPC係数により合成(in-place) */
LPCApiResult LPC_Synthesize(
    int32_t *data, uint32_t num_samples,
    const int32_t *coef, uint32_t coef_order, uint32_t coef_rshift)
{
    uint32_t smpl, ord;

    /* 引数チェック */
    if ((data == NULL) || (coef == NULL) || (coef_rshift == 0)) {
        return LPC_APIRESULT_INVALID_ARGUMENT;
    }

    /* LPC係数による予測 */
    for (smpl = 1; smpl < coef_order; smpl++) {
        int32_t predict = (1 << (coef_rshift - 1));
        for (ord = 0; ord < smpl; ord++) {
            predict += (coef[ord] * data[smpl - ord - 1]);
        }
        data[smpl] -= (predict >> coef_rshift);
    }
    for (smpl = coef_order; smpl < num_samples; smpl++) {
        int32_t predict = (1 << (coef_rshift - 1));
        for (ord = 0; ord < coef_order; ord++) {
            predict += (coef[ord] * data[smpl - ord - 1]);
        }
        data[smpl] -= (predict >> coef_rshift);
    }

    return LPC_APIRESULT_OK;
}
