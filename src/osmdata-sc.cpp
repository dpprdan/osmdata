/***************************************************************************
 *  Project:    osmdata
 *  File:       osmdata-sc.cpp
 *  Language:   C++
 *
 *  osmdata is free software: you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation, either version 3 of the License, or (at your option)
 *  any later version.
 *
 *  osmdata is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along with
 *  osm-router.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  Author:     Mark Padgham 
 *  E-Mail:     mark.padgham@email.com 
 *
 *  Description:    Extract OSM data from an object of class XmlData and return
 *                  it in Rcpp::List format.
 *
 *  Limitations:
 *
 *  Dependencies:       none (rapidXML header included in osmdata)
 *
 *  Compiler Options:   -std=c++11
 ***************************************************************************/

#include "osmdata.h"
#include "osmdata-sc.h"

#include <Rcpp.h>

// Note: This code uses explicit index counters within most loops which use Rcpp
// objects, because these otherwise require a 
// static_cast <size_t> (std::distance (...)). This operation copies each
// instance and can slow the loops down by several orders of magnitude!

/************************************************************************
 ************************************************************************
 **                                                                    **
 **          1. PRIMARY FUNCTIONS TO TRACE WAYS AND RELATIONS          **
 **                                                                    **
 ************************************************************************
 ************************************************************************/


//' get_osm_relations
//'
//' Return an Rcpp::List containing all OSM relations.
//'
//' @param rels Pointer to the vector of Relation objects
//' @param rel_df Pointer to return object containing the members of each
//'         relation.
//' @param rel_kv Pointer to return object containing key-value pairs for each
//'         relation.
//'
//' @noRd 
void osm_sc::get_osm_relations (Rcpp::DataFrame &rel_df, Rcpp::DataFrame &kv_df,
        const Relations &rels)
{
    std::vector <osmid_t> ids (rels.size ()); 

    size_t nrow_kv = 0, nrow_memb = 0;
    for (auto itr = rels.begin (); itr != rels.end (); ++itr)
    {
        nrow_kv += itr->key_val.size ();
        nrow_memb += itr->ways.size ();
    }

    Rcpp::CharacterMatrix kv_mat (Rcpp::Dimension (nrow_kv, 3));
    Rcpp::CharacterMatrix rel_mat (Rcpp::Dimension (nrow_memb, 3));
    // rel_mat has [rel ID, member ID, member role]

    unsigned int i_rel_mat = 0, i_kv_mat = 0; // explicit loop indices
    for (auto itr = rels.begin (); itr != rels.end (); ++itr)
    {
        Rcpp::checkUserInterrupt ();
        osm_str_vec relation_ways;
        std::vector <std::pair <std::string, std::string> > relation_kv;
        trace_relation (itr, relation_ways, relation_kv);

        unsigned int i = std::distance (rels.begin (), itr);
        ids [i] = itr->id;
        for (auto r = relation_ways.begin (); r != relation_ways.end (); ++r)
        {
            rel_mat (i_rel_mat, 0) = std::to_string (itr->id); // relation ID
            rel_mat (i_rel_mat, 1) = std::to_string (r->first); // ref ID of component obj
            rel_mat (i_rel_mat++, 2) = r->second; // role of component
        }

        for (auto k = itr->key_val.begin (); k != itr->key_val.end (); ++k)
        {
            kv_mat (i_kv_mat, 0) = std::to_string (itr->id);
            kv_mat (i_kv_mat, 1) = k->first;
            kv_mat (i_kv_mat++, 2) = k->second;
        }
    }
    rel_df = Rcpp::DataFrame::create (Rcpp::Named ("object_") = rel_mat (Rcpp::_, 0),
                                    Rcpp::Named ("ref") = rel_mat (Rcpp::_, 1),
                                    Rcpp::Named ("role") = rel_mat (Rcpp::_, 2),
                                    Rcpp::_["stringsAsFactors"] = false );

    kv_df = Rcpp::DataFrame::create (Rcpp::Named ("object_") = kv_mat (Rcpp::_, 0),
                                    Rcpp::Named ("key") = kv_mat (Rcpp::_, 1),
                                    Rcpp::Named ("value") = kv_mat (Rcpp::_, 2),
                                    Rcpp::_["stringsAsFactors"] = false );
}


// Function to generate IDs for the edges in each way
std::string random_id (size_t len) {
    auto randchar = []() -> char
    {
        const char charset[] = \
            "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
        const size_t max_index = (sizeof(charset) - 1);
        //return charset [ rand() % max_index ];
        size_t i = static_cast <size_t> (floor (Rcpp::runif (1) [0] * max_index));
        return charset [i];
    };
    std::string str (len, 0);
    std::generate_n (str.begin(), len, randchar);
    return str;
}

