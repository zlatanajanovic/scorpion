#include "pattern_collection_generator_filtered_systematic.h"

#include "pattern_collection_generator_systematic.h"
#include "pattern_evaluator.h"

#include "../option_parser.h"
#include "../plugin.h"
#include "../task_proxy.h"

#include "../algorithms/array_pool.h"
#include "../algorithms/priority_queues.h"
#include "../cost_saturation/projection.h"
#include "../cost_saturation/utils.h"
#include "../task_utils/task_properties.h"
#include "../utils/collections.h"
#include "../utils/countdown_timer.h"
#include "../utils/logging.h"
#include "../utils/math.h"
#include "../utils/rng.h"
#include "../utils/rng_options.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <limits>
#include <queue>

using namespace std;

namespace pdbs {
static vector<int> get_variable_domains(const TaskProxy &task_proxy) {
    VariablesProxy variables = task_proxy.get_variables();
    vector<int> variable_domains;
    variable_domains.reserve(variables.size());
    for (VariableProxy var : variables) {
        variable_domains.push_back(var.get_domain_size());
    }
    return variable_domains;
}

static vector<vector<int>> get_relevant_operators_per_variable(
    const TaskProxy &task_proxy) {
    vector<vector<int>> operators_per_variable(task_proxy.get_variables().size());
    for (OperatorProxy op : task_proxy.get_operators()) {
        for (EffectProxy effect : op.get_effects()) {
            int var = effect.get_fact().get_variable().get_id();
            operators_per_variable[var].push_back(op.get_id());
        }
    }
    for (auto &operators : operators_per_variable) {
        operators.shrink_to_fit();
    }
    return operators_per_variable;
}

template<typename Iterable>
static int get_pdb_size(const vector<int> &domain_sizes, const Iterable &pattern) {
    int size = 1;
    for (int var : pattern) {
        if (utils::is_product_within_limit(
                size, domain_sizes[var], numeric_limits<int>::max())) {
            size *= domain_sizes[var];
        } else {
            return -1;
        }
    }
    return size;
}

template<typename Iterable>
static int get_sum(const Iterable &pattern) {
    int sum = 0;
    for (int var : pattern) {
        sum += var;
    }
    return sum;
}

template<typename Iterable>
static int get_min(const Iterable &pattern) {
    int res = numeric_limits<int>::max();
    for (int var : pattern) {
        res = min(res, var);
    }
    return res;
}

template<typename Iterable>
static int get_max(const Iterable &pattern) {
    int res = -1;
    for (int var : pattern) {
        res = max(res, var);
    }
    return res;
}

template<typename Iterable>
static int get_num_new_var_pairs(
    const Iterable &pattern,
    const vector<vector<bool>> &used_var_pairs) {
    int num_new_var_pairs = 0;
    for (auto it1 = pattern.begin(); it1 != pattern.end(); ++it1) {
        for (auto it2 = it1 + 1; it2 != pattern.end(); ++it2) {
            int var1 = *it1;
            int var2 = *it2;
            if (!used_var_pairs[var1][var2]) {
                ++num_new_var_pairs;
            }
        }
    }
    return num_new_var_pairs;
}

template<typename Iterable>
static int get_num_active_ops(
    const Iterable &pattern,
    const TaskInfo &task_info) {
    int num_active_ops = 0;
    for (int op_id = 0; op_id < task_info.get_num_operators(); ++op_id) {
        if (task_info.operator_affects_pattern(pattern, op_id)) {
            ++num_active_ops;
        }
    }
    return num_active_ops;
}

bool contains_positive_finite_value(const vector<int> &values) {
    return any_of(values.begin(), values.end(),
                  [](int v) {return v > 0 && v != numeric_limits<int>::max();});
}

static bool only_free_operators_affect_pdb(
    const Pattern &pattern,
    const vector<int> &costs,
    const vector<vector<int>> &relevant_operators_per_variable) {
    for (int var : pattern) {
        for (int op : relevant_operators_per_variable[var]) {
            if (costs[op] > 0 && costs[op] != numeric_limits<int>::max()) {
                return false;
            }
        }
    }
    return true;
}

static unique_ptr<PatternCollection> get_patterns(
    const shared_ptr<AbstractTask> &task,
    int pattern_size,
    bool only_sga_patterns,
    const utils::CountdownTimer &timer) {
    utils::Log() << "Generate patterns for size " << pattern_size << endl;
    options::Options opts;
    opts.set<int>("pattern_max_size", pattern_size);
    opts.set<bool>("only_interesting_patterns", true);
    opts.set<bool>("only_sga_patterns", only_sga_patterns);
    PatternCollectionGeneratorSystematic generator(opts);
    unique_ptr<PatternCollection> patterns_ptr =
        utils::make_unique_ptr<PatternCollection>();
    PatternCollection &patterns = *patterns_ptr;
    generator.generate(
        task, [pattern_size, &patterns, &timer](
            const Pattern &pattern, bool handle) {
            if (handle && static_cast<int>(pattern.size()) == pattern_size) {
                patterns.push_back(pattern);
            }
            return timer.is_expired();
        }, timer);
    if (timer.is_expired()) {
        return nullptr;
    }
    return patterns_ptr;
}

static int compute_score(
    const array_pool::ArrayPoolSlice<int> &pattern,
    PatternOrder order_type,
    const TaskInfo &task_info,
    const vector<int> &domains,
    const vector<vector<bool>> &used_var_pairs) {
    if (order_type == PatternOrder::PDB_SIZE_UP ||
        order_type == PatternOrder::PDB_SIZE_DOWN) {
        return get_pdb_size(domains, pattern);
    } else if (order_type == PatternOrder::CG_SUM_UP ||
               order_type == PatternOrder::CG_SUM_DOWN) {
        return get_sum(pattern);
    } else if (order_type == PatternOrder::CG_MIN_UP ||
               order_type == PatternOrder::CG_MIN_DOWN) {
        return get_min(pattern);
    } else if (order_type == PatternOrder::CG_MAX_UP ||
               order_type == PatternOrder::CG_MAX_DOWN) {
        return get_max(pattern);
    } else if (order_type == PatternOrder::NEW_VAR_PAIRS_UP ||
               order_type == PatternOrder::NEW_VAR_PAIRS_DOWN) {
        return get_num_new_var_pairs(pattern, used_var_pairs);
    } else if (order_type == PatternOrder::ACTIVE_OPS_UP ||
               order_type == PatternOrder::ACTIVE_OPS_DOWN) {
        return get_num_active_ops(pattern, task_info);
    } else {
        ABORT("wrong order_type");
    }
}

static void compute_pattern_order(
    const array_pool::ArrayPool<int> &patterns,
    vector<int> &order,
    PatternOrder order_type,
    const TaskInfo &task_info,
    const vector<int> &domains,
    const vector<vector<bool>> &used_var_pairs,
    utils::RandomNumberGenerator &rng) {
    assert(patterns.size() == static_cast<int>(order.size()));
    if (order_type == PatternOrder::ORIGINAL) {
        return;
    } else if (order_type == PatternOrder::REVERSE) {
        reverse(order.begin(), order.end());
        return;
    }

    rng.shuffle(order);

    if (order_type == PatternOrder::RANDOM) {
        return;
    }

    if (order_type == PatternOrder::ACTIVE_OPS_UP_CG_MIN_DOWN ||
        order_type == PatternOrder::CG_MIN_DOWN_ACTIVE_OPS_UP) {
        vector<pair<int, int>> pairs;
        pairs.reserve(patterns.size());
        for (int pattern_id = 0; pattern_id < patterns.size(); ++pattern_id) {
            int active_ops = compute_score(
                patterns.get_slice(pattern_id), PatternOrder::ACTIVE_OPS_UP,
                task_info, domains, used_var_pairs);
            int cg_min = compute_score(
                patterns.get_slice(pattern_id), PatternOrder::CG_MIN_DOWN,
                task_info, domains, used_var_pairs);
            if (order_type == PatternOrder::ACTIVE_OPS_UP_CG_MIN_DOWN) {
                pairs.emplace_back(active_ops, -cg_min);
            } else {
                assert(order_type == PatternOrder::CG_MIN_DOWN_ACTIVE_OPS_UP);
                pairs.emplace_back(-cg_min, active_ops);
            }
        }
        sort(order.begin(), order.end(),
             [&pairs](int i, int j) {
                 return pairs[i] < pairs[j];
             });
        return;
    }

    vector<int> scores;
    scores.reserve(patterns.size());
    for (int pattern_id = 0; pattern_id < patterns.size(); ++pattern_id) {
        scores.push_back(
            compute_score(
                patterns.get_slice(pattern_id), order_type, task_info, domains,
                used_var_pairs));
    }

    sort(order.begin(), order.end(),
         [&scores](int i, int j) {
             return scores[i] < scores[j];
         });

    if (order_type == PatternOrder::PDB_SIZE_DOWN ||
        order_type == PatternOrder::CG_SUM_DOWN ||
        order_type == PatternOrder::CG_MIN_DOWN ||
        order_type == PatternOrder::CG_MAX_DOWN ||
        order_type == PatternOrder::NEW_VAR_PAIRS_DOWN ||
        order_type == PatternOrder::ACTIVE_OPS_DOWN) {
        reverse(order.begin(), order.end());
    }
}


class SequentialPatternGenerator {
    shared_ptr<AbstractTask> task;
    const TaskInfo &task_info;
    int max_pattern_size;
    bool only_sga_patterns;
    PatternOrder order_type;
    PatternOrder default_order_type;
    utils::RandomNumberGenerator &rng;
    vector<int> domains;
    vector<array_pool::ArrayPool<int>> patterns;
    vector<vector<int>> orders;
    int cached_pattern_size;
    int num_generated_patterns;

