/*********************************************************
 *
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
 *
 ************************************************************/
#include "cudnn_batchnorm.h"
#ifdef USE_CUDNN

namespace singa {

RegisterLayerClass(cudnn_batchnorm, CudnnBatchNorm);
CudnnBatchNorm::~CudnnBatchNorm() {
  if (has_init_cudnn_) {
    CUDNN_CHECK(cudnnDestroyTensorDescriptor(shape_desc_));
    CUDNN_CHECK(cudnnDestroyTensorDescriptor(param_desc_));
  }
}

void CudnnBatchNorm::ToDevice(std::shared_ptr<Device> device) {
  BatchNorm::ToDevice(device);
  resultSaveMean_.ToDevice(device);
  resultSaveVariance_.ToDevice(device);
}

void CudnnBatchNorm::Setup(const Shape& in_sample, const LayerConf& conf) {
  BatchNorm::Setup(in_sample, conf);
  resultSaveMean_.Resize(Shape{channels_});
  resultSaveVariance_.Resize(Shape{channels_});
}

void CudnnBatchNorm::InitCudnn(const Shape& shape, DataType dtype) {
  if (!has_init_cudnn_) {
    if (is_2d_)
      mode_ = CUDNN_BATCHNORM_PER_ACTIVATION;
    else
      mode_ = CUDNN_BATCHNORM_SPATIAL;
    CUDNN_CHECK(cudnnCreateTensorDescriptor(&shape_desc_));
    CUDNN_CHECK(cudnnCreateTensorDescriptor(&param_desc_));
  }
  CHECK_EQ(shape.size(), 4u);
  CUDNN_CHECK(cudnnSetTensor4dDescriptor(shape_desc_, CUDNN_TENSOR_NCHW,
                                         GetCudnnDataType(dtype), shape[0],
                                         shape[1], shape[2], shape[3]));
  CUDNN_CHECK(cudnnSetTensor4dDescriptor(param_desc_, CUDNN_TENSOR_NCHW,
                                         GetCudnnDataType(dtype), 1, shape[1],
                                         1, 1));
  has_init_cudnn_ = true;
}
const Tensor CudnnBatchNorm::Forward(int flag, const Tensor& input) {
  auto shape = input.shape();
  auto dtype = input.data_type();
  Tensor output;
  Tensor x;
  if (is_2d_)
    x = Reshape(input, Shape{shape.at(0), shape.at(1), 1, 1});
  else
    x = input;
  shape = x.shape();
  if (!has_init_cudnn_) {
    InitCudnn(shape, dtype);
  } else {
    int n, c, h, w, s;
    cudnnDataType_t type;
    CUDNN_CHECK(cudnnGetTensor4dDescriptor(shape_desc_, &type, &n, &c, &h, &w,
                                           &s, &s, &s, &s));
    if (shape[0] != static_cast<size_t>(n)) InitCudnn(shape, dtype);
    CHECK(shape[1] == static_cast<size_t>(c) &&
          shape[2] == static_cast<size_t>(h) &&
          shape[3] == static_cast<size_t>(w))
        << "input sample shape should not change" << "previous shape " << c
        << ", " << h << ", " << w << "current shape " << shape[1] << ", "
        << shape[2] << ", " << shape[3];
  }

  // TODO(wangji): check device id of input and params
  output.ResetLike(x);
  if ((flag & kTrain) == kTrain) {
    output.device()->Exec(
        [=](Context* ctx) {
          Block *inBlock = x.block(), *outBlock = output.block(),
                *saveMeanBlock = resultSaveMean_.block(),
                *saveVarBlock = resultSaveVariance_.block(),
                *runningMeanBlock = runningMean_.block(),
                *runningVarBlock = runningVariance_.block(),
                *bnScaleBlock = bnScale_.block(),
                *bnBiasBlock = bnBias_.block();
          const float alpha = 1.0f, beta = 0.0f;
          double epsilon = CUDNN_BN_MIN_EPSILON;
          CUDNN_CHECK(cudnnBatchNormalizationForwardTraining(
              ctx->cudnn_handle, this->mode_, &alpha, &beta, shape_desc_,
              inBlock->data(), shape_desc_, outBlock->mutable_data(),
              param_desc_, bnScaleBlock->data(), bnBiasBlock->data(), factor_,
              runningMeanBlock->mutable_data(), runningVarBlock->mutable_data(),
              epsilon, saveMeanBlock->mutable_data(),
              saveVarBlock->mutable_data()));
        },
        {x.block(), bnScale_.block(), bnBias_.block()},
        {output.block(), runningMean_.block(), runningVariance_.block(),
         resultSaveMean_.block(), resultSaveVariance_.block()});
    buf_.push(x);
  } else {
    output.device()->Exec(
        [=](Context* ctx) {
          Block *inBlock = x.block(), *outBlock = output.block(),
                *runningMeanBlock = runningMean_.block(),
                *runningVarBlock = runningVariance_.block(),
                *bnScaleBlock = bnScale_.block(),
                *bnBiasBlock = bnBias_.block();
          const float alpha = 1.0f, beta = 0.0f;
          double epsilon = CUDNN_BN_MIN_EPSILON;
          CUDNN_CHECK(cudnnBatchNormalizationForwardInference(
              ctx->cudnn_handle, this->mode_, &alpha, &beta, shape_desc_,
              inBlock->data(), shape_desc_, outBlock->mutable_data(),
              param_desc_, bnScaleBlock->data(), bnBiasBlock->data(),
              runningMeanBlock->data(), runningVarBlock->data(), epsilon));
        },
        {x.block(), bnScale_.block(), bnBias_.block(), runningMean_.block(),
         runningVariance_.block()},
        {output.block()});
  }
  if (is_2d_) output.Reshape(Shape{shape.at(0), shape.at(1)});
  return output;
}

const std::pair<Tensor, vector<Tensor>> CudnnBatchNorm::Backward(
    int flag, const Tensor& grad) {
  vector<Tensor> param_grad;
  Tensor dx;
  if ((flag & kTrain) == kTrain) {
    Tensor x = buf_.top();
    buf_.pop();
    dx.ResetLike(grad);
    dx.device()->Exec(
        [=](Context* ctx) {
          Block *dyblock = grad.block(), *dxblock = dx.block(),
                *xblock = x.block(), *bnScaleBlock = bnScale_.block(),
                *dbnScaleBlock = dbnScale_.block(),
                *dbnBiasBlock = dbnBias_.block(),
                *saveMeanBlock = resultSaveMean_.block(),
                *saveVarBlock = resultSaveVariance_.block();
          const float alpha = 1.0f, beta = .0f;
          double epsilon = CUDNN_BN_MIN_EPSILON;
          CUDNN_CHECK(cudnnBatchNormalizationBackward(
              ctx->cudnn_handle, this->mode_, &alpha, &beta, &alpha, &beta,
              shape_desc_, xblock->data(), shape_desc_, dyblock->data(),
              shape_desc_, dxblock->mutable_data(), param_desc_,
              bnScaleBlock->data(), dbnScaleBlock->mutable_data(),
              dbnBiasBlock->mutable_data(), epsilon, saveMeanBlock->data(),
              saveVarBlock->data()));
        },
        {x.block(), grad.block(), bnScale_.block(), resultSaveMean_.block(),
         resultSaveVariance_.block()},
        {dx.block(), dbnScale_.block(), dbnBias_.block()});
  } else {
    LOG(ERROR) << "Do not call backward for evaluation phase";
  }
  param_grad.push_back(dbnScale_);
  param_grad.push_back(dbnBias_);
  Tensor dummy;
  param_grad.push_back(dummy);
  param_grad.push_back(dummy);
  if (is_2d_) dx.Reshape(Shape{dx.shape().at(0), dx.shape().at(1)});
  return std::make_pair(dx, param_grad);
}
}  // namespace singa

#endif  // USE_CUDNN