//' get_osm_ways
//'
//' @param edge Pointer to Rcpp::DataFrame to hold the SC::edge table
//' @param object_link_edge Pointer to Rcpp::DataFrame to hold the
//'         SC::object_linkedge table
//' @param kv_df Pointer to Rcpp::DataFrame to hold key-value pairs
//' @param ways Pointer to all ways in data set
//' 
//' @noRd 
void osm_sc::get_osm_ways (Rcpp::DataFrame &edge,
        Rcpp::DataFrame &object_link_edge, Rcpp::DataFrame &kv_df,
        const Ways &ways)
{
    const int length_ids = 10;

    Rcpp::RNGScope scope; // set random seed

    int nedges = 0, nkv = 0;
    for (auto wi = ways.begin (); wi != ways.end (); ++wi)
    {
        if (wi->second.nodes.size () > 0)
            nedges += wi->second.nodes.size () - 1;
        nkv += wi->second.key_val.size ();
    }

    Rcpp::CharacterMatrix edge_mat (Rcpp::Dimension (nedges, 3));
    Rcpp::CharacterMatrix object_link_edge_mat (Rcpp::Dimension (nedges, 2));
    Rcpp::CharacterMatrix kv_mat (Rcpp::Dimension (nkv, 3));

    // TODO: Impelement these properly with std::distance
    int count_w = 0, count_k = 0;
    for (auto wi = ways.begin (); wi != ways.end (); ++wi)
    {
        Rcpp::checkUserInterrupt ();

        auto first = wi->second.nodes.begin ();
        auto last = wi->second.nodes.empty () ? 
                wi->second.nodes.end () : std::prev (wi->second.nodes.end ());
        for (auto wj = first; wj != last; ++wj)
        {
            edge_mat (count_w, 0) = std::to_string (*wj);
            edge_mat (count_w, 1) = std::to_string (*std::next (wj));
            std::string idj = random_id (length_ids);
            edge_mat (count_w, 2) = idj;
            object_link_edge_mat (count_w, 0) = idj;
            object_link_edge_mat (count_w++, 1) = std::to_string (wi->first);
        }
        
        for (auto kj = wi->second.key_val.begin ();
                kj != wi->second.key_val.end (); ++kj)
        {
            kv_mat (count_k, 0) = std::to_string (wi->first);
            kv_mat (count_k, 1) = kj->first;
            kv_mat (count_k++, 2) = kj->second;
        }
    }
    edge = Rcpp::DataFrame::create (Rcpp::Named (".vx0") = edge_mat (Rcpp::_, 0),
                                    Rcpp::Named (".vx1") = edge_mat (Rcpp::_, 1),
                                    Rcpp::Named ("edge_") = edge_mat (Rcpp::_, 2),
                                    Rcpp::_["stringsAsFactors"] = false );

    object_link_edge = Rcpp::DataFrame::create (
            Rcpp::Named ("edge_") = object_link_edge_mat (Rcpp::_, 0),
            Rcpp::Named ("object_") = object_link_edge_mat (Rcpp::_, 1),
            Rcpp::_["stringsAsFactors"] = false );

    kv_df = Rcpp::DataFrame::create (
            Rcpp::Named ("object_") = kv_mat (Rcpp::_, 0),
            Rcpp::Named ("key") = kv_mat (Rcpp::_, 1),
            Rcpp::Named ("value") = kv_mat (Rcpp::_, 2),
            Rcpp::_["stringsAsFactors"] = false );
}

