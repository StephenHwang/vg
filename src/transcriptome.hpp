
#ifndef VG_TRANSCRIPTOME_HPP_INCLUDED
#define VG_TRANSCRIPTOME_HPP_INCLUDED

#include <algorithm>
#include <mutex>
#include <functional>

#include <google/protobuf/util/message_differencer.h>
#include <gbwt/dynamic_gbwt.h>
#include <vg/io/stream.hpp>
#include <vg/io/vpkg.hpp>
#include <handlegraph/mutable_path_mutable_handle_graph.hpp>
#include <bdsg/overlays/path_position_overlays.hpp>
#include <sparsepp/spp.h>

#include "../vg.hpp"
#include "../types.hpp"
#include "../gbwt_helper.hpp"

namespace vg {

using namespace std;


typedef vector<gbwt::node_type> exon_nodes_t;
typedef vector<gbwt::size_type> thread_ids_t;


/**
 * Data structure that defines a transcript annotation.
 */
struct Exon {

    /// Exon coordinates (start and end) on the chromosome/contig.
    pair<int32_t, int32_t> coordinates;

    /// Exon border node offsets (last position in upstream intron and
    /// first position in downstream intron) on a graph. 
    pair<uint32_t, uint32_t> border_offsets;

    /// Exon border reference path steps (last position in upstream intron and
    /// first position in downstream intron) on a graph. 
    pair<step_handle_t, step_handle_t> border_steps;
};

/**
 * Data structure that defines a transcript annotation.
 */
struct Transcript {

    /// Transcript name.
    string name;

    /// Is transcript in reverse direction (strand == '-').
    bool is_reverse;

    /// Name of chromosome/contig where transcript exist.
    string chrom;

    /// Length of chromosome/contig where transcript exist.
    uint32_t chrom_length;

    /// Transcript exons.
    vector<Exon> exons;

    Transcript(const string & name_in, const bool is_reverse_in, const string & chrom_in, const uint32_t & chrom_length_in) : name(name_in), is_reverse(is_reverse_in), chrom(chrom_in), chrom_length(chrom_length_in) {

        assert(chrom_length > 0);
    }
};

/**
 * Data structure that defines a base transcript path.
 */ 
struct TranscriptPath {

    /// Transcript path name.
    string name;

    /// Transcript origin name.
    const string transcript_origin;

    /// Haplotype origin ids.
    vector<uint32_t> haplotype_origin_ids;

    /// Embedded path origin names.
    string path_origin_names;

    TranscriptPath(const string & transcript_origin_in) : transcript_origin(transcript_origin_in) {

        name = "";
        path_origin_names = "";
    }
};

/**
 * Data structure that defines an edited transcript path.
 */ 
struct EditedTranscriptPath : public TranscriptPath {

    /// Transcript path.
    Path path;

    EditedTranscriptPath(const string & transcript_origin_in) : TranscriptPath(transcript_origin_in) {}
};

/**
 * Data structure that defines a completed transcript path.
 */ 
struct CompletedTranscriptPath : public TranscriptPath {

    /// Transcript path.
    vector<handle_t> path;

    CompletedTranscriptPath(const string & transcript_origin_in) : TranscriptPath(transcript_origin_in) {}
};

struct MappingHash
{
    size_t operator()(const Mapping & mapping) const
    {
        size_t seed = 0;

        spp::hash_combine(seed, mapping.position().node_id());
        spp::hash_combine(seed, mapping.position().offset());
        spp::hash_combine(seed, mapping.position().is_reverse());

        for (auto & edit: mapping.edit()) {

            spp::hash_combine(seed, edit.to_length());
        }

        return seed;
    }
 };

/**
 * Class that defines a transcriptome represented by a set of transcript paths.
 */
class Transcriptome {

    public:
    
        Transcriptome(unique_ptr<MutablePathDeletableHandleGraph>&& graph_in); 

        /// Number of threads used for transcript path construction. 
        int32_t num_threads = 1;

        /// Feature type to parse in the gtf/gff file. Parse all types if empty. 
        string feature_type = "exon";

        /// Attribute tag used to parse the transcript id/name in the gtf/gff file. 
        string transcript_tag = "transcript_id";

        /// Collapse identical transcript paths.
        bool collapse_transcript_paths = true;
    
