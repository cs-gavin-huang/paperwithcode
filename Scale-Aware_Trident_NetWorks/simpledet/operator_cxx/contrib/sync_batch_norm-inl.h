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
 * Copyright (c) 2018 by Contributors
 * \file sync_batch_norm-inl.h
 * \brief Synchronized BatchNorm modified from BatchNormV1
 * \author Hang Zhang, Yuntao Chen
*/
#ifndef MXNET_OPERATOR_CONTRIB_SYNC_BATCH_NORM_INL_H_
#define MXNET_OPERATOR_CONTRIB_SYNC_BATCH_NORM_INL_H_

#include <dmlc/logging.h>
#include <dmlc/parameter.h>
#include <mxnet/operator.h>
#include <condition_variable>
#include <map>
#include <vector>
#include <string>
#include <utility>
#include "../operator_common.h"
#include "../mshadow_op.h"

namespace mxnet {
namespace op {

namespace syncbatchnorm {
enum BatchNormOpInputs {kData, kGamma, kBeta};
enum BatchNormOpOutputs {kOut, kMean, kVar};
enum BatchNormOpAuxiliary {kMovingMean, kMovingVar};
enum BatchNormBackResource {kTempSpace};
}  // namespace syncbatchnorm

struct SyncBatchNormParam : public dmlc::Parameter<SyncBatchNormParam> {
  float eps;
  float momentum;
  bool fix_gamma;
  bool use_global_stats;
  bool output_mean_var;
  int ndev;
  std::string key;
  DMLC_DECLARE_PARAMETER(SyncBatchNormParam) {
    DMLC_DECLARE_FIELD(eps).set_default(1e-3f)
    .describe("Epsilon to prevent div 0");
    DMLC_DECLARE_FIELD(momentum).set_default(0.9f)
    .describe("Momentum for moving average");
    DMLC_DECLARE_FIELD(fix_gamma).set_default(true)
    .describe("Fix gamma while training");
    DMLC_DECLARE_FIELD(use_global_stats).set_default(false)
    .describe("Whether use global moving statistics instead of local batch-norm. "
              "This will force change batch-norm into a scale shift operator.");
    DMLC_DECLARE_FIELD(output_mean_var).set_default(false)
    .describe("Output All,normal mean and var");
    DMLC_DECLARE_FIELD(ndev).set_default(1)
      .describe("The count of GPU devices");
    DMLC_DECLARE_FIELD(key)
      .set_default("")
      .describe("Hash key for synchronization, please set the same hash key for same layer, "
                "Block.prefix is typically used as in :class:`gluon.nn.contrib.SyncBatchNorm`.");
  }
};

// Modified from https://github.com/brucechin/SharedTensor
template<class T>
class SharedND {
 private:
  int num_devices_;
  T mean_;
  T *data_;
  bool *flag_;
  bool mean_ready_ = false;
  bool data_inited_ = false;
  std::mutex mutex_;

 public:
  explicit SharedND(int ndev) :num_devices_(ndev) {
    flag_ = new bool[ndev];
    data_ = new T[ndev];
    memset(flag_, false, ndev * sizeof(bool));
  }

  ~SharedND() {
    mshadow::FreeSpace(&mean_);
    for (int i = 0; i < num_devices_; i++) {
      mshadow::FreeSpace(&data_[i]);
    }
    delete [] flag_;
    delete [] data_;
  }

  void Init(mshadow::Shape<1> shape) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!data_inited_) {
      for (int i = 0; i < num_devices_; i++) {
        data_[i] = mshadow::NewTensor<cpu, real_t>(shape, 0.0f);
      }
      mean_ = mshadow::NewTensor<cpu, real_t>(shape, 0.0f);
      data_inited_ = true;
    }
  }

  T* Retrieve(mshadow::Shape<1> shape, int index) {
    if (!data_inited_) {
      Init(shape);
    }
    if (flag_[index] == false) {
      return &data_[index];
    } else {
      return nullptr;
    }
  }

  bool SetReady(int index) {
    if (flag_[index] == false) {
      flag_[index] = true;
      return true;
    } else {
      return false;
    }
  }

