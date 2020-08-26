#include "training/graph_group.h"

namespace marian {

GraphGroup::GraphGroup(Ptr<Options> options, const std::vector<DeviceId> devices)
  : options_(options),
    devices_(devices),
    mbRoundUp_(options_->get<bool>("mini-batch-round-up", true)) {
  if(options_->hasAndNotEmpty("cost-scaling")) {
    auto vcs = options_->get<std::vector<std::string>>("cost-scaling");
    costScale_ = true;
    float costExponent = std::stof(vcs[0]);
    costScaleFactor_ = std::pow(2.0f, costExponent);
    
    if(vcs.size() > 1) costScaleFreq_ = std::stoul(vcs[1]);
    if(vcs.size() > 2) costScaleMultiplier_ = std::stof(vcs[2]);
    if(vcs.size() > 3) costScaleNanTolerance_ = std::stof(vcs[3]);
    if(vcs.size() > 4) costScaleNanRange_ = std::stoul(vcs[4]);
    if(vcs.size() > 5) costScaleFactorMinimum_ = std::stof(vcs[5]);
    

    LOG_ONCE(info,
             "Training with cost scaling - factor: 2^{} = {}, frequency: {}, multiplier: {}, tolerance: {}, range: {}, minimum: {}",
             costExponent,
             costScaleFactor_,
             costScaleFreq_,
             costScaleMultiplier_,
             costScaleNanTolerance_,
             costScaleNanRange_,
             costScaleFactorMinimum_);
  }

  if(options_->hasAndNotEmpty("dynamic-gradient-scaling")) {
    auto vgc = options_->get<std::vector<std::string>>("dynamic-gradient-scaling");
    dynamicGradientScaling_ = true;

    if(vgc.size() > 0) dynamicGradientScalingFactor_  = std::stof(vgc[0]);
    if(vgc.size() > 1) dynamicGradientScalingUseLogs_ = vgc[1] == "log";

    LOG_ONCE(info,
             "Re-scaling gradient to have average gradient norm if (log={}) gradient norm diverges from average by {} sigmas",
             dynamicGradientScalingUseLogs_,
             dynamicGradientScalingFactor_);
  }

  if(options_->get<bool>("check-gradient-nan")) {
    checkGradientNan_ = true;
    LOG_ONCE(info, "Checking gradient for NaN");
  }
}

GraphGroup::GraphGroup(Ptr<Options> options)
  : GraphGroup(options, Config::getDevices(options)) {}

void GraphGroup::initGraphs() {
  for(auto device : devices_) {
    auto graph = New<ExpressionGraph>();
    
    // @TODO: validate precisions in config
    auto precisions = options_->get<std::vector<std::string>>("precision");
    Type parameterType = typeFromString(precisions[0]);
    // Type saveType = typeFromString(precisions[2]); // @TODO: respect this

    graph->setDefaultElementType(parameterType);
    graph->setCheckpointing(options_->get<bool>("gradient-checkpointing"));

    if(options_->get<bool>("check-nan")) // @TODO: add to other places
      graph->setThrowNaN(true);

    graph->setDevice(device);
    
    graph->reserveWorkspaceMB(options_->get<size_t>("workspace"));
    graph->getBackend()->setClip(options_->get<float>("clip-gemm"));

    graphs_.push_back(graph);
  }
}

// increase cost-scaling factor if no NaN has been detected for a
// given number of iterations. Usually we increase by 2 which adds
// one more bit for precision.
void GraphGroup::increaseCostScaleFactor() {
  if(!costScale_)
    return;

  noNanSeen_++;

  size_t total = nanSeen_ + noNanSeen_;
  float nanPercent = noNanSeen_ == (float)nanSeen_ / (float)total; // total is at least 1 because of noNanSeen_++

  if(noNanSeen_ % costScaleFreq_ == 0) {
    costScaleFactor_ *= costScaleMultiplier_;
    LOG(info,
        "NaN/Inf percentage {:.2f} after {} gradient updates. Increasing cost-scaling factor to {}",
        nanPercent,
        total,
        costScaleFactor_);

    // Resetting counts after cost-scale change
    noNanSeen_ = 0;
    nanSeen_ = 0;
  }
}

// call when a NaN was seen to decrease cost-scaling factor
void GraphGroup::decreaseCostScaleFactor() {
  if(!costScale_)
    return;

  nanSeen_++;
  
  size_t total = nanSeen_ + noNanSeen_;
  float nanPercent = (float)nanSeen_ / (float)total; // total is at least 1 because of nanSeen_++
  if(total >= costScaleNanRange_ && nanPercent > costScaleNanTolerance_) {
    if(costScaleFactor_ > costScaleFactorMinimum_) {
      costScaleFactor_ /= costScaleMultiplier_;
      LOG(warn,
          "NaN/Inf percentage {:.2f} in {} gradient updates, reducing cost-scaling factor to {}",
          nanPercent,
          total,
          costScaleFactor_);
    } else {
      // @TODO: think if should this rather abort?
      LOG(warn,
          "NaN/Inf percentage {:.2f} in {} gradient updates, but cost-scaling factor {} is already at minimum",
          nanPercent,
          total,
          costScaleFactor_);
    }

    // Resetting counts after cost-scale change
    noNanSeen_ = 0;
    nanSeen_ = 0;
  }
}

float GraphGroup::checkNanOrNorm(size_t i, size_t begin, size_t end) {
  auto curGrad = graphs_[i]->params()->grads()->subtensor(begin, end-begin);
  
  if(checkGradientNan_ || costScale_) {
    bool hasNan = false, hasInf = false;
    IsNaN(curGrad, graphs_[i]->allocator(), hasNan, hasInf); // @TODO: make safe with different compiler options
    if(hasNan || hasInf) {
      LOG(debug, "Found Nan ({}) or Inf ({})", hasNan, hasInf);
      return std::numeric_limits<float>::quiet_NaN();
    }
  }
  
  if(dynamicGradientScaling_) {
    auto gNorm = L2Norm(curGrad, graphs_[i]->allocator());
    if(isFinite(gNorm) && gNorm > 0.0)
      return gNorm;
    else 
      return std::numeric_limits<float>::quiet_NaN();
  }

  return 0.f;
};

/**
 * This function computes are normalization factor that is applied to the gradient before an update.
 * Depending on various settings this will return a normalizer that can perform a combination of:
 * - apply a cost scaling factor if cost scaling is enabled
 * - normalize the gradient by the number of words in a batch if requested (turning ce-sum in to ce-mean). @TODO: once fp16 stability issues are proven to not be caused by this, remove.
 * - re-scale the gradient based on a dynamic running average of gradient norms
 */
float GraphGroup::computeNormalizationFactor(float gNorm, size_t updateTrgWords) {
  float normalizationFactor = 1.f;

  if(costScale_)
    normalizationFactor *= costScaleFactor_;

  if(options_->get<bool>("normalize-gradient"))
    normalizationFactor *= updateTrgWords;

  if(!isFinite(gNorm)) // we are checking the sanity of the gradient elsewhere
    return normalizationFactor;
  
  if(dynamicGradientScaling_) {
    // make gradient norm invariant to changes in costScaleFactor_, luckily norm(c * g) = c * norm(g)
    if(costScale_)
      gNorm = gNorm / costScaleFactor_;
    
    // Normalize gradient norm w.r.t. number of labels in batch for statistics, 
    // there should be no gradient normalization before this point, @TODO: check this
    gNorm = gNorm / updateTrgWords; 
    
    size_t window; float gNormAvgTransform, gNormVarTransform, gNormTransform, gNormAvg;
    if(dynamicGradientScalingUseLogs_) {
      // tracking the log of the gradient norms rather than the gradient norms itself results in a larger standard deviation as the actual
      // gradient norms go towards 0. From observation, the STD (of log norms) tends to become near constant after some time while the averages keep decreasing.
      std::tie(window, gNormAvgTransform, gNormVarTransform) = scheduler_->getLogGradientNormStats();
      gNormTransform = std::log(gNorm);             // we are using the average of log norms, so we need to compute the log
      gNormAvg       = std::exp(gNormAvgTransform); // for rescaling we need to undo the log, we assume avg(log(norm)) is roughly log(avg(norm))
    } else {
      std::tie(window, gNormAvgTransform, gNormVarTransform) = scheduler_->getGradientNormStats();
      gNormTransform = gNorm;              // we are not using logs, so we can just use the normal gradient norm
      gNormAvg       = gNormAvgTransform;  // we are getting the actual running average of gradient norms, no transformation needed.  
    }
    
    auto deltaTransform    = gNormTransform - gNormAvgTransform; // compute the difference between the current transformer gradient norm and the running average.
    auto gNormStdTransform = std::sqrt(gNormVarTransform);       // compute STD for the running average of (log) gradient norms.

    // delta of (log) gradient norm vs (log) gradient norm average is larger than N standard deviations
    // hence rescale gradient using the average.
    if(scheduler_->numberOfBatches() >= window && deltaTransform > dynamicGradientScalingFactor_ * gNormStdTransform) {
      LOG(debug, "log gradient norms: {} :: {:.4f} - {:.4f} = {:.4f} > {:.4f} * {:.4f}",
          dynamicGradientScalingUseLogs_, gNormTransform, gNormAvgTransform, deltaTransform, dynamicGradientScalingFactor_, gNormStdTransform);

      normalizationFactor *= gNorm / gNormAvg; // since we later do gradient / normalizationFactor this divides by norm and multiplies by the average, rescaling to the average. 
    }
  }

  return normalizationFactor;
};

void GraphGroup::load(const OptimizerBase::ScatterStateFunc& scatterFn) {
  /*
  if not no-reload (=> i.e. do reload):
    restore scheduler
    if checkpoint is available or not no-reload-checkpoint:
      reload from checkpoint
    else if model is available:
      reload from model, but warn that no checkpoint was used and the model could be smoothed
  else if pretrained-model path given:
    initialize matching weights from pretrained model
  else:
    (implicitly) don't do anything => initialize randomly later
  */

  if(!options_->get<bool>("no-reload")) {
    std::string name = options_->get<std::string>("model");

    if(filesystem::exists(name)) {
      if(scheduler_)
        scheduler_->load(name);

      std::string nameGraph = name;
      size_t i = 0;
      for(auto graph : graphs_)
        models_[i++]->load(graph, nameGraph); // we just load it N times from disk (it'll be in disk cache after the first)

      restoreFromCheckpoint(scatterFn);

    } else if(options_->hasAndNotEmpty("pretrained-model")) {
      std::string nameInit = options_->get<std::string>("pretrained-model");
      LOG(info,
          "[training] Initializing model weights with pre-trained model {}",
          nameInit);

      size_t i = 0;
      for(auto graph : graphs_)
        models_[i++]->load(graph, nameInit, false);
    }
  }
}

void GraphGroup::restoreFromCheckpoint(const OptimizerBase::ScatterStateFunc& scatterFn) {
  /*
  if model checkpoint is available:
    - load model from checkpoint, not from model.npz
    - abort if checkpoint model and graph size do not match, probably due to different model or precision
  */

  std::string name = options_->get<std::string>("model");
  std::string checkpointName = name + ".optimizer.npz"; // @TODO: change to .checkpoint.npz, would break backwards compat

  if(!filesystem::exists(checkpointName)) {
    LOG(warn, "No checkpoint found, parameters reloaded from last inference model");
    return;
  }

  auto items = io::loadItems(checkpointName);

  // @TODO: probably we want to have the list of DeviceIds as an attribute
  std::vector<Ptr<Backend>> backends;
  for(auto graph : graphs_)
    backends.push_back(graph->getBackend());
  optimizerShards_[0]->load(items, optimizerShards_, backends, scatterFn);

  // restore the graph parameters from the checkpoint master copy.
  auto found = std::find_if(items.begin(), items.end(),
    [](const io::Item& item) { return item.name == "master_parameters"; });

  if(found == items.end()) {
    LOG(warn, "No master parameters found in checkpoint, parameters reloaded from last inference model");
    return;
  }

  auto& masterParameters = *found;
  for(auto graph : graphs_) {
    graph->forward(); // allocate graph parameter memory and initialize parameters from inference model. This needs to
    // run a full forward pass over the paramters to allocato the parameters values in order (by parameter name).
    // Just doing graph->params()->allocateForward() is not sufficient.
    ABORT_IF(graph->params()->vals()->shape() != masterParameters.shape,
             "Graph parameter sizes and master copy parameter sizes in checkpoint do not match");

    // Convert type of io::Item to match graph parameter type.
    if(masterParameters.type != graph->params()->vals()->type())
      masterParameters.convert(graph->params()->vals()->type());

    graph->params()->vals()->set(masterParameters); // @TODO: make this work for fp16
    graph->clear();
  }

  LOG(info, "[training] Master parameters and optimizers restored from training checkpoint {} and {}", name, checkpointName);
}

void GraphGroup::save(bool isFinal,
                      const std::function<void()>& distributeParamtersFn,
                      const OptimizerBase::GatherStateFunc& gatherOptimizerStateFn,
                      bool isMainProcess) {
  barrier(); // (for better grouping of log messages)
  if(isMainProcess) { // only save from one MPI process
    // bring the smoothed model in
    // Note that it is sharded. For multi-node, it is sharded over multiple machines, so this is a network access.
    // Also note that the swap must run on all MPI processes concurrently, although only one actually validates.

    swapWithSmoothed(graphs_, optimizerShards_, distributeParamtersFn);

    // do final validation
    if(isFinal && scheduler_)
      scheduler_->validate(graphs_, isFinal);

    barrier();// (for better grouping of log messages)
    // save main model file
    saveModel(isFinal);  // if not overwrite then save a copy with number of updates in the model pathname

    swapWithOriginal(graphs_, optimizerShards_, distributeParamtersFn);

    saveCheckpoint(gatherOptimizerStateFn);
  }
  barrier(); // (for better grouping of log messages)
}

void GraphGroup::saveModel(bool isFinal) {
  std::string name = options_->get<std::string>("model");

  if(options_->get<bool>("overwrite")) {
    models_[0]->save(graphs_[0], name, /*saveTranslatorConfig=*/true);
    // save scheduler-related state
    if(scheduler_)
      scheduler_->save(name);
  } else {
    if(!isFinal) { // save a model with iteration number
      std::string numberOfBatches
          = scheduler_ ? std::to_string(scheduler_->numberOfBatches())
                        : "unknown";
      std::string nameOverwrite = name;
      nameOverwrite.replace(name.size() - 4, 4, ".iter" + numberOfBatches + ".npz");
      models_[0]->save(graphs_[0], nameOverwrite);
    }

    models_[0]->save(graphs_[0], name, /*saveTranslatorConfig=*/true);

    // save scheduler-related state
    if(scheduler_)
      scheduler_->save(name);
  }
}

void GraphGroup::saveCheckpoint(const OptimizerBase::GatherStateFunc& gatherFn) {
  std::string name = options_->get<std::string>("model");
  std::string checkpointName = name + ".optimizer.npz"; // @TODO: change to .checkpoint.npz, would break backwards compat

  std::vector<io::Item> items;
  optimizerShards_[0]->save(items,
                            optimizerShards_,
                            gatherFn);

  auto found = std::find_if(items.begin(), items.end(),
    [](const io::Item& item) { return item.name == "master_parameters"; });

  if(found == items.end()) {
    // if the optimizer does not provide a master parameters copy (the default when training with full precision)
    // then dump the parameters of graphs_[0] into the checkpoint. This should be called when the original parameters
    // are in the graph, not the smoothed version. Here we are getting called after a double swap, so that should be
    // the case.
    io::Item masterParameters;
    graphs_[0]->params()->vals()->get(masterParameters, "master_parameters");
    items.push_back(masterParameters);
  }

  LOG(info, "[training] Saving training checkpoint to {} and {}", name, checkpointName);
  io::saveItems(checkpointName, items);
}

void GraphGroup::swapWithSmoothed(const std::vector<Ptr<ExpressionGraph>>& graphs,
                                  const std::vector<Ptr<OptimizerBase>>& opts,
                                  const std::function<void()>& distribute) {
  ABORT_IF(graphs.size() != opts.size(), "Number of graphs and optimizers has to be equal ({} != {})", graphs.size() != opts.size());
  for(size_t i = 0; i < graphs.size(); ++i)
    opts[i]->swapWithSmoothed(graphs[i], i, graphs.size(), /*swapAvg=*/true);
  distribute();
}

void GraphGroup::swapWithOriginal(const std::vector<Ptr<ExpressionGraph>>& graphs,
                                  const std::vector<Ptr<OptimizerBase>>& opts,
                                  const std::function<void()>& distribute) {
  ABORT_IF(graphs.size() != opts.size(), "Number of graphs and optimizers has to be equal ({} != {})", graphs.size() != opts.size());
  for(size_t i = 0; i < graphs.size(); ++i)
    opts[i]->swapWithSmoothed(graphs[i], i, graphs.size(), /*swapAvg=*/false);
  distribute();
}

void GraphGroup::validate() {
  ABORT_IF(finalized_, "Training has already finished.");
}

void GraphGroup::finalize() {
  finalized_ = true;
}

/**
 * Determine maximal batch size that can fit into the given workspace
 * so that reallocation does not happen. Rather adjust the batch size
 * based on the stastistics collected here. Activated with
 * `--mini-batch-fit`.
 * In a multi-GPU scenario, the first GPU is used to determine the size.
 * The actual allowed size is then determined by multiplying it with the
 * number of devices, which is passed in as the 'multiplier'.
 */
// @TODO: Can this be made const? It seems wrong to have a stateful method that still returns a result.
Ptr<data::BatchStats> GraphGroup::collectStats(Ptr<ExpressionGraph> graph,
                                               Ptr<models::ICriterionFunction> model,
                                               const std::vector<Ptr<Vocab>>& vocabs,
                                               double multiplier) {
  // this runs with fake values, we do not care for overflow/underflow
  bool throwNan = graph->getThrowNaN();

  //graph->setFake(true);
  graph->setThrowNaN(false);

  auto stats = New<data::BatchStats>();
  size_t numFiles = numberOfInputFiles();

  // Initialize first batch to step size
  size_t first = options_->get<size_t>("mini-batch-fit-step");

  // Increase batch size and sentence length by this step size
  size_t step = options_->get<size_t>("mini-batch-fit-step");

  size_t maxLength = options_->get<size_t>("max-length");
  maxLength = (size_t)(std::ceil(maxLength / (float)step) * step);

  // this should be only one class label per line on input, hence restricting length to 1
  std::vector<size_t> localMaxes(numFiles, maxLength);
  auto inputTypes = options_->get<std::vector<std::string>>("input-types", {});
  for(int i = 0; i < inputTypes.size(); ++i)
    if(inputTypes[i] == "class")
      localMaxes[i] = 1;

  size_t maxBatch = 512;
  bool fits = true;
  while(fits) {
    std::vector<size_t> lengths(numFiles, first);

    for(int j = 0; j < lengths.size(); ++j) // apply length restrictions
      lengths[j] = std::min(lengths[j], localMaxes[j]);

    auto batch = data::CorpusBatch::fakeBatch(lengths, vocabs, maxBatch, options_);
    auto loss = model->build(graph, batch);
    fits = graph->fits();
    if(fits)
      maxBatch *= 2;
  }

  // Do a binary search for maxmimum batch size that fits into given workspace memory
  // for a tested sentence length.
  for(size_t i = step; i <= maxLength; i += step) {
    size_t start = 1;
    size_t end = maxBatch;

    std::vector<size_t> lengths(numFiles, i);
    for(int j = 0; j < lengths.size(); ++j)  // apply length restrictions
      lengths[j] = std::min(lengths[j], localMaxes[j]);
    fits = true;

    do {
      size_t current = (start + end) / 2;
      auto batch = data::CorpusBatch::fakeBatch(lengths, vocabs, current, options_);
      auto loss = model->build(graph, batch);
      fits = graph->fits();

      LOG(debug, "[batching] length: {} - size: {} - fits: {}", lengths[0], current, fits);

      if(fits) {
        stats->add(batch, multiplier);
        start = current + 1;
      } else {
        end = current - 1;
      }
    } while(end - start > step); // @TODO: better replace with `end >= start` to remove the step here

    maxBatch = start;
  }

  // set back to original value for aborting on NaN or Inf
  graph->setThrowNaN(throwNan);

  return stats;
}

void GraphGroup::setTypicalTrgBatchWords(size_t typicalTrgBatchWords) { // needed for dynamic MB scaling
  typicalTrgBatchWords_ = typicalTrgBatchWords;
}

double GraphGroup::getTypicalTrgBatchWords() {
  return typicalTrgBatchWords_;
}

void GraphGroup::updateAverageTrgBatchWords(size_t trgBatchWords) {
  typicalTrgBatchWords_ = 0.99 * typicalTrgBatchWords_ + 0.01 * (double)trgBatchWords; // record a running average of the batch size, factors are chosen empirically.
}

size_t GraphGroup::numberOfInputFiles() {
  if(options_->get<bool>("tsv", false)) {
    size_t n = options_->get<size_t>("tsv-fields");
    if(n > 0 && options_->get("guided-alignment", std::string("none")) != "none")
      --n;
    if(n > 0 && options_->hasAndNotEmpty("data-weighting"))
      --n;
    return n;
  }
  return options_->get<std::vector<std::string>>("train-sets").size();
}

}  // namespace marian