        /// Treat a missing path in the transcripts/introns as a data error
        bool error_on_missing_path = true;

        /// Adds splice-junstions from intron BED files to the graph. 
        /// Optionally update haplotype GBWT index with new splice-junctions. 
        /// Returns the number of introns parsed. 
        int32_t add_intron_splice_junctions(vector<istream *> intron_streams, unique_ptr<gbwt::GBWT> & haplotype_index, const bool update_haplotypes);

        /// Adds splice-junstions from transcript gtf/gff3 files to the graph and
        /// creates reference transcript paths. Optionally update haplotype GBWT 
        /// index with new splice-junctions. Returns the number of transcripts parsed. 
        int32_t add_reference_transcripts(vector<istream *> transcript_streams, unique_ptr<gbwt::GBWT> & haplotype_index, const bool use_haplotype_paths, const bool update_haplotypes);

        /// Adds haplotype-specific transcript paths by projecting transcripts in
        /// gtf/gff3 files onto either non-reference embedded paths and/or haplotypes
        /// in a GBWT index. Returns the number of haplotype transcript paths projected.   
        int32_t add_haplotype_transcripts(vector<istream *> transcript_streams, const gbwt::GBWT & haplotype_index, const bool proj_emded_paths);

        /// Returns the reference transcript paths.
        const vector<CompletedTranscriptPath> & reference_transcript_paths() const;

        /// Returns the haplotype transcript paths.
        const vector<CompletedTranscriptPath> & haplotype_transcript_paths() const;

        /// Returns the graph.
        const MutablePathDeletableHandleGraph & graph() const; 

        /// Removes non-transcribed (not in transcript paths) nodes.
        void remove_non_transcribed_nodes();

        /// Chop nodes so that they are not longer than the supplied 
        /// maximum node length. Returns number of chopped nodes.
        uint32_t chop_nodes(const uint32_t max_node_length);

        /// Topological sorts graph and compacts node ids. Only works for 
        /// graphs in the PackedGraph format. Return false if not sorted.
        bool sort_compact_nodes();

        /// Embeds reference transcript paths in the graph.  
        /// Returns the number of paths embedded.
        int32_t embed_reference_transcript_paths();

        /// Embeds haplotype transcript paths in the graph.  
        /// Returns the number of paths embedded.
        int32_t embed_haplotype_transcript_paths();

        /// Adds reference transcript paths as threads to a GBWT index.
        /// Returns the number of added threads.
        int32_t add_reference_transcripts_to_gbwt(gbwt::GBWTBuilder * gbwt_builder, const bool add_bidirectional) const;
        
        /// Adds haplotype transcript paths as threads to a GBWT index.
        /// Returns the number of added threads.
        int32_t add_haplotype_transcripts_to_gbwt(gbwt::GBWTBuilder * gbwt_builder, const bool add_bidirectional) const;
        
        /// Writes reference transcript path sequences to a fasta file.  
        /// Returns the number of written sequences.
        int32_t write_reference_sequences(ostream * fasta_ostream) const;

        /// Writes haplotype transcript path sequences to a fasta file.  
        /// Returns the number of written sequences.
        int32_t write_haplotype_sequences(ostream * fasta_ostream) const;

        /// Writes info on reference transcript path to tsv file.
        /// Returns the number of written transcripts.
        int32_t write_reference_transcript_info(ostream * tsv_ostream, const gbwt::GBWT & haplotype_index, const bool add_header) const;

        /// Writes info on haplotype transcript path to tsv file.
        /// Returns the number of written transcripts.
        int32_t write_haplotype_transcript_info(ostream * tsv_ostream, const gbwt::GBWT & haplotype_index, const bool add_header) const;

        /// Writes the graph to a file.
        void write_graph(ostream * graph_ostream) const;
    
    private:

        /// Reference transcript paths representing the transcriptome. 
        vector<CompletedTranscriptPath> _reference_transcript_paths;
        mutex mutex_reference_transcript_paths;

        /// Haplotype transcript paths representing the transcriptome. 
        vector<CompletedTranscriptPath> _haplotype_transcript_paths;
        mutex mutex_haplotype_transcript_paths;

        /// Spliced pangenome graph.
        unique_ptr<MutablePathDeletableHandleGraph> _graph;
        mutex mutex_graph;

