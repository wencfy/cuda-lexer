#include "lexer/lexical_grammar.hpp"
#include "lexer/parallel_lexer.hpp"
#include "lexer/fsa.hpp"
#include "hash_util.hpp"

#include <algorithm>
#include <unordered_map>
#include <queue>
#include <cassert>
#include <iostream>

namespace lexer {
    using StateIndex = ParallelLexer::StateIndex;
    using Transition = ParallelLexer::Transition;

    struct ParallelState {
        std::vector<Transition> transitions;

        ParallelState(size_t states);
        void merge(const ParallelState& other);

        struct Hash {
            size_t operator()(const ParallelState& ps) const;
        };
    };

    ParallelState::ParallelState(size_t states):
        transitions(states) {
    }

    void ParallelState::merge(const ParallelState& other) {
        for (auto& state : this->transitions) {
            state = other.transitions[state.result_state];
        }
    }

    bool operator==(const ParallelState& lhs, const ParallelState& rhs) {
        return std::equal(
            lhs.transitions.begin(),
            lhs.transitions.end(),
            rhs.transitions.begin(),
            rhs.transitions.end(),
            [](const auto& lhs, const auto& rhs) {
                return lhs.result_state == rhs.result_state && lhs.produces_lexeme == rhs.produces_lexeme;
            }
        );
    }

    size_t ParallelState::Hash::operator()(const ParallelState& ps) const {
        size_t hash = 0;
        for (auto state : ps.transitions) {
            hash = hash_combine(hash, state.result_state);
            hash = hash_combine(hash, state.produces_lexeme);
        }
        return hash;
    }
}

namespace lexer {
    ParallelLexer::Transition::Transition():
        result_state(REJECT), produces_lexeme(false) {}

    ParallelLexer::Transition::Transition(StateIndex result_state, bool produces_lexeme):
        result_state(result_state), produces_lexeme(produces_lexeme) {}

    ParallelLexer::MergeTable::MergeTable():
        num_states(0), capacity(0), merge_table(nullptr) {}

    void ParallelLexer::MergeTable::resize(size_t new_num_states) {
        if (new_num_states <= this->capacity) {
            this->num_states = new_num_states;
            return;
        }

        // Crude computation of new capacity, but it'll do
        auto new_capacity = std::max(MIN_SIZE, this->capacity);
        while (new_capacity < new_num_states)
            new_capacity *= GROW_FACTOR;

        auto new_ptr = std::make_unique<Transition[]>(new_capacity * new_capacity);
        for (size_t second = 0; second < this->num_states; ++second) {
            for (size_t first = 0; first < this->num_states; ++first) {
                new_ptr[first + second * new_capacity] = (*this)(first, second);
            }
        }

        this->num_states = new_num_states;
        this->capacity = new_capacity;
        this->merge_table = std::move(new_ptr);
    }

    size_t ParallelLexer::MergeTable::index(StateIndex first, StateIndex second) const {
        assert(first <= this->num_states);
        assert(second <= this->num_states);
        return first + second * this->capacity;
    }

    auto ParallelLexer::MergeTable::operator()(StateIndex first, StateIndex second) -> Transition& {
        return this->merge_table[this->index(first, second)];
    }

    auto ParallelLexer::MergeTable::operator()(StateIndex first, StateIndex second) const -> const Transition& {
        return this->merge_table[this->index(first, second)];
    }

    size_t ParallelLexer::MergeTable::states() const {
        return this->num_states;
    }

