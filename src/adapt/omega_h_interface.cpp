#ifdef HAVE_OMEGA_H
#include "adapt/omega_h_interface.h"

#include <algorithm>
#include <iostream>
#include <sstream>

#include "Omega_h_align.hpp"
#include "Omega_h_array_ops.hpp"
#include "Omega_h_build.hpp"
#include "Omega_h_class.hpp"
#include "Omega_h_element.hpp"
#include "Omega_h_file.hpp"
#include "Omega_h_for.hpp"
#include "Omega_h_functors.hpp"
#include "Omega_h_library.hpp"
#include "Omega_h_map.hpp"
#include "Omega_h_mark.hpp"
#include "Omega_h_mesh.hpp"
#include <Omega_h_adapt.hpp>
#include <Omega_h_bbox.hpp>
#include <Omega_h_class.hpp>
#include <Omega_h_cmdline.hpp>
#include <Omega_h_compare.hpp>
#include <Omega_h_file.hpp>
#include <Omega_h_library.hpp>
#include <Omega_h_mesh.hpp>
#include <Omega_h_metric.hpp>
#include <numeric>
#include <vector>

extern "C" {
#define DISABLE_CPP
#ifndef MAX_PDIM
#define MAX_PDIM 3 /* Maximum physical problem dimension    */
#endif
#include "std.h"

#include "el_geom.h"

#include "rf_allo.h"

#include "rf_fem_const.h"
#include "rf_io.h"
#include "rf_io_const.h"

#include "rf_mp.h"

#include "mm_as.h"
#include "mm_as_const.h"
#include "mm_as_structs.h"
#include "mm_elem_block_structs.h"
#include "mm_mp_const.h"
#include "rf_bc.h"
#include "rf_bc_const.h"
#include "rf_element_storage_struct.h"
#include "rf_fill_const.h"
#include "rf_masks.h"
#include "rf_solver_const.h"
#include "rf_vars_const.h"

#include "mm_mp.h"
#include "mm_mp_structs.h"

#include "mm_species.h"

#include "mm_fill_jac.h"
#include "mm_interface.h"

#include "mm_post_def.h"

#include "mm_eh.h"

#include "dp_types.h"
#include "dpi.h"
#include "exo_struct.h"
#include <mm_bc.h>
#define IGNORE_CPP_DEFINE
#include "sl_util_structs.h"
#undef IGNORE_CPP_DEFINE
#include "adapt/resetup_problem.h"
#include "mm_as.h"
#include "mm_as_structs.h"
#include "mm_eh.h"
#include "mm_unknown_map.h"
#include "rd_dpi.h"
#include "rd_exo.h"
#include "rd_mesh.h"
#include "rf_allo.h"
#include "rf_bc.h"
#include "rf_io.h"
#include "rf_node_const.h"
#include "wr_exo.h"
extern int ***Local_Offset;
extern int ***Dolphin;
extern int *NumUnknowns;    /* Number of unknown variables updated by this   */
extern int *NumExtUnknowns; /* Number of unknown variables updated by this   */
extern int ***idv;
#undef DISABLE_CPP
}