        /// Parse BED file of introns.
        void parse_introns(vector<Transcript> * introns, istream * intron_stream, const bdsg::PositionOverlay & graph_path_pos_overlay) const;

        /// Parse gtf/gff3 file of transcripts. Returns the number of non-header lines in the parsed file.
        int32_t parse_transcripts(vector<Transcript> * transcripts, istream * transcript_stream, const bdsg::PositionOverlay & graph_path_pos_overlay, const gbwt::GBWT & haplotype_index, const bool use_haplotype_paths) const;

        /// Parse gtf/gff3 attribute value.
        string parse_attribute_value(const string & attribute, const string & name) const;

        /// Returns the the mean node length of the graph
        float mean_node_length() const;

        /// Adds the exon coordinates to a transcript.
        void add_exon(Transcript * transcript, const pair<int32_t, int32_t> & exon_pos) const;

        /// Adds the exon coordinates to a transcript and finds the 
        /// position of each end of a exon on the contig path in the graph.
        void add_exon(Transcript * transcript, const pair<int32_t, int32_t> & exon_pos, const bdsg::PositionOverlay & graph_path_pos_overlay) const;

        /// Reverses exon order if the transcript is on the reverse strand and the exons 
        /// are ordered in reverse.
        void reorder_exons(Transcript * transcript) const;

        /// Constructs edited reference transcript paths from a set of 
        /// transcripts using embedded graph paths.
        list<EditedTranscriptPath> construct_reference_transcript_paths_embedded(const vector<Transcript> & transcripts, const bdsg::PositionOverlay & graph_path_pos_overlay) const;

        /// Threaded reference transcript path construction using embedded paths.
        void construct_reference_transcript_paths_embedded_callback(list<EditedTranscriptPath> * edited_transcript_paths, spp::sparse_hash_map<string, vector<EditedTranscriptPath *> > * edited_transcript_paths_index, mutex * edited_transcript_paths_mutex, const int32_t thread_idx, const vector<Transcript> & transcripts, const bdsg::PositionOverlay & graph_path_pos_overlay) const;

        /// Projects transcripts onto embedded paths in a graph and returns the resulting transcript paths.
        list<EditedTranscriptPath> project_transcript_embedded(const Transcript & cur_transcript, const bdsg::PositionOverlay & graph_path_pos_overlay, const bool use_reference_paths, const bool use_haplotype_paths) const;

        /// Constructs edited reference transcript paths from a set of 
        /// transcripts using haplotype paths in a GBWT index.
        list<EditedTranscriptPath> construct_reference_transcript_paths_gbwt(const vector<Transcript> & transcripts, const gbwt::GBWT & haplotype_index) const;

        /// Threaded reference transcript path construction using GBWT haplotype paths.
        void construct_reference_transcript_paths_gbwt_callback(list<EditedTranscriptPath> * edited_transcript_paths, spp::sparse_hash_map<string, vector<EditedTranscriptPath *> > * edited_transcript_paths_index, mutex * edited_transcript_paths_mutex, const int32_t thread_idx, const vector<pair<uint32_t, uint32_t> > & chrom_transcript_sets, const vector<Transcript> & transcripts, const gbwt::GBWT & haplotype_index, const spp::sparse_hash_map<string, map<uint32_t, uint32_t> > & haplotype_name_index) const;

        /// Constructs haplotype transcript paths by projecting transcripts onto
        /// embedded paths in a graph and/or haplotypes in a GBWT index. 
        void project_haplotype_transcripts(const vector<Transcript> & transcripts, const gbwt::GBWT & haplotype_index, const bdsg::PositionOverlay & graph_path_pos_overlay, const bool proj_emded_paths, const float mean_node_length);

        /// Threaded haplotype transcript projecting.
        void project_haplotype_transcripts_callback(spp::sparse_hash_map<string, vector<CompletedTranscriptPath *> > * completed_transcript_paths_index, const int32_t thread_idx, const vector<Transcript> & transcripts, const gbwt::GBWT & haplotype_index, const bdsg::PositionOverlay & graph_path_pos_overlay, const bool proj_emded_paths, const float mean_node_length);

        /// Projects transcripts onto haplotypes in a GBWT index and returns the resulting transcript paths.
        list<EditedTranscriptPath> project_transcript_gbwt(const Transcript & cur_transcript, const gbwt::GBWT & haplotype_index, const float mean_node_length) const;

