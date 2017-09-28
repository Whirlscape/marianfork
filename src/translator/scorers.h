#pragma once

#include "marian.h"
#include "models/model_factory.h"

namespace marian {

class ScorerState {
public:
  virtual Expr getProbs() = 0;

  virtual float breakDown(size_t i) { return getProbs()->val()->get(i); }

  virtual void blacklist(Expr totalCosts, Ptr<data::CorpusBatch> batch){};
};

class Scorer {
protected:
  std::string name_;
  float weight_;

public:
  Scorer(const std::string& name, float weight)
      : name_(name), weight_(weight) {}

  std::string getName() { return name_; }
  float getWeight() { return weight_; }

  virtual void clear(Ptr<ExpressionGraph>) = 0;
  virtual Ptr<ScorerState> startState(Ptr<ExpressionGraph>,
                                      Ptr<data::CorpusBatch>)
      = 0;
  virtual Ptr<ScorerState> step(Ptr<ExpressionGraph>,
                                Ptr<ScorerState>,
                                const std::vector<size_t>&,
                                const std::vector<size_t>&)
      = 0;

  virtual void init(Ptr<ExpressionGraph> graph) {}
};

class ScorerWrapperState : public ScorerState {
protected:
  Ptr<DecoderState> state_;

public:
  ScorerWrapperState(Ptr<DecoderState> state) : state_(state) {}

  virtual Ptr<DecoderState> getState() { return state_; }

  virtual Expr getProbs() { return state_->getProbs(); };

  virtual void blacklist(Expr totalCosts, Ptr<data::CorpusBatch> batch) {
    state_->blacklist(totalCosts, batch);
  }
};

class ScorerWrapper : public Scorer {
private:
  Ptr<EncoderDecoder> encdec_;
  std::string fname_;

public:
  ScorerWrapper(Ptr<EncoderDecoder> encdec,
                const std::string& name,
                float weight,
                const std::string& fname)
      : Scorer(name, weight),
        encdec_(encdec),
        fname_(fname) {}

  virtual void init(Ptr<ExpressionGraph> graph) {
    graph->switchParams(getName());
    encdec_->load(graph, fname_);
  }

  virtual void clear(Ptr<ExpressionGraph> graph) {
    graph->switchParams(getName());
    encdec_->clear(graph);
  }

  virtual Ptr<ScorerState> startState(Ptr<ExpressionGraph> graph,
                                      Ptr<data::CorpusBatch> batch) {
    graph->switchParams(getName());
    return New<ScorerWrapperState>(encdec_->startState(graph, batch));
  }

  virtual Ptr<ScorerState> step(Ptr<ExpressionGraph> graph,
                                Ptr<ScorerState> state,
                                const std::vector<size_t>& hypIndices,
                                const std::vector<size_t>& embIndices) {
    graph->switchParams(getName());
    auto wrappedState
        = std::dynamic_pointer_cast<ScorerWrapperState>(state)->getState();
    return New<ScorerWrapperState>(
        encdec_->step(graph, wrappedState, hypIndices, embIndices));
  }
};

class WordPenaltyState : public ScorerState {
private:
  int dimVocab_;
  Expr penalties_;

public:
  WordPenaltyState(int dimVocab, Expr penalties)
      : dimVocab_(dimVocab), penalties_(penalties) {}

  virtual Expr getProbs() { return penalties_; };

  virtual float breakDown(size_t i) {
    return getProbs()->val()->get(i % dimVocab_);
  }
};

class WordPenalty : public Scorer {
private:
  int dimVocab_;
  Expr penalties_;

public:
  WordPenalty(const std::string& name, float weight, int dimVocab)
      : Scorer(name, weight), dimVocab_(dimVocab) {}

  virtual void clear(Ptr<ExpressionGraph> graph) {}

  virtual Ptr<ScorerState> startState(Ptr<ExpressionGraph> graph,
                                      Ptr<data::CorpusBatch> batch) {
    std::vector<float> p(dimVocab_, 1);
    p[0] = 0;
    p[2] = 0;

    penalties_ = graph->constant({1, dimVocab_},
                                 keywords::init = inits::from_vector(p));
    return New<WordPenaltyState>(dimVocab_, penalties_);
  }

  virtual Ptr<ScorerState> step(Ptr<ExpressionGraph> graph,
                                Ptr<ScorerState> state,
                                const std::vector<size_t>& hypIndices,
                                const std::vector<size_t>& embIndices) {
    return state;
  }
};

class UnseenWordPenalty : public Scorer {
private:
  int batchIndex_;
  int dimVocab_;
  Expr penalties_;

public:
  UnseenWordPenalty(const std::string& name,
                    float weight,
                    int dimVocab,
                    int batchIndex)
      : Scorer(name, weight), dimVocab_(dimVocab), batchIndex_(batchIndex) {}

  virtual void clear(Ptr<ExpressionGraph> graph) {}

  virtual Ptr<ScorerState> startState(Ptr<ExpressionGraph> graph,
                                      Ptr<data::CorpusBatch> batch) {
    std::vector<float> p(dimVocab_, -1);
    for(auto i : (*batch)[batchIndex_]->indices())
      p[i] = 0;
    p[2] = 0;

    penalties_ = graph->constant({1, dimVocab_},
                                 keywords::init = inits::from_vector(p));
    return New<WordPenaltyState>(dimVocab_, penalties_);
  }

  virtual Ptr<ScorerState> step(Ptr<ExpressionGraph> graph,
                                Ptr<ScorerState> state,
                                const std::vector<size_t>& hypIndices,
                                const std::vector<size_t>& embIndices) {
    return state;
  }
};

Ptr<Scorer> scorerByType(std::string fname,
                         float weight,
                         std::string model,
                         Ptr<Config> config) {

  Ptr<Options> options = New<Options>();
  options->merge(config);
  options->set("inference", true);

  std::string type = options->get<std::string>("type");

  // @TODO: solve this better
  if(type == "lm" && config->has("input")) {
    size_t index = config->get<std::vector<std::string>>("input").size();
    options->set("index", index);
  }

  auto encdec = models::from_options(options);

  LOG(info)->info("Loading scorer of type {} as feature {}", type, fname);

  return New<ScorerWrapper>(encdec, fname, weight, model);
}

std::vector<Ptr<Scorer>> createScorers(Ptr<Config> options) {
  std::vector<Ptr<Scorer>> scorers;

  auto models = options->get<std::vector<std::string>>("models");
  int dimVocab = options->get<std::vector<int>>("dim-vocabs").back();

  std::vector<float> weights(models.size(), 1.f);
  if(options->has("weights"))
    weights = options->get<std::vector<float>>("weights");

  int i = 0;
  for(auto model : models) {
    std::string fname = "F" + std::to_string(i);
    auto modelOptions = New<Config>(*options);

    try {
      modelOptions->loadModelParameters(model);
    } catch(std::runtime_error& e) {
      LOG(warn)->warn("No model settings found in model file");
    }

    scorers.push_back(scorerByType(fname, weights[i], model, modelOptions));
    i++;
  }

  return scorers;
}

}
