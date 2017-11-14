#ifndef SAMPLING_H
#define SAMPLING_H

#include <exception>
#include <functional>
#include <memory>
#include <vector>

class State;
class SuccessorGenerator;
class TaskProxy;

namespace utils {
class CountdownTimer;
class RandomNumberGenerator;
}

using DeadEndDetector = std::function<bool (State)>;

struct SamplingTimeout : public std::exception {};

State sample_state_with_random_walk(
    const State &state,
    const SuccessorGenerator &successor_generator,
    int init_h,
    double average_operator_cost,
    utils::RandomNumberGenerator &rng,
    DeadEndDetector is_dead_end = [] (const State &) {return false;});

/*
  Perform 'num_samples' random walks with biomially distributed walk
  lenghts. Whenever a dead end is detected or a state has no
  successors, restart from the initial state. The function
  'is_dead_end' should return whether a given state is a dead end. If
  omitted, no dead end detection is performed. If 'timer' is given the
  sampling procedure will run for at most the specified time limit and
  possibly return less than 'num_samples' states.
*/
std::vector<State> sample_states_with_random_walks(
    const TaskProxy &task_proxy,
    const SuccessorGenerator &successor_generator,
    int num_samples,
    int init_h,
    double average_operator_cost,
    utils::RandomNumberGenerator &rng,
    DeadEndDetector is_dead_end = [] (const State &) {return false;},
    const utils::CountdownTimer *timer = nullptr);


class RandomWalkSampler {
    const std::unique_ptr<SuccessorGenerator> successor_generator;
    const std::unique_ptr<State> initial_state;
    const int init_h;
    const double average_operator_costs;
    utils::RandomNumberGenerator &rng;
    const DeadEndDetector is_dead_end;
    bool returned_initial_state;

public:
    RandomWalkSampler(
        const TaskProxy &task_proxy,
        int init_h,
        utils::RandomNumberGenerator &rng,
        DeadEndDetector is_dead_end = [] (const State &) {return false;});

    State sample_state();
};

#endif