        /// Extracts all unique haplotype paths between two nodes from a GBWT index and returns the 
        /// resulting paths and the corresponding haplotype ids for each path.
        vector<pair<exon_nodes_t, thread_ids_t> > get_exon_haplotypes(const vg::id_t start_node, const vg::id_t end_node, const gbwt::GBWT & haplotype_index, const int32_t expected_length) const;

        /// Adds new transcript paths to current set and optionally collapses 
        /// transcripts paths identical.
        template <class T>
        void append_transcript_paths(list<T> * transcript_paths, spp::sparse_hash_map<string, vector<T*> > * transcript_paths_index, list<T> * new_transcript_paths, const string & name_index_prefix) const;

        /// Constructs completed transcripts paths from 
        /// edited transcript paths. Checks that the
        /// paths contain no edits compared to the graph.
        list<CompletedTranscriptPath> construct_completed_transcript_paths(const list<EditedTranscriptPath> & edited_transcript_paths) const;

        /// Adds reference transcripts paths from edited
        /// transcript paths to transcriptome. Checks that
        /// the paths contain no edits compared to the graph.
        void add_transcript_reference_paths(const list<EditedTranscriptPath> & edited_transcript_paths);

        /// Convert a path to a vector of handles. Checks that 
        /// the path is complete (i.e. only consist of whole nodes).
        vector<handle_t> path_to_handles(const Path & path) const;

        /// Checks whether transcript path only consist of 
        /// whole nodes (complete). 
        bool has_novel_exon_boundaries(const list<EditedTranscriptPath> & edited_transcript_paths, const bool include_transcript_ends) const;

        /// Augments the graph with transcript path exon boundaries and 
        /// splice-junctions. Updates threads in gbwt index to match the augmented graph. 
        /// Adds edited transcript paths as reference transcript paths.
        void augment_graph(const list<EditedTranscriptPath> & edited_transcript_paths, const bool break_at_transcript_ends, unique_ptr<gbwt::GBWT> & haplotype_index, const bool update_haplotypes, const bool add_reference_transcript_paths);

        /// Update threads in gbwt index using graph translations. 
        void update_haplotype_index(unique_ptr<gbwt::GBWT> & haplotype_index, const spp::sparse_hash_map<gbwt::node_type, vector<pair<int32_t, gbwt::node_type> > > & translation_index) const;

        /// Adds transcript path splice-junction edges to the graph
        void add_splice_junction_edges(const list<EditedTranscriptPath> & edited_transcript_paths);
        void add_splice_junction_edges(const vector<CompletedTranscriptPath> & completed_transcript_paths);

        /// Collects all unique nodes in a set of transcript paths and adds them to a set.
        void collect_transcribed_nodes(spp::sparse_hash_set<nid_t> * transcribed_nodes, const vector<CompletedTranscriptPath> & transcript_paths) const;

        /// Split node handles in transcript paths according to index.
        void split_transcript_path_node_handles(vector<CompletedTranscriptPath> * transcript_paths, const spp::sparse_hash_map<handle_t, vector<handle_t> > & split_index);

        /// Update node handles in transcript paths according to index.
        void update_transcript_path_node_handles(vector<CompletedTranscriptPath> * transcript_paths, const spp::sparse_hash_map<handle_t, handle_t> & update_index);

        /// Embeds transcript paths in the graph.  
        /// Returns the number of paths embedded.
        int32_t embed_transcript_paths(const vector<CompletedTranscriptPath> & transcript_paths);

        /// Adds transcript paths as threads to a GBWT index.
        /// Returns the number of added threads.
        int32_t add_transcripts_to_gbwt(gbwt::GBWTBuilder * gbwt_builder, const bool add_bidirectional, const vector<CompletedTranscriptPath> & transcript_paths) const;

        /// Writes transcript path sequences to a fasta file.  
        /// Returns number of written sequences.
        int32_t write_sequences(ostream * fasta_ostream, const vector<CompletedTranscriptPath> & transcript_paths) const;

        /// Writes info on transcript paths to tsv file.
        /// Returns the number of written transcripts.
        int32_t write_transcript_info(ostream * tsv_ostream, const gbwt::GBWT & haplotype_index, const vector<CompletedTranscriptPath> & transcript_paths, const bool add_header, const bool is_reference_transcript_paths) const;
};

}


#endif
