#include "vg.hpp"
#include "stream.hpp"
#include "gssw_aligner.hpp"
// We need to use ultrabubbles for dot output
#include "genotypekit.hpp"
#include "algorithms/topological_sort.hpp"
#include <raptor2/raptor2.h>
#include <stPinchGraphs.h>

#include <stack>

//#define debug

namespace vg {

using namespace std;
using namespace gfak;


// construct from a stream of protobufs
VG::VG(istream& in, bool showp, bool warn_on_duplicates) {
    from_istream(in, showp, warn_on_duplicates);
}

void VG::from_istream(istream& in, bool showp, bool warn_on_duplicates) {
    // set up uninitialized values
    init();
    show_progress = showp;
    // and if we should show progress
    function<void(uint64_t)> handle_count = [this](uint64_t count) {
        create_progress("loading graph", count);
    };

    // the graph is read in chunks, which are attached to this graph
    uint64_t i = 0;
    function<void(Graph&)> lambda = [this, &i, &warn_on_duplicates](Graph& g) {
        update_progress(++i);
        // We usually expect these to not overlap in nodes or edges, so complain unless we've been told not to.
        extend(g, warn_on_duplicates);
    };

    stream::for_each(in, lambda, handle_count);

    // Collate all the path mappings we got from all the different chunks. A
    // mapping from any chunk might fall anywhere in a path (because paths may
    // loop around cycles), so we need to sort on ranks.
    paths.sort_by_mapping_rank();
    paths.rebuild_mapping_aux();

    // store paths in graph
    paths.to_graph(graph);

    destroy_progress();
}

// construct from an arbitrary source of Graph protobuf messages
VG::VG(function<bool(Graph&)>& get_next_graph, bool showp, bool warn_on_duplicates) {
    // set up uninitialized values
    init();
    show_progress = showp;

    // We can't show loading progress since we don't know the total number of
    // subgraphs.

    // Try to load the first graph
    Graph subgraph;
    bool got_subgraph = get_next_graph(subgraph);
    while(got_subgraph) {
        // If there is a valid subgraph, add it to ourselves.
        // We usually expect these to not overlap in nodes or edges, so complain unless we've been told not to.
        extend(subgraph, warn_on_duplicates);
        // Try and load the next subgraph, if it exists.
        got_subgraph = get_next_graph(subgraph);
    }

    // store paths in graph
    paths.to_graph(graph);
}

handle_t VG::get_handle(const id_t& node_id, bool is_reverse) const {
    // Handle is ID in low bits and orientation in high bit
    
    // Where in the g vector do we need to be
    size_t handle = node_id;
    // And set the high bit if it's reverse
    if (is_reverse) handle |= HIGH_BIT;
    return as_handle(handle);
}

id_t VG::get_id(const handle_t& handle) const {
    return as_integer(handle) & LOW_BITS;
}

bool VG::get_is_reverse(const handle_t& handle) const {
    return as_integer(handle) & HIGH_BIT;
}

handle_t VG::flip(const handle_t& handle) const {
    return as_handle(as_integer(handle) ^ HIGH_BIT);
}

size_t VG::get_length(const handle_t& handle) const {
    // Don't get the real sequence because it might need a reverse complement calculation
    auto found = node_by_id.find(get_id(handle));
    if (found != node_by_id.end()) {
        // We found a node. Grab its sequence length
        return (*found).second->sequence().size();
    } else {
        throw runtime_error("No node " + to_string(get_id(handle)) + " in graph");
    }
}

string VG::get_sequence(const handle_t& handle) const {
    
    auto found = node_by_id.find(get_id(handle));
    
    if (found != node_by_id.end()) {
        // We found a node. Grab its sequence
        auto sequence = (*found).second->sequence();
        
        if (as_integer(handle) & HIGH_BIT) {
            // Needs to be reverse-complemented
            return reverse_complement(sequence);
        } else {
            return sequence;
        }
    } else {
        throw runtime_error("No node " + to_string(get_id(handle)) + " in graph");
    }
    
    
    
}

bool VG::follow_edges(const handle_t& handle, bool go_left, const function<bool(const handle_t&)>& iteratee) const {
    // Are we reverse?
    bool is_reverse = get_is_reverse(handle);
    
    // Which edges will we look at?
    auto& edge_set = (go_left != is_reverse) ? edges_on_start : edges_on_end;
    
    // Look up edges of this node specifically
    auto found = edge_set.find(get_id(handle));
    if (found != edge_set.end()) {
        // There are (or may be) edges
        for (auto& id_and_flip : found->second) {
            // For each edge destination and the flag that says if we flip orientation or not
            bool new_reverse = (is_reverse != id_and_flip.second);
            if (!iteratee(get_handle(id_and_flip.first, new_reverse))) {
                // Iteratee said to stop
                return false;
            }
        }
    }
    
    return true;
}

void VG::for_each_handle(const function<bool(const handle_t&)>& iteratee, bool parallel) const {
    if (parallel) {
#pragma omp parallel for schedule(dynamic,1)
        for (id_t i = 0; i < graph.node_size(); ++i) {
            // For each node in the backing graph
            // Get its ID and make a handle to it forward
            // And pass it to the iteratee
            if (!iteratee(get_handle(graph.node(i).id(), false))) {
                // Iteratee stopped, but we can't do anything if we want to run this in parallel
                //return;
            }
        }
    } else { // same but serial
        for (id_t i = 0; i < graph.node_size(); ++i) {
            if (!iteratee(get_handle(graph.node(i).id(), false))) {
                return;
            }
        }
    }
}

size_t VG::node_size() const {
    return graph.node_size();
}

handle_t VG::create_handle(const string& sequence) {
    Node* node = create_node(sequence);
    return get_handle(node->id(), false);
}

handle_t VG::create_handle(const string& sequence, const id_t& id) {
    Node* node = create_node(sequence, id);
    return get_handle(id, false);
}

void VG::destroy_handle(const handle_t& handle) {
    destroy_node(get_id(handle));
    // TODO: does destroy_node update paths?
}

void VG::create_edge(const handle_t& left, const handle_t& right) {
    create_edge(get_node(get_id(left)), get_node(get_id(right)),
        get_is_reverse(left), get_is_reverse(right));
}
    
void VG::destroy_edge(const handle_t& left, const handle_t& right) {
    // Convert to NodeSides and find the edge between them
    Edge* found = get_edge(NodeSide(get_id(left), !get_is_reverse(left)),
        NodeSide(get_id(right), get_is_reverse(right)));
    if (found != nullptr) {
        // If there is one, destroy it.
        destroy_edge(found);
        // TODO: does destroy_edge update paths?
    }
}

void VG::swap_handles(const handle_t& a, const handle_t& b) {
    swap_nodes(get_node(get_id(a)), get_node(get_id(b)));
}

handle_t VG::apply_orientation(const handle_t& handle) {
    if (!get_is_reverse(handle)) {
        // Nothing to do!
        return handle;
    }
    
    // Otherwise we need to reverse it

    // Grab a handle to the reverse version that exists now.
    handle_t rev_handle = flip(handle);
    
    // Find all the edges (including self loops)
    
    // We represent self loops with the (soon to be invalidated) forward and
    // reverse handles to the node we're flipping.
    vector<handle_t> left_nodes;
    vector<handle_t> right_nodes;
    
    follow_edges(handle, false, [&](const handle_t& other) -> void {
        right_nodes.push_back(other);
        return;
    });
    
    follow_edges(handle, true, [&](const handle_t& other) -> void {
        left_nodes.push_back(other);
        return;
    });
    
    // Remove them
    for (auto& left : left_nodes) {
        destroy_edge(left, handle);
    }
    for (auto& right : right_nodes) {
        destroy_edge(handle, right);
    }
    
    // Copy the sequence from the reverse view of the node to become its locally
    // forward sequence.
    string new_sequence = get_sequence(handle);
    
    // Save the ID to reuse
    id_t id = get_id(handle);
    
    // Remove the old node (without destroying the paths???)
    destroy_handle(handle);
    
    // Create a new node, re-using the ID
    Node* new_node = create_node(new_sequence, id);
    handle_t new_handle = get_handle(id, false);
    
    // Connect up the new node
    for (handle_t left : left_nodes) {
        if (left == handle) {
            // Actually go to the reverse of the new handle
            left = flip(new_handle);
        } else if (left == rev_handle) {
            // Actually go to the new handle forward
            left = new_handle;
        }
        
        create_edge(left, new_handle);
    }
    
    for (handle_t right : right_nodes) {
        if (right == handle) {
            // Actually go to the reverse of the new handle
            right = flip(new_handle);
        } else if (right == rev_handle) {
            // Actually go to the new handle forward
            right = new_handle;
        }
        
        create_edge(new_handle, right);
    }
    
    // TODO: Fix up the paths
    return new_handle;
    
}

vector<handle_t> VG::divide_handle(const handle_t& handle, const vector<size_t>& offsets) {
    Node* node = get_node(get_id(handle));
    bool reverse = get_is_reverse(handle);
    
    // We need to convert vector types
    vector<int> int_offsets;
    if (reverse) {
        // We need to fill in the vector of offsets from the end of the node.
        
        auto node_size = get_length(handle);
        
        for (auto it = offsets.rbegin(); it != offsets.rend(); ++it) {
            // Flip the order around, and also measure from the other end
            int_offsets.push_back(node_size - *it);
        }
    } else {
        // Just blit over the offsets
        int_offsets = vector<int>(offsets.begin(), offsets.end());
    }
    
    // Populate this parts vector by doing the division
    vector<Node*> parts;
    divide_node( node, int_offsets, parts);
    
    vector<handle_t> to_return;
    for (Node* n : parts) {
        // Copy the nodes into handles in their final orientation
        to_return.push_back(get_handle(n->id(), reverse));
    }
    if (reverse) {
        // And make sure they are in the right order
        std::reverse(to_return.begin(), to_return.end());
    }
    
    return to_return;
    
}

void VG::clear_paths(void) {
    paths.clear();
    graph.clear_path(); // paths.clear() should do this too
    sync_paths();
}

// synchronize the VG index and its backing store
void VG::sync_paths(void) {
    // ensure we can navigate paths correctly
    // by building paths.
    paths.rebuild_mapping_aux();
}

void VG::serialize_to_ostream(ostream& out, id_t chunk_size) {

    // This makes sure mapping ranks are updated to reflect their actual
    // positions along their paths.
    sync_paths();
    
    create_progress("saving graph", graph.node_size());
    
    // Have a function to grab the chunk for the given range of nodes
    function<Graph(uint64_t, uint64_t)> lambda = [this](uint64_t element_start, uint64_t element_length) -> Graph {
    
        VG g;
        map<string, map<size_t, mapping_t*> > sorted_paths;
        for (size_t j = element_start;
             j < element_start + element_length && j < graph.node_size();
             ++j) {
            Node* node = graph.mutable_node(j);
            // Grab the node and only the edges where it has the lower ID.
            // This prevents duplication of edges in the serialized output.
            nonoverlapping_node_context_without_paths(node, g);
            auto& mappings = paths.get_node_mapping(node);
            //cerr << "getting node mappings for " << node->id() << endl;
            for (auto m : mappings) {
                auto& name = paths.get_path_name(m.first);
                auto& mappings = m.second;
                for (auto& mapping : mappings) {
                    //cerr << "mapping " << name << pb2json(*mapping) << endl;
                    sorted_paths[name][mapping->rank] = mapping;
                }
            }
        }
        // now get the paths for this chunk so that they are ordered correctly
        for (auto& p : sorted_paths) {
            auto& name = p.first;
            auto& path = p.second;
            // now sorted in ascending order by rank
            // May not be contiguous because other chunks may contain nodes between the nodes in this one
            for (auto& m : path) {
                g.paths.append_mapping(name, m.second->to_mapping());
            }
        }

        if (element_start == 0) {
            // The first chunk will always include all the 0-length paths.
            // TODO: if there are too many, this chunk may grow too large!
            paths.for_each_name([&](const string& name) {
                // For every path
                if (paths.get_path(name).empty()) {
                    // If its mapping list has no mappings, make it in the chunk
                    g.paths.create_path(name);
                }
            });
        }

        // record our circular paths
        g.paths.circular = this->paths.circular;
        // TODO: but this is broken as our paths have been reordered as
        // the nodes they cross are stored in graph.nodes
        g.paths.to_graph(g.graph);

        update_progress(element_start);
        return g.graph;
    
    };

    // Write all the dynamically sized chunks, starting with our selected chunk
    // size as a guess.
    stream::write(out, graph.node_size(), chunk_size, lambda);

    destroy_progress();
}

void VG::serialize_to_file(const string& file_name, id_t chunk_size) {
    ofstream f(file_name);
    serialize_to_ostream(f);
    f.close();
}

VG::~VG(void) {
    //destroy_alignable_graph();
}

VG::VG(void) {
    init();
}

void VG::init(void) {
    current_id = 1;
    show_progress = false;
}

VG::VG(set<Node*>& nodes, set<Edge*>& edges) {
    init();
    add_nodes(nodes);
    add_edges(edges);
    algorithms::sort(this);
}


id_t VG::get_node_at_nucleotide(string pathname, int nuc){
    Path p = paths.path(pathname);

    int nt_start = 0;
    int nt_end = 0;
    for (int i = 0; i < p.mapping_size(); i++){
        Mapping m = p.mapping(i);
        Position pos = m.position();
        id_t n_id = pos.node_id();
        Node* node = get_node(n_id);
        nt_end += node->sequence().length();
        if (nuc < nt_end && nuc >= nt_start){
            return n_id;
        }
        nt_start += node->sequence().length();
        if (nt_start > nuc && nt_end > nuc){
            throw std::out_of_range("Nucleotide position not found in path.");
        }
    }
    
    throw std::out_of_range("Nucleotide position not found in path.");

}

void VG::add_nodes(const set<Node*>& nodes) {
    for (auto node : nodes) {
        add_node(*node);
    }
}

void VG::add_edges(const set<Edge*>& edges) {
    for (auto edge : edges) {
        add_edge(*edge);
    }
}

void VG::add_edges(const vector<Edge*>& edges) {
    for (auto edge : edges) {
        add_edge(*edge);
    }
}

void VG::add_nodes(const vector<Node>& nodes) {
    for (auto& node : nodes) {
        add_node(node);
    }
}

void VG::add_edges(const vector<Edge>& edges) {
    for (auto& edge : edges) {
        add_edge(edge);
    }
}

void VG::add_node(const Node& node) {
    if (!has_node(node)) {
        Node* new_node = graph.add_node(); // add it to the graph
        *new_node = node; // overwrite it with the value of the given node
        node_by_id[new_node->id()] = new_node; // and insert into our id lookup table
        node_index[new_node] = graph.node_size()-1;
    }
}

void VG::add_edge(const Edge& edge) {
    if (!has_edge(edge)) {
        Edge* new_edge = graph.add_edge(); // add it to the graph
        *new_edge = edge;
        set_edge(new_edge);
        edge_index[new_edge] = graph.edge_size()-1;
    }
}

void VG::circularize(id_t head, id_t tail) {
    Edge* e = create_edge(tail, head);
    add_edge(*e);
}

void VG::circularize(vector<string> pathnames){
    for(auto p : pathnames){
        Path curr_path = paths.path(p);
        Position start_pos = path_start(curr_path);
        Position end_pos = path_end(curr_path);
        id_t head = start_pos.node_id();
        id_t tail = end_pos.node_id();
        if (start_pos.offset() != 0){
            //VG::divide_node(Node* node, int pos, Node*& left, Node*& right)
            Node* left; Node* right;
            Node* head_node = get_node(head);
            divide_node(head_node, start_pos.offset(), left, right);
            head = left->id();
            paths.compact_ranks();
        }
        if (start_pos.offset() != 0){
            Node* left; Node* right;
            Node* tail_node = get_node(tail);
            divide_node(tail_node, end_pos.offset(), left, right);
            tail = right->id();
            paths.compact_ranks();
        }
        Edge* e = create_edge(tail, head, false, false);
        add_edge(*e);
        // record a flag in the path object to indicate that it is circular
        paths.make_circular(p);
    }
}

size_t VG::node_count(void) const {
    return graph.node_size();
}

size_t VG::edge_count(void) const {
    return graph.edge_size();
}

vector<pair<id_t, bool>>& VG::edges_start(Node* node) {
    if(node == nullptr) {
        return empty_edge_ends;
    }
    return edges_start(node->id());
}

vector<pair<id_t, bool>>& VG::edges_start(id_t id) {
    if(edges_on_start.count(id) == 0) {
        return empty_edge_ends;
    }
    return edges_on_start[id];
}

vector<pair<id_t, bool>>& VG::edges_end(Node* node) {
    if(node == nullptr) {
        return empty_edge_ends;
    }
    return edges_end(node->id());
}

vector<pair<id_t, bool>>& VG::edges_end(id_t id) {
    if(edges_on_end.count(id) == 0) {
        return empty_edge_ends;
    }
    return edges_on_end[id];
}

int VG::start_degree(Node* node) {
    return edges_start(node).size();
}

int VG::end_degree(Node* node) {
    return edges_end(node).size();
}

int VG::left_degree(NodeTraversal node) {
    // If we're backward, the end is on the left. Otherwise, the start is.
    return node.backward ? end_degree(node.node) : start_degree(node.node);
}

int VG::right_degree(NodeTraversal node) {
    // If we're backward, the start is on the right. Otherwise, the end is.
    return node.backward ? start_degree(node.node) : end_degree(node.node);
}

void VG::edges_of_node(Node* node, vector<Edge*>& edges) {
    for(pair<id_t, bool>& off_start : edges_start(node)) {
        // Go through the edges on this node's start
        Edge* edge = edge_by_sides[NodeSide::pair_from_start_edge(node->id(), off_start)];
        if (!edge) {
            cerr << "error:[VG::edges_of_node] nonexistent start edge " << off_start.first << " start <-> "
                 << node->id() << (off_start.second ? " start" : " end") << endl;
            exit(1);
        }
        edges.push_back(edge);
    }

    for(pair<id_t, bool>& off_end : edges_end(node)) {
        // And on its end
        Edge* edge = edge_by_sides[NodeSide::pair_from_end_edge(node->id(), off_end)];
        if (!edge) {
            cerr << "error:[VG::edges_of_node] nonexistent end edge " << off_end.first << " end <-> "
                 << node->id() << (off_end.second ? " end" : " start") << endl;
            exit(1);
        }
        if(edge->from() == edge->to() && edge->from_start() == edge->to_end()) {
            // This edge touches both our start and our end, so we already
            // handled it on our start. Don't produce it twice.
            continue;
        }
        edges.push_back(edge);
    }
}

vector<Edge*> VG::edges_from(Node* node) {
    vector<Edge*> from;
    for (auto e : edges_of(node)) {
        if (e->from() == node->id()) {
            from.push_back(e);
        }
    }
    return from;
}

vector<Edge*> VG::edges_to(Node* node) {
    vector<Edge*> to;
    for (auto e : edges_of(node)) {
        if (e->to() == node->id()) {
            to.push_back(e);
        }
    }
    return to;
}

vector<Edge*> VG::edges_of(Node* node) {
    vector<Edge*> edges;
    edges_of_node(node, edges);
    return edges;
}

void VG::edges_of_nodes(set<Node*>& nodes, set<Edge*>& edges) {
    for (set<Node*>::iterator n = nodes.begin(); n != nodes.end(); ++n) {
        vector<Edge*> ev;
        edges_of_node(*n, ev);
        for (vector<Edge*>::iterator e = ev.begin(); e != ev.end(); ++e) {
            edges.insert(*e);
        }
    }
}

set<pair<NodeSide, bool>> VG::sides_context(id_t node_id) {
    // return the side we're going to and if we go from the start or end to get there
    set<pair<NodeSide, bool>> all;
    for (auto& s : sides_to(NodeSide(node_id, false))) {
        all.insert(make_pair(s, false));
    }
    for (auto& s : sides_to(NodeSide(node_id, true))) {
        all.insert(make_pair(s, true));
    }
    for (auto& s : sides_from(NodeSide(node_id, false))) {
        all.insert(make_pair(s, false));
    }
    for (auto& s : sides_from(NodeSide(node_id, true))) {
        all.insert(make_pair(s, true));
    }
    return all;
}

bool VG::same_context(id_t n1, id_t n2) {
    auto c1 = sides_context(n1);
    auto c2 = sides_context(n2);
    bool same = true;
    for (auto& s : c1) {
        if (!c2.count(s)) { same = false; break; }
    }
    return same;
}

bool VG::is_ancestor_prev(id_t node_id, id_t candidate_id) {
    set<id_t> seen;
    return is_ancestor_prev(node_id, candidate_id, seen);
}

bool VG::is_ancestor_prev(id_t node_id, id_t candidate_id, set<id_t>& seen, size_t steps) {
    if (node_id == candidate_id) return true;
    if (!steps) return false;
    for (auto& side : sides_to(NodeSide(node_id, false))) {
        if (seen.count(side.node)) continue;
        seen.insert(side.node);
        if (is_ancestor_prev(side.node, candidate_id, seen, steps-1)) return true;
    }
    return false;
}

bool VG::is_ancestor_next(id_t node_id, id_t candidate_id) {
    set<id_t> seen;
    return is_ancestor_next(node_id, candidate_id, seen);
}

bool VG::is_ancestor_next(id_t node_id, id_t candidate_id, set<id_t>& seen, size_t steps) {
    if (node_id == candidate_id) return true;
    if (!steps) return false;
    for (auto& side : sides_from(NodeSide(node_id, true))) {
        if (seen.count(side.node)) continue;
        seen.insert(side.node);
        if (is_ancestor_next(side.node, candidate_id, seen, steps-1)) return true;
    }
    return false;
}

id_t VG::common_ancestor_prev(id_t id1, id_t id2, size_t steps) {
    // arbitrarily step back from node 1 asking if we are prev-ancestral to node 2
    auto scan = [this](id_t id1, id_t id2, size_t steps) -> id_t {
        set<id_t> to_visit;
        to_visit.insert(id1);
        for (size_t i = 0; i < steps; ++i) {
            // collect nodes to visit
            set<id_t> to_visit_next;
            for (auto& id : to_visit) {
                if (is_ancestor_prev(id2, id)) return id;
                for (auto& side : sides_to(NodeSide(id, false))) {
                    to_visit_next.insert(side.node);
                }
            }
            to_visit = to_visit_next;
            if (to_visit.empty()) return -1; // we hit the end of the graph
        }
        return 0;
    };
    id_t id3 = scan(id1, id2, steps);
    if (id3) {
        return id3;
    } else {
        return scan(id2, id1, steps);
    }
}

id_t VG::common_ancestor_next(id_t id1, id_t id2, size_t steps) {
    // arbitrarily step forward from node 1 asking if we are next-ancestral to node 2
    auto scan = [this](id_t id1, id_t id2, size_t steps) -> id_t {
        set<id_t> to_visit;
        to_visit.insert(id1);
        for (size_t i = 0; i < steps; ++i) {
            // collect nodes to visit
            set<id_t> to_visit_next;
            for (auto& id : to_visit) {
                if (is_ancestor_next(id2, id)) return id;
                for (auto& side : sides_from(NodeSide(id, true))) {
                    to_visit_next.insert(side.node);
                }
            }
            to_visit = to_visit_next;
            if (to_visit.empty()) return -1; // we hit the end of the graph
        }
        return 0;
    };
    id_t id3 = scan(id1, id2, steps);
    if (id3) {
        return id3;
    } else {
        return scan(id2, id1, steps);
    }
}

set<NodeSide> VG::sides_of(NodeSide side) {
    set<NodeSide> v1 = sides_to(side);
    set<NodeSide> v2 = sides_from(side);
    for (auto s : v2) v1.insert(s);
    return v1;
}

set<NodeSide> VG::sides_to(NodeSide side) {
    set<NodeSide> other_sides;
    vector<Edge*> edges;
    edges_of_node(get_node(side.node), edges);
    for (auto* edge : edges) {
        if (edge->to() == side.node && edge->to_end() == side.is_end) {
            other_sides.insert(NodeSide(edge->from(), !edge->from_start()));
        }
    }
    return other_sides;
}

set<NodeSide> VG::sides_from(NodeSide side) {
    set<NodeSide> other_sides;
    vector<Edge*> edges;
    edges_of_node(get_node(side.node), edges);
    for (auto* edge : edges) {
        if (edge->from() == side.node && edge->from_start() != side.is_end) {
            other_sides.insert(NodeSide(edge->to(), edge->to_end()));
        }
    }
    return other_sides;
}

set<NodeSide> VG::sides_from(id_t id) {
    set<NodeSide> sides;
    for (auto side : sides_from(NodeSide(id, true))) {
        sides.insert(side);
    }
    for (auto side : sides_from(NodeSide(id, false))) {
        sides.insert(side);
    }
    return sides;
}

set<NodeSide> VG::sides_to(id_t id) {
    set<NodeSide> sides;
    for (auto side : sides_to(NodeSide(id, true))) {
        sides.insert(side);
    }
    for (auto side : sides_to(NodeSide(id, false))) {
        sides.insert(side);
    }
    return sides;
}

set<NodeTraversal> VG::siblings_to(const NodeTraversal& trav) {
    // find the sides to
    auto to_sides = sides_to(NodeSide(trav.node->id(), trav.backward));
    // and then find the traversals from them
    set<NodeTraversal> travs_from_to_sides;
    for (auto& s1 : to_sides) {
        // and the from-children of these
        for (auto& s2 : sides_from(s1)) {
            auto sib = NodeTraversal(get_node(s2.node), s2.is_end);
            // which are not this node
            if (sib != trav) {
                travs_from_to_sides.insert(sib);
            }
        }
    }
    return travs_from_to_sides;
}

set<NodeTraversal> VG::siblings_from(const NodeTraversal& trav) {
    // find the sides from
    auto from_sides = sides_from(NodeSide(trav.node->id(), !trav.backward));
    // and then find the traversals from them
    set<NodeTraversal> travs_to_from_sides;
    for (auto& s1 : from_sides) {
        // and the to-children of these
        for (auto& s2 : sides_to(s1)) {
            auto sib = NodeTraversal(get_node(s2.node), !s2.is_end);
            // which are not this node
            if (sib != trav) {
                travs_to_from_sides.insert(sib);
            }
        }
    }
    return travs_to_from_sides;
}

set<Node*> VG::siblings_of(Node* node) {
    set<Node*> sibs;
    for (auto& s : siblings_to(NodeTraversal(node, false))) {
        sibs.insert(s.node);
    }
    for (auto& s : siblings_to(NodeTraversal(node, true))) {
        sibs.insert(s.node);
    }
    for (auto& s : siblings_from(NodeTraversal(node, false))) {
        sibs.insert(s.node);
    }
    for (auto& s : siblings_from(NodeTraversal(node, true))) {
        sibs.insert(s.node);
    }
    return sibs;
}

set<NodeTraversal> VG::full_siblings_to(const NodeTraversal& trav) {
    // get the siblings of
    auto sibs_to = siblings_to(trav);
    // and filter them for nodes with the same inbound sides
    auto to_sides = sides_to(NodeSide(trav.node->id(), trav.backward));
    set<NodeTraversal> full_sibs_to;
    for (auto& sib : sibs_to) {
        auto sib_to_sides = sides_to(NodeSide(sib.node->id(), sib.backward));
        if (sib_to_sides == to_sides) {
            full_sibs_to.insert(sib);
        }
    }
    return full_sibs_to;
}

set<NodeTraversal> VG::full_siblings_from(const NodeTraversal& trav) {
    // get the siblings of
    auto sibs_from = siblings_from(trav);
    // and filter them for nodes with the same outbound sides
    auto from_sides = sides_from(NodeSide(trav.node->id(), !trav.backward));
    set<NodeTraversal> full_sibs_from;
    for (auto& sib : sibs_from) {
        auto sib_from_sides = sides_from(NodeSide(sib.node->id(), !sib.backward));
        if (sib_from_sides == from_sides) {
            full_sibs_from.insert(sib);
        }
    }
    return full_sibs_from;
}

// returns sets of sibling nodes that are only in one set of sibling nodes
set<set<NodeTraversal>> VG::transitive_sibling_sets(const set<set<NodeTraversal>>& sibs) {
    set<set<NodeTraversal>> trans_sibs;
    map<Node*, int> membership;
    // determine the number of sibling sets that each node is in
    for (auto& s : sibs) {
        for (auto& t : s) {
            if (membership.find(t.node) == membership.end()) {
                membership[t.node] = 1;
            } else {
                ++membership[t.node];
            }
        }
    }
    // now exclude components which are intransitive
    // by only keeping those sib sets whose members are in only one set
    for (auto& s : sibs) {
        // all members must only appear in this set
        bool is_transitive = true;
        for (auto& t : s) {
            if (membership[t.node] > 1) {
                is_transitive = false;
                break;
            }
        }
        if (is_transitive) {
            trans_sibs.insert(s);
        }
    }
    return trans_sibs;
}

set<set<NodeTraversal>> VG::identically_oriented_sibling_sets(const set<set<NodeTraversal>>& sibs) {
    set<set<NodeTraversal>> iosibs;
    for (auto& s : sibs) {
        int forward = 0;
        int reverse = 0;
        for (auto& t : s) {
            if (t.backward) {
                ++reverse;
            } else {
                ++forward;
            }
        }
        // if they are all forward or all reverse
        if (forward == 0 || reverse == 0) {
            iosibs.insert(s);
        }
    }
    return iosibs;
}

void VG::simplify_siblings(void) {
    // make a list of all the sets of siblings
    set<set<NodeTraversal>> to_sibs;
    for_each_node([this, &to_sibs](Node* n) {
            auto trav = NodeTraversal(n, false);
            auto tsibs = full_siblings_to(trav);
            tsibs.insert(trav);
            if (tsibs.size() > 1) {
                to_sibs.insert(tsibs);
            }
        });
        
    // make the sibling sets transitive
    // by removing any that are intransitive
    // then simplify
    simplify_to_siblings(
        identically_oriented_sibling_sets(
            transitive_sibling_sets(to_sibs)));
    // and remove any null nodes that result
    remove_null_nodes_forwarding_edges();

    // make a list of the from-siblings
    set<set<NodeTraversal>> from_sibs;
    for_each_node([this, &from_sibs](Node* n) {
            auto trav = NodeTraversal(n, false);
            auto fsibs = full_siblings_from(trav);
            fsibs.insert(trav);
            if (fsibs.size() > 1) {
                from_sibs.insert(fsibs);
            }
        });
    // then do the from direction
    simplify_from_siblings(
        identically_oriented_sibling_sets(
            transitive_sibling_sets(from_sibs)));
    // and remove any null nodes that result
    remove_null_nodes_forwarding_edges();

}

void VG::simplify_to_siblings(const set<set<NodeTraversal>>& to_sibs) {
    for (auto& sibs : to_sibs) {
        // determine the amount of sharing at the start
        // the to-sibs have the same parent(s) feeding into them
        // so we can safely make a single node out of the shared sequence
        // and link this to them and their parent to remove node level redundancy
        vector<string*> seqs;
        size_t min_seq_size = sibs.begin()->node->sequence().size();
        for (auto& sib : sibs) {
            auto seqp = sib.node->mutable_sequence();
            seqs.push_back(seqp);
            if (seqp->size() < min_seq_size) {
                min_seq_size = seqp->size();
            }
        }
        size_t i = 0;
        size_t j = 0;
        bool similar = true;
        for ( ; similar && i < min_seq_size; ++i) {
            //cerr << i << endl;
            char c = seqs.front()->at(i);
            for (auto s : seqs) {
                //cerr << "checking " << c << " vs " << s->at(i) << endl;
                if (c != s->at(i)) {
                    similar = false;
                    break;
                }
            }
            if (!similar) break;
            ++j;
        }
        size_t shared_start = j;
        //cerr << "sharing is " << shared_start << " for to-sibs of "
        //<< sibs.begin()->node->id() << endl;
        if (shared_start == 0) continue;

        // make a new node with the shared sequence
        string seq = seqs.front()->substr(0,shared_start);
        auto new_node = create_node(seq);
        //if (!is_valid()) cerr << "invalid before sibs iteration" << endl;
        /*
        {
            VG subgraph;
            for (auto& sib : sibs) {
                nonoverlapping_node_context_without_paths(sib.node, subgraph);
            }
            expand_context(subgraph, 5);
            stringstream s;
            for (auto& sib : sibs) s << sib.node->id() << "+";
            subgraph.serialize_to_file(s.str() + "-before.vg");
        }
        */

        // remove the sequence of the new node from the old nodes
        for (auto& sib : sibs) {
            //cerr << "to sib " << pb2json(*sib.node) << endl;
            *sib.node->mutable_sequence() = sib.node->sequence().substr(shared_start);
            // for each node mapping of the sibling
            // divide the mapping at the cut point

            // and then switch the node assignment for the cut nodes
            // for each mapping of the node
            auto node_mapping = paths.get_node_mapping(sib.node);
            for (auto& p : node_mapping) {
                vector<mapping_t*> v;
                for (auto& m : p.second) {
                    v.push_back(m);
                }
                for (auto m : v) {
                    auto mpts = paths.divide_mapping(m, shared_start);
                    // and then assign the first part of the mapping to the new node
                    auto o = mpts.first;
                    auto n = mpts.second;
                    paths.reassign_node(new_node->id(), n);
                    // note that the other part now maps to the correct (old) node
                }
            }
        }

        // connect the new node to the common *context* (the union of sides of the old nodes)

        // by definition we are only working with nodes that have exactly the same set of parents
        // so we just use the first node in the set to drive the reconnection
        auto new_left_side = NodeSide(new_node->id(), false);
        auto new_right_side = NodeSide(new_node->id(), true);
        for (auto side : sides_to(NodeSide(sibs.begin()->node->id(), sibs.begin()->backward))) {
            create_edge(side, new_left_side);
        }
        // disconnect the old nodes from their common parents
        for (auto& sib : sibs) {
            auto old_side = NodeSide(sib.node->id(), sib.backward);
            for (auto side : sides_to(old_side)) {
                destroy_edge(side, old_side);
            }
            // connect the new node to the old nodes
            create_edge(new_right_side, old_side);
        }
        /*
        if (!is_valid()) { cerr << "invalid after sibs simplify" << endl;
            {
                VG subgraph;
                for (auto& sib : sibs) {
                    nonoverlapping_node_context_without_paths(sib.node, subgraph);
                }
                expand_context(subgraph, 5);
                stringstream s;
                for (auto& sib : sibs) s << sib.node->id() << "+";
                subgraph.serialize_to_file(s.str() + "-sub-after-corrupted.vg");
                serialize_to_file(s.str() + "-all-after-corrupted.vg");
                exit(1);
            }
        }
        */
    }
    // rebuild path ranks; these may have been affected in the process
    paths.compact_ranks();
}

void VG::simplify_from_siblings(const set<set<NodeTraversal>>& from_sibs) {
    for (auto& sibs : from_sibs) {
        // determine the amount of sharing at the end
        // the from-sibs have the same downstream nodes ("parents")
        // so we can safely make a single node out of the shared sequence at the end
        // and link this to them and their parent to remove node level redundancy
        vector<string*> seqs;
        size_t min_seq_size = sibs.begin()->node->sequence().size();
        for (auto& sib : sibs) {
            auto seqp = sib.node->mutable_sequence();
            seqs.push_back(seqp);
            if (seqp->size() < min_seq_size) {
                min_seq_size = seqp->size();
            }
        }
        size_t i = 0;
        size_t j = 0;
        bool similar = true;
        for ( ; similar && i < min_seq_size; ++i) {
            char c = seqs.front()->at(seqs.front()->size()-(i+1));
            for (auto s : seqs) {
                if (c != s->at(s->size()-(i+1))) {
                    similar = false;
                    break;
                }
            }
            if (!similar) break;
            ++j;
        }
        size_t shared_end = j;
        if (shared_end == 0) continue;
        // make a new node with the shared sequence
        string seq = seqs.front()->substr(seqs.front()->size()-shared_end);
        auto new_node = create_node(seq);
        // chop it off of the old nodes
        for (auto& sib : sibs) {
            *sib.node->mutable_sequence()
                = sib.node->sequence().substr(0, sib.node->sequence().size()-shared_end);

            // and then switch the node assignment for the cut nodes
            // for each mapping of the node
            auto node_mapping = paths.get_node_mapping(sib.node);
            for (auto& p : node_mapping) {
                vector<mapping_t*> v;
                for (auto& m : p.second) {
                    v.push_back(m);
                }
                for (auto m : v) {
                    auto mpts = paths.divide_mapping(m, sib.node->sequence().size());
                    // and then assign the second part of the mapping to the new node
                    auto o = mpts.first;
                    paths.reassign_node(new_node->id(), o);
                    auto n = mpts.second;
                    // note that the other part now maps to the correct (old) node
                }
            }
        }
        // connect the new node to the common downstream nodes
        // by definition we are only working with nodes that have exactly the same set of "children"
        // so we just use the first node in the set to drive the reconnection
        auto new_left_side = NodeSide(new_node->id(), false);
        auto new_right_side = NodeSide(new_node->id(), true);
        for (auto side : sides_from(NodeSide(sibs.begin()->node->id(), !sibs.begin()->backward))) {
            create_edge(new_right_side, side);
        }
        // disconnect the old nodes from their common "children"
        for (auto& sib : sibs) {
            auto old_side = NodeSide(sib.node->id(), !sib.backward);
            for (auto side : sides_from(old_side)) {
                destroy_edge(old_side, side);
            }
            // connect the new node to the old nodes
            create_edge(old_side, new_left_side);
        }
    }
    // rebuild path ranks; these may have been affected in the process
    paths.compact_ranks();
}

void VG::expand_context(VG& g, size_t distance, bool add_paths, bool use_steps) {
    // Dispatch the appropriate implementation
    if (use_steps) {
        expand_context_by_steps(g, distance, add_paths);
    } else {
        expand_context_by_length(g, distance, add_paths);
    }
}

// expand the context of the subgraph g by this many steps
// it's like a neighborhood function
void VG::expand_context_by_steps(VG& g, size_t steps, bool add_paths) {
    set<id_t> to_visit;
    // start with the nodes in the subgraph
    g.for_each_node([&](Node* n) { to_visit.insert(n->id()); });
    g.for_each_edge([&](Edge* e) {
            to_visit.insert(e->from());
            to_visit.insert(e->to()); });
    // and expand
    for (size_t i = 0; i < steps; ++i) {
        // break if we have completed the (sub)graph accessible from our starting graph
        if (to_visit.empty()) break;
        set<id_t> to_visit_next;
        for (auto id : to_visit) {
            // build out the graph
            // if we have nodes we haven't seeen
            if (!g.has_node(id)) {
                g.create_node(get_node(id)->sequence(), id);
            }
            for (auto& e : edges_of(get_node(id))) {
                bool has_from = g.has_node(e->from());
                bool has_to = g.has_node(e->to());
                if (!has_from || !has_to) {
                    g.add_edge(*e);
                    if (e->from() == id) {
                        to_visit_next.insert(e->to());
                    } else {
                        to_visit_next.insert(e->from());
                    }
                }
            }
        }
        to_visit = to_visit_next;
    }
    // then remove orphans
    g.remove_orphan_edges();
    // and add paths
    if (add_paths) {
        g.for_each_node([&](Node* n) {
                for (auto& path : paths.get_node_mapping(n)) {
                    auto& pname = paths.get_path_name(path.first);
                    for (auto& m : path.second) {
                        g.paths.append_mapping(pname, m->to_mapping());
                    }
                }
            });
        g.sync_paths();
    }
}

void VG::expand_context_by_length(VG& g, size_t length, bool add_paths, bool reflect, const set<NodeSide>& barriers) {
    
    // We have a set of newly added nodes.
    set<id_t> new_nodes;
    
    // We have an operation to take a node
    auto take_node = [&](id_t id) {
        if (!g.has_node(id)) {
            g.create_node(get_node(id)->sequence(), id);
            new_nodes.insert(id);
        }
    };
    
    // This holds how many bases of budget are remaining when about to leave
    // from this NodeSide?
    map<NodeSide, int64_t> budget_remaining;
    
    // This is the set of NodeSides we still have to look out from.
    set<NodeSide> active;
    
    // start with the nodes in the subgraph
    g.for_each_node([&](Node* n) {
        // Say every node has a budget of the whole length out from its ends.
        NodeSide left(n->id(), false);
        budget_remaining[left] = length;
        active.insert(left);
#ifdef debug
        cerr << "Start with budget " << length << " at " << left << endl;
#endif
        NodeSide right(n->id(), true);
        budget_remaining[right] = length;
        active.insert(right);
#ifdef debug
        cerr << "Start with budget " << length << " at " << right << endl;
#endif
    });
    
    while (!active.empty()) {
        // While there are still active NodeSides to extend, find one
        NodeSide here = *active.begin();
        
#ifdef debug
        cerr << "Consider " << here << endl;
#endif

        // We know this node is already in the graph, so no need to add it.
        
        if (!barriers.count(here)) {
            // We're allowed to expand out from this NodeSide
        
            // Get its budget
            auto budget = budget_remaining.at(here);
            
#ifdef debug
            cerr << "\tBudget: " << budget << endl;
#endif
            
            for (auto connected : sides_of(here)) {
                // Go through all the NodeSides we can reach from here
            
                // Add each of them to the graph if not there already
                take_node(connected.node);
                
#ifdef debug
                cerr << "\tTake node " << connected.node << " size " << get_node(connected.node)->sequence().size() << endl;
#endif

                if (reflect) {
                    // Bounce right off this NodeSide
                    if (budget > budget_remaining[connected]) {
                        // We actually would make it go further
                        budget_remaining[connected] = budget;
                        active.insert(connected);
                    
#ifdef debug
                        cerr << "\tUp budget on " << connected << " to " << budget << endl;
#endif
                    }
                }
                
                // For each one, flip it to the other side of its node
                auto flipped = connected.flip();
                
                // Deduct the length of the reached node from the budget of this NodeSide
                int64_t new_budget = budget - get_node(connected.node)->sequence().size();

                if (new_budget > 0 && new_budget > budget_remaining[flipped]) {            
                    // If it's greater than the old budget (default budget is 0)
                    
                    // Replace the old budget and activate the other NodeSide
                    budget_remaining[flipped] = new_budget;
                    active.insert(flipped);
                    
#ifdef debug
                    cerr << "\tUp budget on " << flipped << " to " << new_budget << endl;
#endif
                }
            }
            
        } else {
#ifdef debug
            cerr << "\tIt's a barrier. Stop." << endl;
#endif
        }
            
        // Deactivate the NodeSide we just did
        active.erase(here);
    }
    
    // Now take all the edges among the nodes we added. Note that we only do NEW
    // nodes! If you wanted edges between your seed nodes, you should have used
    // nonoverlapping_node_context_without_paths. But that function doesn't
    // respect barriers.
    for (id_t new_id : new_nodes) {
        // For each node, create edges involving any nodes that are
        // in the graph. TODO: this will add edges twice, but they'll be
        // deduplicated.
        
#ifdef debug
        cerr << "For new node " << new_id << endl;
#endif
        
        for (auto* edge : edges_from(get_node(new_id))) {
            // For every edge from here
            if (g.has_node(edge->to())) {
                // If it goes to a node in the graph

                // Break the edge up
                auto sides = NodeSide::pair_from_edge(edge);
                if (!barriers.count(sides.first) && !barriers.count(sides.second)) {
                    // The edge doesn't attach to any barriers, so take it
                
                    g.add_edge(*edge);
#ifdef debug
                    cerr << "\tTake from edge " << pb2json(*edge) << endl;
#endif
                } else {
#ifdef debug
                    cerr << "\tSkip from edge " << pb2json(*edge) << endl;
#endif
                }

            }
        }
        
        for (auto* edge : edges_to(get_node(new_id))) {
            // For every edge to here
            if (g.has_node(edge->from())) {
                // If it goes from a node in the graph
                
                // Break the edge up
                auto sides = NodeSide::pair_from_edge(edge);
                if (!barriers.count(sides.first) && !barriers.count(sides.second)) {
                    // The edge doesn't attach to any barriers, so take it
                
                    g.add_edge(*edge);
#ifdef debug
                    cerr << "\tTake to edge " << pb2json(*edge) << endl;
#endif
                } else {
#ifdef debug
                    cerr << "\tTake skip edge " << pb2json(*edge) << endl;
#endif
                }
            }
        }
        
    }
    
    // then remove orphans
    g.remove_orphan_edges();
    
    // and add paths
    // TODO: deduplicate this code with the node count based version
    if (add_paths) {
        g.for_each_node([&](Node* n) {
                for (auto& path : paths.get_node_mapping(n)) {
                    auto& pname = paths.get_path_name(path.first);
                    for (auto& m : path.second) {
                        g.paths.append_mapping(pname, m->to_mapping());
                    }
                }
            });
        g.sync_paths();
    }
    
#ifdef dubug
    cerr << pb2json(g.graph) << endl;
#endif
}

bool VG::adjacent(const Position& pos1, const Position& pos2) {
    // two positions are on the same node
    if (pos1.node_id() == pos2.node_id()) {
        if (pos1.offset() == pos1.offset()+1) {
            // and have adjacent offsets
            return true;
        } else {
            // if not, they aren't adjacent
            return false;
        }
    } else {
        // is the first at the end of its node
        // and the second at the start of its node
        // determine if the two nodes are connected
        auto* node1 = get_node(pos1.node_id());
        auto* node2 = get_node(pos2.node_id());
        if (pos1.offset() == node1->sequence().size()-1
            && pos2.offset() == 0) {
            // these are adjacent iff we have an edge
            return has_edge(NodeSide(pos1.node_id(), true),
                            NodeSide(pos2.node_id(), false));
        } else {
            // the offsets aren't at the end and start
            // so these positions can't be adjacent
            return false;
        }
    }
}

// edges which are both from_start and to_end can be represented naturally as
// a regular edge, from end to start, so we flip these as part of normalization
void VG::flip_doubly_reversed_edges(void) {
    for_each_edge([this](Edge* e) {
            if (e->from_start() && e->to_end()) {
                unindex_edge_by_node_sides(e);
                e->set_from_start(false);
                e->set_to_end(false);
                id_t f = e->to();
                id_t t = e->from();
                e->set_to(t);
                e->set_from(f);
                index_edge_by_node_sides(e);
            }
        });
}

// by definition, we can merge nodes that are a "simple component"
// without affecting the sequence or path space of the graph
// so we don't unchop nodes when they have mismatched path sets
void VG::unchop(void) {
    for (auto& comp : simple_multinode_components()) {
        concat_nodes(comp);
    }
    // rebuild path ranks, as these will be affected by mapping merging
    paths.compact_ranks();
}

void VG::normalize(int max_iter, bool debug) {
    size_t last_len = 0;
    if (max_iter > 1) {
        last_len = length();
    }
    int iter = 0;
    do {
        // convert edges that go from_start -> to_end to the equivalent "regular" edge
        flip_doubly_reversed_edges();
        //if (!is_valid()) cerr << "invalid after doubly flip" << endl;
        // combine diced/chopped nodes (subpaths with no branching)
        unchop();
        //if (!is_valid()) cerr << "invalid after unchop" << endl;
        // merge redundancy across multiple nodes into single nodes (requires flip_doubly_reversed_edges)
        simplify_siblings();
        //if (!is_valid()) cerr << "invalid after simplify sibs" << endl;
        // compact node ranks
        paths.compact_ranks();
        //if (!is_valid()) cerr << "invalid after compact ranks" << endl;
        // there may now be some cut nodes that can be simplified
        unchop();
        //if (!is_valid()) cerr << "invalid after unchop two" << endl;
        // compact node ranks (again)
        paths.compact_ranks();
        //if (!is_valid()) cerr << "invalid after compact ranks two  " << endl;
        if (max_iter > 1) {
            size_t curr_len = length();
            if (debug) cerr << "[VG::normalize] iteration " << iter+1 << " current length " << curr_len << endl;
            if (curr_len == last_len) break;
            last_len = curr_len;
        }
    } while (++iter < max_iter);
    if (max_iter > 1) {
        if (debug) cerr << "[VG::normalize] normalized in " << iter << " steps" << endl;
    }
}

set<Edge*> VG::get_path_edges(void) {
    // We'll populate a set with edges.
    // This set shadows our function anme but we're not recursive so that's fine.
    set<Edge*> edges;

    function<void(const Path&)> lambda = [this, &edges](const Path& path) {
        for (size_t i = 1; i < path.mapping_size(); ++i) {
            auto& m1 = path.mapping(i-1);
            auto& m2 = path.mapping(i);
            if (!adjacent_mappings(m1, m2)) continue; // the path is completely represented here
            auto s1 = NodeSide(m1.position().node_id(), (m1.position().is_reverse() ? false : true));
            auto s2 = NodeSide(m2.position().node_id(), (m2.position().is_reverse() ? true : false));
            // check that we always have an edge between the two nodes in the correct direction
            if (has_edge(s1, s2)) {
                Edge* edge = get_edge(s1, s2);
                edges.insert(edge);
            }
        }
        // if circular, include the cycle-closing edge
        if (path.is_circular()) {
            auto& m1 = path.mapping(path.mapping_size()-1);
            auto& m2 = path.mapping(0);
            //if (!adjacent_mappings(m1, m2)) continue; // the path is completely represented here
            auto s1 = NodeSide(m1.position().node_id(), (m1.position().is_reverse() ? false : true));
            auto s2 = NodeSide(m2.position().node_id(), (m2.position().is_reverse() ? true : false));
            // check that we always have an edge between the two nodes in the correct direction
            assert(has_edge(s1, s2));
            Edge* edge = get_edge(s1, s2);
            edges.insert(edge);

        }
    };
    paths.for_each(lambda);
    return edges;
}

void VG::remove_non_path(void) {
    
    // Determine which edges are used
    set<Edge*> path_edges(get_path_edges());
    
    // now determine which edges aren't used
    set<Edge*> non_path_edges;
    for_each_edge([this, &path_edges, &non_path_edges](Edge* e) {
            if (!path_edges.count(e)) {
                non_path_edges.insert(e);
            }
        });
    // and destroy them
    for (auto* e : non_path_edges) {
        destroy_edge(e);
    }

    set<id_t> non_path_nodes;
    for_each_node([this, &non_path_nodes](Node* n) {
            if (!paths.has_node_mapping(n->id())) {
                non_path_nodes.insert(n->id());
            }
        });
    for (auto id : non_path_nodes) {
        destroy_node(id);
    }
}

void VG::remove_path(void) {
    
    // Determine which edges are used
    set<Edge*> path_edges(get_path_edges());
    
    // and destroy them
    for (auto* e : path_edges) {
        destroy_edge(e);
    }

    set<id_t> path_nodes;
    for_each_node([this, &path_nodes](Node* n) {
            if (paths.has_node_mapping(n->id())) {
                path_nodes.insert(n->id());
            }
        });
    for (auto id : path_nodes) {
        destroy_node(id);
    }
}

set<list<NodeTraversal>> VG::simple_multinode_components(void) {
    return simple_components(2);
}

// true if the mapping completely covers the node it maps to and is a perfect match
bool VG::mapping_is_total_match(const Mapping& m) {
    return mapping_is_simple_match(m)
        && mapping_from_length(m) == get_node(m.position().node_id())->sequence().size();
}

bool VG::nodes_are_perfect_path_neighbors(NodeTraversal left, NodeTraversal right) {
    // it is not possible for the nodes to be perfect neighbors if
    // they do not have exactly the same counts of paths
    if (paths.of_node(left.node->id()) != paths.of_node(right.node->id())) return false;
    // now we know that the paths are identical in count and name between the two nodes

    // get the mappings for each node
    auto m1 = paths.get_node_mapping_by_path_name(left.node->id());
    auto m2 = paths.get_node_mapping_by_path_name(right.node->id());

    // verify that they are all perfect matches that take up their entire nodes
    /*
    for (auto& p : m1) {
        for (auto* m : p.second) {
            if (!mapping_is_total_match(*m)) return false;
        }
    }
    for (auto& p : m2) {
        for (auto* m : p.second) {
            if (!mapping_is_total_match(*m)) return false;
        }
    }
    */

    // It is still possible that we have the same path annotations, but the
    // components of the paths we have are not contiguous across these nodes. To
    // verify, we check that each mapping on the left node is adjacent to one on
    // the right node in the correct relative order and orientation.

    // order the mappings by rank so we can quickly check if everything is adjacent
    // Holds mappings by path name, then rank.
    map<string, map<int, mapping_t*>> r1, r2;
    for (auto& p : m1) {
        auto& name = p.first;
        auto& mp1 = p.second;
        auto& mp2 = m2[name];
        for (auto* m : mp1) r1[name][m->rank] = m;
        for (auto* m : mp2) r2[name][m->rank] = m;
    }
    // verify adjacency
    for (auto& p : r1) {
        // For every path name and collection of mappings by rank on the left node...
        auto& name = p.first;
        auto& ranked1 = p.second;
        map<int, mapping_t*>& ranked2 = r2[name];
        for (auto& r : ranked1) {
            // For every rank and mapping on the left node...
            auto rank = r.first;
            auto& m = *r.second;
            
            // A forward mapping on a forward traversal, or a reverse mapping on
            // a reverse traversal, means we need the mapping with rank 1
            // greater on the right node. Mismatching combinations means we need
            // the mapping with rank 1 less.
            
            // Look for the mapping on the right node
            auto f = ranked2.find(rank + ((m.is_reverse() == left.backward) ? 1 : -1));
            if (f == ranked2.end()) return false;
            
            // If the mapping went with the traversal on the left, we expect it
            // to go with the traversal on the right. And if it didn't, we
            // expect it not to.
            if ((m.is_reverse() == left.backward) != (f->second->is_reverse() == right.backward)) {
                return false;
            }
            ranked2.erase(f); // remove so we can verify that we have fully matched
        }
    }
    // verify that we fully matched the second node
    for (auto& p : r2) {
        if (!p.second.empty()) return false;
    }

    // we've passed all checks, so we have a node pair with mergable paths
    return true;
}

// the set of components that could be merged into single nodes without
// changing the path space of the graph
// respects stored paths
set<list<NodeTraversal>> VG::simple_components(int min_size) {

    // go around and establish groupings
    set<Node*> seen;
    set<list<NodeTraversal>> components;
    for_each_node([this, min_size, &components, &seen](Node* n) {
            if (seen.count(n)) return;
            
#ifdef debug
            cerr << "Component based on " << n->id() << endl;
#endif
            
            seen.insert(n);
            // go left and right through each as far as we have only single edges connecting us
            // to nodes that have only single edges coming in or out
            // that go to other nodes
            list<NodeTraversal> c;
            // go left
            {
                NodeTraversal l(n, false);
                vector<NodeTraversal> prev = nodes_prev(l);
#ifdef debug
                cerr << "\tLeft: ";
                for (auto& x : prev) {
                    cerr << x << "(" << node_count_next(x) << " edges right) ";
                }
                cerr << endl;
#endif
                while (prev.size() == 1
                       && node_count_next(prev.front()) == 1) {   
                       
                    // While there's only one node left of here, and one node right of that node...
                    auto last = l;
                    // Move over left to that node
                    l = prev.front();
                    // avoid merging if it breaks stored paths
                    if (!nodes_are_perfect_path_neighbors(l, last)) {
#ifdef debug
                        cerr << "\tNot perfect neighbors!" << endl;
#endif
                        break;
                    }
                    // avoid merging if it's already in this or any other component (catch self loops)
                    if (seen.count(l.node)) {
#ifdef debug
                        cerr << "\tAlready seen!" << endl;
#endif
                        break;
                    }
                    prev = nodes_prev(l);
#ifdef debug
                    cerr << "\tLeft: ";
                    for (auto& x : prev) {
                        cerr << x << "(" << node_count_next(x) << " edges right) ";
                    }
                    cerr << endl;
#endif
                    c.push_front(l);
                    seen.insert(l.node);
                }
            }
            // add the node (in the middle)
            c.push_back(NodeTraversal(n, false));
            // go right
            {
                NodeTraversal r(n, false);
                vector<NodeTraversal> next = nodes_next(r);
#ifdef debug
                cerr << "\tRight: ";
                for (auto& x : next) {
                    cerr << x << "(" << node_count_prev(x) << " edges left) ";
                }
                cerr << endl;
#endif
                while (next.size() == 1
                       && node_count_prev(next.front()) == 1) {   
                       
                    // While there's only one node right of here, and one node left of that node...
                    auto last = r;
                    // Move over right to that node
                    r = next.front();
                    // avoid merging if it breaks stored paths
                    if (!nodes_are_perfect_path_neighbors(last, r)) {
#ifdef debug
                        cerr << "\tNot perfect neighbors!" << endl;
#endif
                        break;
                    }
                    // avoid merging if it's already in this or any other component (catch self loops)
                    if (seen.count(r.node)) {
#ifdef debug
                        cerr << "\tAlready seen!" << endl;
#endif
                        break;
                    }
                    next = nodes_next(r);
#ifdef debug
                    cerr << "\tRight: ";
                    for (auto& x : next) {
                        cerr << x << "(" << node_count_prev(x) << " edges left) ";
                    }
                    cerr << endl;
#endif
                    c.push_back(r);
                    seen.insert(r.node);
                }
            }
            if (c.size() >= min_size) {
                components.insert(c);
            }
        });
#ifdef debug
    cerr << "components " << endl;
    for (auto& c : components) {
        for (auto x : c) {
            cerr << x << " ";
        }
        cerr << endl;
    }
#endif
    return components;
}

map<string, vector<mapping_t>>
    VG::concat_mappings_for_nodes(const list<NodeTraversal>& nodes) {

    // We know all the nodes are perfect path neighbors.
    
    // Get the total length of all the nodes
    size_t total_length = 0;
    for (auto& traversal : nodes) {
        total_length += traversal.node->sequence().size();
    }
    
    // Make sure we actually have nodes
    assert(total_length > 0);
    
    // We'll fill this in with a vectors of mappings, one mapping for each visit
    // of the path to the run of nodes.
    map<string, vector<mapping_t>> new_mappings;
    
    // Copy all the mappings for this first node, in a map by path name and then
    // by rank
    auto first_node_mappings = paths.get_node_mapping_copies_by_rank(nodes.front().node->id());
    
    for (auto& name_and_ranked_mappings : first_node_mappings) {
        // For every path
        auto& name = name_and_ranked_mappings.first;
        for (auto& rank_and_mapping : name_and_ranked_mappings.second) {
            // For every mapping on that path
            auto& mapping = rank_and_mapping.second;
            
            // Copy it as the representative for the whole run. Preserves the rank.
            new_mappings[name].push_back(mapping);
            
            if (nodes.front().backward) {
                // Invert the orientation of the mapping if it was to a node
                // that was backward relative to the run.
                new_mappings[name].back().set_is_reverse(!new_mappings[name].back().is_reverse());
            }
            
            // Clobber the edits and replace with a new full-length perfect
            // match. We know all the mappings to these nodes in the run were
            // also full-length perfect matches.
            new_mappings[name].back().length = total_length;
            
            // Caller is responsible for fixing the node ID.
        }
    }
    
    // We know all the other nodes will look like the first node, modulo
    // orientations. So we don't have to look at them.
    
    return new_mappings;
}

Node* VG::concat_nodes(const list<NodeTraversal>& nodes) {

    // Make sure we have at least 2 nodes
    assert(!nodes.empty() && nodes.front() != nodes.back());

    // We also require no edges enter or leave the run of nodes, but we can't check that now.

    // make the new mappings for the node. Doesn't insert them in the paths, but
    // makes sure they have the right ranks.
    map<string, vector<mapping_t> > new_mappings = concat_mappings_for_nodes(nodes);

    // make a new node that concatenates the labels in the order and orientation specified
    string seq;
    for (auto n : nodes) {
        seq += n.backward ? reverse_complement(n.node->sequence()) : n.node->sequence();
    }
    Node* node = create_node(seq);

    // remove the old mappings
    for (auto n : nodes) {
        set<mapping_t*> to_remove;
        for (auto p : paths.get_node_mapping(n.node)) {
            for (auto* m : p.second) {
                to_remove.insert(m);
            }
        }
        for (auto m : to_remove) {
            paths.remove_mapping(m);
        }
    }

    // change the position of the new mappings to point to the new node
    // and store them in the path
    for (map<string, vector<mapping_t> >::iterator nm = new_mappings.begin(); nm != new_mappings.end(); ++nm) {
        // For each path and vector of mappings for this node on this path
        vector<mapping_t>& ms = nm->second;
        for (vector<mapping_t>::iterator m = ms.begin(); m != ms.end(); ++m) {
            // For each new mapping
            // Attach it to the new node
            m->set_node_id(node->id());
            
            // Stick it in the path at the end. Later the mappings will be
            // sorted by rank and ranks recalculated to close the gaps.
            paths.append_mapping(nm->first, *m);
        }
    }

    // connect this node to the left and right connections of the set

    for (auto prev : nodes_prev(nodes.front())) {
        // For each traversal left of our first treaversal
        
        if (prev.node == nodes.back().node) {
            // This is going to become a duplicating self loop.
            
            // Convert to point to the new node in the correct orientation.
            prev.node = node;
            // if the node at the end was concatenated into the new node
            // forward, we keep the orientation. Otherwise we flip.
            prev.backward = prev.backward != nodes.back().backward;
            
            // It has to be in the correct orientation for a duplicating self
            // loop. The above complicated xor should always be false if our
            // caller followed the preconditions.
            assert(!prev.backward);
            
            create_edge(prev, NodeTraversal(node, false));
        } else if (prev.node == nodes.front().node) {
            // This is going to become a reversing self loop.
            
            // Convert to point to the new node in the correct orientation.
            prev.node = node;
            // if the node at the start was concatenated into the new node
            // forward, we keep the orientation. Otherwise we flip.
            prev.backward = prev.backward != nodes.front().backward;
            
            // It has to be in the correct orientation for a reversing self
            // loop. The above complicated xor should always be true if our
            // caller followed the preconditions.
            assert(prev.backward);
            
            create_edge(prev, NodeTraversal(node, false));
        } else {
            // Assume it's some other node not merged into this one at all.
            create_edge(prev, NodeTraversal(node, false));
        }
        
    }
    
    for (auto next : nodes_next(nodes.back())) {
    
        if (next.node == nodes.back().node) {
            // This is going to become a reversing self loop.
            
            // Convert to point to the new node in the correct orientation.
            next.node = node;
            // if the node at the end was concatenated into the new node
            // forward, we keep the orientation. Otherwise we flip.
            next.backward = next.backward != nodes.back().backward;
            
            // It has to be in the correct orientation for a reversing self
            // loop. The above complicated xor should always be true if our
            // caller followed the preconditions.
            assert(next.backward);
            
            create_edge(NodeTraversal(node, false), next);
        } else if (next.node == nodes.front().node) {
            // We already handled this duplicating self loop from the other end!
            continue;
        } else {
            // Assume it's some other node not merged into this one at all.
            create_edge(NodeTraversal(node, false), next);
        }
    
    }

    // remove the old nodes
    for (auto n : nodes) {
        destroy_node(n.node);
    }

    return node;
}

Node* VG::merge_nodes(const list<Node*>& nodes) {
    // make the new node (use the first one in the list)
    assert(!nodes.empty());
    Node* n = nodes.front();
    id_t nid = n->id();
    // create edges to the node
    for (auto& m : nodes) {
        if (m != n) { // skip first, which we're using
            //set<NodeSide> sides_of(NodeSide side);
            id_t id = m->id();
            for (auto& s : sides_to(NodeSide(id, false))) {
                create_edge(s, NodeSide(nid, false));
            }
            for (auto& s : sides_to(NodeSide(id, true))) {
                create_edge(s, NodeSide(nid, true));
            }
            for (auto& s : sides_from(NodeSide(id, false))) {
                create_edge(NodeSide(nid, false), s);
            }
            for (auto& s : sides_from(NodeSide(id, true))) {
                create_edge(NodeSide(nid, true), s);
            }
        }
    }
    // reassign mappings in paths to the new node
    hash_map<id_t, id_t> id_mapping;
    for (auto& m : nodes) {
        if (m != n) {
            id_mapping[m->id()] = nid;
        }
    }
    paths.swap_node_ids(id_mapping);
    // and erase the old nodes
    for (auto& m : nodes) {
        if (m != n) {
            destroy_node(m);
        }
    }
    // return the node we merged into
    return n;
}

id_t VG::total_length_of_nodes(void) {
    id_t length = 0;
    for (id_t i = 0; i < graph.node_size(); ++i) {
        Node* n = graph.mutable_node(i);
        length += n->sequence().size();
    }
    return length;
}
    
void VG::build_node_indexes_no_init_size(void) {
    for (id_t i = 0; i < graph.node_size(); ++i) {
        Node* n = graph.mutable_node(i);
        node_index[n] = i;
        node_by_id[n->id()] = n;
    }
}

void VG::build_node_indexes(void) {
#ifdef USE_DENSE_HASH
    node_by_id.resize(graph.node_size());
    node_index.resize(graph.node_size());
#endif
    build_node_indexes_no_init_size();
}

void VG::build_edge_indexes_no_init_size(void) {
    for (id_t i = 0; i < graph.edge_size(); ++i) {
        Edge* e = graph.mutable_edge(i);
        edge_index[e] = i;
        index_edge_by_node_sides(e);
    }
}

void VG::build_edge_indexes(void) {
#ifdef USE_DENSE_HASH
    edges_on_start.resize(graph.node_size());
    edges_on_end.resize(graph.node_size());
    edge_by_sides.resize(graph.edge_size());
#endif
    build_edge_indexes_no_init_size();
}
    
void VG::build_indexes(void) {
    build_node_indexes();
    build_edge_indexes();
}
    
void VG::build_indexes_no_init_size(void) {
    build_node_indexes_no_init_size();
    build_edge_indexes_no_init_size();
}

void VG::clear_node_indexes(void) {
    node_index.clear();
    node_by_id.clear();
}

void VG::clear_node_indexes_no_resize(void) {
#ifdef USE_DENSE_HASH
    node_index.clear_no_resize();
    node_by_id.clear_no_resize();
#else
    clear_node_indexes();
#endif
}

void VG::clear_edge_indexes(void) {
    edge_by_sides.clear();
    edge_index.clear();
    edges_on_start.clear();
    edges_on_end.clear();
}

void VG::clear_edge_indexes_no_resize(void) {
#ifdef USE_DENSE_HASH
    edge_by_sides.clear_no_resize();
    edge_index.clear_no_resize();
    edges_on_start.clear_no_resize();
    edges_on_end.clear_no_resize();
#else
    clear_edge_indexes();
#endif
}

void VG::clear_indexes(void) {
    clear_node_indexes();
    clear_edge_indexes();
}

void VG::clear_indexes_no_resize(void) {
#ifdef USE_DENSE_HASH
    clear_node_indexes_no_resize();
    clear_edge_indexes_no_resize();
#else
    clear_indexes();
#endif
}

void VG::resize_indexes(void) {
    node_index.resize(graph.node_size());
    node_by_id.resize(graph.node_size());
    edge_by_sides.resize(graph.edge_size());
    edge_index.resize(graph.edge_size());
    edges_on_start.resize(graph.node_size());
    edges_on_end.resize(graph.node_size());
}

void VG::rebuild_indexes(void) {
    clear_indexes_no_resize();
    build_indexes_no_init_size();
    paths.rebuild_node_mapping();
}

void VG::rebuild_edge_indexes(void) {
    clear_edge_indexes_no_resize();
    build_edge_indexes_no_init_size();
}

bool VG::empty(void) {
    return graph.node_size() == 0 && graph.edge_size() == 0;
}

bool VG::has_node(Node* node) {
    return node && has_node(node->id());
}

bool VG::has_node(const Node& node) {
    return has_node(node.id());
}

bool VG::has_node(id_t id) {
    return node_by_id.find(id) != node_by_id.end();
}

Node* VG::find_node_by_name_or_add_new(string name) {
//TODO we need to have real names on id's;
  int namespace_end = name.find_last_of("/#");

	string id_s = name.substr(namespace_end+1, name.length()-2);
	id_t id = stoll(id_s);

	if (has_node(id)){
	   return get_node(id);
	} else {
		Node* new_node = graph.add_node();
		new_node->set_id(id);
        node_by_id[new_node->id()] = new_node;
        node_index[new_node] = graph.node_size()-1;
		return new_node;
	}
}

bool VG::has_edge(Edge* edge) {
    return edge && has_edge(*edge);
}

bool VG::has_edge(const Edge& edge) {
    return edge_by_sides.find(NodeSide::pair_from_edge(edge)) != edge_by_sides.end();
}

bool VG::has_edge(const NodeSide& side1, const NodeSide& side2) {
    return edge_by_sides.find(minmax(side1, side2)) != edge_by_sides.end();
}

bool VG::has_edge(const pair<NodeSide, NodeSide>& sides) {
    return has_edge(sides.first, sides.second);
}

bool VG::has_inverting_edge(Node* n) {
    for (auto e : edges_of(n)) {
        if ((e->from_start() || e->to_end())
            && !(e->from_start() && e->to_end())) {
            return true;
        }
    }
    return false;
}

bool VG::has_inverting_edge_from(Node* n) {
    for (auto e : edges_of(n)) {
        if (e->from() == n->id()
            && (e->from_start() || e->to_end())
            && !(e->from_start() && e->to_end())) {
            return true;
        }
    }
    return false;
}

bool VG::has_inverting_edge_to(Node* n) {
    for (auto e : edges_of(n)) {
        if (e->to() == n->id()
            && (e->from_start() || e->to_end())
            && !(e->from_start() && e->to_end())) {
            return true;
        }
    }
    return false;
}

// remove duplicated nodes and edges that would occur if we merged the graphs
void VG::remove_duplicated_in(VG& g) {
    vector<Node*> nodes_to_destroy;
    for (id_t i = 0; i < graph.node_size(); ++i) {
        Node* n = graph.mutable_node(i);
        if (g.has_node(n)) {
            nodes_to_destroy.push_back(n);
        }
    }
    vector<Edge*> edges_to_destroy;
    for (id_t i = 0; i < graph.edge_size(); ++i) {
        Edge* e = graph.mutable_edge(i);
        if (g.has_edge(e)) {
            edges_to_destroy.push_back(e);
        }
    }
    for (vector<Node*>::iterator n = nodes_to_destroy.begin();
         n != nodes_to_destroy.end(); ++n) {
        g.destroy_node(g.get_node((*n)->id()));
    }
    for (vector<Edge*>::iterator e = edges_to_destroy.begin();
         e != edges_to_destroy.end(); ++e) {
        // Find and destroy the edge that does the same thing in g.
        destroy_edge(g.get_edge(NodeSide::pair_from_edge(*e)));
    }
}

void VG::remove_duplicates(void) {
    map<id_t, size_t> node_counts;
    for (size_t i = 0; i < graph.node_size(); ++i) {
        Node* n = graph.mutable_node(i);
        node_counts[n->id()]++;
    }
    vector<Node*> nodes_to_destroy;
    for (size_t i = 0; i < graph.node_size(); ++i) {
        Node* n = graph.mutable_node(i);
        auto f = node_counts.find(n->id());
        if (f != node_counts.end()
            && f->second > 1) {
            --f->second;
            nodes_to_destroy.push_back(n);
        }
    }
    for (vector<Node*>::iterator n = nodes_to_destroy.begin();
         n != nodes_to_destroy.end(); ++n) {
        destroy_node(get_node((*n)->id()));
    }

    map<pair<NodeSide, NodeSide>, size_t> edge_counts;
    for (id_t i = 0; i < graph.edge_size(); ++i) {
        edge_counts[NodeSide::pair_from_edge(graph.edge(i))]++;
    }
    vector<Edge*> edges_to_destroy;
    for (id_t i = 0; i < graph.edge_size(); ++i) {
        Edge* e = graph.mutable_edge(i);
        auto f = edge_counts.find(NodeSide::pair_from_edge(*e));
        if (f != edge_counts.end()
            && f->second > 1) {
            --f->second;
            edges_to_destroy.push_back(e);
        }
    }
    for (vector<Edge*>::iterator e = edges_to_destroy.begin();
         e != edges_to_destroy.end(); ++e) {
        // Find and destroy the edge that does the same thing in g.
        destroy_edge(get_edge(NodeSide::pair_from_edge(*e)));
    }
}

void VG::merge_union(VG& g) {
    // remove duplicates, then merge
    remove_duplicated_in(g);
    if (g.graph.node_size() > 0) {
        merge(g.graph);
    }
}

void VG::merge(VG& g) {
    merge(g.graph);
}

// this merges without any validity checks
// this could be rather expensive if the graphs to merge are largely overlapping
void VG::merge(Graph& g) {
    graph.mutable_node()->MergeFrom(g.node());
    graph.mutable_edge()->MergeFrom(g.edge());
    rebuild_indexes();
}

// iterates over nodes and edges, adding them in when they don't already exist
void VG::extend(VG& g, bool warn_on_duplicates) {
    for (id_t i = 0; i < g.graph.node_size(); ++i) {
        Node* n = g.graph.mutable_node(i);
        if(n->id() == 0) {
            cerr << "[vg] warning: node ID 0 is not allowed. Skipping." << endl;
        } else if (!has_node(n)) {
            add_node(*n);
        } else if(warn_on_duplicates) {
            cerr << "[vg] warning: node ID " << n->id() << " appears multiple times. Skipping." << endl;
        }
    }
    for (id_t i = 0; i < g.graph.edge_size(); ++i) {
        Edge* e = g.graph.mutable_edge(i);
        if (!has_edge(e)) {
            add_edge(*e);
        } else if(warn_on_duplicates) {
            cerr << "[vg] warning: edge " << e->from() << (e->from_start() ? " start" : " end") << " <-> "
                 << e->to() << (e->to_end() ? " end" : " start") << " appears multiple times. Skipping." << endl;
        }
    }
    // Append the path mappings from this graph, and sort based on rank.
    paths.append(g.paths);
}

// TODO: unify with above. The only difference is what's done with the paths.
void VG::extend(Graph& graph, bool warn_on_duplicates) {
    for (id_t i = 0; i < graph.node_size(); ++i) {
        Node* n = graph.mutable_node(i);
        if(n->id() == 0) {
            cerr << "[vg] warning: node ID 0 is not allowed. Skipping." << endl;
        } else if (!has_node(n)) {
            add_node(*n);
        } else if(warn_on_duplicates) {
            cerr << "[vg] warning: node ID " << n->id() << " appears multiple times. Skipping." << endl;
        }
    }
    for (id_t i = 0; i < graph.edge_size(); ++i) {
        Edge* e = graph.mutable_edge(i);
        if (!has_edge(e)) {
            add_edge(*e);
        } else if(warn_on_duplicates) {
            cerr << "[vg] warning: edge " << e->from() << (e->from_start() ? " start" : " end") << " <-> "
                 << e->to() << (e->to_end() ? " end" : " start") << " appears multiple times. Skipping." << endl;
        }
    }
    // Append the path mappings from this graph, but don't sort by rank
    paths.append(graph);
}

// extend this graph by g, connecting the tails of this graph to the heads of the other
// the ids of the second graph are modified for compact representation
void VG::append(VG& g) {

    // compact and increment the ids of g out of range of this graph
    //g.compact_ids();

    // assume we've already compacted the other, or that id compaction doesn't matter
    // just get out of the way
    g.increment_node_ids(max_node_id());

    // get the heads of the other graph, now that we've compacted the ids
    vector<Node*> heads = g.head_nodes();
    // The heads are guaranteed to be forward-oriented.
    vector<id_t> heads_ids;
    for (Node* n : heads) {
        heads_ids.push_back(n->id());
    }

    // get the current tails of this graph
    vector<Node*> tails = tail_nodes();
    // The tails are also guaranteed to be forward-oriented.
    vector<id_t> tails_ids;
    for (Node* n : tails) {
        tails_ids.push_back(n->id());
    }

    // add in the other graph
    // note that we don't use merge_union because we are ensured non-overlapping ids
    merge(g);

    /*
    cerr << "this graph size " << node_count() << " nodes " << edge_count() << " edges" << endl;
    cerr << "in append with " << heads.size() << " heads and " << tails.size() << " tails" << endl;
    */

    // now join the tails to heads
    for (id_t& tail : tails_ids) {
        for (id_t& head : heads_ids) {
            // Connect the tail to the head with a left to right edge.
            create_edge(tail, head);
        }
    }

    // wipe the ranks of the mappings, as these are destroyed in append
    // NB: append assumes that we are concatenating paths
    paths.clear_mapping_ranks();
    g.paths.clear_mapping_ranks();

    // and join paths that are embedded in the graph, where path names are the same
    paths.append(g.paths);
}

void VG::combine(VG& g) {
    // compact and increment the ids of g out of range of this graph
    //g.compact_ids();
    g.increment_node_ids(max_node_id());
    // now add it into the current graph, without connecting any nodes
    extend(g);
}

void VG::include(const Path& path) {
    for (size_t i = 0; i < path.mapping_size(); ++i) {
        if (!mapping_is_simple_match(path.mapping(i))) {
            cerr << "mapping " << pb2json(path.mapping(i)) << " cannot be included in the graph because it is not a simple match" << endl;
            //exit(1);
        }
    }
    paths.extend(path);
}

id_t VG::max_node_id(void) {
    id_t max_id = 0;
    for (int i = 0; i < graph.node_size(); ++i) {
        Node* n = graph.mutable_node(i);
        if (n->id() > max_id) {
            max_id = n->id();
        }
    }
    return max_id;
}

id_t VG::min_node_id(void) {
    id_t min_id = max_node_id();
    for (int i = 0; i < graph.node_size(); ++i) {
        Node* n = graph.mutable_node(i);
        if (n->id() < min_id) {
            min_id = n->id();
        }
    }
    return min_id;
}

void VG::compact_ids(void) {
    hash_map<id_t, id_t> new_id;
    id_t id = 1; // start at 1
    for_each_node([&id, &new_id](Node* n) {
            new_id[n->id()] = id++; });
//#pragma omp parallel for
    for_each_node([&new_id](Node* n) {
            n->set_id(new_id[n->id()]); });
//#pragma omp parallel for
    for_each_edge([&new_id](Edge* e) {
            e->set_from(new_id[e->from()]);
            e->set_to(new_id[e->to()]); });
    paths.swap_node_ids(new_id);
    rebuild_indexes();
}

void VG::increment_node_ids(id_t increment) {
    for_each_node_parallel([increment](Node* n) {
            n->set_id(n->id()+increment);
        });
    for_each_edge_parallel([increment](Edge* e) {
            e->set_from(e->from()+increment);
            e->set_to(e->to()+increment);
        });
    rebuild_indexes();
    paths.increment_node_ids(increment);
}

void VG::decrement_node_ids(id_t decrement) {
    increment_node_ids(-decrement);
}

void VG::swap_node_id(id_t node_id, id_t new_id) {
    swap_node_id(node_by_id[node_id], new_id);
}

void VG::swap_node_id(Node* node, id_t new_id) {

    int edge_n = edge_count();
    id_t old_id = node->id();
    node->set_id(new_id);
    node_by_id.erase(old_id);

    // we check if the old node exists, and bail out if we're not doing what we expect
    assert(node_by_id.find(new_id) == node_by_id.end());

    // otherwise move to a new id
    node_by_id[new_id] = node;

    // These are sets, so if we try to destroy and recreate the same edge from
    // both ends (i.e. if they both go to this node) we will only do it once.
    set<pair<NodeSide, NodeSide>> edges_to_destroy;
    set<pair<NodeSide, NodeSide>> edges_to_create;

    // Define a function that we will run on every edge this node is involved in
    auto fix_edge = [&](Edge* edge) {

        // Destroy that edge
        edges_to_destroy.emplace(NodeSide(edge->from(), !edge->from_start()), NodeSide(edge->to(), edge->to_end()));

        // Make a new edge with our new ID as from or to (or both), depending on which it was before.
        // TODO: Is there a cleaner way to do this?
        if(edge->from() == old_id) {
            if(edge->to() == old_id) {
                edges_to_create.emplace(NodeSide(new_id, !edge->from_start()), NodeSide(new_id, edge->to_end()));
            } else {
                edges_to_create.emplace(NodeSide(new_id, !edge->from_start()), NodeSide(edge->to(), edge->to_end()));
            }
        } else {
            edges_to_create.emplace(NodeSide(edge->from(), !edge->from_start()), NodeSide(new_id, edge->to_end()));
        }

    };

    for(pair<id_t, bool>& other : edges_start(old_id)) {
        // Get the actual Edge
        // We're at a start, so we go to the end of the other node normally, and the start if the other node is backward
        Edge* edge = edge_by_sides[minmax(NodeSide(old_id, false), NodeSide(other.first, !other.second))];

        // Plan to fix up its IDs.
        fix_edge(edge);
    }

    for(pair<id_t, bool>& other : edges_end(old_id)) {
        // Get the actual Edge
        // We're at an end, so we go to the start of the other node normally, and the end if the other node is backward
        Edge* edge = edge_by_sides[minmax(NodeSide(old_id, true), NodeSide(other.first, other.second))];

        // Plan to fix up its IDs.
        fix_edge(edge);
    }

    assert(edges_to_destroy.size() == edges_to_create.size());

    for (auto& e : edges_to_destroy) {
        // Destroy the edge (only one can exist between any two nodes)
        destroy_edge(e.first, e.second);
    }

    for (auto& e : edges_to_create) {
        // Make an edge with the appropriate start and end flags
        create_edge(e.first, e.second);
    }

    assert(edge_n == edge_count());

    // we maintain a valid graph
    // this an expensive check but should work (for testing only)
    //assert(is_valid());

}

map<id_t, vcflib::Variant> VG::get_node_id_to_variant(vcflib::VariantCallFile vfile){
    map<id_t, vcflib::Variant> ret;
    vcflib::Variant var;

    while(vfile.getNextVariant(var)){
        long nuc = var.position;
        id_t node_id = get_node_at_nucleotide(var.sequenceName, nuc);
        ret[node_id] = var;
    }

    return ret;
}

void VG::dice_nodes(int max_node_size) {
    // We're going to chop up everything, so clear out the path ranks.
    paths.clear_mapping_ranks();

    if (max_node_size) {
        vector<Node*> nodes; nodes.reserve(size());
        for_each_node(
            [this, &nodes](Node* n) {
                nodes.push_back(n);
            });
        auto lambda =
            [this, max_node_size](Node* n) {
            int node_size = n->sequence().size();
            if (node_size > max_node_size) {
                int div = 2;
                while (node_size/div > max_node_size) {
                    ++div;
                }
                int segment_size = node_size/div;

                // Make up all the positions to divide at
                vector<int> divisions;
                int last_division = 0;
                while(last_division + segment_size < node_size) {
                    // We can fit another division point
                    last_division += segment_size;
                    divisions.push_back(last_division);
                }

                // What segments are we making?
                vector<Node*> segments;

                // Do the actual division
                divide_node(n, divisions, segments);
            }
        };
        for (int i = 0; i < nodes.size(); ++i) {
            lambda(nodes[i]);
        }
    }

    // Set the ranks again
    paths.rebuild_mapping_aux();
    paths.compact_ranks();
}

void VG::from_gfa(istream& in, bool showp) {
    // c++... split...
    // for line in stdin
    string line;
    auto too_many_fields = [&line]() {
        cerr << "[vg] error: too many fields in line " << endl << line << endl;
        exit(1);
    };

    bool reduce_overlaps = false;
    GFAKluge gg;
    gg.parse_gfa_file(in);

    map<string, sequence_elem, custom_key> name_to_seq = gg.get_name_to_seq();
    map<std::string, vector<edge_elem> > seq_to_edges = gg.get_seq_to_edges();
    map<string, sequence_elem>::iterator it;
    id_t curr_id = 1;
    map<string, id_t> id_names;
    std::function<id_t(const string&)> get_add_id = [&](const string& name) -> id_t {
        if (is_number(name)) {
            return std::stol(name);
        } else {
            auto id = id_names.find(name);
            if (id == id_names.end()) {
                id_names[name] = curr_id;
                return curr_id++;
            } else {
                return id->second;
            }
        }
    };
    for (it = name_to_seq.begin(); it != name_to_seq.end(); it++){
        auto source_id = get_add_id((it->second).name);
        //Make us some nodes
        Node n;
        n.set_sequence((it->second).sequence);
        n.set_id(source_id);
        n.set_name((it->second).name);
        add_node(n);
        // Now some edges. Since they're placed in this map
        // by their from_node, it's no big deal to just iterate
        // over them.
        for (edge_elem l : seq_to_edges[(it->second).name]){
            auto sink_id = get_add_id(l.sink_name);
            Edge e;
            e.set_from(source_id);
            e.set_to(sink_id);
            e.set_from_start(!l.source_orientation_forward);
            e.set_to_end(!l.sink_orientation_forward);
            // get the cigar
            auto cigar_elems = vcflib::splitCigar(l.alignment);
            if (cigar_elems.size() == 1
                && cigar_elems.front().first > 0
                && cigar_elems.front().second == "M") {
                    reduce_overlaps = true;
                    e.set_overlap(cigar_elems.front().first);
            }
            add_edge(e);
        }
        // for (path_elem p: seq_to_paths[(it->second).name]){
        //     paths.append_mapping(p.name, source_id, p.rank ,p.is_reverse);
        // }
        // remove overlapping sequences from the graph
    }
    map<string, path_elem> n_to_p = gg.get_name_to_path();
    for (auto name_path : n_to_p){
        for (int np = 0; np < name_path.second.segment_names.size(); np++){
            paths.append_mapping(name_path.first, stol(name_path.second.segment_names[np]), np + 1, !name_path.second.orientations[np]);
        }
    }
    if (reduce_overlaps) {
        bluntify();
    }
}

string VG::trav_sequence(const NodeTraversal& trav) {
    string seq = trav.node->sequence();
    if (trav.backward) {
        return reverse_complement(seq);
    } else {
        return seq;
    }
}

void VG::bluntify(void) {
    // we bluntify the graph by converting it from an overlap graph,
    // which is supported by the data format through the edge's overlap field,
    // into a blunt-end string graph which algorithms in VG assume

    // We manage this by turning nodes with overlaps into threads in a pinch
    // graph, and pinching together on the overlaps.
    
    // TODO: this does not preserve existing paths. If your assembler spits out
    // paths you want to preserve, you'll need to write code to do that here.
    paths.clear();
    
    // First we have to validate the overlaps claimed by the edges.

    // We populate this with edges with incorrect overlaps that we want to delete.
    set<Edge*> bad_edges;
    // Run in parallel as this can be very expensive
    for_each_edge_parallel([&](Edge* edge) {
        if (edge->overlap() > 0) {
#ifdef debug
            cerr << "claimed overlap " << edge->overlap() << endl;
#endif
            // derive and check the overlap seqs
            auto from_seq = trav_sequence(NodeTraversal(get_node(edge->from()), edge->from_start()));
            auto to_seq = trav_sequence(NodeTraversal(get_node(edge->to()), edge->to_end()));

            if (edge->overlap() > from_seq.size()) edge->set_overlap(from_seq.size());
            if (edge->overlap() > to_seq.size()) edge->set_overlap(to_seq.size());
        }
        });
    
    for (auto* edge : bad_edges) {
        // Drop all the edges with incorrect overlaps
        destroy_edge(edge);
    }
    
    // Create a pinch graph.
    // Note that in pinch graphs, orientation 1 = forward, and 0 = reverse
    auto* pinch_graph = stPinchThreadSet_construct();
    
    // Have a function to add a node to the pinch graph if it's not there
    // already, or get it if it is.
    auto obtain_thread = [&](Node* node) {
        // IF it's there, grab it
        auto* found = stPinchThreadSet_getThread(pinch_graph, node->id());
        if (found == nullptr) {
            // If not, make it
            found = stPinchThreadSet_addThread(pinch_graph, node->id(), 0, node->sequence().size());
        }
        return found;
    }; 
    
    // We fill this with the nodes we're going to replace.
    set<Node*> overlapped_nodes;
    
    // And this with the overlap edges which we remove from the graph (to be
    // replaced by pinching parts of nodes).
    vector<Edge*> overlap_edges; 
    
    for_each_edge([&](Edge* edge) {
        // Now loop over only the real edges
        if (edge->overlap() > 0) {
            // and for each one with overlap
            
            // Make it a pair of NodeSides
            NodeSide left;
            NodeSide right;
            tie(left, right) = NodeSide::pair_from_edge(*edge);
            
            // Grab the nodes
            Node* left_node = get_node(left.node);
            Node* right_node = get_node(right.node);
            
            // Mark them overlapped
            overlapped_nodes.insert(left_node);
            overlapped_nodes.insert(right_node);
        
            // Grab both the nodes' threads from the pinch graph
            auto* left_thread = obtain_thread(left_node);
            auto* right_thread = obtain_thread(right_node);
            // And the nodes' lengths
            size_t left_length = left_node->sequence().size();
            size_t right_length = right_node->sequence().size();
            
            // What should be the first base merged on the left node? If we're
            // on the start of the node, we start at 0, but if we're on the end
            // of the node, we start however far in we need to be to allow for
            // the overlap.
            size_t left_start = left.is_end ? (left_length - edge->overlap()) : 0;
            // And on the right node?
            size_t right_start = right.is_end ? (right_length - edge->overlap()) : 0;
            
            // Make the actual pinch. We have to do opposite strands if we're
            // pinching the same ends of nodes together. Otherwise the remaining
            // pieces of the node will wind up on the same side of the resulting
            // merged block. Note that we need to send true for a forward
            // relative orientation.
            stPinchThread_pinch(left_thread, right_thread, left_start, right_start, edge->overlap(), !(left.is_end == right.is_end));
#ifdef debug
            cerr << "pinched " << left_node->id() << ":" << left_start << " and "
                << right_node->id() << ":" << right_start << " for "
                << edge->overlap() << " bp in pinch orientation "
                << !(left.is_end == right.is_end) << endl;
#endif

            // Remember to remove the edge from the graph
            overlap_edges.push_back(edge);
            
        }
    });
    
    // Remove all the overlap edges from the graph, since we represent them with
    // stuff coming back from the pinch graph.
    for (auto* edge : overlap_edges) {
        destroy_edge(edge);
    }
    
    // Clean up any trivial boundaries that might be in the pinch graph.
    stPinchThreadSet_joinTrivialBoundaries(pinch_graph);
    
    // This maps from pinch block to new node created for it.
    map<stPinchBlock*, Node*> new_block_nodes;
    // Some segments aren't in blocks but still need nodes.
    map<stPinchSegment*, Node*> new_segment_nodes;
    
    // We have a function that will get the node for a segment's block or for
    // the segment, and the relative orientation of the node to the segment
    // (true for reverse). It will create the node if it's not found.
    auto obtain_node = [&](stPinchSegment* segment) {
        auto* block = stPinchSegment_getBlock(segment);
        
        if (block == nullptr) {
            // No block, just do node by segment
            if (!new_segment_nodes.count(segment)) {
                // Need to make the node. So grab the sequence.
                Node* source_node = get_node(stPinchSegment_getName(segment));
                string segment_sequence = source_node->sequence().substr(stPinchSegment_getStart(segment), stPinchSegment_getLength(segment));
                new_segment_nodes[segment] = create_node(segment_sequence);
            }
            // Send it back saying it's definitely in the right orientation.
            return make_pair(new_segment_nodes[segment], false);
        } else {
            // The segment is in a block, so we look for the block's node.
            if (!new_block_nodes.count(block)) {
                // Need to make the node. So grab the sequence.
                Node* source_node = get_node(stPinchSegment_getName(segment));
                string segment_sequence = source_node->sequence().substr(stPinchSegment_getStart(segment), stPinchSegment_getLength(segment));
                if (!stPinchSegment_getBlockOrientation(segment)) {
                    // This block is really the reverse complement of this segment
                    segment_sequence = reverse_complement(segment_sequence);
                }
                new_block_nodes[block] = create_node(segment_sequence);
            }
            
            // Send the node back, along with its VG-style orientation relative to the segment.
            return make_pair(new_block_nodes[block], !stPinchSegment_getBlockOrientation(segment));
        }
    };
    
    // Now we go through the pinch segments
    auto iterator = stPinchThreadSet_getSegmentIt(pinch_graph);
    auto* segment = stPinchThreadSetSegmentIt_getNext(&iterator);
    
    while (segment != nullptr) {
        // For each segment, we have to have a new node. Find it and its
        // orientation relative to the segment (true = opposite orientations).
        Node* segment_node;
        bool segment_is_reverse;
        tie(segment_node, segment_is_reverse) = obtain_node(segment);
        
        // What node did we break up?
        Node* old_node = get_node(stPinchSegment_getName(segment));
        
#ifdef debug
        cerr << "Handle segment " << segment << " of node " << old_node->id() << " as new node " << segment_node->id() << endl;
#endif
        
        // We look at all the connected blocks, and if they have associated nodes, we create edges to them
        
        auto* prev_segment = stPinchSegment_get5Prime(segment);
        if (prev_segment != nullptr) {
            // There's more of this original node left of here
            Node* prev_node;
            bool prev_is_reverse;
            tie(prev_node, prev_is_reverse) = obtain_node(prev_segment);
            
            // Make the edge (which may exist already)
            create_edge(prev_node, segment_node, prev_is_reverse, segment_is_reverse);

#ifdef debug
            cerr << "\tAttach to prev segment " << prev_segment << " as new node " << prev_node->id() << endl;
#endif
            
        } else {
            // We're the start of our original node
            NodeSide original_start(old_node->id(), false);
            for (auto& attached : sides_of(original_start)) {
                // For everything attached to the start of the original node, attach it to the left of here
                create_edge(attached, NodeSide(segment_node->id(), segment_is_reverse));
                
#ifdef debug
                cerr << "\tAttach to old side " << attached << " on the left" << endl;
#endif
            }
        }
        
        auto* next_segment = stPinchSegment_get3Prime(segment);
        if (next_segment != nullptr) {
            // There's more of our original node right of here
            Node* next_node;
            bool next_is_reverse;
            tie(next_node, next_is_reverse) = obtain_node(next_segment);
            
            // Make the edge (which may exist already)
            create_edge(segment_node, next_node, segment_is_reverse, next_is_reverse);
            
#ifdef debug
            cerr << "\tAttach to next segment " << next_segment << " as new node " << next_node->id() << endl;
#endif
            
        } else {
            // We're the end of our original node
            NodeSide original_end(old_node->id(), true);
            for (auto& attached : sides_of(original_end)) {
                // For everything attached to the end of the original node, attach it to the right of here
                create_edge(attached, NodeSide(segment_node->id(), !segment_is_reverse));
                
#ifdef debug
                cerr << "\tAttach to old side " << attached << " on the right" << endl;
#endif
            }
        }
        
        segment = stPinchThreadSetSegmentIt_getNext(&iterator);
    }
    
    // Now clean up the pinch thread set. This invalidates all the pointers we got from it.
    stPinchThreadSet_destruct(pinch_graph);
    
    for (auto* node : overlapped_nodes) {
        // Then finally we go through all the nodes that had overlaps and remove them and their edges.
        destroy_node(node);
    }
    
}

static
void
triple_to_vg(void* user_data, raptor_statement* triple)
{
    VG* vg = ((std::pair<VG*, Paths*>*) user_data)->first;
    Paths* paths = ((std::pair<VG*, Paths*>*) user_data)->second;
    const string vg_ns ="<http://example.org/vg/";
    const string vg_node_p = vg_ns + "node>" ;
    const string vg_rank_p = vg_ns + "rank>" ;
    const string vg_reverse_of_node_p = vg_ns + "reverseOfNode>" ;
    const string vg_path_p = vg_ns + "path>" ;
    const string vg_linkrr_p = vg_ns + "linksReverseToReverse>";
    const string vg_linkrf_p = vg_ns + "linksReverseToForward>";
    const string vg_linkfr_p = vg_ns + "linksForwardToReverse>";
    const string vg_linkff_p = vg_ns + "linksForwardToForward>";
    const string sub(reinterpret_cast<char*>(raptor_term_to_string(triple->subject)));
    const string pred(reinterpret_cast<char*>(raptor_term_to_string(triple->predicate)));
    const string obj(reinterpret_cast<char*>(raptor_term_to_string(triple->object)));

    bool reverse = pred == vg_reverse_of_node_p;
    if (pred == (vg_node_p) || reverse) {
        Node* node = vg->find_node_by_name_or_add_new(obj);
        Mapping* mapping = new Mapping(); //TODO will this cause a memory leak
        const string pathname = sub.substr(1, sub.find_last_of("/#"));

        //TODO we are using a nasty trick here, which needs to be fixed.
	    //We are using knowledge about the uri format to determine the rank of the step.
        try {
	        int rank = stoi(sub.substr(sub.find_last_of("-")+1, sub.length()-2));
	        mapping->set_rank(rank);
	    } catch(exception& e) {
	        cerr << "[vg view] assumption about rdf structure was wrong, parsing failed" << endl;
            exit(1);
	    }
        Position* p = mapping->mutable_position();
        p->set_offset(0);
        p->set_node_id(node->id());
	    p->set_is_reverse(reverse);
        paths->append_mapping(pathname, *mapping);
    } else if (pred=="<http://www.w3.org/1999/02/22-rdf-syntax-ns#value>"){
        Node* node = vg->find_node_by_name_or_add_new(sub);
        node->set_sequence(obj.substr(1,obj.length()-2));
    } else if (pred == vg_linkrr_p){
        Node* from = vg->find_node_by_name_or_add_new(sub);
        Node* to = vg->find_node_by_name_or_add_new(obj);
        vg->create_edge(from, to, true, true);
    } else if (pred == vg_linkrf_p){
        Node* from = vg->find_node_by_name_or_add_new(sub);
        Node* to = vg->find_node_by_name_or_add_new(obj);
        vg->create_edge(from, to, false, true);
    } else if (pred == vg_linkfr_p){
        Node* from = vg->find_node_by_name_or_add_new(sub);
        Node* to = vg->find_node_by_name_or_add_new(obj);
        vg->create_edge(from, to, true, false);
    } else if (pred == vg_linkff_p){
        Node* from = vg->find_node_by_name_or_add_new(sub);
        Node* to = vg->find_node_by_name_or_add_new(obj);
        vg->create_edge(from, to, false, false);
    }
}

void VG::from_turtle(string filename, string baseuri, bool showp) {
    raptor_world* world;
    world = raptor_new_world();
    if(!world)
    {
        cerr << "[vg view] we could not start the rdf environment needed for parsing" << endl;
        exit(1);
    }
    int st =  raptor_world_open (world);

    if (st!=0) {
	cerr << "[vg view] we could not start the rdf parser " << endl;
	exit(1);
    }
    raptor_parser* rdf_parser;
    const unsigned char *filename_uri_string;
    raptor_uri  *uri_base, *uri_file;
    rdf_parser = raptor_new_parser(world, "turtle");
    //We use a paths object with its convience methods to build up path objects.
    Paths* paths = new Paths();
    std::pair<VG*, Paths*> user_data = make_pair(this, paths);

    //The user_data is cast in the triple_to_vg method.
    raptor_parser_set_statement_handler(rdf_parser, &user_data, triple_to_vg);


    const  char *file_name_string = reinterpret_cast<const char*>(filename.c_str());
    filename_uri_string = raptor_uri_filename_to_uri_string(file_name_string);
    uri_file = raptor_new_uri(world, filename_uri_string);
    uri_base = raptor_new_uri(world, reinterpret_cast<const unsigned char*>(baseuri.c_str()));

    // parse the file indicated by the uri, given an uir_base .
    raptor_parser_parse_file(rdf_parser, uri_file, uri_base);
    // free the different C allocated structures
    raptor_free_uri(uri_base);
    raptor_free_uri(uri_file);
    raptor_free_parser(rdf_parser);
    raptor_free_world(world);
    //sort the mappings in the path
    paths->sort_by_mapping_rank();
    //we need to make sure that we don't have inner mappings
    //we need to do this after collecting all node sequences
    //that can only be ensured by doing this when parsing ended
    paths->for_each_mapping([this](mapping_t& mapping){
        Node* node =this->get_node(mapping.node_id());
        //every mapping in VG RDF matches a whole mapping
        mapping.length = node->sequence().length();
    });
    ///Add the paths that we parsed into the vg object
    paths->for_each([this](const Path& path){
        this->include(path);
    });

}

void VG::print_edges(void) {
    for (int i = 0; i < graph.edge_size(); ++i) {
        Edge* e = graph.mutable_edge(i);
        id_t f = e->from();
        id_t t = e->to();
        cerr << f << "->" << t << " ";
    }
    cerr << endl;
}

// depth first search across node traversals with interface to traversal tree via callback
void VG::dfs(
    const function<void(NodeTraversal)>& node_begin_fn, // called when node orientation is first encountered
    const function<void(NodeTraversal)>& node_end_fn,   // called when node orientation goes out of scope
    const function<bool(void)>& break_fn,       // called to check if we should stop the DFS
    const function<void(Edge*)>& edge_fn,       // called when an edge is encountered
    const function<void(Edge*)>& tree_fn,       // called when an edge forms part of the DFS spanning tree
    const function<void(Edge*)>& edge_curr_fn,  // called when we meet an edge in the current tree component
    const function<void(Edge*)>& edge_cross_fn, // called when we meet an edge in an already-traversed tree component
    const vector<NodeTraversal>* sources,       // start only at these node traversals
    const unordered_set<NodeTraversal>* sinks             // when hitting a sink, don't keep walking
    ) {

    // to maintain search state
    enum SearchState { PRE = 0, CURR, POST };
    unordered_map<NodeTraversal, SearchState> state; // implicitly constructed entries will be PRE.

    // to maintain stack frames
    struct Frame {
        NodeTraversal trav;
        vector<Edge*>::iterator begin, end;
        Frame(NodeTraversal t,
              vector<Edge*>::iterator b,
              vector<Edge*>::iterator e)
            : trav(t), begin(b), end(e) { }
    };

    // maintains edges while the node traversal's frame is on the stack
    unordered_map<NodeTraversal, vector<Edge*> > edges;

    // do dfs from given root.  returns true if terminated via break condition, false otherwise
    function<bool(NodeTraversal&)> dfs_single_source = [&](NodeTraversal& root) {
                
        // to store the stack frames
        deque<Frame> todo;
        if (state[root] == SearchState::PRE) {
            state[root] = SearchState::CURR;
                
            // Collect all the edges attached to the outgoing side of the
            // traversal.
            auto& es = edges[root];
            for(auto& next : travs_from(root)) {
                // Every NodeTraversal following on from this one has an
                // edge we take to get to it.
                Edge* edge = get_edge(root, next);
                assert(edge != nullptr);
                es.push_back(edge);
            }
                
            todo.push_back(Frame(root, es.begin(), es.end()));
            // run our discovery-time callback
            node_begin_fn(root);
            // and check if we should break
            if (break_fn()) {
                return true;
            }
        }
        // now begin the search rooted at this NodeTraversal
        while (!todo.empty()) {
            // get the frame
            auto& frame = todo.back();
            todo.pop_back();
            // and set up reference to it
            auto trav = frame.trav;
            auto edges_begin = frame.begin;
            auto edges_end = frame.end;
            // run through the edges to handle
            while (edges_begin != edges_end) {
                auto edge = *edges_begin;
                // run the edge callback
                edge_fn(edge);
                    
                // what's the traversal we'd get to following this edge
                NodeTraversal target;
                if(edge->from() == trav.node->id() && edge->to() != trav.node->id()) {
                    // We want the to side
                    target.node = get_node(edge->to());
                } else if(edge->to() == trav.node->id() && edge->from() != trav.node->id()) {
                    // We want the from side
                    target.node = get_node(edge->from());
                } else {
                    // It's a self loop, because we have to be on at least
                    // one end of the edge.
                    target.node = trav.node;
                }
                // When we follow this edge, do we reverse traversal orientation?
                bool is_reversing = (edge->from_start() != edge->to_end());
                target.backward = trav.backward != is_reversing;
                    
                auto search_state = state[target];
                // if we've not seen it, follow it
                if (search_state == SearchState::PRE) {
                    tree_fn(edge);
                    // save the rest of the search for this NodeTraversal on the stack
                    todo.push_back(Frame(trav, ++edges_begin, edges_end));
                    // switch our focus to the NodeTraversal at the other end of the edge
                    trav = target;
                    // and store it on the stack
                    state[trav] = SearchState::CURR;
                    auto& es = edges[trav];

                    // only walk out of traversals that are not the sink
                    if (sinks == NULL || sinks->count(trav) == false) {
                        for(auto& next : travs_from(trav)) {
                            // Every NodeTraversal following on from this one has an
                            // edge we take to get to it.
                            Edge* edge = get_edge(trav, next);
                            assert(edge != nullptr);
                            es.push_back(edge);
                        }
                    }
                    
                    edges_begin = es.begin();
                    edges_end = es.end();
                    // run our discovery-time callback
                    node_begin_fn(trav);
                } else if (search_state == SearchState::CURR) {
                    // if it's on the stack
                    edge_curr_fn(edge);
                    ++edges_begin;
                } else {
                    // it's already been handled, so in another part of the tree
                    edge_cross_fn(edge);
                    ++edges_begin;
                }
            }
            state[trav] = SearchState::POST;
            node_end_fn(trav);
            edges.erase(trav); // clean up edge cache
        }

        return false;
    };

    if (sources == NULL) {
        // attempt the search rooted at all NodeTraversals
        for (id_t i = 0; i < graph.node_size(); ++i) {
            Node* root_node = graph.mutable_node(i);
        
            for(int orientation = 0; orientation < 2; orientation++) {
                // Try both orientations
                NodeTraversal root(root_node, (bool)orientation);
                dfs_single_source(root);
            }
        }
    } else {
        for (auto source : *sources) {
            dfs_single_source(source);
        }
    }
}

void VG::dfs(const function<void(NodeTraversal)>& node_begin_fn,
             const function<void(NodeTraversal)>& node_end_fn,
             const vector<NodeTraversal>* sources,
             const unordered_set<NodeTraversal>* sinks) {
    auto edge_noop = [](Edge* e) { };
    dfs(node_begin_fn,
        node_end_fn,
        [](void) { return false; },
        edge_noop,
        edge_noop,
        edge_noop,
        edge_noop,
        sources,
        sinks);
}

void VG::dfs(const function<void(NodeTraversal)>& node_begin_fn,
             const function<void(NodeTraversal)>& node_end_fn,
             const function<bool(void)>& break_fn) {
    auto edge_noop = [](Edge* e) { };
    dfs(node_begin_fn,
        node_end_fn,
        break_fn,
        edge_noop,
        edge_noop,
        edge_noop,
        edge_noop,
        NULL,
        NULL);
}

// recursion-free version of Tarjan's strongly connected components algorithm
// https://en.wikipedia.org/wiki/Tarjan%27s_strongly_connected_components_algorithm
// Generalized to bidirected graphs as described (confusingly) in
// "Decomposition of a bidirected graph into strongly connected components and
// its signed poset structure", by Kazutoshi Ando, Satoru Fujishige, and Toshio
// Nemoto. http://www.sciencedirect.com/science/article/pii/0166218X95000683

// The best way to think about that paper is that the edges are vectors in a
// vector space with number of dimensions equal to the number of nodes in the
// graph, and an edge attaching to the end a node is the positive unit vector in
// its dimension, and an edge attaching to the start of node is the negative
// unit vector in its dimension.

// The basic idea is that you just consider the orientations as different nodes,
// and the edges as existing between both pairs of orientations they connect,
// and do connected components on that graph. Since we don't care about
// "consistent" or "inconsistent" strongly connected components, we just put a
// node in a component if either orientation is in it. But bear in mind that
// both orientations of a node might not actually be in the same strongly
// connected component in a bidirected graph, so now the components may overlap.
set<set<id_t> > VG::strongly_connected_components(void) {

    // What node visit step are we on?
    int64_t index = 0;
    // What's the search root from which a node was reached?
    map<NodeTraversal, NodeTraversal> roots;
    // At what index step was each node discovered?
    map<NodeTraversal, int64_t> discover_idx;
    // We need our own copy of the DFS stack
    deque<NodeTraversal> stack;
    // And our own set of nodes already on the stack
    set<NodeTraversal> on_stack;
    // What components did we find? Because of the way strongly connected
    // components generalizes, both orientations of a node always end up in the
    // same component.
    set<set<id_t> > components;

    dfs([&](NodeTraversal trav) {
            // When a NodeTraversal is first visited
            // It is its own root
            roots[trav] = trav;
            // We discovered it at this step
            discover_idx[trav] = index++;
            // And it's on the stack
            stack.push_back(trav);
            on_stack.insert(trav);
        },
        [&](NodeTraversal trav) {
            // When a NodeTraversal is done being recursed into
            for (auto next : travs_from(trav)) {
                // Go through all the NodeTraversals reachable reading onwards from this traversal.
                if (on_stack.count(next)) {
                    // If any of those NodeTraversals are on the stack already
                    auto& node_root = roots[trav];
                    auto& next_root = roots[next];
                    // Adopt the root of the NodeTraversal that was discovered first.
                    roots[trav] = discover_idx[node_root] <
                        discover_idx[next_root] ?
                        node_root :
                        next_root;
                }
            }
            if (roots[trav] == trav) {
                // If we didn't find a better root
                NodeTraversal other;
                set<id_t> component;
                do
                {
                    // Grab everything that was put on the DFS stack below us
                    // and put it in our component.
                    other = stack.back();
                    stack.pop_back();
                    on_stack.erase(other);
                    component.insert(other.node->id());
                } while (other != trav);
                components.insert(component);
            }
        });

    return components;
}

// returns the rank of the node in the protobuf array that backs the graph
int VG::node_rank(Node* node) {
    return node_index[node];
}

// returns the rank of the node in the protobuf array that backs the graph
int VG::node_rank(id_t id) {
    return node_index[get_node(id)];
}

vector<Edge> VG::break_cycles(void) {
    // ensure we are sorted
    algorithms::sort(this);
    // remove any edge whose from has a higher index than its to
    vector<Edge*> to_remove;
    for_each_edge([&](Edge* e) {
            // if we cycle to this node or one before in the sort
            if (node_rank(e->from()) >= node_rank(e->to())) {
                to_remove.push_back(e);
            }
        });
    vector<Edge> removed;
    for(Edge* edge : to_remove) {
        //cerr << "removing " << pb2json(*edge) << endl;
        removed.push_back(*edge);
        destroy_edge(edge);
    }
    algorithms::sort(this);
    return removed;
}

bool VG::is_single_stranded(void) {
    for (size_t i = 0; i < graph.edge_size(); i++) {
        const Edge& edge = graph.edge(i);
        if (edge.from_start() != edge.to_end()) {
            return false;
        }
    }
    return true;
}
    
void VG::identity_translation(unordered_map<id_t, pair<id_t, bool>>& node_translation) {
    node_translation.clear();
    for (size_t i = 0; i < graph.node_size(); i++) {
        id_t id = graph.node(i).id();
        node_translation[id] = make_pair(id, false);
    }
}
    
VG VG::reverse_complement_graph(unordered_map<id_t, pair<id_t, bool>>& node_translation) {
    id_t max_id = 0;
    VG rev_comp;
    for (size_t i = 0; i < graph.node_size(); i++) {
        const Node& node = graph.node(i);
        Node* rev_node = rev_comp.graph.add_node();
        rev_node->set_sequence(reverse_complement(node.sequence()));
        rev_node->set_id(node.id());
        max_id = max<id_t>(max_id, node.id());
        
        node_translation[node.id()] = make_pair(node.id(), true);
    }
    rev_comp.current_id = max_id + 1;
    
    for (size_t i = 0; i < graph.edge_size(); i++) {
        const Edge& edge = graph.edge(i);
        Edge* rev_edge = rev_comp.graph.add_edge();
        rev_edge->set_from(edge.to());
        rev_edge->set_from_start(edge.to_end());
        rev_edge->set_to(edge.from());
        rev_edge->set_to_end(edge.from_start());
    }
    
    rev_comp.build_indexes();
    
    return rev_comp;
}
    
bool VG::is_directed_acyclic(void) {
    unordered_map<id_t, pair<int64_t, int64_t>> degrees(graph.node_size());
    for (size_t i = 0; i < graph.node_size(); i++) {
        Node* node = graph.mutable_node(i);
        degrees[node->id()] = make_pair(start_degree(node), end_degree(node));
    }
    
    vector<NodeTraversal> stack;
    for (size_t i = 0; i < graph.node_size(); i++) {
        Node* node = graph.mutable_node(i);
        pair<int64_t, int64_t> node_degrees = degrees[node->id()];
        if (node_degrees.first == 0) {
            stack.emplace_back(node, false);
        }
        if (node_degrees.second == 0) {
            stack.emplace_back(node, true);
        }
    }
    
    while (!stack.empty()) {
        NodeTraversal here = stack.back();
        stack.pop_back();
        
        auto iter = degrees.find(here.node->id());
        if (iter == degrees.end()) {
            continue;
        }
        
        degrees.erase(iter);
        
        vector<NodeTraversal> nexts;
        nodes_next(here, nexts);
        for (NodeTraversal next : nexts) {
            auto next_iter = degrees.find(next.node->id());
            if (next_iter != degrees.end()) {
                int64_t& in_degree = next.backward ? next_iter->second.second : next_iter->second.first;
                in_degree--;
                if (in_degree == 0) {
                    stack.push_back(next);
                }
            }
        }
    }
    return degrees.empty();
}
    
void VG::lazy_sort(void) {
    // a map to the degrees on the left and right sides of nodes
    unordered_map<id_t, pair<int64_t, int64_t>> side_degrees;
    for (size_t i = 0; i < graph.node_size(); i++) {
        id_t id = graph.node(i).id();
        side_degrees[id] = make_pair(start_degree(get_node(id)), end_degree(get_node(id)));
    }
    
    // find the nodes with 0 in degree an initialize the queue with them
    vector<NodeTraversal> stack;
    for (const auto& degree_record : side_degrees) {
        if (degree_record.second.first == 0) {
            stack.emplace_back(get_node(degree_record.first));
        }
    }
    
    vector<id_t> order;
    order.reserve(graph.node_size());
    
    while (!stack.empty()) {
        // get a head node off the queue
        NodeTraversal head_trav = stack.back();
        stack.pop_back();
        
        // add it to the topological order
        order.push_back(head_trav.node->id());
        
        // remove its outgoing edges
        vector<NodeTraversal> nexts;
        nodes_next(head_trav, nexts);
        for (NodeTraversal& next : nexts) {
            // reduce the degree of the appropriate side
            int64_t& inward_degree = next.backward ? side_degrees[next.node->id()].second : side_degrees[next.node->id()].first;
            inward_degree--;
            if (inward_degree == 0) {
                // after removing this edge, the node is now a head, add it to the queue
                stack.push_back(next);
            }
        }
    }
    
    for (size_t i = 0; i < order.size(); i++) {
        // Put the nodes in the order we got
        swap_nodes(get_node(order[i]), graph.mutable_node(i));
    }
}

bool VG::is_acyclic(void) {
    unordered_set<NodeTraversal> seen;
    bool acyclic = true;
    dfs([&](NodeTraversal trav) {
            // When a node orientation is first visited
            if (is_self_looping(trav.node)) {
                acyclic = false;
            }

            for (auto& next : travs_from(trav)) {
                if (seen.count(next)) {
                    acyclic = false;
                    break;
                }
            }
            if (acyclic) {
                seen.insert(trav);
            }
        },
        [&](NodeTraversal trav) {
            // When we leave a node orientation

            // Remove it from the seen array. We may later start from a
            // different root and see a way into this node in this orientation,
            // but it's only a cycle if there's a way into this node in this
            // orientation when it's further up the stack from the node
            // traversal we are finding the way in from.
            seen.erase(trav);
        },
        [&](void) { // our break function
            return !acyclic;
        });
    return acyclic;
}

set<set<id_t> > VG::multinode_strongly_connected_components(void) {
    set<set<id_t> > components;
    for (auto& c : strongly_connected_components()) {
        if (c.size() > 1) {
            components.insert(c);
        }
    }
    return components;
}
    
// keeping all components would be redundant, as every node is a self-component
void VG::keep_multinode_strongly_connected_components(void) {
    unordered_set<id_t> keep;
    for (auto& c : multinode_strongly_connected_components()) {
        for (auto& id : c) {
            keep.insert(id);
        }
    }
    unordered_set<Node*> remove;
    for_each_node([&](Node* n) {
            if (!keep.count(n->id())) {
                remove.insert(n);
            }
        });
    for (auto n : remove) {
        destroy_node(n);
    }
    remove_orphan_edges();
}

size_t VG::size(void) {
    return graph.node_size();
}

size_t VG::length(void) {
    size_t l = 0;
    for_each_node([&l](Node* n) { l+=n->sequence().size(); });
    return l;
}

void VG::swap_nodes(Node* a, Node* b) {
    int aidx = node_index[a];
    int bidx = node_index[b];
    graph.mutable_node()->SwapElements(aidx, bidx);
    node_index[a] = bidx;
    node_index[b] = aidx;
}

Edge* VG::create_edge(NodeTraversal left, NodeTraversal right) {
    // Connect to the start of the left node if it is backward, and the end of the right node if it is backward.
    return create_edge(left.node->id(), right.node->id(), left.backward, right.backward);
}

Edge* VG::create_edge(NodeSide side1, NodeSide side2) {
    // Connect to node 1 (from start if the first side isn't an end) to node 2 (to end if the second side is an end)
    return create_edge(side1.node, side2.node, !side1.is_end, side2.is_end);
}

Edge* VG::create_edge(Node* from, Node* to, bool from_start, bool to_end) {
    return create_edge(from->id(), to->id(), from_start, to_end);
}

Edge* VG::create_edge(id_t from, id_t to, bool from_start, bool to_end) {
    //cerr << "creating edge " << from << "->" << to << endl;
    // ensure the edge (or another between the same sides) does not already exist
    Edge* edge = get_edge(NodeSide(from, !from_start), NodeSide(to, to_end));
    if (edge) {
        // The edge we want to make exists.
        return edge;
    }
    // if not, create it
    edge = graph.add_edge();
    edge->set_from(from);
    edge->set_to(to);
    // Only set the backwardness fields if they are true.
    if(from_start) edge->set_from_start(from_start);
    if(to_end) edge->set_to_end(to_end);
    set_edge(edge);
    edge_index[edge] = graph.edge_size()-1;
    //cerr << "created edge " << edge->from() << "->" << edge->to() << endl;
    return edge;
}

Edge* VG::get_edge(const NodeSide& side1, const NodeSide& side2) {
    auto e = edge_by_sides.find(minmax(side1, side2));
    if (e != edge_by_sides.end()) {
        return e->second;
    } else {
        return NULL;
    }
}

Edge* VG::get_edge(const pair<NodeSide, NodeSide>& sides) {
    return get_edge(sides.first, sides.second);
}

Edge* VG::get_edge(const NodeTraversal& left, const NodeTraversal& right) {
    // We went from the right side of left to the left side of right.
    // We used the end of left if if isn't backward, and we used the end of right if it is.
    return get_edge(NodeSide(left.node->id(), !left.backward),
                    NodeSide(right.node->id(), right.backward));
}

void VG::set_edge(Edge* edge) {
    if (!has_edge(edge)) {
        index_edge_by_node_sides(edge);
    }
}

void VG::for_each_edge_parallel(function<void(Edge*)> lambda) {
    create_progress(graph.edge_size());
    id_t completed = 0;
#pragma omp parallel for shared(completed)
    for (id_t i = 0; i < graph.edge_size(); ++i) {
        lambda(graph.mutable_edge(i));
        if (completed++ % 1000 == 0) {
            update_progress(completed);
        }
    }
    destroy_progress();
}

void VG::for_each_edge(function<void(Edge*)> lambda) {
    for (id_t i = 0; i < graph.edge_size(); ++i) {
        lambda(graph.mutable_edge(i));
    }
}

void VG::destroy_edge(const NodeSide& side1, const NodeSide& side2) {
    destroy_edge(get_edge(side1, side2));
}

void VG::destroy_edge(const pair<NodeSide, NodeSide>& sides) {
    destroy_edge(sides.first, sides.second);
}


void VG::destroy_edge(Edge* edge) {
    //cerr << "destroying edge " << edge->from() << "->" << edge->to() << endl;

    // noop on NULL pointer or non-existent edge
    if (!has_edge(edge)) { return; }

    // first remove the edge from the edge-on-node-side indexes.
    unindex_edge_by_node_sides(edge);

    // get the last edge index (lei) and this edge index (tei)
    int lei = graph.edge_size()-1;
    int tei = edge_index[edge];

    // erase this edge from the index by node IDs.
    // we'll fix up below
    edge_index.erase(edge);

    // Why do we check that lei != tei?
    //
    // It seems, after an inordinate amount of testing and probing,
    // that if we call erase twice on the same entry, we'll end up corrupting the hash_map
    //
    // So, if the element is already at the end of the table,
    // take a little break and just remove the last edge in graph

    // if we need to move the element to the last position in the array...
    if (lei != tei) {

        // get a pointer to the last element
        Edge* last = graph.mutable_edge(lei);

        // erase from our index
        edge_index.erase(last);

        // swap
        graph.mutable_edge()->SwapElements(tei, lei);

        // point to new position
        Edge* nlast = graph.mutable_edge(tei);

        // insert the new edge index position
        edge_index[nlast] = tei;

        // and fix edge indexes for moved edge object
        set_edge(nlast);

    }

    // drop the last position, erasing the node
    // manually delete to free memory (RemoveLast does not free)
    Edge* last_edge = graph.mutable_edge()->ReleaseLast();
    delete last_edge;

    //if (!is_valid()) { cerr << "graph ain't valid" << endl; }

}

void VG::unindex_edge_by_node_sides(const NodeSide& side1, const NodeSide& side2) {
    unindex_edge_by_node_sides(get_edge(side1, side2));
}

void VG::unindex_edge_by_node_sides(Edge* edge) {
    // noop on NULL pointer or non-existent edge
    if (!has_edge(edge)) return;
    //if (!is_valid()) { cerr << "graph ain't valid" << endl; }
    // erase from indexes

    auto edge_pair = NodeSide::pair_from_edge(edge);

    //cerr << "erasing from indexes" << endl;

    //cerr << "Unindexing edge " << edge_pair.first << "<-> " << edge_pair.second << endl;

    // Remove from the edge by node side pair index
    edge_by_sides.erase(edge_pair);

    // Does this edge involve a change of relative orientation?
    bool relative_orientation = edge->from_start() != edge->to_end();

    // Un-index its from node, depending on whether it's attached to the start
    // or end.
    if(edge->from_start()) {
        // The edge is on the start of the from node, so remove it from the
        // start of the from node, with the correct relative orientation for the
        // to node.
        std::pair<id_t, bool> to_remove {edge->to(), relative_orientation};
        swap_remove(edges_start(edge->from()), to_remove);
        // removing the sub-indexes if they are now empty
        // we must do this to maintain a valid structure
        if (edges_on_start[edge->from()].empty()) edges_on_start.erase(edge->from());

        //cerr << "Removed " << edge->from() << "-start to " << edge->to() << " orientation " << relative_orientation << endl;
    } else {
        // The edge is on the end of the from node, do remove it form the end of the from node.
        std::pair<id_t, bool> to_remove {edge->to(), relative_orientation};
        swap_remove(edges_end(edge->from()), to_remove);
        if (edges_on_end[edge->from()].empty()) edges_on_end.erase(edge->from());

        //cerr << "Removed " << edge->from() << "-end to " << edge->to() << " orientation " << relative_orientation << endl;
    }

    if(edge->from() != edge->to() || edge->from_start() == edge->to_end()) {
        // Same for the to node, if we aren't just on the same node and side as with the from node.
        if(edge->to_end()) {
            std::pair<id_t, bool> to_remove {edge->from(), relative_orientation};
            swap_remove(edges_end(edge->to()), to_remove);
            if (edges_on_end[edge->to()].empty()) edges_on_end.erase(edge->to());

            //cerr << "Removed " << edge->to() << "-end to " << edge->from() << " orientation " << relative_orientation << endl;
        } else {
            std::pair<id_t, bool> to_remove {edge->from(), relative_orientation};
            swap_remove(edges_start(edge->to()), to_remove);
            if (edges_on_start[edge->to()].empty()) edges_on_start.erase(edge->to());

            //cerr << "Removed " << edge->to() << "-start to " << edge->from() << " orientation "
            //     << relative_orientation << endl;
        }
    }
}

void VG::index_edge_by_node_sides(Edge* edge) {

    // Generate sides, order them, and index the edge by them.
    edge_by_sides[NodeSide::pair_from_edge(edge)] = edge;

    // Index on ends appropriately depending on from_start and to_end.
    bool relative_orientation = edge->from_start() != edge->to_end();

    if(edge->from_start()) {
        edges_on_start[edge->from()].emplace_back(edge->to(), relative_orientation);
    } else {
        edges_on_end[edge->from()].emplace_back(edge->to(), relative_orientation);
    }

    if(edge->from() != edge->to() || edge->from_start() == edge->to_end()) {
        // Only index the other end of the edge if the edge isn't a self-loop on a single side.
        if(edge->to_end()) {
            edges_on_end[edge->to()].emplace_back(edge->from(), relative_orientation);
        } else {
            edges_on_start[edge->to()].emplace_back(edge->from(), relative_orientation);
        }
    }
}

Node* VG::get_node(id_t id) {
    hash_map<id_t, Node*>::iterator n = node_by_id.find(id);
    if (n != node_by_id.end()) {
        return n->second;
    } else {
        serialize_to_file("wtf.vg");
        throw runtime_error("No node " + to_string(id) + " in graph");
    }
}

Node* VG::create_node(const string& seq) {
    // Autodetect the maximum node ID, in case we have had some contents
    // assigned to us already.
    if (current_id == 1) current_id = max_node_id()+1;
    // Make the node with these contents and the next available ID.
    // Ensure we properly update the current_id that's used to generate new ids.
    return create_node(seq, current_id++);
}

Node* VG::create_node(const string& seq, id_t id) {
    // We no longer support a 0 value as a sentinel to represent letting the graph assign the ID.
    // It was too easy to accidentally pass 0 by forgetting to offset an incoming source of IDs by 1.
    // Use the overload without an ID instead.
    assert(id != 0);
    // create the node
    Node* node = graph.add_node();
    node->set_sequence(seq);
    node->set_id(id);
    // copy it into the graphnn
    // and drop into our id index
    node_by_id[node->id()] = node;
    node_index[node] = graph.node_size()-1;
    //if (!is_valid()) cerr << "graph invalid" << endl;
    return node;
}

void VG::for_each_node_parallel(function<void(Node*)> lambda) {
    create_progress(graph.node_size());
    id_t completed = 0;
    #pragma omp parallel for schedule(dynamic,1) shared(completed)
    for (id_t i = 0; i < graph.node_size(); ++i) {
        lambda(graph.mutable_node(i));
        if (completed++ % 1000 == 0) {
            update_progress(completed);
        }
    }
    destroy_progress();
}

void VG::for_each_node(function<void(Node*)> lambda) {
    for (id_t i = 0; i < graph.node_size(); ++i) {
        lambda(graph.mutable_node(i));
    }
}

void VG::for_each_connected_node(Node* node, function<void(Node*)> lambda) {
    // We keep track of nodes to visit.
    set<Node*> to_visit {node};
    // We mark all the nodes we have visited.
    set<Node*> visited;

    while(!to_visit.empty()) {
        // Grab some node to visit
        Node* visiting = *(to_visit.begin());
        to_visit.erase(to_visit.begin());

        // Visit it
        lambda(visiting);
        visited.insert(visiting);

        // Look at all its edges
        vector<Edge*> edges;
        edges_of_node(visiting, edges);
        for(Edge* edge : edges) {
            if(edge->from() != visiting->id() && !visited.count(get_node(edge->from()))) {
                // We found a new node on the from of the edge
                to_visit.insert(get_node(edge->from()));
            } else if(edge->to() != visiting->id() && !visited.count(get_node(edge->to()))) {
                // We found a new node on the to of the edge
                to_visit.insert(get_node(edge->to()));
            }
        }
    }
}

// a graph composed of this node and the edges that can be uniquely assigned to it
void VG::nonoverlapping_node_context_without_paths(Node* node, VG& g) {
    // add the node
    g.add_node(*node);

    auto grab_edge = [&](Edge* e) {
        // What node owns the edge?
        id_t owner_id = min(e->from(), e->to());
        if(node->id() == owner_id || !has_node(owner_id)) {
            // Either we are the owner, or the owner isn't in the graph to get serialized.
            g.add_edge(*e);
        }
    };

    // Go through all its edges
    vector<pair<id_t, bool>>& start = edges_start(node->id());
    for (auto& e : start) {
        grab_edge(get_edge(NodeSide::pair_from_start_edge(node->id(), e)));
    }
    vector<pair<id_t, bool>>& end = edges_end(node->id());
    for (auto& e : end) {
        grab_edge(get_edge(NodeSide::pair_from_end_edge(node->id(), e)));
    }
    // paths must be added externally
}

void VG::destroy_node(id_t id) {
    destroy_node(get_node(id));
}

void VG::destroy_node(Node* node) {
    //if (!is_valid()) cerr << "graph is invalid before destroy_node" << endl;
    //cerr << "destroying node " << node->id() << " degrees " << start_degree(node) << ", " << end_degree(node) << endl;
    // noop on NULL/nonexistent node
    if (!has_node(node)) { return; }
    // remove edges associated with node
    set<pair<NodeSide, NodeSide>> edges_to_destroy;

    for(auto& other_end : edges_start(node)) {
        // Destroy all the edges on its start
        edges_to_destroy.insert(NodeSide::pair_from_start_edge(node->id(), other_end));
    }

    for(auto& other_end : edges_end(node)) {
        // Destroy all the edges on its end
        edges_to_destroy.insert(NodeSide::pair_from_end_edge(node->id(), other_end));
    }

    for (auto& e : edges_to_destroy) {
        //cerr << "Destroying edge " << e.first << ", " << e.second << endl;
        destroy_edge(e.first, e.second);
        //cerr << "Edge destroyed" << endl;
    }

    // assert cleanup
    edges_on_start.erase(node->id());
    edges_on_end.erase(node->id());

    //assert(start_degree(node) == 0);
    //assert(end_degree(node) == 0);

    // swap node with the last in nodes
    // call RemoveLast() to drop the node
    int lni = graph.node_size()-1;
    int tni = node_index[node];

    if (lni != tni) {
        // swap this node with the last one
        Node* last = graph.mutable_node(lni);
        graph.mutable_node()->SwapElements(tni, lni);
        Node* nlast = graph.mutable_node(tni);

        // and fix up the indexes
        node_by_id[last->id()] = nlast;
        node_index.erase(last);
        node_index[nlast] = tni;
    }

    // remove this node (which is now the last one) and remove references from the indexes
    node_by_id.erase(node->id());
    node_index.erase(node);
    // manually delete to free memory (RemoveLast does not free)
    Node* last_node = graph.mutable_node()->ReleaseLast();
    delete last_node;
    //if (!is_valid()) { cerr << "graph is invalid after destroy_node" << endl; exit(1); }
}

void VG::remove_null_nodes(void) {
    vector<id_t> to_remove;
    for (int i = 0; i < graph.node_size(); ++i) {
        Node* node = graph.mutable_node(i);
        if (node->sequence().size() == 0) {
            to_remove.push_back(node->id());
        }
    }
    for (vector<id_t>::iterator n = to_remove.begin(); n != to_remove.end(); ++n) {
        destroy_node(*n);
    }
}

void VG::remove_null_nodes_forwarding_edges(void) {
    vector<Node*> to_remove;
    int i = 0;
    create_progress(graph.node_size()*2);
    for (i = 0; i < graph.node_size(); ++i) {
        Node* node = graph.mutable_node(i);
        if (node->sequence().size() == 0) {
            to_remove.push_back(node);
        }
        update_progress(i);
    }
    for (vector<Node*>::iterator n = to_remove.begin(); n != to_remove.end(); ++n, ++i) {
        remove_node_forwarding_edges(*n);
        update_progress(i);
    }
    // rebuild path ranks; these may have been affected by node removal
    paths.compact_ranks();
}

void VG::remove_node_forwarding_edges(Node* node) {

    // Grab all the nodes attached to our start, with true if the edge goes to their start
    vector<pair<id_t, bool>>& start = edges_start(node);
    // Grab all the nodes attached to our end, with true if the edge goes to their end
    vector<pair<id_t, bool>>& end = edges_end(node);

    // We instantiate the whole cross product first to avoid working on
    // references to the contents of containers we are modifying. This holds the
    // (node ID, relative orientation) pairs above.
    set<pair<pair<id_t, bool>, pair<id_t, bool>>> edges_to_create;

    // Make edges for the cross product of our start and end edges, making sure
    // to maintain relative orientation.
    for(auto& start_pair : start) {
        for(auto& end_pair : end) {
            // We already have the flags for from_start and to_end for the new edge.
            edges_to_create.emplace(start_pair, end_pair);
        }
    }

    for (auto& e : edges_to_create) {
        // make each edge we want to add
        create_edge(e.first.first, e.second.first, e.first.second, e.second.second);
    }

    // remove the node from paths
    if (paths.has_node_mapping(node)) {
        // We need to copy the set here because we're going to be throwing
        // things out of it while iterating over it.
        auto node_mappings = paths.get_node_mapping_by_path_name(node);
        for (auto& p : node_mappings) {
            for (auto& m : p.second) {
                paths.remove_mapping(m);
            }
        }
    }
    // delete the actual node
    destroy_node(node);
}

void VG::remove_orphan_edges(void) {
    set<pair<NodeSide, NodeSide>> edges;
    for_each_edge([this,&edges](Edge* edge) {
            if (!has_node(edge->from())
                || !has_node(edge->to())) {
                edges.insert(NodeSide::pair_from_edge(edge));
            }
        });
    for (auto edge : edges) {
        destroy_edge(edge);
    }
}

void VG::keep_paths(const set<string>& path_names, set<string>& kept_names) {

    set<id_t> to_keep;
    paths.for_each([&](const Path& path) {
            if (path_names.count(path.name())) {
                kept_names.insert(path.name());
                for (int i = 0; i < path.mapping_size(); ++i) {
                    to_keep.insert(path.mapping(i).position().node_id());
                }
            }
        });

    set<id_t> to_remove;
    for_each_node([&](Node* node) {
            id_t id = node->id();
            if (!to_keep.count(id)) {
                to_remove.insert(id);
            }
        });

    for (auto id : to_remove) {
        destroy_node(id);
    }
    // clean up dangling edges
    remove_orphan_edges();

    // Throw out all the paths data for paths we don't want to keep.
    paths.keep_paths(path_names);
}

void VG::keep_path(const string& path_name) {
    set<string> s,k; s.insert(path_name);
    keep_paths(s, k);
}

// divide a node into two pieces at the given offset
void VG::divide_node(Node* node, int pos, Node*& left, Node*& right) {
    vector<Node*> parts;
    vector<int> positions{pos};

    divide_node(node, positions, parts);

    // Pull out the nodes we made
    left = parts.front();
    right = parts.back();
}

void VG::divide_node(Node* node, vector<int>& positions, vector<Node*>& parts) {

#ifdef debug_divide
#pragma omp critical (cerr)
    cerr << "dividing node " << node->id() << " at ";
    for(auto pos : positions) {
        cerr << pos << ", ";
    }
    cerr << endl;
#endif

    // Check all the positions first
    for(auto pos : positions) {

        if (pos < 0 || pos > node->sequence().size()) {
    #pragma omp critical (cerr)
            {
                cerr << omp_get_thread_num() << ": cannot divide node " << node->id();
                
                if(node->sequence().size() <= 1000) {
                    // Add sequences for short nodes
                    cerr << ":" << node->sequence();
                }
                
                cerr << " -- position (" << pos << ") is less than 0 or greater than sequence length ("
                     << node->sequence().size() << ")" << endl;
                exit(1);
            }
        }
    }

    int last_pos = 0;
    for(auto pos : positions) {
        // Make all the nodes ending at the given positions, grabbing the appropriate substrings
        Node* new_node = create_node(node->sequence().substr(last_pos, pos - last_pos));
        last_pos = pos;
        parts.push_back(new_node);
    }

    // Make the last node with the remaining sequence
    Node* last_node = create_node(node->sequence().substr(last_pos));
    parts.push_back(last_node);

#ifdef debug_divide

#pragma omp critical (cerr)
    {
        for(auto* part : parts) {
            cerr << "\tCreated node " << part->id() << " (" << part->sequence().size() << ")";
            if(part->sequence().size() <= 1000) {
                // Add sequences for short nodes
                cerr << ": " << part->sequence();
            }
            cerr << endl;
        }
    }

#endif

    // Our leftmost new node is now parts.front(), and our rightmost parts.back()

    // Create edges between the left node (optionally its start) and the right node (optionally its end)
    set<pair<pair<id_t, bool>, pair<id_t, bool>>> edges_to_create;

    // replace the connections to the node's start
    for(auto e : edges_start(node)) {
        // We have to check for self loops, as these will be clobbered by the division of the node
        if (e.first == node->id()) {
            // if it's a reversed edge, it would be from the start of this node
            if (e.second) {
                // so set it to the front node
                e.first = parts.front()->id();
            } else {
                // otherwise, it's from the end, so set it to the back node
                e.first = parts.back()->id();
            }
        }
        // Make an edge to the left node's start from wherever this edge went.
        edges_to_create.emplace(make_pair(e.first, e.second), make_pair(parts.front()->id(), false));
    }

    // replace the connections to the node's end
    for(auto e : edges_end(node)) {
        // We have to check for self loops, as these will be clobbered by the division of the node
        if (e.first == node->id()) {
            // if it's a reversed edge, it would be to the start of this node
            if (e.second) {
                // so set it to the back node
                e.first = parts.back()->id();
            } else {
                // otherwise, it's to the start, so set it to the front node
                e.first = parts.front()->id();
            }
        }
        // Make an edge from the right node's end to wherever this edge went.
        edges_to_create.emplace(make_pair(parts.back()->id(), false), make_pair(e.first, e.second));
    }

    // create the edges here as otherwise we will invalidate the iterators
    for (auto& e : edges_to_create) {
        // Swizzle the from_start and to_end bits to the right place.
        create_edge(e.first.first, e.second.first, e.first.second, e.second.second);
    }

    for(int i = 0; i < parts.size() - 1; i++) {
        // Connect all the new parts left to right. These edges always go from end to start.
        create_edge(parts[i], parts[i+1]);
    }

    // divide paths
    // note that we can't do this (yet) for non-exact matching paths
    if (paths.has_node_mapping(node)) {
        auto node_path_mapping = paths.get_node_mapping_by_path_name(node);
        // apply to left and right
        vector<mapping_t*> to_divide;
        for (auto& pm : node_path_mapping) {
            string path_name = pm.first;
            for (auto* m : pm.second) {
                to_divide.push_back(m);
            }
        }
        for (auto m : to_divide) {
            // we have to divide the mapping

#ifdef debug_divide

#pragma omp critical (cerr)
            cerr << omp_get_thread_num() << ": dividing mapping " << pb2json(*m) << endl;
#endif

            string path_name = paths.mapping_path_name(m);
            // OK, we're somewhat N^2 in mapping division, if there are edits to
            // copy. But we're nearly linear!

            // We can only work on full-length perfect matches, because we make the cuts in mapping space.

            vector<mapping_t> mapping_parts;
            mapping_t remainder = *m;
            int local_offset = 0;

            for(int i = 0; i < positions.size(); i++) {
                // At this position
                auto& pos = positions[i];
                // Break off the mapping
                // Note that we are cutting at mapping-relative locations and not node-relative locations.
                pair<mapping_t, mapping_t> halves;



                if(remainder.is_reverse()) {
                    // Cut positions are measured from the end of the original node.
                    halves = cut_mapping(remainder, node->sequence().size() - pos);
                    // Turn them around to be in reference order
                    std::swap(halves.first, halves.second);
                } else {
                    // Mapping offsets are the same as offsets from the start of the node.
                    halves = cut_mapping(remainder, pos - local_offset);
                }

                // This is the one we have produced
                mapping_t& chunk = halves.first;

                // Tell it what it's mapping to
                // We'll take all of this node.
                chunk.set_node_id(parts[i]->id());

                mapping_parts.push_back(chunk);
                remainder = halves.second;
                // The remainder needs to be divided at a position relative to
                // here.
                local_offset = pos;
            }
            // Place the last part of the mapping.
            // It takes all of the last node.
            remainder.set_node_id(parts.back()->id());
            mapping_parts.push_back(remainder);

            //divide_mapping
            // with the mapping divided, insert the pieces where the old one was
            bool is_rev = m->is_reverse();
            auto mpit = paths.remove_mapping(m);
            if (is_rev) {
                // insert left then right in the path, since we're going through
                // this node backward (insert puts *before* the iterator)
                for(auto i = mapping_parts.begin(); i != mapping_parts.end(); ++i) {
                    mpit = paths.insert_mapping(mpit, path_name, *i);
                }
            } else {
                // insert right then left (insert puts *before* the iterator)
                for(auto i = mapping_parts.rbegin(); i != mapping_parts.rend(); ++i) {
                    mpit = paths.insert_mapping(mpit, path_name, *i);
                }
            }



#ifdef debug_divide
#pragma omp critical (cerr)
            cerr << omp_get_thread_num() << ": produced mappings:" << endl;
            for(auto mapping : mapping_parts) {
                cerr << "\t" << pb2json(mapping) << endl;
            }
#endif
        }
    }

    destroy_node(node);

}

// for dividing a path of nodes with an underlying coordinate system
void VG::divide_path(map<long, id_t>& path, long pos, Node*& left, Node*& right) {

    map<long, id_t>::iterator target = path.upper_bound(pos);
    --target; // we should now be pointing to the target ref node

    long node_pos = target->first;
    Node* old = get_node(target->second);

    // nothing to do
    if (node_pos == pos) {
        map<long, id_t>::iterator n = target; --n;
        left = get_node(n->second);
        right = get_node(target->second);
    } else {
        // divide the target node at our pos
        int diff = pos - node_pos;
        divide_node(old, diff, left, right);
        // left
        path[node_pos] = left->id();
        // right
        path[pos] = right->id();
    }
}

set<NodeTraversal> VG::travs_of(NodeTraversal node) {
    auto tos = travs_to(node);
    auto froms = travs_from(node);
    set<NodeTraversal> ofs;
    std::set_union(tos.begin(), tos.end(),
                   froms.begin(), froms.end(),
                   std::inserter(ofs, ofs.begin()));
    return ofs;
}

// traversals before this node on the same strand
set<NodeTraversal> VG::travs_to(NodeTraversal node) {
    set<NodeTraversal> tos;
    vector<NodeTraversal> tov;
    nodes_prev(node, tov);
    for (auto& t : tov) tos.insert(t);
    return tos;
}

// traversals after this node on the same strand
set<NodeTraversal> VG::travs_from(NodeTraversal node) {
    set<NodeTraversal> froms;
    vector<NodeTraversal> fromv;
    nodes_next(node, fromv);
    for (auto& t : fromv) froms.insert(t);
    return froms;
}

void VG::nodes_prev(NodeTraversal node, vector<NodeTraversal>& nodes) {
    // Get the node IDs that attach to the left of this node, and whether we are
    // attached relatively forward (false) or backward (true)
    vector<pair<id_t, bool>>& left_nodes = node.backward ? edges_end(node.node) : edges_start(node.node);
    for (auto& prev : left_nodes) {
        // Make a NodeTraversal that is an oriented description of the node attached to our relative left.
        // If we're backward, and it's in the same relative orientation as us, it needs to be backward too.
        nodes.emplace_back(get_node(prev.first), prev.second != node.backward);
    }
}

vector<NodeTraversal> VG::nodes_prev(NodeTraversal n) {
    vector<NodeTraversal> nodes;
    nodes_prev(n, nodes);
    return nodes;
}

void VG::nodes_next(NodeTraversal node, vector<NodeTraversal>& nodes) {
    // Get the node IDs that attach to the right of this node, and whether we
    // are attached relatively forward (false) or backward (true)
    vector<pair<id_t, bool>>& right_nodes = node.backward ? edges_start(node.node) : edges_end(node.node);
    for (auto& next : right_nodes) {
        // Make a NodeTraversal that is an oriented description of the node attached to our relative right.
        // If we're backward, and it's in the same relative orientation as us, it needs to be backward too.
        nodes.emplace_back(get_node(next.first), next.second != node.backward);
    }
}

vector<NodeTraversal> VG::nodes_next(NodeTraversal n) {
    vector<NodeTraversal> nodes;
    nodes_next(n, nodes);
    return nodes;
}

int VG::node_count_prev(NodeTraversal n) {
    vector<NodeTraversal> nodes;
    nodes_prev(n, nodes);
    return nodes.size();
}

int VG::node_count_next(NodeTraversal n) {
    vector<NodeTraversal> nodes;
    nodes_next(n, nodes);
    return nodes.size();
}

// path utilities
// these are in this class because attributes of the path (such as its sequence) are a property of the graph

Path VG::create_path(const list<NodeTraversal>& nodes) {
    Path path;
    for (const NodeTraversal& n : nodes) {
        Mapping* mapping = path.add_mapping();
        mapping->mutable_position()->set_node_id(n.node->id());
        // If the node is backward along this path, note it.
        if(n.backward) mapping->mutable_position()->set_is_reverse(n.backward);
        // TODO: Is it OK if we say we're at a mapping at offset 0 of this node, backwards? Or should we offset ourselves to the end?
    }
    return path;
}

string VG::path_string(const list<NodeTraversal>& nodes) {
    string seq;
    for (const NodeTraversal& n : nodes) {
        if(n.backward) {
            seq.append(reverse_complement(n.node->sequence()));
        } else {
            seq.append(n.node->sequence());
        }
    }
    return seq;
}

string VG::path_string(const Path& path) {
    string seq;
    for (int i = 0; i < path.mapping_size(); ++i) {
        auto& m = path.mapping(i);
        Node* n = node_by_id[m.position().node_id()];
        seq.append(mapping_sequence(m, *n));
    }
    return seq;
}

void VG::expand_path(const list<NodeTraversal>& path, vector<NodeTraversal>& expanded) {
    for (list<NodeTraversal>::const_iterator n = path.begin(); n != path.end(); ++n) {
        NodeTraversal node = *n;
        int s = node.node->sequence().size();
        for (int i = 0; i < s; ++i) {
            expanded.push_back(node);
        }
    }
}

void VG::expand_path(list<NodeTraversal>& path, vector<list<NodeTraversal>::iterator>& expanded) {
    for (list<NodeTraversal>::iterator n = path.begin(); n != path.end(); ++n) {
        int s = (*n).node->sequence().size();
        for (int i = 0; i < s; ++i) {
            expanded.push_back(n);
        }
    }
}

// The correct way to edit the graph
vector<Translation> VG::edit(vector<Path>& paths_to_add, bool save_paths, bool update_paths, bool break_at_ends) {
    // Collect the breakpoints
    map<id_t, set<pos_t>> breakpoints;

#ifdef debug
    for (auto& p : paths_to_add) {
        cerr << pb2json(p) << endl;
    }
#endif

    std::vector<Path> simplified_paths;

    for(auto path : paths_to_add) {
        // Simplify the path, just to eliminate adjacent match Edits in the same
        // Mapping (because we don't have or want a breakpoint there)
        simplified_paths.push_back(simplify(path));
    }

    // If we are going to actually add the paths to the graph, we need to break at path ends
    break_at_ends |= save_paths;

    for(auto path : simplified_paths) {
        // Add in breakpoints from each path
        find_breakpoints(path, breakpoints, break_at_ends);
    }

    // Invert the breakpoints that are on the reverse strand
    breakpoints = forwardize_breakpoints(breakpoints);

    // Clear existing path ranks.
    paths.clear_mapping_ranks();

    // get the node sizes, for use when making the translation
    map<id_t, size_t> orig_node_sizes;
    for_each_node([&](Node* node) {
            orig_node_sizes[node->id()] = node->sequence().size();
        });

    // Break any nodes that need to be broken. Save the map we need to translate
    // from offsets on old nodes to new nodes. Note that this would mess up the
    // ranks of nodes in their existing paths, which is why we clear and rebuild
    // them.
    auto node_translation = ensure_breakpoints(breakpoints);

    // we remember the sequences of nodes we've added at particular positions on the forward strand
    map<pair<pos_t, string>, vector<Node*>> added_seqs;
    // we will record the nodes that we add, so we can correctly make the returned translation
    map<Node*, Path> added_nodes;
    for(auto& path : simplified_paths) {
        // Now go through each new path again, by reference so we can overwrite.
        
        // Create new nodes/wire things up. Get the added version of the path.
        Path added = add_nodes_and_edges(path, node_translation, added_seqs, added_nodes, orig_node_sizes);
        
        if (save_paths) {
            // Add this path to the graph's paths object
            paths.extend(added);
        }
        
        if (update_paths) {
            // Replace the simplified path in original graph space with one in new graph space.
            path = added;
        }
    }

    if (update_paths) {
        // We replaced all the paths in simplifies_paths, so send those back out as the embedded versions.
        std::swap(simplified_paths, paths_to_add);
    }

    // Rebuild path ranks, aux mapping, etc. by compacting the path ranks
    paths.compact_ranks();

    // something is off about this check.
    // with the paths sorted, let's double-check that the edges are here
    paths.for_each([&](const Path& path) {
            for (size_t i = 1; i < path.mapping_size(); ++i) {
                auto& m1 = path.mapping(i-1);
                auto& m2 = path.mapping(i);
                //if (!adjacent_mappings(m1, m2)) continue; // the path is completely represented here
                auto s1 = NodeSide(m1.position().node_id(), (m1.position().is_reverse() ? false : true));
                auto s2 = NodeSide(m2.position().node_id(), (m2.position().is_reverse() ? true : false));
                // check that we always have an edge between the two nodes in the correct direction
                if (!has_edge(s1, s2)) {
                    //cerr << "edge missing! " << s1 << " " << s2 << endl;
                    // force these edges in
                    create_edge(s1, s2);
                }
            }
        });

    // execute a semi partial order sort on the nodes
    algorithms::sort(this);

    // make the translation
    return make_translation(node_translation, added_nodes, orig_node_sizes);
}

// The not quite as robust (TODO: how?) but actually efficient way to edit the graph.
vector<Translation> VG::edit_fast(const Path& path, set<NodeSide>& dangling) {
    // Collect the breakpoints
    map<id_t, set<pos_t>> breakpoints;

    // Every path we add needs to be simplified to merge adjacent match edits
    // and prevent spurious breakpoints.
    Path simplified = simplify(path);

    // Find the breakpoints this path needs
    find_breakpoints(path, breakpoints);

    // Invert the breakpoints that are on the reverse strand
    breakpoints = forwardize_breakpoints(breakpoints);
    
    // Get the node sizes of nodes that are getting destroyed, for use when
    // making the translations and when reverse complementing old-graph paths.
    map<id_t, size_t> orig_node_sizes;
    for (auto& kv : breakpoints) {
        // Just get the size of every node with a breakpoint on it.
        // There might be extra, but it's way smaller than the whole graph.
        orig_node_sizes[kv.first] = get_node(kv.first)->sequence().size();
    }

    // Break any nodes that need to be broken. Save the map we need to translate
    // from start positions on old nodes to new nodes.
    map<pos_t, Node*> node_translation = ensure_breakpoints(breakpoints);
    
#ifdef debug
    for(auto& kv : node_translation) {
        cerr << "Translate old " << kv.first << " to " << (kv.second == nullptr ? (id_t)0 : (id_t)kv.second->id()) << endl;
    }
#endif
    
    // we remember the sequences of nodes we've added at particular positions on the forward strand
    map<pair<pos_t, string>, vector<Node*>> added_seqs;
    // we will record the nodes that we add, so we can correctly make the returned translation for novel insert nodes
    map<Node*, Path> added_nodes;
    // create new nodes/wire things up.
    add_nodes_and_edges(path, node_translation, added_seqs, added_nodes, orig_node_sizes, dangling);

    // Make the translations (in about the same format as VG::edit(), but
    // without a translation for every single node and with just the nodes we
    // touched)
    vector<Translation> translations;
    
    for (auto& kv : node_translation) {
        // For every translation from an old position to a new node (which may be null)
        if (kv.second == nullptr) {
            // Ignore the past-the-end entries
            continue;
        }
        // There are reverse and forward entries and we make Translations for both.
                
        // Get the length of sequence involved
        auto seq_length = kv.second->sequence().size();
                
        // Make a translation
        Translation trans;
        // The translation is "to" the new fragmentary node
        auto* to_path = trans.mutable_to();
        auto* to_mapping = to_path->add_mapping();
        // Make sure the to mapping is in the same orientation as the
        // from mapping, since we're going to be making translations on
        // both strands and the new node is the same local orientation
        // as the old node.
        *(to_mapping->mutable_position()) = make_position(kv.second->id(), is_rev(kv.first), 0);
        auto* to_edit = to_mapping->add_edit();
        to_edit->set_from_length(seq_length);
        to_edit->set_to_length(seq_length );
        // And it is "from" the original position
        Path* from_path = trans.mutable_from();
        auto* from_mapping = from_path->add_mapping();
        *(from_mapping->mutable_position()) = make_position(kv.first);
        auto* from_edit = from_mapping->add_edit();
        from_edit->set_from_length(seq_length);
        from_edit->set_to_length(seq_length);
        
        translations.push_back(trans);
    }
    
    // TODO: maybe we should also add translations to anchor completely novel nodes?

    return translations;
}

vector<Translation> VG::make_translation(const map<pos_t, Node*>& node_translation,
                                         const map<Node*, Path>& added_nodes,
                                         const map<id_t, size_t>& orig_node_sizes) {
    vector<Translation> translation;
    // invert the translation
    map<Node*, pos_t> inv_node_trans;
    for (auto& t : node_translation) {
        if (!is_rev(t.first)) {
            inv_node_trans[t.second] = t.first;
        }
    }
    // walk the whole graph
    for_each_node([&](Node* node) {
            translation.emplace_back();
            auto& trans = translation.back();
            auto f = inv_node_trans.find(node);
            auto added = added_nodes.find(node);
            if (f != inv_node_trans.end()) {
                // if the node is in the inverted translation, use the position to make a mapping
                auto pos = f->second;
                auto from_mapping = trans.mutable_from()->add_mapping();
                auto to_mapping = trans.mutable_to()->add_mapping();
                // Make sure the to mapping is in the same orientation as the
                // from mapping, since we're going to be making translations on
                // both strands and the new node is the same local orientation
                // as the old node.
                *to_mapping->mutable_position() = make_position(node->id(), is_rev(pos), 0);
                *from_mapping->mutable_position() = make_position(pos);
                auto match_length = node->sequence().size();
                auto to_edit = to_mapping->add_edit();
                to_edit->set_to_length(match_length);
                to_edit->set_from_length(match_length);
                auto from_edit = from_mapping->add_edit();
                from_edit->set_to_length(match_length);
                from_edit->set_from_length(match_length);
            } else if (added != added_nodes.end()) {
                // the node is novel
                auto to_mapping = trans.mutable_to()->add_mapping();
                *to_mapping->mutable_position() = make_position(node->id(), false, 0);
                auto to_edit = to_mapping->add_edit();
                to_edit->set_to_length(node->sequence().size());
                to_edit->set_from_length(node->sequence().size());
                auto from_path = trans.mutable_from();
                *trans.mutable_from() = added->second;
            } else {
                // otherwise we assume that the graph is unchanged
                auto from_mapping = trans.mutable_from()->add_mapping();
                auto to_mapping = trans.mutable_to()->add_mapping();
                *to_mapping->mutable_position() = make_position(node->id(), false, 0);
                *from_mapping->mutable_position() = make_position(node->id(), false, 0);
                auto match_length = node->sequence().size();
                auto to_edit = to_mapping->add_edit();
                to_edit->set_to_length(match_length);
                to_edit->set_from_length(match_length);
                auto from_edit = from_mapping->add_edit();
                from_edit->set_to_length(match_length);
                from_edit->set_from_length(match_length);
            }
        });
    std::sort(translation.begin(), translation.end(),
              [&](const Translation& t1, const Translation& t2) {
                  if (!t1.from().mapping_size() && !t2.from().mapping_size()) {
                      // warning: this won't work if we don't have to mappings
                      // this guards against the lurking segfault
                      return t1.to().mapping_size() && t2.to().mapping_size()
                          && make_pos_t(t1.to().mapping(0).position())
                          < make_pos_t(t2.to().mapping(0).position());
                  } else if (!t1.from().mapping_size()) {
                      return true;
                  } else if (!t2.from().mapping_size()) {
                      return false;
                  } else {
                      return make_pos_t(t1.from().mapping(0).position())
                          < make_pos_t(t2.from().mapping(0).position());
                  }
              });
    // append the reverse complement of the translation
    vector<Translation> reverse_translation;
    auto get_curr_node_length = [&](id_t id) {
        return get_node(id)->sequence().size();
    };
    auto get_orig_node_length = [&](id_t id) {
        auto f = orig_node_sizes.find(id);
        if (f == orig_node_sizes.end()) {
            // The node has no entry, so it must not have been broken
            return get_node(id)->sequence().size();
        }
        return f->second;
    };
    for (auto& trans : translation) {
        reverse_translation.emplace_back();
        auto& rev_trans = reverse_translation.back();
        *rev_trans.mutable_to() = simplify(reverse_complement_path(trans.to(), get_curr_node_length));
        *rev_trans.mutable_from() = simplify(reverse_complement_path(trans.from(), get_orig_node_length));
    }
    translation.insert(translation.end(), reverse_translation.begin(), reverse_translation.end());
    return translation;
}

map<id_t, set<pos_t>> VG::forwardize_breakpoints(const map<id_t, set<pos_t>>& breakpoints) {
    map<id_t, set<pos_t>> fwd;
    for (auto& p : breakpoints) {
        id_t node_id = p.first;
        assert(has_node(node_id));
        size_t node_length = get_node(node_id)->sequence().size();
        auto bp = p.second;
        for (auto& pos : bp) {
            pos_t x = pos;
            if (offset(pos) == node_length) continue;
            if (offset(pos) > node_length) {
                cerr << "VG::forwardize_breakpoints error: failure, position " << pos << " is not inside node "
                     << pb2json(*get_node(node_id)) << endl;
                assert(false);
            }
            if (is_rev(pos)) {
                fwd[node_id].insert(reverse(pos, node_length));
            } else {
                fwd[node_id].insert(pos);
            }
        }
    }
    return fwd;
}

// returns breakpoints on the forward strand of the nodes
void VG::find_breakpoints(const Path& path, map<id_t, set<pos_t>>& breakpoints, bool break_ends) {
    // We need to work out what offsets we will need to break each node at, if
    // we want to add in all the new material and edges in this path.

#ifdef debug
    cerr << "Processing path..." << endl;
#endif

    for (size_t i = 0; i < path.mapping_size(); ++i) {
        // For each Mapping in the path
        const Mapping& m = path.mapping(i);

        // What node are we on?
        id_t node_id = m.position().node_id();

        if(node_id == 0) {
            // Skip Mappings that aren't actually to nodes.
            continue;
        }

        // See where the next edit starts in the node. It is always included
        // (even when the edit runs backward), unless the edit has 0 length in
        // the reference.
        pos_t edit_first_position = make_pos_t(m.position());

#ifdef debug
        cerr << "Processing mapping " << pb2json(m) << endl;
#endif

        for(size_t j = 0; j < m.edit_size(); ++j) {
            // For each Edit in the mapping
            const Edit& e = m.edit(j);

            // We know where the mapping starts in its node. But where does it
            // end (inclusive)? Note that if the edit has 0 reference length,
            // this may not actually be included in the edit (and
            // edit_first_position will be further along than
            // edit_last_position).
            pos_t edit_last_position = edit_first_position;
            if (e.from_length()) {
                get_offset(edit_last_position) += e.from_length();
            }

#ifdef debug
            cerr << "Edit on " << node_id << " from " << edit_first_position << " to " << edit_last_position << endl;
            cerr << pb2json(e) << endl;
#endif

            if (!edit_is_match(e) || (j == 0 && (i != 0 || break_ends))) {
                // If this edit is not a perfect match, or if this is the first
                // edit in this mapping and either we had a previous mapping we
                // may need to connect to or we want to break at the path's
                // start, we need to make sure we have a breakpoint at the start
                // of this edit.

#ifdef debug
                cerr << "Need to break " << node_id << " at edit lower end " <<
                    edit_first_position << endl;
#endif

                // We need to snip between edit_first_position and edit_first_position - direction.
                // Note that it doesn't matter if we put breakpoints at 0 and 1-past-the-end; those will be ignored.
                breakpoints[node_id].insert(edit_first_position);
            }

            if (!edit_is_match(e) || (j == m.edit_size() - 1 && (i != path.mapping_size() - 1 || break_ends))) {
                // If this edit is not a perfect match, or if it is the last
                // edit in a mapping and we have a subsequent mapping we might
                // need to connect to or we want to break at the path ends, make
                // sure we have a breakpoint at the end of this edit.

#ifdef debug
                cerr << "Need to break " << node_id << " at past edit upper end " <<
                    edit_last_position << endl;
#endif

                // We also need to snip between edit_last_position and edit_last_position + direction.
                breakpoints[node_id].insert(edit_last_position);
            }

            // TODO: for an insertion or substitution, note that we need a new
            // node and two new edges.

            // TODO: for a deletion, note that we need an edge. TODO: Catch
            // and complain about some things we can't handle (like a path with
            // a leading/trailing deletion)? Or just skip deletions when wiring.

            // Use up the portion of the node taken by this mapping, so we know
            // where the next mapping will start.
            edit_first_position = edit_last_position;
        }
    }

}

map<pos_t, Node*> VG::ensure_breakpoints(const map<id_t, set<pos_t>>& breakpoints) {
    // Set up the map we will fill in with the new node start positions in the
    // old nodes.
    map<pos_t, Node*> toReturn;

    for(auto& kv : breakpoints) {
        // Go through all the nodes we need to break up
        auto original_node_id = kv.first;

        // Save the original node length. We don't want to break here (or later)
        // because that would be off the end.
        id_t original_node_length = get_node(original_node_id)->sequence().size();

        // We are going through the breakpoints left to right, so we need to
        // keep the node pointer for the right part that still needs further
        // dividing.
        Node* right_part = get_node(original_node_id);
        Node* left_part = nullptr;

        pos_t last_bp = make_pos_t(original_node_id, false, 0);
        // How far into the original node does our right part start?
        id_t current_offset = 0;

        for(auto breakpoint : kv.second) {
            // For every point at which we need to make a new node, in ascending
            // order (due to the way sets store ints)...

            // ensure that we're on the forward strand (should be the case due to forwardize_breakpoints)
            assert(!is_rev(breakpoint));

            // This breakpoint already exists, because the node starts or ends here
            if(offset(breakpoint) == 0
               || offset(breakpoint) == original_node_length) {
                continue;
            }

            // How far in do we need to break the remaining right part? And how
            // many bases will be in this new left part?
            id_t divide_offset = offset(breakpoint) - current_offset;


#ifdef debug
            cerr << "Need to divide original " << original_node_id << " at " << breakpoint << "/" <<

                original_node_length << endl;
            cerr << "Translates to " << right_part->id() << " at " << divide_offset << "/" <<
                right_part->sequence().size() << endl;
            cerr << "divide offset is " << divide_offset << endl;
#endif

            if (offset(breakpoint) <= 0) { cerr << "breakpoint is " << breakpoint << endl; }
            assert(offset(breakpoint) > 0);
            if (offset(breakpoint) >= original_node_length) { cerr << "breakpoint is " << breakpoint << endl; }
            assert(offset(breakpoint) < original_node_length);

            // Make a new left part and right part. This updates all the
            // existing perfect match paths in the graph.
            divide_node(right_part, divide_offset, left_part, right_part);

#ifdef debug

            cerr << "Produced " << left_part->id() << " (" << left_part->sequence().size() << " bp)" << endl;
            cerr << "Left " << right_part->id() << " (" << right_part->sequence().size() << " bp)" << endl;
#endif

            // The left part is now done. We know it started at current_offset
            // and ended before breakpoint, so record it by start position.

            // record forward and reverse
            toReturn[last_bp] = left_part;
            toReturn[reverse(breakpoint, original_node_length)] = left_part;

            // Record that more sequence has been consumed
            current_offset += divide_offset;
            last_bp = breakpoint;

        }

        // Now the right part is done too. It's going to be the part
        // corresponding to the remainder of the original node.
        toReturn[last_bp] = right_part;
        toReturn[make_pos_t(original_node_id, true, 0)] = right_part;

        // and record the start and end of the node
        toReturn[make_pos_t(original_node_id, true, original_node_length)] = nullptr;
        toReturn[make_pos_t(original_node_id, false, original_node_length)] = nullptr;

    }

    return toReturn;
}

Path VG::add_nodes_and_edges(const Path& path,
                             const map<pos_t, Node*>& node_translation,
                             map<pair<pos_t, string>, vector<Node*>>& added_seqs,
                             map<Node*, Path>& added_nodes,
                             const map<id_t, size_t>& orig_node_sizes,
                             size_t max_node_size) {

    set<NodeSide> dangling;
    return add_nodes_and_edges(path,
        node_translation,
        added_seqs,
        added_nodes,
        orig_node_sizes,
        dangling,
        max_node_size);  

}

Path VG::add_nodes_and_edges(const Path& path,
                             const map<pos_t, Node*>& node_translation,
                             map<pair<pos_t, string>, vector<Node*>>& added_seqs,
                             map<Node*, Path>& added_nodes,
                             const map<id_t, size_t>& orig_node_sizes,
                             set<NodeSide>& dangling,
                             size_t max_node_size) {

    // The basic algorithm is to traverse the path edit by edit, keeping track
    // of a NodeSide for the last piece of sequence we were on. If we hit an
    // edit that creates new sequence, we check if it has been added before If
    // it has, we use it. If not, we create that new sequence as a node, and
    // attach it to the dangling NodeSide(s), and leave its end dangling
    // instead. If we hit an edit that corresponds to a match, we know that
    // there's a breakpoint on each end (since it's bordered by a non-perfect-
    // match or the end of a node), so we can attach its start to the dangling
    // NodeSide(s) and leave its end dangling instead.

    // We need node_translation to translate between node ID space, where the
    // paths are articulated, and new node ID space, where the edges are being
    // made.

    // We need orig_node_sizes so we can remember the sizes of nodes that we
    // modified, so we can interpret the paths. It only holds sizes for modified
    // nodes.
    
    // This is where we will keep the version of the path articulated as
    // actually embedded in the graph.
    Path embedded;
    embedded.set_name(path.name());

    // We use this function to get the node that contains a position on an
    // original node.
    auto find_new_node = [&](pos_t old_pos) {
        if(node_translation.find(make_pos_t(id(old_pos), false, 0)) == node_translation.end()) {
            // The node is unchanged
            auto n = get_node(id(old_pos));
            assert(n != nullptr);
            return n;
        }
        // Otherwise, get the first new node starting after that position, and
        // then look left.
        auto found = node_translation.upper_bound(old_pos);
        assert(found != node_translation.end());
        if (id(found->first) != id(old_pos)
            || is_rev(found->first) != is_rev(old_pos)) {
            return (Node*)nullptr;
        }
        // Get the thing before that (last key <= the position we want
        --found;
        assert(found->second != nullptr);

        // Return the node we found.
        return found->second;
    };

    auto create_new_mappings = [&](pos_t p1, pos_t p2, bool is_rev) {
        vector<Mapping> mappings;
        vector<Node*> nodes;
        for (pos_t p = p1; p <= p2; ++get_offset(p)) {
            auto n = find_new_node(p);
            assert(n != nullptr);
            nodes.push_back(find_new_node(p));
        }
        auto np = nodes.begin();
        while (np != nodes.end()) {
            size_t c = 0;
            auto n1 = np;
            while (np != nodes.end() && *n1 == *np) {
                ++c;
                ++np; // we'll always increment once
            }
            assert(c);
            // set the mapping position
            Mapping m;
            m.mutable_position()->set_node_id((*n1)->id());
            m.mutable_position()->set_is_reverse(is_rev);
            // and the edit that says we match
            Edit* e = m.add_edit();
            e->set_from_length(c);
            e->set_to_length(c);
            mappings.push_back(m);
        }
        return mappings;
    };

    for (size_t i = 0; i < path.mapping_size(); ++i) {
        // For each Mapping in the path
        const Mapping& m = path.mapping(i);

        // What node are we on? In old node ID space.
        id_t node_id = m.position().node_id();

        // See where the next edit starts in the node. It is always included
        // (even when the edit runs backward), unless the edit has 0 length in
        // the reference.
        pos_t edit_first_position = make_pos_t(m.position());

        for(size_t j = 0; j < m.edit_size(); ++j) {
            // For each Edit in the mapping
            const Edit& e = m.edit(j);

            // Work out where its end position on the original node is (inclusive)
            // We don't use this on insertions, so 0-from-length edits don't matter.
            pos_t edit_last_position = edit_first_position;
            //get_offset(edit_last_position) += (e.from_length()?e.from_length()-1:0);
            get_offset(edit_last_position) += (e.from_length()?e.from_length()-1:0);

//#define debug_edit true
#ifdef debug_edit
            cerr << "Edit on " << node_id << " from " << edit_first_position << " to " << edit_last_position << endl;
            cerr << pb2json(e) << endl;
#endif

            if(edit_is_insertion(e) || edit_is_sub(e)) {
                // This edit introduces new sequence.
#ifdef debug_edit
                cerr << "Handling ins/sub relative to " << node_id << endl;
#endif
                // store the path representing this novel sequence in the translation table
                auto prev_position = edit_first_position;
                Path from_path;
                auto prev_from_mapping = from_path.add_mapping();
                *prev_from_mapping->mutable_position() = make_position(prev_position);
                auto from_edit = prev_from_mapping->add_edit();
                from_edit->set_sequence(e.sequence());
                from_edit->set_to_length(e.to_length());
                from_edit->set_from_length(e.from_length());
                // find the position after the edit
                // if the edit is not the last in a mapping, the position after is from_length of the edit after this
                pos_t next_position;
                if (j + 1 < m.edit_size()) {
                    next_position = prev_position;
                    get_offset(next_position) += e.from_length();
                    auto next_from_mapping = from_path.add_mapping();
                    *next_from_mapping->mutable_position() = make_position(next_position);
                } else { // implicitly (j + 1 == m.edit_size())
                    // if the edit is the last in a mapping, look at the next mapping position
                    if (i + 1 < path.mapping_size()) {
                        auto& next_mapping = path.mapping(i+1);
                        auto next_from_mapping = from_path.add_mapping();
                        *next_from_mapping->mutable_position() = next_mapping.position();
                    } else {
                        // if we are at the end of the path, then this insertion has no end, and we do nothing
                    }
                }
                // TODO what about forward into reverse????
                if (is_rev(prev_position)) {
                    from_path = simplify(
                        reverse_complement_path(from_path, [&](int64_t id) {
                                auto l = orig_node_sizes.find(id);
                                if (l == orig_node_sizes.end()) {
                                    // The node has no entry, so it must not have been broken
                                    return get_node(id)->sequence().size();
                                } else {
                                    return l->second;
                                }
                            }));
                }

                // Create the new nodes, reversing it if we are reversed
                vector<Node*> new_nodes;
                pos_t start_pos = make_pos_t(from_path.mapping(0).position());
                // We put in the reverse of our sdequence if we are an insert on
                // the revers of a node, to keep the graph pointing mostly the
                // same direction.
                auto fwd_seq = m.position().is_reverse() ?
                    reverse_complement(e.sequence())
                    : e.sequence();
                auto novel_edit_key = make_pair(start_pos, fwd_seq);
                auto added = added_seqs.find(novel_edit_key);
                if (added != added_seqs.end()) {
                    // if we have the node run already, don't make it again, just use the existing one
                    new_nodes = added->second;
#ifdef debug_edit
                    cerr << "Re-using already added nodes: ";
                    for (auto n : new_nodes) {
                        cerr << n->id() << " ";
                    }
                    cerr << endl;
#endif
                } else {
                    // Make a new run of nodes of up to max_node_size each
                    
                    // Make sure that we are trying to make a run of nodes of
                    // the length we're supposed to be.
                    assert(path_to_length(from_path) == fwd_seq.size());
                    
                    size_t cursor = 0;
                    while (cursor < fwd_seq.size()) {
                        // Until we used up all the sequence, make nodes
                        Node* new_node = create_node(fwd_seq.substr(cursor, max_node_size));
                        cursor += max_node_size;
                        
#ifdef debug_edit
                        cerr << "Create new node " << pb2json(*new_node) << endl;
#endif
                        if (!new_nodes.empty()) {
                            // Connect each to the previous node in the chain.
                            Edge* e = create_edge(new_nodes.back(), new_node);
#ifdef debug_edit
                            cerr << "Create edge " << pb2json(*e) << endl;
#endif
                        }
                        
                        // Remember the new node
                        new_nodes.push_back(new_node);
                        
                        // Chop the front of the from path off and associate it
                        // with this node. TODO: this is n^2 in number of nodes
                        // we add because we copy the whole path each time.
                        Path front_path;
                        
                        if (path_to_length(from_path) > new_node->sequence().size()) {
                            // There will still be path left, so we cut the path
                            tie(front_path, from_path) = cut_path(from_path, new_node->sequence().size());
                        } else {
                            // We consume the rest of the path. Don't bother cutting it.
                            swap(front_path, from_path);
                        }
                        
                        // The front bit of the path belongs to this new node
                        added_nodes[new_node] = front_path;
                        
                    }

                    // reverse the order of the nodes if we did a rev-comp
                    if (m.position().is_reverse()) {
                        std::reverse(new_nodes.begin(), new_nodes.end());
                    }

                    // TODO: fwd_seq can't be empty or problems will be happen
                    // because we'll have an empty vector of created nodes. I
                    // think the edit won't be an insert or sub if it is,
                    // though.

                    // Remember that this run belongs to this edit
                    added_seqs[novel_edit_key] = new_nodes;
                    
                }

                for (auto* new_node : new_nodes) {
                    // Add a mapping to each newly created node
                    Mapping& nm = *embedded.add_mapping();
                    nm.mutable_position()->set_node_id(new_node->id());
                    nm.mutable_position()->set_is_reverse(m.position().is_reverse());

                    // Don't set a rank; since we're going through the input
                    // path in order, the auto-generated ranks will put our
                    // newly created mappings in order.

                    Edit* e = nm.add_edit();
                    size_t l = new_node->sequence().size();
                    e->set_from_length(l);
                    e->set_to_length(l);
                }

                for (auto& dangler : dangling) {
                    // This actually referrs to a node.
                    
                    // Attach what was dangling to the early-in-the-alignment side of the newly created run.
                    auto to_attach = NodeSide(m.position().is_reverse() ? new_nodes.back()->id() : new_nodes.front()->id(),
                        m.position().is_reverse());
                    
#ifdef debug_edit
                    cerr << "Connecting " << dangler << " and " << to_attach << endl;
#endif
                    // Add an edge from the dangling NodeSide to the start of this new node
                    assert(create_edge(dangler, to_attach));

                }

                // Dangle the late-in-the-alignment end of this run of new nodes
                dangling.clear();
                dangling.insert(NodeSide(m.position().is_reverse() ? new_nodes.front()->id() : new_nodes.back()->id(),
                    !m.position().is_reverse()));

                // save edit into translated path

            } else if(edit_is_match(e)) {
                // We're using existing sequence

                // We know we have breakpoints on both sides, but we also might
                // have additional breakpoints in the middle. So we need the
                // left node, that contains the first base of the match, and the
                // right node, that contains the last base of the match.
                Node* left_node = find_new_node(edit_first_position);
                Node* right_node = find_new_node(edit_last_position);

                // TODO: we just assume the outer edges of these nodes are in
                // the right places. They should be if we cut the breakpoints
                // right.

                // get the set of new nodes that we map to
                // and use the lengths of each to create new mappings
                // and append them to the path we are including
                for (auto nm : create_new_mappings(edit_first_position,
                                                   edit_last_position,
                                                   m.position().is_reverse())) {

                    *embedded.add_mapping() = nm;

                    // Don't set a rank; since we're going through the input
                    // path in order, the auto-generated ranks will put our
                    // newly created mappings in order.
                }

#ifdef debug_edit
                cerr << "Handling match relative to " << node_id << endl;
#endif

                for (auto& dangler : dangling) {
#ifdef debug_edit
                    cerr << "Connecting " << dangler << " and " << NodeSide(left_node->id(), m.position().is_reverse()) << endl;
#endif

                    // Connect the left end of the left node we matched in the direction we matched it
                    assert(create_edge(dangler, NodeSide(left_node->id(), m.position().is_reverse())));
                }

                // Dangle the right end of the right node in the direction we matched it.
                if (right_node != nullptr) {
                    dangling.clear();
                    dangling.insert(NodeSide(right_node->id(), !m.position().is_reverse()));
                }
            } else {
                // We don't need to deal with deletions since we'll deal with the actual match/insert edits on either side
#ifdef debug_edit
                cerr << "Skipping other edit relative to " << node_id << endl;
#endif
            }

            // Advance in the right direction along the original node for this edit.
            // This way the next one will start at the right place.
            get_offset(edit_first_position) += e.from_length();

//#undef debug_edut
        }

    }
    
    // Actually return the embedded path.
    return embedded;

}

void VG::node_starts_in_path(const list<NodeTraversal>& path,
                             map<Node*, int>& node_start) {
    int i = 0;
    for (list<NodeTraversal>::const_iterator n = path.begin(); n != path.end(); ++n) {
        node_start[(*n).node] = i;
        int l = (*n).node->sequence().size();
        i += l;
    }
}

void VG::node_starts_in_path(list<NodeTraversal>& path,
                             map<NodeTraversal*, int>& node_start) {
    int i = 0;
    for (list<NodeTraversal>::iterator n = path.begin(); n != path.end(); ++n) {
        node_start[&(*n)] = i;
        int l = (*n).node->sequence().size();
        i += l;
    }
}

// todo record as an alignment rather than a string
Alignment VG::random_read(size_t read_len,
                          mt19937& rng,
                          id_t min_id,
                          id_t max_id,
                          bool either_strand) {
    // this is broken as it should be scaled by the sequence space
    // not node space
    // TODO BROKEN
    uniform_int_distribution<id_t> int64_dist(min_id, max_id);
    id_t id = int64_dist(rng);
    // We start at the node in its local forward orientation
    NodeTraversal node(get_node(id), false);
    int32_t start_pos = 0;
    if (node.node->sequence().size() > 1) {
        uniform_int_distribution<uint32_t> uint32_dist(0,node.node->sequence().size()-1);
        start_pos = uint32_dist(rng);
    }
    string read = node.node->sequence().substr(start_pos);
    Alignment aln;
    Path* path = aln.mutable_path();
    Mapping* mapping = path->add_mapping();
    Position* position = mapping->mutable_position();
    position->set_offset(start_pos);
    position->set_node_id(node.node->id());
    Edit* edit = mapping->add_edit();
    //edit->set_sequence(read);
    edit->set_from_length(read.size());
    edit->set_to_length(read.size());
    while (read.size() < read_len) {
        // pick a random downstream node
        vector<NodeTraversal> next_nodes;
        nodes_next(node, next_nodes);
        if (next_nodes.empty()) break;
        uniform_int_distribution<int> next_dist(0, next_nodes.size()-1);
        node = next_nodes.at(next_dist(rng));
        // Put in the node sequence in the correct relative orientation
        string addition = (node.backward
                           ? reverse_complement(node.node->sequence()) : node.node->sequence());
        read.append(addition);
        mapping = path->add_mapping();
        position = mapping->mutable_position();
        position->set_offset(0);
        position->set_node_id(node.node->id());
        edit = mapping->add_edit();
        //edit->set_sequence(addition);
        edit->set_from_length(addition.size());
        edit->set_to_length(addition.size());
    }
    aln.set_sequence(read);
    // fix up the length
    read = read.substr(0, read_len);
    size_t to_len = alignment_to_length(aln);
    if ((int)to_len - (int)read_len > 0) {
        aln = strip_from_end(aln, (int)to_len - (int)read_len);
    }
    uniform_int_distribution<int> binary_dist(0, 1);
    if (either_strand && binary_dist(rng) == 1) {
        // We can flip to the other strand (i.e. node's local reverse orientation).
        aln = reverse_complement_alignment(aln,
                                           (function<id_t(id_t)>) ([this](id_t id) {
                                                   return get_node(id)->sequence().size();
                                               }));
    }
    return aln;
}

bool VG::is_valid(bool check_nodes,
                  bool check_edges,
                  bool check_paths,
                  bool check_orphans) {

    if (check_nodes) {

        if (node_by_id.size() != graph.node_size()) {
            cerr << "graph invalid: node count is not equal to that found in node by-id index" << endl;
            return false;
        }

        for (int i = 0; i < graph.node_size(); ++i) {
            Node* n = graph.mutable_node(i);
            if (node_by_id.find(n->id()) == node_by_id.end()) {
                cerr << "graph invalid: node " << n->id() << " missing from by-id index" << endl;
                return false;
            }
        }
    }

    if (check_edges) {
        for (int i = 0; i < graph.edge_size(); ++i) {
            Edge* e = graph.mutable_edge(i);
            id_t f = e->from();
            id_t t = e->to();

            //cerr << "edge " << e << " " << e->from() << "->" << e->to() << endl;

            if (node_by_id.find(f) == node_by_id.end()) {
                cerr << "graph invalid: edge index=" << i
                     << " (" << f << "->" << t << ") cannot find node (from) " << f << endl;
                return false;
            }
            if (node_by_id.find(t) == node_by_id.end()) {
                cerr << "graph invalid: edge index=" << i
                     << " (" << f << "->" << t << ") cannot find node (to) " << t << endl;
                return false;
            }

            if (!edges_on_start.count(f) && !edges_on_end.count(f)) {
                // todo check if it's in the vector
                cerr << "graph invalid: edge index=" << i
                     << " could not find entry in either index for 'from' node " << f << endl;
                return false;
            }

            if (!edges_on_start.count(t) && !edges_on_end.count(t)) {
                // todo check if it's in the vector
                cerr << "graph invalid: edge index=" << i
                     << " could not find entry in either index for 'to' node " << t << endl;
                return false;
            }
        }

        for (pair<const id_t, vector<pair<id_t, bool>>>& start_and_edges : edges_on_start) {
            for (auto& edge_destination : start_and_edges.second) {
                // We're on the start, so we go to the end if we aren't a reversing edge
                Edge* e = get_edge(NodeSide::pair_from_start_edge(start_and_edges.first, edge_destination));
                if (!e) {
                    cerr << "graph invalid, edge is null" << endl;
                    return false;
                }
                if(start_and_edges.first != e->to() && start_and_edges.first != e->from()) {
                    // It needs to be attached to the node we looked up
                    cerr << "graph invalid: edge " << e->from() << "->" << e->to()
                         << " doesn't have start-indexed node in " << start_and_edges.first << "<->"
                         << edge_destination.first << endl;
                    return false;
                }
                if(edge_destination.first != e->to() && edge_destination.first != e->from()) {
                    // It also needs to be attached to the node it says it goes to
                    cerr << "graph invalid: edge " << e->from() << "->" << e->to()
                         << " doesn't have non-start-indexed node in " << start_and_edges.first << "<->"
                         << edge_destination.first << endl;
                    return false;
                }
                if(!((start_and_edges.first == e->to() && !e->to_end()) ||
                     (start_and_edges.first == e->from() && e->from_start()))) {

                    // The edge needs to actually attach to the start of the node we looked it up for.
                    // So at least one of its ends has to be to the start of the correct node.
                    // It may also be attached to the end.
                    cerr << "graph invalid: edge " << e->from() << "->" << e->to()
                         << " doesn't attach to start of " << start_and_edges.first << endl;
                    return false;
                }
                if (!has_node(e->from())) {
                    cerr << "graph invalid: edge from a non-existent node " << e->from() << "->" << e->to() << endl;
                    return false;
                }
                if (!has_node(e->to())) {
                    cerr << "graph invalid: edge to a non-existent node " << e->from() << "->" << e->to() << endl;
                    return false;
                }
            }
        }

        for (pair<const id_t, vector<pair<id_t, bool>>>& end_and_edges : edges_on_end) {
            for (auto& edge_destination : end_and_edges.second) {
                Edge* e = get_edge(NodeSide::pair_from_end_edge(end_and_edges.first, edge_destination));
                if (!e) {
                    cerr << "graph invalid, edge is null" << endl;
                    return false;
                }
                if(end_and_edges.first != e->to() && end_and_edges.first != e->from()) {
                    // It needs to be attached to the node we looked up
                    cerr << "graph invalid: edge " << e->from() << "->" << e->to()
                         << " doesn't have end-indexed node in " << end_and_edges.first << "<->"
                         << edge_destination.first << endl;
                    return false;
                }
                if(edge_destination.first != e->to() && edge_destination.first != e->from()) {
                    // It also needs to be attached to the node it says it goes to
                    cerr << "graph invalid: edge " << e->from() << "->" << e->to()
                         << " doesn't have non-end-indexed node in " << end_and_edges.first << "<->"
                         << edge_destination.first << endl;
                    return false;
                }
                if(!((end_and_edges.first == e->to() && e->to_end()) ||
                     (end_and_edges.first == e->from() && !e->from_start()))) {

                    // The edge needs to actually attach to the end of the node we looked it up for.
                    // So at least one of its ends has to be to the end of the correct node.
                    // It may also be attached to the start.
                    cerr << "graph invalid: edge " << e->from() << "->" << e->to()
                         << " doesn't attach to end of " << end_and_edges.first << endl;
                    return false;
                }
                if (!has_node(e->from())) {
                    cerr << "graph invalid: edge from a non-existent node " << e->from() << "->" << e->to() << endl;
                    return false;
                }
                if (!has_node(e->to())) {
                    cerr << "graph invalid: edge to a non-existent node " << e->from() << "->" << e->to() << endl;
                    return false;
                }
            }
        }
    }

    if (check_paths) {
        bool paths_ok = true;
        function<void(const Path&)> lambda =
            [this, &paths_ok]
            (const Path& path) {
            if (!paths_ok) {
                return;
            }
            if (path.mapping_size() == 1) {
                // handle the single-entry case
                if (!path.mapping(0).has_position()) {
                    cerr << "graph path " << path.name() << " has no position in mapping "
                         << pb2json(path.mapping(0)) << endl;
                    paths_ok = false;
                    return;
                }
            }

            for (size_t i = 1; i < path.mapping_size(); ++i) {
                auto& m1 = path.mapping(i-1);
                auto& m2 = path.mapping(i);
                if (!m1.has_position()) {
                    cerr << "graph path " << path.name() << " has no position in mapping "
                         << pb2json(m1) << endl;
                    paths_ok = false;
                    return;
                }
                if (!m2.has_position()) {
                    cerr << "graph path " << path.name() << " has no position in mapping "
                         << pb2json(m2) << endl;
                    paths_ok = false;
                    return;
                }
                if (!adjacent_mappings(m1, m2)) continue; // the path is completely represented here
                auto s1 = NodeSide(m1.position().node_id(), (m1.position().is_reverse() ? false : true));
                auto s2 = NodeSide(m2.position().node_id(), (m2.position().is_reverse() ? true : false));
                // check that we always have an edge between the two nodes in the correct direction
                if (!has_edge(s1, s2)) {
                    cerr << "graph path '" << path.name() << "' invalid: edge from "
                         << s1 << " to " << s2 << " does not exist" << endl;
                    paths_ok = false;
                    //return;;
                }

                // in the four cases below, we check that edges always incident the tips of nodes
                // when edit length, offsets and strand flipping of mappings are taken into account:

                // NOTE: Because of the !adjacent_mappings check above, mappings that are out of order
                //       will be ignored.  If they are invalid, it won't be caught.  Solution is
                //       to sort by rank, but I'm not sure if any of this is by design or not...

                auto& p1 = m1.position();
                auto& n1 = *get_node(p1.node_id());
                auto& p2 = m2.position();
                auto& n2 = *get_node(p2.node_id());
                // count up how many bases of the node m1 covers.
                id_t m1_edit_length = m1.edit_size() == 0 ? n1.sequence().length() : 0;
                for (size_t edit_idx = 0; edit_idx < m1.edit_size(); ++edit_idx) {
                    m1_edit_length += m1.edit(edit_idx).from_length();
                }

                // verify that m1 ends at offset length-1 for forward mapping
                if (p1.offset() + m1_edit_length != n1.sequence().length()) {
                    cerr << "graph path '" << path.name() << "' has invalid mapping " << pb2json(m1)
                    << ": offset (" << p1.offset() << ") + from_length (" << m1_edit_length << ")"
                    << " != node length (" << n1.sequence().length() << ")" << endl;
                    paths_ok = false;
                    return;
                }
                // verify that m2 starts at offset 0 for forward mapping
                if (p2.offset() > 0) {
                    cerr << "graph path '" << path.name() << "' has invalid mapping " << pb2json(m2)
                    << ": offset=" << p2.offset() << " found when offset=0 expected" << endl;
                    paths_ok = false;
                        return;
                }
            }

            // check that the mappings have the right length
            for (size_t i = 0; i < path.mapping_size(); ++i) {
                auto& m = path.mapping(i);
                // get the node
                auto n = get_node(m.position().node_id());
                if (mapping_from_length(m) + m.position().offset() > n->sequence().size()) {
                    cerr << "graph path " << path.name() << " has a mapping which "
                         << "matches sequence outside of the node it maps to "
                         << pb2json(m)
                         << " vs "
                         << pb2json(*n) << endl;
                    paths_ok = false;
                    return;
                }
            }

            // check that the mappings all match the graph
            /*
            for (size_t i = 0; i < path.mapping_size(); ++i) {
                auto& m = path.mapping(i);
                if (!mapping_is_total_match(m)) {
                    cerr << "graph path " << path.name() << " has an imperfect mapping "
                         << pb2json(m) << endl;
                    paths_ok = false;
                    return;
                }
            }
            */

        };
        paths.for_each(lambda);
        if (!paths_ok) return false;
    }

    return true;
}

void VG::to_dot(ostream& out,
                vector<Alignment> alignments,
                vector<Locus> loci,
                bool show_paths,
                bool walk_paths,
                bool annotate_paths,
                bool show_mappings,
                bool simple_mode,
                bool invert_edge_ports,
                bool color_variants,
                bool ultrabubble_labeling,
                bool skip_missing_nodes,
                bool ascii_labels,
                int random_seed) {

    // setup graphviz output
    out << "digraph graphname {" << endl;
    out << "    node [shape=plaintext];" << endl;
    out << "    rankdir=LR;" << endl;
    //out << "    fontsize=22;" << endl;
    //out << "    colorscheme=paired12;" << endl;
    //out << "    splines=line;" << endl;
    //out << "    splines=true;" << endl;
    //out << "    smoothType=spring;" << endl;

    //map<id_t, vector<
    map<id_t, set<pair<string, string>>> symbols_for_node;
    if (ultrabubble_labeling) {
        Pictographs picts(random_seed);
        Colors colors(random_seed);
        
        // Go get the snarls.
        SnarlManager snarl_manager = CactusSnarlFinder(*this).find_snarls();

        snarl_manager.for_each_snarl_preorder([&](const Snarl* snarl) {
            // For every snarl
            if (!snarl->type() == ULTRABUBBLE) {
                // Make sure it is an ultrabubble
                return;
            }
            
            // Get the deep contents of the snarl, which define it and which
            // need to be labeled with it.
            auto contents = snarl_manager.deep_contents(snarl, *this, true).first;
            
            // Serialize them
            stringstream vb;
            for (Node* node : contents) {
                vb << node->id() << ",";
            }
            auto repr = vb.str();
            
            // Compute a label for the bubble
            string emoji = ascii_labels ? picts.hashed_char(repr) : picts.hashed(repr);
            string color = colors.hashed(repr);
            auto label = make_pair(color, emoji);
            for (Node* node : contents) {
                symbols_for_node[node->id()].insert(label);
            }
        });
    }
    for (int i = 0; i < graph.node_size(); ++i) {
        Node* n = graph.mutable_node(i);
        auto node_paths = paths.of_node(n->id());

        stringstream inner_label;
        if (ultrabubble_labeling) {
            inner_label << "<TD ROWSPAN=\"3\" BORDER=\"2\" CELLPADDING=\"5\">";
            inner_label << "<FONT COLOR=\"black\">" << n->id() << ":" << n->sequence() << "</FONT> ";
            for(auto& string_and_color : symbols_for_node[n->id()]) {
                // Put every symbol in its font tag.
                inner_label << "<FONT COLOR=\"" << string_and_color.first << "\">" << string_and_color.second << "</FONT>";
            }
            inner_label << "</TD>";
        } else if (simple_mode) {
            //inner_label << "<TD ROWSPAN=\"3\" BORDER=\"2\" CELLPADDING=\"5\">";
            inner_label << n->id();
            //inner_label << "</TD>";
        } else {
            inner_label << "<TD ROWSPAN=\"3\" BORDER=\"2\" CELLPADDING=\"5\">";
            inner_label << n->id() << ":" << n->sequence();
            inner_label << "</TD>";
        }

        stringstream nlabel;
        if (simple_mode) {
            nlabel << inner_label.str();
        } else {
            nlabel << "<";
            nlabel << "<TABLE BORDER=\"0\" CELLPADDING=\"0\" CELLSPACING=\"0\"><TR><TD PORT=\"nw\"></TD><TD PORT=\"n\"></TD><TD PORT=\"ne\"></TD></TR><TR><TD></TD><TD></TD></TR><TR><TD></TD>";
            nlabel << inner_label.str();
            nlabel << "<TD></TD></TR><TR><TD></TD><TD></TD></TR><TR><TD PORT=\"sw\"></TD><TD PORT=\"s\"></TD><TD PORT=\"se\"></TD></TR></TABLE>";
            nlabel << ">";
        }

        if (simple_mode) {
            out << "    " << n->id() << " [label=\"" << nlabel.str() << "\",penwidth=2,shape=circle,";
        } else if (ultrabubble_labeling) {
            //out << "    " << n->id() << " [label=" << nlabel.str() << ",shape=box,penwidth=2,";
            out << "    " << n->id() << " [label=" << nlabel.str() << ",shape=none,width=0,height=0,margin=0,";
        } else {
            out << "    " << n->id() << " [label=" << nlabel.str() << ",shape=none,width=0,height=0,margin=0,";
        }

        // set pos for neato output, which tends to randomly order the graph
        if (!simple_mode) {
            if (is_head_node(n)) {
                out << "rank=min,";
                out << "pos=\"" << -graph.node_size()*100 << ", "<< -10 << "\",";
            } else if (is_tail_node(n)) {
                out << "rank=max,";
                out << "pos=\"" << graph.node_size()*100 << ", "<< -10 << "\",";
            }
        }
        if (color_variants && node_paths.size() == 0){
           out << "color=red,";
        }
        out << "];" << endl;
    }

    // We're going to fill this in with all the path (symbol, color) label
    // pairs that each edge should get, by edge pointer. If a path takes an
    // edge multiple times, it will appear only once.
    map<Edge*, set<pair<string, string>>> symbols_for_edge;

    if(annotate_paths) {
        // We're going to annotate the paths, so we need to give them symbols and colors.
        Pictographs picts(random_seed);
        Colors colors(random_seed);
        // Work out what path symbols belong on what edges
        function<void(const Path&)> lambda = [this, &picts, &colors, &symbols_for_edge, &ascii_labels](const Path& path) {
            // Make up the path's label
            string path_label = ascii_labels ? picts.hashed_char(path.name()) : picts.hashed(path.name());
            string color = colors.hashed(path.name());
            for (int i = 0; i < path.mapping_size(); ++i) {
                const Mapping& m1 = path.mapping(i);
                if (i < path.mapping_size()-1) {
                    const Mapping& m2 = path.mapping(i+1);
                    // skip if they are not contiguous
                    if (!adjacent_mappings(m1, m2)) continue;
                    // Find the Edge connecting the mappings in the order they occur in the path.
                    Edge* edge_used = get_edge(NodeTraversal(get_node(m1.position().node_id()), m1.position().is_reverse()),
                                               NodeTraversal(get_node(m2.position().node_id()), m2.position().is_reverse()));

                    // Say that edge should have this symbol
                    symbols_for_edge[edge_used].insert(make_pair(path_label, color));
                }
                if (path.is_circular()) {
                    // make a connection from tail to head
                    const Mapping& m1 = path.mapping(path.mapping_size()-1);
                    const Mapping& m2 = path.mapping(0);
                    // skip if they are not contiguous
                    //if (!adjacent_mappings(m1, m2)) continue;
                    // Find the Edge connecting the mappings in the order they occur in the path.
                    Edge* edge_used = get_edge(NodeTraversal(get_node(m1.position().node_id()), m1.position().is_reverse()),
                                               NodeTraversal(get_node(m2.position().node_id()), m2.position().is_reverse()));
                    // Say that edge should have this symbol
                    symbols_for_edge[edge_used].insert(make_pair(path_label, color));
                }
            }
        };
        paths.for_each(lambda);
    }

    id_t max_edge_id = 0;
    for (int i = 0; i < graph.edge_size(); ++i) {
        Edge* e = graph.mutable_edge(i);
        max_edge_id = max((id_t)max_edge_id, max((id_t)e->from(), (id_t)e->to()));
        auto from_paths = paths.of_node(e->from());
        auto to_paths = paths.of_node(e->to());
        set<string> both_paths;
        std::set_intersection(from_paths.begin(), from_paths.end(),
                              to_paths.begin(), to_paths.end(),
                              std::inserter(both_paths, both_paths.begin()));

        // Grab the annotation symbols for this edge.
        auto annotations = symbols_for_edge.find(e);

        // Is the edge in the "wrong" direction for rank constraints?
        bool is_backward = e->from_start() && e->to_end();

        if(is_backward) {
            // Flip the edge around and write it forward.
            Edge* original = e;
            e = new Edge();

            e->set_from(original->to());
            e->set_from_start(!original->to_end());

            e->set_to(original->from());
            e->set_to_end(!original->from_start());
        }

        // display what kind of edge we have using different edge head and tail styles
        // depending on if the edge comes from the start or not
        if (!simple_mode) {
            out << "    " << e->from() << " -> " << e->to();
            out << " [dir=both,";
            if ((!invert_edge_ports && e->from_start())
                || (invert_edge_ports && !e->from_start())) {
                out << "arrowtail=none,";
                out << "tailport=sw,";
            } else {
                out << "arrowtail=none,";
                out << "tailport=ne,";
            }
            if ((!invert_edge_ports && e->to_end())
                || (invert_edge_ports && !e->to_end())) {
                out << "arrowhead=none,";
                out << "headport=se,";
            } else {
                out << "arrowhead=none,";
                out << "headport=nw,";
            }
            out << "penwidth=2,";

            if(annotations != symbols_for_edge.end()) {
                // We need to put a label on the edge with all the colored
                // characters for paths using it.
                out << "label=<";

                for(auto& string_and_color : (*annotations).second) {
                    // Put every symbol in its font tag.
                    out << "<FONT COLOR=\"" << string_and_color.second << "\">" << string_and_color.first << "</FONT>";
                }

                out << ">";
            }
            out << "];" << endl;
        } else {
            out << "    " << e->from() << " -> " << e->to() << endl;
        }

        if(is_backward) {
            // We don't need this duplicate edge
            delete e;
        }
    }

    // add nodes for the alignments and link them to the nodes they match
    int alnid = max(max_node_id()+1, max_edge_id+1);
    for (auto& aln : alignments) {
        // check direction
        if (!aln.has_path()) continue; // skip pathless alignments
        alnid++;
        // make a node with some info about the alignment
        for (int i = 0; i < aln.path().mapping_size(); ++i) {
            const Mapping& m = aln.path().mapping(i);

            if(!has_node(m.position().node_id()) && skip_missing_nodes) {
                // We don't have the node this is aligned to. We probably are
                // looking at a subset graph, and the user asked us to skip it.
                continue;
            }

            //void mapping_cigar(const Mapping& mapping, vector<pair<int, char> >& cigar);
            //string cigar_string(vector<pair<int, char> >& cigar);
            //mapid << alnid << ":" << m.position().node_id() << ":" << cigar_string(cigar);
            //mapid << cigar_string(cigar) << ":"
            //      << (m.is_reverse() ? "-" : "+") << m.position().offset() << ":"
            string mstr;
            if (!simple_mode) {
                stringstream mapid;
                mapid << pb2json(m);
                mstr = mapid.str();
                mstr.erase(std::remove_if(mstr.begin(), mstr.end(),
                                          [](char c) { return c == '"'; }), mstr.end());
                mstr = wrap_text(mstr, 50);
            }
            // determine sequence of this portion of the alignment
            // set color based on cigar/mapping relationship
            // some mismatch, indicate with orange color
            string color;
            if (!simple_mode) {
                color = mapping_is_simple_match(m) ? "blue" : "orange";
            } else {
                color = "/rdylgn11/" + convert(round((1-divergence(m))*10)+1);
            }

            if (i == 0) {
                out << "    "
                    << alnid++ << " [label=\""
                    << aln.name()
                    << setprecision(5)
                    << "\n(" << aln.score() << " " << aln.mapping_quality() << " " << aln.identity() << ")"
                    << "\",fontcolor=\"black\",fontsize=10];" << endl;
                out << "    "
                    << alnid-1 << " -> "
                    << alnid << "[dir=none,color=\"gray\",style=\"dashed\",constraint=false];" << endl;
                out << "    "
                    << alnid-1 << " -> " << m.position().node_id()
                    << "[dir=none,style=invis];" << endl;
                out << "    { rank = same; " << alnid-1 << "; " << m.position().node_id() << "; };" << endl;
            }
            if (simple_mode) {
                out << "    "
                    << alnid << " [label=\""
                    << m.position().node_id() << "\"" << "shape=circle," //penwidth=2,"
                    << "style=filled,"
                    << "fillcolor=\"" << color << "\","
                    << "color=\"" << color << "\"];" << endl;

            } else {
                out << "    "
                    << alnid << " [label=\""
                    << mstr << "\",fontcolor=" << color << ",fontsize=10];" << endl;
            }
            if (i > 0) {
                out << "    "
                    << alnid-1 << " -> "
                    << alnid << "[dir=none,color=\"black\",constraint=false];" << endl;
            }
            out << "    "
                << alnid << " -> " << m.position().node_id()
                << "[dir=none,style=invis];" << endl;
            out << "    { rank = same; " << alnid << "; " << m.position().node_id() << "; };" << endl;
            //out << "    " << m.position().node_id() << " -- " << alnid << "[color=" << color << ", style=invis];" << endl;
            alnid++;
        }
        alnid++;
        // todo --- circular alignments
    }

    int locusid = alnid;
    {
        Pictographs picts(random_seed);
        Colors colors(random_seed);
        for (auto& locus : loci) {
            // get the paths of the alleles
            string path_label = ascii_labels ? picts.hashed_char(locus.name()) : picts.hashed(locus.name());
            string color = colors.hashed(locus.name());
            for (int j = 0; j < locus.allele_size(); ++j) {
                auto& path = locus.allele(j);
                for (int i = 0; i < path.mapping_size(); ++i) {
                    const Mapping& m = path.mapping(i);
                    stringstream mapid;
                    mapid << path_label << " " << m.position().node_id();
                    out << "    "
                        << locusid << " [label=\""
                        << mapid.str() << "\",fontcolor=\"" << color << "\",fontsize=10];" << endl;
                    if (i > 0) {
                        out << "    "
                            << locusid-1 << " -> "
                            << locusid << " [dir=none,color=\"" << color << "\",constraint=false];" << endl;
                    }
                    out << "    "
                        << locusid << " -> " << m.position().node_id()
                        << " [dir=none,style=invis];" << endl;
                    out << "    { rank = same; " << locusid << "; " << m.position().node_id() << "; };" << endl;
                    locusid++;
                }
            }
        }
    }

    // include paths
    if (show_paths || walk_paths) {
        int pathid = locusid;
        Pictographs picts(random_seed);
        Colors colors(random_seed);
        map<string, int> path_starts;
        function<void(const Path&)> lambda =
            [this,&pathid,&out,&picts,&colors,show_paths,walk_paths,show_mappings,&path_starts,&ascii_labels]
            (const Path& path) {
            string path_label = ascii_labels ? picts.hashed_char(path.name()) : picts.hashed(path.name());
            string color = colors.hashed(path.name());
            path_starts[path.name()] = pathid;
            if (show_paths) {
                for (int i = 0; i < path.mapping_size(); ++i) {
                    const Mapping& m = path.mapping(i);
                    stringstream mapid;
                    mapid << path_label << " " << m.position().node_id();
                    stringstream mappings;
                    if (show_mappings) {
                        mappings << pb2json(m);
                    }
                    string mstr = mappings.str();
                    mstr.erase(std::remove_if(mstr.begin(), mstr.end(), [](char c) { return c == '"'; }), mstr.end());
                    mstr = wrap_text(mstr, 50);

                    if (i == 0) { // add the path name at the start
                        out << "    " << pathid << " [label=\"" << path_label << " "
                            << path.name() << "  " << m.position().node_id() << " "
                            << mstr << "\",fontcolor=\"" << color << "\"];" << endl;
                    } else {
                        out << "    " << pathid << " [label=\"" << mapid.str() << " "
                            << mstr
                            << "\",fontcolor=\"" << color << "\"];" << endl;
                    }
                    if (i > 0 && adjacent_mappings(path.mapping(i-1), m)) {
                        out << "    " << pathid-1 << " -> " << pathid << " [dir=none,color=\"" << color << "\",constraint=false];" << endl;
                    }
                    out << "    " << pathid << " -> " << m.position().node_id()
                        << " [dir=none,color=\"" << color << "\", style=invis,constraint=false];" << endl;
                    out << "    { rank = same; " << pathid << "; " << m.position().node_id() << "; };" << endl;
                    pathid++;
                    // if we're at the end
                    // and the path is circular
                    if (path.is_circular() && i+1 == path.mapping_size()) {
                        // connect to the head of the path
                        out << "    " << pathid-1 << " -> " << path_starts[path.name()]
                            << " [dir=none,color=\"" << color << "\",constraint=false];" << endl;
                    }

                }
            }
            if (walk_paths) {
                for (int i = 0; i < path.mapping_size(); ++i) {
                    const Mapping& m1 = path.mapping(i);
                    if (i < path.mapping_size()-1) {
                        const Mapping& m2 = path.mapping(i+1);
                        out << m1.position().node_id() << " -> " << m2.position().node_id()
                            << " [dir=none,tailport=" << (m1.position().is_reverse() ? "nw" : "ne") 
                            << ",headport=" << (m2.position().is_reverse() ? "ne" : "nw") << ",color=\""
                            << color << "\",label=\"     " << path_label << "     \",fontcolor=\"" << color << "\",constraint=false];" << endl;
                    }
                }
                if (path.is_circular()) {
                    const Mapping& m1 = path.mapping(path.mapping_size()-1);
                    const Mapping& m2 = path.mapping(0);
                    out << m1.position().node_id() << " -> " << m2.position().node_id()
                    << " [dir=none,tailport=ne,headport=nw,color=\""
                    << color << "\",label=\"     " << path_label << "     \",fontcolor=\"" << color << "\",constraint=false];" << endl;
                }
            }
        };
        paths.for_each(lambda);
    }

    out << "}" << endl;

}


void VG::to_gfa(ostream& out) {
  GFAKluge gg;
  gg.set_version(1.0);

    // TODO moving to GFAKluge
    // problem: protobuf longs don't easily go to strings....
    for (int i = 0; i < graph.node_size(); ++i){
        sequence_elem s_elem;
        Node* n = graph.mutable_node(i);
        // Fill seq element for a node
        s_elem.name = to_string(n->id());
        s_elem.sequence = n->sequence();
        gg.add_sequence(s_elem);
    }
    
    auto& pathmap = this->paths._paths;
    for (auto p : pathmap){
        path_elem p_elem;
        p_elem.name = p.first;
        for (auto m : p.second){
            p_elem.segment_names.push_back( std::to_string(m.node_id()) );
            p_elem.orientations.push_back( !m.is_reverse() );
            Node* n = get_node( m.node_id() );
            stringstream cigaro;
            //cigaro << n->sequence().size() << (p.mapping(m_ind.position().is_reverse()) ? "M" : "M");
            cigaro << n->sequence().size() << (m.is_reverse() ? "M" : "M");
            p_elem.overlaps.push_back( cigaro.str() );
        }
        gg.add_path(p_elem.name, p_elem);
    }

    for (int i = 0; i < graph.edge_size(); ++i) {
        Edge* e = graph.mutable_edge(i);
        edge_elem ee;
        ee.type = 1;
        ee.source_name = to_string(e->from());
        ee.sink_name = to_string(e->to());
        ee.source_orientation_forward = ! e->from_start();
        ee.sink_orientation_forward =  ! e->to_end();
        ee.alignment = std::to_string(e->overlap()) + "M";
        gg.add_edge(ee.source_name, ee);
        //link_elem l;
        //l.source_name = to_string(e->from());
        //l.sink_name = to_string(e->to());
        //l.source_orientation_forward = ! e->from_start();
        //l.sink_orientation_forward =  ! e->to_end();
        //l.cigar = std::to_string(e->overlap()) + "M";
        //gg.add_link(l.source_name, l);
    }
    out << gg;
}

void VG::to_turtle(ostream& out, const string& rdf_base_uri, bool precompress) {

    out << "@base <http://example.org/vg/> . " << endl;
    if (precompress) {
       out << "@prefix : <" <<  rdf_base_uri <<"node/> . " << endl;
       out << "@prefix p: <" <<  rdf_base_uri <<"path/> . " << endl;
       out << "@prefix s: <" <<  rdf_base_uri <<"step/> . " << endl;
       out << "@prefix r: <http://www.w3.org/1999/02/22-rdf-syntax-ns#> . " << endl;

    } else {
       out << "@prefix node: <" <<  rdf_base_uri <<"node/> . " << endl;
       out << "@prefix path: <" <<  rdf_base_uri <<"path/> . " << endl;
       out << "@prefix step: <" <<  rdf_base_uri <<"step/> . " << endl;
       out << "@prefix rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#> . " << endl;
    }
    //Ensure that mappings are sorted by ranks
    paths.sort_by_mapping_rank();
    for (int i = 0; i < graph.node_size(); ++i) {
        Node* n = graph.mutable_node(i);
        if (precompress) {
            out << ":" << n->id() << " r:value \"" << n->sequence() << "\" . " ;
	    } else {
            out << "node:" << n->id() << " rdf:value \"" << n->sequence() << "\" . " << endl ;
        }
    }
    function<void(const string&)> url_encode = [&out]
        (const string& value) {
        out.fill('0');
        for (string::const_iterator i = value.begin(), n = value.end(); i != n; ++i) {
            string::value_type c = (*i);

            // Keep alphanumeric and other accepted characters intact
            if (c >= 0 && (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')) {
                out << c;
                continue;
            }
            // Any other characters are percent-encoded
            out << uppercase;
            out << hex;
            out << '%' << setw(2) << int((unsigned char) c);
            out << dec;
            out << nouppercase;
       }
    };
    function<void(const Path&)> lambda = [&out, &precompress, &url_encode]
        (const Path& path) {
            uint64_t offset=0; //We could have more than 2gigabases in a path
            for (auto &m : path.mapping()) {
                string orientation = m.position().is_reverse() ? "<reverseOfNode>" : "<node>";
                if (precompress) {
                	out << "s:";
                    url_encode(path.name());
                    out << "-" << m.rank() << " <rank> " << m.rank() << " ; " ;
                	out << orientation <<" :" << m.position().node_id() << " ;";
                    out << " <path> p:";
                    url_encode(path.name());
                    out << " ; ";
                    out << " <position> "<< offset<<" . ";
                } else {
                    out << "step:";
                    url_encode(path.name());
                    out << "-" << m.rank() << " <position> "<< offset<<" ; " << endl;
                	out << " a <Step> ;" << endl ;
                	out << " <rank> " << m.rank() << " ; "  << endl ;
                	out << " " << orientation <<" node:" << m.position().node_id() << " ; " << endl;
                	out << " <path> path:";
                    url_encode(path.name());
                    out  << " . " << endl;
                }
		        offset += mapping_to_length(m);
            }
        };
    paths.for_each(lambda);
    id_t prev = -1;
    for (int i = 0; i < graph.edge_size(); ++i) {
        Edge* e = graph.mutable_edge(i);
        if(precompress) {
            if (prev == -1){
    	        out << ":" << e->from();
            } else if (prev ==e->from()) {
                out << "; " ;
            } else {
                out << " . :" << e->from();
            }
            prev = e->from();
        } else {
            out << "node:" << e->from();
	    }

        if (e->from_start() && e->to_end()) {
            out << " <linksReverseToReverse> " ; // <--
        } else if (e->from_start() && !e->to_end()) {
            out << " <linksReverseToForward> " ; // -+
        } else if (e->to_end()) {
            out << " <linksForwardToReverse> " ; //+-
        } else {
            out << " <linksForwardToForward> " ; //++
        }
        if (precompress) {
             out << ":" << e->to();
        } else {
            out << "node:" << e->to() << " . " << endl;
	    }
    }
    if(precompress) {
        out << " .";
    }
}

void VG::connect_node_to_nodes(Node* node, vector<Node*>& nodes, bool from_start) {
    for (vector<Node*>::iterator n = nodes.begin(); n != nodes.end(); ++n) {
        // Connect them left to right, unless instructed otherwise
        create_edge(node, (*n), from_start, false);
    }
}

void VG::connect_nodes_to_node(vector<Node*>& nodes, Node* node, bool to_end) {
    for (vector<Node*>::iterator n = nodes.begin(); n != nodes.end(); ++n) {
        // Connect them left to right, unless instructed otherwise
        create_edge((*n), node, false, to_end);
    }
}

void VG::connect_node_to_nodes(NodeTraversal node, vector<NodeTraversal>& nodes) {
    for (vector<NodeTraversal>::iterator n = nodes.begin(); n != nodes.end(); ++n) {
        // Connect them left to right
        create_edge(node, (*n));
    }
}

void VG::connect_nodes_to_node(vector<NodeTraversal>& nodes, NodeTraversal node) {
    for (vector<NodeTraversal>::iterator n = nodes.begin(); n != nodes.end(); ++n) {
        // Connect them left to right
        create_edge((*n), node);
    }
}

// join all subgraphs together to a "null" head node
Node* VG::join_heads(void) {
    // Find the head nodes
    vector<Node*> heads;
    head_nodes(heads);

    // Then create the new node (so it isn't picked up as a head)
    current_id = max_node_id()+1;
    Node* root = create_node("N");

    // Wire it to all the heads and return
    connect_node_to_nodes(root, heads);
    return root;
}

void VG::join_heads(Node* node, bool from_start) {
    vector<Node*> heads;
    head_nodes(heads);

    // If the node we have been given shows up as a head, remove it.
    for(auto i = heads.begin(); i != heads.end(); ++i) {
        if(*i == node) {
            heads.erase(i);
            break;
        }
    }

    connect_node_to_nodes(node, heads, from_start);
}

void VG::join_tails(Node* node, bool to_end) {
    vector<Node*> tails;
    tail_nodes(tails);

    // If the node we have been given shows up as a tail, remove it.
    for(auto i = tails.begin(); i != tails.end(); ++i) {
        if(*i == node) {
            tails.erase(i);
            break;
        }
    }

    connect_nodes_to_node(tails, node, to_end);
}

void VG::add_start_end_markers(int length,
                               char start_char, char end_char,
                               Node*& start_node, Node*& end_node,
                               id_t& start_id, id_t& end_id) {

    //cerr << start_id << " " << end_id << endl;
    if (start_id == 0 || end_id == 0) {
        // get the max id
        id_t max_id = max_node_id();
        start_id = max_id + 1;
        end_id = start_id + 1;
    }
    // This set will hold all the nodes we haven't attached yet. But we don't
    // want it to hold the head_tail_node, so we fill it in now.
    unordered_set<Node*> unattached;
    for_each_node([&](Node* node) {
        unattached.insert(node);
    });

    // We handle the head and tail joining ourselves so we can do connected components.
    // We collect these before we add the new head/tail node so we don't have to filter it out later.
    vector<Node*> heads;
    head_nodes(heads);
    vector<Node*> tails;
    tail_nodes(tails);

    if(start_node == nullptr) {
        // We get to create the node. In its forward orientation it's the start node, so we use the start character.
        string start_string(length, start_char);
        if (start_id != 0) {
            start_node = create_node(start_string, start_id);
        } else {
            start_node = create_node(start_string);
        }
    } else {
        // We got a node to use
        add_node(*start_node);
    }

    if(end_node == nullptr) {
        // We get to create the node. In its forward orientation it's the end node, so we use the end character.
        string end_string(length, end_char);
        if (end_id != 0) {
            end_node = create_node(end_string, end_id);
        } else {
            end_node = create_node(end_string);
        }
    } else {
        // We got a node to use
        add_node(*end_node);
    }

#ifdef debug
    cerr << "Start node is " << start_node->id() << ", end node is " << end_node->id() << endl;
#endif

    for(Node* head : heads) {
        if(unattached.count(head)) {
            // This head is unconnected.

            // Mark everything it's attached to as attached
            for_each_connected_node(head, [&](Node* node) {
                unattached.erase(node);
            });
        }

        // Tie it to the start node
        create_edge(start_node, head);
#ifdef debug
    cerr << "Added edge " << start_node->id() << "->" << head->id() << endl;
#endif
    }

    for(Node* tail : tails) {
        if(unattached.count(tail)) {
            // This tail is unconnected.

            // Mark everything it's attached to as attached
            for_each_connected_node(tail, [&](Node* node) {
                unattached.erase(node);
            });
        }

        // Tie it to the end node
        create_edge(tail, end_node);
#ifdef debug
    cerr << "Added edge " << tail->id() << "->" << end_node->id() << endl;
#endif
    }

    // Find the connected components that aren't attached, if any.
    while(!unattached.empty()) {
        // Grab and attach some unattached node
        Node* to_attach = *(unattached.begin());

        // Mark everything it's attached to as attached
        for_each_connected_node(to_attach, [&](Node* node) {
            unattached.erase(node);
        });

        // Add the edge
        create_edge(start_node, to_attach);
#ifdef debug
        cerr << "Added cycle-breaking edge " << start_node->id() << "->" << to_attach->id() << endl;
#endif
        vector<Edge*> edges;
        edges_of_node(to_attach, edges);
        for (auto edge : edges) {
            //cerr << "edge of " << to_attach->id() << " " << edge->from() << " " << edge->to() << endl;
            if (edge->to() == to_attach->id() && edge->from() != start_node->id()) {
                //cerr << "creating edge" << endl;
                Edge* created = create_edge(edge->from(), end_node->id(), edge->from_start(), false);
#ifdef debug
                cerr << "Added edge " << pb2json(*created) << " in response to " << pb2json(*edge) << endl;
#endif
            }
        }
#ifdef debug
        cerr << "Broke into disconnected component at " << to_attach->id() << endl;
#endif
    }

    // Now we have no more disconnected stuff in our graph.

#ifdef debug
    cerr << "Start node edges: " << endl;
    vector<Edge*> edges;
    edges_of_node(start_node, edges);
    for(auto e : edges) {
        std::cerr << pb2json(*e) << std::endl;
    }

    cerr << "End node edges: " << endl;
    edges.clear();
    edges_of_node(end_node, edges);
    for(auto e : edges) {
        std::cerr << pb2json(*e) << std::endl;
    }

#endif

    // now record the head and tail nodes in our path index
    // this is used during kpath traversals
    paths.head_tail_nodes.insert(start_node->id());
    paths.head_tail_nodes.insert(end_node->id());

}

unordered_map<id_t, pair<id_t, bool> > VG::overlay_node_translations(const unordered_map<id_t, pair<id_t, bool> >& over,
                                                                     const unordered_map<id_t, pair<id_t, bool> >& under) {
    unordered_map<id_t, pair<id_t, bool> > overlay = under;
    // for each over, check if we should map to the under
    // if so, adjust
    for (auto& o : over) {
        id_t new_id = o.first;
        id_t old_id = o.second.first;
        bool is_rev = o.second.second;
        auto u = under.find(old_id);
        if (u != under.end()) {
            id_t oldest_id = u->second.first;
            bool was_rev = u->second.second;
            overlay[new_id] = make_pair(oldest_id,
                                        is_rev ^ was_rev);
            /*
            cerr << "double trans "
                 << new_id << " -> " << old_id
                 << " -> " << oldest_id << endl;
            */
        } else {
            overlay[o.first] = o.second;
        }
    }
    /*
    for (auto& o : overlay) {
        cerr << o.first << " -> " << o.second.first
             << (o.second.second?"-":"+") << endl;
    }
    */
    return overlay;
}

Alignment VG::align(const Alignment& alignment,
                    Aligner* aligner,
                    QualAdjAligner* qual_adj_aligner,
                    bool traceback,
                    bool acyclic_and_sorted,
                    size_t max_query_graph_ratio,
                    bool pinned_alignment,
                    bool pin_left,
                    bool banded_global,
                    size_t band_padding_override,
                    size_t max_span,
                    size_t unroll_length,
                    bool print_score_matrices) {

    auto aln = alignment;

    // empty graph means unaligned
    if (this->size() == 0) {
        // unaligned
        aln.set_score(0);
        aln.clear_path();
        return aln;
    }

#ifdef debug
    cerr << "aligning read of " << alignment.sequence().size() << " to graph of " << length() << endl;
    //cerr << pinned_alignment << " " << pin_left << " " << " " << banded_global << " " << band_padding_override << " "  << max_span << endl;
#endif

    auto do_align = [&](Graph& g) {
#ifdef debug
        write_alignment_to_file(alignment, hash_alignment(alignment) + ".gam");
        serialize_to_file(hash_alignment(alignment) + ".vg");
#endif
        if (aligner && qual_adj_aligner) {
            cerr << "error:[VG] cannot both adjust and not adjust alignment for base quality" << endl;
            exit(1);
        }
        if (banded_global) {
            // Figure out if we should use permissive banding, or a fixed band padding
            bool permissive_banding = (band_padding_override == 0);
            // What band padding do we want? We used to hardcode it as 1, so it should always be at least 1.
            size_t band_padding = permissive_banding ? max(max_span, (size_t) 1) : band_padding_override;
#ifdef debug
            cerr << "Actual graph size: ";
            size_t total_size = 0;
            for(size_t i = 0; i < g.node_size(); i++) {
                total_size += g.node(i).sequence().size();
            }
            cerr << total_size << endl;
#endif
            if (aligner && !qual_adj_aligner) {
                aligner->align_global_banded(aln, g, band_padding, permissive_banding);
            } else if (qual_adj_aligner && !aligner) {
                qual_adj_aligner->align_global_banded(aln, g, band_padding, permissive_banding);
            }
        } else if (pinned_alignment) {
            if (aligner && !qual_adj_aligner) {
                aligner->align_pinned(aln, g, pin_left);
            } else if (qual_adj_aligner && !aligner) {
                qual_adj_aligner->align_pinned(aln, g, pin_left);
            }
        } else {
            if (aligner && !qual_adj_aligner) {
                aligner->align(aln, g, traceback, print_score_matrices);
            } else if (qual_adj_aligner && !aligner) {
                qual_adj_aligner->align(aln, g, traceback, print_score_matrices);
            }
        }
    };

    flip_doubly_reversed_edges();

    if (acyclic_and_sorted) {
        // graph is a non-inverting DAG, so we just need to sort
#ifdef debug
        cerr << "Graph is a non-inverting DAG, so just sort and align" << endl;
#endif
        // run the alignment
        do_align(this->graph);
    } else {
#ifdef debug
        cerr << "Graph is complex, so dagify and unfold before alignment" << endl;
#endif
        
        unordered_map<id_t, pair<id_t, bool> > unfold_trans;
        unordered_map<id_t, pair<id_t, bool> > dagify_trans;
        // Work out how long we could possibly span with an alignment.
        // TODO: we probably want to be able to span more than just the sequence
        // length if we don't get a hint. Look at scores and guess the max span
        // with those scores?
        unroll_length = (unroll_length == 0 ? aln.sequence().size() : unroll_length);
        size_t component_length_max = 100*unroll_length; // hard coded to be 100x

        // dagify the graph by unfolding inversions and then applying dagify forward unroll
        VG dag = unfold(unroll_length, unfold_trans)
            .dagify(unroll_length, dagify_trans, unroll_length, component_length_max);

        // overlay the translations
        auto trans = overlay_node_translations(dagify_trans, unfold_trans);

        // Join to a common root, so alignment covers the entire graph
        // Put the nodes in sort order within the graph
        // and break any remaining cycles
        algorithms::sort(&dag);
        
        // run the alignment
        do_align(dag.graph);

#ifdef debug
        auto check_aln = [&](VG& graph, const Alignment& a) {
            if (a.has_path()) {
                auto seq = graph.path_string(a.path());
                //if (aln.sequence().find('N') == string::npos && seq != aln.sequence()) {
                if (seq != a.sequence()) {
                    cerr << "alignment does not match graph " << endl
                         << pb2json(a) << endl
                         << "expect:\t" << a.sequence() << endl
                         << "got:\t" << seq << endl;
                    write_alignment_to_file(a, "fail.gam");
                    graph.serialize_to_file("fail.vg");
                    assert(false);
                }
            }
        };
        check_aln(dag, aln);
#endif
        
        translate_nodes(aln, trans, [&](id_t node_id) {
                // We need to feed in the lengths of nodes, so the offsets in the alignment can be updated.
                return get_node(node_id)->sequence().size();
            });
#ifdef debug
        check_aln(*this, aln);
#endif

    }

    // Copy back the not-case-corrected sequence
    aln.set_sequence(alignment.sequence());

    return aln;
}

Alignment VG::align(const Alignment& alignment,
                    Aligner* aligner,
                    bool traceback,
                    bool acyclic_and_sorted,
                    size_t max_query_graph_ratio,
                    bool pinned_alignment,
                    bool pin_left,
                    bool banded_global,
                    size_t band_padding_override,
                    size_t max_span,
                    size_t unroll_length,
                    bool print_score_matrices) {
    return align(alignment, aligner, nullptr, traceback, acyclic_and_sorted, max_query_graph_ratio,
                 pinned_alignment, pin_left, banded_global, band_padding_override,
                 max_span, unroll_length, print_score_matrices);
}

Alignment VG::align(const string& sequence,
                    Aligner* aligner,
                    bool traceback,
                    bool acyclic_and_sorted,
                    size_t max_query_graph_ratio,
                    bool pinned_alignment,
                    bool pin_left,
                    bool banded_global,
                    size_t band_padding_override,
                    size_t max_span,
                    size_t unroll_length,
                    bool print_score_matrices) {
    Alignment alignment;
    alignment.set_sequence(sequence);
    return align(alignment, aligner, traceback, acyclic_and_sorted, max_query_graph_ratio,
                 pinned_alignment, pin_left, banded_global, band_padding_override,
                 max_span, unroll_length, print_score_matrices);
}

Alignment VG::align(const Alignment& alignment,
                    bool traceback,
                    bool acyclic_and_sorted,
                    size_t max_query_graph_ratio,
                    bool pinned_alignment,
                    bool pin_left,
                    bool banded_global,
                    size_t band_padding_override,
                    size_t max_span,
                    size_t unroll_length,
                    bool print_score_matrices) {
    Aligner default_aligner = Aligner();
    return align(alignment, &default_aligner, traceback, acyclic_and_sorted, max_query_graph_ratio,
                 pinned_alignment, pin_left, banded_global, band_padding_override,
                 max_span, unroll_length, print_score_matrices);
}

Alignment VG::align(const string& sequence,
                    bool traceback,
                    bool acyclic_and_sorted,
                    size_t max_query_graph_ratio,
                    bool pinned_alignment,
                    bool pin_left,
                    bool banded_global,
                    size_t band_padding_override,
                    size_t max_span,
                    size_t unroll_length,
                    bool print_score_matrices) {
    Alignment alignment;
    alignment.set_sequence(sequence);
    return align(alignment, traceback, acyclic_and_sorted, max_query_graph_ratio,
                 pinned_alignment, pin_left, banded_global, band_padding_override,
                 max_span, unroll_length, print_score_matrices);
}

Alignment VG::align_qual_adjusted(const Alignment& alignment,
                                  QualAdjAligner* qual_adj_aligner,
                                  bool traceback,
                                  bool acyclic_and_sorted,
                                  size_t max_query_graph_ratio,
                                  bool pinned_alignment,
                                  bool pin_left,
                                  bool banded_global,
                                  size_t band_padding_override,
                                  size_t max_span,
                                  size_t unroll_length,
                                  bool print_score_matrices) {
    return align(alignment, nullptr, qual_adj_aligner, traceback, acyclic_and_sorted, max_query_graph_ratio,
                 pinned_alignment, pin_left, banded_global, band_padding_override,
                 max_span, unroll_length, print_score_matrices);
}

Alignment VG::align_qual_adjusted(const string& sequence,
                                  QualAdjAligner* qual_adj_aligner,
                                  bool traceback,
                                  bool acyclic_and_sorted,
                                  size_t max_query_graph_ratio,
                                  bool pinned_alignment,
                                  bool pin_left,
                                  bool banded_global,
                                  size_t band_padding_override,
                                  size_t max_span,
                                  size_t unroll_length,
                                  bool print_score_matrices) {
    Alignment alignment;
    alignment.set_sequence(sequence);
    return align_qual_adjusted(alignment, qual_adj_aligner, traceback, acyclic_and_sorted, max_query_graph_ratio,
                               pinned_alignment, pin_left, banded_global, band_padding_override,
                               max_span, unroll_length, print_score_matrices);
}

const string VG::hash(void) {
    stringstream s;
    serialize_to_ostream(s);
    return sha1sum(s.str());
}


int VG::path_edge_count(list<NodeTraversal>& path, int32_t offset, int path_length) {
    int edge_count = 0;
    // starting from offset in the first node
    // how many edges do we cross?

    // This is the remaining path length
    int l = path_length;

    // This is the next node we are looking at.
    list<NodeTraversal>::iterator pitr = path.begin();

    // How many bases of the first node can we use?
    int available_in_first_node = (*pitr).node->sequence().size() - offset;

    if(available_in_first_node >= l) {
        // Cross no edges
        return 0;
    }

    l -= available_in_first_node;
    pitr++;
    while (l > 0) {
        // Now we can ignore node orientation
        ++edge_count;
        l -= (*pitr++).node->sequence().size();
    }
    return edge_count;
}

int VG::path_end_node_offset(list<NodeTraversal>& path, int32_t offset, int path_length) {
    // This is the remaining path length
    int l = path_length;

    // This is the next node we are looking at.
    list<NodeTraversal>::iterator pitr = path.begin();

    // How many bases of the first node can we use?
    int available_in_first_node = (*pitr).node->sequence().size() - offset;

    if(available_in_first_node >= l) {
        // Cross no edges
        return available_in_first_node - l;
    }

    l -= available_in_first_node;
    pitr++;
    while (l > 0) {
        l -= (*pitr++).node->sequence().size();
    }
    // Now back out the last node we just took.
    l += (*--pitr).node->sequence().size();

    // Measure form the far end of the last node.
    l = (*pitr).node->sequence().size() - l - 1;

    return l;
}

const vector<Alignment> VG::paths_as_alignments(void) {
    vector<Alignment> alns;
    paths.for_each([this,&alns](const Path& path) {
            alns.emplace_back();
            auto& aln = alns.back();
            *aln.mutable_path() = path; // copy the path
            // now reconstruct the sequence
            aln.set_sequence(this->path_sequence(path));
            aln.set_name(path.name());
        });
    return alns;
}

const string VG::path_sequence(const Path& path) {
    string seq;
    for (size_t i = 0; i < path.mapping_size(); ++i) {
        auto& m = path.mapping(i);
        seq.append(mapping_sequence(m, *get_node(m.position().node_id())));
    }
    return seq;
}

double VG::path_identity(const Path& path1, const Path& path2) {
    // convert paths to sequences
    string seq1 = path_sequence(path1);
    string seq2 = path_sequence(path2);
    // align the two path sequences with ssw
    SSWAligner aligner;
    Alignment aln = aligner.align(seq1, seq2);
    // compute best possible score (which is everything matches)
    int max_len = max(seq1.length(), seq2.length());
    int best_score = max_len * aligner.match;
    // return fraction of score over best_score
    return best_score == 0 ? 0 : (double)aln.score() / (double)best_score;
}

void VG::prune_complex_with_head_tail(int path_length, int edge_max) {
    Node* head_node = NULL;
    Node* tail_node = NULL;
    id_t head_id = 0, tail_id = 0;
    add_start_end_markers(path_length, '#', '$', head_node, tail_node, head_id, tail_id);
    prune_complex(path_length, edge_max, head_node, tail_node);
    destroy_node(head_node);
    destroy_node(tail_node);
}

void VG::prune_complex(int path_length, int edge_max, Node* head_node, Node* tail_node) {

    vector<edge_t> to_destroy = find_edges_to_prune(*this, path_length, edge_max);
    for (auto& e : to_destroy) {
        destroy_edge(e.first, e.second);
    }

    for (auto* n : head_nodes()) {
        if (n != head_node) {
            // Fix up multiple heads with a left-to-right edge
            create_edge(head_node, n);
        }
    }
    for (auto* n : tail_nodes()) {
        if (n != tail_node) {
            // Fix up multiple tails with a left-to-right edge
            create_edge(n, tail_node);
        }
    }
}

void VG::prune_short_subgraphs(size_t min_size) {

    // Find the head nodes.
    std::vector<vg::id_t> heads;
    this->for_each_node([&](Node* node) {
        if (this->is_head_node(node)) {
            heads.push_back(node->id());
        }
    });

    for (vg::id_t head : heads) {
        if (!this->has_node(head)) {
            continue;   // Already pruned.
        }

        // Explore the neighborhood until the component is too large.
        Node* head_node = this->get_node(head);
        size_t subgraph_size = head_node->sequence().size();
        std::stack<Node*> to_check; to_check.push(head_node);
        std::unordered_set<vg::id_t> subgraph { head };
        while(subgraph_size < min_size && !to_check.empty()) {
            Node* curr = to_check.top(); to_check.pop();
            std::vector<Edge*> edges = this->edges_of(curr);
            for (Edge* edge : edges) {
                Node* next = this->get_node(edge->from() == curr->id() ? edge->to() : edge->from());
                if (subgraph.find(next->id()) == subgraph.end()) {
                    subgraph_size += next->sequence().size();
                    subgraph.insert(next->id());
                    to_check.push(next);
                }
            }
        }

        // Destroy the component if it was small enough.
        if (subgraph_size < min_size) {
            for (vg::id_t node : subgraph) {
                this->destroy_node(node);
            }
        }
    }
}

/*
// todo
void VG::prune_complex_subgraphs(size_t ) {

}
*/

void VG::collect_subgraph(Node* start_node, set<Node*>& subgraph) {

    // add node to subgraph
    subgraph.insert(start_node);

    set<Node*> checked;
    set<Node*> to_check;
    to_check.insert(start_node);

    while (!to_check.empty()) {
        // for each predecessor of node
        set<Node*> curr_check = to_check;
        to_check.clear();
        for (auto* node : curr_check) {
            if (checked.count(node)) {
                continue;
            } else {
                checked.insert(node);
            }
            vector<NodeTraversal> prev;
            nodes_prev(NodeTraversal(node), prev);
            for (vector<NodeTraversal>::iterator p = prev.begin(); p != prev.end(); ++p) {
            // if it's not already been examined, collect its neighborhood
                if (!subgraph.count((*p).node)) {
                    subgraph.insert((*p).node);
                    to_check.insert((*p).node);
                }
            }
            // for each successor of node
            vector<NodeTraversal> next;
            nodes_next(NodeTraversal(node), next);
            for (vector<NodeTraversal>::iterator n = next.begin(); n != next.end(); ++n) {
                if (!subgraph.count((*n).node)) {
                    subgraph.insert((*n).node);
                    to_check.insert((*n).node);
                }
            }
        }
    }
    //cerr << "node " << start_node->id() << " subgraph size " << subgraph.size() << endl;
}

void VG::disjoint_subgraphs(list<VG>& subgraphs) {
    vector<Node*> heads;
    head_nodes(heads);
    map<Node*, set<Node*> > subgraph_by_head;
    map<Node*, set<Node*>* > subgraph_membership;
    // start at the heads, but keep in mind that we need to explore fully
    for (vector<Node*>::iterator h = heads.begin(); h != heads.end(); ++h) {
        if (subgraph_membership.find(*h) == subgraph_membership.end()) {
            set<Node*>& subgraph = subgraph_by_head[*h];
            collect_subgraph(*h, subgraph);
            for (set<Node*>::iterator n = subgraph.begin(); n != subgraph.end(); ++n) {
                subgraph_membership[*n] = &subgraph;
            }
        }
    }
    for (map<Node*, set<Node*> >::iterator g = subgraph_by_head.begin();
         g != subgraph_by_head.end(); ++ g) {
        set<Node*>& nodes = g->second;
        set<Edge*> edges;
        edges_of_nodes(nodes, edges);
        subgraphs.push_back(VG(nodes, edges));
    }
}

bool VG::is_head_node(id_t id) {
    return is_head_node(get_node(id));
}

bool VG::is_head_node(Node* node) {
    return start_degree(node) == 0;
}

void VG::head_nodes(vector<Node*>& nodes) {
    for (int i = 0; i < graph.node_size(); ++i) {
        Node* n = graph.mutable_node(i);
        if (is_head_node(n)) {
            nodes.push_back(n);
        }
    }
}

vector<Node*> VG::head_nodes(void) {
    vector<Node*> heads;
    head_nodes(heads);
    return heads;
}



bool VG::is_tail_node(id_t id) {
    return is_tail_node(get_node(id));
}

bool VG::is_tail_node(Node* node) {
    return end_degree(node) == 0;
}

void VG::tail_nodes(vector<Node*>& nodes) {
    for (int i = 0; i < graph.node_size(); ++i) {
        Node* n = graph.mutable_node(i);
        if (is_tail_node(n)) {
            nodes.push_back(n);
        }
    }
}

vector<Node*> VG::tail_nodes(void) {
    vector<Node*> tails;
    tail_nodes(tails);
    return tails;
}

void VG::wrap_with_null_nodes(void) {
    vector<Node*> heads;
    head_nodes(heads);
    Node* head = create_node("");
    for (vector<Node*>::iterator h = heads.begin(); h != heads.end(); ++h) {
        create_edge(head, *h);
    }

    vector<Node*> tails;
    tail_nodes(tails);
    Node* tail = create_node("");
    for (vector<Node*>::iterator t = tails.begin(); t != tails.end(); ++t) {
        create_edge(*t, tail);
    }
}

VG VG::split_strands(unordered_map<id_t, pair<id_t, bool> >& node_translation) {
    
    VG split;
    
    split.current_id = 1;
    
    unordered_map<id_t, id_t> forward_node;
    unordered_map<id_t, id_t> reverse_node;
    
    for (int64_t i = 0; i < graph.node_size(); i++) {
        const Node& node = graph.node(i);
        Node* fwd_node = split.graph.add_node();
        fwd_node->set_sequence(node.sequence());
        fwd_node->set_id(split.current_id);
        split.current_id++;
        
        Node* rev_node = split.graph.add_node();
        rev_node->set_sequence(reverse_complement(node.sequence()));
        rev_node->set_id(split.current_id);
        split.current_id++;
        
        forward_node[node.id()] = fwd_node->id();
        reverse_node[node.id()] = rev_node->id();
        
        node_translation[fwd_node->id()] = make_pair(node.id(), false);
        node_translation[rev_node->id()] = make_pair(node.id(), true);
    }
    
    for (int64_t i = 0; i < graph.edge_size(); i++) {
        const Edge& edge = graph.edge(i);
        if (!edge.from_start() && !edge.to_end()) {
            Edge* fwd_edge = split.graph.add_edge();
            fwd_edge->set_from(forward_node[edge.from()]);
            fwd_edge->set_to(forward_node[edge.to()]);
            
            Edge* rev_edge = split.graph.add_edge();
            rev_edge->set_from(reverse_node[edge.to()]);
            rev_edge->set_to(reverse_node[edge.from()]);
        }
        else if (edge.from_start() && edge.to_end()) {
            Edge* fwd_edge = split.graph.add_edge();
            fwd_edge->set_from(reverse_node[edge.from()]);
            fwd_edge->set_to(reverse_node[edge.to()]);
            
            Edge* rev_edge = split.graph.add_edge();
            rev_edge->set_from(forward_node[edge.to()]);
            rev_edge->set_to(forward_node[edge.from()]);
        }
        else if (edge.from_start()) {
            Edge* fwd_edge = split.graph.add_edge();
            fwd_edge->set_from(reverse_node[edge.from()]);
            fwd_edge->set_to(forward_node[edge.to()]);
            
            Edge* rev_edge = split.graph.add_edge();
            rev_edge->set_from(reverse_node[edge.to()]);
            rev_edge->set_to(forward_node[edge.from()]);
        }
        else {
            Edge* fwd_edge = split.graph.add_edge();
            fwd_edge->set_from(forward_node[edge.from()]);
            fwd_edge->set_to(reverse_node[edge.to()]);
            
            Edge* rev_edge = split.graph.add_edge();
            rev_edge->set_from(forward_node[edge.to()]);
            rev_edge->set_to(reverse_node[edge.from()]);
        }
    }
    
    split.build_indexes();
    
    return split;
}

VG VG::unfold(uint32_t max_length,
              unordered_map<id_t, pair<id_t, bool> >& node_translation) {
    
    // graph we will build
    VG unfolded;
    
    // records the induced forward orientation of each node
    unordered_map<id_t, pair<id_t, bool>> main_orientation;
    // edges we have traversed in the forward direction
    unordered_set<Edge*> forward_edges;
    // edges that we find in the traversal that flip onto the reverse strand
    unordered_set<pair<NodeTraversal, NodeTraversal>> reversing_edges;
    
    // initially traverse the entire graph with DFS to induce an orientation
    for (int64_t i = 0; i < graph.node_size(); i++) {
        // iterate along starting nodes in case there are disconnected components
        
        Node* node = graph.mutable_node(i);
        if (main_orientation.count(node->id())) {
            continue;
        }
        
        // let this node greedily induce an orientation on the entire component
        Node* inducing_node = unfolded.create_node(node->sequence());
        main_orientation[node->id()] = make_pair(inducing_node->id(), false);
        
#ifdef debug
        cerr << "[unfold] seeding orientation node " << inducing_node->id() << " fwd" << endl;
#endif
        
        // DFS
        list<NodeTraversal> stack;
        stack.push_back(NodeTraversal(node, false));
        while (!stack.empty()) {
            NodeTraversal trav = stack.back();
            stack.pop_back();
            
#ifdef debug
            cerr << "[unfold] traversing " << trav << endl;
#endif
            
            auto oriented_trav = main_orientation[trav.node->id()];
            
            // check in the forward direction from this node
            for (NodeTraversal next : travs_from(trav)) {
                if (main_orientation.count(next.node->id())) {
                    // we have seen this node before
                    auto oriented_next = main_orientation[next.node->id()];
                    Edge* trav_edge = get_edge(trav, next);
                    
                    if (next.backward != oriented_next.second) {
                        // we've encountered this node in the opposite direction, so the edge
                        // must have reversed the traversal relative to the induce orientation
                        reversing_edges.emplace(trav, next);
#ifdef debug
                        cerr << "[unfold] new reversing edge " << trav << " -> " << next << endl;
#endif
                    }
                    else if (!forward_edges.count(trav_edge)) {
                        // we've never traversed this edge to this node, so add the edge and move on
                        forward_edges.insert(trav_edge);
                        unfolded.create_edge(oriented_trav.first, oriented_next.first);
                    }
                }
                else {
#ifdef debug
                    cerr << "[unfold] orienting node " << next << endl;
#endif
                    // we've never seen this node before, so we induce the current orientation on it
                    Node* new_node = unfolded.create_node(next.backward ? reverse_complement(next.node->sequence())
                                                          : next.node->sequence());
                    main_orientation[next.node->id()] = make_pair(new_node->id(), next.backward);
                    
                    // also copy the edge
                    forward_edges.insert(get_edge(trav, next));
                    unfolded.create_edge(oriented_trav.first, new_node->id());
                    
                    // add to stack to continue traversal
                    stack.push_back(next);
                }
            }
            
            // check in the reverse direction from this node
            for (NodeTraversal prev : travs_to(trav)) {
                if (main_orientation.count(prev.node->id())) {
                    // we have seen this node before
                    auto oriented_prev = main_orientation[prev.node->id()];
                    Edge* trav_edge = get_edge(prev, trav);
                    
                    if (prev.backward != oriented_prev.second) {
                        // we've encountered this node in the opposite direction, so the edge
                        // must have reversed the traversal relative to the induce orientation
                        reversing_edges.emplace(trav.reverse(), prev.reverse());
#ifdef debug
                        cerr << "[unfold] new reversing edge " << trav.reverse() << " -> " << prev.reverse() << endl;
#endif
                    }
                    else if (!forward_edges.count(trav_edge)) {
                        // we've never traversed this edge to this node, so add the edge and move on
                        forward_edges.insert(trav_edge);
                        unfolded.create_edge(oriented_prev.first, oriented_trav.first);
                    }
                }
                else {
#ifdef debug
                    cerr << "[unfold] orienting node " << prev << endl;
#endif
                    // we've never seen this node before, so we induce the current orientation on it
                    Node* new_node = unfolded.create_node(prev.backward ? reverse_complement(prev.node->sequence())
                                                          : prev.node->sequence());
                    
                    main_orientation[prev.node->id()] = make_pair(new_node->id(), prev.backward);
                    
                    // also copy the edge
                    forward_edges.insert(get_edge(prev, trav));
                    unfolded.create_edge(new_node->id(), oriented_trav.first);
                    
                    // add to stack to continue traversal
                    stack.push_back(prev);
                }
            }
        }
    }
    
    // as an edge case, skip traversing the reverse strand if the search length is 0
    if (!max_length) {
        return unfolded;
    }
    
    struct DistTraversal {
        DistTraversal(NodeTraversal trav, int64_t dist) : trav(trav), dist(dist) {}
        NodeTraversal trav;
        int64_t dist;
        inline bool operator<(const DistTraversal& other) const {
            return dist > other.dist; // opposite order so priority queue selects minimum
        }
    };
    
    // the IDs of nodes that we have duplicated in the reverse of the main orientation
    unordered_map<id_t, id_t> reversed_nodes;
    // edges that we have already added within the copy of the reverse strand
    unordered_set<Edge*> reversed_edges;
    
    // initialize the queue with the traversals along all of the reversing edges we found
    // during the orientation-inducing DFS
    unordered_set<pair<id_t, bool>> queued;
    priority_queue<DistTraversal> queue;
    for (auto search_init : reversing_edges) {
        NodeTraversal init_trav = search_init.first;
        NodeTraversal init_next = search_init.second;
        Edge* init_edge = get_edge(init_trav, init_next);
        
#ifdef debug
        cerr << "[unfold] intializing reverse strand search queue with " << init_trav << " -> " << init_next << " over edge " << pb2json(*init_edge) << endl;
#endif
        
        if (!reversed_nodes.count(init_next.node->id())) {
#ifdef debug
            cerr << "[unfold] reversed node " << init_next.node->id() << " has not been seen, adding to unfold graph" << endl;
#endif
            // we have not added this reverse node yet, so do it
            Node* rev_init_node = unfolded.create_node(main_orientation[init_next.node->id()].second ? init_next.node->sequence()
                                                       : reverse_complement(init_next.node->sequence()));
            reversed_nodes[init_next.node->id()] = rev_init_node->id();
        }
        
        if (init_trav.backward == main_orientation[init_trav.node->id()].second) {
            unfolded.create_edge(main_orientation[init_trav.node->id()].first, reversed_nodes[init_next.node->id()]);
        }
        else {
            unfolded.create_edge(reversed_nodes[init_next.node->id()], main_orientation[init_trav.node->id()].first);
        }
#ifdef debug
        cerr << "[unfold] adding edge to unfold graph" << endl;
#endif
        
        if (!queued.count(make_pair(init_next.node->id(), init_next.backward))) {
#ifdef debug
            cerr << "[unfold] traversal " << init_next << " has not been queued, adding to search queue" << endl;
#endif
            // we have not starting a traversal along this node in this direction, so add it to the queue
            queue.emplace(init_next, 0);
            queued.emplace(init_next.node->id(), init_next.backward);
        }
    }
    
    while (!queue.empty()) {
        DistTraversal dist_trav = queue.top();
        queue.pop();
        
#ifdef debug
        cerr << "[unfold] popping traversal " << dist_trav.trav << " at distance " << dist_trav.dist << endl;
#endif
        
        int64_t dist_thru = dist_trav.dist + dist_trav.trav.node->sequence().size();
        if (dist_thru >= max_length) {
#ifdef debug
            cerr << "[unfold] distance is above max of " << max_length << ", not continuing traversal" << endl;
#endif
            continue;
        }
        
        for (NodeTraversal next : travs_from(dist_trav.trav)) {
            
            Edge* edge = get_edge(dist_trav.trav, next);
            
#ifdef debug
            cerr << "[unfold] following edge " << pb2json(*edge) << " to " << next << endl;
#endif
            
            if ((next.backward == main_orientation[next.node->id()].second)
                == (dist_trav.trav.backward == main_orientation[dist_trav.trav.node->id()].second)) {
#ifdef debug
                cerr << "[unfold] stays on reverse strand" << endl;
#endif
                if (!reversed_nodes.count(next.node->id())) {
                    // this is the first time we've encountered this node in any direction on the reverse strand
                    // so add the reverse node
                    Node* rev_node = unfolded.create_node(main_orientation[next.node->id()].second ? next.node->sequence()
                                                          : reverse_complement(next.node->sequence()));
                    reversed_nodes[next.node->id()] = rev_node->id();
                    
#ifdef debug
                    cerr << "[unfold] node has not been observed on reverse strand, adding reverse node " << rev_node->id() << endl;
#endif
                }
                
                // we haven't traversed this edge and it stays on the reverse strand
                if (!reversed_edges.count(edge)) {
                    if (dist_trav.trav.backward == main_orientation[dist_trav.trav.node->id()].second) {
                        unfolded.create_edge(reversed_nodes[next.node->id()], reversed_nodes[dist_trav.trav.node->id()]);
                    }
                    else {
                        unfolded.create_edge(reversed_nodes[dist_trav.trav.node->id()], reversed_nodes[next.node->id()]);
                    }
                    reversed_edges.insert(edge);
#ifdef debug
                    cerr << "[unfold] edge has not been observed on reverse strand, adding reverse edge" << endl;
#endif
                }
                
                if (!queued.count(make_pair(next.node->id(), next.backward))) {
                    // this is the first time we've encountered this node in this direction on the reverse strand
                    // so queue it up for the search through
#ifdef debug
                    cerr << "[unfold] traversal " << next << " has not been seen on reverse strand, queueing up" << endl;
#endif
                    
                    queue.emplace(next, dist_thru);
                    queued.emplace(next.node->id(), next.backward);
                }
            }
        }
    }
    
    // construct the backward node translators
    for (auto orient_record : main_orientation) {
        node_translation[orient_record.second.first] = make_pair(orient_record.first,
                                                                 orient_record.second.second);
    }
    for (auto rev_orient_record : reversed_nodes) {
        node_translation[rev_orient_record.second] = make_pair(rev_orient_record.first,
                                                               !main_orientation[rev_orient_record.first].second);
    }
    
    return unfolded;
}

bool VG::has_inverting_edges(void) {
    for (id_t i = 0; i < graph.edge_size(); ++i) {
        auto& edge = graph.edge(i);
        if (!(edge.from_start() && edge.to_end())
            && (edge.from_start() || edge.to_end())) {
            return true;
        }
    }
    return false;
}

void VG::remove_inverting_edges(void) {
    set<pair<NodeSide, NodeSide>> edges;
    for_each_edge([this,&edges](Edge* edge) {
            if (!(edge->from_start() && edge->to_end())
                && (edge->from_start() || edge->to_end())) {
                edges.insert(NodeSide::pair_from_edge(edge));
            }
        });
    for (auto edge : edges) {
        destroy_edge(edge);
    }
}

bool VG::is_self_looping(Node* node) {
    for(auto* edge : edges_of(node)) {
        // Look at all the edges on the node
        if(edge->from() == node->id() && edge->to() == node->id()) {
            // And decide if any of them are self loops.
            return true;
        }
    }
    return false;
}


VG VG::dagify(uint32_t expand_scc_steps,
              unordered_map<id_t, pair<id_t, bool> >& node_translation,
              size_t target_min_walk_length,
              size_t component_length_max) {

    VG dag;
    // Find the strongly connected components in the graph.
    set<set<id_t>> strong_components = strongly_connected_components();
    // map from component root id to a translation
    // that maps the unrolled id to the original node and whether we've inverted or not

    set<set<id_t>> strongly_connected_and_self_looping_components;
    set<id_t> weak_components;
    for (auto& component : strong_components) {
        // is this node a single component?
        // does this have an inversion as a child?
        // let's add in inversions
        if (component.size() == 1
            && !is_self_looping(get_node(*component.begin()))) {
            // not part of a SCC
            // copy into the new graph
            id_t id = *component.begin();
            Node* node = get_node(id);
            // this node translates to itself
            node_translation[id] = make_pair(node->id(), false);
            dag.add_node(*node);
            weak_components.insert(id);
        } else {
            strongly_connected_and_self_looping_components.insert(component);
        }
    }
    // add in the edges between the weak components
    for (auto& id : weak_components) {
        // get the edges from the graph that link it with other weak components
        for (auto e : edges_of(get_node(id))) {
            if (weak_components.count(e->from())
                && weak_components.count(e->to())) {
                dag.add_edge(*e);
            }
        }
    }

    // add all of the nodes in the strongly connected components to the DAG
    // but do not add their edges
    for (auto& component : strongly_connected_and_self_looping_components) {
        for (auto id : component) {
            dag.create_node(get_node(id)->sequence(), id);
        }
    }

    for (auto& component : strongly_connected_and_self_looping_components) {

        // copy the SCC expand_scc_steps times, each time forwarding links from the old copy into the new
        // the result is a DAG even if the graph is initially cyclic

        // we need to record the minimum distances back to the root(s) of the graph
        // so we can (optionally) stop when we reach a given minimum minimum
        // we derive these using dynamic programming; the new min return length is
        // min(l_(i-1), \forall inbound links)
        size_t min_min_return_length = 0;
        size_t component_length = 0;
        map<Node*, size_t> min_return_length;
        // the nodes in the component that are already copied in
        map<id_t, Node*> base;
        for (auto id : component) {
            Node* node = dag.get_node(id);
            base[id] = node;
            size_t len = node->sequence().size();
            // record the length to the start of the node
            min_return_length[node] = len;
            // and in our count of the size of the component
            component_length += len;
        }
        // pointers to the last copy of the graph in the DAG
        map<id_t, Node*> last = base;
        // create the first copy of every node in the component
        for (uint32_t i = 0; i < expand_scc_steps+1; ++i) {
            map<id_t, Node*> curr = base;
            size_t curr_min_min_return_length = 0;
            // for each iteration, add in a copy of the nodes of the component
            for (auto id : component) {
                Node* node;
                if (last.empty()) { // we've already made it
                    node = dag.get_node(id);
                } else {
                    // get a new id for the node
                    node = dag.create_node(get_node(id)->sequence());
                    component_length += node->sequence().size();
                }
                curr[id] = node;
                node_translation[node->id()] = make_pair(id, false);
            }
            // preserve the edges that connect these nodes to the rest of the graph
            // And connect to the nodes in this and the previous component using the original edges as guide
            // We will break any cycles this introduces at each step
            set<id_t> seen;
            for (auto id : component) {
                seen.insert(id);
                for (auto e : edges_of(get_node(id))) {
                    if (e->from() == id && e->to() != id) {
                        // if other end is not in the component
                        if (!component.count(e->to())) {
                            // link the new node to the old one
                            Edge new_edge = *e;
                            new_edge.set_from(curr[id]->id());
                            dag.add_edge(new_edge);
                        } else if (!seen.count(e->to())) {
                            // otherwise, if it's in the component
                            // link them together
                            Edge new_edge = *e;
                            new_edge.set_from(curr[id]->id());
                            new_edge.set_to(curr[e->to()]->id());
                            dag.add_edge(new_edge);
                            seen.insert(e->to());
                        }
                    } else if (e->to() == id && e->from() != id) {
                        // if other end is not in the component
                        if (!component.count(e->from())) {
                            // link the new node to the old one
                            Edge new_edge = *e;
                            new_edge.set_to(curr[id]->id());
                            dag.add_edge(new_edge);
                        } else if (!seen.count(e->from())) {
                            // adding the node to this component
                            // can introduce self loops
                            Edge new_edge = *e;
                            new_edge.set_to(curr[id]->id());
                            new_edge.set_from(curr[e->from()]->id());
                            dag.add_edge(new_edge);
                            seen.insert(e->from());
                        }
                        if (!last.empty() && component.count(e->from())) {
                            // if we aren't in the first step
                            // and an edge is coming from a node in this component to this one
                            // add the edge that connects back to the previous node in the last copy
                            Edge new_edge = *e;
                            new_edge.set_to(curr[id]->id());
                            new_edge.set_from(last[e->from()]->id());
                            dag.add_edge(new_edge);
                            // update the min-min length
                            size_t& mm = min_return_length[curr[id]];
                            size_t inmm = curr[id]->sequence().size() + min_return_length[last[e->from()]];
                            mm = (mm ? min(mm, inmm) : inmm);
                            curr_min_min_return_length = (curr_min_min_return_length ?
                                                          min(mm, curr_min_min_return_length)
                                                          : mm);
                        }
                    } else if (e->to() == id && e->from() == id) {
                        // we don't add the self loop because we would just need to remove it anyway
                        if (!last.empty()) { // by definition, we are looking at nodes in this component
                            // but if we aren't in the first step
                            // and an edge is coming from a node in this component to this one
                            // add the edge that connects back to the previous node in the last copy
                            Edge new_edge = *e;
                            new_edge.set_to(curr[id]->id());
                            new_edge.set_from(last[id]->id());
                            dag.add_edge(new_edge);
                            // update the min-min length
                            size_t& mm = min_return_length[curr[id]];
                            size_t inmm = curr[id]->sequence().size() + min_return_length[last[e->from()]];
                            mm = (mm ? min(mm, inmm) : inmm);
                            curr_min_min_return_length = (curr_min_min_return_length ?
                                                          min(mm, curr_min_min_return_length)
                                                          : mm);
                        }
                    }
                }
            }
            // update the minimum minimim return length
            min_min_return_length = curr_min_min_return_length;
            // finish if we've reached our target min walk length
            if (target_min_walk_length &&
                min_min_return_length >= target_min_walk_length) {
                break;
            }
            last = curr;
            // break if we've exceeded the length max parameter
            if (component_length_max && component_length >= component_length_max) break;
        }
    }

    // ensure normalized edges in output; we may introduced flipped/flipped edges
    // which are valid but can introduce problems for some algorithms
    dag.flip_doubly_reversed_edges();
    return dag;
}
// Unrolls the graph into a tree in which loops are "unrolled" into new nodes
// up to some max length away from the root node and orientations are flipped.
// A translation between the new nodes that are introduced and the old nodes and graph
// is returned so that we can map reads against this structure and resolve their mappings back
// to the original "rolled" and inverted graph.
// The graph which is returned can be seen as a tree rooted at the source node, so proper
// selection of the new root may be important for performance.
// Paths cannot be maintained provided their current implementation.
// Annotated collections of nodes, or subgraphs, may be a way to preserve the relationshp.
VG VG::backtracking_unroll(uint32_t max_length, uint32_t max_branch,
                           unordered_map<id_t, pair<id_t, bool> >& node_translation) {
    VG unrolled;
    // Find the strongly connected components in the graph.
    set<set<id_t>> strong_components = strongly_connected_components();
    // add in bits where we have inversions that we'd reach from the forward direction
    // we will "unroll" these regions as well to ensure that all is well
    // ....
    //
    map<id_t, VG> trees;
    // maps from entry id to the set of nodes
    map<id_t, set<id_t> > components;
    // map from component root id to a translation
    // that maps the unrolled id to the original node and whether we've inverted or not
    map<id_t, map<id_t, pair<id_t, bool> > > translations;
    map<id_t, map<pair<id_t, bool>, set<id_t> > > inv_translations;

    // ----------------------------------------------------
    // unroll the strong components of the graph into trees
    // ----------------------------------------------------
    //cerr << "unroll the strong components of the graph into trees" << endl;
    // Anything outside the strongly connected components can be copied directly into the DAG.
    // We can also reuse the entry nodes to the strongly connected components.
    for (auto& component : strong_components) {
        // is this node a single component?
        // does this have an inversion as a child?
        // let's add in inversions

        if (component.size() == 1) {
            // copy into the new graph
            id_t id = *component.begin();
            node_translation[id] = make_pair(id, false);
            // we will handle this node if it has an inversion originating from it
            //if (!has_inverting_edge(get_node(id))) {
            //nonoverlapping_node_context_without_paths(get_node(id), unrolled);
            unrolled.add_node(*get_node(id));
            continue;
            //}
            // otherwise we will consider it as a component
            // and it will be unrolled max_length bp
        }

        // we have a multi-node component
        // first find the entry points
        // entry points will be nodes that have connections outside of the component
        set<id_t> entries;
        set<id_t> exits;
        for (auto& n : component) {
            for (auto& e : edges_of(get_node(n))) {
                if (!component.count(e->from())) {
                    entries.insert(n);
                }
                if (!component.count(e->to())) {
                    exits.insert(n);
                }
            }
        }

        // Use backtracking search starting from each entry node of each strongly
        // connected component, keeping track of the nodes on the path from the
        // entry node to the current node:
        for (auto entrypoint : entries) {
            //cerr << "backtracking search from " << entrypoint << endl;
            // each entry point is going to make a tree
            // we can merge them later with some tricks
            // for now just make the tree
            VG& tree = trees[entrypoint];
            components[entrypoint] = component;
            // maps from new ids to old ones
            map<id_t, pair<id_t, bool> >& trans = translations[entrypoint];
            // maps from old to new ids
            map<pair<id_t, bool>, set<id_t> >& itrans = inv_translations[entrypoint];

            /*
            // backtracking search

            procedure bt(c)
            if reject(P,c) then return
            if accept(P,c) then output(P,c)
            s ← first(P,c)
            while s ≠ Λ do
            bt(s)
            s ← next(P,s)
            */

            function<void(pair<id_t,bool>,id_t,bool,uint32_t,uint32_t)>
                bt = [&](pair<id_t, bool> curr,
                         id_t parent,
                         bool in_cycle,
                         uint32_t length,
                         uint32_t branches) {
                //cerr << "bt: curr: " << curr.first << " parent: " << parent << " len: " << length << " " << branches << " " << (in_cycle?"loop":"acyc") << endl;

                // i.   If the current node is outside the component,
                //       terminate this branch and return to the previous branching point.
                if (!component.count(curr.first)) {
                    return;
                }
                // ii.  Create a new copy of the current node in the DAG
                //       and use that copy for this branch.
                Node* node = get_node(curr.first);
                string curr_node_seq = node->sequence();
                // if we've reversed, take the reverse complement of the node we're flipping
                if (curr.second) curr_node_seq = reverse_complement(curr_node_seq);
                // use the forward orientation by default
                id_t cn = tree.create_node(curr_node_seq)->id();
                // record the mapping from the new id to the old
                trans[cn] = curr;
                // and record the inverse mapping
                itrans[curr].insert(cn);
                // and build the tree by connecting to our parent
                if (parent) {
                    tree.create_edge(parent, cn);
                }

                // iii. If we have found the first cycle in the current branch
                //       (the current node is the first one occurring twice in the path),
                //       initialize path length to 1 for this branch.
                //       (We need to find a k-path starting from the last offset of the previous node.)
                // walk the path back to the root to determine if we are the first cycling node
                id_t p = cn;
                // check is borked
                while (!in_cycle) { // && trans[p] != entrypoint) {
                    auto parents = tree.sides_to(NodeSide(p, false));
                    if (parents.size() < 1) { break; }
                    assert(parents.size() == 1);
                    p = parents.begin()->node;
                    // this node cycles
                    if (trans[p] == trans[cn]) {
                        in_cycle = true;
                        //length = 1; // XXX should this be 1? or maybe the length of the node?
                        break;
                    }
                }

                // iv.  If we have found a cycle in the current branch,
                //       increment path length by the length of the label of the current node.
                if (in_cycle) {
                    length += curr_node_seq.length();
                } else {
                    // if we branch here so we need to record it
                    // so as to avoid branching too many times before reaching a loop
                    auto s = start_degree(node);
                    auto e = end_degree(node);
                    branches += max(s-1 + e-1, 0);
                }

                // v.   If path length >= k, terminate this branch
                //       and return to the previous branching point.
                if (length >= max_length || (max_branch > 0 && branches >= max_branch)) {
                    return;
                } else {
                    // for each next node
                    if (!curr.second) {
                        // forward direction -- sides from end
                        for (auto& side : sides_from(node_end(curr.first))) {
                            // we enter the next node in the forward direction if the side is
                            // the start, and the reverse if it is the end
                            bt(make_pair(side.node, side.is_end), cn, in_cycle, length, branches);
                        }
                        // this handles the case of doubly-inverted edges (from start to end)
                        // which we can follow as if they are forward
                        // we stay on the same strand
                        for (auto& side : sides_to(node_end(curr.first))) {
                            // we enter the next node in the forward direction if the side coming
                            // from the start, and the reverse if it is the end
                            bt(make_pair(side.node, !side.is_end), cn, in_cycle, length, branches);
                        }
                    } else { // we've inverted already
                        // so we look at the things that leave the node on the reverse strand
                        // inverted
                        for (auto& side : sides_from(node_start(curr.first))) {
                            // here, sides from the start to the end maintain the flip
                            // but if we go to the start of the next node, we invert back
                            bt(make_pair(side.node, side.is_end), cn, in_cycle, length, branches);
                        }
                        // odd, but important quirk
                        // we also follow the normal edges, but in the reverse direction
                        // because we store them in the forward orientation
                        for (auto& side : sides_to(node_start(curr.first))) {
                            bt(make_pair(side.node, side.is_end), cn, in_cycle, length, branches);
                        }
                    }
                }

            };

            // we start with the entrypoint and run backtracking search
            bt(make_pair(entrypoint, false), 0, false, 0, 0);
        }
    }

    // -------------------------
    // tree -> dag conversion
    // -------------------------
    // now simplify each tree into a dag
    // algorithm sketch:
    // for each tree, we'll make a dag
    //   1) we start by labeling each node with its rank from the root (we have a tree, then DAGs)
    //      among nodes with the same original identity
    //   2) then, we pick the set of nodes that forms the largest group with the same identity
    //      and merge them
    //      if there is no group >1, exit, else goto (1), relabeling
    map<id_t, VG> dags;
    for (auto& g : trees) {
        id_t entrypoint = g.first;
        VG& tree = trees[entrypoint];
        VG& dag = dags[entrypoint];
        dag = tree; // copy
        map<id_t, pair<id_t, bool> >& trans = translations[entrypoint];
        map<pair<id_t, bool>, set<id_t> >& itrans = inv_translations[entrypoint];
        // rank among nodes with same original identity labeling procedure
        map<pair<id_t, bool>, size_t> orig_off;
        size_t i = 0;
        for (auto& j : itrans) {
            orig_off[j.first] = i;
            ++i;
        }
        // set up the initial vector we'll use when we have no inputs
        vector<uint32_t> zeros(orig_off.size(), 0);
        bool stable = false;
        uint16_t iter = 0;
        do {
            // -------------------------
            // first establish the rank of each node among other nodes with the same original identity
            // -------------------------
            // we'll store our positional rank vectors in the current here
            map<id_t, vector<uint32_t> > rankmap;
            // the graph is sorted (and will stay so)
            // as such we can run across it in sorted order
            dag.for_each_node([&](Node* n) {
                    // collect inbound vectors
                    vector<vector<uint32_t> > iv;
                    for (auto& side : dag.sides_to(n->id())) {
                        id_t in = side.node;
                        // should be satisfied by partial order property of DAG
                        assert(rankmap.find(in) != rankmap.end());
                        assert(!rankmap[in].empty());
                        iv.push_back(rankmap[in]);
                    }
                    // take their maximum
                    vector<uint32_t> ranks = (iv.empty() ? zeros : vpmax(iv));
                    // and increment this node so that the rankmap shows our ranks for each
                    // node in the original set at this point in the graph
                    ++ranks[orig_off[trans[n->id()]]];
                    // then save it in the rankmap
                    rankmap[n->id()] = ranks;
                });
            /*
            for (auto& r : rankmap) {
                cerr << r.first << ":" << dag.get_node(r.first)->sequence() << " [ ";
                for (auto c : r.second) {
                    cerr << c << " ";
                }
                cerr << "]" << endl;
            }
            */

            // -------------------------
            // now establish the class relative ranks for each node
            // -------------------------
            // maps from node in the current graph to the original identitiy and its rank among
            // nodes that also are clones of that node
            map<id_t, pair<pair<id_t, bool>, uint32_t> > rank_among_same;
            dag.for_each_node([&](Node* n) {
                    id_t id = n->id();
                    rank_among_same[id] = make_pair(trans[id], rankmap[id][orig_off[trans[id]]]);
                });
            // dump
            /*
            for (auto& r : rank_among_same) {
                cerr << r.first << ":" << dag.get_node(r.first)->sequence()
                     << " " << r.second.first.first << (r.second.first.second?"-":"+")
                     << ":" << r.second.second << endl;
            }
            */
            // groups
            // populate group sizes
            // groups map from the
            map<pair<pair<id_t, bool>, uint32_t>, vector<id_t> > groups;
            for (auto& r : rank_among_same) {
                groups[r.second].push_back(r.first);
            }
            // and find the biggest one
            map<uint16_t, vector<pair<pair<id_t, bool>, uint32_t> > > groups_by_size;
            for (auto& g : groups) {
                groups_by_size[g.second.size()].push_back(g.first);
            }
            // do we have a group that can be merged?
            if (groups_by_size.rbegin()->first > 1) {
                // -----------------------
                // merge the nodes that are the same and in the largest group
                // -----------------------
                auto orig = groups_by_size.rbegin()->second.front();
                auto& group = groups[orig];
                list<Node*> to_merge;
                for (auto id : group) {
                    to_merge.push_back(dag.get_node(id));
                }
                auto merged = dag.merge_nodes(to_merge);
                // we've now merged the redundant nodes
                // now we need to update the translations to reflect the fact
                // that we no longer have certain nodes in the dag
                id_t new_id = merged->id();
                // remove all old translations from new to old
                // and insert the new one
                // do the same for the inverted translations
                // from old to new
                set<id_t>& inv = itrans[orig.first];
                for (auto id : group) {
                    trans.erase(id);
                    inv.erase(id);
                }
                // store the new new->old translation
                trans[new_id] = orig.first;
                // and its inverse
                inv.insert(new_id);
            } else {
                // we have no group with more than one member
                // we are done
                stable = true;
            }
            // sort the graph
            algorithms::sort(&dag);
        } while (!stable);
    }

    // recover all the edges that link the nodes in the acyclic components of the graph
    unrolled.for_each_node([&](Node* n) {
            // get its edges in the original graph, and check if their endpoints are now
            // included in the unrolled graph (in which case they must be acyclic)
            // if they are, add the edge to the graph
            for (auto e : this->edges_of(get_node(n->id()))) {
                if (unrolled.has_node(e->from()) && unrolled.has_node(e->to())) {
                    unrolled.add_edge(*e);
                }
            }
        });


    // -----------------------------------------------
    // connect unrolled components back into the graph
    // -----------------------------------------------
    // now that we've unrolled all of the components
    // what do we do
    // we link them back into the rest of the graph in two steps

    // for each component we've unrolled (actually, for each entrypoint)
    for (auto& g : dags) {
        id_t entrypoint = g.first;
        VG& dag = dags[entrypoint];
        set<id_t> component = components[entrypoint];
        map<id_t, pair<id_t, bool> >& trans = translations[entrypoint];
        map<pair<id_t, bool>, set<id_t> >& itrans = inv_translations[entrypoint];

        // 1) increment the node ids to not conflict with the rest of the graph
        //       while recording the changes to the translation
        id_t max_id = max_node_id();
        // update the node ids in the dag
        dag.increment_node_ids(max_id);
        map<id_t, pair<id_t, bool> > trans_incr;
        // increment the translation from new node to old
        for (auto t = trans.begin(); t != trans.end(); ++t) {
            trans_incr[t->first + max_id] = t->second;
        }
        // save the incremented translation
        trans = trans_incr;
        // and do the same for the reverse mapping from old ids to new ones
        for (auto t = itrans.begin(); t != itrans.end(); ++t) {
            set<id_t> n;
            for (auto i = t->second.begin(); i != t->second.end(); ++i) {
                n.insert(*i + max_id);
            }
            t->second = n;
        }
        // 2) now that we don't conflict, add the component to the graph
        unrolled.extend(dag);
        // and also record the translation into the external reference
        for (auto t : trans) {
            // we map from a node id in the graph to an original node id and its orientation
            node_translation[t.first] = t.second;
        }

        // 3) find all the links into the component
        //       then connect all the nodes they
        //       now relate to using itrans
        for (auto& i : itrans) {
            auto old_id = i.first.first;
            auto is_flipped = i.first.second;
            set<id_t>& new_ids = i.second;
            // collect connections to old id that aren't in the component
            // we need to take the reverse complement of these if we are flipped relatively
            // sides to forward
            // add reverse complement function for edges and nodesides
            // make the connections
            for (auto i : new_ids) {
                // sides to forward
                for (auto& s : sides_to(NodeSide(old_id, false))) {
                    // if we are not in the component
                    if (!component.count(s.node)) {
                        // if we aren't flipped, we just make the connection
                        if (!is_flipped) {
                            unrolled.create_edge(s, NodeSide(i, false));
                        } else {
                            // the new node is flipped in unrolled relative to the other graph
                            // so we need to connect from the opposite side
                            unrolled.create_edge(s, NodeSide(i, true));
                        }
                    }
                }
                // sides to reverse
                for (auto& s : sides_to(NodeSide(old_id, true))) {
                    // if we are not in the component
                    if (!component.count(s.node)) {
                        if (!is_flipped) {
                            unrolled.create_edge(s, NodeSide(i, true));
                        } else {
                            unrolled.create_edge(s, NodeSide(i, false));
                        }
                    }
                }
                // sides from forward
                for (auto& s : sides_from(NodeSide(old_id, true))) {
                    // if we are not in the component
                    if (!component.count(s.node)) {
                        if (!is_flipped) {
                            unrolled.create_edge(NodeSide(i, true), s);
                        } else {
                            unrolled.create_edge(NodeSide(i, false), s);
                        }
                    }
                }
                // sides from reverse
                for (auto& s : sides_from(NodeSide(old_id, false))) {
                    // if we are not in the component
                    if (!component.count(s.node)) {
                        if (!is_flipped) {
                            unrolled.create_edge(NodeSide(i, false), s);
                        } else {
                            unrolled.create_edge(NodeSide(i, true), s);
                        }
                    }
                }
            }
        }
    }

    return unrolled;
}

} // end namespace