    PatternOrder get_order_type() {
        if (order_type == PatternOrder::ALT_TWO) {
            if (rng(2) == 0) {
                return PatternOrder::CG_MIN_DOWN;
            } else {
                return PatternOrder::ACTIVE_OPS_UP;
            }
        }
        return order_type;
    }
public:
    SequentialPatternGenerator(
        const shared_ptr<AbstractTask> &task,
        const TaskInfo &task_info,
        int max_pattern_size_,
        bool only_sga_patterns,
        PatternOrder order,
        utils::RandomNumberGenerator &rng)
        : task(task),
          task_info(task_info),
          max_pattern_size(max_pattern_size_),
          only_sga_patterns(only_sga_patterns),
          order_type(order),
          rng(rng),
          domains(get_variable_domains(TaskProxy(*task))),
          cached_pattern_size(0),
          num_generated_patterns(0) {
        assert(max_pattern_size_ >= 0);
        max_pattern_size = min(
            max_pattern_size, static_cast<int>(TaskProxy(*task).get_variables().size()));
        default_order_type = get_order_type();
    }

    Pattern get_pattern(
        int pattern_id,
        const vector<vector<bool>> &used_var_pairs,
        const utils::CountdownTimer &timer) {
        assert(pattern_id >= 0);
        if (pattern_id < num_generated_patterns) {
            int bucket_id = -1;
            int internal_id = -1;
            int start_id = 0;
            int end_id = -1;
            for (size_t i = 0; i < orders.size(); ++i) {
                const vector<int> &order = orders[i];
                end_id += order.size();
                if (pattern_id >= start_id && pattern_id <= end_id) {
                    internal_id = order[pattern_id - start_id];
                    bucket_id = i;
                    break;
                }
                start_id += order.size();
            }
            assert(internal_id != -1);
            array_pool::ArrayPoolSlice<int> slice = patterns[bucket_id].get_slice(internal_id);
            return {
                       slice.begin(), slice.end()
            };
        } else if (cached_pattern_size < max_pattern_size) {
            unique_ptr<PatternCollection> current_patterns = get_patterns(
                task, cached_pattern_size + 1, only_sga_patterns, timer);
            if (current_patterns) {
                ++cached_pattern_size;
                utils::Log() << "Store patterns of size " << cached_pattern_size << endl;
                num_generated_patterns += current_patterns->size();
                patterns.emplace_back();
                for (Pattern &pattern : *current_patterns) {
                    patterns.back().append(move(pattern));
                }
                vector<int> current_order(current_patterns->size(), -1);
                iota(current_order.begin(), current_order.end(), 0);
                compute_pattern_order(
                    patterns.back(), current_order, default_order_type,
                    task_info, domains, used_var_pairs, rng);
                orders.push_back(move(current_order));
                utils::Log() << "Finished storing patterns of size "
                             << cached_pattern_size << endl;
                return get_pattern(pattern_id, used_var_pairs, timer);
            }
        }
        return {};
    }