  T Pop(int index) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!MeanReady()) {}
    flag_[index] = false;
    T tmp = mean_;
    ResetMean();
    return tmp;
  }

  bool MeanReady() {
    if (mean_ready_) {
      return true;
    }
    for (int i = 0; i < num_devices_; i++) {
      if (!flag_[i]) {
        return false;
      }
    }
    for (int i = 1; i < num_devices_; i++) {
      data_[0] += data_[i];
    }
    mean_ = data_[0] * 1.0f /  num_devices_;
    mean_ready_ = true;
    return true;
  }

  void ResetMean() {
    for (int i = 0; i < num_devices_; i++) {
      if (flag_[i]) return;
    }
    mean_ready_ = false;
  }
};

template<class T>
class GlobalShared {
 public:
  T* Register(const std::string &key, int ndev) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = registry_.find(key);
    if (it != registry_.end()) return it->second;
    T *newT = new T(ndev);
    registry_[key] = newT;
    return newT;
  }
  ~GlobalShared() {
    for (auto it = registry_.begin(); it != registry_.end(); it++) {
      T *ptr = it->second;
      delete ptr;
    }
  }
 private:
  std::mutex mutex_;
  std::map<std::string, T*> registry_;
};

template<class T>
class GlobalSharedRank {
 public:
  T Register(const std::string &key, int ndev) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = registry_.find(key);
    if (it != registry_.end()) {
      T* tmpT = it->second;
      *tmpT = (*tmpT == ndev - 1) ? 0 : *tmpT + 1;
      return *tmpT;
    }
    T *newT = new T(0);
    registry_[key] = newT;
    return *newT;
  }
  ~GlobalSharedRank() {
    for (auto it = registry_.begin(); it != registry_.end(); it++) {
      T *ptr = it->second;
      delete ptr;
    }
  }
 private:
  std::mutex mutex_;
  std::map<std::string, T*> registry_;
};

class Barrier {
 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::size_t count_;
  std::size_t total_count_;
 public:
  explicit Barrier(std::size_t count) : count_{count}, total_count_{count} { }
  void Wait() {
    std::unique_lock<std::mutex> lock{mutex_};
    if (--count_ == 0) {
      count_ = total_count_;
      cv_.notify_all();
    } else {
      cv_.wait(lock, [this] { return count_ == total_count_; });
    }
  }
};

// Global variables for Synchronizations
static GlobalSharedRank<int> global_shared_rank;
static GlobalShared<Barrier> global_shared_barrier;
static GlobalShared<SharedND<mshadow::Tensor<cpu, 1, real_t>>> global_shared_mean;
static GlobalShared<SharedND<mshadow::Tensor<cpu, 1, real_t>>> global_shared_var;
static GlobalShared<SharedND<mshadow::Tensor<cpu, 1, real_t>>> global_shared_grad;
static GlobalShared<SharedND<mshadow::Tensor<cpu, 1, real_t>>> global_shared_prod;

template<typename xpu>
class SyncBatchNorm : public Operator {
 public:
  explicit SyncBatchNorm(SyncBatchNormParam param) {
    this->param_ = param;
  }

