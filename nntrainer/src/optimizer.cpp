/**
 * Copyright (C) 2020 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *   http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *
 * @file	optimizer.cpp
 * @date	08 April 2020
 * @brief	This is Implementation of Optimizer class
 * @see		https://github.com/nnstreamer/nntrainer
 * @author	Jijoong Moon <jijoong.moon@samsung.com>
 * @bug		No known bugs except for NYI items
 *
 */

#include "optimizer.h"
#include <nntrainer_log.h>
#include "util_func.h"

namespace nntrainer {

void Optimizer::initialize(unsigned int height, unsigned int width, bool set_tensor) {
  if (type == OptType::adam && set_tensor) {
    wm = Tensor(height, width);
    wv = Tensor(height, width);
    wm.setZero();
    wv.setZero();
    bm = Tensor(1, width);
    bv = Tensor(1, width);
    bm.setZero();
    bv.setZero();
  }
}

void Optimizer::calculate(Tensor& djdw, Tensor& djdb, Tensor& weight, Tensor& bias, int iteration, bool init_zero) {
  if (popt.weight_decay.type == WeightDecayType::l2norm) {
    djdw = djdw.add(weight.multiply(popt.weight_decay.lambda));
  }

  float ll = popt.learning_rate;
  if (popt.decay_steps != -1) {
    ll = ll * pow(popt.decay_rate, (iteration / popt.decay_steps));
  }

  switch (type) {
    case OptType::sgd:
      weight = weight.subtract(djdw.average().multiply(ll));
      break;
    case OptType::adam:
      wm = wm.multiply(popt.beta1).add(djdw.average().multiply(1 - popt.beta1));
      wv = wv.multiply(popt.beta2).add((djdw.average().multiply(djdw.average())).multiply(1 - popt.beta2));
      wm.divide(1 - pow(popt.beta1, iteration + 1));
      wv.divide(1 - pow(popt.beta2, iteration + 1));
      weight = weight.subtract((wm.divide(wv.apply(sqrtFloat).add(popt.epsilon))).multiply(ll));
      bm = bm.multiply(popt.beta1).add(djdb.average().multiply(1 - popt.beta1));
      bv = bv.multiply(popt.beta2).add((djdb.average().multiply(djdb.average())).multiply(1 - popt.beta2));
      bm.divide(1 - pow(popt.beta1, iteration + 1));
      bv.divide(1 - pow(popt.beta2, iteration + 1));
      bias = bias.subtract((bm.divide(bv.apply(sqrtFloat).add(popt.epsilon))).multiply(ll));
      break;
    default:
      break;
  }

  if (init_zero) {
    bias = bias.subtract(djdb.average().multiply(ll));
  }
}
} /* namesapce nntrainer */