    void restart(const vector<vector<bool>> &used_var_pairs) {
        if (order_type == PatternOrder::RANDOM ||
            order_type == PatternOrder::NEW_VAR_PAIRS_UP ||
            order_type == PatternOrder::NEW_VAR_PAIRS_DOWN ||
            order_type == PatternOrder::ALT_TWO) {
            PatternOrder current_order_type = get_order_type();
            for (size_t i = 0; i < orders.size(); ++i) {
                vector<int> &order = orders[i];
                compute_pattern_order(
                    patterns[i], order, current_order_type, task_info, domains, used_var_pairs, rng);
            }
        }
    }

    int get_num_generated_patterns() const {
        return num_generated_patterns;
    }

    int get_max_generated_pattern_size() const {
        return cached_pattern_size;
    }
};


PatternCollectionGeneratorFilteredSystematic::PatternCollectionGeneratorFilteredSystematic(
    const Options &opts)
    : max_pattern_size(opts.get<int>("max_pattern_size")),
      max_pdb_size(opts.get<int>("max_pdb_size")),
      max_collection_size(opts.get<int>("max_collection_size")),
      max_patterns(opts.get<int>("max_patterns")),
      max_time(opts.get<double>("max_time")),
      max_time_per_restart(opts.get<double>("max_time_per_restart")),
      saturate(opts.get<bool>("saturate")),
      only_sga_patterns(opts.get<bool>("only_sga_patterns")),
      ignore_useless_patterns(opts.get<bool>("ignore_useless_patterns")),
      store_orders(opts.get<bool>("store_orders")),
      dead_end_treatment(static_cast<DeadEndTreatment>(opts.get_enum("dead_ends"))),
      pattern_order(static_cast<PatternOrder>(opts.get_enum("order"))),
      rng(utils::parse_rng_from_options(opts)),
      debug(opts.get<bool>("debug")) {
}

bool PatternCollectionGeneratorFilteredSystematic::select_systematic_patterns(
    const shared_ptr<AbstractTask> &task,
    const shared_ptr<cost_saturation::TaskInfo> &task_info,
    const TaskInfo &evaluator_task_info,
    SequentialPatternGenerator &pattern_generator,
    PartialStateCollection &dead_ends,
    priority_queues::AdaptiveQueue<size_t> &pq,
    const shared_ptr<ProjectionCollection> &projections,
    PatternSet &pattern_set,
    vector<vector<bool>> &used_var_pairs,
    int64_t &collection_size,
    double overall_remaining_time) {
    utils::Log log;
    utils::CountdownTimer timer(min(overall_remaining_time, max_time_per_restart));
    TaskProxy task_proxy(*task);
    State initial_state = task_proxy.get_initial_state();
    vector<int> variable_domains = get_variable_domains(task_proxy);
    vector<int> costs = task_properties::get_operator_costs(task_proxy);
    int pattern_id = -1;
    while (true) {
        ++pattern_id;

        pattern_computation_timer->resume();
        Pattern pattern = pattern_generator.get_pattern(
            pattern_id, used_var_pairs, timer);
        pattern_computation_timer->stop();

        if (timer.is_expired()) {
            log << "Reached restart time limit." << endl;
            return false;
        }

        if (debug) {
            cout << "Pattern " << pattern_id << ": " << pattern << " new:"
                 << get_num_new_var_pairs(pattern, used_var_pairs) << " ops:"
                 << get_num_active_ops(pattern, evaluator_task_info) << endl;
        }

        if (pattern.empty()) {
            log << "Generated all patterns up to size " << max_pattern_size
                << "." << endl;
            return false;
        } else if (pattern_set.count(pattern)) {
            continue;
        }

        int pdb_size = get_pdb_size(variable_domains, pattern);
        if (pdb_size == -1 || pdb_size > max_pdb_size) {
            // Pattern is too large.
            continue;
        }

        if (static_cast<int>(projections->size()) == max_patterns) {
            log << "Reached maximum number of patterns." << endl;
            return true;
        }

        if (max_collection_size != numeric_limits<int>::max() &&
            pdb_size > static_cast<int64_t>(max_collection_size) - collection_size) {
            log << "Reached maximum collection size." << endl;
            return true;
        }

        if (ignore_useless_patterns &&
            only_free_operators_affect_pdb(pattern, costs, relevant_operators_per_variable)) {
            if (debug)
                log << "Only free operators affect " << pattern << endl;
            continue;
        }

        projection_computation_timer->resume();
        PatternEvaluator pattern_evaluator(task_proxy, evaluator_task_info, pattern, costs);
        projection_computation_timer->stop();

        bool select_pattern = true;
        if (saturate) {
            projection_evaluation_timer->resume();
            select_pattern = pattern_evaluator.is_useful(
                pattern, pq, dead_ends, dead_end_treatment, costs);
            projection_evaluation_timer->stop();
#ifndef NDEBUG
            vector<int> goal_distances = cost_saturation::Projection(
                task_proxy, task_info, pattern).compute_goal_distances(costs);
            if (dead_end_treatment == DeadEndTreatment::IGNORE) {
                assert(select_pattern ==
                       contains_positive_finite_value(goal_distances));
            } else if (dead_end_treatment == DeadEndTreatment::ALL) {
                assert(select_pattern ==
                       any_of(goal_distances.begin(), goal_distances.end(),
                              [](int d) {return d > 0;}));
            } else {
                assert(dead_end_treatment == DeadEndTreatment::NEW ||
                       dead_end_treatment == DeadEndTreatment::NEW_FOR_CURRENT_ORDER);
            }
#endif
        }

        ++num_pattern_evaluations;

        if (select_pattern) {
            log << "Add pattern " << pattern << endl;
            unique_ptr<cost_saturation::Projection> projection =
                utils::make_unique_ptr<cost_saturation::Projection>(
                    task_proxy, task_info, pattern);
            if (saturate) {
                vector<int> goal_distances = projection->compute_goal_distances(costs);
                vector<int> saturated_costs = projection->compute_saturated_costs(
                    goal_distances, costs.size());
                cost_saturation::reduce_costs(costs, saturated_costs);
            }
            projections->push_back(move(projection));
            pattern_set.insert(pattern);
            for (int var1 : pattern) {
                for (int var2 : pattern) {
                    used_var_pairs[var1][var2] = true;
                }
            }
            collection_size += pdb_size;
        }
    }
}

PatternCollectionInformation PatternCollectionGeneratorFilteredSystematic::generate(
    const shared_ptr<AbstractTask> &task) {
    utils::CountdownTimer timer(max_time);
    pattern_computation_timer = utils::make_unique_ptr<utils::Timer>();
    pattern_computation_timer->stop();
    projection_computation_timer = utils::make_unique_ptr<utils::Timer>();
    projection_computation_timer->stop();
    projection_evaluation_timer = utils::make_unique_ptr<utils::Timer>();
    projection_evaluation_timer->stop();
    utils::Log log;
    TaskProxy task_proxy(*task);
    shared_ptr<cost_saturation::TaskInfo> task_info =
        make_shared<cost_saturation::TaskInfo>(task_proxy);
    TaskInfo evaluator_task_info(task_proxy);
    if (ignore_useless_patterns) {
        relevant_operators_per_variable = get_relevant_operators_per_variable(task_proxy);
    }
    SequentialPatternGenerator pattern_generator(
        task, evaluator_task_info, max_pattern_size, only_sga_patterns, pattern_order, *rng);
    priority_queues::AdaptiveQueue<size_t> pq;
    PartialStateCollection dead_ends;
    shared_ptr<ProjectionCollection> projections = make_shared<ProjectionCollection>();
    PatternSet pattern_set;
    int num_vars = task_proxy.get_variables().size();
    vector<vector<bool>> used_var_pairs;
    for (int i = 0; i < num_vars; ++i) {
        used_var_pairs.emplace_back(num_vars, false);
    }
    int64_t collection_size = 0;
    num_pattern_evaluations = 0;
    bool limit_reached = false;
    while (!limit_reached) {
        pattern_generator.restart(used_var_pairs);
        if (dead_end_treatment == DeadEndTreatment::NEW_FOR_CURRENT_ORDER) {
            dead_ends.clear();
        }
        int num_patterns_before = projections->size();
        limit_reached = select_systematic_patterns(
            task, task_info, evaluator_task_info, pattern_generator, dead_ends,
            pq, projections, pattern_set, used_var_pairs, collection_size,
            timer.get_remaining_time());
        int num_patterns_after = projections->size();
        log << "Patterns: " << num_patterns_after << ", collection size: "
            << collection_size << endl;
        if (store_orders && num_patterns_after > num_patterns_before) {
            cost_saturation::Order order;
            for (int i = num_patterns_before; i < num_patterns_after; ++i) {
                order.push_back(i);
            }
            cout << "Store order " << order << endl;
            cost_saturation::systematic_generator_orders_hacked.push_back(order);
        }
        if (num_patterns_after == num_patterns_before) {
            log << "Restart did not add any pattern." << endl;
            break;
        }
        if (timer.is_expired()) {
            log << "Reached overall time limit." << endl;
            break;
        }
    }

    log << "Time for computing ordered systematic patterns: "
        << *pattern_computation_timer << endl;
    log << "Time for computing ordered systematic projections: "
        << *projection_computation_timer << endl;
    log << "Time for evaluating ordered systematic projections: "
        << *projection_evaluation_timer << endl;
    log << "Ordered systematic pattern evaluations: "
        << num_pattern_evaluations << endl;
    log << "Maximum generated ordered systematic pattern size: "
        << pattern_generator.get_max_generated_pattern_size() << endl;
    int num_generated_patterns = pattern_generator.get_num_generated_patterns();
    double percent_selected = (num_generated_patterns == 0) ? 0.
        : static_cast<double>(projections->size()) / num_generated_patterns;
    log << "Selected ordered systematic patterns: " << projections->size()
        << "/" << num_generated_patterns << " = " << percent_selected << endl;

    shared_ptr<PatternCollection> patterns = make_shared<PatternCollection>();
    patterns->reserve(projections->size());
    for (auto &projection : *projections) {
        patterns->push_back(projection->get_pattern());
    }
    PatternCollectionInformation pci(task_proxy, patterns);
    pci.set_projections(projections);
    return pci;
}


static void add_options(OptionParser &parser) {
    parser.add_option<int>(
        "max_pattern_size",
        "maximum number of variables per pattern",
        "infinity",
        Bounds("1", "infinity"));
    parser.add_option<int>(
        "max_pdb_size",
        "maximum number of states in a PDB",
        "infinity",
        Bounds("1", "infinity"));
    parser.add_option<int>(
        "max_collection_size",
        "maximum number of states in the pattern collection",
        "infinity",
        Bounds("1", "infinity"));
    parser.add_option<int>(
        "max_patterns",
        "maximum number of patterns",
        "infinity",
        Bounds("1", "infinity"));
    parser.add_option<double>(
        "max_time",
        "maximum time in seconds for generating patterns",
        "100",
        Bounds("0.0", "infinity"));
    parser.add_option<double>(
        "max_time_per_restart",
        "maximum time in seconds for each restart",
        "10",
        Bounds("0.0", "infinity"));
    parser.add_option<bool>(
        "saturate",
        "compute saturated cost partitionings",
        "true");
    parser.add_option<bool>(
        "only_sga_patterns",
        "only consider SGA patterns",
        "false");
    parser.add_option<bool>(
        "ignore_useless_patterns",
        "ignore patterns with only variables that are changed by free operators",
        "false");
    parser.add_option<bool>(
        "store_orders",
        "store orders (filtered_systematic() must be the first generator)",
        "true");
    vector<string> dead_end_treatments;
    dead_end_treatments.push_back("IGNORE");
    dead_end_treatments.push_back("ALL");
    dead_end_treatments.push_back("NEW");
    dead_end_treatments.push_back("NEW_FOR_CURRENT_ORDER");
    parser.add_enum_option(
        "dead_ends",
        dead_end_treatments,
        "how to handle dead ends",
        "NEW");
    vector<string> pattern_orders;
    pattern_orders.push_back("ORIGINAL");
    pattern_orders.push_back("RANDOM");
    pattern_orders.push_back("REVERSE");
    pattern_orders.push_back("PDB_SIZE_UP");
    pattern_orders.push_back("PDB_SIZE_DOWN");
    pattern_orders.push_back("CG_SUM_UP");
    pattern_orders.push_back("CG_SUM_DOWN");
    pattern_orders.push_back("CG_MIN_UP");
    pattern_orders.push_back("CG_MIN_DOWN");
    pattern_orders.push_back("CG_MAX_UP");
    pattern_orders.push_back("CG_MAX_DOWN");
    pattern_orders.push_back("NEW_VAR_PAIRS_UP");
    pattern_orders.push_back("NEW_VAR_PAIRS_DOWN");
    pattern_orders.push_back("ACTIVE_OPS_UP");
    pattern_orders.push_back("ACTIVE_OPS_DOWN");
    pattern_orders.push_back("ALT_TWO");
    pattern_orders.push_back("ACTIVE_OPS_UP_CG_MIN_DOWN");
    pattern_orders.push_back("CG_MIN_DOWN_ACTIVE_OPS_UP");
    parser.add_enum_option(
        "order",
        pattern_orders,
        "order in which to consider patterns of the same size",
        "ORIGINAL");
    utils::add_rng_options(parser);
    parser.add_option<bool>(
        "debug",
        "print debugging messages",
        "false");
}

static shared_ptr<PatternCollectionGenerator> _parse(OptionParser &parser) {
    add_options(parser);

    Options opts = parser.parse();
    if (parser.help_mode())
        return nullptr;

    if (parser.dry_run())
        return nullptr;

    return make_shared<PatternCollectionGeneratorFilteredSystematic>(opts);
}

static Plugin<PatternCollectionGenerator> _plugin("filtered_systematic", _parse);
}