//' get_osm_nodes
//'
//' @param node_df Pointer to Rcpp::DataFrame to hold nodes
//' @param kv_df Pointer to Rcpp::DataFrame to hold key-value pairs
//' @param nodes Pointer to all nodes in data set
//' 
//' @noRd 
void osm_sc::get_osm_nodes (Rcpp::DataFrame &node_df, Rcpp::DataFrame &kv_df,
        const Nodes &nodes)
{
    const size_t nrow = nodes.size ();

    Rcpp::CharacterVector node_ids (nrow);
    Rcpp::NumericMatrix node_mat (nrow, 2); // lon-lat

    int nkeys = 0;
    for (auto ni = nodes.begin (); ni != nodes.end (); ++ni)
        nkeys += ni->second.key_val.size ();
    Rcpp::CharacterMatrix kv_mat (Rcpp::Dimension (nkeys, 3));

    int i = 0, keyj = 0;
    for (auto ni = nodes.begin (); ni != nodes.end (); ++ni)
    {
        if (i % 1000 == 0)
            Rcpp::checkUserInterrupt ();

        node_ids (i) = std::to_string (ni->first);
        node_mat (i, 0) = ni->second.lon;
        node_mat (i++, 1) = ni->second.lat;

        for (auto kv_iter = ni->second.key_val.begin ();
                kv_iter != ni->second.key_val.end (); ++kv_iter)
        {
            kv_mat (keyj, 0) = std::to_string (ni->first);
            kv_mat (keyj, 1) = kv_iter->first;
            kv_mat (keyj++, 2) = kv_iter->second;
        }
    }
    node_df = Rcpp::DataFrame::create (Rcpp::Named ("x_") = node_mat (Rcpp::_, 0),
                                    Rcpp::Named ("y_") = node_mat (Rcpp::_, 1),
                                    Rcpp::Named ("vertex_") = node_ids,
                                    Rcpp::_["stringsAsFactors"] = false );

    kv_df = Rcpp::DataFrame::create (
            Rcpp::Named ("object_") = kv_mat (Rcpp::_, 0),
            Rcpp::Named ("key") = kv_mat (Rcpp::_, 1),
            Rcpp::Named ("value") = kv_mat (Rcpp::_, 2),
            Rcpp::_["stringsAsFactors"] = false );
}


/************************************************************************
 ************************************************************************
 **                                                                    **
 **            THE FINAL RCPP FUNCTION CALLED BY osmdata_sc            **
 **                                                                    **
 ************************************************************************
 ************************************************************************/

//' rcpp_osmdata_sc
//'
//' Return OSM data in silicate (SC) format
//'
//' @param st Text contents of an overpass API query
//' @return Rcpp::List objects of OSM data
//' 
//' @noRd 
// [[Rcpp::export]]
Rcpp::List rcpp_osmdata_sc (const std::string& st)
{
#ifdef DUMP_INPUT
    {
        std::ofstream dump ("./osmdata-sf.xml");
        if (dump.is_open())
        {
            dump.write (st.c_str(), st.size());
        }
    }
#endif

    XmlDataSC xml (st);

    Rcpp::DataFrame vertex = Rcpp::DataFrame::create (
            Rcpp::Named ("x") = xml.vx (),
            Rcpp::Named ("y") = xml.vy (),
            Rcpp::Named ("vertex_") = xml.vert_id (),
            Rcpp::_["stringsAsFactors"] = false );

    Rcpp::DataFrame edge = Rcpp::DataFrame::create (
            Rcpp::Named (".vx0") = xml.vx0 (),
            Rcpp::Named (".vx1") = xml.vx1 (),
            Rcpp::Named ("edge_") = xml.edge (),
            Rcpp::_["stringsAsFactors"] = false );

    Rcpp::DataFrame oXe = Rcpp::DataFrame::create (
            Rcpp::Named ("edge_") = xml.edge (),
            Rcpp::Named ("object_") = xml.object (),
            Rcpp::_["stringsAsFactors"] = false );

    Rcpp::DataFrame obj_node = Rcpp::DataFrame::create (
            Rcpp::Named ("object_") = xml.node_id (),
            Rcpp::Named ("key") = xml.node_key (),
            Rcpp::Named ("val") = xml.node_val (),
            Rcpp::_["stringsAsFactors"] = false );

    Rcpp::DataFrame obj_way = Rcpp::DataFrame::create (
            Rcpp::Named ("object_") = xml.way_id (),
            Rcpp::Named ("key") = xml.way_key (),
            Rcpp::Named ("val") = xml.way_val (),
            Rcpp::_["stringsAsFactors"] = false );

    Rcpp::DataFrame obj_rel = Rcpp::DataFrame::create (
            Rcpp::Named ("object_") = xml.rel_id (),
            Rcpp::Named ("key") = xml.rel_key (),
            Rcpp::Named ("val") = xml.rel_val (),
            Rcpp::_["stringsAsFactors"] = false );

    Rcpp::List ret (6);
    ret [0] = vertex;
    ret [1] = edge;
    ret [2] = oXe;
    ret [3] = obj_node;
    ret [4] = obj_way;
    ret [5] = obj_rel;

    std::vector <std::string> retnames {"vertex", 
                                        "edge", "object_link_edge",
                                        "obj_node", "obj_way", "obj_rel"};
    ret.attr ("names") = retnames;
    
    return ret;
}