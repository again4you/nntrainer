// SPDX-License-Identifier: Apache-2.0
/**
 * Copyright (C) 2020 Jijoong Moon <jijoong.moon@samsung.com>
 *
 * @file    network_graph.h
 * @date    19 Oct 2020
 * @see     https://github.com/nnstreamer/nntrainer
 * @author  Jijoong Moon <jijoong.moon@samsung.com>
 * @bug     No known bugs except for NYI items
 * @brief   This is Network Graph Class for Neural Network
 *
 */

#include <sstream>

#include <activation_layer.h>
#include <addition_layer.h>
#include <bn_layer.h>
#include <concat_layer.h>
#include <flatten_layer.h>
#include <input_layer.h>
#include <layer_factory.h>
#include <loss_layer.h>
#include <network_graph.h>
#include <nntrainer_error.h>
#include <nntrainer_log.h>
#include <output_layer.h>
#include <parse_util.h>
#include <profiler.h>

namespace nntrainer {

/**
 * @todo Make inPlace as a static property of the layer and a state to verify if
 * this layer is working in-place
 */
static const std::vector<std::string> in_place_layers = {
  ActivationLayer::type, BatchNormalizationLayer::type};

void NetworkGraph::updateNameInLayers(
  std::vector<std::shared_ptr<Layer>> &layers, const std::string &cname,
  const std::string &name) {
  for (unsigned int i = 0; i < layers.size(); ++i) {
    for (unsigned int j = 0; j < layers[i]->input_layers.size(); ++j) {
      if (istrequal(layers[i]->input_layers[j], cname)) {
        layers[i]->input_layers[j] = name;
        return;
      }
    }
  }
}

void NetworkGraph::addEdge(unsigned int ith, LayerNode node) {
  if (ith > num_node - 1)
    throw std::invalid_argument("Exceed total number of layer");

  adj[ith].push_back(node);
}

void NetworkGraph::addLayerNode(std::shared_ptr<Layer> layer) {
  std::list<LayerNode> l;
  std::unique_ptr<LayerNode> node = std::make_unique<LayerNode>();

  ensureName(layer);

  node->layer = layer;
  node->index = num_node;

  l.assign(1, *node);
  adj.push_back(l);
  num_node++;
}

void NetworkGraph::topologicalSortUtil(unsigned int ith,
                                       std::vector<bool> &visited,
                                       std::stack<LayerNode> &Stack) {
  visited[ith] = true;

  std::list<LayerNode>::iterator i;
  for (i = adj[ith].begin(); i != adj[ith].end(); ++i) {
    if (!visited[(*i).index])
      topologicalSortUtil((*i).index, visited, Stack);
  }

  Stack.push(getLayerNode(ith));
}

LayerNode &NetworkGraph::getLayerNode(unsigned int ith) {

  std::list<LayerNode>::iterator iter;
  for (unsigned int i = 0; i < num_node; ++i) {
    iter = adj[i].begin();
    if ((*iter).index == ith) {
      return *iter;
    }
  }

  throw std::invalid_argument("Cannot find Layer");
}

LayerNode &NetworkGraph::getSortedLayerNode(unsigned int ith) {

  for (unsigned int i = 0; i < Sorted.size(); ++i) {
    if (Sorted[i].index == ith) {
      return Sorted[i];
    }
  }

  throw std::invalid_argument("Cannot find Layer");
}

void NetworkGraph::countNonTrainableLayersAtBegin() {
  /** TODO: update for multiple inputs when it is supported */
  for (auto iter = Sorted.cbegin(); iter != Sorted.cend(); iter++) {
    if ((*iter).layer->getTrainable()) {
      skip_non_trainable_layers = iter - Sorted.cbegin();
      break;
    }
  }
}

void NetworkGraph::topologicalSort() {
  std::stack<LayerNode> Stack;
  std::vector<bool> visited(num_node);

  std::fill(visited.begin(), visited.end(), false);

  // TODO : After make node list of graph, we have to find root. (That means it
  // should be the only one input for now.). Need to support multiple input and
  // support search.

  for (unsigned int i = 0; i < num_node; ++i) {
    if (visited[i] == false) {
      topologicalSortUtil(i, visited, Stack);
    }
  }

  while (Stack.empty() == false) {
    Sorted.push_back(Stack.top());
    Stack.pop();
  }

  /** TODO: this will be replaced with a corresponding graph function */
  countNonTrainableLayersAtBegin();
}

void NetworkGraph::ensureName(std::shared_ptr<Layer> layer,
                              const std::string &prefix, bool force_rename) {
  std::string orig_name = layer->getName();
  bool orig_name_empty = orig_name.empty();
  if (!orig_name_empty && !force_rename &&
      layer_names.end() == layer_names.find(orig_name))
    return;

  /** If just prefix with layer name makes it unique - directly set the name */
  if (!orig_name_empty) {
    std::string direct_name = prefix + orig_name;
    if (layer_names.find(direct_name) == layer_names.end()) {
      layer->setName(direct_name);
      return;
    }
  }

  std::set<std::string>::iterator iter;
  std::string name;
  if (orig_name_empty)
    orig_name = layer->getType();
  std::string direct_name = prefix + orig_name;

  do {
    name = direct_name + std::to_string(def_name_count++);
    iter = layer_names.find(name);
  } while (iter != layer_names.end());

  layer->setName(name);
}

int NetworkGraph::realizeMultiInputType(Layer &current) {
  int status = ML_ERROR_NONE;
  if (current.num_inputs == 1)
    return ML_ERROR_NONE;

  std::shared_ptr<Layer> layer = nntrainer::createLayer(AdditionLayer::type);
  ensureName(layer, current.getName());
  layer->num_inputs = current.num_inputs;
  layer->input_dim.resize(layer->num_inputs);
  layer->input_layers.clear();
  for (unsigned int i = 0; i < current.input_layers.size(); ++i)
    layer->input_layers.push_back(current.input_layers[i]);

  current.num_inputs = 1;
  current.input_layers.clear();
  current.input_layers.push_back(layer->getName());
  addLayerNode(layer);

  return status;
}

int NetworkGraph::realizeFlattenType(
  Layer &current, std::vector<std::shared_ptr<Layer>> &layers) {
  if (num_node == 0) {
    ml_loge("layer is empty");
    return ML_ERROR_INVALID_PARAMETER;
  }

  if (current.getType() == FlattenLayer::type) {
    ml_loge(
      "It is not allowed to realize flatten layer, possibly flatten layer is "
      "added right after flatten");
    return ML_ERROR_INVALID_PARAMETER;
  }

  std::shared_ptr<Layer> layer = nntrainer::createLayer(FlattenLayer::type);

  ensureName(layer, current.getName());

  layer->num_inputs = 1;
  layer->input_layers.clear();
  layer->input_layers.push_back(current.getName());

  addLayerNode(layer);

  return ML_ERROR_NONE;
}

int NetworkGraph::realizeActivationType(
  Layer &current, std::vector<std::shared_ptr<Layer>> &layers) {
  int status = ML_ERROR_NONE;

  ActivationType act = current.getActivationType();

  if (act == ActivationType::ACT_NONE) {
    /// ActivationType::ACT_NONE does not need realization
    return ML_ERROR_NONE;
  }

  if (num_node == 0) {
    ml_loge("layer is empty");
    return ML_ERROR_INVALID_PARAMETER;
  }

  if (current.getType() == ActivationLayer::type) {
    ml_loge("It is not allowed to realize ativation layer, possibly layer is "
            "added right after activation");
    return ML_ERROR_INVALID_PARAMETER;
  }

  if (act == ActivationType::ACT_UNKNOWN) {
    ml_loge("cannot realize unknown activation type");
    return ML_ERROR_INVALID_PARAMETER;
  }

  std::shared_ptr<Layer> layer = nntrainer::createLayer("activation");

  ensureName(layer, current.getName());
  layer->setActivation(act);
  layer->num_inputs = 1;
  layer->input_layers.clear();
  layer->input_layers.push_back(current.getName());

  if (current.num_outputs != 1)
    return ML_ERROR_INVALID_PARAMETER;

  layer->num_outputs = current.num_outputs;
  layer->output_dim.resize(layer->num_outputs);
  layer->output_layers.clear();
  for (unsigned int i = 0; i < current.num_outputs; ++i)
    layer->output_layers.push_back(current.output_layers[i]);

  current.num_outputs = 1;
  current.output_layers.clear();
  current.output_layers.push_back(layer->getName());

  addLayerNode(layer);

  updateNameInLayers(layers, current.getName(), layer->getName());

  return status;
}

int NetworkGraph::addLossLayer(const LossType loss_type) {
  int status = ML_ERROR_NONE;
  LossType updated_loss_type = loss_type;
  if (num_node == 0) {
    status = ML_ERROR_INVALID_PARAMETER;
    NN_RETURN_STATUS();
  }

  if (updated_loss_type == LossType::LOSS_ENTROPY) {
    if (getLayerNode(num_node - 1).layer->getType() != "activation") {
      ml_loge("Error: Cross Entropy need last layer to have softmax or sigmoid "
              "activation.");
      return ML_ERROR_NOT_SUPPORTED;
    }

    LayerNode act_layer_node = getLayerNode(num_node - 1);
    adj.pop_back();
    num_node--;

    switch (act_layer_node.layer->getActivationType()) {
    case ActivationType::ACT_SIGMOID:
      updated_loss_type = LossType::LOSS_ENTROPY_SIGMOID;
      break;
    case ActivationType::ACT_SOFTMAX:
      updated_loss_type = LossType::LOSS_ENTROPY_SOFTMAX;
      break;
    default:
      ml_loge("Error: Cross Entropy not supported without softmax or sigmoid.");
      return ML_ERROR_NOT_SUPPORTED;
    }
  }

  std::string input_str = getLayerNode(num_node - 1).layer->getName();

  std::shared_ptr<LossLayer> layer = std::make_unique<LossLayer>();

  ensureName(layer);

  LayerNode last_node = getLayerNode(num_node - 1);
  last_node.layer->num_outputs = 1;
  last_node.layer->output_layers.clear();
  last_node.layer->output_layers.push_back(layer->getName());

  layer->num_inputs = 1;
  layer->input_layers.clear();
  layer->input_layers.push_back(input_str);

  if (layer->output_layers.size() == 0) {
    layer->num_outputs = 1;
    layer->output_dim.resize(1);
    layer->output_layers.push_back("__exit__");
  }

  status = layer->setLoss(updated_loss_type);
  NN_RETURN_STATUS();

  addLayerNode(layer);

  return ML_ERROR_NONE;
}

void NetworkGraph::setOutputLayers(
  std::vector<std::shared_ptr<Layer>> &layers) {

  for (unsigned int idx = 0; idx < layers.size(); ++idx) {
    for (unsigned int i = 0; i < layers.size(); ++i) {
      if (istrequal(layers[i]->getName(), layers[idx]->getName()))
        continue;
      for (unsigned int j = 0; j < layers[i]->input_layers.size(); ++j) {
        if (istrequal(layers[i]->input_layers[j], layers[idx]->getName())) {
          for (unsigned int k = 0; k < layers[idx]->output_layers.size(); ++k) {
            if (!istrequal(layers[idx]->output_layers[k], layers[i]->getName()))
              continue;
          }
          layers[idx]->output_layers.push_back(layers[i]->getName());
        }
      }
    }
    if (layers[idx]->num_outputs != layers[idx]->output_layers.size()) {
      layers[idx]->num_outputs = layers[idx]->output_layers.size();
      layers[idx]->output_dim.resize(layers[idx]->output_layers.size());
    }
  }

  if (layers.back()->num_outputs == 0) {
    layers.back()->num_outputs = 1;
    layers.back()->output_dim.resize(1);
    layers.back()->output_layers.push_back("__exit__");
  }

  for (auto idx = layers.begin(); idx < layers.end(); ++idx) {
    if ((*idx)->output_layers.size() == 0)
      throw std::invalid_argument("There is un-connected node");
  }
}

int NetworkGraph::realizeMultiOutputType(
  Layer &current, std::vector<std::shared_ptr<Layer>> &layers) {
  int status = ML_ERROR_NONE;
  if (current.num_outputs == 1)
    return ML_ERROR_NONE;

  std::shared_ptr<Layer> layer = nntrainer::createLayer(OutputLayer::type);
  ensureName(layer, current.getName());

  layer->num_inputs = 1;
  layer->input_layers.clear();
  layer->input_layers.push_back(current.getName());

  layer->num_outputs = current.num_outputs;
  layer->output_layers.clear();

  for (unsigned int i = 0; i < current.output_layers.size(); ++i) {
    layer->output_layers.push_back(current.output_layers[i]);

    updateNameInLayers(layers, current.getName(), layer->getName());
  }

  current.num_outputs = 1;
  current.output_layers.clear();
  current.output_layers.push_back(layer->getName());
  addLayerNode(layer);

  return status;
}

int NetworkGraph::setGraphNode(std::vector<std::shared_ptr<Layer>> &layers,
                               const LossType loss_type) {

  int status = ML_ERROR_NONE;

  setOutputLayers(layers);

  for (unsigned int i = 0; i < layers.size(); ++i) {
    Layer &l = *layers[i];
    ml_logd("layer name: %s", l.getName().c_str());

    if (l.input_layers.size() < 1) {
      for (unsigned int i = 0; i < l.getInputDimension().size(); ++i) {
        if (l.getInputDimension()[i].getDataLen() == 0)
          throw std::invalid_argument("Input Dimension must be set");
      }

      l.num_inputs = 1;
      l.input_layers.clear();
      l.input_layers.push_back("__data__");
    }

    if (l.getType() != AdditionLayer::type &&
        l.getType() != ConcatLayer::type) {
      status = realizeMultiInputType(l);
      NN_RETURN_STATUS();
    }

    addLayerNode(layers[i]);

    if (l.getType() != ActivationLayer::type) {
      status = realizeActivationType(l, layers);
      NN_RETURN_STATUS();
    }

    if (l.getType() != OutputLayer::type) {
      status = realizeMultiOutputType(l, layers);
      NN_RETURN_STATUS();
    }

    if (l.getFlatten()) {
      status = realizeFlattenType(l, layers);
      NN_RETURN_STATUS();
    }
  }

  if (layers.back()->getType() != LossLayer::type &&
      loss_type != LossType::LOSS_NONE) {
    status = addLossLayer(loss_type);
    NN_RETURN_STATUS();
  }

  return status;
}

void NetworkGraph::setNumNetBufferSize() {
  for (unsigned int i = 0; i < Sorted.size(); ++i) {
    bool first = i == 0;
    bool last = i == Sorted.size() - 1;
    if (first) {
      Sorted[i].layer->net_input.resize(Sorted[i].layer->num_inputs);
    } else {
      Sorted[i].layer->net_input.resize(Sorted[i].layer->input_layers.size());
    }

    if (last) {
      Sorted[i].layer->net_hidden.resize(Sorted[i].layer->num_outputs);
    } else {
      Sorted[i].layer->net_hidden.resize(Sorted[i].layer->output_layers.size());
    }
  }
}

LayerNode &NetworkGraph::getLayerNode(const std::string &layer_name) {

  std::list<LayerNode>::iterator iter;
  for (unsigned int i = 0; i < adj.size(); ++i) {
    iter = adj[i].begin();
    if (istrequal((*iter).layer->getName(), layer_name)) {
      return (*iter);
    }
  }

  throw std::invalid_argument("Cannot find Layer");
}

int NetworkGraph::setEdge() {
  int status = ML_ERROR_NONE;

  std::list<LayerNode>::iterator iter;
  for (unsigned int i = 0; i < adj.size(); ++i) {
    iter = adj[i].begin();

    if ((*iter).layer->getInputDimension()[0].getDataLen() != 0)
      continue;

    for (unsigned int j = 0; j < (*iter).layer->input_layers.size(); ++j) {
      if (istrequal((*iter).layer->input_layers[j], "__data__"))
        continue;
      unsigned int to_node_id =
        getLayerNode((*iter).layer->input_layers[j]).index;
      addEdge(to_node_id, (*iter));
    }
  }

  return status;
}

void NetworkGraph::setBatchSize(unsigned int batch_size) {
  for (auto const &layer_node : Sorted) {
    layer_node.layer->setBatch(batch_size);
  }
}

sharedConstTensors NetworkGraph::forwarding(bool training) {
  for (auto const &ln : Sorted) {
    START_PROFILE(ln.event_key);
    ln.layer->forwarding(training);
    END_PROFILE(ln.event_key);
  }

  std::vector<sharedConstTensor> out;
  for (auto const &nh : Sorted.back().layer->net_hidden)
    out.push_back(MAKE_SHARED_TENSOR(nh->getVariable()));

  return out;
}

std::vector<TensorDim> NetworkGraph::getInputDimension() {
  return Sorted[0].layer->getInputDimension();
}

std::vector<TensorDim> NetworkGraph::getOutputDimension() {
  return Sorted.back().layer->getOutputDimension();
}

void NetworkGraph::inPlaceOptimize(const std::string &layer_type,
                                   Manager &manager) {
  for (auto &layer_node : Sorted) {
    auto &l = layer_node.layer;
    if (l->getType() == layer_type &&
        l->getActivationType() != ActivationType::ACT_SOFTMAX) {
      /** @note assumes layer to be optimized is only for single in/out tensor
       */
      if (l->input_layers.size() != 1)
        throw std::runtime_error("Internal error in the formed graph");

      auto &prev_layer = getLayerNode(l->input_layers[0]).layer;

      unsigned int loc;
      auto layer_name = l->getName();
      for (loc = 0; loc < prev_layer->output_layers.size(); ++loc)
        if (prev_layer->output_layers[loc] == layer_name)
          break;

      if (loc == prev_layer->output_layers.size())
        throw std::runtime_error("Internal error in the formed graph.");

      if (prev_layer->getType() == InputLayer::type)
        continue;

      /** check if previous layer was also in-place */
      bool prev_layer_in_place = false;
      for (auto const &in_place_layer : in_place_layers) {
        if (prev_layer->getType() == in_place_layer) {
          prev_layer_in_place = true;
          break;
        }
      }

      /** Two layers cant work in-place consecutively */
      if (prev_layer_in_place)
        continue;

      /** Share tensor with next layer */
      /**
       * Assume two layers, L1 and L2, with I and O corresponding to the layer
       * outputs. Assume L2 to be the layer, needing in-place optimization.
       */
      if (l->getType() == BatchNormalizationLayer::type) {
        /**
         * With batch normalization, neither input nor output of the layer are
         * requried for calculatin gradient and derivative. Just input
         * derivative is required. In this scenraio, L2 is assumed to be batch
         * normaliztion layer, and L1 is assumed to be a non-in-place layer.
         * Hence, L1 layer's output and L2 layer's input var_grad are modified.
         */
        auto &inplace_shared_vg_ptr = l->net_hidden[0];
        l->net_input[0] = inplace_shared_vg_ptr;             /// I2 = O2
        prev_layer->net_hidden[loc] = inplace_shared_vg_ptr; /// O1 = O2
      } else if (l->getType() == ActivationLayer::type) {
        /**
         * For activation layer, output of the layer and input derivative, both
         * , are requried for calculating the gradient and derivative. In this
         * scenraio, L2 is assumed to be activation layer, and L1 is assumed to
         * be a non-in-place layer.
         * Hence, L1 layer's output and L2 layer's input var_grad are
         * differently. L1 layer operates out of place and share the memory for
         * its output (O1.V) and input derivative (O1.G). L2 layer operates
         * in-place and use a common shared derivative memory for their
         * derivatives (handled in manager.cpp).
         */
        /**
         * @note As this updates the tensors used in the prev_layer->net_hidden
         * (O1), the tensors inside l->net_input (I2) are automatically updated
         * as they share the same var_grad object.
         */
        auto &inplace_shared_vg = *l->net_hidden[0].get();
        prev_layer->net_hidden[loc]->updateVariableByVariable(
          inplace_shared_vg); /// O1.G = O2.V
        prev_layer->net_hidden[loc]->updateGradientByVariable(
          inplace_shared_vg); /// O1.V = O2.V
      } else {
        std::stringstream ss;
        ss << l->getType();
        ss << " layer is not supported for in-place optimization";
        throw std::runtime_error(ss.str());
      }

      /** Untrack the memory for this layer */
      manager.untrackLayerInOuts(prev_layer->getName());
    }
  }
}

void NetworkGraph::inPlaceOptimize(Manager &manager) {
  for (auto const &layer_type : in_place_layers)
    inPlaceOptimize(layer_type, manager);
}

} /* namespace nntrainer */
