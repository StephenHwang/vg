//#define debugIndex
//#define debugDistance
//#define debugSubgraph

#include "snarl_distance_index.hpp"

using namespace std;
namespace vg {


///////////////////////////////////////////////////////////////////////////////////////////////////
//Constructor
SnarlDistanceIndex::SnarlDistanceIndex(const HandleGraph* graph, const HandleGraphSnarlFinder* snarl_finder){
    TemporaryDistanceIndex temp_index(graph, snarl_finder);
    vector<const TemporaryDistanceIndex*> indexes;
    indexes.emplace_back(&temp_index);
    snarl_tree_records = get_snarl_tree_records(indexes);
}

SnarlDistanceIndex::TemporaryDistanceIndex::TemporaryDistanceIndex(const HandleGraph* graph, const HandleGraphSnarlFinder* snarl_finder) :
    min_node_id(graph->min_node_id), max_node_id(graph->max_node_id){
    //Construct the distance index using the snarl decomposition
    //traverse_decomposition will visit all structures (including trivial snarls), calling
    //each of the given functions for the start and ends of the snarls and chains
    temp_node_records.resize(max_node_id, min_node_id);


    
    //Stores unfinished records, as type of record and offset into appropriate vector
    //(temp_node/snarl/chain_records)
    vector<pair<temp_record_t, size_t>> stack;

    //Keep track of all chains we find, in order. After the initial pass over the snarl tree,
    //which fills in the connectivity, go back through in reverse order to fill in distances
    //and also catch all nodes that weren't boundaries along chains
    vector<pair<temp_record_t, size_t>> all_chains;
    


    //Go through the decomposition top down and record the connectivity of the snarls and chains
    snarl_finder->traverse_decomposition(
    [&](handle_t chain_start_handle) {
        //This gets called when a new chain is found, starting at the start handle going into chain

        //Fill in node in chain
        stack.emplace_back(TEMP_CHAIN, temp_chain_records.size());
        id_t node_id = graph->get_id(chain_start_handle);
        temp_chain_records.emplace_back();
        temp_chain_records.back().start_node_id = node_id; 
        temp_chain_records.back().start_node_rev = graph->get_is_reverse(chain_start_handle);

        //And the node record itself
        temp_node_records[node_id-min_node_id].node_id = node_id;
        temp_node_records[node_id-min_node_id].node_length = graph->get_length(chain_start_handle);
        temp_node_records[node_id-min_node_id].rank_in_parent = 0;
        temp_node_records[node_id-min_node_id].reversed_in_parent = graph->get_is_reverse(chain_start_handle);
        temp_node_records[node_id-min_node_id].parent = stack.back();

    },
    [&](handle_t chain_end_handle) {
        //This gets called at the end of a chain, facing out

        //Done with this chain
        pair<temp_record_t, size_t> chain_index = stack.back();
        stack.pop_back();
        assert(chain_index.first == TEMP_CHAIN);
        TemporaryChainRecord& chain_index = temp_chain_records[chain_index.second];
        id_t node_id = graph->get_id(chain_end_handle);

        //Fill in node in chain
        chain_index.end_node_id = node_id;
        chain_index.end_node_rev = graph->get_is_reverse(chain_end_handle);
        if (stack.empty()) {
            //If this was the last thing on the stack, then this was a root
            chain_index.parent = make_pair(TEMP_ROOT, 0);
        } else {
            chain_index.parent = stack.back();
            stack.back().children.emplace_back(chain_index);
        }


        all_chains.emplace_back(chain_index);
    },
    [&](handle_t snarl_start_handle) {
        //This gets called at the beginning of a new snarl facing in
        stack.emplace_back(TEMP_SNARL, temp_snarl_records.size());
        temp_snarl_records.emplace_back();
        temp_snarl_records.back().start_node_id = graph->get_id(snarl_start_handle);
        temp_snarl_records.back().start_node_rev = graph->get_is_reverse(snarl_start_handle);
        temp_snarl_records.back().start_node_length = graph->get_length(snarl_start_handle);
    },
    [&](handle_t snarl_end_handle){
        //This gets called at the end of the snarl facing out
        pair<temp_record_t, size_t> snarl_index = stack.back();
        stack.pop_back();
        assert(chain_index.first == TEMP_SNARL);
        assert(stack.back().first == TEMP_CHAIN);
        TemporarySnarlRecord& snarl_index = temp_snarl_records[snarl_index.second];

        //Record the end node in the snarl
        snarl_index.end_node_id = graph->get_id(snarl_end_handle);
        snarl_index.end_node_rev = graph->get_is_reverse(snarl_end_handle);
        snarl_index.end_node_length = graph->get_length(snarl_end_handle);
        //Record the snarl as a child of its chain
        if (stack.empty()) {
            //If this was the last thing on the stack, then this was a root
            //TODO: I'm not sure if this would get put into a chain or not
            snarl_index.parent = make_pair(TEMP_ROOT, 0);
        } else {
            snarl_index.parent = stack.back();
            stack.back().children.emplace_back(snarl_index);
        }

        //Record the node itself (this gets done for the start of the chain, and ends of snarls
        temp_node_records[node_id-min_node_id].node_id = node_id;
        temp_node_records[node_id-min_node_id].node_length = graph->get_length(chain_end_handle);
        temp_node_records[node_id-min_node_id].reversed_in_parent = graph->get_is_reverse(chain_start_handle);
        temp_node_records[node_id-min_node_id].parent = chain_index;
        chain_index.children.emplace_back(snarl_index);
        chain_index.children.emplace_back(TEMP_NODE, node_id-min_node_id);
    });


    //Now go through the decomposition bottom up, to fill in the distances
    for (int i = temp_chain_records.size() ; i >= 0 ; i--) {
        TemporaryChainRecord& temp_chain_record = temp_chain_records[all_chains[i].second];
        //Add the first values for the prefix sum and backwards loop vectors
        temp_chain_record.prefix_sum.emplace_back(0);
        temp_chain_record.backward_loops.emplace_back(std::numeric_limits<int64_t>::max());

        for (pair<temp_record_t, size_t>& chain_child : temp_chain_record.children){ 
            //Go through each of the children in the chain, skipping nodes 
            //The snarl may be trivial, in which case don't fill in the distances

            if (chain_child.first == TEMP_SNARL){
                //This is where all the work gets done. Need to go through the snarl and add 
                //all distances, then add distances to the chain that this is in
                //The parent chain will be the last thing in the stack
                TemporarySnarlRecord& temp_snarl_record = temp_snarl_records[chain_child.second];

                if (temp_snarl_record.is_trivial_snarl) {
                    //For a trivial snarl, don't bother filling in the distances but still add the prefix sum vector
                    temp_chain_record.prefix_sum.emplace_back(chain_index.prefix_sum.back() + 
                                                              temp-snarl_record.start_node_length);
                    //TODO: I think that adding to max() should still be infinity???
                    temp_chain_record.backward_loops.emplace_back(chain_index.backward_loops.back() + 2 * snarl_record.start_node_length);
                } else {

                    //Fill in this snarl's distances
                    populate_snarl_index(temp_snarl_record, graph);

                    //And get the distance values for the end node in the chain
                    temp_chain_record.prefix_sum.emplace_back(chain_index.prefix_sum.back() + 
                                                        temp_snarl_record.min_length + temp_snarl_record.start_node_length);
                    temp_chain_record.backward_loops.emplace_back(temp_snarl_record.loop_end,
                        std::min(chain_index.backward_loops.back() + 2 * (temp_snarl_record.start_node_length + temp_snarl_record.min_length));
                }
            }
        }
        //Now that we've gone through all the snarls in the chain, fill in the forward loop vector 
        temp_chain_record.resize(chain_index.prefix_sum.size(), std::numeric_limits<int64_t>::max());
        for (int j = temp_chain_record.children.size() ; j >= 0 ; j--) {
            if (temp_chain_record.children[j].first == TEMP_SNARL){
                TemporarySnarlRecord& temp_snarl_record = temp_snarl_records[temp_chain_record.children[j].second];
                if (temp_snarl_record.is_trivial_snarl) {
                    temp_chain_record.forward_loops[j] = temp_chain_record.forward_loops[j+1] + 2*temp_snarl_record.end_node_length;
                } else {
                    temp_chain_record.forward_loops[j] = std::min(temp_chain_record.forward_loops[j+1] + 2*temp_snarl_record.end_node_length,
                                                                   temp_snarl_record.loop_start);
                }
            }
        }
    }
}

//Fill in the snarl index.
//The index will already know its boundaries 
void populate_snarl_index(TemporarySnarlRecord& temp_snarl_record, pair<temp_record_t, size_t> snarl_index,
                          const HandleGraph* graph, const HandleGraphSnarlFinder* snarl_finder) {


    //Helper function to find the ancestor of a node that is a child of this snarl
    pair<temp_record_t, size_t> get_ancestor_of_node (pair<temp_record_t, size_t> curr_index) {
        if (curr_index.first == TEMP_ROOT){
            //If this is a node that hasn't been initialized yet, then it is a child of this snarl
            return curr_index;
        } else {
            //This is a child that isn't a node, so it must be a chain
            pair<temp_record_t, size_t> parent_index = temp_node_records[curr_id-min_node_id];
            while (parent_index != snarl_index) {
                curr_index=parent_index;
                parent_index = parent_index.first == TEMP_SNARL ? temp_snarl_records[parent_index.second].parent_index
                                                                : temp_chain_records[parent_index.second].parent_index;
                if (parent_index.first == TEMP_ROOT) {
                    assert(false);//TODO: Take this out
                }
            }
            return curr_index;
        }
    }

    handle_t snarl_start_in = graph->get_handle(temp_snarl_record.start_node_id, temp_snarl_record.start_node_rev);
    handle_t snarl_start_out = graph->get_handle(temp_snarl_record.start_node_id, !temp_snarl_record.start_node_rev);
    handle_t snarl_end_out = graph->get_handle(temp_snarl_record.end_node_id, temp_snarl_record.end_node_rev);
    handle_t snarl_end_in = graph->get_handle(temp_snarl_record.end_node_id, !temp_snarl_record.end_node_rev);


    //Collect all nodes in a vector
    unordered_set<pair<temp_record_t, size_t>> all_nodes;
    all_nodes.emplace(TEMP_NODE, temp_snarl_record.start_node_id-min_node_id);
    vector<handle_t> next_handles;
    temp_node_records[temp_snarl_record.start_node_rev-min_node_id].rank_in_parent = 0;
    next_handles.emplace_back(snarl_start_in);
    next_handles.emplace_back(snarl_end_in);
    while (!next_handles.empty()) {
        handle_t next_handle = std::move(next_handles.back());
        next_handles.pop();
        graph->follow_edges(next_handle, false, [&](const handle_t& h) {
            if (h != snarl_end_out && h != snarl_start_out) {
                id_t curr_id = graph->get_id(h);
                if (temp_node_records[curr_id-min_node_id].node_id==0){
                    //If we haven't seen this node before, record it
                    temp_node_records[curr_id-min_node_id].node_id = curr_id;
                    temp_node_records[curr_id-min_node_id].node_length = graph->get_length(h);
                    temp_node_records[curr_id-min_node_id].reversed_in_parent = false;
                    temp_node_records[curr_id-min_node_id].parent = snarl_index;
                    temp_node_records[curr_id-min_node_id].rank_in_parent= all_nodes.size();
                    all_nodes.emplace(TEMP_NODE, curr_id-min_node_id);
                next_handles.emplace_back(h);
                } else {
                    //This node must be the start or end of a chain
                    assert(temp_node_records[curr_id-min_node_id].node_id=curr_id);
                    pair<temp_record_t, size_t> child_chain_index = get_ancestor_of_node(make_pair(TEMP_NODE, curr_id-min_node_id));
                    if (child_chain_index.first == TEMP_NODE) {
                        temp_node_records[child_chain_index.second].parent = snarl_index;
                        temp_node_records[child_chain_index.second].rank_in_parent = all_nodes.size();
                    } else {
                        assert(child_chain_index.second = TEMP_CHAIN);
                        assert(temp_chain_records[child_chain_index.second].parent == snarl_index);
                        temp_chain_records[child_chain_index.second].rank_in_parent = all_nodes.size();
                    }
                    all_nodes.emplace(child_chain_index);
                }
            }
        });
    }
    temp_node_records[temp_snarl_record.end_node_rev-min_node_id].rank_in_parent = all_nodes.size();
    all_nodes.emplace(TEMP_NODE, temp_snarl_record.end_node_id-min_node_id);
    temp_snarl_record.node_count = all_nodes.size();


    //Now go through each of the children and add distances from that child to everything reachable from it
    for (pair<temp_record_t, size_t> start_index : all_nodes) {
        //Start from either direction for all nodes except start and end
        vector<bool> directions;
        if (start_index.second == temp_snarl_record.start_node_id-min_node_id) {
            directions.emplace_back(temp_snarl_record.start_node_rev);
        } else if (start_index.second == temp_snarl_record.end_node_id-min_node_id){
            directions.emplace_back(!temp_snarl_record.end_node_rev);
        } else {
            directions.emplace_back(true);
            directions.emplace_back(false);
        }
        for (bool start_rev : directions) {
            //Start a dijkstra traversal from start_index going in the direction indicated by start_rev
            //Record the distances to each node (child of the snarl) found

        }
    }
}

vector<size_t> SnarlDistanceIndex::get_snarl_tree_records(const vector<const TemporaryDistanceIndex*>& temporary_indexes) {
    //Convert temporary distance indexes into the final index stored as a single vector
    vector<size_t> new_records;
    size_t total_index_size = 0;
    size_t total_node_count = 0;
    size_t total_component_count = 0;
    id_t min_node_id = 0;
    //Go through each of the indexes to count how many nodes, components, etc
    for (const TemporaryDistanceIndex* temp_index : temporary_indexes) {
        total_index_size += temp_index->index_size;
        total_node_count += temp_index->node_count;
        total_component_count += temp_index->root_structure_count;
        min_node_id = min_node_id == 0 ? temp_index->min_node_id 
                                       : std::min(min_node_id, temp_index->min_node_id);
    }
    new_records.reserve(total_index_size);
    //Allocate memory for the root and the nodes
    //TODO: Could also write directly into snarl_tree_records
    RootRecordConstructor root_record(0, total_component_count, total_node_count, min_node_id, new_records);
    root_record.set_connected_component_count(total_component_count);
    root_record.set_node_count(total_node_count);
    root_record.set_min_node_id(min_node_id);

    //Now go through each of the chain/snarl indexes and copy them into new_records
    //Walk down the snarl tree
    //TODO: For now I'm assuming that I'm including distances
    //TODO: What about connectivity?
    // maps <index into temporary_indexes, <record type, index into chain/snarl/node records>> to new offset
    std::unordered_map<pair<size_t, pair<temp_record_t, size_t>>, size_t> record_to_offset;
    for (size_t temp_index_i = 0 ; temp_index_i < temporary_indexes.size() ; temp_index_i++) {
        //Any root will point to the same root
        record_to_offset.emplace(make_pair(temp_index_i,make_pair(TEMP_ROOT, 0)), 0);
    }
    for (size_t temp_index_i = 0 ; temp_index_i < temporary_indexes.size() ; temp_index_i++) {
        const TemporaryDistanceIndex* temp_index = temporary_indexes[temp_index_i];
        //Get a stack of temporary snarl tree records to be added to the index
        //Initially, it contains only the root components
        //This reverses the order of the connected components but I don't think that matters
        //TODO: this is copying the components but it shouldn't be too big so I think it's fine
        vector<pair<temp_record_t, size_t>> temp_record_stack = temp_index->components;

        while (!temp_record_stack.empty()) {
            pair<temp_record_t, size_t> current_record_offset = temp_record_stack.back();
            temp_record_stack.pop_back();
            record_to_offset.emplace(make_pair(temp_index_i,current_record_offset), new_records.size());

            if (current_record_offset.first == TEMP_CHAIN) {
                //Add a new chain to the index. Each of the chain's child snarls and nodes will also 
                //be added here
                const TemporaryDistanceIndex::TemporaryChainRecord& temp_chain_record = 
                        temp_index->temp_chain_records[current_record_offset.second];
                ChainRecordConstructor chain_record_constructor(new_records.size(), DISTANCED_CHAIN, 
                                                                temp_chain_record.node_count, new_records);
                chain_record_constructor.set_parent_record_pointer(
                        record_to_offset[make_pair(temp_index_i, temp_chain_record.parent)]);//TODO: Get the actual parent
                size_t chain_node_i = 0;
                bool prev_node = false;//Was the previous thing in the chain a node?
                for (const pair<temp_record_t, size_t>& child_record_offset : temp_chain_record.children) {
                    //Go through each node and snarl in the chain and add them to the index
                    if (child_record_offset.first == TEMP_NODE) {
                        //Add a node to the chain
                        if (prev_node) {
                            //If the last thing we saw was a node, then this is the end of a trivial snarl 
                            chain_record_constructor.add_trivial_snarl();
                        }
                        id_t node_id = temp_index->temp_node_records[child_record_offset.second].node_id;
                        //Fill in this node in the index 
                        NodeRecordConstructor node_record_constructor(node_id, DISTANCED_NODE, new_records);
                        //Add the node to the chain
                        chain_record_constructor.add_node(node_id, temp_chain_record.prefix_sum[chain_node_i],
                                                          temp_chain_record.forward_loops[chain_node_i],
                                                          temp_chain_record.backward_loops[chain_node_i]);

                        chain_node_i++;
                        prev_node = true;
                    } else {
                        //TODO: Ignore trivial snarls
                        //Add a snarl to the chain
                        assert(child_record_offset.first == TEMP_SNARL);
                        //Get the temporary snarl record
                        const TemporaryDistanceIndex::TemporarySnarlRecord& temp_snarl_record = 
                             temp_index->temp_snarl_records[child_record_offset.second];
                        record_to_offset.emplace(make_pair(temp_index_i, child_record_offset), new_records.size()+1);
                        //Add the snarl to the chain, and get back the record to fill it in
                        SnarlRecordConstructor snarl_record_constructor = 
                            chain_record_constructor.add_snarl(temp_snarl_record.node_count, DISTANCED_SNARL);
                        snarl_record_constructor.set_parent_record_pointer(chain_record_constructor.record_offset);
                        snarl_record_constructor.set_start_node(temp_snarl_record.start_node_id,
                                                                 temp_snarl_record.start_node_rev);
                        snarl_record_constructor.set_end_node(temp_snarl_record.end_node_id,
                                                                 temp_snarl_record.end_node_rev);
                        snarl_record_constructor.set_min_length(temp_snarl_record.min_length);
                        snarl_record_constructor.set_max_length(temp_snarl_record.max_length);
                        for (
                            temp_record_stack.emplace_back(child);


                        prev_node = false;
                    }
                }
            } else if (current_record_offset.first == TEMP_SNARL) {
                //TODO: Ignore trivial snarls
                //TODO: Actually I don't think this will ever happen, since snarls are all in chains
                //Add a new snarl to the index
                const TemporaryDistanceIndex::TemporarySnarlRecord& temp_snarl_record = 
                        temp_index->temp_snarl_records[current_record_offset.second];
                SnarlRecordConstructor snarl_record_constructor (temp_snarl_record.node_count, new_records, DISTANCED_SNARL); 
                snarl_record_constructor.set_start_node(temp_snarl_record.start_node_id,
                                                         temp_snarl_record.start_node_rev);
                snarl_record_constructor.set_end_node(temp_snarl_record.end_node_id,
                                                         temp_snarl_record.end_node_rev);
                snarl_record_constructor.set_min_length(temp_snarl_record.min_length);
                snarl_record_constructor.set_max_length(temp_snarl_record.max_length);
                snarl_record_constructor.set_parent_record_pointer(record_to_offset[make_pair(temp_index_i, temp_snarl_record.parent)]);
            } else if (current_record_offset.first == TEMP_NODE) {
                //Add a new snarl to the index
                const TemporaryDistanceIndex::TemporaryNodeRecord& temp_node_record = 
                        temp_index->temp_node_records[current_record_offset.second];
                NodeRecordConstructor node_record(temp_node_record.node_id, DISTANCED_NODE, new_records);
                node_record.set_node_length(temp_node_record.node_length);
                node_record.set_rank_in_parent(temp_node_record.rank_in_parent);
                node_record.set_is_rev_in_parent(temp_node_record.reversed_in_parent);
                node_record.set_parent_record_pointer(record_to_offset[make_pair(temp_index_i, temp_node_record.parent)]);
            }
        }
    }
    //Fill in the children
    for (size_t temp_index_i = 0 ; temp_index_i < temporary_indexes.size() ; temp_index_i++) {
        const TemporaryDistanceIndex* temp_index = temporary_indexes[temp_index_i];
        for (size_t temp_snarl_i = 0 ; temp_snarl_i < temp_index->temp_snarl_records.size() ; temp_snarl_i ++) {
            const TemporaryDistanceIndex::TemporarySnarlRecord& temp_snarl_record = temp_index->temp_snarl_records[temp_snarl_i];
            SnarlRecordConstructor snarl_record_constructor (new_records,
                    record_to_offset[make_pair(temp_index_i, make_pair(TEMP_SNARL, temp_snarl_i))]); 
            snarl_record_constructor.set_child_record_pointer(new_records.size());
            for (pair<temp_record_t, size_t> child : temp_snarl_record.children) {
                snarl_record_constructor.add_child(record_to_offset[make_pair(temp_index_i, child)]);
            }
        }
    }
    return new_records;
}
//TODO: Also need to go the other way, from final index to temporary one for merging

///////////////////////////////////////////////////////////////////////////////////////////////////
//Implement the SnarlDecomposition's functions for moving around the snarl tree
//


net_handle_t SnarlDistanceIndex::get_root() const {
    // The root is the first thing in the index, the traversal is tip to tip
    return as_net_handle(1);
}

bool SnarlDistanceIndex::is_root(const net_handle_t& net) const {
    return get_record_offset(net) == 0;
}

bool SnarlDistanceIndex::is_snarl(const net_handle_t& net) const {
    SnarlDistanceIndex::record_t type = SnarlTreeRecord(net, snarl_tree_records).get_record_type();
    return (type == SNARL || type == DISTANCED_SNARL || type == OVERSIZED_SNARL);
}

bool SnarlDistanceIndex::is_chain(const net_handle_t& net) const {
    SnarlDistanceIndex::record_t type = SnarlTreeRecord(net, snarl_tree_records).get_record_type();
    return (type == CHAIN || type == DISTANCED_CHAIN);
}

bool SnarlDistanceIndex::is_node(const net_handle_t& net) const {
    SnarlDistanceIndex::record_t type = SnarlTreeRecord(net, snarl_tree_records).get_record_type();
    return type == NODE;
}


//TODO: I'm interpreting this to mean is this handle a node that is the boundary of a snarl. 
//If its parent is a chain, then it is
bool SnarlDistanceIndex::is_sentinel(const net_handle_t& net) const {
    if (SnarlTreeRecord(net, snarl_tree_records).get_record_type() == NODE) {
        net_handle_record_t type = SnarlTreeRecord(SnarlTreeRecord(net, snarl_tree_records).get_parent_record_offset(), snarl_tree_records).get_record_handle_type();
        return type == CHAIN_HANDLE;
    } else {
        return false ;
    }
}

net_handle_t SnarlDistanceIndex::get_net(const handle_t& handle, const handlegraph::HandleGraph* graph) const{
    return get_net_handle(graph->get_id(handle), 
                          graph->get_is_reverse(handle) ? END_START : START_END, 
                          NODE_HANDLE);
}
handle_t SnarlDistanceIndex::get_handle(const net_handle_t& net, const handlegraph::HandleGraph* graph) const{
    if (get_handle_type(net) != NODE_HANDLE) {
        throw runtime_error("error: trying to get a handle from a snarl, chain, or root");
    } else {
        NodeRecord node_record(net, snarl_tree_records);
        return graph->get_handle(node_record.get_node_id(), 
                                 get_connectivity(net) == START_END ? false : true);
    }
}

net_handle_t SnarlDistanceIndex::get_parent(const net_handle_t& child) const {
    //Get the pointer to the parent, and keep the connectivity of the current handle
    size_t parent_pointer = SnarlTreeRecord(child, snarl_tree_records).get_parent_record_offset();

    connectivity_t child_connectivity = get_connectivity(child);
    //TODO: I"m going into the parent record here, which could be avoided if things knew what their parents were, but I think if you're doing this you'd later go into the parent anyway so it's probably fine
    record_t parent_type = SnarlTreeRecord(parent_pointer, snarl_tree_records).get_record_type();
    connectivity_t parent_connectivity = START_END;
    if ((child_connectivity == START_END || child_connectivity == END_START) 
        && (parent_type == CHAIN  || parent_type == DISTANCED_CHAIN)) {
        //TODO: This also needs to take into account the orientation of the child, which I might be able to get around?
        parent_connectivity = child_connectivity;
    }
    if (get_handle_type(child) == NODE_HANDLE && 
        (parent_type == ROOT || parent_type == SNARL || parent_type == DISTANCED_SNARL || 
         parent_type == SIMPLE_SNARL || parent_type == OVERSIZED_SNARL)) {
        //If this is a node and it's parent is not a chain, we want to pretend that its 
        //parent is a chain
        return get_net_handle(parent_pointer, parent_connectivity, CHAIN_HANDLE);
    }

    return get_net_handle(parent_pointer, parent_connectivity);
}

net_handle_t SnarlDistanceIndex::get_bound(const net_handle_t& snarl, bool get_end, bool face_in) const {
    id_t id = get_end ? SnarlTreeRecord(snarl, snarl_tree_records).get_end_id() : SnarlTreeRecord(snarl, snarl_tree_records).get_start_id();
    bool rev_in_parent = NodeRecord(id, snarl_tree_records).get_is_rev_in_parent();
    if (get_end) {
        rev_in_parent = !rev_in_parent;
    }
    if (!face_in){
        rev_in_parent = !rev_in_parent;
    }
    connectivity_t connectivity = rev_in_parent ? END_START : START_END;
    return get_net_handle(id, connectivity);
}

net_handle_t SnarlDistanceIndex::flip(const net_handle_t& net) const {
    connectivity_t old_connectivity = get_connectivity(net);
    connectivity_t new_connectivity;
    if (old_connectivity == START_END) {
        new_connectivity = END_START;
    } else if (old_connectivity == START_TIP) {
        new_connectivity = TIP_START;
    } else if (old_connectivity = END_START) {
        new_connectivity = START_END;
    } else if (old_connectivity = END_TIP) {
        new_connectivity = TIP_END;
    } else if (old_connectivity = TIP_START) {
        new_connectivity = START_TIP;
    } else if (old_connectivity = TIP_END) {
        new_connectivity = END_TIP;
    } else {
        new_connectivity = old_connectivity;
    }
    return get_net_handle(get_record_offset(net), new_connectivity);
}

net_handle_t SnarlDistanceIndex::canonical(const net_handle_t& net) const {
    SnarlTreeRecord record(net, snarl_tree_records);
    connectivity_t connectivity;
    if (record.is_start_end_connected()) {
        connectivity = START_END;
    } else if (record.is_start_tip_connected()) {
        connectivity = START_TIP;
    } else if (record.is_end_tip_connected()) {
        connectivity = END_TIP;
    } else if (record.is_start_start_connected()) {
        connectivity = START_START;
    } else if (record.is_end_end_connected()) {
        connectivity = END_END;
    } else if (record.is_tip_tip_connected()) {
        connectivity = TIP_TIP;
    } else {
        throw runtime_error("error: This node has no connectivity");
    }
    return get_net_handle(get_record_offset(net), connectivity);
}

SnarlDecomposition::endpoint_t SnarlDistanceIndex::starts_at(const net_handle_t& traversal) const {
    connectivity_t connectivity = get_connectivity(traversal);
    if (connectivity == START_START || connectivity == START_END || connectivity == START_TIP ){
        return START;
    } else if (connectivity == END_START || connectivity == END_END || connectivity == END_TIP ){
        return END;
    } else if (connectivity == TIP_START || connectivity == TIP_END || connectivity == TIP_TIP ){
        return TIP;
    } else {
        throw runtime_error("error: This node has no connectivity");
    }
}
SnarlDecomposition::endpoint_t SnarlDistanceIndex::ends_at(const net_handle_t& traversal) const {
    connectivity_t connectivity = get_connectivity(traversal);
    if (connectivity == START_START || connectivity == END_START || connectivity == TIP_START ){
        return START;
    } else if (connectivity == START_END || connectivity == END_END || connectivity == TIP_END ){
        return END;
    } else if (connectivity == START_TIP || connectivity == END_TIP || connectivity == TIP_TIP ){
        return TIP;
    } else {
        throw runtime_error("error: This node has no connectivity");
    }
}

//TODO: I'm also allowing this for the root
bool SnarlDistanceIndex::for_each_child_impl(const net_handle_t& traversal, const std::function<bool(const net_handle_t&)>& iteratee) const {
    //What is this according to the snarl tree
    net_handle_record_t record_type = SnarlTreeRecord(traversal, snarl_tree_records).get_record_handle_type();
    //What is this according to the handle 
    //(could be a trivial chain but actually a node according to the snarl tree)
    net_handle_record_t handle_type = get_handle_type(traversal);
    if (record_type == SNARL_HANDLE) {
        SnarlRecord snarl_record(traversal, snarl_tree_records);
        return snarl_record.for_each_child(iteratee);
    } else if (record_type == CHAIN_HANDLE) {
        ChainRecord chain_record(traversal, snarl_tree_records);
        return chain_record.for_each_child(iteratee);
    } else if (record_type == ROOT_HANDLE) {
        RootRecord root_record(traversal, snarl_tree_records);
        return root_record.for_each_child(iteratee);
    } else if (record_type == NODE_HANDLE && handle_type == CHAIN_HANDLE) {
        //This is actually a node but we're pretending it's a chain
        NodeRecord chain_as_node_record(traversal, snarl_tree_records);
        return iteratee(get_net_handle(get_record_offset(traversal), get_connectivity(traversal), NODE_HANDLE));
    } else {
        throw runtime_error("error: Looking for children of a node");
    }
   
}

bool SnarlDistanceIndex::for_each_traversal_impl(const net_handle_t& item, const std::function<bool(const net_handle_t&)>& iteratee) const {
    SnarlTreeRecord record(item, snarl_tree_records);
    for ( size_t type = 1 ; type <= 9 ; type ++ ){
        connectivity_t connectivity = static_cast<connectivity_t>(type);
        if (record.has_connectivity(connectivity)) {
            if (!iteratee(get_net_handle(get_record_offset(item), connectivity, get_handle_type(item)))) {
                return false;
            }
        }
    }
    return true;
}

bool SnarlDistanceIndex::follow_net_edges_impl(const net_handle_t& here, const handlegraph::HandleGraph* graph, bool go_left, const std::function<bool(const net_handle_t&)>& iteratee) const {

    SnarlTreeRecord this_record(here, snarl_tree_records);
    SnarlTreeRecord parent_record (this_record.get_parent_record_offset(), snarl_tree_records);

    if (parent_record.get_record_handle_type() == ROOT_HANDLE) {
        //TODO: I'm not sure what to do in this case
    }

    if (get_handle_type(here) == CHAIN_HANDLE && parent_record.get_record_handle_type() == SNARL_HANDLE) {
        //If this is a chain (or a node pretending to be a chain) and it is the child of a snarl
        //It can either run into another chain (or node) or the boundary node
        //TODO: What about if it is the root?
        net_handle_t end_handle = get_bound(here, !go_left, false);
        handle_t graph_handle = get_handle(end_handle, graph);
        graph->follow_edges(graph_handle, false, [&](const handle_t& h) {

            if (graph->get_id(h) == parent_record.get_start_id() || 
                graph->get_id(h) == parent_record.get_end_id()) {
                //If this is the boundary node of the parent snarl
                net_handle_t next_net = get_net(h, graph);
                return iteratee(next_net);
            } else{
                //It is either another chain or a node, but the node needs to pretend to be a chain
                net_handle_t node_handle = get_net(h, graph); //Netgraph of the next node
                SnarlTreeRecord next_record(node_handle, snarl_tree_records);
                net_handle_t next_net;
                if (next_record.get_parent_record_offset() == parent_record.record_offset) {
                    //If the next node's parent is also the current node's parent, then it is a node
                    //make a net_handle_t of a node pretending to be a chain
                    net_handle_t next_net = get_net_handle(next_record.record_offset, 
                                                           graph->get_is_reverse(h) ? END_START : START_END, 
                                                           CHAIN_HANDLE);
                } else {
                    //next_record is a chain
                    bool rev = graph->get_id(h) == next_record.get_start_id() ? false : true;
                    net_handle_t next_net = get_net_handle(next_record.get_parent_record_offset(), 
                                                           rev ? END_START : START_END, 
                                                           CHAIN_HANDLE);
                }
                return iteratee(next_net);
            }
            return false;
        });

        
    } else if (get_handle_type(here) == SNARL_HANDLE || get_handle_type(here) == NODE_HANDLE) {
        //If this is a snarl or node, then it is the component of a (possibly pretend) chain
        ChainRecord this_chain_record(here, snarl_tree_records);
        net_handle_t next_net = this_chain_record.get_next_child(here, go_left);
        if (next_net == here) {
            //If this is the end of the chain
            return true;
        }
        return iteratee(next_net);
        
    }
    return true;
}

net_handle_t SnarlDistanceIndex::get_parent_traversal(const net_handle_t& traversal_start, const net_handle_t& traversal_end) const {
    
    net_handle_record_t start_handle_type = get_handle_type(traversal_start);
    net_handle_record_t end_handle_type = get_handle_type(traversal_end);
    SnarlTreeRecord start_record = get_snarl_tree_record(traversal_start);
    SnarlTreeRecord end_record = get_snarl_tree_record(traversal_end);
    if (start_record.get_parent_record_offset() != end_record.get_parent_record_offset()) {
        throw runtime_error("error: Looking for parent traversal of two non-siblings");
    }
    SnarlTreeRecord parent_record (start_record.get_parent_record_offset(), snarl_tree_records);

    endpoint_t start_endpoint;
    if (start_handle_type == NODE_HANDLE && 
        get_node_id_from_offset(get_record_offset(traversal_start)) == parent_record.get_start_id() &&
        (get_start_endpoint(traversal_start) == START && !parent_record.get_start_orientation() ||
         get_start_endpoint(traversal_start) == END && parent_record.get_start_orientation()) ){
        //If traversal_start is a node and is also the start node oriented into the parent
        start_endpoint = START;

    } else if (start_handle_type == NODE_HANDLE && 
        get_node_id_from_offset(get_record_offset(traversal_start)) == parent_record.get_end_id() &&
        (get_start_endpoint(traversal_start) == START && parent_record.get_end_orientation() ||
         get_start_endpoint(traversal_start) == END && !parent_record.get_end_orientation()) ){
        //If traversal_start is a node and also the end node and oriented going into the parent
        start_endpoint = END;

    } else if (start_handle_type == NODE_HANDLE) {
        //If this is a node in the middle of a chain - boundary of a snarl
        //TODO: I"m assuming that this can't return a traversal from a boundary node along a chain to anything other than a snarl it contains
        size_t node_in_parent = start_record.get_parent_record_offset();
        bool rev_in_parent = start_record.get_is_rev_in_parent();
        ChainRecord parent_as_chain = ChainRecord(start_record.get_parent_record_offset(), snarl_tree_records);
        pair<size_t, bool> next_node = parent_as_chain.get_next_child(make_pair(node_in_parent, false), rev_in_parent);
        if (!next_node.second) {
            //If this is not pointing into a snarl
            throw runtime_error("error: Trying to get traversal of a trivial snarl");
        }
        if (parent_as_chain.get_next_child(next_node, rev_in_parent).first == end_record.get_parent_record_offset() && 
            end_record.get_is_rev_in_parent() == rev_in_parent) {
            //If these are the endpoints of a snarl
            return get_net_handle(next_node.first, rev_in_parent ? END_START : START_END, SNARL_HANDLE);
        } else {
            throw runtime_error("error: trying to get a traversal of a segment of a chain");
        }
    } else {
        start_endpoint = TIP;
    }

    endpoint_t end_endpoint;
    if (end_handle_type == NODE_HANDLE && 
        get_node_id_from_offset(get_record_offset(traversal_end)) == parent_record.get_start_id() &&
        (get_start_endpoint(traversal_end) == START && parent_record.get_start_orientation() ||
         get_start_endpoint(traversal_end) == END && !parent_record.get_start_orientation())){
        //If traversal_end is a node and also the start node oriented out of the parent
        end_endpoint = START;
    } else if (end_handle_type == NODE_HANDLE && 
        get_node_id_from_offset(get_record_offset(traversal_end)) == parent_record.get_end_id() &&
        (get_start_endpoint(traversal_end) == START && !parent_record.get_end_orientation() ||
         get_start_endpoint(traversal_end) == END && parent_record.get_end_orientation()) ){
        //If traversal_end is a node and also the end node oriented out of the parent
        end_endpoint = END;
    } else if (end_handle_type == NODE_HANDLE) {
        //If this is a node in the middle of a chain, then it can only be a snarl and we should have 
        //caught it already
        throw runtime_error("error: trying to get a traversal of a segment of a chain");
    } else {
        end_endpoint = TIP;
    }

    if (!parent_record.has_connectivity(start_endpoint, end_endpoint)) {
        throw runtime_error("error: Trying to get parent traversal that is not connected");
    }
    //TODO: I think this is true, should take the assert out later
    assert(parent_record.get_record_handle_type() == CHAIN_HANDLE);
    return get_net_handle(parent_record.record_offset, endpoints_to_connectivity(start_endpoint, end_endpoint), CHAIN_HANDLE);
}

}