    ParallelLexer::ParallelLexer(const LexicalGrammar* g) {
        auto dfa = FiniteStateAutomaton::build_lexer_dfa(g);

        auto seen = std::unordered_map<ParallelState, StateIndex, ParallelState::Hash>();
        auto states = std::vector<ParallelState>();
        auto transitions = std::vector<Transition>();

        auto enqueue = [&](ParallelState&& ps) {
            auto [it, inserted] = seen.insert({std::move(ps), states.size()});
            if (inserted) {
                states.push_back(it->first);
                this->merge_table.resize(it->second + 1);
            }
            return it->second;
        };

        // Insert the initial states, we need to insert one for every character.
        // States indices of the DFA are mapped to the initial parallel states indices.
        {
            auto initial_states = std::vector<ParallelState>(FiniteStateAutomaton::MAX_SYM + 1, ParallelState(dfa.num_states()));
            // std::cout << dfa.num_states() << std::endl;
            for (size_t src = 0; src < dfa.num_states(); ++src) {
                // std::cout << "src: " << src << ": " << dfa[src].transitions.size() << std::endl;
                // if (dfa[src].lexeme) {
                //     std::cout << dfa[src].lexeme->name << std::endl;
                // }
                for (const auto [sym, dst, produces_lexeme] : dfa[src].transitions) {
                    assert(sym.has_value()); // Not a DFA
                    initial_states[sym.value()].transitions[src].result_state = dst;
                    initial_states[sym.value()].transitions[src].produces_lexeme = produces_lexeme;
                }
            }

            this->initial_states.resize(initial_states.size());
            for (size_t sym = 0; sym < initial_states.size(); ++sym) {
                auto& state = initial_states[sym];
                this->initial_states[sym].produces_lexeme = state.transitions[START].produces_lexeme;
                this->initial_states[sym].result_state = enqueue(std::move(state));

                // if (this->initial_states[sym].result_state) {
                //     printf("%c %lu %d\n", sym, this->initial_states[sym].result_state);
                // }
            }
        }

        
        // Add the identity mapping, which is required for futhark's scan operation.
        {
            auto identity = ParallelState(dfa.num_states());
            for (size_t i = 0; i < identity.transitions.size(); ++i) {
                identity.transitions[i].result_state = i;
            }
            this->identity_state_index = enqueue(std::move(identity));
        }
        // std::cout << "identity_state_index: " << this->identity_state_index << std::endl;

        auto merge = [&](StateIndex i, StateIndex j) {
            StateIndex result;
            // We need to handle the identity stage separately here, as we
            // normally just copy the produces_lexeme property from the right hand site,
            // but if the right hand site is the identity state thats not correct.
            if (i == this->identity_state_index) {
                result = j;
            } else if (j == this->identity_state_index) {
                result = i;
            } else {
                auto ps = states[i];
                ps.merge(states[j]);
                result = enqueue(std::move(ps));
            }

            bool produces_lexeme = states[result].transitions[START].produces_lexeme;
            this->merge_table(i, j) = {result, produces_lexeme};
        };

        // Repeatedly perform the merges until no new merge is added
        for (StateIndex i = 0; i < states.size(); ++i) {
            if (i % 20) {
                std::cout << "\rGenerating Merge Table" << std::string((i / 20) % 4, '.');
                std::cout.flush();
            }
            auto first = states[i];
            for (StateIndex j = 0; j < states.size(); ++j) {
                merge(i, j);
                merge(j, i);
            }
        }
        std::cout << "\rDone!                    \n";


        // std::cout << states.size() << std::endl;
        // for (int i = 0; i < states.size(); i++) {
        //     for (int j = 0; j < states.size(); j++) {
        //         if (this->merge_table(i, j).produces_lexeme) {
        //             std::cout << i << " " << j << std::endl;
        //         }
        //     }
        // }

        // Compute the final state mapping
        this->final_states.resize(seen.size(), nullptr);
        for (const auto& [ps, i] : seen) {
            this->final_states[i] = dfa[ps.transitions[START].result_state].lexeme;
        }
    }

    void ParallelLexer::dump_sizes(std::ostream& out) const {
        printf("Initial states table: %lu element\n", this->initial_states.size());
        printf("Merge table: %lu² elements = %lu elements\n", this->merge_table.states(), this->merge_table.states() * this->merge_table.states());
        printf("Final states table: %lu elements\n", this->final_states.size());
    }
};
