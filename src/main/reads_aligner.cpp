/***************************************************************************
 *   Copyright (C) 2010 by Ari Loytynoja                                   *
 *   ari.loytynoja@gmail.com                                               *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include "main/reads_aligner.h"
#include "utils/exonerate_reads.h"
#include <sstream>
#include <fstream>
#include <algorithm>

using namespace std;
using namespace ppa;

Reads_aligner::Reads_aligner(){}

void Reads_aligner::align(Node *root, Model_factory *mf, int count)
{

    string file = Settings_handle::st.get("readsfile").as<string>();

    Fasta_reader fr;
    vector<Fasta_entry> reads;
    cout<<"Reads data file: "<<file<<endl;

    try
    {
        fr.read(file, reads, true);
    }
    catch (ppa::IOException& e) {
        cout<<"Error reading the reads file '"<<file<<"'.\nExiting.\n\n";
        exit(0);
    }

    int data_type = fr.check_sequence_data_type(&reads);

    if(!fr.check_alphabet(&reads,data_type))
    {
        if(!Settings_handle::st.is("silent"))
            cout<<"\nWarning: Illegal characters in input reads sequences removed!"<<endl;
    }

    bool single_ref_sequence = false;
    if(root->get_number_of_leaves()==1)
        single_ref_sequence = true;

    // Merge overlapping reads
    //
    if( Settings_handle::st.is("overlap-pair-end") )
    {
        if(Settings_handle::st.is("trim-before-merge"))
        {
            fr.trim_fastq_reads(&reads);
         }

        this->merge_paired_reads( &reads, mf );

        if(Settings_handle::st.is("overlap-merge-file"))
        {

            string path = Settings_handle::st.get("overlap-merge-file").as<string>();

            cout<<"Reads output file: "<<path<<".fastq"<<endl;

            fr.write_fastq(path,reads);
        }
        if( Settings_handle::st.is("pair-end") )
        {
            cout<<"\nWarning: both '--overlap-pair-end' and '--pair-end' options defined.\n"<<
                    "Pairing of overlapping reads may cause duplicated sequence regions.\n\n";
        }
    }

    // Trim read ends
    //
    if(Settings_handle::st.is("trim-read-ends"))
    {
        fr.trim_fastq_reads(&reads);
    }

    // Couple paired reads
    //
    if( Settings_handle::st.is("pair-end") )
    {
        this->find_paired_reads( &reads );
    }

    // Or just add the comments..
    else
    {
        this->add_trimming_comment( &reads );
    }

    // No search for optimal node or TID tags in the tree
    //
    if(Settings_handle::st.is("align-reads-at-root") || Settings_handle::st.is("reads-pileup"))
    {
        if(Settings_handle::st.is("discard-pairwise-overlapping-reads"))
        {
            this->remove_overlapping_reads(&reads, mf);
        }

        string ref_root_name = root->get_name();

        global_root = root;
        for(int i=0;i<(int)reads.size();i++)
        {

            Node * node = new Node();

            stringstream ss;
            ss<<"#"<<count<<"#";
            node->set_name(ss.str());

            global_root->set_distance_to_parent(0.001);
            node->add_left_child(global_root);

            if(!Settings_handle::st.is("silent"))
                cout<<"("<<i+1<<"/"<<reads.size()<<") ";

            Node * reads_node = new Node();
            this->copy_node_details(reads_node,&reads.at(i));

            node->add_right_child(reads_node);

            node->set_nhx_tid(node->get_left_child()->get_nhx_tid());
            node->get_right_child()->set_nhx_tid(node->get_left_child()->get_nhx_tid());

            if(!Settings_handle::st.is("silent"))
                cout<<"aligning read: "<<reads.at(i).name<<": "<<reads.at(i).comment<<endl;


            int start_offset = -1;
            int end_offset = -1;

            if(Settings_handle::st.is("pileup-reads-ordered") && !global_root->is_leaf())
            {
                Sequence *tmp_seq = global_root->get_sequence();
                int i=1;
                for(;i<tmp_seq->sites_length();i++)
                {
                    Site_children *offspring = tmp_seq->get_site_at(i)->get_children();
                    if(offspring->right_index>=0)
                        break;
                }

                start_offset = i - Settings_handle::st.get("pileup-offset").as<int>();
                if(start_offset<0 || start_offset>tmp_seq->sites_length())
                    start_offset = -1;
            }

            node->align_sequences_this_node(mf,true,false,start_offset,end_offset);

            // check if the alignment significantly overlaps with the reference alignment
            //
            bool read_overlaps = this->read_alignment_overlaps(node, reads.at(i).name, ref_root_name);

            if(read_overlaps)
            {
                count++;
                global_root = node;
            }
            // else delete the node; do not use the read
            else
            {
                node->has_left_child(false);
                delete node;
            }

        }
    }

    // Proper placement
    //
    else
    {

        global_root = root;

        // vector of node names giving the best node for each read
        //
        this->find_nodes_for_reads(root, &reads, mf);

        if(Settings_handle::st.is("placement-only"))
            exit(1);


        set<string> unique_nodeset;
        for(int i=0;i<(int)reads.size();i++)
        {
            unique_nodeset.insert(reads.at(i).node_to_align);
        }

        if(unique_nodeset.find("discarded_read") != unique_nodeset.end())
            unique_nodeset.erase(unique_nodeset.find("discarded_read"));

        vector<string> unique_nodes;
        for(set<string>::iterator sit = unique_nodeset.begin(); sit != unique_nodeset.end(); sit++)
        {
            unique_nodes.push_back(*sit);
        }
        sort(unique_nodes.begin(),unique_nodes.end(),Reads_aligner::nodeIsSmaller);

        map<string,Node*> nodes_map;
        root->get_all_nodes(&nodes_map);

        // do one tagged node at time
        //
        for(vector<string>::iterator sit = unique_nodes.begin(); sit != unique_nodes.end(); sit++)
        {
            vector<Fasta_entry> reads_for_this;

            for(int i=0;i<(int)reads.size();i++)
                if(reads.at(i).node_to_align == *sit)
                    reads_for_this.push_back(reads.at(i));

            this->sort_reads_vector(&reads_for_this);

            // remove fully overlapping reads that are mapped to this node
            //
            if(Settings_handle::st.is("rank-reads-for-nodes"))
            {
                if(Settings_handle::st.is("discard-overlapping-identical-reads"))
                {
                    this->remove_target_overlapping_identical_reads(&reads_for_this,mf);

                    if(!Settings_handle::st.is("silent"))
                    {
                        cout<<"After removing overlapping ones, for node "<<*sit<<" reads remaining:\n";
                        for(int i=0;i<(int)reads_for_this.size();i++)
                            cout<<" "<<reads_for_this.at(i).name<<" "<<reads_for_this.at(i).node_score<<endl;
                        cout<<endl;
                    }
                }
                else if(Settings_handle::st.is("discard-overlapping-reads"))
                {
                    this->remove_target_overlapping_reads(&reads_for_this);

                    if(!Settings_handle::st.is("silent"))
                    {
                        cout<<"After removing overlapping ones, for node "<<*sit<<" reads remaining:\n";
                        for(int i=0;i<(int)reads_for_this.size();i++)
                            cout<<" "<<reads_for_this.at(i).name<<" "<<reads_for_this.at(i).node_score<<endl;
                        cout<<endl;
                    }
                }
                else if(Settings_handle::st.is("discard-pairwise-overlapping-reads"))
                {
                    this->remove_overlapping_reads(&reads_for_this,mf);

                    if(!Settings_handle::st.is("silent"))
                    {
                        cout<<"After removing overlapping ones, for node "<<*sit<<" reads remaining:\n";
                        for(int i=0;i<(int)reads_for_this.size();i++)
                            cout<<" "<<reads_for_this.at(i).name<<" "<<reads_for_this.at(i).node_score<<endl;
                        cout<<endl;
                    }
                }
            }
            else
            {
                if( Settings_handle::st.is("discard-overlapping-identical-reads") ||
                         Settings_handle::st.is("discard-overlapping-reads") )
                {
                    cout<<"\nWarning: without ranking the reads for nodes, one cannot resolve overlap between reads. The flag has no effect!\n\n";
                }
            }

            string ref_node_name = *sit;

            Node *current_root = nodes_map.find(ref_node_name)->second;
            double orig_dist = current_root->get_distance_to_parent();

            bool alignment_done = false;
            int alignments_done = 0;

            // align the remaining reads to this node
            //
            for(int i=0;i<(int)reads_for_this.size();i++)
            {
                Node * node = new Node();

                stringstream ss;
                ss<<"#"<<count<<"#";
                node->set_name(ss.str());

                current_root->set_distance_to_parent(0.001);
                node->add_left_child(current_root);

                if(!Settings_handle::st.is("silent"))
                    cout<<"("<<i+1<<"/"<<reads_for_this.size()<<") ";

                Node * reads_node = new Node();
                this->copy_node_details(reads_node,&reads_for_this.at(i));


                node->add_right_child(reads_node);

                node->set_nhx_tid(node->get_left_child()->get_nhx_tid());
                node->get_right_child()->set_nhx_tid(node->get_left_child()->get_nhx_tid());

                if(!Settings_handle::st.is("silent"))
                    cout<<"aligning read: "<<reads_for_this.at(i).name<<": "<<reads_for_this.at(i).comment<<endl;

                int start_offset = -1;
                int end_offset = -1;

                if(reads_for_this.at(i).use_local)
                {
                    int offset = 20;
                    int offset_multiplier = 2;

                    start_offset = reads_for_this.at(i).local_tstart - offset - reads_for_this.at(i).local_qstart * offset_multiplier;
                    end_offset = reads_for_this.at(i).local_tend + offset +
                            ( reads_node->get_sequence()->sites_length() - reads_for_this.at(i).local_qend ) * offset_multiplier;

                    if(start_offset < 0)
                        start_offset = -1;

                    if(end_offset > current_root->get_sequence()->sites_length())
                        end_offset = -1;

                }

                node->align_sequences_this_node(mf,true,false,start_offset,end_offset);


                // check if the alignment significantly overlaps with the reference alignment
                //
                bool read_overlaps = this->read_alignment_overlaps(node, reads_for_this.at(i).name, ref_node_name);

                if(read_overlaps)
                {
                    count++;
                    current_root = node;

                    if( orig_dist > current_root->get_distance_to_parent() )
                        orig_dist -= current_root->get_distance_to_parent();

                    alignment_done = true;
                    alignments_done++;
                }
                // else delete the node; do not use the read
                else
                {
                    node->has_left_child(false);
                    delete node;
                }

            }

            current_root->set_distance_to_parent(orig_dist);


            if(alignment_done)
            {
                if(single_ref_sequence)
                {
                    global_root = current_root;
                }
                else
                {
                    bool parent_found = this->correct_sites_index(current_root, ref_node_name, alignments_done, &nodes_map);

                    if(!parent_found)
                    {
                        global_root = current_root;
                    }
                }
            }
        }
    }
}


void Reads_aligner::merge_reads_only()
{
    string file = Settings_handle::st.get("readsfile").as<string>();

    Fasta_reader fr;
    vector<Fasta_entry> reads;
    cout<<"Reads data file: "<<file<<endl;

    fr.read(file, reads, true);

    int data_type = fr.check_sequence_data_type(&reads);
    Model_factory mf(data_type);

    if(!fr.check_alphabet(&reads,Model_factory::dna))
        cout<<"\nWarning: Illegal characters in input sequences removed!"<<endl;

    float *dna_pi = fr.base_frequencies();

    mf.dna_model(dna_pi,&Settings_handle::st);

    this->merge_paired_reads( &reads, &mf );

    string path = "outfile";
    if(Settings_handle::st.is("overlap-merge-file"))
    {
        path = Settings_handle::st.get("overlap-merge-file").as<string>();
    }

    cout<<"Reads output file: "<<path<<".fastq"<<endl;

    fr.write_fastq(path,reads);
}

void Reads_aligner::add_trimming_comment(vector<Fasta_entry> *reads)
{
    vector<Fasta_entry>::iterator fit1 = reads->begin();

    for(;fit1 != reads->end();fit1++)
    {
        stringstream trimming;
        trimming << "P1ST"<<fit1->trim_start<<":P1ET"<<fit1->trim_end;
        fit1->comment += trimming.str();
//        cout<<"s: "<<trimming.str()<<endl;
    }
}


void Reads_aligner::merge_paired_reads(vector<Fasta_entry> *reads, Model_factory *mf)
{

    vector<Fasta_entry>::iterator fit1 = reads->begin();

    for(;fit1 != reads->end();fit1++)
    {
        string name1 = fit1->name;
        if(name1.substr(name1.length()-2) != "/1")
        {
            continue;
        }

        vector<Fasta_entry>::iterator fit2 = fit1;
        fit2++;

        for(;fit2 != reads->end();)
        {
            string name2 = fit2->name;
            if(name2.substr(name2.length()-2) != "/2")
            {
                fit2++;
                continue;
            }

            if(name1.substr(0,name1.length()-2)==name2.substr(0,name2.length()-2))
            {

                Node * node_left = new Node();
                this->copy_node_details(node_left,&(*fit1));

                Node * node_right = new Node();
                this->copy_node_details(node_right,&(*fit2));

                Node * node = new Node();
                node->set_name("merge");

                node->add_left_child(node_left);
                node->add_right_child(node_right);

                node->set_nhx_tid(node->get_left_child()->get_nhx_tid());

                if(Settings::noise>1)
                    cout<<"aligning paired reads: "<<fit1->name<<" and "<<fit2->name<<endl;

                node->align_sequences_this_node(mf,true,true);

                Sequence *anc_seq = node->get_sequence();
                int x = 0; int y = 0;
                stringstream l_seq("");
                stringstream r_seq("");

                int overlap = 0;
                int identical = 0;

                for(int i=1;i<anc_seq->sites_length()-1;i++)
                {
                    int path_state = anc_seq->get_site_at(i)->get_path_state();

                    if(path_state==Site::xgapped)
                    {
                        l_seq<<fit1->sequence.at(x); r_seq<<"-";
                        x++;
                    }
                    else if(path_state==Site::ygapped)
                    {
                        l_seq<<"-"; r_seq<<fit2->sequence.at(y);
                        y++;
                    }
                    else if(path_state==Site::matched)
                    {
                        l_seq<<fit1->sequence.at(x); r_seq<<fit2->sequence.at(y);

                        overlap++;
                        if(fit1->sequence.at(x) == fit2->sequence.at(y))
                            identical++;

                        x++;y++;
                    }
                    else
                    {
                        cout<<"Error in pair-end merge alignment\n";
                    }
                }

                if(Settings::noise>2)
                {
                    cout<<"Alignment read pair "<<fit1->name<<" and "<<fit2->name<<".\n";
                    cout<<l_seq.str()<<endl<<r_seq.str()<<endl<<endl;
                    cout<<"overlap "<<overlap<<", identical "<<identical<<endl;
                }

                if( ( overlap >= Settings_handle::st.get("overlap-minimum").as<int>() &&
                      float(identical)/overlap >= Settings_handle::st.get("overlap-identity").as<float>() )
                 || ( overlap == identical && overlap >= Settings_handle::st.get("overlap-identical-minimum").as<int>() )
                    )
                {

                    if(!Settings_handle::st.is("silent"))
                        cout<<"Merging "<<fit1->name<<" and "<<fit2->name<<": new name ";

                    fit1->name = name1.substr(0,name1.length()-1)+"m12";
                    fit1->comment = fit2->comment;

                    if(!Settings_handle::st.is("silent"))
                        cout<<fit1->name<<".\n";

                    string seq = "";
                    string qsc = "";

                    x=0;y=0;
                    for(int i=1;i<anc_seq->sites_length()-1;i++)
                    {
                        int path_state = anc_seq->get_site_at(i)->get_path_state();

                        if(path_state==Site::xgapped)
                        {
                            seq += fit1->sequence.at(x);
                            qsc += fit1->quality.at(x);
                            x++;
                        }
                        else if(path_state==Site::ygapped)
                        {
                            seq += fit2->sequence.at(y);
                            qsc += fit2->quality.at(y);
                            y++;
                        }
                        else if(path_state==Site::matched)
                        {
                            if(static_cast<int>( fit1->quality.at(x) ) > static_cast<int>( fit2->quality.at(y) ))
                            {
                                seq += fit1->sequence.at(x);
                                qsc += fit1->quality.at(x);
                            }
                            else
                            {
                                seq += fit2->sequence.at(y);
                                qsc += fit2->quality.at(y);
                            }
                            x++;y++;
                        }
                    }

                    fit1->sequence = seq;
                    fit1->quality = qsc;

                    reads->erase(fit2);
                    delete node;

                    continue;
                }
                else
                {
                    fit2++;
                }


                delete node;
                continue;
            }

            fit2++;
        }
    }
}

void Reads_aligner::find_paired_reads(vector<Fasta_entry> *reads)
{

    vector<Fasta_entry>::iterator fit1 = reads->begin();

    for(;fit1 != reads->end();fit1++)
    {
        string name1 = fit1->name;
        if(name1.substr(name1.length()-2) != "/1")
        {
            continue;
        }

        vector<Fasta_entry>::iterator fit2 = fit1;
        fit2++;

        for(;fit2 != reads->end();)
        {
            string name2 = fit2->name;
            if(name2.substr(name2.length()-2) != "/2")
            {
                fit2++;
                continue;
            }

            if(name1.substr(0,name1.length()-2)==name2.substr(0,name2.length()-2))
            {
                if(!Settings_handle::st.is("silent"))
                    cout<<"Pairing "<<fit1->name<<" and "<<fit2->name<<": new name ";

                fit1->name = name1.substr(0,name1.length()-1)+"p12";

                if(!Settings_handle::st.is("silent"))
                    cout<<fit1->name<<".\n";

                fit1->comment = fit2->comment;
                fit1->first_read_length = fit1->sequence.length();
                fit1->sequence += "0"+fit2->sequence;
                if(fit1->quality!="")
                    fit1->quality += "0"+fit2->quality;

                stringstream trimming;
                trimming << "P1ST"<<fit1->trim_start<<":P1ET"<<fit1->trim_end<<":P2ST"<<fit2->trim_start<<":P2ET"<<fit2->trim_end;
                fit1->comment += trimming.str();

                reads->erase(fit2);

                continue;
            }

            fit2++;
        }
    }


    for(fit1 = reads->begin();fit1 != reads->end();fit1++)
    {
        if(fit1->comment.find("P1ST")==string::npos)
        {
            stringstream trimming;
            trimming << "P1ST"<<fit1->trim_start<<":P1ET"<<fit1->trim_end;
            fit1->comment += trimming.str();
        }
    }
}

void Reads_aligner::copy_node_details(Node *reads_node,Fasta_entry *read)
{
    double r_dist = Settings_handle::st.get("reads-distance").as<float>();

    reads_node->set_distance_to_parent(r_dist);
    reads_node->set_name(read->name);
    reads_node->add_name_comment(read->comment);
    reads_node->add_sequence( *read, read->data_type, false, true);

}

bool Reads_aligner::read_alignment_overlaps(Node * node, string read_name, string ref_node_name)
{
    float min_overlap = Settings_handle::st.get("min-reads-overlap").as<float>();
    float min_identity = Settings_handle::st.get("min-reads-identity").as<float>();

    Sequence *node_sequence = node->get_sequence();

    int aligned = 0;
    int read_length = 0;
    int matched = 0;

    for( int j=0; j < node_sequence->sites_length(); j++ )
    {
        bool read_has_site = node->has_site_at_alignment_column(j,read_name);
        bool ref_root_has_site = node->has_site_at_alignment_column(j,ref_node_name);

        if(read_has_site)
            read_length++;

        if(read_has_site && ref_root_has_site)
        {
            aligned++;

            int state_read = node->get_state_at_alignment_column(j,read_name);
            int state_ref  = node->get_state_at_alignment_column(j,ref_node_name);
            if(state_read == state_ref)
                matched++;
        }
    }


    if(!Settings_handle::st.is("silent"))
        cout<<"  aligned positions "<<(float)aligned/(float)read_length<<" ["<<aligned<<"/"<<read_length<<"];"<<
            " identical positions "<<(float)matched/(float)aligned<<" ["<<matched<<"/"<<aligned<<"]"<<endl;

    if( (float)aligned/(float)read_length >= min_overlap && (float)matched/(float)aligned >= min_identity)
    {
        return true;
    }
    else if( (float)aligned/(float)read_length < min_overlap && (float)matched/(float)aligned < min_identity )
    {

        cout<<"Warning: read "<<read_name<<" dropped using the minimum overlap cut-off of "<<min_overlap<<
                " and the minimum identity cut-off of "<<min_identity<<"."<<endl;

        return false;
    }
    else if( (float)aligned/(float)read_length < min_overlap)
    {

        cout<<"Warning: read "<<read_name<<" dropped using the minimum overlap cut-off of "<<min_overlap<<"."<<endl;

        return false;
    }
    else if( (float)matched/(float)aligned < min_identity)
    {

        cout<<"Warning: read "<<read_name<<" dropped using the minimum identity cut-off of "<<min_identity<<"."<<endl;

        return false;
    }

    return false;
}


bool Reads_aligner::correct_sites_index(Node *current_root, string ref_node_name, int alignments_done, map<string,Node*> *nodes_map)
{

    // correct the sites index at the parent node; insertions corrected later
    //
    vector<int> sites_index;
    int index_delta = 0;

    for(int j=0; j<current_root->get_sequence()->sites_length(); j++)
    {
        if(current_root->has_site_at_alignment_column(j,ref_node_name))
        {
            sites_index.push_back(index_delta);
            index_delta = 0;
        }
        else
            index_delta++;
    }

    Node *current_parent = 0;
    map<string,Node*>::iterator mit = nodes_map->begin();
    bool parent_found = false;

    int is_left_child = true;
    for(;mit != nodes_map->end();mit++)
    {
        if(!mit->second->is_leaf() && mit->second->get_left_child()->get_name() == ref_node_name)
        {
            current_parent = mit->second;
            current_parent->add_left_child(current_root);
            parent_found = true;
        }

        if(!mit->second->is_leaf() && mit->second->get_right_child()->get_name() == ref_node_name)
        {
            current_parent = mit->second;
            current_parent->add_right_child(current_root);
            is_left_child = false;
            parent_found = true;
        }
    }


    if(parent_found)
    {
        if(Settings::noise>1)
            cout<<" Parent of "<<ref_node_name<<" is "<<current_parent->get_name()<<"; "
            <<alignments_done<<" alignments done.";//<<endl;

        Sequence *parent_sequence = current_parent->get_sequence();

        index_delta = 0;

        for(int j=0; j<parent_sequence->sites_length(); j++)
        {
            Site *parent_site = parent_sequence->get_site_at(j);

            if(is_left_child && parent_site->get_children()->left_index >= 0)
            {
                index_delta += sites_index.at(parent_site->get_children()->left_index);
                parent_site->get_children()->left_index += index_delta;
            }
            else if(!is_left_child && parent_site->get_children()->right_index >= 0)
            {
                index_delta += sites_index.at(parent_site->get_children()->right_index);
                parent_site->get_children()->right_index += index_delta;
            }
        }

        if(Settings::noise>2)
        {
            if(index_delta>0)
                cout<<" Site index needs correcting.\n";
            else
                cout<<" Site index not changed.\n";
        }

        if(index_delta>0)
        {
            if(is_left_child)
                current_parent->left_needs_correcting_sequence_site_index(true);
            else
                current_parent->right_needs_correcting_sequence_site_index(true);
        }

        return true;

    } // if(parent_found)
    else
    {
        if(Settings::noise>1)
            cout<<" No parent for "<<ref_node_name<<" found. Assuming that this is root.\n";

        return false;
    }

}

void Reads_aligner::find_nodes_for_reads(Node *root, vector<Fasta_entry> *reads, Model_factory *mf)
{

    multimap<string,string> tid_nodes;
    bool ignore_tid_tags = true;

    if(Settings_handle::st.is("test-every-node"))
    {
        root->get_node_names(&tid_nodes);
    }
    else if(Settings_handle::st.is("test-every-internal-node"))
    {
        root->get_internal_node_names(&tid_nodes);
    }
    else
    {
        root->get_node_names_with_tid_tag(&tid_nodes);
        ignore_tid_tags = false;
    }

    ofstream pl_output;

    if(Settings_handle::st.is("placement-file"))
    {
        string fname = Settings_handle::st.get("placement-file").as<string>();
        pl_output.open(fname.append(".tsv").c_str());
    }

    for(int i=0;i<(int)reads->size();i++)
    {
        reads->at(i).node_score = -1.0;

        string tid = reads->at(i).tid;
        if( ignore_tid_tags )
            tid = "<empty>";


        // Call Exonerate to reduce the search space

        map<string,hit> exonerate_hits;

        if(Settings_handle::st.is("use-exonerate-reads-local") || Settings_handle::st.is("fast-placement"))
        {
            Exonerate_reads er;
            if(!er.test_executable())
                cout<<"The executable for exonerate not found! The fast placement search not used!";
            else
            {
                tid_nodes.clear();

                if(Settings_handle::st.is("test-every-internal-node"))
                {
                    root->get_internal_node_names(&tid_nodes);
                }
                else if(Settings_handle::st.is("test-every-node"))
                {
                    root->get_node_names(&tid_nodes);
                }
                else
                {
                    root->get_node_names_with_tid_tag(&tid_nodes);
                    ignore_tid_tags = false;
                }


                if(tid_nodes.size()>0)
                    er.local_alignment(root,&reads->at(i),&tid_nodes,&exonerate_hits, true);

                if(Settings_handle::st.is("use-exonerate-reads-gapped") || Settings_handle::st.is("fast-placement"))
                    er.local_alignment(root,&reads->at(i),&tid_nodes,&exonerate_hits,false);
            }
        }
        else if(Settings_handle::st.is("use-exonerate-reads-gapped"))
        {
            Exonerate_reads er;
            if(!er.test_executable())
                cout<<"The executable for exonerate not found! The option '--exonerate-reads-gapped' not used!";
            else
            {
                tid_nodes.clear();

                if(Settings_handle::st.is("test-every-node"))
                {
                    root->get_node_names(&tid_nodes);
                }
                else if(Settings_handle::st.is("test-every-internal-node"))
                {
                    root->get_internal_node_names(&tid_nodes);
                }
                else
                {
                    root->get_node_names_with_tid_tag(&tid_nodes);
                    ignore_tid_tags = false;
                }

                if(tid_nodes.size()>0)
                    er.local_alignment(root,&reads->at(i),&tid_nodes,&exonerate_hits,false);
            }
        }




        // Discarded by Exonerate

        if(reads->at(i).node_to_align == "discarded_read")
        {
            continue;
        }

        // Has TID or exhaustive search
        else if(tid != "")
        {

            int matches = tid_nodes.count(tid);

            if( ignore_tid_tags )
                matches = tid_nodes.size();


            // has TID but no matching node

            if(matches == 0)
            {
                if(!Settings_handle::st.is("silent"))
                    cout<<"Read "<<reads->at(i).name<<" ("<<i+1<<"/"<<reads->size()<<") with the tid "<<tid<<" has no matching node. Aligned to root.\n";
                reads->at(i).node_to_align = root->get_name();

                if(Settings_handle::st.is("placement-file"))
                {
                    pl_output<<reads->at(i).name<<" "<<root->get_name()<<" TID="<<tid<<" (no match)"<<endl;
                }

            }


            // has only one matching node and no need for ranking

            else if(matches == 1 && !Settings_handle::st.is("rank-reads-for-nodes") )
            {
                multimap<string,string>::iterator tit = tid_nodes.find(tid);

                if( ignore_tid_tags )
                    tit = tid_nodes.begin();

                if(tit != tid_nodes.end())
                {
                    if(!Settings_handle::st.is("silent"))
                        cout<<"Read "<<reads->at(i).name<<" ("<<i+1<<"/"<<reads->size()<<") with the tid "<<tid<<" only matches the node "<<tit->second<<"."<<endl;
                    reads->at(i).node_to_align = tit->second;

                    multimap<string,hit>::iterator ith = exonerate_hits.find(tit->second);
                    if(ith != exonerate_hits.end())
                    {
                        hit h = ith->second;

                        reads->at(i).local_qstart = h.q_start;
                        reads->at(i).local_qend = h.q_end;
                        reads->at(i).local_tstart = h.t_start;
                        reads->at(i).local_tend = h.t_end;
                        if(Settings_handle::st.is("fast-placement") || Settings_handle::st.is("use-exonerate-anchors"))
                          reads->at(i).use_local  = true;
                    }

                    if(Settings_handle::st.is("placement-file"))
                    {
                        pl_output<<reads->at(i).name<<" "<<tit->second<<" TID="<<tid<<endl;
                    }

                }
            }


            // has TID and matching nodes, or exhaustive search

            else
            {
                double best_score = -HUGE_VAL;
                string best_node = root->get_name();

                map<string,Node*> nodes;
                root->get_all_nodes(&nodes);

                multimap<string,string>::iterator tit;

                if( ignore_tid_tags )
                {
                    tit = tid_nodes.begin();
                }
                else
                {
                    tit = tid_nodes.find(tid);
                }

                if(tit != tid_nodes.end())
                {
                    if(!Settings_handle::st.is("silent"))
                        cout<<"Read "<<reads->at(i).name<<" with TID "<<tid<<" matches "<<tid_nodes.size()<<" nodes.\n";

                    while(tit != tid_nodes.end())
                    {
                        map<string,Node*>::iterator nit = nodes.find(tit->second);
                        double score = this->read_match_score( nit->second, &reads->at(i), mf, best_score);

                        if(Settings::noise>0)
                            cout<<"   "<<tit->second<<" with score "<<score<<" (simple p-distance)\n";
                        if(score>best_score)
                        {
                            best_score = score;
                            best_node = tit->second;
                        }
                        tit++;
                    }
                }
                if(best_score<0.05)
                {
                    if(Settings_handle::st.is("align-bad-reads-at-root"))
                    {
                        if(!Settings_handle::st.is("silent"))
                            cout<<"Best node aligns with less than 5% of identical sites. Aligning to root instead.\n";
                        reads->at(i).node_to_align = root->get_name();

                        if(Settings_handle::st.is("placement-file"))
                        {
                            pl_output<<reads->at(i).name<<" "<<root->get_name()<<" TID="<<tid<<" (bad) "<<endl;
                        }
                    }
                    else
                    {
                        if(!Settings_handle::st.is("silent"))
                            cout<<"Best node aligns with less than 5% of identical sites. Read is discarded.\n";
                        reads->at(i).node_to_align = "discarded_read";
                    }
                }
                else
                {
                    if(!Settings_handle::st.is("silent"))
                        cout<<"  best node "<<best_node<<" (score "<<best_score<<").\n";
                    reads->at(i).node_score = best_score;
                    reads->at(i).node_to_align = best_node;

                    if(Settings_handle::st.is("placement-file"))
                    {
                        pl_output<<reads->at(i).name<<" "<<best_node<<" TID="<<tid<<endl;
                    }

                }
            }
        }


        // no TID, aligning at root

        else
        {
//            cout<<"failed "<<reads->at(i).node_to_align<<endl;
            if(!Settings_handle::st.is("silent"))
                cout<<"Read "<<reads->at(i).name<<" ("<<i+1<<"/"<<reads->size()<<") has no tid. Aligned to root.\n";
            reads->at(i).node_to_align = root->get_name();

            if(Settings_handle::st.is("placement-file"))
            {
                pl_output<<reads->at(i).name<<" "<<root->get_name()<<" TID=NULL"<<endl;
            }
        }
    }

    if(Settings_handle::st.is("placement-file"))
    {
        pl_output.close();
    }
}

double Reads_aligner::read_match_score(Node *node, Fasta_entry *read, Model_factory *mf, float best_score)
{

    double r_dist = Settings_handle::st.get("reads-distance").as<float>();

    double org_dist = node->get_distance_to_parent();
    node->set_distance_to_parent(0.001);

    Node * reads_node1 = new Node();
    reads_node1->set_distance_to_parent(r_dist);
    reads_node1->set_name(read->name);
    reads_node1->add_name_comment(read->comment);
    reads_node1->add_sequence( *read, read->data_type, false, true);

    Node * tmpnode = new Node();
    tmpnode->set_name("(tmp)");

    tmpnode->add_left_child(node);
    tmpnode->add_right_child(reads_node1);

    tmpnode->align_sequences_this_node(mf,true);

    node->set_distance_to_parent(org_dist);

    int matching = 0;
    int aligned = 0;

    int node_start_pos1 = -1;
    int node_end_pos1 = -1;
    int node_start_pos2 = -1;
    int node_end_pos2 = -1;
    for( int k=1; k < tmpnode->get_sequence()->sites_length()-1; k++ )
    {
        Site *site = tmpnode->get_sequence()->get_site_at(k);

        if(site->get_children()->right_index == 1)
        {
            node_start_pos1 = k;
        }
        else if(site->get_children()->right_index == read->first_read_length)
        {
            node_end_pos1 = k;
        }
        else if(site->get_children()->right_index == read->first_read_length+1)
        {
            node_start_pos2 = k;
        }
        else if(site->get_children()->right_index > 0)
        {
            node_end_pos2 = k;
        }

        if(site->get_children()->right_index>=0 && site->get_children()->left_index>=0)
        {

            Site *site1 = tmpnode->get_right_child()->get_sequence()->get_site_at(site->get_children()->right_index);
            Site *site2 = tmpnode->get_left_child()->get_sequence()->get_site_at(site->get_children()->left_index);

            if(site1->get_state() == site2->get_state())
                matching++;

            aligned++;
        }
    }

    double score = (double) matching/ (double) reads_node1->get_sequence()->sites_length();
//    cout<<"  "<<score<<" : "<<matching<<" "<<aligned<<" "<<node->get_sequence()->sites_length()<<" "<<reads_node1->get_sequence()->sites_length()<<endl;

    tmpnode->has_left_child(false);
    delete tmpnode;

    if(score>best_score)
    {
        read->node_start_pos1 = node_start_pos1;
        read->node_end_pos1 =   node_end_pos1;
        read->node_start_pos2 = node_start_pos2;
        read->node_end_pos2 =   node_end_pos2;
    }

    return score;
}

void Reads_aligner::remove_target_overlapping_identical_reads(vector<Fasta_entry> *reads, Model_factory *mf)
{
    if(!Settings_handle::st.is("silent"))
        cout<<"Removing identical reads mapped at overlapping positions.\n";

    vector<Fasta_entry>::iterator ri1 = reads->begin();

    for(;ri1 != reads->end();)
    {
        vector<Fasta_entry>::iterator ri2 = ri1;
        ri2++;
        for(;ri2 != reads->end();)
        {
            bool embedded = false;
            if( Settings_handle::st.is("pair-end")
                && (  (ri2->node_start_pos1 >= ri1->node_start_pos1 && ri2->node_end_pos1 <= ri1->node_end_pos1
                    && ri2->node_start_pos2 >= ri1->node_start_pos2 && ri2->node_end_pos2 <= ri1->node_end_pos2 )
                   || (ri2->node_start_pos1 <= ri1->node_start_pos1 && ri2->node_end_pos1 >= ri1->node_end_pos1
                    && ri2->node_start_pos2 <= ri1->node_start_pos2 && ri2->node_end_pos2 >= ri1->node_end_pos2 )
                   )
              )
            {
                embedded = true;
            }
            else if(!Settings_handle::st.is("pair-end")
                    && (  (ri2->node_start_pos1 >= ri1->node_start_pos1 && ri2->node_end_pos2 <= ri1->node_end_pos2 )
                       || (ri2->node_start_pos1 <= ri1->node_start_pos1 && ri2->node_end_pos2 >= ri1->node_end_pos2 )
                       )
                   )
            {
                embedded = true;
            }


            if( embedded )
            {
                Node * node = new Node();
                node->set_name("read pair");

                this->align_two_reads(node,&(*ri1),&(*ri2),mf);

                int matching = this->reads_pairwise_matching_sites(node);

                delete node;

                int r1_length = (int)ri1->sequence.length();
                int r2_length = (int)ri2->sequence.length();

//                cout<<"identical? "<<matching<<" "<<r1_length<<" "<<r2_length<<endl;
                if(Settings_handle::st.is("pair-end")) // remove the midpoint marker
                {
                    r1_length--;
                    r2_length--;
                }

                if( matching == r2_length )
                {
                    if(!Settings_handle::st.is("silent"))
                        cout<<"Read "<<ri2->name<<" is fully embedded in read "<<ri1->name<<" and overlapping sites are identical.  Read "<<ri2->name<<" is deleted.\n";
                    reads->erase(ri2);
                }
                else if( matching == r1_length )
                {
                    if(!Settings_handle::st.is("silent"))
                        cout<<"Read "<<ri1->name<<" is fully embedded in read "<<ri2->name<<" and overlapping sites are identical.  Read "<<ri1->name<<" is deleted.\n";
                    reads->erase(ri1);
                    ri1--;
                    ri2 = reads->end();
                }
                else
                {
                    ri2++;
                }

            }
            else
            {
                ri2++;
            }

        }
        ri1++;
    }
}

void Reads_aligner::remove_target_overlapping_reads(vector<Fasta_entry> *reads)
{
    if(!Settings_handle::st.is("silent"))
        cout<<"Removing reads mapped at overlapping positions.\n";

    vector<Fasta_entry>::iterator ri1 = reads->begin();

    for(;ri1 != reads->end();)
    {
        vector<Fasta_entry>::iterator ri2 = ri1;
        ri2++;
        for(;ri2 != reads->end();)
        {
            if( Settings_handle::st.is("pair-end") )
            {
                if( ri2->node_start_pos1 >= ri1->node_start_pos1 && ri2->node_end_pos1 <= ri1->node_end_pos1 &&
                    ri2->node_start_pos2 >= ri1->node_start_pos2 && ri2->node_end_pos2 <= ri1->node_end_pos2 )
                {
                    if(!Settings_handle::st.is("silent"))
                        cout<<"Read "<<ri2->name<<" is fully embedded in read "<<ri1->name<<".  Read "<<ri2->name<<" is deleted.\n";
                    reads->erase(ri2);
                }
                else
                {
                    ri2++;
                }

            }
            else
            {
                if( ri2->node_start_pos1 >= ri1->node_start_pos1 && ri2->node_end_pos2 <= ri1->node_end_pos2 )
                {
                    if(!Settings_handle::st.is("silent"))
                        cout<<"Read "<<ri2->name<<" is fully embedded in read "<<ri1->name<<".  Read "<<ri2->name<<" is deleted.\n";
                    reads->erase(ri2);
                }
                else
                {
                    ri2++;
                }
            }

        }
        ri1++;
    }
}

void Reads_aligner::align_two_reads(Node *node, Fasta_entry *ri1, Fasta_entry *ri2, Model_factory *mf)
{
    double r_dist = Settings_handle::st.get("reads-distance").as<float>();

    node->set_name("read pair");

    Node * reads_node1 = new Node();
    reads_node1->set_distance_to_parent(r_dist);
    reads_node1->set_name(ri1->name);
    reads_node1->add_name_comment(ri1->comment);
    reads_node1->add_sequence( *ri1, Model_factory::dna);
    node->add_right_child(reads_node1);

    Node * reads_node2 = new Node();
    reads_node2->set_distance_to_parent(r_dist);
    reads_node2->set_name(ri2->name);
    reads_node2->add_name_comment(ri2->comment);
    reads_node2->add_sequence( *ri2, Model_factory::dna);
    node->add_left_child(reads_node2);

    node->align_sequences_this_node(mf,true);

}

int Reads_aligner::reads_pairwise_matching_sites(Node *node)
{
    int matching = 0;

    for( int k=1; k < node->get_sequence()->sites_length()-1; k++ )
    {
        Site *site = node->get_sequence()->get_site_at(k);
        if(site->get_children()->right_index>=0 && site->get_children()->left_index>=0)
        {
            Site *site1 = node->get_right_child()->get_sequence()->get_site_at(site->get_children()->right_index);
            Site *site2 = node->get_left_child()->get_sequence()->get_site_at(site->get_children()->left_index);

            if(site1->get_state() == site2->get_state())
                matching++;
        }
    }

    return matching;
}

void Reads_aligner::remove_overlapping_reads(vector<Fasta_entry> *reads, Model_factory *mf)
{

    if(!Settings_handle::st.is("silent"))
        cout<<"Removing pairwise overlapping reads.\n";

    vector<Fasta_entry>::iterator ri1 = reads->begin();

    for(;ri1 != reads->end();)
    {
        vector<Fasta_entry>::iterator ri2 = ri1;
        ri2++;
        for(;ri2 != reads->end();)
        {

            Node * node = new Node();
            node->set_name("read pair");

            this->align_two_reads(node,&(*ri1),&(*ri2),mf);

            int matching = this->reads_pairwise_matching_sites(node);

            int r1_length = (int)ri1->sequence.length();
            int r2_length = (int)ri2->sequence.length();

            if(Settings_handle::st.is("pair-end")) // remove the midpoint marker
            {
                r1_length--;
                r2_length--;
            }

            if( matching == r1_length
                && matching == r2_length )
            {
                if(!Settings_handle::st.is("silent"))
                    cout<<"Reads "<<ri1->name<<" and "<<ri2->name<<" are identical. Read "<<ri2->name<<" is deleted.\n";
                reads->erase(ri2);
            }
            else if(matching == r1_length)
            {
                if(!Settings_handle::st.is("silent"))
                    cout<<"Read "<<ri1->name<<" is fully embedded in read "<<ri2->name<<". Read "<<ri1->name<<" is deleted.\n";
                reads->erase(ri1);
                ri1--;
                ri2 = reads->end();
            }
            else if(matching == r2_length)
            {
                if(!Settings_handle::st.is("silent"))
                    cout<<"Read "<<ri2->name<<" is fully embedded in read "<<ri1->name<<".  Read "<<ri2->name<<" is deleted.\n";
                reads->erase(ri2);
            }
            else
            {
                ri2++;
            }

            delete node;
        }

        ri1++;
    }

}