namespace Omega_h {

#define CALL(f)                                                                          \
  do {                                                                                   \
    auto f_err = (f);                                                                    \
    if (f_err != 0) {                                                                    \
      const char *errmsg;                                                                \
      const char *errfunc;                                                               \
      int errnum;                                                                        \
      ex_get_err(&errmsg, &errfunc, &errnum);                                            \
      Omega_h_fail("Exodus call %s failed (%d): %s: %s\n", #f, errnum, errfunc, errmsg); \
    }                                                                                    \
  } while (0)

namespace exodus {
static OMEGA_H_INLINE int side_osh2exo(int dim, int side) {
  switch (dim) {
  case 2:
    switch (side) {
    case 1:
      return 1;
    case 2:
      return 2;
    case 0:
      return 3;
    }
    return -1;
  case 3:
    switch (side) {
    case 1:
      return 1;
    case 2:
      return 2;
    case 3:
      return 3;
    case 0:
      return 4;
    }
    return -1;
  }
  return -1;
}

void write_dull(
    filesystem::path const& path, Mesh* mesh, bool verbose, int classify_with) {
  begin_code("exodus::write");
  auto comp_ws = int(sizeof(Real));
  auto io_ws = comp_ws;
  auto mode = EX_CLOBBER | EX_MAPS_INT64_API;
  auto file = ex_create(path.c_str(), mode, &comp_ws, &io_ws);
  if (file < 0) Omega_h_fail("can't create Exodus file %s\n", path.c_str());
  auto title = "Omega_h " OMEGA_H_SEMVER " Exodus Output";
  std::set<LO> region_set;
  auto dim = mesh->dim();
  auto elem_class_ids = mesh->get_array<ClassId>(dim, "class_id");
  auto h_elem_class_ids = HostRead<LO>(elem_class_ids);
  for (LO i = 0; i < h_elem_class_ids.size(); ++i) {
    region_set.insert(h_elem_class_ids[i]);
  }
  auto side_class_ids = mesh->get_array<ClassId>(dim - 1, "class_id");
  auto side_class_dims = mesh->get_array<I8>(dim - 1, "class_dim");
  auto h_side_class_ids = HostRead<LO>(side_class_ids);
  auto h_side_class_dims = HostRead<I8>(side_class_dims);
  std::set<LO> surface_set;
  for (LO i = 0; i < h_side_class_ids.size(); ++i) {
    if (h_side_class_dims[i] == I8(dim - 1)) {
      surface_set.insert(h_side_class_ids[i]);
    }
  }
  auto nelem_blocks = int(region_set.size());
  auto nside_sets =
      (classify_with & exodus::SIDE_SETS) ? int(surface_set.size()) : 0;
  auto nnode_sets =
      (classify_with & exodus::NODE_SETS) ? int(surface_set.size()) : 0;
  if (verbose) {
    std::cout << "init params for " << path << ":\n";
    std::cout << " Exodus ID " << file << '\n';
    std::cout << " comp_ws " << comp_ws << '\n';
    std::cout << " io_ws " << io_ws << '\n';
    std::cout << " Title " << title << '\n';
    std::cout << " num_dim " << dim << '\n';
    std::cout << " num_nodes " << mesh->nverts() << '\n';
    std::cout << " num_elem " << mesh->nelems() << '\n';
    std::cout << " num_elem_blk " << nelem_blocks << '\n';
    std::cout << " num_node_sets " << nnode_sets << '\n';
    std::cout << " num_side_sets " << nside_sets << '\n';
  }
  CALL(ex_put_init(file, title, dim, mesh->nverts(), mesh->nelems(),
      nelem_blocks, nnode_sets, nside_sets));
  Few<Write<Real>, 3> coord_blk;
  for (Int i = 0; i < dim; ++i) coord_blk[i] = Write<Real>(mesh->nverts());
  auto coords = mesh->coords();
  auto f0 = OMEGA_H_LAMBDA(LO i) {
    for (Int j = 0; j < dim; ++j) coord_blk[j][i] = coords[i * dim + j];
  };
  parallel_for(mesh->nverts(), f0, "copy_coords");
  HostRead<Real> h_coord_blk[3];
  for (Int i = 0; i < dim; ++i) h_coord_blk[i] = HostRead<Real>(coord_blk[i]);
  CALL(ex_put_coord(file, h_coord_blk[0].data(), h_coord_blk[1].data(),
      h_coord_blk[2].data()));
  auto all_conn = mesh->ask_elem_verts();
  auto elems2file_idx = Write<LO>(mesh->nelems());
  auto elem_file_offset = LO(0);
  for (auto block_id : region_set) {
    auto type_name = (dim == 3) ? "tetra4" : "tri3";
    auto elems_in_block = each_eq_to(elem_class_ids, block_id);
    auto block_elems2elem = collect_marked(elems_in_block);
    auto nblock_elems = block_elems2elem.size();
    if (verbose) {
      std::cout << "element block " << block_id << " has " << nblock_elems
                << " of type " << type_name << '\n';
    }
    auto deg = element_degree(mesh->family(), dim, VERT);
    CALL(ex_put_block(
        file, EX_ELEM_BLOCK, block_id, type_name, nblock_elems, deg, 0, 0, 0));
    auto block_conn = read(unmap(block_elems2elem, all_conn, deg));
    auto block_conn_ex = add_to_each(block_conn, 1);
    auto h_block_conn = HostRead<LO>(block_conn_ex);
    CALL(ex_put_conn(
        file, EX_ELEM_BLOCK, block_id, h_block_conn.data(), nullptr, nullptr));
    auto f = OMEGA_H_LAMBDA(LO block_elem) {
      elems2file_idx[block_elems2elem[block_elem]] =
          elem_file_offset + block_elem;
    };
    parallel_for(nblock_elems, f);
    elem_file_offset += nblock_elems;
  }
  if (classify_with) {
    for (auto set_id : surface_set) {
      auto sides_in_set = land_each(each_eq_to(side_class_ids, set_id),
          each_eq_to(side_class_dims, I8(dim - 1)));
      if (classify_with & exodus::SIDE_SETS) {
        auto set_sides2side = collect_marked(sides_in_set);
        auto nset_sides = set_sides2side.size();
        if (verbose) {
          std::cout << "side set " << set_id << " has " << nset_sides
                    << " sides\n";
        }
        auto sides2elems = mesh->ask_up(dim - 1, dim);
        Write<int> set_sides2elem(nset_sides);
        Write<int> set_sides2local(nset_sides);
        auto f1 = OMEGA_H_LAMBDA(LO set_side) {
          auto side = set_sides2side[set_side];
          auto side_elem = sides2elems.a2ab[side];
          auto elem = sides2elems.ab2b[side_elem];
          auto elem_in_file = elems2file_idx[elem];
          auto code = sides2elems.codes[side_elem];
          auto which_down = code_which_down(code);
          set_sides2elem[set_side] = elem_in_file + 1;
          set_sides2local[set_side] = side_osh2exo(dim, which_down);
        };
        parallel_for(nset_sides, f1, "set_sides2elem");
        auto h_set_sides2elem = HostRead<int>(set_sides2elem);
        auto h_set_sides2local = HostRead<int>(set_sides2local);
        CALL(ex_put_set_param(file, EX_SIDE_SET, set_id, nset_sides, 0));
        CALL(ex_put_set(file, EX_SIDE_SET, set_id, h_set_sides2elem.data(),
            h_set_sides2local.data()));
      }
      if (classify_with & exodus::NODE_SETS) {
        auto nodes_in_set = mark_down(mesh, dim - 1, VERT, sides_in_set);
        auto set_nodes2node = collect_marked(nodes_in_set);
        auto set_nodes2node_ex = add_to_each(set_nodes2node, 1);
        auto nset_nodes = set_nodes2node.size();
        if (verbose) {
          std::cout << "node set " << set_id << " has " << nset_nodes
                    << " nodes\n";
        }
        auto h_set_nodes2node = HostRead<LO>(set_nodes2node_ex);
        CALL(ex_put_set_param(file, EX_NODE_SET, set_id, nset_nodes, 0));
        CALL(ex_put_set(
            file, EX_NODE_SET, set_id, h_set_nodes2node.data(), nullptr));
      }
    }
    std::vector<std::string> set_names(surface_set.size());
    for (auto& pair : mesh->class_sets) {
      auto& name = pair.first;
      for (auto& cp : pair.second) {
        if (cp.dim != I8(dim - 1)) continue;
        std::size_t index = 0;
        for (auto surface_id : surface_set) {
          if (surface_id == cp.id) {
            set_names[index] = name;
            if (verbose && (classify_with & exodus::NODE_SETS)) {
              std::cout << "node set " << surface_id << " will be called \""
                        << name << "\"\n";
            }
            if (verbose && (classify_with & exodus::SIDE_SETS)) {
              std::cout << "side set " << surface_id << " will be called \""
                        << name << "\"\n";
            }
          }
          ++index;
        }
      }
    }
    std::vector<char*> set_name_ptrs(surface_set.size(), nullptr);
    for (std::size_t i = 0; i < set_names.size(); ++i) {
      if (set_names[i].empty()) {
        std::stringstream ss;
        ss << "surface_" << i;
        set_names[i] = ss.str();
      }
      set_name_ptrs[i] = const_cast<char*>(set_names[i].c_str());
    }
    if (classify_with & exodus::NODE_SETS) {
      CALL(ex_put_names(file, EX_NODE_SET, set_name_ptrs.data()));
    }
    if (classify_with & exodus::SIDE_SETS) {
      CALL(ex_put_names(file, EX_SIDE_SET, set_name_ptrs.data()));
    }
  }
  CALL(ex_close(file));
  end_code();
}

static void setup_names(int nnames, std::vector<char> &storage, std::vector<char *> &ptrs) {
  constexpr auto max_name_length = MAX_STR_LENGTH + 1;
  storage = std::vector<char>(std::size_t(nnames * max_name_length), '\0');
  ptrs = std::vector<char *>(std::size_t(nnames), nullptr);
  for (int i = 0; i < nnames; ++i) {
    ptrs[std::size_t(i)] = storage.data() + max_name_length * i;
  }
}

static OMEGA_H_INLINE int side_exo2osh(Omega_h_Family family, int dim, int side) {
  switch (family) {
  case OMEGA_H_SIMPLEX:
    switch (dim) {
    case 2:
      // seeing files from CUBIT with triangle sides in {3,4,5}...
      // no clue what thats about, just modulo and move on
      return (side + 2) % 3;
    case 3:
      switch (side) {
      case 1:
        return 1;
      case 2:
        return 2;
      case 3:
        return 3;
      case 4:
        return 0;
      }
    }
    return -1;
  case OMEGA_H_HYPERCUBE:
    return -1; // needs to be filled in!
  }
  return -1;
}
static void get_elem_type_info(std::string const &type, int *p_dim, Omega_h_Family *p_family) {
  if (type == "tri3") {
    *p_dim = 2;
    *p_family = OMEGA_H_SIMPLEX;
  } else if (type == "TRI") {
    *p_dim = 2;
    *p_family = OMEGA_H_SIMPLEX;
  } else if (type == "TRI3") {
    *p_dim = 2;
    *p_family = OMEGA_H_SIMPLEX;
  } else if (type == "tetra4") {
    *p_dim = 3;
    *p_family = OMEGA_H_SIMPLEX;
  } else if (type == "TETRA") {
    *p_dim = 3;
    *p_family = OMEGA_H_SIMPLEX;
  } else if (type == "TET4") {
    *p_dim = 3;
    *p_family = OMEGA_H_SIMPLEX;
  } else {
    Omega_h_fail("Unsupported Exodus element type \"%s\"\n", type.c_str());
  }
}

void convert_to_omega_h_mesh_parallel(
    Exo_DB *exo, Dpi *dpi, int file, double **x, Mesh *mesh, bool verbose, int classify_with) {
  std::vector<LO> local_to_global(exo->num_nodes);
  for (int i = 0; i < exo->num_nodes; i++) {
    local_to_global[i] = dpi->node_index_global[i];
  }

  std::vector<LO> owned_elems;
  for (int i = 0; i < exo->num_elems; i++) {
    if (dpi->elem_owner[i] == ProcID) {
      owned_elems.push_back(i);
    }
  }

  //  for (auto e : owned_elems) {
  //    std::cout << "proc " << ProcID << " owns elem " << dpi->elem_index_global[e] << "\n";
  //  }
  int nnodes_per_elem = exo->eb_num_nodes_per_elem[0];

  std::set<LO> local_verts;
  for (auto e : owned_elems) {
    for (int i = 0; i < nnodes_per_elem; i++) {
      local_verts.insert(exo->eb_conn[0][e * nnodes_per_elem + i]);
    }
  }

  //  for (auto it : local_verts) {
  //    std::cout << "proc " << ProcID << " has local vert " << it << "\n";
  //  }

  std::vector<LO> old_locals(local_verts.begin(), local_verts.end());
  std::sort(old_locals.begin(), old_locals.end());
  std::vector<LO> new_local_verts(old_locals.size());
  std::iota(new_local_verts.begin(), new_local_verts.end(), 0);

  std::map<LO, LO> old_to_new;
  std::map<LO, LO> new_to_old;
  for (int i = 0; i < old_locals.size(); i++) {
    old_to_new.insert(std::pair<LO, LO>(old_locals[i], i));
    new_to_old.insert(std::pair<LO, LO>(i, old_locals[i]));
  }

  HostWrite<GO> vert_global;
  vert_global = decltype(vert_global)(GO(new_local_verts.size()), "global vertices");

  for (int i = 0; i < old_locals.size(); i++) {
    vert_global[i] = dpi->node_index_global[old_locals[i]];
  }

  HostWrite<LO> h_conn;
  h_conn = decltype(h_conn)(LO(owned_elems.size() * nnodes_per_elem), "host connectivity");

  for (int i = 0; i < owned_elems.size(); i++) {
    auto e = owned_elems[i];
    for (int j = 0; j < nnodes_per_elem; j++) {
      auto old_index = exo->eb_conn[0][e * nnodes_per_elem + j];
      auto new_index = old_to_new[old_index];
      h_conn[i * nnodes_per_elem + j] = new_index;
    }
  }

  auto dim = exo->num_dim;

  mesh->set_parting(OMEGA_H_ELEM_BASED);

  auto conn = LOs(h_conn.write());
  build_from_elems2verts(mesh, mesh->library()->world(), OMEGA_H_SIMPLEX, dim, conn,
                         GOs(vert_global));

  auto new_verts = mesh->globals(0);

  HostWrite<Real> h_coords(LO(new_verts.size() * dim));
  std::vector<LO> exo_from_omega;

  for (size_t i = 0; i < new_verts.size(); i++) {
    int idx = in_list(new_verts[i], 0, exo->num_nodes, dpi->node_index_global);
    assert(idx != -1);
    exo_from_omega.push_back(idx);
    h_coords[i * dim + 0] = exo->x_coord[idx];
    h_coords[i * dim + 1] = exo->y_coord[idx];
    if (exo->num_dim == 3)
      h_coords[i * dim + 2] = exo->z_coord[idx];
  }
  //  for (size_t i = 0; i < old_locals.size(); i++) {
  //    std::cout << "Proc " << ProcID << " coords " << i << " glob " << new_verts[i] << " ( "
  //              << h_coords[i * dim] << " , " << h_coords[i * dim + 1] << " )\n";
  //  }
  auto coords = Reals(h_coords.write());
  mesh->add_coords(coords);
  Write<LO> elem_class_ids_w(LO(owned_elems.size()));
  for (size_t i = 0; i < owned_elems.size(); i++) {
    elem_class_ids_w[i] = 1;
  }
  /*
  ex_init_params init_params;
  strncpy(init_params.title, exo->title, 80);
  init_params.num_dim = exo->num_dim;
  init_params.num_nodes = exo->num_nodes;
  init_params.num_elem = exo->num_elems;
  init_params.num_elem_blk = exo->num_elem_blocks;
  init_params.num_node_sets = exo->num_node_sets;
  init_params.num_side_sets = exo->num_side_sets;
  if (verbose) {
    std::cout << "init params:\n";
    std::cout << " Exodus ID " << file << '\n';
    std::cout << " Title " << init_params.title << '\n';
    std::cout << " num_dim " << init_params.num_dim << '\n';
    std::cout << " num_nodes " << init_params.num_nodes << '\n';
    std::cout << " num_elem " << init_params.num_elem << '\n';
    std::cout << " num_elem_blk " << init_params.num_elem_blk << '\n';
    std::cout << " num_node_sets " << init_params.num_node_sets << '\n';
    std::cout << " num_side_sets " << init_params.num_side_sets << '\n';
  }
  std::vector<int> block_ids(std::size_t(init_params.num_elem_blk));
  for (int i = 0; i < exo->num_elem_blocks; i++) {
    block_ids[i] = exo->eb_id[i];
  }

  //  CALL(ex_get_ids(file, EX_ELEM_BLOCK, block_ids.data()));
  std::vector<char> block_names_memory;
  std::vector<char *> block_names;
  setup_names(int(init_params.num_elem_blk), block_names_memory, block_names);
  //  CALL(ex_get_names(file, EX_ELEM_BLOCK, block_names.data()));
  HostWrite<LO> h_conn;
  Write<LO> elem_class_ids_w(LO(init_params.num_elem));
  LO elem_start = 0;
  int family_int = -1;
  int dim = -1;
  for (size_t i = 0; i < block_ids.size(); ++i) {
    char elem_type[MAX_STR_LENGTH + 1];
    elem_type[MAX_STR_LENGTH] = '\0';
    int nentries;
    int nnodes_per_entry;
    int nedges_per_entry = 0;
    int nfaces_per_entry = 0;
    strncpy(elem_type, exo->eb_elem_type[i], MAX_STR_LENGTH);
    nentries = exo->eb_num_elems[i];
    nnodes_per_entry = exo->eb_num_nodes_per_elem[i];

    //    CALL(ex_get_block(file, EX_ELEM_BLOCK, block_ids[i], elem_type, &nentries,
    //    &nnodes_per_entry,
    //                      &nedges_per_entry, &nfaces_per_entry, &nattr_per_entry));
    if (verbose) {
      std::cout << "block " << block_ids[i] << " \"" << block_names[i] << "\""
                << " has " << nentries << " elements of type " << elem_type << '\n';
    }
    if (std::string("NULL") == elem_type && nentries == 0)
      continue;
    int dim_from_type;
    Omega_h_Family family_from_type;
    get_elem_type_info(elem_type, &dim_from_type, &family_from_type);
    if (family_int == -1)
      family_int = family_from_type;
    OMEGA_H_CHECK(family_int == family_from_type);
    if (dim == -1)
      dim = dim_from_type;
    OMEGA_H_CHECK(dim == dim_from_type);
    auto deg = element_degree(Omega_h_Family(family_int), dim, VERT);
    OMEGA_H_CHECK(nnodes_per_entry == deg);
    if (!h_conn.exists())
      h_conn = decltype(h_conn)(LO(init_params.num_elem * deg), "host connectivity");
    if (nedges_per_entry < 0)
      nedges_per_entry = 0;
    if (nfaces_per_entry < 0)
      nfaces_per_entry = 0;
    //    CALL(ex_get_conn(file, EX_ELEM_BLOCK, block_ids[i],
    //                     h_conn.data() + elem_start * nnodes_per_entry, NULL,
    //                     NULL));
    for (int j = 0; j < nentries * nnodes_per_entry; j++) {
      h_conn.data()[j + elem_start * nnodes_per_entry] = exo->eb_conn[i][j];
      //      if (h_conn[j+elem_start * nnodes_per_entry]-1 != exo->eb_conn[i][j]) {
      //        std::cout << "Differing conn " << h_conn[j+elem_start*nnodes_per_entry] << " != " <<
      //        exo->eb_conn[i][j];
      //      }
    }
    auto region_id = block_ids[i];
    auto f0 = OMEGA_H_LAMBDA(LO entry) { elem_class_ids_w[elem_start + entry] = region_id; };
    parallel_for(nentries, f0, "set_elem_class_ids");
    mesh->class_sets[block_names[i]].push_back({I8(dim), region_id});
    elem_start += nentries;
  }
  OMEGA_H_CHECK(elem_start == init_params.num_elem);
  auto family = Omega_h_Family(family_int);
  auto conn = LOs(h_conn.write());
  HostWrite<GO> vert_global;
  vert_global = decltype(vert_global)(GO(dpi->num_internal_nodes + dpi->num_boundary_nodes +
  dpi->num_external_nodes), "global vertices"); for (auto i = 0; i < dpi->num_internal_nodes +
  dpi->num_boundary_nodes + dpi->num_external_nodes; i++) { vert_global.data()[i] =
  dpi->node_index_global[i];
  }
  //  build_from_elems_and_coords(mesh, OMEGA_H_SIMPLEX, dim, conn, coords);
  build_from_elems2verts(mesh, mesh->library()->world(), OMEGA_H_SIMPLEX, dim, conn,
                         GOs(vert_global));

  auto new_order_verts = mesh->globals(0);
  HostWrite<Real> h_coords(LO(init_params.num_nodes * dim));
  for (int node = 0; node < new_order_verts.size(); node++) {
    int exo_index = in_list(new_order_verts[node], 0, exo->num_nodes, dpi->node_index_global);
    assert(exo_index != -1);
    h_coords[node * dim + 0] = exo->x_coord[exo_index];
    h_coords[node * dim + 1] = exo->y_coord[exo_index];
    if (exo->num_dim == 3)
      h_coords[node * dim + 2] = exo->z_coord[exo_index];
  }
  auto coords = Reals(h_coords.write());
  mesh->add_coords(coords);
   */
  std::map<LO, LO> exo_to_global;
  for (int node = 0; node < mesh->globals(0).size(); node++) {
    int exo_index = in_list(mesh->globals(0)[node], 0, exo->num_nodes, dpi->node_index_global);
    exo_to_global.insert(std::pair<LO, LO>(exo_index, node));
  }
  for (int j = V_FIRST; j < V_LAST; j++) {
    int imtrx = upd->matrix_index[j];

    if (imtrx >= 0) {
      if (j == MASS_FRACTION) {
        for (int mf = 0; mf < upd->Max_Num_Species; mf++) {
          auto var_values = Omega_h::Write<Omega_h::Real>(mesh->nverts());
          for (int i = 0; i < exo->num_nodes; i++) {
            if (exo_to_global.find(i) != exo_to_global.end()) {
              auto gnode = exo_to_global[i];
              int ja = Index_Solution(i, j, mf, 0, -2, imtrx);
              EH(ja, "could not find solution");
              var_values[gnode] = x[imtrx][ja];
            }
          }
          std::string species_name = Exo_Var_Names[j].name2 + std::to_string(mf);
          mesh->add_tag(Omega_h::VERT, species_name, 1, Omega_h::Reals(var_values));
        }
      } else {
        auto var_values = Omega_h::Write<Omega_h::Real>(mesh->nverts());
        for (int i = 0; i < exo->num_nodes; i++) {
          if (exo_to_global.find(i) != exo_to_global.end()) {
            auto gnode = exo_to_global[i];
            int ja = Index_Solution(i, j, 0, 0, -2, imtrx);
            EH(ja, "could not find solution");
            var_values[gnode] = x[imtrx][ja];
          }
        }
        mesh->add_tag(Omega_h::VERT, Exo_Var_Names[j].name2, 1, Omega_h::Reals(var_values));
        if (j == FILL) {
          //        auto H_values = Omega_h::Write<Omega_h::Real>(mesh.nverts());
          //        auto f0 = OMEGA_H_LAMBDA(Omega_h::LO index) {
          //          H_values[index] =
          //              indicator(smooth_H(var_values[index], ls->Length_Scale),
          //              ls->Length_Scale);
          //        };
          //        Omega_h::parallel_for(mesh.nverts(), f0, "set_indicator_values");
          //        mesh.add_tag(Omega_h::VERT, "indicator", 1, Omega_h::Reals(H_values));
          auto target_metrics =
              Omega_h::Write<Omega_h::Real>(mesh->nverts() * Omega_h::symm_ncomps(mesh->dim()));
          auto f0 = OMEGA_H_LAMBDA(Omega_h::LO index) {
            auto F = var_values[index];
            auto iso_size = ls->adapt_outer_size;
            if (std::abs(F) < ls->adapt_width) {
              iso_size = ls->adapt_inner_size;
            }
            auto target_metric = Omega_h::compose_metric(Omega_h::identity_matrix<2, 2>(),
                                                         Omega_h::vector_2(iso_size, iso_size));
            Omega_h::set_vector(target_metrics, index, Omega_h::symm2vector(target_metric));
          };

          Omega_h::parallel_for(mesh->nverts(), f0, "set_iso_metric_values");
          mesh->add_tag(Omega_h::VERT, "iso_size_metric", Omega_h::symm_ncomps(mesh->dim()),
                        Omega_h::Reals(target_metrics));
        }
      }
    }
  }
  for (int w = 0; w < efv->Num_external_field; w++) {
    auto var_values = Omega_h::Write<Omega_h::Real>(mesh->nverts());
    for (int i = 0; i < exo->num_nodes; i++) {
      if (exo_to_global.find(i) != exo_to_global.end()) {
        auto gnode = exo_to_global[i];
        var_values[gnode] = efv->ext_fld_ndl_val[w][i];
      }
    }
    mesh->add_tag(Omega_h::VERT, efv->name[w], 1, Omega_h::Reals(var_values));
  }
  std::vector<int> side_set_ids(std::size_t(dpi->num_side_sets_global));
  for (int i = 0; i < dpi->num_side_sets_global; i++) {
    side_set_ids[i] = dpi->ss_id_global[i];
  }
  //  CALL(ex_get_ids(file, EX_SIDE_SET, side_set_ids.data()));
  Write<LO> side_class_ids_w(mesh->nents(dim - 1), -1);
  auto sides_are_exposed = mark_exposed_sides(mesh);
  classify_sides_by_exposure(mesh, sides_are_exposed);
  Write<I8> side_class_dims_w = deep_copy(mesh->get_array<I8>(dim - 1, "class_dim"));
  auto exposed_sides2side = collect_marked(sides_are_exposed);
  map_value_into(0, exposed_sides2side, side_class_ids_w);
#if 0
  if (dpi->num_side_sets_global) {
    int max_side_set_id = 0;
    if (side_set_ids.size()) {
      max_side_set_id = *std::max_element(side_set_ids.begin(), side_set_ids.end());
    }
    std::vector<int> node_set_ids(std::size_t(exo->num_node_sets));
    //    CALL(ex_get_ids(file, EX_NODE_SET, node_set_ids.data()));
    for (int i = 0; i < exo->num_node_sets; i++) {
      node_set_ids[i] = exo->ns_id[i];
    }
    std::vector<char> names_memory;
    std::vector<char *> name_ptrs;
    setup_names(int(dpi->num_node_sets_global + dpi->num_side_sets_global), names_memory, name_ptrs);
    //    CALL(ex_get_names(file, EX_NODE_SET, name_ptrs.data()));

    for (size_t i = 0; i < exo->num_node_sets; ++i) {
      int global_offset;
      for (global_offset = 0; global_offset < dpi->num_node_sets_global; global_offset++) {
        if (exo->ns_id[i] == dpi->ns_id_global[global_offset]) break;
      }
      assert(global_offset < dpi->num_node_sets_global);
      int nentries;
      //      CALL(ex_get_set_param(file, EX_NODE_SET, node_set_ids[i], &nentries, &ndist_factors));
      nentries = exo->ns_num_nodes[i];
      if (verbose) {
        std::cout << "node set " << node_set_ids[i] << " has " << nentries << " nodes\n";
      }

      int nnodes_owned = 0;
      for (int index = 0; index < nentries; index++) {
        int exoindex = exo->ns_node_list[exo->ns_node_index[i] + index];
        if (exo_to_global.find(exoindex) != exo_to_global.end()) {
          nnodes_owned++;
        }
      }

      HostWrite<LO> h_set_nodes2nodes(nnodes_owned);
      int index = 0;
      for (int idx = 0; idx < nentries; idx++) {
        int exoindex = exo->ns_node_list[exo->ns_node_index[i] + index];
        if (exo_to_global.find(exoindex) != exo_to_global.end()) {
          h_set_nodes2nodes[index] = exo_to_global.at(exoindex);
          index++;
        }
      }
      //      CALL(ex_get_set(file, EX_NODE_SET, node_set_ids[i], h_set_nodes2nodes.data(),
      //      nullptr));
      auto set_nodes2nodes = LOs(h_set_nodes2nodes.write());
      auto nodes_are_in_set = mark_image(set_nodes2nodes, mesh->nverts());
      auto sides_are_in_set = mark_up_all(mesh, VERT, dim - 1, nodes_are_in_set);
      auto set_sides2side = collect_marked(sides_are_in_set);
      auto surface_id = dpi->ns_id_global[global_offset] + dpi->num_node_sets_global;
      if (verbose) {
        std::cout << "node set #" << node_set_ids[i] << " \"" << name_ptrs[i]
                  << "\" will be surface " << surface_id << '\n';
      }
      map_value_into(surface_id, set_sides2side, side_class_ids_w);
      map_value_into(I8(dim - 1), set_sides2side, side_class_dims_w);
    }
    for (int offset = 0; offset < dpi->num_node_sets_global; offset++) {
      auto surface_id = dpi->ns_id_global[offset] + dpi->num_side_sets_global;
      mesh->class_sets[name_ptrs[offset]].push_back({I8(dim - 1), surface_id});
    }
  }
#endif
  if (1 && exo->num_side_sets) {
    std::vector<char> names_memory;
    std::vector<char *> name_ptrs;
    setup_names(int(dpi->num_side_sets_global), names_memory, name_ptrs);
    //    CALL(ex_get_names(file, EX_SIDE_SET, name_ptrs.data()));
    for (size_t i = 0; i < exo->num_side_sets; ++i) {
      int global_offset;
      for (global_offset = 0; global_offset < dpi->num_side_sets_global; global_offset++) {
        if (exo->ss_id[i] == dpi->ss_id_global[global_offset])
          break;
      }
      assert(global_offset < dpi->num_side_sets_global);
      int nsides;
      //      CALL(ex_get_set_param(file, EX_SIDE_SET, side_set_ids[i], &nsides, &ndist_factors));
      nsides = exo->ss_num_sides[i];
      int nnodes = 0;
      for (int side = 0; side < nsides; side++) {
        nnodes += exo->ss_node_cnt_list[i][side];
      }
      int nnodes_owned = 0;
      for (int side = 0; side < nnodes; side++) {
        int exoindex = exo->ss_node_list[i][side];
        if (exo_to_global.find(exoindex) != exo_to_global.end()) {
          nnodes_owned++;
        }
      }

      HostWrite<LO> h_set_nodes(nnodes_owned);
      int index = 0;
      for (int idx = 0; idx < nnodes; idx++) {
        int exoindex = exo->ss_node_list[i][idx];
        if (exo_to_global.find(exoindex) != exo_to_global.end()) {
          h_set_nodes[index] = exo_to_global.at(exoindex);
          index++;
        }
      }
      auto set_nodes2nodes = LOs(h_set_nodes.write());
      auto nodes_are_in_set = mark_image(set_nodes2nodes, mesh->nverts());
      auto sides_are_in_set = mark_up_all(mesh, VERT, dim - 1, nodes_are_in_set);
      auto set_sides2side = collect_marked(sides_are_in_set);
      auto surface_id = dpi->ss_id_global[global_offset];
      if (verbose) {
        std::cout << "P" << ProcID << " side set #" << surface_id << " \"" << name_ptrs[i]
                  << "\" has " << nsides << " sides, will be surface " << surface_id << "\n";

        std::cout << "P" << ProcID << " side set #" << surface_id << " nodes = ";
        for (int i = 0; i < h_set_nodes.size(); i++) {
          std::cout << mesh->globals(0).data()[h_set_nodes.data()[i]] << " ";
        }
        std::cout << "\n";
      }
      map_value_into(surface_id, set_sides2side, side_class_ids_w);
      map_value_into(I8(dim - 1), set_sides2side, side_class_dims_w);
      mesh->class_sets[name_ptrs[i]].push_back({I8(dim - 1), surface_id});
    }
    for (int offset = 0; offset < dpi->num_side_sets_global; offset++) {
      auto surface_id = dpi->ss_id_global[offset];
      mesh->class_sets[name_ptrs[offset]].push_back({I8(dim - 1), surface_id});
    }
  }
  auto elem_class_ids = LOs(elem_class_ids_w);
  auto side_class_ids = LOs(side_class_ids_w);
  auto side_class_dims = Read<I8>(side_class_dims_w);
  mesh->add_tag(dim, "class_id", 1, elem_class_ids);
  mesh->add_tag(dim - 1, "class_id", 1, side_class_ids);
  mesh->set_tag(dim - 1, "class_dim", side_class_dims);
  /*
  classify_elements(mesh);
  auto elem_class_ids = LOs(elem_class_ids_w);
  //auto side_class_ids = LOs(side_class_ids_w);
  //auto side_class_dims = Read<I8>(side_class_dims_w);
//  mesh->add_tag(dim, "class_id", 1, elem_class_ids);
  mesh->set_parting(OMEGA_H_GHOSTED);
  auto sides_are_exposed = mark_exposed_sides(mesh);
  classify_sides_by_exposure(mesh, sides_are_exposed);
  //mesh->add_tag(dim - 1, "class_id", 1, side_class_ids);
  //mesh->set_tag(dim - 1, "class_dim", side_class_dims);
  finalize_classification(mesh);
//  mesh->balance();
//  mesh->set_parting(OMEGA_H_GHOSTED);
   */
  classify_elements(mesh);
  //  auto elem_class_ids = LOs(elem_class_ids_w);
  // auto side_class_ids = LOs(side_class_ids_w);
  // auto side_class_dims = Read<I8>(side_class_dims_w);
  //  mesh->add_tag(dim, "class_id", 1, elem_class_ids);
  mesh->set_parting(OMEGA_H_GHOSTED);
  //  auto sides_are_exposed = mark_exposed_sides(mesh);
  //  classify_sides_by_exposure(mesh, sides_are_exposed);
  // mesh->add_tag(dim - 1, "class_id", 1, side_class_ids);
  // mesh->set_tag(dim - 1, "class_dim", side_class_dims);
  finalize_classification(mesh);
}

void convert_to_omega_h_mesh(
    Exo_DB *exo, Dpi *dpi, int file, Mesh *mesh, bool verbose, int classify_with) {
  begin_code("exodus::read_mesh");
  ex_init_params init_params;
  memcpy(init_params.title, exo->title, 81);
  init_params.num_dim = exo->num_dim;
  init_params.num_nodes = dpi->num_internal_nodes + dpi->num_boundary_nodes;
  init_params.num_elem = exo->num_elems;
  init_params.num_elem_blk = exo->num_elem_blocks;
  init_params.num_node_sets = exo->num_node_sets;
  init_params.num_side_sets = exo->num_side_sets;
  if (verbose) {
    std::cout << "init params:\n";
    std::cout << " Exodus ID " << file << '\n';
    std::cout << " Title " << init_params.title << '\n';
    std::cout << " num_dim " << init_params.num_dim << '\n';
    std::cout << " num_nodes " << init_params.num_nodes << '\n';
    std::cout << " num_elem " << init_params.num_elem << '\n';
    std::cout << " num_elem_blk " << init_params.num_elem_blk << '\n';
    std::cout << " num_node_sets " << init_params.num_node_sets << '\n';
    std::cout << " num_side_sets " << init_params.num_side_sets << '\n';
  }
  std::vector<int> block_ids(std::size_t(init_params.num_elem_blk));
  for (int i = 0; i < exo->num_elem_blocks; i++) {
    block_ids[i] = exo->eb_id[i];
  }

  //  CALL(ex_get_ids(file, EX_ELEM_BLOCK, block_ids.data()));
  std::vector<char> block_names_memory;
  std::vector<char *> block_names;
  setup_names(int(init_params.num_elem_blk), block_names_memory, block_names);
  //  CALL(ex_get_names(file, EX_ELEM_BLOCK, block_names.data()));
  HostWrite<LO> h_conn;
  Write<LO> elem_class_ids_w(LO(init_params.num_elem));
  LO elem_start = 0;
  int family_int = -1;
  int dim = -1;
  for (size_t i = 0; i < block_ids.size(); ++i) {
    char elem_type[MAX_STR_LENGTH + 1];
    elem_type[MAX_STR_LENGTH] = '\0';
    int nentries;
    int nnodes_per_entry;
    int nedges_per_entry = 0;
    int nfaces_per_entry = 0;
    memcpy(elem_type, exo->eb_elem_type[i], MAX_STR_LENGTH);
    nentries = exo->eb_num_elems[i];
    nnodes_per_entry = exo->eb_num_nodes_per_elem[i];

    //    CALL(ex_get_block(file, EX_ELEM_BLOCK, block_ids[i], elem_type, &nentries,
    //    &nnodes_per_entry,
    //                      &nedges_per_entry, &nfaces_per_entry, &nattr_per_entry));
    if (verbose) {
      std::cout << "block " << block_ids[i] << " \"" << block_names[i] << "\""
                << " has " << nentries << " elements of type " << elem_type << '\n';
    }
    /* some pretty weird blocks from the CDFEM people... */
    if (std::string("NULL") == elem_type && nentries == 0)
      continue;
    int dim_from_type;
    Omega_h_Family family_from_type;
    get_elem_type_info(elem_type, &dim_from_type, &family_from_type);
    if (family_int == -1)
      family_int = family_from_type;
    OMEGA_H_CHECK(family_int == family_from_type);
    if (dim == -1)
      dim = dim_from_type;
    OMEGA_H_CHECK(dim == dim_from_type);
    auto deg = element_degree(Omega_h_Family(family_int), dim, VERT);
    OMEGA_H_CHECK(nnodes_per_entry == deg);
    if (!h_conn.exists())
      h_conn = decltype(h_conn)(LO(init_params.num_elem * deg), "host connectivity");
    if (nedges_per_entry < 0)
      nedges_per_entry = 0;
    if (nfaces_per_entry < 0)
      nfaces_per_entry = 0;
    //    CALL(ex_get_conn(file, EX_ELEM_BLOCK, block_ids[i],
    //                     h_conn.data() + elem_start * nnodes_per_entry, NULL,
    //                     NULL));
    for (int j = 0; j < nentries * nnodes_per_entry; j++) {
      h_conn.data()[j + elem_start * nnodes_per_entry] = exo->eb_conn[i][j];
      //      if (h_conn[j+elem_start * nnodes_per_entry]-1 != exo->eb_conn[i][j]) {
      //        std::cout << "Differing conn " << h_conn[j+elem_start*nnodes_per_entry] << " != " <<
      //        exo->eb_conn[i][j];
      //      }
    }
    auto region_id = block_ids[i];
    auto f0 = OMEGA_H_LAMBDA(LO entry) { elem_class_ids_w[elem_start + entry] = region_id; };
    parallel_for(nentries, f0, "set_elem_class_ids");
    mesh->class_sets[block_names[i]].push_back({I8(dim), region_id});
    elem_start += nentries;
  }
  OMEGA_H_CHECK(elem_start == init_params.num_elem);
  Omega_h_Family family = Omega_h_Family(family_int);
  auto conn = LOs(h_conn.write());
  HostWrite<Real> h_coords(LO(init_params.num_nodes * dim));
  for (LO i = 0; i < exo->num_nodes; ++i) {
    for (Int j = 0; j < exo->num_dim; ++j) {
      h_coords[i * dim + 0] = exo->x_coord[i];
      h_coords[i * dim + 1] = exo->y_coord[i];
      if (exo->num_dim == 3)
        h_coords[i * dim + 2] = exo->z_coord[i];
    }
  }
  HostWrite<GO> vert_global;
  auto coords = Reals(h_coords.write());
  vert_global = decltype(vert_global)(GO(exo->num_nodes), "global vertices");
  for (auto i = 0; i < dpi->num_internal_nodes + dpi->num_boundary_nodes; i++) {
    vert_global.data()[i] = dpi->node_index_global[i];
  }
  //  build_from_elems_and_coords(mesh, OMEGA_H_SIMPLEX, dim, conn, coords);
  build_from_elems2verts(mesh, mesh->library()->world(), OMEGA_H_SIMPLEX, dim, conn,
                         GOs(vert_global));
  mesh->add_coords(coords);
  classify_elements(mesh);
  std::vector<int> side_set_ids(std::size_t(init_params.num_side_sets));
  for (int i = 0; i < exo->num_side_sets; i++) {
    side_set_ids[i] = exo->ss_id[i];
  }
  //  CALL(ex_get_ids(file, EX_SIDE_SET, side_set_ids.data()));
  Write<LO> side_class_ids_w(mesh->nents(dim - 1), -1);
  auto sides_are_exposed = mark_exposed_sides(mesh);
  classify_sides_by_exposure(mesh, sides_are_exposed);
  Write<I8> side_class_dims_w = deep_copy(mesh->get_array<I8>(dim - 1, "class_dim"));
  auto exposed_sides2side = collect_marked(sides_are_exposed);
  map_value_into(0, exposed_sides2side, side_class_ids_w);
  if ((classify_with & NODE_SETS) && init_params.num_node_sets) {
    int max_side_set_id = 0;
    if ((classify_with & SIDE_SETS) && side_set_ids.size()) {
      max_side_set_id = *std::max_element(side_set_ids.begin(), side_set_ids.end());
    }
    std::vector<int> node_set_ids(std::size_t(init_params.num_node_sets));
    //    CALL(ex_get_ids(file, EX_NODE_SET, node_set_ids.data()));
    for (int i = 0; i < exo->num_node_sets; i++) {
      node_set_ids[i] = exo->ns_id[i];
    }
    std::vector<char> names_memory;
    std::vector<char *> name_ptrs;
    setup_names(int(init_params.num_node_sets), names_memory, name_ptrs);
    //    CALL(ex_get_names(file, EX_NODE_SET, name_ptrs.data()));

    for (size_t i = 0; i < node_set_ids.size(); ++i) {
      int nentries;
      //      CALL(ex_get_set_param(file, EX_NODE_SET, node_set_ids[i], &nentries, &ndist_factors));
      nentries = exo->ns_num_nodes[i];
      if (verbose) {
        std::cout << "node set " << node_set_ids[i] << " has " << nentries << " nodes\n";
      }
      HostWrite<LO> h_set_nodes2nodes(nentries);
      //      CALL(ex_get_set(file, EX_NODE_SET, node_set_ids[i], h_set_nodes2nodes.data(),
      //      nullptr));
      auto f0 = OMEGA_H_LAMBDA(LO index) {
        h_set_nodes2nodes[index] = exo->ns_node_list[exo->ns_node_index[i] + index];
      };
      parallel_for(nentries, f0);
      auto set_nodes2nodes = LOs(h_set_nodes2nodes.write());
      auto nodes_are_in_set = mark_image(set_nodes2nodes, mesh->nverts());
      auto sides_are_in_set = mark_up_all(mesh, VERT, dim - 1, nodes_are_in_set);
      auto set_sides2side = collect_marked(sides_are_in_set);
      auto surface_id = node_set_ids[i] + max_side_set_id;
      if (verbose) {
        std::cout << "node set #" << node_set_ids[i] << " \"" << name_ptrs[i]
                  << "\" will be surface " << surface_id << '\n';
      }
      map_value_into(surface_id, set_sides2side, side_class_ids_w);
      map_value_into(I8(dim - 1), set_sides2side, side_class_dims_w);
      mesh->class_sets[name_ptrs[i]].push_back({I8(dim - 1), surface_id});
    }
  }
  if (classify_with & SIDE_SETS) {
    std::vector<char> names_memory;
    std::vector<char *> name_ptrs;
    setup_names(int(init_params.num_side_sets), names_memory, name_ptrs);
    //    CALL(ex_get_names(file, EX_SIDE_SET, name_ptrs.data()));
    for (size_t i = 0; i < side_set_ids.size(); ++i) {
      int nentries;
      //      CALL(ex_get_set_param(file, EX_SIDE_SET, side_set_ids[i], &nentries, &ndist_factors));
      nentries = exo->ss_num_sides[i];
      if (verbose) {
        std::cout << "side set #" << side_set_ids[i] << " \"" << name_ptrs[i] << "\" has "
                  << nentries << " sides, will be surface " << side_set_ids[i] << "\n";
      }
      HostWrite<LO> h_set_sides2elem(nentries);
      HostWrite<LO> h_set_sides2local(nentries);
      //      CALL(ex_get_set(file, EX_SIDE_SET, side_set_ids[i], h_set_sides2elem.data(),
      //                      h_set_sides2local.data()));
      auto f0 = OMEGA_H_LAMBDA(LO index) {
        int offset = exo->ss_elem_index[i];
        h_set_sides2elem[index] = exo->ss_elem_list[offset + index];
        h_set_sides2local[index] = exo->ss_side_list[offset + index];
      };
      parallel_for(nentries, f0);
      auto set_sides2elem = LOs(h_set_sides2elem.write());
      auto set_sides2local = LOs(h_set_sides2local.write());
      auto elems2sides = mesh->ask_down(dim, dim - 1).ab2b;
      auto nsides_per_elem = element_degree(family, dim, dim - 1);
      auto set_sides2side_w = Write<LO>(nentries);
      auto f2 = OMEGA_H_LAMBDA(LO set_side) {
        auto elem = set_sides2elem[set_side];
        auto side_of_element = side_exo2osh(family, dim, set_sides2local[set_side]);
        OMEGA_H_CHECK(side_of_element != -1);
        auto side = elems2sides[elem * nsides_per_elem + side_of_element];
        set_sides2side_w[set_side] = side;
      };
      parallel_for(nentries, f2, "set_sides2side");
      auto set_sides2side = LOs(set_sides2side_w);
      auto surface_id = side_set_ids[i];
      map_value_into(surface_id, set_sides2side, side_class_ids_w);
      map_value_into(I8(dim - 1), set_sides2side, side_class_dims_w);
      mesh->class_sets[name_ptrs[i]].push_back({I8(dim - 1), surface_id});
    }
  }
  auto elem_class_ids = LOs(elem_class_ids_w);
  auto side_class_ids = LOs(side_class_ids_w);
  auto side_class_dims = Read<I8>(side_class_dims_w);
  mesh->add_tag(dim, "class_id", 1, elem_class_ids);
  mesh->add_tag(dim - 1, "class_id", 1, side_class_ids);
  mesh->set_tag(dim - 1, "class_dim", side_class_dims);
  finalize_classification(mesh);
  end_code();
}


void convert_back_to_goma_exo_parallel(
    const char *path, Mesh *mesh, Exo_DB *exo, Dpi *dpi, bool verbose, int classify_with) {

  //  Omega_h::exodus::write(std::to_string(ProcID) + "tmp.e", mesh, true, classify_with);
  char out_par[MAX_FNL];
  strncpy(out_par, "tmp_oh.e", MAX_FNL-1);
  multiname(out_par, ProcID, Num_Proc);
  mesh->set_parting(OMEGA_H_ELEM_BASED);
  Omega_h::exodus::write(out_par, mesh, true, classify_with);
  mesh->set_parting(OMEGA_H_GHOSTED);
  strncpy(out_par, "tmp.e", MAX_FNL-1);
  multiname(out_par, ProcID, Num_Proc);
  auto comp_ws = int(sizeof(Real));
  auto io_ws = comp_ws;
  auto exoid = ex_create(out_par, EX_CLOBBER, &comp_ws, &io_ws);
  std::set<LO> region_set;
  auto dim = mesh->dim();
  auto title = "Omega_h " OMEGA_H_SEMVER " Exodus Output";
  auto elem_class_ids = mesh->get_array<ClassId>(dim, "class_id");
  auto h_elem_class_ids = HostRead<LO>(elem_class_ids);
  for (LO i = 0; i < h_elem_class_ids.size(); ++i) {
    region_set.insert(h_elem_class_ids[i]);
  }
  auto side_class_ids = mesh->get_array<ClassId>(dim - 1, "class_id");
  auto side_class_dims = mesh->get_array<I8>(dim - 1, "class_dim");
  auto h_side_class_ids = HostRead<LO>(side_class_ids);
  auto h_side_class_dims = HostRead<I8>(side_class_dims);
  auto nelem_blocks = int(region_set.size());
  std::set<LO> surface_set;
  for (LO i = 0; i < h_side_class_ids.size(); ++i) {
    if (h_side_class_dims[i] == I8(dim - 1)) {
      surface_set.insert(h_side_class_ids[i]);
    }
  }
  auto nside_sets =
      (classify_with & exodus::SIDE_SETS) ? int(surface_set.size()) : 0;
  auto nnode_sets =
      (classify_with & exodus::NODE_SETS) ? int(surface_set.size()) : 0;
  //  Omega_h::binary::write("tmp.osh", mesh);

  ex_put_init(exoid, title, dim, mesh->nverts(), mesh->nelems(), nelem_blocks, nnode_sets, nside_sets);

  Few<Write<Real>, 3> coord_blk;
  for (Int i = 0; i < dim; ++i) coord_blk[i] = Write<Real>(mesh->nverts());
  auto coords = mesh->coords();
  auto f0 = OMEGA_H_LAMBDA(LO i) {
    for (Int j = 0; j < dim; ++j) coord_blk[j][i] = coords[i * dim + j];
  };
  parallel_for(mesh->nverts(), f0, "copy_coords");
  HostRead<Real> h_coord_blk[3];
  for (Int i = 0; i < dim; ++i) h_coord_blk[i] = HostRead<Real>(coord_blk[i]);
  CALL(ex_put_coord(exoid, h_coord_blk[0].data(), h_coord_blk[1].data(),
                      h_coord_blk[2].data()));
  auto all_conn = mesh->ask_elem_verts();
  auto elems2file_idx = Write<LO>(mesh->nelems());
  auto elem_file_offset = LO(0);

  for (auto block_id : region_set) {
    auto type_name = (dim == 3) ? "tetra4" : "tri3";
    auto elems_in_block = each_eq_to(elem_class_ids, block_id);
    auto block_elems2elem = collect_marked(elems_in_block);
    auto nblock_elems = block_elems2elem.size();
    if (verbose) {
      std::cout << "element block " << block_id << " has " << nblock_elems
                << " of type " << type_name << '\n';
    }
    auto deg = element_degree(mesh->family(), dim, VERT);
    CALL(ex_put_block(
        exoid, EX_ELEM_BLOCK, block_id, type_name, nblock_elems, deg, 0, 0, 0));
    auto block_conn = read(unmap(block_elems2elem, all_conn, deg));
    auto block_conn_ex = add_to_each(block_conn, 1);
    auto h_block_conn = HostRead<LO>(block_conn_ex);
    CALL(ex_put_conn(
        exoid, EX_ELEM_BLOCK, block_id, h_block_conn.data(), nullptr, nullptr));
    auto f = OMEGA_H_LAMBDA(LO block_elem) {
      elems2file_idx[block_elems2elem[block_elem]] =
          elem_file_offset + block_elem;
    };
    parallel_for(nblock_elems, f);
    elem_file_offset += nblock_elems;
  }
  if (classify_with) {
    for (auto set_id : surface_set) {
      auto sides_in_set = land_each(each_eq_to(side_class_ids, set_id),
                                    each_eq_to(side_class_dims, I8(dim - 1)));
      if (classify_with & exodus::SIDE_SETS) {
        auto set_sides2side = collect_marked(sides_in_set);
        auto nset_sides = set_sides2side.size();
        if (verbose) {
          std::cout << "side set " << set_id << " has " << nset_sides
                    << " sides\n";
        }
        auto sides2elems = mesh->ask_up(dim - 1, dim);
        Write<int> set_sides2elem(nset_sides);
        Write<int> set_sides2local(nset_sides);
        auto f1 = OMEGA_H_LAMBDA(LO set_side) {
          auto side = set_sides2side[set_side];
          auto side_elem = sides2elems.a2ab[side];
          auto elem = sides2elems.ab2b[side_elem];
          auto elem_in_file = elems2file_idx[elem];
          auto code = sides2elems.codes[side_elem];
          auto which_down = code_which_down(code);
          set_sides2elem[set_side] = elem_in_file + 1;
          set_sides2local[set_side] = side_osh2exo(dim, which_down);
        };
        parallel_for(nset_sides, f1, "set_sides2elem");
        auto h_set_sides2elem = HostRead<int>(set_sides2elem);
        auto h_set_sides2local = HostRead<int>(set_sides2local);
        CALL(ex_put_set_param(exoid, EX_SIDE_SET, set_id, nset_sides, 0));
        CALL(ex_put_set(exoid, EX_SIDE_SET, set_id, h_set_sides2elem.data(),
                        h_set_sides2local.data()));
      }
      if (classify_with & exodus::NODE_SETS) {
        auto nodes_in_set = mark_down(mesh, dim - 1, VERT, sides_in_set);
        auto set_nodes2node = collect_marked(nodes_in_set);
        auto set_nodes2node_ex = add_to_each(set_nodes2node, 1);
        auto nset_nodes = set_nodes2node.size();
        if (verbose) {
          std::cout << "node set " << set_id << " has " << nset_nodes
                    << " nodes\n";
        }
        auto h_set_nodes2node = HostRead<LO>(set_nodes2node_ex);
        CALL(ex_put_set_param(exoid, EX_NODE_SET, set_id, nset_nodes, 0));
        CALL(ex_put_set(
            exoid, EX_NODE_SET, set_id, h_set_nodes2node.data(), nullptr));
      }
    }
    std::vector<std::string> set_names(surface_set.size());
    for (auto& pair : mesh->class_sets) {
      auto& name = pair.first;
      for (auto& cp : pair.second) {
        if (cp.dim != I8(dim - 1)) continue;
        std::size_t index = 0;
        for (auto surface_id : surface_set) {
          if (surface_id == cp.id) {
            set_names[index] = name;
            if (verbose && (classify_with & exodus::NODE_SETS)) {
              std::cout << "node set " << surface_id << " will be called \""
                        << name << "\"\n";
            }
            if (verbose && (classify_with & exodus::SIDE_SETS)) {
              std::cout << "side set " << surface_id << " will be called \""
                        << name << "\"\n";
            }
          }
          ++index;
        }
      }
    }
    std::vector<char*> set_name_ptrs(surface_set.size(), nullptr);
    for (std::size_t i = 0; i < set_names.size(); ++i) {
      if (set_names[i].empty()) {
        std::stringstream ss;
        ss << "surface_" << i;
        set_names[i] = ss.str();
      }
      set_name_ptrs[i] = const_cast<char*>(set_names[i].c_str());
    }
    if (classify_with & exodus::NODE_SETS) {
      CALL(ex_put_names(exoid, EX_NODE_SET, set_name_ptrs.data()));
    }
    if (classify_with & exodus::SIDE_SETS) {
      CALL(ex_put_names(exoid, EX_SIDE_SET, set_name_ptrs.data()));
    }
  }
  auto node_map = Write<LO>(mesh->nverts());
  auto elem_map = Write<LO>(mesh->nelems());
  for (int i = 0; i < mesh->globals(0).size(); i++) {
    node_map[i] = mesh->globals(0)[i];
  }
  for (int i = 0; i < mesh->globals(0).size(); i++) {
    elem_map[i] = mesh->globals(mesh->dim())[i];
  }

  ex_put_id_map(exoid, EX_NODE_MAP, node_map.data());
  ex_put_id_map(exoid, EX_ELEM_MAP, elem_map.data());

  CALL(ex_close(exoid));

  for (int imtrx = 0; imtrx < upd->Total_Num_Matrices; imtrx++) {
    free(idv[imtrx]);
  }
  free(idv);
  idv = NULL;

  free_Surf_BC(First_Elem_Side_BC_Array, exo);
  free_Edge_BC(First_Elem_Edge_BC_Array, exo, dpi);
  free_nodes();
  free_dpi_uni(dpi);
  free_exo(exo);
  init_exo_struct(exo);
  init_dpi_struct(dpi);

  //  const char * tmpfile = "tmp.e";
  strncpy(ExoFile, "tmp.e", 127);
  strncpy(ExoFileOutMono, path, 127);
  strncpy(ExoFileOut, path, 127);
  int num_total_nodes = dpi->num_internal_nodes + dpi->num_boundary_nodes + dpi->num_external_nodes;

  for (int imtrx = 0; imtrx < upd->Total_Num_Matrices; imtrx++) {
    for (int i = 0; i < num_total_nodes; i++) {
      free(Local_Offset[imtrx][i]);
      free(Dolphin[imtrx][i]);
    }
    free(Dolphin[imtrx]);
    free(Local_Offset[imtrx]);
  }
  safer_free((void **)&Local_Offset);
  safer_free((void **)&Dolphin);

  read_mesh_exoII(exo, dpi);
  one_base(exo);
  wr_mesh_exo(exo, ExoFileOutMono, 0);
  zero_base(exo);

  end_code();
}
void convert_back_to_goma_exo(
    const char *path, Mesh *mesh, Exo_DB *exo, Dpi *dpi, bool verbose, int classify_with) {

  //  Omega_h::exodus::write(std::to_string(ProcID) + "tmp.e", mesh, true, classify_with);
  Omega_h::exodus::write("tmp.e", mesh, true, classify_with);
  //  Omega_h::binary::write("tmp.osh", mesh);

  for (int imtrx = 0; imtrx < upd->Total_Num_Matrices; imtrx++) {
    free(idv[imtrx]);
  }
  free(idv);
  idv = NULL;

  free_Surf_BC(First_Elem_Side_BC_Array, exo);
  free_Edge_BC(First_Elem_Edge_BC_Array, exo, dpi);
  free_nodes();
  free_dpi_uni(dpi);
  free_exo(exo);
  init_exo_struct(exo);
  init_dpi_struct(dpi);

  //  const char * tmpfile = "tmp.e";
  strncpy(ExoFile, "tmp.e", 127);
  strncpy(ExoFileOutMono, path, 127);
  strncpy(ExoFileOut, path, 127);
  int num_total_nodes = dpi->num_internal_nodes + dpi->num_boundary_nodes + dpi->num_external_nodes;

  for (int imtrx = 0; imtrx < upd->Total_Num_Matrices; imtrx++) {
    for (int i = 0; i < num_total_nodes; i++) {
      free(Local_Offset[imtrx][i]);
      free(Dolphin[imtrx][i]);
    }
    free(Dolphin[imtrx]);
    free(Local_Offset[imtrx]);
  }
  safer_free((void **)&Local_Offset);
  safer_free((void **)&Dolphin);

  read_mesh_exoII(exo, dpi);
  one_base(exo);
  wr_mesh_exo(exo, ExoFileOutMono, 0);
  zero_base(exo);

  end_code();
}
// int open(filesystem::path const& path, bool verbose) {
//  auto comp_ws = int(sizeof(Real));
//  int io_ws = 0;
//  float version;
//  auto mode = EX_READ | EX_MAPS_INT64_API;
//  auto exodus_file = ex_open(path.c_str(), mode, &comp_ws, &io_ws, &version);
//  if (exodus_file < 0)
//    Omega_h_fail("can't open Exodus file %s\n", path.c_str());
//  if (verbose) {
//    std::cout << "ex_open(" << path << ")\n";
//    std::cout << "  comp_ws: " << comp_ws << '\n';
//    std::cout << "  io_ws: " << io_ws << '\n';
//    std::cout << "  version: " << version << '\n';
//  }
//  return exodus_file;
//}
} // namespace exodus
} // namespace Omega_h

#if 0
static int exo_side_to_osh(int side, int dim) {
  switch (dim) {
  case 2:
    return (side + 1) % 3;
  case 3:
    return (side + 1) % 4;
  default:
    EH(GOMA_ERROR, "Unknown dim exo_side_to_osh");
    return -1;
  }
}

static double smooth_H(double F, double eps) {
  if (F > eps) {
    return 1;
  } else if (F < -eps) {
    return 0;
  }
  return 0.5 * (1. + F / eps + sin(M_PI * F / eps) / M_PI);
}

static double indicator(double phi, double eps) { return (1.0 / eps) * phi * (1 - phi); }
#endif

void adapt_mesh(Omega_h::Mesh &mesh) {
  Omega_h::MetricInput genopts;
  //  genopts.sources.push_back(
  //      Omega_h::MetricSource{OMEGA_H_VARIATION, 1e-3, "phi", OMEGA_H_ISO_SIZE});
  genopts.sources.push_back(Omega_h::MetricSource{OMEGA_H_GIVEN, 1.0, "iso_size_metric",
                                                  OMEGA_H_ISO_SIZE, OMEGA_H_ABSOLUTE});
  //      genopts.sources.push_back(Omega_h::MetricSource{OMEGA_H_GIVEN, 1.0,
  //    "initial_metric",OMEGA_H_ISO_SIZE,OMEGA_H_ABSOLUTE});
  genopts.should_limit_lengths = true;
  genopts.min_length = 1e-6;
  genopts.max_length = 0.6;
  genopts.should_limit_gradation = true;
  genopts.max_gradation_rate = 0.3;
  Omega_h::add_implied_isos_tag(&mesh);
  Omega_h::generate_target_metric_tag(&mesh, genopts);

  Omega_h::AdaptOpts opts(&mesh);
  for (int j = V_FIRST; j < V_LAST; j++) {
    int imtrx = upd->matrix_index[j];
    if (imtrx >= 0) {
      if (j == MASS_FRACTION) {
        for (int mf = 0; mf < upd->Max_Num_Species; mf++) {
          std::string species_name = Exo_Var_Names[j].name2 + std::to_string(mf);
          opts.xfer_opts.type_map[species_name] = OMEGA_H_LINEAR_INTERP;
        }
      } else {
        opts.xfer_opts.type_map[Exo_Var_Names[j].name2] = OMEGA_H_LINEAR_INTERP;
      }
    }
  }
  for (int w = 0; w < efv->Num_external_field; w++) {
    opts.xfer_opts.type_map[efv->name[w]] = OMEGA_H_LINEAR_INTERP;
  }
  opts.max_length_allowed = 5;
  opts.max_length_desired = 1.6;
  opts.should_coarsen_slivers = true;
  opts.should_refine = true;
  opts.min_quality_desired = 0.6;
  int count = 1000;

  for (int i = 0; i < count; i++) {
    if (Omega_h::approach_metric(&mesh, opts)) {
      Omega_h::adapt(&mesh, opts);
    } else {
      break;
    }
  }
  auto imb = mesh.imbalance();
  std::cout << "Mesh imbalance = " << imb << "\n";
  mesh.balance();
  imb = mesh.imbalance();
  std::cout << "Mesh imbalance after balance = " << imb << "\n";
}

extern "C" {

void copy_solution(Exo_DB *exo, Dpi *dpi, double **x, Omega_h::Mesh &mesh) {
  for (int j = V_FIRST; j < V_LAST; j++) {
    int imtrx = upd->matrix_index[j];
    if (imtrx >= 0) {
      if (j == MASS_FRACTION) {
        for (int mf = 0; mf < upd->Max_Num_Species; mf++) {
          std::string species_name = Exo_Var_Names[j].name2 + std::to_string(mf);
          auto var_values = mesh.get_array<Omega_h::Real>(Omega_h::VERT, species_name);

          for (int i = 0; i < dpi->num_internal_nodes + dpi->num_boundary_nodes; i++) {
            int ja = Index_Solution(i, j, mf, 0, -2, imtrx);
            EH(ja, "could not find solution");
            x[imtrx][ja] = var_values[i];
          }
        }
      } else {
        auto var_values = mesh.get_array<Omega_h::Real>(Omega_h::VERT, Exo_Var_Names[j].name2);

        for (int i = 0; i < dpi->num_internal_nodes + dpi->num_boundary_nodes; i++) {
          int ja = Index_Solution(i, j, 0, 0, -2, imtrx);
          EH(ja, "could not find solution");
          x[imtrx][ja] = var_values[i];
        }
      }
    }
  }
  for (int w = 0; w < efv->Num_external_field; w++) {
    auto var_values = mesh.get_array<Omega_h::Real>(Omega_h::VERT, efv->name[w]);
    for (int i = 0; i < dpi->num_internal_nodes + dpi->num_boundary_nodes; i++) {
      efv->ext_fld_ndl_val[w][i] = var_values[i];
    }
  }
}

// start with just level set field
void adapt_mesh_omega_h(struct Aztec_Linear_Solver_System **ams,
                        Exo_DB *exo,
                        Dpi *dpi,
                        double **x,
                        double **x_old,
                        double **x_older,
                        double **xdot,
                        double **xdot_old,
                        double **x_oldest,
                        double **resid_vector,
                        double **x_update,
                        double **scale,
                        int step) {

  static std::string base_name;
  static bool first_call = true;
  static auto lib = Omega_h::Library();
  auto classify_with = Omega_h::exodus::NODE_SETS | Omega_h::exodus::SIDE_SETS;
  auto verbose = true;
  Omega_h::Mesh mesh(&lib);

  auto exodus_file = 0; // Omega_h::exodus::open("cup.g", verbose);
  Omega_h::exodus::convert_to_omega_h_mesh_parallel(exo, dpi, exodus_file, x, &mesh, verbose,
                                                    classify_with);
  //  if (lib.world()->size() > 1) {
  //  } else {
  //    Omega_h::exodus::convert_to_omega_h_mesh(exo, dpi, exodus_file, &mesh, verbose,
  //    classify_with);
  //  }
  mesh.set_parting(OMEGA_H_ELEM_BASED);
  Omega_h::exodus::write(std::to_string(ProcID) + "convert.e", &mesh, true, classify_with);
  mesh.set_parting(OMEGA_H_GHOSTED);
  auto writer_c = Omega_h::vtk::Writer("convert.vtk", &mesh);

  writer_c.write(step);
  //  std::vector<int> gnode(mesh.nverts());
  ////  for (int node = 0; node < mesh.globals(0).size(); node++) {
  ////    int exo_index = in_list(mesh.globals(0)[node], 0, exo->num_nodes, dpi->node_index_global);
  ////    gnode[exo_index] = node;
  ////  }
  //////  for (int j = V_FIRST; j < V_LAST; j++) {
  //    int imtrx = upd->matrix_index[j];
  //
  //    if (imtrx >= 0) {
  //      auto var_values = Omega_h::Write<Omega_h::Real>(mesh.nverts());
  //      for (int i = 0; i < dpi->num_internal_nodes + dpi->num_boundary_nodes +
  //      dpi->num_external_nodes; i++) {
  //        int ja = Index_Solution(i, j, 0, 0, -2, imtrx);
  //        EH(ja, "could not find solution");
  //        var_values[gnode[i]] = x[imtrx][ja];
  //      }
  //      mesh.add_tag(Omega_h::VERT, Exo_Var_Names[j].name2, 1, Omega_h::Reals(var_values));
  //      if (j == FILL) {
  //        //        auto H_values = Omega_h::Write<Omega_h::Real>(mesh.nverts());
  //        //        auto f0 = OMEGA_H_LAMBDA(Omega_h::LO index) {
  //        //          H_values[index] =
  //        //              indicator(smooth_H(var_values[index], ls->Length_Scale),
  //        ls->Length_Scale);
  //        //        };
  //        //        Omega_h::parallel_for(mesh.nverts(), f0, "set_indicator_values");
  //        //        mesh.add_tag(Omega_h::VERT, "indicator", 1, Omega_h::Reals(H_values));
  //        auto target_metrics =
  //            Omega_h::Write<Omega_h::Real>(mesh.nverts() * Omega_h::symm_ncomps(mesh.dim()));
  //        auto f0 = OMEGA_H_LAMBDA(Omega_h::LO index) {
  //          auto F = var_values[index];
  //          auto iso_size = 0.3;
  //          if (std::abs(F) < 0.5) {
  //            iso_size = 0.02;
  //          }
  //          auto target_metric = Omega_h::compose_metric(Omega_h::identity_matrix<2, 2>(),
  //                                                       Omega_h::vector_2(iso_size, iso_size));
  //          Omega_h::set_vector(target_metrics, index, Omega_h::symm2vector(target_metric));
  //        };
  //
  //        Omega_h::parallel_for(mesh.nverts(), f0, "set_iso_metric_values");
  //        mesh.add_tag(Omega_h::VERT, "iso_size_metric", Omega_h::symm_ncomps(mesh.dim()),
  //                     Omega_h::Reals(target_metrics));
  //      }
  //    }
  //  }
  //  for (int w=0; w<efv->Num_external_field; w++) {
  //      auto var_values = Omega_h::Write<Omega_h::Real>(mesh.nverts());
  //      for (int i = 0; i < dpi->num_internal_nodes + dpi->num_boundary_nodes; i++) {
  //        var_values[i] = efv->ext_fld_ndl_val[w][i];
  //      }
  //      mesh.add_tag(Omega_h::VERT, efv->name[w], 1, Omega_h::Reals(var_values));
  //  }

  auto writer = Omega_h::vtk::Writer("transfer.vtk", &mesh);
  writer.write(step);
//  adapt_mesh(mesh);

  std::string filename;
  std::stringstream ss;

  ss << "adapt." << step << ".vtk";

  auto writer_adapt = Omega_h::vtk::Writer(ss.str(), &mesh);
  writer_adapt.write(step);

  if (first_call) {
    base_name = std::string(ExoFileOutMono);
    first_call = false;
  }

  std::stringstream ss2;

  ss2 << base_name << "-s." << step;

  if (Num_Proc > 1) {
    Omega_h::exodus::convert_back_to_goma_exo_parallel(ss2.str().c_str(), &mesh, exo, dpi, true,
                                              classify_with);
  } else {
    Omega_h::exodus::convert_back_to_goma_exo(ss2.str().c_str(), &mesh, exo, dpi, true,
                                              classify_with);
  }

  resetup_problem(exo, dpi);

  for (int imtrx = 0; imtrx < upd->Total_Num_Matrices; imtrx++) {
    int numProcUnknowns = NumUnknowns[imtrx] + NumExtUnknowns[imtrx];
    realloc_dbl_1(&x[imtrx], numProcUnknowns, 0);
    realloc_dbl_1(&x_old[imtrx], numProcUnknowns, 0);
    realloc_dbl_1(&x_older[imtrx], numProcUnknowns, 0);
    realloc_dbl_1(&x_update[imtrx], numProcUnknowns, 0);
    realloc_dbl_1(&xdot[imtrx], numProcUnknowns, 0);
    realloc_dbl_1(&xdot_old[imtrx], numProcUnknowns, 0);
    realloc_dbl_1(&x_oldest[imtrx], numProcUnknowns, 0);
    realloc_dbl_1(&resid_vector[imtrx], numProcUnknowns, 0);
    realloc_dbl_1(&scale[imtrx], numProcUnknowns, 0);
    realloc_dbl_1(&x_update[imtrx], numProcUnknowns + numProcUnknowns, 0);
    pg->matrices[imtrx].ams = ams[imtrx];
    pg->matrices[imtrx].x = x[imtrx];
    pg->matrices[imtrx].x_old = x_old[imtrx];
    pg->matrices[imtrx].x_older = x_older[imtrx];
    pg->matrices[imtrx].xdot = xdot[imtrx];
    pg->matrices[imtrx].xdot_old = xdot_old[imtrx];
    pg->matrices[imtrx].x_update = x_update[imtrx];
    pg->matrices[imtrx].scale = scale[imtrx];
    pg->matrices[imtrx].resid_vector = resid_vector[imtrx];
  }

  for (int w = 0; w < efv->Num_external_field; w++) {
    realloc_dbl_1(&efv->ext_fld_ndl_val[w], dpi->num_internal_nodes + dpi->num_external_nodes, 0);
  }
  resetup_matrix(ams, exo, dpi);
  copy_solution(exo, dpi, x, mesh);
  step++;
}

} // extern "C"
#endif

// vim: expandtab sw=2 ts=8
