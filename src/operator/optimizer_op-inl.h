/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file optimizer_op-inl.h
 * \brief Optimizer operators
 * \author Junyuan Xie
 */
#ifndef MXNET_OPERATOR_OPTIMIZER_OP_INL_H_
#define MXNET_OPERATOR_OPTIMIZER_OP_INL_H_
#include <dmlc/parameter.h>
#include <mxnet/operator.h>
#include <mxnet/operator_util.h>
#include <mxnet/op_attr_types.h>
#include <mshadow/base.h>
#include <nnvm/op.h>
#include <nnvm/op_attr_types.h>
#include <vector>
#include "./operator_common.h"
#include "./mshadow_op.h"
#include "./elemwise_op_common.h"
#include "mxnet_op.h"
#include "./tensor/init_op.h"

namespace mxnet {
namespace op {
struct SGDParam : public dmlc::Parameter<SGDParam> {
  float lr;
  float wd;
  float rescale_grad;
  float clip_gradient;
  DMLC_DECLARE_PARAMETER(SGDParam) {
    DMLC_DECLARE_FIELD(lr)
    .describe("Learning rate");
    DMLC_DECLARE_FIELD(wd)
    .set_default(0.0f)
    .describe("Weight decay augments the objective function with a "
              "regularization term that penalizes large weights. "
              "The penalty scales with the square of the magnitude of each weight.");
    DMLC_DECLARE_FIELD(rescale_grad)
    .set_default(1.0f)
    .describe("Rescale gradient to grad = rescale_grad*grad.");
    DMLC_DECLARE_FIELD(clip_gradient)
    .set_default(-1.0f)
    .describe("Clip gradient to the range of [-clip_gradient, clip_gradient] "
              "If clip_gradient <= 0, gradient clipping is turned off. "
              "grad = max(min(grad, clip_gradient), -clip_gradient).");
  }
};

struct SGDKernel {
  template<typename DType>
  MSHADOW_XINLINE static void Map(int i, DType* out_data, const DType* weight_data,
    const DType* grad_data, const DType param_clip_gradient,
    const DType param_lr, const DType param_wd, const DType param_rescale_grad,
    const OpReqType req) {
    if (param_clip_gradient >= 0.0f) {
      KERNEL_ASSIGN(out_data[i], req,
             (1.f-param_lr*param_wd)*weight_data[i]
               - (param_lr)
                 * mshadow_op::clip::Map(param_rescale_grad*grad_data[i], param_clip_gradient));
    } else {
      KERNEL_ASSIGN(out_data[i], req,
             (1.f-param_lr*param_wd)*weight_data[i]
               - (param_lr*param_rescale_grad)*grad_data[i]);
    }
  }
};

template<typename xpu>
inline void SGDUpdate(const nnvm::NodeAttrs& attrs,
                      const OpContext &ctx,
                      const std::vector<TBlob> &inputs,
                      const std::vector<OpReqType> &req,
                      const std::vector<TBlob> &outputs) {
  using namespace mxnet_op;
  const SGDParam& param = nnvm::get<SGDParam>(attrs.parsed);
  Stream<xpu>* s = ctx.get_stream<xpu>();
  MSHADOW_REAL_TYPE_SWITCH(inputs[0].type_flag_, DType, {
    Tensor<xpu, 2, DType> weight = inputs[0].FlatTo2D<xpu, DType>(s);
    Tensor<xpu, 2, DType> grad = inputs[1].FlatTo2D<xpu, DType>(s);
    Tensor<xpu, 2, DType> out = outputs[0].FlatTo2D<xpu, DType>(s);
    Kernel<SGDKernel, xpu>::Launch(s, weight.shape_.Size(), out.dptr_, weight.dptr_,
      grad.dptr_, static_cast<DType>(param.clip_gradient),
      static_cast<DType>(param.lr), static_cast<DType>(param.wd),
      static_cast<DType>(param.rescale_grad), req[0]);
  });
}

/*! \brief kernel for sparse sgd
 */
template<int req>
struct SGDDnsRspKernel {
  // DType is the output data type
  // IType is row sparse idx type
  // i is the ith row in row sparse gradient
  template<typename DType, typename IType>
  MSHADOW_XINLINE static void Map(int i, const index_t row_length, DType* out, const DType* weight,
                                  const IType* grad_idx, const DType *grad_val,
                                  const DType clip_gradient, const DType lr,
                                  const DType wd, const DType rescale_grad) {
    for (index_t j = 0; j < row_length; j++) {
      index_t data_i = grad_idx[i] * row_length + j;
      index_t grad_i = i * row_length + j;
      if (clip_gradient >= 0.0f) {
        KERNEL_ASSIGN(out[data_i], req, (1.f - lr * wd) * weight[data_i] -
                     (lr) * mshadow_op::clip::Map(rescale_grad * grad_val[grad_i], clip_gradient));
      } else {
        KERNEL_ASSIGN(out[data_i], req, (1.f - lr * wd) * weight[data_i] -
                      (lr * rescale_grad) * grad_val[grad_i]);
      }
    }
  }
};

template<typename xpu>
inline void SGDUpdateDnsRspImpl(const SGDParam& param,
                                const OpContext &ctx,
                                const TBlob& weight,
                                const NDArray& grad,
                                const OpReqType& req,
                                TBlob *out) {
  using namespace mshadow;
  using namespace mshadow::expr;
  using namespace mshadow_op;
  using namespace mxnet_op;
  Stream<xpu>* s = ctx.get_stream<xpu>();
  CHECK_EQ(grad.storage_type(), kRowSparseStorage);
  // if gradients are zeros, no weights are updated
  if (!grad.storage_initialized() || req == kNullOp) return;
  CHECK_EQ(req, kWriteInplace) << "kWriteInplace is expected for sparse sgd_mom_update";
  CHECK_GT(weight.shape_.Size(), 0);

  MSHADOW_REAL_TYPE_SWITCH(weight.type_flag_, DType, {
    MSHADOW_IDX_TYPE_SWITCH(grad.aux_type(rowsparse::kIdx), IType, {
      MXNET_ASSIGN_REQ_SWITCH(req, req_type, {
        DType* weight_data = weight.dptr<DType>();
        IType* grad_idx = grad.aux_data(rowsparse::kIdx).dptr<IType>();
        DType* grad_val = grad.data().dptr<DType>();
        index_t num_rows = grad.aux_shape(rowsparse::kIdx)[0];
        auto row_length = weight.shape_.ProdShape(1, weight.ndim());
        Kernel<SGDDnsRspKernel<req_type>, xpu>::Launch(s, num_rows, row_length,
          out->dptr<DType>(), weight_data, grad_idx, grad_val,
          static_cast<DType>(param.clip_gradient),
          static_cast<DType>(param.lr), static_cast<DType>(param.wd),
          static_cast<DType>(param.rescale_grad));
      });
    });
  });
}

/*! \brief kernel for sparse sgd
 */
template<int req>
struct SGDRspDnsKernel {
  template<typename DType>
  MSHADOW_XINLINE static void Map(int i, const index_t num_cols, DType* out, const DType* weight,
                                  const DType *grad, const DType clip_gradient, const DType lr,
                                  const DType wd, const DType rescale_grad) {
    bool contains_non_zeros = false;
    index_t j = 0;
    index_t offset = i * num_cols;
    for (; j < num_cols; ++j) {
      if (grad[offset + j] != 0) {
        contains_non_zeros = true;
        break;
      }
    }
    if (!contains_non_zeros) return;
    const DType rate = 1.f - lr * wd;
    for (index_t j = 0; j < num_cols; j++) {
      auto index = offset + j;
      if (clip_gradient >= 0.0f) {
        KERNEL_ASSIGN(out[index], req, rate * weight[index] -
                      lr * mshadow_op::clip::Map(rescale_grad * grad[index], clip_gradient));
      } else {
        KERNEL_ASSIGN(out[index], req, rate * weight[index] -
                      lr * rescale_grad * grad[index]);
      }
    }
  }
};

template<typename xpu>
inline void SGDUpdateRspDnsImpl(const SGDParam& param,
                                const OpContext &ctx,
                                const NDArray& weight,
                                const TBlob& grad,
                                const OpReqType req,
                                NDArray *out) {
  using namespace mshadow;
  using namespace mxnet_op;
  using namespace rowsparse;
  CHECK_RSP_ALL_ROWS_NON_ZERO(weight, "SGDUpdate", "weights");
  CHECK_EQ(weight.storage_type(), kRowSparseStorage);
  if (req == kNullOp) return;
  CHECK_EQ(req, kWriteInplace) << "kWriteInplace is expected for sparse sgd_update";
  CHECK(weight.storage_initialized());
  Stream<xpu>* s = ctx.get_stream<xpu>();
  MSHADOW_REAL_TYPE_SWITCH(weight.dtype(), DType, {
    MXNET_ASSIGN_REQ_SWITCH(req, req_type, {
      DType* weight_data = weight.data().dptr<DType>();
      DType* grad_data = grad.dptr<DType>();
      index_t num_rows = weight.aux_shape(kIdx)[0];
      auto num_cols = weight.shape().ProdShape(1, weight.shape().ndim());
      Kernel<SGDRspDnsKernel<req_type>, xpu>::Launch(s, num_rows, num_cols,
        out->data().dptr<DType>(), weight_data, grad_data,
        static_cast<DType>(param.clip_gradient),
        static_cast<DType>(param.lr), static_cast<DType>(param.wd),
        static_cast<DType>(param.rescale_grad));
    });
  });
}

template<typename xpu>
inline void SGDUpdateRspRspImpl(const SGDParam& param,
                                const OpContext& ctx,
                                const NDArray& weight,
                                const NDArray& grad,
                                const OpReqType& req,
                                NDArray *out) {
  CHECK_RSP_ALL_ROWS_NON_ZERO(weight, "SGDUpdate", "weights");
  // reuse dns rsp implementation when storage_shape == shape
  TBlob out_blob = out->data();
  SGDUpdateDnsRspImpl<xpu>(param, ctx, weight.data(), grad, req, &out_blob);
}

template<typename xpu>
inline void SGDUpdateEx(const nnvm::NodeAttrs& attrs,
                        const OpContext &ctx,
                        const std::vector<NDArray> &inputs,
                        const std::vector<OpReqType> &req,
                        const std::vector<NDArray> &outputs) {
  using namespace mshadow;
  using namespace mshadow::expr;
  using namespace mshadow_op;
  const SGDParam& param = nnvm::get<SGDParam>(attrs.parsed);
  auto weight_stype = inputs[0].storage_type();
  auto grad_stype = inputs[1].storage_type();
  if (weight_stype == kRowSparseStorage && grad_stype == kRowSparseStorage) {
    NDArray out = outputs[0];
    SGDUpdateRspRspImpl<xpu>(param, ctx, inputs[0], inputs[1], req[0], &out);
  } else if (weight_stype == kRowSparseStorage && grad_stype == kDefaultStorage) {
    NDArray out = outputs[0];
    SGDUpdateRspDnsImpl<xpu>(param, ctx, inputs[0], inputs[1].data(), req[0], &out);
  } else {
    FCompExFallback<xpu>(attrs, ctx, inputs, req, outputs, SGDUpdate<xpu>, "SGDUpdate");
  }
}

struct SGDMomParam : public dmlc::Parameter<SGDMomParam> {
  float lr;
  float momentum;
  float wd;
  float rescale_grad;
  float clip_gradient;
  DMLC_DECLARE_PARAMETER(SGDMomParam) {
    DMLC_DECLARE_FIELD(lr)
    .describe("Learning rate");
    DMLC_DECLARE_FIELD(momentum)
    .set_default(0.0f)
    .describe("The decay rate of momentum estimates at each epoch.");
    DMLC_DECLARE_FIELD(wd)
    .set_default(0.0f)
    .describe("Weight decay augments the objective function with a "
              "regularization term that penalizes large weights. "
              "The penalty scales with the square of the magnitude of each weight.");
    DMLC_DECLARE_FIELD(rescale_grad)
    .set_default(1.0f)
    .describe("Rescale gradient to grad = rescale_grad*grad.");
    DMLC_DECLARE_FIELD(clip_gradient)
    .set_default(-1.0f)
    .describe("Clip gradient to the range of [-clip_gradient, clip_gradient] "
              "If clip_gradient <= 0, gradient clipping is turned off. "
              "grad = max(min(grad, clip_gradient), -clip_gradient).");
  }
};

struct SGDMomKernel {
  template<typename DType>
  MSHADOW_XINLINE static void Map(int i, DType* out_data, DType* mom_data, const DType* weight_data,
    const DType* grad_data, const DType param_clip_gradient, const DType param_momentum,
    const DType param_lr, const DType param_wd, const DType param_rescale_grad,
    const OpReqType req) {
    if (param_clip_gradient >= 0.0f) {
      mom_data[i] = param_momentum*mom_data[i]
              - param_lr*param_wd*weight_data[i]
              - param_lr
              *mshadow_op::clip::Map(param_rescale_grad*grad_data[i], param_clip_gradient);
    } else {
      mom_data[i] = param_momentum*mom_data[i]
                - param_lr*param_wd*weight_data[i]
                - param_lr*param_rescale_grad*grad_data[i];
    }
    KERNEL_ASSIGN(out_data[i], req, weight_data[i] + mom_data[i]);
  }
};

template<typename xpu>
inline void SGDMomUpdate(const nnvm::NodeAttrs& attrs,
                         const OpContext &ctx,
                         const std::vector<TBlob> &inputs,
                         const std::vector<OpReqType> &req,
                         const std::vector<TBlob> &outputs) {
  using namespace mxnet_op;
  SGDMomParam param = nnvm::get<SGDMomParam>(attrs.parsed);
  Stream<xpu>* s = ctx.get_stream<xpu>();
  MSHADOW_REAL_TYPE_SWITCH(inputs[0].type_flag_, DType, {
    Tensor<xpu, 2, DType> weight = inputs[0].FlatTo2D<xpu, DType>(s);
    Tensor<xpu, 2, DType> grad = inputs[1].FlatTo2D<xpu, DType>(s);
    Tensor<xpu, 2, DType> mom = inputs[2].FlatTo2D<xpu, DType>(s);
    Tensor<xpu, 2, DType> out = outputs[0].FlatTo2D<xpu, DType>(s);
    Kernel<SGDMomKernel, xpu>::Launch(s, weight.shape_.Size(), out.dptr_, mom.dptr_, weight.dptr_,
      grad.dptr_, static_cast<DType>(param.clip_gradient), static_cast<DType>(param.momentum),
      static_cast<DType>(param.lr), static_cast<DType>(param.wd),
      static_cast<DType>(param.rescale_grad), req[0]);
  });
}

template<int n_in, int n_out, int total_in>
inline bool MP_SGD_InferType(const nnvm::NodeAttrs& attrs,
                             std::vector<int> *in_attrs,
                             std::vector<int> *out_attrs) {
  CHECK_EQ(in_attrs->size(), static_cast<size_t>(total_in)) << " in operator " << attrs.name;
  CHECK_EQ(out_attrs->size(), static_cast<size_t>(n_out)) << " in operator " << attrs.name;
  for (int i = n_in; i < total_in; ++i) {
    TYPE_ASSIGN_CHECK(*in_attrs, i, mshadow::kFloat32);
  }
  return ElemwiseAttr<int, type_is_none, type_assign, true, type_string, n_in, n_out>(
      attrs, in_attrs, out_attrs, -1);
}

struct MP_SGDKernel {
  template<typename DType>
  MSHADOW_XINLINE static void Map(int i, DType* out_data, const DType* weight_data,
    const DType* grad_data, float* weight32, const float param_clip_gradient,
    const float param_lr, const float param_wd, const float param_rescale_grad,
    const OpReqType req) {
    if (param_clip_gradient >= 0.0f) {
      float w = weight32[i];
      w = (1.f - param_lr*param_wd)*w -
          (param_lr) * mshadow_op::clip::Map(param_rescale_grad*static_cast<float>(grad_data[i]),
                                             param_clip_gradient);
      weight32[i] = w;
      KERNEL_ASSIGN(out_data[i], req, (DType)w);
    } else {
      float w = weight32[i];
      w = (1.f-param_lr*param_wd)*w
               - (param_lr*param_rescale_grad)*static_cast<float>(grad_data[i]);
      weight32[i] = w;
      KERNEL_ASSIGN(out_data[i], req, (DType)w);
    }
  }
};

template<typename xpu>
inline void MP_SGDUpdate(const nnvm::NodeAttrs& attrs,
                      const OpContext &ctx,
                      const std::vector<TBlob> &inputs,
                      const std::vector<OpReqType> &req,
                      const std::vector<TBlob> &outputs) {
  using namespace mxnet_op;
  const SGDParam& param = nnvm::get<SGDParam>(attrs.parsed);
  Stream<xpu>* s = ctx.get_stream<xpu>();
  MSHADOW_REAL_TYPE_SWITCH(inputs[0].type_flag_, DType, {
    Tensor<xpu, 2, DType> weight = inputs[0].FlatTo2D<xpu, DType>(s);
    Tensor<xpu, 2, DType> grad = inputs[1].FlatTo2D<xpu, DType>(s);
    Tensor<xpu, 2, float> weight32 = inputs[2].FlatTo2D<xpu, float>(s);
    Tensor<xpu, 2, DType> out = outputs[0].FlatTo2D<xpu, DType>(s);
    Kernel<MP_SGDKernel, xpu>::Launch(s, weight.shape_.Size(), out.dptr_, weight.dptr_,
      grad.dptr_, weight32.dptr_, param.clip_gradient,
      param.lr, param.wd,
      param.rescale_grad, req[0]);
  });
}

struct MP_SGDMomKernel {
  template<typename DType>
  MSHADOW_XINLINE static void Map(int i, DType* out_data, float* mom_data,
    const DType* weight_data, const DType* grad_data, float* weight32,
    const float param_clip_gradient, const float param_momentum, const float param_lr,
    const float param_wd, const float param_rescale_grad, const OpReqType req) {
    float w = weight32[i];
    float mom = mom_data[i];
    if (param_clip_gradient >= 0.0f) {
      mom = param_momentum*mom
              - param_lr*param_wd*w
              - param_lr
              *mshadow_op::clip::Map(param_rescale_grad*static_cast<float>(grad_data[i]),
                                     param_clip_gradient);
    } else {
      mom = param_momentum*mom
                - param_lr*param_wd*w
                - param_lr*param_rescale_grad*static_cast<float>(grad_data[i]);
    }
    mom_data[i] = mom;
    w = w + mom;
    weight32[i] = w;
    KERNEL_ASSIGN(out_data[i], req, w);
  }
};

template<typename xpu>
inline void MP_SGDMomUpdate(const nnvm::NodeAttrs& attrs,
                         const OpContext &ctx,
                         const std::vector<TBlob> &inputs,
                         const std::vector<OpReqType> &req,
                         const std::vector<TBlob> &outputs) {
  using namespace mxnet_op;
  SGDMomParam param = nnvm::get<SGDMomParam>(attrs.parsed);
  Stream<xpu>* s = ctx.get_stream<xpu>();
  MSHADOW_REAL_TYPE_SWITCH(inputs[0].type_flag_, DType, {
    Tensor<xpu, 2, DType> weight = inputs[0].FlatTo2D<xpu, DType>(s);
    Tensor<xpu, 2, DType> grad = inputs[1].FlatTo2D<xpu, DType>(s);
    Tensor<xpu, 2, float> mom = inputs[2].FlatTo2D<xpu, float>(s);
    Tensor<xpu, 2, float> weight32 = inputs[3].FlatTo2D<xpu, float>(s);
    Tensor<xpu, 2, DType> out = outputs[0].FlatTo2D<xpu, DType>(s);
    Kernel<MP_SGDMomKernel, xpu>::Launch(s, weight.shape_.Size(), out.dptr_, mom.dptr_,
      weight.dptr_, grad.dptr_, weight32.dptr_, param.clip_gradient, param.momentum,
      param.lr, param.wd, param.rescale_grad, req[0]);
  });
}

template<int req>
struct SGDMomDnsRspDnsKernel {
  template<typename DType, typename IType>
  MSHADOW_XINLINE static void Map(int i, index_t row_length, DType* out_data,
    DType* mom_data, const DType* weight_data, const IType* grad_idx,
    const DType* grad_data, const DType clip_gradient, const DType momentum,
    const DType lr, const DType wd, const DType rescale_grad) {
    const DType rate = lr * wd;
    for (index_t j = 0; j < row_length; j++) {
      index_t data_i = grad_idx[i] * row_length + j;
      index_t grad_i = i * row_length + j;
      if (clip_gradient >= 0.0f) {
        mom_data[data_i] = momentum * mom_data[data_i]
                - rate * weight_data[data_i]
                - lr *
                mshadow_op::clip::Map(rescale_grad * grad_data[grad_i],
                                      clip_gradient);
      } else {
        mom_data[data_i] = momentum * mom_data[data_i]
                  - rate * weight_data[data_i]
                  - lr * rescale_grad * grad_data[grad_i];
      }
      KERNEL_ASSIGN(out_data[data_i], req, weight_data[data_i] + mom_data[data_i]);
    }
  }
};

template<typename xpu>
inline void SGDMomUpdateDnsRspDnsImpl(const SGDMomParam& param,
                                      const OpContext& ctx,
                                      const TBlob& weight,
                                      const NDArray& grad,
                                      const TBlob& mom,
                                      const OpReqType& req,
                                      TBlob *out) {
  using namespace mxnet_op;
  using namespace rowsparse;
  Stream<xpu>* s = ctx.get_stream<xpu>();
  if (!grad.storage_initialized() || req == kNullOp) return;
  CHECK_EQ(req, kWriteInplace) << "kWriteInplace is expected for sparse sgd_mom_update";
  CHECK_GT(weight.shape_.Size(), 0);
  CHECK_GT(mom.shape_.Size(), 0);

  MSHADOW_REAL_TYPE_SWITCH(weight.type_flag_, DType, {
    MSHADOW_IDX_TYPE_SWITCH(grad.aux_type(kIdx), IType, {
      MXNET_ASSIGN_REQ_SWITCH(req, req_type, {
        DType* weight_data = weight.dptr<DType>();
        IType* grad_idx = grad.aux_data(kIdx).dptr<IType>();
        DType* grad_val = grad.data().dptr<DType>();
        DType* mom_data = mom.dptr<DType>();
        DType* out_data = out->dptr<DType>();
        index_t num_rows = grad.aux_shape(kIdx)[0];
        auto row_length = weight.shape_.ProdShape(1, weight.ndim());
        Kernel<SGDMomDnsRspDnsKernel<req_type>, xpu>::Launch(s, num_rows, row_length,
          out_data, mom_data, weight_data, grad_idx, grad_val,
          static_cast<DType>(param.clip_gradient), static_cast<DType>(param.momentum),
          static_cast<DType>(param.lr), static_cast<DType>(param.wd),
          static_cast<DType>(param.rescale_grad));
      });
    });
  });
}

template<int req>
struct SGDMomRspDnsKernel {
  template<typename DType>
  MSHADOW_XINLINE static void Map(int i, index_t num_cols, DType* out, DType* mom,
                                  const DType* weight, const DType *grad,
                                  const DType clip_gradient, const DType momentum,
                                  const DType lr, const DType wd, const DType rescale_grad) {
    bool contains_non_zeros = false;
    index_t j = 0;
    index_t offset = i * num_cols;
    for (; j < num_cols; ++j) {
      if (grad[offset + j] != 0) {
        contains_non_zeros = true;
        break;
      }
    }
    if (!contains_non_zeros) return;
    const DType rate = lr * wd;
    for (index_t j = 0; j < num_cols; j++) {
      auto index = offset + j;
      if (clip_gradient >= 0.0f) {
        mom[index] = momentum * mom[index] - rate * weight[index]
                   - lr * mshadow_op::clip::Map(rescale_grad * grad[index], clip_gradient);
      } else {
        mom[index] = momentum * mom[index] - rate * weight[index]
                   - lr * rescale_grad * grad[index];
      }
      KERNEL_ASSIGN(out[index], req, weight[index] + mom[index]);
    }
  }
};

template<typename xpu>
inline void SGDMomUpdateRspDnsImpl(const SGDMomParam& param,
                                   const OpContext &ctx,
                                   const NDArray& weight,
                                   const TBlob& grad,
                                   const NDArray& mom,
                                   const OpReqType req,
                                   NDArray *out) {
  using namespace mshadow;
  using namespace mxnet_op;
  using namespace rowsparse;
  CHECK_RSP_ALL_ROWS_NON_ZERO(weight, "SGDMomUpdate", "weights");
  Stream<xpu>* s = ctx.get_stream<xpu>();
  CHECK_EQ(weight.storage_type(), kRowSparseStorage);
  if (req == kNullOp) return;
  CHECK_EQ(req, kWriteInplace) << "kWriteInplace is expected for sparse sgd_mom_update";
  CHECK(weight.storage_initialized());
  // fill mom with zero values if not initialized yet
  if (!mom.storage_initialized()) {
    NDArray mom_zeros = mom;
    FillDnsZerosRspImpl(s, &mom_zeros);
  }
  MSHADOW_REAL_TYPE_SWITCH(weight.dtype(), DType, {
    MXNET_ASSIGN_REQ_SWITCH(req, req_type, {
      DType* weight_data = weight.data().dptr<DType>();
      DType* grad_data = grad.dptr<DType>();
      DType* mom_data = mom.data().dptr<DType>();
      index_t num_rows = weight.aux_shape(kIdx)[0];
      auto num_cols = weight.shape().ProdShape(1, weight.shape().ndim());
      Kernel<SGDMomRspDnsKernel<req_type>, xpu>::Launch(s, num_rows, num_cols,
        out->data().dptr<DType>(), mom_data, weight_data, grad_data,
        static_cast<DType>(param.clip_gradient), static_cast<DType>(param.momentum),
        static_cast<DType>(param.lr), static_cast<DType>(param.wd),
        static_cast<DType>(param.rescale_grad));
    });
  });
}


template<typename xpu>
inline void SGDMomUpdateRspRspRspImpl(const SGDMomParam& param,
                                      const OpContext& ctx,
                                      const NDArray& weight,
                                      const NDArray& grad,
                                      const NDArray& mom,
                                      const OpReqType& req,
                                      NDArray *out) {
  using namespace mshadow;
  using namespace mshadow::expr;
  using namespace mxnet_op;
  using namespace rowsparse;
  CHECK_RSP_ALL_ROWS_NON_ZERO(weight, "SGDMomUpdate", "weights");
  Stream<xpu>* s = ctx.get_stream<xpu>();
  // fill mom with zero values in order to reuse the sgd mom dns impl
  if (!mom.storage_initialized()) {
    NDArray mom_zeros = mom;
    FillDnsZerosRspImpl(s, &mom_zeros);
  }
  TBlob out_blob = out->data();
  // reuse dns rsp implementation when storage_shape == shape
  SGDMomUpdateDnsRspDnsImpl<xpu>(param, ctx, weight.data(), grad,
                                 mom.data(), req, &out_blob);
}

template<typename xpu>
inline void SGDMomUpdateEx(const nnvm::NodeAttrs& attrs,
                           const OpContext &ctx,
                           const std::vector<NDArray> &inputs,
                           const std::vector<OpReqType> &req,
                           const std::vector<NDArray> &outputs) {
  using namespace mxnet_op;
  const SGDMomParam& param = nnvm::get<SGDMomParam>(attrs.parsed);
  auto &weight = inputs[0];
  auto &grad = inputs[1];
  auto &mom = inputs[2];
  auto weight_stype = weight.storage_type();
  auto grad_stype = grad.storage_type();
  auto mom_stype = mom.storage_type();
  CHECK_EQ(weight_stype, mom_stype) << "Inconsistent storage type detected between mom.stype = "
           << mom_stype << " and weight.stype = " << weight_stype;
  if (weight_stype == kRowSparseStorage && grad_stype == kRowSparseStorage &&
      mom_stype == kRowSparseStorage) {
     NDArray out = outputs[0];
     SGDMomUpdateRspRspRspImpl<xpu>(param, ctx, weight, grad, mom, req[0], &out);
  } else if (weight_stype == kRowSparseStorage && grad_stype == kDefaultStorage &&
      mom_stype == kRowSparseStorage) {
     NDArray out = outputs[0];
     SGDMomUpdateRspDnsImpl<xpu>(param, ctx, weight, grad.data(), mom, req[0], &out);
  } else {
    // inputs[2] is a mutable input
    FCompExFallback<xpu>(attrs, ctx, inputs, req, outputs,
                         SGDMomUpdate<xpu>, "SGDMomUpdate", {2});
  }
}

struct AdamParam : public dmlc::Parameter<AdamParam> {
  float lr;
  float beta1;
  float beta2;
  float epsilon;
  float wd;
  float rescale_grad;
  float clip_gradient;
  DMLC_DECLARE_PARAMETER(AdamParam) {
    DMLC_DECLARE_FIELD(lr)
    .describe("Learning rate");
    DMLC_DECLARE_FIELD(beta1)
    .set_default(0.9f)
    .describe("The decay rate for the 1st moment estimates.");
    DMLC_DECLARE_FIELD(beta2)
    .set_default(0.999f)
    .describe("The decay rate for the 2nd moment estimates.");
    DMLC_DECLARE_FIELD(epsilon)
    .set_default(1e-8f)
    .describe("A small constant for numerical stability.");
    DMLC_DECLARE_FIELD(wd)
    .set_default(0.0f)
    .describe("Weight decay augments the objective function with a "
              "regularization term that penalizes large weights. "
              "The penalty scales with the square of the magnitude of each weight.");
    DMLC_DECLARE_FIELD(rescale_grad)
    .set_default(1.0f)
    .describe("Rescale gradient to grad = rescale_grad*grad.");
    DMLC_DECLARE_FIELD(clip_gradient)
    .set_default(-1.0f)
    .describe("Clip gradient to the range of [-clip_gradient, clip_gradient] "
              "If clip_gradient <= 0, gradient clipping is turned off. "
              "grad = max(min(grad, clip_gradient), -clip_gradient).");
  }
};

template<typename xpu>
inline void AdamUpdate(const nnvm::NodeAttrs& attrs,
                       const OpContext &ctx,
                       const std::vector<TBlob> &inputs,
                       const std::vector<OpReqType> &req,
                       const std::vector<TBlob> &outputs) {
  using namespace mshadow;
  using namespace mshadow::expr;
  using namespace mshadow_op;
  const AdamParam& param = nnvm::get<AdamParam>(attrs.parsed);
  Stream<xpu>* s = ctx.get_stream<xpu>();
  MSHADOW_REAL_TYPE_SWITCH(inputs[0].type_flag_, DType, {
    Tensor<xpu, 2, DType> weight = inputs[0].FlatTo2D<xpu, DType>(s);
    Tensor<xpu, 2, DType> grad = inputs[1].FlatTo2D<xpu, DType>(s);
    Tensor<xpu, 2, DType> mean = inputs[2].FlatTo2D<xpu, DType>(s);
    Tensor<xpu, 2, DType> var = inputs[3].FlatTo2D<xpu, DType>(s);
    Tensor<xpu, 2, DType> out = outputs[0].FlatTo2D<xpu, DType>(s);

    grad = scalar<DType>(param.rescale_grad) * grad +
      scalar<DType>(param.wd) * weight;

    if (param.clip_gradient >= 0.0f) {
      mean = scalar<DType>(param.beta1)*mean + scalar<DType>(1.f-param.beta1) *
          F<clip>(grad, DType(param.clip_gradient));
      var = scalar<DType>(param.beta2)*var + scalar<DType>(1.f-param.beta2)*F<square>(
          F<clip>(grad, DType(param.clip_gradient)));
    } else {
      mean = scalar<DType>(param.beta1)*mean + scalar<DType>(1.f-param.beta1) * grad;
      var = scalar<DType>(param.beta2)*var + scalar<DType>(1.f-param.beta2) * F<square>(grad);
    }
    Assign(out, req[0],
           weight -
           scalar<DType>(param.lr) * mean /
           (F<square_root>(var) + scalar<DType>(param.epsilon)));
  });
}

/*!
 * Note: this kernel performs sparse adam update. For each row-slice in row_sparse
 * gradient, it finds the corresponding elements in weight, mean and var and performs
 * the update.
 * The kernel assumes dense weight/mean/var, and row_sparse gradient
 */
template<int req>
struct AdamDnsRspDnsKernel {
  template<typename DType, typename IType>
  MSHADOW_XINLINE static void Map(int i, const nnvm::dim_t row_length, DType* out_data,
    DType* mean_data, DType* var_data, const DType* weight_data, const IType* grad_idx,
    const DType* grad_data, const DType clip_gradient, const DType beta1, const DType beta2,
    const DType lr, const DType wd, const DType epsilon, const DType rescale_grad) {
    using nnvm::dim_t;
    using namespace mshadow_op;
    const dim_t row_offset = grad_idx[i] * row_length;
    for (dim_t j = 0; j < row_length; j++) {
      // index in data/mean/var
      const dim_t data_i = row_offset + j;
      // index in grad
      const dim_t grad_i = i * row_length + j;
      const DType grad_rescaled = grad_data[grad_i] * rescale_grad + weight_data[data_i] * wd;
      if (clip_gradient >= 0.0f) {
        mean_data[data_i] = beta1 * mean_data[data_i] + (1.f - beta1) *
                            clip::Map(grad_rescaled, clip_gradient);
        var_data[data_i] =  beta2 * var_data[data_i] + (1.f - beta2) * square::Map(
                            clip::Map(grad_rescaled, clip_gradient));
      } else {
        mean_data[data_i] = beta1 * mean_data[data_i] + (1.f - beta1) * grad_rescaled;
        var_data[data_i] = beta2 * var_data[data_i] +
                           (1.f - beta2) * grad_rescaled * grad_rescaled;
      }
      KERNEL_ASSIGN(out_data[data_i], req, weight_data[data_i] - lr * mean_data[data_i] /
                    (square_root::Map(var_data[data_i]) + epsilon));
    }
  }
};


template<typename xpu>
inline void AdamUpdateDnsRspDnsImpl(const AdamParam& param,
                                    const OpContext& ctx,
                                    const TBlob& weight,
                                    const NDArray& grad,
                                    const TBlob& mean,
                                    const TBlob& var,
                                    const OpReqType& req,
                                    TBlob *out) {
  using namespace mxnet_op;
  using namespace rowsparse;
  Stream<xpu>* s = ctx.get_stream<xpu>();
  if (!grad.storage_initialized() || req == kNullOp) return;
  CHECK_EQ(req, kWriteInplace) << "kWriteInplace is expected for sparse adam_update";
  CHECK_GT(weight.shape_.Size(), 0);
  CHECK_GT(mean.shape_.Size(), 0);
  CHECK_GT(var.shape_.Size(), 0);

  MSHADOW_REAL_TYPE_SWITCH(weight.type_flag_, DType, {
    MSHADOW_IDX_TYPE_SWITCH(grad.aux_type(kIdx), IType, {
      MXNET_ASSIGN_REQ_SWITCH(req, req_type, {
        const DType* weight_data = weight.dptr<DType>();
        const IType* grad_idx = grad.aux_data(kIdx).dptr<IType>();
        const DType* grad_val = grad.data().dptr<DType>();
        DType* mean_data = mean.dptr<DType>();
        DType* var_data = var.dptr<DType>();
        DType* out_data = out->dptr<DType>();
        nnvm::dim_t num_rows = grad.aux_shape(kIdx)[0];
        const auto row_length = weight.shape_.ProdShape(1, weight.ndim());
        Kernel<AdamDnsRspDnsKernel<req_type>, xpu>::Launch(s, num_rows, row_length,
          out_data, mean_data, var_data, weight_data, grad_idx, grad_val,
          static_cast<DType>(param.clip_gradient), static_cast<DType>(param.beta1),
          static_cast<DType>(param.beta2), static_cast<DType>(param.lr),
          static_cast<DType>(param.wd), static_cast<DType>(param.epsilon),
          static_cast<DType>(param.rescale_grad));
      });
    });
  });
}

template<typename xpu>
inline void AdamUpdateRspRspRspImpl(const AdamParam& param,
                                    const OpContext& ctx,
                                    const NDArray& weight,
                                    const NDArray& grad,
                                    const NDArray& mean,
                                    const NDArray& var,
                                    const OpReqType& req,
                                    NDArray *out) {
  using namespace mshadow;
  using namespace mshadow::expr;
  using namespace mxnet_op;
  using namespace rowsparse;
  CHECK_RSP_ALL_ROWS_NON_ZERO(weight, "AdamUpdate", "weights");
  Stream<xpu>* s = ctx.get_stream<xpu>();
  // fill mean and variance with zero values in order to reuse the sgd mom dns impl
  if (!mean.storage_initialized()) {
    NDArray mean_zeros = mean;
    FillDnsZerosRspImpl(s, &mean_zeros);
  }
  if (!var.storage_initialized()) {
    NDArray var_zeros = var;
    FillDnsZerosRspImpl(s, &var_zeros);
  }
  TBlob out_blob = out->data();
  // reuse dns rsp implementation when storage_shape == shape
  AdamUpdateDnsRspDnsImpl<xpu>(param, ctx, weight.data(), grad, mean.data(),
                               var.data(), req, &out_blob);
}


template<typename xpu>
inline void AdamUpdateEx(const nnvm::NodeAttrs& attrs,
                         const OpContext &ctx,
                         const std::vector<NDArray> &inputs,
                         const std::vector<OpReqType> &req,
                         const std::vector<NDArray> &outputs) {
  const AdamParam& param = nnvm::get<AdamParam>(attrs.parsed);
  mshadow::Stream<xpu>* s = ctx.get_stream<xpu>();
  const auto weight_stype = inputs[0].storage_type();
  const auto grad_stype = inputs[1].storage_type();
  const auto mean_stype = inputs[2].storage_type();
  const auto var_stype = inputs[3].storage_type();

  const auto out_stype = outputs[0].storage_type();
  CHECK_EQ(mean_stype, weight_stype) << "Inconsistent storage type detected between "
           << " mean.stype = " << mean_stype << " and weight.stype = " << weight_stype;
  CHECK_EQ(var_stype, weight_stype) << "Inconsistent storage type detected between "
           << " var.stype = " << var_stype << " and weight.stype = " << weight_stype;
  if (weight_stype == kRowSparseStorage && mean_stype == kRowSparseStorage &&
      var_stype == kRowSparseStorage && grad_stype == kRowSparseStorage &&
      out_stype == kRowSparseStorage) {
     NDArray out = outputs[0];
     AdamUpdateRspRspRspImpl<xpu>(param, ctx, inputs[0], inputs[1], inputs[2],
                                  inputs[3], req[0], &out);
  } else {
    LOG(FATAL) << "Unexpected storage types: weight.stype = " << weight_stype
               << ", var.stype = " << var_stype << ", mean.stype = " << mean_stype
               << ", grad.stype = " << grad_stype;
  }
}

// This RMSProp code follows the version in
// http://arxiv.org/pdf/1308.0850v5.pdf Eq(38) - Eq(45)
// by Alex Graves, 2013.
struct RMSPropAlexParam : public dmlc::Parameter<RMSPropAlexParam> {
  float lr;
  float gamma1;
  float gamma2;
  float epsilon;
  float wd;
  float rescale_grad;
  float clip_gradient;
  float clip_weights;
  DMLC_DECLARE_PARAMETER(RMSPropAlexParam) {
    DMLC_DECLARE_FIELD(lr)
    .describe("Learning rate");
    DMLC_DECLARE_FIELD(gamma1).set_default(0.95f)
    .describe("Decay rate.");
    DMLC_DECLARE_FIELD(gamma2).set_default(0.9f)
    .describe("Decay rate.");
    DMLC_DECLARE_FIELD(epsilon).set_default(1e-8f)
    .describe("A small constant for numerical stability.");
    DMLC_DECLARE_FIELD(wd).set_default(0.0f)
    .describe("Weight decay augments the objective function with a "
              "regularization term that penalizes large weights. "
              "The penalty scales with the square of the magnitude of each weight.");
    DMLC_DECLARE_FIELD(rescale_grad)
    .set_default(1.0f)
    .describe("Rescale gradient to grad = rescale_grad*grad.");
    DMLC_DECLARE_FIELD(clip_gradient)
    .set_default(-1.0f)
    .describe("Clip gradient to the range of [-clip_gradient, clip_gradient] "
              "If clip_gradient <= 0, gradient clipping is turned off. "
              "grad = max(min(grad, clip_gradient), -clip_gradient).");
    DMLC_DECLARE_FIELD(clip_weights)
    .set_default(-1.0f)
    .describe("Clip weights to the range of [-clip_weights, clip_weights] "
              "If clip_weights <= 0, weight clipping is turned off. "
              "weights = max(min(weights, clip_weights), -clip_weights).");
  }
};

template <typename xpu>
inline void RMSPropAlexUpdate(const nnvm::NodeAttrs &attrs,
                              const OpContext &ctx,
                              const std::vector<TBlob> &inputs,
                              const std::vector<OpReqType> &req,
                              const std::vector<TBlob> &outputs) {
  using namespace mshadow;
  using namespace mshadow::expr;
  using namespace mshadow_op;
  const RMSPropAlexParam &param = nnvm::get<RMSPropAlexParam>(attrs.parsed);
  Stream<xpu> *s = ctx.get_stream<xpu>();
  MSHADOW_REAL_TYPE_SWITCH(inputs[0].type_flag_, DType, {
    Tensor<xpu, 2, DType> weight = inputs[0].FlatTo2D<xpu, DType>(s);
    Tensor<xpu, 2, DType> grad = inputs[1].FlatTo2D<xpu, DType>(s);
    Tensor<xpu, 2, DType> state_n = inputs[2].FlatTo2D<xpu, DType>(s);
    Tensor<xpu, 2, DType> state_g = inputs[3].FlatTo2D<xpu, DType>(s);
    Tensor<xpu, 2, DType> delta = inputs[4].FlatTo2D<xpu, DType>(s);
    Tensor<xpu, 2, DType> out = outputs[0].FlatTo2D<xpu, DType>(s);

    grad = scalar<DType>(param.rescale_grad) * grad +
           scalar<DType>(param.wd) * weight;

    if (param.clip_gradient >= 0.0f) {
      state_n = scalar<DType>(1.f - param.gamma1) *
                    F<clip>(grad, DType(param.clip_gradient)) *
                    F<clip>(grad, DType(param.clip_gradient)) +
                scalar<DType>(param.gamma1) * state_n;
      state_g = scalar<DType>(1.f - param.gamma1) *
                    F<clip>(grad, DType(param.clip_gradient)) +
                scalar<DType>(param.gamma1) * state_g;
      delta = scalar<DType>(param.gamma2) * delta -
              scalar<DType>(param.lr) *
                  (F<clip>(grad, DType(param.clip_gradient)) /
                   (F<square_root>(state_n - state_g * state_g +
                                   scalar<DType>(param.epsilon))));
    } else {
      state_n = scalar<DType>(1.f - param.gamma1) * (grad * grad) +
                scalar<DType>(param.gamma1) * state_n;
      state_g = scalar<DType>(1.f - param.gamma1) * grad +
                scalar<DType>(param.gamma1) * state_g;
      delta = scalar<DType>(param.gamma2) * delta -
              scalar<DType>(param.lr) *
                  (grad / (F<square_root>(state_n - state_g * state_g +
                                          scalar<DType>(param.epsilon))));
    }

    if (param.clip_weights >= 0.0f) {
      Assign(out, req[0], F<clip>(weight + delta, DType(param.clip_weights)));
    } else {
      Assign(out, req[0], weight + delta);
    }
  });
}

// This RMSProp code follows the version in
// http://www.cs.toronto.edu/~tijmen/csc321/slides/lecture_slides_lec6.pdf
// by Tieleman & Hinton, 2012
struct RMSPropParam : public dmlc::Parameter<RMSPropParam> {
  float lr;
  float gamma1;
  float epsilon;
  float wd;
  float rescale_grad;
  float clip_gradient;
  float clip_weights;
  DMLC_DECLARE_PARAMETER(RMSPropParam) {
    DMLC_DECLARE_FIELD(lr)
    .describe("Learning rate");
    DMLC_DECLARE_FIELD(gamma1).set_default(0.95f)
    .describe("The decay rate of momentum estimates.");
    DMLC_DECLARE_FIELD(epsilon).set_default(1e-8f)
    .describe("A small constant for numerical stability.");
    DMLC_DECLARE_FIELD(wd).set_default(0.0f)
    .describe("Weight decay augments the objective function with a "
              "regularization term that penalizes large weights. "
              "The penalty scales with the square of the magnitude of each weight.");
    DMLC_DECLARE_FIELD(rescale_grad)
    .set_default(1.0f)
    .describe("Rescale gradient to grad = rescale_grad*grad.");
    DMLC_DECLARE_FIELD(clip_gradient)
    .set_default(-1.0f)
    .describe("Clip gradient to the range of [-clip_gradient, clip_gradient] "
              "If clip_gradient <= 0, gradient clipping is turned off. "
              "grad = max(min(grad, clip_gradient), -clip_gradient).");
    DMLC_DECLARE_FIELD(clip_weights)
    .set_default(-1.0f)
    .describe("Clip weights to the range of [-clip_weights, clip_weights] "
              "If clip_weights <= 0, weight clipping is turned off. "
              "weights = max(min(weights, clip_weights), -clip_weights).");
  }
};

template <typename xpu>
inline void RMSPropUpdate(const nnvm::NodeAttrs &attrs, const OpContext &ctx,
                          const std::vector<TBlob> &inputs,
                          const std::vector<OpReqType> &req,
                          const std::vector<TBlob> &outputs) {
  using namespace mshadow;
  using namespace mshadow::expr;
  using namespace mshadow_op;
  const RMSPropParam &param = nnvm::get<RMSPropParam>(attrs.parsed);
  Stream<xpu> *s = ctx.get_stream<xpu>();
  MSHADOW_REAL_TYPE_SWITCH(inputs[0].type_flag_, DType, {
    Tensor<xpu, 2, DType> weight = inputs[0].FlatTo2D<xpu, DType>(s);
    Tensor<xpu, 2, DType> grad = inputs[1].FlatTo2D<xpu, DType>(s);
    Tensor<xpu, 2, DType> state_n = inputs[2].FlatTo2D<xpu, DType>(s);
    Tensor<xpu, 2, DType> out = outputs[0].FlatTo2D<xpu, DType>(s);

    grad = scalar<DType>(param.rescale_grad) * grad +
           scalar<DType>(param.wd) * weight;

    if (param.clip_gradient >= 0.0f) {
      state_n = scalar<DType>(1.f - param.gamma1) *
                    F<clip>(grad, DType(param.clip_gradient)) *
                    F<clip>(grad, DType(param.clip_gradient)) +
                scalar<DType>(param.gamma1) * state_n;
      if (param.clip_weights >= 0.0f) {
        Assign(out, req[0],
               F<clip>(weight -
                       scalar<DType>(param.lr) *
                           (F<clip>(grad, DType(param.clip_gradient)) /
                            (F<square_root>(state_n +
                                            scalar<DType>(param.epsilon)))),
                       DType(param.clip_weights)));
      } else {
        Assign(out, req[0], weight -
                            scalar<DType>(param.lr) *
                              (F<clip>(grad, DType(param.clip_gradient)) /
                               (F<square_root>(state_n +
                                               scalar<DType>(param.epsilon)))));
      }
    } else {
      state_n = scalar<DType>(1.f - param.gamma1) * (grad * grad) +
                scalar<DType>(param.gamma1) * state_n;
      if (param.clip_weights >= 0.0f) {
        Assign(out, req[0],
               F<clip>(weight -
                       scalar<DType>(param.lr) *
                           (grad /
                            (F<square_root>(state_n +
                                            scalar<DType>(param.epsilon)))),
                       DType(param.clip_weights)));
      } else {
        Assign(out, req[0], weight -
                            scalar<DType>(param.lr) *
                              (grad /
                               (F<square_root>(state_n +
                                               scalar<DType>(param.epsilon)))));
      }
    }
  });
}

}  // namespace op
}  // namespace mxnet


#endif  // MXNET_OPERATOR_OPTIMIZER_OP_INL_H_