  virtual void Forward(const OpContext &ctx,
                       const std::vector<TBlob> &in_data,
                       const std::vector<OpReqType> &req,
                       const std::vector<TBlob> &out_data,
                       const std::vector<TBlob> &aux_states) {
    using namespace mshadow;
    using namespace mshadow::expr;
    using namespace mshadow_op;
    using namespace mxnet_op;
    CHECK_EQ(in_data.size(), 3U);
    CHECK_EQ(aux_states.size(), 2U);
    if (ctx.is_train) {
      CHECK_EQ(out_data.size(), 3U);
      CHECK_EQ(req.size(), 3U);
    } else {
      CHECK_GE(out_data.size(), 1U);
      CHECK_GE(req.size(), 1U);
      CHECK_EQ(req[syncbatchnorm::kOut], kWriteTo);
    }

    Stream<xpu> *s = ctx.get_stream<xpu>();
      MSHADOW_TYPE_SWITCH(in_data[syncbatchnorm::kData].type_flag_, DType, {
      const bool is_double = std::is_same<DType, double>::value;
      CHECK_EQ(is_double, false)
        << "Synchronized BatchNorm does not support double-precision floating number yet...";
      const real_t scale = static_cast<real_t>(in_data[syncbatchnorm::kData].shape_[1]) /
        static_cast<real_t>(in_data[syncbatchnorm::kData].shape_.Size());
      const size_t data_size = in_data[syncbatchnorm::kData].Size();
      Tensor<xpu, 4> data;
      Tensor<xpu, 4> out;
      Tensor<xpu, 1> workspace;
      if (!std::is_same<DType, real_t>::value) {
        workspace = ctx.requested[syncbatchnorm::kTempSpace].get_space<xpu, 1>(
          Shape1(data_size * 2), s);
      }
      if (in_data[syncbatchnorm::kData].ndim() == 2) {
        Shape<4> dshape = Shape4(in_data[syncbatchnorm::kData].shape_[0],
                                 in_data[syncbatchnorm::kData].shape_[1], 1, 1);
        if (std::is_same<DType, real_t>::value) {
          data = in_data[syncbatchnorm::kData].get_with_shape<xpu, 4, real_t>(dshape, s);
          out = out_data[syncbatchnorm::kOut].get_with_shape<xpu, 4, real_t>(dshape, s);
        } else {
          data = Tensor<xpu, 4>(workspace.dptr_, dshape, s);
          out = Tensor<xpu, 4>(workspace.dptr_ + data_size, dshape, s);
        }
      } else {
        if (std::is_same<DType, real_t>::value) {
          data = in_data[syncbatchnorm::kData].get<xpu, 4, real_t>(s);
          out = out_data[syncbatchnorm::kOut].get<xpu, 4, real_t>(s);
        } else {
          Shape<4> dshape = Shape4(in_data[syncbatchnorm::kData].shape_[0],
                                   in_data[syncbatchnorm::kData].shape_[1],
                                   in_data[syncbatchnorm::kData].shape_[2],
                                   in_data[syncbatchnorm::kData].shape_[3]);
          data = Tensor<xpu, 4>(workspace.dptr_, dshape, s);
          out = Tensor<xpu, 4>(workspace.dptr_ + data_size, dshape, s);
        }
      }
      if (!std::is_same<DType, real_t>::value) {
        Kernel<identity_with_cast, xpu>::Launch(
          s, data.shape_.Size(), data.dptr_, in_data[syncbatchnorm::kData].dptr<DType>());
      }
      Tensor<xpu, 1> slope = in_data[syncbatchnorm::kGamma].get<xpu, 1, real_t>(s);
      Tensor<xpu, 1> bias = in_data[syncbatchnorm::kBeta].get<xpu, 1, real_t>(s);
      Tensor<xpu, 1> moving_mean = aux_states[syncbatchnorm::kMovingMean].get<xpu, 1, real_t>(s);
      Tensor<xpu, 1> moving_var = aux_states[syncbatchnorm::kMovingVar].get<xpu, 1, real_t>(s);
  
      if (param_.fix_gamma) slope = 1.f;
  
      // whether use global statistics
      if (ctx.is_train && !param_.use_global_stats) {
        // get my rank
        Barrier *global_barrier = global_shared_barrier.Register(param_.key + "f", param_.ndev);
        int myRank = global_shared_rank.Register(param_.key + "f", param_.ndev);
        // get the mean and var
        Tensor<xpu, 1> mean = out_data[syncbatchnorm::kMean].get<xpu, 1, real_t>(s);
        Tensor<xpu, 1> var = out_data[syncbatchnorm::kVar].get<xpu, 1, real_t>(s);
        CHECK(req[syncbatchnorm::kMean] == kNullOp || req[syncbatchnorm::kMean] == kWriteTo);
        CHECK(req[syncbatchnorm::kVar] == kNullOp || req[syncbatchnorm::kVar] == kWriteTo);
        // E(x) and E(x^2)
        mean = scale * sumall_except_dim<1>(data);
        var = scale * sumall_except_dim<1>(F<mshadow_op::square>(data));
        SharedND<Tensor<cpu, 1, real_t>> *sharedMean =
          global_shared_mean.Register(param_.key, param_.ndev);
        SharedND<Tensor<cpu, 1, real_t>> *sharedVar =
          global_shared_var.Register(param_.key, param_.ndev);
        // copy to cpu, push and pull
        Tensor<cpu, 1, real_t>* mean_cpu_ptr = sharedMean->Retrieve(mean.shape_, myRank);
        Tensor<cpu, 1, real_t>* var_cpu_ptr = sharedVar->Retrieve(mean.shape_, myRank);
        Copy(*mean_cpu_ptr, mean, s);
        Copy(*var_cpu_ptr, var, s);
        sharedMean->SetReady(myRank);
        sharedVar->SetReady(myRank);
        global_barrier->Wait();
        Tensor<cpu, 1, real_t> mean_cpu = sharedMean->Pop(myRank);
        Tensor<cpu, 1, real_t> var_cpu = sharedVar->Pop(myRank);
        // copy back to gpu
        Copy(mean, mean_cpu, s);
        Copy(var, var_cpu, s);
  
        var = var-F<mshadow_op::square>(mean);
        Assign(out, req[syncbatchnorm::kOut], broadcast<1>(slope, out.shape_) *
               (data - broadcast<1>(mean, data.shape_)) /
               F<mshadow_op::square_root>(broadcast<1>(var + param_.eps, data.shape_)) +
               broadcast<1>(bias, out.shape_));
      } else {
        Assign(out, req[syncbatchnorm::kOut], broadcast<1>(slope /
                                            F<mshadow_op::square_root>(moving_var + param_.eps),
                                            data.shape_) * data +
               broadcast<1>(bias - (slope * moving_mean) /
                            F<mshadow_op::square_root>(moving_var + param_.eps), data.shape_));
      }
      if (!std::is_same<DType, real_t>::value) {
        Kernel<identity_with_cast, xpu>::Launch(
          s, out.shape_.Size(), out_data[syncbatchnorm::kOut].dptr<DType>(), out.dptr_);
      }
    });
  }

