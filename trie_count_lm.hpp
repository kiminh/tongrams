#pragma once

#include "vectors/sorted_array.hpp"

namespace tongrams
{
    template<typename Vocabulary,
             typename Mapper,
             typename Values,
             typename Ranks,
             typename Grams,
             typename Pointers>
    struct trie_count_lm
    {
        typedef sorted_array<Grams,
                             Ranks,
                             Pointers> sorted_array_type;
        struct builder
        {
            builder()
            {}

            builder(const char* input_dir,
                    uint8_t order,
                    uint8_t remapping_order)
                : m_input_dir(input_dir)
                , m_order(order)
                , m_remapping_order(remapping_order)
            {
                building_util::check_order(m_order);
                building_util::check_remapping_order(m_remapping_order);
                m_arrays.reserve(m_order);

                typename Values::builder counts_builder(m_order);

                for (uint8_t order = 1; order <= m_order; ++order) {
                    std::string filename;
                    util::input_filename(m_input_dir, order, filename);
                    grams_gzparser gp(filename.c_str());
                    
                    std::vector<uint64_t> counts;
                    counts.reserve(gp.num_lines());

                    m_arrays.push_back(sorted_array_type(gp.num_lines()));
                    util::logger("Reading " + std::to_string(order) + "-grams counts");
                    for (auto const& l: gp) {
                        counts.push_back(l.count);
                    }
                    
                    counts_builder.build_sequence(counts.begin(), counts.size());
                }

                util::logger("Building vocabulary");
                build_vocabulary(counts_builder);
                
                for (uint8_t order = 2; order <= m_order; ++order)
                {    
                    std::string order_grams(std::to_string(order) + "-grams");
                    std::string prv_order_filename;
                    std::string cur_order_filename;
                    util::input_filename(m_input_dir, order - 1, prv_order_filename);
                    util::input_filename(m_input_dir, order, cur_order_filename);

                    grams_gzparser gp_prv_order(prv_order_filename.c_str());
                    grams_gzparser gp_cur_order(cur_order_filename.c_str());
                    
                    uint64_t n = gp_cur_order.num_lines();
                    
                    typename sorted_array_type::builder
                            sa_builder(n,
                                       m_vocab.size(),                  // max_gram_id
                                       counts_builder.size(order - 1),  // max_count_rank
                                       0);                              // quantization_bits not used

                    uint64_t num_pointers = gp_prv_order.num_lines() + 1;

                    // NOTE: we could use this to save pointers' space
                    // compact_vector::builder pointers(num_pointers, util::ceil_log2(n + 1));
                    std::vector<uint64_t> pointers;
                    pointers.reserve(num_pointers);

                    util::logger("Building " + order_grams);
                    build_ngrams(order,
                                 pointers,
                                 gp_cur_order,
                                 gp_prv_order,
                                 counts_builder,
                                 sa_builder);
                    assert(pointers.back() == n);
                    assert(pointers.size() == num_pointers);
                    util::logger("Writing " + order_grams);
                    sa_builder.build(m_arrays[order - 1], pointers, order, value_type::count);
                    util::logger("Writing pointers");
                    sorted_array_type::builder::build_pointers(m_arrays[order - 2], pointers);
                }
                
                counts_builder.build(m_distinct_counts);
            }

            void build(trie_count_lm<Vocabulary,
                                     Mapper,
                                     Values,
                                     Ranks,
                                     Grams,
                                     Pointers>& trie) {
                trie.m_order = m_order;
                trie.m_remapping_order = m_remapping_order;
                trie.m_distinct_counts.swap(m_distinct_counts);
                trie.m_vocab.swap(m_vocab);
                trie.m_arrays.swap(m_arrays);
                builder().swap(*this);
            }

            void swap(builder& other) {
                std::swap(m_order, other.m_order);
                std::swap(m_remapping_order, other.m_remapping_order);
                m_distinct_counts.swap(other.m_distinct_counts);
                m_vocab.swap(other.m_vocab);
                m_arrays.swap(other.m_arrays);
            }

        private:
            const char* m_input_dir;
            uint8_t m_order;
            uint8_t m_remapping_order;
            Mapper m_mapper;
            Values m_distinct_counts;
            Vocabulary m_vocab;
            std::vector<sorted_array_type> m_arrays;

            void build_vocabulary(typename Values::builder const& counts_builder)
            {
                size_t available_ram = sysconf(_SC_PAGESIZE)
                                     * sysconf(_SC_PHYS_PAGES);
                grams_counts_pool unigrams_pool(available_ram);

                std::string filename;
                util::input_filename(m_input_dir, 1, filename);
                unigrams_pool.load_from<grams_gzparser>(filename.c_str());
                
                auto& unigrams_pool_index = unigrams_pool.index();
                uint64_t n = unigrams_pool_index.size();

                std::vector<byte_range> bytes;
                bytes.reserve(n);

                typename sorted_array_type::builder
                    sa_builder(n, 0, counts_builder.size(0), 0);

                for (auto const& record: unigrams_pool_index) {
                    bytes.push_back(record.gram);
                    uint64_t rank = counts_builder.rank(0, record.count);
                    sa_builder.add_count_rank(rank);
                }

                sa_builder.build_counts_ranks(m_arrays.front(), 1);

                compact_vector::builder ids_cvb(n, util::ceil_log2(n + 1));
                for (uint64_t id = 0; id < n; ++id) {
                    ids_cvb.push_back(id);
                }
                
                // NOTE:
                // build vocabulary excluding null terminators
                // from unigrams strings so that we can lookup
                // for any substring of a n-gram
                // without allocating a std::string
                typename Vocabulary::builder
                    builder(bytes,
                            compact_vector(ids_cvb),
                            identity_adaptor());
                builder.build(m_vocab);
            }

