#include "saturated_cost_partitioning_online_heuristic.h"

#include "abstraction.h"
#include "cost_partitioning_heuristic.h"
#include "max_cost_partitioning_heuristic.h"
#include "order_generator.h"
#include "utils.h"

#include "../option_parser.h"
#include "../plugin.h"

#include "../task_utils/task_properties.h"
#include "../utils/logging.h"
#include "../utils/timer.h"

using namespace std;

namespace cost_saturation {
static const int IS_NOVEL = -3;
static const int IS_NOT_NOVEL = -4;

// TODO: avoid code duplication
static void extract_useful_abstraction_functions(
    const vector<CostPartitioningHeuristic> &cp_heuristics,
    const UnsolvabilityHeuristic &unsolvability_heuristic,
    Abstractions &abstractions,
    AbstractionFunctions &abstraction_functions) {
    int num_abstractions = abstractions.size();

    // Collect IDs of useful abstractions.
    vector<bool> useful_abstractions(num_abstractions, false);
    unsolvability_heuristic.mark_useful_abstractions(useful_abstractions);
    for (const auto &cp_heuristic : cp_heuristics) {
        cp_heuristic.mark_useful_abstractions(useful_abstractions);
    }

    abstraction_functions.reserve(num_abstractions);
    for (int i = 0; i < num_abstractions; ++i) {
        if (useful_abstractions[i]) {
            abstraction_functions.push_back(abstractions[i]->extract_abstraction_function());
        } else {
            abstraction_functions.push_back(nullptr);
        }
    }
    assert(abstraction_functions.size() == abstractions.size());
}


SaturatedCostPartitioningOnlineHeuristic::SaturatedCostPartitioningOnlineHeuristic(
    const options::Options &opts,
    Abstractions &&abstractions,
    CPHeuristics &&cp_heuristics,
    UnsolvabilityHeuristic &&unsolvability_heuristic)
    : Heuristic(opts),
      order_generator(opts.get<shared_ptr<OrderGenerator>>("orders")),
      saturator(static_cast<Saturator>(opts.get_enum("saturator"))),
      cp_function(get_cp_function_from_options(opts)),
      abstractions(move(abstractions)),
      cp_heuristics(move(cp_heuristics)),
      unsolvability_heuristic(move(unsolvability_heuristic)),
      interval(opts.get<int>("interval")),
      max_time(opts.get<double>("max_time")),
      max_size_kb(opts.get<int>("max_size")),
      use_evaluated_state_as_sample(opts.get<bool>("use_evaluated_state_as_sample")),
      costs(task_properties::get_operator_costs(task_proxy)),
      improve_heuristic(true),
      size_kb(0),
      num_evaluated_states(0),
      num_scps_computed(0) {
    if (opts.get<double>("max_optimization_time") != 0.0) {
        ABORT("Order optimization is not implemented for online SCP.");
    }
    if (opts.get<int>("max_orders") != INF) {
        ABORT("Limiting the number of orders is not implemented for online SCP.");
    }
    for (auto &cp : cp_heuristics) {
        size_kb += cp.estimate_size_in_kb();
    }

    fact_id_offsets.reserve(task_proxy.get_variables().size());
    int num_facts = 0;
    for (VariableProxy var : task_proxy.get_variables()) {
        fact_id_offsets.push_back(num_facts);
        num_facts += var.get_domain_size();
    }
    cout << "Fact ID offsets: " << fact_id_offsets << endl;

    if (interval == -1) {
        seen_facts.resize(num_facts, false);
    } else if (interval == -2) {
        seen_fact_pairs.resize(num_facts);
        for (int fact_id = 0; fact_id < num_facts; ++fact_id) {
            seen_fact_pairs[fact_id].resize(num_facts, false);
        }
    }

    timer = utils::make_unique_ptr<utils::Timer>(false);
}

SaturatedCostPartitioningOnlineHeuristic::~SaturatedCostPartitioningOnlineHeuristic() {
    print_statistics();
}

bool SaturatedCostPartitioningOnlineHeuristic::visit_fact_pair(int fact_id1, int fact_id2) {
    if (fact_id1 > fact_id2) {
        swap(fact_id1, fact_id2);
    }
    assert(fact_id1 < fact_id2);
    bool novel = !seen_fact_pairs[fact_id1][fact_id2];
    seen_fact_pairs[fact_id1][fact_id2] = true;
    return novel;
}

bool SaturatedCostPartitioningOnlineHeuristic::is_novel(
    OperatorID op_id, const GlobalState &state) {
    if (interval == -1) {
        bool novel = false;
        for (EffectProxy effect : task_proxy.get_operators()[op_id].get_effects()) {
            FactPair fact = effect.get_fact().get_pair();
            int fact_id = get_fact_id(fact.var, fact.value);
            if (!seen_facts[fact_id]) {
                seen_facts[fact_id] = true;
                novel = true;
            }
        }
        return novel;
    } else if (interval == -2) {
        int num_vars = fact_id_offsets.size();
        bool novel = false;
        for (EffectProxy effect : task_proxy.get_operators()[op_id].get_effects()) {
            FactPair fact1 = effect.get_fact().get_pair();
            int fact_id1 = get_fact_id(fact1.var, fact1.value);
            for (int var2 = 0; var2 < num_vars; ++var2) {
                if (fact1.var == var2) {
                    continue;
                }
                FactPair fact2(var2, state[var2]);
                int fact_id2 = get_fact_id(fact2.var, fact2.value);
                if (visit_fact_pair(fact_id1, fact_id2)) {
                    novel = true;
                }
            }
        }
        return novel;
    } else {
        ABORT("invalid value for interval");
    }
}

void SaturatedCostPartitioningOnlineHeuristic::notify_initial_state(
    const GlobalState &initial_state) {
    if (interval >= 1) {
        return;
    }

    heuristic_cache[initial_state].h = IS_NOVEL;
    int num_vars = fact_id_offsets.size();
    if (interval == -1) {
        for (int var = 0; var < num_vars; ++var) {
            seen_facts[get_fact_id(var, initial_state[var])] = true;
        }
    } else if (interval == -2) {
        for (int var1 = 0; var1 < num_vars; ++var1) {
            int fact_id1 = get_fact_id(var1, initial_state[var1]);
            for (int var2 = var1 + 1; var2 < num_vars; ++var2) {
                int fact_id2 = get_fact_id(var2, initial_state[var2]);
                visit_fact_pair(fact_id1, fact_id2);
            }
        }
    } else {
        ABORT("invalid value for interval");
    }
}

void SaturatedCostPartitioningOnlineHeuristic::notify_state_transition(
    const GlobalState &, OperatorID op_id, const GlobalState &global_state) {
    if (!improve_heuristic || interval >= 1) {
        return;
    }

    // We only need to compute novelty for new states.
    if (heuristic_cache[global_state].h == NO_VALUE) {
        timer->resume();
        if (is_novel(op_id, global_state)) {
            heuristic_cache[global_state].h = IS_NOVEL;
        } else {
            heuristic_cache[global_state].h = IS_NOT_NOVEL;
        }
        assert(heuristic_cache[global_state].dirty);
        timer->stop();
    }
}

int SaturatedCostPartitioningOnlineHeuristic::get_fact_id(int var, int value) const {
    return fact_id_offsets[var] + value;
}

bool SaturatedCostPartitioningOnlineHeuristic::should_compute_scp(
    const GlobalState &global_state) {
    if (interval > 0) {
        return num_evaluated_states % interval == 0;
    } else if (interval == -1 || interval == -2) {
        return heuristic_cache[global_state].h == IS_NOVEL;
    } else {
        ABORT("invalid value for interval");
    }
}

int SaturatedCostPartitioningOnlineHeuristic::compute_heuristic(
    const GlobalState &global_state) {
    if (improve_heuristic) {
        timer->resume();
    }

    State state = convert_global_state(global_state);
    vector<int> abstract_state_ids;
    if (improve_heuristic) {
        assert(!abstractions.empty() && abstraction_functions.empty());
        abstract_state_ids = get_abstract_state_ids(abstractions, state);
    } else {
        assert(abstractions.empty() && !abstraction_functions.empty());
        abstract_state_ids = get_abstract_state_ids(abstraction_functions, state);
    }

    if (unsolvability_heuristic.is_unsolvable(abstract_state_ids)) {
        if (improve_heuristic) {
            timer->stop();
        }
        return DEAD_END;
    }

    int max_h = compute_max_h_with_statistics(
        cp_heuristics, abstract_state_ids, num_best_order);

    if (improve_heuristic &&
        ((*timer)() >= max_time || size_kb >= max_size_kb)) {
        utils::Log() << "Stop heuristic improvement phase." << endl;
        improve_heuristic = false;
        utils::release_vector_memory(fact_id_offsets);
        utils::release_vector_memory(seen_facts);
        utils::release_vector_memory(seen_fact_pairs);
        extract_useful_abstraction_functions(
            cp_heuristics, unsolvability_heuristic, abstractions, abstraction_functions);
        utils::release_vector_memory(abstractions);
        print_diversification_statistics();
    }
    if (improve_heuristic && should_compute_scp(global_state)) {
        Order order = order_generator->compute_order_for_state(
            abstract_state_ids, num_evaluated_states == 0);

        CostPartitioningHeuristic cost_partitioning;
        vector<int> remaining_costs;
        if (saturator == Saturator::PERIMSTAR) {
            // Compute only the first SCP here, and the second below if h > max_h.
            remaining_costs = costs;
            cost_partitioning = compute_perim_saturated_cost_partitioning_change_costs(
                abstractions, order, remaining_costs, abstract_state_ids);
        } else {
            cost_partitioning = cp_function(abstractions, order, costs, abstract_state_ids);
        }
        ++num_scps_computed;

        int h = cost_partitioning.compute_heuristic(abstract_state_ids);

        if (saturator == Saturator::PERIMSTAR && h > max_h) {
            cost_partitioning.add(
                compute_saturated_cost_partitioning(
                    abstractions, order, remaining_costs, abstract_state_ids));
        }

        bool is_diverse = (use_evaluated_state_as_sample && h > max_h);
        if (is_diverse) {
            size_kb += cost_partitioning.estimate_size_in_kb();
            cp_heuristics.push_back(move(cost_partitioning));
            utils::Log() << "Stored SCPs in " << *timer << ": "
                         << cp_heuristics.size() << endl;
        }
        max_h = max(max_h, h);
    }
    if (improve_heuristic) {
        timer->stop();
    }

    ++num_evaluated_states;
    return max_h;
}

void SaturatedCostPartitioningOnlineHeuristic::print_diversification_statistics() const {
    // Print the number of stored lookup tables.
    int num_stored_lookup_tables = 0;
    for (const auto &cp_heuristic: cp_heuristics) {
        num_stored_lookup_tables += cp_heuristic.get_num_lookup_tables();
    }
    utils::Log() << "Stored lookup tables: " << num_stored_lookup_tables << endl;

    // Print the number of stored values.
    int num_stored_values = 0;
    for (const auto &cp_heuristic : cp_heuristics) {
        num_stored_values += cp_heuristic.get_num_heuristic_values();
    }
    utils::Log() << "Stored values: " << num_stored_values << endl;

    utils::Log() << "Time for improving heuristic: " << *timer << endl;
    utils::Log() << "Estimated heuristic size: " << size_kb << " KiB" << endl;
}

void SaturatedCostPartitioningOnlineHeuristic::print_statistics() const {
    if (improve_heuristic) {
        print_diversification_statistics();
    }
    cout << "Computed SCPs: " << num_scps_computed << endl;
    cout << "Stored SCPs: " << cp_heuristics.size() << endl;
}


static shared_ptr<Heuristic> _parse(OptionParser &parser) {
    parser.document_synopsis(
        "Saturated cost partitioning online heuristic",
        "");

    // TODO: document that online version is not consistent.
    prepare_parser_for_cost_partitioning_heuristic(parser);
    add_saturator_option(parser);
    add_order_options_to_parser(parser);

    parser.add_option<int>(
        "interval",
        "compute SCP for every interval-th state",
        "1",
        Bounds("-2", "infinity"));
    parser.add_option<bool>(
        "use_evaluated_state_as_sample",
        "keep CP if it improves the overall heuristic  value of the evaluated state",
        "false");

    Options opts = parser.parse();
    if (parser.help_mode())
        return nullptr;

    if (parser.dry_run())
        return nullptr;

    shared_ptr<AbstractTask> task = opts.get<shared_ptr<AbstractTask>>("transform");
    TaskProxy task_proxy(*task);
    vector<int> costs = task_properties::get_operator_costs(task_proxy);
    Abstractions abstractions = generate_abstractions(
        task, opts.get_list<shared_ptr<AbstractionGenerator>>("abstractions"));
    UnsolvabilityHeuristic unsolvability_heuristic(abstractions);
    CPHeuristics cp_heuristics = {};
    shared_ptr<OrderGenerator> order_generator = opts.get<shared_ptr<OrderGenerator>>("orders");
    order_generator->initialize(abstractions, costs);

    return make_shared<SaturatedCostPartitioningOnlineHeuristic>(
        opts,
        move(abstractions),
        move(cp_heuristics),
        move(unsolvability_heuristic));
}

static Plugin<Evaluator> _plugin("scp_online", _parse);
}