  virtual void Backward(const OpContext &ctx,
                        const std::vector<TBlob> &out_grad,
                        const std::vector<TBlob> &in_data,
                        const std::vector<TBlob> &out_data,
                        const std::vector<OpReqType> &req,
                        const std::vector<TBlob> &in_grad,
                        const std::vector<TBlob> &aux_states) {
    using namespace mshadow;
    using namespace mshadow::expr;
    using namespace mshadow_op;
    using namespace mxnet_op;
    CHECK_EQ(out_grad.size(), param_.output_mean_var ? 3U : 1U);
    CHECK_EQ(in_data.size(), 3U);
    CHECK_EQ(out_data.size(), 3U);
    CHECK_EQ(in_grad.size(), 3U);

    Stream<xpu> *s = ctx.get_stream<xpu>();
    Tensor<xpu, 4> data, grad, grad_in;
    Tensor<xpu, 1> workspace;
    const size_t data_size = in_data[syncbatchnorm::kData].Size();
    MSHADOW_TYPE_SWITCH(in_data[syncbatchnorm::kData].type_flag_, DType, {
      const bool is_double = std::is_same<DType, double>::value;
      CHECK_EQ(is_double, false)
        << "Synchronized BatchNorm does not support double-precision floating number yet...";
      size_t total_workspace_size = 0;

      Tensor<xpu, 1> mean = out_data[syncbatchnorm::kMean].get<xpu, 1, real_t>(s);
      Tensor<xpu, 1> var = out_data[syncbatchnorm::kVar].get<xpu, 1, real_t>(s);
      Tensor<xpu, 1> slope = in_data[syncbatchnorm::kGamma].get<xpu, 1, real_t>(s);
      Tensor<xpu, 1> gslope = in_grad[syncbatchnorm::kGamma].get<xpu, 1, real_t>(s);
      Tensor<xpu, 1> gbias = in_grad[syncbatchnorm::kBeta].get<xpu, 1, real_t>(s);
      // update moving avg
      Tensor<xpu, 1> moving_mean = aux_states[syncbatchnorm::kMovingMean].get<xpu, 1, real_t>(s);
      Tensor<xpu, 1> moving_var = aux_states[syncbatchnorm::kMovingVar].get<xpu, 1, real_t>(s);

      if (ctx.is_train && !param_.use_global_stats) {
        total_workspace_size += 4 * mean.shape_[0];
      }
      if (!std::is_same<DType, real_t>::value) {
        total_workspace_size += 3 * data_size;
      }

      workspace = ctx.requested[syncbatchnorm::kTempSpace].get_space<xpu, 1>(
                    mshadow::Shape1(total_workspace_size), s);
      
      const real_t scale = static_cast<real_t>(out_grad[syncbatchnorm::kOut].shape_[1]) /
        static_cast<real_t>(out_grad[syncbatchnorm::kOut].shape_.Size());
      if (in_data[syncbatchnorm::kData].ndim() == 2) {
        Shape<4> dshape = Shape4(out_grad[syncbatchnorm::kOut].shape_[0],
                                 out_grad[syncbatchnorm::kOut].shape_[1], 1, 1);        
        if (!std::is_same<DType, real_t>::value) {
          real_t* starting_ptr = (ctx.is_train && !param_.use_global_stats) ?
                                       workspace.dptr_ + 4 * mean.shape_[0] :
                                       workspace.dptr_;
          data = Tensor<xpu, 4>(starting_ptr, dshape, s);
          grad = Tensor<xpu, 4>(starting_ptr + data_size, dshape, s);
          grad_in = Tensor<xpu, 4>(starting_ptr + 2 * data_size, dshape, s);
        } else {
          data = in_data[syncbatchnorm::kData].get_with_shape<xpu, 4, real_t>(dshape, s);
          grad = out_grad[syncbatchnorm::kOut].get_with_shape<xpu, 4, real_t>(dshape, s);
          grad_in = in_grad[syncbatchnorm::kData].get_with_shape<xpu, 4, real_t>(dshape, s);
        }
      } else {
        Shape<4> dshape = Shape4(out_grad[syncbatchnorm::kOut].shape_[0],
                                 out_grad[syncbatchnorm::kOut].shape_[1],
                                 out_grad[syncbatchnorm::kOut].shape_[2],
                                 out_grad[syncbatchnorm::kOut].shape_[3]);
        if (!std::is_same<DType, real_t>::value) {
          real_t* starting_ptr = (ctx.is_train && !param_.use_global_stats) ?
                                       workspace.dptr_ + 4 * mean.shape_[0] :
                                       workspace.dptr_;
          data = Tensor<xpu, 4>(starting_ptr, dshape, s);
          grad = Tensor<xpu, 4>(starting_ptr + data_size, dshape, s);
          grad_in = Tensor<xpu, 4>(starting_ptr + 2 * data_size, dshape, s);
        } else {
          data = in_data[syncbatchnorm::kData].get<xpu, 4, real_t>(s);
          grad = out_grad[syncbatchnorm::kOut].get<xpu, 4, real_t>(s);
          grad_in = in_grad[syncbatchnorm::kData].get<xpu, 4, real_t>(s);
        }
      }

      if (!std::is_same<DType, real_t>::value) {
        Kernel<identity_with_cast, xpu>::Launch(
          s, data.shape_.Size(), data.dptr_, in_data[syncbatchnorm::kData].dptr<DType>());
        Kernel<identity_with_cast, xpu>::Launch(
          s, grad.shape_.Size(), grad.dptr_, out_grad[syncbatchnorm::kOut].dptr<DType>());
      }

      if (param_.fix_gamma) slope = 1.f;

      if (ctx.is_train && !param_.use_global_stats) {
        // get my rank
        Barrier *global_barrier = global_shared_barrier.Register(param_.key + "b", param_.ndev);
        int myRank = global_shared_rank.Register(param_.key + "b", param_.ndev);

        Shape<1> dshape = Shape1(mean.shape_[0]);
        Tensor<xpu, 1> gmean = Tensor<xpu, 1>(workspace.dptr_, dshape, s);
        Tensor<xpu, 1> gvar = Tensor<xpu, 1>(workspace.dptr_ + mean.shape_[0], dshape, s);

        moving_mean = moving_mean * param_.momentum + mean * (1 - param_.momentum);
        moving_var = moving_var * param_.momentum + var * (1 - param_.momentum);
        // cal
        Tensor<xpu, 1> sumGrad = Tensor<xpu, 1>(workspace.dptr_ + 2 * mean.shape_[0], dshape, s);
        Tensor<xpu, 1> sumProd = Tensor<xpu, 1>(workspace.dptr_ + 3 * mean.shape_[0], dshape, s);
        sumGrad = sumall_except_dim<1>(grad);
        sumProd = sumall_except_dim<1>(grad * (data - broadcast<1>(mean, data.shape_)));
        SharedND<Tensor<cpu, 1, real_t>> *sharedGrad =
          global_shared_grad.Register(param_.key, param_.ndev);
        SharedND<Tensor<cpu, 1, real_t>> *sharedProd =
          global_shared_prod.Register(param_.key, param_.ndev);
        // copy to cpu, push and pull
        Tensor<cpu, 1, real_t>* grad_cpu_ptr = sharedGrad->Retrieve(sumGrad.shape_, myRank);
        Tensor<cpu, 1, real_t>* prod_cpu_ptr = sharedProd->Retrieve(sumProd.shape_, myRank);
        Copy(*grad_cpu_ptr, sumGrad, s);
        Copy(*prod_cpu_ptr, sumProd, s);
        sharedGrad->SetReady(myRank);
        sharedProd->SetReady(myRank);
        global_barrier->Wait();
        Tensor<cpu, 1, real_t> grad_cpu = sharedGrad->Pop(myRank);
        Tensor<cpu, 1, real_t> prod_cpu = sharedProd->Pop(myRank);
        // copy back to gpu
        Copy(sumGrad, grad_cpu, s);
        Copy(sumProd, prod_cpu, s);

        gvar = -0.5f * sumProd * slope * F<mshadow_op::power>(var + param_.eps, -1.5f);
        gmean = sumGrad * slope;
        gmean *= -1.0f / F<mshadow_op::square_root>(var + param_.eps);
        // NOTICE: sum (x_i - mu_B) = 0, so the second term for dl/dmu_B can be ignored

        // assign
        if (!param_.fix_gamma) {
          Assign(gslope, req[syncbatchnorm::kGamma], sumall_except_dim<1>(grad * (data - broadcast<1>(mean, data.shape_)) /
                     F<mshadow_op::square_root>(broadcast<1>(var + param_.eps, data.shape_)))); // piggyback executor AllReduce for multi-dev summation
        } else {
          Assign(gslope, req[syncbatchnorm::kGamma], 0.0f);
        }
        Assign(grad_in, req[syncbatchnorm::kData],
               (grad * broadcast<1>(slope, data.shape_)) *
                 broadcast<1>(1.0f / F<mshadow_op::square_root>(var + param_.eps), data.shape_) +
               broadcast<1>(gvar, data.shape_) *
                 scale * 2.0f * (data - broadcast<1>(mean, data.shape_)) +
               broadcast<1>(gmean, data.shape_) * scale);
        Assign(gbias, req[syncbatchnorm::kBeta], sumall_except_dim<1>(grad)); // piggyback executor AllReduce for multi-dev summation
      } else {
        // use global statistics with freeze moving mean and var.
        if (!param_.fix_gamma) {
          Assign(gslope, req[syncbatchnorm::kGamma],
                 sumall_except_dim<1>(
                   grad * (data - broadcast<1>(moving_mean, data.shape_)) /
                   F<mshadow_op::square_root>(broadcast<1>(moving_var + param_.eps, data.shape_))));
        } else {
          Assign(gslope, req[syncbatchnorm::kGamma], 0.0f);
        }
        Assign(gbias, req[syncbatchnorm::kBeta], sumall_except_dim<1>(grad));
        Assign(grad_in, req[syncbatchnorm::kData], (grad * broadcast<1>(slope, data.shape_)) *
               broadcast<1>(
                 1.0f / F<mshadow_op::square_root>(moving_var + param_.eps), data.shape_));
      }
      if (!std::is_same<DType, real_t>::value) {
        Kernel<identity_with_cast, xpu>::Launch(
          s, grad_in.shape_.Size(), in_grad[syncbatchnorm::kData].dptr<DType>(), grad_in.dptr_);
      }
    });
  } 

