/*
 * Copyright 2021 Garena Online Private Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ENVPOOL_CORE_ENV_H_
#define ENVPOOL_CORE_ENV_H_

#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "envpool/core/env_spec.h"
#include "envpool/core/state_buffer_queue.h"

/**
 * Single RL environment abstraction.
 */
template <typename EnvSpec>
class Env {
 private:
  StateBufferQueue* sbq_;
  int order_, elapsed_step_;
  bool is_single_player_;
  StateBuffer::WritableSlice slice_;
  // for parsing single env action from input action batch
  std::vector<ShapeSpec> action_specs_;
  std::vector<bool> is_player_action_;
  std::shared_ptr<std::vector<Array>> action_batch_;
  std::vector<Array> raw_action_;
  int env_index_;

 protected:
  EnvSpec spec_;
  int env_id_, seed_;
  std::mt19937 gen_;

 public:
  typedef EnvSpec Spec;
  using State = NamedVector<typename EnvSpec::StateKeys, std::vector<Array>>;
  using Action = NamedVector<typename EnvSpec::ActionKeys, std::vector<Array>>;

 public:
  Env(const EnvSpec& spec, int env_id)
      : elapsed_step_(-1),
        is_single_player_(spec.config["max_num_players"_] == 1),
        action_specs_(spec.action_spec.template values<ShapeSpec>()),
        is_player_action_(Transform(action_specs_,
                                    [](const ShapeSpec& s) {
                                      return (s.shape.size() > 0 &&
                                              s.shape[0] == -1);
                                    })),
        spec_(spec),
        env_id_(env_id),
        seed_(spec.config["seed"_] + env_id),
        gen_(seed_) {
    slice_.done_write = [] { LOG(INFO) << "Use `Allocate` to write state."; };
  }

  void SetAction(std::shared_ptr<std::vector<Array>> action_batch,
                 int env_index) {
    action_batch_ = action_batch;
    env_index_ = env_index;
  }

  void ParseAction() {
    raw_action_.clear();
    std::size_t action_size = action_batch_->size();
    if (is_single_player_) {
      for (std::size_t i = 0; i < action_size; ++i) {
        if (is_player_action_[i]) {
          raw_action_.emplace_back(
              (*action_batch_)[i].Slice(env_index_, env_index_ + 1));
        } else {
          raw_action_.emplace_back((*action_batch_)[i][env_index_]);
        }
      }
    } else {
      std::vector<int> env_player_index;
      int* player_env_id = static_cast<int*>((*action_batch_)[1].data());
      int player_offset = (*action_batch_)[1].Shape(0);
      for (int i = 0; i < player_offset; ++i) {
        if (player_env_id[i] == env_id_) {
          env_player_index.push_back(i);
        }
      }
      int player_num = env_player_index.size();
      bool continuous = false;
      int start = 0, end = 0;
      if (player_num > 0) {
        start = env_player_index[0];
        end = env_player_index[player_num - 1] + 1;
        continuous = (player_num == end - start);
      }
      for (std::size_t i = 0; i < action_size; ++i) {
        if (is_player_action_[i]) {
          if (continuous) {
            raw_action_.emplace_back(
                std::move((*action_batch_)[i].Slice(start, end)));
          } else {
            action_specs_[i].shape[0] = player_num;
            Array arr(action_specs_[i]);
            for (int j = 0; j < player_num; ++j) {
              int player_index = env_player_index[j];
              arr[j].Assign((*action_batch_)[i][player_index]);
            }
            raw_action_.emplace_back(std::move(arr));
          }
        } else {
          raw_action_.emplace_back(std::move((*action_batch_)[i][env_index_]));
        }
      }
    }
  }

  void EnvStep(StateBufferQueue* sbq, int order, bool reset) {
    PreProcess(sbq, order, reset);
    if (reset) {
      Reset();
    } else {
      ParseAction();
      Step(Action(&raw_action_));
    }
    PostProcess();
  }

  virtual void Reset() { throw std::runtime_error("reset not implemented"); }
  virtual void Step(const Action& action) {
    throw std::runtime_error("step not implemented");
  }
  virtual bool IsDone() { throw std::runtime_error("is_done not implemented"); }

 protected:
  void PreProcess(StateBufferQueue* sbq, int order, bool reset) {
    sbq_ = sbq;
    order_ = order;
    if (reset) {
      elapsed_step_ = 0;
    } else {
      ++elapsed_step_;
    }
  }

  void PostProcess() {
    slice_.done_write();
    // action_batch_.reset();
  }

  State Allocate(int player_num = 1) {
    slice_ = sbq_->Allocate(player_num, order_);
    State state(&slice_.arr);
    state["done"_] = IsDone();
    state["info:env_id"_] = env_id_;
    state["elapsed_step"_] = elapsed_step_;
    int* player_env_id(static_cast<int*>(state["info:players.env_id"_].data()));
    for (int i = 0; i < player_num; ++i) {
      player_env_id[i] = env_id_;
    }
    return state;
  }
};

#endif  // ENVPOOL_CORE_ENV_H_