            template<typename T>
            void build_ngrams(uint8_t order,
                              T& pointers,
                              grams_gzparser& gp_cur_order,
                              grams_gzparser& gp_prv_order,
                              typename Values::builder const& counts_builder,
                              typename sorted_array_type::builder& sa_builder)
            {
                pointers.push_back(0);
                identity_adaptor adaptor;

                uint64_t pos = 0;

                auto prv_order_begin = gp_prv_order.begin();
                auto prv_order_end = gp_prv_order.end();

                auto end = gp_cur_order.end();
                for (auto begin = gp_cur_order.begin();
                     begin != end; ++begin)
                {
                    auto const& l = *begin;
                    auto gram = l.gram;

                    // NOTE:
                    // in a FORWARD trie, 'pattern' is the predecessor of 'gram'
                    // and 'token' is the last token of 'gram'
                    byte_range pattern = bytes::predecessor(gram);
                    byte_range token(pattern.second + 1, gram.second);

                    while (prv_order_begin != prv_order_end // NOTE:
                                                            // this test is here only to
                                                            // guarantee termination in
                                                            // case of wrong data:
                                                            // 'pattern' should ALWAYS
                                                            // be found within previous order grams
                           && !bytes::equal_bytes(pattern, (*prv_order_begin).gram)) {
                        pointers.push_back(pos);
                        ++prv_order_begin;
                    }

                    // check correctness of ngram file
                    if (prv_order_begin == prv_order_end) {
                        std::cerr << "ngram file contains wrong data:\n";
                        std::cerr << "\t'" << std::string(pattern.first, pattern.second) << "'"
                                  << " should have been found within previous order grams"
                                  << std::endl;
                        exit(1);
                    }

                    ++pos;

                    uint64_t token_id = 0;
                    m_vocab.lookup(token, token_id, adaptor);

                    // apply remapping if Mapper has to
                    if (Mapper::context_remapping && order > m_remapping_order + 1) {
                        token_id = m_mapper.map_id(gram, token_id, &m_vocab,
                                                   &m_arrays.front(),
                                                   m_remapping_order,
                                                   false); // FORWARD trie
                    }

                    sa_builder.add_gram(token_id);

                    uint64_t rank = counts_builder.rank(order - 1, l.count);
                    sa_builder.add_count_rank(rank);
                }

                // set remaining pointers (if any)
                for (; prv_order_begin != prv_order_end;
                     ++prv_order_begin) {
                    pointers.push_back(pos);
                }
            }
        };

        trie_count_lm()
            : m_order(0)
            , m_remapping_order(0)
        {}

        template <typename T, typename Adaptor>
        uint64_t lookup(T gram, Adaptor adaptor)
        {
            static uint64_t word_ids[global::max_order];
            uint64_t order_m1 =
                m_mapper.map_query(adaptor(gram), word_ids,
                                   &m_vocab, &m_arrays.front(),
                                   m_remapping_order);
            assert(order_m1 < m_order);

            if (!order_m1) {
                uint64_t count_rank = m_arrays[0].count_rank(word_ids[0]);
                return m_distinct_counts.access(0, count_rank);
            }

            auto r = m_arrays[0].range(word_ids[0]);
            for (uint64_t i = 1; i < order_m1; ++i) {
                m_arrays[i].next(r, word_ids[i]);
            }
            
            uint64_t pos = m_arrays[order_m1].position(r, word_ids[order_m1]);
            uint64_t count_rank = m_arrays[order_m1].count_rank(pos);
            return m_distinct_counts.access(order_m1, count_rank);
        }

        inline uint64_t order() const {
            return uint64_t(m_order);
        }

        uint64_t remapping_order() const {
            return uint64_t(m_remapping_order);
        }

        void print_stats(size_t bytes) const;

        uint64_t size() const {
            uint64_t size = 0;
            for (auto const& a: m_arrays) {
                size += a.size();
            }
            return size;
        }

        void save(std::ostream& os) const {
            util::save_pod(os, &m_order);
            util::save_pod(os, &m_remapping_order);
            m_distinct_counts.save(os);
            m_vocab.save(os);
            m_arrays.front().save(os, 1, value_type::count);
            for (uint8_t order_m1 = 1;
                         order_m1 < m_order; ++order_m1) {
                m_arrays[order_m1].save(os, order_m1 + 1,
                                        value_type::count);
            }
        }

        void load(std::istream& is) {
            util::load_pod(is, &m_order);
            util::load_pod(is, &m_remapping_order);
            m_distinct_counts.load(is, m_order);
            m_vocab.load(is);
            m_arrays.resize(m_order);
            m_arrays.front().load(is, 1, value_type::count);
            for (uint8_t order_m1 = 1;
                         order_m1 < m_order; ++order_m1) {
                m_arrays[order_m1].load(is, order_m1 + 1,
                                        value_type::count);
            }
        }

    private:
        uint8_t m_order;
        uint8_t m_remapping_order;
        Mapper m_mapper;
        Values m_distinct_counts;
        Vocabulary m_vocab;
        std::vector<sorted_array_type> m_arrays;
    };
}