 private:
  SyncBatchNormParam param_;
};  // class SyncBatchNorm

template<typename xpu>
Operator *CreateOp(SyncBatchNormParam param, int dtype);


#if DMLC_USE_CXX11
class SyncBatchNormProp : public OperatorProperty {
 public:
  void Init(const std::vector<std::pair<std::string, std::string> >& kwargs) override {
    param_.Init(kwargs);
  }

  std::map<std::string, std::string> GetParams() const override {
    return param_.__DICT__();
  }

  bool InferShape(std::vector<TShape> *in_shape,
                  std::vector<TShape> *out_shape,
                  std::vector<TShape> *aux_shape) const override {
    using namespace mshadow;
    CHECK_EQ(in_shape->size(), 3U) << "Input:[data, gamma, beta]";
    const TShape &dshape = in_shape->at(0);
    if (dshape.ndim() == 0) return false;
    in_shape->at(1) = TShape(Shape1(dshape[1]));
    in_shape->at(2) = TShape(Shape1(dshape[1]));
    out_shape->clear();
    out_shape->push_back(dshape);
    out_shape->push_back(Shape1(dshape[1]));
    out_shape->push_back(Shape1(dshape[1]));

    aux_shape->clear();
    aux_shape->push_back(Shape1(dshape[1]));
    aux_shape->push_back(Shape1(dshape[1]));
    return true;
  }

  bool InferType(std::vector<int> *in_type,
                 std::vector<int> *out_type,
                 std::vector<int> *aux_type) const override {
    using namespace mshadow;
    CHECK_GE(in_type->size(), 1U);
    int dtype = (*in_type)[0];
    CHECK_NE(dtype, -1) << "First input must have specified type";
    // For float16 input type beta, gamma, mean, and average are stored in float32.
    // For other input types, these parameters have the same type as input
    // NOTE: This requirement is from cuDNN (v. 4 and 5)
    int dtype_param = (dtype == kFloat16) ? kFloat32 : dtype;
    for (index_t i = 1; i < in_type->size(); ++i) {
      if ((*in_type)[i] == -1) {
        (*in_type)[i] = dtype_param;
      } else {
        UNIFORM_TYPE_CHECK((*in_type)[i], dtype_param, ListArguments()[i]);
      }
    }
    for (index_t i = 0; i < aux_type->size(); ++i) {
      if ((*aux_type)[i] != -1) {
        UNIFORM_TYPE_CHECK((*aux_type)[i], dtype_param, ListArguments()[i]);
      }
    }
    int n_aux = this->ListAuxiliaryStates().size();
    aux_type->clear();
    for (int i = 0; i < n_aux; ++i ) aux_type->push_back(dtype_param);
    int n_out = this->ListOutputs().size();
    out_type->clear();
    out_type->push_back(dtype);
    for (int i = 1; i < n_out; ++i ) out_type->push_back(dtype_param);
    return true;
  }

  OperatorProperty* Copy() const override {
    auto ptr = new SyncBatchNormProp();
    ptr->param_ = param_;
    return ptr;
  }

  std::string TypeString() const override {
    return "_contrib_SyncBatchNorm";
  }

  std::vector<ResourceRequest> ForwardResource(
      const std::vector<TShape> &in_shape) const override {
    return {ResourceRequest::kTempSpace};
  }

  std::vector<int> DeclareBackwardDependency(
    const std::vector<int> &out_grad,
    const std::vector<int> &in_data,
    const std::vector<int> &out_data) const override {
    return {out_grad[syncbatchnorm::kOut],
            out_data[syncbatchnorm::kMean],
            out_data[syncbatchnorm::kVar],
            in_data[syncbatchnorm::kData],
            in_data[syncbatchnorm::kGamma]
           };
  }

  std::vector<ResourceRequest> BackwardResource(
      const std::vector<TShape> &in_shape) const override {
    return {ResourceRequest::kTempSpace};
  }

  int NumVisibleOutputs() const override {
    if (param_.output_mean_var) {
      return 3;
    }
    return 1;
  }

  int NumOutputs() const override {
    return 3;
  }

  std::vector<std::string> ListArguments() const override {
    return {"data", "gamma", "beta"};
  }

  std::vector<std::string> ListOutputs() const override {
    return {"output", "mean", "var"};
  }

  std::vector<std::string> ListAuxiliaryStates() const override {
    return {"moving_mean", "moving_var"};
  }

  Operator* CreateOperator(Context ctx) const override {
      LOG(FATAL) << "Not Implemented.";
      return NULL;
  }

  Operator* CreateOperatorEx(Context ctx, std::vector<TShape> *in_shape,
      std::vector<int> *in_type) const override;

  inline const SyncBatchNormParam& getParam() const {
    return param_;
  }

 private:
  SyncBatchNormParam param_;
};  // class SyncBatchNormProp

#endif  // DMLC_USE_CXX11
}  // namespace op
}  // namespace mxnet
#endif  // MXNET_OPERATOR_CONTRIB_SYNC_BATCH_NORM_INL_H_
